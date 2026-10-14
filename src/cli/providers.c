/* The model catalog pytte uses: the built-in one, models fetched by
 * `pytte update-catalog`, and providers from the user's config.json. */
#define _POSIX_C_SOURCE 200809L
#include "cli/cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Built once, for the life of the process: targets point into it. */
static pt_arena cat_ar;
static pt_ai_catalog cat;
static bool built;

const pt_ai_catalog *cli_catalog(void)
{
    return built ? &cat : pt_ai_builtin_catalog();
}

void cli_catalog_path(char *out, size_t cap)
{
    const char *xdg = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
    if (xdg && *xdg)
        snprintf(out, cap, "%s/pytte/models.bin", xdg);
    else
        snprintf(out, cap, "%s/.cache/pytte/models.bin", home ? home : ".");
}

/* ---- loading ---- */

typedef struct loader {
    const char *path;
    char *warn;
    size_t cap;
    pt_arena tmp;
    pt_buf rows; /* pt_model_entry, in catalog order */
} loader;

static void warnf(loader *L, const char *fmt, const char *a, const char *b)
{
    char msg[300];
    if (L->warn[0])
        return; /* the first problem is the one shown */
    snprintf(msg, sizeof msg, fmt, a, b ? b : "");
    snprintf(L->warn, L->cap, "%s: %s", L->path, msg);
}

/* A string member as a C string in ar; NULL when absent, not a string, or
 * holding a NUL. */
static const char *get_cstr(const pt_json *v, pt_arena *ar)
{
    pt_str s;
    if (!pt_json_is(v, PT_J_STR))
        return NULL;
    s = pt_json_str(v, ar);
    return memchr(s.p, 0, s.n) ? NULL : pt_arena_cstr(ar, s);
}

/* A non-negative integer below 2^32, or false. */
static bool get_u32(const pt_json *v, uint32_t *out)
{
    int64_t i;
    if (!pt_json_int(v, &i) || i < 0 || i > 0xffffffffLL)
        return false;
    *out = (uint32_t)i;
    return true;
}

static bool get_bool(const pt_json *v, bool *out)
{
    if (!pt_json_is(v, PT_J_TRUE) && !pt_json_is(v, PT_J_FALSE))
        return false;
    *out = pt_json_is(v, PT_J_TRUE);
    return true;
}

/* Dollars per million tokens to micro-dollars per million tokens. */
static bool get_cost(const pt_json *v, uint64_t *out)
{
    char num[64];
    double d;
    if (!pt_json_is(v, PT_J_NUM) || v->raw.n >= sizeof num)
        return false;
    memcpy(num, v->raw.p, v->raw.n);
    num[v->raw.n] = 0;
    d = strtod(num, NULL) * 1e6;
    if (!(d >= 0 && d < 1e18))
        return false;
    *out = (uint64_t)(d + 0.5);
    return true;
}

/* A level name ("none" or "off" for off) as its PT_EFF_* bit; 0 for any
 * other string. */
static uint8_t effort_bit(pt_str name)
{
    pt_reasoning r;
    if (pt_str_eqz(name, "none"))
        return PT_EFF_OFF;
    if (!pt_reasoning_parse(name, &r) || r == PT_REASON_DEFAULT)
        return 0;
    return (uint8_t)(1u << (r - PT_REASON_OFF));
}

/* An array of level names as PT_EFF_* bits. Unknown names are skipped
 * (sources list "default"); strict says they are an error. */
static bool effort_bits(const pt_json *arr, bool strict, uint8_t *out, pt_arena *ar)
{
    const pt_json *x;
    *out = 0;
    if (!pt_json_is(arr, PT_J_ARR))
        return false;
    for (x = arr->child; x; x = x->next) {
        uint8_t bit = pt_json_is(x, PT_J_STR) ? effort_bit(pt_json_str(x, ar)) : 0;
        if (!bit && strict)
            return false;
        *out |= bit;
    }
    return !strict || *out;
}

/* Members of obj not in the NULL-ended list, reported as typos. */
static void unknown_keys(loader *L, const char *where, const pt_json *obj, const char *const *known)
{
    const pt_json *m;
    for (m = obj->child; m; m = m->next) {
        const char *const *k = known;
        while (*k && !pt_str_eqz(m->key, *k))
            k++;
        if (!*k) {
            char key[64];
            snprintf(key, sizeof key, "%.*s", (int)(m->key.n > 60 ? 60 : m->key.n), m->key.p);
            warnf(L, "%s: unknown setting \"%s\"", where, key);
        }
    }
}

/* Up to cap - 1 bytes of s, not cutting a UTF-8 sequence. */
static void put_fit(char *dst, size_t cap, pt_str s)
{
    size_t n = s.n < cap ? s.n : cap - 1;
    if (n < s.n)
        while (n && ((unsigned char)s.p[n] & 0xc0) == 0x80)
            n--;
    memcpy(dst, s.p, n);
    dst[n] = 0;
}

/* A model row: v is {} (or NULL) with optional name, context_window,
 * max_tokens, reasoning, images and cost; the provider gives defaults. */
