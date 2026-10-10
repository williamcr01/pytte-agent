/* api: openai-responses (POST /responses, streaming). OpenAI's own API: it
 * takes tools and reasoning together, streams reasoning summaries, and
 * carries reasoning between turns as encrypted items (store: false). */
#include "ai.h"

/* ---- request ---- */

/* A reasoning item is kept whole, as raw JSON, in the thinking block's
 * signature, and sent back byte for byte. */
static bool is_object(pt_str s, pt_arena *a)
{
    pt_json *v = pt_json_parse(s, a, NULL);
    return v && v->type == PT_J_OBJ;
}

static void write_assistant(pt_jw *w, const pt_msg *m, pt_arena *a)
{
    uint32_t i;
    for (i = 0; i < m->nblocks; i++) {
        const pt_block *b = &m->blocks[i];
        switch (b->kind) {
        case PT_BLOCK_THINKING:
            if (b->signature.n && is_object(b->signature, a))
                pt_jw_raw(w, b->signature);
            break;
        case PT_BLOCK_TEXT:
            if (!b->text.n)
                break;
            pt_jw_obj(w);
            pt_jw_key(w, "type");
            pt_jw_strz(w, "message");
            pt_jw_key(w, "role");
            pt_jw_strz(w, "assistant");
            pt_jw_key(w, "content");
            pt_jw_arr(w);
            pt_jw_obj(w);
            pt_jw_key(w, "type");
            pt_jw_strz(w, "output_text");
            pt_jw_key(w, "text");
            pt_jw_str(w, b->text);
            pt_jw_obj_end(w);
            pt_jw_arr_end(w);
            pt_jw_obj_end(w);
            break;
        case PT_BLOCK_TOOL_CALL:
            pt_jw_obj(w);
            pt_jw_key(w, "type");
            pt_jw_strz(w, "function_call");
            pt_jw_key(w, "call_id");
            pt_jw_str(w, b->id);
            pt_jw_key(w, "name");
            pt_jw_str(w, b->name);
            pt_jw_key(w, "arguments");
            pt_jw_str(w, pt_str_trim(b->args).n ? b->args : PT_S("{}"));
            pt_jw_obj_end(w);
            break;
        case PT_BLOCK_OPAQUE:
            if (b->raw.n && is_object(b->raw, a))
                pt_jw_raw(w, b->raw);
            break;
        case PT_BLOCK_IMAGE:
            break;
        }
    }
}

