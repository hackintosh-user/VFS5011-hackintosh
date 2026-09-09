/*
 * hack-touchid-menubar-ipc.c
 *
 * See hack-touchid-menubar-ipc.h for design notes. Deliberately mirrors the
 * existing notification_callback()/CFNotificationCenterAddObserver
 * pattern already used in vfs5011_daemon.c for
 * com.apple.screenIsLocked/Unlocked -- same distributed notification
 * center, same registration style, registered on the main run loop
 * (no separate thread or dispatch queue needed).
 */

#include "hack-touchid-menubar-ipc.h"

#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

/* Read far more often (every arm_polling_for_trigger() call) than
 * written (only on a user-initiated toggle from the menu bar), so a
 * plain volatile bool with no extra locking is an intentional, adequate
 * choice here -- consistent with this file's other atomic/plain-global
 * state (g_state, g_trigger_source use atomic_int; this one doesn't
 * need that strength since it's read-mostly and eventual consistency
 * across the ~300ms poll tick is fine). */
static volatile bool g_scanning_enabled = true;

/* --- Flag file persistence --- */

static bool flag_file_exists(void) {
    struct stat st;
    return stat(VFS5011_SCANNING_DISABLED_FLAG_PATH, &st) == 0;
}

static void write_flag_file(void) {
    FILE *f = fopen(VFS5011_SCANNING_DISABLED_FLAG_PATH, "w");
    if (f == NULL) {
        fprintf(stderr, "vfs5011: failed to write scanning-disabled flag file: %s\n",
                strerror(errno));
        return;
    }
    fprintf(f, "disabled at %ld\n", (long)time(NULL));
    fclose(f);
}

static void remove_flag_file(void) {
    if (unlink(VFS5011_SCANNING_DISABLED_FLAG_PATH) != 0 && errno != ENOENT) {
        fprintf(stderr, "vfs5011: failed to remove scanning-disabled flag file: %s\n",
                strerror(errno));
    }
}

/* --- Public: read current state --- */

bool vfs5011_scanning_is_enabled(void) {
    return g_scanning_enabled;
}

/* --- Public: event posts --- */

void vfs5011_notify_swipe_requested(void) {
    CFNotificationCenterPostNotification(CFNotificationCenterGetDistributedCenter(),
                                          CFSTR(VFS5011_NOTIFY_SWIPE_REQUESTED),
                                          NULL, NULL, TRUE);
}

void vfs5011_notify_swipe_success(void) {
    CFNotificationCenterPostNotification(CFNotificationCenterGetDistributedCenter(),
                                          CFSTR(VFS5011_NOTIFY_SWIPE_SUCCESS),
                                          NULL, NULL, TRUE);
}

void vfs5011_notify_swipe_failed(void) {
    CFNotificationCenterPostNotification(CFNotificationCenterGetDistributedCenter(),
                                          CFSTR(VFS5011_NOTIFY_SWIPE_FAILED),
                                          NULL, NULL, TRUE);
}

/* --- Internal: state transitions, always announce after changing --- */

static void set_scanning_enabled(bool enabled) {
    g_scanning_enabled = enabled;

    CFNotificationCenterRef center = CFNotificationCenterGetDistributedCenter();

    if (enabled) {
        remove_flag_file();
        CFNotificationCenterPostNotification(center, CFSTR(VFS5011_NOTIFY_SCANNING_ENABLED),
                                              NULL, NULL, TRUE);
        printf("vfs5011: fingerprint scanning ENABLED via menu bar request\n");
    } else {
        write_flag_file();
        CFNotificationCenterPostNotification(center, CFSTR(VFS5011_NOTIFY_SCANNING_DISABLED),
                                              NULL, NULL, TRUE);
        printf("vfs5011: fingerprint scanning DISABLED via menu bar request\n");
    }
}

static void announce_current_state(void) {
    CFNotificationCenterPostNotification(
        CFNotificationCenterGetDistributedCenter(),
        g_scanning_enabled ? CFSTR(VFS5011_NOTIFY_SCANNING_ENABLED)
                            : CFSTR(VFS5011_NOTIFY_SCANNING_DISABLED),
        NULL, NULL, TRUE);
}

/*
 * "Restart Daemon" from the menu bar. This daemon is launched via sudo
 * re-exec (see main()'s geteuid() check), not a real installed
 * LaunchDaemon per the top-of-file comment -- so there's no
 * `launchctl kickstart` target yet. For now this just logs; wire up
 * whatever your actual restart mechanism ends up being once this
 * daemon is packaged as a real LaunchDaemon; see the top-of-file
 * comment block for that context.
 */
static void handle_restart_request(void) {
    printf("vfs5011: restart requested via menu bar (no-op for now -- "
           "see hack-touchid-menubar-ipc.c handle_restart_request comment)\n");
}

/* --- Distributed notification callback (mirrors notification_callback) --- */

static void menubar_ipc_callback(CFNotificationCenterRef center,
                                  void *observer,
                                  CFStringRef name,
                                  const void *object,
                                  CFDictionaryRef userInfo) {
    (void)center; (void)observer; (void)object; (void)userInfo;

    if (CFStringCompare(name, CFSTR(VFS5011_NOTIFY_REQUEST_ENABLE), 0) == kCFCompareEqualTo) {
        set_scanning_enabled(true);
    } else if (CFStringCompare(name, CFSTR(VFS5011_NOTIFY_REQUEST_DISABLE), 0) == kCFCompareEqualTo) {
        set_scanning_enabled(false);
    } else if (CFStringCompare(name, CFSTR(VFS5011_NOTIFY_REQUEST_RESTART), 0) == kCFCompareEqualTo) {
        handle_restart_request();
    } else if (CFStringCompare(name, CFSTR(VFS5011_NOTIFY_REQUEST_STATE), 0) == kCFCompareEqualTo) {
        announce_current_state();
    }
}

void vfs5011_menubar_ipc_init(void) {
    g_scanning_enabled = !flag_file_exists();

    CFNotificationCenterRef center = CFNotificationCenterGetDistributedCenter();
    CFNotificationCenterAddObserver(center, NULL, menubar_ipc_callback,
                                     CFSTR(VFS5011_NOTIFY_REQUEST_ENABLE), NULL,
                                     CFNotificationSuspensionBehaviorDeliverImmediately);
    CFNotificationCenterAddObserver(center, NULL, menubar_ipc_callback,
                                     CFSTR(VFS5011_NOTIFY_REQUEST_DISABLE), NULL,
                                     CFNotificationSuspensionBehaviorDeliverImmediately);
    CFNotificationCenterAddObserver(center, NULL, menubar_ipc_callback,
                                     CFSTR(VFS5011_NOTIFY_REQUEST_RESTART), NULL,
                                     CFNotificationSuspensionBehaviorDeliverImmediately);
    CFNotificationCenterAddObserver(center, NULL, menubar_ipc_callback,
                                     CFSTR(VFS5011_NOTIFY_REQUEST_STATE), NULL,
                                     CFNotificationSuspensionBehaviorDeliverImmediately);

    printf("Menu bar IPC observers registered (scanning currently %s).\n",
           g_scanning_enabled ? "enabled" : "disabled");
}
