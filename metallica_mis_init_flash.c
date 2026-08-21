/*
 * metallica_mis_init_flash.c
 *
 * See metallica_mis_init_flash.h for full scope notes. Short version:
 * this covers the parts of python-validity's init_flash.py that don't
 * require an open TLS/ECDH session (metallica_mis_tls.c, not built
 * yet). partition_flash() and init_flash() itself are NOT here.
 *
 * Ported against python-validity source directly (validitysensor/
 * init_flash.py + validitysensor/tls.py's prf()/hs_key()/set_hwkey()),
 * pulled fresh on Aug 20 2026 during this session -- not from memory.
 *
 * NOT YET VERIFIED against real hardware. These are pure-function
 * ports (PRF, PSK derivation, AES/HMAC wrap, EC cert signing) with no
 * device I/O, so they can and should get unit-tested against known
 * python-validity output vectors before ever being wired into a real
 * pairing flow -- see tests/ dir, no metallica_mis test vectors exist
 * there yet, TODO add some (run the python reference against a fixed
 * product_name/serial and fixed EC keypair, capture the exact output
 * bytes, assert this C code matches byte-for-byte).
 */

#include <string.h>
#include <stdlib.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/ec.h>
#include <openssl/obj_mac.h>
#include <openssl/bn.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <openssl/ecdsa.h>

#include "metallica_mis_init_flash.h"
#include "metallica_mis_flash.h"        /* metallica_mis_flash_info_t, get/erase/write_flash, partition_flash()/init_flash() declarations */
#include "metallica_mis_blobs_9a.h"     /* metallica_mis_reset_blob_9a */
#include <stdio.h>

/* password_hardcoded / gwk_sign_hardcoded -- lifted verbatim from
 * python-validity's tls.py. These are NOT secrets specific to any one
 * sensor -- they're shared across the whole Prometheus/MIS chip
 * family, same spirit as the metallica_mis_init_hardcoded blobs in
 * metallica_mis_proto.h. */
static const unsigned char metallica_mis_password_hardcoded[] = { /* 32 B */
    0x71, 0x7c, 0xd7, 0x2d, 0x09, 0x62, 0xbc, 0x4a,
    0x28, 0x46, 0x13, 0x8d, 0xbb, 0x2c, 0x24, 0x19,
    0x25, 0x12, 0xa7, 0x64, 0x07, 0x06, 0x5f, 0x38,
    0x38, 0x46, 0x13, 0x9d, 0x4b, 0xec, 0x20, 0x33,
};

static const unsigned char metallica_mis_gwk_sign_hardcoded[] = { /* 32 B */
    0x3a, 0x4c, 0x76, 0xb7, 0x6a, 0x97, 0x98, 0x1d,
    0x12, 0x74, 0x24, 0x7e, 0x16, 0x66, 0x10, 0xe7,
    0x7f, 0x4d, 0x9c, 0x9d, 0x07, 0xd3, 0xc7, 0x28,
    0xe5, 0x32, 0x91, 0x6b, 0xdd, 0x28, 0xb4, 0x54,
};

/* ---- metallica_mis_prf() ------------------------------------------
 * RFC 5246 5.0 P_hash() construction, HMAC-SHA256 only (python-validity
 * never uses any other hash here). Port of tls.py's prf():
 *
 *   def prf(secret, seed, length):
 *       n = ceil(length / 32)
 *       a = HMAC(secret, seed)
 *       res = b''
 *       while n > 0:
 *           res += HMAC(secret, a + seed)
 *           a = HMAC(secret, a)
 *           n -= 1
 *       return res[:length]
 */
void metallica_mis_prf(const unsigned char *secret, size_t secret_len,
                        const unsigned char *seed, size_t seed_len,
                        unsigned char *out, size_t out_len) {
    unsigned char a[SHA256_DIGEST_LENGTH];
    unsigned int a_len = SHA256_DIGEST_LENGTH;
    unsigned char block[SHA256_DIGEST_LENGTH];
    unsigned int block_len = SHA256_DIGEST_LENGTH;
    unsigned char *concat_buf;
    size_t written = 0;

    HMAC(EVP_sha256(), secret, (int)secret_len, seed, seed_len, a, &a_len);

    concat_buf = malloc(a_len + seed_len);
    if (!concat_buf) return; /* caller-visible failure: out left short/garbage; TODO real error return */

    while (written < out_len) {
        memcpy(concat_buf, a, a_len);
        memcpy(concat_buf + a_len, seed, seed_len);
        HMAC(EVP_sha256(), secret, (int)secret_len, concat_buf, a_len + seed_len, block, &block_len);

        size_t take = (out_len - written < block_len) ? (out_len - written) : block_len;
        memcpy(out + written, block, take);
        written += take;

        /* a = HMAC(secret, a) for next round */
        HMAC(EVP_sha256(), secret, (int)secret_len, a, a_len, a, &a_len);
    }

    free(concat_buf);
}

/* ---- metallica_mis_hs_key() ----------------------------------------
 * Port of tls.py's hs_key():
 *   key  = password_hardcoded[:0x10]
 *   seed = password_hardcoded[0x10:] + b'\xaa\xaa'
 *   hs_key_bytes = prf(key, b'HS_KEY_PAIR_GEN' + seed, 0x20)
 *   return int(hs_key_bytes[::-1].hex(), 16)
 *
 * Python treats the 32-byte PRF output as little-endian and converts
 * to a big int. We skip the bigint step and just return the reversed
 * (big-endian) bytes -- BN_bin2bn() on that is equivalent to python's
 * int(). */
