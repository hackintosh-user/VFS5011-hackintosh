/*
 * vfs5011_enroll_verify.c
 *
 * Step 5: full pipeline. Two modes:
 *
 *   ./vfs5011_enroll_verify enroll
 *       Captures a swipe, extracts minutiae, saves as template.dat
 *
 *   ./vfs5011_enroll_verify verify
 *       Captures a swipe, extracts minutiae, compares against
 *       template.dat, and prints:
 *           checkmark + "Success!"              on match
 *           X + "Incorrect fingerprint"          on no match
 *
 * Build (macOS) — compiles the capture/init code, the matcher
 * wrapper, and every mindtct + bozorth3 source file together:
 *
 *   clang vfs5011_enroll_verify.c vfs5011_matcher.c \
 *       nbis/mindtct/*.c nbis/bozorth3/*.c \
 *       -o vfs5011_enroll_verify \
 *       -I. -Inbis/include \
 *       -I/usr/local/include/libusb-1.0 -L/usr/local/lib -lusb-1.0 \
 *       -lm
 *
 * (A build.sh with this exact command is provided alongside this file.)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <termios.h>
#include <stdarg.h>
#include <ctype.h>
#include <stdbool.h>
#include <time.h>
#include <libusb.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

#include "vfs5011_proto.h"
#include "vfs5011_matcher.h"
#include "supported_sensors.h"
#include "metallica_mis_firmware.h"

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

#define MATCH_THRESHOLD 20       /* testing lower vs. confirmed impostor ceiling of ~18 */

/* Built-in macOS system sound (no bundled asset -- keeps the repo
 * asset-free for open sourcing). Same cue as vfs5011_daemon.c's
 * background polling loop, so a rejected swipe sounds the same
 * whether it happened via the lock screen or this interactive menu. */
#define FAILURE_SOUND_PATH "/System/Library/Sounds/Basso.aiff"
#define SUCCESS_SOUND_PATH "/System/Library/Sounds/Glass.aiff"

/* Best-effort, backgrounded so it never blocks -- a missing sound
 * file or no afplay shouldn't affect matching, just skip the cue. */
static void play_failure_sound(void) {
    int status = system("afplay " FAILURE_SOUND_PATH " > /dev/null 2>&1 &");
    (void)status;
}

static void play_success_sound(void) {
    int status = system("afplay " SUCCESS_SOUND_PATH " > /dev/null 2>&1 &");
    (void)status;
}
#define ENROLL_SWIPES 5          /* how many good swipes make up one enrollment */
#define MAX_STORED_TEMPLATES 8   /* array bound for save/load */
#define MIN_MINUTIAE 20          /* below this, a capture is too weak to trust */
#define MAX_SWIPE_RETRIES 3      /* re-prompt this many times before giving up on one swipe */
#define MIN_SELF_CONSISTENCY 15  /* a new enroll swipe must score at least this well against
                                    at least one already-saved swipe from this same session,
                                    or it's treated as an outlier capture and re-prompted */

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
#define MOUNT_SCRIPT_NAME "vfs5011_volume_mount.sh"
#define UNMOUNT_SCRIPT_NAME "vfs5011_volume_unmount.sh"
#define SETUP_VOLUME_SCRIPT_NAME "vfs5011_setup_volume.sh"
#define GRANT_ACCESSIBILITY_SCRIPT_NAME "vfs5011_grant_accessibility.sh"
#define AGENT_LABEL "com.hackintosh.vfs5011agent"
#define VOLUME_NAME "VFSStore"
#define TCC_DB_PATH "/Library/Application Support/com.apple.TCC/TCC.db"

/* ------------------------------------------------------------------ *
 * Color / style. isatty()-gated so piping the client's output to a
 * file or another program doesn't fill it with raw escape codes --
 * colors are a terminal nicety, not something a log parser should
 * ever have to deal with. g_color_enabled is decided once at startup
 * and every VFSC_* macro below reads through it, so the rest of the
 * file never needs its own isatty() checks. */
static int g_color_enabled = 1;

#define VFSC_RESET   (g_color_enabled ? "\033[0m"    : "")
#define VFSC_BOLD    (g_color_enabled ? "\033[1m"    : "")
#define VFSC_DIM     (g_color_enabled ? "\033[2m"    : "")
#define VFSC_RED     (g_color_enabled ? "\033[31m"   : "")
#define VFSC_GREEN   (g_color_enabled ? "\033[32m"   : "")
#define VFSC_YELLOW  (g_color_enabled ? "\033[33m"   : "")
#define VFSC_BLUE    (g_color_enabled ? "\033[34m"   : "")
#define VFSC_MAGENTA (g_color_enabled ? "\033[35m"   : "")
#define VFSC_CYAN    (g_color_enabled ? "\033[36m"   : "")
#define VFSC_BCYAN   (g_color_enabled ? "\033[1;36m" : "")
#define VFSC_BGREEN  (g_color_enabled ? "\033[1;32m" : "")
#define VFSC_BRED    (g_color_enabled ? "\033[1;31m" : "")
#define VFSC_BYELLOW (g_color_enabled ? "\033[1;33m" : "")

/* Verbose boot log, default ON -- prints a scrolling kernel-log-style
 * flavor line before/around the real startup checks, mirroring a
 * Hackintosh verbose boot (-v). Purely decorative timestamp/prefix
 * text wrapped around REAL check results -- it never replaces or
 * hides the actual pass/fail output those checks already print,
 * only adds atmosphere around it. Pass --q (or --quiet) to skip the
 * flavor lines entirely and get the older, plain startup output.
 * Intentionally kept non-denominational/purely-technical flavor
 * text only (no "gods"/mythology framing) -- this ships to a mixed
 * audience and there's no upside to picking that fight. */
static bool g_verbose_boot = true;
static double g_boot_fake_time = 0.000031;

