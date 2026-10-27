/* Headless tests for the core (no window needed). Run with `make test`. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>
#include <time.h>

#include "config/config.h"
#include "config/ini.h"
#include "highlight/highlight.h"
#include "app/layout.h"
#include "input/click.h"
#include "render/boxdraw.h"
#include "vt/images.h"
#include "input/keybind.h"
#include "input/mouse.h"
#include "ui/search.h"
#include "ui/selection.h"
#include "ui/url.h"
#include "config/theme.h"
#include "input/keys.h"
#include "util/mem.h"
#include "util/str.h"
#include "util/utf8.h"
#include "vt/term.h"

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

/* "<temp dir>/<stem>-XXXXXX" for mkstemp()/mkdtemp(): /tmp, or %TEMP% on Windows. */
static char *tmp_template(char *out, size_t outsz, const char *stem)
{
    char base[400] = "/tmp";
#ifdef _WIN32
    const char *t = getenv("TEMP");
    if (t != nullptr && str_copy(base, sizeof base, t) == TE_OK) {
        for (char *c = base; *c != '\0'; c++) {
            if (*c == '\\') *c = '/';
        }
    }
#endif
    if (str_fmt(out, outsz, "%s/%s-XXXXXX", base, stem) != TE_OK) {
        out[0] = '\0';
    }
    return out;
}

static void feed(te_term *t, const char *s)
{
    term_feed(t, (const uint8_t *)s, strlen(s));
}

static uint32_t cp_at(te_term *t, uint16_t x, uint16_t y)
{
    const te_line *l = term_view_line(t, y);
    if (l == nullptr || x >= l->cols) {
        return 0xFFFFFFFFu;
    }
    return l->cells[x].cp;
}

static const te_cell *cell(te_term *t, uint16_t x, uint16_t y)
{
    const te_line *l = term_view_line(t, y);
    return (l != nullptr && x < l->cols) ? &l->cells[x] : nullptr;
}

/* ------------------------------------------------------------------------- */

static void test_utf8(void)
{
    te_utf8_dec d = { 0 };
    uint32_t cp = 0;
    const uint8_t euro[] = { 0xE2, 0x82, 0xAC };
    CHECK(utf8_feed(&d, euro[0], &cp) == TE_UTF8_MORE);
    CHECK(utf8_feed(&d, euro[1], &cp) == TE_UTF8_MORE);
    CHECK(utf8_feed(&d, euro[2], &cp) == TE_UTF8_DONE && cp == 0x20AC);

    te_utf8_dec d2 = { 0 };
    (void)utf8_feed(&d2, 0xC0, &cp);
    CHECK(utf8_feed(&d2, 0xAF, &cp) == TE_UTF8_ERROR);           /* overlong '/' */

    te_utf8_dec d3 = { 0 };
    (void)utf8_feed(&d3, 0xED, &cp);
    (void)utf8_feed(&d3, 0xA0, &cp);
    CHECK(utf8_feed(&d3, 0x80, &cp) == TE_UTF8_ERROR);           /* surrogate */

    te_utf8_dec d4 = { 0 };
    (void)utf8_feed(&d4, 0xF4, &cp);
    (void)utf8_feed(&d4, 0x90, &cp);
    (void)utf8_feed(&d4, 0x80, &cp);
    CHECK(utf8_feed(&d4, 0x80, &cp) == TE_UTF8_ERROR);           /* > U+10FFFF */

    char buf[4] = { 0 };
    CHECK(utf8_encode(0x1F600, buf, sizeof buf) == 4);
    CHECK(utf8_encode(0x1F600, buf, 3) == 0);                     /* no room */
    CHECK(utf8_encode(0xD800, buf, sizeof buf) == 0);             /* surrogate */

    CHECK(te_wcwidth('a') == 1);
    CHECK(te_wcwidth(0x4E00) == 2);
    CHECK(te_wcwidth(0x0301) == 0);
    CHECK(te_wcwidth(0x1F600) == 2);
    CHECK(te_wcwidth(0x200D) == 0);
    CHECK(te_wcwidth(0xAC00) == 2);
    CHECK(te_wcwidth(0x3099) == 0);
}

static void test_str(void)
{
    char b[8] = { 0 };
    CHECK(str_copy(b, sizeof b, "abcdefghij") == TE_ERR_TRUNC && strcmp(b, "abcdefg") == 0);
    CHECK(str_copy(b, sizeof b, "abc") == TE_OK);
    CHECK(str_append(b, sizeof b, "defgh") == TE_ERR_TRUNC && strcmp(b, "abcdefg") == 0);
    CHECK(str_fmt(b, sizeof b, "%d", 123456789) == TE_ERR_TRUNC && b[7] == '\0');
    long v = 0;
    CHECK(str_to_long("42", 0, 100, &v) && v == 42);
    CHECK(!str_to_long("101", 0, 100, &v));
    CHECK(!str_to_long("12x", 0, 100, &v));
    CHECK(!str_to_long("99999999999999999999", 0, 100, &v));
}

static void test_basic_print(void)
{
    te_theme th;
    theme_defaults(&th);
    te_term *t = nullptr;
    CHECK(term_create(&t, 10, 3, 100, &th, nullptr) == TE_OK);
    if (t == nullptr) return;

    feed(t, "hello");
    CHECK(cp_at(t, 0, 0) == 'h' && cp_at(t, 4, 0) == 'o');
    uint16_t x = 0, y = 0;
    term_cursor(t, &x, &y);
    CHECK(x == 5 && y == 0);

    feed(t, "\x1b[2;3HX");
    CHECK(cp_at(t, 2, 1) == 'X');

    /* Autowrap: exactly 10 chars leave a pending wrap, the 11th wraps. */
    feed(t, "\x1b[H\x1b[2J0123456789");
    term_cursor(t, &x, &y);
    CHECK(x == 9 && y == 0);
    feed(t, "A");
    CHECK(cp_at(t, 0, 1) == 'A');
    CHECK((term_view_line(t, 0)->flags & TE_LINE_WRAPPED) != 0);

    /* Wide character at the right edge wraps whole. */
    feed(t, "\x1b[H\x1b[2J012345678\xE4\xB8\x80");
    CHECK(cp_at(t, 0, 1) == 0x4E00);
    CHECK((cell(t, 0, 1)->attrs & TE_ATTR_WIDE) != 0 && (cell(t, 1, 1)->attrs & TE_ATTR_SPACER) != 0);

    /* Overwriting half of a wide char clears the other half. */
    feed(t, "\x1b[2;2Hz");
    CHECK(cp_at(t, 0, 1) == 0 && cp_at(t, 1, 1) == 'z');

    /* Combining mark attaches to the previous cell. */
    feed(t, "\x1b[3;1He\xCC\x81");
    CHECK(cp_at(t, 0, 2) == 'e' && cell(t, 0, 2)->comb[0] == 0x0301);

    term_destroy(&t);
    CHECK(t == nullptr);
}

static void test_sgr(void)
{
    te_theme th;
    theme_defaults(&th);
    te_term *t = nullptr;
    CHECK(term_create(&t, 20, 2, 0, &th, nullptr) == TE_OK);
    if (t == nullptr) return;

    feed(t, "\x1b[1;3;38;2;10;20;30;48;5;200mA\x1b[0mB");
    const te_cell *a = cell(t, 0, 0);
    const te_cell *b = cell(t, 1, 0);
    CHECK((a->attrs & TE_ATTR_BOLD) && (a->attrs & TE_ATTR_ITALIC));
    CHECK(a->fg == TE_COLOR_RGB(0x0A141E));
    CHECK(a->bg == TE_COLOR_PAL(200));
    CHECK(b->attrs == 0 && b->fg == TE_COLOR_DEFAULT);

    feed(t, "\x1b[38:2::1:2:3;4:3mC");                         /* colon form with colour space */
    const te_cell *c = cell(t, 2, 0);
    CHECK(c->fg == TE_COLOR_RGB(0x010203));
    CHECK(((c->attrs & TE_ATTR_UL_MASK) >> TE_ATTR_UL_SHIFT) == TE_UL_CURLY);

    feed(t, "\x1b[0;38:2:4:5:6mD");                              /* colon form without colour space */
    CHECK(cell(t, 3, 0)->fg == TE_COLOR_RGB(0x040506));

    feed(t, "\x1b[38;2mE\x1b[38;5mF\x1b[48:2mG");                /* truncated: must not crash */
    CHECK(cp_at(t, 6, 0) == 'G');
    term_destroy(&t);
}

static void test_scroll_and_resize(void)
{
    te_theme th;
    theme_defaults(&th);
    te_term *t = nullptr;
    CHECK(term_create(&t, 10, 3, 5, &th, nullptr) == TE_OK);
    if (t == nullptr) return;

    for (int i = 0; i < 10; i++) {
        char line[16] = { 0 };
        (void)str_fmt(line, sizeof line, "L%d\r\n", i);
        feed(t, line);
    }
    CHECK(term_scrollback_count(t) == 5);                       /* capped ring */
    term_scroll_view(t, 100);
    CHECK(term_view_offset(t) == 5);
    CHECK(cp_at(t, 0, 0) == 'L' && cp_at(t, 1, 0) == '3');       /* oldest kept line is L3 */
    term_view_reset(t);

    CHECK(term_resize(t, 4, 2) == TE_OK);
    CHECK(term_cols(t) == 4 && term_rows(t) == 2);
    feed(t, "abcdefghijkl");
    CHECK(term_resize(t, 40, 20) == TE_OK);
    feed(t, "\x1b[20;40Hz");
    CHECK(cp_at(t, 39, 19) == 'z');
    CHECK(term_resize(t, 2, 1) == TE_OK);
    feed(t, "wide\xE4\xB8\x80wide");
    CHECK(term_resize(t, 1, 1) == TE_ERR_INVAL);                /* refused, state intact */

    feed(t, "\x1b[3J");
    CHECK(term_scrollback_count(t) == 0);

    /* Scroll region and insert/delete lines. */
    CHECK(term_resize(t, 10, 5) == TE_OK);
    feed(t, "\x1b[H\x1b[2J" "1\r\n2\r\n3\r\n4\r\n5\x1b[2;4r\x1b[2;1H\x1b[L");
    CHECK(cp_at(t, 0, 1) == 0 && cp_at(t, 0, 2) == '2' && cp_at(t, 0, 4) == '5');
    feed(t, "\x1b[M");
    CHECK(cp_at(t, 0, 1) == '2');
    term_destroy(&t);
}

