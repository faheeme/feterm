/*
 * Tests for the file explorer: the SFTP codec (including hostile replies),
 * local file-name safety, SCP path quoting and the size formatter.
 * Included by test_core.c.
 */
#include <fcntl.h>

#include "remote/remote.h"
#include "remote/sftp_proto.h"
#include "ui/files.h"
#include "app/banner.h"

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* Build a reply packet body (without its length) in buf; returns its size. */
static size_t mk(uint8_t *buf, uint8_t type, uint32_t id, const uint8_t *rest, size_t rn)
{
    buf[0] = type;
    be32(buf + 1, id);
    memcpy(buf + 5, rest, rn);
    return 5 + rn;
}

static void test_sftp_codec(void)
{
    te_sbuf b;
    sbuf_init(&b, 1024 * 1024);
    CHECK(sftp_enc_init(&b) && b.len == 9 && b.data[4] == SSH_FXP_INIT && b.data[8] == 3);
    size_t total = 0;
    CHECK(sftp_frame(b.data, b.len, &total) == 1 && total == 9);
    CHECK(sftp_frame(b.data, 8, &total) == 0);
    b.len = 0;
    CHECK(sftp_enc_path(&b, SSH_FXP_OPENDIR, 7, "/var/log"));
    CHECK(b.len == 4 + 1 + 4 + 4 + 8 && b.data[4] == SSH_FXP_OPENDIR);
    uint8_t h[4] = { 1, 2, 3, 4 };
    uint8_t payload[100] = { 0 };
    b.len = 0;
    CHECK(sftp_enc_write(&b, 9, h, 4, 65536, payload, sizeof payload));
    CHECK(sftp_frame(b.data, b.len, &total) == 1 && total == b.len);
    sbuf_free(&b);

    /* A tiny limit: the encoder refuses and leaves nothing half-written. */
    te_sbuf small;
    sbuf_init(&small, 16);
    CHECK(sftp_enc_init(&small));
    size_t before = small.len;
    CHECK(!sftp_enc_path(&small, SSH_FXP_STAT, 1, "/a/rather/long/path/name"));
    CHECK(small.len == before);
    sbuf_free(&small);

    /* Frames that claim too much, or nothing. */
    uint8_t fr[8] = { 0 };
    be32(fr, 0);
    CHECK(sftp_frame(fr, sizeof fr, &total) == -1);
    be32(fr, TE_SFTP_MAX_PACKET + 1);
    CHECK(sftp_frame(fr, sizeof fr, &total) == -1);

    static uint8_t pkt[4096];
    te_sftp_reply r;
    /* STATUS with message */
    uint8_t st[] = { 0, 0, 0, 2, 0, 0, 0, 5, 'n', 'o', '\x1b', 'p', 'e', 0, 0, 0, 0 };
    size_t n = mk(pkt, SSH_FXP_STATUS, 42, st, sizeof st);
    CHECK(sftp_parse(pkt, n, &r) == TE_OK && r.id == 42 && r.status == SSH_FX_NO_SUCH_FILE && strcmp(r.msg, "no?pe") == 0);
    sftp_reply_free(&r);
    /* STATUS cut short */
    n = mk(pkt, SSH_FXP_STATUS, 42, st, 2);
    CHECK(sftp_parse(pkt, n, &r) != TE_OK);

    /* NAME with one entry and full attributes */
    uint8_t nm[64] = { 0 };
    size_t k = 0;
    be32(nm + k, 1); k += 4;                          /* count */
    be32(nm + k, 3); k += 4; memcpy(nm + k, "abc", 3); k += 3;
    be32(nm + k, 0); k += 4;                          /* longname */
    be32(nm + k, SSH_FILEXFER_ATTR_SIZE | SSH_FILEXFER_ATTR_PERMISSIONS | SSH_FILEXFER_ATTR_ACMODTIME); k += 4;
    be32(nm + k, 0); k += 4; be32(nm + k, 1234); k += 4;   /* size */
    be32(nm + k, 040755); k += 4;                      /* a folder */
    be32(nm + k, 1); k += 4; be32(nm + k, 1700000000); k += 4;
    n = mk(pkt, SSH_FXP_NAME, 5, nm, k);
    CHECK(sftp_parse(pkt, n, &r) == TE_OK && r.nnames == 1 && strcmp(r.names[0].name, "abc") == 0 &&
          r.names[0].a.size == 1234 && sftp_is_dir(&r.names[0].a) && r.names[0].a.mtime == 1700000000);
    sftp_reply_free(&r);
    CHECK(r.names == nullptr && r.nnames == 0);

    /* Hostile NAME replies: every one must fail cleanly. */
    uint8_t bad[32] = { 0 };
    be32(bad, 0xFFFFFFFFu);                            /* absurd count */
    n = mk(pkt, SSH_FXP_NAME, 5, bad, 4);
    CHECK(sftp_parse(pkt, n, &r) != TE_OK);
    be32(bad, 1);
    be32(bad + 4, 0x7FFFFFFFu);                        /* name length past the end */
    n = mk(pkt, SSH_FXP_NAME, 5, bad, 12);
    CHECK(sftp_parse(pkt, n, &r) != TE_OK);
    be32(bad + 4, 3);
    memcpy(bad + 8, "a\0b", 3);                        /* embedded NUL */
    be32(bad + 11, 0);
    be32(bad + 15, 0);
    n = mk(pkt, SSH_FXP_NAME, 5, bad, 19);
    CHECK(sftp_parse(pkt, n, &r) != TE_OK);
    memcpy(bad + 8, "a/b", 3);
    be32(bad + 15, SSH_FILEXFER_ATTR_EXTENDED);        /* extended attrs, then nothing */
    n = mk(pkt, SSH_FXP_NAME, 5, bad, 19);
    CHECK(sftp_parse(pkt, n, &r) != TE_OK);
    be32(bad + 4, 0);                                  /* empty name */
    n = mk(pkt, SSH_FXP_NAME, 5, bad, 16);
    CHECK(sftp_parse(pkt, n, &r) != TE_OK);

    /* HANDLE: empty or oversized is refused. */
    uint8_t hd[8] = { 0 };
    be32(hd, 0);
    n = mk(pkt, SSH_FXP_HANDLE, 1, hd, 4);
    CHECK(sftp_parse(pkt, n, &r) != TE_OK);
    be32(hd, TE_SFTP_MAX_HANDLE + 1);
    n = mk(pkt, SSH_FXP_HANDLE, 1, hd, 8);
    CHECK(sftp_parse(pkt, n, &r) != TE_OK);
    be32(hd, 4);
    memcpy(hd + 4, "\x01\x02\x03\x04", 4);
    n = mk(pkt, SSH_FXP_HANDLE, 1, hd, 8);
    CHECK(sftp_parse(pkt, n, &r) == TE_OK && r.handle_len == 4 && r.handle[3] == 4);

    /* DATA longer than the packet */
    be32(hd, 100);
    n = mk(pkt, SSH_FXP_DATA, 1, hd, 8);
    CHECK(sftp_parse(pkt, n, &r) != TE_OK);
    /* Unknown type */
    n = mk(pkt, 77, 1, hd, 4);
    CHECK(sftp_parse(pkt, n, &r) != TE_OK);
    /* VERSION */
    uint8_t ver[] = { SSH_FXP_VERSION, 0, 0, 0, 3 };
    CHECK(sftp_parse(ver, sizeof ver, &r) == TE_OK && r.version == 3);

    /* Random garbage never crashes the decoder. */
    uint32_t seed = 12345;
    for (int round = 0; round < 3000; round++) {
        size_t len = 1 + (size_t)(seed % 200);
        for (size_t i = 0; i < len; i++) {
            seed = seed * 1103515245u + 12345u;
            pkt[i] = (uint8_t)(seed >> 16);
        }
        pkt[0] = (uint8_t)(101 + round % 5);
        if (sftp_parse(pkt, len, &r) == TE_OK) {
            sftp_reply_free(&r);
        }
    }
    CHECK(true);
}

