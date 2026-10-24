/*
 * metallica_mis_agent.c
 *
 * Background authentication agent for the Synaptics Metallica MIS
 * fingerprint sensor (06cb:009a, 138a:0097, 138a:009d): idles until the
 * screen locks or a macOS authentication padlock appears, then waits for a
 * touch, asks the SENSOR to match it (match-in-sensor, nothing is matched on
 * the Mac) and types the stored login password on a match.
 *
 * This is the Metallica counterpart of vfs5011/vfs5011_daemon.c and shares
 * its lock/unlock detection, padlock watcher, OpenCore gate, password
 * typing and menu bar IPC. What is different:
 *   - no templates: the prints live on the sensor, so the volume is only
 *     mounted to read the stored password
 *   - the sensor session (TLS) is opened when an episode starts and closed
 *     when it ends, through the daemon API in metallica_mis_enroll.h
 *     (metallica_mis_agent_open / _identify / _close)
 *   - there is no swipe image, so Swipe to Lock becomes "Touch to Lock": while the
 *     screen is unlocked and the setting is on, a touch from an enrolled finger locks
 *     the screen (Control-Command-Q). Beta: offered from macOS Sequoia for testing,
 *     switch LOCKTOUCH_MIN_DARWIN to 25 to make it Tahoe only.
 *
 * UNTESTED ON REAL HARDWARE. Ported from the VFS5011 daemon and checked by
 * reading and static analysis only. Ships behind the Deploy gate in the
 * client until a tester has confirmed it.
 *
 * Build: scripts/build_metallica_agent.sh (writes ./metallica_mis_agent).
 * Installed by metallica/metallica_mis_agent_install.sh, run from Deploy [3].
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <time.h>
#include <sys/sysctl.h>
#include <sys/stat.h>
#include <ctype.h>
#include <dirent.h>
#include <libusb.h>
#include <libproc.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ApplicationServices/ApplicationServices.h>
#include <IOKit/IOKitLib.h>

#include "supported_sensors.h"
#include "metallica_mis_daemon.h"
#include "metallica_mis_enroll.h"
#include "hack-touchid-menubar-ipc.h"

/* Passed in by scripts/build_metallica_agent.sh from VERSION.txt. */
#ifndef HT_AGENT_VERSION
#define HT_AGENT_VERSION "unknown"
#endif

#define MAX_FAILED_TOUCHES 4      /* non-matching touches in one episode before falling back to the password */
#define MAX_OPEN_FAILURES 3       /* sensor session could not be opened this many times in a row: give up */
#define MMIS_HOST_ID_CONF_PATH "/usr/local/libexec/hack-touchid/metallica_host_identity.conf"
#define MMIS_HOST_ID_MAX 120

static atomic_int g_failed_swipes = 0; /* reset whenever an episode starts or the screen unlocks */


/* Built-in macOS system sound, not a bundled asset -- keeps the repo
 * asset-free for open sourcing and works on every Mac out of the box.
 * "Basso" is the classic short error/failure tone. */
#define FAILURE_SOUND_PATH "/System/Library/Sounds/Basso.aiff"
/* Bright, distinct confirmation tone -- deliberately different from
 * the failure sound so the two are unmistakable from across a room,
 * not just on close listening. */
#define SUCCESS_SOUND_PATH "/System/Library/Sounds/Glass.aiff"

/* Fires a short audible cue on a rejected (below-threshold) swipe, so
 * a failed attempt gives the same kind of immediate feedback real
 * Touch ID gives (a buzz/reject tone) instead of silently waiting for
 * another swipe. Backgrounded with `&` so afplay's ~1s runtime never
 * blocks the polling loop from immediately trying the next swipe.
 * Runs in the same GUI session as the daemon (that's the whole reason
 * this is a LaunchAgent and not a LaunchDaemon -- see the top-of-file
 * notes on session-scoped notification delivery), so audio output
 * goes to whatever device the user is actually using. Best-effort:
 * if afplay isn't available or the sound file is missing for some
 * reason, this silently does nothing rather than treating a failed
 * *swipe* as a reason to fail the whole daemon. */
static void play_failure_sound(void) {
    int status = system("afplay " FAILURE_SOUND_PATH " > /dev/null 2>&1 &");
    (void)status; /* best-effort — a missing sound file shouldn't affect matching */
}

static void play_success_sound(void) {
    int status = system("afplay " SUCCESS_SOUND_PATH " > /dev/null 2>&1 &");
    (void)status; /* best-effort, same reasoning as play_failure_sound() */
}

#define PASSWORD_FILENAME "password.txt"
#define MAX_PASSWORD_LEN 256
#define MOUNT_SCRIPT_NAME "hack-touchid-volume-mount.sh"
#define UNMOUNT_SCRIPT_NAME "hack-touchid-volume-unmount.sh"


/* Directory this binary is running from, resolved once at startup, so
 * the mount/unmount scripts can be found by absolute path regardless
 * of the caller's current working directory. */
static char g_exec_dir[PATH_MAX];

static void init_exec_dir(const char *argv0) {
    char resolved[PATH_MAX];
    if (realpath(argv0, resolved) == NULL) {
        /* Fall back to argv0 as-is if realpath fails (unusual, but
         * don't crash the whole menu over a cosmetic path lookup). */
        strncpy(resolved, argv0, sizeof(resolved) - 1);
        resolved[sizeof(resolved) - 1] = '\0';
    }
    char *dir = dirname(resolved); /* may alias into `resolved` — copy immediately */
    strncpy(g_exec_dir, dir, sizeof(g_exec_dir) - 1);
    g_exec_dir[sizeof(g_exec_dir) - 1] = '\0';
}

/* The volume scripts live in vol/ in the repo, but the deployed daemon
 * has them flat next to the binary (hack-touchid-agent-install.sh copies them
 * there). Look next to the running binary first, then in the repo layout. */
static const char *ht_vol_script(const char *name) {
    static char bufs[2][PATH_MAX];
    static int next;
    char *out = bufs[next++ & 1];
    snprintf(out, PATH_MAX, "%s/%s", g_exec_dir, name);
    if (access(out, F_OK) != 0)
        snprintf(out, PATH_MAX, "%s/vol/%s", g_exec_dir, name);
    return out;
}

/* Mounts the encrypted template volume via hack-touchid-volume-mount.sh,
 * capturing the mount point path it prints on success. Echoes the
 * script's own diagnostic lines so the behavior looks the same as
 * running it directly. Returns 0 and fills out_path on success. */
static int mount_template_volume(char *out_path, size_t out_path_size) {
    char cmd[PATH_MAX * 2];
    snprintf(cmd, sizeof(cmd), "\"%s\"", ht_vol_script(MOUNT_SCRIPT_NAME));

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        fprintf(stderr, "Failed to run volume mount script: %s\n", strerror(errno));
        return -1;
    }

    char line[PATH_MAX];
    char last_line[PATH_MAX] = {0};
    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';
        if (len > 0) {
            strncpy(last_line, line, sizeof(last_line) - 1);
            last_line[sizeof(last_line) - 1] = '\0';
            printf("  %s\n", line);
        }
    }
    int status = pclose(fp);

    /* The script's last printed line is the mount path on success — a
     * plain absolute path starting with '/'. Anything else (empty, an
     * error message, non-zero exit) means mounting failed. */
    if (status != 0 || last_line[0] != '/') {
        fprintf(stderr, "Volume mount failed (exit status %d).\n", status);
        return -1;
    }
    strncpy(out_path, last_line, out_path_size - 1);
    out_path[out_path_size - 1] = '\0';
    return 0;
}

static void unmount_template_volume(void) {
    char cmd[PATH_MAX * 2];
    snprintf(cmd, sizeof(cmd), "\"%s\"", ht_vol_script(UNMOUNT_SCRIPT_NAME));
    int status = system(cmd);
    if (status != 0) {
        fprintf(stderr,
                "Warning: volume unmount script exited with status %d — the volume may "
                "still be mounted. Run hack-touchid-volume-unmount.sh manually to check.\n",
                status);
    }
}

typedef enum {
    STATE_IDLE,
    STATE_POLLING,   /* a lock screen or padlock episode is active: the sensor is being polled */
    STATE_LOCK_ARMED /* Touch to Lock is on and the screen is unlocked: a matching touch locks it */
} daemon_state_t;

static atomic_int g_state = STATE_IDLE;
/* The state the polling thread opened its current wait for. abort_wait_cb() stops the wait
 * as soon as the real state differs (armed -> lock screen, armed -> idle, ...). */
static atomic_int g_wait_state = STATE_IDLE;
static void lockswipe_try_arm(void);
static void lockswipe_disarm(void);
static atomic_int g_running = 1;

