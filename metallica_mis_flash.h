#ifndef __METALLICA_MIS_FLASH_H
#define __METALLICA_MIS_FLASH_H

/*
 * metallica_mis_flash.h
 *
 * C port of python-validity's validitysensor/flash.py (get_flash_info,
 * erase_flash, write_flash, call_cleanups) + the JEDEC flash-IC lookup
 * table from hw_tables.py, ported directly against real source pulled
 * Aug 20 2026. This is a prerequisite for metallica_mis_init_flash.c's
 * partition_flash()/init_flash() -- both call get_flash_info() first
 * to learn the sensor's flash geometry (size/sector size/erase cmd),
 * and init_flash() calls erase_flash()/write_flash() after pairing to
 * wipe and persist the new partitions.
 *
 * All commands here go through metallica_mis_tls_cmd() (the SECURE
 * session), matching python's tls.cmd() calls in flash.py -- these
 * are NOT plaintext-bootstrap commands, so this module cannot be used
 * before metallica_mis_tls_open() has succeeded.
 *
 * SCOPE: only get_flash_info(), erase_flash(), write_flash(),
 * call_cleanups() are built here -- exactly what partition_flash()/
 * init_flash() need. write_flash_all()/read_flash()/read_flash_all()/
 * write_fw_signature()/read_tls_flash() from the real flash.py are
 * NOT built yet (not needed until firmware upload / calibration /
 * template DB work starts) -- add them when that work begins rather
 * than speculatively now.
 *
 * JEDEC table: only the 19-entry table from hw_tables.py's
 * flash_ic_table is ported (verbatim, all fields, even the ones this
 * project doesn't currently use -- f18/f1b/f1c/f1e/f25/f26 are kept
 * for fidelity with the python source even though nothing here reads
 * them yet, same spirit as PartitionInfo's access_lvl field). Real
 * hardware confirmation: p0cketl1nt's X1C6 send_init() test (Aug 19)
 * logged "Detected Flash IC: W25Q80B, 1048576 bytes" -- this exact
 * entry (jid0=0xef, jid1=0x40, size=1048576) is in the table below,
 * so the lookup is expected to succeed on that hardware without
 * needing any table edits.
 */

#include <stddef.h>
#include <stdint.h>
#include "metallica_mis_tls.h"

typedef struct {
    const char *name;
    uint32_t size;
    uint32_t f18;
    uint8_t jid0;
    uint8_t jid1;
    uint32_t f1b;
    uint32_t f1c;
    uint32_t f1e;
    uint32_t sector_size;      /* python's secror_size (sic, matches upstream's typo) */
    uint8_t sector_erase_cmd;
    uint32_t f25;
    uint32_t f26;
} metallica_mis_flash_ic_info_t;

/* metallica_mis_flash_ic_table_lookup() -- linear scan of the
 * hardcoded 19-entry JEDEC table, matching on (jedec_id0, jedec_id1,
 * size) exactly, per hw_tables.py's flash_ic_table_lookup(). Returns
 * a pointer into the static table (do NOT free), or NULL if no exact
 * match -- matches python's returning None for an unrecognized IC,
 * which flash.py's get_flash_info() turns into a raised exception. */
const metallica_mis_flash_ic_info_t *metallica_mis_flash_ic_table_lookup(uint16_t jedec_id0,
                                                                           uint16_t jedec_id1,
                                                                           uint32_t size);

#include "metallica_mis_init_flash.h" /* for metallica_mis_partition_info_t */

typedef struct {
    const metallica_mis_flash_ic_info_t *ic;
    uint16_t blocks;
    uint16_t unknown0;
    uint16_t blocksize;
    uint16_t unknown1;
    metallica_mis_partition_info_t *partitions; /* malloc'd, caller frees via metallica_mis_flash_info_free() */
    size_t partition_count;
} metallica_mis_flash_info_t;

/* metallica_mis_get_flash_info() -- sends cmd 0x3e over the secure
 * TLS session, parses the JEDEC id / geometry / partition-table
 * response, and resolves the JEDEC id against the hardcoded table.
 * Port of flash.py's get_flash_info(). Returns 0 on success (info
 * populated, partitions malloc'd -- caller must call
 * metallica_mis_flash_info_free()), -1 on transport/protocol failure
 * OR an unrecognized flash IC (matches python raising "Unknown flash
 * IC. JEDEC id=%x:%x, size=%dx%d" -- this port doesn't have a string
 * message channel back to the caller yet, just the -1/errno-style
 * failure; TODO surface the JEDEC id/size in a caller-visible way if
 * this ever needs to be diagnosable without a debugger). */
int metallica_mis_get_flash_info(metallica_mis_tls_t *tls, metallica_mis_flash_info_t *out);

void metallica_mis_flash_info_free(metallica_mis_flash_info_t *info);

/* metallica_mis_flash_call_cleanups() -- sends cmd 0x1a ("commit"
 * pending flash writes/erases). Per python's call_cleanups(): error
 * code 0x0491 ("Nothing to commit") is treated as success, not a
 * failure -- ported verbatim, this is intentional, not a bug. Returns
 * 0 on success (including the "nothing to commit" case), -1 on any
 * other failure status or transport error. */
int metallica_mis_flash_call_cleanups(metallica_mis_tls_t *tls);

/* metallica_mis_write_enable() -- sends the hardcoded
 * db_write_enable blob (see metallica_mis_blobs_9a.h) to unlock flash
 * writes. Port of flash.py's write_enable(). Returns 0 on success,
 * -1 on failure status or transport error. */
int metallica_mis_write_enable(metallica_mis_tls_t *tls);

/* metallica_mis_erase_flash() -- write_enable() + cmd 0x3f (erase
 * partition `partition`), then ALWAYS calls call_cleanups()
 * afterward regardless of the erase result (python's try/finally --
 * ported with the same "cleanup runs even on failure, but the
 * original erase failure is what gets returned" semantics). Returns
 * 0 on success, -1 if either the erase or (when the erase succeeded)
 * the cleanup failed. */
int metallica_mis_erase_flash(metallica_mis_tls_t *tls, uint8_t partition);

/* metallica_mis_write_flash() -- write_enable() (sent but NOT
 * status-checked, matching python's bare `tls.cmd(db_write_enable)`
 * with no assert_status() call here -- yes, that's a real asymmetry
 * vs write_enable()/erase_flash() which DO check status; preserved
 * as-is since deviating would risk behaving differently from a real
 * device expecting this exact sequence), then cmd 0x41 (write `buf`
 * at `addr` in `partition`), then ALWAYS call_cleanups() in a
 * try/finally, same pattern as erase_flash(). buf/buf_len should be
 * <= 0x1000 per call (python's write_flash_all() chunks at that
 * boundary; this port only builds the single-chunk primitive -- see
 * file header for what's still not built). Returns 0 on success, -1
 * on write failure or (if the write succeeded) cleanup failure. */
int metallica_mis_write_flash(metallica_mis_tls_t *tls, uint8_t partition, uint32_t addr,
                               const unsigned char *buf, size_t buf_len);

#endif /* __METALLICA_MIS_FLASH_H */
