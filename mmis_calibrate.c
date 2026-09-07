/*
 * mmis_calibrate.c
 *
 * Implements mmis_calibrate.h. Persists the clean-slate calibration
 * blob (built by metallica_mis_do_calibrate() in metallica_mis_daemon.c)
 * to flash partition 6, using the REAL primitives from
 * metallica_mis_flash.h (metallica_mis_erase_flash(),
 * metallica_mis_write_flash_all() -- both tls-based, already
 * confirmed working via metallica_mis_init_flash.c's own partition
 * erase/write sequence).
 *
 * CORRECTION (this session): an earlier version of this file assumed
 * a different, never-actually-existing set of flash primitives
 * (metallica_mis_flash_read/read_all/write_all/erase, no tls param,
 * via a #include "mmis_flash.h" that didn't exist anywhere in the
 * repo) and so never actually compiled once linked into a real build
 * -- cppcheck's --suppress=missingInclude mode tolerated the missing
 * header silently, which is how this went unnoticed until
 * metallica_mis_do_calibrate() actually tried to call into this file
 * for real. Rewritten against the real metallica_mis_flash.h API.
 */

#include <string.h>

#include "mmis_calibrate.h"
#include "metallica_mis_flash.h"

bool mmis_persist_clean_slate(metallica_mis_tls_t *tls, const uint8_t *clean_slate,
                               size_t clean_slate_len) {
    if (!tls || !clean_slate || clean_slate_len == 0) {
        return false;
    }

    /* See mmis_calibrate.h's doc comment: always unconditionally
     * erase + rewrite partition 6 rather than upstream's
     * read-then-skip-if-identical optimization (read_flash() isn't
     * ported yet). Partition 6 is the same one metallica_mis_init_flash()
     * already erases once during pairing -- this just re-erases it
     * for the calibration data specifically. */
    if (metallica_mis_erase_flash(tls, 6) != 0) {
        return false;
    }

    if (metallica_mis_write_flash_all(tls, 6, 0, clean_slate, clean_slate_len) != 0) {
        return false;
    }

    return true;
}

bool mmis_check_clean_slate(void) {
    /* See mmis_calibrate.h's doc comment -- blocked on read_flash()/
     * read_flash_all() being ported from the real flash.py source,
     * which this project doesn't have yet. Always false = always
     * recalibrate; safe, just not optimal. */
    return false;
}
