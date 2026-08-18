/*
 * vfs5011_daemon.c
 *
 * Background authentication daemon: idles until the screen locks, polls
 * the sensor for a match while locked, and auto-types the stored
 * password on success. Shares the exact same capture/matcher/volume
 * code as hack_touchid_client.c — this is the CLI's proven pipeline wired to a
 * lock/unlock trigger instead of a menu.
 *
 * This is a STANDALONE TEST HARNESS for the daemon logic — not yet a
 * real installed LaunchDaemon (no plist, not backgrounded via launchd
 * yet). Run it directly in a terminal to validate the full lock ->
 * poll -> match -> auto-type -> unlock cycle before wrapping it in a
 * LaunchDaemon.
 *
 * IMPORTANT CAVEAT: macOS's Secure Input protection may block synthetic
 * CGEventPost keystrokes from reaching the lock screen's password
 * field specifically (this exists to stop exactly this kind of
 * synthetic injection from malware). This has NOT been confirmed
 * working on real hardware yet. If auto-type silently does nothing at
 * the lock screen, that's Secure Input blocking it, not a bug in this
 * code — the fallback would be a lower-level virtual HID device
 * instead of CGEventPost.
 *
 * Build:
 *   clang vfs5011_daemon.c vfs5011_matcher.c \
 *       nbis/mindtct/*.c nbis/bozorth3/*.c \
 *       -o vfs5011_daemon \
 *       -I. -Inbis/include \
 *       -I/usr/local/include/libusb-1.0 -L/usr/local/lib -lusb-1.0 \
 *       -framework CoreFoundation -framework ApplicationServices \
 *       -lm -lpthread \
 *       -Wno-implicit-function-declaration
 *
 * (build_daemon.sh runs this exact command.)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <time.h>
#include <ctype.h>
#include <dirent.h>
#include <libusb.h>
#include <libproc.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ApplicationServices/ApplicationServices.h>
#include <IOKit/IOKitLib.h>

#include "vfs5011_proto.h"
#include "vfs5011_matcher.h"
#include "vfs5011_menubar_ipc.h"

#define VFS5011_VID 0x138a
#define VFS5011_PID 0x0018

enum action_type { ACTION_SEND, ACTION_RECEIVE };
struct usb_action {
    enum action_type type;
    const char *name;
    int endpoint;
    int size;
    unsigned char *data;
    int correct_reply_size;
};

#define SEND(ENDPOINT, COMMAND) \
    { ACTION_SEND, #COMMAND, ENDPOINT, sizeof(COMMAND), COMMAND, 0 },
#define RECV(ENDPOINT, SIZE) \
    { ACTION_RECEIVE, "recv", ENDPOINT, SIZE, NULL, 0 },
#define RECV_CHECK(ENDPOINT, SIZE, EXPECTED) \
    { ACTION_RECEIVE, "recv_check", ENDPOINT, SIZE, EXPECTED, sizeof(EXPECTED) },

static struct usb_action vfs5011_initialization[] = {
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_cmd_01)
    RECV(VFS5011_IN_ENDPOINT_CTRL, 64)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_cmd_19)
    RECV(VFS5011_IN_ENDPOINT_CTRL, 64)
    RECV(VFS5011_IN_ENDPOINT_CTRL, 64)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_init_00)
    RECV(VFS5011_IN_ENDPOINT_CTRL, 64)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_init_01)
    RECV(VFS5011_IN_ENDPOINT_CTRL, 64)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_init_02)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_cmd_01)
    RECV(VFS5011_IN_ENDPOINT_CTRL, 64)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_cmd_1A)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_init_03)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_init_04)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    RECV(VFS5011_IN_ENDPOINT_DATA, 256)
    RECV(VFS5011_IN_ENDPOINT_DATA, 64)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_cmd_1A)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_init_05)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_cmd_01)
    RECV(VFS5011_IN_ENDPOINT_CTRL, 64)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_init_06)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    RECV(VFS5011_IN_ENDPOINT_DATA, 17216)
    RECV(VFS5011_IN_ENDPOINT_DATA, 32)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_init_07)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    RECV(VFS5011_IN_ENDPOINT_DATA, 45056)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_init_08)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    RECV(VFS5011_IN_ENDPOINT_DATA, 16896)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_init_09)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    RECV(VFS5011_IN_ENDPOINT_DATA, 4928)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_init_10)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    RECV(VFS5011_IN_ENDPOINT_DATA, 5632)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_init_11)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    RECV(VFS5011_IN_ENDPOINT_DATA, 5632)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_init_12)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    RECV(VFS5011_IN_ENDPOINT_DATA, 3328)
    RECV(VFS5011_IN_ENDPOINT_DATA, 64)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_init_13)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_cmd_1A)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_init_03)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_init_14)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    RECV(VFS5011_IN_ENDPOINT_DATA, 4800)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_cmd_1A)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_init_02)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_cmd_27)
    RECV(VFS5011_IN_ENDPOINT_CTRL, 64)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_cmd_1A)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_init_15)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_init_16)
    RECV(VFS5011_IN_ENDPOINT_CTRL, 2368)
    RECV(VFS5011_IN_ENDPOINT_CTRL, 64)
    RECV(VFS5011_IN_ENDPOINT_DATA, 4800)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_init_17)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_init_18)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
};

static struct usb_action vfs5011_initiate_capture[] = {
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_cmd_04)
    RECV(VFS5011_IN_ENDPOINT_DATA, 64)
    RECV(VFS5011_IN_ENDPOINT_DATA, 84032)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_cmd_1A)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_prepare_00)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_cmd_1A)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_prepare_01)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_prepare_02)
    RECV(VFS5011_IN_ENDPOINT_CTRL, 2368)
    RECV(VFS5011_IN_ENDPOINT_CTRL, 64)
    RECV(VFS5011_IN_ENDPOINT_DATA, 4800)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_prepare_03)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 64, VFS5011_NORMAL_CONTROL_REPLY)
    SEND(VFS5011_OUT_ENDPOINT, vfs5011_prepare_04)
    RECV_CHECK(VFS5011_IN_ENDPOINT_CTRL, 2368, VFS5011_NORMAL_CONTROL_REPLY)
};

/* Attempts a bulk transfer; on LIBUSB_ERROR_PIPE (stall left over from a
 * previous run, or a transient firmware hiccup), clears the halt on that
 * endpoint and retries exactly once before giving up. This is what lets
 * the program recover on its own instead of needing a manual rerun. */
static int bulk_transfer_with_pipe_retry(libusb_device_handle *handle, int endpoint,
                                          unsigned char *data, int size, int *transferred,
                                          unsigned int timeout) {
    int r = libusb_bulk_transfer(handle, endpoint, data, size, transferred, timeout);
    if (r == LIBUSB_ERROR_PIPE) {
        fprintf(stderr, "  (stall on endpoint 0x%02x, clearing halt and retrying)\n", endpoint);
        libusb_clear_halt(handle, endpoint);
        r = libusb_bulk_transfer(handle, endpoint, data, size, transferred, timeout);
    }
    return r;
}

static int run_sequence(libusb_device_handle *handle, struct usb_action *seq, int count) {
    unsigned char recv_buf[VFS5011_RECEIVE_BUF_SIZE];
    int r, transferred, i;
    for (i = 0; i < count; i++) {
        struct usb_action *a = &seq[i];
        if (a->type == ACTION_SEND) {
            r = bulk_transfer_with_pipe_retry(handle, a->endpoint, a->data, a->size,
                                               &transferred, VFS5011_DEFAULT_WAIT_TIMEOUT);
            if (r != 0 || transferred != a->size) {
                fprintf(stderr, "SEND failed at step %d (%s): %s\n", i + 1, a->name, libusb_error_name(r));
                return -1;
            }
        } else {
            r = bulk_transfer_with_pipe_retry(handle, a->endpoint, recv_buf, a->size,
                                               &transferred, VFS5011_DEFAULT_WAIT_TIMEOUT);
            if (r != 0) {
                fprintf(stderr, "RECV failed at step %d: %s\n", i + 1, libusb_error_name(r));
                return -1;
            }
            if (a->data != NULL) {
                if (transferred != a->correct_reply_size ||
                    memcmp(recv_buf, a->data, a->correct_reply_size) != 0) {
                    fprintf(stderr, "RECV_CHECK mismatch at step %d\n", i + 1);
                    return -1;
                }
            }
        }
    }
    return 0;
}

#define CAPTURE_LINES   256
#define MAX_LINES_TOTAL 2000
#define MAX_LINES_READ  100000
#define DEVIATION_THRESHOLD (15*15)
#define DIFFERENCE_THRESHOLD 600
#define STOP_CHECK_LINES 50
#define ASM_RESOLUTION 10
#define ASM_MEDIAN_FILTER_SIZE 25
#define ASM_MAX_SEARCH_OFFSET 30

