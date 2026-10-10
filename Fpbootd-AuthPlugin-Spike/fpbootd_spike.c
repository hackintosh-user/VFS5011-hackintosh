/*
 * fpbootd_spike.c
 *
 * STAGE C -- real fingerprint verification at the login screen.
 * Builds on Stage A (socket reachability, harness-proven) and Stage B
 * (real registration into system.login.console, proven end-to-end on
 * real hardware Sep 22). This stage replaces the diagnostic PING with
 * a real CAPTURE call to the fpbootd daemon. On a genuine match it
 * speaks a confirmation, and if loginwindow has already put a username
 * in the authorization context it also offers the stored password to
 * the later builtin:authenticate step via SetContextValue.
 *
 * WHAT THIS DOES NOT DO: skip the password field. Tried at two
 * mechanism positions (Sep 23), both real-hardware tested: after
 * loginwindow:login the password field is already drawn and blocking,
 * and before it, injecting username and password into the context
 * succeeded but loginwindow:login drew its prompt anyway. A third-party
 * plugin cannot preempt Apple's login UI this way, so the compiled-in
 * username experiment was removed. The real feature is audible
 * fingerprint verification that always falls through to the normal
 * password prompt.
 *
 * THE SAFETY GUARANTEE IS UNCHANGED FROM STAGE A/B: this mechanism
 * ALWAYS calls SetResult(kAuthorizationResultAllow), on every single
 * code path -- match, no match, timeout, socket error, missing
 * username context, anything. Allow does not mean "you're logged
 * in" -- it means "this mechanism is done, move to the next one in
 * the array." The actual login decision is still made by
 * builtin:authenticate,privileged further down the chain. This
 * mechanism cannot deny a login and cannot remove the ability to type
 * a password.
 *
 * Real-hardware findings that shaped the current design:
 *  - Position 0 (first mechanism) stalled the boot progress bar until
 *    a swipe resolved, since it blocked before the login screen even
 *    rendered. The install script places this mechanism right after
 *    builtin:auto-login,privileged instead.
 *  - CAPTURE_TIMEOUT_SEC was 12s, too short (a real match completed
 *    but arrived after this code had already timed out and closed the
 *    socket); 25s is the client-side ceiling, and real round trips
 *    complete well inside it (~8.7s observed). The daemon's own swipe
 *    deadline is 6s, so a no-swipe case falls through in ~6-7s.
 *  - Retry budget: up to MAX_CAPTURE_ATTEMPTS swipes per login screen
 *    appearance, but only continues the loop on an explicit NOMATCH (a
 *    real swipe that didn't match) -- a timeout/no-swipe or a daemon
 *    ERROR stops immediately rather than compounding the wait for a
 *    boot where nobody's using the sensor at all.
 *  - Spoken (not on-screen) feedback via /usr/bin/say, fired off async
 *    so it can never block this mechanism's own timeline. Real
 *    on-screen text needs a separate SFAuthorizationPluginView UI
 *    mechanism -- out of scope here, see the note by speak_async().
 *
 * Security note: the daemon's CAPTURE reply is MATCH:<label>:<password>
 * -- the raw password flows through this process's memory and a local
 * socket read buffer. syslog() calls in this file are audited to NEVER
 * include the reply/password contents, only outcome labels
 * (MATCH/NOMATCH/ERROR/timeout) and the finger label. Buffers holding
 * the password are wiped with secure_wipe() as soon as they are handed
 * off or discarded.
 */

#include <Security/Authorization.h>
#include <Security/AuthorizationPlugin.h>
#include <CoreFoundation/CoreFoundation.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <syslog.h>
#include <time.h>

#define FPBOOTD_SOCKET_PATH "/var/run/fpbootd.sock"
#define CONNECT_TIMEOUT_SEC 2
/* Sep 22 real-hardware finding: 12s was too short. The daemon's own
 * success sound played (a real match happened) but our read timed
 * out before the reply arrived -- real swipe+match latency exceeds
 * an instant PING round trip by more than expected. Bumped to 25s
 * and now logging elapsed time on a timeout so a future tune (if
 * still needed) is based on a real number instead of another guess. */