/* Auth triggers, same as the VFS5011 daemon: the lock screen, or a
 * SecurityAgent / System Settings padlock with a secure text field. */

typedef enum {
    TRIGGER_NONE,
    TRIGGER_LOCK_SCREEN,
    TRIGGER_PADLOCK
} trigger_source_t;

static atomic_int g_trigger_source = TRIGGER_NONE;

/* pid that must still be focused/frontmost right before we type -- if
 * the user alt-tabbed away mid-swipe, we abort instead of typing into
 * whatever now has focus. Not used for TRIGGER_LOCK_SCREEN (there's
 * only ever one thing focused at the lock screen). */
static pid_t g_trigger_target_pid = 0;

/* Only populated for TRIGGER_PADLOCK -- the specific secure field to
 * focus right before typing, found once at detection time so we don't
 * have to re-walk the AX tree at match time. Retained on set, released
 * when the episode ends. */
static AXUIElementRef g_trigger_secure_field = NULL;

/* The stored login password is read from the encrypted volume once per
 * episode, on its own thread, so the sensor can light up straight away.
 * g_pw_lock protects the two fields below; g_password_ready is set by the
 * loader thread after it has finished writing. */
static char g_cached_password[MAX_PASSWORD_LEN + 1];
static int g_cached_password_valid = 0;
static pthread_mutex_t g_pw_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool g_password_ready = false;


static void print_timestamp(void) {
    CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
    CFDateFormatterRef fmt = CFDateFormatterCreate(NULL, CFLocaleCopyCurrent(),
                                                    kCFDateFormatterNoStyle,
                                                    kCFDateFormatterMediumStyle);
    CFDateRef date = CFDateCreate(NULL, now);
    CFStringRef str = CFDateFormatterCreateStringWithDate(NULL, fmt, date);
    char buf[64];
    CFStringGetCString(str, buf, sizeof(buf), kCFStringEncodingUTF8);
    printf("[%s] ", buf);
    CFRelease(str);
    CFRelease(date);
    CFRelease(fmt);
}

/* When the lock screen last appeared (monotonic ms, 0 = unknown). A
 * match that lands right after a lock (typical for swipe-to-lock, where
 * the finger is still on the sensor) must not type until the password
 * field is ready, or the characters are dropped but Return still lands. */
static atomic_llong g_lock_screen_since_ms = 0;
#define LOCK_SCREEN_SETTLE_MS 1500

static long long monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void wait_for_lock_screen_settle(void) {
    long long since = atomic_load(&g_lock_screen_since_ms);
    if (since == 0) return;
    long long wait = LOCK_SCREEN_SETTLE_MS - (monotonic_ms() - since);
    if (wait > 0) {
        print_timestamp();
        printf("Lock screen just appeared, waiting %lld ms before typing.\n", wait);
        usleep((useconds_t)(wait * 1000));
    }
}

/* Types a UTF-8 string via synthetic keyboard events, then presses
 * Return. Uses CGEventKeyboardSetUnicodeString rather than per-key
 * virtual keycodes, so it doesn't need to know the active keyboard
 * layout -- it injects the literal characters directly.
 *
 * SEE FILE-HEADER CAVEAT: this may be blocked by Secure Input at the
 * lock screen. This function will report success/failure of the
 * *posting* call, which is NOT the same as confirming the OS actually
 * accepted it into the password field -- that can only be confirmed
 * by watching whether the screen actually unlocks. */
static void type_password_and_enter(const char *password) {
    size_t len = strlen(password);
    if (len == 0 || len > MAX_PASSWORD_LEN) {
        fprintf(stderr, "Refusing to type password: invalid length (%zu).\n", len);
        return;
    }

    UniChar unichars[MAX_PASSWORD_LEN];
    for (size_t i = 0; i < len; i++) {
        unichars[i] = (UniChar)(unsigned char)password[i]; /* ASCII passwords only */
    }

    CGEventRef key_down = CGEventCreateKeyboardEvent(NULL, 0, true);
    CGEventKeyboardSetUnicodeString(key_down, (UniCharCount)len, unichars);
    CGEventSetFlags(key_down, 0); /* no Ctrl/Cmd leaking in from the lock keystroke */
    CGEventPost(kCGSessionEventTap, key_down);
    CFRelease(key_down);

    CGEventRef key_up = CGEventCreateKeyboardEvent(NULL, 0, false);
    CGEventKeyboardSetUnicodeString(key_up, (UniCharCount)len, unichars);
    CGEventSetFlags(key_up, 0);
    CGEventPost(kCGSessionEventTap, key_up);
    CFRelease(key_up);

    /* Small delay before Return -- typing the whole password as one
     * event and immediately submitting can race the field's own
     * internal handling on some macOS versions. */
    usleep(150000);

    CGEventRef return_down = CGEventCreateKeyboardEvent(NULL, (CGKeyCode)0x24 /* kVK_Return */, true);
    CGEventSetFlags(return_down, 0);
    CGEventPost(kCGSessionEventTap, return_down);
    CFRelease(return_down);

    CGEventRef return_up = CGEventCreateKeyboardEvent(NULL, (CGKeyCode)0x24, false);
    CGEventSetFlags(return_up, 0);
    CGEventPost(kCGSessionEventTap, return_up);
    CFRelease(return_up);
}

/* ------------------------------------------------------------------ *
 * Sensor presence, client yield, state helpers
 * ------------------------------------------------------------------ */

/* True if any Metallica MIS identity from supported_sensors.h is on the USB
 * bus right now. Checked fresh on every trigger so a plugged-in sensor is
 * picked up without restarting the agent, and a lock on a machine without
 * the sensor never shows a "touch now" notification. */
static bool metallica_sensor_is_present(void) {
    libusb_context *probe_ctx = NULL;
    if (libusb_init(&probe_ctx) < 0) return true; /* let the real open surface the error */
    libusb_set_option(probe_ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_NONE);
    libusb_device **list = NULL;
    ssize_t count = libusb_get_device_list(probe_ctx, &list);
    bool found = false;
    for (ssize_t i = 0; i < count && !found; i++) {
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(list[i], &desc) != 0) continue;
        for (size_t k = 0; k < HACK_TOUCHID_SENSOR_COUNT; k++) {
            const hack_touchid_sensor_t *s = &HACK_TOUCHID_SENSORS[k];
            if (strcmp(s->family, "metallica") == 0 && desc.idVendor == s->vid && desc.idProduct == s->pid) {
                found = true;
                break;
            }
        }
    }
    if (list) libusb_free_device_list(list, 1);
    libusb_exit(probe_ctx);
    return found;
}

/* The saved host identity (product name + serial the sensor was paired
 * with), written by the client the first time a session needed it. Without
 * it a sensor that was paired on Windows or Linux refuses our session. */
static char g_id_product[MMIS_HOST_ID_MAX + 1];
static char g_id_serial[MMIS_HOST_ID_MAX + 1];

static void load_saved_host_identity(void) {
    FILE *f = fopen(MMIS_HOST_ID_CONF_PATH, "r");
    if (!f) return;
    char line[256];
    g_id_product[0] = g_id_serial[0] = '\0';
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "product=", 8) == 0) snprintf(g_id_product, sizeof(g_id_product), "%s", line + 8);
        else if (strncmp(line, "serial=", 7) == 0) snprintf(g_id_serial, sizeof(g_id_serial), "%s", line + 7);
    }
    fclose(f);
    if (g_id_product[0] && g_id_serial[0]) {
        g_metallica_mis_host_product_override = g_id_product;
        g_metallica_mis_host_serial_override = g_id_serial;
        printf("Using the saved host identity from %s.\n", MMIS_HOST_ID_CONF_PATH);
    }
}

/* The client asks the agent to let go of the sensor while it enrolls or
 * verifies (yield), and tells it when it is done. A yield that is never
 * ended times out so a crashed client cannot lock the agent out forever. */
#define YIELD_TIMEOUT_SEC 300
static atomic_bool g_yield_active = false;
static atomic_bool g_yield_ack_pending = false;
static _Atomic long long g_yield_deadline = 0;

static bool yield_is_active(void) {
    if (!atomic_load(&g_yield_active)) return false;
    if ((long long)time(NULL) < atomic_load(&g_yield_deadline)) return true;
    atomic_store(&g_yield_active, false);
    print_timestamp();
    printf("Sensor yield timed out, taking the sensor back.\n");
    return false;
}

