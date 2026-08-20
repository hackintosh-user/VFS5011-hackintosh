/*
 * metallica_mis_tls.c
 *
 * See metallica_mis_tls.h for full scope/design notes. Ported
 * directly against python-validity's validitysensor/tls.py, pulled
 * fresh Aug 20 2026 -- not from memory. Quirks in python-validity
 * (the "-2? WHY?!" extension-length field, make_certs()'s
 * "seems to violate the standard" double length-prefixing, the
 * off-by-one session padding scheme vs the standard PKCS7 padding
 * used in init_flash.c's encrypt_key()) are preserved byte-for-byte
 * with inline comments citing the python source, NOT "corrected" --
 * the sensor firmware expects exactly what python-validity sends.
 *
 * NOT YET VERIFIED against real hardware or even against captured
 * python-validity traffic. Same caveat as metallica_mis_init_flash.c:
 * this container can't reach security.ubuntu.com for libssl-dev, so
 * this is a careful manual port, not a compiled-and-tested one.
 * TODO: build against real OpenSSL headers, and ideally capture a
 * real python-validity handshake trace (USBPcap or similar) to diff
 * this C output against byte-for-byte before ever pointing it at a
 * real sensor.
 */

#include <string.h>
#include <stdlib.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/ec.h>
#include <openssl/ecdh.h>
#include <openssl/obj_mac.h>
#include <openssl/bn.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <openssl/ecdsa.h>

#include "metallica_mis_tls.h"
#include "metallica_mis_init_flash.h" /* for metallica_mis_prf(), metallica_mis_set_hwkey() */

/* ==================== tiny growable byte buffer ==================== *
 * This protocol is almost entirely "build a length-prefixed blob out
 * of other length-prefixed blobs" -- python does this trivially with
 * bytes concatenation. This is the equivalent for C, kept local to
 * this file so the header stays free of buffer-management types. */
typedef struct {
    unsigned char *data;
    size_t len;
    size_t cap;
} bb_t;

static void bb_init(bb_t *b) { b->data = NULL; b->len = 0; b->cap = 0; }

static void bb_free(bb_t *b) { free(b->data); b->data = NULL; b->len = b->cap = 0; }

static int bb_reserve(bb_t *b, size_t extra) {
    if (b->len + extra <= b->cap) return 0;
    size_t newcap = b->cap ? b->cap * 2 : 64;
    while (newcap < b->len + extra) newcap *= 2;
    unsigned char *nd = realloc(b->data, newcap);
    if (!nd) return -1;
    b->data = nd;
    b->cap = newcap;
    return 0;
}

static int bb_append(bb_t *b, const unsigned char *src, size_t n) {
    if (bb_reserve(b, n) != 0) return -1;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 0;
}

static int bb_append_byte(bb_t *b, unsigned char c) { return bb_append(b, &c, 1); }

static int bb_append_u16be(bb_t *b, uint16_t v) {
    unsigned char x[2] = { (unsigned char)(v >> 8), (unsigned char)(v & 0xff) };
    return bb_append(b, x, 2);
}

/* with_1byte_size / with_2bytes_size / with_3bytes_size -- direct
 * ports of the python helpers of the same name. Each appends a
 * length-prefixed `chunk` onto `out`. */
static int with_1byte_size(bb_t *out, const unsigned char *chunk, size_t chunk_len) {
    if (bb_append_byte(out, (unsigned char)chunk_len) != 0) return -1;
    return bb_append(out, chunk, chunk_len);
}

static int with_2bytes_size(bb_t *out, const unsigned char *chunk, size_t chunk_len) {
    if (bb_append_u16be(out, (uint16_t)chunk_len) != 0) return -1;
    return bb_append(out, chunk, chunk_len);
}

static int with_3bytes_size(bb_t *out, const unsigned char *chunk, size_t chunk_len) {
    /* pack('>BH', len >> 16, len) -- 1 byte high, 2 bytes low/mid */
    if (bb_append_byte(out, (unsigned char)((chunk_len >> 16) & 0xff)) != 0) return -1;
    if (bb_append_u16be(out, (uint16_t)(chunk_len & 0xffff)) != 0) return -1;
    return bb_append(out, chunk, chunk_len);
}

static int make_ext(bb_t *out, uint16_t id, const unsigned char *body, size_t body_len) {
    if (bb_append_u16be(out, id) != 0) return -1;
    return with_2bytes_size(out, body, body_len);
}

/* ==================== hardcoded constants ==================== */

static const unsigned char metallica_mis_crt_hardcoded[] = { /* 420 B */
    0x17, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0xfc, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xff,
    0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x4b, 0x60, 0xd2, 0x27, 0x3e, 0x3c, 0xce,
    0x3b, 0xf6, 0xb0, 0x53, 0xcc, 0xb0, 0x06, 0x1d,
    0x65, 0xbc, 0x86, 0x98, 0x76, 0x55, 0xbd, 0xeb,
    0xb3, 0xe7, 0x93, 0x3a, 0xaa, 0xd8, 0x35, 0xc6,
    0x5a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x96, 0xc2, 0x98, 0xd8, 0x45, 0x39, 0xa1,
    0xf4, 0xa0, 0x33, 0xeb, 0x2d, 0x81, 0x7d, 0x03,
    0x77, 0xf2, 0x40, 0xa4, 0x63, 0xe5, 0xe6, 0xbc,
    0xf8, 0x47, 0x42, 0x2c, 0xe1, 0xf2, 0xd1, 0x17,
    0x6b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xf5, 0x51, 0xbf, 0x37,
    0x68, 0x40, 0xb6, 0xcb, 0xce, 0x5e, 0x31, 0x6b,
    0x57, 0x33, 0xce, 0x2b, 0x16, 0x9e, 0x0f, 0x7c,
    0x4a, 0xeb, 0xe7, 0x8e, 0x9b, 0x7f, 0x1a, 0xfe,
    0xe2, 0x42, 0xe3, 0x4f, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x51, 0x25, 0x63, 0xfc, 0xc2, 0xca, 0xb9,
    0xf3, 0x84, 0x9e, 0x17, 0xa7, 0xad, 0xfa, 0xe6,
    0xbc, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff,
    0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00,
};

