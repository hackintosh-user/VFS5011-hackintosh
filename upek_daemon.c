/*
 * upek_daemon.c
 *
 * Capture backend for the UPEK/AuthenTec TouchStrip Sensor-Only
 * sensor (147e:2016), r/hackintosh-confirmed on a ThinkPad T420
 * (Aug 16 2026). This fills in the "capture backend not built yet"
 * gap noted against this sensor in supported_sensors.h.
 *
 * Ported from libfprint's upeksonly.c (Copyright 2008 Daniel Drake
 * <dsd@gentoo.org>, LGPL 2.1) -- see upek_proto.h for the register
 * tables and a note on where each one fits in the sequence below.
 * libfprint's driver is async (libusb_transfer + callbacks running
 * under a glib main loop, because it has to serve multiple sensors
 * and share a process with other libfprint consumers). This project
 * has no such constraint -- like vfs5011_daemon.c's capture path,
 * this is a synchronous, blocking port: same register writes, same
 * order, same thresholds, just via libusb_control_transfer() /
 * libusb_bulk_transfer() / libusb_interrupt_transfer() called
 * straight-line instead of submitted-and-awaited-via-callback. This
 * is a substantially easier port than VFS5011 or Metallica MIS were:
 * no blob playback, no ECDH, no firmware upload -- just 8-bit
 * register read/write and one bulk stream.
 *
 * STATUS: capture function only, tested logic-only against the
 * libfprint source (not yet run against real hardware -- Cold_
 * Salamander7764 has a UPEK ThinkPad and is willing to test whenever
 * he's free). NOT YET WIRED into hack_touchid_client.c's
 * capture_fingerprint_image() dispatch or supported_sensors.h's
 * backend_available flag -- that dispatch is currently hardcoded to
 * VFS5011 (see hack_touchid_client.c around open_device() /
 * capture_fingerprint_image()) and needs a small sensor-id branch
 * added once this has a first real-hardware pass. Flagging that
 * explicitly rather than flipping backend_available before hardware
 * has actually confirmed it works, same policy this project already
 * follows for Metallica MIS.
 *
 * Build (once wired into the client -- for now this compiles
 * standalone via upek_capture_test_main() below for a first
 * hardware smoke test):
 *   clang upek_daemon.c -o upek_capture_test \
 *       -I. -I/usr/local/include/libusb-1.0 -L/usr/local/lib -lusb-1.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <libusb.h>

#include "upek_proto.h"

/* ---- low-level register read/write, synchronous ---- */

/* Returns 0 on success (with *out_value set), or a negative libusb
 * error code. Mirrors sm_read_reg()/sm_read_reg_cb() in upeksonly.c,
 * collapsed into one blocking call since there's no event loop here
 * to hand a callback back into. */
static int upek_read_reg(libusb_device_handle *handle, uint8_t reg, uint8_t *out_value) {
    unsigned char buf[UPEK_CTRL_READ_RESPONSE_LEN];
    int r = libusb_control_transfer(handle,
        UPEK_CTRL_READ_REQTYPE, UPEK_CTRL_REQUEST,
        /* wValue */ 0, /* wIndex */ reg,
        buf, sizeof(buf), UPEK_CTRL_TIMEOUT_MS);
    if (r < 0) return r;
    /* Only byte[0] of the 8-byte response is meaningful -- same as
     * upeksonly.c's sm_read_reg_cb() taking data[0] and discarding
     * the rest. */
    *out_value = buf[0];
    return 0;
}

/* Mirrors sm_write_reg(). */
static int upek_write_reg(libusb_device_handle *handle, uint8_t reg, uint8_t value) {
    unsigned char data = value;
    int r = libusb_control_transfer(handle,
        UPEK_CTRL_WRITE_REQTYPE, UPEK_CTRL_REQUEST,
        /* wValue */ 0, /* wIndex */ reg,
        &data, 1, UPEK_CTRL_TIMEOUT_MS);
    if (r < 0) return r;
    return 0;
}

/* Mirrors sm_write_regs() -- writes a whole table in order, aborting
 * on the first failure (same behavior as the original: one failed
 * write tears down the whole sequence rather than skipping ahead). */
static int upek_write_regs(libusb_device_handle *handle, const upek_regwrite_t *regs, size_t count) {
    for (size_t i = 0; i < count; i++) {
        int r = upek_write_reg(handle, regs[i].reg, regs[i].value);
        if (r < 0) {
            fprintf(stderr, "upek: write reg 0x%02x=0x%02x failed: %s\n",
                    regs[i].reg, regs[i].value, libusb_error_name(r));
            return r;
        }
    }
    return 0;
}

