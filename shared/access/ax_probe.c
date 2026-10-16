/* ax_probe.c -- standalone diagnostic, NOT part of the daemon.
 *
 * Prints exactly what auth_prompt_poll_callback() would see for
 * whatever currently has system-wide keyboard focus: the owning
 * process name, the AX role of the focused element, and its text
 * value (truncated). Run this while a Terminal "Password:" prompt
 * is on screen to find out what role it actually reports.
 *
 * Also dumps the full AX tree for EVERY layer-0 on-screen window's
 * owning process (deduped by pid) -- not just System Settings. This
 * is the section to watch when diagnosing the v1.0.2 Keychain-autofill
 * watcher: trigger a Safari/Chrome "Touch ID to autofill" sheet, or a
 * "App wants to use your confidential information stored in keychain"
 * popup, during the 5-second countdown, and this dump will show
 * exactly which process actually owns the resulting secure field --
 * whether that's SecurityAgent (already allowlisted for the padlock
 * watcher) or something else entirely (Safari/Chrome itself, or a
 * private helper process neither of us has seen the name of yet). No
 * guessing needed; this is ground truth, same philosophy as the
 * System Settings dump below (which was originally built for the same
 * reason, before the padlock watcher shipped).
 *
 * Needs the same Accessibility grant the daemon has -- run it from
 * the SAME terminal app you're testing against isn't required, but
 * the process running ax_probe itself needs Accessibility access.
 * Easiest: just run it via sudo from a root shell that already has
 * Accessibility (or grant Terminal.app itself Accessibility in
 * System Settings for this one-off diagnostic).
 *
 * Build:
 *   clang ax_probe.c -o ax_probe -framework CoreFoundation -framework ApplicationServices
 *
 * Run (5-second countdown before it dumps -- trigger the prompt
 * you're diagnosing during that window):
 *   sudo ./ax_probe
 */
#include <ApplicationServices/ApplicationServices.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreFoundation/CoreFoundation.h>
#include <libproc.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void print_cfstring(const char *label, CFStringRef s) {
    if (!s) { printf("%s: (null)\n", label); return; }
    char buf[512];
    if (CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8)) {
        printf("%s: \"%s\"\n", label, buf);
    } else {
        printf("%s: (unrepresentable / too long)\n", label);
    }
}

/* Recursively prints role/subrole/value(short) for element and every
 * descendant, indented by depth, so we can see the real shape of an
 * app's AX tree instead of guessing. *budget is decremented on every
 * node visited (across the whole recursion, not per-branch) and
 * recursion stops once it hits zero, so a huge tree can't make this
 * hang or spam thousands of lines. */
static void dump_ax_tree(AXUIElementRef element, int depth, int *budget) {
    if (!element || depth > 12 || *budget <= 0) return;
    (*budget)--;

    CFStringRef role = NULL, subrole = NULL, value = NULL;
    AXUIElementCopyAttributeValue(element, kAXRoleAttribute, (CFTypeRef *)&role);
    AXUIElementCopyAttributeValue(element, kAXSubroleAttribute, (CFTypeRef *)&subrole);
    AXUIElementCopyAttributeValue(element, kAXValueAttribute, (CFTypeRef *)&value);

    char role_buf[128] = "(null)", subrole_buf[128] = "", value_buf[64] = "";
    if (role) CFStringGetCString(role, role_buf, sizeof(role_buf), kCFStringEncodingUTF8);
    if (subrole) CFStringGetCString(subrole, subrole_buf, sizeof(subrole_buf), kCFStringEncodingUTF8);
    if (value && CFGetTypeID(value) == CFStringGetTypeID()) {
        CFStringGetCString((CFStringRef)value, value_buf, sizeof(value_buf), kCFStringEncodingUTF8);
    }

    for (int i = 0; i < depth; i++) printf("  ");
    printf("- %s%s%s%s%s\n", role_buf,
           subrole_buf[0] ? " [" : "", subrole_buf[0] ? subrole_buf : "", subrole_buf[0] ? "]" : "",
           value_buf[0] ? (strcmp(role_buf, "AXSecureTextField") == 0 ? " value=(redacted, non-empty)" : "") : "");
    if (value_buf[0] && strcmp(role_buf, "AXSecureTextField") != 0) {
        for (int i = 0; i < depth; i++) printf("  ");
        printf("    value=\"%s\"\n", value_buf);
    }

    if (role) CFRelease(role);
    if (subrole) CFRelease(subrole);
    if (value) CFRelease(value);

    CFArrayRef children = NULL;
    if (AXUIElementCopyAttributeValue(element, kAXChildrenAttribute, (CFTypeRef *)&children) == kAXErrorSuccess && children) {
        CFIndex count = CFArrayGetCount(children);
        for (CFIndex i = 0; i < count && *budget > 0; i++) {
            dump_ax_tree((AXUIElementRef)CFArrayGetValueAtIndex(children, i), depth + 1, budget);
        }
        CFRelease(children);
    }
}

