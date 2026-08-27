/*
 * test_matcher.c
 *
 * Standalone unit tests for the parts of hack-touchid-matcher.c that don't
 * touch the sensor: template serialization (vfs5011_save_templates /
 * vfs5011_load_templates) and the fingers/ directory scan pattern used
 * by both vfs_client.c (list_enrolled_fingers) and vfs5011_daemon.c
 * (load_templates_for_episode).
 *
 * These two functions were exactly what broke in the v1.0.1 regression
 * (daemon looking for a flat template.dat instead of scanning
 * fingers/*.dat) -- this file exists so that class of bug fails CI
 * instead of getting found on real hardware.
 *
 * No USB, no CoreFoundation, no libfprint -- just NBIS + matcher.c, so
 * this builds and runs on a plain GitHub Actions macos-latest runner.
 *
 * Build: see tests/run_tests.sh
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

#include "fp_types.h"
#include "lfs.h"
#include "bozorth.h"
#include "hack-touchid-matcher.h"

#define MAX_STORED_TEMPLATES 8 /* mirrors vfs5011_daemon.c */

static int g_failures = 0;

#define CHECK(cond, msg) do { \
    if (cond) { \
        printf("  [PASS] %s\n", msg); \
    } else { \
        printf("  [FAIL] %s (line %d)\n", msg, __LINE__); \
        g_failures++; \
    } \
} while (0)

/* Fills a template with deterministic, distinguishable values so
 * round-trip tests can tell templates apart and detect corruption. */
static void make_fake_template(struct xyt_struct *t, int seed) {
    memset(t, 0, sizeof(*t));
    t->nrows = 3 + (seed % 5);
    for (int i = 0; i < t->nrows; i++) {
        t->xcol[i] = seed * 10 + i;
        t->ycol[i] = seed * 20 + i;
        t->thetacol[i] = (seed * 7 + i) % 360;
    }
}

static int templates_equal(const struct xyt_struct *a, const struct xyt_struct *b) {
    if (a->nrows != b->nrows) return 0;
    for (int i = 0; i < a->nrows; i++) {
        if (a->xcol[i] != b->xcol[i]) return 0;
        if (a->ycol[i] != b->ycol[i]) return 0;
        if (a->thetacol[i] != b->thetacol[i]) return 0;
    }
    return 1;
}

/* ---- Test 1: single-template round trip ---- */
static void test_single_roundtrip(const char *tmp_dir) {
    printf("test_single_roundtrip:\n");
    char path[1024];
    snprintf(path, sizeof(path), "%s/single.dat", tmp_dir);

    struct xyt_struct original;
    make_fake_template(&original, 1);

    CHECK(vfs5011_save_templates(path, &original, 1) == 0, "save single template succeeds");

    struct xyt_struct loaded[MAX_STORED_TEMPLATES];
    int count = 0;
    CHECK(vfs5011_load_templates(path, loaded, MAX_STORED_TEMPLATES, &count) == 0,
          "load single template succeeds");
    CHECK(count == 1, "loaded count is 1");
    CHECK(templates_equal(&original, &loaded[0]), "loaded template matches original byte-for-byte");
}

/* ---- Test 2: multi-swipe round trip (enrollment stores several swipes) ---- */
static void test_multi_swipe_roundtrip(const char *tmp_dir) {
    printf("test_multi_swipe_roundtrip:\n");
    char path[1024];
    snprintf(path, sizeof(path), "%s/multi.dat", tmp_dir);

    struct xyt_struct originals[5];
    for (int i = 0; i < 5; i++) make_fake_template(&originals[i], 100 + i);

    CHECK(vfs5011_save_templates(path, originals, 5) == 0, "save 5 templates succeeds");

    struct xyt_struct loaded[MAX_STORED_TEMPLATES];
    int count = 0;
    CHECK(vfs5011_load_templates(path, loaded, MAX_STORED_TEMPLATES, &count) == 0,
          "load 5 templates succeeds");
    CHECK(count == 5, "loaded count is 5");

    int all_match = 1;
    for (int i = 0; i < 5; i++) {
        if (!templates_equal(&originals[i], &loaded[i])) all_match = 0;
    }
    CHECK(all_match, "all 5 loaded templates match originals in order");
}

/* ---- Test 3: missing file is a clean failure, not a crash ---- */
static void test_missing_file(const char *tmp_dir) {
    printf("test_missing_file:\n");
    char path[1024];
    snprintf(path, sizeof(path), "%s/does_not_exist.dat", tmp_dir);

    struct xyt_struct loaded[MAX_STORED_TEMPLATES];
    int count = 0;
    CHECK(vfs5011_load_templates(path, loaded, MAX_STORED_TEMPLATES, &count) == -1,
          "loading a nonexistent file returns -1");
}

