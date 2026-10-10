/*
 * metallica_mis_enroll.c -- see metallica_mis_enroll.h.
 *
 * Python references are quoted next to each step. Where the C differs on
 * purpose it says so: every wait has a limit (upstream loops forever
 * until cancelled), and the loop gives up after a few failed swipes.
 */

#include "metallica_mis_enroll.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "metallica_mis_daemon.h"
#include "metallica_mis_db.h"
#include "metallica_mis_debug.h"
#include "metallica_mis_flash.h"
#include "metallica_mis_tls.h"
#include "metallica_type0199_tables.h"
#include "mmis_factory_bits.h"
#include "mmis_timeslot.h"

/* ---- limits ---- */
#define ENROLL_MAX_ATTEMPTS      30      /* total capture attempts before giving up */
#define ENROLL_MAX_BAD_IN_A_ROW  4       /* consecutive failed attempts before giving up */
#define WAIT_START_MS            10000   /* capture start interrupt */
#define WAIT_FINGER_MS           30000   /* user has this long to touch the sensor */
#define WAIT_DONE_MS             10000   /* capture-complete interrupt */
#define WAIT_UPDATE_MS           15000   /* interrupt after enrollment_update_start/_update */
#define TEMPLATE_MAX             65536
#define REPLY_MAX                (128 * 1024)

/* Subtype stored with the finger. 0x02 is the subtype the sensor reported for
 * the tester's right-index print that python-validity enrolled earlier. */
#define ENROLL_DEFAULT_SUBTYPE   0x02

/* python sensor.py glow_start_scan() / glow_end_scan(): LED patterns, sent
 * as app commands. Copied byte for byte from upstream. */
static const unsigned char GLOW_START_SCAN[125] = {
    0x39, 0x20, 0xbf, 0x02, 0x00, 0xff, 0xff, 0x00, 0x00, 0x01, 0x99, 0x00,
    0x20, 0x00, 0x00, 0x00, 0x00, 0x99, 0x99, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x00,
    0x00, 0x00, 0x99, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
};

static const unsigned char GLOW_END_SCAN[125] = {
    0x39, 0xf4, 0x01, 0x00, 0x00, 0xf4, 0x01, 0x00, 0x00, 0x01, 0xff, 0x00,
    0x20, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf4, 0x01, 0x00,
    0x00, 0x00, 0xff, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
};

/* ---- little helpers ---- */
static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr16(unsigned char *p, uint16_t v) { p[0] = (unsigned char)(v & 0xff); p[1] = (unsigned char)(v >> 8); }
static void wr32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v & 0xff); p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff); p[3] = (unsigned char)((v >> 24) & 0xff);
}

/* One app-layer command with logging. Returns the reply length or -1. */
static int app_cmd(metallica_mis_tls_t *tls, const char *what, const unsigned char *cmd, size_t cmd_len,
                   unsigned char *rsp, size_t rsp_cap) {
    mmis_dbg("enroll: %s -> cmd 0x%02x (%zu bytes)", what, cmd_len ? cmd[0] : 0, cmd_len);
    int n = metallica_mis_tls_cmd(tls, cmd, cmd_len, rsp, rsp_cap);
    if (n < 0) {
        fprintf(stderr, "metallica_mis_enroll: %s: transport/session failure\n", what);
        return -1;
    }
    mmis_dbg_status(what, rsp, (size_t)n);
    return n;
}

/* python assert_status(): 2-byte status word, 0 = ok. */
static int status_ok(const char *what, const unsigned char *rsp, int n) {
    if (n < 2) {
        fprintf(stderr, "metallica_mis_enroll: %s: reply too short (%d bytes)\n", what, n);
        return -1;
    }
    uint16_t st = rd16(rsp);
    if (st != 0) {
        fprintf(stderr, "metallica_mis_enroll: %s: command failed, status=0x%04x (%s)\n",
                what, st, mmis_status_name(st));
        return -1;
    }
    return 0;
}

static int simple_app(metallica_mis_tls_t *tls, const char *what, const unsigned char *cmd, size_t len) {
    static unsigned char rsp[512];
    int n = app_cmd(tls, what, cmd, len, rsp, sizeof(rsp));
    if (n < 0) return -1;
    return status_ok(what, rsp, n);
}

