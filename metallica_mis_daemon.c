/*
 * metallica_mis_daemon.c
 *
 * SCAFFOLD ONLY -- not a working daemon yet.
 *
 * This follows vfs5011_daemon.c's structure per supported_sensors.h's
 * own instructions ("Build its capture backend as its own
 * <name>_daemon.c, following vfs5011_daemon.c's structure -- NBIS
 * matching / template storage / LaunchAgent IPC are all reusable
 * as-is -- only the USB init handshake and image capture are
 * sensor-specific").
 *
 * WHAT'S DIFFERENT FROM VFS5011 (read this before filling anything in):
 *
 * VFS5011 talks in plaintext for its entire init sequence -- every
 * SEND/RECV in vfs5011_initialization[] is a raw, static byte blob
 * from vfs5011_proto.h with no crypto involved at any point.
 *
 * The Metallica MIS family (06cb:009a) does NOT work that way. Per
 * uunicorn/python-validity (the Linux reference implementation this
 * is ported from), only the first three commands are plaintext
 * (RomInfo, an unknown cmd_19, get_fw_info) plus two hardcoded init
 * blobs. Everything after that -- firmware upload, calibration, and
 * every single capture-mode command -- goes through an ECDH-derived
 * session cipher. A flat SEND/RECV script like vfs5011_initialization
 * literally cannot represent that; the moment the handshake completes,
 * every cmd() call needs to encrypt outgoing and decrypt incoming
 * before this daemon ever sees plaintext bytes.
 *
 * So: DO NOT try to force this into vfs5011's run_sequence() +
 * struct usb_action pattern past the plaintext bootstrap stage. The
 * plan is a separate metallica_mis_tls.c/.h pair implementing the
 * ECDH key exchange (see tls.py's make_keys()/self.ecdh_q in
 * python-validity) and a cmd() wrapper that encrypts/decrypts
 * transparently, THEN this daemon's capture logic calls that cmd()
 * the same way vfs5011_daemon.c calls run_sequence() -- just with a
 * different transport underneath. Not built yet. Do not stub fake
 * crypto here; get the real handshake working in isolation first.
 *
 * REUSED AS-IS FROM vfs5011_daemon.c (do not reimplement, just wire
 * these same subsystems to this daemon once capture works):
 *   - vfs5011_matcher.c / NBIS mindtct+bozorth3 matching
 *   - Template volume mount/unmount (encrypted APFS "VFSStore" volume)
 *   - LaunchAgent / vfs5011_menubar_ipc.c notification plumbing
 *   - Screen-lock trigger (on_screen_locked/on_screen_unlocked) and
 *     the AX watchers for the padlock/coreautha auth surface
 *   - OpenCore min-version NVRAM gate (check_opencore_version_requirement)
 *   - type_password_and_enter() / Secure Input caveat
 * None of that is sensor-specific -- it's all keyed off the finished
 * fingerprint template, not off how the template got captured.
 *
 * Build: not wired into build_daemon.sh yet. Once metallica_mis_tls.c
 * exists, this should build the same way vfs5011_daemon.c does --
 * this file + vfs5011_matcher.c + NBIS sources + metallica_mis_tls.c,
 * linked against libusb-1.0 + an SSL/EC-capable crypto lib (OpenSSL or
 * LibreSSL -- TBD which one plays nicest with the existing macOS
 * build setup) + the same CoreFoundation/ApplicationServices/IOKit
 * frameworks vfs5011_daemon.c already links.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>
#include <libusb.h>

#include "metallica_mis_proto.h"
/* #include "metallica_mis_tls.h"      -- not built yet */
/* #include "vfs5011_matcher.h"        -- reused unmodified once capture works */
/* #include "vfs5011_menubar_ipc.h"    -- reused unmodified once capture works */

#define METALLICA_MIS_VID 0x06cb
#define METALLICA_MIS_PID 0x009a

static libusb_context *g_ctx = NULL;
static libusb_device_handle *g_handle = NULL;

/*
 * open_device() -- transport open/claim is genuinely reusable in
 * shape from vfs5011_daemon.c's open_device(), same libusb calls,
 * same macOS quirks likely apply (kernel driver auto-detach before
 * claim, retry-before-reset on claim failure). Copied structurally,
 * NOT verified against real 06cb:009a hardware yet -- p0cketl1nt's
 * ioreg/System Info check only confirmed the device enumerates
 * unclaimed, not that these exact retry/timeout values are right for
 * this sensor. Treat the retry counts/sleep durations as inherited
 * defaults to revisit once this actually runs against hardware.
 */
