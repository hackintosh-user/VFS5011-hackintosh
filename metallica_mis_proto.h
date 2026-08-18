#ifndef __METALLICA_MIS_PROTO_H
#define __METALLICA_MIS_PROTO_H

/*
 * metallica_mis_proto.h
 *
 * Protocol constants and (eventually) firmware/init blobs for the
 * Synaptics "Metallica MIS" sensor family (USB 06cb:009a), confirmed
 * on p0cketl1nt's ThinkPad X1C6 (Sequoia 15.7.1, enumerated as a
 * Vendor-Specific Device, Built-In: Yes, no macOS driver claiming it).
 *
 * Unlike VFS5011, this sensor family does NOT talk in plaintext after
 * the first couple of commands. Per uunicorn/python-validity (the
 * Linux reference implementation this is being ported from), 06cb:009a
 * is one of the "Prometheus" chips: it needs a TLS-like ECDH handshake
 * negotiated from the host + device identity before firmware upload
 * and calibration can happen, and every command after that handshake
 * is wrapped in a session cipher, not sent raw like vfs5011_cmd_XX.
 *
 * So this header only owns the PLAINTEXT bootstrap stage (RomInfo,
 * FwInfo, and the two hardcoded init blobs sent before any crypto is
 * involved) plus endpoint/timeout constants. The ECDH key exchange,
 * session encrypt/decrypt wrapping, and the actual per-variant
 * firmware blob (equivalent to python-validity's blobs_9a.py /
 * 6_07f_lenovo_mis_qm.xpfwext) belong in metallica_mis_tls.c /
 * metallica_mis_tls.h once that stage is actually built --
 * deliberately NOT stubbed here yet since faking a crypto interface
 * before the handshake is understood would just be something to rip
 * out and redo. See the project's active-development notes for the
 * reverse-engineering trail (python-validity usb.py / sensor.py).
 *
 * TODO(metallica-mis): confirm real endpoint numbers against this
 * sensor once hands-on with hardware -- these are carried over from
 * python-validity's Usb class (write EP1, read command replies from
 * EP0x81, bulk image data from EP0x82, interrupt/notify from EP0x83)
 * but every sensor family in this project so far has needed its own
 * libusb_claim_interface()/detach-kernel-driver quirks worked out
 * against real macOS behavior (see vfs5011_daemon.c's open_device()
 * comments), so treat these as a starting guess, not verified fact.
 */

#define METALLICA_MIS_PROJECT_VERSION "0.0.1-scaffold"

enum {
    METALLICA_MIS_DEFAULT_WAIT_TIMEOUT = 3000,

    METALLICA_MIS_OUT_ENDPOINT      = 1 | LIBUSB_ENDPOINT_OUT,
    METALLICA_MIS_IN_ENDPOINT_CTRL  = 1 | LIBUSB_ENDPOINT_IN,  /* 0x81 */
    METALLICA_MIS_IN_ENDPOINT_DATA  = 2 | LIBUSB_ENDPOINT_IN,  /* 0x82 */
    METALLICA_MIS_IN_ENDPOINT_INT   = 3 | LIBUSB_ENDPOINT_IN,  /* 0x83 */
};

enum {
    METALLICA_MIS_RECEIVE_BUF_SIZE = 102400
};

static unsigned char METALLICA_MIS_NORMAL_CONTROL_REPLY[] = {0x00, 0x00};

/* RomInfo.get() -- plaintext, sent before any handshake.
 * Byte value carried over from python-validity's send_init(); TODO
 * confirm identical against 06cb:009a once hardware is available. */
static unsigned char metallica_mis_cmd_rominfo[] = { /* 1 B */
    0x01,
};

/* Unknown plaintext init command sent immediately after RomInfo in
 * python-validity's send_init(). Not yet reverse-engineered further
 * than "it's required and it's plaintext". */
static unsigned char metallica_mis_cmd_19[] = { /* 1 B */
    0x19,
};

/* get_fw_info() -- requests partition header / fwext build time.
 * TODO: confirm response parsing (python-validity reads a build-time
 * u32 out of this reply before deciding whether firmware is already
 * loaded or a "clean slate" firmware upload is needed). */
static unsigned char metallica_mis_cmd_fwinfo[] = { /* 2 B */
    0x43, 0x02,
};

/*
 * init_hardcoded / init_hardcoded_clean_slate:
 *
 * python-validity's blobs.py ships these as precomputed byte
 * sequences (NOT derived from the ECDH handshake -- they're sent
 * before any crypto starts, same bootstrap stage as cmd_rominfo/
 * cmd_19/cmd_fwinfo above). Deliberately left empty here rather than
 * filled with guessed bytes: these need to come from either (a) a USB
 * trace against real 06cb:009a hardware, or (b) extracting them from
 * the same source python-validity itself pulls from. Do not ship a
 * build with these still empty -- send_init() will no-op the blob
 * send and nothing downstream will work.
 */
static unsigned char metallica_mis_init_hardcoded[] = {
    /* TODO(metallica-mis): populate from hardware trace or
     * python-validity blobs_9a.py equivalent. */
    0x00,
};

static unsigned char metallica_mis_init_hardcoded_clean_slate[] = {
    /* TODO(metallica-mis): populate -- only sent if get_fw_info()
     * indicates fwext isn't loaded yet ("Clean slate" path). */
    0x00,
};

#endif /* __METALLICA_MIS_PROTO_H */