static bool model_row(loader *L, const char *where, uint8_t prov, const pt_provider *p, pt_str id,
                      const pt_json *v, pt_model_entry *e)
{
    static const char *const known[] = {"name",   "context_window", "max_tokens", "reasoning",
                                        "images", "cost",           "efforts",    NULL};
    static const char *const costs[] = {"input", "output", "cache_read", "cache_write", NULL};
    const pt_json *x;
    char idz[PT_MODEL_ID_CAP + 8];
    memset(e, 0, sizeof *e);
    put_fit(idz, sizeof idz, id);
    if (!id.n || id.n >= PT_MODEL_ID_CAP || memchr(id.p, 0, id.n)) {
        warnf(L, "%s: model id \"%s\" is empty or too long; skipped", where, idz);
        return false;
    }
    e->provider = prov;
    memcpy(e->id, id.p, id.n);
    put_fit(e->name, sizeof e->name, id);
    e->context_window = p->context_window;
    e->max_tokens = p->max_tokens;
    e->reasoning = p->reasoning;
    if (!v)
        return true;
    if (!pt_json_is(v, PT_J_OBJ)) {
        warnf(L, "%s: model \"%s\" is not an object; skipped", where, idz);
        return false;
    }
    unknown_keys(L, where, v, known);
    if ((x = pt_json_get(v, "name")) && pt_json_is(x, PT_J_STR))
        put_fit(e->name, sizeof e->name, pt_json_str(x, &L->tmp));
    if (((x = pt_json_get(v, "context_window")) && !get_u32(x, &e->context_window)) ||
        ((x = pt_json_get(v, "max_tokens")) && !get_u32(x, &e->max_tokens)) ||
        ((x = pt_json_get(v, "reasoning")) && !get_bool(x, &e->reasoning)) ||
        ((x = pt_json_get(v, "images")) && !get_bool(x, &e->images))) {
        warnf(L, "%s: model \"%s\" has a value of the wrong type; skipped", where, idz);
        return false;
    }
    if ((x = pt_json_get(v, "efforts")) && !effort_bits(x, true, &e->efforts, &L->tmp)) {
        warnf(L, "%s: model \"%s\": efforts is a list of off, minimal, low, medium, high, xhigh, max; skipped",
              where, idz);
        return false;
    }
    if ((x = pt_json_get(v, "cost"))) {
        uint64_t *c[4];
        int i;
        c[0] = &e->cost.input;
        c[1] = &e->cost.output;
        c[2] = &e->cost.cache_read;
        c[3] = &e->cost.cache_write;
        for (i = 0; i < 4; i++) {
            const pt_json *y = pt_json_is(x, PT_J_OBJ) ? pt_json_get(x, costs[i]) : NULL;
            if (!pt_json_is(x, PT_J_OBJ) || (y && !get_cost(y, c[i]))) {
                warnf(L, "%s: model \"%s\": cost is {\"input\": dollars per million tokens, ...}; skipped",
                      where, idz);
                return false;
            }
        }
    }
    return true;
}

/* Rows for a "models" value: an array of IDs, or an object of ID → model. */
static void add_models(loader *L, const char *where, uint8_t prov, const pt_provider *p, const pt_json *models)
{
    const pt_json *m;
    pt_model_entry e;
    if (pt_json_is(models, PT_J_ARR)) {
        for (m = models->child; m; m = m->next)
            if (!pt_json_is(m, PT_J_STR))
                warnf(L, "%s: models: an array of model IDs, or an object of ID to model", where, NULL);
            else if (model_row(L, where, prov, p, pt_json_str(m, &L->tmp), NULL, &e))
                pt_buf_put(&L->rows, &e, sizeof e);
    } else if (pt_json_is(models, PT_J_OBJ)) {
        for (m = models->child; m; m = m->next)
            if (model_row(L, where, prov, p, m->key, m, &e))
                pt_buf_put(&L->rows, &e, sizeof e);
    } else {
        warnf(L, "%s: models: an array of model IDs, or an object of ID to model", where, NULL);
    }
}

static int cmp_rows(const void *a, const void *b)
{
    return strcmp(((const pt_model_entry *)a)->id, ((const pt_model_entry *)b)->id);
}

/* Sort the rows of b from `from` by ID and drop repeats, keeping the first
 * of each (the catalog is binary searched). A merge sort, since it is
 * stable. */
static void settle(pt_buf *b, size_t from)
{
    pt_model_entry *r = (pt_model_entry *)(void *)(b->p + from), *src, *dst, *t, *tmp;
    size_t n = (b->n - from) / sizeof *r, w, lo, i, k = 0;
    if (n < 2)
        return;
    if (!(tmp = (pt_model_entry *)malloc(n * sizeof *tmp))) {
        b->oom = true;
        return;
    }
    src = r;
    dst = tmp;
    for (w = 1; w < n; w *= 2) {
        for (lo = 0; lo < n; lo += 2 * w) {
            size_t mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
            size_t x = lo, y = mid, o = lo;
            while (x < mid && y < hi)
                dst[o++] = cmp_rows(&src[y], &src[x]) < 0 ? src[y++] : src[x++];
            while (x < mid)
                dst[o++] = src[x++];
            while (y < hi)
                dst[o++] = src[y++];
        }
        t = src;
        src = dst;
        dst = t;
    }
    if (src != r)
        memcpy(r, src, n * sizeof *r);
    free(tmp);
    for (i = 0; i < n; i++)
        if (!k || strcmp(r[k - 1].id, r[i].id) != 0)
            r[k++] = r[i];
    b->n = from + k * sizeof *r;
}

