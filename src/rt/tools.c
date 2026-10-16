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
        if (!(d = (pt_tool_def *)realloc(rt->on, rt->tcap * sizeof *d)))
            return false;
        rt->on = d;
    }
    rt->tools[rt->ntools] = *t;
    rt->defs[rt->ntools++] = t->def;
    return true;
}

void pt_rt_tool_remove(pt_rt *rt, pt_str name)
{
    uint32_t i, k;
    for (i = 0; i < rt->ntools; i++)
        if (pt_str_eq(rt->tools[i].def.name, name)) {
            /* keep the order: the tools array is what the model sees */
            memmove(&rt->tools[i], &rt->tools[i + 1], (rt->ntools - i - 1) * sizeof *rt->tools);
            memmove(&rt->defs[i], &rt->defs[i + 1], (rt->ntools - i - 1) * sizeof *rt->defs);
            rt->ntools--;
            for (k = 0; k < rt->ncalls; k++)
                if (rt->calls[k].tool > i && rt->calls[k].tool != RT_FALLBACK)
                    rt->calls[k].tool--;
            return;
        }
}

bool pt_rt_tool_has(pt_rt *rt, pt_str name)
{
    uint32_t i;
    for (i = 0; i < rt->ntools; i++)
        if (pt_str_eq(rt->tools[i].def.name, name))
            return true;
    return false;
}

const pt_tool_def *pt_rt_tool_all(pt_rt *rt, uint32_t *n)
{
    *n = rt->ntools;
    return rt->defs;
}

const pt_tool_def *pt_rt_tool_defs(pt_rt *rt, uint32_t *n)
{
    uint32_t i;
    if (!rt->noff)
        return pt_rt_tool_all(rt, n);
    *n = 0;
    for (i = 0; i < rt->ntools; i++) /* rt->on has room for every tool */
        if (!pt_rt_tool_off(rt, rt->defs[i].name))
            rt->on[(*n)++] = rt->defs[i];
    return rt->on;
}

bool pt_rt_tool_off(const pt_rt *rt, pt_str name)
{
    uint32_t i;
    for (i = 0; i < rt->noff; i++)
        if (pt_str_eqz(name, rt->off[i]))
            return true;
    return false;
}

bool pt_rt_tool_set_off(pt_rt *rt, pt_str name, bool off)
{
    uint32_t i;
    char *z;
    for (i = 0; i < rt->noff && !pt_str_eqz(name, rt->off[i]); i++)
        ;
    if (!off) {
        if (i < rt->noff) {
            free(rt->off[i]);
            rt->off[i] = rt->off[--rt->noff];
        }
        return true;
    }
    if (i < rt->noff)
        return true;
    if (!pt_rt_grow((void **)&rt->off, &rt->ocap, rt->noff + 1, sizeof *rt->off) || !(z = (char *)malloc(name.n + 1)))
        return false;
    if (name.n)
        memcpy(z, name.p, name.n);
    z[name.n] = 0;
    rt->off[rt->noff++] = z;
    return true;
}

void pt_rt_tool_fallback(pt_rt *rt, const pt_rt_tool *t)
{
    rt->fallback = *t;
}

static int find_tool(pt_rt *rt, pt_str name)
{
    uint32_t i;
    for (i = 0; i < rt->ntools; i++)
        if (pt_str_eq(rt->tools[i].def.name, name))
            return (int)i;
    return -1;
}

/* An unregistered tool the fallback claims. */
static bool fallback_claims(pt_rt *rt, pt_str name)
{
    return rt->fallback.def.name.n && rt->fallback.run && pt_str_starts(name, rt->fallback.def.name) &&
           name.n > rt->fallback.def.name.n;
}

