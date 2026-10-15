/*
 * upek_proto.h
 *
 * Protocol constants for the UPEK/AuthenTec TouchStrip Sensor-Only
 * sensor (147e:2016). Ported directly from libfprint's upeksonly.c
 * driver (Copyright 2008 Daniel Drake <dsd@gentoo.org>, LGPL 2.1),
 * the same way VFS5011's blobs were pulled from libfprint and
 * Metallica MIS's protocol was pulled from python-validity -- this
 * project treats existing open-source drivers as ground truth rather
 * than guessing at USB protocols from scratch.
 *
 * Architecturally this sensor is much simpler than VFS5011 or
 * Metallica MIS: there's no encrypted blob playback and no pairing
 * handshake, just plain USB control transfers to read/write 8-bit
 * registers, an interrupt endpoint that fires when a finger is
 * detected, and a bulk endpoint that streams raw swipe image data
 * once capture mode is entered. This matches the sensor's own
 * "Sensor-Only" naming -- no on-board matching, no host-lock, it's a
 * dumb swipe sensor like VFS5011, just with a register-based init
 * sequence instead of one long init blob.
 */

#ifndef __UPEK_PROTO_H
#define __UPEK_PROTO_H

#include <stdint.h>

#define UPEK_VID 0x147e
#define UPEK_PID 0x2016

/* Bulk IN endpoint that streams raw image data once capture mode is
 * entered (24 outstanding 4096-byte transfers kept in flight in the
 * original driver -- see upek_daemon.c's capture loop). */
#define UPEK_IN_ENDPOINT_BULK  0x81
/* Interrupt IN endpoint that fires once when a finger touches the
 * sensor (used to gate entry into capture mode instead of polling). */
#define UPEK_IN_ENDPOINT_INTR  0x83

#define UPEK_CTRL_TIMEOUT_MS 1000

/* Every register read/write goes through control transfer request
 * 0x0c: bmRequestType 0x40 (host-to-device, vendor, device) to WRITE
 * a register (wIndex = register, 1-byte data = value), and 0xc0
 * (device-to-host, vendor, device) to READ one (wIndex = register,
 * device returns 8 bytes back, only byte[0] is meaningful). */
#define UPEK_CTRL_REQUEST 0x0c
#define UPEK_CTRL_WRITE_REQTYPE 0x40
#define UPEK_CTRL_READ_REQTYPE  0xc0
#define UPEK_CTRL_READ_RESPONSE_LEN 8

/* Each 4096-byte bulk transfer is 64 packets of 64 bytes. Each packet
 * is a 2-byte big-endian sequence number followed by 62 bytes of raw
 * image data -- the sequence number is what lets us detect dropped
 * packets and 14-bit (16384) wraparound and reassemble packets into
 * IMG_WIDTH-wide rows even though 62 doesn't evenly divide 288. */
#define UPEK_BULK_TRANSFER_SIZE 4096
#define UPEK_PACKETS_PER_TRANSFER 64
#define UPEK_PACKET_SIZE 64
#define UPEK_PACKET_HEADER_SIZE 2
#define UPEK_PACKET_DATA_SIZE 62
#define UPEK_SEQNUM_WRAP 16384

/* Number of bulk transfers kept simultaneously in flight during
 * capture in the original async driver. upek_daemon.c's synchronous
 * port doesn't need to keep this many in flight at once (no libusb
 * event loop to juggle), but the same total transfer count is kept
 * as the natural read-ahead depth so a slow swipe doesn't starve. */
#define UPEK_NUM_BULK_TRANSFERS 24

#define UPEK_IMG_WIDTH 288
/* Upper bound on captured rows per swipe -- a full-length swipe that
 * never triggers finger-removal detection stops here regardless. */
#define UPEK_MAX_ROWS 700

/* Row-to-row comparison thresholds used for two purposes: detecting
 * a genuinely new row worth keeping (DIFF) vs. a duplicate/near-
 * duplicate row from a slow swipe, and detecting a "blank" row (no
 * finger contact, i.e. lifted off) via its total brightness (TOTAL).
 * Ported as-is from upeksonly.c's row_complete() -- these are tuned
 * against this exact sensor's raw output range, not arbitrary. */
#define UPEK_ROW_DIFF_THRESHOLD 3000
#define UPEK_ROW_BLANK_TOTAL_THRESHOLD 52000
/* Consecutive blank rows required before treating the finger as
 * lifted and ending capture. */
#define UPEK_BLANK_ROWS_FOR_REMOVAL 500

typedef struct {
    uint8_t reg;
    uint8_t value;
} upek_regwrite_t;

