/* The faux provider: scripted responses for tests and demos.
 *
 * pt_faux_respond() is the "server". It answers a request built by the faux
 * API from a script, as an SSE stream of unified events, so the whole stream
 * path (SSE parsing, chunking, cuts) is exercised without a network.
 *
 * Script:
 *   {"chunk": 16, "delay_ms": 0,
 *    "turns": [
 *      {"content": [{"type": "thinking", "text": "..."},
 *                   {"type": "text", "text": "..."},
 *                   {"type": "tool_call", "name": "echo", "args": {"text": "hi"}}],
 *       "stop": "tool_use", "usage": {"input": 10, "output": 5}},
 *      {"status": 529, "body": {"class": "retryable", "message": "overloaded"}},
 *      {"content": [...], "error": "boom", "class": "retryable"},
 *      {"content": [...], "cut": 120}]}
 * Turn n answers the request that has n assistant messages in its context. */
#include "ai.h"

/* ---- API ---- */

static bool build(const pt_ai_target *t, const pt_ai_context *ctx, const pt_ai_opts *o,
                  const pt_credentials *cred, pt_http_req *out, pt_arena *a)
{
    pt_buf b;
    pt_jw w;
    uint32_t i, turn = 0;
    (void)cred;
    for (i = 0; i < ctx->nmsgs; i++)
        if (ctx->msgs[i].role == PT_ROLE_ASSISTANT)
            turn++;
    pt_buf_init(&b, a->al);
    pt_jw_init(&w, &b);
    pt_jw_obj(&w);
    pt_jw_key(&w, "model");
    pt_jw_strz(&w, t->model.id);
    pt_jw_key(&w, "turn");
    pt_jw_uint(&w, turn);
    pt_jw_key(&w, "messages");
    pt_jw_uint(&w, ctx->nmsgs);
    pt_jw_key(&w, "tools");
    pt_jw_uint(&w, ctx->ntools);
    if (ctx->nmsgs) {
        pt_jw_key(&w, "last");
        pt_jw_str(&w, ctx->msgs[ctx->nmsgs - 1].text);
    }
    pt_jw_obj_end(&w);
    if (!pt_req_init(out, t, o, "/v1/respond", 1, a)) {
        pt_buf_free(&b);
        return false;
    }
    pt_req_header(out, "content-type", "application/json");
    pt_req_extra_headers(out, t, o);
    return pt_req_body(out, &b, a);
}

typedef struct state {
    long cur;
} state;

static bool lookup_stop(pt_str s, pt_stop *out)
{
    unsigned i;
    for (i = 0; i <= PT_STOP_ABORTED; i++)
        if (pt_str_eqz(s, pt_stop_name((pt_stop)i))) {
            *out = (pt_stop)i;
            return true;
        }
    return false;
}

static pt_err_class lookup_class(pt_str s)
{
    unsigned i;
    for (i = 1; i <= PT_ERR_ABORTED; i++)
        if (pt_str_eqz(s, pt_err_class_name((pt_err_class)i)))
            return (pt_err_class)i;
    return PT_ERR_FATAL;
}

static int feed(pt_ai_stream *s, void *vst, const pt_sse_event *ev)
{
    state *st = (state *)vst;
    pt_arena *sa = pt_emit_scratch(s), *fa = pt_emit_arena(s);
    pt_msg *m = pt_emit_msg(s);
    pt_json *d = pt_json_parse(ev->data, sa, NULL);
    pt_str e = ev->event;
    if (!d)
        return 0;
    if (pt_str_eqz(e, "start")) {
        m->response_id = pt_arena_dup(fa, pt_json_gets(d, "id", sa));
        m->response_model = pt_arena_dup(fa, pt_json_gets(d, "model", sa));
        pt_emit_start(s);
    } else if (pt_str_eqz(e, "block")) {
        pt_str type = pt_json_gets(d, "type", sa);
        pt_block_kind k = pt_str_eqz(type, "thinking")    ? PT_BLOCK_THINKING
                          : pt_str_eqz(type, "tool_call") ? PT_BLOCK_TOOL_CALL
                                                          : PT_BLOCK_TEXT;
        st->cur = pt_emit_block(s, k, pt_json_gets(d, "id", sa), pt_json_gets(d, "name", sa));
    } else if (pt_str_eqz(e, "delta")) {
        pt_emit_delta(s, st->cur, pt_json_gets(d, "text", sa));
    } else if (pt_str_eqz(e, "signature")) {
        pt_emit_signature(s, st->cur, pt_json_gets(d, "text", sa));
    } else if (pt_str_eqz(e, "end")) {
        pt_emit_end(s, st->cur);
    } else if (pt_str_eqz(e, "done")) {
        const pt_json *u = pt_json_get(d, "usage");
        pt_str stop = pt_json_gets(d, "stop", sa);
        m->raw_stop = pt_arena_dup(fa, stop);
        if (!lookup_stop(stop, &m->stop))
            m->stop = PT_STOP_STOP;
        m->usage.input = (uint64_t)pt_json_geti(u, "input", 0);
        m->usage.output = (uint64_t)pt_json_geti(u, "output", 0);
        pt_emit_done(s);
    } else if (pt_str_eqz(e, "error")) {
        pt_emit_fail(s, lookup_class(pt_json_gets(d, "class", sa)), pt_json_gets(d, "message", sa));
    }
    return 0;
}

