/*
 * metallica_mis_flash.c
 *
 * C port of python-validity's validitysensor/flash.py (get_flash_info,
 * erase_flash, write_flash, call_cleanups, write_enable) + the JEDEC
 * flash-IC lookup table from hw_tables.py. See metallica_mis_flash.h
 * for scope notes -- ported directly against real source pulled Aug
 * 20 2026, not from memory.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "metallica_mis_flash.h"
#include "metallica_mis_blobs_9a.h"

/* ==================== JEDEC flash IC table ==================== */
/* Verbatim port of hw_tables.py's flash_ic_table (20 entries). */
static const metallica_mis_flash_ic_info_t metallica_mis_flash_ic_table[] = {
    { "M25P05-A", 65536, 0x5, 0x20, 0x20, 0x10, 0x8000, 0x0, 0x8000, 0xd8, 0x0, 0x4 },
    { "M25P10-A", 131072, 0x10, 0x20, 0x20, 0x11, 0x8000, 0x0, 0x8000, 0xd8, 0x0, 0x4 },
    { "M25P20", 262144, 0x11, 0x20, 0x20, 0x12, 0x0, 0x1, 0x10000, 0xd8, 0x0, 0x4 },
    { "M25P40", 524288, 0x12, 0x20, 0x20, 0x13, 0x0, 0x1, 0x10000, 0xd8, 0x0, 0x4 },
    { "M25P80", 1048576, 0x13, 0x20, 0x20, 0x14, 0x0, 0x1, 0x10000, 0xd8, 0x0, 0x4 },
    { "M25P16", 2097152, 0x14, 0x20, 0x20, 0x15, 0x0, 0x1, 0x10000, 0xd8, 0x0, 0x4 },
    { "M25P32", 4194304, 0x15, 0x20, 0x20, 0x16, 0x0, 0x1, 0x10000, 0xd8, 0x0, 0x4 },
    { "M25P64", 8388608, 0x16, 0x20, 0x20, 0x17, 0x0, 0x1, 0x10000, 0xd8, 0x0, 0x4 },
    { "SST25VF040B", 524288, 0x8d, 0xbf, 0x25, 0x8d, 0x0, 0x1, 0x10000, 0xd8, 0x0, 0x4 },
    { "W25X10A", 131072, 0x10, 0xef, 0x30, 0x11, 0x0, 0x1, 0x1000, 0x20, 0x0, 0x4 },
    { "W25X20A", 262144, 0x11, 0xef, 0x30, 0x12, 0x0, 0x1, 0x1000, 0x20, 0x0, 0x4 },
    { "W25X40A", 524288, 0x12, 0xef, 0x30, 0x13, 0x0, 0x1, 0x1000, 0x20, 0x0, 0x4 },
    { "W25X80A", 1048576, 0x13, 0xef, 0x30, 0x14, 0x0, 0x1, 0x1000, 0x20, 0x0, 0x4 },
    { "W25Q40B", 524288, 0x12, 0xef, 0x40, 0x13, 0x0, 0x1, 0x1000, 0x20, 0x0, 0x4 },
    { "W25Q80B", 1048576, 0x13, 0xef, 0x40, 0x14, 0x0, 0x1, 0x1000, 0x20, 0x0, 0x4 },
    { "MX25L4006E", 524288, 0x12, 0xc2, 0x20, 0x13, 0x0, 0x1, 0x1000, 0x20, 0x0, 0x4 },
    { "EN25Q40", 524288, 0x12, 0x1c, 0x30, 0x13, 0x0, 0x1, 0x1000, 0x20, 0x0, 0x4 },
    { "MX25V8035F", 1048576, 0x14, 0xc2, 0x23, 0x14, 0x0, 0x1, 0x1000, 0x20, 0x0, 0x4 },
    { "AT25SF081", 1048576, 0x14, 0x1f, 0x85, 0x1, 0x0, 0x1, 0x1000, 0x20, 0x0, 0x4 },
    { "GD25Q80C", 1048576, 0x13, 0xc8, 0x40, 0x14, 0x0, 0x1, 0x1000, 0x20, 0x0, 0x4 },
};

#define METALLICA_MIS_FLASH_IC_TABLE_LEN \
    (sizeof(metallica_mis_flash_ic_table) / sizeof(metallica_mis_flash_ic_table[0]))