static int get_deviation(unsigned char *buf, int size) {
    int mean = 0, res = 0, i;
    for (i = 0; i < size; i++) mean += buf[i];
    mean /= size;
    for (i = 0; i < size; i++) { int d = (int)buf[i] - mean; res += d * d; }
    return res / size;
}
static int get_diff_norm(unsigned char *a, unsigned char *b, int size) {
    int res = 0, i;
    for (i = 0; i < size; i++) { int d = (int)a[i] - (int)b[i]; res += d * d; }
    return res / size;
}
static unsigned char get_pixel(unsigned char *line, int x) { return line[8 + x]; }
static int get_deviation2(unsigned char *row1, unsigned char *row2) {
    unsigned char *buf1 = row1 + 56;
    unsigned char *buf2 = row2 + 168;
    const int size = 64;
    int mean = 0, res = 0, i;
    for (i = 0; i < size; i++) mean += (int)buf1[i] + (int)buf2[i];
    mean /= size;
    for (i = 0; i < size; i++) { int d = (int)buf1[i] + (int)buf2[i] - mean; res += d * d; }
    return res / size;
}
static int cmpint(const void *a, const void *b) { return (*(const int *)a) - (*(const int *)b); }
static void median_filter(int *data, int size, int filtersize) {
    int *result = calloc(size, sizeof(int));
    int *sortbuf = calloc(filtersize, sizeof(int));
    for (int i = 0; i < size; i++) {
        int i1 = i - (filtersize - 1) / 2, i2 = i + (filtersize - 1) / 2;
        if (i1 < 0) i1 = 0;
        if (i2 >= size) i2 = size - 1;
        memmove(sortbuf, data + i1, (size_t)(i2 - i1 + 1) * sizeof(int));
        qsort(sortbuf, i2 - i1 + 1, sizeof(int), cmpint);
        result[i] = sortbuf[(i2 - i1 + 1) / 2];
    }
    memmove(data, result, (size_t)size * sizeof(int));
    free(result); free(sortbuf);
}
static void interpolate_lines(unsigned char *line1, float y1, unsigned char *line2,
                               float y2, unsigned char *output, float yi, int size) {
    if (!line1 || !line2) return;
    for (int i = 0; i < size; i++) {
        unsigned char p1 = get_pixel(line1, i), p2 = get_pixel(line2, i);
        output[i] = (unsigned char)((float)p1 + (yi - y1) / (y2 - y1) * ((float)p2 - (float)p1));
    }
}
static unsigned char *assemble_lines(unsigned char *lines, int lines_len, int max_height, int *out_height) {
    int line_stride = VFS5011_LINE_SIZE, width = VFS5011_IMAGE_WIDTH;
    int *offsets = calloc((size_t)(lines_len / 2), sizeof(int));
    unsigned char *output = calloc((size_t)width * max_height, 1);
    float y = 0.0f; int line_ind = 0;
    for (int i = 0; i < lines_len - 1; i += 2) {
        int bestmatch = i, bestdiff = 0;
        int firstrow = i + 1;
        int lastrow = (i + ASM_MAX_SEARCH_OFFSET < lines_len - 1) ? i + ASM_MAX_SEARCH_OFFSET : lines_len - 1;
        for (int j = firstrow; j <= lastrow; j++) {
            int diff = get_deviation2(lines + (size_t)i * line_stride, lines + (size_t)j * line_stride);
            if (j == firstrow || diff < bestdiff) { bestdiff = diff; bestmatch = j; }
        }
        offsets[i / 2] = bestmatch - i;
    }
    int off_count = (lines_len / 2) - 1;
    if (off_count > 0) median_filter(offsets, off_count, ASM_MEDIAN_FILTER_SIZE);
    for (int i = 0; i < lines_len - 1; i++) {
        int offset = offsets[i / 2];
        unsigned char *row1 = lines + (size_t)i * line_stride;
        unsigned char *row2 = lines + (size_t)(i + 1) * line_stride;
        if (offset > 0) {
            float ynext = y + (float)ASM_RESOLUTION / (float)offset;
            while ((float)line_ind < ynext) {
                if (line_ind > max_height - 1) goto out;
                interpolate_lines(row1, y, row2, ynext, output + (size_t)line_ind * width, (float)line_ind, width);
                line_ind++;
            }
            y = ynext;
        }
    }
out:
    free(offsets);
    *out_height = line_ind;
    return output;
}

/* Runs the full pipeline: init -> initiate-capture -> swipe capture ->
 * alignment. Returns a malloc'd VFS5011_IMAGE_WIDTH x *out_height
 * grayscale buffer, or NULL on failure. Caller must libusb_init/open
 * the device and pass a claimed handle. */
static unsigned char *capture_fingerprint_image(libusb_device_handle *handle, int *out_height) {
    if (run_sequence(handle, vfs5011_initialization,
                      sizeof(vfs5011_initialization)/sizeof(vfs5011_initialization[0])) != 0) {
        fprintf(stderr, "Init sequence failed\n");
        return NULL;
    }
    if (run_sequence(handle, vfs5011_initiate_capture,
                      sizeof(vfs5011_initiate_capture)/sizeof(vfs5011_initiate_capture[0])) != 0) {
        fprintf(stderr, "Initiate-capture sequence failed\n");
        return NULL;
    }
    printf("Swipe your finger across the sensor now...\n");

    unsigned char *recorded = malloc((size_t)MAX_LINES_TOTAL * VFS5011_LINE_SIZE);
    int lines_recorded = 0, lines_captured = 0, empty_lines = 0;
    unsigned char *lastline = NULL;
    unsigned char *chunk_buf = malloc((size_t)CAPTURE_LINES * VFS5011_LINE_SIZE);
    int finished = 0, r;

    while (!finished) {
        int transferred = 0;
        r = libusb_bulk_transfer(handle, VFS5011_IN_ENDPOINT_DATA, chunk_buf,
                                  CAPTURE_LINES * VFS5011_LINE_SIZE, &transferred, 0);
        if (r != 0 && r != LIBUSB_ERROR_TIMEOUT) {
            fprintf(stderr, "Capture read failed: %s\n", libusb_error_name(r));
            break;
        }
        if (transferred <= 0) continue;
        int lines_in_chunk = transferred / VFS5011_LINE_SIZE;
        for (int i = 0; i < lines_in_chunk; i++) {
            unsigned char *line = chunk_buf + i * VFS5011_LINE_SIZE;
            if (get_deviation(line + 8, VFS5011_IMAGE_WIDTH) < DEVIATION_THRESHOLD) {
                if (lines_captured == 0) continue;
                empty_lines++;
            } else empty_lines = 0;
            if (empty_lines >= STOP_CHECK_LINES) { finished = 1; break; }
            lines_captured++;
            if (lines_captured > MAX_LINES_READ) { finished = 1; break; }
            if (lastline == NULL || get_diff_norm(lastline + 8, line + 8, VFS5011_IMAGE_WIDTH) >= DIFFERENCE_THRESHOLD) {
                if (lines_recorded >= MAX_LINES_TOTAL) { finished = 1; break; }
                lastline = recorded + (size_t)lines_recorded * VFS5011_LINE_SIZE;
                memcpy(lastline, line, VFS5011_LINE_SIZE);
                lines_recorded++;
            }
        }
    }
    free(chunk_buf);

    if (lines_recorded < 2) {
        fprintf(stderr, "Not enough lines captured (%d) — try a slower, fuller swipe.\n", lines_recorded);
        free(recorded);
        return NULL;
    }

    int height = 0;
    unsigned char *aligned = assemble_lines(recorded, lines_recorded, MAX_LINES_TOTAL, &height);
    free(recorded);

    if (height <= 0) {
        fprintf(stderr, "Alignment produced no output rows.\n");
        free(aligned);
        return NULL;
    }
    *out_height = height;
    return aligned;
}

static libusb_context *g_ctx = NULL;
static libusb_device_handle *g_handle = NULL;

static int open_device(void) {
    if (libusb_init(&g_ctx) < 0) return -1;
    libusb_set_option(g_ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_NONE);

    /* Right after a close_device() from a previous attempt, macOS
     * sometimes hasn't finished settling the device back into a
     * re-openable state yet — libusb_open_device_with_vid_pid can
     * transiently return NULL even though the device is still
     * physically present. Retry a few times with backoff before
     * treating it as a genuine "device not found". */
    for (int i = 0; i < 5; i++) {
        g_handle = libusb_open_device_with_vid_pid(g_ctx, VFS5011_VID, VFS5011_PID);
        if (g_handle) break;
        usleep(300000); /* 300ms between open attempts */
    }
    if (!g_handle) { fprintf(stderr, "Device not found\n"); return -1; }

    /* Tell libusb to forcibly detach whatever kernel driver has grabbed
     * this interface (common on macOS for HID-ish USB devices) BEFORE we
     * try to claim it. Without this, claim_interface loses the race
     * against the OS almost every time, and we were papering over that
     * with a full device reset on every single run — which was hurting
     * capture quality (device never fully settled before the swipe). */
    libusb_set_auto_detach_kernel_driver(g_handle, 1);

    if (libusb_claim_interface(g_handle, 0) == 0) return 0;

    /* Before resorting to a full device reset (which is known to hurt
     * capture quality on the next swipe), try a few quick re-claims —
     * a lot of "Claim failed" cases on macOS are IOKit holding the
     * interface exclusively for a brief moment right after enumeration
     * or after a previous close, and that clears on its own within a
     * few hundred ms without needing a disruptive reset. */
    for (int i = 0; i < 3; i++) {
        usleep(150000);
        if (libusb_claim_interface(g_handle, 0) == 0) return 0;
    }

    /* Still failed after quick retries — now fall back to reset. This
     * should be the rare case, not the common one. */
    fprintf(stderr, "Claim failed, resetting device and retrying...\n");
    int reset_r = libusb_reset_device(g_handle);
    if (reset_r != 0) {
        fprintf(stderr, "Device reset failed: %s\n", libusb_error_name(reset_r));
    }
    usleep(500000);

    if (libusb_claim_interface(g_handle, 0) != 0) {
        fprintf(stderr, "Claim failed again after reset\n");
        return -1;
    }
    return 0;
}
static void close_device(void) {
    if (g_handle) {
        /* Proactively clear any halt on the endpoints we use before
         * releasing, so the *next* run doesn't inherit a stalled pipe
         * from this session (this is what caused the PIPE error /
         * cascading Claim failed seen after a previous run). */
        libusb_clear_halt(g_handle, VFS5011_IN_ENDPOINT_CTRL);
        libusb_clear_halt(g_handle, VFS5011_IN_ENDPOINT_DATA);
        libusb_clear_halt(g_handle, VFS5011_OUT_ENDPOINT);
        libusb_release_interface(g_handle, 0);
        libusb_close(g_handle);
    }
    if (g_ctx) libusb_exit(g_ctx);
}

/* Cheap, non-invasive check for whether a VFS5011 is physically present
 * on this system at all. Uses its own short-lived libusb context and
 * just walks the device list comparing vendor/product IDs -- it never
 * opens or claims the device, so it's safe to call from
 * arm_polling_for_trigger() on every single trigger without any risk
 * of interfering with an in-flight capture_quality_template() open/
 * close cycle (which uses the separate g_ctx/g_handle globals).
 *
 * This exists so a screen lock or auth padlock on a machine with no
 * sensor at all doesn't fire a "swipe now" notification that can never
 * be satisfied, followed by an automatic failure a few seconds later,
 * forever, on every single lock. Checking fresh on every trigger
 * (rather than caching a startup-time result) also means a sensor
 * plugged in mid-session is picked up immediately, no daemon restart
 * needed. */
