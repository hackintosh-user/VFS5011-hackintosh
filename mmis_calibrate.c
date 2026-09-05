/*
 * mmis_calibrate.c
 *
 * Port of sensor.py's Sensor.persist_clean_slate() / Sensor.check_clean_slate()
 * from upstream python-validity, for the Metallica MIS (type 0x199) backend.
 *
 * ASSUMED existing primitives from the firmware-upload work (adjust names/
 * signatures below to match whatever you actually have in flash.c/.h):
 *
 *   bool metallica_mis_flash_read(int partition, uint32_t offset,
 *                                  uint8_t *out, size_t len);
 *   bool metallica_mis_flash_read_all(int partition, uint32_t offset,
 *                                      uint8_t *out, size_t len);
 *   bool metallica_mis_flash_write_all(int partition, uint32_t offset,
 *                                       const uint8_t *data, size_t len);
 *   bool metallica_mis_flash_erase(int partition);
 *
 * These mirror upstream's read_flash()/read_flash_all()/write_flash_all()/
 * erase_flash() from validitysensor/flash.py. If your actual primitives use
 * different names, this file only needs the calls in mmis_flash_* below
 * updated -- the logic itself is a direct 1:1 port.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/sha.h>

#include "mmis_calibrate.h"
#include "mmis_flash.h"   /* metallica_mis_flash_read / write_all / erase / read_all */

#define MMIS_CALIB_PARTITION   6
#define MMIS_CALIB_HEADER_LEN  0x44
#define MMIS_CALIB_MAGIC       0x5002

/*
 * persist_clean_slate(clean_slate, len)
 *
 * Python:
 *   def persist_clean_slate(self, clean_slate: bytes):
 *       start = read_flash(6, 0, 0x44)
 *       if start != b'\xff' * 0x44:
 *           if clean_slate[:0x44] == start:
 *               logging.info('Calibration data already matches the data on the flash.')
 *               return
 *           else:
 *               logging.info('Calibration flash already written. Erasing.')
 *               erase_flash(6)
 *       write_flash_all(6, 0, clean_slate)
 */
bool mmis_persist_clean_slate(const uint8_t *clean_slate, size_t clean_slate_len) {
    if (clean_slate_len < MMIS_CALIB_HEADER_LEN) {
        fprintf(stderr, "mmis_persist_clean_slate: clean_slate too short (%zu bytes)\n",
                clean_slate_len);
        return false;
    }

    uint8_t start[MMIS_CALIB_HEADER_LEN];
    if (!metallica_mis_flash_read(MMIS_CALIB_PARTITION, 0, start, MMIS_CALIB_HEADER_LEN)) {
        fprintf(stderr, "mmis_persist_clean_slate: flash read failed\n");
        return false;
    }

    uint8_t erased[MMIS_CALIB_HEADER_LEN];
    memset(erased, 0xff, sizeof(erased));

    if (memcmp(start, erased, MMIS_CALIB_HEADER_LEN) != 0) {
        /* Partition isn't in the erased (0xff-filled) state */
        if (memcmp(clean_slate, start, MMIS_CALIB_HEADER_LEN) == 0) {
            fprintf(stderr, "mmis_persist_clean_slate: calibration data already matches flash\n");
            return true;
        }

        fprintf(stderr, "mmis_persist_clean_slate: calibration flash already written, erasing\n");
        if (!metallica_mis_flash_erase(MMIS_CALIB_PARTITION)) {
            fprintf(stderr, "mmis_persist_clean_slate: flash erase failed\n");
            return false;
        }
    }

    if (!metallica_mis_flash_write_all(MMIS_CALIB_PARTITION, 0, clean_slate, clean_slate_len)) {
        fprintf(stderr, "mmis_persist_clean_slate: flash write failed\n");
        return false;
    }

    return true;
}

/*
 * check_clean_slate()
 *
 * Python:
 *   def check_clean_slate(self):
 *       start = read_flash(6, 0, 0x44)
 *       magic, l = unpack('<HH', start[:4])
 *       start = start[4:]
 *       if magic != 0x5002:
 *           return False
 *       hs, zeroes = start[0:0x20], start[0x20:0x40]
 *       if zeroes != b'\0' * 0x20:
 *           logging.warning('Unexpected contents in calibration flash partition')
 *           return False
 *       img = read_flash_all(6, 0x44, l)
 *       if hs != sha256(img).digest():
 *           logging.warning('Calibration flash hash mismatch')
 *           return False
 *       return True
 */
bool mmis_check_clean_slate(void) {
    uint8_t start[MMIS_CALIB_HEADER_LEN];
    if (!metallica_mis_flash_read(MMIS_CALIB_PARTITION, 0, start, MMIS_CALIB_HEADER_LEN)) {
        fprintf(stderr, "mmis_check_clean_slate: flash read failed\n");
        return false;
    }

    uint16_t magic = (uint16_t)(start[0] | (start[1] << 8));
    uint16_t l     = (uint16_t)(start[2] | (start[3] << 8));

    if (magic != MMIS_CALIB_MAGIC) {
        return false;
    }

    const uint8_t *hs     = start + 4;         /* 0x20 bytes: sha256 digest */
    const uint8_t *zeroes = start + 4 + 0x20;   /* 0x20 bytes: must be all-zero */

    uint8_t zero_block[0x20];
    memset(zero_block, 0, sizeof(zero_block));
    if (memcmp(zeroes, zero_block, sizeof(zero_block)) != 0) {
        fprintf(stderr, "mmis_check_clean_slate: unexpected contents in calibration flash partition\n");
        return false;
    }

    uint8_t *img = malloc(l);
    if (img == NULL) {
        fprintf(stderr, "mmis_check_clean_slate: allocation failed (%u bytes)\n", l);
        return false;
    }

    bool read_ok = metallica_mis_flash_read_all(MMIS_CALIB_PARTITION, MMIS_CALIB_HEADER_LEN, img, l);
    if (!read_ok) {
        fprintf(stderr, "mmis_check_clean_slate: flash read_all failed\n");
        free(img);
        return false;
    }

    uint8_t digest[SHA256_DIGEST_LENGTH]; /* 0x20 bytes */
    SHA256(img, l, digest);
    free(img);

    if (memcmp(hs, digest, SHA256_DIGEST_LENGTH) != 0) {
        fprintf(stderr, "mmis_check_clean_slate: calibration flash hash mismatch\n");
        return false;
    }

    return true;
}
