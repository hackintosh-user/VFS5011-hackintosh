#ifndef MMIS_FACTORY_BITS_H
#define MMIS_FACTORY_BITS_H

/*
 * mmis_factory_bits.h
 *
 * Port of sensor.py's module-level get_factory_bits(tag), needed because
 * Sensor.open() calls it once (tag=0x0e00) to get factory_calibration_values,
 * which build_cmd_02()/line_update_type_1() require for every CALIBRATE/
 * ENROLL/IDENTIFY capture -- including the calibrate() orchestration loop
 * (metallica_mis_do_calibrate() in metallica_mis_daemon.c).
 *
 * Python:
 *   def get_factory_bits(tag: int):
 *       rsp = tls.cmd(pack('<B H HL', 0x6f, tag, 0, 0))
 *       assert_status(rsp)
 *       rsp = rsp[2:]
 *       wtf, entries = unpack('<LL', rsp[:8])
 *       rsp = rsp[8:]
 *       rc = {}
 *       for x in range(0, entries):
 *           hdr, rsp = rsp[:12], rsp[12:]
 *           ptr, l, tag, subtag, flags = unpack('<LHHHH', hdr)
 *           value = rsp[:l]
 *           if len(value) != l: raise Exception(...)
 *           rc[subtag] = value
 *           rsp = rsp[l:]
 *       if len(rsp) > 0: raise Exception('Garbage at the end of reply')
 *       return rc
 *
 * This C port has no dict, so instead of returning every subtag it takes
 * the ONE subtag the caller wants and copies just that value out -- but it
 * still walks and validates every entry in the reply exactly like python
 * does (a truncated entry or trailing garbage after the last entry both
 * fail the call), so a malformed reply is never silently accepted just
 * because the wanted subtag happened to parse first.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "metallica_mis_tls.h"

/* metallica_mis_get_factory_bits() -- sends cmd 0x6f <u16le tag> <u16le 0>
 * <u32le 0> (9 bytes total, matches pack('<B H HL', 0x6f, tag, 0, 0) exactly)
 * via metallica_mis_tls_cmd(), parses the entry table, and copies the value
 * for `want_subtag` into `out` (up to out_max bytes -- fails if the value
 * is larger). Returns true (and sets *out_len) only if want_subtag was
 * found AND the whole reply parsed cleanly (no truncated entry, no
 * trailing garbage). Returns false on any transport failure, malformed
 * reply, or if want_subtag was never present -- python would instead
 * return a dict without that key, but this port has no way to signal
 * "parsed fine, subtag just absent" separately from "parse failed"; that's
 * acceptable since sensor.py's only real caller unconditionally indexes
 * factory_bits[3], which would itself raise KeyError in the same case. */
bool metallica_mis_get_factory_bits(metallica_mis_tls_t *tls, uint16_t tag, uint16_t want_subtag,
                                     uint8_t *out, size_t out_max, size_t *out_len);

/* metallica_mis_get_factory_calibration_values() -- the one call sensor.py's
 * open() actually needs: `self.factory_calibration_values =
 * get_factory_bits(0x0e00)[3][4:]`. Thin wrapper: tag=0x0e00, subtag=3,
 * then skips the first 4 bytes of the value (the `[4:]` slice, not a
 * guess -- ported literally). Returns true and sets *out_len on success. */
bool metallica_mis_get_factory_calibration_values(metallica_mis_tls_t *tls,
                                                    uint8_t *out, size_t out_max, size_t *out_len);

#endif /* MMIS_FACTORY_BITS_H */