/* ---- capture context: what build_cmd_02(ENROLL) needs ---- */
typedef struct {
    uint8_t prog[METALLICA_TYPE0199_PROG_MAX_LEN];
    size_t prog_len;
    size_t lines_per_frame;
    uint8_t factory[4096];
    size_t factory_len;
    uint8_t calib[16384];
    size_t calib_len;
} enroll_ctx_t;

/* python Sensor.capture(mode), mode = ENROLL or IDENTIFY. Returns 0 ok, -2 if
 * the finger never came (timeout), -1 on any other failure. */
static int capture_mode(metallica_mis_tls_t *tls, enroll_ctx_t *ctx, mmis_capture_mode_t mode) {
    const char *mname = (mode == MMIS_CAPTURE_IDENTIFY) ? "IDENTIFY" : "ENROLL";
    char what[64];
    static uint8_t cmd_buf[4096];
    static uint8_t scratch[8192];
    static unsigned char rsp[REPLY_MAX];
    unsigned char intr[1024];
    int rc = -1;

    size_t cmd_len = mmis_build_cmd_02(
        mode, ctx->prog, ctx->prog_len,
        METALLICA_TYPE0199_BYTES_PER_LINE, METALLICA_TYPE0199_CALIBRATION_FRAMES, ctx->lines_per_frame,
        METALLICA_TYPE0199_REPEAT_MULTIPLIER, METALLICA_TYPE0199_KEY_CALIBRATION_LINE,
        ctx->factory, ctx->factory_len,
        ctx->calib, ctx->calib_len,
        METALLICA_TYPE0199_LINES_PER_CALIBRATION_DATA, METALLICA_TYPE0199_LINE_WIDTH,
        METALLICA_TYPE0199_CALIB_BLOB, sizeof(METALLICA_TYPE0199_CALIB_BLOB),
        cmd_buf, sizeof(cmd_buf), scratch, sizeof(scratch));
    if (cmd_len == 0) {
        fprintf(stderr, "metallica_mis_enroll: build_cmd_02(%s) failed\n", mname);
        return -1;
    }

    snprintf(what, sizeof(what), "capture: cmd_02 (%s)", mname);
    int n = app_cmd(tls, what, cmd_buf, cmd_len, rsp, sizeof(rsp));
    if (n < 0 || status_ok(what, rsp, n) != 0) goto done;

    /* start: first interrupt must be type 0 */
    int w = metallica_mis_wait_interrupt(intr, sizeof(intr), WAIT_START_MS);
    if (w <= 0) {
        fprintf(stderr, "metallica_mis_enroll: capture: no start interrupt from the sensor (%s)\n",
                w == 0 ? "timed out" : "USB error");
        goto done;
    }
    if (intr[0] != 0) {
        fprintf(stderr, "metallica_mis_enroll: capture: unexpected start interrupt type 0x%02x\n", intr[0]);
        goto done;
    }

    printf("  Touch the sensor with the finger now...\n");
    fflush(stdout);

    /* wait for the finger: interrupt type 2 (anything else is skipped, as upstream) */
    for (;;) {
        w = metallica_mis_wait_interrupt(intr, sizeof(intr), WAIT_FINGER_MS);
        if (w == 0) { rc = -2; goto done; }
        if (w < 0) goto done;
        if (intr[0] == 2) break;
    }

    /* wait for capture complete: type 3 with bit 2 set in byte 2 */
    for (;;) {
        w = metallica_mis_wait_interrupt(intr, sizeof(intr), WAIT_DONE_MS);
        if (w <= 0) {
            fprintf(stderr, "metallica_mis_enroll: capture: finger seen but the capture never completed (%s)\n",
                    w == 0 ? "timed out" : "USB error");
            goto done;
        }
        if (intr[0] != 3 || w < 3) {
            fprintf(stderr, "metallica_mis_enroll: capture: unexpected interrupt type 0x%02x (%d bytes)\n",
                    intr[0], w);
            goto done;
        }
        if (intr[2] & 4) break;
    }

    /* get_prg_status2(): tls.app(5100200000) */
    {
        static const unsigned char get_status2[5] = { 0x51, 0x00, 0x20, 0x00, 0x00 };
        n = app_cmd(tls, "capture: get_prg_status2", get_status2, sizeof(get_status2), rsp, sizeof(rsp));
        if (n < 0 || status_ok("capture: get_prg_status2", rsp, n) != 0) goto done;
        /* res = rsp[2:], then u32 length, then <HHHHL x,y,w1,w2,error */
        if (n < 2 + 4) { fprintf(stderr, "metallica_mis_enroll: capture: status reply too short\n"); goto done; }
        uint32_t l = rd32(rsp + 2);
        if ((size_t)l != (size_t)n - 6 || l < 12) {
            fprintf(stderr, "metallica_mis_enroll: capture: response size does not match (%u vs %d)\n",
                    l, n - 6);
            goto done;
        }
        const unsigned char *p = rsp + 6;
        uint16_t x = rd16(p), y = rd16(p + 2), w1 = rd16(p + 4), w2 = rd16(p + 6);
        uint32_t err = rd32(p + 8);
        mmis_dbg("enroll: capture result x=%u y=%u w1=%u w2=%u error=0x%08x", x, y, w1, w2, err);
        if (err != 0) {
            fprintf(stderr, "metallica_mis_enroll: capture: scanning problem 0x%04x\n", err);
            goto done;
        }
    }
    rc = 0;

done:
    {
        /* python: finally: tls.app(unhexlify('04'))  # capture stop, cleanup */
        static const unsigned char stop[1] = { 0x04 };
        static unsigned char stop_rsp[64];
        metallica_mis_tls_cmd(tls, stop, sizeof(stop), stop_rsp, sizeof(stop_rsp));
    }
    return rc;
}

