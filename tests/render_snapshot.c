/*
 * Render one frame of sample content and write it to a PPM image.
 * Exercises the real platform, font, renderer and terminal code with no
 * display server: run with SDL_VIDEO_DRIVER=offscreen.
 *   usage: render_snapshot out.ppm [theme]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/theme.h"
#include "app/layout.h"
#include "highlight/highlight.h"
#include "ui/search.h"
#include "ui/selection.h"
#include "ui/url.h"
#include "platform/platform.h"
#include "render/font.h"
#include "render/gl.h"
#include "render/renderer.h"
#include "util/log.h"
#include "util/mem.h"
#include "vt/term.h"

static const char demo[] =
    "\x1b[1;38;2;137;180;250m FeTerm \x1b[0m\x1b[2m phase-1 render test\x1b[0m\r\n\r\n"
    " \x1b[1mbold\x1b[0m \x1b[3mitalic\x1b[0m \x1b[4munderline\x1b[0m \x1b[4:3;58:2::243:139:168mcurly\x1b[0m"
    " \x1b[9mstrike\x1b[0m \x1b[7minverse\x1b[0m \x1b[2mdim\x1b[0m \x1b[53moverline\x1b[0m\r\n\r\n"
    " \x1b[30;40m  \x1b[31;41m  \x1b[32;42m  \x1b[33;43m  \x1b[34;44m  \x1b[35;45m  \x1b[36;46m  \x1b[37;47m  \x1b[0m"
    " \x1b[90;100m  \x1b[91;101m  \x1b[92;102m  \x1b[93;103m  \x1b[94;104m  \x1b[95;105m  \x1b[96;106m  \x1b[97;107m  \x1b[0m\r\n"
    " \x1b(0lqqqqqqqqqqk\x1b(B  \xe2\x94\x8c\xe2\x94\x80\xe2\x94\x80\xe2\x94\x90 caf\xc3\xa9 na\xc3\xafve \xe2\x86\x92 \xe2\x9c\x93 \xf0\x9f\x9a\x80\xf0\x9f\x98\x80 \xe2\x82\xac\r\n"
    " \x1b(0x\x1b(B  network  \x1b(0x\x1b(B\r\n"
    " \x1b(0mqqqqqqqqqqj\x1b(B\r\n\r\n"
    "core-sw01#show ip interface brief\r\n"
    "Interface              IP-Address      OK? Method Status                Protocol\r\n"
    "GigabitEthernet0/0     10.0.0.1        YES NVRAM  up                    up\r\n"
    "GigabitEthernet0/1     unassigned      YES unset  administratively down down\r\n"
    "core-sw01#";

static const char cisco_demo[] =
    "Cisco IOS XE Software, Version 17.09.04a\r\n"
    "core-sw01#show ip interface brief\r\n"
    "Interface              IP-Address      OK? Method Status                Protocol\r\n"
    "GigabitEthernet1/0/1   10.0.0.1        YES NVRAM  up                    up\r\n"
    "GigabitEthernet1/0/2   unassigned      YES unset  administratively down down\r\n"
    "Vlan20                 192.168.20.1    YES NVRAM  up                    down\r\n"
    "core-sw01#show interfaces Gi1/0/1 | i errors\r\n"
    "     0 input errors, 12 CRC, 0 frame, 0 overrun, 0 ignored\r\n"
    "     3 output errors, 0 collisions, 1 interface resets\r\n"
    "*Oct 21 18:02:11.123: %LINK-3-UPDOWN: Interface GigabitEthernet1/0/2, changed state to down\r\n"
    "*Oct 21 18:02:12.004: %SYS-5-CONFIG_I: Configured from console by admin\r\n"
    "core-sw01#show run interface Vlan20\r\n"
    "!\r\n"
    "interface Vlan20\r\n"
    " description Users floor 2\r\n"
    " ip address 192.168.20.1 255.255.255.0\r\n"
    " ipv6 address 2001:db8:20::1/64\r\n"
    " no shutdown\r\n"
    "core-sw01(config-if)#";

static const char juniper_demo[] =
    "{master:0}\r\n"
    "admin@mx1> show interfaces terse ge-0/0/*\r\n"
    "Interface               Admin Link Proto    Local                 Remote\r\n"
    "ge-0/0/0                up    up\r\n"
    "ge-0/0/0.0              up    up   inet     10.10.0.1/30\r\n"
    "ge-0/0/1                up    down\r\n"
    "admin@mx1> show bgp summary | match Estab\r\n"
    "10.10.0.2           65002       1234       1250       0       0  2d 3:04:05 Establ\r\n"
    "admin@mx1> configure\r\n"
    "[edit]\r\n"
    "admin@mx1# show interfaces ae0\r\n"
    "description \"to core\";\r\n"
    "unit 0 {\r\n"
    "    family inet {\r\n"
    "        address 192.0.2.1/31;\r\n"
    "    }\r\n"
    "}\r\n"
    "[edit]\r\n"
    "admin@mx1# set interfaces ge-0/0/1 disable\r\n"
    "admin@mx1# delete protocols ospf area 0.0.0.0\r\n"
    "admin@mx1# commit\r\n"
    "commit complete\r\n"
    "admin@mx1# ";

static const char arista_demo[] =
    "Arista DCS-7050SX3-48YC8\r\n"
    "leaf1#show interfaces status\r\n"
    "Port       Name        Status       Vlan     Duplex Speed  Type\r\n"
    "Et1        spine1      connected    routed   full   100G   100GBASE-SR4\r\n"
    "Et2        spine2      notconnect   routed   full   100G   100GBASE-SR4\r\n"
    "Et49/1     mlag-peer   errdisabled  trunk    full   25G    25GBASE-CR\r\n"
    "leaf1#show ip bgp summary\r\n"
    "  Neighbor   V AS      MsgRcvd MsgSent  InQ OutQ  Up/Down State   PfxRcd\r\n"
    "  10.0.1.0   4 65000     1203    1199    0    0 16:01:05 Estab   12\r\n"
    "  10.0.2.0   4 65000        0       0    0    0 00:00:12 Active\r\n"
    "leaf1#configure\r\n"
    "leaf1(config)#interface Ethernet1\r\n"
    "leaf1(config-if-Et1)#";

static void write_ppm(const char *path, int w, int h, const uint8_t *rgba)
{
    FILE *fp = fopen(path, "wb");
    if (fp == nullptr) {
        perror(path);
        return;
    }
    fprintf(fp, "P6\n%d %d\n255\n", w, h);
    for (int y = h - 1; y >= 0; y--) {          /* GL rows are bottom-up */
        for (int x = 0; x < w; x++) {
            const uint8_t *p = rgba + ((size_t)y * (size_t)w + (size_t)x) * 4u;
            fputc(p[0], fp);
            fputc(p[1], fp);
            fputc(p[2], fp);
        }
    }
    fclose(fp);
}

