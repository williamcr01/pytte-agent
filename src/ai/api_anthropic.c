/* api: anthropic-messages (POST /v1/messages, streaming). */
#include "ai.h"

#include <string.h>

/* ---- request ---- */

static const char *effort(pt_reasoning r)
{
    switch (r) {
    case PT_REASON_OFF:     /* thinking cannot be turned off on current models */
    case PT_REASON_MINIMAL:
    case PT_REASON_LOW: return "low";
    case PT_REASON_MEDIUM: return "medium";
    case PT_REASON_XHIGH: return "xhigh";
    case PT_REASON_MAX: return "max";
    case PT_REASON_DEFAULT:
    case PT_REASON_HIGH:
    default: return "high";
    }
}

/* Tool input must be a JSON object. Arguments that are not (cut off, or
 * invalid from eager streaming) are sent back wrapped, never as broken JSON. */
static void write_input(pt_jw *w, pt_str args, pt_arena *a)
{
    pt_json *v;
    if (!pt_str_trim(args).n) {
        pt_jw_raw(w, PT_S("{}"));
        return;
    }
    v = pt_json_parse(args, a, NULL);
    if (v && v->type == PT_J_OBJ) {
        pt_jw_raw(w, args);
        return;
    }
    pt_jw_obj(w);
    pt_jw_key(w, "INVALID_JSON");
    pt_jw_str(w, args);
    pt_jw_obj_end(w);
}

static void write_assistant(pt_jw *w, const pt_msg *m, pt_arena *a)
{
    uint32_t i;
    pt_jw_obj(w);
    pt_jw_key(w, "role");
    pt_jw_strz(w, "assistant");
    pt_jw_key(w, "content");
    pt_jw_arr(w);
    for (i = 0; i < m->nblocks; i++) {
        const pt_block *b = &m->blocks[i];
        switch (b->kind) {
        case PT_BLOCK_TEXT:
            if (!b->text.n)
                break;
            pt_jw_obj(w);
            pt_jw_key(w, "type");
            pt_jw_strz(w, "text");
            pt_jw_key(w, "text");
            pt_jw_str(w, b->text);
            pt_jw_obj_end(w);
            break;
        case PT_BLOCK_THINKING:
            if (!b->signature.n)
                break; /* unsigned thinking cannot be sent back */
            pt_jw_obj(w);
            pt_jw_key(w, "type");
            if (b->redacted) {
                pt_jw_strz(w, "redacted_thinking");
                pt_jw_key(w, "data");
                pt_jw_str(w, b->signature);
            } else {
                pt_jw_strz(w, "thinking");
                pt_jw_key(w, "thinking");
                pt_jw_str(w, b->text);
                pt_jw_key(w, "signature");
                pt_jw_str(w, b->signature);
            }
            pt_jw_obj_end(w);
            break;
        case PT_BLOCK_TOOL_CALL:
            pt_jw_obj(w);
            pt_jw_key(w, "type");
            pt_jw_strz(w, "tool_use");
            pt_jw_key(w, "id");
            pt_jw_str(w, b->id);
            pt_jw_key(w, "name");
            pt_jw_str(w, b->name);
            pt_jw_key(w, "input");
            write_input(w, b->args, a);
            pt_jw_obj_end(w);
            break;
        case PT_BLOCK_OPAQUE:
            if (b->raw.n)
                pt_jw_raw(w, b->raw);
            break;
        case PT_BLOCK_IMAGE:
            break;
        }
    }
    pt_jw_arr_end(w);
    pt_jw_obj_end(w);
}

