#ifndef __METALLICA_MIS_ENROLL_H
#define __METALLICA_MIS_ENROLL_H

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

#endif /* __METALLICA_MIS_ENROLL_H */