/* ---- INITIALIZATION (dev_activate -> initsm in upeksonly.c) ---- */
static const upek_regwrite_t UPEK_INIT_WRITEV_1[] = {
    { 0x49, 0x00 },

    /* The original driver's comment on this block, kept verbatim
     * because it's exactly the kind of thing you'd otherwise waste an
     * afternoon rediscovering: "BSAPI writes different values to
     * register 0x3e each time. I initially thought this was some kind
     * of clever authentication, but just blasting these sniffed
     * values each time seems to work." i.e. this looks scary but
     * isn't a real handshake -- unlike Metallica MIS's actual ECDH,
     * there's nothing here to derive per-session. */
    { 0x3e, 0x83 }, { 0x3e, 0x4f }, { 0x3e, 0x0f }, { 0x3e, 0xbf },
    { 0x3e, 0x45 }, { 0x3e, 0x35 }, { 0x3e, 0x1c }, { 0x3e, 0xae },

    { 0x44, 0x01 }, { 0x43, 0x06 }, { 0x43, 0x05 }, { 0x43, 0x04 },
    { 0x44, 0x00 }, { 0x0b, 0x00 },
};
/* After UPEK_INIT_WRITEV_1: read reg 0x09, write it back with bit
 * 0x08 cleared; read reg 0x13, write it back with bit 0x10 cleared;
 * then write 0x04=0x00 and 0x05=0x00. These four steps are
 * read-modify-write (the value written depends on what's read back),
 * so they're implemented as explicit steps in upek_daemon.c rather
 * than a static table -- see upek_run_initsm(). */

/* ---- AWAIT FINGER (awfsm in upeksonly.c) ---- */
static const upek_regwrite_t UPEK_AWFSM_WRITEV_1[] = {
    { 0x0a, 0x00 }, { 0x0a, 0x00 }, { 0x09, 0x20 }, { 0x03, 0x3b },
    { 0x00, 0x67 }, { 0x00, 0x67 },
};
/* Then: read reg 0x01; write 0x01 = (read == 0xc6) ? 0xc6 : 0x46. */

static const upek_regwrite_t UPEK_AWFSM_WRITEV_2[] = {
    { 0x01, 0xc6 }, { 0x0c, 0x13 }, { 0x0d, 0x0d }, { 0x0e, 0x0e },
    { 0x0f, 0x0d }, { 0x0b, 0x00 },
};
/* Then: read reg 0x13; write 0x13 = (read == 0x45) ? 0x45 : 0x05. */

static const upek_regwrite_t UPEK_AWFSM_WRITEV_3[] = {
    { 0x13, 0x45 }, { 0x30, 0xe0 }, { 0x12, 0x01 }, { 0x20, 0x01 },
    { 0x09, 0x20 }, { 0x0a, 0x00 }, { 0x30, 0xe0 }, { 0x20, 0x01 },
};
/* Then: read reg 0x07; write 0x07 = whatever was just read back
 * unchanged (the original driver only warns if it's not 0x10/0x90,
 * it doesn't refuse to proceed -- kept as a warning here too). */

static const upek_regwrite_t UPEK_AWFSM_WRITEV_4[] = {
    { 0x08, 0x00 }, { 0x10, 0x00 }, { 0x12, 0x01 }, { 0x11, 0xbf },
    { 0x12, 0x01 }, { 0x07, 0x10 }, { 0x07, 0x10 }, { 0x04, 0x00 },
    { 0x05, 0x00 }, { 0x0b, 0x00 },

    /* enter finger detection mode -- after this, the interrupt
     * endpoint fires once a finger is actually on the sensor. */
    { 0x15, 0x20 }, { 0x30, 0xe1 }, { 0x15, 0x24 }, { 0x15, 0x04 },
    { 0x15, 0x84 },
};

/* ---- CAPTURE MODE (capsm in upeksonly.c) ---- */
/* Entered right after the interrupt fires: write 0x15=0x20, then
 * 0x30=0xe0, THEN submit the bulk reads, THEN this final writev. */
#define UPEK_CAPSM_PRE_WRITE_15 0x15
#define UPEK_CAPSM_PRE_WRITE_15_VAL 0x20
#define UPEK_CAPSM_PRE_WRITE_30 0x30
#define UPEK_CAPSM_PRE_WRITE_30_VAL 0xe0

static const upek_regwrite_t UPEK_CAPSM_WRITEV[] = {
    /* enter capture mode */
    { 0x09, 0x28 }, { 0x13, 0x55 }, { 0x0b, 0x80 }, { 0x04, 0x00 },
    { 0x05, 0x00 },
};

/* ---- DEINITIALIZATION (deinitsm in upeksonly.c) ---- */
static const upek_regwrite_t UPEK_DEINIT_WRITEV[] = {
    /* reset + enter low power mode -- note 0x13=0x45 is genuinely
     * sent twice in the original driver, not a copy/paste artifact
     * here; kept exactly as upeksonly.c has it. */
    { 0x0b, 0x00 }, { 0x09, 0x20 }, { 0x13, 0x45 }, { 0x13, 0x45 },
};

#endif /* __UPEK_PROTO_H */