static bool build(const pt_ai_target *t, const pt_ai_context *ctx, const pt_ai_opts *o,
                  const pt_credentials *cred, pt_http_req *out, pt_arena *a)
{
    pt_buf b;
    pt_jw w;
    uint32_t i, first = 0, max_tokens;
    bool in_user = false;
    pt_buf_init(&b, a->al);
    pt_jw_init(&w, &b);

    max_tokens = o->max_tokens ? o->max_tokens : pt_ai_auto_max_tokens(t, ctx, o->input_estimate);
    if (!max_tokens)
        max_tokens = 4096; /* required by the API; only for a model with unknown limits */

    pt_jw_obj(&w);
    pt_jw_key(&w, "model");
    pt_jw_strz(&w, t->model.id);
    pt_jw_key(&w, "max_tokens");
    pt_jw_uint(&w, max_tokens);
    pt_jw_key(&w, "stream");
    pt_jw_bool(&w, true);
    if (!o->no_cache) {
        pt_jw_key(&w, "cache_control");
        pt_jw_raw(&w, PT_S("{\"type\":\"ephemeral\"}"));
    }
    if (t->model.reasoning) {
        /* Always explicit: on older models omitting it means no thinking.
         * A summary trails the model, and the answer waits for it to end:
         * unread, it only delays the first text. Without display, current
         * models omit it. */
        pt_jw_key(&w, "thinking");
        pt_jw_raw(&w, t->compat.thinking_display == PT_YES && !o->hide_thinking
                          ? PT_S("{\"type\":\"adaptive\",\"display\":\"summarized\"}")
                          : PT_S("{\"type\":\"adaptive\"}"));
        pt_jw_key(&w, "output_config");
        pt_jw_obj(&w);
        pt_jw_key(&w, "effort");
        pt_jw_strz(&w, effort(o->reasoning));
        pt_jw_obj_end(&w);
    }
    if (t->compat.fallbacks == PT_YES) {
        pt_jw_key(&w, "fallbacks");
        pt_jw_strz(&w, "default");
    }
    if (o->temperature.n) {
        pt_jw_key(&w, "temperature");
        pt_jw_raw(&w, o->temperature);
    }
    if (ctx->nmsgs && ctx->msgs[0].role == PT_ROLE_SYSTEM) {
        first = 1;
        if (ctx->msgs[0].text.n) {
            pt_jw_key(&w, "system");
            pt_jw_str(&w, ctx->msgs[0].text);
        }
    }
    if (ctx->ntools) {
        pt_jw_key(&w, "tools");
        pt_jw_arr(&w);
        for (i = 0; i < ctx->ntools; i++) {
            const pt_tool *tool = &ctx->tools[i];
            pt_jw_obj(&w);
            pt_jw_key(&w, "name");
            pt_jw_str(&w, tool->name);
            if (tool->description.n) {
                pt_jw_key(&w, "description");
                pt_jw_str(&w, tool->description);
            }
            pt_jw_key(&w, "input_schema");
            pt_jw_raw(&w, tool->schema.n ? tool->schema : PT_S("{\"type\":\"object\"}"));
            pt_jw_key(&w, "eager_input_streaming");
            pt_jw_bool(&w, true);
            pt_jw_obj_end(&w);
        }
        pt_jw_arr_end(&w);
    }

    /* All user-side content between two assistant turns (tool results
     * first, then text) goes into one user message. */
    pt_jw_key(&w, "messages");
    pt_jw_arr(&w);
    for (i = first; i < ctx->nmsgs; i++) {
        const pt_msg *m = &ctx->msgs[i];
        if (m->role == PT_ROLE_USER || m->role == PT_ROLE_TOOL_RESULT) {
            if (m->role == PT_ROLE_USER && !m->text.n)
                continue;
            if (!in_user) {
                pt_jw_obj(&w);
                pt_jw_key(&w, "role");
                pt_jw_strz(&w, "user");
                pt_jw_key(&w, "content");
                pt_jw_arr(&w);
                in_user = true;
            }
            pt_jw_obj(&w);
            if (m->role == PT_ROLE_USER) {
                pt_jw_key(&w, "type");
                pt_jw_strz(&w, "text");
                pt_jw_key(&w, "text");
                pt_jw_str(&w, m->text);
            } else {
                pt_jw_key(&w, "type");
                pt_jw_strz(&w, "tool_result");
                pt_jw_key(&w, "tool_use_id");
                pt_jw_str(&w, m->call_id);
                if (pt_ai_sends_image(t, m)) {
                    pt_jw_key(&w, "content");
                    pt_jw_arr(&w);
                    if (m->text.n) {
                        pt_jw_obj(&w);
                        pt_jw_key(&w, "type");
                        pt_jw_strz(&w, "text");
                        pt_jw_key(&w, "text");
                        pt_jw_str(&w, m->text);
                        pt_jw_obj_end(&w);
                    }
                    pt_jw_obj(&w);
                    pt_jw_key(&w, "type");
                    pt_jw_strz(&w, "image");
                    pt_jw_key(&w, "source");
                    pt_jw_obj(&w);
                    pt_jw_key(&w, "type");
                    pt_jw_strz(&w, "base64");
                    pt_jw_key(&w, "media_type");
                    pt_jw_str(&w, m->image_mime);
                    pt_jw_key(&w, "data");
                    pt_jw_base64(&w, NULL, m->image);
                    pt_jw_obj_end(&w);
                    pt_jw_obj_end(&w);
                    pt_jw_arr_end(&w);
                } else {
                    pt_str text = pt_ai_result_text(t, m, a);
                    if (text.n) {
                        pt_jw_key(&w, "content");
                        pt_jw_str(&w, text);
                    }
                }
                if (m->is_error) {
                    pt_jw_key(&w, "is_error");
                    pt_jw_bool(&w, true);
                }
            }
            pt_jw_obj_end(&w);
            continue;
        }
        if (in_user) {
            pt_jw_arr_end(&w);
            pt_jw_obj_end(&w);
            in_user = false;
        }
        if (m->role == PT_ROLE_ASSISTANT) {
            write_assistant(&w, m, a);
        } else { /* later system message, in place (the view folded it otherwise) */
            pt_jw_obj(&w);
            pt_jw_key(&w, "role");
            pt_jw_strz(&w, "system");
            pt_jw_key(&w, "content");
            pt_jw_str(&w, m->text);
            pt_jw_obj_end(&w);
        }
    }
    if (in_user) {
        pt_jw_arr_end(&w);
        pt_jw_obj_end(&w);
    }
    pt_jw_arr_end(&w);
    pt_jw_obj_end(&w);

    if (!pt_req_init(out, t, o, "/v1/messages", 6, a)) {
        pt_buf_free(&b);
        return false;
    }
    pt_req_header(out, "content-type", "application/json");
    pt_req_header(out, "accept", "text/event-stream");
    pt_req_header(out, "anthropic-version", "2023-06-01");
    if (t->compat.fallbacks == PT_YES)
        pt_req_header(out, "anthropic-beta", "server-side-fallback-2026-07-01");
    if (cred && cred->api_key.n)
        pt_req_header(out, "x-api-key", pt_arena_cstr(a, cred->api_key));
    pt_req_extra_headers(out, t, o);
    return pt_req_body(out, &b, a);
}