static char g_title[256];
static char g_clip[256];
static void cb_title(void *u, const char *s) { (void)u; (void)str_copy(g_title, sizeof g_title, s); }
static void cb_clip(void *u, const char *s) { (void)u; (void)str_copy(g_clip, sizeof g_clip, s); }

static void test_osc_and_replies(void)
{
    te_theme th;
    theme_defaults(&th);
    te_term_host host = { .set_title = cb_title, .clipboard_set = cb_clip };
    te_term *t = nullptr;
    CHECK(term_create(&t, 20, 5, 10, &th, &host) == TE_OK);
    if (t == nullptr) return;

    feed(t, "\x1b]2;hello\x07");
    CHECK(strcmp(g_title, "hello") == 0);
    feed(t, "\x1b]0;evil\x1b[31mtitle\x1b\\");                  /* ESC ends the OSC early */
    CHECK(strcmp(g_title, "evil") == 0);
    feed(t, "\x1b]2;a\x01" "b\x07");
    CHECK(strcmp(g_title, "ab") == 0);                           /* control chars removed */

    feed(t, "\x1b]52;c;aGVsbG8gd29ybGQ=\x07");
    CHECK(strcmp(g_clip, "hello world") == 0);
    g_clip[0] = '\0';
    feed(t, "\x1b]52;c;?\x07");                                  /* read request: refused */
    size_t len = 0;
    (void)term_output(t, &len);
    CHECK(len == 0 && g_clip[0] == '\0');

    feed(t, "\x1b[c");
    const uint8_t *out = term_output(t, &len);
    CHECK(len == 9 && memcmp(out, "\x1b[?62;22c", 9) == 0);
    term_output_consume(t, len);

    feed(t, "\x1b[3;4H\x1b[6n");
    out = term_output(t, &len);
    CHECK(len == 6 && memcmp(out, "\x1b[3;4R", 6) == 0);
    term_output_consume(t, len);

    feed(t, "\x1b]4;1;?\x07");
    out = term_output(t, &len);
    CHECK(len > 10 && memcmp(out, "\x1b]4;1;rgb:", 10) == 0);
    term_output_consume(t, len);

    feed(t, "\x1b[21t");                                         /* title report: disabled */
    (void)term_output(t, &len);
    CHECK(len == 0);

    feed(t, "\x1b[?2004h");
    CHECK(term_modes(t)->bracketed_paste);
    feed(t, "\x1b[?2004$p");
    out = term_output(t, &len);
    CHECK(len == 11 && memcmp(out, "\x1b[?2004;1$y", 11) == 0);
    term_output_consume(t, len);
    term_destroy(&t);
}

static void test_hostile_input(void)
{
    te_theme th;
    theme_defaults(&th);
    te_term *t = nullptr;
    CHECK(term_create(&t, 80, 24, 1000, &th, nullptr) == TE_OK);
    if (t == nullptr) return;

    feed(t, "\x1b[99999999999999999999A\x1b[99999;99999H\x1b[99999@\x1b[99999P\x1b[99999L\x1b[99999M");
    feed(t, "\x1b[1;2;3;4;5;6;7;8;9;10;11;12;13;14;15;16;17;18;19;20;21;22;23;24;25;26;27;28;29;30;31;32;33;34;35m");
    feed(t, "\x1b[65535b");                                      /* REP flood is capped */
    feed(t, "\x1b[99999X\x1b[99999S\x1b[99999T\x1b[99999I\x1b[99999Z");
    uint16_t x = 0, y = 0;
    term_cursor(t, &x, &y);
    CHECK(x < 80 && y < 24);

    /* An OSC larger than the cap is discarded, not truncated. */
    size_t big = VT_MAX_STRING + 100;
    char *s = te_malloc(big + 16);
    CHECK(s != nullptr);
    if (s != nullptr) {
        memcpy(s, "\x1b]2;", 4);
        memset(s + 4, 'A', big);
        s[4 + big] = '\x07';
        term_feed(t, (const uint8_t *)s, big + 5);
        TE_FREE(s);
    }
    CHECK(strlen(term_title(t)) == 0);

    /* Random bytes with interleaved resizes: must stay memory-safe. */
    srand(12345);
    uint8_t buf[4096];
    for (int round = 0; round < 400; round++) {
        for (size_t i = 0; i < sizeof buf; i++) {
            int r = rand();
            /* Bias towards escape-heavy input. */
            buf[i] = (r % 7 == 0) ? 0x1b : (r % 11 == 0) ? (uint8_t)'[' : (uint8_t)(r & 0xFF);
        }
        term_feed(t, buf, sizeof buf);
        if (round % 37 == 0) {
            uint16_t c = (uint16_t)(2 + rand() % 200);
            uint16_t r = (uint16_t)(1 + rand() % 80);
            CHECK(term_resize(t, c, r) == TE_OK);
        }
        if (round % 53 == 0) {
            term_scroll_view(t, rand() % 500);
            for (uint16_t row = 0; row < term_rows(t); row++) {
                (void)term_view_line(t, row);
            }
        }
        size_t len = 0;
        (void)term_output(t, &len);
        term_output_consume(t, len);
    }
    term_cursor(t, &x, &y);
    CHECK(x < term_cols(t) && y < term_rows(t));
    term_destroy(&t);
}

static void test_keys(void)
{
    te_modes m = { 0 };
    char out[64] = { 0 };
    CHECK(keys_encode(TE_KEY_UP, 0, 0, &m, out, sizeof out) == 3 && memcmp(out, "\x1b[A", 3) == 0);
    m.app_cursor = true;
    CHECK(keys_encode(TE_KEY_UP, 0, 0, &m, out, sizeof out) == 3 && memcmp(out, "\x1bOA", 3) == 0);
    CHECK(keys_encode(TE_KEY_LEFT, 0, TE_MOD_CTRL, &m, out, sizeof out) == 6 && memcmp(out, "\x1b[1;5D", 6) == 0);
    CHECK(keys_encode(TE_KEY_CHAR, 'c', TE_MOD_CTRL, &m, out, sizeof out) == 1 && out[0] == 3);
    CHECK(keys_encode(TE_KEY_CHAR, 'x', TE_MOD_ALT, &m, out, sizeof out) == 2 && memcmp(out, "\x1bx", 2) == 0);
    CHECK(keys_encode(TE_KEY_CHAR, 'x', 0, &m, out, sizeof out) == 0);
    CHECK(keys_encode(TE_KEY_DELETE, 0, TE_MOD_SHIFT, &m, out, sizeof out) == 6 && memcmp(out, "\x1b[3;2~", 6) == 0);

    size_t len = 0;
    char *p = keys_prepare_paste("ls\x1b[201~; rm -rf ~\n", true, true, &len);
    CHECK(p != nullptr);
    if (p != nullptr) {
        /* The only ESC bytes left are our own bracket markers. */
        size_t escs = 0;
        for (size_t i = 0; i < len; i++) escs += p[i] == '\x1b';
        CHECK(escs == 2);
        CHECK(memcmp(p, "\x1b[200~", 6) == 0 && memcmp(p + len - 6, "\x1b[201~", 6) == 0);
        CHECK(p[len - 7] == '\r');
        TE_FREE(p);
    }
    /* C1 controls (here U+009B CSI, UTF-8 0xC2 0x9B) are stripped when sanitising. */
    char *pc = keys_prepare_paste("a\xC2\x9B" "b", false, true, &len);
    CHECK(pc != nullptr);
    if (pc != nullptr) {
        CHECK(len == 2 && memcmp(pc, "ab", 2) == 0);
        TE_FREE(pc);
    }
}

typedef struct { int n; bool saw_regex; bool saw_quoted; } ini_ctx;

static te_err ini_cb(void *u, const char *sec, const char *key, const char *val, const char *path, unsigned line)
{
    (void)path;
    (void)line;
    ini_ctx *c = u;
    c->n++;
    if (strcmp(sec, "rule.x") == 0 && strcmp(key, "regex") == 0 && strcmp(val, "^a;b#c$") == 0) {
        c->saw_regex = true;
    }
    if (strcmp(key, "quoted") == 0 && strcmp(val, "\"[^\"]*\"") == 0) {
        c->saw_quoted = true;   /* quotes are kept literally */
    }
    return TE_OK;
}

static void test_ini(void)
{
    char path[512];
    (void)tmp_template(path, sizeof path, "te-ini");
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    if (fd < 0) return;
    FILE *fp = fdopen(fd, "w");
    if (fp == nullptr) { close(fd); return; }
    fprintf(fp, "; comment\n[a]\nk = v\n[rule.x]\nregex = ^a;b#c$\nquoted = \"[^\"]*\"\n# c\nnoequals\n");
    for (int i = 0; i < 6000; i++) fputc('x', fp);   /* over-long line is skipped */
    fprintf(fp, "\nlast = 1\n");
    fclose(fp);
    ini_ctx c = { 0 };
    CHECK(ini_parse_file(path, ini_cb, &c) == TE_OK);
    CHECK(c.n == 4 && c.saw_regex && c.saw_quoted);
    CHECK(ini_parse_file("/nonexistent/x.ini", ini_cb, &c) == TE_ERR_NOTFOUND);
    unlink(path);

    uint32_t rgb = 0;
    CHECK(color_parse("#a1B2c3", &rgb) && rgb == 0xA1B2C3);
    CHECK(!color_parse("#a1B2c", &rgb) && !color_parse("#a1B2c3d", &rgb) && !color_parse("zzzzzz", &rgb));
}

/* Writes text to a new temporary file; path receives its name. */
static bool write_tmp(char *path, const char *text)
{
    int fd = mkstemp(path);
    if (fd < 0) return false;
    FILE *fp = fdopen(fd, "w");
    if (fp == nullptr) { close(fd); unlink(path); return false; }
    bool ok = fputs(text, fp) >= 0;
    ok = fclose(fp) == 0 && ok;
    if (!ok) unlink(path);
    return ok;
}