static int capture_enroll(metallica_mis_tls_t *tls, enroll_ctx_t *ctx) {
    return capture_mode(tls, ctx, MMIS_CAPTURE_ENROLL);
}

/* python enrollment_update_start(key): 0x68, u32 key, u32 0 -> new key, then wait_int() */
static int enrollment_update_start(metallica_mis_tls_t *tls, uint32_t key, uint32_t *new_key) {
    unsigned char cmd[9] = { 0x68 };
    unsigned char rsp[64];
    unsigned char intr[1024];
    wr32(cmd + 1, key); wr32(cmd + 5, 0);
    int n = app_cmd(tls, "enrollment_update_start", cmd, sizeof(cmd), rsp, sizeof(rsp));
    if (n < 0 || status_ok("enrollment_update_start", rsp, n) != 0) return -1;
    if (n != 2 + 4) {
        fprintf(stderr, "metallica_mis_enroll: enrollment_update_start: unexpected reply length %d\n", n);
        return -1;
    }
    *new_key = rd32(rsp + 2);
    if (metallica_mis_wait_interrupt(intr, sizeof(intr), WAIT_UPDATE_MS) <= 0) {
        fprintf(stderr, "metallica_mis_enroll: enrollment_update_start: no interrupt afterwards\n");
        return -1;
    }
    return 0;
}

/* python create_enrollment() / enrollment_update_end(): 0x69, u32 1 / 0 */
static int enrollment_flag(metallica_mis_tls_t *tls, const char *what, uint32_t v) {
    unsigned char cmd[5] = { 0x69 };
    wr32(cmd + 1, v);
    return simple_app(tls, what, cmd, sizeof(cmd));
}

/* python enrollment_update(prev): write_enable(); 0x6b + prev; call_cleanups().
 * The reply body (without the status word) goes to out/out_len. */
static int enrollment_update(metallica_mis_tls_t *tls, const unsigned char *prev, size_t prev_len,
                             unsigned char *out, size_t out_cap, size_t *out_len) {
    static unsigned char rsp[REPLY_MAX];
    unsigned char *cmd = (unsigned char *)malloc(1 + prev_len);
    if (!cmd) return -1;
    cmd[0] = 0x6b;
    if (prev_len) memcpy(cmd + 1, prev, prev_len);

    int rc = -1;
    if (metallica_mis_write_enable(tls) != 0) {
        fprintf(stderr, "metallica_mis_enroll: enrollment_update: write_enable failed\n");
        free(cmd);
        return -1;
    }
    int n = app_cmd(tls, "enrollment_update", cmd, 1 + prev_len, rsp, sizeof(rsp));
    free(cmd);
    if (n >= 0 && status_ok("enrollment_update", rsp, n) == 0) {
        size_t body = (size_t)n - 2;
        if (body <= out_cap) {
            memcpy(out, rsp + 2, body);
            *out_len = body;
            rc = 0;
        } else {
            fprintf(stderr, "metallica_mis_enroll: enrollment_update: reply (%zu bytes) too large\n", body);
        }
    }
    if (metallica_mis_flash_call_cleanups(tls) != 0) {
        fprintf(stderr, "metallica_mis_enroll: enrollment_update: call_cleanups failed\n");
        rc = -1;
    }
    return rc;
}

