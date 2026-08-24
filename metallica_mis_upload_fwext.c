/*
 * metallica_mis_upload_fwext.c
 *
 * See metallica_mis_upload_fwext.h for scope/porting notes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <limits.h>

#include "metallica_mis_upload_fwext.h"
#include "metallica_mis_flash.h"
#include "metallica_mis_firmware.h"

#define MMUF_SIGNATURE_LEN 0x100

static void mmuf_info(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static void mmuf_err(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "metallica_mis_upload_fwext: ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* Reads the whole file at path into a malloc'd buffer. Returns 0 on
 * success (*out_buf/*out_len populated, caller must free(*out_buf)),
 * -1 on any I/O failure. */
static int read_whole_file(const char *path, unsigned char **out_buf, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        mmuf_err("couldn't open firmware file: %s\n", path);
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long size = ftell(f);
    if (size < 0) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }

    unsigned char *buf = malloc((size_t)size);
    if (!buf) { fclose(f); return -1; }

    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (n != (size_t)size) {
        mmuf_err("short read on firmware file (expected %ld, got %zu)\n", size, n);
        free(buf);
        return -1;
    }

    *out_buf = buf;
    *out_len = (size_t)size;
    return 0;
}

int metallica_mis_upload_fwext(metallica_mis_tls_t *tls) {
    bool fw_present = false;
    metallica_mis_fw_info_t fwi;

    /* Step 1: get_fw_info(2) -- early return if already loaded. */
    if (metallica_mis_get_fw_info(tls, 2, &fw_present, &fwi) != 0) {
        mmuf_err("get_fw_info() failed\n");
        return -1;
    }
    if (fw_present) {
        time_t bt = (time_t)fwi.buildtime;
        mmuf_info("Detected firmware version %u.%u (%s)\n",
                   fwi.major, fwi.minor, ctime(&bt));
        metallica_mis_fw_info_free(&fwi);
        return 0;
    }
    mmuf_info("No firmware detected. Uploading...\n");

    /* Step 2: the "no idea what this is" register precondition,
     * ported verbatim from upstream -- see header comment. */
    if (metallica_mis_write_hw_reg32(tls, 0x8000205c, 7) != 0) {
        mmuf_err("write_hw_reg32(0x8000205c, 7) failed\n");
        return -1;
    }
    uint32_t reg_val = 0;
    if (metallica_mis_read_hw_reg32(tls, 0x80002080, &reg_val) != 0) {
        mmuf_err("read_hw_reg32(0x80002080) failed\n");
        return -1;
    }
    if (reg_val != 2 && reg_val != 3) {
        mmuf_err("unexpected register value: got %u, expected 2 or 3\n", reg_val);
        return -1;
    }

    /* Step 3: resolve + ensure the firmware file is on disk.
     * identify_sensor() is intentionally not ported -- see header
     * note, we already know the exact filename for 06cb:009a. */
    if (!metallica_mis_firmware_is_present()) {
        mmuf_info("Firmware file not present locally, fetching...\n");
        if (!metallica_mis_firmware_fetch()) {
            mmuf_err("could not obtain firmware file\n");
            return -1;
        }
    }
    char fw_path[PATH_MAX];
    if (metallica_mis_firmware_path(fw_path, sizeof(fw_path)) != 0) {
        mmuf_err("could not resolve firmware path\n");
        return -1;
    }

    /* Step 4: read file, find first raw 0x1a byte, drop everything up
     * to and including it, split the last 0x100 bytes off as the
     * signature. Ported verbatim -- see header note, meaning of the
     * 0x1a-prefixed header isn't documented upstream either. */
    unsigned char *raw = NULL;
    size_t raw_len = 0;
    if (read_whole_file(fw_path, &raw, &raw_len) != 0) {
        return -1;
    }

    size_t marker = 0;
    bool found_marker = false;
    for (size_t i = 0; i < raw_len; i++) {
        if (raw[i] == 0x1a) {
            marker = i;
            found_marker = true;
            break;
        }
    }
    if (!found_marker) {
        mmuf_err("firmware file has no 0x1a marker byte -- not a valid xpfwext file?\n");
        free(raw);
        return -1;
    }

    unsigned char *fwext = raw + marker + 1;
    size_t fwext_total_len = raw_len - (marker + 1);

    if (fwext_total_len < MMUF_SIGNATURE_LEN) {
        mmuf_err("firmware file too short after the 0x1a marker (%zu bytes, need >= %d)\n",
                  fwext_total_len, MMUF_SIGNATURE_LEN);
        free(raw);
        return -1;
    }

    size_t fwext_body_len = fwext_total_len - MMUF_SIGNATURE_LEN;
    unsigned char *fwext_body = fwext;
    unsigned char *signature = fwext + fwext_body_len;

    /* Step 5 + 6: write the body, then the signature. */
    mmuf_info("Writing firmware body (%zu bytes)...\n", fwext_body_len);
    if (metallica_mis_write_flash_all(tls, 2, 0, fwext_body, fwext_body_len) != 0) {
        mmuf_err("write_flash_all() failed\n");
        free(raw);
        return -1;
    }
    if (metallica_mis_write_fw_signature(tls, 2, signature, MMUF_SIGNATURE_LEN) != 0) {
        mmuf_err("write_fw_signature() failed\n");
        free(raw);
        return -1;
    }
    free(raw);

    /* Step 7: verify the upload actually took. */
    bool fw_present_after = false;
    metallica_mis_fw_info_t fwi_after;
    if (metallica_mis_get_fw_info(tls, 2, &fw_present_after, &fwi_after) != 0) {
        mmuf_err("get_fw_info() (post-upload) failed\n");
        return -1;
    }
    if (!fw_present_after) {
        mmuf_err("no firmware detected after upload\n");
        return -1;
    }
    time_t bt = (time_t)fwi_after.buildtime;
    mmuf_info("Loaded FWExt version %u.%u (%s), %zu modules\n",
               fwi_after.major, fwi_after.minor, ctime(&bt), fwi_after.module_count);
    metallica_mis_fw_info_free(&fwi_after);

    /* Step 8: reboot. See header note -- caller must not send
     * anything else to tls/the transport after this returns 0. */
    mmuf_info("Firmware upload complete. Sending reboot command...\n");
    if (metallica_mis_reboot(tls) != 0) {
        mmuf_err("reboot command failed (firmware upload itself may still have "
                  "succeeded -- flash was already written before this point; "
                  "only the reboot command itself failed to send/ack)\n");
        return -1;
    }

    mmuf_info("Reboot command sent. Device should be re-enumerating now.\n");
    return 0;
}