static const char *const apis[] = {"openai-completions", "openai-responses", "anthropic-messages", NULL};

static bool one_of(pt_str s, const char *const *list, int *at)
{
    int i;
    for (i = 0; list[i]; i++)
        if (pt_str_eqz(s, list[i])) {
            *at = i;
            return true;
        }
    return false;
}

static bool parse_compat(loader *L, const char *where, const pt_json *v, pt_compat *c)
{
    static const char *const known[] = {"max_tokens_field", "system_role", "stream_usage", "reasoning_param",
                                        "reasoning_delta", "parallel_tool_calls", "mid_conversation_system",
                                        NULL};
    static const char *const maxtok[] = {"max_tokens", "max_completion_tokens", NULL};
    static const char *const sysrole[] = {"system", "developer", NULL};
    static const char *const rparam[] = {"none", "effort", "openrouter", NULL};
    static const char *const flags[] = {"stream_usage", "reasoning_delta", "parallel_tool_calls",
                                        "mid_conversation_system"};
    uint8_t *flag[4];
    const pt_json *x;
    int at, i;
    bool b;
    if (!pt_json_is(v, PT_J_OBJ)) {
        warnf(L, "%s: compat is not an object", where, NULL);
        return false;
    }
    unknown_keys(L, where, v, known);
    if ((x = pt_json_get(v, "max_tokens_field"))) {
        if (!one_of(pt_json_str(x, &L->tmp), maxtok, &at))
            goto bad;
        c->max_tokens_field = (uint8_t)(at == 0 ? PT_MAXTOK_MAX_TOKENS : PT_MAXTOK_MAX_COMPLETION_TOKENS);
    }
    if ((x = pt_json_get(v, "system_role"))) {
        if (!one_of(pt_json_str(x, &L->tmp), sysrole, &at))
            goto bad;
        c->system_role = (uint8_t)(at == 0 ? PT_SYSROLE_SYSTEM : PT_SYSROLE_DEVELOPER);
    }
    if ((x = pt_json_get(v, "reasoning_param"))) {
        if (!one_of(pt_json_str(x, &L->tmp), rparam, &at))
            goto bad;
        c->reasoning_param = (uint8_t)(at == 0 ? PT_RPARAM_NONE : at == 1 ? PT_RPARAM_EFFORT : PT_RPARAM_OPENROUTER);
    }
    flag[0] = &c->stream_usage;
    flag[1] = &c->reasoning_delta;
    flag[2] = &c->parallel_tool_calls;
    flag[3] = &c->mid_conversation_system;
    for (i = 0; i < 4; i++)
        if ((x = pt_json_get(v, flags[i]))) {
            if (!get_bool(x, &b))
                goto bad;
            *flag[i] = (uint8_t)(b ? PT_YES : PT_NO);
        }
    return true;
bad:
    warnf(L, "%s: compat has a value pytte does not know", where, NULL);
    return false;
}

