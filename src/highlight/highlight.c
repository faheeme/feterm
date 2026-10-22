#include "highlight/highlight.h"

#include <dirent.h>
#include <string.h>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include "config/ini.h"
#include "config/paths.h"
#include "config/theme.h"
#include "util/log.h"
#include "util/mem.h"
#include "util/str.h"
#include "util/utf8.h"

#define HL_MAX_LINE_BYTES  8192u
#define HL_MAX_MATCHES     256u
#define HL_MAX_STRIKES     3u
#define HL_OVECTOR_PAIRS   16u
#define HL_CACHE_SIZE      2048u
#define HL_CACHE_SPANS     48u
#define HL_MATCH_LIMIT     100000u
#define HL_DEPTH_LIMIT     1000u
#define HL_HEAP_LIMIT_KIB  4096u
#define HL_NAME_MAX        64u

typedef struct hl_rule {
    char         name[HL_NAME_MAX];
    char        *pattern;        /* source text until compiled */
    pcre2_code  *re;
    te_hl_style  style;
    int          priority;
    uint8_t      group;
    bool         enabled;
    bool         ignore_case;
    uint8_t      strikes;
    char         origin[160];    /* "file:line" for diagnostics */
} hl_rule;

typedef struct hl_profile {
    char         name[HL_NAME_MAX];
    char         inherits[HL_MAX_INHERITS][HL_NAME_MAX];
    size_t       ninherits;
    hl_rule     *rules;
    size_t       nrules, cap;
    pcre2_code  *detect[HL_MAX_PATTERNS];
    size_t       ndetect;
    pcre2_code  *prompt[HL_MAX_PATTERNS];
    size_t       nprompt;
} hl_profile;

typedef struct hl_cache_entry {
    uint64_t   hash;
    uint32_t   gen;
    uint16_t   n;
    uint16_t   nspans;
    bool       used;
    te_hl_span spans[HL_CACHE_SPANS];
} hl_cache_entry;

struct te_highlighter {
    hl_profile            *profiles[HL_MAX_PROFILES];
    size_t                 nprofiles;
    te_hl_mode             mode;
    int                    active;             /* profile index, -1 = none */
    int                    fallback;           /* profile index, -1 = none */
    char                   fallback_name[HL_NAME_MAX];
    hl_rule               *rules[HL_MAX_RULES];/* flattened active rules, ascending priority */
    size_t                 nrules;
    uint32_t               gen;
    bool                   changed;

    pcre2_match_data      *md;
    pcre2_match_context   *mctx;
    pcre2_compile_context *cctx;
    pcre2_jit_stack       *jit;
    pcre2_code            *generic_prompt[2];

    hl_cache_entry        *cache;
    char                   text[HL_MAX_LINE_BYTES + 8];
    uint16_t               col_of[HL_MAX_LINE_BYTES + 8];
    uint16_t               paint[HL_MAX_COLS];
    te_hl_span             spans[HL_MAX_COLS];
    char                   msg[128];
};

/* ------------------------------------------------------------------------- */
/* Compilation */

static pcre2_code *compile(te_highlighter *h, const char *pattern, bool caseless, const char *origin)
{
    size_t len = strnlen(pattern, HL_MAX_REGEX + 1);
    if (len == 0 || len > HL_MAX_REGEX) {
        LOG_W("%s: regex empty or longer than %d bytes", origin, HL_MAX_REGEX);
        return nullptr;
    }
    int err = 0;
    PCRE2_SIZE off = 0;
    uint32_t opts = PCRE2_UTF | PCRE2_UCP | PCRE2_NEVER_BACKSLASH_C | (caseless ? PCRE2_CASELESS : 0u);
    pcre2_code *re = pcre2_compile((PCRE2_SPTR)pattern, len, opts, &err, &off, h->cctx);
    if (re == nullptr) {
        PCRE2_UCHAR msg[256] = { 0 };
        if (pcre2_get_error_message(err, msg, sizeof msg) < 0) {
            msg[0] = '\0';
        }
        LOG_W("%s: regex error at offset %zu: %s", origin, (size_t)off, (const char *)msg);
        return nullptr;
    }
    /* JIT is an optimisation; the interpreter is used if it is unavailable. */
    (void)pcre2_jit_compile(re, PCRE2_JIT_COMPLETE);
    return re;
}

