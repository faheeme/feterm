/*
 * VT escape sequence parser (after Paul Williams' DEC ANSI state machine).
 * Every buffer has a hard upper bound: parameters, intermediates, and
 * OSC/DCS string payloads. Sequences that go over a bound are discarded,
 * not truncated, so a partial sequence is never acted on.
 */
#ifndef TE_VT_PARSER_H
#define TE_VT_PARSER_H

#include <stdbool.h>
#include "common.h"
#include "util/utf8.h"

#define VT_MAX_PARAMS        32
#define VT_MAX_PARAM_VALUE   65535u
#define VT_MAX_INTERMEDIATES 2
#define VT_MAX_STRING        (8u * 1024u * 1024u)   /* OSC / DCS / APC payload cap (sixel images) */

typedef struct vt_params {
    uint16_t v[VT_MAX_PARAMS];
    bool     sub[VT_MAX_PARAMS];  /* true: this value is a ':' sub-parameter of the previous one */
    uint8_t  n;
} vt_params;

typedef struct vt_callbacks {
    void (*print)(void *user, uint32_t cp);
    void (*execute)(void *user, uint8_t c0);
    void (*csi)(void *user, const vt_params *p, const uint8_t *inter, size_t ninter, uint8_t priv, uint8_t final);
    void (*esc)(void *user, const uint8_t *inter, size_t ninter, uint8_t final);
    void (*osc)(void *user, const char *data, size_t len);
    void (*dcs)(void *user, const vt_params *p, const uint8_t *inter, size_t ninter, uint8_t priv,
                uint8_t final, const char *data, size_t len);
    void (*apc)(void *user, const char *data, size_t len);   /* APC strings (kitty graphics) */
} vt_callbacks;

typedef enum {
    VT_GROUND, VT_ESCAPE, VT_ESCAPE_INTER, VT_CSI_ENTRY, VT_CSI_PARAM, VT_CSI_INTER, VT_CSI_IGNORE,
    VT_DCS_ENTRY, VT_DCS_PARAM, VT_DCS_INTER, VT_DCS_PASS, VT_DCS_IGNORE, VT_OSC, VT_SOS_PM_APC, VT_APC,
} vt_state;

typedef struct vt_parser {
    vt_state            state;
    vt_params           params;
    bool                params_overflow;
    uint8_t             inter[VT_MAX_INTERMEDIATES];
    size_t              ninter;
    bool                inter_overflow;
    uint8_t             priv;
    uint8_t             dcs_final;
    char               *str;        /* OSC/DCS payload (heap, grows to VT_MAX_STRING) */
    size_t              str_len;
    size_t              str_cap;
    bool                str_overflow;
    te_utf8_dec         utf8;
    const vt_callbacks *cb;
    void               *user;
} vt_parser;

void vt_parser_init(vt_parser *p, const vt_callbacks *cb, void *user);
void vt_parser_destroy(vt_parser *p);
void vt_parser_feed(vt_parser *p, const uint8_t *data, size_t len);

#endif
