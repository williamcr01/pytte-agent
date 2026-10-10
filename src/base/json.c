#include "pytte_base.h"

#include <string.h>

/* ---- UTF-8 ----------------------------------------------------------------- */

/* Length of the valid UTF-8 sequence at s[0..n), or 0 if invalid. */
static size_t utf8_len(const unsigned char *s, size_t n)
{
    unsigned c = s[0];
    if (c < 0x80)
        return 1;
    if (c >= 0xC2 && c <= 0xDF)
        return (n >= 2 && (s[1] & 0xC0) == 0x80) ? 2 : 0;
    if (c >= 0xE0 && c <= 0xEF) {
        unsigned lo = 0x80, hi = 0xBF;
        if (c == 0xE0)
            lo = 0xA0;
        if (c == 0xED)
            hi = 0x9F;
        return (n >= 3 && s[1] >= lo && s[1] <= hi && (s[2] & 0xC0) == 0x80) ? 3 : 0;
    }
    if (c >= 0xF0 && c <= 0xF4) {
        unsigned lo = 0x80, hi = 0xBF;
        if (c == 0xF0)
            lo = 0x90;
        if (c == 0xF4)
            hi = 0x8F;
        return (n >= 4 && s[1] >= lo && s[1] <= hi && (s[2] & 0xC0) == 0x80 &&
                (s[3] & 0xC0) == 0x80)
                   ? 4
                   : 0;
    }
    return 0;
}

static size_t utf8_put(char *out, uint32_t cp)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Four hex digits at s, or -1. */
static long hex4(const char *s, size_t n)
{
    long v = 0;
    size_t i;
    if (n < 4)
        return -1;
    for (i = 0; i < 4; i++) {
        int h = hexval(s[i]);
        if (h < 0)
            return -1;
        v = v * 16 + h;
    }
    return v;
}

/* Decode string contents (validated, or at least well formed) into out,
 * which needs room for n bytes. Lone surrogates become U+FFFD. */
static size_t unescape_to(const char *s, size_t n, char *out)
{
    size_t i = 0, o = 0;
    while (i < n) {
        char c = s[i];
        if (c != '\\' || i + 1 >= n) {
            out[o++] = c;
            i++;
            continue;
        }
        c = s[i + 1];
        i += 2;
        switch (c) {
        case 'b': out[o++] = '\b'; break;
        case 'f': out[o++] = '\f'; break;
        case 'n': out[o++] = '\n'; break;
        case 'r': out[o++] = '\r'; break;
        case 't': out[o++] = '\t'; break;
        case 'u': {
            long cp = hex4(s + i, n - i);
            if (cp < 0) {
                out[o++] = '?';
                break;
            }
            i += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                long lo = (i + 6 <= n && s[i] == '\\' && s[i + 1] == 'u') ? hex4(s + i + 2, n - i - 2) : -1;
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 6;
                } else {
                    cp = 0xFFFD;
                }
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                cp = 0xFFFD;
            }
            o += utf8_put(out + o, (uint32_t)cp);
            break;
        }
        default: out[o++] = c; break; /* " \ / */
        }
    }
    return o;
}

bool pt_json_unescape(pt_str s, pt_buf *b)
{
    if (!pt_buf_reserve(b, s.n))
        return false;
    b->n += unescape_to(s.p, s.n, b->p + b->n);
    return true;
}

/* ---- parser ---------------------------------------------------------------- */

typedef struct parser {
    const char *s;
    size_t n, i;
    pt_arena *a;
    pt_json_err *err;
} parser;

static pt_json *fail(parser *p, const char *msg)
{
    if (p->err && !p->err->msg) {
        p->err->pos = p->i;
        p->err->msg = msg;
    }
    return NULL;
}

static void skip_ws(parser *p)
{
    while (p->i < p->n) {
        char c = p->s[p->i];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
            break;
        p->i++;
    }
}

