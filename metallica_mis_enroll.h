#ifndef __METALLICA_MIS_ENROLL_H
#define __METALLICA_MIS_ENROLL_H

#include <stdint.h>

/*
 * metallica_mis_enroll.h
 *
 * First native enrollment on a Metallica MIS sensor (06cb:009a family),
 * a C port of python-validity's Sensor.enroll() / capture() /
 * append_new_image() / make_finger_data() (validitysensor/sensor.py,
 * MIT, uunicorn/python-validity), driven by the tester's --enroll-test
 * flag. Match-in-sensor: the print lives on the sensor, not on the Mac.
 *
 * What it does, in order (every step is logged under --debug):
 *   1. open + plaintext bootstrap + secure session (same as --list-records)
 *   2. list what is already stored on the sensor
 *   3. calibrate in this same session (keep the finger OFF the sensor):
 *      ENROLL captures need the calibration data in memory and this
 *      client keeps no on-disk cache of it
 *   4. enrollment loop: touch the sensor, the sensor returns an updated
 *      template and a "header"; repeat until it hands back a template id
 *   5. save the print on the sensor (user record + finger record)
 *   6. list the records again to show the result
 *
 * NOT YET VERIFIED against real hardware: this port is only checked
 * against the python reference by reading it. The match half lives in
 * metallica_mis_do_verify_test() below (--verify-test).
 *
 * Returns 0 on success (a finger record was saved), -1 on failure. On
 * failure it says which step stopped, and for a rejected save (status
 * 0x04c3) it points at --wipe-records. Does not wipe anything itself.
 */
int metallica_mis_do_enroll_test(void);

/* Return value shared by metallica_mis_do_enroll_test() and metallica_mis_do_verify_test():
 * 0 success, -1 failure, MMIS_SESSION_FAILED (-2) when the secure session could not be
 * opened and the sensor did not just reboot after a fresh pairing, which usually means
 * the host identity (product name + serial) does not match the one the sensor was paired
 * with. The client reacts by asking for that identity once. */
#define MMIS_SESSION_FAILED (-2)
/* Returned by metallica_mis_do_enroll_guided() when the person declined to replace the
 * prints already stored on the sensor. Nothing was changed. */
#define MMIS_ENROLL_CANCELLED (-3)

/* Asked once, after the sensor's stored prints are counted, when there is at least one.
 * Return 1 to delete ALL user records on the sensor and enroll, 0 to cancel. */
typedef int (*mmis_replace_cb)(int users_on_sensor);

/* The enrollment behind menu item [1]: same steps as metallica_mis_do_enroll_test() but
 * without the technical output, and an existing print is handled through `replace_cb`
 * instead of telling the person to run --wipe-records. Returns 0, -1,
 * MMIS_SESSION_FAILED or MMIS_ENROLL_CANCELLED. */
int metallica_mis_do_enroll_guided(mmis_replace_cb replace_cb);

/* Returned by metallica_mis_do_verify_guided() when the sensor captured the touch but did not
 * recognise the finger. 0 is a match, MMIS_SESSION_FAILED and -1 are as above. */
#define MMIS_VERIFY_NO_MATCH 1

/* The verification behind menu item [2]: same steps as metallica_mis_do_verify_test() with
 * plain-language output. */
int metallica_mis_do_verify_guided(void);

/*
 * metallica_mis_do_verify_test(): first native verify on a Metallica MIS
 * sensor, a C port of python-validity's Sensor.identify() / match_finger()
 * (capture in IDENTIFY mode, then 0x5e match, 0x60 result, 0x62 cleanup).
 * Match-in-sensor: it needs a print saved by --enroll-test. Calibrates,
 * asks for one touch, and reports MATCH (user record, subtype) or NO MATCH.
 *
 * NOT YET VERIFIED against real hardware, and it cannot be until enroll
 * completes there. Returns 0 on MATCH, -1 on NO MATCH or any failure.
 */
int metallica_mis_do_verify_test(void);

/* ---- daemon API (lock screen / padlock unlock) ----
 * One secure session is kept open for a lock episode; calibration is done once per process
 * and kept in memory. Not thread-safe: call from the single polling thread. */
#define MMIS_AGENT_MATCH      0
#define MMIS_AGENT_NO_MATCH   1
#define MMIS_AGENT_NO_FINGER (-2)   /* nobody touched the sensor in time */
#define MMIS_AGENT_ABORTED   (-4)   /* the abort callback fired: close and reopen before reuse */
#define MMIS_AGENT_ERROR     (-1)   /* close and reopen before reuse */

/* Polled while waiting for a finger (about every 300 ms). Return non-zero to stop waiting. */
void metallica_mis_agent_set_abort_cb(int (*cb)(void));

/* Opens the sensor, secure session, counts stored prints, calibrates (first call only).
 * Returns 0, -1, or MMIS_SESSION_FAILED when the host identity is wrong. */
int metallica_mis_agent_open(void);

/* Number of prints the sensor held when the session was opened. */
int metallica_mis_agent_prints(void);

/* Relights the sensor, waits up to 30 s for a touch, asks the sensor to match it. */
int metallica_mis_agent_identify(uint32_t *usr_id, uint16_t *subtype);

void metallica_mis_agent_close(void);

#endif /* __METALLICA_MIS_ENROLL_H */
