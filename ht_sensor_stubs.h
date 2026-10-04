/*
 * ht_sensor_stubs.h
 *
 * Declarations used by hack_touchid_client.c when it is built with
 * -DHT_NO_METALLICA (the user picked a sensor that is not a Metallica
 * MIS, so OpenSSL and innoextract were never installed). The names and
 * signatures match metallica_mis_daemon.h / metallica_mis_firmware.h so
 * the client source needs no other changes; ht_sensor_stubs.c holds the
 * do-nothing implementations.
 */

#ifndef __HT_SENSOR_STUBS_H
#define __HT_SENSOR_STUBS_H

#include <stdbool.h>
#include <stddef.h>

/* Placeholder for metallica_mis_tls_t, which needs OpenSSL headers. The
 * client only ever passes a pointer to it, and the stubs never read it. */
typedef struct { int unused; } metallica_mis_tls_t;

extern int g_metallica_mis_force_pair;
extern const char *g_metallica_mis_host_product_override;
extern const char *g_metallica_mis_host_serial_override;
extern int g_metallica_mis_debug;

int metallica_mis_open_device(void);
void metallica_mis_close_device(void);
int metallica_mis_send_init(void);
int metallica_mis_do_pairing(void);
int metallica_mis_open_calibration_session(metallica_mis_tls_t *tls_out);
int metallica_mis_do_calibrate(metallica_mis_tls_t *tls);
int metallica_mis_do_records(bool wipe);

bool metallica_mis_firmware_is_present(void);
bool metallica_mis_firmware_fetch(void);

#endif /* __HT_SENSOR_STUBS_H */
