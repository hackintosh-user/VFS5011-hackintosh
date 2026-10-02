#ifndef __METALLICA_MIS_DEBUG_H
#define __METALLICA_MIS_DEBUG_H

/*
 * metallica_mis_debug.h
 *
 * Shared --debug logging for the Metallica MIS backend (pairing,
 * firmware upload, calibration, record DB, and the future enroll
 * path). Everything here is a no-op unless g_metallica_mis_debug is
 * non-zero, so normal runs keep their current output exactly.
 *
 * Levels:
 *   0  off (default)
 *   1  --debug       every USB transfer, every TLS command/reply,
 *                    every DB call, with timestamps. Payloads larger
 *                    than MMIS_DBG_HEX_CAP bytes (firmware / flash
 *                    bulk chunks) print head + tail + length + FNV-1a
 *                    checksum instead of the full dump, to keep the
 *                    log postable.
 *   2  --debug-full  same, but no payload cap at all.
 *
 * All output goes to stderr, prefixed with a "+seconds" timestamp
 * relative to the first debug line, so a log can be read top to
 * bottom without correlating against anything else. Capture it with:
 *   sudo hack-touchid --debug ... 2>&1 | tee ~/htid-debug.log
 */

#include <stddef.h>
#include <stdint.h>

#define MMIS_DBG_HEX_CAP 4096

extern int g_metallica_mis_debug;

void mmis_dbg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Full hexdump (16 bytes/line, offsets, ASCII gutter) of buf, subject
 * to the MMIS_DBG_HEX_CAP rule above at level 1. */
void mmis_dbg_hex(const char *label, const unsigned char *buf, size_t len);

/* Key redaction. Key material is never printed. Writes
 * "<redacted, fp=xxxxxxxx>" into out (>= MMIS_KEY_FP_LEN bytes), where
 * fp is a 32-bit FNV-1a fingerprint, enough to see whether two runs
 * derived the same key without revealing it. */
#define MMIS_KEY_FP_LEN 32
void mmis_key_fp(const unsigned char *key, size_t len, char out[MMIS_KEY_FP_LEN]);

/* Human name for a TLS alert description byte (RFC 5246 7.2). */
const char *mmis_tls_alert_name(unsigned char desc);

/* Human name for the 2-byte little-endian status word most replies
 * start with. Returns "unknown" for anything not seen in real logs
 * yet -- unknown does NOT mean "bad", just "not catalogued". */
const char *mmis_status_name(uint16_t status);

/* Logs the status word at the front of `reply` (if >= 2 bytes) as
 * "<label>: status=0x%04x (<name>)". */
void mmis_dbg_status(const char *label, const unsigned char *reply, size_t reply_len);

#endif /* __METALLICA_MIS_DEBUG_H */
