/*
 * A saved session (SSH, Telnet, serial console or local shell) and the
 * per-folder defaults sessions inherit. Every text field is validated before
 * it is stored or used: nothing here ever reaches a shell.
 */
#ifndef TE_SESSION_H
#define TE_SESSION_H

#include <stdbool.h>
#include "common.h"

#define TE_SESS_NAME_MAX   64      /* bytes, without the terminator */
#define TE_SESS_HOST_MAX   255
#define TE_SESS_USER_MAX   64
#define TE_SESS_PATH_MAX   1024
#define TE_SESS_FOLDER_MAX 512     /* relative folder path, '/'-separated */
#define TE_SESS_DEPTH_MAX  8       /* folder nesting */
#define TE_ANTI_IDLE_INHERIT (-1)
#define TE_ANTI_IDLE_MAX   86400

typedef enum { TE_SESS_SSH, TE_SESS_TELNET, TE_SESS_SERIAL, TE_SESS_LOCAL, TE_SESS_TYPE_COUNT } te_sess_type;
typedef enum { TE_PARITY_NONE, TE_PARITY_EVEN, TE_PARITY_ODD } te_parity;
typedef enum { TE_FLOW_NONE, TE_FLOW_RTSCTS, TE_FLOW_XONXOFF } te_flow;

/* Tag colours, independent of the theme so "red means production" everywhere. */
typedef enum {
    TE_TAG_NONE, TE_TAG_RED, TE_TAG_ORANGE, TE_TAG_YELLOW, TE_TAG_GREEN, TE_TAG_CYAN, TE_TAG_BLUE,
    TE_TAG_PURPLE, TE_TAG_PINK, TE_TAG_GREY, TE_TAG_INHERIT, TE_TAG_COUNT
} te_tag_color;

typedef struct te_serial_cfg {
    char      device[TE_SESS_PATH_MAX];
    uint32_t  baud;            /* any rate 50..16000000; non-standard ones are set with termios2 */
    uint8_t   data_bits;       /* 5..8 */
    te_parity parity;
    uint8_t   stop_bits;       /* 1 or 2 */
    te_flow   flow;
} te_serial_cfg;

typedef struct te_session {
    char          name[TE_SESS_NAME_MAX + 1];
    char          folder[TE_SESS_FOLDER_MAX + 1];   /* "" = top level */
    te_sess_type  type;
    /* SSH / Telnet */
    char          host[TE_SESS_HOST_MAX + 1];
    uint16_t      port;            /* 0 = protocol default */
    char          user[TE_SESS_USER_MAX + 1];
    char          identity[TE_SESS_PATH_MAX];       /* SSH private key, may start with ~/ */
    char          jump[TE_SESS_HOST_MAX + 1];       /* SSH ProxyJump: [user@]host[:port][,...] */
    bool          share_conn;      /* SSH ControlMaster multiplexing */
    /* Serial */
    te_serial_cfg serial;
    /* Local */
    char          command[TE_SESS_PATH_MAX];        /* empty: login shell */
    char          directory[TE_SESS_PATH_MAX];
    /* Common */
    te_tag_color  color;
    int32_t       anti_idle;       /* seconds; 0 off; TE_ANTI_IDLE_INHERIT */
    char          highlight[64];   /* "" or "auto": detect; else a profile name */
} te_session;

/* Defaults a folder passes to everything inside it (_folder.ini). */
typedef struct te_folder_defaults {
    te_tag_color color;            /* TE_TAG_INHERIT: from the parent */
    int32_t      anti_idle;        /* TE_ANTI_IDLE_INHERIT: from the parent */
    char         user[TE_SESS_USER_MAX + 1];   /* "": from the parent */
} te_folder_defaults;

void session_defaults(te_session *s, te_sess_type type);
void folder_defaults_init(te_folder_defaults *d);

const char *session_type_name(te_sess_type t);        /* "ssh", "telnet", "serial", "local" */
[[nodiscard]] bool session_type_parse(const char *s, te_sess_type *out);
const char *tag_color_name(te_tag_color c);           /* "none", "red", ... */
[[nodiscard]] bool tag_color_parse(const char *s, te_tag_color *out);
uint32_t tag_color_rgb(te_tag_color c);              /* 0 for none/inherit */
uint16_t session_default_port(te_sess_type t);

/*
 * Validation. A name: 1..64 bytes of valid UTF-8 with no control characters,
 * '/', '\\', no leading '.', '-' or space, no trailing space. A folder path:
 * up to TE_SESS_DEPTH_MAX valid names joined by '/'.
 */
[[nodiscard]] bool session_name_valid(const char *name);
[[nodiscard]] bool session_folder_valid(const char *folder);
/* Host name or address: letters, digits, '.', '-', '_', ':' (IPv6), '[' ']', '%' (scope); no leading '-'. */
[[nodiscard]] bool session_host_valid(const char *host);
[[nodiscard]] bool session_user_valid(const char *user);
/* A value safe to store and pass as one argv element: printable, no leading '-'. */
[[nodiscard]] bool session_value_safe(const char *v);

/* What a serial device name looks like here, for hints and examples. */
#ifdef _WIN32
#define TE_SERIAL_EXAMPLE "COM3"
#else
#define TE_SERIAL_EXAMPLE "/dev/ttyUSB0"
#endif

/* Serial device: an absolute path under /dev/ without ".." (POSIX); COM1 to COM256 (Windows). */
[[nodiscard]] bool session_device_valid(const char *dev);

/* Checks every field for the session's type; on failure *why says which. */
[[nodiscard]] bool session_validate(const te_session *s, const char **why);

/* Parse one "key = value" from a session file into s (section is the INI section). */
[[nodiscard]] te_err session_parse_kv(te_session *s, const char *section, const char *key, const char *value);
[[nodiscard]] te_err folder_parse_kv(te_folder_defaults *d, const char *section, const char *key, const char *value);
/* Serialize to INI text in out (capacity outsz). TE_ERR_TRUNC if it does not fit. */
[[nodiscard]] te_err session_serialize(const te_session *s, char *out, size_t outsz);
[[nodiscard]] te_err folder_serialize(const te_folder_defaults *d, char *out, size_t outsz);

/* "115200,8N1,none" style summary of a serial configuration. */
[[nodiscard]] te_err serial_format(const te_serial_cfg *c, char *out, size_t outsz);
/* Parse the same format; unspecified parts keep their current values. */
[[nodiscard]] bool serial_parse(const char *spec, te_serial_cfg *c);
/* The standard rates offered in the dialog, ascending; returns the count. */
size_t serial_standard_bauds(const uint32_t **out);

#endif
