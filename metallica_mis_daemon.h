/*
 * metallica_mis_daemon.h
 *
 * Thin extern surface over metallica_mis_daemon.c so
 * hack_touchid_client.c can drive the plaintext-bootstrap + pairing
 * + firmware-upload sequence directly, instead of requiring
 * p0cketl1nt (or any tester) to separately build and run the
 * standalone metallica_mis_daemon test-harness binary.
 *
 * Only these calls are exposed -- everything else in
 * metallica_mis_daemon.c (cmd(), assert_status(), mis_transport(),
 * get_host_identity(), the METALLICA_MIS_IDENTITIES table) stays
 * static/internal. Capture (Enroll/Verify) is NOT exposed here
 * because it doesn't exist yet -- see metallica_mis_daemon.c's
 * capture_quality_template() stub comment. This header is pairing +
 * presence-check only.
 *
 * Callers must define HACK_TOUCHID_CLIENT_BUILD before compiling
 * metallica_mis_daemon.c into their binary, so that file's own
 * main() (the standalone test-harness entry point) is compiled out
 * and doesn't collide with the client's main().
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "metallica_mis_tls.h"
#include "metallica_mis_debug.h"

#ifndef __METALLICA_MIS_DAEMON_H
#define __METALLICA_MIS_DAEMON_H

/* Set to 1 (e.g. from --force-pair) before calling do_pairing() to
 * wipe the identity partitions first. See the Sep 15 comment at its
 * usage site in metallica_mis_daemon.c for why this exists: init_flash()
 * silently no-ops if the device already reports any partitions, even
 * if what's stored there was written under a stale/wrong PSK and can
 * never actually be read back. Destructive -- the existing paired
 * identity is gone for good once this runs. */
extern int g_metallica_mis_force_pair;

/* Set from --host-product / --host-serial. When non-NULL, get_host_identity()
 * uses these instead of the macOS IOPlatformExpertDevice values. On a
 * Hackintosh those values are the SPOOFED Mac model/serial, so a sensor that
 * was paired from Linux or Windows (which use the laptop's real DMI
 * product_name / product_serial) derives a different PSK and fails the
 * handle_priv() HMAC check. Passing the real values lets HTID derive the
 * same key. Either flag may be used alone; the other value is then still
 * read from IOKit. */
extern const char *g_metallica_mis_host_product_override;
extern const char *g_metallica_mis_host_serial_override;

/* Opens the sensor over libusb and claims its interface. Tries each
 * known Metallica MIS identity (06cb:009a, 138a:0097, 138a:009d) in
 * turn. Returns 0 on success, -1 on failure (already prints its own
 * diagnostic on failure). Must be called before send_init() or
 * do_pairing(). */
int metallica_mis_open_device(void);

/* Local libusb cleanup (clear_halt/release/close). Safe to call even
 * after the device has disconnected (e.g. right after a successful
 * pairing reboot). */
void metallica_mis_close_device(void);

/* Non-invasive presence check -- own short-lived context, never
 * opens/claims, safe to call speculatively without disturbing an
 * in-flight open_device()/close_device() cycle. Checks all three
 * known OEM identities (06cb:009a, 138a:0097, 138a:009d). */
bool metallica_mis_sensor_is_present(void);

/* Plaintext bootstrap stage only (RomInfo, unknown cmd_19,
 * get_fw_info, hardcoded init blob, clean-slate blob if needed).
 * Returns 0 on success, -1 on failure. Safe to run repeatedly --
 * does not write pairing state. Must succeed before do_pairing(). */
int metallica_mis_send_init(void);

/* Real pairing: writes partition table + cert material to the
 * sensor's flash, uploads the Metallica MIS firmware blob (fetching
 * it from Lenovo first if not cached), and ends with a real reboot
 * command sent to the device. NOT reversible by re-running -- if it
 * fails partway through, the sensor's flash is left in whatever
 * state the last completed step left it in (no rollback). Only
 * meaningful to call after metallica_mis_send_init() has succeeded.
 * Returns 0 on success (including "already paired and firmware
 * already loaded"), -1 on any failure. */
