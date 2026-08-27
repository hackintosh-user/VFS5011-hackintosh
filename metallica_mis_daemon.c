/*
 * metallica_mis_daemon.c
 *
 * PARTIAL -- the plaintext bootstrap stage (send_init()), pairing
 * (do_pairing() -> metallica_mis_init_flash()), and firmware upload
 * (do_pairing() -> metallica_mis_upload_fwext(), same session, no
 * reboot in between) are all implemented and ready to test against
 * real hardware -- NONE of it has been run against a real device yet.
 * Calibration and every capture-mode command are still not built.
 * This is NOT a working daemon -- it's a test harness for --pair,
 * same spirit as vfs5011_daemon.c's own "standalone test harness"
 * caveat in its header before it got its real LaunchDaemon wiring.
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
 *   - hack-touchid-matcher.c / NBIS mindtct+bozorth3 matching
 *   - Template volume mount/unmount (encrypted APFS "HackTouchIDStore" volume)
 *   - LaunchAgent / hack-touchid-menubar-ipc.c notification plumbing
 *   - Screen-lock trigger (on_screen_locked/on_screen_unlocked) and
 *     the AX watchers for the padlock/coreautha auth surface
 *   - OpenCore min-version NVRAM gate (check_opencore_version_requirement)
 *   - type_password_and_enter() / Secure Input caveat
 * None of that is sensor-specific -- it's all keyed off the finished
 * fingerprint template, not off how the template got captured.
 *
 * Build: wired up via build_metallica_mis.sh (session Aug 24), which
 * links this file + metallica_mis_tls.c + metallica_mis_init_flash.c
 * + metallica_mis_flash.c + metallica_mis_blobs_9a.c +
 * metallica_mis_upload_fwext.c + metallica_mis_firmware.c against
 * libusb-1.0 + OpenSSL + CoreFoundation/IOKit. Not part of build.sh's
 * default hack-touchid/vfs5011_daemon targets -- build.sh calls this
 * script as a third, separate step. See that script for the exact
 * source list and flags.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>
#include <libusb.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

#include "metallica_mis_proto.h"
#include "metallica_mis_tls.h"
#include "metallica_mis_flash.h"
#include "metallica_mis_blobs_9a.h"
#include "metallica_mis_upload_fwext.h"
/* #include "hack-touchid-matcher.h"        -- reused unmodified once capture works */
/* #include "hack-touchid-menubar-ipc.h"    -- reused unmodified once capture works */

/*
 * Known Metallica MIS USB identities. python-validity's blobs_9a.py,
 * blobs_97.py, and blobs_9d.py are byte-for-byte identical (confirmed
 * via direct diff Aug 26 2026), and firmware_tables.py maps all three
 * to the same driver URL, same firmware sha512, and same firmware
 * filename (6_07f_lenovo_mis_qm.xpfwext) -- these are the same
 * underlying Synaptics silicon under different OEM-branded VID:PIDs,
 * not different hardware. 138a:0090 is deliberately NOT listed here:
 * its blobs and firmware genuinely differ (see blobs_90.py /
 * firmware_tables.py DEV_90), and metallica_mis_init_flash.c already
 * has a separate, explicitly-unsafe-for-real-hardware special case
 * for it. Do not add 0090 to this table without porting its own
 * blobs first.
 */
typedef struct {
    unsigned short vid;
    unsigned short pid;
    const char *label; /* for log messages only */
} metallica_mis_ident_t;

static const metallica_mis_ident_t METALLICA_MIS_IDENTITIES[] = {
    { 0x06cb, 0x009a, "06cb:009a" },
    { 0x138a, 0x0097, "138a:0097" },
    { 0x138a, 0x009d, "138a:009d" },
};
#define METALLICA_MIS_IDENTITIES_COUNT \
    (sizeof(METALLICA_MIS_IDENTITIES) / sizeof(METALLICA_MIS_IDENTITIES[0]))