/* ---- INITIALIZATION ---- */

/* Mirrors initsm_run_state() end to end -- the writev table, then the
 * four read-modify-write steps upek_proto.h documents but can't
 * express as a static table. */
static int upek_run_init(libusb_device_handle *handle) {
    int r;
    uint8_t val;

    r = upek_write_regs(handle, UPEK_INIT_WRITEV_1, sizeof(UPEK_INIT_WRITEV_1) / sizeof(UPEK_INIT_WRITEV_1[0]));
    if (r < 0) return r;

    r = upek_read_reg(handle, 0x09, &val);
    if (r < 0) return r;
    r = upek_write_reg(handle, 0x09, (uint8_t)(val & ~0x08));
    if (r < 0) return r;

    r = upek_read_reg(handle, 0x13, &val);
    if (r < 0) return r;
    r = upek_write_reg(handle, 0x13, (uint8_t)(val & ~0x10));
    if (r < 0) return r;

    r = upek_write_reg(handle, 0x04, 0x00);
    if (r < 0) return r;
    r = upek_write_reg(handle, 0x05, 0x00);
    if (r < 0) return r;

    return 0;
}

/* ---- AWAIT FINGER ---- */

/* Mirrors awfsm_run_state() end to end, finishing with a blocking
 * interrupt read that only returns once a finger is actually on the
 * sensor -- same trigger upeksonly.c uses instead of polling. */
static int upek_await_finger(libusb_device_handle *handle) {
    int r;
    uint8_t val;

    r = upek_write_regs(handle, UPEK_AWFSM_WRITEV_1, sizeof(UPEK_AWFSM_WRITEV_1) / sizeof(UPEK_AWFSM_WRITEV_1[0]));
    if (r < 0) return r;

    r = upek_read_reg(handle, 0x01, &val);
    if (r < 0) return r;
    r = upek_write_reg(handle, 0x01, (val != 0xc6) ? 0x46 : 0xc6);
    if (r < 0) return r;

    r = upek_write_regs(handle, UPEK_AWFSM_WRITEV_2, sizeof(UPEK_AWFSM_WRITEV_2) / sizeof(UPEK_AWFSM_WRITEV_2[0]));
    if (r < 0) return r;

    r = upek_read_reg(handle, 0x13, &val);
    if (r < 0) return r;
    r = upek_write_reg(handle, 0x13, (val != 0x45) ? 0x05 : 0x45);
    if (r < 0) return r;

    r = upek_write_regs(handle, UPEK_AWFSM_WRITEV_3, sizeof(UPEK_AWFSM_WRITEV_3) / sizeof(UPEK_AWFSM_WRITEV_3[0]));
    if (r < 0) return r;

    r = upek_read_reg(handle, 0x07, &val);
    if (r < 0) return r;
    if (val != 0x10 && val != 0x90) {
        fprintf(stderr, "upek: warning: unexpected reg 0x07 value 0x%02x (expected 0x10 or 0x90)\n", val);
    }
    r = upek_write_reg(handle, 0x07, val);
    if (r < 0) return r;

    r = upek_write_regs(handle, UPEK_AWFSM_WRITEV_4, sizeof(UPEK_AWFSM_WRITEV_4) / sizeof(UPEK_AWFSM_WRITEV_4[0]));
    if (r < 0) return r;

    /* Blocking wait for the interrupt that signals finger-on-sensor.
     * upeksonly.c logs the 4 interrupt bytes at debug level and
     * otherwise ignores their content -- only the fact that the
     * transfer completed matters. */
    unsigned char intr_buf[4];
    int transferred = 0;
    r = libusb_interrupt_transfer(handle, UPEK_IN_ENDPOINT_INTR,
        intr_buf, sizeof(intr_buf), &transferred, 0 /* no timeout: block until a finger arrives */);
    if (r < 0) {
        fprintf(stderr, "upek: interrupt wait failed: %s\n", libusb_error_name(r));
        return r;
    }
    return 0;
}

/* ---- CAPTURE: row/image assembly state, mirrors struct sonly_dev's capture fields ---- */

typedef struct {
    unsigned char *rows[UPEK_MAX_ROWS];
    size_t num_rows;

    unsigned char *rowbuf;      /* IMG_WIDTH bytes, or NULL if not mid-row */
    int rowbuf_offset;          /* -1 when rowbuf is inactive */

    int wraparounds;
    uint16_t last_seqnum;

    int num_blank;
    int finger_removed;
} upek_capture_state_t;

