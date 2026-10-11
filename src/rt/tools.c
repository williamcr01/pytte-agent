/* Tools in the runtime: the registry, the gate (permissions, section
 * 15.5) and running calls, with their output kept for the result: all of
 * it up to OUT_CAP, else its head and tail, with the whole output in a
 * blob (section 12.2). */
#define _POSIX_C_SOURCE 200809L
#include "rt/rt_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- registry ---- */

bool pt_rt_tool_add(pt_rt *rt, const pt_rt_tool *t)
{
    uint32_t cap = rt->tcap;
    if (!pt_rt_grow((void **)&rt->tools, &rt->tcap, rt->ntools + 1, sizeof *rt->tools))
        return false;
    if (cap != rt->tcap) {
        pt_tool_def *d = (pt_tool_def *)realloc(rt->defs, rt->tcap * sizeof *d);
        if (!d)
            return false;
        rt->defs = d;
    }
    rt->tools[rt->ntools] = *t;
    rt->defs[rt->ntools++] = t->def;
    return true;
}

const pt_tool_def *pt_rt_tool_defs(pt_rt *rt, uint32_t *n)
{
    *n = rt->ntools;
    return rt->defs;
}

static int find_tool(pt_rt *rt, pt_str name)
{
    uint32_t i;
    for (i = 0; i < rt->ntools; i++)
        if (pt_str_eq(rt->tools[i].def.name, name))
            return (int)i;
    return -1;
}

/* ---- permissions ---- */

static const char *const perm_names[] = {"ask", "auto", "read-only"};

bool pt_perm_parse(pt_str s, pt_perm_mode *out)
{
    unsigned i;
    for (i = 0; i < 3; i++)
        if (pt_str_eqz(s, perm_names[i])) {
            *out = (pt_perm_mode)i;
            return true;
        }
    return false;
}

const char *pt_perm_name(pt_perm_mode m)
{
    return perm_names[m];
}

void pt_rt_set_perm(pt_rt *rt, pt_perm_mode mode)
{
    rt->perm = mode;
}

static char *dupz(pt_str s)
{
    char *p = (char *)malloc(s.n + 1);
    if (p) {
        if (s.n)
            memcpy(p, s.p, s.n);
        p[s.n] = 0;
    }
    return p;
}

bool pt_rt_allow(pt_rt *rt, pt_str tool, pt_str prefix)
{
    rt_rule *r;
    if (!pt_rt_grow((void **)&rt->rules, &rt->rucap, rt->nrules + 1, sizeof *rt->rules))
        return false;
    r = &rt->rules[rt->nrules];
    r->tool = dupz(tool);
    r->prefix = dupz(prefix);
    if (!r->tool || !r->prefix) {
        free(r->tool);
        free(r->prefix);
        return false;
    }
    rt->nrules++;
    return true;
}

pt_str pt_rt_gist(pt_str args, pt_arena *a)
{
    const pt_json *j = pt_json_parse(args, a, NULL), *c;
    if (j && j->type == PT_J_OBJ)
        for (c = j->child; c; c = c->next)
            if (c->type == PT_J_STR)
                return pt_json_str(c, a);
    return PT_STR_NULL;
}

/* A rule with a prefix allows "git status", not "git status; rm -rf x":
 * gists that could run more than one command never match one. */
static bool allowed(pt_rt *rt, pt_str name, pt_str gist)
{
    uint32_t i;
    size_t k;
    bool plain = true;
    for (k = 0; k < gist.n && plain; k++)
        plain = !strchr(";&|<>()`$\n\r", gist.p[k]) && gist.p[k];
    for (i = 0; i < rt->nrules; i++) {
        pt_str p = pt_strz(rt->rules[i].prefix);
        if (!pt_str_eqz(name, rt->rules[i].tool))
            continue;
        if (!p.n || (plain && pt_str_starts(gist, p) && (gist.n == p.n || gist.p[p.n] == ' ')))
            return true;
    }
    return false;
}