static bool vfs5011_sensor_is_present(void) {
    libusb_context *probe_ctx = NULL;
    if (libusb_init(&probe_ctx) < 0) {
        /* If libusb itself can't even initialize, don't block the
         * trigger on that -- let the real open_device() path in
         * capture_quality_template() surface the actual error. */
        return true;
    }
    libusb_set_option(probe_ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_NONE);

    libusb_device **list = NULL;
    ssize_t count = libusb_get_device_list(probe_ctx, &list);
    bool found = false;
    for (ssize_t i = 0; i < count; i++) {
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(list[i], &desc) != 0) continue;
        if (desc.idVendor == VFS5011_VID && desc.idProduct == VFS5011_PID) {
            found = true;
            break;
        }
    }
    if (list) libusb_free_device_list(list, 1);
    libusb_exit(probe_ctx);
    return found;
}

#define MATCH_THRESHOLD 20       /* testing lower vs. confirmed impostor ceiling of ~18 */
#define ENROLL_SWIPES 5          /* how many good swipes make up one enrollment */
#define MAX_STORED_TEMPLATES 8   /* array bound for save/load */
#define MIN_MINUTIAE 20          /* below this, a capture is too weak to trust */
#define MAX_SWIPE_RETRIES 3      /* re-prompt this many times before giving up on one swipe */
#define MIN_SELF_CONSISTENCY 15  /* a new enroll swipe must score at least this well against
                                    at least one already-saved swipe from this same session,
                                    or it's treated as an outlier capture and re-prompted */

/* Built-in macOS system sound, not a bundled asset -- keeps the repo
 * asset-free for open sourcing and works on every Mac out of the box.
 * "Basso" is the classic short error/failure tone. */
#define FAILURE_SOUND_PATH "/System/Library/Sounds/Basso.aiff"
/* Bright, distinct confirmation tone -- deliberately different from
 * the failure sound so the two are unmistakable from across a room,
 * not just on close listening. */
#define SUCCESS_SOUND_PATH "/System/Library/Sounds/Glass.aiff"

/* Fires a short audible cue on a rejected (below-threshold) swipe, so
 * a failed attempt gives the same kind of immediate feedback real
 * Touch ID gives (a buzz/reject tone) instead of silently waiting for
 * another swipe. Backgrounded with `&` so afplay's ~1s runtime never
 * blocks the polling loop from immediately trying the next swipe.
 * Runs in the same GUI session as the daemon (that's the whole reason
 * this is a LaunchAgent and not a LaunchDaemon -- see the top-of-file
 * notes on session-scoped notification delivery), so audio output
 * goes to whatever device the user is actually using. Best-effort:
 * if afplay isn't available or the sound file is missing for some
 * reason, this silently does nothing rather than treating a failed
 * *swipe* as a reason to fail the whole daemon. */
static void play_failure_sound(void) {
    int status = system("afplay " FAILURE_SOUND_PATH " > /dev/null 2>&1 &");
    (void)status; /* best-effort — a missing sound file shouldn't affect matching */
}

static void play_success_sound(void) {
    int status = system("afplay " SUCCESS_SOUND_PATH " > /dev/null 2>&1 &");
    (void)status; /* best-effort, same reasoning as play_failure_sound() */
}

/* Captures one swipe and extracts its template, re-prompting the user
 * up to MAX_SWIPE_RETRIES times if the capture comes back too weak
 * (too few minutiae) to be worth keeping.
 *
 * IMPORTANT: this opens and closes the device fresh for EVERY attempt.
 * Testing showed the sensor's internal state doesn't tolerate two
 * initiate-capture sequences back-to-back on the same open handle —
 * the second swipe stalls both endpoints and never recovers, even
 * with clear_halt. A full close+reopen between swipes is what was
 * actually working in the separate-process-per-swipe testing, so we
 * do that here automatically instead of relying on one long-lived
 * handle across multiple swipes. */
static int capture_quality_template(struct xyt_struct *out_tmpl) {
    for (int attempt = 1; attempt <= MAX_SWIPE_RETRIES; attempt++) {
        if (open_device() != 0) {
            close_device();
            fprintf(stderr, "Could not open device (attempt %d/%d)\n", attempt, MAX_SWIPE_RETRIES);
            usleep(500000);
            continue;
        }

        int height = 0;
        unsigned char *image = capture_fingerprint_image(g_handle, &height);
        if (!image) {
            close_device();
            fprintf(stderr, "Capture failed (attempt %d/%d)\n", attempt, MAX_SWIPE_RETRIES);
            usleep(500000);
            continue;
        }

        memset(out_tmpl, 0, sizeof(*out_tmpl));
        int r = vfs5011_extract_template(image, VFS5011_IMAGE_WIDTH, height, out_tmpl);
        free(image);
        close_device();

        if (r != 0) {
            fprintf(stderr, "Minutiae extraction failed (attempt %d/%d)\n", attempt, MAX_SWIPE_RETRIES);
            usleep(500000);
            continue;
        }
        if (out_tmpl->nrows < MIN_MINUTIAE) {
            fprintf(stderr, "Swipe too weak (%d minutiae, need %d) — swipe again, slower and fuller.\n",
                    out_tmpl->nrows, MIN_MINUTIAE);
            usleep(500000);
            continue;
        }
        return 0;
    }
    fprintf(stderr, "Gave up after %d weak/failed swipes.\n", MAX_SWIPE_RETRIES);
    return -1;
}

/* ------------------------------------------------------------------ *
 * VFS CLIENT — interactive menu shell
 *
 * Everything above this point is the untouched enroll/verify pipeline
 * from vfs5011_enroll_verify.c. Below is the menu wrapper: it calls
 * straight into capture_quality_template(), vfs5011_save_templates(),
 * vfs5011_load_templates(), and vfs5011_match_score() — no duplicated
 * capture logic, no reinvented constants.
 * ------------------------------------------------------------------ */

#define VFSC_RULE "--------------------------------------------------------------------"
#define TEMPLATE_FILENAME "template.dat"  /* legacy single-finger fallback only */
#define FINGERS_DIRNAME "fingers"
#define MOUNT_SCRIPT_NAME "vfs5011_volume_mount.sh"
#define UNMOUNT_SCRIPT_NAME "vfs5011_volume_unmount.sh"

/* Directory this binary is running from, resolved once at startup, so
 * the mount/unmount scripts can be found by absolute path regardless
 * of the caller's current working directory. */
static char g_exec_dir[PATH_MAX];

static void init_exec_dir(const char *argv0) {
    char resolved[PATH_MAX];
    if (realpath(argv0, resolved) == NULL) {
        /* Fall back to argv0 as-is if realpath fails (unusual, but
         * don't crash the whole menu over a cosmetic path lookup). */
        strncpy(resolved, argv0, sizeof(resolved) - 1);
        resolved[sizeof(resolved) - 1] = '\0';
    }
    char *dir = dirname(resolved); /* may alias into `resolved` — copy immediately */
    strncpy(g_exec_dir, dir, sizeof(g_exec_dir) - 1);
    g_exec_dir[sizeof(g_exec_dir) - 1] = '\0';
}

/* Cached template count for the status line. NOT re-checked on every
 * menu redraw on purpose — the templates volume is unmounted at rest,
 * and mounting it just to paint a status line would mean mounting
 * constantly while someone sits at the menu, defeating the point of
 * per-operation mounting. Enroll/Verify update this for free as a
 * side effect since they already have the volume mounted anyway. */
static int g_cached_template_count = -1; /* -1 = not checked yet this session */

/* Mounts the encrypted template volume via vfs5011_volume_mount.sh,
 * capturing the mount point path it prints on success. Echoes the
 * script's own diagnostic lines so the behavior looks the same as
 * running it directly. Returns 0 and fills out_path on success. */
static int mount_template_volume(char *out_path, size_t out_path_size) {
    char cmd[PATH_MAX * 2];
    snprintf(cmd, sizeof(cmd), "\"%s/%s\"", g_exec_dir, MOUNT_SCRIPT_NAME);

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        fprintf(stderr, "Failed to run volume mount script: %s\n", strerror(errno));
        return -1;
    }

    char line[PATH_MAX];
    char last_line[PATH_MAX] = {0};
    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';
        if (len > 0) {
            strncpy(last_line, line, sizeof(last_line) - 1);
            last_line[sizeof(last_line) - 1] = '\0';
            printf("  %s\n", line);
        }
    }
    int status = pclose(fp);

    /* The script's last printed line is the mount path on success — a
     * plain absolute path starting with '/'. Anything else (empty, an
     * error message, non-zero exit) means mounting failed. */
    if (status != 0 || last_line[0] != '/') {
        fprintf(stderr, "Volume mount failed (exit status %d).\n", status);
        return -1;
    }
    strncpy(out_path, last_line, out_path_size - 1);
    out_path[out_path_size - 1] = '\0';
    return 0;
}

static void unmount_template_volume(void) {
    char cmd[PATH_MAX * 2];
    snprintf(cmd, sizeof(cmd), "\"%s/%s\"", g_exec_dir, UNMOUNT_SCRIPT_NAME);
    int status = system(cmd);
    if (status != 0) {
        fprintf(stderr,
                "Warning: volume unmount script exited with status %d — the volume may "
                "still be mounted. Run vfs5011_volume_unmount.sh manually to check.\n",
                status);
    }
}

/* ------------------------------------------------------------------ *
 * Daemon logic: lock/unlock state machine, sensor polling, auto-type
 * ------------------------------------------------------------------ */

#define PASSWORD_FILENAME "password.txt"
#define POLL_RETRY_DELAY_USEC 400000  /* 400ms between failed swipes while polling */
#define MAX_PASSWORD_LEN 256

typedef enum {
    STATE_IDLE,
    STATE_POLLING
} daemon_state_t;

static atomic_int g_state = STATE_IDLE;
static atomic_int g_running = 1;

/* ------------------------------------------------------------------ *
 * Auth-prompt watcher: extends the same lock/poll/type pipeline to
 * fire on one more trigger besides the lock screen --
 *
 *   TRIGGER_PADLOCK -- a SecurityAgent/authorizationhost dialog
 *                       (System Settings padlocks, installer auth,
 *                       etc) appeared with a secure text field.
 *                       Detected via the same 300ms CFRunLoopTimer
 *                       poll used for the lock screen, checking
 *                       whatever currently has system-wide keyboard
 *                       focus.
 *
 * (A terminal-sudo trigger -- detecting a "Password:" prompt in a
 * frontmost terminal emulator's scrollback -- was designed and built
 * alongside this, but deliberately removed: not a priority right now,
 * and diagnosing it surfaced a real AX API issue (see ax_probe.c
 * findings) that also affects this padlock path, since both rely on
 * the same AXUIElementCopyAttributeValue calls to read the focused
 * element.)
 *
 * Reuses load_templates_for_episode(), polling_thread_main()'s
 * capture/match loop, and type_password_and_enter() completely
 * unchanged -- CGEventPost always goes to whatever currently has
 * keyboard focus, so no new typing logic is needed, only new
 * *detection* logic plus a refocus safety check right before typing
 * (see arm_polling_for_trigger and the success branch in
 * polling_thread_main). */
