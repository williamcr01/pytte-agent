/* From a unified context to a request: cross-provider transforms (the
 * target view), max_tokens, and helpers shared by the APIs. Everything here
 * is pure, so the same context always gives the same request bytes. */
#include "ai.h"

#include <string.h>

/* ---- tool call IDs ---- */

static bool id_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '-';
}

pt_str pt_fix_tool_id(pt_str id, uint32_t max, pt_arena *a)
{
    static const char hex[] = "0123456789abcdef";
    uint32_t h = 2166136261u; /* FNV-1a */
    size_t i, keep;
    bool ok = id.n > 0 && (max == 0 || id.n <= max);
    char *out;
    pt_str r;
    for (i = 0; i < id.n && ok; i++)
        ok = id_char(id.p[i]);
    if (ok || max == 0)
        return id;
    for (i = 0; i < id.n; i++) {
        h ^= (unsigned char)id.p[i];
        h *= 16777619u;
    }
    /* sanitized prefix + '_' + 8 hex digits of the original */
    keep = max > 9 ? max - 9 : 0;
    if (keep > id.n)
        keep = id.n;
    out = (char *)pt_arena_alloc(a, keep + 10);
    if (!out)
        return id;
    for (i = 0; i < keep; i++)
        out[i] = id_char(id.p[i]) ? id.p[i] : '_';
    out[keep] = '_';
    for (i = 0; i < 8; i++)
        out[keep + 1 + i] = hex[(h >> (28 - 4 * i)) & 15];
    out[keep + 9] = 0;
    r.p = out;
    r.n = keep + 9;
    if (r.n > max) { /* max < 9: keep the tail of the hash */
        r.p += r.n - max;
        r.n = max;
    }
    return r;
}

/* ---- the target view ---- */

static pt_str wrap(pt_arena *a, const char *open, pt_str text, const char *close)
{
    pt_buf b;
    pt_str r;
    pt_buf_init(&b, a->al);
    pt_buf_putz(&b, open);
    pt_buf_puts(&b, text);
    pt_buf_putz(&b, close);
    r = pt_arena_dup(a, pt_buf_str(&b));
    if (b.oom)
        a->oom = true;
    pt_buf_free(&b);
    return r;
}

static bool same_source(const pt_ai_target *t, const pt_msg *m)
{
    return pt_str_eqz(m->provider, t->provider->id) && pt_str_eqz(m->api, t->api->id);
}

static void view_assistant(const pt_ai_target *t, const pt_msg *in, pt_msg *out, pt_arena *a)
{
    bool same = same_source(t, in);
    uint32_t i, n = 0;
    pt_block *blocks = (pt_block *)pt_arena_alloc(a, sizeof(pt_block) * (in->nblocks ? in->nblocks : 1));
    *out = *in;
    out->blocks = blocks;
    out->nblocks = 0;
    if (!blocks)
        return;
    for (i = 0; i < in->nblocks; i++) {
        pt_block b = in->blocks[i];
        switch (b.kind) {
        case PT_BLOCK_THINKING:
            if (!same) {
                /* Another provider's reasoning: plain text, if there is any. */
                if (b.redacted || !pt_str_trim(b.text).n)
                    continue;
                b.kind = PT_BLOCK_TEXT;
                b.text = wrap(a, "<thinking>\n", b.text, "\n</thinking>");
                b.signature = PT_STR_NULL;
            }
            break;
        case PT_BLOCK_OPAQUE:
            if (!same || !pt_str_eqz(b.api, t->api->id))
                continue;
            break;
        case PT_BLOCK_TOOL_CALL:
            b.id = pt_fix_tool_id(b.id, t->api->tool_id_max, a);
            break;
        default:
            break;
        }
        blocks[n++] = b;
    }
    out->nblocks = n;
}

static bool user_side(const pt_msg *m)
{
    return m->role == PT_ROLE_USER || m->role == PT_ROLE_TOOL_RESULT;
}

bool pt_ai_target_view(const pt_ai_target *t, const pt_ai_context *in, pt_ai_context *out,
                       pt_arena *a)
{
    uint32_t i, n = 0;
    pt_msg *msgs = (pt_msg *)pt_arena_alloc(a, sizeof(pt_msg) * (in->nmsgs ? in->nmsgs : 1));
    bool mid_system = t->compat.mid_conversation_system == PT_YES;
    pt_buf pending; /* later system messages waiting to be folded */
    bool have_pending = false, seen_user = false;

    out->tools = in->tools;
    out->ntools = in->ntools;
    out->msgs = msgs;
    out->nmsgs = 0;
    if (!msgs)
        return false;
    pt_buf_init(&pending, a->al);

    for (i = 0; i < in->nmsgs; i++) {
        const pt_msg *m = &in->msgs[i];
        pt_msg *o = &msgs[n];
        if (m->role == PT_ROLE_SYSTEM && i > 0 && !mid_system) {
            /* Folded: delivered after the next user-side content. */
            if (have_pending)
                pt_buf_putz(&pending, "\n\n");
            pt_buf_puts(&pending, m->text);
            have_pending = true;
            continue;
        }
        if (m->role == PT_ROLE_ASSISTANT)
            view_assistant(t, m, o, a);
        else
            *o = *m;
        if (m->role == PT_ROLE_TOOL_RESULT)
            o->call_id = pt_fix_tool_id(m->call_id, t->api->tool_id_max, a);
        n++;
        if (have_pending && user_side(m))
            seen_user = true;
        if (have_pending && seen_user && (i + 1 == in->nmsgs || !user_side(&in->msgs[i + 1]))) {
            /* The user-side run ends here: fold the pending system text.
             * Each fold consumed at least one system message, so the
             * array never needs more than in->nmsgs slots. */
            pt_msg *f = &msgs[n++];
            memset(f, 0, sizeof *f);
            f->role = PT_ROLE_USER;
            f->text = wrap(a, "<system>\n", pt_buf_str(&pending), "\n</system>");
            pt_buf_reset(&pending);
            have_pending = seen_user = false;
        }
    }
    if (have_pending) { /* nothing user-side followed: send it at the end */
        memset(&msgs[n], 0, sizeof msgs[n]);
        msgs[n].role = PT_ROLE_USER;
        msgs[n].text = wrap(a, "<system>\n", pt_buf_str(&pending), "\n</system>");
        n++;
    }
    if (pending.oom)
        a->oom = true;
    pt_buf_free(&pending);
    out->nmsgs = n;
    return !a->oom;
}