static void vfsc_boot_line(const char *fmt, ...) {
    if (!g_verbose_boot) return;

    /* Fake-but-monotonic timestamp, small pseudo-random-ish increment
     * each call so it reads like a real dmesg/kernel log scroll
     * rather than a suspiciously round counter. */
    g_boot_fake_time += 0.000037 + (double)(rand() % 419) / 1000000.0;

    printf("%s[%9.6f]%s ", VFSC_DIM, g_boot_fake_time, VFSC_RESET);

    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

/* Like vfsc_boot_line(), but for the REAL check functions' own
 * routine status lines (e.g. "OpenCore v1.0.7 detected: continue.")
 * rather than pure decorative flavor. Unlike vfsc_boot_line(), this
 * ALWAYS prints -- quiet mode (--q) still needs to see real check
 * results, it just drops the timestamp prefix and goes back to the
 * older plain output exactly. Verbose mode gets the same timestamp
 * treatment as the flavor lines around it, so the whole boot reads
 * as one continuous log instead of some lines having timestamps and
 * others not. */
static void vfsc_status_line(const char *fmt, ...) {
    if (g_verbose_boot) {
        g_boot_fake_time += 0.000037 + (double)(rand() % 419) / 1000000.0;
        printf("%s[%9.6f]%s ", VFSC_DIM, g_boot_fake_time, VFSC_RESET);
    }

    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

/* Small printf-style helpers so success/error/warning lines look the
 * same everywhere instead of every call site hand-rolling its own
 * color codes. Errors go to stderr (matching the rest of the file's
 * existing convention), success/info/warn go to stdout. */
static void vfsc_ok(const char *fmt, ...) {
    va_list ap;
    printf("%s", VFSC_GREEN);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("%s", VFSC_RESET);
}
static void vfsc_err(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s", VFSC_BRED);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "%s", VFSC_RESET);
}
static void vfsc_warn(const char *fmt, ...) {
    va_list ap;
    printf("%s", VFSC_YELLOW);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("%s", VFSC_RESET);
}

/* ASCII banner shown once at startup, above the interactive menu loop
 * -- a stylized fingertip next to a "VFS CLIENT" wordmark. Kept to 70
 * columns so it doesn't wrap in a standard 80-column terminal. The
 * fingertip glyph is printed in one color, the wordmark in another,
 * so it reads as a proper logo rather than a wall of one-color ASCII. */
/* Disable color when stdout isn't a real terminal (piped/redirected)
 * or when the caller opts out via the NO_COLOR convention
 * (https://no-color.org/) -- both are standard courtesies for a CLI
 * tool that other scripts or logs might capture output from. */
static void init_color_support(void) {
    if (!isatty(STDOUT_FILENO)) {
        g_color_enabled = 0;
        return;
    }
    const char *no_color = getenv("NO_COLOR");
    if (no_color && no_color[0] != '\0') {
        g_color_enabled = 0;
    }
}

static void print_banner(void) {
    printf("%s", VFSC_CYAN);
    printf("        ,ad8888ba,             %s█   █ █████ █████ ████ %s\n", VFSC_BCYAN, VFSC_CYAN);
    printf("      ,8P'  \"Y8\"  `Y8,         %s█   █   █     █   █   █%s\n", VFSC_BCYAN, VFSC_CYAN);
    printf("     ,8'   .-\"\"-.   `8,        %s█████   █     █   █   █%s\n", VFSC_BCYAN, VFSC_CYAN);
    printf("     8)   /  ()  \\   (8        %s█   █   █     █   █   █%s\n", VFSC_BCYAN, VFSC_CYAN);
    printf("     8   |  ()()  |   8        %s█   █   █   █████ ████ %s\n", VFSC_BCYAN, VFSC_CYAN);
    printf("     8)   \\  ()  /   (8          %sCLIENT%s\n", VFSC_BCYAN, VFSC_CYAN);
    printf("      `8,   `-..-'   ,8'\n");
    printf("       `8a,        ,a8'   %sMulti-Sensor Fingerprint Auth%s\n", VFSC_DIM, VFSC_CYAN);
    printf("         `\"Y8888P\"'%s                              %sv%s%s\n", VFSC_RESET, VFSC_DIM, VFS5011_PROJECT_VERSION, VFSC_RESET);
}

/* Clears the terminal and homes the cursor, then redraws the banner --
 * used when returning to the main menu after an action completes, so
 * the screen doesn't accumulate every enroll/verify/settings output
 * from the whole session. Gated on g_color_enabled (same isatty()
 * check used for color) since clearing a piped/redirected output
 * stream makes no sense and would just inject garbage escape codes
 * into a log file. */
static void clear_screen_and_redraw_banner(void) {
    if (!g_color_enabled) return;
    printf("\033[2J\033[H");
    fflush(stdout);
    print_banner();
}

/* Multi-finger storage: each enrolled finger gets its own file of
 * ENROLL_SWIPES templates, named "<label>.dat", inside a "fingers/"
 * subdirectory on the encrypted volume. This replaces the old single
 * template.dat (which only ever supported one finger — the multiple
 * templates in that file were multiple swipes of that ONE finger, for
 * robustness, not multiple distinct fingers). */
#define FINGERS_DIRNAME "fingers"
#define MAX_FINGER_LABEL 40
#define MAX_ENROLLED_FINGERS 10
#define PASSWORD_FILENAME "password.txt" /* must match vfs5011_store_password.sh / vfs5011_daemon.c */

/* Reads one line of input with terminal echo turned off (like a
 * normal sudo password prompt), stripping the trailing newline.
 * Restores the terminal's original echo setting before returning,
 * including on Ctrl+D/EOF. Returns 0 on success, -1 on EOF/error. */
static int read_hidden_line(char *buf, size_t buf_size) {
    struct termios old_term, new_term;
    int have_term = (tcgetattr(STDIN_FILENO, &old_term) == 0);
    if (have_term) {
        new_term = old_term;
        new_term.c_lflag &= ~ECHO;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &new_term);
    }

    int ok = (fgets(buf, buf_size, stdin) != NULL);

    if (have_term) tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_term);
    printf("\n");

    if (!ok) return -1;
    size_t len = strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = '\0';
    return 0;
}

/* Prompts for and stores the password the daemon auto-types on a
 * fingerprint match, directly onto the already-mounted VFSStore
 * volume at mount_path — same file, same permissions (root:wheel,
 * 600) as vfs5011_store_password.sh, just inline in the client so
 * Deploy can offer this as part of the same flow instead of requiring
 * a separate manual script run. Does NOT mount/unmount the volume
 * itself — the caller is expected to already have it mounted, since
 * both call sites (Deploy, Settings) need to check for an existing
 * password.txt first anyway. Returns 0 if a password ends up stored
 * (either just now, or already present when only_if_missing is set),
 * -1 on cancel/mismatch/error. */
static int prompt_and_store_password(const char *mount_path, int only_if_missing) {
    char password_path[PATH_MAX];
    snprintf(password_path, sizeof(password_path), "%s/%s", mount_path, PASSWORD_FILENAME);

    if (only_if_missing && access(password_path, F_OK) == 0) {
        return 0; /* already set — nothing to do */
    }

    printf("No stored password found — this is what the daemon types on a successful\n");
    printf("fingerprint match, so it needs to be your actual macOS login password.\n\n");

    char password[256], confirm[256];
    printf("Password: ");
    fflush(stdout);
    if (read_hidden_line(password, sizeof(password)) != 0) {
        printf("Cancelled.\n\n");
        return -1;
    }
    printf("Confirm:  ");
    fflush(stdout);
    if (read_hidden_line(confirm, sizeof(confirm)) != 0) {
        memset(password, 0, sizeof(password));
        printf("Cancelled.\n\n");
        return -1;
    }

    if (strcmp(password, confirm) != 0) {
        memset(password, 0, sizeof(password));
        memset(confirm, 0, sizeof(confirm));
        vfsc_err("Passwords did not match — nothing was saved.\n\n");
        return -1;
    }

    FILE *f = fopen(password_path, "w");
    if (!f) {
        memset(password, 0, sizeof(password));
        memset(confirm, 0, sizeof(confirm));
        vfsc_err("Could not write password file: %s\n\n", strerror(errno));
        return -1;
    }
    fputs(password, f); /* no trailing newline — it would get typed too */
    fclose(f);
    chmod(password_path, 0600); /* chown to root:wheel happens for free — we're already root here */

    memset(password, 0, sizeof(password));
    memset(confirm, 0, sizeof(confirm));
    printf("Password stored.\n\n");
    return 0;
}

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

/* Cached enrolled-finger list for the status line and for Enroll's
 * duplicate-name / capacity checks. NOT re-checked on every menu
 * redraw on purpose — the templates volume is unmounted at rest, and
 * mounting it just to paint a status line would mean mounting
 * constantly while someone sits at the menu, defeating the point of
 * per-operation mounting. Enroll/Verify/Manage refresh this for free
 * as a side effect since they already have the volume mounted anyway.
 * -1 = not checked yet this session. */
static int g_finger_count = -1;
static char g_finger_labels[MAX_ENROLLED_FINGERS][MAX_FINGER_LABEL + 1];

/* Trims whitespace/newline off a raw line of input and rejects
 * anything that would be unsafe or ambiguous as a filename. Path
 * separators are mapped to underscores rather than rejected outright,
 * so a fat-fingered "Right/Index" doesn't just fail with no
 * explanation. Returns 0 on success, -1 if the result would be empty. */
static int sanitize_finger_label(const char *input, char *out, size_t out_size) {
    while (*input == ' ' || *input == '\t') input++;
    size_t len = strlen(input);
    while (len > 0 && (input[len - 1] == ' '  || input[len - 1] == '\t' ||
                        input[len - 1] == '\n' || input[len - 1] == '\r')) {
        len--;
    }
    if (len == 0) return -1;
    if (len > out_size - 1) len = out_size - 1;
    for (size_t i = 0; i < len; i++) {
        char c = input[i];
        if (c == '/' || c == '\\') c = '_';
        out[i] = c;
    }
    out[len] = '\0';
    return 0;
}

/* Lists every enrolled finger by scanning fingers_dir for "*.dat"
 * files and stripping the extension to recover the label. A missing
 * directory (nothing enrolled yet) is reported as zero fingers, not
 * an error — callers shouldn't need to special-case first-run. */
static int list_enrolled_fingers(const char *fingers_dir,
                                  char labels[][MAX_FINGER_LABEL + 1],
                                  int max_count) {
    DIR *d = opendir(fingers_dir);
    if (!d) return 0;

    struct dirent *entry;
    int count = 0;
    while (count < max_count && (entry = readdir(d)) != NULL) {
        size_t len = strlen(entry->d_name);
        if (len > 4 && strcmp(entry->d_name + len - 4, ".dat") == 0) {
            size_t label_len = len - 4;
            if (label_len > MAX_FINGER_LABEL) label_len = MAX_FINGER_LABEL;
            memcpy(labels[count], entry->d_name, label_len);
            labels[count][label_len] = '\0';
            count++;
        }
    }
    closedir(d);
    return count;
}