typedef enum {
    TRIGGER_NONE,
    TRIGGER_LOCK_SCREEN,
    TRIGGER_PADLOCK
} trigger_source_t;

static atomic_int g_trigger_source = TRIGGER_NONE;

/* pid that must still be focused/frontmost right before we type -- if
 * the user alt-tabbed away mid-swipe, we abort instead of typing into
 * whatever now has focus. Not used for TRIGGER_LOCK_SCREEN (there's
 * only ever one thing focused at the lock screen). */
static pid_t g_trigger_target_pid = 0;

/* Only populated for TRIGGER_PADLOCK -- the specific secure field to
 * focus right before typing, found once at detection time so we don't
 * have to re-walk the AX tree at match time. Retained on set, released
 * when the episode ends. */
static AXUIElementRef g_trigger_secure_field = NULL;

/* Loaded once per lock episode (in on_screen_locked, while the volume
 * is briefly mounted), then matched against repeatedly in memory while
 * polling -- so the volume does NOT stay mounted for the whole time
 * the screen is locked, only for the brief load. */
static struct xyt_struct g_enrolled_templates[MAX_STORED_TEMPLATES];
static int g_enrolled_count = 0;

/* Cached alongside the templates during the same lock-time mount, so a
 * match doesn't require a second mount/unmount round trip just to read
 * password.txt. Lives only as long as g_enrolled_templates does --
 * populated in load_templates_for_episode(), zeroed in
 * on_screen_unlocked(). Same trust model as before: still only ever
 * sits in root process memory, just for one lock episode instead of a
 * few hundred extra ms at match time. */
static char g_cached_password[MAX_PASSWORD_LEN + 1];
static int g_cached_password_valid = 0;

/* g_enrolled_templates / g_enrolled_count / g_cached_password /
 * g_cached_password_valid are now written by a dedicated loader thread
 * (see load_templates_thread_main) instead of inline on the CFRunLoop
 * thread that detects the trigger, so STATE_POLLING can start (and the
 * sensor can light up) immediately instead of waiting on the encrypted
 * volume's mount/unmount. That means those four globals now have two
 * potential writers -- the loader thread, and on_screen_unlocked()
 * clearing them if the screen unlocks mid-load -- plus one reader
 * (polling_thread_main). g_templates_lock protects all four; nothing
 * about the mount/unmount itself needs the lock, only the brief writes
 * into these arrays/fields.
 *
 * g_templates_ready is set true by the loader thread only after it has
 * finished writing under the lock, and is otherwise read/reset with
 * plain atomics. Default sequentially-consistent atomic ordering here
 * (same idiom already used for g_state elsewhere in this file) means a
 * thread that observes g_templates_ready == true is guaranteed to see
 * every write the loader made before setting it, without needing the
 * mutex held for that check itself. */
static pthread_mutex_t g_templates_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool g_templates_ready = false;

static void print_timestamp(void) {
    CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
    CFDateFormatterRef fmt = CFDateFormatterCreate(NULL, CFLocaleCopyCurrent(),
                                                    kCFDateFormatterNoStyle,
                                                    kCFDateFormatterMediumStyle);
    CFDateRef date = CFDateCreate(NULL, now);
    CFStringRef str = CFDateFormatterCreateStringWithDate(NULL, fmt, date);
    char buf[64];
    CFStringGetCString(str, buf, sizeof(buf), kCFStringEncodingUTF8);
    printf("[%s] ", buf);
    CFRelease(str);
    CFRelease(date);
    CFRelease(fmt);
}

/* Types a UTF-8 string via synthetic keyboard events, then presses
 * Return. Uses CGEventKeyboardSetUnicodeString rather than per-key
 * virtual keycodes, so it doesn't need to know the active keyboard
 * layout -- it injects the literal characters directly.
 *
 * SEE FILE-HEADER CAVEAT: this may be blocked by Secure Input at the
 * lock screen. This function will report success/failure of the
 * *posting* call, which is NOT the same as confirming the OS actually
 * accepted it into the password field -- that can only be confirmed
 * by watching whether the screen actually unlocks. */
static void type_password_and_enter(const char *password) {
    size_t len = strlen(password);
    if (len == 0 || len > MAX_PASSWORD_LEN) {
        fprintf(stderr, "Refusing to type password: invalid length (%zu).\n", len);
        return;
    }

    UniChar unichars[MAX_PASSWORD_LEN];
    for (size_t i = 0; i < len; i++) {
        unichars[i] = (UniChar)(unsigned char)password[i]; /* ASCII passwords only */
    }

    CGEventRef key_down = CGEventCreateKeyboardEvent(NULL, 0, true);
    CGEventKeyboardSetUnicodeString(key_down, (UniCharCount)len, unichars);
    CGEventPost(kCGSessionEventTap, key_down);
    CFRelease(key_down);

    CGEventRef key_up = CGEventCreateKeyboardEvent(NULL, 0, false);
    CGEventKeyboardSetUnicodeString(key_up, (UniCharCount)len, unichars);
    CGEventPost(kCGSessionEventTap, key_up);
    CFRelease(key_up);

    /* Small delay before Return -- typing the whole password as one
     * event and immediately submitting can race the field's own
     * internal handling on some macOS versions. */
    usleep(150000);

    CGEventRef return_down = CGEventCreateKeyboardEvent(NULL, (CGKeyCode)0x24 /* kVK_Return */, true);
    CGEventPost(kCGSessionEventTap, return_down);
    CFRelease(return_down);

    CGEventRef return_up = CGEventCreateKeyboardEvent(NULL, (CGKeyCode)0x24, false);
    CGEventPost(kCGSessionEventTap, return_up);
    CFRelease(return_up);
}

/* ------------------------------------------------------------------ *
 * Accessibility helpers used by the padlock watcher
 * ------------------------------------------------------------------ */

/* Looks up a process's name (e.g. "SecurityAgent") via libproc -- no
 * Cocoa/NSRunningApplication needed. Returns 0 on success. */
static int get_process_name(pid_t pid, char *out, size_t out_size) {
    char buf[PROC_PIDPATHINFO_MAXSIZE];
    if (proc_name(pid, buf, sizeof(buf)) <= 0) return -1;
    strncpy(out, buf, out_size - 1);
    out[out_size - 1] = '\0';
    return 0;
}

/* Looks up a process's full executable path via libproc. Returns 0 on
 * success. Used to catch System Settings pane extensions by location
 * rather than by name -- see is_system_auth_process's comment. */
static int get_process_path(pid_t pid, char *out, size_t out_size) {
    char buf[PROC_PIDPATHINFO_MAXSIZE];
    if (proc_pidpath(pid, buf, sizeof(buf)) <= 0) return -1;
    strncpy(out, buf, out_size - 1);
    out[out_size - 1] = '\0';
    return 0;
}

/* Returns the frontmost on-screen window's owning PID by walking the
 * window list front-to-back and taking the first real (layer-0,
 * non-desktop) window's owner. Doesn't touch Accessibility at all --
 * no TCC grant needed for this part; only window titles are gated
 * behind Screen Recording on modern macOS, and we don't read titles. */
static pid_t get_frontmost_window_owner_pid(void) {
    CFArrayRef window_list = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
        kCGNullWindowID);
    if (!window_list) return 0;

    pid_t result_pid = 0;
    CFIndex count = CFArrayGetCount(window_list);
    for (CFIndex i = 0; i < count; i++) {
        CFDictionaryRef entry = (CFDictionaryRef)CFArrayGetValueAtIndex(window_list, i);
        CFNumberRef layer_num = (CFNumberRef)CFDictionaryGetValue(entry, kCGWindowLayer);
        int layer = -1;
        if (layer_num) CFNumberGetValue(layer_num, kCFNumberIntType, &layer);
        if (layer != 0) continue;

        CFNumberRef pid_num = (CFNumberRef)CFDictionaryGetValue(entry, kCGWindowOwnerPID);
        if (!pid_num) continue;
        int pid_val = 0;
        CFNumberGetValue(pid_num, kCFNumberIntType, &pid_val);
        result_pid = (pid_t)pid_val;
        break;
    }
    CFRelease(window_list);
    return result_pid;
}

/* Fills candidate_pids (capacity max_count) with every currently-live
 * process whose executable lives under a given app bundle's PlugIns
 * directory -- i.e. its active pane extensions. Returns the count
 * found. Used because a System Settings pane's actual keyboard focus
 * and AX tree live in its extension process, not in "System Settings"
 * itself, and there's no reliable window-based way to discover which
 * extension is currently active -- but there are only ever a couple
 * running at once, so just checking each one directly is cheap. */
static int find_live_pids_under_path(const char *path_substring, pid_t *candidate_pids, int max_count) {
    int found = 0;
    int num_pids = proc_listallpids(NULL, 0);
    if (num_pids <= 0) return 0;
    pid_t *all_pids = malloc(sizeof(pid_t) * (num_pids + 64));
    if (!all_pids) return 0;
    num_pids = proc_listallpids(all_pids, sizeof(pid_t) * (num_pids + 64));

    for (int i = 0; i < num_pids && found < max_count; i++) {
        char path_buf[1024];
        if (proc_pidpath(all_pids[i], path_buf, sizeof(path_buf)) <= 0) continue;
        if (strstr(path_buf, path_substring) != NULL) {
            candidate_pids[found++] = all_pids[i];
        }
    }
    free(all_pids);
    return found;
}