static void test_config_transparency(void)
{
    te_config c;
    config_defaults(&c);
    CHECK(c.opacity > 0.899f && c.opacity < 0.901f);
    CHECK(!c.transparent_cells);

    /* Out-of-range and malformed values are rejected and leave the defaults. */
    char bad[512];
    (void)tmp_template(bad, sizeof bad, "te-cfg");
    CHECK(write_tmp(bad, "[window]\nopacity = 0.05\ntransparent_cell_backgrounds = maybe\n"));
    CHECK(config_load(&c, bad) == TE_OK);
    CHECK(c.opacity > 0.899f && c.opacity < 0.901f && !c.transparent_cells);
    unlink(bad);
    char high[512];
    (void)tmp_template(high, sizeof high, "te-cfg");
    CHECK(write_tmp(high, "[window]\nopacity = 1.5\n"));
    CHECK(config_load(&c, high) == TE_OK);
    CHECK(c.opacity > 0.899f && c.opacity < 0.901f);
    unlink(high);

    char good[512];
    (void)tmp_template(good, sizeof good, "te-cfg");
    CHECK(write_tmp(good, "[window]\nopacity = 0.75\ntransparent_cell_backgrounds = true\n"));
    CHECK(config_load(&c, good) == TE_OK);
    CHECK(c.opacity > 0.749f && c.opacity < 0.751f && c.transparent_cells);
    unlink(good);
    char off[512];
    (void)tmp_template(off, sizeof off, "te-cfg");
    CHECK(write_tmp(off, "[window]\nopacity = 1.0\ntransparent_cell_backgrounds = false\n"));
    CHECK(config_load(&c, off) == TE_OK);
    CHECK(c.opacity > 0.999f && !c.transparent_cells);
    unlink(off);
}

/* ------------------------------------------------------------------------- */
/* Highlighting */

static te_term *line_term(const char *text, uint16_t cols)
{
    te_theme th;
    theme_defaults(&th);
    te_term *t = nullptr;
    if (term_create(&t, cols, 4, 10, &th, nullptr) != TE_OK) {
        return nullptr;
    }
    feed(t, text);
    return t;
}

/* Style applied at column col of row 0 of a terminal showing text, or nullptr. */
static const te_hl_style *style_at(te_highlighter *h, const char *text, uint16_t col)
{
    te_term *t = line_term(text, 120);
    if (t == nullptr) return nullptr;
    const te_line *l = term_view_line(t, 0);
    const te_hl_span *sp = nullptr;
    size_t n = hl_line(h, l->cells, l->cols, &sp);
    const te_hl_style *st = nullptr;
    for (size_t i = 0; i < n; i++) {
        if (col >= sp[i].start && col < sp[i].end) {
            st = hl_style(h, sp[i].style);
        }
    }
    term_destroy(&t);
    return st;
}

static char g_hl_root[256];

/* XDG_CONFIG_HOME=<tmp>; <tmp>/<app>/highlight holds the bundled profiles plus a ReDoS one. */
static bool setup_hl_dir(void)
{
    char cwd[512] = { 0 };
    if (getcwd(cwd, sizeof cwd) == nullptr) return false;
    if (tmp_template(g_hl_root, sizeof g_hl_root, "te-hl")[0] == '\0' || mkdtemp(g_hl_root) == nullptr) {
        return false;
    }
    char dir[600] = { 0 };
    (void)str_fmt(dir, sizeof dir, "%s/%s", g_hl_root, APP_NAME);
    if (mkdir(dir, 0700) != 0) return false;
    (void)str_append(dir, sizeof dir, "/highlight");
    if (mkdir(dir, 0700) != 0) return false;
    static const char *const files[] = { "network-common.ini", "cisco.ini", "arista.ini", "juniper.ini" };
    for (size_t i = 0; i < TE_ARRAY_LEN(files); i++) {
        char src[800] = { 0 };
        char dst[800] = { 0 };
        (void)str_fmt(src, sizeof src, "%s/data/highlight/%s", cwd, files[i]);
        (void)str_fmt(dst, sizeof dst, "%s/%s", dir, files[i]);
#ifdef _WIN32
        if (!CopyFileA(src, dst, TRUE)) return false;
#else
        if (symlink(src, dst) != 0) return false;
#endif
    }
    char redos[800] = { 0 };
    (void)str_fmt(redos, sizeof redos, "%s/zz-redos.ini", dir);
    FILE *fp = fopen(redos, "w");
    if (fp == nullptr) return false;
    fprintf(fp, "[profile]\nname = zz-redos\n[rule.evil]\nregex = ^(a|a)+$\nfg = red\n"
                "[rule.bad-regex]\nregex = (unclosed\n[rule.ok]\nregex = b\nfg = blue\n");
    fclose(fp);
#ifdef _WIN32
    return setenv("APPDATA", g_hl_root, 1) == 0;   /* the config base on Windows */
#else
    return setenv("XDG_CONFIG_HOME", g_hl_root, 1) == 0;
#endif
}

static void test_highlight(void)
{
    CHECK(setup_hl_dir());
    te_highlighter *h = nullptr;
    CHECK(hl_create(&h) == TE_OK);
    if (h == nullptr) return;
    hl_set_fallback(h, "network-common");
    CHECK(hl_load_all(h) == TE_OK);
    CHECK(hl_profile_count(h) == 5);
    CHECK(strcmp(hl_profile_name(h, 0), "arista") == 0 && strcmp(hl_profile_name(h, 3), "network-common") == 0);
    CHECK(hl_set_mode(h, TE_HL_AUTO, nullptr) == TE_OK);
    CHECK(hl_active_name(h) != nullptr && strcmp(hl_active_name(h), "network-common") == 0);

    /* Shared rules. */
    const te_hl_style *st = style_at(h, "addr 10.1.2.3/24 is up, peer down", 6);
    CHECK(st != nullptr && st->fg == TE_COLOR_PAL(5));             /* ipv4: magenta */
    st = style_at(h, "addr 10.1.2.3/24 is up, peer down", 20);
    CHECK(st != nullptr && st->fg == TE_COLOR_PAL(2));             /* up: green */
    st = style_at(h, "addr 10.1.2.3/24 is up, peer down", 30);
    CHECK(st != nullptr && st->fg == TE_COLOR_PAL(1));             /* down: red */
    st = style_at(h, "Gi0/2 administratively down", 10);
    CHECK(st != nullptr && st->fg == TE_COLOR_PAL(3));             /* admin down: yellow wins */
    st = style_at(h, "    0 input errors, 12 CRC, 0 frame", 5);
    CHECK(st != nullptr && st->fg == TE_COLOR_PAL(2));             /* zero counter: fine */
    st = style_at(h, "    0 input errors, 12 CRC, 0 frame", 22);
    CHECK(st != nullptr && st->fg == TE_COLOR_PAL(1) && (st->attrs & TE_ATTR_BOLD)); /* non-zero */
    st = style_at(h, "neighbor 2001:db8::1 fe80::1%eth0 aa:bb:cc:dd:ee:ff", 10);
    CHECK(st != nullptr && st->fg == TE_COLOR_PAL(5));             /* ipv6 */
    st = style_at(h, "neighbor 2001:db8::1 fe80::1%eth0 aa:bb:cc:dd:ee:ff", 36);
    CHECK(st != nullptr && st->fg == TE_COLOR_PAL(3));             /* mac */
    st = style_at(h, "uptime 12:30:45", 8);
    CHECK(st == nullptr || st->fg != TE_COLOR_PAL(5));             /* a time is not IPv6 */
    st = style_at(h, "*Mar  1 00:01:02: %LINK-3-UPDOWN: Interface", 20);
    CHECK(st != nullptr && st->fg == TE_COLOR_PAL(1));             /* syslog severity 3 */

    /* Auto-detection: Cisco banner. */
    (void)style_at(h, "Cisco IOS XE Software, Version 17.09.04a", 0);
    CHECK(hl_take_changed(h));
    CHECK(strcmp(hl_active_name(h), "cisco") == 0);
    st = style_at(h, "GigabitEthernet1/0/1 is up", 3);
    CHECK(st != nullptr && st->fg == TE_COLOR_PAL(4));             /* interface: blue */
    st = style_at(h, "core-sw01(config-if)#shutdown", 2);
    CHECK(st != nullptr && st->fg == TE_COLOR_PAL(10));            /* prompt host */
    st = style_at(h, "core-sw01(config-if)#shutdown", 12);
    CHECK(st != nullptr && st->fg == TE_COLOR_PAL(3));             /* prompt mode */
    st = style_at(h, " ip address 10.0.0.1 255.255.255.0", 2);
    CHECK(st != nullptr && st->fg == TE_COLOR_PAL(6) && (st->attrs & TE_ATTR_BOLD)); /* keyword */
    st = style_at(h, " description Uplink to core", 16);
    CHECK(st != nullptr && (st->attrs & TE_ATTR_ITALIC));          /* description text */
    st = style_at(h, "username admin secret 9 $9$abc", 25);
    CHECK(st != nullptr && ((st->attrs & TE_ATTR_UL_MASK) >> TE_ATTR_UL_SHIFT) == TE_UL_CURLY);

    /* Juniper. */
    (void)style_at(h, "{master:0}", 0);
    CHECK(strcmp(hl_active_name(h), "juniper") == 0);
    st = style_at(h, "set interfaces ge-0/0/1 unit 0 family inet", 17);
    CHECK(st != nullptr && st->fg == TE_COLOR_PAL(4));
    st = style_at(h, "admin@mx1# delete protocols ospf", 12);
    CHECK(st != nullptr && st->fg == TE_COLOR_PAL(1));             /* delete after a prompt */
    st = style_at(h, "description \"to core\";", 1);
    CHECK(st == nullptr || st->fg != TE_COLOR_PAL(2));             /* plain text stays plain */
    st = style_at(h, "admin@mx1> show bgp summary", 3);
    CHECK(st != nullptr && st->fg == TE_COLOR_PAL(10));

    /* Arista: inherits Cisco rules, replaces the interface rule. */
    (void)style_at(h, "Arista DCS-7050SX3-48YC8", 0);
    CHECK(strcmp(hl_active_name(h), "arista") == 0);
    st = style_at(h, "Ethernet49/1 is up", 3);
    CHECK(st != nullptr && st->fg == TE_COLOR_PAL(4));
    st = style_at(h, "leaf1(config-if-Et1)#", 1);
    CHECK(st != nullptr && st->fg == TE_COLOR_PAL(10));            /* inherited prompt rule */

    /* Shell prompt seen again: back to the fallback. */
    hl_auto_reset(h);
    CHECK(strcmp(hl_active_name(h), "network-common") == 0);

    /* Weak prompt detection only from the fallback. */
    (void)style_at(h, "edge-rtr2#", 0);
    CHECK(strcmp(hl_active_name(h), "cisco") == 0);

    /* ReDoS: a catastrophic rule is stopped by the match limit and disabled. */
    CHECK(hl_set_mode(h, TE_HL_PINNED, "zz-redos") == TE_OK);
    struct timespec t0 = { 0 }, t1 = { 0 };
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < 6; i++) {
        char evil[200] = { 0 };
        memset(evil, 'a', 60);
        evil[60] = (char)('0' + i);                                  /* new text each time: no cache hit */
        (void)style_at(h, evil, 0);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    CHECK(secs < 5.0);
    st = style_at(h, "xxbxx", 2);
    CHECK(st != nullptr && st->fg == TE_COLOR_PAL(4));             /* other rules still work */

    CHECK(hl_set_mode(h, TE_HL_PINNED, "no-such-profile") == TE_ERR_NOTFOUND);
    CHECK(hl_set_mode(h, TE_HL_OFF, nullptr) == TE_OK);
    CHECK(style_at(h, "10.0.0.1 up", 0) == nullptr);

    /* Prompt-end detection for click-to-position. */
    static const struct { const char *line; int end; } prompts[] = {
        { "sw1(config-if)#shutdown", 15 },
        { "admin@mx1> show route", 10 },
        { "user@host:~/src$ make -j8", 17 },
        { "[root@box ~]# ls -la", 14 },
        { "no prompt here", -1 },
    };
    for (size_t i = 0; i < TE_ARRAY_LEN(prompts); i++) {
        te_term *t = line_term(prompts[i].line, 80);
        if (t == nullptr) continue;
        const te_line *l = term_view_line(t, 0);
        int got = hl_prompt_end(h, l->cells, l->cols);
        if (got != prompts[i].end) {
            fprintf(stderr, "prompt '%s': got %d want %d\n", prompts[i].line, got, prompts[i].end);
        }
        CHECK(got == prompts[i].end);
        term_destroy(&t);
    }
    hl_destroy(&h);
    CHECK(h == nullptr);

    /* Remove the temporary profile directory. */
    static const char *const names[] = { "network-common.ini", "cisco.ini", "arista.ini", "juniper.ini", "zz-redos.ini" };
    char path[800] = { 0 };
    for (size_t i = 0; i < TE_ARRAY_LEN(names); i++) {
        (void)str_fmt(path, sizeof path, "%s/%s/highlight/%s", g_hl_root, APP_NAME, names[i]);
        (void)unlink(path);
    }
    (void)str_fmt(path, sizeof path, "%s/%s/highlight", g_hl_root, APP_NAME);
    (void)rmdir(path);
    (void)str_fmt(path, sizeof path, "%s/%s", g_hl_root, APP_NAME);
    (void)rmdir(path);
    (void)rmdir(g_hl_root);
}

