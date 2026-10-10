#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "timeslot.h"

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror("fopen"); exit(1); }
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
    size_t len;
    uint8_t *prog = read_file("prog0199.bin", &len);
    printf("prog len=%zu\n", len);

    mmis_chunk_t chunks[32];
    size_t n = mmis_split_chunks(prog, len, chunks, 32);
    printf("n_chunks=%zu\n", n);
    const uint8_t *tst = NULL;
    size_t tst_len = 0;
    for (size_t i = 0; i < n; i++) {
        printf("tag=0x%02x len=%u\n", chunks[i].tag, chunks[i].len);
        if (chunks[i].tag == 0x34) { tst = chunks[i].data; tst_len = chunks[i].len; }
    }

    if (!tst) { printf("no 0x34 chunk found!\n"); return 1; }
    printf("\ntst len=%zu\n", tst_len);

    size_t pc, ilen;
    if (mmis_find_nth_insn(tst, tst_len, MMIS_OP_ENABLE_RX, 2, &pc, &ilen)) {
        printf("2nd Enable Rx (op=6) at pc=0x%zx, len=%zu\n", pc, ilen);
    } else {
        printf("2nd Enable Rx NOT FOUND\n");
    }

    if (mmis_find_nth_regwrite(tst, tst_len, 0x8000203C, 1, &pc, &ilen)) {
        printf("1st RegWrite 0x8000203C at pc=0x%zx, len=%zu\n", pc, ilen);
    } else {
        printf("1st RegWrite NOT FOUND\n");
    }

    /* round-trip merge_chunks and compare byte-for-byte against original */
    uint8_t merged[1024];
    size_t merged_len = mmis_merge_chunks(chunks, n, merged, sizeof(merged));
    printf("\nmerged_len=%zu original_len=%zu match=%s\n",
           merged_len, len, (merged_len == len && memcmp(merged, prog, len) == 0) ? "YES" : "NO");

    return 0;
}