/* A provider from config.json. False (with a warning) skips it. */
static bool parse_provider(loader *L, pt_str id, const pt_json *v, pt_provider *p, const pt_json **models)
{
    static const char *const known[] = {"name",       "base_url",  "api",     "auth",   "key_env", "headers",
                                        "context_window", "max_tokens", "reasoning", "efforts", "compat", "models",
                                        NULL};
    static const char *const auths[] = {"bearer", "x-api-key", "none", NULL};
    const pt_ai_catalog *b = pt_ai_builtin_catalog();
    char where[120];
    const pt_json *x;
    size_t i;
    int at;
    snprintf(where, sizeof where, "providers.%.*s", (int)(id.n > 60 ? 60 : id.n), id.p);
    memset(p, 0, sizeof *p);
    *models = NULL;
    if (!id.n || id.n > 60) {
        warnf(L, "%s: a provider id is 1 to 60 characters; skipped", where, NULL);
        return false;
    }
    for (i = 0; i < id.n; i++) {
        char c = id.p[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) {
            warnf(L, "%s: a provider id is letters, digits, - and _; skipped", where, NULL);
            return false;
        }
    }
    for (i = 0; i < b->nproviders; i++)
        if (pt_str_eqz(id, b->providers[i].id)) {
            warnf(L, "%s: a built-in provider; pick another id; skipped", where, NULL);
            return false;
        }
    if (!pt_json_is(v, PT_J_OBJ)) {
        warnf(L, "%s: not an object; skipped", where, NULL);
        return false;
    }
    unknown_keys(L, where, v, known);
    p->id = pt_arena_cstr(&cat_ar, id);
    p->name = get_cstr(pt_json_get(v, "name"), &cat_ar);
    if (!p->name)
        p->name = p->id;
    p->base_url = get_cstr(pt_json_get(v, "base_url"), &cat_ar);
    if (!p->base_url || (strncmp(p->base_url, "https://", 8) != 0 && strncmp(p->base_url, "http://", 7) != 0)) {
        warnf(L, "%s: base_url is required: http(s)://host/path, e.g. https://api.groq.com/openai/v1; skipped",
              where, NULL);
        return false;
    }
    p->api = apis[0];
    if ((x = pt_json_get(v, "api"))) {
        if (!one_of(pt_json_str(x, &L->tmp), apis, &at)) {
            warnf(L, "%s: api is openai-completions, openai-responses or anthropic-messages; skipped", where, NULL);
            return false;
        }
        p->api = apis[at];
    }
    p->auth = strcmp(p->api, "anthropic-messages") == 0 ? PT_AUTH_X_API_KEY : PT_AUTH_BEARER;
    if ((x = pt_json_get(v, "auth"))) {
        if (!one_of(pt_json_str(x, &L->tmp), auths, &at)) {
            warnf(L, "%s: auth is bearer, x-api-key or none; skipped", where, NULL);
            return false;
        }
        p->auth = at == 0 ? PT_AUTH_BEARER : at == 1 ? PT_AUTH_X_API_KEY : PT_AUTH_NONE;
    }
    if ((x = pt_json_get(v, "key_env"))) {
        const pt_json *k = pt_json_is(x, PT_J_ARR) ? x->child : x;
        for (i = 0; k && i < 4; i++, k = pt_json_is(x, PT_J_ARR) ? k->next : NULL)
            if (!(p->key_env[i] = get_cstr(k, &cat_ar)) || !*p->key_env[i]) {
                warnf(L, "%s: key_env is a variable name, or a list of up to 4; skipped", where, NULL);
                return false;
            }
        if (k || !i) {
            warnf(L, "%s: key_env is a variable name, or a list of up to 4; skipped", where, NULL);
            return false;
        }
    } else { /* "my-llm" → MY_LLM_API_KEY */
        char *e = (char *)pt_arena_alloc(&cat_ar, id.n + sizeof "_API_KEY");
        if (e) {
            for (i = 0; i < id.n; i++) {
                char c = id.p[i];
                e[i] = c >= 'a' && c <= 'z' ? (char)(c - 32) : c == '-' ? '_' : c;
            }
            memcpy(e + id.n, "_API_KEY", sizeof "_API_KEY");
        }
        p->key_env[0] = e;
    }
    if ((x = pt_json_get(v, "headers"))) {
        const pt_json *h;
        if (!pt_json_is(x, PT_J_OBJ) || x->len > 4) {
            warnf(L, "%s: headers is an object of up to 4 names to values; skipped", where, NULL);
            return false;
        }
        for (i = 0, h = x->child; h; h = h->next, i++) {
            p->headers[i].name = pt_arena_cstr(&cat_ar, h->key);
            if (!(p->headers[i].value = get_cstr(h, &cat_ar)) || !h->key.n) {
                warnf(L, "%s: headers is an object of up to 4 names to values; skipped", where, NULL);
                return false;
            }
        }
    }
    p->context_window = 128000;
    if (((x = pt_json_get(v, "context_window")) && !get_u32(x, &p->context_window)) ||
        ((x = pt_json_get(v, "max_tokens")) && !get_u32(x, &p->max_tokens)) ||
        ((x = pt_json_get(v, "reasoning")) && !get_bool(x, &p->reasoning))) {
        warnf(L, "%s: context_window and max_tokens are numbers, reasoning true or false; skipped", where, NULL);
        return false;
    }
    if ((x = pt_json_get(v, "efforts")) && !effort_bits(x, true, &p->efforts, &L->tmp)) {
        warnf(L, "%s: efforts is a list of off, minimal, low, medium, high, xhigh, max; skipped", where, NULL);
        return false;
    }
    if ((x = pt_json_get(v, "compat")) && !parse_compat(L, where, x, &p->compat))
        return false;
    *models = pt_json_get(v, "models");
    return !cat_ar.oom;
}

/* The built-in rows of provider `prov` (the table is ordered by provider). */
static const pt_model_entry *builtin_rows(uint8_t prov, uint32_t *n)
{
    const pt_ai_catalog *b = pt_ai_builtin_catalog();
    uint32_t lo = 0, hi;
    while (lo < b->nmodels && b->models[lo].provider < prov)
        lo++;
    for (hi = lo; hi < b->nmodels && b->models[hi].provider == prov; hi++)
        ;
    *n = hi - lo;
    return b->models + lo;
}

/* ---- models.bin ----
 * What `pytte update-catalog` fetched, ready to use: a header, then one
 * section per provider (its id and row count), then its rows. The rows
 * are pt_model_entry as this build lays it out, sorted by id without
 * repeats, provider 0. It is a cache: a file another build or machine
 * wrote fails the checks and is ignored. The checks keep every read in
 * bounds; there is no checksum (it cost more than the rest of the load),
 * since the file is replaced atomically. */
#define CAT_MAGIC "PYTTECAT"
/* Bump CAT_VERSION when pt_model_entry changes without changing size. */
enum { CAT_VERSION = 2, CAT_ORDER = 0x01020304 };

