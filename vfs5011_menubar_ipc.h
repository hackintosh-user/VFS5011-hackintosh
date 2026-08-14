/*
 * vfs5011_menubar_ipc.h
 *
 * Daemon-side glue for the optional VFS5011 menu bar companion app.
 * #include this from vfs5011_daemon.c.
 *
 * REVISED: uses CFNotificationCenterGetDistributedCenter(), the exact
 * same mechanism vfs5011_daemon.c already uses for
 * com.apple.screenIsLocked / com.apple.screenIsUnlocked. That pairing is
 * already proven in this codebase to cross the root-daemon /
 * user-session boundary correctly -- reusing it here instead of the
 * lower-level Darwin `notify` API means no new cross-UID behavior to
 * verify, and it matches the existing code style
 * (notification_callback / CFNotificationCenterAddObserver in main()).
 *
 * IMPORTANT: distributed notifications carry userInfo only within a
 * single UID's session reliably; across the root/user boundary (as
 * here), don't rely on it -- exactly like the existing
 * screenIsLocked/Unlocked handling, every event and request below is
 * its own distinct notification name rather than one name + a payload.
 * Keep these strings byte-for-byte in sync with VFS5011Notification in
 * AppDelegate.swift.
 */

#ifndef VFS5011_MENUBAR_IPC_H
#define VFS5011_MENUBAR_IPC_H

#include <stdbool.h>

/* --- Notification names (must match AppDelegate.swift exactly) --- */

#define VFS5011_NOTIFY_PREFIX "com.vfs5011.hackintosh"

/* Daemon -> menu bar app (events) */
#define VFS5011_NOTIFY_SWIPE_REQUESTED   VFS5011_NOTIFY_PREFIX ".swipe_requested"
#define VFS5011_NOTIFY_SWIPE_SUCCESS     VFS5011_NOTIFY_PREFIX ".swipe_success"
#define VFS5011_NOTIFY_SWIPE_FAILED      VFS5011_NOTIFY_PREFIX ".swipe_failed"

/* Daemon -> menu bar app (state confirmation) */
#define VFS5011_NOTIFY_SCANNING_ENABLED  VFS5011_NOTIFY_PREFIX ".scanning_enabled"
#define VFS5011_NOTIFY_SCANNING_DISABLED VFS5011_NOTIFY_PREFIX ".scanning_disabled"

/* Menu bar app -> daemon (requests) */
#define VFS5011_NOTIFY_REQUEST_ENABLE    VFS5011_NOTIFY_PREFIX ".request_enable"
#define VFS5011_NOTIFY_REQUEST_DISABLE   VFS5011_NOTIFY_PREFIX ".request_disable"
#define VFS5011_NOTIFY_REQUEST_RESTART   VFS5011_NOTIFY_PREFIX ".request_restart"
#define VFS5011_NOTIFY_REQUEST_STATE     VFS5011_NOTIFY_PREFIX ".request_state_announce"

/*
 * Path to the persisted pause/resume flag. Presence of this file means
 * scanning is paused. Survives daemon restarts since the on-disk state
 * is the source of truth -- the in-memory flag is just a fast-path
 * cache of it. Adjust if you keep daemon state elsewhere.
 */
#define VFS5011_SCANNING_DISABLED_FLAG_PATH \
    "/Library/Application Support/VFS5011/scanning_disabled"

/*
 * Call once from main(), after the screenIsLocked/Unlocked observers
 * are registered and before CFRunLoopRun(). Registers observers on the
 * distributed notification center for the four "request_*" names, and
 * restores the in-memory scanning flag from the on-disk flag file so a
 * daemon restart doesn't forget a pause.
 */
void vfs5011_menubar_ipc_init(void);

/*
 * Returns true if arm_polling_for_trigger() should proceed. Call this
 * as the very first line of arm_polling_for_trigger(), before any
 * printf/template loading/state change -- if false, log and return
 * immediately, exactly like the existing "no templates available"
 * early-return right below it.
 */
bool vfs5011_scanning_is_enabled(void);

/* Fire-and-forget event posts. */
void vfs5011_notify_swipe_requested(void); /* after successfully entering STATE_POLLING */
void vfs5011_notify_swipe_success(void);   /* after a match is confirmed AND typed */
void vfs5011_notify_swipe_failed(void);    /* after a real captured swipe scores below MATCH_THRESHOLD */

#endif /* VFS5011_MENUBAR_IPC_H */