#define CAPTURE_TIMEOUT_SEC 25
#define CAPTURE_REPLY_BUFSZ 512  /* label + password both live here; passwords can be long, keep generous */

/* Same literal strings Apple's AuthorizationTags.h defines as
 * kAuthorizationEnvironmentUsername / kAuthorizationEnvironmentPassword.
 * Using the literals directly rather than pulling in AuthorizationTags.h,
 * since those constants are documented for the AuthorizationEnvironment
 * array (AuthorizationCopyRights) rather than mechanism context values --
 * but the same key strings are the established convention other
 * third-party login mechanisms use for this exact handoff. */
#define FPBOOTD_CTX_USERNAME "username"
#define FPBOOTD_CTX_PASSWORD "password"

/* Sep 22: "4 attempts" is scoped to ONE appearance of the login screen
 * (one MechanismInvoke call), not persisted across boots/logouts -- no
 * state file, no cross-process bookkeeping. The loop below only keeps
 * retrying on an explicit NOMATCH (a real swipe happened and didn't
 * match). On FAILED (timeout/no swipe at all) or ERROR it stops
 * immediately instead of consuming the rest of the budget -- otherwise
 * a boot where nobody touches the sensor would wait
 * MAX_CAPTURE_ATTEMPTS * CAPTURE_TIMEOUT_SEC (up to 100s) instead of
 * just falling through once, the same unresolved cost already flagged
 * for a single attempt but multiplied. */
#define MAX_CAPTURE_ATTEMPTS 4

/* memset() on a buffer that is about to go out of scope can be removed
 * by the optimizer as a dead store. Writing through a volatile pointer
 * cannot, so password material is actually cleared. */
static void secure_wipe(void *p, size_t n) {
    volatile unsigned char *v = (volatile unsigned char *)p;
    while (n--) *v++ = 0;
}

/* Real on-screen text (a "Fingerprint unlock disabled" banner drawn in
 * the login window itself) needs a whole separate UI mechanism built
 * on SFAuthorizationPluginView -- its own windowed view class hooked
 * into the mechanism, not a hint string. Apple's own developer forums
 * show this is thinly documented and reportedly unreliable even for
 * Apple's own use (e.g. rendering behind the background in fast-user-
 * switching). Not attempted here. Spoken feedback via /usr/bin/say is
 * used instead -- genuinely reaches the user without that scope. */
static void speak_async(const char *text) {
    pid_t pid = fork();
    if (pid == 0) {
        /* child: replace image entirely, never returns to plugin code */
        execl("/usr/bin/say", "say", text, (char *)NULL);
        _exit(127); /* only reached if execl itself failed */
    }
    /* parent: deliberately no waitpid() -- speech must never block
     * MechanismInvoke's own timeline. The child is reparented to init
     * on exit; no zombie risk worth guarding since this process's
     * lifetime is short (one mechanism invocation). */
}

/* --- Plugin-level state: one instance for the whole loaded plugin --- */
typedef struct FpbootdSpikePlugin {
    const AuthorizationCallbacks *callbacks;
} FpbootdSpikePlugin;

/* --- Per-mechanism state: one instance per mechanism invocation slot --- */
typedef struct FpbootdSpikeMechanism {
    FpbootdSpikePlugin *plugin;
    AuthorizationEngineRef engine;
} FpbootdSpikeMechanism;

typedef enum {
    CAPTURE_RESULT_MATCH,
    CAPTURE_RESULT_NOMATCH,
    CAPTURE_RESULT_ERROR,      /* daemon replied ERROR:<reason>, or an unrecognized reply */
    CAPTURE_RESULT_FAILED      /* connect/write/read/timeout -- never reached the daemon meaningfully */
} CaptureResult;

/* Connects to FPBOOTD_SOCKET_PATH with a hard connect timeout, sends
 * "CAPTURE\n", reads up to outlen-1 bytes of the reply with its own
 * hard timeout, NUL-terminates into out. Same non-blocking
 * connect()+select() pattern as Stage A/B -- this is the one property
 * that must never regress, since a hang here (not a wrong answer) is
 * the only way this mechanism could ever actually delay/block a real
 * login. Returns CAPTURE_RESULT_FAILED on any transport-level failure;
 * out is only meaningful when it returns something else. */