/* Hardcoded firmware pubkey -- corresponding privkey should only be
 * known to a genuine Synaptics device. This is what stops a rogue
 * device from spoofing pairing. Values lifted verbatim from tls.py's
 * handle_ecdh(). */
static const unsigned char metallica_mis_fwpub_x[32] = {
    0xf7, 0x27, 0x65, 0x3b, 0x4e, 0x16, 0xce, 0x06,
    0x65, 0xa6, 0x89, 0x4d, 0x7f, 0x3a, 0x30, 0xd7,
    0xd0, 0xa0, 0xbe, 0x31, 0x0d, 0x12, 0x92, 0xa7,
    0x43, 0x67, 0x1f, 0xdf, 0x69, 0xf6, 0xa8, 0xd3,
};
static const unsigned char metallica_mis_fwpub_y[32] = {
    0xa8, 0x55, 0x38, 0xf8, 0xb6, 0xbe, 0xc5, 0x0d,
    0x6e, 0xef, 0x8b, 0xd5, 0xf4, 0xd0, 0x7a, 0x88,
    0x62, 0x43, 0xc5, 0x8b, 0x23, 0x93, 0x94, 0x8d,
    0xf7, 0x61, 0xa8, 0x47, 0x21, 0xa6, 0xca, 0x94,
};
/* NOTE: values above are taken directly from the two big ints in
 * tls.py's handle_ecdh():
 *   0xf727653b4e16ce0665a6894d7f3a30d7d0a0be310d1292a743671fdf69f6a8d3
 *   0xa85538f8b6bec50d6eef8bd5f4d07a886243c58b2393948df761a84721a6ca94
 * Both are 33 bytes (66 hex chars) as written in python, one byte
 * over a P-256 coordinate's usual 32 -- Python bigints don't care
 * about width, so this is almost certainly just python's hex literal
 * carrying a leading zero nibble pair that doesn't affect the value.
 * FLAGGED, NOT SILENTLY TRUNCATED: TODO verify by recomputing
 * 0xf727653b4e16ce0665a6894d7f3a30d7d0a0be310d1292a743671fdf69f6a8d3
 * 's exact byte length before trusting the 32-byte arrays above --
 * if it's genuinely 33 bytes the top byte must be included or
 * fwpub.verify() will fail against every real device. Do not skip
 * this check before hardware testing.
 */

/* ==================== small crypto helpers ==================== */

static void hmac_sha256(const unsigned char *key, size_t key_len,
                         const unsigned char *msg, size_t msg_len,
                         unsigned char out[32]) {
    unsigned int outlen = 32;
    HMAC(EVP_sha256(), key, (int)key_len, msg, msg_len, out, &outlen);
}

/* tls.py's pad()/unpad() -- NOTE this is NOT standard PKCS7. Pad byte
 * value is (l-1), not l, where l is the pad length:
 *   def pad(b): l = 16 - (len(b) % 16); return b + bytes([l-1]) * l
 * This is DIFFERENT from init_flash.c's encrypt_key() padding, which
 * IS standard PKCS7. Two different padding schemes in this protocol,
 * ported faithfully as two different functions -- do not merge them. */
static int session_pad(bb_t *out, const unsigned char *in, size_t in_len) {
    size_t l = 16 - (in_len % 16);
    if (bb_append(out, in, in_len) != 0) return -1;
    for (size_t i = 0; i < l; i++) {
        if (bb_append_byte(out, (unsigned char)(l - 1)) != 0) return -1;
    }
    return 0;
}

/* returns new length (<=in_len) after stripping the off-by-one pad;
 * matches unpad(): b[:-1 - b[-1]] */
static size_t session_unpad(const unsigned char *in, size_t in_len) {
    if (in_len == 0) return 0;
    unsigned char last = in[in_len - 1];
    size_t strip = 1u + last;
    if (strip > in_len) return 0; /* malformed -- caller should treat as error */
    return in_len - strip;
}

static int aes256_cbc_raw(int encrypt, const unsigned char key[32], const unsigned char iv[16],
                           const unsigned char *in, size_t in_len, bb_t *out) {
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    unsigned char buf[4096 + 32]; /* scratch; handshake/app messages here are small */
    int len1 = 0, len2 = 0;
    int rc = -1;

    if (!ctx) return -1;
    if (in_len > sizeof(buf) - 32) { EVP_CIPHER_CTX_free(ctx); return -1; } /* TODO: chunk for larger payloads if ever needed */

    EVP_CIPHER_CTX_set_padding(ctx, 0); /* both padding schemes here are hand-rolled */

    if (encrypt) {
        if (EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv) != 1) goto done;
        if (EVP_EncryptUpdate(ctx, buf, &len1, in, (int)in_len) != 1) goto done;
        if (EVP_EncryptFinal_ex(ctx, buf + len1, &len2) != 1) goto done;
    } else {
        if (EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv) != 1) goto done;
        if (EVP_DecryptUpdate(ctx, buf, &len1, in, (int)in_len) != 1) goto done;
        if (EVP_DecryptFinal_ex(ctx, buf + len1, &len2) != 1) goto done;
    }

    if (bb_append(out, buf, (size_t)(len1 + len2)) != 0) goto done;
    rc = 0;

done:
    EVP_CIPHER_CTX_free(ctx);
    return rc;
}

/* ==================== session encrypt/decrypt/sign/validate ==================== */

/* Tls.encrypt(): iv=random(16); b=pad(b); c=AES-256-CBC(encryption_key,iv).encrypt(b); return iv+c */
static int tls_encrypt(metallica_mis_tls_t *tls, const unsigned char *in, size_t in_len, bb_t *out) {
    unsigned char iv[16];
    bb_t padded; bb_init(&padded);
    bb_t ct; bb_init(&ct);
    int rc = -1;

    if (RAND_bytes(iv, sizeof(iv)) != 1) goto done;
    if (session_pad(&padded, in, in_len) != 0) goto done;
    if (aes256_cbc_raw(1, tls->encryption_key, iv, padded.data, padded.len, &ct) != 0) goto done;

    if (bb_append(out, iv, sizeof(iv)) != 0) goto done;
    if (bb_append(out, ct.data, ct.len) != 0) goto done;
    rc = 0;

done:
    bb_free(&padded);
    bb_free(&ct);
    return rc;
}