/* Returns the AX application element (CFRetain'd -- caller releases)
 * for whichever process is most likely to hold the currently-relevant
 * auth prompt, or NULL if nothing auth-relevant is in play this tick.
 *
 * Historically this avoided AXUIElementCreateSystemWide() +
 * kAXFocusedApplicationAttribute entirely -- confirmed, via live
 * [DIAG] logging in this exact daemon, to fail unconditionally with
 * kAXErrorCannotComplete on this hardware for the padlock/System
 * Settings cases. Direct by-PID AXUIElementCreateApplication() calls
 * are confirmed reliable (see ax_probe.c's Dock control test), so the
 * candidate list was built a different way:
 *   1. The frontmost window's owning PID (via CGWindowList) --
 *      correctly covers SecurityAgent, authorizationhost, coreauthd,
 *      loginwindow, and non-extension-hosted System Settings/System
 *      Preferences windows directly.
 *   2. If that owner is System Settings or System Preferences, ALSO
 *      every live PlugInKit extension process under its bundle --
 *      correctly covers extension-hosted panes (e.g. Users & Groups),
 *      where the window owner and the actually-focused process are
 *      different PIDs entirely.
 *
 * v1.0.2 addition (Keychain-access consent dialogs, e.g. Keychain
 * Access's "wants to use your confidential information" alert, owned
 * by the coreautha process): confirmed via ax_probe.c that this exact
 * dialog does NOT show up in the layer-0 CGWindowList scan at all --
 * source (1) above returns nothing for it -- but the system-wide
 * focused-application lookup DOES resolve it correctly, contradicting
 * the historical finding above for this specific dialog type. Rather
 * than replace the window-based approach (still the more reliable
 * source for the padlock/Settings/Passwords cases already confirmed
 * working), this adds source (3): a best-effort system-wide
 * focused-app lookup, appended as an extra candidate if it succeeds
 * and isn't already in the list. If it fails (kAXErrorCannotComplete
 * or otherwise), that's silently ignored and sources (1)/(2) still
 * apply as before -- this is additive, not a replacement, since we
 * don't yet know if it behaves identically once running under launchd
 * as root versus this interactive sudo ax_probe test.
 *
 * The caller checks each candidate against the auth-process allowlist
 * and only proceeds with ones that pass. */
static int get_auth_candidate_pids(pid_t *candidate_pids, int max_count) {
    if (max_count < 1) return 0;
    int count = 0;

    pid_t frontmost = get_frontmost_window_owner_pid();
    if (frontmost != 0) {
        candidate_pids[count++] = frontmost;

        char name_buf[256];
        if (count < max_count && get_process_name(frontmost, name_buf, sizeof(name_buf)) == 0
            && (strcmp(name_buf, "System Settings") == 0 || strcmp(name_buf, "System Preferences") == 0)) {
            pid_t ext_pids[8];
            int ext_count = find_live_pids_under_path("System Settings.app/Contents/PlugIns/", ext_pids, 8);
            if (ext_count == 0) {
                ext_count = find_live_pids_under_path("System Preferences.app/Contents/PlugIns/", ext_pids, 8);
            }
            for (int i = 0; i < ext_count && count < max_count; i++) {
                candidate_pids[count++] = ext_pids[i];
            }
        }
    }

    /* Source (3): system-wide focused application, best-effort. Covers
     * alert-style dialogs (confirmed: coreautha's Keychain-access
     * consent alert) that never appear in the layer-0 window list. */
    if (count < max_count) {
        AXUIElementRef system_wide = AXUIElementCreateSystemWide();
        AXUIElementRef focused_app = NULL;
        AXError err = AXUIElementCopyAttributeValue(system_wide, kAXFocusedApplicationAttribute,
                                                      (CFTypeRef *)&focused_app);
        CFRelease(system_wide);

        if (err == kAXErrorSuccess && focused_app) {
            pid_t focused_pid = 0;
            AXUIElementGetPid(focused_app, &focused_pid);
            CFRelease(focused_app);

            if (focused_pid != 0) {
                bool already_listed = false;
                for (int i = 0; i < count; i++) {
                    if (candidate_pids[i] == focused_pid) { already_listed = true; break; }
                }
                if (!already_listed) candidate_pids[count++] = focused_pid;
            }
        }
        /* err != kAXErrorSuccess is expected/normal in many cases (see
         * comment above) -- not logged here to avoid spamming the log
         * every 300ms tick when nothing auth-relevant is focused. */
    }

    return count;
}

/* Loads every enrolled finger's templates into memory for the duration
 * of one lock episode. Mounts/unmounts once, not per-swipe.
 *
 * The client enrolls fingers into fingers/<label>.dat (one file per
 * named finger, each holding up to MAX_STORED_TEMPLATES swipes) so
 * multiple fingers can be enrolled and matched against. This scans
 * that directory and pools every finger's templates into one flat
 * array for matching, up to the array's total capacity.
 *
 * NOTE: g_enrolled_templates is sized MAX_STORED_TEMPLATES (8), the
 * same constant used as the per-finger swipe cap. With only one
 * enrolled finger (<=8 templates) this is fine, but enrolling several
 * fingers can exceed total capacity -- extras are silently dropped
 * (see the count>=MAX_STORED_TEMPLATES check in the loop below). If
 * more than one finger gets enrolled, bump g_enrolled_templates to a
 * larger fixed size (e.g. MAX_ENROLLED_FINGERS * MAX_STORED_TEMPLATES)
 * so nothing gets silently dropped. */
static int load_templates_for_episode(void) {
    char mount_path[PATH_MAX];
    if (mount_template_volume(mount_path, sizeof(mount_path)) != 0) {
        fprintf(stderr, "Could not mount volume to load templates for this lock episode.\n");
        return -1;
    }

    /* Lock held from here through the password read below -- covers
     * every write into g_enrolled_templates/g_enrolled_count/
     * g_cached_password/g_cached_password_valid, but NOT the
     * mount_template_volume()/unmount_template_volume() calls
     * themselves, so on_screen_unlocked() (which takes the same lock
     * only for its own brief zeroing) is never blocked on the
     * multi-second disk+crypto mount/unmount round trip -- only ever
     * on these fast in-memory writes. */
    pthread_mutex_lock(&g_templates_lock);

    char fingers_dir[PATH_MAX];
    snprintf(fingers_dir, sizeof(fingers_dir), "%s/%s", mount_path, FINGERS_DIRNAME);

    g_enrolled_count = 0;
    int rc = -1;

    DIR *d = opendir(fingers_dir);
    if (d) {
        struct dirent *entry;
        while ((entry = readdir(d)) != NULL && g_enrolled_count < MAX_STORED_TEMPLATES) {
            size_t len = strlen(entry->d_name);
            if (len <= 4 || strcmp(entry->d_name + len - 4, ".dat") != 0) continue;

            char finger_path[PATH_MAX];
            snprintf(finger_path, sizeof(finger_path), "%s/%s", fingers_dir, entry->d_name);

            struct xyt_struct finger_templates[MAX_STORED_TEMPLATES];
            int finger_count = 0;
            if (vfs5011_load_templates(finger_path, finger_templates, MAX_STORED_TEMPLATES, &finger_count) != 0) {
                fprintf(stderr, "Warning: could not load templates from %s, skipping.\n", entry->d_name);
                continue;
            }

            for (int i = 0; i < finger_count && g_enrolled_count < MAX_STORED_TEMPLATES; i++) {
                g_enrolled_templates[g_enrolled_count++] = finger_templates[i];
            }
            rc = 0; /* loaded at least one finger's templates */
        }
        closedir(d);
    }

    /* Legacy fallback: pre-multi-finger installs may still have a flat
     * template.dat sitting in the volume root instead of fingers/. */
    if (rc != 0) {
        char template_path[PATH_MAX];
        snprintf(template_path, sizeof(template_path), "%s/%s", mount_path, TEMPLATE_FILENAME);
        rc = vfs5011_load_templates(template_path, g_enrolled_templates, MAX_STORED_TEMPLATES, &g_enrolled_count);
    }

    /* Piggyback the password read onto this same mount, so match time
     * only has to touch the sensor -- not the volume. Failure to find a
     * stored password doesn't block polling; it just means a later
     * match will report "no password available" same as before. */
    g_cached_password_valid = 0;
    char password_path[PATH_MAX];
    snprintf(password_path, sizeof(password_path), "%s/%s", mount_path, PASSWORD_FILENAME);
    FILE *pf = fopen(password_path, "r");
    if (pf) {
        size_t n = fread(g_cached_password, 1, MAX_PASSWORD_LEN, pf);
        g_cached_password[n] = '\0';
        fclose(pf);
        g_cached_password_valid = 1;
    } else {
        fprintf(stderr, "No stored password found (run vfs5011_store_password.sh first).\n");
    }

    int enrolled_count = g_enrolled_count; /* snapshot before unlocking, for the check below */
    pthread_mutex_unlock(&g_templates_lock);

    unmount_template_volume();

    if (rc != 0 || enrolled_count == 0) {
        fprintf(stderr, "No enrolled templates available -- polling will not start.\n");
        pthread_mutex_lock(&g_templates_lock);
        g_enrolled_count = 0;
        pthread_mutex_unlock(&g_templates_lock);
        return -1;
    }
    return 0;
}

/* Runs load_templates_for_episode() off the CFRunLoop thread so
 * arm_polling_for_trigger() can enter STATE_POLLING and notify the
 * user immediately, instead of the encrypted volume's mount/unmount
 * sitting in the critical path before the sensor even lights up. Only
 * responsibility beyond calling that function: flip g_templates_ready
 * on success, or unwind back to STATE_IDLE (and tell the menu bar app
 * this episode didn't pan out) on failure -- mirroring what
 * arm_polling_for_trigger used to do inline when loading failed. */
static void *load_templates_thread_main(void *arg) {
    (void)arg;

    if (load_templates_for_episode() == 0) {
        atomic_store(&g_templates_ready, true);
    } else {
        /* Only actually meaningful if we're still mid-episode -- if the
         * screen was already unlocked while this was loading,
         * on_screen_unlocked() has already put us back in STATE_IDLE
         * and there's nothing left to unwind. */
        if (atomic_load(&g_state) == STATE_POLLING) {
            print_timestamp();
            printf("Staying IDLE (no templates available).\n");
            atomic_store(&g_state, STATE_IDLE);
            atomic_store(&g_trigger_source, TRIGGER_NONE);
            if (g_trigger_secure_field) { CFRelease(g_trigger_secure_field); g_trigger_secure_field = NULL; }
            vfs5011_notify_swipe_failed(); /* we already told them to swipe -- let the menu bar app walk that back */
        }
    }
    return NULL;
}

/* Forward declaration -- defined further down alongside the padlock
 * watcher, but on_screen_locked (right here) needs to call it too. */
static void arm_polling_for_trigger(trigger_source_t source, pid_t target_pid,
                                     AXUIElementRef secure_field, const char *label);

static void on_screen_locked(void) {
    if (atomic_load(&g_state) != STATE_IDLE) return; /* shouldn't happen, but don't interrupt an episode */
    arm_polling_for_trigger(TRIGGER_LOCK_SCREEN, 0, NULL, "Screen lock");
}