/* ---- Test 4: truncated/corrupt file is rejected, not partially trusted ---- */
static void test_corrupt_file(const char *tmp_dir) {
    printf("test_corrupt_file:\n");
    char path[1024];
    snprintf(path, sizeof(path), "%s/corrupt.dat", tmp_dir);

    /* Write a count header claiming 3 templates but only supply 1's
     * worth of bytes -- the fread() of the body should come up short
     * and vfs5011_load_templates must report failure. */
    FILE *f = fopen(path, "wb");
    int claimed_count = 3;
    fwrite(&claimed_count, sizeof(int), 1, f);
    struct xyt_struct partial;
    make_fake_template(&partial, 9);
    fwrite(&partial, sizeof(struct xyt_struct), 1, f);
    fclose(f);

    struct xyt_struct loaded[MAX_STORED_TEMPLATES];
    int count = 0;
    CHECK(vfs5011_load_templates(path, loaded, MAX_STORED_TEMPLATES, &count) == -1,
          "truncated file (short read) is rejected");

    /* A count header bigger than the caller's buffer must also be
     * rejected rather than overflowing the caller's array. */
    snprintf(path, sizeof(path), "%s/oversized.dat", tmp_dir);
    f = fopen(path, "wb");
    int huge_count = MAX_STORED_TEMPLATES + 1;
    fwrite(&huge_count, sizeof(int), 1, f);
    fclose(f);
    CHECK(vfs5011_load_templates(path, loaded, MAX_STORED_TEMPLATES, &count) == -1,
          "count exceeding max_count is rejected, not truncated/overflowed");
}

/* ---- Test 5: fingers/ directory multi-finger scan pattern ----
 * Mirrors load_templates_for_episode()'s loop in vfs5011_daemon.c:
 * open fingers_dir, skip anything not ending in ".dat", load each
 * file's templates, and pool them up to MAX_STORED_TEMPLATES total.
 * This is the exact shape of the v1.0.1 regression (daemon wasn't
 * looking in fingers/ at all), so this test recreates that directory
 * layout and checks the pooling logic that scans it. */
static void test_fingers_directory_scan(const char *tmp_dir) {
    printf("test_fingers_directory_scan:\n");
    char fingers_dir[1024];
    snprintf(fingers_dir, sizeof(fingers_dir), "%s/fingers", tmp_dir);
    mkdir(fingers_dir, 0700);

    char alice_path[1024], bob_path[1024], decoy_path[1024];
    snprintf(alice_path, sizeof(alice_path), "%s/alice.dat", fingers_dir);
    snprintf(bob_path, sizeof(bob_path), "%s/bob.dat", fingers_dir);
    snprintf(decoy_path, sizeof(decoy_path), "%s/readme.txt", fingers_dir);

    struct xyt_struct alice_tmpls[3], bob_tmpls[2];
    for (int i = 0; i < 3; i++) make_fake_template(&alice_tmpls[i], 200 + i);
    for (int i = 0; i < 2; i++) make_fake_template(&bob_tmpls[i], 300 + i);

    vfs5011_save_templates(alice_path, alice_tmpls, 3);
    vfs5011_save_templates(bob_path, bob_tmpls, 2);

    /* Decoy non-.dat file in the same directory -- must be skipped,
     * not fed into vfs5011_load_templates as if it were a template. */
    FILE *decoy = fopen(decoy_path, "w");
    fprintf(decoy, "not a template file\n");
    fclose(decoy);

    /* Replicates load_templates_for_episode()'s scan-and-pool loop. */
    struct xyt_struct pooled[MAX_STORED_TEMPLATES];
    int pooled_count = 0;
    int files_loaded = 0;

    DIR *d = opendir(fingers_dir);
    CHECK(d != NULL, "fingers/ directory opens");
    if (d) {
        struct dirent *entry;
        while ((entry = readdir(d)) != NULL && pooled_count < MAX_STORED_TEMPLATES) {
            size_t len = strlen(entry->d_name);
            if (len <= 4 || strcmp(entry->d_name + len - 4, ".dat") != 0) continue;

            char finger_path[1024];
            snprintf(finger_path, sizeof(finger_path), "%s/%s", fingers_dir, entry->d_name);

            struct xyt_struct finger_templates[MAX_STORED_TEMPLATES];
            int finger_count = 0;
            if (vfs5011_load_templates(finger_path, finger_templates, MAX_STORED_TEMPLATES, &finger_count) != 0) {
                continue;
            }
            files_loaded++;
            for (int i = 0; i < finger_count && pooled_count < MAX_STORED_TEMPLATES; i++) {
                pooled[pooled_count++] = finger_templates[i];
            }
        }
        closedir(d);
    }

    CHECK(files_loaded == 2, "exactly 2 .dat files were loaded (decoy .txt skipped)");
    CHECK(pooled_count == 5, "alice's 3 + bob's 2 templates pooled into 5 total");
}

int main(void) {
    char tmp_template[] = "/tmp/vfs5011_test_XXXXXX";
    char *tmp_dir = mkdtemp(tmp_template);
    if (!tmp_dir) {
        fprintf(stderr, "Could not create temp dir for tests\n");
        return 1;
    }

    test_single_roundtrip(tmp_dir);
    test_multi_swipe_roundtrip(tmp_dir);
    test_missing_file(tmp_dir);
    test_corrupt_file(tmp_dir);
    test_fingers_directory_scan(tmp_dir);

    printf("\n");
    if (g_failures == 0) {
        printf("All tests passed.\n");
        return 0;
    } else {
        printf("%d check(s) failed.\n", g_failures);
        return 1;
    }
}