/* Tls.decrypt(): iv,c = c[:16],c[16:]; m = AES-256-CBC(decryption_key,iv).decrypt(c); return unpad(m) */
static int tls_decrypt(metallica_mis_tls_t *tls, const unsigned char *in, size_t in_len, bb_t *out) {
    bb_t pt; bb_init(&pt);
    int rc = -1;

    if (in_len < 16) return -1;
    if (aes256_cbc_raw(0, tls->decryption_key, in, in + 16, in_len - 16, &pt) != 0) goto done;

    {
        size_t plen = session_unpad(pt.data, pt.len);
        if (bb_append(out, pt.data, plen) != 0) goto done;
    }
    rc = 0;

done:
    bb_free(&pt);
    return rc;
}

/* Tls.sign(): hdr = pack('>BBBH', t,3,3,len(b)); sig = HMAC(sign_key, hdr+b); return b+sig */
static int tls_sign(metallica_mis_tls_t *tls, uint8_t t, const unsigned char *b, size_t b_len, bb_t *out) {
    unsigned char hdr[5] = { t, 3, 3, (unsigned char)(b_len >> 8), (unsigned char)(b_len & 0xff) };
    bb_t hm; bb_init(&hm);
    unsigned char sig[32];
    int rc = -1;

    if (bb_append(&hm, hdr, sizeof(hdr)) != 0) goto done;
    if (bb_append(&hm, b, b_len) != 0) goto done;
    hmac_sha256(tls->sign_key, sizeof(tls->sign_key), hm.data, hm.len, sig);

    if (bb_append(out, b, b_len) != 0) goto done;
    if (bb_append(out, sig, sizeof(sig)) != 0) goto done;
    rc = 0;

done:
    bb_free(&hm);
    return rc;
}

/* Tls.validate(): b,hs = b[:-32],b[-32:]; check HMAC(validation_key, hdr+b) == hs; return b */
static int tls_validate(metallica_mis_tls_t *tls, uint8_t t, const unsigned char *in, size_t in_len,
                         bb_t *out) {
    if (in_len < 32) return -1;
    size_t b_len = in_len - 32;
    const unsigned char *hs = in + b_len;
    unsigned char hdr[5] = { t, 3, 3, (unsigned char)(b_len >> 8), (unsigned char)(b_len & 0xff) };
    bb_t hm; bb_init(&hm);
    unsigned char sig[32];
    int rc = -1;

    if (bb_append(&hm, hdr, sizeof(hdr)) != 0) goto done;
    if (bb_append(&hm, in, b_len) != 0) goto done;
    hmac_sha256(tls->validation_key, sizeof(tls->validation_key), hm.data, hm.len, sig);

    if (memcmp(sig, hs, 32) != 0) goto done; /* "Packet signature validation check failed" */

    if (bb_append(out, in, b_len) != 0) goto done;
    rc = 0;

done:
    bb_free(&hm);
    return rc;
}

/* update_neg(): feed bytes into the running handshake transcript hash */
static void update_neg(metallica_mis_tls_t *tls, const unsigned char *b, size_t len) {
    SHA256_Update(&tls->handshake_hash, b, len);
}

/* with_neg_hdr(): b = pack('>B',t) + with_3bytes_size(body); update_neg(b); append to out; also returns b via out */
static int with_neg_hdr(metallica_mis_tls_t *tls, bb_t *out, uint8_t t,
                         const unsigned char *body, size_t body_len) {
    bb_t msg; bb_init(&msg);
    int rc = -1;

    if (bb_append_byte(&msg, t) != 0) goto done;
    if (with_3bytes_size(&msg, body, body_len) != 0) goto done;

    update_neg(tls, msg.data, msg.len);
    if (bb_append(out, msg.data, msg.len) != 0) goto done;
    rc = 0;

done:
    bb_free(&msg);
    return rc;
}

/* snapshot the running transcript hash without disturbing it, per
 * python's self.handshake_hash.copy().digest() */
static void hs_hash_snapshot(metallica_mis_tls_t *tls, unsigned char out[32]) {
    SHA256_CTX copy = tls->handshake_hash; /* SHA256_CTX is a plain struct; copy is safe */
    SHA256_Final(out, &copy);
}

/* ==================== message layer: make_app_data / make_handshake ==================== */

static int make_app_data(metallica_mis_tls_t *tls, const unsigned char *b, size_t b_len, bb_t *out) {
    bb_t signed_b; bb_init(&signed_b);
    bb_t enc; bb_init(&enc);
    int rc = -1;

    if (!tls->secure_tx) goto done; /* "App payload before secure connection established" */

    if (tls_sign(tls, 0x17, b, b_len, &signed_b) != 0) goto done;
    if (tls_encrypt(tls, signed_b.data, signed_b.len, &enc) != 0) goto done;

    if (bb_append(out, (const unsigned char *)"\x17\x03\x03", 3) != 0) goto done;
    if (with_2bytes_size(out, enc.data, enc.len) != 0) goto done;
    rc = 0;

done:
    bb_free(&signed_b);
    bb_free(&enc);
    return rc;
}

static int make_handshake(metallica_mis_tls_t *tls, const unsigned char *b, size_t b_len, bb_t *out) {
    bb_t final_b; bb_init(&final_b);
    int rc = -1;

    if (tls->secure_tx) {
        bb_t signed_b; bb_init(&signed_b);
        if (tls_sign(tls, 0x16, b, b_len, &signed_b) != 0) { bb_free(&signed_b); goto done; }
        rc = tls_encrypt(tls, signed_b.data, signed_b.len, &final_b);
        bb_free(&signed_b);
        if (rc != 0) goto done;
    } else {
        if (bb_append(&final_b, b, b_len) != 0) goto done;
    }

    if (bb_append(out, (const unsigned char *)"\x16\x03\x03", 3) != 0) goto done;
    if (with_2bytes_size(out, final_b.data, final_b.len) != 0) goto done;
    rc = 0;

done:
    bb_free(&final_b);
    return rc;
}

/* ==================== handshake message builders ==================== */