/* At an opening quote. On success p->i is past the closing quote. */
static bool scan_string(parser *p, bool *esc)
{
    *esc = false;
    p->i++;
    while (p->i < p->n) {
        unsigned char c = (unsigned char)p->s[p->i];
        if (c == '"') {
            p->i++;
            return true;
        }
        if (c < 0x20) {
            fail(p, "control character in string");
            return false;
        }
        if (c == '\\') {
            char e;
            *esc = true;
            if (p->i + 1 >= p->n)
                break;
            e = p->s[p->i + 1];
            if (e == 'u') {
                if (hex4(p->s + p->i + 2, p->n - p->i - 2) < 0) {
                    fail(p, "bad \\u escape");
                    return false;
                }
                p->i += 6;
            } else if (e == '"' || e == '\\' || e == '/' || e == 'b' || e == 'f' || e == 'n' ||
                       e == 'r' || e == 't') {
                p->i += 2;
            } else {
                fail(p, "bad escape");
                return false;
            }
            continue;
        }
        if (c < 0x80) {
            p->i++;
        } else {
            size_t l = utf8_len((const unsigned char *)p->s + p->i, p->n - p->i);
            if (!l) {
                fail(p, "invalid UTF-8");
                return false;
            }
            p->i += l;
        }
    }
    fail(p, "unterminated string");
    return false;
}

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static bool scan_number(parser *p)
{
    size_t i = p->i;
    const char *s = p->s;
    size_t n = p->n;
    if (i < n && s[i] == '-')
        i++;
    if (i < n && s[i] == '0') {
        i++;
    } else if (i < n && s[i] >= '1' && s[i] <= '9') {
        while (i < n && is_digit(s[i]))
            i++;
    } else {
        p->i = i;
        return false;
    }
    if (i < n && s[i] == '.') {
        i++;
        if (i >= n || !is_digit(s[i])) {
            p->i = i;
            return false;
        }
        while (i < n && is_digit(s[i]))
            i++;
    }
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < n && (s[i] == '+' || s[i] == '-'))
            i++;
        if (i >= n || !is_digit(s[i])) {
            p->i = i;
            return false;
        }
        while (i < n && is_digit(s[i]))
            i++;
    }
    p->i = i;
    return true;
}

static bool literal(parser *p, const char *word)
{
    size_t l = strlen(word);
    if (p->n - p->i < l || memcmp(p->s + p->i, word, l) != 0)
        return false;
    p->i += l;
    return true;
}

static pt_json *new_node(parser *p, pt_jtype t, size_t start)
{
    pt_json *v = (pt_json *)pt_arena_zalloc(p->a, sizeof *v);
    if (!v)
        return fail(p, "out of memory");
    v->type = t;
    v->raw.p = p->s + start;
    return v;
}

static pt_json *parse_value(parser *p, int depth);

static pt_json *parse_container(parser *p, int depth, bool obj)
{
    size_t start = p->i;
    pt_json *v = new_node(p, obj ? PT_J_OBJ : PT_J_ARR, start), *last = NULL;
    char close = obj ? '}' : ']';
    if (!v)
        return NULL;
    if (depth >= PT_JSON_MAX_DEPTH)
        return fail(p, "nesting too deep");
    p->i++;
    skip_ws(p);
    if (p->i < p->n && p->s[p->i] == close) {
        p->i++;
        v->raw.n = p->i - start;
        return v;
    }
    for (;;) {
        pt_json *child;
        pt_str key = {"", 0};
        skip_ws(p);
        if (obj) {
            bool esc;
            size_t ks;
            if (p->i >= p->n || p->s[p->i] != '"')
                return fail(p, "expected key");
            ks = p->i;
            if (!scan_string(p, &esc))
                return NULL;
            key.p = p->s + ks + 1;
            key.n = p->i - ks - 2;
            if (esc) {
                char *d = (char *)pt_arena_alloc(p->a, key.n + 1);
                if (!d)
                    return fail(p, "out of memory");
                key.n = unescape_to(key.p, key.n, d);
                d[key.n] = 0;
                key.p = d;
            }
            skip_ws(p);
            if (p->i >= p->n || p->s[p->i] != ':')
                return fail(p, "expected ':'");
            p->i++;
        }
        child = parse_value(p, depth + 1);
        if (!child)
            return NULL;
        child->key = key;
        if (last)
            last->next = child;
        else
            v->child = child;
        last = child;
        v->len++;
        skip_ws(p);
        if (p->i < p->n && p->s[p->i] == ',') {
            p->i++;
            continue;
        }
        if (p->i < p->n && p->s[p->i] == close) {
            p->i++;
            v->raw.n = p->i - start;
            return v;
        }
        return fail(p, obj ? "expected ',' or '}'" : "expected ',' or ']'");
    }
}