void pt_rt_gate(pt_rt *rt, const pt_fx *fx)
{
    int k = find_tool(rt, fx->name);
    pt_gate g;
    pt_str gist;
    pt_arena_reset(&rt->ar);
    gist = pt_rt_gist(fx->args, &rt->ar);
    memset(&g, 0, sizeof g);
    g.kind = PT_GATE_ALLOW;
    if (k < 0 || rt->tools[k].def.read_only || rt->perm == PT_PERM_AUTO || allowed(rt, fx->name, gist)) {
        /* allow; an unknown tool fails when it runs */
    } else if (rt->perm == PT_PERM_READ_ONLY) {
        g.kind = PT_GATE_DENY;
        g.text = PT_S("pytte runs in read-only mode; this tool may change things");
    } else {
        g.kind = PT_GATE_ASK;
        g.text = gist;
    }
    pt_fx_gate(rt->a, fx->id, &g);
}

/* ---- calls ---- */

static rt_call *find_call(pt_rt *rt, uint32_t fx)
{
    uint32_t i;
    for (i = 0; i < rt->ncalls; i++)
        if (rt->calls[i].fx == fx)
            return &rt->calls[i];
    return NULL;
}

void pt_rt_run_tool(pt_rt *rt, const pt_fx *fx)
{
    int k = find_tool(rt, fx->name);
    rt_call *c;
    if (k < 0) {
        pt_tool_result r;
        memset(&r, 0, sizeof r);
        r.content = PT_S("tool_unavailable: this tool is not available here.");
        r.is_error = true;
        pt_fx_tool(rt->a, fx->id, &r);
        return;
    }
    if (!pt_rt_grow((void **)&rt->calls, &rt->ccap, rt->ncalls + 1, sizeof *rt->calls)) {
        pt_tool_result r;
        memset(&r, 0, sizeof r);
        r.content = PT_S("out of memory starting the tool");
        r.is_error = true;
        pt_fx_tool(rt->a, fx->id, &r);
        return;
    }
    c = &rt->calls[rt->ncalls];
    if (rt->ncalls == rt->cinit) { /* a fresh slot */
        memset(c, 0, sizeof *c);
        pt_buf_init(&c->head, pt_sys_allocator());
        pt_buf_init(&c->tail, pt_sys_allocator());
        rt->cinit++;
    }
    rt->ncalls++;
    c->fx = fx->id;
    c->tool = (uint32_t)k;
    c->started = pt_rt_now(rt);
    c->total = 0;
    c->blob = NULL;
    c->nospill = false;
    pt_buf_reset(&c->head);
    pt_buf_reset(&c->tail);
    rt->tools[k].run(rt->tools[k].ud, rt, fx);
}

void pt_rt_cancel_tool(pt_rt *rt, uint32_t fx)
{
    rt_call *c = find_call(rt, fx);
    if (c && rt->tools[c->tool].cancel)
        rt->tools[c->tool].cancel(rt->tools[c->tool].ud, rt, fx);
}

static void blob_abort(pt_rt *rt, rt_call *c)
{
    if (c->blob)
        rt->c.store->blob_abort(rt->c.store, c->blob);
    c->blob = NULL;
}

static bool blob_put(pt_rt *rt, rt_call *c, const char *p, size_t n)
{
    if (rt->c.store->blob_write(rt->c.store, c->blob, p, n) != 0) {
        blob_abort(rt, c); /* the result still has the head and tail */
        c->nospill = true;
        return false;
    }
    pt_sha256_update(&c->sha, p, n);
    return true;
}

/* Before keep(): once the output passes OUT_CAP, start a blob with
 * everything so far (all still in head) and stream the rest into it. */
static void spill(pt_rt *rt, rt_call *c, const char *p, size_t n)
{
    pt_store *st = rt->c.store;
    if (c->nospill)
        return;
    if (!c->blob) {
        if (c->total + n <= OUT_CAP)
            return;
        c->blob = st->blob_begin ? st->blob_begin(st) : NULL;
        if (!c->blob) {
            c->nospill = true;
            return;
        }
        pt_sha256_init(&c->sha);
        if (!blob_put(rt, c, c->head.p, c->head.n))
            return;
    }
    blob_put(rt, c, p, n);
}