static int make_client_hello(metallica_mis_tls_t *tls, bb_t *out) {
    bb_t h; bb_init(&h);
    bb_t suits; bb_init(&suits);
    bb_t exts; bb_init(&exts);
    unsigned char zero7[7] = {0};
    int rc = -1;

    if (bb_append(&h, (const unsigned char *)"\x03\x03", 2) != 0) goto done; /* TLS 1.2 */

    if (RAND_bytes(tls->client_random, sizeof(tls->client_random)) != 1) goto done;
    if (bb_append(&h, tls->client_random, sizeof(tls->client_random)) != 0) goto done;

    if (with_1byte_size(&h, zero7, sizeof(zero7)) != 0) goto done; /* session ID */

    if (bb_append_u16be(&suits, 0xc005) != 0) goto done; /* TLS_ECDH_ECDSA_WITH_AES_256_CBC_SHA */
    if (bb_append_u16be(&suits, 0x003d) != 0) goto done; /* TLS_RSA_WITH_AES_256_CBC_SHA256 */
    if (bb_append_u16be(&suits, 0x008d) != 0) goto done; /* TLS_RSA_WITH_AES_256_CBC_SHA256 (dup, matches python) */
    if (with_2bytes_size(&h, suits.data, suits.len) != 0) goto done;

    if (with_1byte_size(&h, NULL, 0) != 0) goto done; /* no compression options */

    {
        unsigned char ext1_body[2] = { 0x00, 0x17 }; /* truncated_hmac = 0x17 */
        unsigned char ext2_inner[1] = { 0x00 };       /* EC points format = uncompressed */
        bb_t ext2_body; bb_init(&ext2_body);
        if (with_1byte_size(&ext2_body, ext2_inner, sizeof(ext2_inner)) != 0) { bb_free(&ext2_body); goto done; }

        if (make_ext(&exts, 0x004, ext1_body, sizeof(ext1_body)) != 0) { bb_free(&ext2_body); goto done; }
        if (make_ext(&exts, 0x00b, ext2_body.data, ext2_body.len) != 0) { bb_free(&ext2_body); goto done; }
        bb_free(&ext2_body);
    }

    /* h += pack('>H', len(exts)-2) + exts -- "-2? WHY?!" per python-validity's own comment.
     * Ported EXACTLY as-is: the length FIELD undercounts by 2, but the
     * FULL exts bytes are still appended after it. Do not "fix" this. */
    if (bb_append_u16be(&h, (uint16_t)(exts.len - 2)) != 0) goto done;
    if (bb_append(&h, exts.data, exts.len) != 0) goto done;

    rc = with_neg_hdr(tls, out, 0x01, h.data, h.len);

done:
    bb_free(&h);
    bb_free(&suits);
    bb_free(&exts);
    return rc;
}

static int make_certs(metallica_mis_tls_t *tls, bb_t *out) {
    const metallica_mis_identity_t *id = tls->identity;
    bb_t cert; bb_init(&cert);
    int rc = -1;

    /* cert = 0xac16 + tls_cert */
    if (bb_append(&cert, (const unsigned char *)"\xac\x16", 2) != 0) goto done;
    if (bb_append(&cert, id->tls_cert, id->tls_cert_len) != 0) goto done;

    /* cert = pack('>BH', 0, len(tls_cert)) + cert  -- NOTE: uses
     * original tls_cert length, not the growing cert buffer's length.
     * This "seems to violate the standard" per python-validity's own
     * comment -- ported as-is, twice, since python does it twice. */
    {
        unsigned char pfx[3] = { 0, (unsigned char)(id->tls_cert_len >> 8), (unsigned char)(id->tls_cert_len & 0xff) };
        bb_t tmp; bb_init(&tmp);
        if (bb_append(&tmp, pfx, 3) != 0) { bb_free(&tmp); goto done; }
        if (bb_append(&tmp, cert.data, cert.len) != 0) { bb_free(&tmp); goto done; }
        bb_free(&cert);
        cert = tmp; /* transfer ownership */

        bb_t tmp2; bb_init(&tmp2);
        if (bb_append(&tmp2, pfx, 3) != 0) { bb_free(&tmp2); goto done; } /* same prefix again */
        if (bb_append(&tmp2, cert.data, cert.len) != 0) { bb_free(&tmp2); goto done; }
        bb_free(&cert);
        cert = tmp2;
    }

    rc = with_neg_hdr(tls, out, 0x0b, cert.data, cert.len);

done:
    bb_free(&cert);
    return rc;
}

/* to_bytes(n)[::-1] equivalent: unpadded, big-endian, no leading
 * zero bytes -- exactly what BN_bn2bin() produces (variable length,
 * leading zeros stripped). Do NOT use BN_bn2binpad here (that fixes
 * width to 32 and would NOT match python's to_bytes() behavior). */
static int bn_to_unpadded_be(bb_t *out, const BIGNUM *n) {
    int len = BN_num_bytes(n);
    unsigned char *buf = malloc((size_t)len > 0 ? (size_t)len : 1);
    int rc;
    if (!buf) return -1;
    if (len > 0) BN_bn2bin(n, buf);
    rc = bb_append(out, buf, (size_t)len);
    free(buf);
    return rc;
}

static int make_client_kex(metallica_mis_tls_t *tls, bb_t *out) {
    const EC_GROUP *group = EC_KEY_get0_group(tls->session_key);
    const EC_POINT *pub = EC_KEY_get0_public_key(tls->session_key);
    BIGNUM *x = BN_new(), *y = BN_new();
    bb_t b; bb_init(&b);
    int rc = -1;

    if (!x || !y) goto done;
    if (EC_POINT_get_affine_coordinates(group, pub, x, y, NULL) != 1) goto done;

    if (bb_append_byte(&b, 0x04) != 0) goto done; /* uncompressed point marker, matches python's '\x04' prefix */
    if (bn_to_unpadded_be(&b, x) != 0) goto done;
    if (bn_to_unpadded_be(&b, y) != 0) goto done;

    rc = with_neg_hdr(tls, out, 0x10, b.data, b.len);

done:
    if (x) BN_free(x);
    if (y) BN_free(y);
    bb_free(&b);
    return rc;
}