typedef struct cat_head {
    char magic[8];
    uint32_t version, order, entry_size, nsections;
    uint32_t pad[2];
    char updated[24];  /* "2026-10-13T08:58:26Z" */
} cat_head;

typedef struct cat_section {
    char provider[64];
    uint32_t nrows, pad;
} cat_section;

/* Every size here is a multiple of 8, so rows in a malloc'd file are
 * aligned. */
typedef char cat_aligned[(sizeof(cat_head) % 8 == 0 && sizeof(cat_section) % 8 == 0 &&
                          sizeof(pt_model_entry) % 8 == 0) ? 1 : -1];

static bool cat_ok(const char *p, size_t n)
{
    cat_head h;
    size_t at = sizeof h;
    uint32_t i, k;
    if (n < sizeof h)
        return false;
    memcpy(&h, p, sizeof h);
    if (memcmp(h.magic, CAT_MAGIC, 8) != 0 || h.version != CAT_VERSION || h.order != CAT_ORDER ||
        h.entry_size != sizeof(pt_model_entry))
        return false;
    for (i = 0; i < h.nsections; i++) {
        const cat_section *s = (const cat_section *)(const void *)(p + at);
        const pt_model_entry *r;
        if (n - at < sizeof *s || !memchr(s->provider, 0, sizeof s->provider) ||
            s->nrows > (n - at - sizeof *s) / sizeof *r)
            return false;
        r = (const pt_model_entry *)(const void *)(s + 1);
        for (k = 0; k < s->nrows; k++)
            if (!memchr(r[k].id, 0, sizeof r[k].id) || !memchr(r[k].name, 0, sizeof r[k].name) ||
                (k && strcmp(r[k - 1].id, r[k].id) >= 0))
                return false;
        at += sizeof *s + s->nrows * sizeof *r;
    }
    return at == n;
}

/* The fetched rows of a provider, or NULL. p passed cat_ok. */
static const pt_model_entry *cat_rows(const char *p, const char *id, uint32_t *n)
{
    cat_head h;
    size_t at = sizeof h;
    uint32_t i;
    if (!p)
        return NULL;
    memcpy(&h, p, sizeof h);
    for (i = 0; i < h.nsections; i++) {
        const cat_section *s = (const cat_section *)(const void *)(p + at);
        if (strcmp(s->provider, id) == 0) {
            *n = s->nrows;
            return (const pt_model_entry *)(const void *)(s + 1);
        }
        at += sizeof *s + s->nrows * sizeof(pt_model_entry);
    }
    return NULL;
}

/* Both sorted by id: the union, fetched values first, compat flags (which
 * no source records) the binary's. */
static void merge(pt_buf *b, uint8_t prov, const pt_model_entry *f, uint32_t nf, const pt_model_entry *own,
                  uint32_t n)
{
    uint32_t i = 0, j = 0;
    if (!pt_buf_reserve(b, ((size_t)nf + n) * sizeof *f))
        return;
    while (i < nf || j < n) {
        int c = i < nf && j < n ? strcmp(f[i].id, own[j].id) : i < nf ? -1 : 1;
        pt_model_entry e = c > 0 ? own[j] : f[i];
        if (c == 0)
            e.compat = own[j].compat;
        i += c <= 0;
        j += c >= 0;
        e.provider = prov;
        pt_buf_put(b, &e, sizeof e);
    }
}