const metallica_mis_flash_ic_info_t *metallica_mis_flash_ic_table_lookup(uint16_t jedec_id0,
                                                                           uint16_t jedec_id1,
                                                                           uint32_t size) {
    for (size_t i = 0; i < METALLICA_MIS_FLASH_IC_TABLE_LEN; i++) {
        const metallica_mis_flash_ic_info_t *e = &metallica_mis_flash_ic_table[i];
        if (e->jid0 == jedec_id0 && e->jid1 == jedec_id1 && e->size == size) {
            return e;
        }
    }
    return NULL;
}

/* ==================== status checking ==================== */

/* assert_status() -- same convention as metallica_mis_daemon.c's
 * static helper of the same name (kept file-local rather than shared
 * across translation units, matching that file's own scoping). See
 * util.py's assert_status(). */
static int assert_status(const unsigned char *reply, int reply_len) {
    if (reply_len < 2) {
        fprintf(stderr, "metallica_mis_flash: reply too short to contain a status word (%d bytes)\n",
                reply_len);
        return -1;
    }
    unsigned short status = (unsigned short)reply[0] | ((unsigned short)reply[1] << 8);
    if (status != 0x0000) {
        fprintf(stderr, "metallica_mis_flash: command failed, status=0x%04x\n", status);
        return -1;
    }
    return 0;
}

/* ==================== get_flash_info ==================== */

int metallica_mis_get_flash_info(metallica_mis_tls_t *tls, metallica_mis_flash_info_t *out) {
    unsigned char cmd = 0x3e;
    unsigned char rsp[8192];
    int n;

    memset(out, 0, sizeof(*out));

    n = metallica_mis_tls_cmd(tls, &cmd, 1, rsp, sizeof(rsp));
    if (n < 0) return -1;
    if (assert_status(rsp, n) != 0) return -1;

    /* rsp = rsp[2:]; hdr = rsp[:0xe] -- 7x u16le: jid0, jid1, blocks,
     * unknown0, blocksize, unknown1, pcnt */
    if (n < 2 + 0xe) return -1;
    const unsigned char *p = rsp + 2;

    uint16_t jid0     = (uint16_t)p[0]  | ((uint16_t)p[1]  << 8);
    uint16_t jid1     = (uint16_t)p[2]  | ((uint16_t)p[3]  << 8);
    uint16_t blocks   = (uint16_t)p[4]  | ((uint16_t)p[5]  << 8);
    uint16_t unknown0 = (uint16_t)p[6]  | ((uint16_t)p[7]  << 8);
    uint16_t blocksize= (uint16_t)p[8]  | ((uint16_t)p[9]  << 8);
    uint16_t unknown1 = (uint16_t)p[10] | ((uint16_t)p[11] << 8);
    uint16_t pcnt     = (uint16_t)p[12] | ((uint16_t)p[13] << 8);
    p += 0xe;
    size_t remaining = (size_t)n - 2 - 0xe;

    uint32_t size = (uint32_t)blocks * (uint32_t)blocksize;
    const metallica_mis_flash_ic_info_t *ic = metallica_mis_flash_ic_table_lookup(jid0, jid1, size);
    if (!ic) {
        fprintf(stderr,
                "metallica_mis_flash: Unknown flash IC. JEDEC id=%04x:%04x, size=%ux%u\n",
                jid0, jid1, (unsigned)blocks, (unsigned)blocksize);
        return -1; /* matches python's raised Exception('Unknown flash IC...') */
    }

    /* partitions = [rsp[i*0xc:(i+1)*0xc] for i in range(pcnt)], each
     * unpacked as <BBHLL (id, type, access_lvl, offset, size) */
    if (remaining < (size_t)pcnt * 0xc) return -1;

    metallica_mis_partition_info_t *partitions = NULL;
    if (pcnt > 0) {
        partitions = malloc(sizeof(metallica_mis_partition_info_t) * pcnt);
        if (!partitions) return -1;
        for (uint16_t i = 0; i < pcnt; i++) {
            const unsigned char *e = p + (size_t)i * 0xc;
            partitions[i].id         = e[0];
            partitions[i].type       = e[1];
            partitions[i].access_lvl = (uint16_t)e[2] | ((uint16_t)e[3] << 8);
            partitions[i].offset     = (uint32_t)e[4] | ((uint32_t)e[5] << 8) |
                                        ((uint32_t)e[6] << 16) | ((uint32_t)e[7] << 24);
            partitions[i].size       = (uint32_t)e[8] | ((uint32_t)e[9] << 8) |
                                        ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
        }
    }

    out->ic = ic;
    out->blocks = blocks;
    out->unknown0 = unknown0;
    out->blocksize = blocksize;
    out->unknown1 = unknown1;
    out->partitions = partitions;
    out->partition_count = pcnt;
    return 0;
}

