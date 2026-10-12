/* api: openai-completions (Chat Completions, as spoken by OpenAI, OpenRouter
 * and most other providers). Differences between servers are compat flags. */
#include "ai.h"

/* ---- request ---- */

const char *pt_openai_effort(pt_reasoning r)
{
    switch (r) {
    case PT_REASON_OFF: return "none";
    case PT_REASON_MINIMAL: return "minimal";
    case PT_REASON_LOW: return "low";
    case PT_REASON_MEDIUM: return "medium";
    case PT_REASON_XHIGH:
    case PT_REASON_MAX: return "xhigh";
    case PT_REASON_DEFAULT:
    case PT_REASON_HIGH:
    default: return "high";
    }
}

static void write_assistant(pt_jw *w, const pt_msg *m)
{
    uint32_t i, ntext = 0, ncalls = 0;
    pt_jw_obj(w);
    pt_jw_key(w, "role");
    pt_jw_strz(w, "assistant");
    for (i = 0; i < m->nblocks; i++) {
        if (m->blocks[i].kind == PT_BLOCK_TEXT && m->blocks[i].text.n)
            ntext++;
        if (m->blocks[i].kind == PT_BLOCK_TOOL_CALL)
            ncalls++;
    }
    pt_jw_key(w, "content");
    if (ntext == 0 && ncalls) {
        pt_jw_null(w);
    } else { /* text blocks joined into one string */
        pt_buf text;
        pt_buf_init(&text, w->b->al);
        for (i = 0; i < m->nblocks; i++)
            if (m->blocks[i].kind == PT_BLOCK_TEXT)
                pt_buf_puts(&text, m->blocks[i].text);
        pt_jw_str(w, pt_buf_str(&text));
        if (text.oom)
            w->b->oom = true;
        pt_buf_free(&text);
    }
    if (ncalls) {
        pt_jw_key(w, "tool_calls");
        pt_jw_arr(w);
        for (i = 0; i < m->nblocks; i++) {
            const pt_block *b = &m->blocks[i];
            if (b->kind != PT_BLOCK_TOOL_CALL)
                continue;
            pt_jw_obj(w);
            pt_jw_key(w, "id");
            pt_jw_str(w, b->id);
            pt_jw_key(w, "type");
            pt_jw_strz(w, "function");
            pt_jw_key(w, "function");
            pt_jw_obj(w);
            pt_jw_key(w, "name");
            pt_jw_str(w, b->name);
            pt_jw_key(w, "arguments");
            pt_jw_str(w, pt_str_trim(b->args).n ? b->args : PT_S("{}"));
            pt_jw_obj_end(w);
            pt_jw_obj_end(w);
        }
        pt_jw_arr_end(w);
    }
    /* Opaque blocks are objects of extra fields for the wire message, such
     * as OpenRouter's reasoning_details. */
    for (i = 0; i < m->nblocks; i++) {
        const pt_block *b = &m->blocks[i];
        size_t n = b->raw.n;
        if (b->kind != PT_BLOCK_OPAQUE || n < 2 || b->raw.p[0] != '{' || b->raw.p[n - 1] != '}')
            continue;
        if (!pt_str_trim((pt_str){b->raw.p + 1, n - 2}).n)
            continue;
        pt_buf_putc(w->b, ',');
        pt_buf_put(w->b, b->raw.p + 1, n - 2);
    }
    pt_jw_obj_end(w);
}

/* Tool messages hold only text: the images of the results that end at
 * last follow them in one user message. */
static void write_result_images(pt_jw *w, const pt_ai_target *t, const pt_ai_context *ctx, uint32_t last,
                                pt_arena *a)
{
    uint32_t first = last, i;
    bool any = false;
    while (first > 0 && ctx->msgs[first - 1].role == PT_ROLE_TOOL_RESULT)
        first--;
    for (i = first; i <= last; i++) {
        const pt_msg *m = &ctx->msgs[i];
        if (!pt_ai_sends_image(t, m))
            continue;
        if (!any) {
            pt_jw_obj(w);
            pt_jw_key(w, "role");
            pt_jw_strz(w, "user");
            pt_jw_key(w, "content");
            pt_jw_arr(w);
            pt_jw_obj(w);
            pt_jw_key(w, "type");
            pt_jw_strz(w, "text");
            pt_jw_key(w, "text");
            pt_jw_strz(w, "The images from the tool results above, in order:");
            pt_jw_obj_end(w);
            any = true;
        }
        pt_jw_obj(w);
        pt_jw_key(w, "type");
        pt_jw_strz(w, "image_url");
        pt_jw_key(w, "image_url");
        pt_jw_obj(w);
        pt_jw_key(w, "url");
        pt_ai_image_url(w, m, a);
        pt_jw_obj_end(w);
        pt_jw_obj_end(w);
    }
    if (any) {
        pt_jw_arr_end(w);
        pt_jw_obj_end(w);
    }
}

