#ifndef __METALLICA_MIS_TLS_H
#define __METALLICA_MIS_TLS_H

/*
 * metallica_mis_tls.h
 *
 * C port of python-validity's validitysensor/tls.py -- the custom,
 * TLS-1.2-*shaped* (but not actually interoperable with a real TLS
 * stack) handshake this sensor family uses to establish a session
 * cipher after the plaintext bootstrap stage (send_init(), already
 * built in metallica_mis_daemon.c) and before firmware upload,
 * calibration, or capture can happen.
 *
 * Ported directly against python-validity source pulled Aug 20 2026.
 * "TLS-shaped" is doing a lot of work in that description: this reuses
 * TLS 1.2 record-layer framing (type/version/length headers, a
 * ClientHello/ServerHello/Certificate/ClientKeyExchange/CertVerify/
 * Finished handshake) and HMAC-based MAC-then-encrypt, but several
 * details are deliberately non-standard (see make_certs()'s "what's
 * this?" and "seems to violate the standard" comments carried over
 * verbatim from python-validity below) -- do NOT reach for a real TLS
 * library here, this must byte-for-byte match python-validity's
 * framing quirks or the sensor will reject it.
 *
 * TRANSPORT DESIGN (read before wiring this into metallica_mis_daemon.c):
 *
 * python-validity's Tls class owns a Usb instance and calls
 * self.usb.cmd() directly. This C port does NOT hard-depend on
 * metallica_mis_daemon.c's static cmd() (which is scoped to the
 * plaintext bootstrap stage only, per that file's own header comment
 * saying not to call it past send_init()). Instead this module takes
 * a transport callback at init time -- metallica_mis_tls_transport_fn --
 * so:
 *   (a) metallica_mis_daemon.c can pass in a *different*, non-static
 *       raw send/receive function once it's ready to hand off from
 *       send_init() to the handshake, without this module needing to
 *       know libusb exists, and
 *   (b) this module's pure protocol logic (framing, HMAC, AES, ECDH)
 *       can be unit-tested against captured/synthetic byte streams
 *       without any real USB device attached.
 *
 * SESSION LIFECYCLE:
 *   1. metallica_mis_tls_init()      -- derives PSK pair from host
 *                                        identity, zeroes session state
 *   2. metallica_mis_tls_open()      -- runs the full handshake:
 *                                        ClientHello -> ServerHello/
 *                                        CertRequest/ServerHelloDone ->
 *                                        Certificate+ClientKeyExchange+
 *                                        CertVerify+ChangeCipherSpec+
 *                                        Finished -> Finished. On
 *                                        return, secure_tx/secure_rx
 *                                        are both true and metallica_mis_tls_cmd()
 *                                        is usable.
 *   3. metallica_mis_tls_cmd()       -- send an application-layer
 *                                        command through the now-open
 *                                        session, get the response
 *
 * PREREQUISITE: metallica_mis_tls_open() needs self.tls_cert (the
 * client's cert blob) and self.priv_key (the client's identity
 * keypair) already populated -- those come from the pairing flow
 * (metallica_mis_handle_cert() / metallica_mis_handle_priv(), called
 * during init_flash()'s partition_flash()/flash-read sequence, NOT
 * built here). On a sensor that's ALREADY been paired before (flash
 * has partitions), those get loaded from the sensor's own cert
 * partition instead -- that flash-read path also isn't built yet.
 * So: this module is complete and testable in isolation, but nothing
 * end-to-end works until init_flash.c's partition_flash() (still
 * blocked, see metallica_mis_init_flash.h) actually populates a
 * metallica_mis_identity_t and calls metallica_mis_tls_open() with it.
 *
 * NOT YET VERIFIED against real hardware -- same caveat as everything
 * else in this project's metallica_mis_* files so far.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <openssl/ec.h>
#include <openssl/sha.h>

#define METALLICA_MIS_TLS_KEYLEN 0x20 /* 32 bytes, matches all key sizes in this protocol */

/* Transport callback: write out_len bytes from `out`, then read up to
 * in_buf_size bytes into `in_buf`, returning the number of bytes
 * actually read, or a negative value on transport failure. Mirrors
 * python-validity's Usb.cmd() semantics (one write, one read, no
 * separate framing at this layer -- framing is handled above this by
 * the TLS record layer). ctx is an opaque pointer the caller can use
 * for its own device handle (e.g. daemon.c's libusb_device_handle*). */
typedef int (*metallica_mis_tls_transport_fn)(void *ctx, const unsigned char *out, size_t out_len,
                                               unsigned char *in_buf, size_t in_buf_size);

/* The client's paired identity: its own EC keypair + the cert blob
 * the sensor issued/accepted for it during pairing. Populated either
 * fresh (during init_flash()'s pairing flow) or loaded back from the
 * sensor's cert flash partition on a subsequent connection -- neither
 * path is built yet (see file header), so callers currently have no
 * real way to fill this in. Structure defined now so metallica_mis_tls_open()'s
 * signature is stable once that path exists. */
typedef struct {
    EC_KEY *priv_key;         /* client's own identity keypair (NOT the ephemeral session key) */
    unsigned char *tls_cert;  /* cert blob, as received via metallica_mis_handle_cert() during pairing */
    size_t tls_cert_len;

    /* Device's own PERSISTENT ECDH public key (self.ecdh_q in
     * python-validity). NOT re-negotiated per session -- this suite
     * is TLS_ECDH_ECDSA, which per RFC 4492 derives ECDH params from
     * the certificate/identity rather than a ServerKeyExchange
     * message, so this gets set ONCE (metallica_mis_handle_ecdh(),
     * during pairing, signature-checked against the hardcoded
     * Synaptics firmware pubkey) and reused by every subsequent
     * metallica_mis_tls_open() call's make_keys() step. */
    EC_KEY *device_ecdh_pub;
} metallica_mis_identity_t;

