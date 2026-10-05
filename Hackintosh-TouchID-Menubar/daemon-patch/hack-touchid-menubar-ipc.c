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

/* Swipe-to-lock (Tahoe feature). Same read-mostly reasoning as above.
 * The handler is set once before init and only called from the main
 * run loop, so it needs no locking either. */
static volatile bool g_lockswipe_enabled = false;
static void (*g_lockswipe_handler)(bool enabled) = NULL;

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

static bool lockswipe_flag_exists(void) {
    struct stat st;
    return stat(VFS5011_LOCKSWIPE_ENABLED_FLAG_PATH, &st) == 0;
}

static void write_lockswipe_flag(void) {
    FILE *f = fopen(VFS5011_LOCKSWIPE_ENABLED_FLAG_PATH, "w");
    if (f == NULL) {
        fprintf(stderr, "vfs5011: failed to write swipe-to-lock flag file: %s\n",
                strerror(errno));
        return;
    }
    fprintf(f, "enabled at %ld\n", (long)time(NULL));
    fclose(f);
    /* World-readable on purpose: the sandboxed Control Center control
     * reads this file (read-only) to show its current state. */
    chmod(VFS5011_LOCKSWIPE_ENABLED_FLAG_PATH, 0644);
}

static void remove_lockswipe_flag(void) {
    if (unlink(VFS5011_LOCKSWIPE_ENABLED_FLAG_PATH) != 0 && errno != ENOENT) {
        fprintf(stderr, "vfs5011: failed to remove swipe-to-lock flag file: %s\n",
                strerror(errno));
    }
}

/* --- Public: read current state --- */

bool vfs5011_lockswipe_is_enabled(void) {
    return g_lockswipe_enabled;
}

void vfs5011_set_lockswipe_handler(void (*handler)(bool enabled)) {
    g_lockswipe_handler = handler;
}

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

static void announce_lockswipe_state(void) {
    CFNotificationCenterPostNotification(
        CFNotificationCenterGetDistributedCenter(),
        g_lockswipe_enabled ? CFSTR(VFS5011_NOTIFY_LOCKSWIPE_ENABLED)
                             : CFSTR(VFS5011_NOTIFY_LOCKSWIPE_DISABLED),
        NULL, NULL, TRUE);
}

static void set_lockswipe_enabled(bool enabled) {
    if (g_lockswipe_enabled == enabled) {
        announce_lockswipe_state(); /* still confirm, so a UI that guessed wrong corrects itself */
        return;
    }
    g_lockswipe_enabled = enabled;
    if (enabled) write_lockswipe_flag(); else remove_lockswipe_flag();
    printf("vfs5011: swipe-to-lock %s via request\n", enabled ? "ENABLED" : "DISABLED");
    if (g_lockswipe_handler) g_lockswipe_handler(enabled);
    announce_lockswipe_state();
}

static void announce_current_state(void) {
    CFNotificationCenterPostNotification(
        CFNotificationCenterGetDistributedCenter(),
        g_scanning_enabled ? CFSTR(VFS5011_NOTIFY_SCANNING_ENABLED)
                            : CFSTR(VFS5011_NOTIFY_SCANNING_DISABLED),
        NULL, NULL, TRUE);
    announce_lockswipe_state();
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
    } else if (CFStringCompare(name, CFSTR(VFS5011_NOTIFY_REQUEST_LOCKSWIPE_ENABLE), 0) == kCFCompareEqualTo) {
        set_lockswipe_enabled(true);
    } else if (CFStringCompare(name, CFSTR(VFS5011_NOTIFY_REQUEST_LOCKSWIPE_DISABLE), 0) == kCFCompareEqualTo) {
        set_lockswipe_enabled(false);
    }
}

void vfs5011_menubar_ipc_init(void) {
    g_scanning_enabled = !flag_file_exists();
    g_lockswipe_enabled = lockswipe_flag_exists();

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
    CFNotificationCenterAddObserver(center, NULL, menubar_ipc_callback,
                                     CFSTR(VFS5011_NOTIFY_REQUEST_LOCKSWIPE_ENABLE), NULL,
                                     CFNotificationSuspensionBehaviorDeliverImmediately);
    CFNotificationCenterAddObserver(center, NULL, menubar_ipc_callback,
                                     CFSTR(VFS5011_NOTIFY_REQUEST_LOCKSWIPE_DISABLE), NULL,
                                     CFNotificationSuspensionBehaviorDeliverImmediately);

    printf("Menu bar IPC observers registered (scanning currently %s, swipe-to-lock %s).\n",
           g_scanning_enabled ? "enabled" : "disabled",
           g_lockswipe_enabled ? "on" : "off");
}