static pt_json *parse_value(parser *p, int depth)
{
    size_t start;
    pt_json *v;
    skip_ws(p);
    if (p->i >= p->n)
        return fail(p, "unexpected end");
    start = p->i;
    switch (p->s[p->i]) {
    case '{':
        return parse_container(p, depth, true);
    case '[':
        return parse_container(p, depth, false);
    case '"': {
        bool esc;
        if (!scan_string(p, &esc))
            return NULL;
        v = new_node(p, PT_J_STR, start);
        if (v)
            v->esc = esc;
        break;
    }
    case 't':
        if (!literal(p, "true"))
            return fail(p, "bad literal");
        v = new_node(p, PT_J_TRUE, start);
        break;
    case 'f':
        if (!literal(p, "false"))
            return fail(p, "bad literal");
        v = new_node(p, PT_J_FALSE, start);
        break;
    case 'n':
        if (!literal(p, "null"))
            return fail(p, "bad literal");
        v = new_node(p, PT_J_NULL, start);
        break;
    default:
        if (!scan_number(p))
            return fail(p, "bad value");
        v = new_node(p, PT_J_NUM, start);
        break;
    }
    if (v)
        v->raw.n = p->i - start;
    return v;
}

pt_json *pt_json_parse(pt_str src, pt_arena *a, pt_json_err *err)
{
    parser p;
    pt_json *v;
    p.s = src.p ? src.p : "";
    p.n = src.n;
    p.i = 0;
    p.a = a;
    p.err = err;
    if (err) {
        err->pos = 0;
        err->msg = NULL;
    }
    v = parse_value(&p, 0);
    if (!v)
        return NULL;
    skip_ws(&p);
    if (p.i != p.n)
        return fail(&p, "trailing data");
    return v;
}

/* ---- access ---------------------------------------------------------------- */

const pt_json *pt_json_get(const pt_json *obj, const char *key)
{
    const pt_json *c;
    pt_str k = pt_strz(key);
    if (!obj || obj->type != PT_J_OBJ)
        return NULL;
    for (c = obj->child; c; c = c->next)
        if (pt_str_eq(c->key, k))
            return c;
    return NULL;
}

const pt_json *pt_json_at(const pt_json *arr, uint32_t i)
{
    const pt_json *c;
    if (!arr || (arr->type != PT_J_ARR && arr->type != PT_J_OBJ))
        return NULL;
    for (c = arr->child; c && i; c = c->next)
        i--;
    return c;
}

bool pt_json_is(const pt_json *v, pt_jtype t)
{
    return v && v->type == t;
}

pt_str pt_json_str(const pt_json *v, pt_arena *a)
{
    pt_str s = {"", 0};
    char *d;
    if (!v || v->type != PT_J_STR)
        return s;
    s.p = v->raw.p + 1;
    s.n = v->raw.n - 2;
    if (!v->esc)
        return s;
    d = (char *)pt_arena_alloc(a, s.n + 1);
    if (!d) {
        s.p = "";
        s.n = 0;
        return s;
    }
    s.n = unescape_to(s.p, s.n, d);
    d[s.n] = 0;
    s.p = d;
    return s;
}

pt_str pt_json_gets(const pt_json *obj, const char *key, pt_arena *a)
{
    return pt_json_str(pt_json_get(obj, key), a);
}

bool pt_json_int(const pt_json *v, int64_t *out)
{
    pt_str r;
    uint64_t u;
    bool neg = false;
    if (!v || v->type != PT_J_NUM)
        return false;
    r = v->raw;
    if (r.n && r.p[0] == '-') {
        neg = true;
        r.p++;
        r.n--;
    }
    if (!pt_str_to_u64(r, &u))
        return false;
    if (neg) {
        if (u > (uint64_t)INT64_MAX + 1)
            return false;
        *out = u == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)u;
    } else {
        if (u > (uint64_t)INT64_MAX)
            return false;
        *out = (int64_t)u;
    }
    return true;
}

int64_t pt_json_geti(const pt_json *obj, const char *key, int64_t dflt)
{
    int64_t v;
    return pt_json_int(pt_json_get(obj, key), &v) ? v : dflt;
}