/* ------------------------------------------------------------------------- */
/* Click-to-position */

static size_t count_seq(const char *buf, size_t len, const char *seq)
{
    size_t n = 0;
    size_t sl = strlen(seq);
    for (size_t i = 0; i + sl <= len; i++) {
        if (memcmp(buf + i, seq, sl) == 0) {
            n++;
            i += sl - 1;
        }
    }
    return n;
}

static void test_click(void)
{
    char out[8192] = { 0 };
    te_click_opts o = { .emacs_keys = false, .prompt_end = -1 };

    /* Plain line, prompt given by the caller. */
    te_term *t = line_term("user@host:~$ echo hello world", 80);
    if (t == nullptr) return;
    o.prompt_end = 13;
    size_t n = click_to_position(t, 18, 0, &o, out, sizeof out);       /* on the 'h' of hello */
    CHECK(count_seq(out, n, "\x1b[D") == 11 && n == 33);
    n = click_to_position(t, 2, 0, &o, out, sizeof out);               /* inside the prompt: clamp */
    CHECK(count_seq(out, n, "\x1b[D") == 16);
    n = click_to_position(t, 70, 0, &o, out, sizeof out);              /* past the end: no move */
    CHECK(n == 0);
    n = click_to_position(t, 5, 2, &o, out, sizeof out);               /* another row: ignored */
    CHECK(n == 0);
    feed(t, "\x1b[11D");                                              /* cursor back to 'h' */
    n = click_to_position(t, 27, 0, &o, out, sizeof out);
    CHECK(count_seq(out, n, "\x1b[C") == 9);
    feed(t, "\x1b[?1h");                                              /* application cursor keys */
    n = click_to_position(t, 13, 0, &o, out, sizeof out);
    CHECK(count_seq(out, n, "\x1bOD") == 5);
    o.emacs_keys = true;
    n = click_to_position(t, 13, 0, &o, out, sizeof out);
    CHECK(n == 5 && out[0] == 0x02);
    o.emacs_keys = false;
    term_destroy(&t);

    /* Wide characters: one key press per character, not per cell. */
    t = line_term("$ \xe6\xbc\xa2\xe5\xad\x97" "abc", 80);   /* "$ " + 2 wide + abc */
    if (t == nullptr) return;
    o.prompt_end = 2;
    n = click_to_position(t, 3, 0, &o, out, sizeof out);               /* right half of the first */
    CHECK(count_seq(out, n, "\x1b[D") == 5);
    term_destroy(&t);

    /* Soft-wrapped input across rows. */
    t = line_term("$ 0123456789abcdefghij", 10);                     /* 22 chars over 3 rows */
    if (t == nullptr) return;
    uint16_t top = 9, bottom = 9;
    click_input_block(t, &top, &bottom);
    CHECK(top == 0 && bottom == 2);
    n = click_to_position(t, 4, 0, &o, out, sizeof out);               /* on '2' */
    CHECK(count_seq(out, n, "\x1b[D") == 18);
    term_destroy(&t);

    /* OSC 133: input start is known; clicks while a command runs are ignored. */
    t = line_term("\x1b]133;A\x07$ \x1b]133;B\x07" "abc def", 80);
    if (t == nullptr) return;
    o.prompt_end = -1;
    n = click_to_position(t, 0, 0, &o, out, sizeof out);
    CHECK(count_seq(out, n, "\x1b[D") == 7);
    feed(t, "\x1b]133;C\x07");
    n = click_to_position(t, 3, 0, &o, out, sizeof out);
    CHECK(n == 0);
    term_destroy(&t);

    /* OSC 133 click_events=1: the shell gets an SGR mouse click instead. */
    t = line_term("\x1b]133;A;click_events=1\x07$ \x1b]133;B\x07" "ab", 80);
    if (t == nullptr) return;
    n = click_to_position(t, 2, 0, &o, out, sizeof out);
    CHECK(n > 0 && strcmp(out, "\x1b[<0;3;1M\x1b[<0;3;1m") == 0);
    term_destroy(&t);

    /* Scrolled back: never sends keys. */
    t = line_term("$ abc", 80);
    if (t == nullptr) return;
    for (int i = 0; i < 10; i++) feed(t, "\r\nline");
    feed(t, "\r\n$ abc");
    term_scroll_view(t, 3);
    n = click_to_position(t, 2, 3, &o, out, sizeof out);
    CHECK(n == 0);
    term_destroy(&t);
}

/* ------------------------------------------------------------------------- */
/* Phase 3 */

static te_term *mk_term(uint16_t cols, uint16_t rows, uint32_t sb)
{
    te_theme th;
    theme_defaults(&th);
    te_term *t = nullptr;
    return term_create(&t, cols, rows, sb, &th, nullptr) == TE_OK ? t : nullptr;
}

/* Text of one absolute line, trailing blanks trimmed. */
static void abs_text(te_term *t, uint64_t abs, char *out, size_t outsz)
{
    const te_line *l = term_abs_line(t, abs);
    size_t o = 0;
    out[0] = '\0';
    if (l == nullptr) return;
    for (uint16_t x = 0; x < l->cols && o + 1 < outsz; x++) {
        uint32_t cp = l->cells[x].cp;
        if ((l->cells[x].attrs & TE_ATTR_SPACER) != 0) continue;
        out[o++] = cp == 0 ? ' ' : (cp < 0x80 ? (char)cp : '#');
    }
    while (o > 0 && out[o - 1] == ' ') o--;
    out[o] = '\0';
}

static void test_reflow(void)
{
    te_term *t = mk_term(10, 4, 100);
    if (t == nullptr) return;
    feed(t, "abcdefghijKLMNO\r\nxyz");                     /* first line wraps after 10 */
    char buf[64];
    uint64_t g0 = term_view_abs(t, 0);
    abs_text(t, g0, buf, sizeof buf);
    CHECK(strcmp(buf, "abcdefghij") == 0);

    CHECK(term_resize(t, 20, 4) == TE_OK);                 /* widen: the wrap is undone */
    g0 = term_view_abs(t, 0);
    abs_text(t, g0, buf, sizeof buf);
    CHECK(strcmp(buf, "abcdefghijKLMNO") == 0);
    abs_text(t, g0 + 1, buf, sizeof buf);
    CHECK(strcmp(buf, "xyz") == 0);
    uint16_t x = 0, y = 0;
    term_cursor(t, &x, &y);
    CHECK(x == 3 && y == 1);                                /* cursor stays after "xyz" */

    CHECK(term_resize(t, 6, 4) == TE_OK);                  /* narrow: wraps into 3 rows */
    g0 = term_view_abs(t, 0);
    abs_text(t, g0, buf, sizeof buf);
    CHECK(strcmp(buf, "abcdef") == 0);
    abs_text(t, g0 + 2, buf, sizeof buf);
    CHECK(strcmp(buf, "MNO") == 0);
    CHECK((term_abs_line(t, g0)->flags & TE_LINE_WRAPPED) != 0);
    abs_text(t, g0 + 3, buf, sizeof buf);
    CHECK(strcmp(buf, "xyz") == 0);
    term_cursor(t, &x, &y);
    CHECK(x == 3 && y == 3);

    CHECK(term_resize(t, 3, 2) == TE_OK);                  /* overflow goes into scrollback */
    CHECK(term_scrollback_count(t) > 0);
    term_cursor(t, &x, &y);
    CHECK(y < 2 && x < 3);
    feed(t, "Q");
    CHECK(term_resize(t, 40, 10) == TE_OK);
    g0 = term_view_abs(t, 0);
    bool found = false;
    for (uint64_t r = term_abs_first(t); r < term_abs_end(t); r++) {
        abs_text(t, r, buf, sizeof buf);
        if (strcmp(buf, "abcdefghijKLMNO") == 0) found = true;
    }
    CHECK(found);                                           /* survives a round trip via scrollback */

    /* Wide characters never split across rows. */
    te_term *w = mk_term(5, 3, 10);
    if (w != nullptr) {
        feed(w, "ab\xe6\xbc\xa2\xe5\xad\x97" "c");          /* ab + 2 wide + c = 7 cells */
        CHECK(term_resize(w, 4, 3) == TE_OK);
        const te_line *l0 = term_abs_line(w, term_view_abs(w, 0));
        const te_line *l1 = term_abs_line(w, term_view_abs(w, 1));
        CHECK(l0->cells[2].cp == 0x6F22 && (l0->cells[3].attrs & TE_ATTR_SPACER));
        CHECK(l1->cells[0].cp == 0x5B57 && l1->cells[2].cp == 'c');
        term_destroy(&w);
    }
    term_destroy(&t);
}

