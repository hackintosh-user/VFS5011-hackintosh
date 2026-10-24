/*
 * fp_types.h
 *
 * NBIS's mindtct headers (nbis/include/lfs.h) declare:
 *     typedef struct fp_minutia MINUTIA;
 *     typedef struct fp_minutiae MINUTIAE;
 * ...but expect the actual struct bodies to be supplied by whoever
 * links against them. Inside the full libfprint project these bodies
 * live in fprint.h / fp_internal.h. Since we're using mindtct/bozorth3
 * standalone (no libfprint, no GLib), we supply the same struct bodies
 * here, copied field-for-field from libfprint's public fprint.h so the
 * NBIS code compiles and behaves identically.
 */

#ifndef __FP_TYPES_H
#define __FP_TYPES_H

/* mindtct's source normally relies on GLib's MAX/MIN macros (libfprint
 * links GLib). We deliberately avoid GLib here, so supply the standard
 * equivalents ourselves. */
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

struct fp_minutia {
    int x;
    int y;
    int ex;
    int ey;
    int direction;
    double reliability;
    int type;
    int appearing;
    int feature_id;
    int *nbrs;
    int *ridge_counts;
    int num_nbrs;
};

struct fp_minutiae {
    int alloc;
    int num;
    struct fp_minutia **list;
};

#endif
