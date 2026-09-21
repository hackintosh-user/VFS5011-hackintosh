/*
 * fpbootd_spike.c
 *
 * STAGE A ONLY -- this is a standalone diagnostic Authorization Plugin,
 * NOT the real Fpbootd login mechanism. Its entire job is to answer
 * exactly one question: can code running in an Authorization Plugin's
 * pluginhost process reach Fpbootd's Unix domain socket at all?
 *
 * It does this by connecting to /var/run/fpbootd.sock, sending PING,
 * reading the reply, and logging the outcome via syslog (there's no
 * visible console at the login screen to print to). It NEVER touches
 * the sensor, NEVER sends CAPTURE, and NEVER returns anything other
 * than kAuthorizationResultAllow -- regardless of what it found. This
 * is the load-bearing safety property of this whole file: even if
 * this mechanism is later registered into system.login.console for a
 * real test, it can only ever observe and log, never block or deny a
 * real login. Authorization proceeds to the next mechanism in the
 * array (ultimately the real password prompt) no matter what happens
 * in here.
 *
 * The one failure mode that WOULD still be dangerous is this code
 * hanging and never calling SetResult() at all -- so every blocking
 * socket call (connect, read) is wrapped in a hard, short timeout via
 * select() before it's attempted. Worst case if fpbootd is dead,
 * unreachable, or hung: this mechanism waits at most ~2 seconds, logs
 * a failure, and still calls SetResult(kAuthorizationResultAllow).
 *
 * Structs below are transcribed directly from Apple's own
 * AuthorizationPlugin.h (opensource.apple.com, Security-59306.80.4),
 * not from memory -- this is uncompiled, unverified-on-real-hardware
 * code. Build it, inspect it (nm/otool -L), and ideally load it in a
 * standalone test harness before it ever touches system.login.console.
 *
 * Registration into system.login.console is Stage B and is
 * deliberately NOT part of this file or its build script -- that step
 * doesn't happen until Stage A is reviewed and built successfully.
 */

#include <Security/Authorization.h>
#include <Security/AuthorizationPlugin.h>
#include <CoreFoundation/CoreFoundation.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <syslog.h>

#define FPBOOTD_SOCKET_PATH "/var/run/fpbootd.sock"
#define SPIKE_TIMEOUT_SEC 2

/* --- Plugin-level state: one instance for the whole loaded plugin --- */
typedef struct FpbootdSpikePlugin {
    const AuthorizationCallbacks *callbacks;
} FpbootdSpikePlugin;

/* --- Per-mechanism state: one instance per mechanism invocation slot --- */
typedef struct FpbootdSpikeMechanism {
    FpbootdSpikePlugin *plugin;
    AuthorizationEngineRef engine;
} FpbootdSpikeMechanism;

/* Connects to FPBOOTD_SOCKET_PATH with a hard timeout, sends "PING\n",
 * reads up to outlen-1 bytes of the reply with its own hard timeout,
 * NUL-terminates into out. Returns 0 on a completed round trip
 * (regardless of what fpbootd actually said back), -1 on any failure
 * (connect refused, timeout, socket error) -- out is only meaningful
 * on a 0 return. Never blocks longer than roughly 2x SPIKE_TIMEOUT_SEC
 * total, connect and read each bounded separately. */