/* Mounts the encrypted template volume via vfs5011_volume_mount.sh,
 * capturing the mount point path it prints on success. The script's
 * own diagnostic lines are captured but only surfaced if the mount
 * actually fails — on the success path they're just noise ahead of
 * every enroll/verify/deploy operation. Returns 0 and fills out_path
 * on success. */
static int mount_template_volume(char *out_path, size_t out_path_size) {
    char cmd[PATH_MAX * 2];
    snprintf(cmd, sizeof(cmd), "\"%s/%s\"", g_exec_dir, MOUNT_SCRIPT_NAME);

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        vfsc_err("Failed to run volume mount script: %s\n", strerror(errno));
        return -1;
    }

    char line[PATH_MAX];
    char last_line[PATH_MAX] = {0};
    char captured[2048] = {0};
    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';
        if (len > 0) {
            strncpy(last_line, line, sizeof(last_line) - 1);
            last_line[sizeof(last_line) - 1] = '\0';
            strncat(captured, line, sizeof(captured) - strlen(captured) - 2);
            strncat(captured, "\n", sizeof(captured) - strlen(captured) - 1);
        }
    }
    int status = pclose(fp);

    /* The script's last printed line is the mount path on success — a
     * plain absolute path starting with '/'. Anything else (empty, an
     * error message, non-zero exit) means mounting failed. */
    if (status != 0 || last_line[0] != '/') {
        vfsc_err("Volume mount failed (exit status %d):\n%s\n", status, captured);
        return -1;
    }
    strncpy(out_path, last_line, out_path_size - 1);
    out_path[out_path_size - 1] = '\0';
    return 0;
}

static void unmount_template_volume(void) {
    char cmd[PATH_MAX * 2];
    snprintf(cmd, sizeof(cmd), "\"%s/%s\" > /dev/null 2>&1", g_exec_dir, UNMOUNT_SCRIPT_NAME);
    int status = system(cmd);
    if (status != 0) {
        vfsc_err(
                "Warning: volume unmount script exited with status %d — the volume may "
                "still be mounted. Run vfs5011_volume_unmount.sh manually to check.\n",
                status);
    }
}

/* Mounts the volume just long enough to (re)list enrolled fingers into
 * the g_finger_* cache, then unmounts. Used whenever the cache is
 * stale (-1) and something needs an authoritative answer — e.g. Enroll
 * checking for a name collision, or Manage Fingerprints. */
static int refresh_finger_cache(void) {
    char mount_path[PATH_MAX];
    if (mount_template_volume(mount_path, sizeof(mount_path)) != 0) {
        return -1;
    }
    char fingers_dir[PATH_MAX];
    snprintf(fingers_dir, sizeof(fingers_dir), "%s/%s", mount_path, FINGERS_DIRNAME);
    g_finger_count = list_enrolled_fingers(fingers_dir, g_finger_labels, MAX_ENROLLED_FINGERS);
    unmount_template_volume();
    return g_finger_count;
}

/* Set once by detect_supported_sensor() at startup and reused for the
 * rest of the session -- everything downstream (status line, gates,
 * Enroll/Verify/Deploy dispatch) reads this instead of re-probing or
 * hardcoding one sensor's VID:PID. NULL means nothing in
 * supported_sensors.h was found on the bus. */
static const hack_touchid_sensor_t *g_detected_sensor = NULL;

/* Scans supported_sensors.h against whatever's actually on the USB
 * bus and returns the first match (or NULL). Deliberately does NOT
 * claim the interface -- this is a presence check only, so it doesn't
 * fight with (or get blocked by) whatever a real enroll/verify call
 * is doing elsewhere. Safe to call whether or not we're root.
 *
 * First-match-wins: if a machine somehow has two different supported
 * sensors attached, whichever is earlier in the table wins for this
 * session. Good enough for v1.1 -- true multi-sensor-on-one-host
 * support isn't a real scenario worth designing around yet. */
static const hack_touchid_sensor_t *detect_supported_sensor(void) {
    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) < 0) return NULL;
    libusb_set_option(ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_NONE);

    libusb_device **list = NULL;
    ssize_t count = libusb_get_device_list(ctx, &list);
    const hack_touchid_sensor_t *match = NULL;
    for (ssize_t i = 0; i < count && !match; i++) {
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(list[i], &desc) != 0) continue;
        for (size_t s = 0; s < HACK_TOUCHID_SENSOR_COUNT; s++) {
            if (desc.idVendor == HACK_TOUCHID_SENSORS[s].vid &&
                desc.idProduct == HACK_TOUCHID_SENSORS[s].pid) {
                match = &HACK_TOUCHID_SENSORS[s];
                break;
            }
        }
    }
    if (list) libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return match;
}

/* Builds the install path for whichever daemon owns g_detected_sensor,
 * e.g. /usr/local/libexec/hack-touchid/vfs5011_daemon. Callers must
 * have already confirmed g_detected_sensor is non-NULL. */
static void get_daemon_install_path(char *out, size_t out_size) {
    snprintf(out, out_size, "/usr/local/libexec/hack-touchid/%s",
             g_detected_sensor->daemon_binary_name);
}

/* Legacy helper kept for the handful of callers that only care
 * "is *something* supported currently plugged in" without needing to
 * know which one -- thin wrapper over the real detector. */
static int probe_sensor_present(void) {
    return detect_supported_sensor() != NULL;
}

/* Deploy state is tracked by asking launchd directly whether the
 * agent is loaded AND actually running (not just registered) in the
 * calling user's own GUI session -- gui/<uid>, never gui/0, since
 * the hack-touchid client re-execs itself under sudo at startup and getuid() at
 * that point would report the invoking user, but geteuid() reports
 * 0. Reads the true console user via $SUDO_USER (set by sudo), same
 * as vfs5011_agent_install.sh does, so this check targets the same
 * domain the installer bootstraps into. */
static int is_auth_service_deployed(void) {
    const char *sudo_user = getenv("SUDO_USER");
    char cmd[512];
    if (sudo_user && strcmp(sudo_user, "root") != 0) {
        snprintf(cmd, sizeof(cmd),
            "uid=$(id -u \"%s\" 2>/dev/null); "
            "[ -n \"$uid\" ] && launchctl print \"gui/$uid/" AGENT_LABEL "\" 2>/dev/null "
            "| grep -q 'state = running'",
            sudo_user);
    } else {
        snprintf(cmd, sizeof(cmd),
            "launchctl print \"gui/%d/" AGENT_LABEL "\" 2>/dev/null | grep -q 'state = running'",
            (int)getuid());
    }
    /* system() returns the child's exit status; grep -q exits 0 on a
     * match, non-zero if not found or if the service isn't loaded at
     * all -- either case correctly means "not deployed" here. */
    return system(cmd) == 0;
}

/* Does the encrypted VFSStore volume exist at all yet (regardless of
 * whether it's currently mounted)? A quick, non-mounting check --
 * `diskutil info` on a volume name that doesn't exist exits non-zero,
 * which is all this needs to know for the status line and for
 * deciding whether Deploy/Settings should offer first-time setup. */
static int is_volume_configured(void) {
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "diskutil info \"%s\" >/dev/null 2>&1", VOLUME_NAME);
    return system(cmd) == 0;
}

/* Checks the system TCC database directly for an Allowed
 * (auth_value=2) Accessibility grant tied to the daemon's installed
 * path -- the same table vfs5011_grant_accessibility.sh writes to.
 * Returns 0 if the daemon isn't installed yet, the TCC db is missing,
 * or no matching row exists. */
static int is_accessibility_granted(void) {
    if (access(TCC_DB_PATH, F_OK) != 0) return 0;
    if (!g_detected_sensor) return 0;
    char daemon_path[PATH_MAX];
    get_daemon_install_path(daemon_path, sizeof(daemon_path));
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "sqlite3 \"%s\" \"SELECT auth_value FROM access WHERE "
        "service='kTCCServiceAccessibility' AND client='%s';\" 2>/dev/null",
        TCC_DB_PATH, daemon_path);
    FILE *fp = popen(cmd, "r");
    if (!fp) return 0;
    char line[16] = {0};
    int got = fgets(line, sizeof(line), fp) != NULL;
    pclose(fp);
    if (!got) return 0;
    return atoi(line) == 2;
}