void metallica_mis_hs_key(unsigned char out[32]) {
    unsigned char key[16];
    unsigned char seed[16 + 2];
    unsigned char label_seed[15 + sizeof(seed)]; /* "HS_KEY_PAIR_GEN" is 15 bytes */
    unsigned char prf_out[32];
    static const unsigned char label[] = "HS_KEY_PAIR_GEN"; /* 15 chars, no implicit NUL used */

    memcpy(key, metallica_mis_password_hardcoded, 16);
    memcpy(seed, metallica_mis_password_hardcoded + 16, 16);
    seed[16] = 0xaa;
    seed[17] = 0xaa;

    memcpy(label_seed, label, 15);
    memcpy(label_seed + 15, seed, sizeof(seed));

    metallica_mis_prf(key, sizeof(key), label_seed, sizeof(label_seed), prf_out, sizeof(prf_out));

    /* reverse bytes: python's hs_key_bytes[::-1] */
    for (int i = 0; i < 32; i++) {
        out[i] = prf_out[31 - i];
    }
}

/* ---- metallica_mis_set_hwkey() -------------------------------------
 * Port of tls.py's Tls.set_hwkey():
 *   hw_key = product_name + '\0' + serial_number + '\0'
 *   psk_encryption_key = prf(password_hardcoded, b'GWK' + hw_key, 0x20)
 *   psk_validation_key = prf(psk_encryption_key, b'GWK_SIGN' + gwk_sign_hardcoded, 0x20)
 *
 * TODO(metallica-mis): see header -- product_name/serial should come
 * from IOKit (IOPlatformExpertDevice) on macOS. Caller passes them in
 * explicitly for now rather than this function reaching into IOKit
 * itself, so the pure-function/no-device-I/O property of this file
 * holds until that's actually decided. */
void metallica_mis_set_hwkey(const char *product_name, const char *serial_number,
                              unsigned char psk_encryption_key[METALLICA_MIS_PRF_KEYLEN],
                              unsigned char psk_validation_key[METALLICA_MIS_PRF_KEYLEN]) {
    size_t pn_len = strlen(product_name);
    size_t sn_len = strlen(serial_number);
    size_t hw_key_len = pn_len + 1 + sn_len + 1;
    unsigned char *hw_key = malloc(hw_key_len);
    if (!hw_key) return;

    memcpy(hw_key, product_name, pn_len);
    hw_key[pn_len] = '\0';
    memcpy(hw_key + pn_len + 1, serial_number, sn_len);
    hw_key[pn_len + 1 + sn_len] = '\0';

    /* seed1 = "GWK" + hw_key */
    size_t seed1_len = 3 + hw_key_len;
    unsigned char *seed1 = malloc(seed1_len);
    if (!seed1) { free(hw_key); return; }
    memcpy(seed1, "GWK", 3);
    memcpy(seed1 + 3, hw_key, hw_key_len);

    metallica_mis_prf(metallica_mis_password_hardcoded, sizeof(metallica_mis_password_hardcoded),
                       seed1, seed1_len, psk_encryption_key, METALLICA_MIS_PRF_KEYLEN);

    /* seed2 = "GWK_SIGN" + gwk_sign_hardcoded */
    unsigned char seed2[8 + sizeof(metallica_mis_gwk_sign_hardcoded)];
    memcpy(seed2, "GWK_SIGN", 8);
    memcpy(seed2 + 8, metallica_mis_gwk_sign_hardcoded, sizeof(metallica_mis_gwk_sign_hardcoded));

    metallica_mis_prf(psk_encryption_key, METALLICA_MIS_PRF_KEYLEN,
                       seed2, sizeof(seed2), psk_validation_key, METALLICA_MIS_PRF_KEYLEN);

    free(hw_key);
    free(seed1);
}

/* ---- metallica_mis_generate_client_keypair() ------------------------
 * Fresh SECP256R1 keypair, one per pairing. This is the step Mohammad
 * confirmed against python-validity: we generate this locally, we do
 * NOT read a pre-existing device secret. */
EC_KEY *metallica_mis_generate_client_keypair(void) {
    EC_KEY *key = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    if (!key) return NULL;
    if (EC_KEY_generate_key(key) != 1) {
        EC_KEY_free(key);
        return NULL;
    }
    return key;
}

/* helper: extract x, y (public point) and d (private scalar) as
 * 32-byte little-endian buffers, matching python-validity's
 * unhexlify('%064x' % n)[::-1] pattern used throughout init_flash.py. */
static int extract_xyd_le(const EC_KEY *key, unsigned char x_le[32],
                           unsigned char y_le[32], unsigned char d_le[32]) {
    const EC_GROUP *group = EC_KEY_get0_group(key);
    const EC_POINT *pub = EC_KEY_get0_public_key(key);
    const BIGNUM *priv = EC_KEY_get0_private_key(key);
    BIGNUM *x = BN_new(), *y = BN_new();
    unsigned char x_be[32], y_be[32], d_be[32];
    int ok = 0;

    if (!x || !y || !priv) goto done;

    if (EC_POINT_get_affine_coordinates(group, pub, x, y, NULL) != 1) goto done;

    /* BN_bn2binpad zero-pads to exactly 32 bytes, big-endian */
    if (BN_bn2binpad(x, x_be, 32) < 0) goto done;
    if (BN_bn2binpad(y, y_be, 32) < 0) goto done;
    if (BN_bn2binpad(priv, d_be, 32) < 0) goto done;

    for (int i = 0; i < 32; i++) {
        x_le[i] = x_be[31 - i];
        y_le[i] = y_be[31 - i];
        d_le[i] = d_be[31 - i];
    }
    ok = 1;

done:
    if (x) BN_free(x);
    if (y) BN_free(y);
    return ok ? 0 : -1;
}

