/*
 * supported_sensors.h
 *
 * Single registry of every fingerprint sensor this project knows
 * about. hack_touchid_client.c probes the USB bus against this table
 * at startup, on [1] Enroll, on [2] Verify, and on [3] Deploy -- one
 * source of truth so a new sensor is added here once and the client
 * automatically knows its VID:PID, which daemon binary owns it, and
 * whether its capture backend actually exists yet.
 *
 * Adding a new sensor:
 *   1. Add a row below with its VID:PID and a display name.
 *   2. Build its capture backend as its own <name>_daemon.c, following
 *      vfs5011_daemon.c's structure (NBIS matching / template storage /
 *      LaunchAgent IPC are all reusable as-is -- only the USB init
 *      handshake and image capture are sensor-specific).
 *   3. Flip backend_available to true once that daemon is built,
 *      tested on real hardware, and its own vfs5011_agent_install.sh-
 *      style installer exists.
 * Until backend_available is true, the client will detect the sensor
 * and say so, but Enroll/Verify/Deploy will refuse with a clear
 * "not yet implemented" message rather than pretending to work.
 */

#ifndef __SUPPORTED_SENSORS_H
#define __SUPPORTED_SENSORS_H

typedef struct {
    const char *display_name;      /* shown in the client, e.g. "VFS5011" */
    unsigned short vid;
    unsigned short pid;
    const char *daemon_binary_name; /* installed under /usr/local/libexec/hack-touchid/ */
    const char *install_script_name; /* e.g. "vfs5011_agent_install.sh", run alongside the client */
    int backend_available;          /* 1 = Enroll/Verify/Deploy actually work, 0 = detected only */
} hack_touchid_sensor_t;

static const hack_touchid_sensor_t HACK_TOUCHID_SENSORS[] = {
    {
        .display_name        = "Validity VFS5011",
        .vid                  = 0x138a,
        .pid                  = 0x0018,
        .daemon_binary_name   = "vfs5011_daemon",
        .install_script_name  = "vfs5011_agent_install.sh",
        .backend_available    = 1,
    },
    {
        /* r/hackintosh-confirmed on a ThinkPad T420 (Aug 16 2026); the
         * committed first multi-sensor target. Capture backend not
         * built yet -- needs its own from-scratch USB init sequence,
         * ported similarly to how VFS5011's was pulled from libfprint.
         * install_script_name is a placeholder name for the installer
         * that will exist once upek_daemon.c does. */
        .display_name        = "UPEK/AuthenTec TouchStrip",
        .vid                  = 0x147e,
        .pid                  = 0x2016,
        .daemon_binary_name   = "upek_daemon",
        .install_script_name  = "upek_agent_install.sh",
        .backend_available    = 0,
    },
};

#define HACK_TOUCHID_SENSOR_COUNT \
    (sizeof(HACK_TOUCHID_SENSORS) / sizeof(HACK_TOUCHID_SENSORS[0]))

#endif /* __SUPPORTED_SENSORS_H */