static pt_rt_tool *tool_of(pt_rt *rt, uint32_t k)
{
    return k == RT_FALLBACK ? &rt->fallback : &rt->tools[k];
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

/* ---- rules ---- */

/* Split a spec into its tool name and pattern, and say how the pattern
 * matches. A bash pattern ending in ":*" is a prefix of words. */
static bool rule_split(pt_str spec, pt_str *tool, pt_str *pat, rt_rule_mode *mode, const char **why)
{
    long open;
    size_t i;
    *pat = PT_STR_NULL;
    *mode = RULE_ANY;
    open = pt_str_find(spec, '(');
    *tool = open < 0 ? spec : (pt_str){spec.p, (size_t)open};
    if (!tool->n) {
        *why = "a rule starts with a tool name";
        return false;
    }
    for (i = 0; i < tool->n; i++)
        if (!tool->p[i] || strchr(" \t\n)", tool->p[i])) {
            *why = "a tool name has no spaces or parentheses";
            return false;
        }
    if (open < 0)
        return true;
    if (spec.p[spec.n - 1] != ')' || spec.n < (size_t)open + 3) {
        *why = "a rule is tool or tool(pattern)";
        return false;
    }
    *pat = (pt_str){spec.p + open + 1, spec.n - (size_t)open - 2};
    *mode = RULE_GLOB;
    if (pt_str_eqz(*tool, "bash") && pat->n >= 2 && pat->p[pat->n - 2] == ':' && pat->p[pat->n - 1] == '*') {
        pat->n -= 2;
        *mode = pat->n ? RULE_PREFIX : RULE_ANY;
    }
    return true;
}

bool pt_rule_check(pt_str spec, const char **why)
{
    pt_str tool, pat;
    rt_rule_mode mode;
    const char *w = "";
    if (!why)
        why = &w;
    return rule_split(pt_str_trim(spec), &tool, &pat, &mode, why);
}

bool pt_rt_rule_add(pt_rt *rt, bool deny, pt_str spec, pt_rule_src src)
{
    pt_str tool, pat;
    rt_rule_mode mode;
    const char *why;
    rt_rule *r;
    char *data;
    spec = pt_str_trim(spec);
    if (!rule_split(spec, &tool, &pat, &mode, &why) ||
        !pt_rt_grow((void **)&rt->rules, &rt->rucap, rt->nrules + 1, sizeof *rt->rules))
        return false;
    /* spec, tool and pattern, each ended by a NUL */
    data = (char *)malloc(spec.n + tool.n + pat.n + 3);
    if (!data)
        return false;
    r = &rt->rules[rt->nrules];
    memcpy(data, spec.p, spec.n);
    data[spec.n] = 0;
    memcpy(data + spec.n + 1, tool.p, tool.n);
    data[spec.n + 1 + tool.n] = 0;
    if (pat.n)
        memcpy(data + spec.n + tool.n + 2, pat.p, pat.n);
    data[spec.n + tool.n + 2 + pat.n] = 0;
    r->spec = data;
    r->tool = data + spec.n + 1;
    r->pat = mode == RULE_ANY ? NULL : data + spec.n + tool.n + 2;
    r->mode = mode;
    r->deny = deny;
    r->src = src;
    rt->nrules++;
    return true;
}

void pt_rt_rules_drop_config(pt_rt *rt)
{
    uint32_t i, k = 0;
    for (i = 0; i < rt->nrules; i++) {
        if (rt->rules[i].src == PT_RULE_SESSION)
            rt->rules[k++] = rt->rules[i];
        else
            free(rt->rules[i].spec);
    }
    rt->nrules = k;
}

void pt_rt_rules_trust(pt_rt *rt, bool trusted)
{
    rt->project_trusted = trusted;
}

static bool rule_active(const pt_rt *rt, const rt_rule *r)
{
    return r->deny || r->src != PT_RULE_PROJECT || rt->project_trusted;
}

uint32_t pt_rt_rules(pt_rt *rt, pt_rt_rule_info *out, uint32_t max)
{
    uint32_t i;
    for (i = 0; i < rt->nrules && i < max; i++) {
        out[i].spec = pt_strz(rt->rules[i].spec);
        out[i].deny = rt->rules[i].deny;
        out[i].src = rt->rules[i].src;
        out[i].active = rule_active(rt, &rt->rules[i]);
    }
    return rt->nrules;
}

bool pt_rt_allow(pt_rt *rt, pt_str tool, pt_str prefix)
{
    pt_buf b;
    bool ok;
    pt_buf_init(&b, pt_sys_allocator());
    pt_buf_puts(&b, tool);
    if (prefix.n) {
        pt_buf_putc(&b, '(');
        pt_buf_puts(&b, prefix);
        pt_buf_putz(&b, pt_str_eqz(tool, "bash") ? ":*)" : ")");
    }
    ok = !b.oom && pt_rt_rule_add(rt, false, pt_buf_str(&b), PT_RULE_SESSION);
    pt_buf_free(&b);
    return ok;
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

/* p against s, * matching any run of characters. Linear in the common
 * case, quadratic at worst; patterns are short. */
static bool glob(pt_str p, pt_str s, bool fold)
{
    size_t pi = 0, si = 0, star = (size_t)-1, mark = 0;
    while (si < s.n) {
        char pc = pi < p.n ? p.p[pi] : 0, sc = s.p[si];
        if (pc == '*') {
            star = pi++;
            mark = si;
        } else if (pi < p.n && (pc == sc || (fold && pc >= 'A' && pc <= 'Z' && pc + 32 == sc) ||
                                (fold && sc >= 'A' && sc <= 'Z' && sc + 32 == pc))) {
            pi++;
            si++;
        } else if (star != (size_t)-1) {
            pi = star + 1;
            si = ++mark;
        } else {
            return false;
        }
    }
    while (pi < p.n && p.p[pi] == '*')
        pi++;
    return pi == p.n;
}

/* A bash line's commands: the next one in *rest, without spaces; false
 * when there is none. */
#define SEP_CMDS ";&|\n\r"
#define SEP_ALL ";&|\n\r<>()`$"
static bool next_cmd(pt_str *rest, const char *seps, pt_str *cmd)
{
    while (rest->n) {
        size_t k = 0, skip;
        pt_str c;
        while (k < rest->n && rest->p[k] && !strchr(seps, rest->p[k]))
            k++;
        c = pt_str_trim((pt_str){rest->p, k});
        skip = k < rest->n ? k + 1 : k;
        rest->p += skip;
        rest->n -= skip;
        if (c.n) {
            *cmd = c;
            return true;
        }
    }
    return false;
}

/* Whether the rule's tool name and pattern match text: for bash, one
 * command; for web_fetch, the URL; else the first string argument. */
static bool rule_matches(const rt_rule *r, pt_str name, pt_str text)
{
    pt_str pat = pt_strz(r->pat);
    bool fold = false;
    if (!glob(pt_strz(r->tool), name, false))
        return false;
    if (r->mode == RULE_ANY)
        return true;
    if (r->mode == RULE_PREFIX) /* the words of a command, not the letters */
        return pt_str_starts(text, pat) && (text.n == pat.n || text.p[pat.n] == ' ' || text.p[pat.n] == '\t');
    if (pt_str_eqz(name, "web_fetch")) { /* by host */
        text = pt_rt_url_host(text);
        fold = true;
    }
    return glob(pat, text, fold);
}

/* The deny rule that matches the call, or NULL. A bash line is cut at
 * everything that could start a command, so no rule is dodged by
 * writing "ls; rm -rf x" or "echo $(rm -rf x)". */
static const rt_rule *denied(pt_rt *rt, pt_str name, pt_str gist)
{
    bool is_bash = pt_str_eqz(name, "bash");
    uint32_t i;
    for (i = 0; i < rt->nrules; i++) {
        const rt_rule *r = &rt->rules[i];
        pt_str rest = gist, cmd;
        if (!r->deny)
            continue;
        if (!is_bash || r->mode == RULE_ANY) {
            if (rule_matches(r, name, gist))
                return r;
            continue;
        }
        while (next_cmd(&rest, SEP_ALL, &cmd))
            if (rule_matches(r, name, cmd))
                return r;
    }
    return NULL;
}

/* The allow rule that lets the call run without asking, or NULL. A bash
 * line is allowed when rules match every command in it; a line with
 * substitution, a subshell or a redirect is allowed only by a rule
 * without a pattern, as the commands in it cannot be told. The rule
 * returned is the one for the first command. */
static const rt_rule *allowed(pt_rt *rt, pt_str name, pt_str gist)
{
    const rt_rule *first = NULL;
    pt_str rest = gist, cmd;
    uint32_t i;
    size_t k;
    bool plain = true;
    for (i = 0; i < rt->nrules; i++) {
        const rt_rule *r = &rt->rules[i];
        if (!r->deny && rule_active(rt, r) && (r->mode == RULE_ANY || !pt_str_eqz(name, "bash")) &&
            rule_matches(r, name, gist))
            return r;
    }
    if (!pt_str_eqz(name, "bash"))
        return NULL;
    for (k = 0; k < gist.n && plain; k++)
        plain = gist.p[k] && !strchr("<>()`$", gist.p[k]);
    if (!plain)
        return NULL;
    while (next_cmd(&rest, SEP_CMDS, &cmd)) {
        const rt_rule *hit = NULL;
        for (i = 0; i < rt->nrules && !hit; i++)
            if (!rt->rules[i].deny && rule_active(rt, &rt->rules[i]) && rule_matches(&rt->rules[i], name, cmd))
                hit = &rt->rules[i];
        if (!hit)
            return NULL;
        if (!first)
            first = hit;
    }
    return first;
}

static const char *const src_names[] = {"session", "user config", "project config"};

/* "<before><spec> (<source>)", in the arena. */
static pt_str rule_text(pt_rt *rt, const char *before, const rt_rule *r)
{
    pt_buf b;
    pt_str s;
    pt_buf_init(&b, pt_sys_allocator());
    pt_buf_putz(&b, before);
    pt_buf_putz(&b, r->spec);
    pt_buf_putz(&b, " (");
    pt_buf_putz(&b, src_names[r->src]);
    pt_buf_putc(&b, ')');
    s = b.oom ? PT_STR_NULL : pt_arena_dup(&rt->ar, pt_buf_str(&b));
    pt_buf_free(&b);
    return s;
}

/* The decision (section 15.5): a tool turned off first, then a deny
 * rule, then read-only mode, then the tool being read-only or the mode
 * auto or an allow rule; else ask. The text of a rule that decided goes with the answer, and is
 * committed with it. */
void pt_rt_gate(pt_rt *rt, const pt_fx *fx)
{
    int k = find_tool(rt, fx->name);
    bool claimed = k < 0 && fallback_claims(rt, fx->name);
    const rt_rule *rule = NULL;
    pt_gate g;
    pt_str gist;
    pt_arena_reset(&rt->ar);
    gist = pt_rt_gist(fx->args, &rt->ar);
    memset(&g, 0, sizeof g);
    g.kind = PT_GATE_ALLOW;
    if (pt_rt_tool_off(rt, fx->name)) { /* still in an epoch frozen before */
        g.kind = PT_GATE_DENY;
        g.text = PT_S("the user turned this tool off");
    } else if ((k >= 0 || claimed) && (rule = denied(rt, fx->name, gist))) {
        g.kind = PT_GATE_DENY;
        g.text = rule_text(rt, "blocked by the deny rule ", rule);
        rule = NULL;
    } else if (claimed) {
        /* not listed yet: it may change things */
        if (rt->perm == PT_PERM_READ_ONLY) {
            g.kind = PT_GATE_DENY;
            g.text = PT_S("pytte runs in read-only mode: only tools that change nothing run");
        } else if (rt->perm != PT_PERM_AUTO && !(rule = allowed(rt, fx->name, gist))) {
            g.kind = PT_GATE_ASK;
            g.text = gist;
        }
    } else if (k < 0) {
        /* allow: an unknown tool fails when it runs */
    } else if (rt->perm == PT_PERM_READ_ONLY) {
        /* Only tools that change nothing; allow rules do not apply. */
        if (!rt->tools[k].def.read_only) {
            g.kind = PT_GATE_DENY;
            g.text = PT_S("pytte runs in read-only mode: only tools that change nothing run (read, grep, "
                          "find and ls)");
        }
    } else if (rt->tools[k].def.read_only || rt->tools[k].no_ask || rt->perm == PT_PERM_AUTO) {
        /* allow */
    } else if (!(rule = allowed(rt, fx->name, gist))) {
        g.kind = PT_GATE_ASK;
        g.text = gist;
    }
    if (g.kind == PT_GATE_ALLOW && rule)
        g.text = rule_text(rt, "allow ", rule);
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
    uint32_t idx = (uint32_t)k;
    rt_call *c;
    if (k < 0 && fallback_claims(rt, fx->name))
        idx = RT_FALLBACK;
    else if (k < 0) {
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
    c->tool = idx;
    c->started = pt_rt_now(rt);
    c->total = 0;
    c->blob = NULL;
    c->nospill = false;
    c->output_oom = false;
    c->head.oom = c->tail.oom = false;
    pt_buf_reset(&c->head);
    pt_buf_reset(&c->tail);
    tool_of(rt, idx)->run(tool_of(rt, idx)->ud, rt, fx);
}

void pt_rt_cancel_tool(pt_rt *rt, uint32_t fx)
{
    rt_call *c = find_call(rt, fx);
    pt_rt_tool *t = c ? tool_of(rt, c->tool) : NULL;
    if (t && t->cancel)
        t->cancel(t->ud, rt, fx);
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
    if (c->head.oom || c->tail.oom)
        c->output_oom = true;
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

bool pt_rt_put_blob(pt_rt *rt, const void *p, size_t n, char ref[72])
{
    pt_store *st = rt->c.store;
    unsigned char d[32];
    pt_sha256 sha;
    void *h = st->blob_begin ? st->blob_begin(st) : NULL;
    if (!h)
        return false;
    if (n && st->blob_write(st, h, p, n) != 0) {
        st->blob_abort(st, h);
        return false;
    }
    pt_sha256_init(&sha);
    pt_sha256_update(&sha, p, n);
    pt_sha256_final(&sha, d);
    memcpy(ref, "sha256:", 7);
    pt_hex(ref + 7, d, 32);
    ref[71] = 0;
    return st->blob_commit(st, h, ref + 7) == 0;
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
    if (r->content.p && r->whole) { /* bounded by the tool: kept as it is */
        blob_abort(rt, c);
        c->nospill = true;
        pt_buf_reset(&c->head);
        pt_buf_reset(&c->tail);
        pt_buf_puts(&c->head, r->content);
        c->total = r->content.n;
    } else if (r->content.p) { /* given whole: cut it the same way */
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
    if (c->head.oom || c->tail.oom)
        c->output_oom = true;
    if (rt->scratch.n == dn)
        pt_buf_putz(&rt->scratch, "(no output)");
    if (!rt->closing && rt->builtin.ended) /* background commands that ended meanwhile */
        rt->builtin.ended(rt->builtin.ud, &rt->scratch);
    memset(&res, 0, sizeof res);
    res.content = rt->scratch.oom || c->output_oom ? PT_S("out of memory keeping the output")
                                                   : (pt_str){rt->scratch.p + dn, rt->scratch.n - dn};
    res.is_error = r->is_error || rt->scratch.oom || c->output_oom;
    res.details = stored && !rt->scratch.oom ? (pt_str){rt->scratch.p, dn} : r->details;
    res.doc_key = r->doc_key;
    res.doc_value = r->doc_value;
    res.image = r->image;
    res.image_mime = r->image_mime;
    /* The slot goes back to the free part of the array, buffers and all. */
    i = (uint32_t)(c - rt->calls);
    done = *c;
    rt->calls[i] = rt->calls[--rt->ncalls];
    rt->calls[rt->ncalls] = done;
    if (!rt->closing)
        pt_fx_tool(rt->a, fx, &res);
    else if (rt->free_waiting && !rt->ncalls)
        pt_rt_free(rt); /* finish deferred destruction after the last late completion */
}

void pt_rt_tools_stop(pt_rt *rt)
{
    uint32_t i = 0;
    while (i < rt->ncalls) {
        uint32_t before = rt->ncalls;
        rt_call *c = &rt->calls[i];
        if (!c->cancel_requested) {
            c->cancel_requested = true;
            pt_rt_cancel_tool(rt, c->fx);
        }
        /* A synchronous completion swaps a different call into this slot. */
        if (rt->ncalls == before)
            i++;
    }
}

void pt_rt_tools_free(pt_rt *rt)
{
    uint32_t i;
    pt_rt_tools_stop(rt);
    if (rt->builtin.free)
        rt->builtin.free(rt->builtin.ud);
    rt->builtin.free = NULL;
    for (i = 0; i < rt->cinit; i++) {
        pt_buf_free(&rt->calls[i].head);
        pt_buf_free(&rt->calls[i].tail);
    }
    for (i = 0; i < rt->nrules; i++)
        free(rt->rules[i].spec);
    free(rt->calls);
    free(rt->tools);
    free(rt->defs);
    free(rt->on);
    for (i = 0; i < rt->noff; i++)
        free(rt->off[i]);
    free(rt->off);
    free(rt->rules);
}