static int open_device(void) {
    if (libusb_init(&g_ctx) < 0) return -1;
    libusb_set_option(g_ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_NONE);

    for (int i = 0; i < 5; i++) {
        g_handle = libusb_open_device_with_vid_pid(g_ctx, METALLICA_MIS_VID, METALLICA_MIS_PID);
        if (g_handle) break;
        usleep(300000);
    }
    if (!g_handle) { fprintf(stderr, "Metallica MIS device not found\n"); return -1; }

    libusb_set_auto_detach_kernel_driver(g_handle, 1);

    if (libusb_claim_interface(g_handle, 0) == 0) return 0;

    for (int i = 0; i < 3; i++) {
        usleep(150000);
        if (libusb_claim_interface(g_handle, 0) == 0) return 0;
    }

    fprintf(stderr, "Claim failed, resetting device and retrying...\n");
    int reset_r = libusb_reset_device(g_handle);
    if (reset_r != 0) {
        fprintf(stderr, "Device reset failed: %s\n", libusb_error_name(reset_r));
    }
    usleep(500000);

    if (libusb_claim_interface(g_handle, 0) != 0) {
        fprintf(stderr, "Claim failed again after reset\n");
        return -1;
    }
    return 0;
}

static void close_device(void) {
    if (g_handle) {
        libusb_clear_halt(g_handle, METALLICA_MIS_IN_ENDPOINT_CTRL);
        libusb_clear_halt(g_handle, METALLICA_MIS_IN_ENDPOINT_DATA);
        libusb_clear_halt(g_handle, METALLICA_MIS_OUT_ENDPOINT);
        libusb_release_interface(g_handle, 0);
        libusb_close(g_handle);
        g_handle = NULL;
    }
    if (g_ctx) {
        libusb_exit(g_ctx);
        g_ctx = NULL;
    }
}

/*
 * send_init() -- ONLY the plaintext bootstrap stage. This is as far
 * as this daemon can get without metallica_mis_tls.c existing.
 * Mirrors python-validity's Usb.send_init(): RomInfo -> cmd_19 ->
 * get_fw_info -> hardcoded init blob -> (if fwext not loaded) clean
 * slate blob. Returns 0 on success reaching end of plaintext stage,
 * negative on failure. Does NOT attempt firmware upload, calibration,
 * or capture -- those all require the session cipher to exist first.
 */
static int send_init(void) {
    /* TODO(metallica-mis): implement the actual cmd() write/read pair
     * against METALLICA_MIS_OUT_ENDPOINT / METALLICA_MIS_IN_ENDPOINT_CTRL,
     * same shape as vfs5011_daemon.c's run_sequence() but without a
     * canned struct usb_action[] script since this stage is only ~4
     * commands and the branch on get_fw_info()'s response (clean slate
     * or not) doesn't fit a static SEND/RECV list cleanly.
     *
     * fprintf(stderr, "metallica_mis: send_init() not implemented\n");
     */
    return -1;
}

/*
 * capture_quality_template() -- NOT STARTED. Blocked entirely on
 * metallica_mis_tls.c existing, since every capture-mode command on
 * this sensor family goes through the session cipher. Do not attempt
 * to write this against raw USB reads the way vfs5011's version
 * works -- it will not produce usable image data post-handshake.
 */
/*
static int capture_quality_template(struct xyt_struct *out_tmpl) {
    return -1;
}
*/

int main(int argc, char **argv) {
    fprintf(stderr,
        "metallica_mis_daemon: scaffold only, not a working daemon.\n"
        "Blocked on: firmware blob extraction (see metallica_mis_proto.h "
        "TODOs) and the ECDH handshake / session cipher (metallica_mis_tls.c, "
        "not yet written). See file header for what's safe to build next.\n");

    if (open_device() != 0) {
        return 1;
    }

    int rc = send_init();
    if (rc != 0) {
        fprintf(stderr, "metallica_mis: plaintext bootstrap stage failed "
                         "(expected -- send_init() is unimplemented)\n");
    }

    close_device();
    return 1;
}