/* ---- metallica_mis_encrypt_key() ------------------------------------
 * Port of init_flash.py's encrypt_key():
 *   x = client_public.x as 32B little-endian
 *   y = client_public.y as 32B little-endian
 *   d = client_private   as 32B little-endian
 *   m = x + y + d                     (96 bytes)
 *   m = PKCS7-pad(m, 16)              (96 is already a multiple of 16,
 *                                       so this always adds a full
 *                                       16-byte pad block of 0x10s --
 *                                       matches python's behavior
 *                                       exactly since l = 16 - (96%16) = 16)
 *   iv = random 16 bytes
 *   c = iv + AES-256-CBC(psk_encryption_key, iv).encrypt(m)
 *   sig = HMAC-SHA256(psk_validation_key, c)
 *
 * NOTE: psk_encryption_key is METALLICA_MIS_PRF_KEYLEN (32) bytes, so
 * this MUST be AES-256-CBC, not AES-128-CBC -- a 32-byte key handed to
 * EVP_aes_128_cbc() is a real bug (either an OpenSSL error or a silent
 * truncation to the first 16 bytes depending on version/build), not a
 * cosmetic mismatch. Confirmed against tls.py's set_hwkey(), which
 * derives psk_encryption_key via the same 32-byte PRF as everything
 * else in this file.
 *   return b'\x02' + c + sig
 */
int metallica_mis_encrypt_key(const EC_KEY *client_keypair,
                               const unsigned char psk_encryption_key[METALLICA_MIS_PRF_KEYLEN],
                               const unsigned char psk_validation_key[METALLICA_MIS_PRF_KEYLEN],
                               unsigned char **out, size_t *out_len) {
    unsigned char x_le[32], y_le[32], d_le[32];
    unsigned char m[96 + 16]; /* 96 raw + always exactly one 16B pad block */
    unsigned char iv[16];
    unsigned char ciphertext[sizeof(m)];
    int clen = 0, clen2 = 0;
    EVP_CIPHER_CTX *ctx = NULL;
    unsigned char *result = NULL;
    unsigned int sig_len = 32;
    int rc = -1;

    if (extract_xyd_le(client_keypair, x_le, y_le, d_le) != 0) return -1;

    memcpy(m, x_le, 32);
    memcpy(m + 32, y_le, 32);
    memcpy(m + 64, d_le, 32);
    /* PKCS7 pad: 96 % 16 == 0, so l = 16, append 16 bytes of 0x10 */
    memset(m + 96, 0x10, 16);

    if (RAND_bytes(iv, sizeof(iv)) != 1) return -1;

    ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;
    EVP_CIPHER_CTX_set_padding(ctx, 0); /* we already padded manually, matching python */

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, psk_encryption_key, iv) != 1) goto cleanup;
    if (EVP_EncryptUpdate(ctx, ciphertext, &clen, m, sizeof(m)) != 1) goto cleanup;
    if (EVP_EncryptFinal_ex(ctx, ciphertext + clen, &clen2) != 1) goto cleanup;
    clen += clen2;

    {
        /* c = iv + ciphertext */
        size_t c_len = sizeof(iv) + (size_t)clen;
        unsigned char *c = malloc(c_len);
        if (!c) goto cleanup;
        memcpy(c, iv, sizeof(iv));
        memcpy(c + sizeof(iv), ciphertext, (size_t)clen);

        unsigned char sig[32];
        HMAC(EVP_sha256(), psk_validation_key, METALLICA_MIS_PRF_KEYLEN, c, c_len, sig, &sig_len);

        /* result = 0x02 + c + sig */
        size_t result_len = 1 + c_len + sig_len;
        result = malloc(result_len);
        if (!result) { free(c); goto cleanup; }
        result[0] = 0x02;
        memcpy(result + 1, c, c_len);
        memcpy(result + 1 + c_len, sig, sig_len);
        free(c);

        *out = result;
        *out_len = result_len;
        rc = 0;
    }

cleanup:
    if (ctx) EVP_CIPHER_CTX_free(ctx);
    return rc;
}

/* ---- metallica_mis_make_cert() --------------------------------------
 * Port of init_flash.py's make_cert():
 *   msg = pack('<LL', 0x17, 0x20) + x(32B LE) + 0x24 zero bytes +
 *         y(32B LE) + 0x4c zero bytes
 *   pk  = derive_private_key(hs_key(), SECP256R1)
 *   s   = pk.sign(msg, ECDSA(SHA256))            -- DER-encoded sig
 *   s   = pack('<L', len(s)) + s
 *   msg = msg + s
 *   msg += zero-pad to 444 bytes total
 *
 * NOTE msg length before signature: 8 + 32 + 0x24(36) + 32 + 0x4c(76)
 *    = 8 + 32 + 36 + 32 + 76 = 184 bytes. Then + 4-byte sig-len prefix
 * + DER sig (variable, typically ~70-72 bytes for P-256) + zero-pad
 * to exactly 444. If the signature is ever long enough to blow past
 * 444, that's a hard error -- matches python-validity's own
 * "FIXME not sure this math is right" comment on this exact line, so
 * this is flagged rather than silently truncated. */
