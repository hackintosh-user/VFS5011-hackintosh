/*
 * ht_sensor_tool.c
 *
 * Tiny helper for the build scripts. It includes supported_sensors.h
 * directly, so the sensor table in that header stays the single source
 * of truth for "which sensor needs which dependency". It only needs
 * clang from the Xcode Command Line Tools: no libusb, no OpenSSL, no
 * Homebrew. That is the point, the scripts can run it BEFORE any
 * dependency has been installed.
 *
 * Build:  clang ht_sensor_tool.c -o .ht_sensor_tool -I.
 *
 * Commands:
 *   ht_sensor_tool families
 *       One line per build family: "<family>|<display name>|<vid:pid list>"
 *   ht_sensor_tool resolve <spec>[,<spec>...]
 *       <spec> is a family name, a VID:PID (hex, e.g. 06cb:009a) or "all".
 *       Prints the resolved family names, one per line, de-duplicated.
 *       Exit 1 if a spec matches nothing.
 *   ht_sensor_tool deps <build|runtime|all> <family>[,<family>...]
 *       Prints the Homebrew formula names that set of families needs,
 *       one per line, de-duplicated.
 *   ht_sensor_tool detect
 *       Reads `ioreg -p IOUSB -l -w0` output on stdin and prints the
 *       families whose VID:PID is currently plugged in.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "supported_sensors.h"

#define MAX_FAMILIES 16

static int family_seen(char seen[][32], int n, const char *f) {
    for (int i = 0; i < n; i++) if (strcmp(seen[i], f) == 0) return 1;
    return 0;
}

static int cmd_families(void) {
    char seen[MAX_FAMILIES][32];
    int n = 0;
    for (size_t i = 0; i < HACK_TOUCHID_SENSOR_COUNT; i++) {
        const char *fam = HACK_TOUCHID_SENSORS[i].family;
        if (family_seen(seen, n, fam)) continue;
        if (n < MAX_FAMILIES) { snprintf(seen[n], sizeof(seen[n]), "%s", fam); n++; }

        char ids[128] = "";
        for (size_t j = i; j < HACK_TOUCHID_SENSOR_COUNT; j++) {
            if (strcmp(HACK_TOUCHID_SENSORS[j].family, fam) != 0) continue;
            char one[16];
            snprintf(one, sizeof(one), "%s%04x:%04x", ids[0] ? " " : "",
                     HACK_TOUCHID_SENSORS[j].vid, HACK_TOUCHID_SENSORS[j].pid);
            strncat(ids, one, sizeof(ids) - strlen(ids) - 1);
        }
        printf("%s|%s|%s\n", fam, HACK_TOUCHID_SENSORS[i].display_name, ids);
    }
    return 0;
}

static int add_family(char out[][32], int *n, const char *f) {
    if (family_seen(out, *n, f)) return 0;
    if (*n >= MAX_FAMILIES) return -1;
    snprintf(out[*n], 32, "%s", f);
    (*n)++;
    return 0;
}

/* Resolves one spec (family, VID:PID, or "all") into out[]. Returns 0 if
 * it matched at least one row. */
static int resolve_spec(const char *spec, char out[][32], int *n) {
    int matched = 0;
    if (strcmp(spec, "all") == 0) {
        for (size_t i = 0; i < HACK_TOUCHID_SENSOR_COUNT; i++) {
            add_family(out, n, HACK_TOUCHID_SENSORS[i].family);
            matched = 1;
        }
        return matched ? 0 : 1;
    }

    unsigned vid = 0, pid = 0;
    int is_id = (sscanf(spec, "%x:%x", &vid, &pid) == 2 && strchr(spec, ':') != NULL);
    for (size_t i = 0; i < HACK_TOUCHID_SENSOR_COUNT; i++) {
        const hack_touchid_sensor_t *s = &HACK_TOUCHID_SENSORS[i];
        if ((is_id && s->vid == vid && s->pid == pid) ||
            (!is_id && strcmp(s->family, spec) == 0)) {
            add_family(out, n, s->family);
            matched = 1;
        }
    }
    return matched ? 0 : 1;
}

static int resolve_list(const char *list, char out[][32], int *n) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", list);
    int bad = 0;
    for (char *tok = strtok(buf, ", "); tok; tok = strtok(NULL, ", ")) {
        if (resolve_spec(tok, out, n) != 0) {
            fprintf(stderr, "unknown sensor or family: %s\n", tok);
            bad = 1;
        }
    }
    return bad;
}

static int cmd_resolve(const char *list) {
    char fams[MAX_FAMILIES][32];
    int n = 0;
    int bad = resolve_list(list, fams, &n);
    for (int i = 0; i < n; i++) printf("%s\n", fams[i]);
    return bad ? 1 : 0;
}

static int cmd_deps(const char *phase, const char *list) {
    char fams[MAX_FAMILIES][32];
    int n = 0;
    if (resolve_list(list, fams, &n) != 0) return 1;

    unsigned mask = 0;
    for (int f = 0; f < n; f++) {
        for (size_t i = 0; i < HACK_TOUCHID_SENSOR_COUNT; i++) {
            const hack_touchid_sensor_t *s = &HACK_TOUCHID_SENSORS[i];
            if (strcmp(s->family, fams[f]) != 0) continue;
            if (strcmp(phase, "build") == 0 || strcmp(phase, "all") == 0)   mask |= s->deps_build;
            if (strcmp(phase, "runtime") == 0 || strcmp(phase, "all") == 0) mask |= s->deps_runtime;
        }
    }
    for (unsigned bit = 1; bit & HT_DEP_ALL; bit <<= 1)
        if (mask & bit) printf("%s\n", ht_dep_formula(bit));
    return 0;
}

/* ioreg prints each USB device's "idVendor" and "idProduct" as decimal
 * integers on separate lines. Track the most recent of each and test the
 * pair whenever a product id arrives. */
static int cmd_detect(void) {
    char line[1024];
    long vid = -1, pid = -1;
    char fams[MAX_FAMILIES][32];
    int n = 0;

    while (fgets(line, sizeof(line), stdin)) {
        char *p;
        if ((p = strstr(line, "\"idVendor\" = ")) != NULL)  vid = strtol(p + 13, NULL, 10);
        if ((p = strstr(line, "\"idProduct\" = ")) != NULL) {
            pid = strtol(p + 14, NULL, 10);
            if (vid >= 0 && pid >= 0) {
                for (size_t i = 0; i < HACK_TOUCHID_SENSOR_COUNT; i++) {
                    const hack_touchid_sensor_t *s = &HACK_TOUCHID_SENSORS[i];
                    if ((long)s->vid == vid && (long)s->pid == pid) add_family(fams, &n, s->family);
                }
            }
            vid = pid = -1;
        }
    }
    for (int i = 0; i < n; i++) printf("%s\n", fams[i]);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "families") == 0) return cmd_families();
    if (argc >= 3 && strcmp(argv[1], "resolve") == 0)  return cmd_resolve(argv[2]);
    if (argc >= 4 && strcmp(argv[1], "deps") == 0)     return cmd_deps(argv[2], argv[3]);
    if (argc >= 2 && strcmp(argv[1], "detect") == 0)   return cmd_detect();
    fprintf(stderr, "usage: %s families | resolve <spec,...> | deps <build|runtime|all> <family,...> | detect\n",
            argv[0]);
    return 2;
}