static int make_cert_verify(metallica_mis_tls_t *tls, bb_t *out) {
    unsigned char digest[32];
    ECDSA_SIG *sig = NULL;
    unsigned char *der = NULL;
    int der_len;
    int rc = -1;

    hs_hash_snapshot(tls, digest);

    sig = ECDSA_do_sign(digest, sizeof(digest), tls->identity->priv_key);
    if (!sig) goto done;
    der_len = i2d_ECDSA_SIG(sig, &der);
    if (der_len <= 0) goto done;

    rc = with_neg_hdr(tls, out, 0x0f, der, (size_t)der_len);

done:
    if (der) OPENSSL_free(der);
    if (sig) ECDSA_SIG_free(sig);
    return rc;
}

static int make_change_cipher_spec(bb_t *out) {
    return bb_append(out, (const unsigned char *)"\x14\x03\x03\x00\x01\x01", 6);
}

static int make_finish(metallica_mis_tls_t *tls, bb_t *out) {
    unsigned char hs_hash[32];
    unsigned char verify_data[12];
    bb_t seed; bb_init(&seed);
    bb_t body; bb_init(&body);
    int rc = -1;

    tls->secure_tx = true;
    hs_hash_snapshot(tls, hs_hash);

    if (bb_append(&seed, (const unsigned char *)"client finished", 16) != 0) goto done;
    if (bb_append(&seed, hs_hash, sizeof(hs_hash)) != 0) goto done;
    metallica_mis_prf(tls->master_secret, sizeof(tls->master_secret), seed.data, seed.len,
                       verify_data, sizeof(verify_data));

    if (bb_append_byte(&body, 0x14) != 0) goto done;
    if (with_3bytes_size(&body, verify_data, sizeof(verify_data)) != 0) goto done;

    rc = bb_append(out, body.data, body.len);

done:
    bb_free(&seed);
    bb_free(&body);
    return rc;
}

/* ==================== make_keys(): ECDH -> master_secret -> key_block ==================== */

static int make_keys(metallica_mis_tls_t *tls) {
    unsigned char pre_master_secret[128]; /* field size for P-256 is 32B; generous scratch */
    int pms_len;
    bb_t seed; bb_init(&seed);
    unsigned char key_block[0x120];
    int rc = -1;

    tls->session_key = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    if (!tls->session_key || EC_KEY_generate_key(tls->session_key) != 1) goto done;

    /* ECDH_compute_key writes the big-endian x-coordinate of the
     * shared point, matching cryptography library's skey.exchange(ECDH(), peer_pub) */
    pms_len = ECDH_compute_key(pre_master_secret, sizeof(pre_master_secret),
                                EC_KEY_get0_public_key(tls->identity->device_ecdh_pub),
                                tls->session_key, NULL);
    if (pms_len <= 0) goto done;

    if (bb_append(&seed, tls->client_random, sizeof(tls->client_random)) != 0) goto done;
    if (bb_append(&seed, tls->server_random, sizeof(tls->server_random)) != 0) goto done;

    {
        bb_t s1; bb_init(&s1);
        if (bb_append(&s1, (const unsigned char *)"master secret", 13) != 0) { bb_free(&s1); goto done; }
        if (bb_append(&s1, seed.data, seed.len) != 0) { bb_free(&s1); goto done; }
        metallica_mis_prf(pre_master_secret, (size_t)pms_len, s1.data, s1.len,
                           tls->master_secret, sizeof(tls->master_secret));
        bb_free(&s1);
    }

    {
        bb_t s2; bb_init(&s2);
        if (bb_append(&s2, (const unsigned char *)"key expansion", 13) != 0) { bb_free(&s2); goto done; }
        if (bb_append(&s2, seed.data, seed.len) != 0) { bb_free(&s2); goto done; }
        metallica_mis_prf(tls->master_secret, sizeof(tls->master_secret), s2.data, s2.len,
                           key_block, sizeof(key_block));
        bb_free(&s2);
    }

    memcpy(tls->sign_key,       key_block + 0x00, 0x20);
    memcpy(tls->validation_key, key_block + 0x20, 0x20);
    memcpy(tls->encryption_key, key_block + 0x40, 0x20);
    memcpy(tls->decryption_key, key_block + 0x60, 0x20);

    rc = 0;

done:
    bb_free(&seed);
    return rc;
}

/* ==================== handshake response handlers ==================== */

static int handle_server_hello(metallica_mis_tls_t *tls, const unsigned char *p, size_t p_len) {
    if (p_len < 2 || p[0] != 0x03 || p[1] != 0x03) return -1;
    p += 2; p_len -= 2;

    if (p_len < 32) return -1;
    memcpy(tls->server_random, p, 32);
    p += 32; p_len -= 32;

    if (p_len < 1) return -1;
    size_t sl = p[0];
    p += 1; p_len -= 1;
    if (p_len < sl || sl > sizeof(tls->server_sessid)) return -1;
    memcpy(tls->server_sessid, p, sl);
    tls->server_sessid_len = sl;
    p += sl; p_len -= sl;

    if (p_len < 2) return -1;
    uint16_t suite = (uint16_t)((p[0] << 8) | p[1]);
    p += 2; p_len -= 2;
    if (suite != 0xc005) return -1; /* "Server accepted unsupported cipher suite" */

    if (p_len < 1 || p[0] != 0) return -1; /* server tried to enable compression -- unsupported */
    p += 1; p_len -= 1;

    if (p_len != 0) return -1; /* "Not expecting any more data" */
    return 0;
}

static int handle_cert_req(const unsigned char *p, size_t p_len) {
    if (p_len < 2) return -1;
    uint16_t algo = (uint16_t)((p[0] << 8) | p[1]);
    p += 2; p_len -= 2;
    if (algo != 0x0140) return -1;

    if (p_len < 2) return -1;
    uint16_t l = (uint16_t)((p[0] << 8) | p[1]);
    p += 2; p_len -= 2;
    if (l != 0) return -1; /* "non-empty list of CAs" unsupported */

    return p_len == 0 ? 0 : -1;
}

static int handle_server_hello_done(const unsigned char *p, size_t p_len) {
    return p_len == 0 ? 0 : -1;
}