static void print_menu(void) {
    int sensor_present = (g_detected_sensor != NULL);
    int deployed = is_auth_service_deployed();
    int volume_ready = is_volume_configured();
    int accessibility_ready = is_accessibility_granted();

    printf("%s%s%s\n", VFSC_CYAN, VFSC_RULE, VFSC_RESET);
    printf("%s[1]%s Enroll a Finger\n", VFSC_BOLD, VFSC_RESET);
    printf("%s[2]%s Verify Fingerprint Match [Score / %d]\n", VFSC_BOLD, VFSC_RESET, MATCH_THRESHOLD);
    printf("%s[3]%s Deploy for Authentication Services\n", VFSC_BOLD, VFSC_RESET);
    printf("\n");
    printf("%s[S]%s Settings\n", VFSC_BOLD, VFSC_RESET);
    printf("%s[A]%s About\n", VFSC_BOLD, VFSC_RESET);
    printf("%s[Q]%s Quit\n", VFSC_BOLD, VFSC_RESET);
    printf("%s%s%s\n", VFSC_CYAN, VFSC_RULE, VFSC_RESET);
    printf("  * Sensor Status        : %s%s%s\n",
           sensor_present ? VFSC_GREEN : VFSC_YELLOW,
           sensor_present ? "Ready" : "Not Detected", VFSC_RESET);
    printf("  * Authentication Service: %s%s%s\n",
           deployed ? VFSC_GREEN : VFSC_YELLOW,
           deployed ? "Deployed" : "Not Deployed", VFSC_RESET);
    printf("  * Template Volume      : %s%s%s\n",
           volume_ready ? VFSC_GREEN : VFSC_YELLOW,
           volume_ready ? "Configured" : "Not Set Up", VFSC_RESET);
    printf("  * Accessibility Grant  : %s%s%s\n",
           accessibility_ready ? VFSC_GREEN : VFSC_YELLOW,
           accessibility_ready ? "Granted" : "Not Detected", VFSC_RESET);
    /* Not a live check — see g_finger_count comment above. The
     * templates volume is unmounted at rest; run Enroll, Verify, or
     * Manage once this session to populate this. */
    if (g_finger_count < 0) {
        printf("  * Fingers              : %sUnknown (run Verify/Enroll to check)%s\n", VFSC_DIM, VFSC_RESET);
    } else if (g_finger_count == 0) {
        printf("  * Fingers              : %sNone enrolled%s\n", VFSC_YELLOW, VFSC_RESET);
    } else {
        printf("  * Fingers              : %s%d Enrolled%s (", VFSC_GREEN, g_finger_count, VFSC_RESET);
        for (int i = 0; i < g_finger_count; i++) {
            printf("%s%s", i > 0 ? ", " : "", g_finger_labels[i]);
        }
        printf(")\n");
    }
    if (g_detected_sensor) {
        printf("  * Sensor Model         : %s {0x%04X:0x%04X}\n",
               g_detected_sensor->display_name, g_detected_sensor->vid, g_detected_sensor->pid);
    } else {
        printf("  * Sensor Model         : %sNone Detected%s\n", VFSC_YELLOW, VFSC_RESET);
    }
    printf("%s%s%s\n\n", VFSC_CYAN, VFSC_RULE, VFSC_RESET);
}

static void print_about(void) {
    printf("\n%sHACK-TOUCHID CLIENT%s\n", VFSC_BCYAN, VFSC_RESET);
    printf("Multi-sensor fingerprint authentication for macOS Ventura+.\n");
    printf("Currently supported: Validity VFS5011 (capture backend live);\n");
    printf("UPEK/AuthenTec TouchStrip (detection only, capture backend pending).\n");
    printf("Capture pipelines ported from libfprint; matching via NBIS mindtct/bozorth3.\n");
    printf("%sMATCH_THRESHOLD=%d, ENROLL_SWIPES=%d, MIN_SELF_CONSISTENCY=%d%s\n\n",
           VFSC_DIM, MATCH_THRESHOLD, ENROLL_SWIPES, MIN_SELF_CONSISTENCY, VFSC_RESET);
}

/* Enrolls ONE named finger. Multiple fingers can be enrolled by
 * calling this repeatedly with different names — each gets its own
 * file under fingers/ on the volume, holding its own ENROLL_SWIPES
 * templates (multiple swipes of THAT finger, for robustness, same as
 * before — that part is unchanged). */
static void do_enroll(void) {
    if (!g_detected_sensor) {
        vfsc_err("No supported sensor detected. Can't enroll without one.\n\n");
        return;
    }
    if (!g_detected_sensor->backend_available) {
        vfsc_err("%s detected, but its capture backend isn't implemented yet.\n\n",
                  g_detected_sensor->display_name);
        return;
    }

    /* Metallica MIS-specific: this sensor needs a proprietary
     * firmware blob uploaded during first-run pairing before it can
     * do anything. Other sensors (VFS5011, UPEK) don't have this
     * requirement, so this check is scoped to this exact VID:PID
     * rather than being a general precondition every sensor goes
     * through. See metallica_mis_firmware.h for why this fetches
     * from Lenovo directly rather than bundling the firmware. */
    if (g_detected_sensor->vid == 0x06cb && g_detected_sensor->pid == 0x009a) {
        if (!metallica_mis_firmware_is_present()) {
            if (!metallica_mis_firmware_fetch()) {
                vfsc_err("Couldn't obtain the sensor firmware. Enrollment can't "
                          "continue until this is resolved.\n\n");
                return;
            }
            printf("\n");
        }
    }

    if (g_finger_count < 0) refresh_finger_cache();

    printf("Enter a name for this finger (e.g. \"Right Index\"): ");
    fflush(stdout);
    char raw_label[128];
    if (!fgets(raw_label, sizeof(raw_label), stdin)) {
        printf("\nEnrollment cancelled.\n\n");
        return;
    }
    char label[MAX_FINGER_LABEL + 1];
    if (sanitize_finger_label(raw_label, label, sizeof(label)) != 0) {
        vfsc_err("Invalid finger name.\n\n");
        return;
    }

    int existing_idx = -1;
    for (int i = 0; i < g_finger_count; i++) {
        if (strcmp(g_finger_labels[i], label) == 0) { existing_idx = i; break; }
    }

    if (existing_idx < 0 && g_finger_count >= MAX_ENROLLED_FINGERS) {
        vfsc_err(
                "Maximum of %d enrolled fingers reached — delete one via "
                "[3] Manage Enrolled Fingers first.\n\n",
                MAX_ENROLLED_FINGERS);
        return;
    }

    if (existing_idx >= 0) {
        printf("A finger named \"%s\" is already enrolled. Re-enrolling will replace it.\n", label);
        printf("Continue? [y/N]: ");
        fflush(stdout);
        char confirm[8];
        if (!fgets(confirm, sizeof(confirm), stdin) || (confirm[0] != 'y' && confirm[0] != 'Y')) {
            printf("Enrollment cancelled.\n\n");
            return;
        }
    }

    struct xyt_struct templates[ENROLL_SWIPES];
    int good = 0;
    for (int i = 0; i < ENROLL_SWIPES; i++) {
        printf("Enrollment swipe %d of %d:\n", i + 1, ENROLL_SWIPES);

        struct xyt_struct candidate;
        int outlier_retries = 0;
        for (;;) {
            if (capture_quality_template(&candidate) != 0) {
                vfsc_err("Skipping this swipe slot due to repeated failures.\n");
                goto slot_done;
            }

            if (good == 0) {
                printf("  -> captured, %d minutiae\n", candidate.nrows);
                templates[good++] = candidate;
                break;
            }

            int best_self_score = -1;
            for (int j = 0; j < good; j++) {
                int s = vfs5011_match_score(&candidate, &templates[j]);
                if (s > best_self_score) best_self_score = s;
            }

            if (best_self_score >= MIN_SELF_CONSISTENCY) {
                printf("  -> captured, %d minutiae (self-check: %d)\n",
                       candidate.nrows, best_self_score);
                templates[good++] = candidate;
                break;
            }

            outlier_retries++;
            vfsc_err(
                    "  Swipe doesn't match your other swipes well (self-check: %d, need %d) "
                    "— treating as an outlier, swipe again.\n",
                    best_self_score, MIN_SELF_CONSISTENCY);
            if (outlier_retries >= MAX_SWIPE_RETRIES) {
                vfsc_err("  Repeated outliers on this slot — keeping it anyway to avoid stalling enrollment.\n");
                printf("  -> captured, %d minutiae (self-check: %d, kept despite low consistency)\n",
                       candidate.nrows, best_self_score);
                templates[good++] = candidate;
                break;
            }
        }
        slot_done: ;
    }

    if (good == 0) {
        vfsc_err("Enrollment failed: no usable swipes captured.\n\n");
        return;
    }

    /* Volume is only mounted for this final save step — every swipe
     * above happened with it fully unmounted, so the exposure window
     * is as short as physically possible: mount, write, unmount. */
    char mount_path[PATH_MAX];
    if (mount_template_volume(mount_path, sizeof(mount_path)) != 0) {
        vfsc_err("Enrollment captured but could not be saved: template volume unavailable.\n\n");
        return;
    }
    char fingers_dir[PATH_MAX];
    snprintf(fingers_dir, sizeof(fingers_dir), "%s/%s", mount_path, FINGERS_DIRNAME);
    mkdir(fingers_dir, 0700); /* ignore EEXIST — just needs to exist */

    char template_path[PATH_MAX];
    snprintf(template_path, sizeof(template_path), "%s/%s.dat", fingers_dir, label);

    if (vfs5011_save_templates(template_path, templates, good) != 0) {
        vfsc_err("Failed to save templates\n\n");
        unmount_template_volume();
        return;
    }
    unmount_template_volume();
    vfsc_ok("Enrolled \"%s\" with %d template(s) saved.\n\n", label, good);
    g_finger_count = -1; /* stale — next status/enroll/verify will re-list */
}

