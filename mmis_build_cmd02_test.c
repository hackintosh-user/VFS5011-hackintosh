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
    uint8_t *calib_data = read_file("fake_calib_data.bin", &cdlen);

    size_t lines_per_frame;
    bool ok = mmis_get_lines_per_frame(prog, plen, 2, &lines_per_frame);
    printf("get_lines_per_frame: ok=%d lines_per_frame=%zu\n", ok, lines_per_frame);

    /* Case A: CALIBRATE, empty calib_data */
    {
        static uint8_t out[4096], scratch[8192];
        size_t olen = mmis_build_cmd_02(MMIS_CAPTURE_CALIBRATE, prog, plen,
                                         0x78, 3, lines_per_frame,
                                         2, 0x38, factory, flen,
                                         NULL, 0, 112, 112, CALIB_BLOB, sizeof(CALIB_BLOB),
                                         out, sizeof(out), scratch, sizeof(scratch));
        size_t reflen; uint8_t *ref = read_file("cmd02_caseA.bin", &reflen);
        printf("Case A: len=%zu ref_len=%zu match=%s header=%02x%02x%02x%02x%02x\n",
               olen, reflen, (olen==reflen && memcmp(out,ref,olen)==0) ? "YES":"NO",
               out[0],out[1],out[2],out[3],out[4]);
    }

    /* Case B: ENROLL, populated calib_data */
    {
        static uint8_t out[32768], scratch[32768];
        size_t olen = mmis_build_cmd_02(MMIS_CAPTURE_ENROLL, prog, plen,
                                         0x78, 3, lines_per_frame,
                                         2, 0x38, factory, flen,
                                         calib_data, cdlen, 112, 112, CALIB_BLOB, sizeof(CALIB_BLOB),
                                         out, sizeof(out), scratch, sizeof(scratch));
        size_t reflen; uint8_t *ref = read_file("cmd02_caseB.bin", &reflen);
        printf("Case B: len=%zu ref_len=%zu match=%s header=%02x%02x%02x%02x%02x\n",
               olen, reflen, (olen==reflen && memcmp(out,ref,olen)==0) ? "YES":"NO",
               out[0],out[1],out[2],out[3],out[4]);
    }

    return 0;
}
