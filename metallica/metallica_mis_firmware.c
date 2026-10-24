/*
 * metallica_mis_firmware.c
 *
 * See metallica_mis_firmware.h for the why/licensing rationale.
 * This is a C port of metallica_mis_fetch_firmware.sh's logic,
 * wired into the client instead of left as a standalone script.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <limits.h>

#include "metallica_mis_firmware.h"

/* Same source as the standalone script -- Lenovo's own CDN, the
 * official driver for ThinkPad X1 Carbon 6th Gen (Type 20KH/20KG),
 * confirmed correct for USB 06cb:009a as of Aug 2026. */
#define METALLICA_MIS_DRIVER_URL "https://download.lenovo.com/pccbbs/mobiles/nz3gf09w.exe"
#define METALLICA_MIS_DRIVER_FILENAME "nz3gf09w.exe"
#define METALLICA_MIS_DOWNLOAD_PAGE \
    "https://pcsupport.lenovo.com/us/en/products/laptops-and-netbooks/" \
    "thinkpad-x-series-laptops/thinkpad-x1-carbon-6th-gen-type-20kh-20kg/downloads/ds502265"

/* Known-good size as of Aug 2026 (218482 bytes exactly). Used only
 * for a soft sanity check/warning, not a hard pass/fail gate --
 * Lenovo may ship a firmware revision with a different size someday
 * and that isn't necessarily wrong. */
#define METALLICA_MIS_FW_SIZE_MIN 100000
#define METALLICA_MIS_FW_SIZE_MAX 500000

/* Matches vfsc_err()'s spirit from hack_touchid_client.c, kept
 * self-contained here so this file has no hard dependency on the
 * client's color macros / globals. */
