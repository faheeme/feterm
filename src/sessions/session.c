#include "sessions/session.h"

#include <string.h>

#include "config/paths.h"
#include "util/str.h"
#include "util/utf8.h"

static const char *const type_names[TE_SESS_TYPE_COUNT] = { "ssh", "telnet", "serial", "local" };

static const struct { const char *name; uint32_t rgb; } tag_colors[TE_TAG_COUNT] = {
    [TE_TAG_NONE] = { "none", 0 },          [TE_TAG_RED] = { "red", 0xF38BA8 },
    [TE_TAG_ORANGE] = { "orange", 0xFAB387 }, [TE_TAG_YELLOW] = { "yellow", 0xF9E2AF },
    [TE_TAG_GREEN] = { "green", 0xA6E3A1 },  [TE_TAG_CYAN] = { "cyan", 0x94E2D5 },
    [TE_TAG_BLUE] = { "blue", 0x89B4FA },    [TE_TAG_PURPLE] = { "purple", 0xCBA6F7 },
    [TE_TAG_PINK] = { "pink", 0xF5C2E7 },    [TE_TAG_GREY] = { "grey", 0x9399B2 },
    [TE_TAG_INHERIT] = { "inherit", 0 },
};

static const uint32_t std_bauds[] = {
    300, 600, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800,
    500000, 576000, 921600, 1000000, 1500000, 2000000, 3000000, 4000000,
};

void session_defaults(te_session *s, te_sess_type type)
{
    memset(s, 0, sizeof *s);
    s->type = type;
    s->share_conn = true;
    s->color = TE_TAG_INHERIT;
    s->anti_idle = TE_ANTI_IDLE_INHERIT;
    s->serial.baud = 9600;
    s->serial.data_bits = 8;
    s->serial.parity = TE_PARITY_NONE;
    s->serial.stop_bits = 1;
    s->serial.flow = TE_FLOW_NONE;
}

void folder_defaults_init(te_folder_defaults *d)
{
    memset(d, 0, sizeof *d);
    d->color = TE_TAG_INHERIT;
    d->anti_idle = TE_ANTI_IDLE_INHERIT;
}

const char *session_type_name(te_sess_type t)
{
    return (unsigned)t < TE_SESS_TYPE_COUNT ? type_names[t] : "ssh";
}

bool session_type_parse(const char *s, te_sess_type *out)
{
    for (size_t i = 0; i < TE_SESS_TYPE_COUNT; i++) {
        if (str_eq_ci(s, type_names[i])) {
            *out = (te_sess_type)i;
            return true;
        }
    }
    return false;
}

const char *tag_color_name(te_tag_color c)
{
    return (unsigned)c < TE_TAG_COUNT ? tag_colors[c].name : "none";
}

bool tag_color_parse(const char *s, te_tag_color *out)
{
    for (size_t i = 0; i < TE_TAG_COUNT; i++) {
        if (str_eq_ci(s, tag_colors[i].name)) {
            *out = (te_tag_color)i;
            return true;
        }
    }
    if (str_eq_ci(s, "gray")) {
        *out = TE_TAG_GREY;
        return true;
    }
    return false;
}

uint32_t tag_color_rgb(te_tag_color c)
{
    return (unsigned)c < TE_TAG_COUNT ? tag_colors[c].rgb : 0;
}

uint16_t session_default_port(te_sess_type t)
{
    return t == TE_SESS_SSH ? 22 : (t == TE_SESS_TELNET ? 23 : 0);
}

size_t serial_standard_bauds(const uint32_t **out)
{
    *out = std_bauds;
    return TE_ARRAY_LEN(std_bauds);
}

/* ------------------------------------------------------------------------- */
/* Validation */

/* Valid UTF-8 with no C0/C1 control characters or DEL. */
static bool printable_utf8(const char *s)
{
    te_utf8_dec d = { 0 };
    te_utf8_res last = TE_UTF8_DONE;
    for (; *s != '\0'; s++) {
        uint32_t cp = 0;
        last = utf8_feed(&d, (uint8_t)*s, &cp);
        if (last == TE_UTF8_ERROR) {
            return false;
        }
        if (last == TE_UTF8_DONE && (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F))) {
            return false;
        }
    }
    return last == TE_UTF8_DONE;   /* no truncated sequence at the end */
}