static libusb_context *g_ctx = NULL;
static libusb_device_handle *g_handle = NULL;
static unsigned short g_detected_vid = 0;
static unsigned short g_detected_pid = 0;

/*
 * open_device() -- transport open/claim is genuinely reusable in
 * shape from vfs5011_daemon.c's open_device(), same libusb calls,
 * same macOS quirks likely apply (kernel driver auto-detach before
 * claim, retry-before-reset on claim failure). Copied structurally,
 * NOT verified against real hardware for the 138a:0097/009d
 * identities yet -- only 06cb:009a has an actual hardware test log
 * (p0cketl1nt, Aug 25-26). Treat the retry counts/sleep durations as
 * inherited defaults to revisit once each identity actually runs
 * against hardware.
 *
 * Tries each known identity in turn rather than a single hardcoded
 * VID/PID, since 09a/97/9d are the same chip under different OEM
 * USB IDs (see METALLICA_MIS_IDENTITIES comment above). Whichever
 * one opens first is recorded in g_detected_vid/g_detected_pid for
 * later use (e.g. metallica_mis_init_flash()'s 0090 special case).
 */
static int open_device(void) {
    if (libusb_init(&g_ctx) < 0) return -1;
    libusb_set_option(g_ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_NONE);

    for (int i = 0; i < 5 && !g_handle; i++) {
        for (size_t j = 0; j < METALLICA_MIS_IDENTITIES_COUNT; j++) {
            g_handle = libusb_open_device_with_vid_pid(
                g_ctx, METALLICA_MIS_IDENTITIES[j].vid, METALLICA_MIS_IDENTITIES[j].pid);
            if (g_handle) {
                g_detected_vid = METALLICA_MIS_IDENTITIES[j].vid;
                g_detected_pid = METALLICA_MIS_IDENTITIES[j].pid;
                fprintf(stderr, "metallica_mis: matched identity %s\n",
                        METALLICA_MIS_IDENTITIES[j].label);
                break;
            }
        }
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
/*
 * get_host_identity() -- fetches the two values metallica_mis_tls_init()
 * needs for PSK derivation (see set_hwkey() in metallica_mis_init_flash.c),
 * which on Linux python-validity reads from /sys/class/dmi/id/
 * product_name and product_serial. There's no DMI on macOS -- the
 * equivalent host-identity source is IOPlatformExpertDevice's "model"
 * and "IOPlatformSerialNumber" properties (the same values macOS
 * itself uses to identify the machine, and on a Hackintosh, exactly
 * what OpenCore's SMBIOS spoofing injects -- so pairing is tied to
 * the SPOOFED model+serial, same as any other macOS-facing identity
 * check on this machine, not the real physical hardware's identity).
 * NOT YET TESTED against real hardware -- this is new code written
 * this session, unlike open_device()/cmd() which were carried over
 * from the already-tested plaintext bootstrap stage. First real test
 * of this function is whatever session actually runs do_pairing()
 * against a live device.
 *
 * out_product/out_serial are caller-provided buffers of the given
 * capacity; both are null-terminated on success. Returns 0 on
 * success, -1 if either IOKit lookup fails.
 */
static int get_host_identity(char *out_product, size_t product_cap,
                              char *out_serial, size_t serial_cap) {
    int rc = -1;
    io_service_t platform_expert = IOServiceGetMatchingService(
        kIOMasterPortDefault, IOServiceMatching("IOPlatformExpertDevice"));
    if (platform_expert == IO_OBJECT_NULL) {
        fprintf(stderr, "metallica_mis: get_host_identity(): IOPlatformExpertDevice not found\n");
        return -1;
    }

    CFTypeRef model_ref = IORegistryEntryCreateCFProperty(
        platform_expert, CFSTR("model"), kCFAllocatorDefault, 0);
    CFTypeRef serial_ref = IORegistryEntryCreateCFProperty(
        platform_expert, CFSTR(kIOPlatformSerialNumberKey), kCFAllocatorDefault, 0);

    /* "model" comes back as CFDataRef (raw bytes, null-terminated C
     * string content) rather than CFStringRef -- this is a real
     * IOKit quirk, not a bug; CFStringGetCString() would fail on it. */
    if (model_ref && CFGetTypeID(model_ref) == CFDataGetTypeID()) {
        CFDataRef data = (CFDataRef)model_ref;
        CFIndex len = CFDataGetLength(data);
        if ((size_t)len < product_cap) {
            memcpy(out_product, CFDataGetBytePtr(data), (size_t)len);
            out_product[len] = '\0';
        } else {
            fprintf(stderr, "metallica_mis: get_host_identity(): product_name buffer too small\n");
            goto done;
        }
    } else {
        fprintf(stderr, "metallica_mis: get_host_identity(): couldn't read \"model\" property\n");
        goto done;
    }

    if (serial_ref && CFGetTypeID(serial_ref) == CFStringGetTypeID()) {
        if (!CFStringGetCString((CFStringRef)serial_ref, out_serial, (CFIndex)serial_cap,
                                 kCFStringEncodingUTF8)) {
            fprintf(stderr, "metallica_mis: get_host_identity(): CFStringGetCString() failed for serial\n");
            goto done;
        }
    } else {
        fprintf(stderr, "metallica_mis: get_host_identity(): couldn't read IOPlatformSerialNumber\n");
        goto done;
    }

    rc = 0;

done:
    if (model_ref) CFRelease(model_ref);
    if (serial_ref) CFRelease(serial_ref);
    IOObjectRelease(platform_expert);
    return rc;
}

/*
 * mis_transport() -- adapts the existing cmd() (int-typed lengths, no
 * ctx param) to metallica_mis_tls_transport_fn's exact signature
 * (size_t-typed lengths, void *ctx first). ctx is unused -- this
 * daemon only ever talks to one device via the g_handle global, same
 * as cmd() itself already assumes. Thin wrapper, no new logic.
 */
static int mis_transport(void *ctx, const unsigned char *out, size_t out_len,
                          unsigned char *in_buf, size_t in_buf_size) {
    (void)ctx;
    return cmd(out, (int)out_len, in_buf, (int)in_buf_size);
}

/*
 * do_pairing() -- calls metallica_mis_init_flash() using this
 * daemon's own cmd()-based transport, THEN metallica_mis_upload_fwext()
 * in the SAME still-open TLS session, matching upstream's
 * open_common() flow (init() if not yet paired, immediately followed
 * by upload_fwext() -- both run before any reboot). Only meaningful
 * to call AFTER send_init() has succeeded (same plaintext-bootstrap
 * precondition init_flash.py itself assumes).
 *
 * IMPORTANT CORRECTION (session Aug 24, after reading real
 * upload_fwext.py source): an earlier version of this function sent
 * its own reboot command immediately after metallica_mis_init_flash()
 * succeeded. That was wrong -- upload_fwext() needs a LIVE secure TLS
 * session to upload firmware (it's called in the same session right
 * after pairing, per upstream's own open_common()), and upload_fwext()
 * itself owns the actual reboot at the very end of the whole flow.
 * Rebooting right after init_flash() would have killed the session
 * before firmware could ever be uploaded. This was caught before
 * being run against real hardware -- see the memory note on why this
 * matters if this comment is ever read in isolation.
 *
 * NOT YET TESTED against real hardware -- this is the very first
 * thing in this whole project that would attempt a REAL WRITE to the
 * sensor's flash (partition table, cert material, then the firmware
 * blob itself). Read the header comments on metallica_mis_init_flash()
 * in metallica_mis_flash.h and metallica_mis_upload_fwext() in
 * metallica_mis_upload_fwext.h before running this against real
 * hardware: if either partially succeeds and fails partway through,
 * the sensor's flash state is left in whatever partial state the
 * last completed step left it in -- there is no rollback/transaction
 * semantics here, matching python's own lack of any either.
 *
 * Returns 0 on success (including "already paired AND firmware
 * already loaded, nothing to do"), -1 on any failure. On success, the
 * device sends itself a real reboot command as the last step (inside
 * metallica_mis_upload_fwext()) -- see the loud warning in main()
 * about what NOT to do immediately after this returns.
 */
static int do_pairing(void) {
    char product_name[256];
    char serial_number[256];
    metallica_mis_tls_t tls;
    metallica_mis_identity_t identity;

    if (get_host_identity(product_name, sizeof(product_name),
                           serial_number, sizeof(serial_number)) != 0) {
        fprintf(stderr, "metallica_mis: do_pairing(): failed to get host identity, aborting\n");
        return -1;
    }
    fprintf(stderr, "metallica_mis: host identity: product=\"%s\" serial=\"%s\"\n",
            product_name, serial_number);

    if (metallica_mis_tls_init(&tls, mis_transport, NULL, product_name, serial_number) != 0) {
        fprintf(stderr, "metallica_mis: do_pairing(): metallica_mis_tls_init() failed\n");
        return -1;
    }

    memset(&identity, 0, sizeof(identity));

    if (metallica_mis_init_flash(&tls, &identity, product_name, serial_number,
                                  g_detected_vid, g_detected_pid) != 0) {
        fprintf(stderr, "metallica_mis: do_pairing(): init_flash() FAILED\n");
        return -1;
    }

    fprintf(stderr, "metallica_mis: init_flash() succeeded (session still open). "
                     "Proceeding to firmware upload before any reboot...\n");

    if (metallica_mis_upload_fwext(&tls) != 0) {
        fprintf(stderr, "metallica_mis: do_pairing(): upload_fwext() FAILED. Pairing itself "
                         "(partition table + cert material) already succeeded and was "
                         "written to flash -- only the firmware upload step failed. Do NOT "
                         "assume the device is unpaired; get_flash_info() on the next run "
                         "will report the truth.\n");
        return -1;
    }

    fprintf(stderr, "metallica_mis: upload_fwext() succeeded -- reboot command already "
                     "sent as its last step. Device should be re-enumerating now.\n");
    return 0;
}

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
    bool do_pair = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--pair") == 0) do_pair = true;
    }

    fprintf(stderr,
        "metallica_mis_daemon: plaintext bootstrap + pairing + firmware\n"
        "upload stage. Calibration/capture still require work beyond\n"
        "this. See file header for status.\n");

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

    fprintf(stderr, "metallica_mis: bootstrap OK.\n");

    if (!do_pair) {
        fprintf(stderr,
            "metallica_mis: stopping here (pass --pair to attempt real\n"
            "pairing). Pairing writes the partition table + cert material\n"
            "to the sensor's flash, uploads the Metallica MIS firmware blob\n"
            "(downloading it from Lenovo first if not already cached), and\n"
            "ends with a real reboot command -- this is NOT reversible by\n"
            "just re-running the daemon, and has not been tested against\n"
            "real hardware yet. Don't pass --pair casually; understand what\n"
            "it does first (see do_pairing()'s doc comment above).\n");
        close_device();
        return 0;
    }

    fprintf(stderr, "metallica_mis: --pair given, attempting real pairing "
                     "+ firmware upload now...\n");
    rc = do_pairing();
    if (rc != 0) {
        fprintf(stderr, "metallica_mis: do_pairing() FAILED. Sensor flash state is "
                         "whatever the last completed step left it in -- there is no "
                         "rollback. Do not assume the device is in a clean/unpaired "
                         "state before trying again; get_flash_info() on the next run "
                         "will report the truth.\n");
        close_device();
        return 1;
    }

    fprintf(stderr, "metallica_mis: pairing + firmware upload succeeded, device is "
                     "rebooting. Not sending any further application-protocol commands "
                     "to it -- close_device() below is just local libusb cleanup "
                     "(clear_halt/release/close), which is safe to call even on "
                     "a handle whose device just disconnected; it's not another "
                     "command to the sensor itself.\n");

    close_device();
    return 0;
}
