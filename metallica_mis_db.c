/*
 * metallica_mis_db.c -- see metallica_mis_db.h.
 */

#include "metallica_mis_db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "metallica_mis_debug.h"
#include "metallica_mis_flash.h"

#define DB_REPLY_MAX 4096

static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr16(unsigned char *p, uint16_t v) { p[0] = (unsigned char)(v & 0xff); p[1] = (unsigned char)(v >> 8); }

/* One command round trip with full logging. Returns reply length, or -1. */
static int db_cmd(metallica_mis_tls_t *tls, const char *what, const unsigned char *cmd, size_t cmd_len,
                  unsigned char *rsp, size_t rsp_cap) {
    mmis_dbg("db: %s -> sending cmd 0x%02x (%zu bytes)", what, cmd[0], cmd_len);
    mmis_dbg_hex("db: command", cmd, cmd_len);
    int n = metallica_mis_tls_cmd(tls, cmd, cmd_len, rsp, rsp_cap);
    if (n < 0) {
        fprintf(stderr, "metallica_mis_db: %s: transport/session failure\n", what);
        mmis_dbg("db: %s -> transport/session failure (tls_cmd returned %d)", what, n);
        return -1;
    }
    mmis_dbg_hex("db: reply", rsp, (size_t)n);
    mmis_dbg_status(what, rsp, (size_t)n);
    return n;
}

/* python's assert_status(): reply must carry a 0x0000 status word. */
static int status_ok(const char *what, const unsigned char *rsp, int n) {
    if (n < 2) {
        fprintf(stderr, "metallica_mis_db: %s: reply too short (%d bytes)\n", what, n);
        return -1;
    }
    uint16_t st = rd16(rsp);
    if (st != 0) {
        fprintf(stderr, "metallica_mis_db: %s: command failed, status=0x%04x (%s)\n",
                what, st, mmis_status_name(st));
        return -1;
    }
    return 0;
}

int mmis_db_info(metallica_mis_tls_t *tls, mmis_db_info_t *out) {
    unsigned char cmd[1] = { 0x45 };
    unsigned char rsp[DB_REPLY_MAX];
    int n = db_cmd(tls, "db_info", cmd, sizeof(cmd), rsp, sizeof(rsp));
    if (n < 0 || status_ok("db_info", rsp, n) != 0) return -1;
    if (n < 2 + 0x18) {
        fprintf(stderr, "metallica_mis_db: db_info: reply too short (%d bytes)\n", n);
        return -1;
    }
    const unsigned char *p = rsp + 2;
    memset(out, 0, sizeof(*out));
    /* <LLLLLHH: unknown1, unknown0, total, used, free, records, nroots */
    out->total = rd32(p + 8);
    out->used = rd32(p + 12);
    out->free_bytes = rd32(p + 16);
    out->records = rd16(p + 20);
    out->root_count = rd16(p + 22);
    p += 0x18;
    int avail = (n - 2 - 0x18) / 2;
    for (int i = 0; i < out->root_count && i < avail && i < 32; i++) out->roots[i] = rd16(p + i * 2);
    mmis_dbg("db_info: total=%u used=%u free=%u records=%u roots=%u",
             out->total, out->used, out->free_bytes, out->records, out->root_count);
    return 0;
}