/* metallica_mis_handle_cert() -- stores the raw cert blob the device
 * hands back, verbatim (matches Tls.handle_cert(); python-validity's
 * own comment: "TODO validate cert, check if pub keys match" -- not
 * validated there either, ported as-is). Copies body into a
 * malloc'd buffer owned by *identity (caller must free identity->tls_cert
 * eventually). Returns 0 on success, -1 on allocation failure. */
int metallica_mis_handle_cert(metallica_mis_identity_t *identity,
                               const unsigned char *body, size_t body_len);

/* metallica_mis_handle_ecdh() -- parses the device's persistent ECDH
 * public key + Synaptics-firmware-signed proof of authenticity, sets
 * identity->device_ecdh_pub. Port of Tls.handle_ecdh(). Returns 0 on
 * success, -1 if the blob is malformed OR the signature check fails
 * (matches python's fwpub.verify() raising InvalidSignature -- this
 * is a real security check, not decorative: it's what stops a rogue
 * device from pairing as if it were a genuine Synaptics sensor). */
int metallica_mis_handle_ecdh(metallica_mis_identity_t *identity,
                               const unsigned char *body, size_t body_len);

/* metallica_mis_handle_priv() -- unwraps the device's response to
 * init_flash.c's metallica_mis_encrypt_key() (the PSK-wrapped client
 * identity keypair sent back by the device during pairing -- yes,
 * the device just echoes back what we sent it, encrypted the same
 * way, as an implicit ack). Sets identity->priv_key. Returns 0 on
 * success, -1 on HMAC mismatch (python's exact wording: "This device
 * was probably paired with another computer" -- preserved in the
 * error path below) or malformed body. */
int metallica_mis_handle_priv(metallica_mis_identity_t *identity,
                               const unsigned char psk_encryption_key[METALLICA_MIS_TLS_KEYLEN],
                               const unsigned char psk_validation_key[METALLICA_MIS_TLS_KEYLEN],
                               const unsigned char *body, size_t body_len);

typedef struct {
    metallica_mis_tls_transport_fn transport;
    void *transport_ctx;

    /* pre-session PSK pair, from metallica_mis_set_hwkey() (see
     * metallica_mis_init_flash.h -- reused here rather than
     * duplicated, since it's the same derivation) */
    unsigned char psk_encryption_key[METALLICA_MIS_TLS_KEYLEN];
    unsigned char psk_validation_key[METALLICA_MIS_TLS_KEYLEN];

    /* client's paired identity -- must be set by caller before
     * metallica_mis_tls_open() */
    const metallica_mis_identity_t *identity;

    /* handshake transcript hash -- fed every handshake message as it's
     * built/parsed, per make_finish()/handle_finish()'s use of
     * self.handshake_hash */
    SHA256_CTX handshake_hash;

    /* handshake-local state */
    unsigned char client_random[32];
    unsigned char server_random[32];
    unsigned char server_sessid[32];
    size_t server_sessid_len;
    EC_KEY *session_key;      /* ephemeral ECDH keypair, this side */
    EC_KEY *ecdh_q;           /* server's ephemeral ECDH public key */

    /* derived session key material, from make_keys() */
    unsigned char master_secret[0x30];
    unsigned char sign_key[METALLICA_MIS_TLS_KEYLEN];
    unsigned char validation_key[METALLICA_MIS_TLS_KEYLEN];
    unsigned char encryption_key[METALLICA_MIS_TLS_KEYLEN];
    unsigned char decryption_key[METALLICA_MIS_TLS_KEYLEN];

    bool secure_tx;
    bool secure_rx;
} metallica_mis_tls_t;

/* metallica_mis_tls_init() -- zeroes session state and derives the
 * PSK pair from host product name/serial (see set_hwkey() port in
 * metallica_mis_init_flash.c). Does NOT open the session. Returns 0
 * on success, -1 on failure. */
int metallica_mis_tls_init(metallica_mis_tls_t *tls,
                            metallica_mis_tls_transport_fn transport, void *transport_ctx,
                            const char *product_name, const char *serial_number);

/* metallica_mis_tls_open() -- runs the full handshake. tls->identity
 * must be set first (see struct comment above -- currently nothing
 * populates this). Returns 0 on success, -1 on any handshake failure
 * (bad server response, signature mismatch, transport error, etc). */
int metallica_mis_tls_open(metallica_mis_tls_t *tls, const metallica_mis_identity_t *identity);

/* metallica_mis_tls_cmd() -- sends `cmd` (cmd_len bytes) as an
 * application-layer command through the open session and returns the
 * decrypted, validated response. Requires secure_tx && secure_rx
 * (i.e. metallica_mis_tls_open() succeeded). Response written to
 * out_buf (caller-provided, out_buf_size capacity); returns number of
 * bytes written, or -1 on failure. Mirrors Tls.cmd()/Tls.app(). */
int metallica_mis_tls_cmd(metallica_mis_tls_t *tls, const unsigned char *cmd, size_t cmd_len,
                           unsigned char *out_buf, size_t out_buf_size);

/* metallica_mis_tls_free() -- releases session_key/ecdh_q. Does not
 * free tls->identity (caller-owned). */
void metallica_mis_tls_free(metallica_mis_tls_t *tls);

#endif /* __METALLICA_MIS_TLS_H */