static void test_osc8_and_abs(void)
{
    te_term *t = mk_term(40, 3, 50);
    if (t == nullptr) return;
    feed(t, "see \x1b]8;;https://example.com/a\x1b\\here\x1b]8;;\x1b\\ done");
    uint64_t r = term_view_abs(t, 0);
    const te_line *l = term_abs_line(t, r);
    CHECK(l->cells[4].link != 0 && l->cells[7].link == l->cells[4].link && l->cells[8].link == 0);
    CHECK(strcmp(term_link_uri(t, l->cells[4].link), "https://example.com/a") == 0);
    feed(t, "\r\n\x1b]8;id=x;https://example.com/a\x07z\x1b]8;;\x07");
    feed(t, "\r\n\x1b]8;;https://bad uri\x07q\x1b]8;;\x07");    /* space: rejected */
    const te_line *l2 = term_abs_line(t, r + 2);
    CHECK(l2 != nullptr && l2->cells[0].link == 0);
    feed(t, "\x1b]8;;https://x/\x07\x1b[0mA\x1b]8;;\x07");      /* SGR 0 keeps the link */
    CHECK(term_abs_line(t, r + 2)->cells[1].link != 0);

    /* Absolute numbers follow text into the scrollback. */
    uint64_t first_line = r;
    for (int i = 0; i < 10; i++) feed(t, "\r\nmore");
    abs_text(t, first_line, (char[64]){0}, 64);
    char buf[64];
    abs_text(t, first_line, buf, sizeof buf);
    CHECK(strncmp(buf, "see here done", 13) == 0);
    CHECK(term_view_abs(t, 0) > first_line);

    /* Prompt marks. */
    feed(t, "\r\n\x1b]133;A\x07$ one\r\nout\r\n\x1b]133;A\x07$ two");
    uint64_t p = 0;
    CHECK(term_find_prompt(t, term_abs_end(t), -1, &p));
    abs_text(t, p, buf, sizeof buf);
    CHECK(strcmp(buf, "$ two") == 0);
    CHECK(term_find_prompt(t, p, -1, &p));
    abs_text(t, p, buf, sizeof buf);
    CHECK(strcmp(buf, "$ one") == 0);

    /* CAN (0x18) cancels an in-progress OSC instead of dispatching it. */
    feed(t, "\x1b]0;good\x1b\\");
    CHECK(strcmp(term_title(t), "good") == 0);
    feed(t, "\x1b]0;canceled\x18");
    CHECK(strcmp(term_title(t), "good") == 0);

    /* OSC 7: only this host's cwd is honoured; another host's is ignored. */
    feed(t, "\x1b]7;file://localhost/tmp/here\x1b\\");
    CHECK(strcmp(term_cwd(t), "/tmp/here") == 0);
    feed(t, "\x1b]7;file://evil.example.com/tmp/pwned\x1b\\");
    CHECK(strcmp(term_cwd(t), "/tmp/here") == 0);
    feed(t, "\x1b]7;file://localhost/bad%01path\x1b\\");   /* percent-encoded control byte: rejected */
    CHECK(strcmp(term_cwd(t), "/tmp/here") == 0);

    /* RIS (ESC c) frees the hyperlink table; a fresh link still works afterwards. */
    feed(t, "\x1b""c");
    feed(t, "\x1b]8;;https://y/z\x07K\x1b]8;;\x07");
    const te_line *lr = term_abs_line(t, term_view_abs(t, 0));
    CHECK(lr != nullptr && lr->cells[0].link != 0 &&
          strcmp(term_link_uri(t, lr->cells[0].link), "https://y/z") == 0);
    term_destroy(&t);
}

static void test_selection(void)
{
    te_term *t = mk_term(12, 4, 20);
    if (t == nullptr) return;
    feed(t, "hello there world\r\nip 10.0.0.1\r\nlast");      /* row0 wraps: "hello there " + "world" */
    uint64_t r0 = term_view_abs(t, 0);
    te_selection s;
    sel_clear(&s);
    size_t len = 0;
    sel_start(&s, t, (te_selpos){ r0, 0 }, TE_SEL_STREAM, "-_./");
    sel_update(&s, t, (te_selpos){ r0 + 1, 4 }, "-_./");
    char *txt = sel_text(&s, t, &len);
    CHECK(txt != nullptr && strcmp(txt, "hello there world") == 0);  /* soft wrap joined */
    TE_FREE(txt);

    sel_update(&s, t, (te_selpos){ r0 + 2, 4 }, "-_./");
    txt = sel_text(&s, t, &len);
    CHECK(txt != nullptr && strcmp(txt, "hello there world\nip 10") == 0);
    TE_FREE(txt);

    sel_start(&s, t, (te_selpos){ r0 + 2, 6 }, TE_SEL_WORD, "-_./");  /* double-click on the IP */
    txt = sel_text(&s, t, &len);
    CHECK(txt != nullptr && strcmp(txt, "10.0.0.1") == 0);
    TE_FREE(txt);

    sel_start(&s, t, (te_selpos){ r0 + 1, 0 }, TE_SEL_LINE, "");       /* triple-click on the wrapped part */
    txt = sel_text(&s, t, &len);
    CHECK(txt != nullptr && strcmp(txt, "hello there world") == 0);
    TE_FREE(txt);

    sel_start(&s, t, (te_selpos){ r0 + 2, 0 }, TE_SEL_BLOCK, "");
    sel_update(&s, t, (te_selpos){ r0 + 3, 1 }, "");
    txt = sel_text(&s, t, &len);
    CHECK(txt != nullptr && strcmp(txt, "ip\nla") == 0);
    TE_FREE(txt);
    CHECK(sel_contains(&s, r0 + 3, 1) && !sel_contains(&s, r0 + 3, 2));

    /* A plain click (no movement) selects nothing. */
    sel_start(&s, t, (te_selpos){ r0, 3 }, TE_SEL_STREAM, "");
    sel_update(&s, t, (te_selpos){ r0, 3 }, "");
    CHECK(!s.active && sel_text(&s, t, &len) == nullptr);

    /* Lines dropping out of the scrollback invalidate the selection. */
    sel_start(&s, t, (te_selpos){ r0, 0 }, TE_SEL_STREAM, "");
    sel_update(&s, t, (te_selpos){ r0, 4 }, "");
    for (int i = 0; i < 40; i++) feed(t, "\r\nscroll");
    sel_validate(&s, t);
    CHECK(!s.active);
    term_destroy(&t);
}

static void test_mouse(void)
{
    te_modes m = { 0 };
    char out[64];
    m.mouse = TE_MOUSE_NORMAL;
    m.mouse_enc = TE_MOUSE_ENC_SGR;
    CHECK(mouse_wants(&m, TE_MOUSE_PRESS, false) && !mouse_wants(&m, TE_MOUSE_MOVE, true));
    size_t n = mouse_encode(&m, TE_MOUSE_PRESS, TE_MB_LEFT, TE_MOD_CTRL, 4, 2, 0, 0, out, sizeof out);
    CHECK(n > 0 && strcmp(out, "\x1b[<16;5;3M") == 0);
    n = mouse_encode(&m, TE_MOUSE_RELEASE, TE_MB_RIGHT, 0, 4, 2, 0, 0, out, sizeof out);
    CHECK(n > 0 && strcmp(out, "\x1b[<2;5;3m") == 0);
    n = mouse_encode(&m, TE_MOUSE_PRESS, TE_MB_WHEEL_DOWN, 0, 0, 0, 0, 0, out, sizeof out);
    CHECK(n > 0 && strcmp(out, "\x1b[<65;1;1M") == 0);
    m.mouse = TE_MOUSE_BUTTON;
    CHECK(mouse_wants(&m, TE_MOUSE_MOVE, true) && !mouse_wants(&m, TE_MOUSE_MOVE, false));
    n = mouse_encode(&m, TE_MOUSE_MOVE, TE_MB_LEFT, 0, 9, 9, 0, 0, out, sizeof out);
    CHECK(n > 0 && strcmp(out, "\x1b[<32;10;10M") == 0);
    m.mouse_enc = TE_MOUSE_ENC_DEFAULT;
    n = mouse_encode(&m, TE_MOUSE_RELEASE, TE_MB_LEFT, 0, 0, 0, 0, 0, out, sizeof out);
    CHECK(n == 6 && (unsigned char)out[3] == 32 + 3 && out[4] == 33 && out[5] == 33);
    n = mouse_encode(&m, TE_MOUSE_PRESS, TE_MB_LEFT, 0, 300, 0, 0, 0, out, sizeof out);
    CHECK(n == 0);                                            /* column 301 cannot be encoded */
    m.mouse_enc = TE_MOUSE_ENC_UTF8;
    n = mouse_encode(&m, TE_MOUSE_PRESS, TE_MB_LEFT, 0, 300, 0, 0, 0, out, sizeof out);
    CHECK(n == 7);                                            /* 2-byte UTF-8 x coordinate */
    m.mouse_enc = TE_MOUSE_ENC_URXVT;
    n = mouse_encode(&m, TE_MOUSE_PRESS, TE_MB_MIDDLE, 0, 1, 1, 0, 0, out, sizeof out);
    CHECK(n > 0 && strcmp(out, "\x1b[33;2;2M") == 0);
    m.mouse_enc = TE_MOUSE_ENC_SGR_PIXELS;
    n = mouse_encode(&m, TE_MOUSE_PRESS, TE_MB_LEFT, 0, 1, 1, 123, 45, out, sizeof out);
    CHECK(n > 0 && strcmp(out, "\x1b[<0;124;46M") == 0);
    m.mouse = TE_MOUSE_X10;
    n = mouse_encode(&m, TE_MOUSE_PRESS, TE_MB_LEFT, TE_MOD_SHIFT, 0, 0, 0, 0, out, sizeof out);
    CHECK(n > 0 && strcmp(out, "\x1b[<0;1;1M") == 0);         /* X10: no modifiers */
    CHECK(mouse_encode(&m, TE_MOUSE_RELEASE, TE_MB_LEFT, 0, 0, 0, 0, 0, out, sizeof out) == 0);
}