typedef struct {
    unsigned char template_[TEMPLATE_MAX];
    size_t template_len;
    unsigned char tid[1024];
    size_t tid_len;
    size_t header_len;
} enroll_result_t;

/* python append_new_image(prev): update, wait_int, update again, then walk the
 * tagged reply: tag 0 = template, 1 = header, 3 = template id. The 0x38 is
 * "hardcoded in the DLL" upstream. */
static int append_new_image(metallica_mis_tls_t *tls, enroll_result_t *r) {
    static unsigned char first[REPLY_MAX];
    static unsigned char res[REPLY_MAX];
    unsigned char intr[1024];
    size_t first_len = 0, res_len = 0;
    const size_t magic_len = 0x38;

    /* the template we send is the previous one; keep a copy since r is rewritten */
    static unsigned char prev[TEMPLATE_MAX];
    size_t prev_len = r->template_len;
    memcpy(prev, r->template_, prev_len);

    if (enrollment_update(tls, prev, prev_len, first, sizeof(first), &first_len) != 0) return -1;
    if (metallica_mis_wait_interrupt(intr, sizeof(intr), WAIT_UPDATE_MS) <= 0) {
        fprintf(stderr, "metallica_mis_enroll: append_new_image: no interrupt between the two updates\n");
        return -1;
    }
    if (enrollment_update(tls, prev, prev_len, res, sizeof(res), &res_len) != 0) return -1;

    if (res_len < 2) { fprintf(stderr, "metallica_mis_enroll: append_new_image: reply too short\n"); return -1; }
    uint16_t l = rd16(res);
    if ((size_t)l != res_len - 2) {
        fprintf(stderr, "metallica_mis_enroll: append_new_image: response size does not match (%u vs %zu)\n",
                l, res_len - 2);
        return -1;
    }

    const unsigned char *p = res + 2;
    size_t left = res_len - 2;
    bool have_template = false;
    size_t new_template_len = 0, new_tid_len = 0, new_header_len = 0;
    static unsigned char new_template[TEMPLATE_MAX];
    unsigned char new_tid[1024];

    while (left > 0) {
        if (left < 4) { fprintf(stderr, "metallica_mis_enroll: append_new_image: truncated tag header\n"); return -1; }
        uint16_t tag = rd16(p), tl = rd16(p + 2);
        if (tag == 0) {
            size_t want = magic_len + tl;
            if (want > left || want > sizeof(new_template)) {
                fprintf(stderr, "metallica_mis_enroll: append_new_image: template tag overruns the reply\n");
                return -1;
            }
            memcpy(new_template, p, want);
            new_template_len = want;
            have_template = true;
        } else if (tag == 1) {
            if (magic_len + tl > left) { fprintf(stderr, "metallica_mis_enroll: append_new_image: header tag overruns\n"); return -1; }
            new_header_len = tl;
        } else if (tag == 3) {
            if (magic_len + tl > left || tl > sizeof(new_tid)) {
                fprintf(stderr, "metallica_mis_enroll: append_new_image: template id tag overruns\n");
                return -1;
            }
            memcpy(new_tid, p + magic_len, tl);
            new_tid_len = tl;
        } else {
            mmis_dbg("enroll: ignoring unknown tag 0x%x", tag);
        }
        size_t step = magic_len + tl;
        if (step > left) { fprintf(stderr, "metallica_mis_enroll: append_new_image: tag overruns the reply\n"); return -1; }
        p += step; left -= step;
    }

    if (!have_template) {
        fprintf(stderr, "metallica_mis_enroll: append_new_image: reply carried no template\n");
        return -1;
    }
    memcpy(r->template_, new_template, new_template_len);
    r->template_len = new_template_len;
    memcpy(r->tid, new_tid, new_tid_len);
    r->tid_len = new_tid_len;
    r->header_len = new_header_len;
    return 0;
}