static const char box_demo[] =
    "\x1b[1m System \x1b[0m\r\n"
    "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xac\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x90\r\n"
    "\xe2\x94\x82 CPU    \xe2\x94\x82 \x1b[32m\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x8c\x1b[0m  \xe2\x94\x82\r\n"
    "\xe2\x94\x82 Memory \xe2\x94\x82 \x1b[33m\xe2\x96\x88\xe2\x96\x88\xe2\x96\x93\xe2\x96\x92\xe2\x96\x91\x1b[0m  \xe2\x94\x82\r\n"
    "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xbc\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xa4\r\n"
    "\xe2\x94\x82 Disk   \xe2\x94\x82 \x1b[31m\xe2\x96\x81\xe2\x96\x82\xe2\x96\x83\xe2\x96\x84\xe2\x96\x85\xe2\x96\x86\xe2\x96\x87\x1b[0m\xe2\x94\x82\r\n"
    "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xb4\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x98\r\n"
    "\xe2\x95\x94\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\xa6\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x97 \xe2\x94\x8f\xe2\x94\x81\xe2\x94\x81\xe2\x94\xb3\xe2\x94\x81\xe2\x94\x81\xe2\x94\x93\r\n"
    "\xe2\x95\x91 db \xe2\x95\x91 ok \xe2\x95\x91 \xe2\x94\x83 a\xe2\x94\x83 b\xe2\x94\x83\r\n"
    "\xe2\x95\x9a\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\xa9\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x9d \xe2\x94\x97\xe2\x94\x81\xe2\x94\x81\xe2\x94\xbb\xe2\x94\x81\xe2\x94\x81\xe2\x94\x9b\r\n"
    "\xe2\x94\x84\xe2\x94\x84\xe2\x94\x84 \xe2\x95\x8c\xe2\x95\x8c \xe2\x96\x9b\xe2\x96\x9c\xe2\x96\x99\xe2\x96\x9f \xe2\x96\x80\xe2\x96\x84\r\n$ ";

static uint16_t snap_cw = 9, snap_ch = 20;
static void snap_cell_size(void *u, uint16_t *w, uint16_t *h)
{
    (void)u;
    *w = snap_cw;
    *h = snap_ch;
}