/* ---- stream ---- */

typedef struct state {
    long *map; /* wire block index → unified index */
    uint32_t cap;
} state;

static long *slot(pt_ai_stream *s, state *st, int64_t wire)
{
    if (wire < 0 || wire > 100000)
        return NULL;
    if ((uint64_t)wire >= st->cap) {
        uint32_t ncap = st->cap ? st->cap : 16, i;
        long *nm;
        while (ncap <= (uint64_t)wire)
            ncap *= 2;
        nm = (long *)pt_arena_alloc(pt_emit_own(s), sizeof(long) * ncap);
        if (!nm)
            return NULL;
        for (i = 0; i < ncap; i++)
            nm[i] = i < st->cap ? st->map[i] : -1;
        st->map = nm;
        st->cap = ncap;
    }
    return &st->map[wire];
}

static long block_of(pt_ai_stream *s, state *st, const pt_json *data)
{
    long *p = slot(s, st, pt_json_geti(data, "index", -1));
    return p ? *p : -1;
}

static pt_stop map_stop(pt_str r)
{
    if (pt_str_eqz(r, "tool_use"))
        return PT_STOP_TOOL_USE;
    if (pt_str_eqz(r, "max_tokens") || pt_str_eqz(r, "model_context_window_exceeded"))
        return PT_STOP_LENGTH;
    if (pt_str_eqz(r, "refusal"))
        return PT_STOP_REFUSAL;
    return PT_STOP_STOP; /* end_turn, stop_sequence, pause_turn */
}

