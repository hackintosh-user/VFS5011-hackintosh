/*
 * hack-touchid-matcher.c
 *
 * Standalone port of libfprint's img.c minutiae-extraction and
 * bozorth-matching glue (fpi_img_detect_minutiae, minutiae_to_xyt,
 * fpi_img_compare_print_data), adapted to operate on a plain
 * width*height 8-bit grayscale buffer instead of libfprint's fp_img
 * struct, with no GLib dependency. The underlying NBIS calls
 * (get_minutiae, bozorth_probe_init, bozorth_to_gallery) and their
 * exact parameters (DEFAULT_PPI=500, g_lfsparms_V2) are unchanged
 * from upstream, so behavior should match libfprint's own matcher.
 *
 * NBIS (mindtct, bozorth3) is U.S. government work and is in the
 * public domain (no copyright, no license restrictions).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fp_types.h"
#include "lfs.h"
#include "bozorth.h"

#define DEFAULT_PPI 500

/* ---- port of libfprint img.c's minutiae_to_xyt() ---- */
static void minutiae_to_xyt(MINUTIAE *minutiae, int bwidth, int bheight,
                             struct xyt_struct *xyt) {
    int i;
    struct minutiae_struct c[MAX_FILE_MINUTIAE];
    int nmin = minutiae->num < MAX_FILE_MINUTIAE ? minutiae->num : MAX_FILE_MINUTIAE;

    for (i = 0; i < nmin; i++) {
        MINUTIA *minutia = minutiae->list[i];
        lfs2nist_minutia_XYT(&c[i].col[0], &c[i].col[1], &c[i].col[2],
                              minutia, bwidth, bheight);
        c[i].col[3] = sround(minutia->reliability * 100.0);
        if (c[i].col[2] > 180) c[i].col[2] -= 360;
    }

    qsort((void *)&c, (size_t)nmin, sizeof(struct minutiae_struct), sort_x_y);

    for (i = 0; i < nmin; i++) {
        xyt->xcol[i] = c[i].col[0];
        xyt->ycol[i] = c[i].col[1];
        xyt->thetacol[i] = c[i].col[2];
    }
    xyt->nrows = nmin;
}

/* Extract minutiae from a raw 8-bit grayscale buffer and produce an
 * xyt_struct template, ready to save or match. Returns 0 on success. */
int vfs5011_extract_template(unsigned char *image_data, int width, int height,
                              struct xyt_struct *out_template) {
    MINUTIAE *minutiae = NULL;
    int *direction_map = NULL, *low_contrast_map = NULL, *low_flow_map = NULL;
    int *high_curve_map = NULL, *quality_map = NULL;
    int map_w, map_h;
    unsigned char *bdata = NULL;
    int bw, bh, bd;
    int r;

    g_lfsparms_V2.remove_perimeter_pts = FALSE;

    r = get_minutiae(&minutiae, &quality_map, &direction_map,
                      &low_contrast_map, &low_flow_map, &high_curve_map,
                      &map_w, &map_h, &bdata, &bw, &bh, &bd,
                      image_data, width, height, 8,
                      (double)DEFAULT_PPI / 25.4, &g_lfsparms_V2);

    if (r) {
        fprintf(stderr, "get_minutiae failed, code %d\n", r);
        return r;
    }

    fprintf(stderr, "Detected %d minutiae\n", minutiae->num);
    minutiae_to_xyt(minutiae, width, height, out_template);

    free(quality_map);
    free(direction_map);
    free(low_contrast_map);
    free(low_flow_map);
    free(high_curve_map);
    if (bdata) free(bdata);
    /* free minutiae list */
    for (int i = 0; i < minutiae->num; i++) {
        if (minutiae->list[i]->nbrs) free(minutiae->list[i]->nbrs);
        if (minutiae->list[i]->ridge_counts) free(minutiae->list[i]->ridge_counts);
        free(minutiae->list[i]);
    }
    free(minutiae->list);
    free(minutiae);

    return 0;
}

/* Score a freshly-captured template against an enrolled one.
 * Higher score = more confident match. libfprint's own drivers
 * generally use a threshold around 40-50 for verify (varies by
 * sensor); this needs empirical tuning on your own captures. */
int vfs5011_match_score(struct xyt_struct *probe, struct xyt_struct *enrolled) {
    int probe_len = bozorth_probe_init(probe);
    return bozorth_to_gallery(probe_len, probe, enrolled);
}

int vfs5011_save_template(const char *path, struct xyt_struct *tmpl) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t written = fwrite(tmpl, sizeof(struct xyt_struct), 1, f);
    fclose(f);
    return written == 1 ? 0 : -1;
}

int vfs5011_load_template(const char *path, struct xyt_struct *tmpl) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t read = fread(tmpl, sizeof(struct xyt_struct), 1, f);
    fclose(f);
    return read == 1 ? 0 : -1;
}

/* Multi-template format: a 4-byte little-endian-ish (native) int count,
 * followed by `count` back-to-back xyt_struct records. This lets
 * enrollment store several swipes of the same finger so verify can
 * take the best score across all of them instead of relying on one
 * frozen template being representative of every future swipe. */
int vfs5011_save_templates(const char *path, struct xyt_struct *tmpls, int count) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (fwrite(&count, sizeof(int), 1, f) != 1) { fclose(f); return -1; }
    size_t written = fwrite(tmpls, sizeof(struct xyt_struct), (size_t)count, f);
    fclose(f);
    return written == (size_t)count ? 0 : -1;
}

int vfs5011_load_templates(const char *path, struct xyt_struct *tmpls, int max_count, int *out_count) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    int count = 0;
    if (fread(&count, sizeof(int), 1, f) != 1 || count <= 0 || count > max_count) {
        fclose(f);
        return -1;
    }
    size_t read = fread(tmpls, sizeof(struct xyt_struct), (size_t)count, f);
    fclose(f);
    if (read != (size_t)count) return -1;
    *out_count = count;
    return 0;
}