bool session_name_valid(const char *name)
{
    if (name == nullptr) {
        return false;
    }
    size_t n = strnlen(name, TE_SESS_NAME_MAX + 1);
    if (n == 0 || n > TE_SESS_NAME_MAX || !printable_utf8(name)) {
        return false;
    }
    if (name[0] == '.' || name[0] == '-' || name[0] == ' ' || name[n - 1] == ' ') {
        return false;
    }
    if (strchr(name, '/') != nullptr || strchr(name, '\\') != nullptr) {
        return false;
    }
    return strcmp(name, "_folder") != 0;   /* reserved for folder settings */
}

bool session_folder_valid(const char *folder)
{
    if (folder == nullptr) {
        return false;
    }
    size_t n = strnlen(folder, TE_SESS_FOLDER_MAX + 1);
    if (n == 0) {
        return true;   /* top level */
    }
    if (n > TE_SESS_FOLDER_MAX) {
        return false;
    }
    char part[TE_SESS_NAME_MAX + 2] = { 0 };
    size_t depth = 0;
    const char *s = folder;
    for (;;) {
        const char *slash = strchr(s, '/');
        size_t len = slash != nullptr ? (size_t)(slash - s) : strlen(s);
        if (len == 0 || len > TE_SESS_NAME_MAX || ++depth > TE_SESS_DEPTH_MAX) {
            return false;
        }
        memcpy(part, s, len);
        part[len] = '\0';
        if (!session_name_valid(part)) {
            return false;
        }
        if (slash == nullptr) {
            return true;
        }
        s = slash + 1;
    }
}

bool session_host_valid(const char *host)
{
    if (host == nullptr) {
        return false;
    }
    size_t n = strnlen(host, TE_SESS_HOST_MAX + 1);
    if (n == 0 || n > TE_SESS_HOST_MAX || host[0] == '-') {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)host[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                  c == '-' || c == '_' || c == ':' || c == '[' || c == ']' || c == '%';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool session_user_valid(const char *user)
{
    if (user == nullptr) {
        return false;
    }
    size_t n = strnlen(user, TE_SESS_USER_MAX + 1);
    if (n > TE_SESS_USER_MAX || (n > 0 && user[0] == '-')) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)user[i];
        if (c <= 0x20 || c == 0x7F || c >= 0x80 || c == '@' || c == ':') {
            return false;
        }
    }
    return true;
}

bool session_value_safe(const char *v)
{
    return v != nullptr && v[0] != '-' && printable_utf8(v);
}

bool session_device_valid(const char *dev)
{
#ifdef _WIN32
    /* COM1 to COM256 (any case); \\.\ is added when the port is opened. */
    if (dev == nullptr || _strnicmp(dev, "COM", 3) != 0 || dev[3] < '1' || dev[3] > '9') {
        return false;
    }
    long n = 0;
    return str_to_long(dev + 3, 1, 256, &n);
#else
    if (dev == nullptr || strncmp(dev, "/dev/", 5) != 0 || dev[5] == '\0') {
        return false;
    }
    size_t n = strnlen(dev, TE_SESS_PATH_MAX);
    if (n >= TE_SESS_PATH_MAX || strstr(dev, "/..") != nullptr || strstr(dev, "//") != nullptr) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)dev[i];
        if (c <= 0x20 || c >= 0x7F) {
            return false;
        }
    }
    return true;
#endif
}

