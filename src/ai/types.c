/* Unified types: names and the JSON form of messages. */
#include "ai.h"

#include <string.h>

static const char *const stop_names[] = {"stop", "length", "tool_use", "refusal", "error", "aborted"};
static const char *const role_names[] = {"system", "user", "assistant", "tool_result"};
static const char *const ev_names[] = {
    "start",          "text_start",    "text_delta",     "text_end",
    "thinking_start", "thinking_delta", "thinking_end",  "toolcall_start",
    "toolcall_delta", "toolcall_end",  "done",           "error"};
static const char *const err_names[] = {"none", "retryable", "overflow", "fatal", "aborted"};
static const char *const reason_names[] = {"default", "off",  "minimal", "low",
                                           "medium",  "high", "xhigh",   "max"};

const char *pt_stop_name(pt_stop s)
{
    return (unsigned)s < 6 ? stop_names[s] : "?";
}

const char *pt_role_name(pt_role r)
{
    return (unsigned)r < 4 ? role_names[r] : "?";
}

const char *pt_ev_name(pt_ev_kind k)
{
    return (unsigned)k < 12 ? ev_names[k] : "?";
}

const char *pt_err_class_name(pt_err_class c)
{
    return (unsigned)c < 5 ? err_names[c] : "?";
}

const char *pt_reasoning_name(pt_reasoning r)
{
    return (unsigned)r < 8 ? reason_names[r] : "?";
}

bool pt_reasoning_parse(pt_str s, pt_reasoning *out)
{
    unsigned i;
    for (i = 0; i < 8; i++)
        if (pt_str_eqz(s, reason_names[i])) {
            *out = (pt_reasoning)i;
            return true;
        }
    return false;
}

static bool lookup(pt_str s, const char *const *names, unsigned n, unsigned *out)
{
    unsigned i;
    for (i = 0; i < n; i++)
        if (pt_str_eqz(s, names[i])) {
            *out = i;
            return true;
        }
    return false;
}

/* ---- writing ---- */

static void kstr(pt_jw *w, const char *k, pt_str v)
{
    if (!v.n)
        return;
    pt_jw_key(w, k);
    pt_jw_str(w, v);
}

static void kuint(pt_jw *w, const char *k, uint64_t v)
{
    if (!v)
        return;
    pt_jw_key(w, k);
    pt_jw_uint(w, v);
}

static void write_block(pt_jw *w, const pt_block *b)
{
    pt_jw_obj(w);
    switch (b->kind) {
    case PT_BLOCK_TEXT:
        pt_jw_key(w, "type");
        pt_jw_strz(w, "text");
        pt_jw_key(w, "text");
        pt_jw_str(w, b->text);
        break;
    case PT_BLOCK_THINKING:
        pt_jw_key(w, "type");
        pt_jw_strz(w, "thinking");
        pt_jw_key(w, "thinking");
        pt_jw_str(w, b->text);
        kstr(w, "signature", b->signature);
        if (b->redacted) {
            pt_jw_key(w, "redacted");
            pt_jw_bool(w, true);
        }
        break;
    case PT_BLOCK_TOOL_CALL:
        pt_jw_key(w, "type");
        pt_jw_strz(w, "tool_call");
        pt_jw_key(w, "id");
        pt_jw_str(w, b->id);
        pt_jw_key(w, "name");
        pt_jw_str(w, b->name);
        pt_jw_key(w, "arguments"); /* the raw text, as a string: lossless */
        pt_jw_str(w, b->args);
        break;
    case PT_BLOCK_IMAGE:
        pt_jw_key(w, "type");
        pt_jw_strz(w, "image");
        break;
    case PT_BLOCK_OPAQUE:
        pt_jw_key(w, "type");
        pt_jw_strz(w, "opaque");
        pt_jw_key(w, "api");
        pt_jw_str(w, b->api);
        pt_jw_key(w, "raw");
        if (b->raw.n)
            pt_jw_raw(w, b->raw);
        else
            pt_jw_null(w);
        break;
    }
    pt_jw_obj_end(w);
}