int metallica_mis_make_cert(const EC_KEY *client_keypair, unsigned char out[444]) {
    unsigned char x_le[32], y_le[32], d_le_unused[32];
    unsigned char msg[184];
    unsigned char hs_priv_be[32];
    EC_KEY *hs_key = NULL;
    BIGNUM *priv_bn = NULL;
    ECDSA_SIG *sig = NULL;
    unsigned char *der = NULL;
    int der_len = 0;
    int rc = -1;

    if (extract_xyd_le(client_keypair, x_le, y_le, d_le_unused) != 0) return -1;

    memset(msg, 0, sizeof(msg));
    /* pack('<LL', 0x17, 0x20) */
    msg[0] = 0x17; msg[1] = 0x00; msg[2] = 0x00; msg[3] = 0x00;
    msg[4] = 0x20; msg[5] = 0x00; msg[6] = 0x00; msg[7] = 0x00;
    memcpy(msg + 8, x_le, 32);
    /* msg[40 .. 40+36) already zeroed */
    memcpy(msg + 8 + 32 + 36, y_le, 32);
    /* remaining 76 bytes already zeroed -- total = 8+32+36+32+76 = 184 */

    /* hs_key() gives 32 big-endian bytes usable directly as the
     * private scalar (see metallica_mis_hs_key() doc comment). */
    metallica_mis_hs_key(hs_priv_be);

    hs_key = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    if (!hs_key) goto cleanup;
    priv_bn = BN_bin2bn(hs_priv_be, 32, NULL);
    if (!priv_bn) goto cleanup;
    if (EC_KEY_set_private_key(hs_key, priv_bn) != 1) goto cleanup;
    /* derive + set public key from the private scalar so ECDSA_do_sign has a complete key */
    {
        const EC_GROUP *group = EC_KEY_get0_group(hs_key);
        EC_POINT *pub = EC_POINT_new(group);
        if (!pub || EC_POINT_mul(group, pub, priv_bn, NULL, NULL, NULL) != 1 ||
            EC_KEY_set_public_key(hs_key, pub) != 1) {
            if (pub) EC_POINT_free(pub);
            goto cleanup;
        }
        EC_POINT_free(pub);
    }

    {
        unsigned char digest[SHA256_DIGEST_LENGTH];
        SHA256(msg, sizeof(msg), digest);
        sig = ECDSA_do_sign(digest, sizeof(digest), hs_key);
        if (!sig) goto cleanup;
        der_len = i2d_ECDSA_SIG(sig, &der);
        if (der_len <= 0) goto cleanup;
    }

    if ((size_t)(4 + der_len) + sizeof(msg) > 444) {
        /* matches python's own flagged uncertainty about this math --
         * if this ever fires, the 444-byte assumption from
         * python-validity needs re-checking, not silently patched over */
        goto cleanup;
    }

    memset(out, 0, 444);
    memcpy(out, msg, sizeof(msg));
    out[sizeof(msg) + 0] = (unsigned char)(der_len & 0xff);
    out[sizeof(msg) + 1] = (unsigned char)((der_len >> 8) & 0xff);
    out[sizeof(msg) + 2] = (unsigned char)((der_len >> 16) & 0xff);
    out[sizeof(msg) + 3] = (unsigned char)((der_len >> 24) & 0xff);
    memcpy(out + sizeof(msg) + 4, der, (size_t)der_len);
    /* remaining bytes to 444 stay zero from the memset above */

    rc = 0;

cleanup:
    if (der) OPENSSL_free(der);
    if (sig) ECDSA_SIG_free(sig);
    if (priv_bn) BN_free(priv_bn);
    if (hs_key) EC_KEY_free(hs_key);
    return rc;
}

/* ---- serialize_flash_params() / serialize_partition() --------------
 * Pure struct packing, ported from init_flash.py:
 *   serialize_flash_params: pack('<LLxxBx', ic.size, ic.secror_size, ic.sector_erase_cmd)
 *     -> 4B size + 4B sector_size + 2B pad + 1B cmd + 1B pad = 12 bytes
 *   serialize_partition: pack('<BBHLL', id, type, access_lvl, offset, size)
 *     -> 1+1+2+4+4 = 12 bytes, then + 4 zero bytes + SHA256(those 12 bytes)
 */
size_t metallica_mis_serialize_flash_params(uint32_t ic_size, uint32_t sector_size,
                                             uint8_t sector_erase_cmd,
                                             unsigned char out[12]) {
    memset(out, 0, 12);
    out[0] = (unsigned char)(ic_size & 0xff);
    out[1] = (unsigned char)((ic_size >> 8) & 0xff);
    out[2] = (unsigned char)((ic_size >> 16) & 0xff);
    out[3] = (unsigned char)((ic_size >> 24) & 0xff);
    out[4] = (unsigned char)(sector_size & 0xff);
    out[5] = (unsigned char)((sector_size >> 8) & 0xff);
    out[6] = (unsigned char)((sector_size >> 16) & 0xff);
    out[7] = (unsigned char)((sector_size >> 24) & 0xff);
    /* out[8], out[9] = pad (xx) */
    out[10] = sector_erase_cmd;
    /* out[11] = pad (x) */
    return 12;
}

size_t metallica_mis_serialize_partition(const metallica_mis_partition_info_t *p,
                                          unsigned char out[48]) {
    unsigned char hdr[12];

    hdr[0] = p->id;
    hdr[1] = p->type;
    hdr[2] = (unsigned char)(p->access_lvl & 0xff);
    hdr[3] = (unsigned char)((p->access_lvl >> 8) & 0xff);
    hdr[4] = (unsigned char)(p->offset & 0xff);
    hdr[5] = (unsigned char)((p->offset >> 8) & 0xff);
    hdr[6] = (unsigned char)((p->offset >> 16) & 0xff);
    hdr[7] = (unsigned char)((p->offset >> 24) & 0xff);
    hdr[8] = (unsigned char)(p->size & 0xff);
    hdr[9] = (unsigned char)((p->size >> 8) & 0xff);
    hdr[10] = (unsigned char)((p->size >> 16) & 0xff);
    hdr[11] = (unsigned char)((p->size >> 24) & 0xff);

    memcpy(out, hdr, 12);
    memset(out + 12, 0, 4);
    SHA256(hdr, 12, out + 16);

    return 48;
}