static void on_yield_request(bool begin) {
    if (begin) {
        atomic_store(&g_yield_deadline, (long long)time(NULL) + YIELD_TIMEOUT_SEC);
        atomic_store(&g_yield_active, true);
        atomic_store(&g_yield_ack_pending, true);
        print_timestamp();
        printf("Sensor yielded to the client.\n");
        lockswipe_disarm();
    } else {
        if (!atomic_exchange(&g_yield_active, false)) return;
        atomic_store(&g_yield_ack_pending, false);
        print_timestamp();
        printf("Client finished, taking the sensor back.\n");
        lockswipe_try_arm(); /* only re-arms if Touch to Lock is on */
    }
}

/* Called from the menu bar IPC (main run loop) on enable/disable. */
static void on_lockswipe_setting_changed(bool enabled) {
    if (enabled) lockswipe_try_arm(); else lockswipe_disarm();
}

/* After an episode or an unlock the finger that just touched the sensor is probably still on it,
 * so Touch to Lock waits a few seconds before it arms again. */
#define LOCKTOUCH_GRACE_SEC 4
static _Atomic long long g_arm_not_before = 0;

static void lockswipe_set_grace(int seconds) {
    atomic_store(&g_arm_not_before, (long long)time(NULL) + seconds);
}

static void settle_to_idle(void) {
    atomic_store(&g_state, STATE_IDLE);
    lockswipe_set_grace(LOCKTOUCH_GRACE_SEC);
    lockswipe_try_arm();
}

/* Polled by the daemon API while it waits for a finger (about every 300 ms):
 * stop waiting when the episode ended, the client wants the sensor, or we quit. */
static int abort_wait_cb(void) {
    return atomic_load(&g_state) != atomic_load(&g_wait_state) || yield_is_active() || !atomic_load(&g_running);
}


static bool screen_is_locked_now(void) {
    CFDictionaryRef session = CGSessionCopyCurrentDictionary();
    if (!session) return false;
    /* Key is only present (and true) while the screen is locked. */
    const void *v = CFDictionaryGetValue(session, CFSTR("CGSSessionScreenIsLocked"));
    bool locked = (v != NULL && CFGetTypeID(v) == CFBooleanGetTypeID() && CFBooleanGetValue((CFBooleanRef)v));
    CFRelease(session);
    return locked;
}

/* ------------------------------------------------------------------ *
 * Touch to Lock (beta)
 * ------------------------------------------------------------------ *
 * While the screen is unlocked and the setting is on, the agent keeps a sensor session open and
 * asks the sensor to match every touch. A match locks the screen with Control-Command-Q, the same
 * keystroke the VFS5011 daemon uses. A touch that does not match does nothing. */
#define LOCKTOUCH_MIN_DARWIN 24 /* 24 = Sequoia (testing). Set to 25 for Tahoe only once it is confirmed. */
#define TOUCHLOCK_MARKER "/Library/Application Support/VFS5011/touch_lock_available"

static bool os_allows_touch_lock(void) {
    char rel[32] = "";
    size_t len = sizeof(rel);
    if (sysctlbyname("kern.osrelease", rel, &len, NULL, 0) != 0) return false;
    return atoi(rel) >= LOCKTOUCH_MIN_DARWIN;
}

/* The menu bar app and the Settings pane show the toggle when this file exists. */
static void touch_lock_publish(bool available) {
    if (!available || !os_allows_touch_lock()) { unlink(TOUCHLOCK_MARKER); return; }
    mkdir("/Library/Application Support/VFS5011", 0755);
    FILE *f = fopen(TOUCHLOCK_MARKER, "w");
    if (!f) return;
    fputs("1\n", f);
    fclose(f);
    chmod(TOUCHLOCK_MARKER, 0644);
}

/* Logs why arming was skipped, only when the reason changes (the 2 s self-heal would spam it). */
static void lockswipe_note_skip(const char *why) {
    static const char *last = NULL;
    if (last == why) return;
    last = why;
    if (!why) return;
    print_timestamp();
    printf("Touch to Lock not armed: %s\n", why);
}

static void lockswipe_try_arm(void) {
    if (!vfs5011_lockswipe_is_enabled()) return;
    if (yield_is_active()) return; /* the client has the sensor */
    if (!os_allows_touch_lock()) { lockswipe_note_skip("this macOS version is not supported for Touch to Lock"); return; }
    if (!vfs5011_scanning_is_enabled()) { lockswipe_note_skip("scanning is paused"); return; }
    if ((long long)time(NULL) < atomic_load(&g_arm_not_before)) { lockswipe_note_skip("waiting for the finger to lift"); return; }
    if (screen_is_locked_now()) { lockswipe_note_skip("screen is locked"); return; }
    if (!metallica_sensor_is_present()) { lockswipe_note_skip("no Metallica MIS sensor present"); return; }
    int cur = atomic_load(&g_state);
    if (cur != STATE_IDLE && cur != STATE_LOCK_ARMED) { lockswipe_note_skip("another auth episode is active"); return; }
    int expected = STATE_IDLE;
    if (atomic_compare_exchange_strong(&g_state, &expected, STATE_LOCK_ARMED)) {
        lockswipe_note_skip(NULL);
        print_timestamp();
        printf("Touch to Lock armed.\n");
    }
}

static void lockswipe_disarm(void) {
    int expected = STATE_LOCK_ARMED;
    if (atomic_compare_exchange_strong(&g_state, &expected, STATE_IDLE)) {
        print_timestamp();
        printf("Touch to Lock disarmed.\n");
    }
}

/* The sensor session cannot be used (no prints, refused identity, repeated errors): stop arming
 * for a while instead of retrying in a tight loop. The setting itself stays on. */
static void lockswipe_pause(const char *why, int seconds) {
    lockswipe_set_grace(seconds);
    int expected = STATE_LOCK_ARMED;
    if (atomic_compare_exchange_strong(&g_state, &expected, STATE_IDLE)) {
        print_timestamp();
        printf("Touch to Lock paused for %d s: %s\n", seconds, why);
    }
}

static void post_lock_keystroke(void) {
    const CGKeyCode kVK_ANSI_Q = 0x0C;
    CGEventFlags flags = kCGEventFlagMaskControl | kCGEventFlagMaskCommand;
    CGEventRef down = CGEventCreateKeyboardEvent(NULL, kVK_ANSI_Q, true);
    CGEventSetFlags(down, flags);
    CGEventPost(kCGSessionEventTap, down);
    CFRelease(down);
    CGEventRef up = CGEventCreateKeyboardEvent(NULL, kVK_ANSI_Q, false);
    CGEventSetFlags(up, flags);
    CGEventPost(kCGSessionEventTap, up);
    CFRelease(up);
}

static void trigger_touch_lock(void) {
    if (screen_is_locked_now()) return;
    /* Drop to IDLE first: on_screen_locked() arms the lock-screen episode from IDLE or LOCK_ARMED,
     * and the CAS also catches the state having changed under us (a padlock just appeared). */
    int expected = STATE_LOCK_ARMED;
    if (!atomic_compare_exchange_strong(&g_state, &expected, STATE_IDLE)) return;
    lockswipe_set_grace(LOCKTOUCH_GRACE_SEC);
    print_timestamp();
    printf("Touch to Lock: matched touch -> locking the screen (Ctrl+Cmd+Q).\n");
    post_lock_keystroke();
    usleep(1000000);
    if (!screen_is_locked_now() && atomic_load(&g_state) == STATE_IDLE) {
        print_timestamp();
        printf("Touch to Lock: the screen did not lock (is Accessibility granted to the agent?). Re-arming.\n");
        lockswipe_try_arm();
    }
}

/* ------------------------------------------------------------------ *
 * Accessibility helpers used by the padlock watcher
 * ------------------------------------------------------------------ */

/* Looks up a process's name (e.g. "SecurityAgent") via libproc -- no
 * Cocoa/NSRunningApplication needed. Returns 0 on success. */
static int get_process_name(pid_t pid, char *out, size_t out_size) {
    char buf[PROC_PIDPATHINFO_MAXSIZE];
    if (proc_name(pid, buf, sizeof(buf)) <= 0) return -1;
    strncpy(out, buf, out_size - 1);
    out[out_size - 1] = '\0';
    return 0;
}

/* Looks up a process's full executable path via libproc. Returns 0 on
 * success. Used to catch System Settings pane extensions by location
 * rather than by name -- see is_system_auth_process's comment. */
static int get_process_path(pid_t pid, char *out, size_t out_size) {
    char buf[PROC_PIDPATHINFO_MAXSIZE];
    if (proc_pidpath(pid, buf, sizeof(buf)) <= 0) return -1;
    strncpy(out, buf, out_size - 1);
    out[out_size - 1] = '\0';
    return 0;
}

/* Returns the frontmost on-screen window's owning PID by walking the
 * window list front-to-back and taking the first real (layer-0,
 * non-desktop) window's owner. Doesn't touch Accessibility at all --
 * no TCC grant needed for this part; only window titles are gated
 * behind Screen Recording on modern macOS, and we don't read titles. */