static CaptureResult capture_fpbootd(char *out, size_t outlen) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        syslog(LOG_NOTICE, "fpbootd-spike: socket() failed: %s", strerror(errno));
        return CAPTURE_RESULT_FAILED;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", FPBOOTD_SOCKET_PATH);

    int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc != 0 && errno != EINPROGRESS) {
        syslog(LOG_NOTICE, "fpbootd-spike: connect() failed immediately: %s", strerror(errno));
        close(fd);
        return CAPTURE_RESULT_FAILED;
    }

    if (rc != 0) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        struct timeval tv = { .tv_sec = CONNECT_TIMEOUT_SEC, .tv_usec = 0 };

        rc = select(fd + 1, NULL, &wfds, NULL, &tv);
        if (rc <= 0) {
            syslog(LOG_NOTICE, "fpbootd-spike: connect() timed out or select() failed (rc=%d)", rc);
            close(fd);
            return CAPTURE_RESULT_FAILED;
        }
        int so_error = 0;
        socklen_t so_error_len = sizeof(so_error);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len);
        if (so_error != 0) {
            syslog(LOG_NOTICE, "fpbootd-spike: connect() completed with error: %s", strerror(so_error));
            close(fd);
            return CAPTURE_RESULT_FAILED;
        }
    }

    syslog(LOG_NOTICE, "fpbootd-spike: connected to %s, sending CAPTURE", FPBOOTD_SOCKET_PATH);

    time_t sent_at = time(NULL);
    const char *msg = "CAPTURE\n";
    if (write(fd, msg, strlen(msg)) < 0) {
        syslog(LOG_NOTICE, "fpbootd-spike: write() failed: %s", strerror(errno));
        close(fd);
        return CAPTURE_RESULT_FAILED;
    }

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    struct timeval rtv = { .tv_sec = CAPTURE_TIMEOUT_SEC, .tv_usec = 0 };
    rc = select(fd + 1, &rfds, NULL, NULL, &rtv);
    if (rc <= 0) {
        syslog(LOG_NOTICE, "fpbootd-spike: CAPTURE read timed out after %lds waiting for reply (rc=%d)",
               (long)(time(NULL) - sent_at), rc);
        close(fd);
        return CAPTURE_RESULT_FAILED;
    }

    ssize_t n = read(fd, out, outlen - 1);
    close(fd);
    if (n <= 0) {
        syslog(LOG_NOTICE, "fpbootd-spike: read() failed or EOF: %s", strerror(errno));
        return CAPTURE_RESULT_FAILED;
    }
    out[n] = '\0';
    char *nl = strchr(out, '\n');
    if (nl) *nl = '\0';

    /* Never log `out` itself past this point -- it may contain the
     * real account password. Only ever log derived, non-secret facts
     * about it (which branch we took, the finger label). */
    if (strncmp(out, "MATCH:", 6) == 0) return CAPTURE_RESULT_MATCH;
    if (strcmp(out, "NOMATCH") == 0) return CAPTURE_RESULT_NOMATCH;
    if (strncmp(out, "ERROR:", 6) == 0) return CAPTURE_RESULT_ERROR;
    syslog(LOG_NOTICE, "fpbootd-spike: unrecognized reply format from daemon");
    return CAPTURE_RESULT_ERROR;
}

/* Splits a "MATCH:<label>:<password>" line (already known to start
 * with "MATCH:") into label and password, splitting on the FIRST two
 * colons only -- the password itself is taken as everything after the
 * second colon verbatim, so a password containing colons is never
 * truncated. label_out/password_out point INTO reply (no copy); both
 * are only valid as long as reply is. Returns 0 on success, -1 if the
 * format doesn't have a second colon at all (malformed). */
static int split_match_reply(char *reply, char **label_out, char **password_out) {
    char *after_match = reply + 6; /* skip "MATCH:" */
    char *second_colon = strchr(after_match, ':');
    if (!second_colon) return -1;
    *second_colon = '\0';
    *label_out = after_match;
    *password_out = second_colon + 1;
    return 0;
}

/* --- AuthorizationPluginInterface implementation --- */