void cli_catalog_load(const pt_json *providers, const char *path, char *warn, size_t cap)
{
    const pt_ai_catalog *b = pt_ai_builtin_catalog();
    char fpath[4096];
    pt_provider *ps;
    const pt_json *m;
    const pt_json **pmodels;
    loader L;
    char *fbin;
    size_t fn = 0, nps = b->nproviders, nmax, i;
    bool changed = false;

    if (built)
        pt_arena_free(&cat_ar);
    built = false;
    pt_arena_init(&cat_ar, pt_sys_allocator(), 0);
    memset(&L, 0, sizeof L);
    L.warn = warn;
    L.cap = cap;
    pt_arena_init(&L.tmp, pt_sys_allocator(), 0);
    pt_buf_init(&L.rows, pt_sys_allocator());

    cli_catalog_path(fpath, sizeof fpath);
    if ((fbin = pt_sys_read_regular_file(fpath, &fn)) && !cat_ok(fbin, fn)) {
        L.path = fpath;
        warnf(&L, "not a catalog this pytte wrote; ignored (pytte update-catalog writes it again)", NULL, NULL);
        free(fbin);
        fbin = NULL;
    }

    L.path = path;
    if (providers && !pt_json_is(providers, PT_J_OBJ)) {
        warnf(&L, "providers is not an object; ignored", NULL, NULL);
        providers = NULL;
    }
    nmax = b->nproviders + (providers ? providers->len : 0);
    ps = (pt_provider *)pt_arena_zalloc(&cat_ar, nmax * sizeof *ps);
    pmodels = (const pt_json **)pt_arena_zalloc(&cat_ar, nmax * sizeof *pmodels);
    if (!ps || !pmodels)
        goto done;
    memcpy(ps, b->providers, b->nproviders * sizeof *ps);
    for (m = providers ? providers->child : NULL; m; m = m->next) {
        if (nps >= 255) {
            warnf(&L, "too many providers; the rest are skipped", NULL, NULL);
            break;
        }
        if (parse_provider(&L, m->key, m, &ps[nps], &pmodels[nps])) {
            uint32_t k;
            for (k = (uint32_t)b->nproviders; k < nps && strcmp(ps[k].id, ps[nps].id) != 0; k++)
                ;
            if (k == nps)
                nps++;
        }
    }

    /* The rows, provider by provider. A built-in provider's fetched models
     * are merged with its built-in ones, so a binary newer than the fetch
     * keeps its own. A config provider's listed models replace fetched ones. */
    for (i = 0; i < nps; i++) {
        size_t from = L.rows.n;
        uint32_t nf = 0, n, k;
        const pt_model_entry *f = cat_rows(fbin, ps[i].id, &nf);
        uint8_t prov = (uint8_t)i;
        if (i < b->nproviders) {
            const pt_model_entry *own = builtin_rows(prov, &n);
            if (f) {
                merge(&L.rows, prov, f, nf, own, n);
                changed = true;
            } else {
                pt_buf_put(&L.rows, own, n * sizeof *own);
            }
        } else if (pmodels[i]) {
            char where[120];
            snprintf(where, sizeof where, "providers.%s", ps[i].id);
            add_models(&L, where, prov, &ps[i], pmodels[i]);
            settle(&L.rows, from);
            changed |= L.rows.n > from;
        } else if (f) {
            for (k = 0; k < nf; k++) {
                pt_model_entry e = f[k];
                e.provider = prov;
                pt_buf_put(&L.rows, &e, sizeof e);
            }
            changed |= nf > 0;
        }
    }

    if (L.rows.oom || cat_ar.oom)
        goto done;
    cat.providers = ps;
    cat.nproviders = (uint32_t)nps;
    if (changed) {
        pt_model_entry *rows = (pt_model_entry *)pt_arena_alloc(&cat_ar, L.rows.n);
        if (!rows)
            goto done;
        memcpy(rows, L.rows.p, L.rows.n);
        cat.models = rows;
        cat.nmodels = (uint32_t)(L.rows.n / sizeof *rows);
    } else { /* nothing changed: the static table, not a copy */
        cat.models = b->models;
        cat.nmodels = b->nmodels;
    }
    built = true;
done:
    if (!built) {
        L.path = path;
        warnf(&L, "out of memory loading providers; using the built-in catalog", NULL, NULL);
    }
    pt_buf_free(&L.rows);
    pt_arena_free(&L.tmp);
    free(fbin);
}

/* ---- update-catalog: models.dev and OpenRouter into models.bin ----
 * The same rules as scripts/gen_models.py: only models that call tools. */

/* pytte sends adaptive thinking, which starts with Claude 4.6; older
 * Claude models only take thinking budgets: claude-<family>-3 and -4-0..5. */
static bool old_claude(pt_str id)
{
    size_t i = 7;
    if (!pt_str_starts(id, PT_S("claude-")))
        return false;
    while (i < id.n && id.p[i] >= 'a' && id.p[i] <= 'z')
        i++;
    if (i == 7 || i >= id.n || id.p[i++] != '-')
        return false;
    if (i < id.n && id.p[i] == '3')
        i++;
    else if (i + 2 < id.n && id.p[i] == '4' && id.p[i + 1] == '-' && id.p[i + 2] >= '0' && id.p[i + 2] <= '5')
        i += 3;
    else
        return false;
    return i == id.n || id.p[i] == '-' || id.p[i] == '.';
}

/* A row with id, name (cut to fit) and the given limits; false when the id
 * does not fit. */
static bool new_row(pt_model_entry *e, pt_str id, pt_str name, const pt_json *context, const pt_json *output)
{
    memset(e, 0, sizeof *e);
    if (!id.n || id.n >= PT_MODEL_ID_CAP || memchr(id.p, 0, id.n))
        return false;
    memcpy(e->id, id.p, id.n);
    put_fit(e->name, sizeof e->name, name.n && !memchr(name.p, 0, name.n) ? name : id);
    if (context)
        get_u32(context, &e->context_window);
    if (output)
        get_u32(output, &e->max_tokens);
    return true;
}

static bool has(const pt_json *arr, const char *z, pt_arena *ar)
{
    const pt_json *x;
    for (x = pt_json_is(arr, PT_J_ARR) ? arr->child : NULL; x; x = x->next)
        if (pt_str_eqz(pt_json_str(x, ar), z))
            return true;
    return false;
}

/* The rules of scripts/gen_models.py (efforts_dev, efforts_openrouter):
 * a level counts only when the source lists it; no list is unknown (0). */