static pid_t get_frontmost_window_owner_pid(void) {
    CFArrayRef window_list = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
        kCGNullWindowID);
    if (!window_list) return 0;

    pid_t result_pid = 0;
    CFIndex count = CFArrayGetCount(window_list);
    for (CFIndex i = 0; i < count; i++) {
        CFDictionaryRef entry = (CFDictionaryRef)CFArrayGetValueAtIndex(window_list, i);
        CFNumberRef layer_num = (CFNumberRef)CFDictionaryGetValue(entry, kCGWindowLayer);
        int layer = -1;
        if (layer_num) CFNumberGetValue(layer_num, kCFNumberIntType, &layer);
        if (layer != 0) continue;

        CFNumberRef pid_num = (CFNumberRef)CFDictionaryGetValue(entry, kCGWindowOwnerPID);
        if (!pid_num) continue;
        int pid_val = 0;
        CFNumberGetValue(pid_num, kCFNumberIntType, &pid_val);
        result_pid = (pid_t)pid_val;
        break;
    }
    CFRelease(window_list);
    return result_pid;
}

/* Fills candidate_pids (capacity max_count) with every currently-live
 * process whose executable lives under a given app bundle's PlugIns
 * directory -- i.e. its active pane extensions. Returns the count
 * found. Used because a System Settings pane's actual keyboard focus
 * and AX tree live in its extension process, not in "System Settings"
 * itself, and there's no reliable window-based way to discover which
 * extension is currently active -- but there are only ever a couple
 * running at once, so just checking each one directly is cheap. */
static int find_live_pids_under_path(const char *path_substring, pid_t *candidate_pids, int max_count) {
    int found = 0;
    int num_pids = proc_listallpids(NULL, 0);
    if (num_pids <= 0) return 0;
    pid_t *all_pids = malloc(sizeof(pid_t) * (num_pids + 64));
    if (!all_pids) return 0;
    num_pids = proc_listallpids(all_pids, sizeof(pid_t) * (num_pids + 64));

    for (int i = 0; i < num_pids && found < max_count; i++) {
        char path_buf[1024];
        if (proc_pidpath(all_pids[i], path_buf, sizeof(path_buf)) <= 0) continue;
        if (strstr(path_buf, path_substring) != NULL) {
            candidate_pids[found++] = all_pids[i];
        }
    }
    free(all_pids);
    return found;
}

/* Returns the AX application element (CFRetain'd -- caller releases)
 * for whichever process is most likely to hold the currently-relevant
 * auth prompt, or NULL if nothing auth-relevant is in play this tick.
 *
 * Historically this avoided AXUIElementCreateSystemWide() +
 * kAXFocusedApplicationAttribute entirely -- confirmed, via live
 * [DIAG] logging in this exact daemon, to fail unconditionally with
 * kAXErrorCannotComplete on this hardware for the padlock/System
 * Settings cases. Direct by-PID AXUIElementCreateApplication() calls
 * are confirmed reliable (see ax_probe.c's Dock control test), so the
 * candidate list was built a different way:
 *   1. The frontmost window's owning PID (via CGWindowList) --
 *      correctly covers SecurityAgent, authorizationhost, coreauthd,
 *      loginwindow, and non-extension-hosted System Settings/System
 *      Preferences windows directly.
 *   2. If that owner is System Settings or System Preferences, ALSO
 *      every live PlugInKit extension process under its bundle --
 *      correctly covers extension-hosted panes (e.g. Users & Groups),
 *      where the window owner and the actually-focused process are
 *      different PIDs entirely.
 *
 * v1.0.2 addition (Keychain-access consent dialogs, e.g. Keychain
 * Access's "wants to use your confidential information" alert, owned
 * by the coreautha process): confirmed via ax_probe.c that this exact
 * dialog does NOT show up in the layer-0 CGWindowList scan at all --
 * source (1) above returns nothing for it -- but the system-wide
 * focused-application lookup DOES resolve it correctly, contradicting
 * the historical finding above for this specific dialog type. Rather
 * than replace the window-based approach (still the more reliable
 * source for the padlock/Settings/Passwords cases already confirmed
 * working), this adds source (3): a best-effort system-wide
 * focused-app lookup, appended as an extra candidate if it succeeds
 * and isn't already in the list. If it fails (kAXErrorCannotComplete
 * or otherwise), that's silently ignored and sources (1)/(2) still
 * apply as before -- this is additive, not a replacement, since we
 * don't yet know if it behaves identically once running under launchd
 * as root versus this interactive sudo ax_probe test.
 *
 * The caller checks each candidate against the auth-process allowlist
 * and only proceeds with ones that pass. */
static int get_auth_candidate_pids(pid_t *candidate_pids, int max_count) {
    if (max_count < 1) return 0;
    int count = 0;

    pid_t frontmost = get_frontmost_window_owner_pid();
    if (frontmost != 0) {
        candidate_pids[count++] = frontmost;

        char name_buf[256];
        if (count < max_count && get_process_name(frontmost, name_buf, sizeof(name_buf)) == 0
            && (strcmp(name_buf, "System Settings") == 0 || strcmp(name_buf, "System Preferences") == 0)) {
            pid_t ext_pids[8];
            int ext_count = find_live_pids_under_path("System Settings.app/Contents/PlugIns/", ext_pids, 8);
            if (ext_count == 0) {
                ext_count = find_live_pids_under_path("System Preferences.app/Contents/PlugIns/", ext_pids, 8);
            }
            for (int i = 0; i < ext_count && count < max_count; i++) {
                candidate_pids[count++] = ext_pids[i];
            }
        }
    }

    /* Source (3): system-wide focused application, best-effort. Covers
     * alert-style dialogs (confirmed: coreautha's Keychain-access
     * consent alert) that never appear in the layer-0 window list. */
    if (count < max_count) {
        AXUIElementRef system_wide = AXUIElementCreateSystemWide();
        AXUIElementRef focused_app = NULL;
        AXError err = AXUIElementCopyAttributeValue(system_wide, kAXFocusedApplicationAttribute,
                                                      (CFTypeRef *)&focused_app);
        CFRelease(system_wide);

        if (err == kAXErrorSuccess && focused_app) {
            pid_t focused_pid = 0;
            AXUIElementGetPid(focused_app, &focused_pid);
            CFRelease(focused_app);

            if (focused_pid != 0) {
                bool already_listed = false;
                for (int i = 0; i < count; i++) {
                    if (candidate_pids[i] == focused_pid) { already_listed = true; break; }
                }
                if (!already_listed) candidate_pids[count++] = focused_pid;
            }
        }
        /* err != kAXErrorSuccess is expected/normal in many cases (see
         * comment above) -- not logged here to avoid spamming the log
         * every 300ms tick when nothing auth-relevant is focused. */
    }

    return count;
}


/* ------------------------------------------------------------------ *
 * Episode handling: password load, lock / unlock, arming
 * ------------------------------------------------------------------ */

/* Mounts the encrypted volume only long enough to read the stored password,
 * then unmounts it again. There are no templates to load for this sensor. */
static int load_password_for_episode(void) {
    char mount_path[PATH_MAX];
    if (mount_template_volume(mount_path, sizeof(mount_path)) != 0) {
        fprintf(stderr, "Could not mount the volume to read the stored password.\n");
        return -1;
    }
    char password_path[PATH_MAX];
    snprintf(password_path, sizeof(password_path), "%s/%s", mount_path, PASSWORD_FILENAME);

    pthread_mutex_lock(&g_pw_lock);
    g_cached_password_valid = 0;
    FILE *pf = fopen(password_path, "r");
    if (pf) {
        size_t n = fread(g_cached_password, 1, MAX_PASSWORD_LEN, pf);
        g_cached_password[n] = '\0';
        fclose(pf);
        g_cached_password_valid = 1;
    } else {
        fprintf(stderr, "No stored password found (run hack-touchid-store-password.sh first).\n");
    }
    int ok = g_cached_password_valid ? 0 : -1;
    pthread_mutex_unlock(&g_pw_lock);

    unmount_template_volume();
    return ok;
}

static void *load_password_thread_main(void *arg) {
    (void)arg;
    load_password_for_episode(); /* a missing password is reported at match time, not here */
    atomic_store(&g_password_ready, true);
    return NULL;
}

static void arm_polling_for_trigger(trigger_source_t source, pid_t target_pid,
                                     AXUIElementRef secure_field, const char *label);

