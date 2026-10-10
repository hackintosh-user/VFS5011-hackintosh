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
    size_t plen;
    uint8_t *prog = read_file("prog0199.bin", &plen);

    mmis_chunk_t chunks[32];
    size_t n = mmis_split_chunks(prog, plen, chunks, 32);
    const uint8_t *tst = NULL;
    size_t tst_len = 0;
    for (size_t i = 0; i < n; i++) {
        if (chunks[i].tag == 0x34) { tst = chunks[i].data; tst_len = chunks[i].len; }
    }

    /* --- patch_timeslot_table --- */
    uint8_t *buf1 = malloc(tst_len);
    memcpy(buf1, tst, tst_len);
    mmis_patch_timeslot_table(buf1, tst_len, true, 2);

    size_t ref1_len;
    uint8_t *ref1 = read_file("tst_patched_calibrate.bin", &ref1_len);
    printf("patch_timeslot_table match: %s\n",
           (ref1_len == tst_len && memcmp(buf1, ref1, tst_len) == 0) ? "YES" : "NO");
    printf("  C   0x70-0x80: ");
    for (int i = 0x70; i < 0x80; i++) printf("%02x", buf1[i]);
    printf("\n  ref 0x70-0x80: ");
    for (int i = 0x70; i < 0x80; i++) printf("%02x", ref1[i]);
    printf("\n");

    /* --- patch_timeslot_again --- */
    size_t flen;
    uint8_t *fake_factory = read_file("fake_factory.bin", &flen);
    uint8_t *buf2 = malloc(tst_len);
    memcpy(buf2, buf1, tst_len);
    bool ok = mmis_patch_timeslot_again(buf2, tst_len, fake_factory, flen, 0x38);
    printf("\npatch_timeslot_again returned: %s\n", ok ? "true" : "false");

    size_t ref2_len;
    uint8_t *ref2 = read_file("tst_patched_again.bin", &ref2_len);
    printf("patch_timeslot_again match: %s\n",
           (ref2_len == tst_len && memcmp(buf2, ref2, tst_len) == 0) ? "YES" : "NO");

    return 0;
}
