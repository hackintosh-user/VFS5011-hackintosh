/*
 * mmis_rom_info.c
 *
 * Port of sensor.py's RomInfo class (RomInfo.get()) from upstream
 * python-validity, for the Metallica MIS backend.
 *
 * Python:
 *   class RomInfo:
 *       @classmethod
 *       def get(cls):
 *           rsp = tls.cmd(b'\x01')
 *           assert_status(rsp)
 *           rsp = rsp[2:]
 *           return cls(*unpack('<LLBBxBxxxB', rsp[0:0x10]))
 *
 * Matches the real metallica_mis_tls_cmd()/assert_status() signatures
 * already used across the repo (see metallica_mis_flash.c's
 * metallica_mis_get_flash_info() for the identical pattern this follows).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mmis_rom_info.h"
#include "metallica_mis_tls.h"

/* assert_status() -- python-validity's convention (see util.py's
 * assert_status()) is that most command replies start with a 2-byte
 * little-endian status word, 0x0000 meaning success. Same convention/
 * copy as metallica_mis_daemon.c and metallica_mis_flash.c. Returns 0 if
 * status is OK, -1 otherwise. */
static int assert_status(const unsigned char *reply, int reply_len) {
    if (reply_len < 2) {
        fprintf(stderr, "metallica_mis: reply too short to contain a status word (%d bytes)\n",
                reply_len);
        return -1;
    }
    unsigned short status = (unsigned short)reply[0] | ((unsigned short)reply[1] << 8);
    if (status != 0x0000) {
        fprintf(stderr, "metallica_mis: command failed, status=0x%04x\n", status);
        return -1;
    }
    return 0;
}

/*
 * Layout of unpack('<LLBBxBxxxB', rsp[0:0x10]) -- all little-endian, offsets
 * relative to rsp AFTER the 2-byte status word is stripped:
 *
 *   offset  size  field
 *   0       4     timestamp   (L)
 *   4       4     build       (L)
 *   8       1     major       (B)
 *   9       1     minor       (B)
 *   10      1     --pad--     (x)
 *   11      1     product     (B)
 *   12      3     --pad--     (xxx)
 *   15      1     u1          (B)
 */
int metallica_mis_rom_info_get(metallica_mis_tls_t *tls, MmisRomInfo *out) {
    unsigned char cmd = 0x01;
    unsigned char rsp[64];
    int n;

    if (out == NULL) return -1;
    memset(out, 0, sizeof(*out));

    n = metallica_mis_tls_cmd(tls, &cmd, 1, rsp, sizeof(rsp));
    if (n < 0) return -1;
    if (assert_status(rsp, n) != 0) return -1;

    /* rsp = rsp[2:] then unpack rsp[0:0x10] -- need 2 (status) + 0x10 (payload) */
    if (n < 2 + 0x10) {
        fprintf(stderr, "metallica_mis: rom_info reply too short (%d bytes)\n", n);
        return -1;
    }

    const unsigned char *p = rsp + 2;

    out->timestamp = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                      ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    out->build     = (uint32_t)p[4] | ((uint32_t)p[5] << 8) |
                      ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
    out->major     = p[8];
    out->minor     = p[9];
    /* p[10] is padding, skipped */
    out->product   = p[11];
    /* p[12..14] is padding, skipped */
    out->u1        = p[15];

    return 0;
}

void mmis_rom_info_print(const MmisRomInfo *info) {
    if (info == NULL) return;

    printf("rom_info: timestamp=%u build=%u major=%u minor=%u product=0x%02x u1=%u\n",
           info->timestamp, info->build, info->major, info->minor,
           info->product, info->u1);

    if (info->product != MMIS_ROM_INFO_EXPECTED_PRODUCT) {
        printf("rom_info: WARNING product=0x%02x (expected 0x%02x) -- "
               "build_cmd_02() gate will reject this device until support "
               "for this product value is ported\n",
               info->product, MMIS_ROM_INFO_EXPECTED_PRODUCT);
    }
}