static void on_screen_locked(void) {
    int st0 = atomic_load(&g_state);
    if (st0 != STATE_IDLE && st0 != STATE_LOCK_ARMED) return; /* don't interrupt an episode */
    atomic_store(&g_lock_screen_since_ms, monotonic_ms());
    arm_polling_for_trigger(TRIGGER_LOCK_SCREEN, 0, NULL, "Screen lock");
}

static void on_screen_unlocked(void) {
    atomic_store(&g_state, STATE_IDLE);
    lockswipe_set_grace(LOCKTOUCH_GRACE_SEC); /* the unlocking finger may still be on the sensor */
    atomic_store(&g_trigger_source, TRIGGER_NONE);
    atomic_store(&g_lock_screen_since_ms, 0);
    atomic_store(&g_failed_swipes, 0);
    if (g_trigger_secure_field) { CFRelease(g_trigger_secure_field); g_trigger_secure_field = NULL; }

    /* Never keep the password resident while idle. */
    pthread_mutex_lock(&g_pw_lock);
    memset(g_cached_password, 0, sizeof(g_cached_password));
    g_cached_password_valid = 0;
    pthread_mutex_unlock(&g_pw_lock);
    atomic_store(&g_password_ready, false);

    print_timestamp();
    printf("Screen UNLOCKED -> entering IDLE state.\n");
}

/* Shared by both triggers: enters POLLING (the polling thread opens the
 * sensor session and lights it up) and starts the password load. */
static void arm_polling_for_trigger(trigger_source_t source, pid_t target_pid,
                                     AXUIElementRef secure_field, const char *label) {
    if (!vfs5011_scanning_is_enabled()) {
        print_timestamp();
        printf("%s detected, but scanning is paused -- staying IDLE.\n", label);
        settle_to_idle();
        if (secure_field) CFRelease(secure_field);
        return;
    }
    if (yield_is_active()) {
        print_timestamp();
        printf("%s detected, but the client has the sensor -- staying IDLE.\n", label);
        settle_to_idle();
        if (secure_field) CFRelease(secure_field);
        return;
    }
    if (!metallica_sensor_is_present()) {
        print_timestamp();
        printf("%s detected, but no Metallica MIS sensor found on this system -- staying IDLE.\n", label);
        settle_to_idle();
        if (secure_field) CFRelease(secure_field);
        return;
    }

    g_trigger_target_pid = target_pid;
    if (g_trigger_secure_field) { CFRelease(g_trigger_secure_field); g_trigger_secure_field = NULL; }
    g_trigger_secure_field = secure_field; /* already retained by the caller */
    atomic_store(&g_trigger_source, source);

    atomic_store(&g_password_ready, false);
    atomic_store(&g_failed_swipes, 0);
    atomic_store(&g_state, STATE_POLLING);
    print_timestamp();
    printf("%s detected -> entering POLLING state, loading the stored password in background...\n", label);
    vfs5011_notify_swipe_requested();

    pthread_t loader_thread;
    if (pthread_create(&loader_thread, NULL, load_password_thread_main, NULL) != 0) {
        print_timestamp();
        printf("Could not start the password loader thread -- staying IDLE.\n");
        settle_to_idle();
        atomic_store(&g_trigger_source, TRIGGER_NONE);
        if (g_trigger_secure_field) { CFRelease(g_trigger_secure_field); g_trigger_secure_field = NULL; }
        vfs5011_notify_swipe_failed();
        return;
    }
    pthread_detach(loader_thread);
}


/* Process names that legitimately ask for the macOS LOGIN password via
 * a secure field -- as opposed to some arbitrary app's own unrelated
 * password prompt (a website login form, a mail account password, a
 * VPN client, etc), which we must never auto-type the login password
 * into. "System Settings" replaced "SecurityAgent" for most padlocks
 * as of the redesigned Settings app -- but as of the PlugInKit-based
 * pane architecture, the padlock sheet's actual keyboard focus lives
 * in that pane's own extension PROCESS, not the "System Settings"
 * host process itself. Each pane's extension has its own distinct
 * name (e.g. "UsersGroups", "GeneralSettings", "PrivacySecurity") --
 * too many, and too likely to change/add more, to hardcode by name.
 * Instead, check the executable's actual path: any pane extension
 * lives under System Settings.app's own PlugIns directory, regardless
 * of what it's individually called. */
static bool is_system_auth_process(const char *name, const char *path) {
    static const char *allowlist[] = {
        "SecurityAgent", "authorizationhost", "coreauthd",
        "System Settings", "System Preferences", "loginwindow",
        /* Passwords.app (Sequoia+) -- its own lock-gate password field,
         * confirmed via ax_probe.c to be a distinct process (NOT
         * SecurityAgent), reporting a plain AXTextField/AXSecureTextField
         * that hits the same AXError -25205 "no focused UI element" as
         * System Settings panes -- already covered by the existing
         * find_secure_text_field tree-walk fallback below, so no new
         * detection logic was needed, just this allowlist entry. */
        "Passwords",
        /* coreautha (LocalAuthentication.framework/Support/coreautha.bundle)
         * -- distinct from coreauthd above, NOT a typo. Owns Keychain
         * Access's "wants to use your confidential information stored
         * in keychain" consent alert. Confirmed via ax_probe.c; only
         * discoverable via the system-wide focused-app fallback in
         * get_auth_candidate_pids (source 3), since this alert never
         * appears in the layer-0 window list. */
        "coreautha"
    };
    for (size_t i = 0; i < sizeof(allowlist) / sizeof(allowlist[0]); i++) {
        if (strcmp(name, allowlist[i]) == 0) return true;
    }
    if (path && (strstr(path, "/System Settings.app/Contents/PlugIns/") != NULL
                 || strstr(path, "/System Preferences.app/Contents/PlugIns/") != NULL)) {
        return true;
    }
    return false;
}

/* Like get_focused_element_role_and_text, but also hands back the
 * element itself (retained) so the padlock path can keep it around to
 * re-focus right before typing. */
/* Fallback for windows that don't support kAXFocusedUIElementAttribute
 * (observed on modern System Settings panes -- e.g. Users & Groups --
 * which are rendered by a separate PlugInKit extension process via a
 * remote view; that window returns kAXErrorAttributeUnsupported for
 * "what's focused" even though the window and its children are
 * otherwise perfectly queryable). Walks the AX tree looking for the
 * first AXSecureTextField, since that's specifically what we're after
 * here -- not a general "find whatever's focused" replacement.
 * Capped by both depth and total node budget so a pathological view
 * hierarchy can't make this poll callback hang. Returns a retained
 * element the caller must release, or NULL. */
static AXUIElementRef find_secure_text_field(AXUIElementRef element, int depth, int *budget) {
    if (!element || depth <= 0 || *budget <= 0) return NULL;
    (*budget)--;

    /* NOTE: secure fields in these SwiftUI-hosted System Settings
     * sheets report role=AXTextField with subrole=AXSecureTextField --
     * NOT role=AXSecureTextField directly, despite that being the
     * commonly-assumed check (and what every version of this function
     * checked until a full AX tree dump proved otherwise). Check
     * subrole; fall back to role for compatibility with any other
     * context that genuinely does use it as the role. */
    CFStringRef subrole = NULL, role = NULL;
    AXUIElementCopyAttributeValue(element, kAXSubroleAttribute, (CFTypeRef *)&subrole);
    bool is_secure = subrole && CFStringCompare(subrole, CFSTR("AXSecureTextField"), 0) == kCFCompareEqualTo;
    if (subrole) CFRelease(subrole);
    if (!is_secure) {
        AXUIElementCopyAttributeValue(element, kAXRoleAttribute, (CFTypeRef *)&role);
        is_secure = role && CFStringCompare(role, CFSTR("AXSecureTextField"), 0) == kCFCompareEqualTo;
        if (role) CFRelease(role);
    }
    if (is_secure) {
        CFRetain(element);
        return element;
    }

    CFArrayRef children = NULL;
    if (AXUIElementCopyAttributeValue(element, kAXChildrenAttribute, (CFTypeRef *)&children) != kAXErrorSuccess || !children) {
        return NULL;
    }

    CFIndex count = CFArrayGetCount(children);
    AXUIElementRef found = NULL;
    for (CFIndex i = 0; i < count && !found && *budget > 0; i++) {
        AXUIElementRef child = (AXUIElementRef)CFArrayGetValueAtIndex(children, i);
        found = find_secure_text_field(child, depth - 1, budget);
    }
    CFRelease(children);
    return found;
}

