#ifndef MMIS_CALIBRATE_H
#define MMIS_CALIBRATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "metallica_mis_tls.h"

/* Magic + fixed-header layout for the clean-slate blob, per
 * sensor.py's persist_clean_slate()/check_clean_slate():
 *   [0:2]   magic (0x5002, LE)
 *   [2:4]   length of the variable "image" section that follows the
 *           fixed 0x44-byte header (LE)
 *   [4:36]  SHA-256 of that image section
 *   [36:68] 32 reserved zero bytes
 *   [68:]   the image section itself (length from [2:4])
 */
#define MMIS_CALIB_HEADER_LEN 0x44

/* Writes the clean-slate calibration blob to flash partition 6,
 * matching upstream python-validity's Sensor.persist_clean_slate().
 *
 * SCOPE REDUCTION vs upstream (intentional, documented -- not a
 * silent gap): upstream's persist_clean_slate() first calls
 * check_clean_slate() and skips the write entirely if the flash
 * already holds byte-identical data, or reads the existing header to
 * decide erase-vs-append. That requires read_flash()/read_flash_all()
 * from flash.py, which are NOT ported here yet (see
 * mmis_check_clean_slate()'s doc comment below for why) -- so this
 * port always unconditionally erases partition 6 then writes the
 * whole blob fresh via the real, already-confirmed-working
 * metallica_mis_erase_flash()/metallica_mis_write_flash_all()
 * primitives (metallica_mis_flash.h). Functionally correct either
 * way (calibration data always ends up right); just skips upstream's
 * "don't bother if it's already correct" optimization. Revisit once
 * read_flash()/read_flash_all() get ported for real.
 *
 * Requires an already-open, already-secure TLS session (same
 * precondition as every other metallica_mis_flash.h call). Returns
 * true on success, false on any erase/write failure. */
bool mmis_persist_clean_slate(metallica_mis_tls_t *tls, const uint8_t *clean_slate,
                               size_t clean_slate_len);

/* Validates the clean-slate blob already on flash partition 6: checks the
 * magic (0x5002), the reserved zero block, and a SHA-256 hash match against
 * the stored image. Matches upstream's Sensor.check_clean_slate().
 *
 * NOT YET IMPLEMENTED -- always returns false. Requires
 * read_flash()/read_flash_all() from flash.py, which this project's
 * metallica_mis_flash.h explicitly hasn't ported yet (its own header
 * comment flags this: "not needed until template DB / calibration
 * work starts -- add them when that work begins"). That work has
 * now started (mmis_persist_clean_slate() above), but porting a flash
 * *read* command needs the real flash.py source for its opcode/framing
 * -- guessing one here would risk sending an unverified command to
 * real sensor hardware, which this project deliberately avoids
 * everywhere else. Until read_flash()/read_flash_all() are ported for
 * real, always returning false just means callers always recalibrate
 * rather than skipping when it's already valid -- safe, just not
 * optimal. */
bool mmis_check_clean_slate(void);

#endif /* MMIS_CALIBRATE_H */