int metallica_mis_do_pairing(void);

/* Establishes a live, secure TLS session for calibration/capture use,
 * WITHOUT going through the caller having to know about pairing at
 * all -- reuses the same init_flash()+upload_fwext() sequence as
 * metallica_mis_do_pairing(), but only succeeds (returns 0) when both
 * steps took their no-op paths (already paired, firmware already
 * loaded) and hands back the open session via *tls_out instead of
 * discarding it. If real pairing/upload/reboot happens instead
 * (device wasn't ready yet), returns -1 -- the session doesn't
 * survive that, so it's not handed back. Call [P] Pair Sensor /
 * metallica_mis_do_pairing() first if this fails on a fresh device. */
int metallica_mis_open_calibration_session(metallica_mis_tls_t *tls_out);

/* Raw bulk-data read from the sensor's image endpoint (0x82) -- used by
 * calibrate()/capture() to pull one frame's worth of raw sensor data
 * after a CALIBRATE/ENROLL/IDENTIFY capture command has been issued.
 * Blocks up to 10s (matches upstream python-validity's read_82()
 * timeout). Returns the number of bytes actually read, or -1 on
 * failure (device not open, libusb error, or timeout). */
int metallica_mis_read_bulk_data(unsigned char *out_buf, size_t out_buf_size);

/* Runs the full type-0x199 calibration sequence (3 capture iterations +
 * one blank-image capture) and persists the resulting clean-slate blob
 * to flash partition 6. Requires an already-open, already-secure TLS
 * session (post metallica_mis_do_pairing() or a prior successful
 * pairing). Caller should check mmis_check_clean_slate() first and only
 * call this if it returns false -- this always runs the full sequence,
 * it does not skip on its own if valid calibration data already exists.
 * Returns 0 on success, -1 on any failure. */
int metallica_mis_do_calibrate(metallica_mis_tls_t *tls);

/* Same as metallica_mis_do_calibrate(), but also hands back the final
 * in-memory calibration data (python's self.calib_data) in calib_out /
 * *calib_len_out. ENROLL and IDENTIFY captures need that data to build
 * their cmd_02, and this client keeps no on-disk cache of it, so the
 * enroll test calibrates in the same session and uses what comes back.
 * calib_out may be NULL (then this behaves exactly like
 * metallica_mis_do_calibrate()). Returns 0 on success, -1 on failure. */
int metallica_mis_do_calibrate_ex(metallica_mis_tls_t *tls,
                                  uint8_t *calib_out, size_t calib_out_max, size_t *calib_len_out);

/* Reads the sensor's interrupt endpoint (EP 0x83), python-validity's
 * Usb.wait_int(). Capture and enroll are driven by these interrupts.
 * total_timeout_ms > 0 bounds the wait, <= 0 waits without a limit.
 * Returns bytes received (> 0), 0 on timeout, -1 on a USB error. */
int metallica_mis_wait_interrupt(unsigned char *out_buf, size_t out_buf_size, int total_timeout_ms);

/* Lists (wipe=false) or wipes (wipe=true) the fingerprint records stored
 * ON the sensor, via metallica_mis_db.c. Opens/closes the device itself.
 * Needs an already-paired sensor with firmware loaded. Wipe deletes every
 * user record in the sensor's StgWindsor storage and verifies by reading
 * back -- this is the manual fix for the 0x04c3 "record save rejected"
 * enroll failure. Does not touch HTID's own saved templates on the Mac.
 * Returns 0 on success, -1 on failure. Honours g_metallica_mis_debug
 * (metallica_mis_debug.h). */
int metallica_mis_do_records(bool wipe);

#endif /* __METALLICA_MIS_DAEMON_H */