/* Same verify logic as the original CLI's "verify" mode, pulled into
 * a function. Returns 0 on match, 2 on no-match, 1 on hard error —
 * matching the original process exit codes, in case the menu ever
 * needs to react to the outcome (e.g. counting consecutive failures
 * for the daemon's lockout behavior later).
 *
 * As with enroll, the volume is only mounted around the load step —
 * the swipe capture itself happens first, fully unmounted. */
static int do_verify(void) {
    if (!g_detected_sensor) {
        vfsc_err("No supported sensor detected. Can't verify without one.\n\n");
        return 1;
    }
    if (!g_detected_sensor->backend_available) {
        vfsc_err("%s detected, but its capture backend isn't implemented yet.\n\n",
                  g_detected_sensor->display_name);
        return 1;
    }
    struct xyt_struct probe;
    if (capture_quality_template(&probe) != 0) {
        vfsc_err("Verify failed: could not get a usable swipe.\n\n");
        return 1;
    }

    char mount_path[PATH_MAX];
    if (mount_template_volume(mount_path, sizeof(mount_path)) != 0) {
        vfsc_err("Cannot verify: template volume unavailable.\n\n");
        return 1;
    }
    char fingers_dir[PATH_MAX];
    snprintf(fingers_dir, sizeof(fingers_dir), "%s/%s", mount_path, FINGERS_DIRNAME);

    char labels[MAX_ENROLLED_FINGERS][MAX_FINGER_LABEL + 1];
    int finger_count = list_enrolled_fingers(fingers_dir, labels, MAX_ENROLLED_FINGERS);
    if (finger_count <= 0) {
        vfsc_err("No enrolled fingers found — run Enroll first.\n\n");
        unmount_template_volume();
        g_finger_count = 0;
        return 1;
    }

    /* Check the swipe against every enrolled finger, taking the best
     * score within each finger's own swipe set, then the best finger
     * overall — the same "any enrolled finger unlocks it" behavior as
     * real Touch ID. */
    int best_score = -1;
    int best_finger = -1;
    for (int f = 0; f < finger_count; f++) {
        char template_path[PATH_MAX];
        snprintf(template_path, sizeof(template_path), "%s/%s.dat", fingers_dir, labels[f]);

        struct xyt_struct enrolled[MAX_STORED_TEMPLATES];
        int enrolled_count = 0;
        if (vfs5011_load_templates(template_path, enrolled, MAX_STORED_TEMPLATES, &enrolled_count) != 0) {
            vfsc_err("  (could not load \"%s\", skipping)\n", labels[f]);
            continue;
        }

        int finger_best = -1;
        for (int i = 0; i < enrolled_count; i++) {
            int score = vfs5011_match_score(&probe, &enrolled[i]);
            if (score > finger_best) finger_best = score;
        }
        printf("  vs \"%s\": best score %d (%d template(s))\n", labels[f], finger_best, enrolled_count);
        if (finger_best > best_score) { best_score = finger_best; best_finger = f; }
    }
    unmount_template_volume();

    /* Refresh the cache for free since we just listed it anyway. */
    g_finger_count = finger_count;
    memcpy(g_finger_labels, labels, sizeof(labels));

    if (best_finger < 0) {
        vfsc_err("No enrolled finger could be loaded — check the volume.\n\n");
        return 1;
    }

    printf("Best match: \"%s\" score %d (threshold: %d)\n", labels[best_finger], best_score, MATCH_THRESHOLD);
    if (best_score >= MATCH_THRESHOLD) {
        play_success_sound();
        printf("\n  %s\xE2\x9C\x93  Success! (%s)%s\n\n", VFSC_BGREEN, labels[best_finger], VFSC_RESET);
        return 0;
    } else {
        play_failure_sound();
        printf("\n  %s\xE2\x9C\x97  Incorrect fingerprint%s\n\n", VFSC_BRED, VFSC_RESET);
        return 2;
    }
}

/* --- Settings: [1] Delete Fingerprint Templates --- */
/* Lists enrolled fingers and lets the user delete one by number.
 * Deletion just removes that finger's .dat file from the volume —
 * the other fingers are untouched. */
static void do_settings_delete_fingers(void) {
    refresh_finger_cache();

    if (g_finger_count < 0) {
        vfsc_err("Could not check enrolled fingers: template volume unavailable.\n\n");
        return;
    }
    if (g_finger_count == 0) {
        printf("No fingers enrolled yet. Use [1] Enroll a Finger to add one.\n\n");
        return;
    }

    printf("Enrolled fingers:\n");
    for (int i = 0; i < g_finger_count; i++) {
        printf("  [%d] %s\n", i + 1, g_finger_labels[i]);
    }
    printf("\nEnter a number to delete that finger, or press Enter to go back: ");
    fflush(stdout);

    char line[16];
    if (!fgets(line, sizeof(line), stdin)) { printf("\n"); return; }
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
    if (len == 0) { printf("\n"); return; }

    int idx = atoi(line);
    if (idx < 1 || idx > g_finger_count) {
        vfsc_err("Invalid selection.\n\n");
        return;
    }

    printf("Delete \"%s\"? This cannot be undone. [y/N]: ", g_finger_labels[idx - 1]);
    fflush(stdout);
    char confirm[8];
    if (!fgets(confirm, sizeof(confirm), stdin) || (confirm[0] != 'y' && confirm[0] != 'Y')) {
        printf("Cancelled.\n\n");
        return;
    }

    char mount_path[PATH_MAX];
    if (mount_template_volume(mount_path, sizeof(mount_path)) != 0) {
        vfsc_err("Cannot delete: template volume unavailable.\n\n");
        return;
    }
    char template_path[PATH_MAX];
    snprintf(template_path, sizeof(template_path), "%s/%s/%s.dat",
             mount_path, FINGERS_DIRNAME, g_finger_labels[idx - 1]);

    if (remove(template_path) != 0) {
        vfsc_err("Failed to delete \"%s\": %s\n\n", g_finger_labels[idx - 1], strerror(errno));
    } else {
        printf("Deleted \"%s\".\n\n", g_finger_labels[idx - 1]);
    }
    unmount_template_volume();
    g_finger_count = -1; /* stale — force a re-list next time */
}

/* --- Settings: [2] Clear Password Cache --- */
/* Removes password.txt from the volume — the login password the
 * daemon reads at lock time and auto-types on a match (see
 * vfs5011_store_password.sh / vfs5011_daemon.c). This does NOT touch
 * enrolled fingerprints. After clearing, the daemon has nothing to
 * type until vfs5011_store_password.sh is run again. */
static void do_settings_clear_password_cache(void) {
    char mount_path[PATH_MAX];
    if (mount_template_volume(mount_path, sizeof(mount_path)) != 0) {
        vfsc_err("Cannot clear password cache: template volume unavailable.\n\n");
        return;
    }
    char password_path[PATH_MAX];
    snprintf(password_path, sizeof(password_path), "%s/%s", mount_path, PASSWORD_FILENAME);

    if (access(password_path, F_OK) != 0) {
        printf("No cached password found — nothing to clear.\n\n");
        unmount_template_volume();
        return;
    }

    printf("Clear the cached login password? The daemon won't be able to auto-type\n");
    printf("on unlock until you run vfs5011_store_password.sh again. [y/N]: ");
    fflush(stdout);
    char confirm[8];
    if (!fgets(confirm, sizeof(confirm), stdin) || (confirm[0] != 'y' && confirm[0] != 'Y')) {
        printf("Cancelled.\n\n");
        unmount_template_volume();
        return;
    }

    if (remove(password_path) != 0) {
        vfsc_err("Failed to clear password cache: %s\n\n", strerror(errno));
    } else {
        printf("Password cache cleared.\n\n");
    }
    unmount_template_volume();
}