static void get_focused_element_full(AXUIElementRef focused_app, bool allow_expensive_fallback,
                                      CFStringRef *out_role, CFStringRef *out_subrole,
                                      CFStringRef *out_value, AXUIElementRef *out_element) {
    *out_role = NULL;
    *out_subrole = NULL;
    *out_value = NULL;
    *out_element = NULL;
    if (!focused_app) return;

    AXUIElementRef focused_window = NULL;
    if (AXUIElementCopyAttributeValue(focused_app, kAXFocusedWindowAttribute,
                                       (CFTypeRef *)&focused_window) != kAXErrorSuccess || !focused_window) {
        return;
    }

    AXUIElementRef focused_element = NULL;
    AXError err = AXUIElementCopyAttributeValue(focused_window, kAXFocusedUIElementAttribute,
                                                 (CFTypeRef *)&focused_element);

    if ((err != kAXErrorSuccess || !focused_element) && allow_expensive_fallback) {
        /* Fallback path -- see find_secure_text_field's comment above.
         * Gated behind allow_expensive_fallback: only worth paying for
         * when we already know this process is on the auth allowlist,
         * so we don't tree-walk whatever ordinary app happens to be
         * frontmost on every single 300ms tick all day. */
        int budget = 500; /* generous but bounded node visit cap */
        focused_element = find_secure_text_field(focused_window, 8, &budget);
    }
    CFRelease(focused_window);
    if (!focused_element) return;

    AXUIElementCopyAttributeValue(focused_element, kAXRoleAttribute, (CFTypeRef *)out_role);
    AXUIElementCopyAttributeValue(focused_element, kAXSubroleAttribute, (CFTypeRef *)out_subrole);
    AXUIElementCopyAttributeValue(focused_element, kAXValueAttribute, (CFTypeRef *)out_value);
    *out_element = focused_element; /* transfer ownership to caller */
}

/* Debounce for the padlock watcher -- without it, a padlock sheet
 * sitting on screen mid-swipe would re-arm on every 300ms tick. */
static atomic_bool g_padlock_prompt_already_armed = false;

/* Single poll, checked every 300ms on the main run loop. Checks each
 * candidate PID from get_auth_candidate_pids() (usually just one or
 * two) directly by PID, classifying any secure text field owned by a
 * system-auth process as a padlock. Clears the debounce flag when
 * nothing currently matches, so a resolved/cancelled prompt can re-arm
 * cleanly next time. */
static void auth_prompt_poll_callback(CFRunLoopTimerRef timer, void *info) {
    (void)timer; (void)info;
    int st = atomic_load(&g_state);

    /* Stuck-episode heal: a lock-screen episode whose screen is no longer
     * locked (e.g. logged in by typing after a cold boot, so no
     * screenIsUnlocked ever fired) would block swipe-to-lock forever. */
    static int not_locked_ticks = 0;
    if (st == STATE_POLLING && atomic_load(&g_trigger_source) == TRIGGER_LOCK_SCREEN) {
        if (!screen_is_locked_now()) {
            if (++not_locked_ticks >= 10) { /* ~3 s */
                not_locked_ticks = 0;
                print_timestamp();
                printf("Lock-screen episode but screen is not locked -> treating as unlocked.\n");
                on_screen_unlocked();
            }
        } else {
            not_locked_ticks = 0;
        }
        return;
    }
    not_locked_ticks = 0;

    /* Self-heal for Touch to Lock: arms again after the post-unlock wait, after a pause, or when
     * the setting was switched on while the screen was locked. About every 2 s. */
    static int heal_ticks = 0;
    if (st == STATE_IDLE && ++heal_ticks >= 7) { heal_ticks = 0; lockswipe_try_arm(); }

    if (st != STATE_IDLE && st != STATE_LOCK_ARMED) return; /* already mid-episode elsewhere */

    pid_t candidates[8];
    int candidate_count = get_auth_candidate_pids(candidates, 8);

    if (candidate_count == 0) {
        atomic_store(&g_padlock_prompt_already_armed, false);
        return;
    }

    for (int i = 0; i < candidate_count; i++) {
        pid_t pid = candidates[i];

        char proc_name_buf[256];
        char proc_path_buf[1024];
        bool got_name = get_process_name(pid, proc_name_buf, sizeof(proc_name_buf)) == 0;
        bool got_path = get_process_path(pid, proc_path_buf, sizeof(proc_path_buf)) == 0;
        bool is_auth_process = got_name && is_system_auth_process(proc_name_buf, got_path ? proc_path_buf : NULL);
        if (!is_auth_process) continue; /* not worth an AX round-trip at all */

        AXUIElementRef app = AXUIElementCreateApplication(pid);
        CFStringRef role = NULL, subrole = NULL, value = NULL;
        AXUIElementRef focused_element = NULL;
        get_focused_element_full(app, true /* always allow fallback -- already confirmed auth process */,
                                  &role, &subrole, &value, &focused_element);
        CFRelease(app);

        /* Secure fields in these SwiftUI-hosted sheets report
         * role=AXTextField, subrole=AXSecureTextField -- not
         * role=AXSecureTextField directly (confirmed via a full AX
         * tree dump). Check subrole; fall back to role for any other
         * context that genuinely uses it as the role. */
        bool is_secure_field = (subrole && CFStringCompare(subrole, CFSTR("AXSecureTextField"), 0) == kCFCompareEqualTo)
                                || (role && CFStringCompare(role, CFSTR("AXSecureTextField"), 0) == kCFCompareEqualTo);

        if (role) CFRelease(role);
        if (subrole) CFRelease(subrole);
        if (value) CFRelease(value);

        if (!is_secure_field) {
            if (focused_element) CFRelease(focused_element);
            continue;
        }

        /* Found a live padlock among the candidates. */
        if (!atomic_load(&g_padlock_prompt_already_armed)) {
            atomic_store(&g_padlock_prompt_already_armed, true);
            arm_polling_for_trigger(TRIGGER_PADLOCK, pid, focused_element, "System auth padlock");
        } else if (focused_element) {
            CFRelease(focused_element);
        }
        return; /* stop checking other candidates once we've found/confirmed one */
    }

    /* No candidate this tick matched a live padlock. */
    atomic_store(&g_padlock_prompt_already_armed, false);
}

static void notification_callback(CFNotificationCenterRef center,
                                   void *observer,
                                   CFStringRef name,
                                   const void *object,
                                   CFDictionaryRef userInfo) {
    (void)center; (void)observer; (void)object; (void)userInfo;
    if (CFStringCompare(name, CFSTR("com.apple.screenIsLocked"), 0) == kCFCompareEqualTo) {
        on_screen_locked();
    } else if (CFStringCompare(name, CFSTR("com.apple.screenIsUnlocked"), 0) == kCFCompareEqualTo) {
        on_screen_unlocked();
    }
}

/* ------------------------------------------------------------------ *
 * Polling thread: one sensor session per episode
 * ------------------------------------------------------------------ */

/* Ends the current episode without a match: back to IDLE, tell the menu bar. */
static void abandon_episode(const char *why) {
    print_timestamp();
    printf("%s\n", why);
    atomic_store(&g_trigger_source, TRIGGER_NONE);
    atomic_store(&g_failed_swipes, 0);
    if (g_trigger_secure_field) { CFRelease(g_trigger_secure_field); g_trigger_secure_field = NULL; }
    settle_to_idle();
    vfs5011_notify_swipe_failed();
}