static bool jump_valid(const char *j)
{
    if (j[0] == '\0') {
        return true;
    }
    if (j[0] == '-' || strnlen(j, TE_SESS_HOST_MAX + 1) > TE_SESS_HOST_MAX) {
        return false;
    }
    for (const char *s = j; *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                  c == '-' || c == '_' || c == ':' || c == '@' || c == ',' || c == '[' || c == ']' || c == '%';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool session_validate(const te_session *s, const char **why)
{
    const char *dummy = nullptr;
    const char **w = why != nullptr ? why : &dummy;
    if (!session_name_valid(s->name)) {
        *w = "Name: 1-64 characters, no '/', '\\' or control characters, not starting with '.' or '-'";
        return false;
    }
    if (!session_folder_valid(s->folder)) {
        *w = "Folder path is not valid";
        return false;
    }
    if ((unsigned)s->type >= TE_SESS_TYPE_COUNT) {
        *w = "Unknown session type";
        return false;
    }
    if ((unsigned)s->color >= TE_TAG_COUNT) {
        *w = "Unknown colour";
        return false;
    }
    if (s->anti_idle != TE_ANTI_IDLE_INHERIT && (s->anti_idle < 0 || s->anti_idle > TE_ANTI_IDLE_MAX)) {
        *w = "Anti-idle must be 0 (off) to 86400 seconds";
        return false;
    }
    if (s->highlight[0] != '\0' && strcmp(s->highlight, "auto") != 0 && !paths_is_safe_name(s->highlight)) {
        *w = "Highlight profile name is not valid";
        return false;
    }
    switch (s->type) {
    case TE_SESS_SSH:
    case TE_SESS_TELNET:
        if (!session_host_valid(s->host)) {
            *w = "Host: a name or address (letters, digits, '.', '-', ':')";
            return false;
        }
        if (!session_user_valid(s->user)) {
            *w = "User name may not contain spaces, '@', ':' or start with '-'";
            return false;
        }
        if (s->type == TE_SESS_SSH) {
            if (s->identity[0] != '\0' && !session_value_safe(s->identity)) {
                *w = "Key file path is not valid";
                return false;
            }
            if (!jump_valid(s->jump)) {
                *w = "Jump host: [user@]host[:port], comma-separated";
                return false;
            }
        }
        return true;
    case TE_SESS_SERIAL:
        if (!session_device_valid(s->serial.device)) {
#ifdef _WIN32
            *w = "Device must be a COM port, e.g. " TE_SERIAL_EXAMPLE;
#else
            *w = "Device must be a path under /dev/, e.g. " TE_SERIAL_EXAMPLE;
#endif
            return false;
        }
        if (s->serial.baud < 50 || s->serial.baud > 16000000) {
            *w = "Baud rate must be 50 to 16000000";
            return false;
        }
        if (s->serial.data_bits < 5 || s->serial.data_bits > 8 || s->serial.stop_bits < 1 ||
            s->serial.stop_bits > 2 || (unsigned)s->serial.parity > TE_PARITY_ODD ||
            (unsigned)s->serial.flow > TE_FLOW_XONXOFF) {
            *w = "Serial line settings are not valid";
            return false;
        }
        return true;
    case TE_SESS_LOCAL:
        if (s->command[0] != '\0' && !session_value_safe(s->command)) {
            *w = "Command may not start with '-' or contain control characters";
            return false;
        }
        if (s->directory[0] != '\0' && !session_value_safe(s->directory)) {
            *w = "Directory is not valid";
            return false;
        }
        return true;
    case TE_SESS_TYPE_COUNT:
    default:
        break;
    }
    *w = "Unknown session type";
    return false;
}

/* ------------------------------------------------------------------------- */
/* Serial line format */

static const char parity_letters[] = { 'N', 'E', 'O' };
static const char *const flow_names[] = { "none", "rtscts", "xonxoff" };

te_err serial_format(const te_serial_cfg *c, char *out, size_t outsz)
{
    if ((unsigned)c->parity > TE_PARITY_ODD || (unsigned)c->flow > TE_FLOW_XONXOFF) {
        return TE_ERR_INVAL;
    }
    return str_fmt(out, outsz, "%u,%u%c%u,%s", (unsigned)c->baud, (unsigned)c->data_bits,
                   parity_letters[c->parity], (unsigned)c->stop_bits, flow_names[c->flow]);
}

bool serial_parse(const char *spec, te_serial_cfg *c)
{
    char buf[64] = { 0 };
    if (spec == nullptr || str_copy(buf, sizeof buf, spec) != TE_OK) {
        return false;
    }
    te_serial_cfg n = *c;
    char *save = nullptr;
    size_t field = 0;
    for (char *tok = strtok_r(buf, ",", &save); tok != nullptr; tok = strtok_r(nullptr, ",", &save), field++) {
        tok = str_trim(tok);
        if (field == 0) {
            long v = 0;
            if (!str_to_long(tok, 50, 16000000, &v)) {
                return false;
            }
            n.baud = (uint32_t)v;
        } else if (field == 1) {
            if (strlen(tok) != 3 || tok[0] < '5' || tok[0] > '8' || (tok[2] != '1' && tok[2] != '2')) {
                return false;
            }
            n.data_bits = (uint8_t)(tok[0] - '0');
            n.stop_bits = (uint8_t)(tok[2] - '0');
            char p = tok[1];
            if (p == 'N' || p == 'n') n.parity = TE_PARITY_NONE;
            else if (p == 'E' || p == 'e') n.parity = TE_PARITY_EVEN;
            else if (p == 'O' || p == 'o') n.parity = TE_PARITY_ODD;
            else return false;
        } else if (field == 2) {
            if (str_eq_ci(tok, "none")) n.flow = TE_FLOW_NONE;
            else if (str_eq_ci(tok, "rtscts") || str_eq_ci(tok, "hardware")) n.flow = TE_FLOW_RTSCTS;
            else if (str_eq_ci(tok, "xonxoff") || str_eq_ci(tok, "software")) n.flow = TE_FLOW_XONXOFF;
            else return false;
        } else {
            return false;
        }
    }
    if (field == 0) {
        return false;
    }
    *c = n;
    return true;
}

/* ------------------------------------------------------------------------- */
/* INI reading and writing */

static te_err set_text(char *dst, size_t dstsz, const char *value)
{
    if (!printable_utf8(value)) {
        return TE_ERR_PARSE;
    }
    return str_copy(dst, dstsz, value) == TE_OK ? TE_OK : TE_ERR_PARSE;
}

static te_err parse_anti_idle(const char *value, int32_t *out)
{
    if (str_eq_ci(value, "inherit")) {
        *out = TE_ANTI_IDLE_INHERIT;
        return TE_OK;
    }
    long v = 0;
    if (!str_to_long(value, 0, TE_ANTI_IDLE_MAX, &v)) {
        return TE_ERR_PARSE;
    }
    *out = (int32_t)v;
    return TE_OK;
}

te_err session_parse_kv(te_session *s, const char *section, const char *key, const char *value)
{
    long v = 0;
    bool b = false;
    if (strcmp(section, "session") == 0) {
        if (strcmp(key, "type") == 0) return session_type_parse(value, &s->type) ? TE_OK : TE_ERR_PARSE;
        if (strcmp(key, "host") == 0) return set_text(s->host, sizeof s->host, value);
        if (strcmp(key, "port") == 0) {
            if (!str_to_long(value, 0, 65535, &v)) return TE_ERR_PARSE;
            s->port = (uint16_t)v;
            return TE_OK;
        }
        if (strcmp(key, "user") == 0) return set_text(s->user, sizeof s->user, value);
        if (strcmp(key, "identity_file") == 0) return set_text(s->identity, sizeof s->identity, value);
        if (strcmp(key, "jump_host") == 0) return set_text(s->jump, sizeof s->jump, value);
        if (strcmp(key, "share_connection") == 0) {
            if (!str_to_bool(value, &b)) return TE_ERR_PARSE;
            s->share_conn = b;
            return TE_OK;
        }
        if (strcmp(key, "color") == 0) return tag_color_parse(value, &s->color) ? TE_OK : TE_ERR_PARSE;
        if (strcmp(key, "anti_idle") == 0) return parse_anti_idle(value, &s->anti_idle);
        if (strcmp(key, "highlight") == 0) return set_text(s->highlight, sizeof s->highlight, value);
    } else if (strcmp(section, "serial") == 0) {
        if (strcmp(key, "device") == 0) return set_text(s->serial.device, sizeof s->serial.device, value);
        if (strcmp(key, "line") == 0) return serial_parse(value, &s->serial) ? TE_OK : TE_ERR_PARSE;
    } else if (strcmp(section, "local") == 0) {
        if (strcmp(key, "command") == 0) return set_text(s->command, sizeof s->command, value);
        if (strcmp(key, "directory") == 0) return set_text(s->directory, sizeof s->directory, value);
    }
    return TE_OK;   /* unknown keys are ignored, so newer files still load */
}

te_err folder_parse_kv(te_folder_defaults *d, const char *section, const char *key, const char *value)
{
    if (strcmp(section, "folder") != 0) {
        return TE_OK;
    }
    if (strcmp(key, "color") == 0) return tag_color_parse(value, &d->color) ? TE_OK : TE_ERR_PARSE;
    if (strcmp(key, "anti_idle") == 0) return parse_anti_idle(value, &d->anti_idle);
    if (strcmp(key, "user") == 0) {
        return session_user_valid(value) && str_copy(d->user, sizeof d->user, value) == TE_OK ? TE_OK : TE_ERR_PARSE;
    }
    return TE_OK;
}

static te_err put(char *out, size_t outsz, const char *key, const char *value)
{
    if (value == nullptr || value[0] == '\0') {
        return TE_OK;
    }
    char line[TE_SESS_PATH_MAX + 64] = { 0 };
    te_err rc = str_fmt(line, sizeof line, "%s = %s\n", key, value);
    return rc != TE_OK ? rc : str_append(out, outsz, line);
}

static void anti_idle_text(int32_t v, char *buf, size_t bufsz)
{
    if (v == TE_ANTI_IDLE_INHERIT) {
        (void)str_copy(buf, bufsz, "inherit");
    } else {
        (void)str_fmt(buf, bufsz, "%d", (int)v);
    }
}

te_err session_serialize(const te_session *s, char *out, size_t outsz)
{
    if (outsz == 0) {
        return TE_ERR_INVAL;
    }
    out[0] = '\0';
    char num[32] = { 0 };
    te_err rc = str_append(out, outsz, "[session]\n");
    if (rc == TE_OK) rc = put(out, outsz, "type", session_type_name(s->type));
    if (rc == TE_OK && (s->type == TE_SESS_SSH || s->type == TE_SESS_TELNET)) {
        rc = put(out, outsz, "host", s->host);
        if (rc == TE_OK && s->port != 0) {
            (void)str_fmt(num, sizeof num, "%u", (unsigned)s->port);
            rc = put(out, outsz, "port", num);
        }
        if (rc == TE_OK) rc = put(out, outsz, "user", s->user);
        if (rc == TE_OK && s->type == TE_SESS_SSH) {
            rc = put(out, outsz, "identity_file", s->identity);
            if (rc == TE_OK) rc = put(out, outsz, "jump_host", s->jump);
            if (rc == TE_OK) rc = put(out, outsz, "share_connection", s->share_conn ? "true" : "false");
        }
    }
    if (rc == TE_OK) rc = put(out, outsz, "color", tag_color_name(s->color));
    if (rc == TE_OK) {
        anti_idle_text(s->anti_idle, num, sizeof num);
        rc = put(out, outsz, "anti_idle", num);
    }
    if (rc == TE_OK) rc = put(out, outsz, "highlight", s->highlight);
    if (rc == TE_OK && s->type == TE_SESS_SERIAL) {
        char line[64] = { 0 };
        rc = str_append(out, outsz, "\n[serial]\n");
        if (rc == TE_OK) rc = put(out, outsz, "device", s->serial.device);
        if (rc == TE_OK) rc = serial_format(&s->serial, line, sizeof line);
        if (rc == TE_OK) rc = put(out, outsz, "line", line);
    }
    if (rc == TE_OK && s->type == TE_SESS_LOCAL) {
        rc = str_append(out, outsz, "\n[local]\n");
        if (rc == TE_OK) rc = put(out, outsz, "command", s->command);
        if (rc == TE_OK) rc = put(out, outsz, "directory", s->directory);
    }
    return rc;
}

te_err folder_serialize(const te_folder_defaults *d, char *out, size_t outsz)
{
    if (outsz == 0) {
        return TE_ERR_INVAL;
    }
    out[0] = '\0';
    char num[32] = { 0 };
    te_err rc = str_append(out, outsz, "[folder]\n");
    if (rc == TE_OK) rc = put(out, outsz, "color", tag_color_name(d->color));
    if (rc == TE_OK) {
        anti_idle_text(d->anti_idle, num, sizeof num);
        rc = put(out, outsz, "anti_idle", num);
    }
    if (rc == TE_OK) rc = put(out, outsz, "user", d->user);
    return rc;
}
