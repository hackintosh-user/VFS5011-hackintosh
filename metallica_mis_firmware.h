#ifndef __METALLICA_MIS_FIRMWARE_H
#define __METALLICA_MIS_FIRMWARE_H

/*
 * metallica_mis_firmware.h
 *
 * On-demand firmware fetch for the Synaptics Metallica MIS sensor
 * (06cb:009a). This sensor needs a proprietary Synaptics/Lenovo
 * firmware blob (6_07f_lenovo_mis_qm.xpfwext) uploaded to it during
 * first-run pairing (see upload_fwext() in python-validity's
 * upload_fwext.py, which this project's pairing port is based on).
 *
 * WHY THIS EXISTS AS CLIENT CODE, NOT JUST A STANDALONE SCRIPT:
 * leaving this as a one-off shell script is easy for a tester to
 * lose track of, run from the wrong directory, or accidentally
 * delete after running once. Wiring it into hack-touchid means the
 * client detects a not-yet-paired Metallica MIS sensor and fetches
 * the firmware automatically into a fixed, predictable location
 * before enrollment ever starts -- same spirit as how
 * vfs5011_setup_volume.sh's one-time setup became a checked
 * precondition rather than something the user has to remember to
 * run first.
 *
 * WHY WE STILL SHELL OUT TO curl/innoextract RATHER THAN LINKING A
 * library: innoextract has no stable C API meant for embedding, and
 * shelling out matches how the rest of hack_touchid_client.c already
 * handles similar one-off external-tool calls (see
 * is_accessibility_granted()'s sqlite3 popen() call). Keeps this
 * function simple and matches existing project conventions rather
 * than introducing a new dependency-linking pattern for one feature.
 *
 * LICENSING NOTE (do not change this without re-reading the
 * reasoning in metallica_mis_fetch_firmware.sh): the firmware itself
 * is never bundled or hosted by this project. Every call in this
 * file either downloads directly from Lenovo's own CDN or fails and
 * tells the user how to get it manually from Lenovo's site. Do not
 * add any URL here that isn't Lenovo's own download.lenovo.com.
 */

#include <stdbool.h>

/* Exact firmware filename this sensor variant needs. Matches
 * FIRMWARE_NAMES's entry for 06cb:009a in python-validity's
 * firmware_tables.py. */
#define METALLICA_MIS_FW_FILENAME "6_07f_lenovo_mis_qm.xpfwext"

/* Fills out_path (a buffer of at least PATH_MAX bytes) with the full
 * path where the firmware should live: ~/.hack-touchid/firmware/<name>.
 * Does not check whether the file actually exists there -- use
 * metallica_mis_firmware_is_present() for that. Returns 0 on success,
 * -1 if $HOME couldn't be resolved. */
int metallica_mis_firmware_path(char *out_path, int out_path_size);

/* Cheap existence+non-empty check against the path from
 * metallica_mis_firmware_path(). Does NOT validate file contents or
 * size -- just enough to decide whether fetch is needed. */
bool metallica_mis_firmware_is_present(void);

/* Runs the fetch: checks for innoextract, downloads the driver
 * installer from Lenovo, extracts just the firmware file, sanity
 * checks its size, and copies it to the path from
 * metallica_mis_firmware_path(). Prints user-facing progress/error
 * messages itself (matching the rest of the client's style) rather
 * than returning error codes for the caller to translate. Returns
 * true on success, false on any failure (already logged by this
 * function -- caller doesn't need to print anything more). */
bool metallica_mis_firmware_fetch(void);

#endif /* __METALLICA_MIS_FIRMWARE_H */