/* --- Settings: [3] Set/Update Auto-Type Password --- */
static void do_settings_set_password(void) {
    char mount_path[PATH_MAX];
    if (mount_template_volume(mount_path, sizeof(mount_path)) != 0) {
        vfsc_err("Cannot set password: template volume unavailable.\n\n");
        return;
    }
    prompt_and_store_password(mount_path, /*only_if_missing=*/0);
    unmount_template_volume();
}

/* Runs vfs5011_setup_volume.sh (the one-time encrypted-volume creation
 * script), streaming its output live rather than capturing-then-
 * dumping like do_deploy() does — this one runs interactively rarely
 * enough, and takes long enough (diskutil work), that the person
 * running it benefits from watching it progress rather than staring
 * at a blank line. Returns 0 on success. Shared between the Settings
 * entry point and Deploy's own first-run auto-setup. */
static int do_run_volume_setup(void) {
    char cmd[PATH_MAX + 32];
    snprintf(cmd, sizeof(cmd), "sh \"%s/%s\" 2>&1", g_exec_dir, SETUP_VOLUME_SCRIPT_NAME);

    printf("%s", VFSC_DIM);
    int status = system(cmd);
    printf("%s", VFSC_RESET);

    if (status != 0) {
        vfsc_err("Volume setup failed (exit status %d) — see output above.\n\n", status);
        return -1;
    }
    vfsc_ok("Template volume set up successfully.\n\n");
    return 0;
}

/* --- Settings: [4] Set Up / Repair Template Volume ---
 * Guards against the easy mistake of re-running this against an
 * already-configured volume: vfs5011_setup_volume.sh doesn't check
 * for an existing VFSStore, so running it twice would create a SECOND
 * volume of the same name rather than repairing the first one. */
static void do_settings_setup_volume(void) {
    if (is_volume_configured()) {
        vfsc_warn("A template volume named \"%s\" already exists.\n", VOLUME_NAME);
        printf("Re-running setup will create a SEPARATE volume with the same name —\n");
        printf("it will NOT repair or replace the existing one. Only do this if you\n");
        printf("know the existing volume is broken and you're prepared to clean up\n");
        printf("the old one yourself afterward (diskutil apfs deleteVolume).\n\n");
        printf("Type \"yes\" to proceed anyway, or press Enter to cancel: ");
        fflush(stdout);
        char line[16];
        if (!fgets(line, sizeof(line), stdin) || strncmp(line, "yes", 3) != 0) {
            printf("Cancelled.\n\n");
            return;
        }
        printf("\n");
    }
    do_run_volume_setup();
}

/* Runs vfs5011_grant_accessibility.sh against the daemon's fixed
 * install path. Safe to call even if the daemon hasn't been deployed
 * yet — the script itself checks the binary exists and reports a
 * clear error rather than doing anything destructive. */
static int do_run_accessibility_grant(void) {
    char cmd[PATH_MAX + 64];
    snprintf(cmd, sizeof(cmd), "sh \"%s/%s\" 2>&1", g_exec_dir, GRANT_ACCESSIBILITY_SCRIPT_NAME);

    printf("%s", VFSC_DIM);
    int status = system(cmd);
    printf("%s", VFSC_RESET);

    if (status != 0) {
        vfsc_err("Accessibility grant failed (exit status %d) — see output above.\n\n", status);
        return -1;
    }
    if (is_accessibility_granted()) {
        vfsc_ok("Accessibility permission confirmed granted.\n\n");
        return 0;
    }
    vfsc_warn("Grant script exited cleanly, but the permission still isn't showing as\n"
              "granted. Double-check that Filesystem Protections are disabled\n"
              "(csrutil status) — the grant script relies on that.\n\n");
    return -1;
}

/* --- Settings: [5] Grant/Verify Accessibility Permission --- */
static void do_settings_grant_accessibility(void) {
    if (!g_detected_sensor) {
        vfsc_err("No supported sensor detected -- can't determine which daemon to check.\n\n");
        return;
    }
    char daemon_path[PATH_MAX];
    get_daemon_install_path(daemon_path, sizeof(daemon_path));
    if (access(daemon_path, F_OK) != 0) {
        vfsc_err("Daemon isn't installed yet at:\n  %s\n"
                 "Run [3] Deploy from the main menu first.\n\n", daemon_path);
        return;
    }
    if (is_accessibility_granted()) {
        printf("Accessibility is already granted for the installed daemon.\n");
        printf("Re-grant anyway (e.g. after a manual rebuild)? [y/N]: ");
        fflush(stdout);
        char line[8];
        if (!fgets(line, sizeof(line), stdin) || (line[0] != 'y' && line[0] != 'Y')) {
            printf("Cancelled.\n\n");
            return;
        }
        printf("\n");
    }
    do_run_accessibility_grant();
}

static void print_settings_menu(void) {
    printf("%s%s%s\n", VFSC_CYAN, VFSC_RULE, VFSC_RESET);
    printf("%s                              SETTINGS%s\n", VFSC_BCYAN, VFSC_RESET);
    printf("%s%s%s\n", VFSC_CYAN, VFSC_RULE, VFSC_RESET);
    printf("%s[1]%s Delete Fingerprint Templates\n", VFSC_BOLD, VFSC_RESET);
    printf("%s[2]%s Clear Password Cache\n", VFSC_BOLD, VFSC_RESET);
    printf("%s[3]%s Set/Update Auto-Type Password\n", VFSC_BOLD, VFSC_RESET);
    printf("%s[4]%s Set Up / Repair Template Volume\n", VFSC_BOLD, VFSC_RESET);
    printf("%s[5]%s Grant/Verify Accessibility Permission\n", VFSC_BOLD, VFSC_RESET);
    printf("\n");
    printf("%s[B]%s Back\n", VFSC_BOLD, VFSC_RESET);
    printf("%s%s%s\n\n", VFSC_CYAN, VFSC_RULE, VFSC_RESET);
}

/* Small dedicated loop, same pattern as main()'s — stays inside
 * Settings until the user picks [B] Back or hits EOF. Naming a finger
 * (e.g. "Left Index Finger", "Right Middle Finger") already happens
 * as part of [1] Enroll a Finger on the main menu, so it isn't
 * duplicated here. */
static void do_settings_menu(void) {
    char line[64];
    for (;;) {
        print_settings_menu();
        printf("%s<VFSC Settings>%s ", VFSC_BOLD, VFSC_RESET);
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) {
            printf("\n");
            return;
        }
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' || line[len-1] == ' ')) {
            line[--len] = '\0';
        }
        if (len == 0) continue;

        char cmd = line[0];
        printf("\n");
        switch (cmd) {
            case '1': do_settings_delete_fingers(); break;
            case '2': do_settings_clear_password_cache(); break;
            case '3': do_settings_set_password(); break;
            case '4': do_settings_setup_volume(); break;
            case '5': do_settings_grant_accessibility(); break;
            case 'B': case 'b': return;
            default:
                vfsc_err("Unrecognized option '%s'. Choose 1-5 or B.\n\n", line);
        }
    }
}

/* Deploy installs/reinstalls vfs5011_agent_install.sh, which handles
 * everything: rebuilding the daemon from source, copying it into
 * place, tearing down any prior registration, the scoped NOPASSWD
 * sudoers rule, writing the LaunchAgent plist to the console user's
 * own ~/Library/LaunchAgents (NOT /Library/LaunchAgents — see that
 * script's header comment for why the system-domain location doesn't
 * work), and bootstrapping it into that user's gui/<uid> session.
 * the hack-touchid client is already running as root at this point (see main()'s
 * self-elevation), so the script's own root check passes straight
 * through without a second sudo prompt.
 *
 * The install script's own output is captured rather than streamed —
 * on success we print one clean summary line per phase; on failure we
 * dump everything captured so far so the actual error is still
 * visible. */