static uint8_t dev_efforts(const pt_json *options, pt_arena *ar)
{
    const pt_json *o;
    uint8_t bits = 0, b;
    bool toggle = false;
    for (o = pt_json_is(options, PT_J_ARR) ? options->child : NULL; o; o = o->next) {
        pt_str type = pt_json_gets(o, "type", ar);
        if (pt_str_eqz(type, "effort") && effort_bits(pt_json_get(o, "values"), false, &b, ar))
            bits |= b;
        toggle |= pt_str_eqz(type, "toggle");
    }
    if (!(bits & ~PT_EFF_OFF)) /* nothing but off (or "default"): unknown */
        return 0;
    return toggle ? (uint8_t)(bits | PT_EFF_OFF) : bits;
}

static uint8_t openrouter_efforts(const pt_json *reasoning, pt_arena *ar)
{
    bool mandatory = pt_json_truthy(pt_json_get(reasoning, "mandatory"));
    uint8_t bits = 0;
    if (!pt_json_is(reasoning, PT_J_OBJ))
        return 0;
    effort_bits(pt_json_get(reasoning, "supported_efforts"), false, &bits, ar);
    if (!bits) { /* OpenRouter maps the level itself */
        if (mandatory)
            return 0;
        bits = PT_EFF_LOW | PT_EFF_MEDIUM | PT_EFF_HIGH;
    }
    return mandatory ? bits : (uint8_t)(bits | PT_EFF_OFF);
}

/* A models.dev model (m, under its id) as a row of provider's; false when
 * it is skipped: no tools, deprecated, realtime. */
static bool dev_row(const pt_json *m, const char *provider, pt_model_entry *e, pt_arena *ar)
{
    const pt_json *limit = pt_json_get(m, "limit"), *cost = pt_json_get(m, "cost"), *x;
    if (!pt_json_truthy(pt_json_get(m, "tool_call")) || pt_str_eqz(pt_json_gets(m, "status", ar), "deprecated") ||
        pt_str_contains(m->key, PT_S("realtime")) ||
        !new_row(e, m->key, pt_json_gets(m, "name", ar), pt_json_get(limit, "context"), pt_json_get(limit, "output")))
        return false;
    e->reasoning = pt_json_truthy(pt_json_get(m, "reasoning")) &&
                   !(strcmp(provider, "anthropic") == 0 && old_claude(m->key));
    e->images = has(pt_json_get(pt_json_get(m, "modalities"), "input"), "image", ar);
    if ((x = pt_json_get(cost, "input")))
        get_cost(x, &e->cost.input);
    if ((x = pt_json_get(cost, "output")))
        get_cost(x, &e->cost.output);
    if ((x = pt_json_get(cost, "cache_read")))
        get_cost(x, &e->cost.cache_read);
    if ((x = pt_json_get(cost, "cache_write")))
        get_cost(x, &e->cost.cache_write);
    e->efforts = dev_efforts(pt_json_get(m, "reasoning_options"), ar);
    return true;
}

/* models.dev's models of one provider, sorted, into rows. */
static void from_models_dev(pt_buf *rows, const pt_json *dev, const char *provider, pt_arena *ar)
{
    const pt_json *ms = pt_json_get(pt_json_get(dev, provider), "models"), *m;
    pt_model_entry e;
    for (m = pt_json_is(ms, PT_J_OBJ) ? ms->child : NULL; m; m = m->next)
        if (dev_row(m, provider, &e, ar))
            pt_buf_put(rows, &e, sizeof e);
    settle(rows, 0);
}

/* The models a ChatGPT subscription serves through the Codex backend. No
 * source lists them, so like pi this is an explicit list, with the context
 * window the backend allows; the rest comes from models.dev's openai model
 * of the same id. Keep in step with CODEX in scripts/gen_models.py. */
static const struct {
    const char *id;
    uint32_t context;
} codex_models[] = {
    {"gpt-5.3-codex-spark", 128000}, {"gpt-5.5", 272000},   {"gpt-5.6-luna", 272000},
    {"gpt-5.6-sol", 272000},         {"gpt-5.6-terra", 272000}, {"gpt-6-astra", 272000},
    {"gpt-6-luna", 272000},          {"gpt-6-sol", 272000}, {"gpt-6.1-sol", 272000},
};
enum { CODEX_MAX_TOKENS = 128000 };

static void from_codex(pt_buf *rows, const pt_json *dev, pt_arena *ar)
{
    const pt_json *ms = pt_json_get(pt_json_get(dev, "openai"), "models"), *m;
    pt_model_entry e;
    size_t i;
    for (i = 0; i < sizeof codex_models / sizeof codex_models[0]; i++)
        if ((m = pt_json_get(ms, codex_models[i].id)) && dev_row(m, "openai-codex", &e, ar)) {
            e.context_window = codex_models[i].context;
            e.max_tokens = CODEX_MAX_TOKENS;
            pt_buf_put(rows, &e, sizeof e);
        }
    settle(rows, 0);
}

/* OpenRouter's prices are dollars per token, as strings: micro-dollars per
 * million tokens. */
static uint64_t price(pt_str s)
{
    char num[64], *end;
    double d;
    if (!s.n || s.n >= sizeof num)
        return 0;
    memcpy(num, s.p, s.n);
    num[s.n] = 0;
    d = strtod(num, &end) * 1e12;
    return *end || !(d >= 0 && d < 1e18) ? 0 : (uint64_t)(d + 0.5);
}