/* ------------------------------------------------------------------------- */
/* Profiles */

static void profile_free(hl_profile **pp)
{
    if (pp == nullptr || *pp == nullptr) {
        return;
    }
    hl_profile *p = *pp;
    for (size_t i = 0; i < p->nrules; i++) {
        TE_FREE(p->rules[i].pattern);
        pcre2_code_free(p->rules[i].re);
        p->rules[i].re = nullptr;
    }
    TE_FREE(p->rules);
    for (size_t i = 0; i < p->ndetect; i++) {
        pcre2_code_free(p->detect[i]);
        p->detect[i] = nullptr;
    }
    for (size_t i = 0; i < p->nprompt; i++) {
        pcre2_code_free(p->prompt[i]);
        p->prompt[i] = nullptr;
    }
    TE_FREE(*pp);
}

static hl_rule *rule_get(hl_profile *p, const char *name, const char *origin)
{
    for (size_t i = 0; i < p->nrules; i++) {
        if (strcmp(p->rules[i].name, name) == 0) {
            return &p->rules[i];
        }
    }
    if (p->nrules >= HL_MAX_RULES) {
        LOG_W("%s: more than %d rules in one profile; rule ignored", origin, HL_MAX_RULES);
        return nullptr;
    }
    if (p->nrules == p->cap) {
        size_t ncap = p->cap == 0 ? 16 : p->cap * 2;
        void *tmp = p->rules;
        if (te_realloc_array(&tmp, p->cap, ncap, sizeof(hl_rule)) != TE_OK) {
            return nullptr;
        }
        p->rules = tmp;
        p->cap = ncap;
    }
    hl_rule *r = &p->rules[p->nrules];
    memset(r, 0, sizeof *r);
    if (str_copy(r->name, sizeof r->name, name) != TE_OK) {
        LOG_W("%s: rule name too long", origin);
        return nullptr;
    }
    r->enabled = true;
    (void)str_copy(r->origin, sizeof r->origin, origin);
    p->nrules++;
    return r;
}

/* Colour: palette name, colorN, bright-name, #rrggbb, or "default". */
static bool parse_hl_color(const char *s, uint32_t *out)
{
    static const char *const names[8] = { "black", "red", "green", "yellow", "blue", "magenta", "cyan", "white" };
    if (strcmp(s, "default") == 0 || strcmp(s, "none") == 0) {
        *out = TE_COLOR_DEFAULT;
        return true;
    }
    uint32_t rgb = 0;
    if (s[0] == '#') {
        if (!color_parse(s, &rgb)) {
            return false;
        }
        *out = TE_COLOR_RGB(rgb);
        return true;
    }
    if (strncmp(s, "color", 5) == 0) {
        long idx = 0;
        if (!str_to_long(s + 5, 0, 255, &idx)) {
            return false;
        }
        *out = TE_COLOR_PAL(idx);
        return true;
    }
    bool bright = false;
    if (strncmp(s, "bright-", 7) == 0 || strncmp(s, "bright_", 7) == 0) {
        bright = true;
        s += 7;
    }
    for (uint32_t i = 0; i < 8; i++) {
        if (strcmp(s, names[i]) == 0) {
            *out = TE_COLOR_PAL(bright ? i + 8u : i);
            return true;
        }
    }
    return false;
}

typedef struct load_ctx {
    te_highlighter *h;
    hl_profile     *p;
    const char     *file;   /* basename for messages */
} load_ctx;

static void set_attr(te_hl_style *st, uint16_t bit, const char *value, const char *origin)
{
    bool b = false;
    if (!str_to_bool(value, &b)) {
        LOG_W("%s: expected true/false", origin);
        return;
    }
    if (b) {
        st->attrs |= bit;
    } else {
        st->attrs &= (uint16_t)~bit;
    }
}