static bool build(const pt_ai_target *t, const pt_ai_context *ctx, const pt_ai_opts *o,
                  const pt_credentials *cred, pt_http_req *out, pt_arena *a)
{
    pt_buf b;
    pt_jw w;
    uint32_t i, first = 0, max_tokens;
    const char *sysrole = t->compat.system_role == PT_SYSROLE_SYSTEM ? "system" : "developer";
    bool summary = t->compat.thinking_display == PT_YES;
    pt_buf_init(&b, a->al);
    pt_jw_init(&w, &b);

    max_tokens = o->max_tokens ? o->max_tokens : pt_ai_auto_max_tokens(t, ctx, o->input_estimate);

    pt_jw_obj(&w);
    pt_jw_key(&w, "model");
    pt_jw_strz(&w, t->model.id);
    pt_jw_key(&w, "stream");
    pt_jw_bool(&w, true);
    pt_jw_key(&w, "store"); /* the transcript is ours; nothing is kept server-side */
    pt_jw_bool(&w, false);
    if (ctx->nmsgs && ctx->msgs[0].role == PT_ROLE_SYSTEM) {
        first = 1;
        if (ctx->msgs[0].text.n) {
            pt_jw_key(&w, "instructions");
            pt_jw_str(&w, ctx->msgs[0].text);
        }
    }
    if (max_tokens) {
        pt_jw_key(&w, "max_output_tokens");
        pt_jw_uint(&w, max_tokens);
    }
    if (t->model.reasoning && (o->reasoning != PT_REASON_DEFAULT || summary)) {
        pt_jw_key(&w, "reasoning");
        pt_jw_obj(&w);
        if (o->reasoning != PT_REASON_DEFAULT) {
            pt_jw_key(&w, "effort");
            pt_jw_strz(&w, pt_openai_effort(o->reasoning));
        }
        if (summary && o->reasoning != PT_REASON_OFF) {
            pt_jw_key(&w, "summary");
            pt_jw_strz(&w, "auto");
        }
        pt_jw_obj_end(&w);
    }
    if (t->model.reasoning) {
        pt_jw_key(&w, "include");
        pt_jw_raw(&w, PT_S("[\"reasoning.encrypted_content\"]"));
    }
    if (o->session_id.n) {
        pt_jw_key(&w, "prompt_cache_key");
        pt_jw_str(&w, o->session_id);
    }
    if (o->temperature.n) {
        pt_jw_key(&w, "temperature");
        pt_jw_raw(&w, o->temperature);
    }

    pt_jw_key(&w, "input");
    pt_jw_arr(&w);
    for (i = first; i < ctx->nmsgs; i++) {
        const pt_msg *m = &ctx->msgs[i];
        switch (m->role) {
        case PT_ROLE_SYSTEM:
            if (!m->text.n)
                break;
            pt_jw_obj(&w);
            pt_jw_key(&w, "role");
            pt_jw_strz(&w, sysrole);
            pt_jw_key(&w, "content");
            pt_jw_str(&w, m->text);
            pt_jw_obj_end(&w);
            break;
        case PT_ROLE_USER:
            pt_jw_obj(&w);
            pt_jw_key(&w, "role");
            pt_jw_strz(&w, "user");
            pt_jw_key(&w, "content");
            pt_jw_arr(&w);
            pt_jw_obj(&w);
            pt_jw_key(&w, "type");
            pt_jw_strz(&w, "input_text");
            pt_jw_key(&w, "text");
            pt_jw_str(&w, m->text);
            pt_jw_obj_end(&w);
            pt_jw_arr_end(&w);
            pt_jw_obj_end(&w);
            break;
        case PT_ROLE_ASSISTANT:
            write_assistant(&w, m, a);
            break;
        case PT_ROLE_TOOL_RESULT:
            pt_jw_obj(&w);
            pt_jw_key(&w, "type");
            pt_jw_strz(&w, "function_call_output");
            pt_jw_key(&w, "call_id");
            pt_jw_str(&w, m->call_id);
            pt_jw_key(&w, "output");
            pt_jw_str(&w, m->text);
            pt_jw_obj_end(&w);
            break;
        }
    }
    pt_jw_arr_end(&w);

    if (ctx->ntools) {
        pt_jw_key(&w, "tools");
        pt_jw_arr(&w);
        for (i = 0; i < ctx->ntools; i++) {
            const pt_tool *tool = &ctx->tools[i];
            pt_jw_obj(&w);
            pt_jw_key(&w, "type");
            pt_jw_strz(&w, "function");
            pt_jw_key(&w, "name");
            pt_jw_str(&w, tool->name);
            if (tool->description.n) {
                pt_jw_key(&w, "description");
                pt_jw_str(&w, tool->description);
            }
            pt_jw_key(&w, "parameters");
            pt_jw_raw(&w, tool->schema.n ? tool->schema : PT_S("{\"type\":\"object\"}"));
            pt_jw_obj_end(&w);
        }
        pt_jw_arr_end(&w);
    }
    pt_jw_obj_end(&w);

    if (!pt_req_init(out, t, o, "/responses", 4, a)) {
        pt_buf_free(&b);
        return false;
    }
    pt_req_header(out, "content-type", "application/json");
    pt_req_header(out, "accept", "text/event-stream");
    if (cred && cred->api_key.n) {
        pt_buf auth;
        pt_buf_init(&auth, a->al);
        pt_buf_putz(&auth, "Bearer ");
        pt_buf_puts(&auth, cred->api_key);
        pt_req_header(out, "authorization", pt_arena_cstr(a, pt_buf_str(&auth)));
        pt_buf_free(&auth);
    }
    pt_req_extra_headers(out, t, o);
    return pt_req_body(out, &b, a);
}

/* ---- stream ---- */

typedef struct state {
    long *map; /* output_index → block */
    uint32_t cap;
    bool any_call, refusal;
} state;

static long *slot(pt_ai_stream *s, state *st, int64_t i)
{
    if (i < 0 || i > 100000)
        return NULL;
    if ((uint64_t)i >= st->cap) {
        uint32_t ncap = st->cap ? st->cap : 16, j;
        long *n;
        while (ncap <= (uint64_t)i)
            ncap *= 2;
        n = (long *)pt_arena_alloc(pt_emit_own(s), sizeof(long) * ncap);
        if (!n)
            return NULL;
        for (j = 0; j < ncap; j++)
            n[j] = j < st->cap ? st->map[j] : -1;
        st->map = n;
        st->cap = ncap;
    }
    return &st->map[i];
}