/* python make_finger_data(subtype, template, tid) */
static size_t make_finger_data(uint16_t subtype, const unsigned char *tmpl, size_t tmpl_len,
                               const unsigned char *tid, size_t tid_len,
                               unsigned char *out, size_t out_cap) {
    size_t tinfo_len = (4 + tmpl_len) + (4 + tid_len);
    size_t total = 8 + tinfo_len + 0x20;
    if (total > out_cap) return 0;
    unsigned char *p = out;
    wr16(p, subtype); wr16(p + 2, 3); wr16(p + 4, (uint16_t)tinfo_len); wr16(p + 6, 0x20);
    p += 8;
    wr16(p, 1); wr16(p + 2, (uint16_t)tmpl_len); memcpy(p + 4, tmpl, tmpl_len);
    p += 4 + tmpl_len;
    wr16(p, 2); wr16(p + 2, (uint16_t)tid_len); memcpy(p + 4, tid, tid_len);
    p += 4 + tid_len;
    memset(p, 0, 0x20);
    return total;
}

/* The identity HTID stores its user under: python's identity_to_bytes() of a
 * SID (revision 1, authority 5, sub-authorities 21, 0x48544944 "HTID", 0, 0,
 * 1000): u32 3, u32 len, the SID, zero padded to 0x4c. */
static size_t htid_identity(unsigned char *out, size_t cap) {
    static const uint32_t sub[5] = { 21, 0x48544944u, 0, 0, 1000 };
    unsigned char sid[8 + 5 * 4];
    sid[0] = 1; sid[1] = 5;
    sid[2] = 0; sid[3] = 0;                               /* auth >> 32, big endian u16 */
    sid[4] = 0; sid[5] = 0; sid[6] = 0; sid[7] = 5;       /* auth & 0xffffffff, big endian u32 */
    for (int i = 0; i < 5; i++) wr32(sid + 8 + i * 4, sub[i]);
    size_t total = 8 + sizeof(sid);
    if (total < 0x4c) total = 0x4c;
    if (total > cap) return 0;
    memset(out, 0, total);
    wr32(out, 3); wr32(out + 4, (uint32_t)sizeof(sid));
    memcpy(out + 8, sid, sizeof(sid));
    return total;
}