static void on_screen_unlocked(void) {
    atomic_store(&g_state, STATE_IDLE);
    atomic_store(&g_trigger_source, TRIGGER_NONE);
    if (g_trigger_secure_field) { CFRelease(g_trigger_secure_field); g_trigger_secure_field = NULL; }

    /* g_enrolled_templates/g_enrolled_count/g_cached_password/
     * g_cached_password_valid can now also be mid-write on the loader
     * thread -- someone can unlock (trackpad, mouse click) before the
     * encrypted volume even finishes mounting. Same lock the loader
     * thread holds while writing these, so this either runs cleanly
     * before or after that write, never torn in the middle of it. */
    pthread_mutex_lock(&g_templates_lock);
    g_enrolled_count = 0; /* don't keep templates resident in memory once idle */
    memset(g_cached_password, 0, sizeof(g_cached_password)); /* don't leave it sitting in memory either */
    g_cached_password_valid = 0;
    pthread_mutex_unlock(&g_templates_lock);
    atomic_store(&g_templates_ready, false); /* a stale "ready" must never leak into the next episode */

    print_timestamp();
    printf("Screen UNLOCKED -> entering IDLE state.\n");
}

/* Shared by both triggers: loads templates for one episode and flips
 * into POLLING, tagged with which trigger armed it and (for the
 * padlock trigger) what must still be focused right before we type. */
static void arm_polling_for_trigger(trigger_source_t source, pid_t target_pid,
                                     AXUIElementRef secure_field, const char *label) {
    if (!vfs5011_scanning_is_enabled()) {
        /* Paused via the menu bar. Don't wake the sensor, don't load
         * templates, don't post swipe_requested -- stay IDLE so the
         * next auth-prompt detection (after a resume) re-triggers this
         * function fresh rather than needing a new prompt to appear. */
        print_timestamp();
        printf("%s detected, but scanning is paused -- staying IDLE.\n", label);
        atomic_store(&g_state, STATE_IDLE);
        if (secure_field) CFRelease(secure_field);
        return;
    }

    if (!vfs5011_sensor_is_present()) {
        /* No VFS5011 physically attached. Without this check we'd fire
         * vfs5011_notify_swipe_requested() below, the user would see
         * "Swipe to authenticate!" with nothing to swipe, and it would
         * fail a few seconds later when capture_quality_template()'s
         * own open_device() attempts all come back empty -- repeating
         * on every single lock. Bail out here instead, before any
         * notification goes out. */
        print_timestamp();
        printf("%s detected, but no VFS5011 sensor found on this system -- staying IDLE.\n", label);
        atomic_store(&g_state, STATE_IDLE);
        if (secure_field) CFRelease(secure_field);
        return;
    }

    g_trigger_target_pid = target_pid;
    if (g_trigger_secure_field) { CFRelease(g_trigger_secure_field); g_trigger_secure_field = NULL; }
    g_trigger_secure_field = secure_field; /* already retained by caller */
    atomic_store(&g_trigger_source, source);

    /* Enter POLLING and notify right away -- the sensor lights up and
     * the user sees "swipe now" immediately, instead of the encrypted
     * volume's mount/unmount sitting in the critical path first (that
     * round trip alone was the 3-5 second delay users were seeing
     * before the sensor even lit up). Templates load on their own
     * thread in the background (load_templates_thread_main);
     * polling_thread_main keeps the sensor warm and holds any captured
     * swipe until g_templates_ready flips true before scoring it. */
    atomic_store(&g_templates_ready, false);
    atomic_store(&g_state, STATE_POLLING);
    print_timestamp();
    printf("%s detected -> entering POLLING state, loading templates in background...\n", label);
    vfs5011_notify_swipe_requested();

    pthread_t loader_thread;
    if (pthread_create(&loader_thread, NULL, load_templates_thread_main, NULL) != 0) {
        /* Couldn't even start the loader thread -- unwind exactly like
         * the old inline failure path did, plus walk back the
         * swipe_requested we already sent. */
        print_timestamp();
        printf("Could not start template loader thread -- staying IDLE.\n");
        atomic_store(&g_state, STATE_IDLE);
        atomic_store(&g_trigger_source, TRIGGER_NONE);
        if (g_trigger_secure_field) { CFRelease(g_trigger_secure_field); g_trigger_secure_field = NULL; }
        vfs5011_notify_swipe_failed();
        return;
    }
    pthread_detach(loader_thread);
}

/* Process names that legitimately ask for the macOS LOGIN password via
 * a secure field -- as opposed to some arbitrary app's own unrelated
 * password prompt (a website login form, a mail account password, a
 * VPN client, etc), which we must never auto-type the login password
 * into. "System Settings" replaced "SecurityAgent" for most padlocks
 * as of the redesigned Settings app -- but as of the PlugInKit-based
 * pane architecture, the padlock sheet's actual keyboard focus lives
 * in that pane's own extension PROCESS, not the "System Settings"
 * host process itself. Each pane's extension has its own distinct
 * name (e.g. "UsersGroups", "GeneralSettings", "PrivacySecurity") --
 * too many, and too likely to change/add more, to hardcode by name.
 * Instead, check the executable's actual path: any pane extension
 * lives under System Settings.app's own PlugIns directory, regardless
 * of what it's individually called. */
static bool is_system_auth_process(const char *name, const char *path) {
    static const char *allowlist[] = {
        "SecurityAgent", "authorizationhost", "coreauthd",
        "System Settings", "System Preferences", "loginwindow",
        /* Passwords.app (Sequoia+) -- its own lock-gate password field,
         * confirmed via ax_probe.c to be a distinct process (NOT
         * SecurityAgent), reporting a plain AXTextField/AXSecureTextField
         * that hits the same AXError -25205 "no focused UI element" as
         * System Settings panes -- already covered by the existing
         * find_secure_text_field tree-walk fallback below, so no new
         * detection logic was needed, just this allowlist entry. */
        "Passwords",
        /* coreautha (LocalAuthentication.framework/Support/coreautha.bundle)
         * -- distinct from coreauthd above, NOT a typo. Owns Keychain
         * Access's "wants to use your confidential information stored
         * in keychain" consent alert. Confirmed via ax_probe.c; only
         * discoverable via the system-wide focused-app fallback in
         * get_auth_candidate_pids (source 3), since this alert never
         * appears in the layer-0 window list. */
        "coreautha"
    };
    for (size_t i = 0; i < sizeof(allowlist) / sizeof(allowlist[0]); i++) {
        if (strcmp(name, allowlist[i]) == 0) return true;
    }
    if (path && (strstr(path, "/System Settings.app/Contents/PlugIns/") != NULL
                 || strstr(path, "/System Preferences.app/Contents/PlugIns/") != NULL)) {
        return true;
    }
    return false;
}

/* Like get_focused_element_role_and_text, but also hands back the
 * element itself (retained) so the padlock path can keep it around to
 * re-focus right before typing. */
/* Fallback for windows that don't support kAXFocusedUIElementAttribute
 * (observed on modern System Settings panes -- e.g. Users & Groups --
 * which are rendered by a separate PlugInKit extension process via a
 * remote view; that window returns kAXErrorAttributeUnsupported for
 * "what's focused" even though the window and its children are
 * otherwise perfectly queryable). Walks the AX tree looking for the
 * first AXSecureTextField, since that's specifically what we're after
 * here -- not a general "find whatever's focused" replacement.
 * Capped by both depth and total node budget so a pathological view
 * hierarchy can't make this poll callback hang. Returns a retained
 * element the caller must release, or NULL. */
static AXUIElementRef find_secure_text_field(AXUIElementRef element, int depth, int *budget) {
    if (!element || depth <= 0 || *budget <= 0) return NULL;
    (*budget)--;

    /* NOTE: secure fields in these SwiftUI-hosted System Settings
     * sheets report role=AXTextField with subrole=AXSecureTextField --
     * NOT role=AXSecureTextField directly, despite that being the
     * commonly-assumed check (and what every version of this function
     * checked until a full AX tree dump proved otherwise). Check
     * subrole; fall back to role for compatibility with any other
     * context that genuinely does use it as the role. */
    CFStringRef subrole = NULL, role = NULL;
    AXUIElementCopyAttributeValue(element, kAXSubroleAttribute, (CFTypeRef *)&subrole);
    bool is_secure = subrole && CFStringCompare(subrole, CFSTR("AXSecureTextField"), 0) == kCFCompareEqualTo;
    if (subrole) CFRelease(subrole);
    if (!is_secure) {
        AXUIElementCopyAttributeValue(element, kAXRoleAttribute, (CFTypeRef *)&role);
        is_secure = role && CFStringCompare(role, CFSTR("AXSecureTextField"), 0) == kCFCompareEqualTo;
        if (role) CFRelease(role);
    }
    if (is_secure) {
        CFRetain(element);
        return element;
    }

    CFArrayRef children = NULL;
    if (AXUIElementCopyAttributeValue(element, kAXChildrenAttribute, (CFTypeRef *)&children) != kAXErrorSuccess || !children) {
        return NULL;
    }

    CFIndex count = CFArrayGetCount(children);
    AXUIElementRef found = NULL;
    for (CFIndex i = 0; i < count && !found && *budget > 0; i++) {
        AXUIElementRef child = (AXUIElementRef)CFArrayGetValueAtIndex(children, i);
        found = find_secure_text_field(child, depth - 1, budget);
    }
    CFRelease(children);
    return found;
}

static void get_focused_element_full(AXUIElementRef focused_app, bool allow_expensive_fallback,
                                      CFStringRef *out_role, CFStringRef *out_subrole,
                                      CFStringRef *out_value, AXUIElementRef *out_element) {
    *out_role = NULL;
    *out_subrole = NULL;
    *out_value = NULL;
    *out_element = NULL;
    if (!focused_app) return;

    AXUIElementRef focused_window = NULL;
    if (AXUIElementCopyAttributeValue(focused_app, kAXFocusedWindowAttribute,
                                       (CFTypeRef *)&focused_window) != kAXErrorSuccess || !focused_window) {
        return;
    }

    AXUIElementRef focused_element = NULL;
    AXError err = AXUIElementCopyAttributeValue(focused_window, kAXFocusedUIElementAttribute,
                                                 (CFTypeRef *)&focused_element);

    if ((err != kAXErrorSuccess || !focused_element) && allow_expensive_fallback) {
        /* Fallback path -- see find_secure_text_field's comment above.
         * Gated behind allow_expensive_fallback: only worth paying for
         * when we already know this process is on the auth allowlist,
         * so we don't tree-walk whatever ordinary app happens to be
         * frontmost on every single 300ms tick all day. */
        int budget = 500; /* generous but bounded node visit cap */
        focused_element = find_secure_text_field(focused_window, 8, &budget);
    }
    CFRelease(focused_window);
    if (!focused_element) return;

    AXUIElementCopyAttributeValue(focused_element, kAXRoleAttribute, (CFTypeRef *)out_role);
    AXUIElementCopyAttributeValue(focused_element, kAXSubroleAttribute, (CFTypeRef *)out_subrole);
    AXUIElementCopyAttributeValue(focused_element, kAXValueAttribute, (CFTypeRef *)out_value);
    *out_element = focused_element; /* transfer ownership to caller */
}