static void do_deploy(void) {
    /* Hard gate, generalized in v1.1: refuse to install anything at
     * all unless a sensor from supported_sensors.h is actually on the
     * USB bus. Deploy used to happily install/register the LaunchAgent
     * on any machine regardless of hardware, which meant a fresh
     * checkout run on the wrong laptop (or with the sensor unplugged)
     * would leave a dead agent behind that could never do anything
     * useful. g_detected_sensor is set once at startup by
     * detect_supported_sensor(), the same non-claiming enumeration
     * used for the status line, so this is safe pre-root-check and
     * won't fight a concurrent enroll/verify. */
    if (!g_detected_sensor) {
        vfsc_err("No supported sensor detected on the USB bus.\n"
                  "Refusing to deploy -- this installs a background service tied\n"
                  "to a specific sensor, so it's not installed on hardware that\n"
                  "doesn't have one.\n\n");
        return;
    }

    /* Detected, but that sensor's capture backend isn't built yet
     * (backend_available == 0 in the table) -- e.g. UPEK is
     * recognized on sight but has no daemon/installer to deploy. */
    if (!g_detected_sensor->backend_available) {
        vfsc_err("%s detected, but its capture backend isn't implemented yet.\n"
                  "Deploy isn't available for this sensor until that's built.\n\n",
                  g_detected_sensor->display_name);
        return;
    }

    /* First-run convenience: Deploy needs the template volume to exist
     * (it stores the auto-type password there right below), so set it
     * up automatically rather than making the person discover Settings
     * [4] on their own after a confusing failure. */
    if (!is_volume_configured()) {
        vfsc_warn("\nTemplate volume isn't set up yet — setting it up now...\n\n");
        if (do_run_volume_setup() != 0) {
            vfsc_err("Cannot continue deployment without the template volume.\n\n");
            return;
        }
    }

    printf("\n%sDeploying Authentication Service (%s)...%s\n",
           VFSC_CYAN, g_detected_sensor->display_name, VFSC_RESET);

    char cmd[PATH_MAX + 32];
    snprintf(cmd, sizeof(cmd), "sh \"%s/%s\" 2>&1", g_exec_dir, g_detected_sensor->install_script_name);

    FILE *fp = popen(cmd, "r");
    char captured[4096] = {0};
    if (fp) {
        char line[512];
        while (fgets(line, sizeof(line), fp)) {
            strncat(captured, line, sizeof(captured) - strlen(captured) - 1);
        }
    }
    int status = fp ? pclose(fp) : -1;

    if (status != 0) {
        printf("\n%s\n", captured);
        vfsc_err("Service deployment failed (exit status %d) — see output above.\n\n", status);
        return;
    }

    printf("%sBuilding daemon executable...%s\n", VFSC_DIM, VFSC_RESET);
    printf("%sRegistering background service...%s\n", VFSC_DIM, VFSC_RESET);
    vfsc_ok("Service deployed successfully with 0 errors.\n");

    /* vfs5011_agent_install.sh already re-grants Accessibility on every
     * deploy internally (see that script's header for why it must be
     * redone every rebuild). This just surfaces whether it actually
     * took, since a silent TCC failure would otherwise only show up
     * later as "sensor matched but nothing got typed". */
    if (is_accessibility_granted()) {
        vfsc_ok("Accessibility permission: Granted.\n\n");
    } else {
        vfsc_warn("Accessibility permission: NOT detected.\n"
                  "Run Settings [5] Grant/Verify Accessibility Permission to retry.\n\n");
    }

    /* Now that the service is live, make sure it actually has a
     * password to type on a match — a fresh deploy on a machine that
     * has never run vfs5011_store_password.sh (or the older manual
     * script flow) would otherwise sit there matching fingerprints
     * and silently failing to type anything. */
    char mount_path[PATH_MAX];
    if (mount_template_volume(mount_path, sizeof(mount_path)) == 0) {
        char password_path[PATH_MAX];
        snprintf(password_path, sizeof(password_path), "%s/%s", mount_path, PASSWORD_FILENAME);
        if (access(password_path, F_OK) != 0) {
            prompt_and_store_password(mount_path, /*only_if_missing=*/1);
        }
        unmount_template_volume();
    }

    printf("Lock your screen and swipe an enrolled finger to test it.\n\n");
}

/* Darwin kernel major version 22 == macOS 13 Ventura, the new stated
 * floor as of this change. Originally this gated at Darwin 24 (macOS
 * 15 Sequoia, the OS this project was first developed against), but a
 * source review found no actual Sequoia/Sonoma-only API dependency
 * anywhere in the daemon, client, or menu bar app -- every system call
 * in use (AX APIs, CFNotificationCenterGetDistributedCenter, diskutil
 * apfs, TCC.db writes, SMAppService) has worked since well before
 * Ventura. The "Passwords" entry in is_system_auth_process() is
 * Sequoia-only in practice (that app doesn't exist earlier) but is a
 * harmless no-op allowlist entry on older OSes, not a hard dependency.
 *
 * The one confirmed empirical gap: the coreautha/Keychain-Access
 * auth-surface finding from v1.0.2 was only verified via ax_probe.c on
 * Sequoia. It has NOT yet been re-verified on Ventura or Sonoma, so
 * that specific feature may behave differently there until confirmed.
 *
 * Anything older than Darwin 22 is still untested -- this remains a
 * heads-up, not a block. Someone running this on an older OS may know
 * exactly what they're doing (or be deliberately porting it backward),
 * but they should know up front that nothing here has been verified
 * there and support is on them. */
static void check_macos_version_warning(void) {
    struct utsname uts;
    if (uname(&uts) != 0) return; /* can't determine it -- don't nag about something unconfirmed */

    int darwin_major = atoi(uts.release); /* "22.6.0" -> 22 */
    if (darwin_major > 0 && darwin_major < 22) {
        printf("\n");
        printf("############################################################\n");
        printf("  WARNING: Darwin %s detected -- older than macOS Ventura\n", uts.release);
        printf("  (Darwin 22.x). This project is developed and tested\n");
        printf("  against Ventura and later only. Older macOS versions are\n");
        printf("  untested territory -- things may work, may not, or may\n");
        printf("  behave differently. You're on your own for support here.\n");
        printf("############################################################\n\n");
    }
}

/* ------------------------------------------------------------------ *
 * OpenCore version gate (v1.0.2 requirement)
 * ------------------------------------------------------------------ *
 * Same gate as vfs5011_daemon.c -- kept as a separate copy here rather
 * than a shared header, matching this project's existing pattern of
 * self-contained client/daemon .c files. See the daemon's copy of this
 * function for the full explanation of the NVRAM key, the version
 * string format, and the ExposeSensitiveData caveat -- keep both
 * copies in sync if either needs updating. As of v1.0.2, OpenCore
 * 1.0.6 is the recommended MINIMUM version (no ceiling), RELEASE or
 * DEBUG build both fine.
 *
 * FAILURE MODE: only refuses to run when the version genuinely can't
 * be determined at all (not booted via OpenCore, NVRAM variable
 * missing/unreadable, or unparseable value) -- that indicates
 * something fundamentally broken, not just "old." If a version WAS
 * successfully read and it's simply below 1.0.6, this warns loudly
 * and lets the user proceed anyway -- on their own for any issues on
 * OpenCore 1.0.5 or older. */
#define REQUIRED_OC_VERSION_CODE 106 /* 1.0.6 minimum -- MAJOR*100+MINOR*10+PATCH, no ceiling */

/* Returns true if the client should proceed (version OK, or version
 * too low but the user's been warned), false only when the version
 * genuinely could not be determined at all. */
static bool check_opencore_version_requirement(void) {
    io_registry_entry_t options = IORegistryEntryFromPath(kIOMasterPortDefault, "IODeviceTree:/options");
    if (options == MACH_PORT_NULL) {
        vfsc_err("OpenCore version check FAILED: could not open IODeviceTree:/options "
                 "(are you booted via OpenCore at all?). Refusing to run.\n");
        return false;
    }

    CFStringRef key = CFSTR("4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:opencore-version");
    CFTypeRef value = IORegistryEntryCreateCFProperty(options, key, kCFAllocatorDefault, 0);
    IOObjectRelease(options);

    if (!value) {
        vfsc_err("OpenCore version check FAILED: \"opencore-version\" NVRAM variable not found.\n");
        vfsc_err("This means either (a) you're not booted via OpenCore, or (b) your config.plist's\n");
        vfsc_err("NVRAM -> ExposeSensitiveData bitmask doesn't expose this variable.\n");
        vfsc_err("This tool requires OpenCore 1.0.6 or newer -- refusing to run.\n");
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
        vfsc_err("OpenCore version check FAILED: could not read \"opencore-version\" as text. Refusing to run.\n");
        return false;
    }

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
        vfsc_err("OpenCore version check FAILED: could not parse a version out of \"%s\".\n", buf);
        vfsc_err("Expected a format like \"REL-106-2025-08-01\". If OpenCore's NVRAM string format\n");
        vfsc_err("has changed, this parser needs updating -- refusing to run rather than guess.\n");
        return false;
    }

    if (found_code < REQUIRED_OC_VERSION_CODE) {
        vfsc_err("OpenCore v%d.%d.%d detected: Officially not supported, proceed at your own risk.\n",
                 found_code / 100, (found_code / 10) % 10, found_code % 10);
        return true;
    }

    vfsc_status_line("OpenCore v%d.%d.%d detected: continue.",
           found_code / 100, (found_code / 10) % 10, found_code % 10);
    return true;
}

