/*
 * metallica_mis_debug.c -- see metallica_mis_debug.h.
 */

#include "metallica_mis_debug.h"

#include <stdarg.h>
#include <stdio.h>
#include <sys/time.h>

int g_metallica_mis_debug = 0;

static double now_seconds(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

static double elapsed(void) {
    static double t0 = 0.0;
    double t = now_seconds();
    if (t0 == 0.0) t0 = t;
    return t - t0;
}

void mmis_dbg(const char *fmt, ...) {
    if (!g_metallica_mis_debug) return;
    fprintf(stderr, "[dbg +%9.3f] ", elapsed());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

static uint32_t fnv1a(const unsigned char *p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static void dump_range(const unsigned char *buf, size_t start, size_t end) {
    for (size_t i = start; i < end; i += 16) {
        fprintf(stderr, "[dbg          ]   %06zx: ", i);
        for (size_t j = 0; j < 16; j++) {
            if (i + j < end) fprintf(stderr, "%02x ", buf[i + j]);
            else fprintf(stderr, "   ");
        }
        fputc(' ', stderr);
        for (size_t j = 0; j < 16 && i + j < end; j++) {
            unsigned char c = buf[i + j];
            fputc((c >= 0x20 && c < 0x7f) ? c : '.', stderr);
        }
        fputc('\n', stderr);
    }
}

void mmis_dbg_hex(const char *label, const unsigned char *buf, size_t len) {
    if (!g_metallica_mis_debug) return;
    fprintf(stderr, "[dbg +%9.3f] %s (%zu bytes, fnv1a=%08x)\n",
            elapsed(), label, len, (!buf) ? 0u : fnv1a(buf, len));
    if (!buf || len == 0) { fflush(stderr); return; }

    if (g_metallica_mis_debug >= 2 || len <= MMIS_DBG_HEX_CAP) {
        dump_range(buf, 0, len);
    } else {
        size_t head = 256, tail = 64;
        dump_range(buf, 0, head);
        fprintf(stderr, "[dbg          ]   ... %zu bytes omitted (use --debug-full for everything) ...\n",
                len - head - tail);
        /* tail starts on a 16-byte boundary so the offsets stay aligned */
        size_t tail_start = (len - tail) & ~(size_t)15;
        dump_range(buf, tail_start, len);
    }
    fflush(stderr);
}

const char *mmis_status_name(uint16_t status) {
    switch (status) {
        case 0x0000: return "OK";
        case 0x0404: return "rejected (seen when a command is sent in plaintext instead of through the TLS session)";
        case 0x0491: return "nothing to commit (treated as success by flash cleanup)";
        case 0x04b3: return "not found (storage/user does not exist)";
        case 0x04c3: return "record save rejected (seen on enroll when a print for this user already exists on the sensor)";
        default:     return "unknown";
    }
}

void mmis_dbg_status(const char *label, const unsigned char *reply, size_t reply_len) {
    if (!g_metallica_mis_debug) return;
    if (!reply || reply_len < 2) {
        mmis_dbg("%s: reply too short for a status word (%zu bytes)", label, reply_len);
        return;
    }
    uint16_t st = (uint16_t)(reply[0] | (reply[1] << 8));
    mmis_dbg("%s: status=0x%04x (%s)", label, st, mmis_status_name(st));
}

void mmis_key_fp(const unsigned char *key, size_t len, char out[MMIS_KEY_FP_LEN]) {
    snprintf(out, MMIS_KEY_FP_LEN, "<redacted, fp=%08x>", key ? fnv1a(key, len) : 0u);
}

const char *mmis_tls_alert_name(unsigned char d) {
    switch (d) {
    case 0:   return "close_notify";
    case 10:  return "unexpected_message";
    case 20:  return "bad_record_mac";
    case 21:  return "decryption_failed";
    case 22:  return "record_overflow";
    case 30:  return "decompression_failure";
    case 40:  return "handshake_failure";
    case 41:  return "no_certificate";
    case 42:  return "bad_certificate";
    case 43:  return "unsupported_certificate";
    case 44:  return "certificate_revoked";
    case 45:  return "certificate_expired";
    case 46:  return "certificate_unknown";
    case 47:  return "illegal_parameter";
    case 48:  return "unknown_ca";
    case 49:  return "access_denied";
    case 50:  return "decode_error";
    case 51:  return "decrypt_error";
    case 70:  return "protocol_version";
    case 71:  return "insufficient_security";
    case 80:  return "internal_error";
    case 90:  return "user_canceled";
    case 100: return "no_renegotiation";
    default:  return "unknown";
    }
}