/* ---- the whole test ---- */
int metallica_mis_do_enroll_test(void) {
    static enroll_ctx_t ctx;
    static enroll_result_t res;
    static unsigned char tinfo[TEMPLATE_MAX + 2048];
    metallica_mis_tls_t tls;
    int rc = -1;
    bool device_open = false;

    mmis_dbg("enroll: begin");

    if (metallica_mis_open_device() != 0) {
        fprintf(stderr, "metallica_mis: enroll: could not open the sensor\n");
        return -1;
    }
    device_open = true;
    if (metallica_mis_send_init() != 0) {
        fprintf(stderr, "metallica_mis: enroll: plaintext bootstrap failed\n");
        goto done;
    }
    if (metallica_mis_open_calibration_session(&tls) != 0) {
        fprintf(stderr, "metallica_mis: enroll: could not establish a secure session "
                         "(sensor not paired/loaded yet? run Pair Sensor first)\n");
        goto done;
    }

    printf("Prints on the sensor now:\n");
    int users_before = 0;
    if (mmis_db_list(&tls, &users_before) != 0) {
        fprintf(stderr, "metallica_mis: enroll: could not read the sensor's records, stopping\n");
        goto done;
    }
    if (users_before > 0) {
        printf("\nNote: the sensor already holds %d user record(s). If saving the new print is rejected\n"
               "with status 0x04c3, run --wipe-records first and enroll again.\n", users_before);
    }

    /* capture setup, same as metallica_mis_do_calibrate() */
    metallica_type0199_build_prog(ctx.prog, &ctx.prog_len);
    if (!mmis_get_lines_per_frame(ctx.prog, ctx.prog_len, METALLICA_TYPE0199_REPEAT_MULTIPLIER,
                                  &ctx.lines_per_frame)) {
        fprintf(stderr, "metallica_mis: enroll: mmis_get_lines_per_frame() failed\n");
        goto done;
    }
    if (!metallica_mis_get_factory_calibration_values(&tls, ctx.factory, sizeof(ctx.factory), &ctx.factory_len)) {
        fprintf(stderr, "metallica_mis: enroll: get_factory_calibration_values() failed\n");
        goto done;
    }

    printf("\nStep 1 of 3: calibrating. Keep your finger OFF the sensor until this is done.\n");
    fflush(stdout);
    if (metallica_mis_do_calibrate_ex(&tls, ctx.calib, sizeof(ctx.calib), &ctx.calib_len) != 0) {
        fprintf(stderr, "metallica_mis: enroll: calibration failed, cannot enroll without it\n");
        goto done;
    }
    mmis_dbg("enroll: calibration data %zu bytes", ctx.calib_len);

    printf("\nStep 2 of 3: enrolling. The sensor will ask for several touches of the SAME finger.\n");
    fflush(stdout);

    uint32_t key = 0;
    int attempts = 0, bad_in_a_row = 0, good = 0;
    bool done_enrolling = false;
    res.template_len = 0;
    res.tid_len = 0;

    if (enrollment_flag(&tls, "create_enrollment", 1) != 0) goto done;
    simple_app(&tls, "glow_start_scan", GLOW_START_SCAN, sizeof(GLOW_START_SCAN));

    while (!done_enrolling && attempts < ENROLL_MAX_ATTEMPTS && bad_in_a_row < ENROLL_MAX_BAD_IN_A_ROW) {
        attempts++;
        printf("\nTouch %d:\n", good + 1);
        int cap = capture_enroll(&tls, &ctx);
        bool ok = false;
        if (cap == 0) {
            uint32_t new_key = 0;
            if (enrollment_update_start(&tls, key, &new_key) == 0) {
                key = new_key;
                if (append_new_image(&tls, &res) == 0) {
                    ok = true;
                    good++;
                    printf("  Accepted (header %zu bytes, template %zu bytes).\n", res.header_len, res.template_len);
                    if (res.tid_len > 0) done_enrolling = true;
                }
            }
        } else if (cap == -2) {
            printf("  No finger detected in time.\n");
        } else {
            printf("  That touch was not usable, try again.\n");
        }
        bad_in_a_row = ok ? 0 : bad_in_a_row + 1;
        /* python: finally: enrollment_update_end() runs after every attempt, including the last */
        enrollment_flag(&tls, "enrollment_update_end", 0);
    }

    if (!done_enrolling) {
        fprintf(stderr, "\nmetallica_mis: enroll: stopped after %d attempt(s), %d usable touch(es), without "
                         "the sensor completing the enrollment.\n", attempts, good);
        simple_app(&tls, "glow_end_scan", GLOW_END_SCAN, sizeof(GLOW_END_SCAN));
        goto done;
    }
    enrollment_flag(&tls, "enrollment_update_end (second, as upstream)", 0);

    printf("\nStep 3 of 3: saving the print on the sensor...\n");
    fflush(stdout);
    size_t tinfo_len = make_finger_data(ENROLL_DEFAULT_SUBTYPE, res.template_, res.template_len,
                                        res.tid, res.tid_len, tinfo, sizeof(tinfo));
    if (tinfo_len == 0) {
        fprintf(stderr, "metallica_mis: enroll: finger data does not fit its buffer\n");
        goto done;
    }

    unsigned char identity[0x4c + 16];
    size_t identity_len = htid_identity(identity, sizeof(identity));
    uint16_t user_dbid = 0, finger_dbid = 0;
    int lk = mmis_db_lookup_user(&tls, identity, identity_len, &user_dbid);
    if (lk < 0) goto done;
    if (lk == 1 && mmis_db_new_user(&tls, identity, identity_len, &user_dbid) != 0) {
        fprintf(stderr, "metallica_mis: enroll: could not create the user record\n");
        goto done;
    }
    if (mmis_db_new_finger(&tls, user_dbid, tinfo, tinfo_len, &finger_dbid) != 0) {
        fprintf(stderr, "metallica_mis: enroll: the sensor rejected the new print. Status 0x04c3 means an\n"
                         "older print is still stored: run --wipe-records, then --enroll-test again.\n");
        simple_app(&tls, "glow_end_scan", GLOW_END_SCAN, sizeof(GLOW_END_SCAN));
        goto done;
    }
    {
        /* python: usb.wait_int() after new_finger(), then glow_end_scan() */
        unsigned char intr[1024];
        metallica_mis_wait_interrupt(intr, sizeof(intr), 3000);
    }
    simple_app(&tls, "glow_end_scan", GLOW_END_SCAN, sizeof(GLOW_END_SCAN));

    printf("\nSaved: user record %u, finger record %u (subtype 0x%02x).\n\nPrints on the sensor now:\n",
           user_dbid, finger_dbid, ENROLL_DEFAULT_SUBTYPE);
    mmis_db_list(&tls, NULL);
    rc = 0;

done:
    if (device_open) metallica_mis_close_device();
    mmis_dbg("enroll: end, rc=%d", rc);
    return rc == 0 ? 0 : -1;
}