static int ping_fpbootd(char *out, size_t outlen) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        syslog(LOG_NOTICE, "fpbootd-spike: socket() failed: %s", strerror(errno));
        return -1;
    }

    /* Non-blocking connect + select() so a hung/unreachable daemon
     * can never stall this mechanism indefinitely -- a plain blocking
     * connect() on a Unix socket usually returns fast, but "usually"
     * isn't good enough for something that can affect login. */
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
        return -1;
    }

    if (rc != 0) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        struct timeval tv = { .tv_sec = SPIKE_TIMEOUT_SEC, .tv_usec = 0 };

        rc = select(fd + 1, NULL, &wfds, NULL, &tv);
        if (rc <= 0) {
            syslog(LOG_NOTICE, "fpbootd-spike: connect() timed out or select() failed (rc=%d, errno=%s)",
                   rc, strerror(errno));
            close(fd);
            return -1;
        }
        int so_error = 0;
        socklen_t so_error_len = sizeof(so_error);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len);
        if (so_error != 0) {
            syslog(LOG_NOTICE, "fpbootd-spike: connect() completed with error: %s", strerror(so_error));
            close(fd);
            return -1;
        }
    }

    syslog(LOG_NOTICE, "fpbootd-spike: connected to %s successfully", FPBOOTD_SOCKET_PATH);

    const char *msg = "PING\n";
    if (write(fd, msg, strlen(msg)) < 0) {
        syslog(LOG_NOTICE, "fpbootd-spike: write() failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    struct timeval rtv = { .tv_sec = SPIKE_TIMEOUT_SEC, .tv_usec = 0 };
    rc = select(fd + 1, &rfds, NULL, NULL, &rtv);
    if (rc <= 0) {
        syslog(LOG_NOTICE, "fpbootd-spike: read timed out waiting for reply (rc=%d)", rc);
        close(fd);
        return -1;
    }

    ssize_t n = read(fd, out, outlen - 1);
    close(fd);
    if (n <= 0) {
        syslog(LOG_NOTICE, "fpbootd-spike: read() failed or EOF: %s", strerror(errno));
        return -1;
    }
    out[n] = '\0';
    char *nl = strchr(out, '\n');
    if (nl) *nl = '\0';
    return 0;
}

/* --- AuthorizationPluginInterface implementation --- */

static OSStatus FpbootdSpikeMechanismInvoke(AuthorizationMechanismRef inMechanism) {
    FpbootdSpikeMechanism *mech = (FpbootdSpikeMechanism *)inMechanism;

    openlog("fpbootd-spike", LOG_PID, LOG_AUTH);
    syslog(LOG_NOTICE, "fpbootd-spike: MechanismInvoke starting (diagnostic only, never blocks login)");

    char reply[64] = {0};
    int rc = ping_fpbootd(reply, sizeof(reply));
    if (rc == 0) {
        syslog(LOG_NOTICE, "fpbootd-spike: round trip succeeded, reply=\"%s\"", reply);
    } else {
        syslog(LOG_NOTICE, "fpbootd-spike: round trip FAILED (see prior log lines for why)");
    }

    /* THE critical line. No matter what happened above -- success,
     * failure, timeout, an error path I didn't anticipate -- this
     * mechanism always reports Allow and authorization moves on to
     * the next mechanism in the array. This mechanism decides
     * nothing about whether login succeeds; it only observes. */
    OSStatus setResultStatus = mech->plugin->callbacks->SetResult(mech->engine, kAuthorizationResultAllow);
    if (setResultStatus != errSecSuccess) {
        syslog(LOG_NOTICE, "fpbootd-spike: SetResult() itself returned %d", (int)setResultStatus);
    }

    closelog();
    return errSecSuccess;
}

static OSStatus FpbootdSpikeMechanismDeactivate(AuthorizationMechanismRef inMechanism) {
    FpbootdSpikeMechanism *mech = (FpbootdSpikeMechanism *)inMechanism;
    return mech->plugin->callbacks->DidDeactivate(mech->engine);
}

static OSStatus FpbootdSpikeMechanismDestroy(AuthorizationMechanismRef inMechanism) {
    free(inMechanism);
    return errSecSuccess;
}

static OSStatus FpbootdSpikeMechanismCreate(AuthorizationPluginRef inPlugin,
                                             AuthorizationEngineRef inEngine,
                                             AuthorizationMechanismId mechanismId,
                                             AuthorizationMechanismRef *outMechanism) {
    (void)mechanismId; /* only one mechanism exported by this bundle -- ID unused */
    FpbootdSpikeMechanism *mech = (FpbootdSpikeMechanism *)calloc(1, sizeof(FpbootdSpikeMechanism));
    if (!mech) return errSecAllocate;
    mech->plugin = (FpbootdSpikePlugin *)inPlugin;
    mech->engine = inEngine;
    *outMechanism = (AuthorizationMechanismRef)mech;
    return errSecSuccess;
}

static OSStatus FpbootdSpikePluginDestroy(AuthorizationPluginRef inPlugin) {
    free(inPlugin);
    return errSecSuccess;
}

/* This interface struct must outlive the plugin -- malloc'd once in
 * AuthorizationPluginCreate below and never freed until PluginDestroy
 * runs (matches the documented pattern; the compiler can't just stack
 * -allocate this and let it go out of scope). */
OSStatus AuthorizationPluginCreate(const AuthorizationCallbacks *callbacks,
                                    AuthorizationPluginRef *outPlugin,
                                    const AuthorizationPluginInterface **outPluginInterface) {
    FpbootdSpikePlugin *plugin = (FpbootdSpikePlugin *)calloc(1, sizeof(FpbootdSpikePlugin));
    if (!plugin) return errSecAllocate;
    plugin->callbacks = callbacks;

    AuthorizationPluginInterface *interface =
        (AuthorizationPluginInterface *)calloc(1, sizeof(AuthorizationPluginInterface));
    if (!interface) {
        free(plugin);
        return errSecAllocate;
    }
    interface->version = kAuthorizationPluginInterfaceVersion;
    interface->PluginDestroy = FpbootdSpikePluginDestroy;
    interface->MechanismCreate = FpbootdSpikeMechanismCreate;
    interface->MechanismInvoke = FpbootdSpikeMechanismInvoke;
    interface->MechanismDeactivate = FpbootdSpikeMechanismDeactivate;
    interface->MechanismDestroy = FpbootdSpikeMechanismDestroy;

    *outPlugin = (AuthorizationPluginRef)plugin;
    *outPluginInterface = interface;
    return errSecSuccess;
}