static long block_at(pt_ai_stream *s, state *st, const pt_json *d)
{
    long *p = slot(s, st, pt_json_geti(d, "output_index", -1));
    return p ? *p : -1;
}

static void read_usage(pt_msg *m, const pt_json *u)
{
    int64_t in = pt_json_geti(u, "input_tokens", 0);
    int64_t cached = pt_json_geti(pt_json_get(u, "input_tokens_details"), "cached_tokens", 0);
    int64_t out = pt_json_geti(u, "output_tokens", 0);
    int64_t reasoning = pt_json_geti(pt_json_get(u, "output_tokens_details"), "reasoning_tokens", 0);
    if (!pt_json_is(u, PT_J_OBJ))
        return;
    if (cached < 0)
        cached = 0;
    m->usage.input = in > cached ? (uint64_t)(in - cached) : 0;
    m->usage.cache_read = (uint64_t)cached;
    m->usage.output = out > 0 ? (uint64_t)out : 0;
    m->usage.reasoning = reasoning > 0 ? (uint64_t)reasoning : 0;
}

/* response.failed and error events: {code, message}. */
static void fail_with(pt_ai_stream *s, const pt_json *err, pt_str fallback, pt_arena *a)
{
    pt_ai_error e;
    pt_str code = pt_json_gets(err, "code", a);
    int status = 500;
    if (pt_str_contains(code, PT_S("rate_limit")))
        status = 429;
    else if (pt_str_contains(code, PT_S("invalid")) || pt_str_contains(code, PT_S("context_length")))
        status = 400;
    pt_openai_error(err, fallback, status, &e, a);
    pt_emit_fail(s, e.cls, e.message);
}

static void finished(pt_ai_stream *s, state *st, const pt_json *resp, pt_arena *sa)
{
    pt_msg *m = pt_emit_msg(s);
    pt_str status = pt_json_gets(resp, "status", sa);
    pt_str reason = pt_json_gets(pt_json_get(resp, "incomplete_details"), "reason", sa);
    read_usage(m, pt_json_get(resp, "usage"));
    m->raw_stop = pt_arena_dup(pt_emit_arena(s), reason.n ? reason : status);
    if (pt_str_eqz(reason, "max_output_tokens"))
        m->stop = PT_STOP_LENGTH;
    else if (pt_str_eqz(reason, "content_filter") || st->refusal)
        m->stop = PT_STOP_REFUSAL;
    else if (st->any_call)
        m->stop = PT_STOP_TOOL_USE;
    else
        m->stop = PT_STOP_STOP;
    pt_emit_done(s);
}

/* The `field` strings of an item's `list` array, joined by blank lines. */
static pt_str joined(const pt_json *item, const char *list, const char *field, pt_arena *a)
{
    const pt_json *arr = pt_json_get(item, list), *c;
    pt_buf b;
    pt_str r;
    pt_buf_init(&b, a->al);
    for (c = pt_json_is(arr, PT_J_ARR) ? arr->child : NULL; c; c = c->next) {
        pt_str t = pt_json_gets(c, field, a);
        if (!t.n)
            continue;
        if (b.n)
            pt_buf_putz(&b, "\n\n");
        pt_buf_puts(&b, t);
    }
    r = pt_arena_dup(a, pt_buf_str(&b));
    pt_buf_free(&b);
    return r;
}