/* ---- max_tokens ---- */

static uint64_t estimate_tokens(const pt_ai_context *ctx)
{
    uint64_t bytes = 0;
    uint32_t i, j;
    for (i = 0; i < ctx->nmsgs; i++) {
        const pt_msg *m = &ctx->msgs[i];
        bytes += m->text.n + 16;
        for (j = 0; j < m->nblocks; j++)
            bytes += m->blocks[j].text.n + m->blocks[j].args.n + m->blocks[j].raw.n +
                     m->blocks[j].signature.n / 4 + 16;
    }
    for (i = 0; i < ctx->ntools; i++)
        bytes += ctx->tools[i].name.n + ctx->tools[i].description.n + ctx->tools[i].schema.n + 32;
    return bytes / 3; /* deliberately generous: a small overestimate is harmless */
}

uint32_t pt_ai_auto_max_tokens(const pt_ai_target *t, const pt_ai_context *ctx, uint32_t input_estimate)
{
    const uint64_t margin = 4096;
    uint64_t est, window = t->model.context_window, max = t->model.max_tokens;
    if (!max)
        return 0;
    if (!window)
        return (uint32_t)max;
    est = input_estimate ? input_estimate : estimate_tokens(ctx);
    if (est + margin >= window)
        return 1; /* will not fit; let the provider report the overflow */
    return (uint32_t)(window - est - margin < max ? window - est - margin : max);
}

/* ---- build ---- */

bool pt_ai_build(const pt_ai_target *t, const pt_ai_context *ctx, const pt_ai_opts *o,
                 const pt_credentials *cred, pt_http_req *out, pt_arena *a)
{
    pt_ai_context view;
    static const pt_ai_opts none;
    if (!o)
        o = &none;
    memset(out, 0, sizeof *out);
    if (!pt_ai_target_view(t, ctx, &view, a))
        return false;
    return t->api->build(t, &view, o, cred, out, a) && !a->oom;
}

bool pt_req_init(pt_http_req *r, const pt_ai_target *t, const pt_ai_opts *o, const char *path,
                 uint32_t extra, pt_arena *a)
{
    pt_buf url;
    pt_str base = pt_strz(t->provider->base_url);
    uint32_t cap = extra + 4 + o->nheaders;
    memset(r, 0, sizeof *r);
    while (base.n && base.p[base.n - 1] == '/')
        base.n--;
    pt_buf_init(&url, a->al);
    pt_buf_puts(&url, base);
    pt_buf_putz(&url, path);
    r->url = pt_arena_cstr(a, pt_buf_str(&url));
    pt_buf_free(&url);
    r->method = "POST";
    r->headers = (pt_header *)pt_arena_alloc(a, sizeof(pt_header) * cap);
    r->hcap = r->headers ? cap : 0;
    return r->headers && r->url && !url.oom;
}

void pt_req_header(pt_http_req *r, const char *name, const char *value)
{
    if (r->nheaders < r->hcap && name && value) {
        r->headers[r->nheaders].name = name;
        r->headers[r->nheaders].value = value;
        r->nheaders++;
    }
}

void pt_req_extra_headers(pt_http_req *r, const pt_ai_target *t, const pt_ai_opts *o)
{
    uint32_t i;
    for (i = 0; i < 4 && t->provider->headers[i].name; i++)
        pt_req_header(r, t->provider->headers[i].name, t->provider->headers[i].value);
    for (i = 0; i < o->nheaders; i++)
        pt_req_header(r, o->headers[i].name, o->headers[i].value);
}

bool pt_req_body(pt_http_req *r, pt_buf *b, pt_arena *a)
{
    bool ok = !b->oom;
    r->body = pt_arena_dup(a, pt_buf_str(b));
    pt_buf_free(b);
    return ok && !a->oom;
}

/* ---- errors ---- */

static bool overflow_message(pt_str m)
{
    static const char *const phrases[] = {
        "prompt is too long",     "context_length_exceeded", "maximum context length",
        "context length",         "context window",          "too many tokens",
        "reduce the length",      "input is too long",
    };
    size_t i;
    for (i = 0; i < sizeof phrases / sizeof phrases[0]; i++)
        if (pt_str_icontains(m, pt_strz(phrases[i])))
            return true;
    return false;
}

void pt_classify_status(int status, pt_str message, pt_ai_error *out)
{
    out->status = status;
    out->message = message;
    if (status == 408 || status == 409 || status == 425 || status == 429 || status >= 500)
        out->cls = PT_ERR_RETRYABLE;
    else if (status == 413 || overflow_message(message))
        out->cls = PT_ERR_OVERFLOW;
    else
        out->cls = PT_ERR_FATAL;
}