static void from_openrouter(pt_buf *rows, const pt_json *or, pt_arena *ar)
{
    const pt_json *data = pt_json_get(or, "data"), *m;
    for (m = pt_json_is(data, PT_J_ARR) ? data->child : NULL; m; m = m->next) {
        const pt_json *params = pt_json_get(m, "supported_parameters"), *pricing = pt_json_get(m, "pricing");
        pt_str prompt = pt_json_gets(pricing, "prompt", ar);
        pt_model_entry e;
        if (!has(params, "tools", ar) || (prompt.n && prompt.p[0] == '-') ||
            !new_row(&e, pt_json_gets(m, "id", ar), pt_json_gets(m, "name", ar), pt_json_get(m, "context_length"),
                     pt_json_get(pt_json_get(m, "top_provider"), "max_completion_tokens")))
            continue;
        e.reasoning = has(params, "reasoning", ar);
        e.images = has(pt_json_get(pt_json_get(m, "architecture"), "input_modalities"), "image", ar);
        e.cost.input = price(prompt);
        e.cost.output = price(pt_json_gets(pricing, "completion", ar));
        e.cost.cache_read = price(pt_json_gets(pricing, "input_cache_read", ar));
        e.cost.cache_write = price(pt_json_gets(pricing, "input_cache_write", ar));
        e.efforts = openrouter_efforts(pt_json_get(m, "reasoning"), ar);
        pt_buf_put(rows, &e, sizeof e);
    }
    settle(rows, 0);
}

/* rows as provider's section of out, and "provider N" in msg. */
static void section(pt_buf *out, uint32_t *nsections, const char *provider, pt_buf *rows, char *msg, size_t cap)
{
    cat_section s;
    size_t used = strlen(msg);
    memset(&s, 0, sizeof s);
    snprintf(s.provider, sizeof s.provider, "%s", provider);
    s.nrows = (uint32_t)(rows->n / sizeof(pt_model_entry));
    pt_buf_put(out, &s, sizeof s);
    pt_buf_puts(out, pt_buf_str(rows));
    pt_buf_reset(rows);
    (*nsections)++;
    snprintf(msg + used, cap - used, "%s%s %u", used ? ", " : "", provider, (unsigned)s.nrows);
}

bool cli_catalog_convert(pt_str dev_json, pt_str openrouter_json, const char *updated, pt_buf *out, char *msg,
                         size_t cap)
{
    const pt_ai_catalog *c = cli_catalog(), *b = pt_ai_builtin_catalog();
    pt_arena ar;
    pt_json *dev, *or;
    pt_json_err e;
    pt_buf rows;
    cat_head h;
    uint32_t i;
    bool ok = false;
    pt_arena_init(&ar, pt_sys_allocator(), 0);
    pt_buf_init(&rows, pt_sys_allocator());
    dev = pt_json_parse(dev_json, &ar, &e);
    if (!pt_json_is(dev, PT_J_OBJ) || !pt_json_is(pt_json_get(dev, "anthropic"), PT_J_OBJ)) {
        snprintf(msg, cap, "models.dev: %s", dev ? "not the expected JSON" : e.msg);
        goto done;
    }
    or = pt_json_parse(openrouter_json, &ar, &e);
    if (!pt_json_is(pt_json_get(or, "data"), PT_J_ARR)) {
        snprintf(msg, cap, "openrouter.ai: %s", or ? "not the expected JSON" : e.msg);
        goto done;
    }
    memset(&h, 0, sizeof h);
    memcpy(h.magic, CAT_MAGIC, 8);
    h.version = CAT_VERSION;
    h.order = CAT_ORDER;
    h.entry_size = sizeof(pt_model_entry);
    snprintf(h.updated, sizeof h.updated, "%s", updated);
    msg[0] = 0;
    pt_buf_reset(out);
    pt_buf_put(out, &h, sizeof h); /* filled in at the end */
    from_models_dev(&rows, dev, "anthropic", &ar);
    section(out, &h.nsections, "anthropic", &rows, msg, cap);
    from_models_dev(&rows, dev, "openai", &ar);
    section(out, &h.nsections, "openai", &rows, msg, cap);
    from_codex(&rows, dev, &ar);
    section(out, &h.nsections, "openai-codex", &rows, msg, cap);
    from_openrouter(&rows, or, &ar);
    section(out, &h.nsections, "openrouter", &rows, msg, cap);
    /* Providers from config.json that models.dev knows by the same id. */
    for (i = b->nproviders; i < c->nproviders; i++)
        if (pt_json_is(pt_json_get(pt_json_get(dev, c->providers[i].id), "models"), PT_J_OBJ)) {
            from_models_dev(&rows, dev, c->providers[i].id, &ar);
            section(out, &h.nsections, c->providers[i].id, &rows, msg, cap);
        }
    ok = !out->oom && !rows.oom && !ar.oom;
    if (!ok) {
        snprintf(msg, cap, "out of memory");
        goto done;
    }
    memcpy(out->p, &h, sizeof h);
done:
    pt_buf_free(&rows);
    pt_arena_free(&ar);
    return ok;
}