/* Debounce for the padlock watcher -- without it, a padlock sheet
 * sitting on screen mid-swipe would re-arm on every 300ms tick. */
static atomic_bool g_padlock_prompt_already_armed = false;

/* Single poll, checked every 300ms on the main run loop. Checks each
 * candidate PID from get_auth_candidate_pids() (usually just one or
 * two) directly by PID, classifying any secure text field owned by a
 * system-auth process as a padlock. Clears the debounce flag when
 * nothing currently matches, so a resolved/cancelled prompt can re-arm
 * cleanly next time. */
static void auth_prompt_poll_callback(CFRunLoopTimerRef timer, void *info) {
    (void)timer; (void)info;
    if (atomic_load(&g_state) != STATE_IDLE) return; /* already mid-episode elsewhere */

    pid_t candidates[8];
    int candidate_count = get_auth_candidate_pids(candidates, 8);

    if (candidate_count == 0) {
        atomic_store(&g_padlock_prompt_already_armed, false);
        return;
    }

    for (int i = 0; i < candidate_count; i++) {
        pid_t pid = candidates[i];

        char proc_name_buf[256];
        char proc_path_buf[1024];
        bool got_name = get_process_name(pid, proc_name_buf, sizeof(proc_name_buf)) == 0;
        bool got_path = get_process_path(pid, proc_path_buf, sizeof(proc_path_buf)) == 0;
        bool is_auth_process = got_name && is_system_auth_process(proc_name_buf, got_path ? proc_path_buf : NULL);
        if (!is_auth_process) continue; /* not worth an AX round-trip at all */

        AXUIElementRef app = AXUIElementCreateApplication(pid);
        CFStringRef role = NULL, subrole = NULL, value = NULL;
        AXUIElementRef focused_element = NULL;
        get_focused_element_full(app, true /* always allow fallback -- already confirmed auth process */,
                                  &role, &subrole, &value, &focused_element);
        CFRelease(app);

        /* Secure fields in these SwiftUI-hosted sheets report
         * role=AXTextField, subrole=AXSecureTextField -- not
         * role=AXSecureTextField directly (confirmed via a full AX
         * tree dump). Check subrole; fall back to role for any other
         * context that genuinely uses it as the role. */
        bool is_secure_field = (subrole && CFStringCompare(subrole, CFSTR("AXSecureTextField"), 0) == kCFCompareEqualTo)
                                || (role && CFStringCompare(role, CFSTR("AXSecureTextField"), 0) == kCFCompareEqualTo);

        if (role) CFRelease(role);
        if (subrole) CFRelease(subrole);
        if (value) CFRelease(value);

        if (!is_secure_field) {
            if (focused_element) CFRelease(focused_element);
            continue;
        }

        /* Found a live padlock among the candidates. */
        if (!atomic_load(&g_padlock_prompt_already_armed)) {
            atomic_store(&g_padlock_prompt_already_armed, true);
            arm_polling_for_trigger(TRIGGER_PADLOCK, pid, focused_element, "System auth padlock");
        } else if (focused_element) {
            CFRelease(focused_element);
        }
        return; /* stop checking other candidates once we've found/confirmed one */
    }

    /* No candidate this tick matched a live padlock. */
    atomic_store(&g_padlock_prompt_already_armed, false);
}

static void notification_callback(CFNotificationCenterRef center,
                                   void *observer,
                                   CFStringRef name,
                                   const void *object,
                                   CFDictionaryRef userInfo) {
    (void)center; (void)observer; (void)object; (void)userInfo;
    if (CFStringCompare(name, CFSTR("com.apple.screenIsLocked"), 0) == kCFCompareEqualTo) {
        on_screen_locked();
    } else if (CFStringCompare(name, CFSTR("com.apple.screenIsUnlocked"), 0) == kCFCompareEqualTo) {
        on_screen_unlocked();
    }
}

/* Runs on its own thread so the main thread stays free to run
 * CFRunLoopRun() and receive lock/unlock notifications. Starts
 * capturing/matching as soon as STATE_POLLING is set, which now
 * happens immediately on trigger detection -- templates load
 * concurrently on a separate loader thread (see
 * load_templates_thread_main), so a captured swipe is held until
 * g_templates_ready flips true, then g_enrolled_templates and the
 * cached password are read under g_templates_lock (see the snapshot
 * below) rather than assumed single-writer/single-reader. */
