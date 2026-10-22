#include "app/pane.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "config/paths.h"
#include "util/log.h"
#include "util/mem.h"
#include "util/str.h"

#define PANE_READ_CHUNK     65536u
#define PANE_READ_BUDGET_NS (8u * 1000u * 1000u)

static uint64_t mono_ns(void)
{
    struct timespec ts = { 0 };
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void pane_setup_highlight(te_pane *p, const te_config *cfg)
{
    hl_destroy(&p->hl);
    if (hl_create(&p->hl) != TE_OK) {
        LOG_W("syntax highlighting unavailable (out of memory)");
        p->hl = nullptr;
        return;
    }
    hl_set_fallback(p->hl, cfg->hl_fallback);
    (void)hl_load_all(p->hl);
    te_err rc = TE_OK;
    if (strcmp(cfg->hl_mode, "off") == 0) {
        rc = hl_set_mode(p->hl, TE_HL_OFF, nullptr);
    } else if (strcmp(cfg->hl_mode, "auto") == 0) {
        rc = hl_set_mode(p->hl, TE_HL_AUTO, nullptr);
    } else {
        rc = hl_set_mode(p->hl, TE_HL_PINNED, cfg->hl_mode);
        if (rc != TE_OK) {
            LOG_W("highlight profile '%s' not found; using auto", cfg->hl_mode);
            rc = hl_set_mode(p->hl, TE_HL_AUTO, nullptr);
        }
    }
    (void)rc;
    (void)hl_take_changed(p->hl);   /* the initial activation is not news */
}

te_err pane_create(te_pane **out, const te_pane_spawn *sp)
{
    if (out == nullptr || sp == nullptr || sp->cfg == nullptr || sp->theme == nullptr) {
        return TE_ERR_INVAL;
    }
    *out = nullptr;
    te_pane *p = te_calloc(1, sizeof *p);
    if (p == nullptr) {
        return TE_ERR_NOMEM;
    }
    p->id = sp->id;
    p->owner = sp->owner;
    p->tag = -1;
    pty_init(&p->pty);
    te_term_host host = sp->host;
    host.user = p;
    te_err rc = term_create(&p->term, sp->cols, sp->rows, sp->cfg->scrollback, sp->theme, &host);
    if (rc != TE_OK) {
        goto fail;
    }
    te_pty_opts po = {
        .argv = sp->argv,
        .shell = sp->cfg->shell,
        .cwd = sp->cwd,
        .term = sp->cfg->term,
        .cols = sp->cols,
        .rows = sp->rows,
        .px_w = sp->px_w,
        .px_h = sp->px_h,
    };
    rc = pty_spawn(&p->pty, &po);
    if (rc != TE_OK) {
        goto fail;
    }
    if (search_create(&p->search) != TE_OK) {
        p->search = nullptr;   /* optional */
    }
    pane_setup_highlight(p, sp->cfg);
    *out = p;
    return TE_OK;

fail:
    pane_destroy(&p);
    return rc;
}

te_err pane_respawn(te_pane *p, const te_config *cfg, const char *const *argv, const char *cwd)
{
    if (p == nullptr || cfg == nullptr) {
        return TE_ERR_INVAL;
    }
    pty_destroy(&p->pty);
    pty_init(&p->pty);
    te_pty_opts po = {
        .argv = argv,
        .shell = cfg->shell,
        .cwd = cwd,
        .term = cfg->term,
        .cols = term_cols(p->term),
        .rows = term_rows(p->term),
        .px_w = 0,
        .px_h = 0,
    };
    te_err rc = pty_spawn(&p->pty, &po);
    if (rc != TE_OK) {
        return rc;
    }
    p->exited = false;
    p->status = 0;
    p->wq_len = 0;
    p->fd_ready = false;
    return TE_OK;
}

void pane_destroy(te_pane **pp)
{
    if (pp == nullptr || *pp == nullptr) {
        return;
    }
    te_pane *p = *pp;
    TE_FREE(p->sess);
    pty_destroy(&p->pty);
    hl_destroy(&p->hl);
    search_destroy(&p->search);
    term_destroy(&p->term);
    TE_FREE(p->wq);
    p->wq_len = 0;
    p->wq_cap = 0;
    TE_FREE(*pp);
}

void pane_flush(te_pane *p)
{
    while (p->wq_len > 0) {
        ssize_t n = pty_write(&p->pty, p->wq, p->wq_len);
        if (n <= 0) {
            if (n < 0) {
                p->wq_len = 0;   /* pty gone: drop pending input */
            }
            return;
        }
        size_t done = (size_t)n;
        memmove(p->wq, p->wq + done, p->wq_len - done);
        p->wq_len -= done;
    }
}

void pane_queue(te_pane *p, const void *data, size_t len)
{
    if (p == nullptr || data == nullptr || len == 0 || p->exited) {
        return;
    }
    if (len > TE_PANE_WRITE_MAX - p->wq_len) {
        LOG_W("input queue full; %zu bytes dropped", len);
        return;
    }
    size_t need = p->wq_len + len;
    if (need > p->wq_cap) {
        size_t ncap = p->wq_cap == 0 ? 4096 : p->wq_cap;
        while (ncap < need) {
            ncap *= 2;
        }
        if (ncap > TE_PANE_WRITE_MAX) {
            ncap = TE_PANE_WRITE_MAX;
        }
        void *tmp = p->wq;
        if (te_realloc_array(&tmp, p->wq_cap, ncap, 1) != TE_OK) {
            LOG_W("out of memory; input dropped");
            return;
        }
        p->wq = tmp;
        p->wq_cap = ncap;
    }
    memcpy(p->wq + p->wq_len, data, len);
    p->wq_len += len;
    pane_flush(p);
}

bool pane_read(te_pane *p, bool hold)
{
    if (p->exited) {
        return false;
    }
    uint8_t buf[PANE_READ_CHUNK];
    uint64_t start = mono_ns();
    for (;;) {
        ssize_t n = pty_read(&p->pty, buf, sizeof buf);
        if (n > 0) {
            term_feed(p->term, buf, (size_t)n);
        } else if (n == 0) {
            break;
        } else {
            /* The pty closes a moment before the child is gone: give it up to
             * 200 ms so the exit status (logout vs. dropped connection) is real. */
            int status = 0;
            for (int i = 0; i < 20 && !pty_reap(&p->pty, &status); i++) {
                struct timespec ts = { .tv_sec = 0, .tv_nsec = 10 * 1000 * 1000 };
                nanosleep(&ts, nullptr);
            }
            p->exited = true;
            p->status = status;
            p->wq_len = 0;
            if (hold) {
                static const char msg[] = "\r\n\x1b[2m[process exited]\x1b[0m";
                term_feed(p->term, (const uint8_t *)msg, sizeof msg - 1);
            }
            break;
        }
        if (mono_ns() - start >= PANE_READ_BUDGET_NS) {
            break;   /* let a frame through during floods */
        }
    }
    /* A fresh local shell prompt (OSC 133;A) means we left the device: reset auto detection. */
    uint32_t pc = term_prompt_count(p->term);
    if (pc != p->prompt_count) {
        p->prompt_count = pc;
        if (p->hl != nullptr) {
            hl_auto_reset(p->hl);
        }
    }
    size_t olen = 0;
    const uint8_t *reply = term_output(p->term, &olen);
    if (olen > 0) {
        pane_queue(p, reply, olen);
        term_output_consume(p->term, olen);
    }
    return !p->exited;
}

te_err pane_resize(te_pane *p, uint16_t cols, uint16_t rows, uint16_t px_w, uint16_t px_h)
{
    if (cols == term_cols(p->term) && rows == term_rows(p->term)) {
        return TE_OK;
    }
    sel_clear(&p->sel);
    te_err rc = term_resize(p->term, cols, rows);
    if (rc != TE_OK) {
        LOG_W("resize: %s", te_strerror(rc));
    }
    if (!p->exited) {
        (void)pty_resize(&p->pty, cols, rows, px_w, px_h);
    }
    return rc;
}

te_err pane_cwd(const te_pane *p, char *out, size_t outsz)
{
    const char *osc7 = term_cwd(p->term);
    if (osc7 != nullptr && osc7[0] == '/') {
        return str_copy(out, outsz, osc7);
    }
    if (p->pty.child <= 0) {
        return TE_ERR_NOTFOUND;
    }
    char link[64] = { 0 };
    char buf[TE_PATH_MAX] = { 0 };
    if (str_fmt(link, sizeof link, "/proc/%d/cwd", (int)p->pty.child) != TE_OK) {
        return TE_ERR_TRUNC;
    }
    ssize_t n = readlink(link, buf, sizeof buf - 1);
    if (n <= 0 || (size_t)n >= sizeof buf) {
        return TE_ERR_NOTFOUND;
    }
    buf[n] = '\0';
    return str_copy(out, outsz, buf);
}