int mmis_db_get_user_storage(metallica_mis_tls_t *tls, uint16_t dbid, const char *name,
                             mmis_db_storage_t *out) {
    unsigned char cmd[5 + 64];
    size_t nlen = (name && *name) ? strlen(name) + 1 : 0;
    if (nlen > 64) return -1;
    cmd[0] = 0x4b;
    wr16(cmd + 1, dbid);
    wr16(cmd + 3, (uint16_t)nlen);
    if (nlen) memcpy(cmd + 5, name, nlen); /* includes the trailing NUL, as python does */

    unsigned char rsp[DB_REPLY_MAX];
    int n = db_cmd(tls, "get_user_storage", cmd, 5 + nlen, rsp, sizeof(rsp));
    if (n < 2) return -1;
    if (rd16(rsp) == 0x04b3) {
        mmis_dbg("get_user_storage: storage not found (0x04b3)");
        return 1;
    }
    if (status_ok("get_user_storage", rsp, n) != 0) return -1;

    if (n < 2 + 8) { fprintf(stderr, "metallica_mis_db: get_user_storage: short reply\n"); return -1; }
    const unsigned char *p = rsp + 2;
    size_t left = (size_t)n - 2;
    uint16_t recid = rd16(p), usercnt = rd16(p + 2), namesz = rd16(p + 4), unk = rd16(p + 6);
    p += 8; left -= 8;
    if ((size_t)usercnt * 4 + namesz > left) {
        fprintf(stderr, "metallica_mis_db: get_user_storage: reply shorter than its own header claims "
                        "(usercnt=%u namesz=%u have=%zu)\n", usercnt, namesz, left);
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->dbid = recid;
    out->user_count = usercnt;
    for (int i = 0; i < usercnt && i < MMIS_DB_MAX_USERS; i++) {
        out->users[i].dbid = rd16(p + i * 4);
        out->users[i].value_size = rd16(p + i * 4 + 2);
        out->users_stored++;
    }
    p += (size_t)usercnt * 4; left -= (size_t)usercnt * 4;
    size_t copy = namesz < sizeof(out->name) - 1 ? namesz : sizeof(out->name) - 1;
    memcpy(out->name, p, copy);
    left -= namesz;
    if (left > 0) mmis_dbg("get_user_storage: %zu unexpected trailing bytes (python would raise)", left);
    mmis_dbg("get_user_storage: dbid=%u users=%u namesz=%u unknown=0x%04x name=\"%s\"",
             recid, usercnt, namesz, unk, out->name);
    if (usercnt > MMIS_DB_MAX_USERS)
        fprintf(stderr, "metallica_mis_db: warning: sensor reports %u users, only the first %d are tracked\n",
                usercnt, MMIS_DB_MAX_USERS);
    return 0;
}

int mmis_db_get_user(metallica_mis_tls_t *tls, uint16_t dbid, mmis_db_user_t *out) {
    unsigned char cmd[7] = { 0x4a };
    wr16(cmd + 1, dbid); wr16(cmd + 3, 0); wr16(cmd + 5, 0);
    unsigned char rsp[DB_REPLY_MAX];
    int n = db_cmd(tls, "get_user", cmd, sizeof(cmd), rsp, sizeof(rsp));
    if (n < 0 || status_ok("get_user", rsp, n) != 0) return -1;
    if (n < 2 + 8) { fprintf(stderr, "metallica_mis_db: get_user: short reply\n"); return -1; }

    const unsigned char *p = rsp + 2;
    size_t left = (size_t)n - 2;
    uint16_t recid = rd16(p), fingercnt = rd16(p + 2), unk = rd16(p + 4), idsz = rd16(p + 6);
    p += 8; left -= 8;
    if ((size_t)fingercnt * 8 + idsz > left) {
        fprintf(stderr, "metallica_mis_db: get_user: reply shorter than its own header claims "
                        "(fingers=%u identity=%u have=%zu)\n", fingercnt, idsz, left);
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->dbid = recid;
    out->finger_count = fingercnt;
    for (int i = 0; i < fingercnt && i < MMIS_DB_MAX_FINGERS; i++) {
        const unsigned char *f = p + i * 8;
        out->fingers[i].dbid = rd16(f);
        out->fingers[i].subtype = rd16(f + 2);
        out->fingers[i].storage = rd16(f + 4);
        out->fingers[i].value_size = rd16(f + 6);
        out->fingers_stored++;
    }
    p += (size_t)fingercnt * 8; left -= (size_t)fingercnt * 8;
    out->identity_len = idsz < MMIS_DB_MAX_IDENTITY ? idsz : MMIS_DB_MAX_IDENTITY;
    memcpy(out->identity, p, out->identity_len);
    left -= idsz;
    if (left > 0) mmis_dbg("get_user: %zu unexpected trailing bytes (python would raise)", left);
    mmis_dbg("get_user: dbid=%u fingers=%u unknown=0x%04x identity_len=%u", recid, fingercnt, unk, idsz);
    mmis_dbg_hex("get_user: identity blob", out->identity, out->identity_len);
    return 0;
}

int mmis_db_del_record(metallica_mis_tls_t *tls, uint16_t dbid) {
    unsigned char cmd[3] = { 0x48 };
    wr16(cmd + 1, dbid);
    unsigned char rsp[64];
    int n = db_cmd(tls, "del_record", cmd, sizeof(cmd), rsp, sizeof(rsp));
    if (n < 0) return -1;
    return status_ok("del_record", rsp, n);
}

int mmis_db_new_record(metallica_mis_tls_t *tls, uint16_t parent, uint16_t type,
                       uint16_t storage, const unsigned char *data, size_t data_len,
                       uint16_t *recid_out) {
    if (data_len > 0xffff) return -1;
    mmis_db_info_t info;
    if (mmis_db_info(tls, &info) != 0) return -1; /* python calls this first too */

    mmis_dbg("new_record: parent=%u type=0x%x storage=%u data_len=%zu", parent, type, storage, data_len);
    unsigned char rsp[DB_REPLY_MAX];

    if (metallica_mis_write_enable(tls) != 0) {
        fprintf(stderr, "metallica_mis_db: new_record: db_write_enable failed\n");
        return -1;
    }

    int rc = -1;
    unsigned char *cmd = (unsigned char *)malloc(9 + data_len);
    if (!cmd) goto cleanup;
    cmd[0] = 0x47;
    wr16(cmd + 1, parent); wr16(cmd + 3, type); wr16(cmd + 5, storage); wr16(cmd + 7, (uint16_t)data_len);
    if (data_len) memcpy(cmd + 9, data, data_len);

    int n = db_cmd(tls, "new_record", cmd, 9 + data_len, rsp, sizeof(rsp));
    free(cmd);
    if (n < 0) goto cleanup;
    if (status_ok("new_record", rsp, n) != 0) goto cleanup;
    if (n < 4) { fprintf(stderr, "metallica_mis_db: new_record: reply missing record id\n"); goto cleanup; }
    if (recid_out) *recid_out = rd16(rsp + 2);
    mmis_dbg("new_record: created record id %u", rd16(rsp + 2));
    rc = 0;

cleanup:
    /* python: try/finally call_cleanups() -- always runs. */
    if (metallica_mis_flash_call_cleanups(tls) != 0) {
        fprintf(stderr, "metallica_mis_db: new_record: call_cleanups failed\n");
        rc = -1;
    }
    return rc;
}

int mmis_db_list(metallica_mis_tls_t *tls, int *user_count_out) {
    if (user_count_out) *user_count_out = 0;

    mmis_db_info_t info;
    if (mmis_db_info(tls, &info) == 0) {
        printf("Sensor database: %u bytes total, %u used, %u free, %u records\n",
               info.total, info.used, info.free_bytes, info.records);
    }

    mmis_db_storage_t stg;
    int rc = mmis_db_get_user_storage(tls, 0, MMIS_DB_STORAGE_NAME, &stg);
    if (rc < 0) return -1;
    if (rc == 1) {
        printf("No \"%s\" storage on this sensor: nothing enrolled.\n", MMIS_DB_STORAGE_NAME);
        return 0;
    }

    printf("Storage \"%s\" (dbid %u): %d user record(s)\n", stg.name, stg.dbid, stg.user_count);
    for (int i = 0; i < stg.users_stored; i++) {
        mmis_db_user_t u;
        if (mmis_db_get_user(tls, stg.users[i].dbid, &u) != 0) {
            printf("  user dbid %u: could not be read\n", stg.users[i].dbid);
            continue;
        }
        printf("  user dbid %u: %d finger(s)\n", u.dbid, u.finger_count);
        for (int f = 0; f < u.fingers_stored; f++) {
            printf("    finger dbid %u, subtype 0x%02x, %u bytes\n",
                   u.fingers[f].dbid, u.fingers[f].subtype, u.fingers[f].value_size);
        }
    }
    if (user_count_out) *user_count_out = stg.user_count;
    return 0;
}

int mmis_db_wipe_users(metallica_mis_tls_t *tls, int *deleted_out) {
    if (deleted_out) *deleted_out = 0;

    mmis_db_storage_t stg;
    int rc = mmis_db_get_user_storage(tls, 0, MMIS_DB_STORAGE_NAME, &stg);
    if (rc < 0) return -1;
    if (rc == 1 || stg.user_count == 0) {
        mmis_dbg("wipe_users: nothing to delete");
        return 0;
    }
    if (stg.user_count > stg.users_stored) {
        fprintf(stderr, "metallica_mis_db: sensor reports %d users but only %d can be tracked; "
                        "will delete those and re-check\n", stg.user_count, stg.users_stored);
    }

    int deleted = 0;
    for (int i = 0; i < stg.users_stored; i++) {
        mmis_dbg("wipe_users: deleting user record %u (%d of %d)", stg.users[i].dbid, i + 1, stg.users_stored);
        if (mmis_db_del_record(tls, stg.users[i].dbid) != 0) return -1;
        deleted++;
    }
    if (deleted_out) *deleted_out = deleted;

    /* Verify by reading back -- the tester's run showed "not found" (0x04b3)
     * or an empty user table after the delete, so either counts as gone. */
    mmis_db_storage_t after;
    rc = mmis_db_get_user_storage(tls, 0, MMIS_DB_STORAGE_NAME, &after);
    if (rc < 0) return -1;
    if (rc == 1 || after.user_count == 0) {
        mmis_dbg("wipe_users: verified, no user records remain");
        return 0;
    }
    fprintf(stderr, "metallica_mis_db: wipe verification FAILED: %d user record(s) still present "
                    "after delete\n", after.user_count);
    return -1;
}

/* ---- enroll-side helpers (python db.py: lookup_user / new_user /
 * new_user_storate / new_finger). Used by metallica_mis_enroll.c. ---- */

int mmis_db_ensure_storage(metallica_mis_tls_t *tls, uint16_t *dbid_out) {
    mmis_db_storage_t stg;
    int rc = mmis_db_get_user_storage(tls, 0, MMIS_DB_STORAGE_NAME, &stg);
    if (rc < 0) return -1;
    if (rc == 1) {
        /* python: db.new_record(1, 4, 3, b'StgWindsor\0') */
        static const unsigned char name[] = MMIS_DB_STORAGE_NAME "\0";
        mmis_dbg("ensure_storage: no \"%s\" storage yet, creating it", MMIS_DB_STORAGE_NAME);
        if (mmis_db_new_record(tls, 1, 4, 3, name, sizeof(name) - 1, NULL) != 0) return -1;
        rc = mmis_db_get_user_storage(tls, 0, MMIS_DB_STORAGE_NAME, &stg);
        if (rc != 0) {
            fprintf(stderr, "metallica_mis_db: storage was created but cannot be read back\n");
            return -1;
        }
    }
    if (dbid_out) *dbid_out = stg.dbid;
    return 0;
}

int mmis_db_lookup_user(metallica_mis_tls_t *tls, const unsigned char *identity, size_t identity_len,
                        uint16_t *dbid_out) {
    if (identity_len > 256) return -1;
    uint16_t stg_dbid = 0;
    if (mmis_db_ensure_storage(tls, &stg_dbid) != 0) return -1;

    unsigned char cmd[7 + 256];
    cmd[0] = 0x4a;
    wr16(cmd + 1, 0); wr16(cmd + 3, stg_dbid); wr16(cmd + 5, (uint16_t)identity_len);
    memcpy(cmd + 7, identity, identity_len);

    unsigned char rsp[DB_REPLY_MAX];
    int n = db_cmd(tls, "lookup_user", cmd, 7 + identity_len, rsp, sizeof(rsp));
    if (n < 2) return -1;
    if (rd16(rsp) == 0x04b3) {
        mmis_dbg("lookup_user: no user with this identity (0x04b3)");
        return 1;
    }
    if (status_ok("lookup_user", rsp, n) != 0) return -1;
    if (n < 4) { fprintf(stderr, "metallica_mis_db: lookup_user: reply missing record id\n"); return -1; }
    if (dbid_out) *dbid_out = rd16(rsp + 2);
    mmis_dbg("lookup_user: found user record %u", rd16(rsp + 2));
    return 0;
}

int mmis_db_new_user(metallica_mis_tls_t *tls, const unsigned char *identity, size_t identity_len,
                     uint16_t *dbid_out) {
    uint16_t stg_dbid = 0;
    if (mmis_db_ensure_storage(tls, &stg_dbid) != 0) return -1;
    /* python: new_record(stg.dbid, 5, stg.dbid, identity bytes) */
    return mmis_db_new_record(tls, stg_dbid, 5, stg_dbid, identity, identity_len, dbid_out);
}

int mmis_db_new_finger(metallica_mis_tls_t *tls, uint16_t user_dbid,
                       const unsigned char *tinfo, size_t tinfo_len, uint16_t *recid_out) {
    uint16_t stg_dbid = 0;
    if (mmis_db_ensure_storage(tls, &stg_dbid) != 0) return -1;
    /* python: new_record(userid, 0xb, stg.dbid, template). We ask for type
     * 0xb; the sensor's db_write_enable blob turns it into 0x6. */
    return mmis_db_new_record(tls, user_dbid, 0xb, stg_dbid, tinfo, tinfo_len, recid_out);
}