void pt_msg_write_json(pt_jw *w, const pt_msg *m)
{
    uint32_t i;
    pt_jw_obj(w);
    pt_jw_key(w, "role");
    pt_jw_strz(w, pt_role_name(m->role));
    switch (m->role) {
    case PT_ROLE_SYSTEM:
    case PT_ROLE_USER:
        pt_jw_key(w, "text");
        pt_jw_str(w, m->text);
        break;
    case PT_ROLE_TOOL_RESULT:
        pt_jw_key(w, "call_id");
        pt_jw_str(w, m->call_id);
        kstr(w, "name", m->name);
        pt_jw_key(w, "text");
        pt_jw_str(w, m->text);
        if (m->is_error) {
            pt_jw_key(w, "is_error");
            pt_jw_bool(w, true);
        }
        break;
    case PT_ROLE_ASSISTANT:
        kstr(w, "api", m->api);
        kstr(w, "provider", m->provider);
        kstr(w, "model", m->model);
        kstr(w, "response_model", m->response_model);
        kstr(w, "response_id", m->response_id);
        pt_jw_key(w, "content");
        pt_jw_arr(w);
        for (i = 0; i < m->nblocks; i++)
            write_block(w, &m->blocks[i]);
        pt_jw_arr_end(w);
        pt_jw_key(w, "usage");
        pt_jw_obj(w);
        pt_jw_key(w, "input");
        pt_jw_uint(w, m->usage.input);
        pt_jw_key(w, "output");
        pt_jw_uint(w, m->usage.output);
        kuint(w, "cache_read", m->usage.cache_read);
        kuint(w, "cache_write", m->usage.cache_write);
        kuint(w, "reasoning", m->usage.reasoning);
        pt_jw_obj_end(w);
        pt_jw_key(w, "stop");
        pt_jw_strz(w, pt_stop_name(m->stop));
        kstr(w, "raw_stop", m->raw_stop);
        kstr(w, "error", m->error);
        if (m->timestamp) {
            pt_jw_key(w, "timestamp");
            pt_jw_int(w, m->timestamp);
        }
        if (m->duration_ms) {
            pt_jw_key(w, "duration_ms");
            pt_jw_int(w, m->duration_ms);
        }
        break;
    }
    pt_jw_obj_end(w);
}

/* ---- reading ---- */

static pt_str gets(const pt_json *o, const char *k, pt_arena *a)
{
    return pt_arena_dup(a, pt_json_gets(o, k, a));
}

static uint64_t getu(const pt_json *o, const char *k)
{
    int64_t v = pt_json_geti(o, k, 0);
    return v > 0 ? (uint64_t)v : 0;
}

static bool read_block(const pt_json *v, pt_block *b, pt_arena *a)
{
    pt_str type = pt_json_gets(v, "type", a);
    if (pt_str_eqz(type, "text")) {
        b->kind = PT_BLOCK_TEXT;
        b->text = gets(v, "text", a);
    } else if (pt_str_eqz(type, "thinking")) {
        b->kind = PT_BLOCK_THINKING;
        b->text = gets(v, "thinking", a);
        b->signature = gets(v, "signature", a);
        b->redacted = pt_json_truthy(pt_json_get(v, "redacted"));
    } else if (pt_str_eqz(type, "tool_call")) {
        b->kind = PT_BLOCK_TOOL_CALL;
        b->id = gets(v, "id", a);
        b->name = gets(v, "name", a);
        b->args = gets(v, "arguments", a);
    } else if (pt_str_eqz(type, "opaque")) {
        const pt_json *raw = pt_json_get(v, "raw");
        b->kind = PT_BLOCK_OPAQUE;
        b->api = gets(v, "api", a);
        if (raw && raw->type != PT_J_NULL)
            b->raw = pt_arena_dup(a, raw->raw);
    } else if (pt_str_eqz(type, "image")) {
        b->kind = PT_BLOCK_IMAGE;
    } else {
        return false;
    }
    return true;
}

bool pt_msg_read_json(const pt_json *v, pt_msg *m, pt_arena *a)
{
    unsigned r;
    const pt_json *content, *usage, *c;
    uint32_t i = 0;
    memset(m, 0, sizeof *m);
    if (!pt_json_is(v, PT_J_OBJ) || !lookup(pt_json_gets(v, "role", a), role_names, 4, &r))
        return false;
    m->role = (pt_role)r;
    m->text = gets(v, "text", a);
    m->call_id = gets(v, "call_id", a);
    m->name = gets(v, "name", a);
    m->is_error = pt_json_truthy(pt_json_get(v, "is_error"));
    if (m->role != PT_ROLE_ASSISTANT)
        return true;
    m->api = gets(v, "api", a);
    m->provider = gets(v, "provider", a);
    m->model = gets(v, "model", a);
    m->response_model = gets(v, "response_model", a);
    m->response_id = gets(v, "response_id", a);
    m->raw_stop = gets(v, "raw_stop", a);
    m->error = gets(v, "error", a);
    m->timestamp = pt_json_geti(v, "timestamp", 0);
    m->duration_ms = pt_json_geti(v, "duration_ms", 0);
    if (!lookup(pt_json_gets(v, "stop", a), stop_names, 6, &r))
        return false;
    m->stop = (pt_stop)r;
    usage = pt_json_get(v, "usage");
    m->usage.input = getu(usage, "input");
    m->usage.output = getu(usage, "output");
    m->usage.cache_read = getu(usage, "cache_read");
    m->usage.cache_write = getu(usage, "cache_write");
    m->usage.reasoning = getu(usage, "reasoning");
    content = pt_json_get(v, "content");
    if (!pt_json_is(content, PT_J_ARR))
        return false;
    m->blocks = (pt_block *)pt_arena_zalloc(a, sizeof(pt_block) * (content->len ? content->len : 1));
    if (!m->blocks)
        return false;
    for (c = content->child; c; c = c->next)
        if (!read_block(c, &m->blocks[i++], a))
            return false;
    m->nblocks = i;
    return true;
}