static void test_remote_helpers(void)
{
    char out[64] = { 0 };
    CHECK(remote_local_name("report.pdf", out, sizeof out) && strcmp(out, "report.pdf") == 0);
    CHECK(remote_local_name("../etc/passwd", out, sizeof out) && strchr(out, '/') == nullptr);
    CHECK(!remote_local_name("..", out, sizeof out));
    CHECK(!remote_local_name(".", out, sizeof out));
    CHECK(!remote_local_name("\x01\x02", out, sizeof out));
    CHECK(remote_local_name("bad\x1b[31mname", out, sizeof out) && strchr(out, 0x1b) == nullptr);
    CHECK(remote_local_name("-rf", out, sizeof out) && out[0] == '_');

    char q[128] = { 0 };
    CHECK(remote_scp_quote("flash:/config.text", q, sizeof q) && strcmp(q, "flash:/config.text") == 0);
    CHECK(remote_scp_quote("/tmp/a b", q, sizeof q) && strcmp(q, "'/tmp/a b'") == 0);
    CHECK(remote_scp_quote("/tmp/it's", q, sizeof q) && strcmp(q, "'/tmp/it'\\''s'") == 0);
    CHECK(remote_scp_quote("/tmp/$(reboot)", q, sizeof q) && q[0] == '\'');
    CHECK(!remote_scp_quote("-oProxyCommand=x", q, sizeof q));
    CHECK(!remote_scp_quote("/tmp/a\nreboot", q, sizeof q));
    CHECK(!remote_scp_quote("", q, sizeof q));

    char j[64] = { 0 };
    CHECK(remote_join("/", "etc", j, sizeof j) == TE_OK && strcmp(j, "/etc") == 0);
    CHECK(remote_join("/var/log/", "x", j, sizeof j) == TE_OK && strcmp(j, "/var/log/x") == 0);
    CHECK(remote_join("", "x", j, sizeof j) == TE_OK && strcmp(j, "x") == 0);

    char dir[512];
    (void)tmp_template(dir, sizeof dir, "te-uniq");
    if (mkdtemp(dir) != nullptr) {
        int fd = open(dir, O_RDONLY | O_DIRECTORY);
        char p[300] = { 0 };
        (void)str_fmt(p, sizeof p, "%s/log.txt", dir);
        FILE *fp = fopen(p, "w");
        if (fp != nullptr) fclose(fp);
        char u[64] = { 0 };
        CHECK(fd >= 0 && remote_unique_name(fd, "log.txt", u, sizeof u) == TE_OK && strcmp(u, "log (1).txt") == 0);
        CHECK(fd >= 0 && remote_unique_name(fd, "new.txt", u, sizeof u) == TE_OK && strcmp(u, "new.txt") == 0);
        if (fd >= 0) close(fd);
        unlink(p);
        rmdir(dir);
    }

    char sz[16] = { 0 };
    files_human_size(512, sz, sizeof sz);
    CHECK(strcmp(sz, "512") == 0);
    files_human_size(1536, sz, sizeof sz);
    CHECK(strcmp(sz, "1.5K") == 0);
    files_human_size(50u * 1024u * 1024u, sz, sizeof sz);
    CHECK(strcmp(sz, "50M") == 0);
}