/* Base64-encode n bytes (for building a kitty graphics command). */
static void b64(const uint8_t *in, size_t n, char *out)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = i + 1 < n ? tbl[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? tbl[v & 63] : '=';
    }
    out[o] = '\0';
}

/* Two tabs; the active one split into three panes with box drawing, network output and images. */
static bool phase4(const char *out_path, const te_theme *th)
{
    bool ok = false;
    te_platform *plat = nullptr;
    te_font *font = nullptr;
    te_renderer *ren = nullptr;
    te_term *terms[3] = { nullptr, nullptr, nullptr };
    te_highlighter *hl = nullptr;
    uint8_t *px = nullptr;
    char *kcmd = nullptr;
    te_layout l;
    layout_init(&l, 2);
    te_platform_opts po = { .title = "snapshot", .width = 1200, .height = 640 };
    if (plat_create(&plat, &po) != TE_OK) goto out;
    if (font_create(&font, "monospace", 15.0f, 1.1f) != TE_OK) goto out;
    if (renderer_create(&ren, font) != TE_OK) goto out;
    const te_font_metrics *m = font_metrics(font);
    snap_cw = (uint16_t)m->cell_w;
    snap_ch = (uint16_t)m->cell_h;
    int w = 0, h = 0;
    plat_drawable_size(plat, &w, &h);
    int bar = (int)((float)m->cell_h * 1.6f);
    int pad = 8;
    /* Panes are identified by their terminal in this harness. */
    struct te_pane *ids[4];
    char d[4];
    for (int i = 0; i < 4; i++) ids[i] = (struct te_pane *)(void *)&d[i];
    if (layout_add_tab(&l, ids[3]) != TE_OK) goto out;                 /* background tab */
    if (layout_add_tab(&l, ids[0]) != TE_OK) goto out;
    if (layout_split(&l, ids[1], true) != TE_OK) goto out;
    if (layout_split(&l, ids[2], false) != TE_OK) goto out;
    layout_set_focus(&l, ids[1]);
    layout_compute(&l, 0, bar, w, h - bar);
    te_term_host host = { .cell_size = snap_cell_size };
    int rects[3][4];
    for (int i = 0; i < 3; i++) {
        if (!layout_pane_rect(&l, ids[i], rects[i])) goto out;
        uint16_t cols = (uint16_t)((rects[i][2] - 2 * pad) / m->cell_w);
        uint16_t rows = (uint16_t)((rects[i][3] - 2 * pad) / m->cell_h);
        if (term_create(&terms[i], cols, rows, 100, th, &host) != TE_OK) goto out;
    }
    term_feed(terms[0], (const uint8_t *)box_demo, sizeof box_demo - 1);
    term_feed(terms[1], (const uint8_t *)cisco_demo, strlen(cisco_demo));
    if (hl_create(&hl) == TE_OK) {
        hl_set_fallback(hl, "network-common");
        (void)hl_load_all(hl);
        (void)hl_set_mode(hl, TE_HL_AUTO, nullptr);
    }
    /* Pane 3: a sixel gradient and a kitty RGBA image. */
    static const char intro[] = "\x1b[1mimages\x1b[0m sixel:\r\n";
    term_feed(terms[2], (const uint8_t *)intro, sizeof intro - 1);
    char six[8192] = "\x1bPq\"1;1;96;48";
    size_t sl = strlen(six);
    for (int c = 0; c < 8; c++) {
        sl += (size_t)snprintf(six + sl, sizeof six - sl, "#%d;2;%d;%d;%d", c, c * 14, 30 + c * 8, 100 - c * 10);
    }
    for (int band = 0; band < 8; band++) {
        for (int c = 0; c < 8; c++) {
            sl += (size_t)snprintf(six + sl, sizeof six - sl, "#%d!12~", c);
        }
        sl += (size_t)snprintf(six + sl, sizeof six - sl, "-");
    }
    sl += (size_t)snprintf(six + sl, sizeof six - sl, "\x1b\\");
    term_feed(terms[2], (const uint8_t *)six, sl);
    static const char kmsg[] = "kitty graphics:\r\n";
    term_feed(terms[2], (const uint8_t *)kmsg, sizeof kmsg - 1);
    uint32_t kw = 64, kh = 40;
    px = te_malloc((size_t)kw * kh * 4);
    kcmd = te_malloc((size_t)kw * kh * 4 * 2 + 128);
    if (px == nullptr || kcmd == nullptr) goto out;
    for (uint32_t y = 0; y < kh; y++) {
        for (uint32_t x = 0; x < kw; x++) {
            uint8_t *q = px + ((size_t)y * kw + x) * 4;
            float dx = (float)x - 32.0f, dy = (float)y - 20.0f;
            bool in = dx * dx / 900.0f + dy * dy / 324.0f < 1.0f;
            q[0] = (uint8_t)(x * 4);
            q[1] = (uint8_t)(120 + y * 3);
            q[2] = 220;
            q[3] = in ? 255 : 0;
        }
    }
    int k = snprintf(kcmd, 128, "\x1b_Ga=T,f=32,s=%u,v=%u,i=1,q=2;", kw, kh);
    b64(px, (size_t)kw * kh * 4, kcmd + k);
    strcat(kcmd, "\x1b\\");
    term_feed(terms[2], (const uint8_t *)kcmd, strlen(kcmd));
    static const char after[] = " <- circle with alpha\r\n$ ";
    term_feed(terms[2], (const uint8_t *)after, sizeof after - 1);

    static const char *titles[2] = { "build-server", "core-sw01 [3]" };
    int dividers[8][4];
    te_chrome c = {
        .win_w = w, .win_h = h, .tab_titles = titles, .ntabs = 2, .active_tab = 1, .tab_bar_h = bar,
        .bar_bg = th->ansi[0], .bar_fg = th->fg, .active_bg = th->bg, .active_fg = th->fg, .accent = th->cursor,
        .divider = th->selection_bg, .toast_fg = th->selection_fg, .toast_bg = th->selection_bg, .dim_bg = th->bg,
        .scale = 1.0f, .toast = "Highlighting: cisco",
    };
    c.ndividers = layout_dividers(&l, dividers, 8);
    c.dividers = (const int (*)[4])dividers;
    for (int pass = 0; pass < 3; pass++) {
        renderer_begin(ren, w, h, th->bg, 1.0f);
        for (int i = 0; i < 3; i++) {
            te_render_opts ro = { .padding = pad, .opacity = 1.0f, .cursor_shape = TE_CURSOR_BLOCK,
                                  .cursor_text = th->cursor_text, .hl = i == 1 ? hl : nullptr,
                                  .toast_fg = th->selection_fg, .toast_bg = th->selection_bg };
            te_frame f = { .width = rects[i][2], .height = rects[i][3], .x = rects[i][0], .y = rects[i][1],
                           .focused = i == 1, .cursor_on = true, .scale = 1.0f, .dim = i == 1 ? 0.0f : 0.3f };
            if (renderer_draw(ren, terms[i], &ro, &f) != TE_OK) goto out;
        }
        renderer_chrome(ren, &c);
        if (renderer_end(ren) != TE_OK) goto out;
    }
    TE_FREE(px);
    px = te_calloc((size_t)w * (size_t)h, 4);
    if (px == nullptr) goto out;
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    write_ppm(out_path, w, h, px);
    printf("wrote %s (%dx%d), images in pane 3: %zu\n", out_path, w, h, term_image_count(terms[2]));
    ok = true;
out:
    TE_FREE(px);
    TE_FREE(kcmd);
    hl_destroy(&hl);
    for (int i = 0; i < 3; i++) term_destroy(&terms[i]);
    layout_destroy(&l);
    renderer_destroy(&ren);
    font_destroy(&font);
    plat_destroy(&plat);
    return ok;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s out.ppm [theme] [demo|cisco|juniper|arista]\n", argv[0]);
        return 2;
    }
    int rc = EXIT_FAILURE;
    te_platform *plat = nullptr;
    te_font *font = nullptr;
    te_renderer *ren = nullptr;
    te_term *term = nullptr;
    uint8_t *px = nullptr;
    te_highlighter *hl = nullptr;
    const char *which = argc > 3 ? argv[3] : "demo";
    const char *text = demo;
    bool phase3 = strcmp(which, "phase3") == 0 || strcmp(which, "modal") == 0;
    bool modal = strcmp(which, "modal") == 0;
    if (strcmp(which, "cisco") == 0 || phase3) text = cisco_demo;
    else if (strcmp(which, "juniper") == 0) text = juniper_demo;
    else if (strcmp(which, "arista") == 0) text = arista_demo;
    te_theme th;
    if (theme_load(&th, argc > 2 ? argv[2] : "catppuccin-mocha") != TE_OK) {
        theme_defaults(&th);
    }
    if (strcmp(which, "phase4") == 0) {
        rc = phase4(argv[1], &th) ? EXIT_SUCCESS : EXIT_FAILURE;
        return rc;
    }
    te_platform_opts po = { .title = "snapshot", .width = 1000, .height = text == demo ? 400 : 560 };
    if (plat_create(&plat, &po) != TE_OK) goto out;
    if (font_create(&font, "monospace", 15.0f, 1.1f) != TE_OK) goto out;
    if (renderer_create(&ren, font) != TE_OK) goto out;
    const te_font_metrics *m = font_metrics(font);
    int pad = 10;
    int w = 0, h = 0;
    plat_drawable_size(plat, &w, &h);
    uint16_t cols = (uint16_t)((w - 2 * pad) / m->cell_w);
    uint16_t rows = (uint16_t)((h - 2 * pad) / m->cell_h);
    if (term_create(&term, cols, rows, 100, &th, nullptr) != TE_OK) goto out;
    term_feed(term, (const uint8_t *)text, strlen(text));
    if (hl_create(&hl) == TE_OK) {
        hl_set_fallback(hl, "network-common");
        (void)hl_load_all(hl);
        (void)hl_set_mode(hl, TE_HL_AUTO, nullptr);
    }

    te_selection sel;
    sel_clear(&sel);
    te_search *search = nullptr;
    te_urlfinder *urls = nullptr;
    te_url_hit hover;
    bool hover_ok = false;
    if (phase3) {
        static const char extra[] = "\r\n! docs: https://www.cisco.com/c/en/us/support/index.html";
        term_feed(term, (const uint8_t *)extra, sizeof extra - 1);
        /* Select the two interface rows. */
        uint64_t r = term_view_abs(term, 3);
        sel_start(&sel, term, (te_selpos){ r, 0 }, TE_SEL_STREAM, "");
        sel_update(&sel, term, (te_selpos){ r + 1, 60 }, "");
        if (search_create(&search) == TE_OK) {
            search_open(search);
            search_append(search, term, "errors");
        }
        if (url_create(&urls) == TE_OK) {
            uint16_t rows_n = term_rows(term);
            for (uint16_t y = 0; y < rows_n && !hover_ok; y++) {
                hover_ok = url_at(urls, term, term_view_abs(term, y), 15, &hover);
            }
        }
    }
    te_render_opts ro = { .padding = pad, .opacity = 1.0f, .cursor_shape = TE_CURSOR_BLOCK,
                          .bold_is_bright = false, .cursor_text = th.cursor_text, .hl = hl,
                          .toast_fg = th.selection_fg, .toast_bg = th.selection_bg,
                          .sel = &sel, .sel_fg = th.selection_fg, .sel_bg = th.selection_bg,
                          .search = search, .search_fg = th.search_fg, .search_bg = th.search_bg,
                          .search_cur_bg = th.search_current_bg, .hover = hover_ok ? &hover : nullptr };
    te_frame f = { .width = w, .height = h, .focused = true, .cursor_on = true, .bell = 0.0f, .scale = 1.0f };
    /* Several passes: the first fills the glyph atlas and lets auto-detection settle. */
    static char bar[256];
    if (search != nullptr) {
        snprintf(bar, sizeof bar, "Find: %s\xe2\x96\x8f   %s      Enter: older   Shift+Enter: newer   Esc: close",
                 search_query(search), search_status(search));
        f.bar = bar;
    }
    if (modal) {
        f.modal = "Paste 3 lines? Each line runs as it arrives.   Enter: paste    Esc: cancel";
        f.bar = nullptr;
    }
    for (int i = 0; i < 3; i++) {
        renderer_begin(ren, w, h, th.bg, 1.0f);
        if (i == 2 && !phase3 && hl != nullptr && hl_active_name(hl) != nullptr) {
            static char msg[96];
            snprintf(msg, sizeof msg, "Highlighting: %s", hl_active_name(hl));
            f.toast = msg;
        }
        if (renderer_draw(ren, term, &ro, &f) != TE_OK) goto out;
        if (renderer_end(ren) != TE_OK) goto out;
    }
    px = te_calloc((size_t)w * (size_t)h, 4);
    if (px == nullptr) goto out;
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    write_ppm(argv[1], w, h, px);
    printf("wrote %s (%dx%d, grid %ux%u, cell %dx%d)\n", argv[1], w, h, cols, rows, m->cell_w, m->cell_h);
    rc = EXIT_SUCCESS;
out:
    TE_FREE(px);
    search_destroy(&search);
    url_destroy(&urls);
    hl_destroy(&hl);
    term_destroy(&term);
    renderer_destroy(&ren);
    font_destroy(&font);
    plat_destroy(&plat);
    return rc;
}
