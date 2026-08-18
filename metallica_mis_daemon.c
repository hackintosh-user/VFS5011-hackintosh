/*
 * metallica_mis_daemon.c
 *
 * PARTIAL -- the plaintext bootstrap stage (send_init()) is
 * implemented and ready to test against real hardware. Everything
 * past that (firmware upload, calibration, capture) is blocked on
 * metallica_mis_tls.c, which does not exist yet. This is NOT a
 * working daemon -- it's a test harness for one stage of the
 * handshake, same spirit as vfs5011_daemon.c's own "standalone test
 * harness" caveat in its header before it got its real LaunchDaemon
 * wiring.
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
 * bulk_transfer_with_pipe_retry() -- copied verbatim from
 * vfs5011_daemon.c. This is pure libusb/macOS glue, not sensor
 * protocol logic, so it's identical regardless of which sensor is
 * on the other end of the pipe. If VFS5011's version of this ever
 * changes (e.g. a new macOS quirk is discovered), port the change
 * here too.
 */
static int bulk_transfer_with_pipe_retry(libusb_device_handle *handle, int endpoint,
                                          unsigned char *data, int size, int *transferred,
                                          unsigned int timeout) {
    int r = libusb_bulk_transfer(handle, endpoint, data, size, transferred, timeout);
    if (r == LIBUSB_ERROR_PIPE) {
        fprintf(stderr, "  (stall on endpoint 0x%02x, clearing halt and retrying)\n", endpoint);
        libusb_clear_halt(handle, endpoint);
        r = libusb_bulk_transfer(handle, endpoint, data, size, transferred, timeout);
    }
    return r;
}

/*
 * cmd() -- the plaintext-stage equivalent of python-validity's
 * Usb.cmd(): write `out` (out_len bytes) to the OUT endpoint, then
 * read a reply into `in_buf` (up to in_buf_size bytes) from the
 * control-reply IN endpoint. Returns the number of bytes actually
 * received on success, or a negative libusb error code on failure.
 *
 * IMPORTANT: this is ONLY valid during the plaintext bootstrap
 * stage. Once metallica_mis_tls.c exists and the ECDH handshake
 * completes, all further commands must go through that module's
 * encrypt/decrypt-wrapped cmd(), not this one. Do not call this
 * function anywhere past send_init() succeeding.
 */
static int cmd(const unsigned char *out, int out_len,
                unsigned char *in_buf, int in_buf_size) {
    int transferred = 0;

    if (out_len > 0) {
        int r = bulk_transfer_with_pipe_retry(g_handle, METALLICA_MIS_OUT_ENDPOINT,
                                               (unsigned char *)out, out_len, &transferred,
                                               METALLICA_MIS_DEFAULT_WAIT_TIMEOUT);
        if (r != 0 || transferred != out_len) {
            fprintf(stderr, "metallica_mis: cmd() SEND failed: %s\n", libusb_error_name(r));
            return (r != 0) ? r : LIBUSB_ERROR_IO;
        }
    }

    transferred = 0;
    int r = bulk_transfer_with_pipe_retry(g_handle, METALLICA_MIS_IN_ENDPOINT_CTRL,
                                           in_buf, in_buf_size, &transferred,
                                           METALLICA_MIS_DEFAULT_WAIT_TIMEOUT);
    if (r != 0) {
        fprintf(stderr, "metallica_mis: cmd() RECV failed: %s\n", libusb_error_name(r));
        return r;
    }

    return transferred;
}

/*
 * assert_status() -- python-validity's convention (see util.py's
 * assert_status()) is that most command replies start with a 2-byte
 * little-endian status word, 0x0000 meaning success. Mirrors that
 * check. Returns 0 if status is OK, -1 otherwise (and logs the raw
 * status bytes so a real failure is diagnosable rather than silent).
 */
static int assert_status(const unsigned char *reply, int reply_len) {
    if (reply_len < 2) {
        fprintf(stderr, "metallica_mis: reply too short to contain a status word (%d bytes)\n",
                reply_len);
        return -1;
    }
    unsigned short status = (unsigned short)reply[0] | ((unsigned short)reply[1] << 8);
    if (status != 0x0000) {
        fprintf(stderr, "metallica_mis: command failed, status=0x%04x\n", status);
        return -1;
    }
    return 0;
}