static void finish(pt_ai_stream *s, void *st)
{
    (void)s;
    (void)st;
}

static void classify_error(int status, pt_str body, pt_ai_error *out, pt_arena *a)
{
    pt_json *d = pt_json_parse(body, a, NULL);
    pt_str msg = pt_json_gets(d, "message", a);
    pt_classify_status(status, msg.n ? msg : body, out);
    if (pt_json_get(d, "class"))
        out->cls = lookup_class(pt_json_gets(d, "class", a));
}

const pt_api_vt pt_api_faux = {"faux", build, sizeof(state), feed, finish, classify_error, {0}, 0};

/* ---- server ---- */

static void sse(pt_buf *b, const char *event, pt_buf *data)
{
    pt_buf_putz(b, "event: ");
    pt_buf_putz(b, event);
    pt_buf_putz(b, "\ndata: ");
    pt_buf_puts(b, pt_buf_str(data));
    pt_buf_putz(b, "\n\n");
    pt_buf_reset(data);
}

static void sse_text(pt_buf *b, const char *event, pt_buf *tmp, pt_str text)
{
    pt_jw w;
    pt_jw_init(&w, tmp);
    pt_jw_obj(&w);
    pt_jw_key(&w, "text");
    pt_jw_str(&w, text);
    pt_jw_obj_end(&w);
    sse(b, event, tmp);
}

/* Deltas of a few bytes each, split on UTF-8 character boundaries. */
static void deltas(pt_buf *b, pt_buf *tmp, pt_str text)
{
    size_t i = 0;
    while (i < text.n) {
        size_t j = i + 6;
        if (j > text.n)
            j = text.n;
        while (j < text.n && ((unsigned char)text.p[j] & 0xC0) == 0x80)
            j++;
        sse_text(b, "delta", tmp, (pt_str){text.p + i, j - i});
        i = j;
    }
}