/* ------------------------------------------------------------------ *
 * Sensor presence check (startup gate, v1.0.5, generalized in v1.1)
 * ------------------------------------------------------------------ *
 * Runs detect_supported_sensor() once and stores the result in
 * g_detected_sensor for the rest of the session -- the status line,
 * do_deploy(), and check_daemon_version_gate() all read that instead
 * of re-probing. Exits if nothing in supported_sensors.h is found;
 * printed as a verbose loading line so it reads like part of the
 * normal startup sequence rather than a silent hang. */
static bool check_sensor_presence_gate(void) {
    vfsc_status_line("Checking if Sensor is active / enabled...");

    g_detected_sensor = detect_supported_sensor();
    if (!g_detected_sensor) {
        printf("Sensor not found. Launching Failed\n");
        printf("No supported sensor was found. Check if it's enabled or\n");
        printf("if you don't have one. Then quit this application.\n");
        return false;
    }

    vfsc_status_line("%s detected. continuing...", g_detected_sensor->display_name);
    return true;
}

/* ------------------------------------------------------------------ *
 * Daemon version check (startup gate, v1.0.5, generalized in v1.1)
 * ------------------------------------------------------------------ *
 * The client and the installed daemon binary are two separately
 * compiled artifacts that only stay in sync because prep_and_build.sh
 * rebuilds/redeploys both together -- nothing stops someone from
 * updating one and forgetting the other (e.g. `git pull` + rebuild
 * just the client, or a Deploy that failed partway through). Running
 * against a mismatched daemon is exactly the kind of thing that's
 * hard to diagnose after the fact, so catch it here instead.
 *
 * Shells out to whichever daemon binary owns g_detected_sensor with
 * --version, which (per each daemon's early argv check) just prints
 * the version and exits(0) immediately -- no root re-exec, no
 * OpenCore gate, no actual daemon startup, so this is cheap and
 * side-effect-free even though the client is already running as root
 * at this point. Must run after check_sensor_presence_gate(). */
static bool check_daemon_version_gate(void) {
    vfsc_status_line("Checking Daemon version...");

    if (!g_detected_sensor) {
        printf("No supported sensor detected -- skipping.\n");
        return true;
    }

    char daemon_path[PATH_MAX];
    get_daemon_install_path(daemon_path, sizeof(daemon_path));

    if (access(daemon_path, F_OK) != 0) {
        printf("No daemon installed yet -- skipping (run Deploy [3] first).\n");
        return true;
    }

    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof(cmd), "\"%s\" --version", daemon_path);

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        vfsc_err("Failed to query installed daemon version: %s\n", strerror(errno));
        return false;
    }

    char daemon_version[64] = {0};
    bool got_line = (fgets(daemon_version, sizeof(daemon_version), fp) != NULL);
    pclose(fp);

    if (!got_line) {
        printf("Could not read a version from the installed daemon. Stop launch\n");
        return false;
    }

    size_t len = strlen(daemon_version);
    while (len > 0 && (daemon_version[len-1] == '\n' || daemon_version[len-1] == '\r')) {
        daemon_version[--len] = '\0';
    }

    if (strcmp(daemon_version, VFS5011_PROJECT_VERSION) != 0) {
        printf("v%s detected... Stop launch\n", daemon_version);
        printf("Installed daemon (v%s) doesn't match this client (v%s).\n",
               daemon_version, VFS5011_PROJECT_VERSION);
        printf("The client can't launch until they match. From outside\n");
        printf("this client, run prep_and_build.sh (or the matching\n");
        printf("<sensor>_agent_install.sh) to rebuild and reinstall the\n");
        printf("daemon at the matching version, then relaunch.\n");
        return false;
    }

    vfsc_status_line("v%s detected continuing...", daemon_version);
    return true;
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            g_verbose_boot = false;
        }
    }
    srand((unsigned int)time(NULL));

    /* Claiming the USB interface needs root on macOS. Re-exec the whole
     * menu session under sudo up front, same approach as the original
     * CLI, so options 1/2 don't each need their own privilege prompt. */
    if (geteuid() != 0) {
        vfsc_err("Root privileges are required to access the USB device — requesting via sudo...\n");
        char *sudo_argv[4];
        sudo_argv[0] = "sudo";
        sudo_argv[1] = argv[0];
        sudo_argv[2] = g_verbose_boot ? NULL : "--q";
        sudo_argv[3] = NULL;
        execvp("sudo", sudo_argv);
        vfsc_err("Failed to re-exec with sudo: %s\n", strerror(errno));
        return 1;
    }

    /* Needed so mount_template_volume()/unmount_template_volume() can
     * find vfs5011_volume_mount.sh / _unmount.sh by absolute path,
     * regardless of what directory this was launched from. */
    init_exec_dir(argv[0]);
    init_color_support();

    /* Quiet mode keeps the banner where it's always been (first thing
     * shown) -- only verbose mode moves it to the end, after the log
     * scroll, so it isn't quiet mode's problem too. */
    if (!g_verbose_boot) {
        print_banner();
    }

    vfsc_boot_line("AppleACPIPlatform: enumerating hardware...");
    vfsc_boot_line("IOKit: matching USB device tree...");
    vfsc_boot_line("com.hack-touchid.client @ 0x0000 (v%s)", VFS5011_PROJECT_VERSION);
    vfsc_boot_line("libusb-1.0: context initialized");

    vfsc_boot_line("NVRAM: reading IODeviceTree:/options...");
    if (!check_opencore_version_requirement()) {
        return 1;
    }

    check_macos_version_warning();

    vfsc_boot_line("USB: probing supported_sensors.h device table...");
    if (!check_sensor_presence_gate()) {
        return 1;
    }
    vfsc_boot_line("Sensor descriptor matched, claiming interface...");

    vfsc_boot_line("launchd: querying installed daemon version...");
    if (!check_daemon_version_gate()) {
        return 1;
    }

    vfsc_boot_line("VFSStore: mounting encrypted APFS volume...");
    vfsc_boot_line("AX: checking Accessibility grant...");

    /* Mount the template volume once up front to find out what's
     * actually enrolled, so the status line below doesn't have to
     * show "Unknown" until the user happens to hit Enroll/Verify.
     * Failure here (e.g. volume not set up yet) just leaves the
     * count at 0/unset -- Settings will explain why if relevant. */
    vfsc_status_line("Checking enrolled fingers...");
    refresh_finger_cache();
    vfsc_boot_line("Template DB: %d enrolled", g_finger_count > 0 ? g_finger_count : 0);
    vfsc_boot_line("hack-touchid: init complete");

    /* Banner prints LAST in verbose mode, once the whole boot log has
     * scrolled by -- reads as "boot finished, here's the app" rather
     * than a logo sitting in the middle of a log stream. Quiet mode
     * already printed it up front (see above), so skip it here to
     * avoid a duplicate. */
    if (g_verbose_boot) {
        print_banner();
    }

    char line[64];
    for (;;) {
        print_menu();
        printf("<Hack-touchid> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) {
            printf("\n");
            break;
        }
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' || line[len-1] == ' ')) {
            line[--len] = '\0';
        }
        if (len == 0) continue;

        char cmd = line[0];
        printf("\n");
        bool ran_action = true;
        switch (cmd) {
            case '1': do_enroll(); break;
            case '2': do_verify(); break;
            case '3': do_deploy(); break;
            case 'S': case 's': do_settings_menu(); break;
            case 'A': case 'a': print_about(); break;
            case 'Q': case 'q':
                printf("Exiting Hack-TouchID Client.\n");
                return 0;
            default:
                printf("Unrecognized option '%s'. Choose 1, 2, 3, S, A, or Q.\n\n", line);
                ran_action = false;
        }
        if (ran_action) {
            clear_screen_and_redraw_banner();
        }
    }
    return 0;
}