/* ==================== hardcoded partition layouts ==================== */
/* Ported verbatim from init_flash.py's flash_layout_hardcoded[_0090]
 * module-level lists. Field order: id, type, access_lvl, offset,
 * size -- matches metallica_mis_partition_info_t exactly. */
static const metallica_mis_partition_info_t metallica_mis_flash_layout_hardcoded[] = {
    /* id  type  access_lvl  offset       size */
    {  1,   4,   7,          0x00001000, 0x00001000 }, /* cert store */
    {  2,   1,   2,          0x00002000, 0x0003e000 }, /* xpfwext */
    {  5,   5,   3,          0x00040000, 0x00008000 }, /* ??? (unknown in python source too) */
    {  6,   6,   3,          0x00048000, 0x00008000 }, /* calibration data */
    {  4,   3,   5,          0x00050000, 0x00080000 }, /* template database */
};
#define METALLICA_MIS_FLASH_LAYOUT_HARDCODED_LEN \
    (sizeof(metallica_mis_flash_layout_hardcoded) / sizeof(metallica_mis_flash_layout_hardcoded[0]))

static const metallica_mis_partition_info_t metallica_mis_flash_layout_hardcoded_0090[] = {
    /* id  type  access_lvl  offset       size */
    {  1,   4,   7,          0x00001000, 0x00001000 }, /* cert store */
    {  2,   1,   2,          0x00002000, 0x0003e000 }, /* xpfwext */
    {  5,   5,   3,          0x00040000, 0x00008000 }, /* ??? */
    {  6,   6,   3,          0x00048000, 0x00008000 }, /* calibration data */
    {  4,   3,   5,          0x00050000, 0x00030000 }, /* template database -- smaller than the default variant */
};
#define METALLICA_MIS_FLASH_LAYOUT_HARDCODED_0090_LEN \
    (sizeof(metallica_mis_flash_layout_hardcoded_0090) / sizeof(metallica_mis_flash_layout_hardcoded_0090[0]))

static const unsigned char metallica_mis_partition_signature[] = { /* 256 B */
    0x1d, 0xb0, 0x2a, 0x88, 0x6b, 0x00, 0x7e, 0x2b, 0x47, 0x26, 0x3b, 0xb8,
    0xfe, 0x30, 0xbd, 0x64, 0xa1, 0xf5, 0x8b, 0xea, 0x7b, 0x25, 0xf1, 0xe1,
    0xba, 0x9a, 0xe0, 0x9a, 0xdd, 0x7e, 0xcf, 0xf3, 0x63, 0x33, 0xf8, 0x19,
    0x83, 0x39, 0xcd, 0xd7, 0x13, 0xf0, 0x43, 0x63, 0x37, 0x10, 0xa1, 0x7b,
    0xc7, 0xb3, 0xf4, 0x18, 0xf1, 0xd8, 0xff, 0x43, 0x5a, 0x1b, 0xf4, 0x7f,
    0x06, 0x5d, 0xff, 0xca, 0x72, 0x71, 0x09, 0x15, 0x22, 0x17, 0xfc, 0xe7,
    0x3b, 0xf2, 0xbf, 0x8e, 0x01, 0xa1, 0x64, 0x1f, 0x6a, 0x24, 0xb0, 0xc4,
    0x92, 0xa6, 0xa3, 0xf1, 0x01, 0x14, 0x05, 0x72, 0x75, 0x84, 0x68, 0x42,
    0xb1, 0xc8, 0xb6, 0x6b, 0xd6, 0x70, 0x07, 0x38, 0x52, 0x4d, 0x44, 0x71,
    0xbc, 0xa3, 0x31, 0x5b, 0xa2, 0x3b, 0xb8, 0x32, 0x74, 0x32, 0x20, 0xad,
    0x19, 0x5b, 0x60, 0x55, 0x8a, 0xa7, 0x9a, 0x3e, 0xde, 0xb2, 0x60, 0x48,
    0x34, 0xe2, 0xbb, 0x62, 0xe8, 0x90, 0xb0, 0xce, 0x40, 0x5b, 0x3b, 0x8e,
    0xf2, 0xfe, 0xc2, 0xaa, 0xb3, 0xe2, 0x2b, 0xff, 0x23, 0xf8, 0x9a, 0x58,
    0xff, 0x0d, 0xc0, 0x15, 0xfe, 0xce, 0x5d, 0x3e, 0xd3, 0xf5, 0x49, 0x6a,
    0xce, 0x87, 0x9a, 0x92, 0x98, 0x0a, 0xec, 0x9d, 0x85, 0xeb, 0x7e, 0x9d,
    0xf2, 0x45, 0xea, 0xe0, 0x3a, 0x41, 0xac, 0xfd, 0x4e, 0x7d, 0x1c, 0xb1,
    0xdb, 0xd0, 0xdf, 0x42, 0xd5, 0x34, 0x90, 0x4d, 0xe0, 0x0b, 0x63, 0x89,
    0xf6, 0x88, 0x67, 0x64, 0x6e, 0x9d, 0x7c, 0x3d, 0x0b, 0x1d, 0xff, 0xd7,
    0x40, 0x70, 0xb2, 0xd0, 0xf2, 0x04, 0x9b, 0x9f, 0x1d, 0xc7, 0xb0, 0xc9,
    0x65, 0x1c, 0x59, 0xbe, 0x3e, 0xa8, 0x91, 0x67, 0x47, 0x25, 0xe1, 0xf2,
    0xf7, 0xa4, 0x84, 0xa9, 0x41, 0x61, 0x5b, 0x80, 0x21, 0x11, 0x05, 0x97,
    0x83, 0x69, 0xcf, 0x71,
};