static void mmfw_err(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "Error: ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static void mmfw_info(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

int metallica_mis_firmware_path(char *out_path, int out_path_size) {
    const char *home = getenv("HOME");
    if (!home || home[0] == '\0') {
        return -1;
    }
    int n = snprintf(out_path, (size_t)out_path_size,
                      "%s/.hack-touchid/firmware/%s", home, METALLICA_MIS_FW_FILENAME);
    if (n < 0 || n >= out_path_size) {
        return -1;
    }
    return 0;
}

bool metallica_mis_firmware_is_present(void) {
    char path[PATH_MAX];
    if (metallica_mis_firmware_path(path, sizeof(path)) != 0) {
        return false;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }
    return st.st_size > 0;
}

/* Runs a shell command, returns its exit status (0 == success), and
 * discards stdout/stderr from OUR perspective -- the child still
 * writes directly to the terminal (system() doesn't redirect), so
 * curl's own progress bar / innoextract's own output are visible to
 * the user as normal. This matches how the standalone script behaves
 * when run directly. */
static int run_cmd(const char *cmd) {
    int rc = system(cmd);
    if (rc == -1) return -1;
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

bool metallica_mis_firmware_fetch(void) {
    char fw_dest_path[PATH_MAX];
    if (metallica_mis_firmware_path(fw_dest_path, sizeof(fw_dest_path)) != 0) {
        mmfw_err("Could not resolve $HOME to determine firmware install path.\n");
        return false;
    }

    if (metallica_mis_firmware_is_present()) {
        /* Caller should normally check this first via
         * metallica_mis_firmware_is_present(), but handle it
         * gracefully here too rather than re-fetching needlessly. */
        return true;
    }

    mmfw_info("Installing firmware from official source (Lenovo)...\n");

    if (run_cmd("command -v innoextract >/dev/null 2>&1") != 0) {
        mmfw_err("innoextract not found. Install it first:\n");
        mmfw_err("  brew install innoextract\n");
        return false;
    }

    char work_dir[] = "/tmp/hack-touchid-fw-XXXXXX";
    if (mkdtemp(work_dir) == NULL) {
        mmfw_err("Could not create a temporary working directory.\n");
        return false;
    }

    char driver_path[PATH_MAX];
    snprintf(driver_path, sizeof(driver_path), "%s/%s", work_dir, METALLICA_MIS_DRIVER_FILENAME);

    mmfw_info("Downloading driver installer from Lenovo...\n");
    char dl_cmd[1024];
    snprintf(dl_cmd, sizeof(dl_cmd), "curl -fL --progress-bar -o '%s' '%s'",
              driver_path, METALLICA_MIS_DRIVER_URL);
    if (run_cmd(dl_cmd) != 0) {
        mmfw_err("Download failed. Lenovo may have moved this file.\n");
        mmfw_err("Grab it manually from the download page instead:\n");
        mmfw_err("  %s\n", METALLICA_MIS_DOWNLOAD_PAGE);
        mmfw_err("then run:\n");
        mmfw_err("  innoextract <path-to-downloaded-exe> -e -I %s\n", METALLICA_MIS_FW_FILENAME);
        mmfw_err("and move the extracted file to: %s\n", fw_dest_path);
        char rm_cmd[PATH_MAX + 16];
        snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf '%s'", work_dir);
        run_cmd(rm_cmd);
        return false;
    }

    mmfw_info("Extracting %s (only this one file, not the whole installer)...\n",
              METALLICA_MIS_FW_FILENAME);
    char extract_cmd[PATH_MAX * 2];
    snprintf(extract_cmd, sizeof(extract_cmd), "cd '%s' && innoextract '%s' -e -I '%s'",
              work_dir, METALLICA_MIS_DRIVER_FILENAME, METALLICA_MIS_FW_FILENAME);
    if (run_cmd(extract_cmd) != 0) {
        mmfw_err("innoextract failed to run against the downloaded installer.\n");
        char rm_cmd[PATH_MAX + 16];
        snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf '%s'", work_dir);
        run_cmd(rm_cmd);
        return false;
    }

    char find_cmd[PATH_MAX * 2];
    snprintf(find_cmd, sizeof(find_cmd), "find '%s' -name '%s' -type f 2>/dev/null | head -n1",
              work_dir, METALLICA_MIS_FW_FILENAME);
    FILE *fp = popen(find_cmd, "r");
    char extracted_path[PATH_MAX] = {0};
    if (fp) {
        if (fgets(extracted_path, sizeof(extracted_path), fp)) {
            size_t len = strlen(extracted_path);
            if (len > 0 && extracted_path[len - 1] == '\n') extracted_path[len - 1] = '\0';
        }
        pclose(fp);
    }

    if (extracted_path[0] == '\0') {
        mmfw_err("Extraction ran, but %s wasn't found in the output.\n", METALLICA_MIS_FW_FILENAME);
        mmfw_err("The driver package contents may have changed. Try:\n");
        mmfw_err("  innoextract -l %s | grep -i xpfwext\n", driver_path);
        mmfw_err("to see what's actually inside this installer version.\n");
        char rm_cmd[PATH_MAX + 16];
        snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf '%s'", work_dir);
        run_cmd(rm_cmd);
        return false;
    }

    struct stat st;
    long fw_size = (stat(extracted_path, &st) == 0) ? (long)st.st_size : -1;
    mmfw_info("Extracted: %ld bytes\n", fw_size);

    if (fw_size >= 0 &&
        (fw_size < METALLICA_MIS_FW_SIZE_MIN || fw_size > METALLICA_MIS_FW_SIZE_MAX)) {
        mmfw_info("Warning: extracted file size (%ld bytes) is well outside the\n", fw_size);
        mmfw_info("expected ~218KB range. This may still be fine (Lenovo may have\n");
        mmfw_info("shipped a firmware update), but worth knowing about.\n");
    }

    /* Ensure the destination directory exists (strip the filename off
     * fw_dest_path to get its parent directory). */
    char fw_dest_dir[PATH_MAX];
    strncpy(fw_dest_dir, fw_dest_path, sizeof(fw_dest_dir) - 1);
    fw_dest_dir[sizeof(fw_dest_dir) - 1] = '\0';
    char *slash = strrchr(fw_dest_dir, '/');
    if (slash) *slash = '\0';

    char mkdir_cmd[PATH_MAX + 16];
    snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p '%s'", fw_dest_dir);
    run_cmd(mkdir_cmd);

    char cp_cmd[PATH_MAX * 2];
    snprintf(cp_cmd, sizeof(cp_cmd), "cp '%s' '%s'", extracted_path, fw_dest_path);
    bool ok = (run_cmd(cp_cmd) == 0);

    char rm_cmd[PATH_MAX + 16];
    snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf '%s'", work_dir);
    run_cmd(rm_cmd);

    if (!ok) {
        mmfw_err("Failed to install firmware to %s\n", fw_dest_path);
        return false;
    }

    mmfw_info("Firmware installed: %s\n", fw_dest_path);
    return true;
}