static void read_usage(pt_msg *m, const pt_json *u)
{
    int64_t v;
    if (!u)
        return;
    if (pt_json_int(pt_json_get(u, "input_tokens"), &v) && v >= 0)
        m->usage.input = (uint64_t)v;
    if (pt_json_int(pt_json_get(u, "output_tokens"), &v) && v >= 0)
        m->usage.output = (uint64_t)v;
    if (pt_json_int(pt_json_get(u, "cache_read_input_tokens"), &v) && v >= 0)
        m->usage.cache_read = (uint64_t)v;
    if (pt_json_int(pt_json_get(u, "cache_creation_input_tokens"), &v) && v >= 0)
        m->usage.cache_write = (uint64_t)v;
}

static int status_of_error_type(pt_str type)
{
    static const struct {
        const char *type;
        int status;
    } map[] = {
        {"invalid_request_error", 400}, {"authentication_error", 401}, {"billing_error", 402},
        {"permission_error", 403},      {"not_found_error", 404},      {"request_too_large", 413},
        {"rate_limit_error", 429},      {"api_error", 500},            {"timeout_error", 504},
        {"overloaded_error", 529},
    };
    size_t i;
    for (i = 0; i < sizeof map / sizeof map[0]; i++)
        if (pt_str_eqz(type, map[i].type))
            return map[i].status;
    return 500;
}