static te_err load_kv(void *user, const char *sec, const char *key, const char *value,
                      const char *path, unsigned line)
{
    (void)path;
    load_ctx *c = user;
    hl_profile *p = c->p;
    char origin[160] = { 0 };
    (void)str_fmt(origin, sizeof origin, "%s:%u", c->file, line);

    if (strcmp(sec, "profile") == 0) {
        if (strcmp(key, "name") == 0) {
            if (!paths_is_safe_name(value) || str_copy(p->name, sizeof p->name, value) != TE_OK) {
                LOG_W("%s: invalid profile name '%s'", origin, value);
            }
        } else if (strcmp(key, "inherits") == 0) {
            if (p->ninherits >= HL_MAX_INHERITS) {
                LOG_W("%s: at most %d inherits", origin, HL_MAX_INHERITS);
            } else if (str_copy(p->inherits[p->ninherits], HL_NAME_MAX, value) == TE_OK) {
                p->ninherits++;
            }
        } else if (strcmp(key, "detect") == 0 || strcmp(key, "prompt") == 0) {
            bool det = key[0] == 'd';
            size_t *n = det ? &p->ndetect : &p->nprompt;
            pcre2_code **arr = det ? p->detect : p->prompt;
            if (*n >= HL_MAX_PATTERNS) {
                LOG_W("%s: at most %d %s patterns", origin, HL_MAX_PATTERNS, key);
            } else {
                pcre2_code *re = compile(c->h, value, false, origin);
                if (re != nullptr) {
                    arr[(*n)++] = re;
                }
            }
        } else if (strcmp(key, "description") != 0) {
            LOG_W("%s: unknown [profile] key '%s'", origin, key);
        }
        return TE_OK;
    }

    if (strncmp(sec, "rule.", 5) != 0 || sec[5] == '\0') {
        LOG_W("%s: unknown section [%s]", origin, sec);
        return TE_OK;
    }
    hl_rule *r = rule_get(p, sec + 5, origin);
    if (r == nullptr) {
        return TE_OK;
    }
    long l = 0;
    bool b = false;
    if (strcmp(key, "regex") == 0) {
        TE_FREE(r->pattern);
        r->pattern = str_dup(value);
        if (r->pattern == nullptr) {
            return TE_ERR_NOMEM;
        }
        (void)str_copy(r->origin, sizeof r->origin, origin);
    } else if (strcmp(key, "fg") == 0 || strcmp(key, "bg") == 0) {
        uint32_t col = 0;
        if (!parse_hl_color(value, &col)) {
            LOG_W("%s: invalid colour '%s'", origin, value);
        } else if (key[0] == 'f') {
            r->style.fg = col;
        } else {
            r->style.bg = col;
        }
    } else if (strcmp(key, "bold") == 0) {
        set_attr(&r->style, TE_ATTR_BOLD, value, origin);
    } else if (strcmp(key, "italic") == 0) {
        set_attr(&r->style, TE_ATTR_ITALIC, value, origin);
    } else if (strcmp(key, "dim") == 0) {
        set_attr(&r->style, TE_ATTR_DIM, value, origin);
    } else if (strcmp(key, "strike") == 0) {
        set_attr(&r->style, TE_ATTR_STRIKE, value, origin);
    } else if (strcmp(key, "underline") == 0) {
        unsigned ul = TE_UL_NONE;
        if (strcmp(value, "curly") == 0) ul = TE_UL_CURLY;
        else if (strcmp(value, "double") == 0) ul = TE_UL_DOUBLE;
        else if (strcmp(value, "dotted") == 0) ul = TE_UL_DOTTED;
        else if (strcmp(value, "dashed") == 0) ul = TE_UL_DASHED;
        else if (str_to_bool(value, &b)) ul = b ? TE_UL_SINGLE : TE_UL_NONE;
        else LOG_W("%s: underline must be true/false/curly/double/dotted/dashed", origin);
        r->style.attrs = (uint16_t)(((unsigned)r->style.attrs & ~(unsigned)TE_ATTR_UL_MASK) | (ul << TE_ATTR_UL_SHIFT));
    } else if (strcmp(key, "priority") == 0) {
        if (str_to_long(value, -1000, 1000, &l)) r->priority = (int)l;
        else LOG_W("%s: priority must be -1000..1000", origin);
    } else if (strcmp(key, "group") == 0) {
        if (str_to_long(value, 0, HL_OVECTOR_PAIRS - 1, &l)) r->group = (uint8_t)l;
        else LOG_W("%s: group must be 0..%u", origin, HL_OVECTOR_PAIRS - 1);
    } else if (strcmp(key, "override") == 0) {
        if (str_to_bool(value, &b)) r->style.override = b;
        else LOG_W("%s: expected true/false", origin);
    } else if (strcmp(key, "ignore_case") == 0) {
        if (str_to_bool(value, &b)) r->ignore_case = b;
        else LOG_W("%s: expected true/false", origin);
    } else if (strcmp(key, "enabled") == 0) {
        if (str_to_bool(value, &b)) r->enabled = b;
        else LOG_W("%s: expected true/false", origin);
    } else {
        LOG_W("%s: unknown rule key '%s'", origin, key);
    }
    return TE_OK;
}