/* The sensor said MATCH. Same safety checks and typing as the VFS5011 daemon. */
static void handle_match(void) {
    /* The password loads in parallel with the touch: wait for it (bounded). */
    for (int i = 0; i < 200 && !atomic_load(&g_password_ready) && atomic_load(&g_state) == STATE_POLLING; i++)
        usleep(50000);
    if (atomic_load(&g_state) != STATE_POLLING) return; /* the episode ended underneath us */

    char password_snapshot[MAX_PASSWORD_LEN + 1];
    int password_valid_snapshot;
    pthread_mutex_lock(&g_pw_lock);
    password_valid_snapshot = g_cached_password_valid;
    memcpy(password_snapshot, g_cached_password, sizeof(password_snapshot));
    pthread_mutex_unlock(&g_pw_lock);


            print_timestamp();
            printf("MATCH -- checking target is still focused before typing...\n");

            trigger_source_t source = atomic_load(&g_trigger_source);
            bool ok_to_type = true;

            if (source == TRIGGER_PADLOCK) {
                /* Re-verify right now, not at detection time -- capture
                 * takes a couple seconds, plenty of time for the user
                 * to have alt-tabbed away. Directly re-query the
                 * target PID (not "whatever's frontmost" -- there's no
                 * single reliable call for that anymore, see
                 * get_auth_candidate_pids' comment) and confirm it
                 * still shows a secure field. If the process is gone,
                 * or it no longer reports one (e.g. because it lost
                 * key window status when the user switched away),
                 * refuse to type. */
                AXUIElementRef target_app = AXUIElementCreateApplication(g_trigger_target_pid);
                CFStringRef role = NULL, subrole = NULL, value = NULL;
                AXUIElementRef elem = NULL;
                get_focused_element_full(target_app, true, &role, &subrole, &value, &elem);
                CFRelease(target_app);
                bool still_secure_field = (subrole && CFStringCompare(subrole, CFSTR("AXSecureTextField"), 0) == kCFCompareEqualTo)
                                           || (role && CFStringCompare(role, CFSTR("AXSecureTextField"), 0) == kCFCompareEqualTo);
                if (role) CFRelease(role);
                if (subrole) CFRelease(subrole);
                if (value) CFRelease(value);
                if (elem) CFRelease(elem);

                if (!still_secure_field) {
                    ok_to_type = false;
                    print_timestamp();
                    printf("ABORTED -- focus moved away from the target window since detection. Not typing.\n");
                }
            }

            if (ok_to_type) play_success_sound();

            if (ok_to_type && source == TRIGGER_PADLOCK && g_trigger_secure_field) {
                /* Belt-and-suspenders: explicitly focus the secure
                 * field before typing, in case something else in the
                 * dialog grabbed focus first. */
                AXUIElementSetAttributeValue(g_trigger_secure_field, kAXFocusedAttribute, kCFBooleanTrue);
                usleep(50000);
            }

            if (ok_to_type && password_valid_snapshot && source == TRIGGER_LOCK_SCREEN) {
                wait_for_lock_screen_settle();
                if (atomic_load(&g_state) != STATE_POLLING) ok_to_type = false; /* unlocked meanwhile */
            }

            if (ok_to_type && password_valid_snapshot) {
                type_password_and_enter(password_snapshot);
                print_timestamp();
                printf("Typed.\n");
                vfs5011_notify_swipe_success();
            } else if (ok_to_type) {
                print_timestamp();
                printf("Match succeeded but no stored password was available -- cannot auto-type.\n");
            }

            if (g_trigger_secure_field) { CFRelease(g_trigger_secure_field); g_trigger_secure_field = NULL; }
            atomic_store(&g_trigger_source, TRIGGER_NONE);

            /* Stop hammering the sensor until the next trigger. For
             * the lock screen specifically, on_screen_unlocked will
             * also fire and re-confirm IDLE -- harmless overlap. */
            settle_to_idle();

    memset(password_snapshot, 0, sizeof(password_snapshot));
}

static void handle_no_match(void) {
    play_failure_sound();
    int fails = atomic_fetch_add(&g_failed_swipes, 1) + 1;
    if (fails >= MAX_FAILED_TOUCHES) {
        print_timestamp();
        printf("Touch failed %d times in a row -> giving up, use the password.\n", fails);
        if (g_trigger_secure_field) { CFRelease(g_trigger_secure_field); g_trigger_secure_field = NULL; }
        atomic_store(&g_trigger_source, TRIGGER_NONE);
        atomic_store(&g_failed_swipes, 0);
        settle_to_idle();
        vfs5011_notify_swipe_lockout();
    } else {
        print_timestamp();
        printf("Touch not recognised (%d of %d).\n", fails, MAX_FAILED_TOUCHES);
        vfs5011_notify_swipe_failed();
    }
}

static void *polling_thread_main(void *arg) {
    (void)arg;
    bool session_open = false;
    int open_failures = 0;
    int error_streak = 0;
    metallica_mis_agent_set_abort_cb(abort_wait_cb);

    while (atomic_load(&g_running)) {
        /* A client yield is only confirmed once no session is held. */
        if (!session_open && atomic_exchange(&g_yield_ack_pending, false)) vfs5011_notify_yield_ready();

        int st = atomic_load(&g_state);
        bool want_session = (st == STATE_POLLING || st == STATE_LOCK_ARMED) && !yield_is_active();
        if (!want_session) {
            if (session_open) { metallica_mis_agent_close(); session_open = false; }
            open_failures = 0;
            error_streak = 0;
            usleep(200000);
            continue;
        }

        if (!session_open) {
            int rc = metallica_mis_agent_open();
            if (rc == 0) {
                if (metallica_mis_agent_prints() == 0) {
                    metallica_mis_agent_close();
                    if (st == STATE_LOCK_ARMED) { lockswipe_pause("no fingerprints are stored on the sensor", 300); continue; }
                    abandon_episode("No fingerprints are stored on the sensor -- enroll one with [1] Enroll first. Staying IDLE.");
                    continue;
                }
                session_open = true;
                open_failures = 0;
            } else if (rc == MMIS_SESSION_FAILED) {
                if (st == STATE_LOCK_ARMED) { lockswipe_pause("the sensor refused the session (host identity)", 300); continue; }
                abandon_episode("The sensor refused the session: the saved host identity does not match the one it was paired with. Run [2] Verify in the client once to enter it. Staying IDLE.");
                continue;
            } else {
                if (st == STATE_LOCK_ARMED && ++open_failures >= MAX_OPEN_FAILURES) {
                    lockswipe_pause("could not open a session with the sensor", 60);
                    open_failures = 0;
                } else if (st == STATE_LOCK_ARMED) {
                    usleep(1000000);
                } else if (++open_failures >= MAX_OPEN_FAILURES) {
                    abandon_episode("Could not open a session with the sensor several times in a row. Staying IDLE.");
                } else {
                    print_timestamp();
                    printf("Could not open the sensor session (attempt %d of %d), retrying...\n", open_failures, MAX_OPEN_FAILURES);
                    usleep(1000000);
                }
                continue;
            }
        }

        uint32_t user_id = 0;
        uint16_t subtype = 0;
        atomic_store(&g_wait_state, st);
        int r = metallica_mis_agent_identify(&user_id, &subtype);
        if (st == STATE_LOCK_ARMED) {
            /* Touch to Lock: a match locks the screen, anything else is ignored. If the state moved
             * on while waiting (the screen locked first), trigger_touch_lock() sees that and does nothing. */
            if (r == MMIS_AGENT_MATCH) {
                error_streak = 0;
                trigger_touch_lock();
            } else if (r == MMIS_AGENT_NO_MATCH || r == MMIS_AGENT_NO_FINGER) {
                error_streak = 0;
            } else {
                metallica_mis_agent_close();
                session_open = false;
                if (r == MMIS_AGENT_ERROR && ++error_streak >= MAX_OPEN_FAILURES) {
                    lockswipe_pause("the sensor kept returning errors", 60);
                    error_streak = 0;
                } else if (r == MMIS_AGENT_ERROR) {
                    usleep(500000);
                }
            }
            continue;
        }
        if (r == MMIS_AGENT_MATCH) {
            error_streak = 0;
            print_timestamp();
            printf("Sensor matched a stored print (user %u, finger %u).\n", (unsigned)user_id, (unsigned)subtype);
            handle_match();
        } else if (r == MMIS_AGENT_NO_MATCH) {
            error_streak = 0;
            handle_no_match();
        } else if (r == MMIS_AGENT_NO_FINGER) {
            error_streak = 0; /* nobody touched it in 30 s: keep waiting while the episode lasts */
        } else {
            /* MMIS_AGENT_ABORTED (episode ended / yield) or an error: the session must be closed and reopened. */
            metallica_mis_agent_close();
            session_open = false;
            if (r == MMIS_AGENT_ERROR && ++error_streak >= MAX_OPEN_FAILURES) {
                abandon_episode("The sensor kept returning errors. Staying IDLE.");
                error_streak = 0;
            } else if (r == MMIS_AGENT_ERROR) {
                usleep(500000);
            }
        }
    }
    if (session_open) metallica_mis_agent_close();
    return NULL;
}


