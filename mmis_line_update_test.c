#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "timeslot.h"

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(sz);
    fread(buf, 1, sz, f);
    fclose(f);
    *len = (size_t)sz;
    return buf;
}

static const uint8_t CALIB_BLOB[112] = {
    0x9b,0x9a,0x99,0x97,0x96,0x95,0x93,0x92,0x91,0x8f,0x8e,0x8d,0x8b,0x8a,0x89,0x87,
    0x86,0x85,0x83,0x82,0x81,0x7f,0x7e,0x7d,0x7b,0x7a,0x79,0x77,0x76,0x75,0x73,0x72,
    0x71,0x6f,0x6e,0x6d,0x6b,0x6a,0x69,0x67,0x66,0x65,0x63,0x62,0x61,0x5f,0x5e,0x5d,
    0x5b,0x5a,0x59,0x57,0x56,0x55,0x52,0x51,0x50,0x4e,0x4d,0x4c,0x4a,0x49,0x48,0x46,
    0x45,0x44,0x42,0x41,0x40,0x3e,0x3d,0x3c,0x3a,0x39,0x38,0x36,0x35,0x34,0x32,0x31,
    0x30,0x2e,0x2d,0x2c,0x2a,0x29,0x28,0x26,0x25,0x24,0x22,0x21,0x20,0x1e,0x1d,0x1c,
    0x1a,0x19,0x18,0x16,0x15,0x14,0x12,0x11,0x10,0x0e,0x0d,0x0c,0x0a,0x09,0x08,0x06,
};

int main(void) {
    size_t plen, flen, cdlen;
    uint8_t *prog = read_file("prog0199.bin", &plen);
    uint8_t *factory = read_file("factory_calibration_values.bin", &flen);
    uint8_t *caseA_ref = read_file("lu_caseA_merged.bin", &cdlen);
    (void)caseA_ref;

    /* ---- Case A: CALIBRATE, empty calib_data ---- */
    {
        mmis_chunk_t chunks[32];
        size_t n = mmis_split_chunks(prog, plen, chunks, 32);
        static uint8_t scratch[8192];
        bool ok = mmis_line_update_type_1(MMIS_CAPTURE_CALIBRATE, chunks, &n, 32,
                                           2, 0x38, factory, flen,
                                           NULL, 0, 112, 112, CALIB_BLOB, sizeof(CALIB_BLOB),
                                           scratch, sizeof(scratch));
        printf("Case A: ok=%d n_chunks=%zu\n", ok, n);

        uint8_t merged[4096];
        size_t mlen = mmis_merge_chunks(chunks, n, merged, sizeof(merged));

        size_t reflen;
        uint8_t *ref = read_file("lu_caseA_merged.bin", &reflen);
        printf("Case A merged_len=%zu ref_len=%zu match=%s\n", mlen, reflen,
               (mlen == reflen && memcmp(merged, ref, mlen) == 0) ? "YES" : "NO");
        if (mlen != reflen || memcmp(merged, ref, mlen) != 0) {
            size_t i;
            for (i = 0; i < (mlen < reflen ? mlen : reflen); i++) {
                if (merged[i] != ref[i]) { printf("  first diff at byte %zu: C=0x%02x ref=0x%02x\n", i, merged[i], ref[i]); break; }
            }
        }
    }

    /* ---- Case B: ENROLL, populated calib_data ---- */
    {
        uint8_t *calib_data = read_file("fake_calib_data.bin", &cdlen);
        mmis_chunk_t chunks[32];
        size_t n = mmis_split_chunks(prog, plen, chunks, 32);
        static uint8_t scratch[32768];
        bool ok = mmis_line_update_type_1(MMIS_CAPTURE_ENROLL, chunks, &n, 32,
                                           2, 0x38, factory, flen,
                                           calib_data, cdlen, 112, 112, CALIB_BLOB, sizeof(CALIB_BLOB),
                                           scratch, sizeof(scratch));
        printf("\nCase B: ok=%d n_chunks=%zu\n", ok, n);

        uint8_t merged[32768];
        size_t mlen = mmis_merge_chunks(chunks, n, merged, sizeof(merged));

        size_t reflen;
        uint8_t *ref = read_file("lu_caseB_merged.bin", &reflen);
        printf("Case B merged_len=%zu ref_len=%zu match=%s\n", mlen, reflen,
               (mlen == reflen && memcmp(merged, ref, mlen) == 0) ? "YES" : "NO");
        if (mlen != reflen || memcmp(merged, ref, mlen) != 0) {
            size_t i;
            for (i = 0; i < (mlen < reflen ? mlen : reflen); i++) {
                if (merged[i] != ref[i]) { printf("  first diff at byte %zu: C=0x%02x ref=0x%02x\n", i, merged[i], ref[i]); break; }
            }
        }
    }

    return 0;
}
