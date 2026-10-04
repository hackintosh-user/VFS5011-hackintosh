/*
 * ht_sensor_stubs.c
 *
 * Stand-in for the Metallica MIS code when hack-touchid is built for a
 * different sensor (./prep_and_build.sh --sensor vfs5011, for example).
 * Linked INSTEAD of metallica_mis_*.c / mmis_*.c, so the build needs
 * neither OpenSSL nor innoextract. Every entry point fails with the same
 * message telling the user how to get a build that includes Metallica.
 */

#include <stdio.h>
#include "ht_sensor_stubs.h"

int g_metallica_mis_force_pair = 0;
const char *g_metallica_mis_host_product_override = NULL;
const char *g_metallica_mis_host_serial_override = NULL;
int g_metallica_mis_debug = 0;

static void not_built(void) {
    fprintf(stderr,
        "This hack-touchid build does not include Metallica MIS support.\n"
        "Rebuild it for your sensor with:\n"
        "  ./prep_and_build.sh --sensor metallica\n");
}

int metallica_mis_open_device(void)  { not_built(); return -1; }
void metallica_mis_close_device(void) {}
int metallica_mis_send_init(void)    { not_built(); return -1; }
int metallica_mis_do_pairing(void)   { not_built(); return -1; }
int metallica_mis_open_calibration_session(metallica_mis_tls_t *tls_out) { (void)tls_out; not_built(); return -1; }
int metallica_mis_do_calibrate(metallica_mis_tls_t *tls) { (void)tls; not_built(); return -1; }
int metallica_mis_do_records(bool wipe) { (void)wipe; not_built(); return -1; }

bool metallica_mis_firmware_is_present(void) { return false; }
bool metallica_mis_firmware_fetch(void)      { not_built(); return false; }