/* Handles a MATCH reply: parses label/password, and if loginwindow has
 * already put a username in the authorization context, offers the
 * password to builtin:authenticate via SetContextValue. Never injects a
 * username of its own. Returns 1 if the password context was actually
 * set, 0 otherwise (malformed reply, no username in context yet, or the
 * SetContextValue call failed) -- the return value only drives which
 * spoken message plays, it has no bearing on SetResult, which stays
 * Allow regardless either way. */
static int handle_match(FpbootdSpikeMechanism *mech, char *reply) {
    char *label = NULL, *password = NULL;
    if (split_match_reply(reply, &label, &password) != 0) {
        syslog(LOG_NOTICE, "fpbootd-spike: MATCH reply malformed, no second colon -- falling through");
        return 0;
    }
    syslog(LOG_NOTICE, "fpbootd-spike: match (%s) -- checking for an existing username context", label);

    int handed_off = 0;
    AuthorizationContextFlags userFlags = 0;
    const AuthorizationValue *userVal = NULL;
    OSStatus getRc = mech->plugin->callbacks->GetContextValue(
        mech->engine, FPBOOTD_CTX_USERNAME, &userFlags, &userVal);

    if (getRc == noErr && userVal && userVal->data && userVal->length > 0) {
        AuthorizationValue passVal;
        passVal.length = strlen(password);
        passVal.data = password;

        OSStatus setRc = mech->plugin->callbacks->SetContextValue(
            mech->engine, FPBOOTD_CTX_PASSWORD,
            kAuthorizationContextFlagExtractable, &passVal);

        if (setRc == noErr) {
            syslog(LOG_NOTICE, "fpbootd-spike: password context set for builtin:authenticate to consume");
            handed_off = 1;
        } else {
            syslog(LOG_NOTICE, "fpbootd-spike: SetContextValue(password) failed, status=%d -- falling through", (int)setRc);
        }
    } else {
        syslog(LOG_NOTICE, "fpbootd-spike: no username in context yet -- falling through");
    }

    /* Wipe the password out of the reply buffer immediately -- it's
     * served its purpose (or failed to), no reason to leave it sitting
     * in memory any longer than necessary. */
    secure_wipe(password, strlen(password));
    return handed_off;
}

static OSStatus FpbootdSpikeMechanismInvoke(AuthorizationMechanismRef inMechanism) {
    FpbootdSpikeMechanism *mech = (FpbootdSpikeMechanism *)inMechanism;

    openlog("fpbootd-spike", LOG_PID, LOG_AUTH);
    syslog(LOG_NOTICE, "fpbootd-spike: MechanismInvoke starting (Stage C -- real capture, up to %d attempts)", MAX_CAPTURE_ATTEMPTS);

    int matched_and_handed_off = 0;

    for (int attempt = 1; attempt <= MAX_CAPTURE_ATTEMPTS; attempt++) {
        char reply[CAPTURE_REPLY_BUFSZ] = {0};
        CaptureResult result = capture_fpbootd(reply, sizeof(reply));

        if (result == CAPTURE_RESULT_MATCH) {
            syslog(LOG_NOTICE, "fpbootd-spike: attempt %d/%d: match", attempt, MAX_CAPTURE_ATTEMPTS);
            matched_and_handed_off = handle_match(mech, reply);
            speak_async(matched_and_handed_off ? "Fingerprint accepted" : "Fingerprint recognized");
            secure_wipe(reply, sizeof(reply));
            break;
        }

        secure_wipe(reply, sizeof(reply));

        if (result == CAPTURE_RESULT_NOMATCH) {
            syslog(LOG_NOTICE, "fpbootd-spike: attempt %d/%d: no match", attempt, MAX_CAPTURE_ATTEMPTS);
            if (attempt < MAX_CAPTURE_ATTEMPTS) {
                speak_async("Try again");
                continue; /* a real swipe happened and failed -- worth another try */
            }
            syslog(LOG_NOTICE, "fpbootd-spike: %d failed attempts reached -- falling through to password", MAX_CAPTURE_ATTEMPTS);
            speak_async("Fingerprint unlock disabled, use password");
            break;
        }

        /* CAPTURE_RESULT_ERROR or CAPTURE_RESULT_FAILED -- daemon
         * error, or no swipe/timeout/transport failure. Deliberately
         * NOT retried: retrying against a sensor that isn't
         * responding, or when nobody's using it this time, would only
         * multiply the wait for no benefit. Stop now and fall through
         * silently (no spoken message -- this path also covers "user
         * never intended to use the sensor," where a message would be
         * noise, not help). */
        syslog(LOG_NOTICE, "fpbootd-spike: attempt %d/%d: %s -- not retrying, falling through",
               attempt, MAX_CAPTURE_ATTEMPTS, result == CAPTURE_RESULT_ERROR ? "daemon error" : "capture failed/timeout");
        break;
    }

    /* THE critical line, unchanged from Stage A/B/C so far. No matter
     * what happened above -- match, no match after N attempts, error,
     * transport failure, a context-value set that failed -- this
     * mechanism always reports Allow. It never decides the login on
     * its own; it only ever hands builtin:authenticate a shortcut to
     * try, or doesn't. */
    OSStatus setResultStatus = mech->plugin->callbacks->SetResult(mech->engine, kAuthorizationResultAllow);
    if (setResultStatus != noErr) {
        syslog(LOG_NOTICE, "fpbootd-spike: SetResult() itself returned %d", (int)setResultStatus);
    }

    closelog();
    return noErr;
}