/* Parse one file into a new profile. Returns nullptr on failure. */
static hl_profile *load_file(te_highlighter *h, const char *path, const char *fname)
{
    hl_profile *p = te_calloc(1, sizeof *p);
    if (p == nullptr) {
        return nullptr;
    }
    load_ctx c = { .h = h, .p = p, .file = fname };
    te_err rc = ini_parse_file(path, load_kv, &c);
    if (rc != TE_OK) {
        LOG_W("highlight profile %s: %s", path, te_strerror(rc));
        profile_free(&p);
        return nullptr;
    }
    if (p->name[0] == '\0') {
        /* Default the name to the file stem. */
        char stem[HL_NAME_MAX] = { 0 };
        (void)str_copy(stem, sizeof stem, fname);
        char *dot = strrchr(stem, '.');
        if (dot != nullptr) {
            *dot = '\0';
        }
        (void)str_copy(p->name, sizeof p->name, stem);
    }
    /* Compile rules now that every option (ignore_case) is known. */
    for (size_t i = 0; i < p->nrules; i++) {
        hl_rule *r = &p->rules[i];
        if (r->pattern == nullptr) {
            if (r->enabled) {
                LOG_W("%s: rule '%s' has no regex", r->origin, r->name);
            }
            continue;
        }
        r->re = compile(h, r->pattern, r->ignore_case, r->origin);
        TE_FREE(r->pattern);
        if (r->re == nullptr) {
            r->enabled = false;
        }
    }
    return p;
}