/* Keep p in the call's head and tail. Amortized O(n): the tail buffer
 * holds up to twice OUT_TAIL bytes before its newest OUT_TAIL move down. */
static void keep(rt_call *c, const char *p, size_t n)
{
    c->total += n;
    if (c->head.n < OUT_CAP && !c->tail.n) {
        size_t room = OUT_CAP - c->head.n, take = n < room ? n : room;
        pt_buf_put(&c->head, p, take);
        p += take;
        n -= take;
        if (!n)
            return;
        /* Over the cap: everything after OUT_HEAD becomes the tail. */
        pt_buf_put(&c->tail, c->head.p + OUT_HEAD, c->head.n - OUT_HEAD);
        c->head.n = OUT_HEAD;
    }
    if (n >= OUT_TAIL) {
        pt_buf_reset(&c->tail);
        pt_buf_put(&c->tail, p + n - OUT_TAIL, OUT_TAIL);
        return;
    }
    if (c->tail.n + n > 2 * OUT_TAIL) {
        size_t k = OUT_TAIL - n;
        memmove(c->tail.p, c->tail.p + c->tail.n - k, k);
        c->tail.n = k;
    }
    pt_buf_put(&c->tail, p, n);
}

void pt_rt_tool_output(pt_rt *rt, uint32_t fx, const void *p, size_t n)
{
    rt_call *c = find_call(rt, fx);
    if (!c || !n)
        return;
    spill(rt, c, (const char *)p, n);
    keep(c, (const char *)p, n);
    if (!rt->closing)
        pt_fx_data(rt->a, fx, p, n);
}

/* The kept output: whole, or head, a marker and tail. The marker names
 * the file with all of it, when there is one. */
static void kept_text(const rt_call *c, const char *path, pt_buf *out)
{
    pt_str tail = pt_buf_str(&c->tail);
    uint64_t omitted;
    if (!c->tail.n) {
        pt_buf_puts(out, pt_buf_str(&c->head));
        return;
    }
    if (tail.n > OUT_TAIL) {
        tail.p += tail.n - OUT_TAIL;
        tail.n = OUT_TAIL;
    }
    while (tail.n && ((unsigned char)*tail.p & 0xC0) == 0x80) { /* start on a character */
        tail.p++;
        tail.n--;
    }
    omitted = c->total - c->head.n - tail.n;
    pt_buf_puts(out, pt_buf_str(&c->head));
    pt_buf_putz(out, "\n\n…[");
    pt_buf_u64(out, omitted);
    pt_buf_putz(out, " bytes omitted");
    if (path[0]) {
        pt_buf_putz(out, "; the whole output is in ");
        pt_buf_putz(out, path);
    }
    pt_buf_putz(out, "]…\n\n");
    pt_buf_puts(out, tail);
}

/* Seal the call's blob: true with its reference merged into details and
 * its file in path ("" when the store has none). The blob is durable
 * before the result that names it is committed. */
static bool blob_seal(pt_rt *rt, rt_call *c, pt_str *details, char *path, size_t cap)
{
    static const char digits[] = "0123456789abcdef";
    unsigned char d[32];
    char hex[65];
    pt_str in = *details;
    pt_buf *b = &rt->scratch;
    size_t i;
    if (!c->blob)
        return false;
    if (rt->closing) {
        blob_abort(rt, c);
        return false;
    }
    pt_sha256_final(&c->sha, d);
    for (i = 0; i < 32; i++) {
        hex[2 * i] = digits[d[i] >> 4];
        hex[2 * i + 1] = digits[d[i] & 15];
    }
    hex[64] = 0;
    i = (size_t)rt->c.store->blob_commit(rt->c.store, c->blob, hex);
    c->blob = NULL;
    if (i != 0)
        return false;
    if (rt->c.store->blob_path && rt->c.store->blob_path(rt->c.store, hex, path, cap) != 0)
        path[0] = 0;
    /* {"blob":"sha256:…","size":N, and the tool's own fields} */
    pt_buf_putz(b, "{\"blob\":\"sha256:");
    pt_buf_put(b, hex, 64);
    pt_buf_putz(b, "\",\"size\":");
    pt_buf_u64(b, c->total);
    in = pt_str_trim(in);
    if (in.n >= 2 && in.p[0] == '{' && in.p[in.n - 1] == '}') {
        in = pt_str_trim((pt_str){in.p + 1, in.n - 2});
        if (in.n) {
            pt_buf_putz(b, ",");
            pt_buf_puts(b, in);
        }
    }
    pt_buf_putz(b, "}");
    return true;
}

