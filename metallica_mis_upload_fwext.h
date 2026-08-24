#ifndef __METALLICA_MIS_UPLOAD_FWEXT_H
#define __METALLICA_MIS_UPLOAD_FWEXT_H

/*
 * metallica_mis_upload_fwext.h
 *
 * C port of python-validity's validitysensor/upload_fwext.py, ported
 * directly against real source Mohammad pulled Aug 24 2026, not from
 * memory. This is NOT part of pairing (metallica_mis_init_flash()) --
 * upstream calls this from open_common(), AFTER send_init() and
 * (if needed) init()/pairing, EVERY time the device is opened,
 * whether or not it was just freshly paired. If firmware is already
 * loaded, this is a fast no-op (one get_fw_info() round-trip).
 *
 * SCOPE REDUCTIONS vs upstream (intentional, not gaps):
 *   - upstream's upload_fwext(fw_path=None) accepts an optional
 *     explicit path and otherwise resolves a per-device default via
 *     identify_sensor() + FIRMWARE_NAMES[dev]. This project only
 *     supports 06cb:009a, so the exact filename is already hardcoded
 *     in metallica_mis_firmware.h (METALLICA_MIS_FW_FILENAME) --
 *     identify_sensor() is not ported, it exists upstream only to
 *     pick a filename we already know.
 *   - If the firmware file isn't present at
 *     metallica_mis_firmware_path() yet, this function calls
 *     metallica_mis_firmware_fetch() itself rather than requiring the
 *     caller to have already fetched it (upstream just open()s
 *     fw_path and lets a missing file raise -- we have a working
 *     fetcher already built for the client's enroll path, so reusing
 *     it here for the daemon path too is a strict improvement, not a
 *     deviation in observable protocol behavior).
 *
 * WHAT THIS DOES NOT DO: upstream's upload_fwext() ends by calling
 * reboot() and then unconditionally raising RebootException() -- the
 * caller is expected to treat that as "device is gone now, stop."
 * This port's metallica_mis_upload_fwext() calls
 * metallica_mis_reboot() as its own last step and returns 0 on
 * success; same as metallica_mis_daemon.c's do_pairing()/
 * metallica_mis_reboot() doc comment: DO NOT send anything else to
 * tls/the transport after this returns 0, the device will not be
 * there to receive it.
 */

#include <stdbool.h>
#include "metallica_mis_tls.h"

/* metallica_mis_upload_fwext() -- full port of upload_fwext():
 *   1. get_fw_info(tls, 2, ...) -- if firmware already loaded, log
 *      version and return 0 immediately (matches python's early
 *      return, NOT an error).
 *   2. write_hw_reg32(0x8000205c, 7), then read_hw_reg32(0x80002080)
 *      and require the result to be 2 or 3 -- ported verbatim from
 *      upstream's own "no idea what this is" precondition check.
 *      Fails (-1) if the register doesn't come back 2 or 3, matching
 *      python's raised Exception('Unexpected register value').
 *   3. Resolve the firmware file via metallica_mis_firmware_path(),
 *      fetching it via metallica_mis_firmware_fetch() first if not
 *      already present (see header note above -- this is the one
 *      deliberate behavioral addition vs upstream).
 *   4. Read the whole file, find the first raw 0x1a byte anywhere in
 *      it and discard everything up to and including that byte (this
 *      exact scan-for-0x1a-then-drop-the-prefix logic is ported
 *      verbatim from upstream -- the meaning of that prefix isn't
 *      documented upstream either), then split the LAST 0x100 bytes
 *      off as the signature -- everything remaining is the actual
 *      fwext body.
 *   5. metallica_mis_write_flash_all(tls, 2, 0, fwext_body, ...)
 *   6. metallica_mis_write_fw_signature(tls, 2, signature, 0x100)
 *   7. get_fw_info(tls, 2, ...) again to confirm the upload actually
 *      took -- fails (-1) if firmware still isn't detected, matching
 *      python's raised Exception('No firmware detected').
 *   8. metallica_mis_reboot(tls) -- see header note above for what
 *      happens after this returns 0.
 *
 * Returns 0 on success (including the "already loaded, nothing to
 * do" case), -1 on any step's failure. Must be called with `tls` in
 * an already-open secure session (i.e. AFTER metallica_mis_tls_open()
 * succeeded) -- unlike the plaintext-capable flash.c primitives this
 * calls, every command in this specific sequence is upstream-documented
 * as running post-handshake only. */
int metallica_mis_upload_fwext(metallica_mis_tls_t *tls);

#endif /* __METALLICA_MIS_UPLOAD_FWEXT_H */
