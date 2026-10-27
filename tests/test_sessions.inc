/*
 * Tests for the session manager: validation, the on-disk store (including a
 * planted symlink), command building, the Telnet state machine, the UI
 * widgets, and the rule that right-click only ever opens a menu.
 * Included by test_core.c.
 */
#include <ftw.h>

#include "sessions/launch.h"
#include "sessions/session.h"
#include "sessions/store.h"
#include "sessions/telnet.h"
#include "ui/form.h"
#include "ui/menu.h"
#include "ui/paint.h"
#include "ui/sidebar.h"

static int rm_cb(const char *path, const struct stat *sb, int flag, struct FTW *ftw)
{
    (void)sb;
    (void)flag;
    (void)ftw;
    return remove(path);
}

static void rm_tree(const char *path)
{
    (void)nftw(path, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
}

static bool argv_has(const te_launch *l, const char *s)
{
    for (size_t i = 0; i < l->argc; i++) {
        if (strcmp(l->argv[i], s) == 0) return true;
    }
    return false;
}

static void test_session_validation(void)
{
    CHECK(session_name_valid("core-sw1"));
    CHECK(session_name_valid("R\xc3\xb6uter \xe2\x9c\x93"));
    CHECK(!session_name_valid(""));
    CHECK(!session_name_valid(".hidden"));
    CHECK(!session_name_valid("-rf"));
    CHECK(!session_name_valid("a/b"));
    CHECK(!session_name_valid("a\\b"));
    CHECK(!session_name_valid("trailing "));
    CHECK(!session_name_valid("_folder"));
    CHECK(!session_name_valid("bell\x07"));
    CHECK(!session_name_valid("bad\xff"));
    CHECK(!session_name_valid("cut\xc3"));
    CHECK(!session_name_valid("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));   /* 65 */

    CHECK(session_folder_valid(""));
    CHECK(session_folder_valid("Datacenter/Core"));
    CHECK(!session_folder_valid("A//B"));
    CHECK(!session_folder_valid("A/../B"));
    CHECK(!session_folder_valid("/abs"));
    CHECK(!session_folder_valid("a/b/c/d/e/f/g/h/i"));   /* 9 levels */

    CHECK(session_host_valid("10.0.0.1"));
    CHECK(session_host_valid("fe80::1%eth0"));
    CHECK(session_host_valid("core-sw1.example.net"));
    CHECK(!session_host_valid("-oProxyCommand=sh"));
    CHECK(!session_host_valid("a b"));
    CHECK(!session_host_valid("a;b"));
    CHECK(!session_host_valid(""));

    CHECK(session_user_valid("admin"));
    CHECK(session_user_valid("user.name-1_2"));
    CHECK(session_user_valid(""));
    CHECK(!session_user_valid("-l"));
    CHECK(!session_user_valid("a@b"));
    CHECK(!session_user_valid("a b"));
    CHECK(!session_user_valid("a;id"));       /* shell metacharacters rejected */
    CHECK(!session_user_valid("a|id"));
    CHECK(!session_user_valid("a&id"));
    CHECK(!session_user_valid("a`id`"));
    CHECK(!session_user_valid("a$x"));
    CHECK(!session_user_valid("a\\x"));
    CHECK(!session_user_valid("a'x"));
    CHECK(!session_user_valid("a\"x"));

#ifdef _WIN32
    CHECK(session_device_valid("COM3") && session_device_valid("com12") && session_device_valid("COM256"));
    CHECK(!session_device_valid("COM0") && !session_device_valid("COM257") && !session_device_valid("COM03"));
    CHECK(!session_device_valid("COM3 ") && !session_device_valid("COM") && !session_device_valid("LPT1"));
    CHECK(!session_device_valid("\\\\.\\COM3") && !session_device_valid("/dev/ttyUSB0"));
#else
    CHECK(session_device_valid("/dev/ttyUSB0"));
    CHECK(!session_device_valid("/dev/../etc/shadow"));
    CHECK(!session_device_valid("/tmp/x"));
    CHECK(!session_device_valid("/dev/"));
#endif

    te_serial_cfg c = { .baud = 9600, .data_bits = 8, .stop_bits = 1 };
    CHECK(serial_parse("115200,8N1,none", &c) && c.baud == 115200 && c.data_bits == 8 && c.parity == TE_PARITY_NONE &&
          c.stop_bits == 1 && c.flow == TE_FLOW_NONE);
    CHECK(serial_parse("250000,7E2,rtscts", &c) && c.baud == 250000 && c.data_bits == 7 && c.parity == TE_PARITY_EVEN &&
          c.stop_bits == 2 && c.flow == TE_FLOW_RTSCTS);
    CHECK(!serial_parse("8N1", &c));
    CHECK(!serial_parse("115200,9N1", &c));
    CHECK(!serial_parse("115200,8N1,none,extra", &c));
    CHECK(!serial_parse("20000000", &c));
    CHECK(c.baud == 250000);   /* a failed parse changes nothing */
    char spec[64] = { 0 };
    CHECK(serial_format(&c, spec, sizeof spec) == TE_OK && strcmp(spec, "250000,7E2,rtscts") == 0);

    uint8_t bytes[16] = { 0 };
    size_t n = 0;
    CHECK(str_unescape("\\0", bytes, sizeof bytes, &n) && n == 1 && bytes[0] == 0);
    CHECK(str_unescape(" \\x08", bytes, sizeof bytes, &n) && n == 2 && bytes[0] == ' ' && bytes[1] == 8);
    CHECK(!str_unescape("\\q", bytes, sizeof bytes, &n));
    CHECK(!str_unescape("\\x4", bytes, sizeof bytes, &n));
    CHECK(!str_unescape("abc\\", bytes, sizeof bytes, &n));
    CHECK(!str_unescape("0123456789abcdefX", bytes, sizeof bytes, &n));   /* 17 bytes */
}

static void test_session_store(void)
{
    char root[512];
    (void)tmp_template(root, sizeof root, "te-store");
    if (mkdtemp(root) == nullptr) {
        CHECK(false);
        return;
    }
    char sroot[256] = { 0 };
    CHECK(str_fmt(sroot, sizeof sroot, "%s/sessions", root) == TE_OK);
    te_store st;
    CHECK(store_open(&st, sroot) == TE_OK);
    CHECK(st.len == 0);

    CHECK(store_create_folder(&st, "", "Datacenter") == TE_OK);
    CHECK(store_create_folder(&st, "Datacenter", "Core") == TE_OK);
    CHECK(store_create_folder(&st, "", "Lab") == TE_OK);
    CHECK(store_create_folder(&st, "", "Lab") == TE_ERR_EXISTS);

    te_session s;
    session_defaults(&s, TE_SESS_SSH);
    (void)str_copy(s.name, sizeof s.name, "core-sw1");
    (void)str_copy(s.folder, sizeof s.folder, "Datacenter/Core");
    (void)str_copy(s.host, sizeof s.host, "10.0.0.1");
    s.port = 2222;
    CHECK(store_save_session(&st, &s, nullptr, nullptr) == TE_OK);
    CHECK(store_save_session(&st, &s, nullptr, nullptr) == TE_ERR_EXISTS);   /* no silent overwrite */

    te_session ser;
    session_defaults(&ser, TE_SESS_SERIAL);
    (void)str_copy(ser.name, sizeof ser.name, "console");
    (void)str_copy(ser.serial.device, sizeof ser.serial.device, TE_SERIAL_EXAMPLE);
    ser.serial.baud = 115200;
    CHECK(store_save_session(&st, &ser, nullptr, nullptr) == TE_OK);

    /* Pre-order with folders first: Datacenter, Core, core-sw1, Lab, console. */
    CHECK(st.len == 5);
    if (st.len == 5) {
        CHECK(st.nodes[0].folder && strcmp(st.nodes[0].name, "Datacenter") == 0);
        CHECK(st.nodes[1].folder && strcmp(st.nodes[1].path, "Datacenter/Core") == 0 && st.nodes[1].parent == 0);
        CHECK(!st.nodes[2].folder && strcmp(st.nodes[2].name, "core-sw1") == 0 && st.nodes[2].depth == 2);
        CHECK(st.nodes[3].folder && strcmp(st.nodes[3].name, "Lab") == 0);
        CHECK(!st.nodes[4].folder && strcmp(st.nodes[4].name, "console") == 0);
    }
    int si = store_find_session(&st, "Datacenter/Core", "core-sw1");
    CHECK(si >= 0 && st.nodes[si].sess != nullptr && st.nodes[si].sess->port == 2222 &&
          strcmp(st.nodes[si].sess->host, "10.0.0.1") == 0);
    int ci = store_find_session(&st, "", "console");
    CHECK(ci >= 0 && st.nodes[ci].sess != nullptr && st.nodes[ci].sess->serial.baud == 115200 &&
          st.nodes[ci].sess->type == TE_SESS_SERIAL);

    /* Folder defaults are inherited. */
    te_folder_defaults d;
    folder_defaults_init(&d);
    d.color = TE_TAG_RED;
    d.anti_idle = 60;
    (void)str_copy(d.user, sizeof d.user, "netops");
    CHECK(store_save_folder_defaults(&st, "Datacenter", &d) == TE_OK);
    si = store_find_session(&st, "Datacenter/Core", "core-sw1");
    if (si >= 0) {
        te_session_eff eff;
        store_effective(&st, st.nodes[si].sess, &eff);
        CHECK(eff.color == TE_TAG_RED && eff.anti_idle == 60 && strcmp(eff.user, "netops") == 0);
    }

    /* Rename, move, keep folders open. */
    int fi = store_find_folder(&st, "Datacenter");
    if (fi >= 0) st.nodes[fi].expanded = true;
    CHECK(store_rename_folder(&st, "Datacenter", "DC") == TE_OK);
    fi = store_find_folder(&st, "DC");
    CHECK(fi >= 0 && st.nodes[fi].expanded);
    CHECK(store_find_session(&st, "DC/Core", "core-sw1") >= 0);
    CHECK(store_move_session(&st, "DC/Core", "core-sw1", "Lab") == TE_OK);
    CHECK(store_find_session(&st, "Lab", "core-sw1") >= 0 && store_find_session(&st, "DC/Core", "core-sw1") < 0);

    /* Edit with a rename replaces the old file. */
    si = store_find_session(&st, "Lab", "core-sw1");
    if (si >= 0) {
        te_session e = *st.nodes[si].sess;
        (void)str_copy(e.name, sizeof e.name, "core-sw2");
        CHECK(store_save_session(&st, &e, "Lab", "core-sw1") == TE_OK);
        CHECK(store_find_session(&st, "Lab", "core-sw2") >= 0 && store_find_session(&st, "Lab", "core-sw1") < 0);
    }

#ifndef _WIN32
    /* A planted symlink is never listed or followed. */
    char link[300] = { 0 };
    CHECK(str_fmt(link, sizeof link, "%s/evil", sroot) == TE_OK);
    CHECK(symlink("/tmp", link) == 0);
    CHECK(store_reload(&st) == TE_OK);
    CHECK(store_find_folder(&st, "evil") < 0);
    te_session ev = s;
    (void)str_copy(ev.folder, sizeof ev.folder, "evil");
    (void)str_copy(ev.name, sizeof ev.name, "escape");
    CHECK(store_save_session(&st, &ev, nullptr, nullptr) != TE_OK);
    CHECK(access("/tmp/escape.ini", F_OK) != 0);
#endif

    /* Deleting a folder with someone else's file in it is refused, and nothing is removed. */
    char foreign[300] = { 0 };
    CHECK(str_fmt(foreign, sizeof foreign, "%s/Lab/notes.txt", sroot) == TE_OK);
    FILE *fp = fopen(foreign, "w");
    if (fp != nullptr) fclose(fp);
    CHECK(store_delete_folder(&st, "Lab") == TE_ERR_EXISTS);
    CHECK(store_find_session(&st, "Lab", "core-sw2") >= 0);
    unlink(foreign);
    CHECK(store_delete_folder(&st, "Lab") == TE_OK);
    CHECK(store_find_folder(&st, "Lab") < 0);
    CHECK(store_delete_session(&st, "", "console") == TE_OK);
    CHECK(store_find_session(&st, "", "console") < 0);

    /* Session files are private. */
    CHECK(store_save_session(&st, &ser, nullptr, nullptr) == TE_OK);
    char file[300] = { 0 };
    struct stat sb = { 0 };
    CHECK(str_fmt(file, sizeof file, "%s/console.ini", sroot) == TE_OK);
    CHECK(stat(file, &sb) == 0 && (sb.st_mode & 0077) == 0);

#ifndef _WIN32
    /* A session file writable by group/others is untrusted (a "local" session runs
     * a shell command), so it is refused on load, not silently honoured. */
    CHECK(store_find_session(&st, "", "console") >= 0);
    CHECK(chmod(file, 0666) == 0);
    CHECK(store_reload(&st) == TE_OK);
    CHECK(store_find_session(&st, "", "console") < 0);
    CHECK(chmod(file, 0600) == 0);
    CHECK(store_reload(&st) == TE_OK);
    CHECK(store_find_session(&st, "", "console") >= 0);
#endif

    store_close(&st);
    rm_tree(root);
}

static void test_launch_argv(void)
{
    te_session s;
    session_defaults(&s, TE_SESS_SSH);
    (void)str_copy(s.name, sizeof s.name, "r1");
    (void)str_copy(s.host, sizeof s.host, "10.1.1.1");
    s.port = 2222;
    te_session_eff eff = { .color = TE_TAG_NONE, .anti_idle = 0 };
    (void)str_copy(eff.user, sizeof eff.user, "admin");
#ifdef _WIN32
    te_launch_opts o = { .self_exe = "C:/Program Files/FeTerm/feterm.exe", .control_dir = nullptr, .keepalive = 30 };
#else
    te_launch_opts o = { .self_exe = "/usr/bin/feterm", .control_dir = "/run/user/1000/feterm", .keepalive = 30 };
#endif
    te_launch l;
    CHECK(launch_build(&s, &eff, &o, &l) == TE_OK);
    CHECK(l.argc > 4 && strcmp(l.argv[0], "ssh") == 0 && l.argv[l.argc] == nullptr);
#ifdef _WIN32
    /* No connection sharing with Windows' OpenSSH. */
    CHECK(argv_has(&l, "ServerAliveInterval=30") && !argv_has(&l, "ControlMaster=auto") && argv_has(&l, "admin"));
#else
    CHECK(argv_has(&l, "ServerAliveInterval=30") && argv_has(&l, "ControlMaster=auto"));
    CHECK(argv_has(&l, "ControlPath=/run/user/1000/feterm/cm-%C") && argv_has(&l, "admin"));
#endif
    CHECK(l.argc >= 2 && strcmp(l.argv[l.argc - 2], "--") == 0 && strcmp(l.argv[l.argc - 1], "10.1.1.1") == 0);
    s.share_conn = false;
    CHECK(launch_build(&s, &eff, &o, &l) == TE_OK && !argv_has(&l, "ControlMaster=auto"));

    (void)str_copy(s.host, sizeof s.host, "-oProxyCommand=touch /tmp/pwned");
    CHECK(launch_build(&s, &eff, &o, &l) == TE_ERR_INVAL);

    te_session t;
    session_defaults(&t, TE_SESS_TELNET);
    (void)str_copy(t.name, sizeof t.name, "old");
    (void)str_copy(t.host, sizeof t.host, "192.0.2.7");
    CHECK(launch_build(&t, &eff, &o, &l) == TE_OK && l.argc == 5 && strcmp(l.argv[1], "--telnet-helper") == 0 &&
          strcmp(l.argv[3], "23") == 0);

    te_session c;
    session_defaults(&c, TE_SESS_SERIAL);
    (void)str_copy(c.name, sizeof c.name, "con");
    (void)str_copy(c.serial.device, sizeof c.serial.device, TE_SERIAL_EXAMPLE);
    c.serial.baud = 115200;
    CHECK(launch_build(&c, &eff, &o, &l) == TE_OK && l.argc == 4 && strcmp(l.argv[1], "--serial-helper") == 0 &&
          strcmp(l.argv[3], "115200,8N1,none") == 0);

    te_session loc;
    session_defaults(&loc, TE_SESS_LOCAL);
    (void)str_copy(loc.name, sizeof loc.name, "logs");
    CHECK(launch_build(&loc, &eff, &o, &l) == TE_OK && l.argc == 0);   /* login shell */
    (void)str_copy(loc.command, sizeof loc.command, "htop -d 10");
#ifdef _WIN32
    CHECK(launch_build(&loc, &eff, &o, &l) == TE_OK && l.argc == 4 && strcmp(l.argv[2], "/c") == 0 &&
          strcmp(l.argv[3], "htop -d 10") == 0);
#else
    CHECK(launch_build(&loc, &eff, &o, &l) == TE_OK && l.argc == 3 && strcmp(l.argv[0], "/bin/sh") == 0 &&
          strcmp(l.argv[2], "htop -d 10") == 0);
#endif
}

static void test_telnet_protocol(void)
{
    static te_telnet t;
    uint8_t tb[256];
    uint8_t nb[256];
    te_tbuf term = { .data = tb, .cap = sizeof tb };
    te_tbuf net = { .data = nb, .cap = sizeof nb };
    telnet_init(&t, "xterm-256color", 80, 24);
    telnet_start(&t, &net);
    static const uint8_t start[] = { TN_IAC, TN_DO, TN_OPT_SGA, TN_IAC, TN_WILL, TN_OPT_NAWS, TN_IAC, TN_WILL, TN_OPT_TTYPE };
    CHECK(net.len == sizeof start && memcmp(nb, start, sizeof start) == 0);
    net.len = 0;

    /* The server agrees to NAWS: no second WILL (no loop), just the size. */
    static const uint8_t do_naws[] = { TN_IAC, TN_DO, TN_OPT_NAWS };
    telnet_rx(&t, do_naws, sizeof do_naws, &term, &net);
    static const uint8_t naws[] = { TN_IAC, TN_SB, TN_OPT_NAWS, 0, 80, 0, 24, TN_IAC, TN_SE };
    CHECK(net.len == sizeof naws && memcmp(nb, naws, sizeof naws) == 0);
    net.len = 0;
    telnet_rx(&t, do_naws, sizeof do_naws, &term, &net);
    CHECK(net.len == 0);

    static const uint8_t will_echo[] = { TN_IAC, TN_WILL, TN_OPT_ECHO };
    telnet_rx(&t, will_echo, sizeof will_echo, &term, &net);
    CHECK(net.len == 3 && nb[1] == TN_DO && nb[2] == TN_OPT_ECHO && telnet_remote_echo(&t));
    net.len = 0;
    telnet_rx(&t, will_echo, sizeof will_echo, &term, &net);
    CHECK(net.len == 0);

    static const uint8_t do_unknown[] = { TN_IAC, TN_DO, 99, TN_IAC, TN_WILL, 98 };
    telnet_rx(&t, do_unknown, sizeof do_unknown, &term, &net);
    CHECK(net.len == 6 && nb[1] == TN_WONT && nb[2] == 99 && nb[4] == TN_DONT && nb[5] == 98);
    net.len = 0;

    static const uint8_t ttype[] = { TN_IAC, TN_DO, TN_OPT_TTYPE, TN_IAC, TN_SB, TN_OPT_TTYPE, 1, TN_IAC, TN_SE };
    telnet_rx(&t, ttype, sizeof ttype, &term, &net);
    static const uint8_t is[] = { TN_IAC, TN_SB, TN_OPT_TTYPE, 0, 'X', 'T', 'E', 'R', 'M', '-', '2', '5', '6',
                                  'C', 'O', 'L', 'O', 'R', TN_IAC, TN_SE };
    CHECK(net.len == sizeof is && memcmp(nb, is, sizeof is) == 0);
    net.len = 0;

    static const uint8_t data[] = { 'o', 'k', TN_IAC, TN_IAC, '\r', 0, 'x' };
    telnet_rx(&t, data, sizeof data, &term, &net);
    static const uint8_t want[] = { 'o', 'k', 0xFF, '\r', 'x' };
    CHECK(term.len == sizeof want && memcmp(tb, want, sizeof want) == 0);
    term.len = 0;

    /* A sub-negotiation that never ends is dropped without overflowing anything. */
    static uint8_t junk[1200];
    junk[0] = TN_IAC;
    junk[1] = TN_SB;
    for (size_t i = 2; i < sizeof junk; i++) junk[i] = 'A';
    telnet_rx(&t, junk, sizeof junk, &term, &net);
    static const uint8_t after[] = { TN_IAC, TN_SE, 'h', 'i' };
    telnet_rx(&t, after, sizeof after, &term, &net);
    CHECK(term.len == 2 && tb[0] == 'h' && tb[1] == 'i' && net.len == 0);
    term.len = 0;

    static const uint8_t keys[] = { 'a', 0xFF, '\r', 'b', '\r', '\n' };
    telnet_tx(&t, keys, sizeof keys, &net);
    static const uint8_t sent[] = { 'a', TN_IAC, TN_IAC, '\r', 0, 'b', '\r', '\n' };
    CHECK(net.len == sizeof sent && memcmp(nb, sent, sizeof sent) == 0);
    net.len = 0;

    telnet_resize(&t, 255, 50, &net);
    static const uint8_t big[] = { TN_IAC, TN_SB, TN_OPT_NAWS, 0, 255, 255, 0, 50, TN_IAC, TN_SE };
    CHECK(net.len == sizeof big && memcmp(nb, big, sizeof big) == 0);

    uint8_t tiny[2];
    te_tbuf small = { .data = tiny, .cap = sizeof tiny };
    static const uint8_t will_x[] = { TN_IAC, TN_WILL, 77 };
    telnet_rx(&t, will_x, sizeof will_x, &term, &small);
    CHECK(small.len == 0 && small.overflow);   /* never half a command */
}

/* A painter that only counts, for drawing code that must not crash. */
static int g_paint_calls;
static void count_rect(void *u, te_layer l, float x, float y, float w, float h, uint32_t c, float a)
{
    (void)u; (void)l; (void)x; (void)y; (void)w; (void)h; (void)c; (void)a;
    g_paint_calls++;
}
static void count_text(void *u, te_layer l, float x, float y, const char *s, size_t m, uint32_t c, bool b)
{
    (void)u; (void)l; (void)x; (void)y; (void)s; (void)m; (void)c; (void)b;
    g_paint_calls++;
}

static void test_ui_widgets(void)
{
    CHECK(ui_text_cols("abc") == 3);
    CHECK(ui_text_cols("\xe4\xb8\x80x") == 3);     /* wide CJK + 1 */
    CHECK(ui_text_fit("\xe4\xb8\x80x", 1) == 0);   /* the wide char does not fit in 1 */
    CHECK(ui_text_fit("ab\xc3\xa9z", 3) == 4);      /* 'a' 'b' 'é' */
    CHECK(ui_mix(0x000000, 0xFFFFFF, 0.5f) == 0x808080);

    te_form_field fl = { .kind = TE_FIELD_TEXT, .maxlen = 6 };
    CHECK(field_insert(&fl, "ab\xc3\xa9"));        /* "abé" = 4 bytes */
    CHECK(strcmp(fl.value, "ab\xc3\xa9") == 0 && fl.cursor == 4);
    field_backspace(&fl);
    CHECK(strcmp(fl.value, "ab") == 0 && fl.cursor == 2);
    CHECK(!field_insert(&fl, "x\x1b[31m"));        /* "x[31m" after cleaning: over the limit */
    CHECK(field_insert(&fl, "x\x1b"));             /* the escape byte is dropped */
    CHECK(strcmp(fl.value, "abx") == 0);
    CHECK(!field_insert(&fl, "123456789"));        /* over maxlen: refused whole */
    field_move(&fl, -1);
    field_move(&fl, -1);
    field_delete(&fl);
    CHECK(strlen(fl.value) < 7);
    te_form_field num = { .kind = TE_FIELD_NUMBER, .maxlen = 5 };
    CHECK(!field_insert(&num, "abc"));
    CHECK(field_insert(&num, "9a6") && strcmp(num.value, "96") == 0);

    te_painter p = { .cell_w = 8.0f, .cell_h = 16.0f, .scale = 1.0f, .rect = count_rect, .text = count_text };
    te_menu m;
    te_menu_item items[3] = {
        menu_item(1, "Connect", "Enter", true), menu_item(2, "Disabled", "", false), menu_item(3, "Delete...", "Del", true),
    };
    menu_open(&m, items, 3, 990.0f, 590.0f, 1000.0f, 600.0f, &p);
    CHECK(m.open && m.x + m.w <= 1000.0f && m.y + m.h <= 600.0f && m.x >= 0.0f && m.y >= 0.0f);
    CHECK(menu_key(&m, TE_KEY_DOWN) == TE_MENU_NOTHING && m.hover == 0);
    CHECK(menu_key(&m, TE_KEY_DOWN) == TE_MENU_NOTHING && m.hover == 2);   /* skips the disabled item */
    CHECK(menu_key(&m, TE_KEY_ENTER) == 3 && !m.open);
    menu_open(&m, items, 3, 10.0f, 10.0f, 1000.0f, 600.0f, &p);
    CHECK(menu_click(&m, 900.0f, 500.0f, &p) == TE_MENU_CLOSED && !m.open);
    g_paint_calls = 0;
    menu_open(&m, items, 3, 10.0f, 10.0f, 1000.0f, 600.0f, &p);
    te_ui_colors uc = { 0 };
    menu_draw(&m, &p, &uc);
    CHECK(g_paint_calls > 0);

    te_session q;
    CHECK(quick_parse(TE_SESS_SSH, " admin@10.0.0.1:2222 ", &q) && strcmp(q.user, "admin") == 0 &&
          strcmp(q.host, "10.0.0.1") == 0 && q.port == 2222);
    CHECK(quick_parse(TE_SESS_SSH, "[fe80::1]:22", &q) && strcmp(q.host, "fe80::1") == 0 && q.port == 22);
    CHECK(quick_parse(TE_SESS_SSH, "fe80::1", &q) && strcmp(q.host, "fe80::1") == 0 && q.port == 0);
    CHECK(quick_parse(TE_SESS_TELNET, "ts1 2003", &q) && strcmp(q.host, "ts1") == 0 && q.port == 2003);
#ifdef _WIN32
    CHECK(quick_parse(TE_SESS_SERIAL, "COM3,115200,8N1,none", &q) && q.serial.baud == 115200 &&
          strcmp(q.serial.device, "COM3") == 0 && strcmp(q.name, "COM3 115200") == 0);
#else
    CHECK(quick_parse(TE_SESS_SERIAL, "/dev/ttyUSB0,115200,8N1,none", &q) && q.serial.baud == 115200 &&
          strcmp(q.serial.device, "/dev/ttyUSB0") == 0 && strcmp(q.name, "ttyUSB0 115200") == 0);
#endif
    CHECK(session_validate(&q, nullptr));   /* every quick result must be launchable */
    CHECK(quick_parse(TE_SESS_SSH, "admin@10.0.0.1:2222", &q) && session_validate(&q, nullptr));
    CHECK(quick_parse(TE_SESS_TELNET, "ts1 2003", &q) && session_validate(&q, nullptr));
    CHECK(!quick_parse(TE_SESS_SSH, "-oProxyCommand=x", &q));
    CHECK(!quick_parse(TE_SESS_SSH, "host:99999", &q));
    CHECK(!quick_parse(TE_SESS_SERIAL, "/etc/passwd", &q));
    CHECK(!quick_parse(TE_SESS_SSH, "", &q));

    /* Jump hosts: every comma-separated hop is validated, not just the first. */
    te_session jt;
    session_defaults(&jt, TE_SESS_SSH);
    (void)str_copy(jt.name, sizeof jt.name, "jumpy");
    (void)str_copy(jt.host, sizeof jt.host, "10.0.0.1");
    (void)str_copy(jt.jump, sizeof jt.jump, "bastion,hop2.example.net");
    CHECK(session_validate(&jt, nullptr));
    (void)str_copy(jt.jump, sizeof jt.jump, "bastion,-oProxyCommand=x");
    CHECK(!session_validate(&jt, nullptr));   /* a hop starting with '-' is rejected */
    (void)str_copy(jt.jump, sizeof jt.jump, "bastion,");
    CHECK(!session_validate(&jt, nullptr));   /* empty trailing hop rejected */
    (void)str_copy(jt.jump, sizeof jt.jump, "");
    CHECK(session_validate(&jt, nullptr));    /* no jump host is fine */
}

/* Right-click only opens a menu; Delete only asks; nothing changes until confirmed. */
static int g_connects;
static void sb_connect(void *u, const te_session *s, const te_session_eff *e, bool split)
{
    (void)u; (void)s; (void)e; (void)split;
    g_connects++;
}

static void test_sidebar_rules(void)
{
    char root[512];
    (void)tmp_template(root, sizeof root, "te-sb");
    if (mkdtemp(root) == nullptr) {
        CHECK(false);
        return;
    }
    char sroot[256] = { 0 };
    CHECK(str_fmt(sroot, sizeof sroot, "%s/sessions", root) == TE_OK);
    te_store st;
    CHECK(store_open(&st, sroot) == TE_OK);
    te_session s;
    session_defaults(&s, TE_SESS_SSH);
    (void)str_copy(s.name, sizeof s.name, "alpha");
    (void)str_copy(s.host, sizeof s.host, "192.0.2.1");
    CHECK(store_save_session(&st, &s, nullptr, nullptr) == TE_OK);
    (void)str_copy(s.name, sizeof s.name, "beta");
    (void)str_copy(s.host, sizeof s.host, "198.51.100.9");
    CHECK(store_save_session(&st, &s, nullptr, nullptr) == TE_OK);
    store_close(&st);

    static te_sidebar sb;
    te_sidebar_host host = { .connect = sb_connect };
    sidebar_init(&sb, sroot, &host, nullptr);
    te_painter p = { .cell_w = 8.0f, .cell_h = 16.0f, .scale = 1.0f, .rect = count_rect, .text = count_text };
    sidebar_configure(&sb, true, false, 280.0f, true);
    sidebar_layout(&sb, &p, 1200.0f, 700.0f);
    CHECK(sb.nvis == 2 && sidebar_width(&sb) == 280.0f && sidebar_term_x(&sb) == 280.0f);

    float row0 = 16.0f * 1.6f + 16.0f * 1.8f + 16.0f * 0.7f;   /* inside the first tree row */
    CHECK(sidebar_button(&sb, 50.0f, row0, 2, true, 1, 0));
    CHECK(sb.menu.open && sb.sel >= 0 && g_connects == 0);
    CHECK(sidebar_button(&sb, 900.0f, 600.0f, 0, true, 1, 0));   /* click outside: just closes */
    CHECK(!sb.menu.open && !sb.form.open && sb.store.len == 2);

    sb.focused = true;
    CHECK(sidebar_key(&sb, TE_KEY_DELETE, 0, 0));
    CHECK(sb.form.open && sb.form.danger && sb.store.len == 2);   /* asks first */
    CHECK(sidebar_key(&sb, TE_KEY_ESCAPE, 0, 0));
    CHECK(!sb.form.open && sb.store.len == 2);                    /* cancelled: still there */
    CHECK(sidebar_key(&sb, TE_KEY_DELETE, 0, 0) && sb.form.open && sb.form.focus == 1);   /* Cancel focused */
    CHECK(sidebar_key(&sb, TE_KEY_ENTER, 0, 0));                  /* Enter on Cancel cancels */
    CHECK(!sb.form.open && sb.store.len == 2);
    CHECK(sidebar_key(&sb, TE_KEY_DELETE, 0, 0));
    sb.form.focus = (int)sb.form.n;                                /* move to Delete */
    CHECK(sidebar_key(&sb, TE_KEY_ENTER, 0, 0));
    CHECK(!sb.form.open && sb.store.len == 1);                    /* only now is it gone */

    CHECK(sidebar_text(&sb, "zzz") && sb.nvis == 0);              /* filter */
    CHECK(sidebar_key(&sb, TE_KEY_ESCAPE, 0, 0) && sb.filter[0] == '\0' && sb.nvis == 1);
    CHECK(sidebar_key(&sb, TE_KEY_ENTER, 0, 0) && g_connects == 1);   /* Enter connects */

    g_paint_calls = 0;
    te_ui_colors uc = { 0 };
    sidebar_draw(&sb, &p, &uc);
    CHECK(g_paint_calls > 10);
    sidebar_toggle(&sb);
    sidebar_layout(&sb, &p, 1200.0f, 700.0f);
    CHECK(!sb.visible && sidebar_width(&sb) == 24.0f);            /* the collapsed strip */
    sidebar_configure(&sb, true, true, 300.0f, false);
    sidebar_toggle(&sb);
    sidebar_layout(&sb, &p, 1200.0f, 700.0f);
    CHECK(sb.right && sidebar_term_x(&sb) == 0.0f && sb.x == 900.0f);
    sidebar_destroy(&sb);
    rm_tree(root);
}

static void test_example_config(void)
{
    te_config c;
    config_defaults(&c);
    CHECK(config_load(&c, "data/config.ini.example") == TE_OK);
    CHECK(c.anti_idle == 240 && c.anti_idle_len == 1 && c.anti_idle_bytes[0] == 0);
    CHECK(c.keepalive == 30 && c.reconnect == TE_RECONNECT_ASK && !c.sidebar_right && c.sidebar_width == 280.0f);
    CHECK(c.keys.b[TE_ACT_TOGGLE_SIDEBAR][0].key == TE_KEY_CHAR && c.keys.b[TE_ACT_SEND_BREAK][0].key == TE_KEY_PAUSE);
}

static void test_sessions_all(void)
{
    test_example_config();
    test_session_validation();
    test_session_store();
    test_launch_argv();
    test_telnet_protocol();
    test_ui_widgets();
    test_sidebar_rules();
}