/* Mirrors compute_rows(): sum of absolute per-pixel differences (for
 * "is this a new row"), and total brightness of the new row (for
 * "has the finger lifted off"). */
static void upek_compute_row_diff(const unsigned char *a, const unsigned char *b, int *out_diff, int *out_total) {
    int diff = 0, total = 0;
    for (int i = 0; i < UPEK_IMG_WIDTH; i++) {
        diff += (a[i] > b[i]) ? (a[i] - b[i]) : (b[i] - a[i]);
        total += b[i];
    }
    *out_diff = diff;
    *out_total = total;
}

/* Mirrors row_complete(): decide whether the just-filled rowbuf is a
 * genuinely new row worth keeping, a near-duplicate to discard, or
 * evidence the finger has lifted off (via the blank-row run-length
 * counter). Returns 1 if capture should stop (finger removed or row
 * limit hit), 0 to keep going. Takes ownership of state->rowbuf on
 * either path (keeps it in state->rows[], or frees it as a
 * duplicate) and always clears rowbuf_offset back to -1 after. */
static int upek_row_complete(upek_capture_state_t *state) {
    state->rowbuf_offset = -1;

    if (state->num_rows > 0) {
        unsigned char *lastrow = state->rows[state->num_rows - 1];
        int diff, total;
        upek_compute_row_diff(lastrow, state->rowbuf, &diff, &total);

        if (total < UPEK_ROW_BLANK_TOTAL_THRESHOLD) {
            state->num_blank = 0;
        } else {
            state->num_blank++;
            if (state->num_blank > UPEK_BLANK_ROWS_FOR_REMOVAL) {
                state->finger_removed = 1;
                free(state->rowbuf);
                state->rowbuf = NULL;
                return 1;
            }
        }

        if (diff < UPEK_ROW_DIFF_THRESHOLD) {
            /* Near-duplicate of the last row (slow part of the
             * swipe) -- discard rather than keep, same as the
             * original driver's early return here. */
            free(state->rowbuf);
            state->rowbuf = NULL;
            return 0;
        }
    }

    if (state->num_rows >= UPEK_MAX_ROWS) {
        /* Shouldn't normally get here -- the caller checks the row
         * count before starting a new row -- but guard anyway rather
         * than overflow state->rows[]. */
        free(state->rowbuf);
        state->rowbuf = NULL;
        return 1;
    }

    state->rows[state->num_rows++] = state->rowbuf;
    state->rowbuf = NULL;

    if (state->num_rows >= UPEK_MAX_ROWS) {
        return 1;
    }
    return 0;
}

/* Mirrors add_to_rowbuf(): append `size` bytes of packet payload
 * (already past the 2-byte sequence header) into the in-progress
 * row buffer. Returns 1 if capture should stop (via
 * upek_row_complete), 0 to keep going. */
static int upek_add_to_rowbuf(upek_capture_state_t *state, const unsigned char *data, int size) {
    memcpy(state->rowbuf + state->rowbuf_offset, data, size);
    state->rowbuf_offset += size;
    if (state->rowbuf_offset >= UPEK_IMG_WIDTH) {
        return upek_row_complete(state);
    }
    return 0;
}

/* Mirrors start_new_row(): a packet that starts a new row doesn't
 * necessarily start it at byte 0 of that row (62 doesn't evenly
 * divide 288), so the tail of the PREVIOUS row's last 2 bytes get
 * folded in at the end of the new buffer before the new packet's
 * data is copied in after them. This looks backwards at a glance but
 * is exactly what upeksonly.c does -- kept as-is since it works. */
static void upek_start_new_row(upek_capture_state_t *state, const unsigned char *data, int size) {
    if (!state->rowbuf) {
        state->rowbuf = malloc(UPEK_IMG_WIDTH);
    }
    memcpy(state->rowbuf + UPEK_IMG_WIDTH - 2, data, 2);
    memcpy(state->rowbuf, data + 2, size - 2);
    state->rowbuf_offset = size;
}

/* Mirrors rowbuf_remaining(): -1 if no row is in progress, else how
 * many bytes of the NEXT packet still belong to the current row
 * (capped to a full packet's 62 data bytes). */