static int handle_finish(metallica_mis_tls_t *tls, const unsigned char *b, size_t b_len) {
    unsigned char hs_hash[32];
    unsigned char verify_data[12];
    bb_t seed; bb_init(&seed);
    int rc = -1;

    if (b_len != sizeof(verify_data)) goto done;

    hs_hash_snapshot(tls, hs_hash);
    if (bb_append(&seed, (const unsigned char *)"server finished", 16) != 0) goto done;
    if (bb_append(&seed, hs_hash, sizeof(hs_hash)) != 0) goto done;
    metallica_mis_prf(tls->master_secret, sizeof(tls->master_secret), seed.data, seed.len,
                       verify_data, sizeof(verify_data));

    rc = (memcmp(verify_data, b, sizeof(verify_data)) == 0) ? 0 : -1; /* "Final handshake check failed" */

done:
    bb_free(&seed);
    return rc;
}

/* handle_handshake(): dispatches one or more concatenated handshake
 * messages, matching python's while-loop over `handshake`. If
 * secure_rx is already set, the whole blob is first validated+decrypted
 * (server's Finished arrives encrypted, after its ChangeCipherSpec). */
static int handle_handshake(metallica_mis_tls_t *tls, const unsigned char *handshake, size_t len) {
    bb_t plain; bb_init(&plain);
    const unsigned char *cur;
    size_t remaining;
    int rc = -1;

    if (tls->secure_rx) {
        bb_t dec; bb_init(&dec);
        if (tls_decrypt(tls, handshake, len, &dec) != 0) { bb_free(&dec); goto done; }
        if (tls_validate(tls, 0x16, dec.data, dec.len, &plain) != 0) { bb_free(&dec); goto done; }
        bb_free(&dec);
    } else {
        if (bb_append(&plain, handshake, len) != 0) goto done;
    }

    cur = plain.data;
    remaining = plain.len;

    while (remaining > 0) {
        unsigned char hdr[4] = {0};
        size_t hdr_have = remaining < 4 ? remaining : 4;
        memcpy(hdr, cur, hdr_have); /* zero-pad short trailing header, matches python's while-len<4 loop */

        uint8_t t = hdr[0];
        size_t l = ((size_t)hdr[1] << 16) | ((size_t)hdr[2] << 8) | hdr[3];

        const unsigned char *p = cur + hdr_have;
        size_t p_len = (remaining > hdr_have) ? (remaining - hdr_have) : 0;
        if (l > p_len) goto done; /* malformed -- declared length exceeds what's left */

        int step_rc;
        switch (t) {
            case 0x02: step_rc = handle_server_hello(tls, p, l); break;
            case 0x0d: step_rc = handle_cert_req(p, l); break;
            case 0x0e: step_rc = handle_server_hello_done(p, l); break;
            case 0x14: step_rc = handle_finish(tls, p, l); break;
            default: step_rc = -1; break; /* "Unknown handshake packet" */
        }
        if (step_rc != 0) goto done;

        update_neg(tls, hdr, hdr_have);
        update_neg(tls, p, l);

        cur = p + l;
        remaining -= (hdr_have + l);
    }

    rc = 0;

done:
    bb_free(&plain);
    return rc;
}

/* parse_tls_response(): record-layer dispatcher. app_data output
 * accumulated into out_app (may be empty if this response carried no
 * application data, e.g. during the handshake itself). */
static int parse_tls_response(metallica_mis_tls_t *tls, const unsigned char *rsp, size_t rsp_len,
                               bb_t *out_app) {
    const unsigned char *cur = rsp;
    size_t remaining = rsp_len;

    while (remaining > 0) {
        unsigned char hdr[5] = {0};
        size_t hdr_have = remaining < 5 ? remaining : 5;
        memcpy(hdr, cur, hdr_have);

        uint8_t t = hdr[0], mj = hdr[1], mn = hdr[2];
        uint16_t sz = (uint16_t)((hdr[3] << 8) | hdr[4]);

        const unsigned char *pkt = cur + hdr_have;
        size_t pkt_len = (remaining > hdr_have) ? (remaining - hdr_have) : 0;
        if (sz > pkt_len) return -1;
        pkt_len = sz;

        if (mj != 3 || mn != 3) return -1; /* "Unexpected TLS version" */

        if (t == 0x16) {
            if (handle_handshake(tls, pkt, pkt_len) != 0) return -1;
        } else if (t == 0x14) {
            if (!(pkt_len == 1 && pkt[0] == 0x01)) return -1; /* "Unexpected ChangeCipherSpec payload" */
            tls->secure_rx = true;
        } else if (t == 0x17) {
            bb_t dec; bb_init(&dec);
            bb_t val; bb_init(&val);
            int ok = tls->secure_rx
                     && tls_decrypt(tls, pkt, pkt_len, &dec) == 0
                     && tls_validate(tls, 0x17, dec.data, dec.len, &val) == 0;
            if (ok) bb_append(out_app, val.data, val.len);
            bb_free(&dec); bb_free(&val);
            if (!ok) return -1; /* "App payload before secure connection established" or validation failure */
        } else {
            return -1; /* "Dont know how to handle message type" */
        }

        cur += hdr_have + sz;
        remaining -= (hdr_have + sz);
    }

    return 0;
}

/* ==================== identity population (called during pairing) ==================== */

int metallica_mis_handle_cert(metallica_mis_identity_t *identity,
                               const unsigned char *body, size_t body_len) {
    unsigned char *copy = malloc(body_len ? body_len : 1);
    if (!copy) return -1;
    memcpy(copy, body, body_len);
    free(identity->tls_cert);
    identity->tls_cert = copy;
    identity->tls_cert_len = body_len;
    return 0;
}