static bool build(const pt_ai_target *t, const pt_ai_context *ctx, const pt_ai_opts *o,
                  const pt_credentials *cred, pt_http_req *out, pt_arena *a)
{
    pt_buf b;
    pt_jw w;
    uint32_t i, max_tokens;
    const char *sysrole = t->compat.system_role == PT_SYSROLE_DEVELOPER ? "developer" : "system";
    pt_buf_init(&b, a->al);
    pt_jw_init(&w, &b);

    max_tokens = o->max_tokens ? o->max_tokens : pt_ai_auto_max_tokens(t, ctx, o->input_estimate);

    pt_jw_obj(&w);
    pt_jw_key(&w, "model");
    pt_jw_strz(&w, t->model.id);
    pt_jw_key(&w, "stream");
    pt_jw_bool(&w, true);
    if (t->compat.stream_usage != PT_NO) {
        pt_jw_key(&w, "stream_options");
        pt_jw_raw(&w, PT_S("{\"include_usage\":true}"));
    }
    if (max_tokens) {
        pt_jw_key(&w, t->compat.max_tokens_field == PT_MAXTOK_MAX_COMPLETION_TOKENS
                          ? "max_completion_tokens"
                          : "max_tokens");
        pt_jw_uint(&w, max_tokens);
    }
    /* Reasoning is only asked for when the caller chose a level: servers
     * have their own defaults and some models reject the parameter. */
    if (o->reasoning != PT_REASON_DEFAULT) {
        if (t->compat.reasoning_param == PT_RPARAM_EFFORT) {
            pt_jw_key(&w, "reasoning_effort");
            pt_jw_strz(&w, pt_openai_effort(o->reasoning));
        } else if (t->compat.reasoning_param == PT_RPARAM_OPENROUTER) {
            pt_jw_key(&w, "reasoning");
            pt_jw_obj(&w);
            if (o->reasoning == PT_REASON_OFF) {
                pt_jw_key(&w, "enabled");
                pt_jw_bool(&w, false);
            } else {
                pt_jw_key(&w, "effort");
                pt_jw_strz(&w, o->reasoning == PT_REASON_XHIGH || o->reasoning == PT_REASON_MAX
                                   ? "high"
                                   : pt_openai_effort(o->reasoning));
            }
            pt_jw_obj_end(&w);
        }
    }
    if (o->temperature.n) {
        pt_jw_key(&w, "temperature");
        pt_jw_raw(&w, o->temperature);
    }

    pt_jw_key(&w, "messages");
    pt_jw_arr(&w);
    for (i = 0; i < ctx->nmsgs; i++) {
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
            pt_jw_str(&w, m->text);
            pt_jw_obj_end(&w);
            break;
        case PT_ROLE_ASSISTANT:
            write_assistant(&w, m);
            break;
        case PT_ROLE_TOOL_RESULT:
            pt_jw_obj(&w);
            pt_jw_key(&w, "role");
            pt_jw_strz(&w, "tool");
            pt_jw_key(&w, "tool_call_id");
            pt_jw_str(&w, m->call_id);
            pt_jw_key(&w, "content");
            pt_jw_str(&w, pt_ai_result_text(t, m, a));
            pt_jw_obj_end(&w);
            if (i + 1 == ctx->nmsgs || ctx->msgs[i + 1].role != PT_ROLE_TOOL_RESULT)
                write_result_images(&w, t, ctx, i, a);
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
            pt_jw_key(&w, "function");
            pt_jw_obj(&w);
            pt_jw_key(&w, "name");
            pt_jw_str(&w, tool->name);
            if (tool->description.n) {
                pt_jw_key(&w, "description");
                pt_jw_str(&w, tool->description);
            }
            pt_jw_key(&w, "parameters");
            pt_jw_raw(&w, tool->schema.n ? tool->schema : PT_S("{\"type\":\"object\"}"));
            pt_jw_obj_end(&w);
            pt_jw_obj_end(&w);
        }
        pt_jw_arr_end(&w);
        if (t->compat.parallel_tool_calls == PT_YES) {
            pt_jw_key(&w, "parallel_tool_calls");
            pt_jw_bool(&w, true);
        }
    }
    pt_jw_obj_end(&w);

    if (!pt_req_init(out, t, o, "/chat/completions", 4, a)) {
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
    long text, thinking; /* open text / thinking block, or -1 */
    long *calls;         /* tool call index → block */
    uint32_t cap;
    bool init, finished, any_call;
} state;

static void close_open(pt_ai_stream *s, state *st)
{
    if (st->text >= 0)
        pt_emit_end(s, st->text);
    if (st->thinking >= 0)
        pt_emit_end(s, st->thinking);
    st->text = st->thinking = -1;
}

