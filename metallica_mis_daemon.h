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
#include <stddef.h>

#include "metallica_mis_tls.h"

#ifndef __METALLICA_MIS_DAEMON_H
#define __METALLICA_MIS_DAEMON_H

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

#endif /* __METALLICA_MIS_DAEMON_H */