static void test_banner(void)
{
    char b[32] = { 0 };
    CHECK(base64_encode((const uint8_t *)"Man", 3, b, sizeof b) == 4 && strcmp(b, "TWFu") == 0);
    CHECK(base64_encode((const uint8_t *)"Ma", 2, b, sizeof b) == 4 && strcmp(b, "TWE=") == 0);
    CHECK(base64_encode((const uint8_t *)"M", 1, b, sizeof b) == 4 && strcmp(b, "TQ==") == 0);
    CHECK(base64_encode((const uint8_t *)"Man", 3, b, 4) == 0);   /* no room for the terminator */

    static uint8_t logo[64 * 64 * 4];
    for (size_t i = 0; i < sizeof logo; i++) logo[i] = (uint8_t)i;
    char *text = nullptr;
    size_t n = 0;
    CHECK(banner_build(logo, sizeof logo, 64, 4, 2, &text, &n) == TE_OK && text != nullptr);
    if (text != nullptr) {
        CHECK(strstr(text, "\x1b_Ga=T,f=32,s=64,v=64,c=4,r=2,C=1,q=2,m=1;") == text);
        CHECK(strstr(text, APP_DISPLAY_NAME) != nullptr && strstr(text, "v" APP_VERSION) != nullptr);
        CHECK(strstr(text, "\xc2\xa9 " APP_AUTHOR " - " APP_EMAIL) != nullptr);
        CHECK(strstr(text, "\x1b_Gm=0;") != nullptr && n == strlen(text));
        /* Fed to a terminal: the logo becomes one placed image, the text sits beside it. */
        te_theme th;
        theme_defaults(&th);
        te_term *t = nullptr;
        CHECK(term_create(&t, 80, 24, 100, &th, nullptr) == TE_OK);
        if (t != nullptr) {
            term_feed(t, (const uint8_t *)text, n);
            CHECK(term_image_count(t) == 1);
            CHECK(cp_at(t, 5, 0) == (uint32_t)APP_DISPLAY_NAME[0]);
            uint16_t x = 0, y = 0;
            term_cursor(t, &x, &y);
            CHECK(x == 0 && y == 3);   /* two banner lines, a blank one, then the prompt */
            /* A width change (window resize, sidebar toggle) keeps the logo on its row. */
            CHECK(term_resize(t, 120, 24) == TE_OK && term_image_count(t) == 1);
            const te_image *im = term_image_at(t, 0);
            CHECK(im != nullptr && im->placed && im->row == 0 && im->col == 0);
            CHECK(term_resize(t, 60, 24) == TE_OK && im->placed && im->row == 0);
            term_destroy(&t);
        }
        /* An image below a wrapped line moves up when the line un-wraps. */
        CHECK(term_create(&t, 20, 10, 100, &th, nullptr) == TE_OK);
        if (t != nullptr) {
            feed(t, "aaaaaaaaaaaaaaaaaaaaaaaaa\r\n");   /* 25 chars: two rows at 20 columns */
            term_feed(t, (const uint8_t *)text, n);
            const te_image *im = term_image_at(t, 0);
            CHECK(im != nullptr && im->placed && im->row == 2);
            CHECK(term_resize(t, 40, 10) == TE_OK && im->placed && im->row == 1);
            term_destroy(&t);
        }
        TE_FREE(text);
    }
    CHECK(banner_build(nullptr, 0, 0, 0, 0, &text, &n) == TE_OK && text != nullptr &&
          strstr(text, APP_DISPLAY_NAME) != nullptr && strstr(text, "\x1b_G") == nullptr);   /* text only */
    TE_FREE(text);
    CHECK(banner_build(logo, 10, 64, 4, 2, &text, &n) == TE_OK && strstr(text, "\x1b_G") == nullptr);   /* bad size: text only */
    TE_FREE(text);
}

static void test_remote_all(void)
{
    test_banner();
    test_sftp_codec();
    test_remote_helpers();
}