static void *polling_thread_main(void *arg) {
    (void)arg;
    while (atomic_load(&g_running)) {
        if (atomic_load(&g_state) != STATE_POLLING) {
            usleep(200000);
            continue;
        }

        struct xyt_struct probe;
        if (capture_quality_template(&probe) != 0) {
            /* Weak/failed swipe -- or nobody swiped at all yet. Back
             * off briefly and try again as long as we're still locked. */
            usleep(POLL_RETRY_DELAY_USEC);
            continue;
        }

        /* A real swipe just came in, but the loader thread might still
         * be mid-mount on the encrypted volume. Rather than throw away
         * a swipe the user just gave us, hold it and wait briefly for
         * g_templates_ready. Bail instead of spinning forever if the
         * episode ends underneath us -- screen unlocked, or the loader
         * itself failed and on_screen_unlocked/load_templates_thread_main
         * already put us back in STATE_IDLE. */
        while (!atomic_load(&g_templates_ready) && atomic_load(&g_state) == STATE_POLLING) {
            usleep(50000);
        }
        if (atomic_load(&g_state) != STATE_POLLING) {
            continue;
        }

        /* Snapshot everything the loader thread wrote under its lock --
         * cheap (a handful of small structs + one short string), and
         * means the rest of this iteration (matching, then typing) runs
         * against a consistent copy even if on_screen_unlocked clears
         * the live globals out from under us moments later. */
        struct xyt_struct enrolled_snapshot[MAX_STORED_TEMPLATES];
        int enrolled_count_snapshot;
        char password_snapshot[MAX_PASSWORD_LEN + 1];
        int password_valid_snapshot;
        pthread_mutex_lock(&g_templates_lock);
        enrolled_count_snapshot = g_enrolled_count;
        memcpy(enrolled_snapshot, g_enrolled_templates, sizeof(enrolled_snapshot));
        password_valid_snapshot = g_cached_password_valid;
        memcpy(password_snapshot, g_cached_password, sizeof(password_snapshot));
        pthread_mutex_unlock(&g_templates_lock);

        int best_score = -1;
        for (int i = 0; i < enrolled_count_snapshot; i++) {
            int score = vfs5011_match_score(&probe, &enrolled_snapshot[i]);
            if (score > best_score) best_score = score;
        }

        print_timestamp();
        printf("Poll swipe scored %d (threshold %d)\n", best_score, MATCH_THRESHOLD);

        if (best_score >= MATCH_THRESHOLD) {
            print_timestamp();
            printf("MATCH -- checking target is still focused before typing...\n");

            trigger_source_t source = atomic_load(&g_trigger_source);
            bool ok_to_type = true;

            if (source == TRIGGER_PADLOCK) {
                /* Re-verify right now, not at detection time -- capture
                 * takes a couple seconds, plenty of time for the user
                 * to have alt-tabbed away. Directly re-query the
                 * target PID (not "whatever's frontmost" -- there's no
                 * single reliable call for that anymore, see
                 * get_auth_candidate_pids' comment) and confirm it
                 * still shows a secure field. If the process is gone,
                 * or it no longer reports one (e.g. because it lost
                 * key window status when the user switched away),
                 * refuse to type. */
                AXUIElementRef target_app = AXUIElementCreateApplication(g_trigger_target_pid);
                CFStringRef role = NULL, subrole = NULL, value = NULL;
                AXUIElementRef elem = NULL;
                get_focused_element_full(target_app, true, &role, &subrole, &value, &elem);
                CFRelease(target_app);
                bool still_secure_field = (subrole && CFStringCompare(subrole, CFSTR("AXSecureTextField"), 0) == kCFCompareEqualTo)
                                           || (role && CFStringCompare(role, CFSTR("AXSecureTextField"), 0) == kCFCompareEqualTo);
                if (role) CFRelease(role);
                if (subrole) CFRelease(subrole);
                if (value) CFRelease(value);
                if (elem) CFRelease(elem);

                if (!still_secure_field) {
                    ok_to_type = false;
                    print_timestamp();
                    printf("ABORTED -- focus moved away from the target window since detection. Not typing.\n");
                }
            }

            if (ok_to_type) play_success_sound();

            if (ok_to_type && source == TRIGGER_PADLOCK && g_trigger_secure_field) {
                /* Belt-and-suspenders: explicitly focus the secure
                 * field before typing, in case something else in the
                 * dialog grabbed focus first. */
                AXUIElementSetAttributeValue(g_trigger_secure_field, kAXFocusedAttribute, kCFBooleanTrue);
                usleep(50000);
            }

            if (ok_to_type && password_valid_snapshot) {
                type_password_and_enter(password_snapshot);
                print_timestamp();
                printf("Typed.\n");
                vfs5011_notify_swipe_success();
            } else if (ok_to_type) {
                print_timestamp();
                printf("Match succeeded but no stored password was available -- cannot auto-type.\n");
            }

            if (g_trigger_secure_field) { CFRelease(g_trigger_secure_field); g_trigger_secure_field = NULL; }
            atomic_store(&g_trigger_source, TRIGGER_NONE);

            /* Stop hammering the sensor until the next trigger. For
             * the lock screen specifically, on_screen_unlocked will
             * also fire and re-confirm IDLE -- harmless overlap. */
            atomic_store(&g_state, STATE_IDLE);
        } else {
            /* A real swipe was captured (capture_quality_template
             * succeeded above) but didn't match any enrolled finger
             * well enough -- give an audible reject cue and let the
             * loop immediately try the next swipe. */
            play_failure_sound();
            vfs5011_notify_swipe_failed();
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ *
 * OpenCore version gate (v1.0.2 requirement)
 * ------------------------------------------------------------------ *
 * As of v1.0.2, this tool refuses to run unless it can confirm, via
 * NVRAM, that the system is booted on OpenCore 1.0.6 or newer (MINIMUM
 * only -- there is no ceiling, any version >= 1.0.6 passes, RELEASE or
 * DEBUG build both fine). This is a security floor: typing a login
 * password on the user's behalf isn't something this daemon should do
 * on a boot chain it can't verify meets a known-good baseline.
 *
 * OpenCore publishes its own version into NVRAM under its vendor GUID
 * (4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102), key "opencore-version" --
 * documented in the OpenCore Reference Manual's Debug Properties
 * section. Historically (confirmed across real-world OC installs
 * through at least the 0.6.x/0.7.x/0.8.x lines) this reports a string
 * like "REL-107-2025-08-01" (RELEASE build) or "DBG-107-2025-08-01"
 * (DEBUG build) -- the middle field is the version squished to 3
 * digits with no dots (0.6.6 -> "066", 1.0.6 -> "106").
 *
 * IMPORTANT / UNVERIFIED ON REAL 1.0.x HARDWARE: this project has not
 * yet empirically confirmed the exact NVRAM string OpenCore 1.0.x
 * itself reports, the way the auth-prompt watcher changes earlier in
 * this file WERE confirmed via ax_probe.c (see the "Passwords" /
 * "coreautha" findings). The 3-digit-code format above is what every
 * OC release through at least 0.8.x has used and nothing in OpenCore's
 * own changelog suggests it changed for 1.0.x, but before shipping,
 * run this on the real machine:
 *     nvram 4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:opencore-version
 * and confirm the output matches the format parsed below. If it
 * doesn't, this function fails CLOSED rather than silently accepting
 * an unrecognized value -- update the parser to match reality first,
 * same philosophy as the ax_probe-driven changes above.
 *
 * ALSO REQUIRED: OpenCore only publishes this variable at all if
 * config.plist's NVRAM -> ExposeSensitiveData bitmask includes the bit
 * that exposes opencore-version. Plenty of existing configs (including
 * ones from otherwise-correct guides) don't set this, which would make
 * this check fail closed on an up-to-date, perfectly fine OpenCore
 * install. Confirm this bit is set BEFORE relying on this gate -- see
 * the OpenCore Reference Manual's NVRAM Properties section for the
 * exact bitmask, and test with the `nvram` command above first.
 *
 * FAILURE MODE: this only refuses to run outright when the version
 * genuinely can't be determined at all (not booted via OpenCore, the
 * NVRAM variable is missing/unreadable, or the value doesn't parse) --
 * those indicate something is fundamentally broken, not just "old."
 * If a version WAS successfully read and it's simply below 1.0.6, this
 * prints a loud warning and lets the user proceed anyway -- they're on
 * their own for any issues that show up on 1.0.5 or older. */
#define REQUIRED_OC_VERSION_CODE 106 /* 1.0.6 minimum -- MAJOR*100+MINOR*10+PATCH, no ceiling */

/* Returns true if the daemon should proceed (version OK, or version
 * too low but the user's been warned), false only when the version
 * genuinely could not be determined at all. */
static bool check_opencore_version_requirement(void) {
    io_registry_entry_t options = IORegistryEntryFromPath(kIOMasterPortDefault, "IODeviceTree:/options");
    if (options == MACH_PORT_NULL) {
        fprintf(stderr, "OpenCore version check FAILED: could not open IODeviceTree:/options "
                        "(are you booted via OpenCore at all?). Refusing to run.\n");
        return false;
    }

    CFStringRef key = CFSTR("4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:opencore-version");
    CFTypeRef value = IORegistryEntryCreateCFProperty(options, key, kCFAllocatorDefault, 0);
    IOObjectRelease(options);

    if (!value) {
        fprintf(stderr, "OpenCore version check FAILED: \"opencore-version\" NVRAM variable not found.\n");
        fprintf(stderr, "This means either (a) you're not booted via OpenCore, or (b) your config.plist's\n");
        fprintf(stderr, "NVRAM -> ExposeSensitiveData bitmask doesn't expose this variable.\n");
        fprintf(stderr, "This tool requires OpenCore 1.0.6 or newer -- refusing to run.\n");
        return false;
    }

    char buf[128] = {0};
    bool got_string = false;

    if (CFGetTypeID(value) == CFDataGetTypeID()) {
        CFDataRef data = (CFDataRef)value;
        CFIndex len = CFDataGetLength(data);
        if (len > 0 && (size_t)len < sizeof(buf)) {
            CFDataGetBytes(data, CFRangeMake(0, len), (UInt8 *)buf);
            buf[len] = '\0';
            got_string = true;
        }
    } else if (CFGetTypeID(value) == CFStringGetTypeID()) {
        got_string = CFStringGetCString((CFStringRef)value, buf, sizeof(buf), kCFStringEncodingUTF8);
    }
    CFRelease(value);

    if (!got_string || buf[0] == '\0') {
        fprintf(stderr, "OpenCore version check FAILED: could not read \"opencore-version\" as text. Refusing to run.\n");
        return false;
    }

    /* Parse the 3-digit version code out of "REL-106-..." / "DBG-106-...".
     * Deliberately narrow (exact "-DDD-" or "-DDD" at end) rather than a
     * loose scan -- a loose match risks silently accepting a malformed
     * or unexpected string as if it passed. */
    int found_code = -1;
    size_t buf_len = strlen(buf);
    for (size_t i = 0; i + 4 <= buf_len; i++) {
        if (buf[i] == '-' && isdigit((unsigned char)buf[i+1]) && isdigit((unsigned char)buf[i+2])
            && isdigit((unsigned char)buf[i+3])
            && (i + 4 == buf_len || buf[i+4] == '-')) {
            found_code = (buf[i+1] - '0') * 100 + (buf[i+2] - '0') * 10 + (buf[i+3] - '0');
            break;
        }
    }

    if (found_code < 0) {
        fprintf(stderr, "OpenCore version check FAILED: could not parse a version out of \"%s\".\n", buf);
        fprintf(stderr, "Expected a format like \"REL-106-2025-08-01\". If OpenCore's NVRAM string format\n");
        fprintf(stderr, "has changed, this parser needs updating -- refusing to run rather than guess.\n");
        return false;
    }

    if (found_code < REQUIRED_OC_VERSION_CODE) {
        fprintf(stderr, "OpenCore v%d.%d.%d detected: Officially not supported, proceed at your own risk.\n",
                found_code / 100, (found_code / 10) % 10, found_code % 10);
        return true;
    }

    printf("OpenCore v%d.%d.%d detected: continue.\n",
           found_code / 100, (found_code / 10) % 10, found_code % 10);
    return true;
}

int main(int argc, char **argv) {
    /* v1.0.5: plain, unprivileged version query. Client's startup gate
     * execs this (`vfs5011_daemon --version`) via popen and diffs the
     * output against its own VFS5011_PROJECT_VERSION, so this has to
     * print just the bare version and exit(0) before anything else --
     * no root re-exec, no OpenCore gate, no daemon startup -- so the
     * check itself never triggers a sudo prompt or a launchd conflict
     * with an already-running instance. */
    if (argc > 1 && strcmp(argv[1], "--version") == 0) {
        printf("%s\n", VFS5011_PROJECT_VERSION);
        return 0;
    }

    /* Force line-buffered stdout regardless of whether it's a tty.
     * Interactively, stdout is line-buffered automatically. But under
     * launchd, StandardOutPath redirects stdout to a plain file, which
     * makes libc switch to fully-buffered mode by default — every
     * printf() then sits in an internal buffer and never reaches the
     * log until it fills (often several KB) or the process exits.
     * That makes `tail -f` on the log look completely dead even while
     * the daemon is actively running and working correctly. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (geteuid() != 0) {
        fprintf(stderr, "Root privileges are required to access the USB device — requesting via sudo...\n");
        char *sudo_argv[3];
        sudo_argv[0] = "sudo";
        sudo_argv[1] = argv[0];
        sudo_argv[2] = NULL;
        execvp("sudo", sudo_argv);
        fprintf(stderr, "Failed to re-exec with sudo: %s\n", strerror(errno));
        return 1;
    }

    init_exec_dir(argv[0]);

    if (!check_opencore_version_requirement()) {
        return 1;
    }

    printf("VFS5011 authentication daemon running -- idle until screen locks.\n");
    printf("Ctrl+C to quit.\n\n");

    if (!vfs5011_sensor_is_present()) {
        printf("WARNING: No VFS5011 sensor detected on this system. The daemon\n");
        printf("will keep running and re-check on every lock/auth-prompt trigger,\n");
        printf("but it will stay idle (no swipe prompt) until a sensor is plugged in.\n\n");
    }

    pthread_t poll_thread;
    if (pthread_create(&poll_thread, NULL, polling_thread_main, NULL) != 0) {
        fprintf(stderr, "Failed to start polling thread: %s\n", strerror(errno));
        return 1;
    }

    CFNotificationCenterRef center = CFNotificationCenterGetDistributedCenter();
    CFNotificationCenterAddObserver(center, NULL, notification_callback,
                                     CFSTR("com.apple.screenIsLocked"), NULL,
                                     CFNotificationSuspensionBehaviorDeliverImmediately);
    CFNotificationCenterAddObserver(center, NULL, notification_callback,
                                     CFSTR("com.apple.screenIsUnlocked"), NULL,
                                     CFNotificationSuspensionBehaviorDeliverImmediately);

    CFRunLoopTimerRef auth_prompt_timer = CFRunLoopTimerCreate(
        NULL, CFAbsoluteTimeGetCurrent(), 0.3 /* every 300ms */, 0, 0,
        auth_prompt_poll_callback, NULL);
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), auth_prompt_timer, kCFRunLoopDefaultMode);
    printf("Auth-prompt watcher registered (padlock, polling every 300ms).\n");

    vfs5011_menubar_ipc_init();

    CFRunLoopRun();

    atomic_store(&g_running, 0);
    pthread_join(poll_thread, NULL);
    return 0;
}