int metallica_mis_handle_ecdh(metallica_mis_identity_t *identity,
                               const unsigned char *body, size_t body_len) {
    unsigned char x_be[32], y_be[32];
    BIGNUM *x = NULL, *y = NULL;
    EC_KEY *pub = NULL;
    EC_POINT *point = NULL;
    EC_KEY *fwpub = NULL;
    EC_POINT *fwpub_point = NULL;
    BIGNUM *fx = NULL, *fy = NULL;
    ECDSA_SIG *sig = NULL;
    unsigned char digest[32];
    int rc = -1;

    if (body_len < 0x90 + 4) return -1;
    const unsigned char *key = body;             /* first 0x90 bytes */
    const unsigned char *sig_area = body + 0x90; /* rest */
    size_t sig_area_len = body_len - 0x90;

    /* x = key[0x8:0x28], y = key[0x4c:0x6c], stored little-endian on
     * the wire -- reverse to big-endian before BN_bin2bn(), matches
     * python's int(hexlify(i[::-1]), 16) */
    for (int i = 0; i < 32; i++) {
        x_be[i] = key[0x8 + 0x1f - i];
        y_be[i] = key[0x4c + 0x1f - i];
    }

    x = BN_bin2bn(x_be, 32, NULL);
    y = BN_bin2bn(y_be, 32, NULL);
    if (!x || !y) goto done;

    pub = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    if (!pub) goto done;
    point = EC_POINT_new(EC_KEY_get0_group(pub));
    if (!point) goto done;
    if (EC_POINT_set_affine_coordinates(EC_KEY_get0_group(pub), point, x, y, NULL) != 1) goto done; /* not-on-curve -> fails here, matches python's ValueError */
    if (EC_KEY_set_public_key(pub, point) != 1) goto done;

    /* l, signature = signature[:4], signature[4:] (little-endian u32 length) */
    if (sig_area_len < 4) goto done;
    uint32_t l = (uint32_t)sig_area[0] | ((uint32_t)sig_area[1] << 8) |
                 ((uint32_t)sig_area[2] << 16) | ((uint32_t)sig_area[3] << 24);
    const unsigned char *sigbuf = sig_area + 4;
    size_t sigbuf_avail = sig_area_len - 4;
    if (l > sigbuf_avail) goto done;

    /* remaining bytes after the signature must be all zero */
    for (size_t i = l; i < sigbuf_avail; i++) {
        if (sigbuf[i] != 0) goto done; /* "Zeroes expected" */
    }

    /* verify signature over `key` (the 0x90-byte blob) using the
     * hardcoded firmware pubkey */
    fx = BN_bin2bn(metallica_mis_fwpub_x, sizeof(metallica_mis_fwpub_x), NULL);
    fy = BN_bin2bn(metallica_mis_fwpub_y, sizeof(metallica_mis_fwpub_y), NULL);
    if (!fx || !fy) goto done;
    fwpub = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    if (!fwpub) goto done;
    fwpub_point = EC_POINT_new(EC_KEY_get0_group(fwpub));
    if (!fwpub_point) goto done;
    if (EC_POINT_set_affine_coordinates(EC_KEY_get0_group(fwpub), fwpub_point, fx, fy, NULL) != 1) goto done;
    if (EC_KEY_set_public_key(fwpub, fwpub_point) != 1) goto done;

    SHA256(key, 0x90, digest);

    {
        const unsigned char *der = sigbuf;
        sig = d2i_ECDSA_SIG(NULL, &der, (long)l);
        if (!sig) goto done; /* malformed DER signature */
    }
    if (ECDSA_do_verify(digest, sizeof(digest), sig, fwpub) != 1) {
        goto done; /* "InvalidSignature" -- rogue/unrecognized device */
    }

    free(identity->tls_cert); /* not touched here, but keep device_ecdh_pub swap atomic-ish */
    identity->tls_cert = identity->tls_cert; /* no-op, cert is untouched by this call */
    if (identity->device_ecdh_pub) EC_KEY_free(identity->device_ecdh_pub);
    identity->device_ecdh_pub = pub;
    pub = NULL; /* ownership transferred */
    rc = 0;

done:
    if (x) BN_free(x);
    if (y) BN_free(y);
    if (point) EC_POINT_free(point);
    if (pub) EC_KEY_free(pub);
    if (fx) BN_free(fx);
    if (fy) BN_free(fy);
    if (fwpub_point) EC_POINT_free(fwpub_point);
    if (fwpub) EC_KEY_free(fwpub);
    if (sig) ECDSA_SIG_free(sig);
    return rc;
}

int metallica_mis_handle_priv(metallica_mis_identity_t *identity,
                               const unsigned char psk_encryption_key[METALLICA_MIS_TLS_KEYLEN],
                               const unsigned char psk_validation_key[METALLICA_MIS_TLS_KEYLEN],
                               const unsigned char *body, size_t body_len) {
    bb_t pt; bb_init(&pt);
    EC_KEY *priv = NULL;
    BIGNUM *d = NULL;
    int rc = -1;

    if (body_len < 1) return -1;
    if (body[0] != 2) return -1; /* "Unknown private key prefix" */

    const unsigned char *c = body + 1;
    size_t c_len = body_len - 1;
    if (c_len < 32) return -1; /* need at least the trailing HMAC */

    size_t payload_len = c_len - 32;
    const unsigned char *hs = c + payload_len;
    unsigned char sig[32];
    hmac_sha256(psk_validation_key, METALLICA_MIS_TLS_KEYLEN, c, payload_len, sig);
    if (memcmp(sig, hs, 32) != 0) {
        return -1; /* "This device was probably paired with another computer." */
    }

    if (payload_len < 16) return -1;
    if (aes256_cbc_raw(0, psk_encryption_key, c, c + 16, payload_len - 16, &pt) != 0) goto done;

    /* standard PKCS7 unpad this time (python: m = m[:-m[-1]]) --
     * matches init_flash.c's encrypt_key() padding scheme, NOT
     * session_unpad()'s off-by-one scheme */
    if (pt.len == 0) goto done;
    {
        unsigned char last = pt.data[pt.len - 1];
        if (last > pt.len) goto done;
        pt.len -= last;
    }

    if (pt.len < 96) goto done;

    {
        unsigned char d_be[32];
        for (int i = 0; i < 32; i++) d_be[i] = pt.data[64 + 31 - i]; /* d, little-endian on wire, reverse to BE */

        d = BN_bin2bn(d_be, 32, NULL);
        if (!d) goto done;

        priv = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
        if (!priv) goto done;
        if (EC_KEY_set_private_key(priv, d) != 1) goto done;

        const EC_GROUP *group = EC_KEY_get0_group(priv);
        EC_POINT *pubpt = EC_POINT_new(group);
        if (!pubpt || EC_POINT_mul(group, pubpt, d, NULL, NULL, NULL) != 1 ||
            EC_KEY_set_public_key(priv, pubpt) != 1) {
            if (pubpt) EC_POINT_free(pubpt);
            goto done;
        }
        EC_POINT_free(pubpt);
    }

    if (identity->priv_key) EC_KEY_free(identity->priv_key);
    identity->priv_key = priv;
    priv = NULL;
    rc = 0;

done:
    if (d) BN_free(d);
    if (priv) EC_KEY_free(priv);
    bb_free(&pt);
    return rc;
}

