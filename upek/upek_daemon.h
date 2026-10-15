/*
 * upek_daemon.h
 *
 * Extern surface over upek_daemon.c's one real public entry point,
 * so hack_touchid_client.c's capture dispatch can call it directly.
 * upek_daemon.c's own main() is gated behind #ifdef
 * UPEK_STANDALONE_TEST already (see that file), so simply not
 * defining that macro when linking into the client is enough to
 * avoid a duplicate main() -- no build-flag gymnastics needed here,
 * unlike metallica_mis_daemon.h's HACK_TOUCHID_CLIENT_BUILD guard.
 */

#ifndef __UPEK_DAEMON_H
#define __UPEK_DAEMON_H

#include <libusb.h>
#include <stdbool.h>

/* Full one-swipe capture: init -> await finger -> capture -> deinit.
 * Assumes the caller has already opened the device and claimed
 * interface 0 (same division of responsibility as VFS5011's own
 * capture path -- device open/claim is identical across sensors,
 * only the capture protocol itself differs). Returns a malloc'd raw
 * 8-bit grayscale buffer (caller frees), UPEK_IMG_WIDTH wide and
 * *out_height rows tall, or NULL on failure. */
unsigned char *upek_capture_fingerprint_image(libusb_device_handle *handle, int *out_height);

/* Device open/close/presence (Aug 29) -- same retry-with-backoff and
 * non-invasive-presence-check shape as vfs5011_daemon.c's own
 * open_device()/close_device()/vfs5011_sensor_is_present(). Not
 * currently called from hack_touchid_client.c (the client has its
 * own generalized open_device()/close_device() that branch on
 * g_detected_sensor and manage a single shared g_handle covering
 * whichever sensor is active) -- these exist for upek_daemon.c's own
 * standalone test harness, and as the shape a future shared daemon
 * core would expect from this sensor's backend. */
int upek_open_device(void);
void upek_close_device(void);
bool upek_sensor_is_present(void);
int upek_image_width(void);

#endif /* __UPEK_DAEMON_H */