bool pt_json_truthy(const pt_json *v)
{
    return v && v->type == PT_J_TRUE;
}

/* ---- writer ---------------------------------------------------------------- */

void pt_json_quote(pt_buf *b, pt_str s)
{
    static const char hex[] = "0123456789abcdef";
    size_t i = 0, run = 0;
    pt_buf_reserve(b, s.n + 2);
    pt_buf_putc(b, '"');
    while (i < s.n) {
        unsigned char c = (unsigned char)s.p[i];
        const char *esc = NULL;
        char u[6];
        size_t l;
        if (c >= 0x20 && c != '"' && c != '\\' && c < 0x80) {
            i++;
            run++;
            continue;
        }
        if (c >= 0x80) {
            l = utf8_len((const unsigned char *)s.p + i, s.n - i);
            if (l) {
                i += l;
                run += l;
                continue;
            }
        }
        pt_buf_put(b, s.p + i - run, run);
        run = 0;
        switch (c) {
        case '"': esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\b': esc = "\\b"; break;
        case '\f': esc = "\\f"; break;
        case '\n': esc = "\\n"; break;
        case '\r': esc = "\\r"; break;
        case '\t': esc = "\\t"; break;
        default:
            if (c < 0x20) {
                u[0] = '\\';
                u[1] = 'u';
                u[2] = '0';
                u[3] = '0';
                u[4] = hex[c >> 4];
                u[5] = hex[c & 15];
                pt_buf_put(b, u, 6);
            } else {
                pt_buf_put(b, "\xEF\xBF\xBD", 3); /* invalid UTF-8 byte */
            }
        }
        if (esc)
            pt_buf_putz(b, esc);
        i++;
    }
    pt_buf_put(b, s.p + i - run, run);
    pt_buf_putc(b, '"');
}

void pt_jw_init(pt_jw *w, pt_buf *b)
{
    w->b = b;
    w->depth = 0;
    w->after_key = false;
    w->comma[0] = 0;
}

static void before_value(pt_jw *w)
{
    if (w->after_key) {
        w->after_key = false;
        return;
    }
    if (w->comma[w->depth])
        pt_buf_putc(w->b, ',');
    w->comma[w->depth] = 1;
}

static void open_container(pt_jw *w, char c)
{
    before_value(w);
    pt_buf_putc(w->b, c);
    if (w->depth >= PT_JSON_MAX_DEPTH) {
        w->b->oom = true; /* treat as a hard failure */
        return;
    }
    w->comma[++w->depth] = 0;
}

static void close_container(pt_jw *w, char c)
{
    pt_buf_putc(w->b, c);
    if (w->depth)
        w->depth--;
}

void pt_jw_obj(pt_jw *w) { open_container(w, '{'); }
void pt_jw_obj_end(pt_jw *w) { close_container(w, '}'); }
void pt_jw_arr(pt_jw *w) { open_container(w, '['); }
void pt_jw_arr_end(pt_jw *w) { close_container(w, ']'); }

void pt_jw_key(pt_jw *w, const char *k)
{
    pt_jw_keys(w, pt_strz(k));
}

void pt_jw_keys(pt_jw *w, pt_str k)
{
    if (w->comma[w->depth])
        pt_buf_putc(w->b, ',');
    w->comma[w->depth] = 1;
    pt_json_quote(w->b, k);
    pt_buf_putc(w->b, ':');
    w->after_key = true;
}

void pt_jw_str(pt_jw *w, pt_str s)
{
    before_value(w);
    pt_json_quote(w->b, s);
}

void pt_jw_strz(pt_jw *w, const char *z)
{
    pt_jw_str(w, pt_strz(z));
}

void pt_jw_int(pt_jw *w, int64_t v)
{
    before_value(w);
    pt_buf_i64(w->b, v);
}

void pt_jw_uint(pt_jw *w, uint64_t v)
{
    before_value(w);
    pt_buf_u64(w->b, v);
}

void pt_jw_bool(pt_jw *w, bool v)
{
    before_value(w);
    pt_buf_putz(w->b, v ? "true" : "false");
}

void pt_jw_null(pt_jw *w)
{
    before_value(w);
    pt_buf_putz(w->b, "null");
}

void pt_jw_raw(pt_jw *w, pt_str json)
{
    before_value(w);
    pt_buf_puts(w->b, json);
}
