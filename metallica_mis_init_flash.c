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