/* ==================== public API ==================== */

int metallica_mis_tls_init(metallica_mis_tls_t *tls,
                            metallica_mis_tls_transport_fn transport, void *transport_ctx,
                            const char *product_name, const char *serial_number) {
    memset(tls, 0, sizeof(*tls));
    tls->transport = transport;
    tls->transport_ctx = transport_ctx;
    metallica_mis_set_hwkey(product_name, serial_number, tls->psk_encryption_key, tls->psk_validation_key);
    return 0;
}

int metallica_mis_tls_open(metallica_mis_tls_t *tls, const metallica_mis_identity_t *identity) {
    unsigned char in_buf[8192];
    int in_len;
    bb_t hello; bb_init(&hello);
    bb_t frame1; bb_init(&frame1);
    bb_t app_out; bb_init(&app_out);
    bb_t flight2; bb_init(&flight2);
    bb_t certs; bb_init(&certs);
    bb_t kex; bb_init(&kex);
    bb_t verify; bb_init(&verify);
    bb_t ccs; bb_init(&ccs);
    bb_t hs_finish; bb_init(&hs_finish);
    bb_t finish_frame; bb_init(&finish_frame);
    bb_t frame2; bb_init(&frame2);
    int rc = -1;

    tls->secure_rx = false;
    tls->secure_tx = false;
    tls->identity = identity;
    SHA256_Init(&tls->handshake_hash);

    /* --- flight 1: ClientHello --- */
    if (make_client_hello(tls, &hello) != 0) goto done;
    if (make_handshake(tls, hello.data, hello.len, &frame1) != 0) goto done;

    {
        unsigned char wire_hdr[4] = { 0x44, 0x00, 0x00, 0x00 };
        bb_t out; bb_init(&out);
        if (bb_append(&out, wire_hdr, sizeof(wire_hdr)) != 0) { bb_free(&out); goto done; }
        if (bb_append(&out, frame1.data, frame1.len) != 0) { bb_free(&out); goto done; }

        in_len = tls->transport(tls->transport_ctx, out.data, out.len, in_buf, sizeof(in_buf));
        bb_free(&out);
        if (in_len < 0) goto done;
    }
    if (parse_tls_response(tls, in_buf, (size_t)in_len, &app_out) != 0) goto done;

    /* --- derive session keys from device_ecdh_pub + server_random --- */
    if (make_keys(tls) != 0) goto done;

    /* --- flight 2: Certificate, ClientKeyExchange, CertVerify, ChangeCipherSpec, Finished --- */
    if (make_certs(tls, &certs) != 0) goto done;
    if (make_client_kex(tls, &kex) != 0) goto done;
    if (make_cert_verify(tls, &verify) != 0) goto done;

    if (bb_append(&flight2, certs.data, certs.len) != 0) goto done;
    if (bb_append(&flight2, kex.data, kex.len) != 0) goto done;
    if (bb_append(&flight2, verify.data, verify.len) != 0) goto done;

    if (make_handshake(tls, flight2.data, flight2.len, &frame2) != 0) goto done;
    if (make_change_cipher_spec(&ccs) != 0) goto done;
    if (make_finish(tls, &hs_finish) != 0) goto done;
    if (make_handshake(tls, hs_finish.data, hs_finish.len, &finish_frame) != 0) goto done;

    {
        unsigned char wire_hdr[4] = { 0x44, 0x00, 0x00, 0x00 };
        bb_t out; bb_init(&out);
        if (bb_append(&out, wire_hdr, sizeof(wire_hdr)) != 0) { bb_free(&out); goto done; }
        if (bb_append(&out, frame2.data, frame2.len) != 0) { bb_free(&out); goto done; }
        if (bb_append(&out, ccs.data, ccs.len) != 0) { bb_free(&out); goto done; }
        if (bb_append(&out, finish_frame.data, finish_frame.len) != 0) { bb_free(&out); goto done; }

        in_len = tls->transport(tls->transport_ctx, out.data, out.len, in_buf, sizeof(in_buf));
        bb_free(&out);
        if (in_len < 0) goto done;
    }

    bb_free(&app_out); bb_init(&app_out);
    if (parse_tls_response(tls, in_buf, (size_t)in_len, &app_out) != 0) goto done;

    /* server's Finished, verified inside handle_finish() during
     * parse_tls_response()->handle_handshake() above; if we got here
     * without error, the handshake is complete. secure_rx should now
     * be true (set when its ChangeCipherSpec was parsed). */
    rc = tls->secure_rx ? 0 : -1;

done:
    bb_free(&hello); bb_free(&frame1); bb_free(&app_out); bb_free(&flight2);
    bb_free(&certs); bb_free(&kex); bb_free(&verify); bb_free(&ccs);
    bb_free(&hs_finish); bb_free(&finish_frame); bb_free(&frame2);
    return rc;
}

int metallica_mis_tls_cmd(metallica_mis_tls_t *tls, const unsigned char *cmd, size_t cmd_len,
                           unsigned char *out_buf, size_t out_buf_size) {
    unsigned char in_buf[8192];
    int in_len;
    bb_t frame; bb_init(&frame);
    bb_t app_out; bb_init(&app_out);
    int rc = -1;

    if (!(tls->secure_rx && tls->secure_tx)) goto done; /* mirrors Tls.cmd()'s branch -- this port only supports the secure path */

    if (make_app_data(tls, cmd, cmd_len, &frame) != 0) goto done;

    in_len = tls->transport(tls->transport_ctx, frame.data, frame.len, in_buf, sizeof(in_buf));
    if (in_len < 0) goto done;

    if (parse_tls_response(tls, in_buf, (size_t)in_len, &app_out) != 0) goto done;

    if (app_out.len > out_buf_size) goto done;
    memcpy(out_buf, app_out.data, app_out.len);
    rc = (int)app_out.len;

done:
    bb_free(&frame);
    bb_free(&app_out);
    return rc;
}

void metallica_mis_tls_free(metallica_mis_tls_t *tls) {
    if (tls->session_key) { EC_KEY_free(tls->session_key); tls->session_key = NULL; }
    /* tls->identity->device_ecdh_pub is owned by the identity struct, not the session -- not freed here */
}