static int upek_rowbuf_remaining(const upek_capture_state_t *state) {
    if (state->rowbuf_offset == -1) return -1;
    int r = UPEK_IMG_WIDTH - state->rowbuf_offset;
    return (r > UPEK_PACKET_DATA_SIZE) ? UPEK_PACKET_DATA_SIZE : r;
}

/* Mirrors handle_packet(): one 64-byte packet in, dispatched to
 * whichever of the three cases above applies based on the running
 * absolute byte address (sequence number * 62, adjusted for 14-bit
 * wraparound). Returns 1 if capture should stop. */
static int upek_handle_packet(upek_capture_state_t *state, const unsigned char *packet) {
    uint16_t seqnum = (uint16_t)((packet[0] << 8) | packet[1]);
    const unsigned char *data = packet + UPEK_PACKET_HEADER_SIZE;

    if (seqnum <= state->last_seqnum) {
        state->wraparounds++;
    }
    state->last_seqnum = seqnum;

    long abs_seqnum = (long)seqnum + (long)state->wraparounds * UPEK_SEQNUM_WRAP;
    long abs_base_addr = abs_seqnum * UPEK_PACKET_DATA_SIZE;

    int for_rowbuf = upek_rowbuf_remaining(state);
    if (for_rowbuf != -1) {
        return upek_add_to_rowbuf(state, data, for_rowbuf);
    }

    if (abs_base_addr % UPEK_IMG_WIDTH == 0) {
        upek_start_new_row(state, data, UPEK_PACKET_DATA_SIZE);
        return 0;
    }

    long next_row_addr = ((abs_base_addr / UPEK_IMG_WIDTH) + 1) * UPEK_IMG_WIDTH;
    long diff = next_row_addr - abs_base_addr;
    if (diff < UPEK_PACKET_DATA_SIZE) {
        upek_start_new_row(state, data + diff, (int)(UPEK_PACKET_DATA_SIZE - diff));
    }
    return 0;
}

/* ---- CAPTURE: full swipe, synchronous bulk-read loop ---- */

/* Mirrors capsm_run_state() + the bulk transfer callback loop, minus
 * the async transfer juggling (no need for it single-threaded and
 * blocking). Returns a malloc'd UPEK_IMG_WIDTH x *out_height
 * grayscale image on success (caller frees it), or NULL on failure.
 * Shaped to match vfs5011's capture_fingerprint_image() so it can be
 * wired into the same dispatch point once tested. */
static unsigned char *upek_capture_swipe(libusb_device_handle *handle, int *out_height) {
    int r;

    r = upek_write_reg(handle, UPEK_CAPSM_PRE_WRITE_15, UPEK_CAPSM_PRE_WRITE_15_VAL);
    if (r < 0) return NULL;
    r = upek_write_reg(handle, UPEK_CAPSM_PRE_WRITE_30, UPEK_CAPSM_PRE_WRITE_30_VAL);
    if (r < 0) return NULL;
    r = upek_write_regs(handle, UPEK_CAPSM_WRITEV, sizeof(UPEK_CAPSM_WRITEV) / sizeof(UPEK_CAPSM_WRITEV[0]));
    if (r < 0) return NULL;

    upek_capture_state_t state;
    memset(&state, 0, sizeof(state));
    state.rowbuf_offset = -1;
    state.wraparounds = -1;
    state.last_seqnum = UPEK_SEQNUM_WRAP - 1;

    unsigned char *buf = malloc(UPEK_BULK_TRANSFER_SIZE);
    if (!buf) return NULL;

    int stop = 0;
    while (!stop) {
        int transferred = 0;
        r = libusb_bulk_transfer(handle, UPEK_IN_ENDPOINT_BULK, buf,
            UPEK_BULK_TRANSFER_SIZE, &transferred, UPEK_CTRL_TIMEOUT_MS * 5);
        if (r < 0) {
            fprintf(stderr, "upek: bulk read failed: %s\n", libusb_error_name(r));
            break;
        }
        if (transferred != UPEK_BULK_TRANSFER_SIZE) {
            fprintf(stderr, "upek: short bulk read (%d of %d bytes) -- treating as a dropped frame, continuing\n",
                    transferred, UPEK_BULK_TRANSFER_SIZE);
            continue;
        }

        for (int i = 0; i < UPEK_PACKETS_PER_TRANSFER; i++) {
            stop = upek_handle_packet(&state, buf + i * UPEK_PACKET_SIZE);
            if (stop) break;
        }
    }
    free(buf);
    if (state.rowbuf) free(state.rowbuf);

    if (state.num_rows == 0) {
        fprintf(stderr, "upek: no rows captured\n");
        for (size_t i = 0; i < state.num_rows; i++) free(state.rows[i]);
        return NULL;
    }

    /* upeksonly.c's handoff_img() builds rows[] as a reverse-order
     * (most-recent-first) singly-linked list via g_slist_prepend and
     * walks it once at the end, which produces the image in the
     * correct top-to-bottom order despite the prepend. This port
     * appends to an array in capture order instead, which already IS
     * top-to-bottom -- so unlike the original, no reversal is needed
     * here. */
    unsigned char *image = malloc((size_t)UPEK_IMG_WIDTH * state.num_rows);
    if (!image) {
        for (size_t i = 0; i < state.num_rows; i++) free(state.rows[i]);
        return NULL;
    }
    for (size_t i = 0; i < state.num_rows; i++) {
        memcpy(image + i * UPEK_IMG_WIDTH, state.rows[i], UPEK_IMG_WIDTH);
        free(state.rows[i]);
    }

    *out_height = (int)state.num_rows;
    return image;
}

