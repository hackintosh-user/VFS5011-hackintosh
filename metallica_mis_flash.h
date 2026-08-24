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
 * All commands here go through metallica_mis_tls_cmd(), which
 * dual-mode dispatches: plaintext (falls through to the raw
 * transport) if no secure session is up yet, or wrapped/encrypted if
 * one is -- matching Tls.cmd() exactly. During init_flash()'s
 * pairing flow (this module's main caller) NO secure session exists
 * yet, so every call here runs in plaintext, same as python. This
 * module works identically before or after pairing; it just calls
 * metallica_mis_tls_cmd() and lets IT decide the mode.
 *
 * SCOPE: get_flash_info(), erase_flash(), write_flash(),
 * call_cleanups(), write_flash_all(), get_fw_info(), write_fw_signature()
 * are built here (the last three added when firmware-upload work
 * started, ported against real flash.py source pulled Aug 24 2026,
 * not from memory). read_flash()/read_flash_all()/read_tls_flash()
 * from the real flash.py are still NOT built -- not needed until
 * template DB / calibration work starts -- add them when that work
 * begins rather than speculatively now.
 *
 * write_hw_reg32()/read_hw_reg32() are also here, even though they're
 * sensor.py's functions in upstream, not flash.py's -- this project
 * already put reboot() (also sensor.py's) here for the same reason:
 * metallica_mis_init_flash.c needs one shared low-level-primitives
 * layer, and splitting two functions into their own
 * metallica_mis_sensor.c/.h for this project's single-sensor scope
 * isn't worth the extra file. Revisit if/when this project ever needs
 * more of sensor.py (identify_sensor(), RomInfo, factory_reset()).
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
#include <stdbool.h>
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

/* metallica_mis_get_flash_info() -- sends cmd 0x3e via
 * metallica_mis_tls_cmd() (plaintext during pairing, encrypted
 * afterward -- see file header), parses the JEDEC id / geometry / partition-table
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

/* metallica_mis_write_flash_all() -- direct port of flash.py's
 * write_flash_all(): chunks buf into <= 0x1000-byte pieces and calls
 * metallica_mis_write_flash() once per chunk, advancing addr by each
 * chunk's actual length. This is the piece flash.h's earlier scope
 * note flagged as not-yet-built -- needed because firmware blobs
 * (the .xpfwext this sensor needs) are far larger than one
 * write_flash() call's 0x1000-byte limit. Returns 0 on success, -1 on
 * the first chunk that fails (matches python: write_flash() itself
 * would raise and abort the loop; this port stops and returns -1 the
 * same way, WITHOUT attempting remaining chunks -- flash may be left
 * partially written on failure, same as upstream's own lack of
 * rollback). */
int metallica_mis_write_flash_all(metallica_mis_tls_t *tls, uint8_t partition, uint32_t addr,
                                   const unsigned char *buf, size_t buf_len);

/* ==================== firmware (fwext) info / upload ==================== */

typedef struct {
    uint16_t type;
    uint16_t subtype;
    uint16_t major;
    uint16_t minor;
    uint32_t size;
} metallica_mis_module_info_t;

typedef struct {
    uint16_t major;
    uint16_t minor;
    uint32_t buildtime; /* unix timestamp, matches python's ctime(fwi.buildtime) usage */
    metallica_mis_module_info_t *modules; /* malloc'd, caller frees via metallica_mis_fw_info_free() */
    size_t module_count;
} metallica_mis_fw_info_t;

/* metallica_mis_get_fw_info() -- direct port of flash.py's
 * get_fw_info(partition): sends cmd 0x43 <partition>, and unlike
 * every other command in this file, a "b0 04" reply (2 bytes, status
 * word 0x04b0) is NOT a failure here -- it means no firmware is
 * loaded yet, which is the expected/normal state before first upload.
 * That specific reply returns 0 with *out_present = false and *out
 * left zeroed (do not treat this as an error and do not call
 * metallica_mis_fw_info_free() on it). Any OTHER non-OK status IS a
 * real failure (-1). On a genuine OK reply, parses major/minor/
 * buildtime + the module table, sets *out_present = true, and the
 * caller must call metallica_mis_fw_info_free() when done. Returns 0
 * on success (loaded or not), -1 on transport/protocol failure. */
int metallica_mis_get_fw_info(metallica_mis_tls_t *tls, uint8_t partition,
                               bool *out_present, metallica_mis_fw_info_t *out);

void metallica_mis_fw_info_free(metallica_mis_fw_info_t *info);

/* metallica_mis_write_fw_signature() -- direct port of flash.py's
 * write_fw_signature(): sends cmd 0x42 <partition> <pad-byte> <u16le
 * sig_len> + signature bytes (python's pack('<BBxH', ...) -- the 'x'
 * is one skipped/padding byte between partition and the length
 * field, ported literally as hdr[2]=0). Returns 0 on success, -1 on
 * failure status or transport error. */
int metallica_mis_write_fw_signature(metallica_mis_tls_t *tls, uint8_t partition,
                                      const unsigned char *signature, size_t signature_len);

/* metallica_mis_write_hw_reg32() / metallica_mis_read_hw_reg32() --
 * direct ports of sensor.py's write_hw_reg32()/read_hw_reg32(): raw
 * 32-bit register poke/peek used by upload_fwext()'s "no idea what
 * this is" precondition check (ported as a documented gap, not
 * silently dropped -- matches upstream's own comment verbatim: nobody
 * knows what these registers mean, only that the sequence is required
 * before a firmware upload will succeed). write: cmd 0x08 <u32le addr>
 * <u32le val> <u8 0x04>. read: cmd 0x07 <u32le addr> <u8 0x04>, reply
 * is status word + u32le value. Both return 0/value success, -1 on
 * failure status or transport error (read has no separate error
 * output channel beyond that, matching python raising on bad status
 * before ever unpacking a value). */
int metallica_mis_write_hw_reg32(metallica_mis_tls_t *tls, uint32_t addr, uint32_t val);
int metallica_mis_read_hw_reg32(metallica_mis_tls_t *tls, uint32_t addr, uint32_t *out_val);

/* metallica_mis_reboot() -- sends cmd 0x05 0x02 0x00 via
 * metallica_mis_tls_cmd(), the exact 3 bytes sensor.py's reboot()
 * sends (`unhex('050200')`). Direct port -- this really is the
 * entire command, nothing more to it. python raises a
 * RebootException() immediately after to signal the caller that the
 * device is about to disconnect/re-enumerate; this port doesn't have
 * an exception mechanism, so it just returns 0 on success (status
 * word OK) and the CALLER must treat that as "device is rebooting
 * now" -- do not send anything else to tls/the transport after this
 * returns 0, the device will not be there to receive it. Returns -1
 * if the status word itself indicated failure or the transport
 * failed outright (in which case the device likely did NOT reboot). */
int metallica_mis_reboot(metallica_mis_tls_t *tls);

/* ==================== pairing orchestration ==================== */
/* Declared here (not in metallica_mis_init_flash.h) because both
 * need metallica_mis_flash_info_t, which only this header defines --
 * init_flash.h can't include this file back (this file already
 * includes init_flash.h), so this is the one layer both sides can
 * safely depend on. Implemented in metallica_mis_init_flash.c, which
 * includes this header for exactly this reason. */

/* metallica_mis_partition_flash() -- direct port of init_flash.py's
 * partition_flash(). Sends cmd 0x4f (flash params + partition table
 * + signature + a fresh self-signed cert over client_keypair's public
 * point + the hardcoded firmware CA cert) via metallica_mis_tls_cmd()
 * (plaintext at this point in pairing -- see metallica_mis_tls_cmd()
 * doc comment), then hands the device's returned cert blob to
 * metallica_mis_handle_cert(identity, ...) -- identity is passed
 * directly (NOT read from tls->identity, which is const/read-only
 * and typically not even set yet at this point in pairing; this
 * function is one of the things that POPULATES identity, before
 * tls_open() is ever called with it). layout/layout_count and
 * signature/signature_len are the flash_layout_hardcoded[_0090] and
 * partition_signature[_0090] tables from metallica_mis_init_flash.c,
 * selected by metallica_mis_init_flash() based on VID:PID (matching
 * python's `if usb_dev.idVendor == 0x138a and idProduct == 0x0090`
 * check). Returns 0 on success, -1 on any protocol failure. NOTE:
 * python leaves a `# TODO - figure out what the rest of rsp means`
 * after crt_len bytes are consumed -- ported as a documented gap,
 * not silently dropped: the remaining response bytes are currently
 * discarded here too, matching python's own actual behavior (it
 * never uses them either), not a shortcut this port is taking on its
 * own. */
int metallica_mis_partition_flash(metallica_mis_tls_t *tls, metallica_mis_identity_t *identity,
                                   const metallica_mis_flash_info_t *info,
                                   const metallica_mis_partition_info_t *layout, size_t layout_count,
                                   const unsigned char *signature, size_t signature_len,
                                   const EC_KEY *client_keypair);

/* metallica_mis_init_flash() -- direct port of init_flash.py's
 * init_flash(), the top-level pairing orchestrator. Full sequence,
 * matching python exactly (including the plaintext-vs-secure timing,
 * which only works now that metallica_mis_tls_cmd() dual-mode
 * dispatches -- see that function's doc comment):
 *
 *   1. get_flash_info() -- if partitions already exist, this device
 *      is already paired: return 0 immediately, nothing else to do
 *      (matches python's early return, NOT an error).
 *   2. Otherwise: send reset_blob (06cb:009a variant, see
 *      metallica_mis_blobs_9a.h) via metallica_mis_tls_cmd() in
 *      plaintext.
 *   3. Generate a fresh SECP256R1 keypair locally (the actual pairing
 *      secret -- NOT read from the device).
 *   4. Select flash_layout_hardcoded/partition_signature based on
 *      usb_vid/usb_pid (0090 variant vs the default/009a variant --
 *      NOTE: only the 009a variant's reset_blob/db_write_enable
 *      blobs exist in this codebase; passing 0090 VID:PID here will
 *      select the 0090 layout/signature tables correctly but
 *      metallica_mis_write_enable()/erase_flash() will still send
 *      the 009a blobs underneath, since metallica_mis_flash.c only
 *      has 009a's -- NOT SAFE for real 0090 hardware yet, this
 *      project has none to test against; 009a is the only variant
 *      this function should be considered ready for).
 *   5. metallica_mis_partition_flash() with the selected layout.
 *   6. Read the device's ECDH pubkey via cmd 0x50, hand to
 *      metallica_mis_handle_ecdh(). (python's RomInfo.get() call
 *      here is skipped -- its own comment says the result isn't used
 *      yet either, `# TODO: use the firmware version...`; skipping a
 *      currently-inert call is not skipping real functionality.)
 *   7. metallica_mis_handle_priv(encrypt_key(client_private,
 *      client_public)) -- wraps and hands over our fresh keypair.
 *   8. metallica_mis_tls_open() -- the actual first real handshake.
 *   9. erase_flash() partitions 1,2,5,6,4 in that exact order
 *      (matches python).
 *  10. write_flash(1, 0, make_tls_flash()) -- persist the paired
 *      identity to the cert partition.
 *  11. Caller's responsibility, NOT done here: send the reboot
 *      command afterward (python's reboot() at the very end) -- no
 *      reboot primitive exists in this codebase yet, so this
 *      function stops at step 10 and returns success; the caller
 *      must reboot the device before anything else will work
 *      correctly (matches how e.g. write_flash_all doesn't exist yet
 *      either -- documented gap, not a silent omission).
 *
 * identity must be zero-initialized by the caller before this call
 * (metallica_mis_handle_ecdh/_priv/_cert all populate it in place).
 * Returns 0 on success (including the "already paired, nothing to
 * do" case), -1 on any step's failure. */
int metallica_mis_init_flash(metallica_mis_tls_t *tls, metallica_mis_identity_t *identity,
                              const char *product_name, const char *serial_number,
                              uint16_t usb_vid, uint16_t usb_pid);

#endif /* __METALLICA_MIS_FLASH_H */
