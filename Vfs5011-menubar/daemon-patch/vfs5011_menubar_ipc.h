/*
 * vfs5011_menubar_ipc.h
 *
 * Daemon-side glue for the optional VFS5011 menu bar companion app.
 * Drop into the same directory as vfs5011_daemon.c and #include it there.
 *
 * Uses the low-level Darwin `notify` API (<notify.h>), which is the same
 * underlying mechanism behind CFNotificationCenterGetDarwinNotifyCenter()
 * on the Swift side -- it's the lighter-weight C entry point, no
 * CoreFoundation runloop bookkeeping required.
 *
 * IMPORTANT: notify_post() carries NO payload -- only the notification
 * name crosses the process boundary. Every event and request below is
 * therefore its own distinct name. Keep these strings byte-for-byte in
 * sync with VFS5011Notification in AppDelegate.swift.
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
 * scanning is paused. Survives daemon restarts (including the "Restart
 * Daemon" menu action) since the on-disk state is the source of truth --
 * the in-memory flag is just a fast-path cache of it.
 *
 * Adjust to match wherever VFS5011's other state already lives
 * (e.g. alongside VFSStore's mount point config) rather than introducing
 * a new location.
 */
#define VFS5011_SCANNING_DISABLED_FLAG_PATH \
    "/Library/Application Support/VFS5011/scanning_disabled"

/*
 * Call once at daemon startup, after other initialization. Starts a
 * background dispatch listener for the three "request_*" notifications
 * and restores the in-memory scanning flag from the on-disk flag file
 * (so a daemon restart doesn't silently forget it was paused).
 */
void vfs5011_menubar_ipc_init(void);

/*
 * Returns true if a fingerprint auth-prompt handler should proceed with
 * waking the sensor. Call this at the very top of the auth-prompt-detected
 * handler, before touching the sensor -- if false, return immediately
 * without posting SWIPE_REQUESTED and without any sensor I/O.
 */
bool vfs5011_scanning_is_enabled(void);

/* Fire-and-forget event posts -- call at the corresponding points in the
 * existing auth-prompt-detected / verify flow. */
void vfs5011_notify_swipe_requested(void);
void vfs5011_notify_swipe_success(void);
void vfs5011_notify_swipe_failed(void);

#endif /* VFS5011_MENUBAR_IPC_H */