/* ---- DEINITIALIZATION ---- */

static int upek_run_deinit(libusb_device_handle *handle) {
    return upek_write_regs(handle, UPEK_DEINIT_WRITEV, sizeof(UPEK_DEINIT_WRITEV) / sizeof(UPEK_DEINIT_WRITEV[0]));
}

/* ---- Public capture entry point ---- */

/* Full one-swipe capture: init -> await finger -> capture -> deinit.
 * This is the function to call from hack_touchid_client.c's dispatch
 * once wired in -- same shape as VFS5011's
 * capture_fingerprint_image(), so integration should just be a
 * VID:PID branch there rather than anything structural. Assumes the
 * caller has already opened the device and claimed interface 0 (same
 * division of responsibility as VFS5011's open_device(); this
 * doesn't duplicate that step since it's identical across sensors --
 * only the actual capture protocol differs). */
unsigned char *upek_capture_fingerprint_image(libusb_device_handle *handle, int *out_height) {
    if (upek_run_init(handle) < 0) {
        fprintf(stderr, "upek: init sequence failed\n");
        return NULL;
    }
    if (upek_await_finger(handle) < 0) {
        fprintf(stderr, "upek: await-finger sequence failed\n");
        return NULL;
    }

    unsigned char *image = upek_capture_swipe(handle, out_height);

    /* Run deinit regardless of whether capture succeeded, same as
     * the original driver always tearing down cleanly -- an image
     * capture failure shouldn't leave the sensor stuck in capture
     * mode for the next attempt. */
    if (upek_run_deinit(handle) < 0) {
        fprintf(stderr, "upek: warning: deinit sequence failed (non-fatal)\n");
    }

    return image;
}

/* ---- device open/close/presence, promoted out of main() below into
 * standalone functions (Aug 29) so they match the shape
 * vfs5011_daemon.c's own open_device()/close_device()/
 * vfs5011_sensor_is_present() already have -- this is prep for the
 * eventual shared daemon core (see hack_touchid_client.c's capture
 * dispatch comment and the project's own notes on that refactor),
 * which expects every sensor backend to expose exactly this shape:
 * backend_open_device(), backend_close_device(),
 * backend_sensor_is_present(), backend_image_width(). Naming these
 * with the upek_ prefix rather than backend_ for now since this file
 * isn't linked into that shared core yet -- rename is a 1-line sed
 * away whenever that wiring actually happens. */

static libusb_context *g_upek_ctx = NULL;
static libusb_device_handle *g_upek_handle = NULL;

/* Same retry-with-backoff shape as vfs5011_daemon.c's open_device() --
 * copied deliberately rather than reinvented, since that logic exists
 * specifically to paper over real macOS/IOKit timing quirks seen on
 * real hardware (a device not being immediately re-openable right
 * after a previous close, IOKit holding the interface exclusively for
 * a brief moment after enumeration). UPEK hasn't been run against
 * real hardware yet, so there's no confirmation these same quirks
 * apply here too -- but there's no reason to assume a T420's USB
 * stack behaves any better than a DV6's, so start with the same
 * defensiveness rather than adding it reactively after a tester hits
 * the same "Claim failed" problem VFS5011 already solved. */