/*
 * send_init() -- ONLY the plaintext bootstrap stage. This is as far
 * as this daemon can get without metallica_mis_tls.c existing.
 * Mirrors python-validity's Usb.send_init():
 *
 *   1. RomInfo.get()          (metallica_mis_cmd_rominfo)
 *   2. unknown init command   (metallica_mis_cmd_19)
 *   3. get_fw_info()          (metallica_mis_cmd_fwinfo) -- reply's
 *      first 2 bytes (after the status word) are inspected; a
 *      nonzero "err" there means fwext isn't loaded yet
 *   4. metallica_mis_init_hardcoded is always sent
 *   5. IF step 3 indicated fwext isn't loaded, ALSO send
 *      metallica_mis_init_hardcoded_clean_slate ("Clean slate" path
 *      in python-validity's logging)
 *
 * Returns 0 on success reaching end of plaintext stage, negative on
 * failure. Does NOT attempt firmware upload, calibration, or
 * capture -- those all require the session cipher to exist first.
 *
 * NOT YET VERIFIED against real hardware. First real test: does step
 * 1 even get a valid-looking reply back from a live 06cb:009a sensor.
 */
static int send_init(void) {
    unsigned char reply[256];
    int n;

    /* Step 1: RomInfo.get() */
    n = cmd(metallica_mis_cmd_rominfo, sizeof(metallica_mis_cmd_rominfo), reply, sizeof(reply));
    if (n < 0) return -1;
    if (assert_status(reply, n) != 0) {
        fprintf(stderr, "metallica_mis: RomInfo.get() failed\n");
        return -1;
    }

    /* Step 2: unknown plaintext init command. python-validity doesn't
     * appear to check this reply's status the same way (TODO: confirm
     * against a real trace once hardware is available) -- send it and
     * move on rather than hard-failing if status looks odd here. */
    n = cmd(metallica_mis_cmd_19, sizeof(metallica_mis_cmd_19), reply, sizeof(reply));
    if (n < 0) return -1;

    /* Step 3: get_fw_info() -- reply layout per python-validity's
     * send_init(): 2-byte status word, then a 2-byte little-endian
     * "err" field. err != 0 means fwext isn't loaded ("Clean slate"). */
    n = cmd(metallica_mis_cmd_fwinfo, sizeof(metallica_mis_cmd_fwinfo), reply, sizeof(reply));
    if (n < 0) return -1;
    if (n < 4) {
        fprintf(stderr, "metallica_mis: get_fw_info() reply too short (%d bytes)\n", n);
        return -1;
    }
    unsigned short fw_err = (unsigned short)reply[2] | ((unsigned short)reply[3] << 8);

    /* Step 4: always send the hardcoded init blob. */
    n = cmd(metallica_mis_init_hardcoded, sizeof(metallica_mis_init_hardcoded), reply, sizeof(reply));
    if (n < 0) return -1;
    if (assert_status(reply, n) != 0) {
        fprintf(stderr, "metallica_mis: init_hardcoded blob rejected\n");
        return -1;
    }

    /* Step 5: only if fwext isn't loaded yet. */
    if (fw_err != 0) {
        fprintf(stderr, "metallica_mis: fwext not loaded, sending clean-slate blob\n");
        n = cmd(metallica_mis_init_hardcoded_clean_slate,
                sizeof(metallica_mis_init_hardcoded_clean_slate), reply, sizeof(reply));
        if (n < 0) return -1;
        if (assert_status(reply, n) != 0) {
            fprintf(stderr, "metallica_mis: init_hardcoded_clean_slate blob rejected\n");
            return -1;
        }
    }

    fprintf(stderr, "metallica_mis: plaintext bootstrap stage completed OK\n");
    return 0;
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
        "metallica_mis_daemon: plaintext bootstrap stage only, not a\n"
        "working daemon yet. Blocked on the ECDH handshake / session\n"
        "cipher (metallica_mis_tls.c, not yet written) -- see file\n"
        "header for what's safe to build next. This run only tests\n"
        "send_init() against real hardware.\n");

    if (open_device() != 0) {
        return 1;
    }

    int rc = send_init();
    if (rc != 0) {
        fprintf(stderr, "metallica_mis: plaintext bootstrap stage FAILED. "
                         "This is the first real signal from hardware -- "
                         "check the specific step that failed above.\n");
        close_device();
        return 1;
    }

    fprintf(stderr, "metallica_mis: bootstrap OK. Stopping here -- "
                     "firmware upload/calibration/capture all require "
                     "metallica_mis_tls.c, which doesn't exist yet.\n");

    close_device();
    return 0;
}