static int feed(pt_ai_stream *s, void *vst, const pt_sse_event *ev)
{
    state *st = (state *)vst;
    pt_arena *sa = pt_emit_scratch(s), *fa = pt_emit_arena(s);
    pt_msg *m = pt_emit_msg(s);
    pt_json *d = pt_json_parse(ev->data, sa, NULL);
    pt_str type;
    if (!d)
        return 0;
    type = pt_json_gets(d, "type", sa);

    if (pt_str_eqz(type, "response.created") || pt_str_eqz(type, "response.in_progress")) {
        const pt_json *r = pt_json_get(d, "response");
        if (!m->response_id.n)
            m->response_id = pt_arena_dup(fa, pt_json_gets(r, "id", sa));
        if (!m->response_model.n)
            m->response_model = pt_arena_dup(fa, pt_json_gets(r, "model", sa));
        pt_emit_start(s);
    } else if (pt_str_eqz(type, "response.output_item.added")) {
        const pt_json *item = pt_json_get(d, "item");
        pt_str it = pt_json_gets(item, "type", sa);
        long *p = slot(s, st, pt_json_geti(d, "output_index", -1));
        if (!p)
            return 0;
        if (pt_str_eqz(it, "reasoning")) {
            *p = pt_emit_block(s, PT_BLOCK_THINKING, PT_STR_NULL, PT_STR_NULL);
        } else if (pt_str_eqz(it, "message")) {
            *p = pt_emit_block(s, PT_BLOCK_TEXT, PT_STR_NULL, PT_STR_NULL);
        } else if (pt_str_eqz(it, "function_call")) {
            *p = pt_emit_block(s, PT_BLOCK_TOOL_CALL, pt_json_gets(item, "call_id", sa),
                               pt_json_gets(item, "name", sa));
            pt_emit_delta(s, *p, pt_json_gets(item, "arguments", sa));
            st->any_call = true;
        }
        /* other items become opaque blocks when they are done */
    } else if (pt_str_eqz(type, "response.reasoning_summary_part.added")) {
        if (pt_json_geti(d, "summary_index", 0) > 0)
            pt_emit_delta(s, block_at(s, st, d), PT_S("\n\n"));
    } else if (pt_str_eqz(type, "response.reasoning_summary_text.delta") ||
               pt_str_eqz(type, "response.reasoning_text.delta") ||
               pt_str_eqz(type, "response.output_text.delta") ||
               pt_str_eqz(type, "response.function_call_arguments.delta")) {
        pt_emit_delta(s, block_at(s, st, d), pt_json_gets(d, "delta", sa));
    } else if (pt_str_eqz(type, "response.refusal.delta")) {
        st->refusal = true;
        pt_emit_delta(s, block_at(s, st, d), pt_json_gets(d, "delta", sa));
    } else if (pt_str_eqz(type, "response.output_item.done")) {
        const pt_json *item = pt_json_get(d, "item");
        pt_str it = pt_json_gets(item, "type", sa);
        long i = block_at(s, st, d);
        /* The finished item is authoritative: the deltas may have been
         * revised along the way (reasoning summaries are). */
        if (pt_str_eqz(it, "reasoning")) {
            pt_emit_set_content(s, i, joined(item, "summary", "text", sa));
            pt_emit_signature(s, i, item->raw);
        } else if (pt_str_eqz(it, "function_call")) {
            const pt_json *args = pt_json_get(item, "arguments");
            if (pt_json_is(args, PT_J_STR))
                pt_emit_set_content(s, i, pt_json_str(args, sa));
        } else if (pt_str_eqz(it, "message")) {
            pt_str text = joined(item, "content", "text", sa);
            if (!text.n)
                text = joined(item, "content", "refusal", sa);
            pt_emit_set_content(s, i, text);
        } else if (item) {
            pt_emit_opaque(s, item->raw);
            return 0;
        }
        pt_emit_end(s, i);
    } else if (pt_str_eqz(type, "response.completed") || pt_str_eqz(type, "response.incomplete")) {
        finished(s, st, pt_json_get(d, "response"), sa);
    } else if (pt_str_eqz(type, "response.failed")) {
        fail_with(s, pt_json_get(pt_json_get(d, "response"), "error"), ev->data, sa);
    } else if (pt_str_eqz(type, "error")) {
        fail_with(s, pt_json_get(d, "error") ? pt_json_get(d, "error") : d, ev->data, sa);
    }
    return 0;
}

static void finish(pt_ai_stream *s, void *st)
{
    /* No response.completed: the stream was cut; the stream reports it. */
    (void)s;
    (void)st;
}

static void classify_error(int status, pt_str body, pt_ai_error *out, pt_arena *a)
{
    pt_json *d = pt_json_parse(body, a, NULL);
    pt_openai_error(pt_json_get(d, "error"), body, status, out, a);
}

const pt_api_vt pt_api_openai_responses = {
    "openai-responses", build, sizeof(state), feed, finish, classify_error,
    {.system_role = PT_SYSROLE_DEVELOPER, .mid_conversation_system = PT_YES, .thinking_display = PT_YES},
    64,
};
