/* A JSON Schema subset validator for tool arguments (section 13.2):
 * type, properties, required, additionalProperties (false or a schema),
 * enum, items, minimum/maximum (integers), minLength/maxLength. Other
 * keywords are ignored. Recursion is bounded by the parser's depth limit. */
#include "pytte_base.h"

#include <string.h>

typedef struct sctx {
    char *err;
    size_t cap;
    char path[192];
    size_t plen;
    pt_arena *a;
} sctx;

static void put(char *dst, size_t cap, size_t *n, pt_str s)
{
    size_t i;
    for (i = 0; i < s.n && *n + 1 < cap; i++)
        dst[(*n)++] = s.p[i];
    dst[*n] = 0;
}

static bool fail(sctx *c, const char *msg, pt_str detail)
{
    size_t n = 0;
    if (!c->cap)
        return false;
    put(c->err, c->cap, &n, (pt_str){c->path, c->plen});
    put(c->err, c->cap, &n, PT_S(": "));
    put(c->err, c->cap, &n, pt_strz(msg));
    put(c->err, c->cap, &n, detail);
    return false;
}

static size_t push_key(sctx *c, pt_str key)
{
    size_t at = c->plen;
    put(c->path, sizeof c->path, &c->plen, PT_S("."));
    put(c->path, sizeof c->path, &c->plen, key);
    return at;
}

static size_t push_index(sctx *c, uint32_t i)
{
    char num[16];
    size_t at = c->plen, n = 0;
    uint32_t d = 1;
    while (i / d >= 10)
        d *= 10;
    num[n++] = '[';
    for (; d; d /= 10)
        num[n++] = (char)('0' + i / d % 10);
    num[n++] = ']';
    put(c->path, sizeof c->path, &c->plen, (pt_str){num, n});
    return at;
}

static bool is_integer(const pt_json *v)
{
    size_t i;
    if (v->type != PT_J_NUM)
        return false;
    for (i = 0; i < v->raw.n; i++)
        if (v->raw.p[i] == '.' || v->raw.p[i] == 'e' || v->raw.p[i] == 'E')
            return false;
    return true;
}

static bool type_is(const pt_json *v, pt_str t)
{
    switch (v->type) {
    case PT_J_NULL:
        return pt_str_eqz(t, "null");
    case PT_J_TRUE:
    case PT_J_FALSE:
        return pt_str_eqz(t, "boolean");
    case PT_J_NUM:
        return pt_str_eqz(t, "number") || (pt_str_eqz(t, "integer") && is_integer(v));
    case PT_J_STR:
        return pt_str_eqz(t, "string");
    case PT_J_ARR:
        return pt_str_eqz(t, "array");
    case PT_J_OBJ:
        return pt_str_eqz(t, "object");
    }
    return false;
}

static const char *type_name(const pt_json *v)
{
    static const char *const names[] = {"null", "boolean", "boolean", "number", "string", "array", "object"};
    return names[v->type];
}

static bool same_value(sctx *c, const pt_json *x, const pt_json *y)
{
    if (x->type != y->type)
        return false;
    if (x->type == PT_J_STR) {
        bool equal = pt_str_eq(pt_json_str(x, c->a), pt_json_str(y, c->a));
        return !c->a->oom && equal;
    }
    return pt_str_eq(x->raw, y->raw);
}

static uint64_t code_points(pt_str s)
{
    uint64_t n = 0;
    size_t i;
    for (i = 0; i < s.n; i++)
        n += ((unsigned char)s.p[i] & 0xC0) != 0x80;
    return n;
}

