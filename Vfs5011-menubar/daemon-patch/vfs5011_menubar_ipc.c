/*
 * vfs5011_menubar_ipc.c
 *
 * See vfs5011_menubar_ipc.h for the design notes. This file implements:
 *   - in-memory + on-disk scanning-enabled flag
 *   - a background dispatch source listening for menu bar requests
 *   - notify_post() wrappers for the three swipe events
 *
 * Link against libnotify (part of libSystem, no extra -l flag needed on
 * macOS) and Dispatch (also libSystem). No new build dependencies.
 */

#include "vfs5011_menubar_ipc.h"

#include <notify.h>
#include <dispatch/dispatch.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <syslog.h>

/* NOTE: this daemon is single-process and the flag is read far more often
 * than it's written (checked on every auth-prompt-detected event, written
 * only on user-initiated toggles), so a plain volatile bool with no extra
 * locking is an intentional, adequate choice here -- not an oversight.
 * If this daemon ever grows multiple worker threads that both read and
 * write the flag under real contention, revisit with a proper lock. */
static volatile bool g_scanning_enabled = true;

static int g_token_enable  = -1;
static int g_token_disable = -1;
static int g_token_restart = -1;
static int g_token_state   = -1;

/* --- Flag file persistence --- */

static bool flag_file_exists(void)
{
    struct stat st;
    return stat(VFS5011_SCANNING_DISABLED_FLAG_PATH, &st) == 0;
}

static void write_flag_file(void)
{
    FILE *f = fopen(VFS5011_SCANNING_DISABLED_FLAG_PATH, "w");
    if (f == NULL) {
        syslog(LOG_ERR, "vfs5011: failed to write scanning-disabled flag file: %s",
               strerror(errno));
        return;
    }
    /* Content is irrelevant -- presence is the signal. A timestamp is
     * written purely as a debugging aid if you ever `cat` the file. */
    fprintf(f, "disabled at %ld\n", (long)time(NULL));
    fclose(f);
}

static void remove_flag_file(void)
{
    if (unlink(VFS5011_SCANNING_DISABLED_FLAG_PATH) != 0 && errno != ENOENT) {
        syslog(LOG_ERR, "vfs5011: failed to remove scanning-disabled flag file: %s",
               strerror(errno));
    }
}

/* --- Public: read current state --- */

bool vfs5011_scanning_is_enabled(void)
{
    return g_scanning_enabled;
}

/* --- Public: event posts --- */

void vfs5011_notify_swipe_requested(void)
{
    notify_post(VFS5011_NOTIFY_SWIPE_REQUESTED);
}

void vfs5011_notify_swipe_success(void)
{
    notify_post(VFS5011_NOTIFY_SWIPE_SUCCESS);
}

void vfs5011_notify_swipe_failed(void)
{
    notify_post(VFS5011_NOTIFY_SWIPE_FAILED);
}

/* --- Internal: state transitions, always announce after changing --- */

static void set_scanning_enabled(bool enabled)
{
    g_scanning_enabled = enabled;

    if (enabled) {
        remove_flag_file();
        notify_post(VFS5011_NOTIFY_SCANNING_ENABLED);
        syslog(LOG_NOTICE, "vfs5011: fingerprint scanning ENABLED via menu bar request");
    } else {
        write_flag_file();
        notify_post(VFS5011_NOTIFY_SCANNING_DISABLED);
        syslog(LOG_NOTICE, "vfs5011: fingerprint scanning DISABLED via menu bar request");
    }
}

static void announce_current_state(void)
{
    notify_post(g_scanning_enabled
                    ? VFS5011_NOTIFY_SCANNING_ENABLED
                    : VFS5011_NOTIFY_SCANNING_DISABLED);
}

/*
 * "Restart Daemon" from the menu bar. This daemon is a root LaunchDaemon,
 * so the actual restart is expected to be driven externally via
 * `launchctl kickstart -k system/<your-daemon-label>` through the narrow
 * sudoers NOPASSWD rule the client already uses elsewhere -- the daemon
 * does not re-exec itself here. This handler exists as a hook in case you
 * want the daemon to do graceful pre-restart cleanup (flush state, close
 * the sensor handle cleanly) before launchd actually kills and relaunches
 * it. If no cleanup is needed, this can just log and return.
 */
static void handle_restart_request(void)
{
    syslog(LOG_NOTICE, "vfs5011: restart requested via menu bar "
                        "(expects external launchctl kickstart -k)");
    /* TODO: any pre-restart cleanup specific to your sensor handle /
     * VFSStore volume goes here, if needed. */
}

/* --- Darwin notification listener --- */

void vfs5011_menubar_ipc_init(void)
{
    /* Restore persisted state before registering listeners, so there's
     * no window where g_scanning_enabled is wrong. */
    g_scanning_enabled = !flag_file_exists();

    dispatch_queue_t q = dispatch_queue_create("com.vfs5011.hackintosh.menubaripc", NULL);

    notify_register_dispatch(VFS5011_NOTIFY_REQUEST_ENABLE, &g_token_enable, q,
        ^(int token) {
            (void)token;
            set_scanning_enabled(true);
        });

    notify_register_dispatch(VFS5011_NOTIFY_REQUEST_DISABLE, &g_token_disable, q,
        ^(int token) {
            (void)token;
            set_scanning_enabled(false);
        });

    notify_register_dispatch(VFS5011_NOTIFY_REQUEST_RESTART, &g_token_restart, q,
        ^(int token) {
            (void)token;
            handle_restart_request();
        });

    notify_register_dispatch(VFS5011_NOTIFY_REQUEST_STATE, &g_token_state, q,
        ^(int token) {
            (void)token;
            announce_current_state();
        });

    syslog(LOG_NOTICE, "vfs5011: menu bar IPC listener registered (scanning currently %s)",
           g_scanning_enabled ? "enabled" : "disabled");
}