/* ------------------------------------------------------------------ *
 * OpenCore version gate (v1.0.2 requirement)
 * ------------------------------------------------------------------ *
 * As of v1.0.2, this tool refuses to run unless it can confirm, via
 * NVRAM, that the system is booted on OpenCore 1.0.6 or newer (MINIMUM
 * only -- there is no ceiling, any version >= 1.0.6 passes, RELEASE or
 * DEBUG build both fine). This is a security floor: typing a login
 * password on the user's behalf isn't something this daemon should do
 * on a boot chain it can't verify meets a known-good baseline.
 *
 * OpenCore publishes its own version into NVRAM under its vendor GUID
 * (4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102), key "opencore-version" --
 * documented in the OpenCore Reference Manual's Debug Properties
 * section. Historically (confirmed across real-world OC installs
 * through at least the 0.6.x/0.7.x/0.8.x lines) this reports a string
 * like "REL-107-2025-08-01" (RELEASE build) or "DBG-107-2025-08-01"
 * (DEBUG build) -- the middle field is the version squished to 3
 * digits with no dots (0.6.6 -> "066", 1.0.6 -> "106").
 *
 * IMPORTANT / UNVERIFIED ON REAL 1.0.x HARDWARE: this project has not
 * yet empirically confirmed the exact NVRAM string OpenCore 1.0.x
 * itself reports, the way the auth-prompt watcher changes earlier in
 * this file WERE confirmed via ax_probe.c (see the "Passwords" /
 * "coreautha" findings). The 3-digit-code format above is what every
 * OC release through at least 0.8.x has used and nothing in OpenCore's
 * own changelog suggests it changed for 1.0.x, but before shipping,
 * run this on the real machine:
 *     nvram 4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:opencore-version
 * and confirm the output matches the format parsed below. If it
 * doesn't, this function fails CLOSED rather than silently accepting
 * an unrecognized value -- update the parser to match reality first,
 * same philosophy as the ax_probe-driven changes above.
 *
 * ALSO REQUIRED: OpenCore only publishes this variable at all if
 * config.plist's NVRAM -> ExposeSensitiveData bitmask includes the bit
 * that exposes opencore-version. Plenty of existing configs (including
 * ones from otherwise-correct guides) don't set this, which would make
 * this check fail closed on an up-to-date, perfectly fine OpenCore
 * install. Confirm this bit is set BEFORE relying on this gate -- see
 * the OpenCore Reference Manual's NVRAM Properties section for the
 * exact bitmask, and test with the `nvram` command above first.
 *
 * FAILURE MODE: this only refuses to run outright when the version
 * genuinely can't be determined at all (not booted via OpenCore, the
 * NVRAM variable is missing/unreadable, or the value doesn't parse) --
 * those indicate something is fundamentally broken, not just "old."
 * If a version WAS successfully read and it's simply below 1.0.6, this
 * prints a loud warning and lets the user proceed anyway -- they're on
 * their own for any issues that show up on 1.0.5 or older. */
#define REQUIRED_OC_VERSION_CODE 106 /* 1.0.6 minimum -- MAJOR*100+MINOR*10+PATCH, no ceiling */

/* Returns true if the daemon should proceed (version OK, or version
 * too low but the user's been warned), false only when the version
 * genuinely could not be determined at all. */
static bool check_opencore_version_requirement(void) {
    io_registry_entry_t options = IORegistryEntryFromPath(kIOMasterPortDefault, "IODeviceTree:/options");
    if (options == MACH_PORT_NULL) {
        fprintf(stderr, "OpenCore version check FAILED: could not open IODeviceTree:/options "
                        "(are you booted via OpenCore at all?). Refusing to run.\n");
        return false;
    }

    CFStringRef key = CFSTR("4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:opencore-version");
    CFTypeRef value = IORegistryEntryCreateCFProperty(options, key, kCFAllocatorDefault, 0);
    IOObjectRelease(options);

    if (!value) {
        fprintf(stderr, "OpenCore version check FAILED: \"opencore-version\" NVRAM variable not found.\n");
        fprintf(stderr, "This means either (a) you're not booted via OpenCore, or (b) your config.plist's\n");
        fprintf(stderr, "NVRAM -> ExposeSensitiveData bitmask doesn't expose this variable.\n");
        fprintf(stderr, "This tool requires OpenCore 1.0.6 or newer -- refusing to run.\n");
        return false;
    }

    char buf[128] = {0};
    bool got_string = false;

    if (CFGetTypeID(value) == CFDataGetTypeID()) {
        CFDataRef data = (CFDataRef)value;
        CFIndex len = CFDataGetLength(data);
        if (len > 0 && (size_t)len < sizeof(buf)) {
            CFDataGetBytes(data, CFRangeMake(0, len), (UInt8 *)buf);
            buf[len] = '\0';
            got_string = true;
        }
    } else if (CFGetTypeID(value) == CFStringGetTypeID()) {
        got_string = CFStringGetCString((CFStringRef)value, buf, sizeof(buf), kCFStringEncodingUTF8);
    }
    CFRelease(value);

    if (!got_string || buf[0] == '\0') {
        fprintf(stderr, "OpenCore version check FAILED: could not read \"opencore-version\" as text. Refusing to run.\n");
        return false;
    }

    /* Parse the 3-digit version code out of "REL-106-..." / "DBG-106-...".
     * Deliberately narrow (exact "-DDD-" or "-DDD" at end) rather than a
     * loose scan -- a loose match risks silently accepting a malformed
     * or unexpected string as if it passed. */
    int found_code = -1;
    size_t buf_len = strlen(buf);
    for (size_t i = 0; i + 4 <= buf_len; i++) {
        if (buf[i] == '-' && isdigit((unsigned char)buf[i+1]) && isdigit((unsigned char)buf[i+2])
            && isdigit((unsigned char)buf[i+3])
            && (i + 4 == buf_len || buf[i+4] == '-')) {
            found_code = (buf[i+1] - '0') * 100 + (buf[i+2] - '0') * 10 + (buf[i+3] - '0');
            break;
        }
    }

    if (found_code < 0) {
        fprintf(stderr, "OpenCore version check FAILED: could not parse a version out of \"%s\".\n", buf);
        fprintf(stderr, "Expected a format like \"REL-106-2025-08-01\". If OpenCore's NVRAM string format\n");
        fprintf(stderr, "has changed, this parser needs updating -- refusing to run rather than guess.\n");
        return false;
    }

    if (found_code < REQUIRED_OC_VERSION_CODE) {
        fprintf(stderr, "OpenCore v%d.%d.%d detected: Officially not supported, proceed at your own risk.\n",
                found_code / 100, (found_code / 10) % 10, found_code % 10);
        return true;
    }

    printf("OpenCore v%d.%d.%d detected: continue.\n",
           found_code / 100, (found_code / 10) % 10, found_code % 10);
    return true;
}


int main(int argc, char **argv) {
    /* Plain, unprivileged version query: no root re-exec, no OpenCore gate. */
    if (argc > 1 && strcmp(argv[1], "--version") == 0) {
        printf("%s\n", HT_AGENT_VERSION);
        return 0;
    }

    /* Line-buffered stdout even under launchd's file redirect, so `tail -f`
     * on the log shows output as it happens. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (geteuid() != 0) {
        fprintf(stderr, "Root privileges are required to access the USB device -- requesting via sudo...\n");
        char *sudo_argv[3];
        sudo_argv[0] = "sudo";
        sudo_argv[1] = argv[0];
        sudo_argv[2] = NULL;
        execvp("sudo", sudo_argv);
        fprintf(stderr, "Failed to re-exec with sudo: %s\n", strerror(errno));
        return 1;
    }

    init_exec_dir(argv[0]);

    if (!check_opencore_version_requirement()) {
        return 1;
    }

    load_saved_host_identity();

    printf("Metallica MIS authentication agent running -- idle until the screen locks.\n");
    printf("Ctrl+C to quit.\n\n");

    if (!metallica_sensor_is_present()) {
        printf("WARNING: No Metallica MIS sensor detected on this system. The agent\n");
        printf("will keep running and re-check on every lock/auth-prompt trigger,\n");
        printf("but it will stay idle until a sensor is plugged in.\n\n");
    }

    pthread_t poll_thread;
    if (pthread_create(&poll_thread, NULL, polling_thread_main, NULL) != 0) {
        fprintf(stderr, "Failed to start polling thread: %s\n", strerror(errno));
        return 1;
    }

    CFNotificationCenterRef center = CFNotificationCenterGetDistributedCenter();
    CFNotificationCenterAddObserver(center, NULL, notification_callback,
                                     CFSTR("com.apple.screenIsLocked"), NULL,
                                     CFNotificationSuspensionBehaviorDeliverImmediately);
    CFNotificationCenterAddObserver(center, NULL, notification_callback,
                                     CFSTR("com.apple.screenIsUnlocked"), NULL,
                                     CFNotificationSuspensionBehaviorDeliverImmediately);

    CFRunLoopTimerRef auth_prompt_timer = CFRunLoopTimerCreate(
        NULL, CFAbsoluteTimeGetCurrent(), 0.3 /* every 300ms */, 0, 0,
        auth_prompt_poll_callback, NULL);
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), auth_prompt_timer, kCFRunLoopDefaultMode);
    printf("Auth-prompt watcher registered (padlock, polling every 300ms).\n");

    vfs5011_set_lockswipe_handler(on_lockswipe_setting_changed);
    vfs5011_set_yield_handler(on_yield_request);
    vfs5011_menubar_ipc_init();
    touch_lock_publish(true);
    lockswipe_try_arm(); /* restores Touch to Lock after an agent restart */

    CFRunLoopRun();
    touch_lock_publish(false);

    atomic_store(&g_running, 0);
    pthread_join(poll_thread, NULL);
    return 0;
}
