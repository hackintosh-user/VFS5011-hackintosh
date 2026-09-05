#ifndef MMIS_ROM_INFO_H
#define MMIS_ROM_INFO_H

#include <stdint.h>

#include "metallica_mis_tls.h"

/* build_cmd_02() gates on rom_info.product == this value; upstream has no
 * ported code path for any other product byte. */
#define MMIS_ROM_INFO_EXPECTED_PRODUCT 0x30

typedef struct {
    uint32_t timestamp;
    uint32_t build;
    uint8_t  major;
    uint8_t  minor;
    uint8_t  product;
    uint8_t  u1;
} MmisRomInfo;

/* Sends cmd 0x01 (RomInfo.get()) through tls and parses the response.
 * Matches upstream's RomInfo.get(). Returns 0 on success, -1 on any I/O,
 * status, or short-response failure -- same convention as the rest of the
 * metallica_mis_* API (see metallica_mis_get_flash_info()). */
int metallica_mis_rom_info_get(metallica_mis_tls_t *tls, MmisRomInfo *out);

/* Prints the rom_info fields, plus a warning if product doesn't match
 * MMIS_ROM_INFO_EXPECTED_PRODUCT (the value build_cmd_02() requires). */
void mmis_rom_info_print(const MmisRomInfo *info);

#endif /* MMIS_ROM_INFO_H */
