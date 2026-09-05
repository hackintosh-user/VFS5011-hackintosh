#ifndef MMIS_CALIBRATE_H
#define MMIS_CALIBRATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Writes the clean-slate calibration blob to flash partition 6, matching
 * upstream python-validity's Sensor.persist_clean_slate(). Skips the write
 * if the flash already holds identical data; erases first if it holds
 * different (non-erased) data. */
bool mmis_persist_clean_slate(const uint8_t *clean_slate, size_t clean_slate_len);

/* Validates the clean-slate blob already on flash partition 6: checks the
 * magic (0x5002), the reserved zero block, and a SHA-256 hash match against
 * the stored image. Matches upstream's Sensor.check_clean_slate(). Returns
 * true only if a valid, matching clean-slate blob is already present. */
bool mmis_check_clean_slate(void);

#endif /* MMIS_CALIBRATE_H */