bool pt_faux_respond(pt_str script, pt_str request_body, pt_faux_response *out, pt_arena *a)
{
    pt_json_err err;
    pt_json *root = pt_json_parse(script, a, &err);
    pt_json *req = pt_json_parse(request_body, a, NULL);
    const pt_json *turns, *turn, *content, *c, *body, *usage;
    int64_t n = pt_json_geti(req, "turn", 0), status;
    pt_buf b, tmp;
    pt_jw w;
    uint64_t out_bytes = 0, ncalls = 0, i = 0;
    pt_str stop;

    out->status = 200;
    out->body = PT_STR_NULL;
    out->chunk = 0;
    out->delay_ms = 0;
    out->cut = -1;
    if (!root || !req)
        return false;
    turns = pt_json_get(root, "turns");
    turn = n >= 0 ? pt_json_at(turns, (uint32_t)n) : NULL;
    out->chunk = (uint32_t)pt_json_geti(turn, "chunk", pt_json_geti(root, "chunk", 0));
    out->delay_ms = (uint32_t)pt_json_geti(turn, "delay_ms", pt_json_geti(root, "delay_ms", 0));
    out->cut = pt_json_geti(turn, "cut", -1);

    status = pt_json_geti(turn, "status", 200);
    if (status != 200) {
        out->status = (int)status;
        body = pt_json_get(turn, "body");
        out->body = pt_json_is(body, PT_J_STR) ? pt_json_str(body, a) : body ? body->raw : PT_STR_NULL;
        return true;
    }

    pt_buf_init(&b, a->al);
    pt_buf_init(&tmp, a->al);
    pt_jw_init(&w, &tmp);
    pt_jw_obj(&w);
    pt_jw_key(&w, "id");
    pt_buf_putz(&tmp, "\"faux-");
    pt_buf_i64(&tmp, n);
    pt_buf_putc(&tmp, '"');
    pt_jw_key(&w, "model");
    pt_jw_str(&w, pt_json_gets(req, "model", a));
    pt_jw_obj_end(&w);
    sse(&b, "start", &tmp);

    content = pt_json_get(turn, "content");
    if (!turn) { /* past the end of the script */
        pt_buf_putz(&tmp, "{\"type\":\"text\"}");
        sse(&b, "block", &tmp);
        deltas(&b, &tmp, PT_S("(the faux script has no more turns)"));
        pt_buf_putz(&tmp, "{}");
        sse(&b, "end", &tmp);
    }
    for (c = pt_json_is(content, PT_J_ARR) ? content->child : NULL; c; c = c->next, i++) {
        pt_str type = pt_json_gets(c, "type", a), payload;
        bool call = pt_str_eqz(type, "tool_call");
        pt_jw_init(&w, &tmp);
        pt_jw_obj(&w);
        pt_jw_key(&w, "type");
        pt_jw_str(&w, type);
        if (call) {
            const pt_json *args = pt_json_get(c, "args");
            pt_str id = pt_json_gets(c, "id", a);
            pt_jw_key(&w, "id");
            if (id.n) {
                pt_jw_str(&w, id);
            } else {
                pt_buf_putz(&tmp, "\"call_");
                pt_buf_i64(&tmp, n);
                pt_buf_putc(&tmp, '_');
                pt_buf_u64(&tmp, i);
                pt_buf_putc(&tmp, '"');
            }
            pt_jw_key(&w, "name");
            pt_jw_str(&w, pt_json_gets(c, "name", a));
            payload = pt_json_is(args, PT_J_STR) ? pt_json_str(args, a) : args ? args->raw : PT_S("{}");
            ncalls++;
        } else {
            payload = pt_json_gets(c, "text", a);
        }
        pt_jw_obj_end(&w);
        sse(&b, "block", &tmp);
        deltas(&b, &tmp, payload);
        out_bytes += payload.n;
        if (pt_str_eqz(type, "thinking")) {
            pt_str sig = pt_json_gets(c, "signature", a);
            sse_text(&b, "signature", &tmp, sig.n ? sig : PT_S("faux-signature"));
        }
        pt_buf_putz(&tmp, "{}");
        sse(&b, "end", &tmp);
    }

    if (pt_json_get(turn, "error")) {
        pt_jw_init(&w, &tmp);
        pt_jw_obj(&w);
        pt_jw_key(&w, "class");
        pt_jw_str(&w, pt_json_get(turn, "class") ? pt_json_gets(turn, "class", a) : PT_S("retryable"));
        pt_jw_key(&w, "message");
        pt_jw_str(&w, pt_json_gets(turn, "error", a));
        pt_jw_obj_end(&w);
        sse(&b, "error", &tmp);
    } else {
        stop = pt_json_gets(turn, "stop", a);
        usage = pt_json_get(turn, "usage");
        pt_jw_init(&w, &tmp);
        pt_jw_obj(&w);
        pt_jw_key(&w, "stop");
        pt_jw_str(&w, stop.n ? stop : ncalls ? PT_S("tool_use") : PT_S("stop"));
        pt_jw_key(&w, "usage");
        pt_jw_obj(&w);
        pt_jw_key(&w, "input");
        pt_jw_int(&w, pt_json_geti(usage, "input", (int64_t)request_body.n / 4));
        pt_jw_key(&w, "output");
        pt_jw_int(&w, pt_json_geti(usage, "output", (int64_t)(out_bytes / 4 + 1)));
        pt_jw_obj_end(&w);
        pt_jw_obj_end(&w);
        sse(&b, "done", &tmp);
    }

    out->body = pt_arena_dup(a, pt_buf_str(&b));
    if (b.oom || tmp.oom)
        a->oom = true;
    pt_buf_free(&b);
    pt_buf_free(&tmp);
    return !a->oom;
}