static long *call_slot(pt_ai_stream *s, state *st, int64_t idx)
{
    if (idx < 0 || idx > 10000)
        return NULL;
    if ((uint64_t)idx >= st->cap) {
        uint32_t ncap = st->cap ? st->cap : 8, i;
        long *n;
        while (ncap <= (uint64_t)idx)
            ncap *= 2;
        n = (long *)pt_arena_alloc(pt_emit_own(s), sizeof(long) * ncap);
        if (!n)
            return NULL;
        for (i = 0; i < ncap; i++)
            n[i] = i < st->cap ? st->calls[i] : -1;
        st->calls = n;
        st->cap = ncap;
    }
    return &st->calls[idx];
}

static pt_stop map_stop(pt_str r)
{
    if (pt_str_eqz(r, "tool_calls") || pt_str_eqz(r, "function_call"))
        return PT_STOP_TOOL_USE;
    if (pt_str_eqz(r, "length"))
        return PT_STOP_LENGTH;
    if (pt_str_eqz(r, "content_filter"))
        return PT_STOP_REFUSAL;
    return PT_STOP_STOP;
}

static void read_usage(pt_msg *m, const pt_json *u)
{
    int64_t prompt = pt_json_geti(u, "prompt_tokens", 0);
    int64_t out = pt_json_geti(u, "completion_tokens", 0);
    const pt_json *pd = pt_json_get(u, "prompt_tokens_details");
    const pt_json *cd = pt_json_get(u, "completion_tokens_details");
    int64_t cached = pt_json_geti(pd, "cached_tokens", 0);
    int64_t written = pt_json_geti(pd, "cache_write_tokens", 0);
    int64_t reasoning = pt_json_geti(cd, "reasoning_tokens", 0);
    if (!pt_json_is(u, PT_J_OBJ))
        return;
    {
        uint64_t p = prompt > 0 ? (uint64_t)prompt : 0;
        uint64_t c = cached > 0 ? (uint64_t)cached : 0;
        uint64_t w = written > 0 ? (uint64_t)written : 0;
        m->usage.input = p > c && p - c > w ? p - c - w : 0;
        m->usage.cache_read = c;
        m->usage.cache_write = w;
    }
    m->usage.output = out > 0 ? (uint64_t)out : 0;
    m->usage.reasoning = reasoning > 0 ? (uint64_t)reasoning : 0;
}

/* {"error":{"message","type","code"}}: OpenAI's errors; OpenRouter sends
 * the same shape with a numeric code, also inside the stream. */
void pt_openai_error(const pt_json *err, pt_str fallback, int status, pt_ai_error *out, pt_arena *a)
{
    pt_str msg = pt_json_gets(err, "message", a);
    const pt_json *code = pt_json_get(err, "code");
    pt_str code_s = pt_json_str(code, a);
    int64_t code_i;
    pt_buf b;
    if (pt_json_int(code, &code_i) && code_i >= 400 && code_i < 600 && !status)
        status = (int)code_i;
    if (!msg.n) {
        msg = pt_str_trim(fallback);
        if (msg.n > 500)
            msg.n = 500;
    }
    pt_buf_init(&b, a->al);
    if (status) {
        pt_buf_putz(&b, "HTTP ");
        pt_buf_i64(&b, status);
        pt_buf_putc(&b, ' ');
    }
    if (code_s.n) {
        pt_buf_puts(&b, code_s);
        pt_buf_putz(&b, ": ");
    }
    pt_buf_puts(&b, msg);
    pt_classify_status(status ? status : 500, pt_arena_dup(a, pt_buf_str(&b)), out);
    if (pt_str_eqz(code_s, "context_length_exceeded"))
        out->cls = PT_ERR_OVERFLOW;
    pt_buf_free(&b);
}

static pt_str reasoning_text(const pt_json *delta, pt_arena *a)
{
    static const char *const fields[] = {"reasoning_content", "reasoning", "reasoning_text"};
    size_t i;
    for (i = 0; i < sizeof fields / sizeof fields[0]; i++) {
        pt_str t = pt_json_gets(delta, fields[i], a);
        if (t.n)
            return t;
    }
    return PT_STR_NULL;
}

static void settle(pt_ai_stream *s, state *st)
{
    pt_msg *m = pt_emit_msg(s);
    if (!st->finished) {
        pt_emit_fail(s, PT_ERR_RETRYABLE, PT_S("stream ended without a finish reason"));
        return;
    }
    if (m->stop == PT_STOP_STOP && st->any_call)
        m->stop = PT_STOP_TOOL_USE;
    pt_emit_done(s);
}

