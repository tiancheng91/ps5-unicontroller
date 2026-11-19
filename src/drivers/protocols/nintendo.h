#ifndef UC_NINTENDO_PROTOCOL_H
#define UC_NINTENDO_PROTOCOL_H
#include "profiles.h"

enum {
    NINTENDO_TYPE_JOYCON_LEFT = 1,
    NINTENDO_TYPE_JOYCON_RIGHT = 2,
    NINTENDO_TYPE_PRO = 3,
    NINTENDO_TYPE_LICENSED_PRO = 6,
    NINTENDO_TYPE_NES_LEFT = 9,
    NINTENDO_TYPE_NES_RIGHT = 10,
    NINTENDO_TYPE_SNES = 11,
    NINTENDO_TYPE_N64 = 12,
    NINTENDO_TYPE_GENESIS = 13
};
void switch_setup(void *ctx, uint16_t vid, uint16_t pid);
int switch_parse(void *, const uint8_t *, int, pad_state *);
int switch_output(void *, const pad_output *, uint8_t *, int);
int switch_init(void *, int, const pad_output *, uint8_t *, int);
int joycon_left_parse(void *, const uint8_t *, int, pad_state *);
int joycon_right_parse(void *, const uint8_t *, int, pad_state *);
int profile_switch_info_request(void *ctx, uint8_t *out, int capacity);
/* Mandatory Pro candidate check; other Nintendo families use their known-ID path. */
int profile_switch_info_reply(void *ctx, const uint8_t *report, int length, uint8_t *type);
#endif