int main(void) {
    printf("Starting in 5 seconds -- click/focus the window you want to inspect now...\n");
    fflush(stdout);
    sleep(5);
    printf("Go.\n\n");

    /* Bare check first (cached, client-side, matches what the daemon
     * itself effectively relies on via prior grants). */
    bool bare_trusted = AXIsProcessTrusted();
    printf("AXIsProcessTrusted() [bare]: %s\n", bare_trusted ? "true" : "false");

    /* Now force the REAL check, with the prompt option on. If macOS's
     * actual enforcement layer doesn't consider this binary trusted --
     * regardless of what TCC.db says or what the bare check above
     * returned -- this will pop a genuine system permission dialog.
     * That dialog appearing is the ground truth; nothing else here is. */
    const void *keys[] = { kAXTrustedCheckOptionPrompt };
    const void *values[] = { kCFBooleanTrue };
    CFDictionaryRef options = CFDictionaryCreate(NULL, keys, values, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    bool prompted_trusted = AXIsProcessTrustedWithOptions(options);
    CFRelease(options);
    printf("AXIsProcessTrustedWithOptions(prompt=true): %s\n", prompted_trusted ? "true" : "false");
    printf("(If a system permission dialog just appeared on screen, that's the real signal --\n");
    printf(" it means macOS's actual enforcement never accepted the existing grant as valid.)\n\n");

    if (!bare_trusted && !prompted_trusted) {
        printf("Both checks say NOT trusted -- stopping here, the rest would just fail too.\n");
        return 1;
    }

    /* Full recursive dump of System Settings' AX tree -- every window,
     * every role, so we can see exactly where the padlock's secure
     * field actually lives (or doesn't) instead of guessing. */
    {
        int num_pids = proc_listallpids(NULL, 0);
        pid_t *pids = malloc(sizeof(pid_t) * (num_pids + 64));
        num_pids = proc_listallpids(pids, sizeof(pid_t) * (num_pids + 64));
        pid_t settings_pid = 0;
        for (int i = 0; i < num_pids; i++) {
            char name[256];
            if (proc_name(pids[i], name, sizeof(name)) > 0 && strcmp(name, "System Settings") == 0) {
                settings_pid = pids[i];
                break;
            }
        }
        free(pids);

        if (settings_pid == 0) {
            printf("Could not find a running 'System Settings' process.\n\n");
        } else {
            printf("--- Full AX tree dump for System Settings (pid %d) ---\n", settings_pid);
            AXUIElementRef app = AXUIElementCreateApplication(settings_pid);

            CFArrayRef windows = NULL;
            AXError werr = AXUIElementCopyAttributeValue(app, kAXWindowsAttribute, (CFTypeRef *)&windows);
            if (werr != kAXErrorSuccess || !windows) {
                printf("Could not get kAXWindowsAttribute (AXError %d).\n", (int)werr);
            } else {
                CFIndex wcount = CFArrayGetCount(windows);
                printf("Total windows: %ld\n", (long)wcount);
                for (CFIndex w = 0; w < wcount; w++) {
                    AXUIElementRef win = (AXUIElementRef)CFArrayGetValueAtIndex(windows, w);
                    CFStringRef wtitle = NULL;
                    AXUIElementCopyAttributeValue(win, kAXTitleAttribute, (CFTypeRef *)&wtitle);
                    char title_buf[256] = "(no title)";
                    if (wtitle) CFStringGetCString(wtitle, title_buf, sizeof(title_buf), kCFStringEncodingUTF8);
                    printf("\n[Window %ld] title=\"%s\"\n", (long)w, title_buf);
                    if (wtitle) CFRelease(wtitle);
                    int budget = 300;
                    dump_ax_tree(win, 1, &budget);
                }
                CFRelease(windows);
            }
            CFRelease(app);
            printf("--- end AX tree dump ---\n\n");
        }
    }

    /* Dump the raw on-screen window list, layer 0 only, so we can see
     * whether whatever's currently frontmost (e.g. a padlock sheet)
     * actually shows up here, and under which owning process/PID. */
    printf("--- On-screen windows (layer 0 only, front-to-back) ---\n");
    CFArrayRef window_list = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
        kCGNullWindowID);
    if (!window_list) {
        printf("CGWindowListCopyWindowInfo returned NULL.\n");
    } else {
        CFIndex count = CFArrayGetCount(window_list);
        printf("Total on-screen window entries (all layers): %ld\n", (long)count);
        for (CFIndex i = 0; i < count; i++) {
            CFDictionaryRef entry = (CFDictionaryRef)CFArrayGetValueAtIndex(window_list, i);
            CFNumberRef layer_num = (CFNumberRef)CFDictionaryGetValue(entry, kCGWindowLayer);
            int layer = -999;
            if (layer_num) CFNumberGetValue(layer_num, kCFNumberIntType, &layer);
            if (layer != 0) continue;

            CFNumberRef pid_num = (CFNumberRef)CFDictionaryGetValue(entry, kCGWindowOwnerPID);
            int pid_val = 0;
            if (pid_num) CFNumberGetValue(pid_num, kCFNumberIntType, &pid_val);

            CFStringRef owner_name = (CFStringRef)CFDictionaryGetValue(entry, kCGWindowOwnerName);
            char owner_buf[256] = "(unknown)";
            if (owner_name) CFStringGetCString(owner_name, owner_buf, sizeof(owner_buf), kCFStringEncodingUTF8);

            printf("  [layer 0] pid=%d owner=\"%s\"\n", pid_val, owner_buf);
        }
        CFRelease(window_list);
    }
    printf("--- end window list ---\n\n");

    /* Full AX tree dump for every DISTINCT layer-0 window owner --
     * generalized version of the System Settings-only dump above.
     * This is the section that actually answers "what process/role/
     * subrole does the Safari or Chrome Touch ID autofill sheet use"
     * or "what does the Keychain access popup's secure field look
     * like" -- we don't filter by name here on purpose, since the
     * whole point is not knowing the name ahead of time. Kept separate
     * from the System Settings dump above rather than replacing it, so
     * existing padlock-debugging usage of this tool doesn't change. */
    printf("--- Full AX tree dump per distinct layer-0 window owner ---\n");
    {
        CFArrayRef wlist = CGWindowListCopyWindowInfo(
            kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
            kCGNullWindowID);
        if (!wlist) {
            printf("CGWindowListCopyWindowInfo returned NULL.\n");
        } else {
            pid_t seen_pids[64];
            int seen_count = 0;
            CFIndex count = CFArrayGetCount(wlist);

            for (CFIndex i = 0; i < count; i++) {
                CFDictionaryRef entry = (CFDictionaryRef)CFArrayGetValueAtIndex(wlist, i);
                CFNumberRef layer_num = (CFNumberRef)CFDictionaryGetValue(entry, kCGWindowLayer);
                int layer = -999;
                if (layer_num) CFNumberGetValue(layer_num, kCFNumberIntType, &layer);
                if (layer != 0) continue;

                CFNumberRef pid_num = (CFNumberRef)CFDictionaryGetValue(entry, kCGWindowOwnerPID);
                if (!pid_num) continue;
                int pid_val = 0;
                CFNumberGetValue(pid_num, kCFNumberIntType, &pid_val);
                pid_t pid = (pid_t)pid_val;

                bool already = false;
                for (int s = 0; s < seen_count; s++) {
                    if (seen_pids[s] == pid) { already = true; break; }
                }
                if (already || seen_count >= 64) continue;
                seen_pids[seen_count++] = pid;

                char proc_name_buf[256] = "(unknown)";
                proc_name(pid, proc_name_buf, sizeof(proc_name_buf));

                char path_buf[1024] = "(unknown)";
                proc_pidpath(pid, path_buf, sizeof(path_buf));

                /* Best-effort window title -- may come back empty if
                 * Screen Recording permission isn't granted to this
                 * binary; harmless either way, just informational. */
                CFStringRef wname = (CFStringRef)CFDictionaryGetValue(entry, kCGWindowName);
                char title_buf[256] = "(no title / no Screen Recording perm)";
                if (wname) CFStringGetCString(wname, title_buf, sizeof(title_buf), kCFStringEncodingUTF8);

                printf("\n=== pid=%d  process=\"%s\"  window title=\"%s\" ===\n", pid, proc_name_buf, title_buf);
                printf("    path=\"%s\"\n", path_buf);

                AXUIElementRef app = AXUIElementCreateApplication(pid);
                CFArrayRef windows = NULL;
                AXError werr = AXUIElementCopyAttributeValue(app, kAXWindowsAttribute, (CFTypeRef *)&windows);
                if (werr != kAXErrorSuccess || !windows) {
                    printf("    Could not get kAXWindowsAttribute (AXError %d).\n", (int)werr);
                } else {
                    CFIndex wcount = CFArrayGetCount(windows);
                    for (CFIndex w = 0; w < wcount; w++) {
                        AXUIElementRef win = (AXUIElementRef)CFArrayGetValueAtIndex(windows, w);
                        int budget = 300;
                        dump_ax_tree(win, 1, &budget);
                    }
                    CFRelease(windows);
                }
                CFRelease(app);
            }
            CFRelease(wlist);
        }
    }
    printf("--- end per-owner AX tree dump ---\n\n");

    AXUIElementRef system_wide = AXUIElementCreateSystemWide();
    AXUIElementRef focused_app = NULL;
    AXError err = AXUIElementCopyAttributeValue(system_wide, kAXFocusedApplicationAttribute,
                                                 (CFTypeRef *)&focused_app);
    CFRelease(system_wide);

    if (err != kAXErrorSuccess || !focused_app) {
        printf("Could not get focused application (AXError %d).\n", (int)err);
        return 1;
    }

    pid_t pid = 0;
    AXUIElementGetPid(focused_app, &pid);
    char proc_name_buf[256] = "(unknown)";
    proc_name(pid, proc_name_buf, sizeof(proc_name_buf));
    printf("Focused app PID: %d  Process: %s\n", pid, proc_name_buf);

    AXUIElementRef focused_window = NULL;
    err = AXUIElementCopyAttributeValue(focused_app, kAXFocusedWindowAttribute,
                                         (CFTypeRef *)&focused_window);
    CFRelease(focused_app);
    if (err != kAXErrorSuccess || !focused_window) {
        printf("Could not get focused window (AXError %d).\n", (int)err);
        return 1;
    }

    AXUIElementRef focused_element = NULL;
    err = AXUIElementCopyAttributeValue(focused_window, kAXFocusedUIElementAttribute,
                                         (CFTypeRef *)&focused_element);
    CFRelease(focused_window);
    if (err != kAXErrorSuccess || !focused_element) {
        printf("Could not get focused UI element (AXError %d).\n", (int)err);
        return 1;
    }

    CFStringRef role = NULL, subrole = NULL, value = NULL;
    AXUIElementCopyAttributeValue(focused_element, kAXRoleAttribute, (CFTypeRef *)&role);
    AXUIElementCopyAttributeValue(focused_element, kAXSubroleAttribute, (CFTypeRef *)&subrole);
    AXUIElementCopyAttributeValue(focused_element, kAXValueAttribute, (CFTypeRef *)&value);

    print_cfstring("Role", role);
    print_cfstring("Subrole", subrole);

    if (value) {
        CFIndex len = CFStringGetLength(value);
        CFIndex tail_start = len > 200 ? len - 200 : 0;
        CFStringRef tail = CFStringCreateWithSubstring(NULL, value,
            CFRangeMake(tail_start, len - tail_start));
        print_cfstring("Value (last 200 chars)", tail);
        if (tail) CFRelease(tail);
    } else {
        printf("Value: (null)\n");
    }

    if (role) CFRelease(role);
    if (subrole) CFRelease(subrole);
    if (value) CFRelease(value);
    CFRelease(focused_element);
    return 0;
}