static int find_profile(const te_highlighter *h, const char *name)
{
    if (name == nullptr) {
        return -1;
    }
    for (size_t i = 0; i < h->nprofiles; i++) {
        if (strcmp(h->profiles[i]->name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static bool is_ini(const char *name)
{
    size_t n = strlen(name);
    return n > 4 && strcmp(name + n - 4, ".ini") == 0 && paths_is_safe_name(name);
}

static void load_dir(te_highlighter *h, const char *base)
{
    char dir[TE_PATH_MAX] = { 0 };
    if (str_fmt(dir, sizeof dir, "%s/highlight", base) != TE_OK) {
        return;
    }
    DIR *d = opendir(dir);
    if (d == nullptr) {
        return;
    }
    struct dirent *e = nullptr;
    while ((e = readdir(d)) != nullptr) {
        if (!is_ini(e->d_name)) {
            continue;
        }
        char path[TE_PATH_MAX] = { 0 };
        if (str_fmt(path, sizeof path, "%s/%s", dir, e->d_name) != TE_OK) {
            continue;
        }
        hl_profile *p = load_file(h, path, e->d_name);
        if (p == nullptr) {
            continue;
        }
        int existing = find_profile(h, p->name);
        if (existing >= 0) {
            /* Later directories (the user's) replace earlier ones. */
            profile_free(&h->profiles[existing]);
            h->profiles[existing] = p;
        } else if (h->nprofiles < HL_MAX_PROFILES) {
            h->profiles[h->nprofiles++] = p;
        } else {
            LOG_W("%s: more than %d profiles; ignored", path, HL_MAX_PROFILES);
            profile_free(&p);
        }
    }
    closedir(d);
}

/* ------------------------------------------------------------------------- */
/* Activation: flatten inheritance into one priority-ordered rule list */

static void flatten(te_highlighter *h, int idx, unsigned depth, uint64_t *visiting)
{
    if (idx < 0 || (size_t)idx >= h->nprofiles || depth > 8 || idx >= 64) {
        return;
    }
    uint64_t bit = 1ull << (unsigned)idx;
    if ((*visiting & bit) != 0) {
        LOG_W("highlight profile '%s': inheritance cycle", h->profiles[idx]->name);
        return;
    }
    *visiting |= bit;
    hl_profile *p = h->profiles[idx];
    for (size_t i = 0; i < p->ninherits; i++) {
        int parent = find_profile(h, p->inherits[i]);
        if (parent < 0) {
            LOG_W("highlight profile '%s': inherits unknown profile '%s'", p->name, p->inherits[i]);
            continue;
        }
        flatten(h, parent, depth + 1, visiting);
    }
    for (size_t i = 0; i < p->nrules; i++) {
        hl_rule *r = &p->rules[i];
        size_t k = 0;
        for (; k < h->nrules; k++) {
            if (strcmp(h->rules[k]->name, r->name) == 0) {
                break;
            }
        }
        if (k < h->nrules) {
            h->rules[k] = r;            /* override the inherited rule */
        } else if (h->nrules < HL_MAX_RULES) {
            h->rules[h->nrules++] = r;
        }
    }
    *visiting &= ~bit;
}

static void activate(te_highlighter *h, int idx)
{
    h->active = idx;
    h->nrules = 0;
    if (idx >= 0) {
        uint64_t visiting = 0;
        flatten(h, idx, 0, &visiting);
        /* Stable insertion sort by priority, ascending (higher priority paints last). */
        for (size_t i = 1; i < h->nrules; i++) {
            hl_rule *r = h->rules[i];
            size_t j = i;
            while (j > 0 && h->rules[j - 1]->priority > r->priority) {
                h->rules[j] = h->rules[j - 1];
                j--;
            }
            h->rules[j] = r;
        }
    }
    h->gen++;
    h->changed = true;
}

/* ------------------------------------------------------------------------- */
/* Public: lifecycle */

te_err hl_create(te_highlighter **out)
{
    if (out == nullptr) {
        return TE_ERR_INVAL;
    }
    *out = nullptr;
    te_highlighter *h = te_calloc(1, sizeof *h);
    if (h == nullptr) {
        return TE_ERR_NOMEM;
    }
    h->active = -1;
    h->fallback = -1;
    h->mode = TE_HL_AUTO;
    h->cache = te_calloc(HL_CACHE_SIZE, sizeof *h->cache);
    h->md = pcre2_match_data_create(HL_OVECTOR_PAIRS, nullptr);
    h->mctx = pcre2_match_context_create(nullptr);
    h->cctx = pcre2_compile_context_create(nullptr);
    h->jit = pcre2_jit_stack_create(32 * 1024, 512 * 1024, nullptr);
    if (h->cache == nullptr || h->md == nullptr || h->mctx == nullptr || h->cctx == nullptr) {
        hl_destroy(&h);
        return TE_ERR_NOMEM;
    }
    (void)pcre2_set_match_limit(h->mctx, HL_MATCH_LIMIT);
    (void)pcre2_set_depth_limit(h->mctx, HL_DEPTH_LIMIT);
    (void)pcre2_set_heap_limit(h->mctx, HL_HEAP_LIMIT_KIB);
    (void)pcre2_set_parens_nest_limit(h->cctx, 64);
    (void)pcre2_set_max_pattern_length(h->cctx, HL_MAX_REGEX);
    if (h->jit != nullptr) {
        pcre2_jit_stack_assign(h->mctx, nullptr, h->jit);
    }
    /*
     * Generic prompt-prefix finders for click-to-position when the shell gives
     * no OSC 133 marks: a network prompt ("sw1(config-if)#", "user@r1>") or a
     * shell prompt ending in one of $ # % > followed by a space.
     */
    h->generic_prompt[0] = compile(h, "^[\\w.\\-/:@()\\[\\]{}~]{1,80}?[#>]", false, "builtin");
    h->generic_prompt[1] = compile(h, "^.{0,120}?[$#%>\\x{276F}\\x{00BB}]\\s", false, "builtin");
    *out = h;
    return TE_OK;
}

void hl_destroy(te_highlighter **hp)
{
    if (hp == nullptr || *hp == nullptr) {
        return;
    }
    te_highlighter *h = *hp;
    for (size_t i = 0; i < h->nprofiles; i++) {
        profile_free(&h->profiles[i]);
    }
    h->nprofiles = 0;
    for (size_t i = 0; i < TE_ARRAY_LEN(h->generic_prompt); i++) {
        pcre2_code_free(h->generic_prompt[i]);
        h->generic_prompt[i] = nullptr;
    }
    pcre2_match_data_free(h->md);
    pcre2_match_context_free(h->mctx);
    pcre2_compile_context_free(h->cctx);
    pcre2_jit_stack_free(h->jit);
    TE_FREE(h->cache);
    TE_FREE(*hp);
}

static int cmp_profiles(const void *a, const void *b)
{
    const hl_profile *const *pa = a;
    const hl_profile *const *pb = b;
    return strcmp((*pa)->name, (*pb)->name);
}

te_err hl_load_all(te_highlighter *h)
{
    if (h == nullptr) {
        return TE_ERR_INVAL;
    }
    char dirs[3][TE_PATH_MAX] = { { 0 } };
    size_t n = paths_data_dirs(dirs, 3);
    for (size_t i = 0; i < n; i++) {
        load_dir(h, dirs[i]);
    }
    /* Deterministic order regardless of readdir(). */
    qsort(h->profiles, h->nprofiles, sizeof h->profiles[0], cmp_profiles);
    LOG_D("loaded %zu highlight profiles", h->nprofiles);
    h->fallback = find_profile(h, h->fallback_name);
    if (h->mode == TE_HL_AUTO) {
        activate(h, h->fallback);
    }
    return h->nprofiles > 0 ? TE_OK : TE_ERR_NOTFOUND;
}

size_t hl_profile_count(const te_highlighter *h) { return h->nprofiles; }

const char *hl_profile_name(const te_highlighter *h, size_t i)
{
    return i < h->nprofiles ? h->profiles[i]->name : nullptr;
}

void hl_set_fallback(te_highlighter *h, const char *name)
{
    if (name == nullptr || strcmp(name, "none") == 0) {
        h->fallback_name[0] = '\0';
    } else {
        (void)str_copy(h->fallback_name, sizeof h->fallback_name, name);
    }
    h->fallback = find_profile(h, h->fallback_name);
}

te_err hl_set_mode(te_highlighter *h, te_hl_mode mode, const char *profile)
{
    if (mode == TE_HL_PINNED) {
        int idx = find_profile(h, profile);
        if (idx < 0) {
            return TE_ERR_NOTFOUND;
        }
        h->mode = mode;
        activate(h, idx);
        return TE_OK;
    }
    h->mode = mode;
    activate(h, mode == TE_HL_AUTO ? h->fallback : -1);
    return TE_OK;
}

te_hl_mode hl_mode(const te_highlighter *h) { return h->mode; }

const char *hl_active_name(const te_highlighter *h)
{
    if (h->mode == TE_HL_OFF || h->active < 0 || (size_t)h->active >= h->nprofiles) {
        return nullptr;
    }
    return h->profiles[h->active]->name;
}

void hl_auto_reset(te_highlighter *h)
{
    if (h->mode == TE_HL_AUTO && h->active != h->fallback) {
        activate(h, h->fallback);
    }
}

const char *hl_cycle(te_highlighter *h)
{
    if (h->mode == TE_HL_AUTO) {
        (void)hl_set_mode(h, TE_HL_OFF, nullptr);
    } else if (h->mode == TE_HL_OFF) {
        if (h->nprofiles > 0) {
            (void)hl_set_mode(h, TE_HL_PINNED, h->profiles[0]->name);
        } else {
            (void)hl_set_mode(h, TE_HL_AUTO, nullptr);
        }
    } else {
        size_t next = (size_t)(h->active + 1);
        if (next < h->nprofiles) {
            (void)hl_set_mode(h, TE_HL_PINNED, h->profiles[next]->name);
        } else {
            (void)hl_set_mode(h, TE_HL_AUTO, nullptr);
        }
    }
    const char *name = hl_active_name(h);
    switch (h->mode) {
    case TE_HL_OFF:
        (void)str_copy(h->msg, sizeof h->msg, "Highlighting: off");
        break;
    case TE_HL_AUTO:
        (void)str_fmt(h->msg, sizeof h->msg, "Highlighting: auto (%s)", name != nullptr ? name : "none");
        break;
    case TE_HL_PINNED:
        (void)str_fmt(h->msg, sizeof h->msg, "Highlighting: %s", name != nullptr ? name : "?");
        break;
    }
    return h->msg;
}

bool hl_take_changed(te_highlighter *h)
{
    bool c = h->changed;
    h->changed = false;
    return c;
}

const te_hl_style *hl_style(const te_highlighter *h, uint16_t idx)
{
    if (idx == 0 || idx > h->nrules) {
        return nullptr;
    }
    return &h->rules[idx - 1]->style;
}

/* ------------------------------------------------------------------------- */
/* Matching */

/* Build UTF-8 text for the line; col_of[byte] gives the cell column. Returns length. */
static size_t build_text(te_highlighter *h, const te_cell *cells, uint16_t n)
{
    size_t len = 0;
    for (uint16_t x = 0; x < n; x++) {
        const te_cell *c = &cells[x];
        if ((c->attrs & TE_ATTR_SPACER) != 0) {
            continue;
        }
        uint32_t cps[1 + TE_CELL_MAX_COMBINING] = { c->cp != 0 ? c->cp : ' ', c->comb[0], c->comb[1] };
        for (size_t k = 0; k < TE_ARRAY_LEN(cps); k++) {
            if (k > 0 && cps[k] == 0) {
                break;
            }
            char enc[4] = { 0 };
            size_t m = utf8_encode(cps[k], enc, sizeof enc);
            if (m == 0 || m > sizeof enc) {
                enc[0] = '?';
                m = 1;
            }
            if (len + m > HL_MAX_LINE_BYTES) {
                h->text[len] = '\0';
                return len;
            }
            for (size_t b = 0; b < m; b++) {
                h->text[len] = enc[b];
                h->col_of[len] = x;
                len++;
            }
        }
    }
    h->text[len] = '\0';
    return len;
}

/* Never-written cells at the end of a line are not text: `$` must match after the last character. */
static uint16_t trimmed_len(const te_cell *cells, uint16_t n)
{
    while (n > 0 && cells[n - 1].cp == 0) {
        n--;
    }
    return n;
}

static uint64_t line_hash(const te_cell *cells, uint16_t n)
{
    uint64_t hv = 1469598103934665603ull ^ n;
    for (uint16_t x = 0; x < n; x++) {
        uint32_t parts[3] = { cells[x].cp, cells[x].comb[0], cells[x].comb[1] };
        for (size_t k = 0; k < 3; k++) {
            hv ^= parts[k];
            hv *= 1099511628211ull;
        }
    }
    return hv;
}

static bool limit_error(int rc)
{
    return rc == PCRE2_ERROR_MATCHLIMIT || rc == PCRE2_ERROR_DEPTHLIMIT ||
           rc == PCRE2_ERROR_HEAPLIMIT || rc == PCRE2_ERROR_JIT_STACKLIMIT;
}

static bool matches(te_highlighter *h, pcre2_code *re, size_t len)
{
    if (re == nullptr) {
        return false;
    }
    return pcre2_match(re, (PCRE2_SPTR)h->text, len, 0, 0, h->md, h->mctx) >= 0;
}

/* Auto mode: switch profile if this new line identifies a device. */
static void detect(te_highlighter *h, size_t len)
{
    if (h->mode != TE_HL_AUTO || len == 0) {
        return;
    }
    for (size_t i = 0; i < h->nprofiles; i++) {
        if ((int)i == h->active) {
            continue;
        }
        hl_profile *p = h->profiles[i];
        for (size_t k = 0; k < p->ndetect; k++) {
            if (matches(h, p->detect[k], len)) {
                LOG_D("highlight: detected %s", p->name);
                activate(h, (int)i);
                return;
            }
        }
    }
    if (h->active != h->fallback && h->active >= 0) {
        return;   /* prompts only switch away from the fallback */
    }
    for (size_t i = 0; i < h->nprofiles; i++) {
        hl_profile *p = h->profiles[i];
        for (size_t k = 0; k < p->nprompt && (int)i != h->active; k++) {
            if (matches(h, p->prompt[k], len)) {
                LOG_D("highlight: prompt looks like %s", p->name);
                activate(h, (int)i);
                return;
            }
        }
    }
}

static void paint_rule(te_highlighter *h, hl_rule *r, uint16_t style, size_t len, uint16_t n)
{
    PCRE2_SIZE start = 0;
    for (unsigned count = 0; start <= len && count < HL_MAX_MATCHES; count++) {
        int rc = pcre2_match(r->re, (PCRE2_SPTR)h->text, len, start, 0, h->md, h->mctx);
        if (rc == PCRE2_ERROR_NOMATCH) {
            return;
        }
        if (rc < 0) {
            if (limit_error(rc) && ++r->strikes >= HL_MAX_STRIKES) {
                LOG_W("%s: rule '%s' exceeded the match limit %u times and was disabled "
                      "(the regex probably backtracks catastrophically)", r->origin, r->name, HL_MAX_STRIKES);
                r->enabled = false;
            }
            return;
        }
        const PCRE2_SIZE *ov = pcre2_get_ovector_pointer(h->md);
        uint32_t g = r->group;
        /* rc == 0 means the ovector was too small; group 0 is still valid. */
        if (g == 0 || (rc > 0 && g < (uint32_t)rc)) {
            PCRE2_SIZE s = ov[2 * g];
            PCRE2_SIZE e = ov[2 * g + 1];
            if (s != PCRE2_UNSET && e != PCRE2_UNSET && e > s && e <= len) {
                uint16_t c0 = h->col_of[s];
                uint16_t c1 = (uint16_t)(h->col_of[e - 1] + 1u);
                for (uint16_t x = c0; x < c1 && x < n && x < HL_MAX_COLS; x++) {
                    h->paint[x] = style;
                }
            }
        }
        PCRE2_SIZE ms = ov[0];
        PCRE2_SIZE me = ov[1];
        if (me <= ms) {
            /* Empty match: step one whole UTF-8 character forward. */
            start = ms + 1;
            while (start < len && ((unsigned char)h->text[start] & 0xC0u) == 0x80u) {
                start++;
            }
        } else {
            start = me;
        }
    }
}

size_t hl_line(te_highlighter *h, const te_cell *cells, uint16_t n, const te_hl_span **spans)
{
    *spans = h->spans;
    if (h->mode == TE_HL_OFF || cells == nullptr || n == 0) {
        return 0;
    }
    if (n > HL_MAX_COLS) {
        n = HL_MAX_COLS;
    }
    n = trimmed_len(cells, n);
    if (n == 0) {
        return 0;
    }
    uint64_t hv = line_hash(cells, n);
    hl_cache_entry *ce = &h->cache[hv % HL_CACHE_SIZE];
    if (ce->used && ce->hash == hv && ce->gen == h->gen && ce->n == n) {
        *spans = ce->spans;
        return ce->nspans;
    }

    size_t len = build_text(h, cells, n);
    detect(h, len);
    if (h->nrules == 0) {
        return 0;
    }
    memset(h->paint, 0, (size_t)n * sizeof h->paint[0]);
    for (size_t i = 0; i < h->nrules; i++) {
        hl_rule *r = h->rules[i];
        if (r->enabled && r->re != nullptr) {
            paint_rule(h, r, (uint16_t)(i + 1), len, n);
        }
    }
    /* Compress painted columns into runs. */
    size_t ns = 0;
    for (uint16_t x = 0; x < n;) {
        uint16_t s = h->paint[x];
        uint16_t x0 = x;
        while (x < n && h->paint[x] == s) {
            x++;
        }
        if (s != 0) {
            h->spans[ns++] = (te_hl_span){ .start = x0, .end = x, .style = s };
        }
    }
    if (ns <= HL_CACHE_SPANS) {
        ce->used = true;
        ce->hash = hv;
        ce->gen = h->gen;
        ce->n = n;
        ce->nspans = (uint16_t)ns;
        memcpy(ce->spans, h->spans, ns * sizeof h->spans[0]);
    }
    return ns;
}

int hl_prompt_end(te_highlighter *h, const te_cell *cells, uint16_t n)
{
    if (h == nullptr || cells == nullptr || n == 0) {
        return -1;
    }
    size_t len = build_text(h, cells, trimmed_len(cells, n > HL_MAX_COLS ? HL_MAX_COLS : n));
    for (size_t i = 0; i < TE_ARRAY_LEN(h->generic_prompt); i++) {
        if (h->generic_prompt[i] == nullptr) {
            continue;
        }
        int rc = pcre2_match(h->generic_prompt[i], (PCRE2_SPTR)h->text, len, 0, 0, h->md, h->mctx);
        if (rc < 0) {
            continue;
        }
        const PCRE2_SIZE *ov = pcre2_get_ovector_pointer(h->md);
        if (ov[1] == 0 || ov[1] > len) {
            continue;
        }
        return h->col_of[ov[1] - 1] + 1;
    }
    return -1;
}