static OSStatus FpbootdSpikeMechanismDeactivate(AuthorizationMechanismRef inMechanism) {
    FpbootdSpikeMechanism *mech = (FpbootdSpikeMechanism *)inMechanism;
    return mech->plugin->callbacks->DidDeactivate(mech->engine);
}

static OSStatus FpbootdSpikeMechanismDestroy(AuthorizationMechanismRef inMechanism) {
    free(inMechanism);
    return noErr;
}

static OSStatus FpbootdSpikeMechanismCreate(AuthorizationPluginRef inPlugin,
                                             AuthorizationEngineRef inEngine,
                                             AuthorizationMechanismId mechanismId,
                                             AuthorizationMechanismRef *outMechanism) {
    (void)mechanismId; /* only one mechanism exported by this bundle -- ID unused */
    FpbootdSpikeMechanism *mech = (FpbootdSpikeMechanism *)calloc(1, sizeof(FpbootdSpikeMechanism));
    if (!mech) return errAuthorizationInternal;
    mech->plugin = (FpbootdSpikePlugin *)inPlugin;
    mech->engine = inEngine;
    *outMechanism = (AuthorizationMechanismRef)mech;
    return noErr;
}

static OSStatus FpbootdSpikePluginDestroy(AuthorizationPluginRef inPlugin) {
    free(inPlugin);
    return noErr;
}

/* This interface struct must outlive the plugin -- malloc'd once in
 * AuthorizationPluginCreate below and never freed until PluginDestroy
 * runs (matches the documented pattern; the compiler can't just stack
 * -allocate this and let it go out of scope). */
OSStatus AuthorizationPluginCreate(const AuthorizationCallbacks *callbacks,
                                    AuthorizationPluginRef *outPlugin,
                                    const AuthorizationPluginInterface **outPluginInterface) {
    FpbootdSpikePlugin *plugin = (FpbootdSpikePlugin *)calloc(1, sizeof(FpbootdSpikePlugin));
    if (!plugin) return errAuthorizationInternal;
    plugin->callbacks = callbacks;

    AuthorizationPluginInterface *interface =
        (AuthorizationPluginInterface *)calloc(1, sizeof(AuthorizationPluginInterface));
    if (!interface) {
        free(plugin);
        return errAuthorizationInternal;
    }
    interface->version = kAuthorizationPluginInterfaceVersion;
    interface->PluginDestroy = FpbootdSpikePluginDestroy;
    interface->MechanismCreate = FpbootdSpikeMechanismCreate;
    interface->MechanismInvoke = FpbootdSpikeMechanismInvoke;
    interface->MechanismDeactivate = FpbootdSpikeMechanismDeactivate;
    interface->MechanismDestroy = FpbootdSpikeMechanismDestroy;

    *outPlugin = (AuthorizationPluginRef)plugin;
    *outPluginInterface = interface;
    return noErr;
}