static void test_urls(void)
{
    te_urlfinder *u = nullptr;
    CHECK(url_create(&u) == TE_OK);
    if (u == nullptr) return;
    te_term *t = mk_term(30, 4, 10);
    if (t == nullptr) { url_destroy(&u); return; }
    feed(t, "see https://ex.com/a_(b)). ok\r\n");      /* 29 chars: fits in 30 columns */
    uint64_t r = term_view_abs(t, 0);
    te_url_hit h;
    CHECK(url_at(u, t, r, 12, &h) && strcmp(h.uri, "https://ex.com/a_(b)") == 0);
    CHECK(!url_at(u, t, r, 2, &h));
    feed(t, "https://example.org/very/long/path?q=1");   /* wraps onto the next row */
    uint64_t r2 = term_view_abs(t, 1);
    CHECK(url_at(u, t, r2 + 1, 2, &h) && strcmp(h.uri, "https://example.org/very/long/path?q=1") == 0);
    if (!(h.row0 == r2 && h.row1 == r2 + 1)) {
        fprintf(stderr, "url rows %llu..%llu cols %u..%u, expected %llu..%llu\n", (unsigned long long)h.row0,
                (unsigned long long)h.row1, h.col0, h.col1, (unsigned long long)r2, (unsigned long long)(r2 + 1));
    }
    CHECK(h.row0 == r2 && h.row1 == r2 + 1);
    feed(t, "\r\n\x1b]8;;file:///etc/hosts\x07hosts\x1b]8;;\x07");
    CHECK(url_at(u, t, term_view_abs(t, 3), 2, &h) && strcmp(h.uri, "file:///etc/hosts") == 0);
    CHECK(h.col0 == 0 && h.col1 == 4);

    CHECK(url_is_openable("https://a.b/c"));
    CHECK(url_is_openable("www.example.com"));
    CHECK(!url_is_openable("-dangerous"));
    CHECK(!url_is_openable("javascript:alert(1)"));
    CHECK(!url_is_openable("https://a.b/\x1b[31m"));
    CHECK(!url_is_openable("http://a b"));
    CHECK(url_is_openable("file:///etc/hosts"));              /* local file:// is fine */
    CHECK(!url_is_openable("file://host/share/x.exe"));       /* UNC / remote host rejected */
    CHECK(!url_is_openable("file://server/x"));               /* non-empty host rejected */
    CHECK(!url_is_openable("file://"));                       /* nothing to open */
    CHECK(!url_is_openable("https://a.b/\\\\host\\share"));   /* backslash rejected */
    term_destroy(&t);
    url_destroy(&u);
    CHECK(u == nullptr);
}

static void test_search(void)
{
    te_search *s = nullptr;
    CHECK(search_create(&s) == TE_OK);
    if (s == nullptr) return;
    te_term *t = mk_term(40, 3, 100);
    if (t == nullptr) { search_destroy(&s); return; }
    feed(t, "Gi0/1 up\r\nGi0/2 down\r\nlo0 up\r\nGi0/3 UP\r\nend");
    search_open(s);
    search_append(s, t, "up");                               /* smart case: matches UP too */
    CHECK(strcmp(search_status(s), "3/3") == 0);
    search_append(s, t, "");
    search_backspace(s, t);
    search_backspace(s, t);
    search_append(s, t, "UP");                               /* upper case: exact */
    CHECK(strcmp(search_status(s), "1/1") == 0);
    search_backspace(s, t);
    search_backspace(s, t);
    search_append(s, t, "Gi0/[13]");
    CHECK(strcmp(search_status(s), "2/2") == 0);
    te_search_match m;
    CHECK(search_current(s, &m) && m.col0 == 0 && m.col1 == 4);
    uint64_t newest = m.row;
    CHECK(search_step(s, t, -1) && search_current(s, &m) && m.row < newest);
    CHECK(search_step(s, t, -1) && search_current(s, &m) && m.row == newest);   /* wraps */
    bool cur = false;
    CHECK(search_cell(s, newest, 2, &cur) && cur);
    for (int i = 0; i < 8; i++) search_backspace(s, t);
    search_append(s, t, "(unclosed");                         /* invalid regex: literal text */
    CHECK(strcmp(search_status(s), "no matches") == 0);
    feed(t, "\r\nx(unclosed");
    CHECK(search_step(s, t, -1));                             /* new output is picked up */
    CHECK(strstr(search_status(s), "(text)") != nullptr);
    search_close(s);
    CHECK(!search_cell(s, newest, 2, &cur));
    term_destroy(&t);
    search_destroy(&s);
}

static void test_keymap(void)
{
    te_keymap k;
    keymap_defaults(&k);
    uint32_t cs = TE_MOD_CTRL | TE_MOD_SHIFT;
    char d[48] = { 0 };
#ifdef __APPLE__
    /* macOS: Cmd shortcuts; Ctrl and Ctrl+Shift belong to the shell. */
    uint32_t cmd = TE_MOD_SUPER;
    CHECK(keymap_lookup(&k, TE_KEY_CHAR, 'c', cmd) == TE_ACT_COPY);
    CHECK(keymap_lookup(&k, TE_KEY_CHAR, 'v', cmd) == TE_ACT_PASTE);
    CHECK(keymap_lookup(&k, TE_KEY_CHAR, 'c', cs) == TE_ACT_NONE);
    CHECK(keymap_lookup(&k, TE_KEY_CHAR, 'c', TE_MOD_CTRL) == TE_ACT_NONE);
    CHECK(keymap_lookup(&k, TE_KEY_CHAR, 'w', cmd) == TE_ACT_CLOSE_PANE);
    CHECK(keymap_lookup(&k, TE_KEY_CHAR, 't', cmd) == TE_ACT_NEW_TAB);
    CHECK(keymap_lookup(&k, TE_KEY_CHAR, 'd', cmd) == TE_ACT_SPLIT_RIGHT);
    CHECK(keymap_lookup(&k, TE_KEY_CHAR, 'd', cmd | TE_MOD_SHIFT) == TE_ACT_SPLIT_DOWN);
    CHECK(keymap_lookup(&k, TE_KEY_CHAR, '1', cmd) == TE_ACT_GOTO_TAB_1);
    CHECK(keymap_lookup(&k, TE_KEY_CHAR, '9', cmd) == TE_ACT_GOTO_TAB_9);
    CHECK(keymap_lookup(&k, TE_KEY_CHAR, '1', TE_MOD_ALT) == TE_ACT_NONE);   /* Option+digit types characters */
    CHECK(keymap_lookup(&k, TE_KEY_CHAR, ']', cmd | TE_MOD_SHIFT) == TE_ACT_NEXT_TAB);
    CHECK(keymap_lookup(&k, TE_KEY_CHAR, ']', cmd) == TE_ACT_FOCUS_NEXT_PANE);
    CHECK(keymap_lookup(&k, TE_KEY_UP, 0, cmd) == TE_ACT_PREV_PROMPT);
    CHECK(keymap_lookup(&k, TE_KEY_LEFT, 0, cmd | TE_MOD_CTRL) == TE_ACT_RESIZE_LEFT);
    CHECK(keymap_lookup(&k, TE_KEY_CHAR, 'b', cmd | TE_MOD_ALT) == TE_ACT_SEND_BREAK);
    CHECK(keymap_lookup(&k, TE_KEY_INSERT, 0, TE_MOD_SHIFT) == TE_ACT_NONE);   /* no primary selection */
    CHECK(keymap_describe(&k, TE_ACT_CLOSE_PANE, d, sizeof d) == TE_OK && strcmp(d, "Cmd+W") == 0);
    CHECK(keymap_describe(&k, TE_ACT_QUICK_CONNECT, d, sizeof d) == TE_OK && strcmp(d, "Shift+Cmd+N") == 0);
    CHECK(keymap_describe(&k, TE_ACT_PASTE_PRIMARY, d, sizeof d) == TE_ERR_NOTFOUND && d[0] == '\0');
#else
    CHECK(keymap_lookup(&k, TE_KEY_CHAR, 'c', cs) == TE_ACT_COPY);
    CHECK(keymap_lookup(&k, TE_KEY_CHAR, 'C', cs) == TE_ACT_COPY);
    CHECK(keymap_lookup(&k, TE_KEY_CHAR, 'c', TE_MOD_CTRL) == TE_ACT_NONE);   /* plain Ctrl+C goes to the shell */
    CHECK(keymap_lookup(&k, TE_KEY_CHAR, '1', TE_MOD_ALT) == TE_ACT_GOTO_TAB_1);
    CHECK(keymap_describe(&k, TE_ACT_CLOSE_PANE, d, sizeof d) == TE_OK && strcmp(d, "Ctrl+Shift+W") == 0);
    CHECK(keymap_describe(&k, TE_ACT_SEND_BREAK, d, sizeof d) == TE_OK && strcmp(d, "Ctrl+Shift+Pause") == 0);
#endif
    CHECK(keymap_lookup(&k, TE_KEY_PAGE_UP, 0, TE_MOD_SHIFT) == TE_ACT_SCROLL_PAGE_UP);
    /* Mac spellings of the modifiers work on every platform. */
    CHECK(keymap_set(&k, "new_tab", "cmd+shift+t, command+t") == TE_OK);
    CHECK(keymap_lookup(&k, TE_KEY_CHAR, 't', TE_MOD_SUPER | TE_MOD_SHIFT) == TE_ACT_NEW_TAB);
    CHECK(keymap_lookup(&k, TE_KEY_CHAR, 't', TE_MOD_SUPER) == TE_ACT_NEW_TAB);
    CHECK(keymap_set(&k, "prev_prompt", "option+up, opt+shift+up") == TE_OK);
    CHECK(keymap_lookup(&k, TE_KEY_UP, 0, TE_MOD_ALT) == TE_ACT_PREV_PROMPT);
    CHECK(keymap_lookup(&k, TE_KEY_UP, 0, TE_MOD_ALT | TE_MOD_SHIFT) == TE_ACT_PREV_PROMPT);
    CHECK(keymap_set(&k, "search", "ctrl+space") == TE_OK);
    CHECK(keymap_describe(&k, TE_ACT_SEARCH, d, sizeof d) == TE_OK && strcmp(d, "Ctrl+Space") == 0);
    CHECK(keymap_describe(&k, TE_ACT_SEARCH, d, 4) == TE_ERR_TRUNC && d[0] == '\0');   /* never a cut-off hint */
    CHECK(keymap_describe(&k, TE_ACT_NONE, d, sizeof d) == TE_ERR_INVAL);
    CHECK(keymap_set(&k, "copy", "ctrl+insert, alt+c") == TE_OK);
    CHECK(keymap_lookup(&k, TE_KEY_INSERT, 0, TE_MOD_CTRL) == TE_ACT_COPY);
    CHECK(keymap_lookup(&k, TE_KEY_CHAR, 'c', TE_MOD_ALT) == TE_ACT_COPY);
    CHECK(keymap_lookup(&k, TE_KEY_CHAR, 'c', cs) == TE_ACT_NONE);
    CHECK(keymap_set(&k, "zoom_in", "ctrl++") == TE_OK);
    CHECK(keymap_lookup(&k, TE_KEY_CHAR, '+', TE_MOD_CTRL) == TE_ACT_ZOOM_IN);
    CHECK(keymap_set(&k, "search", "none") == TE_OK);
    CHECK(keymap_lookup(&k, TE_KEY_CHAR, 'f', cs) == TE_ACT_NONE);
    CHECK(keymap_set(&k, "search", "ctrl+hyper+f") == TE_ERR_PARSE);
    CHECK(keymap_set(&k, "search", "a, b, c") == TE_ERR_PARSE);
    CHECK(keymap_set(&k, "fly", "ctrl+f") == TE_ERR_NOTFOUND);
    CHECK(keymap_set(&k, "scroll_top", "shift+F1") == TE_OK);
    CHECK(keymap_lookup(&k, TE_KEY_F1, 0, TE_MOD_SHIFT) == TE_ACT_SCROLL_TOP);
}