/* "<type>: <message>" from {"type":"error","error":{"type","message"}}. */
static pt_str error_text(const pt_json *err, pt_str fallback, pt_str *type, pt_arena *a)
{
    pt_buf b;
    pt_str msg = pt_json_gets(err, "message", a), r;
    *type = pt_json_gets(err, "type", a);
    if (!msg.n) {
        msg = pt_str_trim(fallback);
        if (msg.n > 500)
            msg.n = 500;
    }
    pt_buf_init(&b, a->al);
    if (type->n) {
        pt_buf_puts(&b, *type);
        pt_buf_putz(&b, ": ");
    }
    pt_buf_puts(&b, msg);
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
        return 0; /* not JSON: ignore */
    type = pt_json_gets(d, "type", sa);

    if (pt_str_eqz(type, "message_start")) {
        const pt_json *msg = pt_json_get(d, "message");
        m->response_id = pt_arena_dup(fa, pt_json_gets(msg, "id", sa));
        m->response_model = pt_arena_dup(fa, pt_json_gets(msg, "model", sa));
        read_usage(m, pt_json_get(msg, "usage"));
        pt_emit_start(s);
    } else if (pt_str_eqz(type, "content_block_start")) {
        const pt_json *cb = pt_json_get(d, "content_block");
        pt_str bt = pt_json_gets(cb, "type", sa);
        long *p = slot(s, st, pt_json_geti(d, "index", -1)), i;
        if (!p)
            return 0;
        if (pt_str_eqz(bt, "text")) {
            i = pt_emit_block(s, PT_BLOCK_TEXT, PT_STR_NULL, PT_STR_NULL);
            pt_emit_delta(s, i, pt_json_gets(cb, "text", sa));
        } else if (pt_str_eqz(bt, "thinking")) {
            i = pt_emit_block(s, PT_BLOCK_THINKING, PT_STR_NULL, PT_STR_NULL);
            pt_emit_delta(s, i, pt_json_gets(cb, "thinking", sa));
            pt_emit_signature(s, i, pt_json_gets(cb, "signature", sa));
        } else if (pt_str_eqz(bt, "redacted_thinking")) {
            pt_emit_redacted(s, pt_json_gets(cb, "data", sa));
            i = (long)m->nblocks - 1;
        } else if (pt_str_eqz(bt, "tool_use")) {
            const pt_json *input = pt_json_get(cb, "input");
            i = pt_emit_block(s, PT_BLOCK_TOOL_CALL, pt_json_gets(cb, "id", sa),
                              pt_json_gets(cb, "name", sa));
            /* Input normally streams as deltas after an empty {}. */
            if (input && !(input->type == PT_J_OBJ && input->len == 0))
                pt_emit_delta(s, i, input->raw);
        } else {
            pt_emit_opaque(s, cb ? cb->raw : PT_S("null"));
            i = -1; /* deltas for unknown blocks are ignored */
        }
        *p = i;
    } else if (pt_str_eqz(type, "content_block_delta")) {
        const pt_json *delta = pt_json_get(d, "delta");
        pt_str dt = pt_json_gets(delta, "type", sa);
        long i = block_of(s, st, d);
        if (pt_str_eqz(dt, "text_delta"))
            pt_emit_delta(s, i, pt_json_gets(delta, "text", sa));
        else if (pt_str_eqz(dt, "thinking_delta"))
            pt_emit_delta(s, i, pt_json_gets(delta, "thinking", sa));
        else if (pt_str_eqz(dt, "signature_delta"))
            pt_emit_signature(s, i, pt_json_gets(delta, "signature", sa));
        else if (pt_str_eqz(dt, "input_json_delta"))
            pt_emit_delta(s, i, pt_json_gets(delta, "partial_json", sa));
    } else if (pt_str_eqz(type, "content_block_stop")) {
        pt_emit_end(s, block_of(s, st, d));
    } else if (pt_str_eqz(type, "message_delta")) {
        const pt_json *delta = pt_json_get(d, "delta");
        const pt_json *reason = pt_json_get(delta, "stop_reason");
        if (pt_json_is(reason, PT_J_STR)) {
            m->raw_stop = pt_arena_dup(fa, pt_json_str(reason, sa));
            m->stop = map_stop(m->raw_stop);
        }
        read_usage(m, pt_json_get(d, "usage"));
    } else if (pt_str_eqz(type, "message_stop")) {
        if (!m->raw_stop.n)
            m->stop = PT_STOP_STOP;
        pt_emit_done(s);
    } else if (pt_str_eqz(type, "error")) {
        pt_ai_error e;
        pt_str et;
        pt_str text = error_text(pt_json_get(d, "error"), ev->data, &et, sa);
        pt_classify_status(status_of_error_type(et), text, &e);
        pt_emit_fail(s, e.cls, text);
    }
    /* ping and unknown events are ignored */
    return 0;
}

static void finish(pt_ai_stream *s, void *st)
{
    /* No message_stop: the stream was cut. The stream reports the error. */
    (void)s;
    (void)st;
}

static void classify_error(int status, pt_str body, pt_ai_error *out, pt_arena *a)
{
    pt_json *d = pt_json_parse(body, a, NULL);
    pt_str type;
    pt_str text = error_text(pt_json_get(d, "error"), body, &type, a);
    pt_buf b;
    pt_buf_init(&b, a->al);
    pt_buf_putz(&b, "HTTP ");
    pt_buf_i64(&b, status);
    if (text.n) {
        pt_buf_putz(&b, " ");
        pt_buf_puts(&b, text);
    }
    pt_classify_status(status, pt_arena_dup(a, pt_buf_str(&b)), out);
    pt_buf_free(&b);
}

const pt_api_vt pt_api_anthropic = {
    "anthropic-messages", build, sizeof(state), feed, finish, classify_error,
    {.thinking_display = PT_NO, .mid_conversation_system = PT_NO, .fallbacks = PT_NO},
    64,
};