void metallica_mis_flash_info_free(metallica_mis_flash_info_t *info) {
    free(info->partitions);
    info->partitions = NULL;
    info->partition_count = 0;
}

/* ==================== call_cleanups / write_enable ==================== */

int metallica_mis_flash_call_cleanups(metallica_mis_tls_t *tls) {
    unsigned char cmd = 0x1a;
    unsigned char rsp[64];
    int n = metallica_mis_tls_cmd(tls, &cmd, 1, rsp, sizeof(rsp));
    if (n < 2) return -1;

    unsigned short err = (unsigned short)rsp[0] | ((unsigned short)rsp[1] << 8);
    if (err == 0x0491) return 0; /* "Nothing to commit" -- treated as success, matches python exactly */

    return assert_status(rsp, n);
}

int metallica_mis_write_enable(metallica_mis_tls_t *tls) {
    unsigned char rsp[64];
    int n = metallica_mis_tls_cmd(tls, metallica_mis_db_write_enable_9a,
                                   metallica_mis_db_write_enable_9a_len, rsp, sizeof(rsp));
    if (n < 0) return -1;
    return assert_status(rsp, n);
}

/* ==================== erase_flash / write_flash ==================== */

int metallica_mis_erase_flash(metallica_mis_tls_t *tls, uint8_t partition) {
    if (metallica_mis_write_enable(tls) != 0) return -1;

    unsigned char cmd[2] = { 0x3f, partition };
    unsigned char rsp[64];
    int n = metallica_mis_tls_cmd(tls, cmd, sizeof(cmd), rsp, sizeof(rsp));

    int erase_ok = (n >= 0) && (assert_status(rsp, n) == 0);

    /* python's try/finally: call_cleanups() always runs; but the
     * original erase result is what propagates if the erase itself
     * failed (cleanup running afterward doesn't rescue a failed
     * erase). If the erase succeeded, cleanup's own result matters. */
    int cleanup_ok = (metallica_mis_flash_call_cleanups(tls) == 0);

    if (!erase_ok) return -1;
    if (!cleanup_ok) return -1;
    return 0;
}

int metallica_mis_write_flash(metallica_mis_tls_t *tls, uint8_t partition, uint32_t addr,
                               const unsigned char *buf, size_t buf_len) {
    /* NOTE: python sends db_write_enable here WITHOUT checking its
     * status (bare `tls.cmd(db_write_enable)`, no assert_status()) --
     * a real asymmetry vs write_enable()/erase_flash(). Preserved
     * as-is; see header comment for why this isn't "fixed" here. */
    {
        unsigned char rsp[64];
        metallica_mis_tls_cmd(tls, metallica_mis_db_write_enable_9a,
                               metallica_mis_db_write_enable_9a_len, rsp, sizeof(rsp));
    }

    /* cmd = pack('<BBBHLL', 0x41, partition, 1, 0, addr, len(buf)) + buf */
    unsigned char hdr[13];
    hdr[0] = 0x41;
    hdr[1] = partition;
    hdr[2] = 0x01;
    hdr[3] = 0x00; hdr[4] = 0x00; /* <H 0 */
    hdr[5]  = (unsigned char)(addr & 0xff);
    hdr[6]  = (unsigned char)((addr >> 8) & 0xff);
    hdr[7]  = (unsigned char)((addr >> 16) & 0xff);
    hdr[8]  = (unsigned char)((addr >> 24) & 0xff);
    uint32_t blen = (uint32_t)buf_len;
    hdr[9]  = (unsigned char)(blen & 0xff);
    hdr[10] = (unsigned char)((blen >> 8) & 0xff);
    hdr[11] = (unsigned char)((blen >> 16) & 0xff);
    hdr[12] = (unsigned char)((blen >> 24) & 0xff);

    unsigned char *cmd = malloc(sizeof(hdr) + buf_len);
    if (!cmd) return -1;
    memcpy(cmd, hdr, sizeof(hdr));
    memcpy(cmd + sizeof(hdr), buf, buf_len);

    unsigned char rsp[8192];
    int n = metallica_mis_tls_cmd(tls, cmd, sizeof(hdr) + buf_len, rsp, sizeof(rsp));
    free(cmd);

    int write_ok = (n >= 0) && (assert_status(rsp, n) == 0);
    int cleanup_ok = (metallica_mis_flash_call_cleanups(tls) == 0);

    if (!write_ok) return -1;
    if (!cleanup_ok) return -1;
    return 0;
}
