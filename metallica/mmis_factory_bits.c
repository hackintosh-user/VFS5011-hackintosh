/*
 * mmis_factory_bits.c
 *
 * Implements mmis_factory_bits.h -- port of sensor.py's module-level
 * get_factory_bits(tag) and the one-shot Sensor.open() call
 * `factory_calibration_values = get_factory_bits(0x0e00)[3][4:]`.
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
 *           if len(value) != l: raise Exception('Truncated response %d != %d' % (len(value), l))
 *           rc[subtag] = value
 *           rsp = rsp[l:]
 *       if len(rsp) > 0: raise Exception('Garbage at the end of reply')
 *       return rc
 *
 * Matches the whole-reply validation exactly: every entry is walked and
 * length-checked (a truncated entry anywhere fails the call, not just one
 * containing the wanted subtag), and any trailing bytes after the last
 * entry fail the call too -- same as upstream's "Garbage at the end of
 * reply" check. If want_subtag appears more than once, the LAST occurrence
 * wins (mirrors Python's rc[subtag] = value dict overwrite), so this loop
 * deliberately does not stop at the first match.
 */

#include <stdio.h>
#include <string.h>

#include "mmis_factory_bits.h"

/* assert_status() -- duplicated per-file per this project's existing
 * convention (see metallica_mis_daemon.c's own copy). */
static int assert_status(const unsigned char *reply, int reply_len) {
    if (reply_len < 2) {
        fprintf(stderr, "mmis_factory_bits: reply too short to contain a status word (%d bytes)\n",
                reply_len);
        return -1;
    }
    unsigned short status = (unsigned short)reply[0] | ((unsigned short)reply[1] << 8);
    if (status != 0x0000) {
        fprintf(stderr, "mmis_factory_bits: command failed, status=0x%04x\n", status);
        return -1;
    }
    return 0;
}

static uint16_t rd_u16le(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool metallica_mis_get_factory_bits(metallica_mis_tls_t *tls, uint16_t tag, uint16_t want_subtag,
                                     uint8_t *out, size_t out_max, size_t *out_len) {
    /* pack('<B H HL', 0x6f, tag, 0, 0) -- 1 + 2 + 2 + 4 = 9 bytes */
    unsigned char cmd[9];
    cmd[0] = 0x6f;
    cmd[1] = (unsigned char)(tag & 0xff);
    cmd[2] = (unsigned char)((tag >> 8) & 0xff);
    cmd[3] = 0x00;
    cmd[4] = 0x00;
    cmd[5] = 0x00;
    cmd[6] = 0x00;
    cmd[7] = 0x00;
    cmd[8] = 0x00;

    unsigned char reply[4096];
    int n = metallica_mis_tls_cmd(tls, cmd, sizeof(cmd), reply, sizeof(reply));
    if (n < 0) {
        fprintf(stderr, "mmis_factory_bits: tls_cmd transport failure (%d)\n", n);
        return false;
    }
    if (assert_status(reply, n) != 0) {
        return false;
    }

    const uint8_t *rsp = reply + 2;
    size_t rsp_len = (size_t)n - 2;

    if (rsp_len < 8) {
        fprintf(stderr, "mmis_factory_bits: reply too short for wtf/entries header (%zu bytes)\n",
                rsp_len);
        return false;
    }

    /* wtf is unused (mirrors Python's discarded `wtf`) */
    uint32_t entries = rd_u32le(rsp + 4);
    rsp += 8;
    rsp_len -= 8;

    bool found = false;

    for (uint32_t i = 0; i < entries; i++) {
        if (rsp_len < 12) {
            fprintf(stderr, "mmis_factory_bits: truncated entry header at entry %u\n", i);
            return false;
        }

        uint16_t l       = rd_u16le(rsp + 4);
        uint16_t subtag  = rd_u16le(rsp + 8);
        /* ptr (rsp+0, u32), entry tag (rsp+6, u16), flags (rsp+10, u16)
         * are parsed by upstream but never used beyond the dict key --
         * not read here since nothing downstream needs them. */

        rsp += 12;
        rsp_len -= 12;

        if (rsp_len < l) {
            fprintf(stderr, "mmis_factory_bits: truncated value at entry %u (%zu != %u)\n",
                    i, rsp_len, l);
            return false;
        }

        if (subtag == want_subtag) {
            if (l > out_max) {
                fprintf(stderr, "mmis_factory_bits: value for subtag %u too large (%u > %zu)\n",
                        subtag, l, out_max);
                return false;
            }
            memcpy(out, rsp, l);
            *out_len = l;
            found = true;
        }

        rsp += l;
        rsp_len -= l;
    }

    if (rsp_len > 0) {
        fprintf(stderr, "mmis_factory_bits: garbage at the end of reply (%zu bytes)\n", rsp_len);
        return false;
    }

    return found;
}

bool metallica_mis_get_factory_calibration_values(metallica_mis_tls_t *tls,
                                                    uint8_t *out, size_t out_max, size_t *out_len) {
    uint8_t val[4096];
    size_t val_len = 0;

    if (!metallica_mis_get_factory_bits(tls, 0x0e00, 3, val, sizeof(val), &val_len)) {
        fprintf(stderr, "mmis_factory_bits: get_factory_calibration_values: subtag 3 lookup failed\n");
        return false;
    }

    if (val_len < 4) {
        fprintf(stderr, "mmis_factory_bits: factory value too short to slice [4:] (%zu bytes)\n",
                val_len);
        return false;
    }

    size_t sliced_len = val_len - 4;
    if (sliced_len > out_max) {
        fprintf(stderr, "mmis_factory_bits: sliced factory value too large for out buffer "
                         "(%zu > %zu)\n", sliced_len, out_max);
        return false;
    }

    memcpy(out, val + 4, sliced_len);
    *out_len = sliced_len;
    return true;
}
