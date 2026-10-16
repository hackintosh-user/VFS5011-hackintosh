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

int main(void) {
    size_t lines_per_frame = 224, bytes_per_line = 0x78, lines_per_calibration_data = 112;

    static uint8_t calib_data[16384];
    size_t calib_data_len = 0;

    const char *raw_files[3] = {"raw_calib_1.bin", "raw_calib_2.bin", "raw_calib_3.bin"};

    for (int i = 0; i < 3; i++) {
        size_t rawlen;
        uint8_t *raw = read_file(raw_files[i], &rawlen);

        static uint8_t avg[16384];
        size_t avg_len;
        bool ok1 = mmis_average(raw, rawlen, lines_per_frame, bytes_per_line, lines_per_calibration_data,
                                 avg, sizeof(avg), &avg_len);

        static uint8_t new_calib[16384];
        size_t new_calib_len;
        bool ok2 = mmis_process_calibration_results(avg, avg_len, bytes_per_line,
                                                      calib_data, calib_data_len,
                                                      new_calib, sizeof(new_calib), &new_calib_len);

        printf("iter %d: avg ok=%d len=%zu | process ok=%d len=%zu\n", i, ok1, avg_len, ok2, new_calib_len);

        memcpy(calib_data, new_calib, new_calib_len);
        calib_data_len = new_calib_len;
        free(raw);
    }

    size_t reflen;
    uint8_t *ref = read_file("calib_data_final.bin", &reflen);
    printf("\nFinal: C_len=%zu ref_len=%zu match=%s\n", calib_data_len, reflen,
           (calib_data_len == reflen && memcmp(calib_data, ref, reflen) == 0) ? "YES" : "NO");
    printf("C   first 32 bytes: ");
    for (int i = 0; i < 32; i++) printf("%02x", calib_data[i]);
    printf("\nref first 32 bytes: ");
    for (int i = 0; i < 32; i++) printf("%02x", ref[i]);
    printf("\n");

    return 0;
}