static const unsigned char metallica_mis_partition_signature_0090[] = { /* 256 B */
    0xe4, 0x4f, 0x7a, 0x80, 0xd6, 0x13, 0x77, 0x94, 0xd3, 0x30, 0xb5, 0xd0,
    0x26, 0xc3, 0x28, 0xa7, 0x3c, 0x90, 0x7f, 0x3f, 0x65, 0x3d, 0x41, 0x12,
    0x55, 0xb7, 0xc2, 0xf8, 0xb4, 0x25, 0xd8, 0x70, 0xa8, 0xa5, 0x3c, 0x66,
    0x30, 0xca, 0x86, 0x4b, 0x84, 0x59, 0x0e, 0x3c, 0x67, 0x86, 0xf0, 0xd6,
    0x9b, 0xe4, 0xbb, 0xab, 0x57, 0x36, 0x38, 0x8f, 0x85, 0x27, 0x23, 0x7a,
    0x0a, 0x86, 0xbb, 0xce, 0x7c, 0xed, 0x94, 0x50, 0xc4, 0x96, 0x47, 0x09,
    0xe8, 0x9a, 0xc5, 0x35, 0xaa, 0x00, 0x78, 0x71, 0x58, 0xe0, 0xa8, 0xd9,
    0xb1, 0xfb, 0x75, 0xf0, 0xf7, 0xae, 0x53, 0xd4, 0xbd, 0x11, 0xab, 0xfc,
    0xf5, 0xee, 0x67, 0xa5, 0xa7, 0x1e, 0x24, 0x8a, 0x42, 0x6b, 0x3a, 0xff,
    0x45, 0x67, 0x04, 0x8f, 0xa9, 0x3d, 0xe6, 0x59, 0x39, 0xcc, 0xfb, 0xe3,
    0xf3, 0x11, 0x49, 0xa8, 0x2c, 0x64, 0xfb, 0xfd, 0x6a, 0x2a, 0x6c, 0xf7,
    0x48, 0xe1, 0xd9, 0xbd, 0x85, 0x62, 0xcf, 0x39, 0xb1, 0xa4, 0xb3, 0x07,
    0xb3, 0x7b, 0xe2, 0x23, 0x31, 0x7b, 0x1b, 0x81, 0x7e, 0x36, 0x4f, 0x28,
    0x77, 0xd2, 0x9d, 0x12, 0x37, 0x31, 0x31, 0x4a, 0xa6, 0x27, 0xcb, 0xf2,
    0x34, 0xe0, 0xea, 0x69, 0xa4, 0x06, 0xa4, 0x73, 0x5a, 0x03, 0xa4, 0x54,
    0x95, 0x02, 0x3e, 0xf7, 0x06, 0xbd, 0xb5, 0x42, 0xc9, 0x49, 0xd2, 0x43,
    0xac, 0x2c, 0x08, 0xc0, 0x0a, 0xbf, 0x43, 0xfa, 0xa5, 0x52, 0x8a, 0x0a,
    0x8e, 0x49, 0xb0, 0x2c, 0x50, 0x7b, 0x01, 0xb6, 0xf1, 0xc9, 0xab, 0xff,
    0xc6, 0x69, 0xd8, 0xc8, 0x4d, 0x7e, 0x4a, 0x71, 0x4d, 0xa3, 0x2a, 0xad,
    0xe7, 0x92, 0x8e, 0xca, 0x96, 0x98, 0xb8, 0x2b, 0xee, 0x6b, 0x72, 0xc6,
    0x42, 0xc9, 0xad, 0xd8, 0x0b, 0xbd, 0x7c, 0xcc, 0x41, 0x21, 0xb8, 0x02,
    0x20, 0xd5, 0x2b, 0x8a,
};


/* ==================== minimal local buffer builder ==================== */
/* Small malloc/realloc-based growable buffer, scoped to this file
 * only -- metallica_mis_tls.c has its own equivalent (bb_t) but it's
 * file-static there, so this is a deliberate small duplication rather
 * than exposing tls.c internals across a translation unit boundary
 * for one helper. */
typedef struct { unsigned char *data; size_t len, cap; } local_bb_t;

static void local_bb_init(local_bb_t *b) { b->data = NULL; b->len = 0; b->cap = 0; }
static void local_bb_free(local_bb_t *b) { free(b->data); b->data = NULL; b->len = b->cap = 0; }

