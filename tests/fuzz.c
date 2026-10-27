/*
 * Fuzz targets for FeTerm's untrusted-input parsers. Build with:
 *
 *     make fuzz                 # one libFuzzer binary per target (needs clang)
 *     make fuzz-standalone      # replay a corpus/crash file with gcc, no libFuzzer
 *
 * Which parser a binary exercises is chosen at compile time with FUZZ_TARGET:
 *   1 term_feed (VT/escape parser)  2 sftp_parse   3 str_unescape
 *   4 sixel_decode                  5 ini_parse_file
 * Each target is fed attacker-controlled bytes and must never crash, leak, or
 * trip UBSan. Run under -fsanitize=fuzzer,address,undefined.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config/ini.h"
#include "config/theme.h"
#include "remote/sftp_proto.h"
#include "util/str.h"
#include "vt/images.h"
#include "vt/parser.h"
#include "vt/term.h"

#ifndef FUZZ_TARGET
#define FUZZ_TARGET 1
#endif

#if FUZZ_TARGET == 1
static void fuzz_one(const uint8_t *data, size_t size)
{
    te_theme th = { 0 };
    te_term *t = nullptr;
    if (term_create(&t, 80, 24, 500, &th, nullptr) != TE_OK) {
        return;
    }
    term_feed(t, data, size);
    size_t ol = 0;
    const uint8_t *o = term_output(t, &ol);
    (void)o;
    if (ol > 0) {
        term_output_consume(t, ol);
    }
    term_destroy(&t);
}
#elif FUZZ_TARGET == 2
static void fuzz_one(const uint8_t *data, size_t size)
{
    te_sftp_reply r;
    if (sftp_parse(data, size, &r) == TE_OK) {
        sftp_reply_free(&r);
    }
}
#elif FUZZ_TARGET == 3
static void fuzz_one(const uint8_t *data, size_t size)
{
    char in[1024];
    size_t n = size < sizeof in - 1 ? size : sizeof in - 1;
    memcpy(in, data, n);
    in[n] = '\0';
    uint8_t out[1024];
    size_t outlen = 0;
    (void)str_unescape(in, out, sizeof out, &outlen);
}
#elif FUZZ_TARGET == 4
static void fuzz_one(const uint8_t *data, size_t size)
{
    vt_params p = { 0 };
    uint8_t *rgba = nullptr;
    uint32_t w = 0, h = 0;
    if (sixel_decode(&p, (const char *)data, size, 0x000000, &rgba, &w, &h) == TE_OK) {
        free(rgba);
    }
}
#elif FUZZ_TARGET == 5
static te_err ini_sink(void *u, const char *sec, const char *key, const char *val, const char *path, unsigned line)
{
    (void)u; (void)sec; (void)key; (void)val; (void)path; (void)line;
    return TE_OK;
}
static void fuzz_one(const uint8_t *data, size_t size)
{
    char path[] = "/tmp/feterm-fuzz-ini-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        return;
    }
    (void)!write(fd, data, size);
    close(fd);
    (void)ini_parse_file(path, ini_sink, nullptr);
    unlink(path);
}
#else
#error "unknown FUZZ_TARGET"
#endif

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    fuzz_one(data, size);
    return 0;
}

#ifdef FUZZ_STANDALONE
/* Replay driver for toolchains without libFuzzer: run each file argument once. */
int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        FILE *fp = fopen(argv[i], "rb");
        if (fp == nullptr) {
            continue;
        }
        (void)fseek(fp, 0, SEEK_END);
        long sz = ftell(fp);
        (void)fseek(fp, 0, SEEK_SET);
        if (sz > 0) {
            uint8_t *buf = malloc((size_t)sz);
            if (buf != nullptr && fread(buf, 1, (size_t)sz, fp) == (size_t)sz) {
                fuzz_one(buf, (size_t)sz);
            }
            free(buf);
        }
        fclose(fp);
        printf("ok: %s\n", argv[i]);
    }
    return 0;
}
#endif
