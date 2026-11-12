/* Controller profiles: which pads are known by vendor and product id, how
 * their input reports read, and how to talk back to them.
 *
 * Adding a controller means adding one profile: an id table, a parser and,
 * if it has rumble, lights or needs waking up, the output builders.
 * Controllers with no profile go to the generic one (generic.c), which
 * reads the pad's own HID descriptor.
 */
#ifndef PH_PROFILES_H
#define PH_PROFILES_H

#include "../../core/pad.h"

#include <stddef.h>

/* Per-pad working memory a profile may use (packet counters, the generic
 * profile's report layout). */
#define PROFILE_CTX_MAX 2048

/* parse() results. */
#define PARSE_NONE 0 /* not a report this profile reads */
#define PARSE_OK 1
#define PARSE_FULL 2 /* the full report the init sequence asks for */

typedef struct pad_profile {
    const char *name;
    const uint16_t (*ids)[2]; /* {vid, pid} pairs */
    int nids;

    /* Reads one input report (report id first, HID header stripped). */
    int (*parse)(void *ctx, const uint8_t *rep, int len, pad_state *st);

    /* Writes the output report for *out (report id first, HID header not
     * included) and returns its length, or 0 if there is none. */
    int (*build_output)(void *ctx, const pad_output *out, uint8_t *buf, int max);

    /* Writes step `step` of the pad's wake-up sequence, or returns 0 once
     * there are no more steps. NULL: the pad needs none. */
    int (*init_report)(void *ctx, int step, const pad_output *out, uint8_t *buf, int max);

    /* Repeat the wake-up sequence until parse() returns PARSE_FULL. */
    int needs_full;

    /* Non-zero: schedule rumble at this minimum interval (ms), refresh it
     * while active, and repeat stop reports a bounded number of times.
     * Output waits for initialization to finish. 0: send immediately. */
    int rumble_refresh_ms;

    /* Prepares ctx for the exact model, when the profile covers several
     * that differ. NULL: nothing to prepare. */
    void (*setup)(void *ctx, uint16_t vid, uint16_t pid);
} pad_profile;

typedef enum {
    PROFILE_GENERIC_HID,
    PROFILE_SWITCH,
    PROFILE_XBOX,
    PROFILE_KNOWN_OTHER
} profile_family;
/* VID/PID evidence only; names and address prefixes never determine a protocol. */
profile_family profile_classify(uint16_t vid, uint16_t pid);
/* USB FF/5D/01 only: known wired identities permitted LED activation before
 * their first input report. This does not classify Bluetooth or HID devices. */
int profile_usb_xinput_activation(uint16_t vid, uint16_t pid);
/* Receiver-backed modes require real reports; USB presence cannot renew held input. */
int profile_usb_input_watchdog(uint16_t vid, uint16_t pid);
const pad_profile *profile_find(uint16_t vid, uint16_t pid);
int profile_ble_allowed(uint16_t vid, uint16_t pid);
/* Builtin resolver is separate so override validation cannot recursively query itself. */
const pad_profile *profile_builtin_named(const char *key);
const pad_profile *profile_find_for_transport(pad_transport transport, uint16_t vid, uint16_t pid);
profile_family profile_classify_for_transport(pad_transport transport, uint16_t vid, uint16_t pid);
const char *pad_display_name_for_transport(pad_transport transport, uint16_t vid, uint16_t pid,
                                           const char *profile);
int profile_usb_protocol_hint(uint16_t vid, uint16_t pid);

/* The pad's model as people know it ("Xbox Series X|S"), or NULL. */
const char *profile_model_name(uint16_t vid, uint16_t pid);
/* Optional named HID layout from the same device registry; NULL uses defaults. */
const char *profile_hid_mapping(uint16_t vid, uint16_t pid);

/* What to call a pad on screen: its model, else its profile's name (not
 * the generic one), else "Controller". Never NULL. */
const char *pad_display_name(uint16_t vid, uint16_t pid, const char *profile);

/* Major class 5 (peripheral) with a gamepad or joystick minor class. */
int profile_class_is_gamepad(uint32_t class_of_device);

/* ---- generic HID (generic.c) ---- */

extern const pad_profile generic_profile;

/* Prepares descriptor-based HID decoding with registry bindings.
 * Returns 0 if the descriptor shows no usable gamepad controls. */
int generic_setup_for_transport(void *ctx, const uint8_t *desc, int len, uint16_t vid, uint16_t pid,
                                pad_transport transport);
int generic_setup(void *ctx, const uint8_t *desc, int len, uint16_t vid, uint16_t pid);

#endif