static int local_bb_append(local_bb_t *b, const unsigned char *p, size_t n) {
    if (b->len + n > b->cap) {
        size_t newcap = b->cap ? b->cap * 2 : 256;
        while (newcap < b->len + n) newcap *= 2;
        unsigned char *nd = realloc(b->data, newcap);
        if (!nd) return -1;
        b->data = nd;
        b->cap = newcap;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
    return 0;
}

/* with_hdr() -- pack('<HH', id, len(buf)) + buf, per init_flash.py's
 * with_hdr(). NOTE this is a plain 4-byte header, NOT the same as
 * make_tls_flash_block() in tls.c (which also includes a SHA-256 of
 * the body) -- these are two different wire formats used in two
 * different places, not the same helper reused. */
static int with_hdr(local_bb_t *out, uint16_t id, const unsigned char *body, size_t body_len) {
    unsigned char hdr[4];
    hdr[0] = (unsigned char)(id & 0xff);
    hdr[1] = (unsigned char)((id >> 8) & 0xff);
    hdr[2] = (unsigned char)(body_len & 0xff);
    hdr[3] = (unsigned char)((body_len >> 8) & 0xff);
    if (local_bb_append(out, hdr, sizeof(hdr)) != 0) return -1;
    if (body_len > 0 && local_bb_append(out, body, body_len) != 0) return -1;
    return 0;
}

/* ==================== partition_flash() ==================== */

int metallica_mis_partition_flash(metallica_mis_tls_t *tls, metallica_mis_identity_t *identity,
                                   const metallica_mis_flash_info_t *info,
                                   const metallica_mis_partition_info_t *layout, size_t layout_count,
                                   const unsigned char *signature, size_t signature_len,
                                   const EC_KEY *client_keypair) {
    local_bb_t cmd; local_bb_init(&cmd);
    local_bb_t partitions_blob; local_bb_init(&partitions_blob);
    unsigned char flash_params[12];
    unsigned char cert[444];
    unsigned char *rsp = NULL;
    size_t rsp_cap = 8192;
    int n;
    int rc = -1;

    fprintf(stderr, "metallica_mis: Detected Flash IC: %s, %u bytes\n",
            info->ic->name, (unsigned)info->ic->size);

    /* cmd = unhex('4f 0000 0000') -- opcode 0x4f + 4 reserved bytes */
    {
        unsigned char op[5] = { 0x4f, 0x00, 0x00, 0x00, 0x00 };
        if (local_bb_append(&cmd, op, sizeof(op)) != 0) goto done;
    }

    /* block 0: serialize_flash_params(info.ic) */
    metallica_mis_serialize_flash_params(info->ic->size, info->ic->sector_size,
                                          info->ic->sector_erase_cmd, flash_params);
    if (with_hdr(&cmd, 0, flash_params, sizeof(flash_params)) != 0) goto done;

    /* block 1: join(serialize_partition(p) for p in layout) + signature */
    for (size_t i = 0; i < layout_count; i++) {
        unsigned char part_buf[48];
        metallica_mis_serialize_partition(&layout[i], part_buf);
        if (local_bb_append(&partitions_blob, part_buf, sizeof(part_buf)) != 0) goto done;
    }
    if (local_bb_append(&partitions_blob, signature, signature_len) != 0) goto done;
    if (with_hdr(&cmd, 1, partitions_blob.data, partitions_blob.len) != 0) goto done;

    /* block 5: make_cert(client_public) */
    if (metallica_mis_make_cert(client_keypair, cert) != 0) goto done;
    if (with_hdr(&cmd, 5, cert, sizeof(cert)) != 0) goto done;

    /* block 3: crt_hardcoded (the hardcoded firmware CA cert, defined in metallica_mis_tls.c) */
    if (with_hdr(&cmd, 3, metallica_mis_crt_hardcoded, metallica_mis_crt_hardcoded_len) != 0) goto done;

    rsp = malloc(rsp_cap);
    if (!rsp) goto done;
    n = metallica_mis_tls_cmd(tls, cmd.data, cmd.len, rsp, rsp_cap);
    if (n < 2) goto done; /* assert_status()-equivalent -- too short to even hold a status word */

    {
        unsigned short status = (unsigned short)rsp[0] | ((unsigned short)rsp[1] << 8);
        if (status != 0) goto done;
    }

    /* rsp = rsp[2:]; crt_len = unpack('<L', rsp[:4]); rsp = rsp[4:] */
    if ((size_t)n < 2 + 4) goto done;
    const unsigned char *p = rsp + 2;
    size_t remaining = (size_t)n - 2;
    uint32_t crt_len = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    p += 4; remaining -= 4;

    if (crt_len > remaining) goto done;
    if (metallica_mis_handle_cert(identity, p, crt_len) != 0) goto done;
    /* rsp = rsp[crt_len:] -- the remaining bytes are intentionally
     * discarded here, matching python's own `# TODO - figure out
     * what the rest of rsp means` -- python doesn't use them either,
     * this isn't a shortcut this port introduced on its own. */

    rc = 0;

done:
    local_bb_free(&cmd);
    local_bb_free(&partitions_blob);
    free(rsp);
    return rc;
}


/* ==================== init_flash() ==================== */

int metallica_mis_init_flash(metallica_mis_tls_t *tls, metallica_mis_identity_t *identity,
                              const char *product_name, const char *serial_number,
                              uint16_t usb_vid, uint16_t usb_pid) {
    metallica_mis_flash_info_t info;
    EC_KEY *client_keypair = NULL;
    unsigned char psk_encryption_key[METALLICA_MIS_PRF_KEYLEN];
    unsigned char psk_validation_key[METALLICA_MIS_PRF_KEYLEN];
    unsigned char *encrypted_key = NULL;
    size_t encrypted_key_len = 0;
    unsigned char tls_flash[0x1000];
    unsigned char rsp[8192];
    int n;
    int rc = -1;
    int have_flash_info = 0;

    /* Step 1: get_flash_info() -- if partitions already exist, this
     * device is already paired. Matches python's early return. */
    if (metallica_mis_get_flash_info(tls, &info) != 0) goto done;
    have_flash_info = 1;

    if (info.partition_count > 0) {
        fprintf(stderr, "metallica_mis: Flash has %zu partitions.\n", info.partition_count);
        rc = 0;
        goto done;
    }
    fprintf(stderr, "metallica_mis: Flash was not initialized yet. Formatting...\n");

    /* Step 2: send reset_blob. python uses usb.cmd(reset_blob)
     * directly here (not tls.cmd()) -- but at this point in pairing
     * secure_rx/secure_tx are both still false, so tls.cmd() would
     * take the exact same plaintext-passthrough branch anyway. Using
     * metallica_mis_tls_cmd() here is behaviorally identical, not a
     * deviation from python. */
    n = metallica_mis_tls_cmd(tls, metallica_mis_reset_blob_9a, metallica_mis_reset_blob_9a_len,
                               rsp, sizeof(rsp));
    if (n < 2) goto done;
    {
        unsigned short status = (unsigned short)rsp[0] | ((unsigned short)rsp[1] << 8);
        if (status != 0) goto done;
    }

    /* Step 3: generate a fresh SECP256R1 keypair locally -- the
     * actual pairing secret, not read from the device. */
    client_keypair = metallica_mis_generate_client_keypair();
    if (!client_keypair) goto done;

    /* Step 4: select layout/signature by VID:PID. NOTE (see header
     * doc comment): the 0090 branch selects the right TABLES, but
     * this codebase only has 009a's reset_blob/db_write_enable --
     * calling this with 0090's VID:PID would still send 009a's blobs
     * above via the hardcoded metallica_mis_reset_blob_9a reference.
     * Not fixed here since there's no 0090 hardware to test against;
     * flagged rather than silently wrong. */
    const metallica_mis_partition_info_t *layout = metallica_mis_flash_layout_hardcoded;
    size_t layout_count = METALLICA_MIS_FLASH_LAYOUT_HARDCODED_LEN;
    const unsigned char *signature = metallica_mis_partition_signature;
    size_t signature_len = sizeof(metallica_mis_partition_signature);

    if (usb_vid == 0x138a && usb_pid == 0x0090) {
        layout = metallica_mis_flash_layout_hardcoded_0090;
        layout_count = METALLICA_MIS_FLASH_LAYOUT_HARDCODED_0090_LEN;
        signature = metallica_mis_partition_signature_0090;
        signature_len = sizeof(metallica_mis_partition_signature_0090);
    }

    /* Step 5: partition_flash() -- writes the partition table +
     * signature + our fresh cert + the hardcoded firmware CA cert,
     * and stores the device's returned cert into identity. */
    if (metallica_mis_partition_flash(tls, identity, &info, layout, layout_count,
                                       signature, signature_len, client_keypair) != 0) {
        goto done;
    }

    /* RomInfo.get() intentionally skipped here -- see header doc
     * comment: python's own code doesn't use the result either yet. */

    /* Step 6: read device's ECDH pubkey via cmd 0x50, wrapped in a
     * try/finally in python that ALWAYS calls call_cleanups()
     * afterward regardless of success/failure -- ported the same
     * way: cleanup always runs, but a failed read/status still fails
     * this function overall. */
    n = metallica_mis_tls_cmd(tls, (const unsigned char[]){ 0x50 }, 1, rsp, sizeof(rsp));
    int ecdh_read_ok = (n >= 2);
    if (ecdh_read_ok) {
        unsigned short status = (unsigned short)rsp[0] | ((unsigned short)rsp[1] << 8);
        ecdh_read_ok = (status == 0);
    }
    int cleanup_ok = (metallica_mis_flash_call_cleanups(tls) == 0);

    if (!ecdh_read_ok) goto done;
    if (!cleanup_ok) goto done;

    /* rsp = rsp[2:]; l = unpack('<L', rsp[:4]); zeroes, rsp = rsp[4:-400], rsp[-400:] */
    if ((size_t)n < 2 + 4) goto done;
    {
        const unsigned char *p = rsp + 2;
        size_t remaining = (size_t)n - 2;
        uint32_t l = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        if (l != remaining) goto done; /* "Length mismatch" */

        p += 4; remaining -= 4;
        if (remaining < 400) goto done;
        size_t zeroes_len = remaining - 400;
        const unsigned char *zeroes = p;
        const unsigned char *ecdh_body = p + zeroes_len;

        for (size_t i = 0; i < zeroes_len; i++) {
            if (zeroes[i] != 0) goto done; /* "Expected zeroes" */
        }

        /* Step 6 cont: handle_ecdh() */
        if (metallica_mis_handle_ecdh(identity, ecdh_body, 400) != 0) goto done;
    }

    /* Step 7: derive PSK pair (same derivation tls_init() does, but
     * needed standalone here since handle_priv() takes it directly
     * rather than reaching into a tls_t). */
    metallica_mis_set_hwkey(product_name, serial_number, psk_encryption_key, psk_validation_key);

    if (metallica_mis_encrypt_key(client_keypair, psk_encryption_key, psk_validation_key,
                                   &encrypted_key, &encrypted_key_len) != 0) {
        goto done;
    }
    if (metallica_mis_handle_priv(identity, psk_encryption_key, psk_validation_key,
                                   encrypted_key, encrypted_key_len) != 0) {
        goto done;
    }

    /* Step 8: the actual first real handshake. */
    if (metallica_mis_tls_open(tls, identity) != 0) goto done;

    /* Step 9: wipe newly created partitions, exact order from python. */
    if (metallica_mis_erase_flash(tls, 1) != 0) goto done;
    if (metallica_mis_erase_flash(tls, 2) != 0) goto done;
    if (metallica_mis_erase_flash(tls, 5) != 0) goto done;
    if (metallica_mis_erase_flash(tls, 6) != 0) goto done;
    if (metallica_mis_erase_flash(tls, 4) != 0) goto done;

    /* Step 10: persist paired identity to the cert partition. */
    if (metallica_mis_make_tls_flash(identity, tls_flash) != 0) goto done;
    if (metallica_mis_write_flash(tls, 1, 0, tls_flash, sizeof(tls_flash)) != 0) goto done;

    /* Step 11 (reboot) is the caller's responsibility -- see header
     * doc comment. */

    rc = 0;

done:
    if (client_keypair) EC_KEY_free(client_keypair);
    free(encrypted_key);
    if (have_flash_info) metallica_mis_flash_info_free(&info);
    return rc;
}