void pt_rt_tool_done(pt_rt *rt, uint32_t fx, const pt_rt_out *r)
{
    rt_call *c = find_call(rt, fx), done;
    pt_tool_result res;
    pt_str details = r->details;
    size_t dn = 0;
    char path[4200] = ""; /* the blob's file */
    bool stored;
    uint32_t i;
    if (!c)
        return;
    pt_buf_reset(&rt->scratch);
    if (r->content.p) { /* given whole: cut it the same way */
        blob_abort(rt, c);
        c->nospill = false;
        pt_buf_reset(&c->head);
        pt_buf_reset(&c->tail);
        c->total = 0;
        spill(rt, c, r->content.p, r->content.n);
        keep(c, r->content.p, r->content.n);
    }
    /* scratch: the merged details, then the text */
    stored = blob_seal(rt, c, &details, path, sizeof path);
    if (stored)
        dn = rt->scratch.n;
    kept_text(c, path, &rt->scratch);
    pt_buf_puts(&rt->scratch, r->note);
    if (rt->scratch.n == dn)
        pt_buf_putz(&rt->scratch, "(no output)");
    memset(&res, 0, sizeof res);
    res.content = rt->scratch.oom ? PT_S("out of memory keeping the output")
                                  : (pt_str){rt->scratch.p + dn, rt->scratch.n - dn};
    res.is_error = r->is_error || rt->scratch.oom;
    res.details = stored && !rt->scratch.oom ? (pt_str){rt->scratch.p, dn} : r->details;
    res.doc_key = r->doc_key;
    res.doc_value = r->doc_value;
    /* The slot goes back to the free part of the array, buffers and all. */
    i = (uint32_t)(c - rt->calls);
    done = *c;
    rt->calls[i] = rt->calls[--rt->ncalls];
    rt->calls[rt->ncalls] = done;
    if (!rt->closing)
        pt_fx_tool(rt->a, fx, &res);
}

void pt_rt_tools_stop(pt_rt *rt)
{
    /* A tool's cancel ends its call at once while closing. */
    while (rt->ncalls) {
        uint32_t before = rt->ncalls;
        pt_rt_cancel_tool(rt, rt->calls[0].fx);
        if (rt->ncalls == before) { /* a tool that cannot stop: drop the call */
            rt_call done;
            blob_abort(rt, &rt->calls[0]);
            done = rt->calls[0];
            rt->calls[0] = rt->calls[--rt->ncalls];
            rt->calls[rt->ncalls] = done;
        }
    }
}

void pt_rt_tools_free(pt_rt *rt)
{
    uint32_t i;
    pt_rt_tools_stop(rt);
    if (rt->builtin_free)
        rt->builtin_free(rt->builtin);
    rt->builtin_free = NULL;
    for (i = 0; i < rt->cinit; i++) {
        pt_buf_free(&rt->calls[i].head);
        pt_buf_free(&rt->calls[i].tail);
    }
    for (i = 0; i < rt->nrules; i++) {
        free(rt->rules[i].tool);
        free(rt->rules[i].prefix);
    }
    free(rt->calls);
    free(rt->tools);
    free(rt->defs);
    free(rt->rules);
}
