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
#include <stdint.h>

/* Placeholder for metallica_mis_tls_t, which needs OpenSSL headers. The
 * client only ever passes a pointer to it, and the stubs never read it. */
typedef struct { int unused; } metallica_mis_tls_t;

extern int g_metallica_mis_force_pair;
extern const char *g_metallica_mis_host_product_override;
extern const char *g_metallica_mis_host_serial_override;
extern int g_metallica_mis_session_rebooted;
#define MMIS_SESSION_FAILED (-2)
#define MMIS_SESSION_HANDSHAKE (-4)
#define MMIS_ENROLL_CANCELLED (-3)
typedef int (*mmis_replace_cb)(int users_on_sensor);
int metallica_mis_do_enroll_guided(mmis_replace_cb replace_cb);
#define MMIS_PRINTS_CANCEL  0
#define MMIS_PRINTS_ADD     1
#define MMIS_PRINTS_REPLACE 2
typedef int (*mmis_prints_cb)(const uint16_t *subtypes, int count);
void metallica_mis_set_prints_cb(mmis_prints_cb cb);
int metallica_mis_list_prints(uint16_t *subtypes, int max, int *count);
#define MMIS_VERIFY_NO_MATCH 1
int metallica_mis_do_verify_guided(void);
void metallica_mis_set_enroll_subtype(uint16_t subtype);
const char *metallica_mis_finger_name(uint16_t subtype);
int metallica_mis_last_match_subtype(void);
extern int g_metallica_mis_debug;

int metallica_mis_open_device(void);
void metallica_mis_close_device(void);
int metallica_mis_send_init(void);
int metallica_mis_do_pairing(void);
int metallica_mis_open_calibration_session(metallica_mis_tls_t *tls_out);
int metallica_mis_do_calibrate(metallica_mis_tls_t *tls);
int metallica_mis_do_records(bool wipe);
int metallica_mis_do_enroll_test(void);
int metallica_mis_do_verify_test(void);

bool metallica_mis_firmware_is_present(void);
bool metallica_mis_firmware_fetch(void);

#endif /* __HT_SENSOR_STUBS_H */