int upek_open_device(void) {
    if (libusb_init(&g_upek_ctx) < 0) return -1;
    libusb_set_option(g_upek_ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_NONE);

    for (int i = 0; i < 5; i++) {
        g_upek_handle = libusb_open_device_with_vid_pid(g_upek_ctx, UPEK_VID, UPEK_PID);
        if (g_upek_handle) break;
        usleep(300000);
    }
    if (!g_upek_handle) {
        fprintf(stderr, "upek: device not found\n");
        return -1;
    }

    libusb_set_auto_detach_kernel_driver(g_upek_handle, 1);

    if (libusb_claim_interface(g_upek_handle, 0) == 0) return 0;

    for (int i = 0; i < 3; i++) {
        usleep(150000);
        if (libusb_claim_interface(g_upek_handle, 0) == 0) return 0;
    }

    fprintf(stderr, "upek: claim failed, resetting device and retrying...\n");
    int reset_r = libusb_reset_device(g_upek_handle);
    if (reset_r != 0) {
        fprintf(stderr, "upek: device reset failed: %s\n", libusb_error_name(reset_r));
    }
    usleep(500000);

    if (libusb_claim_interface(g_upek_handle, 0) != 0) {
        fprintf(stderr, "upek: claim failed again after reset\n");
        return -1;
    }
    return 0;
}

void upek_close_device(void) {
    if (g_upek_handle) {
        /* UPEK has no bulk OUT endpoint like VFS5011 -- just the bulk
         * IN image stream and a separate interrupt IN for finger
         * presence, both already given endpoint macros in
         * upek_proto.h (see hack_touchid_client.c's close_device(),
         * which already clears these same two for the client's own
         * UPEK path). */
        libusb_clear_halt(g_upek_handle, UPEK_IN_ENDPOINT_BULK);
        libusb_clear_halt(g_upek_handle, UPEK_IN_ENDPOINT_INTR);
        libusb_release_interface(g_upek_handle, 0);
        libusb_close(g_upek_handle);
    }
    if (g_upek_ctx) libusb_exit(g_upek_ctx);
}

/* Same non-invasive presence-check shape as
 * vfs5011_sensor_is_present() -- its own short-lived context, never
 * opens/claims, safe to call speculatively without disturbing an
 * in-flight capture on g_upek_ctx/g_upek_handle. */
bool upek_sensor_is_present(void) {
    libusb_context *probe_ctx = NULL;
    if (libusb_init(&probe_ctx) < 0) return true;
    libusb_set_option(probe_ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_NONE);

    libusb_device **list = NULL;
    ssize_t count = libusb_get_device_list(probe_ctx, &list);
    bool found = false;
    for (ssize_t i = 0; i < count; i++) {
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(list[i], &desc) != 0) continue;
        if (desc.idVendor == UPEK_VID && desc.idProduct == UPEK_PID) {
            found = true;
            break;
        }
    }
    if (list) libusb_free_device_list(list, 1);
    libusb_exit(probe_ctx);
    return found;
}

int upek_image_width(void) {
    return UPEK_IMG_WIDTH;
}

/* ---- Standalone smoke-test harness ----
 *
 * Not part of the real client integration -- this exists so a first
 * real-hardware pass can happen (swipe once, confirm a PGM comes out
 * looking like a fingerprint) before wiring this into
 * hack_touchid_client.c properly. Delete once that wiring lands and
 * the real Enroll/Verify path is what's actually being tested.
 *
 * Build: clang upek_daemon.c -o upek_capture_test -I. \
 *   -I/usr/local/include/libusb-1.0 -L/usr/local/lib -lusb-1.0
 * Run: sudo ./upek_capture_test out.pgm
 */
#ifdef UPEK_STANDALONE_TEST
int main(int argc, char **argv) {
    const char *out_path = (argc > 1) ? argv[1] : "upek_capture.pgm";

    if (upek_open_device() != 0) {
        return 1;
    }

    printf("Swipe your finger now...\n");
    int height = 0;
    unsigned char *image = upek_capture_fingerprint_image(g_upek_handle, &height);

    upek_close_device();

    if (!image) {
        fprintf(stderr, "Capture failed\n");
        return 1;
    }

    printf("Captured %d rows x %d columns\n", height, upek_image_width());

    FILE *f = fopen(out_path, "wb");
    if (!f) {
        fprintf(stderr, "Could not open %s for writing\n", out_path);
        free(image);
        return 1;
    }
    fprintf(f, "P5\n%d %d\n255\n", upek_image_width(), height);
    fwrite(image, 1, (size_t)upek_image_width() * height, f);
    fclose(f);
    free(image);

    printf("Wrote %s -- open it to sanity-check it looks like a fingerprint swipe.\n", out_path);
    return 0;
}
#endif