/* ------------------------------------------------------------------------- */
/* Phase 4 */

static void test_kitty_keyboard(void)
{
    te_term *t = mk_term(20, 3, 0);
    if (t == nullptr) return;
    char out[64];
    size_t len = 0;
    feed(t, "\x1b[>1u");
    CHECK(term_modes(t)->kitty_flags == 1);
    feed(t, "\x1b[?u");
    const uint8_t *o = term_output(t, &len);
    CHECK(len == 5 && memcmp(o, "\x1b[?1u", 5) == 0);
    term_output_consume(t, len);
    const te_modes *m = term_modes(t);
    size_t n = keys_encode_ev(TE_KEY_ESCAPE, 0, 0, TE_KEYEV_PRESS, m, out, sizeof out);
    CHECK(n == 5 && memcmp(out, "\x1b[27u", 5) == 0);
    n = keys_encode_ev(TE_KEY_CHAR, 'c', TE_MOD_CTRL, TE_KEYEV_PRESS, m, out, sizeof out);
    CHECK(n > 0 && strcmp(out, "\x1b[99;5u") == 0);
    CHECK(keys_encode_ev(TE_KEY_CHAR, 'a', 0, TE_KEYEV_PRESS, m, out, sizeof out) == 0);       /* text */
    CHECK(keys_encode_ev(TE_KEY_CHAR, 'a', TE_MOD_SHIFT, TE_KEYEV_PRESS, m, out, sizeof out) == 0);
    n = keys_encode_ev(TE_KEY_ENTER, 0, 0, TE_KEYEV_PRESS, m, out, sizeof out);
    CHECK(n == 1 && out[0] == '\r');                                     /* Enter stays legacy */
    n = keys_encode_ev(TE_KEY_ENTER, 0, TE_MOD_CTRL, TE_KEYEV_PRESS, m, out, sizeof out);
    CHECK(n > 0 && strcmp(out, "\x1b[13;5u") == 0);
    n = keys_encode_ev(TE_KEY_UP, 0, 0, TE_KEYEV_PRESS, m, out, sizeof out);
    CHECK(n == 3 && memcmp(out, "\x1b[A", 3) == 0);
    n = keys_encode_ev(TE_KEY_UP, 0, TE_MOD_SHIFT, TE_KEYEV_PRESS, m, out, sizeof out);
    CHECK(n > 0 && strcmp(out, "\x1b[1;2A") == 0);
    CHECK(keys_encode_ev(TE_KEY_ESCAPE, 0, 0, TE_KEYEV_RELEASE, m, out, sizeof out) == 0);    /* no flag 2 */

    feed(t, "\x1b[=10;2u");                                              /* OR in 2 and 8 */
    CHECK(term_modes(t)->kitty_flags == 11);
    m = term_modes(t);
    n = keys_encode_ev(TE_KEY_CHAR, 'a', 0, TE_KEYEV_PRESS, m, out, sizeof out);
    CHECK(n > 0 && strcmp(out, "\x1b[97u") == 0);                         /* all keys as escapes */
    n = keys_encode_ev(TE_KEY_CHAR, 'a', 0, TE_KEYEV_RELEASE, m, out, sizeof out);
    CHECK(n > 0 && strcmp(out, "\x1b[97;1:3u") == 0);
    n = keys_encode_ev(TE_KEY_UP, 0, 0, TE_KEYEV_REPEAT, m, out, sizeof out);
    CHECK(n > 0 && strcmp(out, "\x1b[1;1:2A") == 0);
    n = keys_encode_ev(TE_KEY_F5, 0, TE_MOD_ALT, TE_KEYEV_PRESS, m, out, sizeof out);
    CHECK(n > 0 && strcmp(out, "\x1b[15;3~") == 0);

    feed(t, "\x1b[?1049h");                                              /* separate stack per screen */
    CHECK(term_modes(t)->kitty_flags == 0);
    feed(t, "\x1b[>31u");                                                /* unsupported bits masked */
    CHECK(term_modes(t)->kitty_flags == (31u & TE_KITTY_SUPPORTED));
    feed(t, "\x1b[?1049l");
    CHECK(term_modes(t)->kitty_flags == 11);
    for (int i = 0; i < 20; i++) feed(t, "\x1b[>1u");                    /* overflowing the stack is safe */
    feed(t, "\x1b[<100u");
    CHECK(term_modes(t)->kitty_flags == 0);
    term_destroy(&t);
}

static char g_ntitle[160];
static char g_nbody[300];
static int g_ncount;
static void cb_notify(void *u, const char *title, const char *body)
{
    (void)u;
    (void)str_copy(g_ntitle, sizeof g_ntitle, title);
    (void)str_copy(g_nbody, sizeof g_nbody, body);
    g_ncount++;
}

static void test_notifications(void)
{
    te_theme th;
    theme_defaults(&th);
    te_term_host host = { .notify = cb_notify };
    te_term *t = nullptr;
    CHECK(term_create(&t, 20, 3, 0, &th, &host) == TE_OK);
    if (t == nullptr) return;
    feed(t, "\x1b]9;Build finished\x07");
    CHECK(g_ncount == 1 && strcmp(g_nbody, "Build finished") == 0 && g_ntitle[0] == '\0');
    feed(t, "\x1b]9;4;1;50\x07");                                         /* ConEmu progress: ignored */
    CHECK(g_ncount == 1);
    feed(t, "\x1b]777;notify;Backup;done\x01 ok\x07");
    CHECK(g_ncount == 2 && strcmp(g_ntitle, "Backup") == 0 && strcmp(g_nbody, "done ok") == 0);
    feed(t, "\x1b]99;i=1:p=body;the body\x1b\\");
    CHECK(g_ncount == 3 && strcmp(g_nbody, "the body") == 0);
    feed(t, "\x1b]99;;Just a title\x1b\\");
    CHECK(g_ncount == 4 && strcmp(g_ntitle, "Just a title") == 0);
    feed(t, "\x1b]777;bogus;x;y\x07");
    CHECK(g_ncount == 4);
    term_destroy(&t);
}

static void test_layout(void)
{
    char d[20];                                                          /* stand-in panes (never dereferenced) */
    struct te_pane *p[20];
    for (int i = 0; i < 20; i++) p[i] = (struct te_pane *)(void *)&d[i];
    te_layout l;
    layout_init(&l, 2);
    CHECK(layout_add_tab(&l, p[0]) == TE_OK);
    CHECK(layout_split(&l, p[1], true) == TE_OK);                         /* p0 | p1 */
    CHECK(layout_focus(&l) == p[1]);
    CHECK(layout_split(&l, p[2], false) == TE_OK);                        /* p0 | (p1 / p2) */
    layout_compute(&l, 0, 0, 1002, 802);
    int r0[4], r1[4], r2[4];
    CHECK(layout_pane_rect(&l, p[0], r0) && layout_pane_rect(&l, p[1], r1) && layout_pane_rect(&l, p[2], r2));
    CHECK(r0[0] == 0 && r0[2] == 500 && r0[3] == 802);
    CHECK(r1[0] == 502 && r1[1] == 0 && r1[2] == 500 && r1[3] == 400);
    CHECK(r2[0] == 502 && r2[1] == 402 && r2[3] == 400);
    CHECK(layout_pane_at(&l, 10, 10) == p[0] && layout_pane_at(&l, 700, 600) == p[2]);
    CHECK(layout_pane_at(&l, 501, 10) == nullptr);                        /* on the divider */
    int dv[8][4];
    CHECK(layout_dividers(&l, dv, 8) == 2);
    te_node *split = layout_divider_at(&l, 501, 300, 2);
    CHECK(split != nullptr && split->side_by_side);
    layout_drag(split, 251, 300);
    layout_compute(&l, 0, 0, 1002, 802);
    CHECK(layout_pane_rect(&l, p[0], r0) && r0[2] == 250);
    layout_drag(split, 0, 0);
    CHECK(split->ratio >= TE_LAYOUT_MIN_RATIO);                           /* clamped */

    layout_set_focus(&l, p[2]);
    CHECK(layout_resize(&l, 0, 1, 0.05f));                                 /* the horizontal split moves */
    layout_cycle_focus(&l, 1);
    CHECK(layout_focus(&l) == p[0]);
    layout_toggle_zoom(&l);
    layout_compute(&l, 0, 0, 1002, 802);
    struct te_pane *vis[16];
    CHECK(layout_visible_panes(&l, vis, 16) == 1 && vis[0] == p[0]);
    CHECK(layout_pane_rect(&l, p[0], r0) && r0[2] == 1002);
    CHECK(layout_dividers(&l, dv, 8) == 0);
    layout_toggle_zoom(&l);

    CHECK(layout_add_tab(&l, p[3]) == TE_OK);                              /* second tab */
    CHECK(l.ntabs == 2 && l.active == 1 && layout_tab_of(&l, p[1]) == 0);
    layout_cycle_tab(&l, 1);
    CHECK(l.active == 0);
    CHECK(!layout_remove(&l, p[1]));                                       /* sibling p2 takes its place */
    CHECK(layout_count_panes(&l) == 3 && l.tabs[0].npanes == 2);
    layout_compute(&l, 0, 0, 1002, 802);
    CHECK(layout_pane_rect(&l, p[2], r2) && r2[1] == 0 && r2[3] == 802);
    CHECK(!layout_remove(&l, p[3]));                                       /* tab 2 disappears */
    CHECK(l.ntabs == 1);
    for (int i = 4; i < 4 + TE_MAX_PANES_PER_TAB - 2; i++) CHECK(layout_split(&l, p[i], i % 2 == 0) == TE_OK);
    CHECK(layout_split(&l, p[19], true) == TE_ERR_RANGE);                  /* per-tab limit */
    struct te_pane *all[32];
    size_t n = layout_all_panes(&l, all, 32);
    for (size_t i = 0; i < n; i++) {
        bool last = layout_remove(&l, all[i]);
        CHECK(last == (i + 1 == n));
    }
    CHECK(l.ntabs == 0 && layout_focus(&l) == nullptr);
    layout_destroy(&l);
}