static int feed(pt_ai_stream *s, void *vst, const pt_sse_event *ev)
{
    state *st = (state *)vst;
    pt_arena *sa = pt_emit_scratch(s), *fa = pt_emit_arena(s);
    pt_msg *m = pt_emit_msg(s);
    const pt_json *choice, *delta, *calls, *c, *fr;
    pt_json *d;
    pt_str t;

    if (!st->init) {
        st->text = st->thinking = -1;
        st->init = true;
    }
    if (pt_str_eqz(pt_str_trim(ev->data), "[DONE]")) {
        settle(s, st);
        return 1;
    }
    d = pt_json_parse(ev->data, sa, NULL);
    if (!d)
        return 0;
    if (pt_json_get(d, "error")) {
        pt_ai_error e;
        pt_openai_error(pt_json_get(d, "error"), ev->data, 0, &e, sa);
        pt_emit_fail(s, e.cls, e.message);
        return 1;
    }
    if (!m->response_id.n)
        m->response_id = pt_arena_dup(fa, pt_json_gets(d, "id", sa));
    if (!m->response_model.n)
        m->response_model = pt_arena_dup(fa, pt_json_gets(d, "model", sa));
    pt_emit_start(s);
    if (pt_json_is(pt_json_get(d, "usage"), PT_J_OBJ))
        read_usage(m, pt_json_get(d, "usage"));

    choice = pt_json_at(pt_json_get(d, "choices"), 0);
    if (!choice)
        return 0;
    delta = pt_json_get(choice, "delta");
    t = pt_emit_target(s)->compat.reasoning_delta == PT_NO ? PT_STR_NULL : reasoning_text(delta, sa);
    if (t.n) {
        if (st->thinking < 0) {
            close_open(s, st);
            st->thinking = pt_emit_block(s, PT_BLOCK_THINKING, PT_STR_NULL, PT_STR_NULL);
        }
        pt_emit_delta(s, st->thinking, t);
    }
    t = pt_json_gets(delta, "content", sa);
    if (t.n) {
        if (st->text < 0) {
            close_open(s, st);
            st->text = pt_emit_block(s, PT_BLOCK_TEXT, PT_STR_NULL, PT_STR_NULL);
        }
        pt_emit_delta(s, st->text, t);
    }
    calls = pt_json_get(delta, "tool_calls");
    for (c = pt_json_is(calls, PT_J_ARR) ? calls->child : NULL; c; c = c->next) {
        const pt_json *fn = pt_json_get(c, "function");
        long *slot = call_slot(s, st, pt_json_geti(c, "index", 0));
        if (!slot)
            continue;
        if (*slot < 0) {
            pt_str id = pt_json_gets(c, "id", sa);
            close_open(s, st);
            if (!id.n) { /* some servers omit IDs: make a stable one */
                pt_buf b;
                pt_buf_init(&b, sa->al);
                pt_buf_putz(&b, "call_");
                pt_buf_i64(&b, pt_json_geti(c, "index", 0));
                id = pt_arena_dup(sa, pt_buf_str(&b));
                pt_buf_free(&b);
            }
            *slot = pt_emit_block(s, PT_BLOCK_TOOL_CALL, id, pt_json_gets(fn, "name", sa));
            st->any_call = true;
        }
        pt_emit_delta(s, *slot, pt_json_gets(fn, "arguments", sa));
    }
    fr = pt_json_get(choice, "finish_reason");
    if (pt_json_is(fr, PT_J_STR)) {
        pt_str r = pt_json_str(fr, sa);
        if (pt_str_eqz(r, "error")) {
            pt_emit_fail(s, PT_ERR_RETRYABLE, PT_S("the provider reported an error mid-stream"));
            return 1;
        }
        m->raw_stop = pt_arena_dup(fa, r);
        m->stop = map_stop(r);
        st->finished = true;
    }
    return 0;
}

static void finish(pt_ai_stream *s, void *vst)
{
    /* no [DONE]: fine if a finish reason arrived (usage may be missing) */
    state *st = (state *)vst;
    settle(s, st);
}

static void classify_error(int status, pt_str body, pt_ai_error *out, pt_arena *a)
{
    pt_json *d = pt_json_parse(body, a, NULL);
    pt_openai_error(pt_json_get(d, "error"), body, status, out, a);
}

const pt_api_vt pt_api_openai = {
    "openai-completions", build, sizeof(state), feed, finish, classify_error,
    {.max_tokens_field = PT_MAXTOK_MAX_TOKENS,
     .system_role = PT_SYSROLE_SYSTEM,
     .stream_usage = PT_YES,
     .reasoning_param = PT_RPARAM_NONE,
     .reasoning_delta = PT_YES,
     .mid_conversation_system = PT_YES,
     .parallel_tool_calls = PT_NO},
    40,
};