/* ---- verify (match in the sensor) ---- */

/* python Sensor.parse_dict(): repeated <HH tag, len> + data. Fills vals/lens
 * for tags 0..7 (pointers into buf). */
static int parse_dict(const unsigned char *buf, size_t len, const unsigned char **vals, size_t *lens, int ntags) {
    for (int i = 0; i < ntags; i++) { vals[i] = NULL; lens[i] = 0; }
    while (len > 0) {
        if (len < 4) return -1;
        uint16_t t = rd16(buf), l = rd16(buf + 2);
        buf += 4; len -= 4;
        if (l > len) return -1;
        if (t < ntags) { vals[t] = buf; lens[t] = l; }
        buf += l; len -= l;
    }
    return 0;
}

/* python Sensor.match_finger(): 0x5e match command, one interrupt (type 3 =
 * recognised), 0x60 to fetch the result, 0x62 cleanup. Returns 0 and fills
 * the outputs on a match, 1 if the sensor did not recognise the finger,
 * -1 on any failure. */
static int match_finger(metallica_mis_tls_t *tls, uint32_t *usr_id, uint16_t *subtype, size_t *hash_len) {
    static unsigned char rsp[REPLY_MAX];
    unsigned char intr[1024];
    unsigned char cmd[13];
    int rc = -1;

    /* pack('<BBBHHHHH', 0x5e, 2, 0xff, stg_id=0, usr_id=0, 1, 0, 0) */
    cmd[0] = 0x5e; cmd[1] = 2; cmd[2] = 0xff;
    wr16(cmd + 3, 0); wr16(cmd + 5, 0); wr16(cmd + 7, 1); wr16(cmd + 9, 0); wr16(cmd + 11, 0);

    int n = app_cmd(tls, "match: 0x5e", cmd, sizeof(cmd), rsp, sizeof(rsp));
    if (n < 0 || status_ok("match: 0x5e", rsp, n) != 0) goto done;

    int w = metallica_mis_wait_interrupt(intr, sizeof(intr), WAIT_UPDATE_MS);
    if (w <= 0) {
        fprintf(stderr, "metallica_mis_enroll: match: no interrupt after the match command (%s)\n",
                w == 0 ? "timed out" : "USB error");
        goto done;
    }
    if (intr[0] != 3) {
        mmis_dbg("match: interrupt type 0x%02x, finger not recognised", intr[0]);
        rc = 1;
        goto done;
    }

    {
        static const unsigned char get_result[5] = { 0x60, 0x00, 0x00, 0x00, 0x00 };
        n = app_cmd(tls, "match: get result (0x60)", get_result, sizeof(get_result), rsp, sizeof(rsp));
        if (n < 0 || status_ok("match: get result (0x60)", rsp, n) != 0) goto done;
        if (n < 4) { fprintf(stderr, "metallica_mis_enroll: match: result reply too short\n"); goto done; }
        uint16_t l = rd16(rsp + 2);
        if ((size_t)l != (size_t)n - 4) {
            fprintf(stderr, "metallica_mis_enroll: match: response size does not match (%u vs %d)\n", l, n - 4);
            goto done;
        }
        const unsigned char *v[8]; size_t vl[8];
        if (parse_dict(rsp + 4, (size_t)n - 4, v, vl, 8) != 0) {
            fprintf(stderr, "metallica_mis_enroll: match: result dictionary is malformed\n");
            goto done;
        }
        if (!v[1] || vl[1] < 4 || !v[3] || vl[3] < 2) {
            fprintf(stderr, "metallica_mis_enroll: match: result is missing the user id or subtype\n");
            goto done;
        }
        *usr_id = rd32(v[1]);
        *subtype = rd16(v[3]);
        *hash_len = vl[4];
    }
    rc = 0;

done:
    {
        /* python: finally: tls.app(unhexlify('6200000000')), errors ignored */
        static const unsigned char cleanup[5] = { 0x62, 0x00, 0x00, 0x00, 0x00 };
        static unsigned char c_rsp[64];
        metallica_mis_tls_cmd(tls, cleanup, sizeof(cleanup), c_rsp, sizeof(c_rsp));
    }
    return rc;
}