typedef struct { int n; float minx, miny, maxx, maxy, alpha; } box_acc;
static void box_cb(void *u, float x, float y, float w, float h, float a)
{
    box_acc *b = u;
    b->n++;
    if (x < b->minx) b->minx = x;
    if (y < b->miny) b->miny = y;
    if (x + w > b->maxx) b->maxx = x + w;
    if (y + h > b->maxy) b->maxy = y + h;
    b->alpha = a;
}

static void test_boxdraw(void)
{
    box_acc b = { 0, 1e9f, 1e9f, -1e9f, -1e9f, 0 };
    CHECK(boxdraw(0x2500, 10, 20, 9, 20, 1, box_cb, &b) && b.n == 2);       /* ─: two arms */
    CHECK(b.minx == 10 && b.maxx == 19);                                   /* reaches both edges */
    b = (box_acc){ 0, 1e9f, 1e9f, -1e9f, -1e9f, 0 };
    CHECK(boxdraw(0x253C, 0, 0, 9, 20, 1, box_cb, &b) && b.n == 4);         /* ┼ */
    CHECK(b.miny == 0 && b.maxy == 20);
    b = (box_acc){ 0, 1e9f, 1e9f, -1e9f, -1e9f, 0 };
    CHECK(boxdraw(0x2550, 0, 0, 9, 20, 1, box_cb, &b) && b.n == 4);         /* ═: double */
    b = (box_acc){ 0, 1e9f, 1e9f, -1e9f, -1e9f, 0 };
    CHECK(boxdraw(0x2591, 0, 0, 9, 20, 1, box_cb, &b) && b.alpha == 0.25f); /* light shade */
    CHECK(!boxdraw_handles(0x2571) && !boxdraw(0x2571, 0, 0, 9, 20, 1, box_cb, &b));   /* diagonal: font */
    CHECK(!boxdraw_handles('A'));
    /* Every handled character stays inside its cell. */
    for (uint32_t cp = 0x2500; cp <= 0x259F; cp++) {
        if (!boxdraw_handles(cp)) continue;
        b = (box_acc){ 0, 1e9f, 1e9f, -1e9f, -1e9f, 0 };
        CHECK(boxdraw(cp, 100, 200, 10, 22, 2, box_cb, &b) && b.n > 0);
        CHECK(b.minx >= 100 - 0.01f && b.maxx <= 110.01f && b.miny >= 200 - 0.01f && b.maxy <= 222.01f);
    }
}

static void test_sixel(void)
{
    vt_params p = { 0 };
    uint8_t *px = nullptr;
    uint32_t w = 0, h = 0;
    static const char red[] = "#1;2;100;0;0#1!10~-!10~";                 /* 10 wide, two bands */
    CHECK(sixel_decode(&p, red, sizeof red - 1, 0x000000, &px, &w, &h) == TE_OK);
    CHECK(w == 10 && h == 12 && px != nullptr && px[0] == 255 && px[1] == 0 && px[3] == 255);
    TE_FREE(px);
    p.n = 2;
    p.v[1] = 1;                                                          /* transparent background */
    static const char sparse[] = "\"1;1;8;6#2;2;0;100;0#2@";               /* raster 8x6, one pixel */
    CHECK(sixel_decode(&p, sparse, sizeof sparse - 1, 0x112233, &px, &w, &h) == TE_OK);
    CHECK(w == 8 && h == 6 && px[1] == 255 && px[4 * 1 + 3] == 0);         /* (1,0) unset: transparent */
    TE_FREE(px);
    static const char huge[] = "#0!999999~-!999999~";
    CHECK(sixel_decode(&p, huge, sizeof huge - 1, 0, &px, &w, &h) == TE_OK);
    CHECK(w == TE_SIXEL_MAX_DIM && h == 12);                                /* clamped, not overflowed */
    TE_FREE(px);
    static const char nothing[] = "$$$---#5;2;1;2;3 \r\n";            /* no sixel data at all */
    CHECK(sixel_decode(&p, nothing, sizeof nothing - 1, 0, &px, &w, &h) == TE_ERR_INVAL && px == nullptr);
    TE_FREE(px);
    static const char hls[] = "#3;1;120;50;100#3~";                      /* HLS 120 = red in sixel */
    CHECK(sixel_decode(&p, hls, sizeof hls - 1, 0, &px, &w, &h) == TE_OK && px[0] > 200 && px[1] < 40);
    TE_FREE(px);

    /* Through the terminal: placed at the cursor, cursor moves below it. */
    te_theme th;
    theme_defaults(&th);
    te_term *t = nullptr;
    CHECK(term_create(&t, 40, 10, 20, &th, nullptr) == TE_OK);
    if (t == nullptr) return;
    feed(t, "ab\r\n\x1bPq#1;2;100;0;0#1!20~-!20~-!20~-!20~\x1b\\");      /* 20x24 px */
    CHECK(term_image_count(t) == 1);
    const te_image *im = term_image_at(t, 0);
    CHECK(im != nullptr && im->placed && im->cols == 2 && im->rows == 2);   /* 10x20 default cells */
    uint16_t x = 9, y = 9;
    term_cursor(t, &x, &y);
    CHECK(x == 0 && y == 3);
    for (int i = 0; i < 40; i++) feed(t, "\r\nscroll");                     /* scrolled out of history */
    CHECK(term_image_count(t) == 0);
    term_destroy(&t);
}

static void test_kitty_graphics(void)
{
    te_theme th;
    theme_defaults(&th);
    te_term *t = nullptr;
    CHECK(term_create(&t, 40, 10, 20, &th, nullptr) == TE_OK);
    if (t == nullptr) return;
    size_t len = 0;
    /* 2x1 RGBA: red, half-transparent green. */
    feed(t, "\x1b_Ga=T,f=32,s=2,v=1,i=5;/wAA/wD/AIA=\x1b\\");
    const uint8_t *o = term_output(t, &len);
    CHECK(len > 0 && memcmp(o, "\x1b_Gi=5;OK\x1b\\", len) == 0);
    term_output_consume(t, len);
    CHECK(term_image_count(t) == 1);
    const te_image *im = term_image_at(t, 0);
    CHECK(im->id == 5 && im->w == 2 && im->h == 1 && im->placed);
    CHECK(im->rgba[0] == 255 && im->rgba[3] == 255);                       /* opaque red */
    CHECK(im->rgba[4 + 1] == 128 && im->rgba[4 + 3] == 128);              /* green 255 at alpha 128, premultiplied */
    uint16_t x = 0, y = 0;
    term_cursor(t, &x, &y);
    CHECK(x == 1 && y == 0);                                              /* cursor after the image */

    /* Chunked transfer, transmit only, then place. */
    feed(t, "\x1b_Ga=t,f=24,s=1,v=1,i=7,m=1;AA\x1b\\");
    (void)term_output(t, &len);
    CHECK(len == 0);                                                      /* waiting for the rest */
    feed(t, "\x1b_Gm=0;AA\x1b\\");
    o = term_output(t, &len);
    CHECK(len > 0 && memcmp(o, "\x1b_Gi=7;OK\x1b\\", len) == 0);
    term_output_consume(t, len);
    CHECK(term_image_count(t) == 2 && !term_image_at(t, 1)->placed);
    feed(t, "\x1b_Ga=p,i=7,q=1\x1b\\");
    (void)term_output(t, &len);
    CHECK(len == 0 && term_image_at(t, 1)->placed);                        /* q=1: no OK */

    /* Errors. */
    feed(t, "\x1b_Ga=T,f=32,s=2,v=2,i=9;AAAA\x1b\\");                       /* too little data */
    o = term_output(t, &len);
    CHECK(len > 10 && memcmp(o, "\x1b_Gi=9;ENODATA", 14) == 0);
    term_output_consume(t, len);
    feed(t, "\x1b_Ga=T,f=100,s=1,v=1,i=9;AAAA\x1b\\");                      /* PNG */
    o = term_output(t, &len);
    CHECK(len > 10 && memcmp(o, "\x1b_Gi=9;EINVAL", 13) == 0);
    term_output_consume(t, len);
    feed(t, "\x1b_Ga=T,f=32,s=99999,v=99999,i=9;AAAA\x1b\\");              /* absurd size */
    o = term_output(t, &len);
    CHECK(len > 10 && memcmp(o, "\x1b_Gi=9;EINVAL", 13) == 0);
    term_output_consume(t, len);
    feed(t, "\x1b_Ga=p,i=404,q=2\x1b\\");                                   /* q=2: errors silent too */
    (void)term_output(t, &len);
    CHECK(len == 0);
    feed(t, "\x1b_Ga=d,d=i,i=5\x1b\\");
    CHECK(term_image_count(t) == 1);
    feed(t, "\x1b_Ga=d\x1b\\");
    CHECK(term_image_count(t) == 0);

    /* Many images: count and memory stay bounded. */
    for (int i = 0; i < 300; i++) {
        char cmd[96];
        (void)str_fmt(cmd, sizeof cmd, "\x1b_Ga=t,f=24,s=1,v=1,i=%d,q=2;AAAA\x1b\\", 1000 + i);
        feed(t, cmd);
    }
    CHECK(term_image_count(t) <= TE_MAX_IMAGES);
    feed(t, "\x1b" "c");                                                  /* RIS clears images */
    CHECK(term_image_count(t) == 0);
    term_destroy(&t);
}

#include "test_sessions.inc"
#include "test_remote.inc"

int main(void)
{
    test_utf8();
    test_str();
    test_basic_print();
    test_sgr();
    test_scroll_and_resize();
    test_osc_and_replies();
    test_hostile_input();
    test_keys();
    test_ini();
    test_config_transparency();
    test_highlight();
    test_click();
    test_reflow();
    test_osc8_and_abs();
    test_selection();
    test_mouse();
    test_urls();
    test_search();
    test_keymap();
    test_kitty_keyboard();
    test_notifications();
    test_layout();
    test_boxdraw();
    test_sixel();
    test_kitty_graphics();
    test_sessions_all();
    test_remote_all();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