static bool check(sctx *c, const pt_json *s, const pt_json *v)
{
    const pt_json *k, *e;
    int64_t lim, have;
    if (!pt_json_is(s, PT_J_OBJ))
        return true; /* true, {} and anything odd accept everything */
    if ((k = pt_json_get(s, "type"))) {
        bool ok = false;
        if (k->type == PT_J_STR)
            ok = type_is(v, pt_json_str(k, c->a));
        else if (k->type == PT_J_ARR)
            for (e = k->child; e && !ok; e = e->next)
                ok = e->type == PT_J_STR && type_is(v, pt_json_str(e, c->a));
        else
            ok = true;
        if (!ok) {
            fail(c, "expected ", k->type == PT_J_STR ? pt_json_str(k, c->a) : k->raw);
            if (c->cap) {
                size_t n = strlen(c->err);
                put(c->err, c->cap, &n, PT_S(", got "));
                put(c->err, c->cap, &n, pt_strz(type_name(v)));
            }
            return false;
        }
    }
    if ((k = pt_json_get(s, "enum")) && k->type == PT_J_ARR) {
        for (e = k->child; e; e = e->next)
            if (same_value(c, e, v))
                break;
        if (!e)
            return fail(c, "must be one of ", k->raw);
    }
    if (v->type == PT_J_STR) {
        have = (int64_t)code_points(pt_json_str(v, c->a));
        if (pt_json_int(pt_json_get(s, "minLength"), &lim) && have < lim)
            return fail(c, "too short", PT_STR_NULL);
        if (pt_json_int(pt_json_get(s, "maxLength"), &lim) && have > lim)
            return fail(c, "too long", PT_STR_NULL);
    }
    /* Integers only: the portable code has no floating point. */
    if (v->type == PT_J_NUM && pt_json_int(v, &have)) {
        if (pt_json_int(pt_json_get(s, "minimum"), &lim) && have < lim)
            return fail(c, "must be at least ", pt_json_get(s, "minimum")->raw);
        if (pt_json_int(pt_json_get(s, "maximum"), &lim) && have > lim)
            return fail(c, "must be at most ", pt_json_get(s, "maximum")->raw);
    }
    if (v->type == PT_J_ARR && (k = pt_json_get(s, "items")) && k->type == PT_J_OBJ) {
        uint32_t i = 0;
        for (e = v->child; e; e = e->next, i++) {
            size_t at = push_index(c, i);
            if (!check(c, k, e))
                return false;
            c->plen = at;
            c->path[at] = 0;
        }
    }
    if (v->type == PT_J_OBJ) {
        const pt_json *props = pt_json_get(s, "properties"), *extra = pt_json_get(s, "additionalProperties");
        if ((k = pt_json_get(s, "required")) && k->type == PT_J_ARR)
            for (e = k->child; e; e = e->next)
                if (e->type == PT_J_STR) {
                    pt_str name = pt_json_str(e, c->a);
                    const pt_json *m;
                    for (m = v->child; m && !pt_str_eq(m->key, name); m = m->next)
                        ;
                    if (!m)
                        return fail(c, "missing required property ", name);
                }
        for (e = v->child; e; e = e->next) {
            const pt_json *ps = NULL, *p;
            size_t at;
            for (p = pt_json_is(props, PT_J_OBJ) ? props->child : NULL; p; p = p->next)
                if (pt_str_eq(p->key, e->key)) {
                    ps = p;
                    break;
                }
            if (!ps && extra && extra->type == PT_J_FALSE)
                return fail(c, "unknown property ", e->key);
            if (!ps)
                ps = extra;
            at = push_key(c, e->key);
            if (ps && !check(c, ps, e))
                return false;
            c->plen = at;
            c->path[at] = 0;
        }
    }
    return true;
}

bool pt_schema_check(const pt_json *schema, const pt_json *value, pt_arena *a, char *err, size_t cap)
{
    sctx c;
    memset(&c, 0, sizeof c);
    c.err = err;
    c.cap = cap;
    c.a = a;
    c.path[0] = '$';
    c.plen = 1;
    if (cap)
        err[0] = 0;
    if (!value)
        return fail(&c, "no value", PT_STR_NULL);
    if (!check(&c, schema, value))
        return false;
    if (a->oom)
        return fail(&c, "out of memory", PT_STR_NULL);
    return true;
}

const char *pt_json_type_name(const pt_json *v)
{
    return v ? type_name(v) : "nothing";
}
