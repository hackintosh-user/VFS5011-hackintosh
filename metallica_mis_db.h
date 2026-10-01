#ifndef __METALLICA_MIS_DB_H
#define __METALLICA_MIS_DB_H

/*
 * metallica_mis_db.h
 *
 * C port of the record-database half of python-validity's
 * validitysensor/db.py (pulled 2026-10-01, MIT, uunicorn/python-validity),
 * for the Metallica MIS sensor family.
 *
 * WHY THIS EXISTS: tester p0cketl1nt's Linux logs (log9-29/log9-30)
 * showed every enroll attempt capturing the full scan and then failing
 * at the very last step with status 0x04c3 in db.new_finger() ->
 * new_record(), while an earlier enroll's print was still stored on
 * the sensor. After fprintd-delete removed that print (cmd 0x48 on the
 * USER record, which takes its fingers with it), enrollment succeeded.
 * So: before the future HTID enroll path saves a new print it must be
 * able to see and clear what is already on the sensor. This file is
 * that layer. Enroll/capture itself does NOT exist yet (see
 * metallica_mis_daemon.c); nothing in the client calls
 * mmis_db_new_record() until it does.
 *
 * DATA MODEL (from db.py + the tester's wire log):
 *   storage "StgWindsor"  (dbid 3 on the tester's sensor)
 *     -> user records     (type 5, one per host identity)
 *          -> finger records (type 6 on the sensor, requested as 0xb)
 *
 * Every function needs a live, SECURE session (tls_open() done), e.g.
 * from metallica_mis_open_calibration_session(). All of them log every
 * command and reply when --debug is on (metallica_mis_debug.h).
 *
 * Return convention: 0 = success, -1 = failure (transport, status,
 * or malformed reply); mmis_db_get_user_storage() also returns 1 for
 * "does not exist" (status 0x04b3), which is a normal answer on a
 * sensor that has never stored a print.
 *
 * NOT YET VERIFIED against real hardware in this C port; the command
 * bytes and parsing mirror a hardware-verified Python trace.
 */

#include <stddef.h>
#include <stdint.h>

#include "metallica_mis_tls.h"

#define MMIS_DB_STORAGE_NAME "StgWindsor"

#define MMIS_DB_MAX_USERS   64
#define MMIS_DB_MAX_FINGERS 16
#define MMIS_DB_MAX_IDENTITY 256

typedef struct {
    uint16_t dbid;
    uint16_t value_size;
} mmis_db_user_ref_t;

typedef struct {
    uint16_t dbid;
    char name[64];
    int user_count;                      /* as reported by the sensor */
    mmis_db_user_ref_t users[MMIS_DB_MAX_USERS];
    int users_stored;                    /* min(user_count, MMIS_DB_MAX_USERS) */
} mmis_db_storage_t;

typedef struct {
    uint16_t dbid;
    uint16_t subtype;                    /* finger id, see fingerprint_constants.py upstream */
    uint16_t storage;
    uint16_t value_size;
} mmis_db_finger_t;

typedef struct {
    uint16_t dbid;
    int finger_count;
    mmis_db_finger_t fingers[MMIS_DB_MAX_FINGERS];
    int fingers_stored;
    unsigned char identity[MMIS_DB_MAX_IDENTITY];  /* raw identity blob, not decoded */
    size_t identity_len;
} mmis_db_user_t;

typedef struct {
    uint32_t total, used, free_bytes, records;
    uint16_t root_count;
    uint16_t roots[32];
} mmis_db_info_t;

/* cmd 0x45. */
int mmis_db_info(metallica_mis_tls_t *tls, mmis_db_info_t *out);

/* cmd 0x4b. name may be "" to look up by dbid. Returns 1 if not found. */
int mmis_db_get_user_storage(metallica_mis_tls_t *tls, uint16_t dbid, const char *name,
                             mmis_db_storage_t *out);

/* cmd 0x4a (by dbid). */
int mmis_db_get_user(metallica_mis_tls_t *tls, uint16_t dbid, mmis_db_user_t *out);

/* cmd 0x48. Deleting a USER record also removes its fingers. Mirrors
 * python's del_record(): no write-enable/cleanup around it (that is
 * what the hardware-verified tester run did). */
int mmis_db_del_record(metallica_mis_tls_t *tls, uint16_t dbid);

/* db_info + write_enable + cmd 0x47 + call_cleanups, as python's
 * new_record(). Writes the new record id to *recid_out. Unused until
 * enroll exists. */
int mmis_db_new_record(metallica_mis_tls_t *tls, uint16_t parent, uint16_t type,
                       uint16_t storage, const unsigned char *data, size_t data_len,
                       uint16_t *recid_out);

/* Prints the storage / users / fingers currently on the sensor to
 * stdout (human readable). *user_count_out (optional) receives the
 * number of user records found. */
int mmis_db_list(metallica_mis_tls_t *tls, int *user_count_out);

/* Deletes every user record in StgWindsor (and therefore every stored
 * print), then re-reads the storage to verify they are gone. This is
 * the "clear existing prints before enrolling" step that avoids the
 * 0x04c3 failure. *deleted_out (optional) receives how many user
 * records were removed. Returns 0 only if the verification read-back
 * shows zero users left. */
int mmis_db_wipe_users(metallica_mis_tls_t *tls, int *deleted_out);

#endif /* __METALLICA_MIS_DB_H */