int metallica_mis_do_verify_test(void) {
    static enroll_ctx_t ctx;
    metallica_mis_tls_t tls;
    int rc = -1;
    bool device_open = false;

    mmis_dbg("verify: begin");

    if (metallica_mis_open_device() != 0) {
        fprintf(stderr, "metallica_mis: verify: could not open the sensor\n");
        return -1;
    }
    device_open = true;
    if (metallica_mis_send_init() != 0) {
        fprintf(stderr, "metallica_mis: verify: plaintext bootstrap failed\n");
        goto done;
    }
    if (metallica_mis_open_calibration_session(&tls) != 0) {
        fprintf(stderr, "metallica_mis: verify: could not establish a secure session "
                         "(sensor not paired/loaded yet? run Pair Sensor first)\n");
        goto done;
    }

    printf("Prints on the sensor now:\n");
    int users = 0;
    if (mmis_db_list(&tls, &users) != 0) {
        fprintf(stderr, "metallica_mis: verify: could not read the sensor's records, stopping\n");
        goto done;
    }
    if (users == 0) {
        fprintf(stderr, "\nmetallica_mis: verify: the sensor holds no user records, nothing to match against.\n"
                         "Run --enroll-test first.\n");
        goto done;
    }

    metallica_type0199_build_prog(ctx.prog, &ctx.prog_len);
    if (!mmis_get_lines_per_frame(ctx.prog, ctx.prog_len, METALLICA_TYPE0199_REPEAT_MULTIPLIER,
                                  &ctx.lines_per_frame)) {
        fprintf(stderr, "metallica_mis: verify: mmis_get_lines_per_frame() failed\n");
        goto done;
    }
    if (!metallica_mis_get_factory_calibration_values(&tls, ctx.factory, sizeof(ctx.factory), &ctx.factory_len)) {
        fprintf(stderr, "metallica_mis: verify: get_factory_calibration_values() failed\n");
        goto done;
    }

    printf("\nStep 1 of 2: calibrating. Keep your finger OFF the sensor until this is done.\n");
    fflush(stdout);
    if (metallica_mis_do_calibrate_ex(&tls, ctx.calib, sizeof(ctx.calib), &ctx.calib_len) != 0) {
        fprintf(stderr, "metallica_mis: verify: calibration failed, cannot verify without it\n");
        goto done;
    }
    mmis_dbg("verify: calibration data %zu bytes", ctx.calib_len);

    printf("\nStep 2 of 2: verifying. Touch the sensor with the enrolled finger.\n");
    fflush(stdout);

    int attempts = 0, bad_in_a_row = 0;
    bool matched = false, not_recognised = false;
    uint32_t usr_id = 0; uint16_t subtype = 0; size_t hash_len = 0;

    simple_app(&tls, "glow_start_scan", GLOW_START_SCAN, sizeof(GLOW_START_SCAN));
    while (attempts < ENROLL_MAX_ATTEMPTS && bad_in_a_row < ENROLL_MAX_BAD_IN_A_ROW) {
        attempts++;
        int cap = capture_mode(&tls, &ctx, MMIS_CAPTURE_IDENTIFY);
        if (cap == 0) {
            int m = match_finger(&tls, &usr_id, &subtype, &hash_len);
            if (m == 0) { matched = true; break; }
            if (m == 1) { not_recognised = true; break; }
            printf("  The match step failed, try again.\n");
        } else if (cap == -2) {
            printf("  No finger detected in time.\n");
        } else {
            printf("  That touch was not usable, try again.\n");
        }
        bad_in_a_row++;
    }
    simple_app(&tls, "glow_end_scan", GLOW_END_SCAN, sizeof(GLOW_END_SCAN));

    if (matched) {
        printf("\nMATCH: user record %u, finger subtype 0x%02x (hash %zu bytes).\n", usr_id, subtype, hash_len);
        rc = 0;
    } else if (not_recognised) {
        printf("\nNO MATCH: the sensor captured the touch but did not recognise the finger.\n");
        rc = 1;
    } else {
        fprintf(stderr, "\nmetallica_mis: verify: stopped after %d attempt(s) without a usable result.\n", attempts);
    }

done:
    if (device_open) metallica_mis_close_device();
    mmis_dbg("verify: end, rc=%d", rc);
    return rc == 0 ? 0 : -1;
}
