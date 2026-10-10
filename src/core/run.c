/* The run task: answers one input, one request at a time (section 8.1).
 *
 *   prepare ──► request ──► done
 *      ▲          │  │
 *      │          │  └──► retry (sleep) ──► prepare, attempt + 1
 *      └──────────┘ tool calls (no tools yet: each gets an error result)
 *
 * prepare is pure; it pins the request's parameters in one commit.
 * request sends exactly what the pinned checkpoint describes, so a request
 * re-sent after a crash has the same bytes as the original. */
#include "core/core.h"

#include <string.h>

struct run_live {
    pt_agent *agent;
    uint32_t task;
    pt_arena a; /* request and final message */
    pt_ai_target target;
    pt_http_req req;
    pt_ai_stream *s;
    uint32_t http_fx;
    bool http_done, finished;
    int64_t last_live;
    bool dirty; /* content arrived since the last snapshot */
    pt_buf snap;
};

void pt_run_free_live(pt_agent *a, task *t)
{
    run_live *rl = t->live;
    if (!rl)
        return;
    pt_ai_stream_free(rl->s);
    pt_buf_free(&rl->snap);
    pt_arena_free(&rl->a);
    a->al.fn(a->al.ud, rl, sizeof *rl, 0);
    t->live = NULL;
}

static bool resolve(pt_agent *a, pt_str ref, pt_ai_target *out, pt_arena *ar)
{
    const char *why;
    if (!ref.n) /* none configured: keep the session's model */
        ref = a->last_model;
    if (!ref.n) {
        pt_core_fail(a, PT_FATAL_CONFIG, "no model configured", PT_STR_NULL);
        return false;
    }
    if (!pt_ai_resolve(a->o.catalog, ref, out, ar, &why)) {
        pt_core_fail(a, PT_FATAL_CONFIG, why ? why : "out of memory", ref);
        return false;
    }
    return true;
}

/* The provider's API key; a missing one stops the agent without a commit,
 * so the run can continue once it is set (or with another model). */
static bool credentials(pt_agent *a, const pt_provider *p, pt_credentials *cred)
{
    pt_buf m;
    memset(cred, 0, sizeof *cred);
    if (!a->o.credentials || a->o.credentials(a->o.ud, p, cred))
        return true;
    pt_buf_init(&m, a->al);
    pt_buf_putz(&m, "no API key for ");
    pt_buf_putz(&m, p->id);
    if (p->key_env[0]) {
        pt_buf_putz(&m, ": set ");
        pt_buf_putz(&m, p->key_env[0]);
    }
    pt_core_fail(a, PT_FATAL_CONFIG, "", pt_buf_str(&m));
    pt_buf_free(&m);
    return false;
}

/* ---- prepare ---- */

static bool prepare(pt_agent *a, task *t, uint32_t attempt)
{
    pt_arena *sa = &a->scratch;
    const pt_config *c = &a->o.config;
    pt_ai_target target;
    pt_ai_context ctx;
    pt_credentials cred;
    entry *last = pt_core_last_entry(a);
    task nt = *t;
    pt_buf ref;
    bool ok;
    if (!resolve(a, c->model, &target, sa) || !credentials(a, target.provider, &cred))
        return false;
    nt.cutoff = last ? last->id : 0;
    if (!pt_core_context(a, nt.cutoff, &ctx, sa)) {
        pt_core_fail(a, PT_FATAL_OOM, "out of memory", PT_STR_NULL);
        return false;
    }
    pt_buf_init(&ref, a->al);
    pt_buf_putz(&ref, target.provider->id);
    pt_buf_putc(&ref, '/');
    pt_buf_putz(&ref, target.model.id);
    nt.status = ST_RUNNING;
    nt.phase = PH_REQUEST;
    nt.attempt = attempt;
    nt.model = pt_buf_str(&ref);
    nt.reasoning = c->reasoning;
    nt.max_tokens = c->max_tokens ? c->max_tokens : pt_ai_auto_max_tokens(&target, &ctx, 0);
    pt_core_commit_begin(a, t->id);
    pt_write_task(&a->cw, &nt);
    ok = pt_core_commit_end(a);
    pt_buf_free(&ref);
    return ok;
}

/* ---- request ---- */

/* The partial message, for watchers and for the record after a crash: at
 * most every live_interval_ms, and once more after the last change. */
static bool snapshot(run_live *rl, const pt_msg *m, bool force)
{
    pt_agent *a = rl->agent;
    int64_t now = pt_core_now(a);
    uint32_t every = a->o.config.live_interval_ms ? a->o.config.live_interval_ms : 250;
    pt_jw w;
    pt_fx fx;
    if (!force && rl->last_live && now - rl->last_live < (int64_t)every) {
        rl->dirty = true;
        return false;
    }
    rl->last_live = now ? now : 1;
    rl->dirty = false;
    pt_buf_reset(&rl->snap);
    pt_jw_init(&w, &rl->snap);
    pt_msg_write_json(&w, m);
    memset(&fx, 0, sizeof fx);
    fx.kind = PT_FX_LIVE;
    fx.id = pt_core_new_fx(a);
    fx.task = rl->task;
    return !rl->snap.oom && pt_core_queue(a, &fx, pt_buf_str(&rl->snap));
}

static void on_ai(void *ud, const pt_ai_event *ev)
{
    run_live *rl = (run_live *)ud;
    pt_event e;
    memset(&e, 0, sizeof e);
    e.kind = PT_E_STREAM;
    e.task = rl->task;
    e.ai = ev;
    pt_core_emit(rl->agent, &e);
    if (ev->kind == PT_EV_DONE || ev->kind == PT_EV_ERROR)
        rl->finished = true;
    else
        snapshot(rl, ev->msg, false);
}

static bool start_request(pt_agent *a, task *t)
{
    run_live *rl = (run_live *)a->al.fn(a->al.ud, NULL, 0, sizeof *rl);
    pt_credentials cred;
    pt_ai_context ctx;
    pt_ai_opts opts;
    pt_fx fx;
    if (!rl) {
        pt_core_fail(a, PT_FATAL_OOM, "out of memory", PT_STR_NULL);
        return false;
    }
    memset(rl, 0, sizeof *rl);
    rl->agent = a;
    rl->task = t->id;
    pt_arena_init(&rl->a, a->al, 65536);
    pt_buf_init(&rl->snap, a->al);
    t->live = rl;
    if (!resolve(a, t->model, &rl->target, &rl->a))
        goto fail;
    memset(&cred, 0, sizeof cred);
    if (!credentials(a, rl->target.provider, &cred))
        goto fail;
    memset(&opts, 0, sizeof opts);
    opts.max_tokens = t->max_tokens;
    opts.reasoning = t->reasoning;
    if (!pt_core_context(a, t->cutoff, &ctx, &rl->a) ||
        !pt_ai_build(&rl->target, &ctx, &opts, &cred, &rl->req, &rl->a)) {
        pt_core_fail(a, PT_FATAL_OOM, "could not build the request", PT_STR_NULL);
        goto fail;
    }
    rl->s = pt_ai_stream_new(&rl->target, &rl->a, a->al, on_ai, rl, pt_core_now(a));
    if (!rl->s) {
        pt_core_fail(a, PT_FATAL_OOM, "out of memory", PT_STR_NULL);
        goto fail;
    }
    memset(&fx, 0, sizeof fx);
    fx.kind = PT_FX_HTTP;
    fx.id = pt_core_new_fx(a);
    fx.task = t->id;
    fx.http = &rl->req;
    rl->http_fx = fx.id;
    return pt_core_queue(a, &fx, PT_STR_NULL);
fail:
    pt_run_free_live(a, t);
    return false;
}

static run_live *find_http(pt_agent *a, uint32_t fx)
{
    uint32_t i;
    for (i = 0; i < a->ntasks; i++) {
        run_live *rl = a->tasks[i]->live;
        if (rl && rl->http_fx == fx && !rl->http_done)
            return rl;
    }
    return NULL;
}

bool pt_run_http_head(pt_agent *a, uint32_t fx, int status, pt_str retry_after)
{
    run_live *rl = find_http(a, fx);
    if (!rl)
        return false;
    pt_ai_stream_head(rl->s, status, retry_after);
    return true;
}

bool pt_run_http_data(pt_agent *a, uint32_t fx, const void *p, size_t n)
{
    run_live *rl = find_http(a, fx);
    if (!rl)
        return false;
    pt_ai_stream_body(rl->s, (const char *)p, n, pt_core_now(a));
    return true;
}

bool pt_run_http_done(pt_agent *a, uint32_t fx, const char *error)
{
    run_live *rl = find_http(a, fx);
    if (!rl)
        return false;
    rl->http_done = true;
    pt_ai_stream_end(rl->s, error, pt_core_now(a));
    return true;
}

/* ---- the outcome ---- */

static int64_t backoff(pt_agent *a, uint32_t attempt, int64_t retry_after_ms)
{
    const pt_config *c = &a->o.config;
    int64_t base = c->retry_base_ms ? c->retry_base_ms : 1000;
    int64_t max = c->retry_max_ms ? c->retry_max_ms : 300000;
    int64_t d = base;
    uint32_t r = 0, i;
    if (retry_after_ms >= 0)
        return retry_after_ms; /* the server knows best */
    for (i = 1; i < attempt && d < max; i++)
        d *= 2;
    if (d > max)
        d = max;
    if (a->o.random)
        a->o.random(a->o.ud, &r, sizeof r);
    return d * 3 / 4 + (int64_t)(r % (uint32_t)(d / 2 + 1)); /* ±25% jitter */
}

static void write_assistant(pt_agent *a, uint32_t id, const pt_msg *m)
{
    pt_core_entry_begin(a, id, "assistant");
    pt_jw_key(&a->cw, "message");
    pt_msg_write_json(&a->cw, m);
    pt_core_entry_end(a);
}

static void write_error(pt_agent *a, uint32_t attempt, const pt_ai_error *e, int64_t retry_at)
{
    pt_jw *w = &a->cw;
    pt_core_entry_begin(a, pt_core_new_id(a), "error");
    pt_jw_key(w, "attempt");
    pt_jw_uint(w, attempt);
    pt_jw_key(w, "http");
    pt_jw_int(w, e->status);
    pt_jw_key(w, "class");
    pt_jw_strz(w, pt_err_class_name(e->cls));
    pt_jw_key(w, "message");
    pt_jw_str(w, e->message);
    if (retry_at) {
        pt_jw_key(w, "retry_at");
        pt_jw_int(w, retry_at);
    }
    pt_core_entry_end(a);
}

static void write_tool_error(pt_agent *a, const pt_block *call, const char *text)
{
    pt_jw *w = &a->cw;
    pt_core_entry_begin(a, pt_core_new_id(a), "tool_result");
    pt_jw_key(w, "call_id");
    pt_jw_str(w, call->id);
    pt_jw_key(w, "name");
    pt_jw_str(w, call->name);
    pt_jw_key(w, "content");
    pt_jw_strz(w, text);
    pt_jw_key(w, "is_error");
    pt_jw_bool(w, true);
    pt_core_entry_end(a);
}

static void after(pt_agent *a, after_kind k, const char *outcome, pt_str error, int64_t until)
{
    a->after = k;
    a->after_outcome = pt_arena_dup(&a->after_a, pt_strz(outcome));
    a->after_error = pt_arena_dup(&a->after_a, error);
    a->after_until = until;
}

static bool classify(pt_agent *a, task *t)
{
    run_live *rl = t->live;
    const pt_msg *m = pt_ai_stream_message(rl->s);
    const pt_ai_error *e = pt_ai_stream_error(rl->s);
    input *in = pt_core_input(a, t->input), nin;
    task nt = *t;
    uint32_t i, calls = 0;
    bool input_changed = false, ok;
    for (i = 0; i < m->nblocks; i++)
        calls += m->blocks[i].kind == PT_BLOCK_TOOL_CALL;
    pt_core_commit_begin(a, t->id);
    a->after_live_del = t->id;
    if (in)
        nin = *in;
    else
        memset(&nin, 0, sizeof nin);

    if (m->stop == PT_STOP_ERROR || m->stop == PT_STOP_ABORTED) {
        if (m->nblocks) /* keep what arrived, for the record; never sent again */
            write_assistant(a, pt_core_new_id(a), m);
        if (e->cls == PT_ERR_RETRYABLE) {
            int64_t until = pt_core_now(a) + backoff(a, t->attempt, e->retry_after_ms);
            write_error(a, t->attempt, e, until);
            nt.phase = PH_RETRY;
            nt.until = until;
            after(a, AFTER_RETRY, "", e->message, until);
        } else {
            /* Fatal, or an overflow (compaction comes in M3). */
            write_error(a, t->attempt, e, 0);
            nt.status = ST_DONE;
            nt.outcome = PT_S("failed");
            nt.reason = e->message;
            nt.result = 0;
            nin.status = IN_FAILED;
            input_changed = true;
            after(a, AFTER_RUN_DONE, "failed", e->message, 0);
        }
    } else if (calls && (m->stop == PT_STOP_TOOL_USE || m->stop == PT_STOP_LENGTH)) {
        write_assistant(a, pt_core_new_id(a), m);
        for (i = 0; i < m->nblocks; i++)
            if (m->blocks[i].kind == PT_BLOCK_TOOL_CALL)
                write_tool_error(a, &m->blocks[i],
                                 m->stop == PT_STOP_LENGTH
                                     ? "The tool call was cut off by max_tokens. Retry in smaller pieces."
                                     : "This tool is not available: pytte has no tools yet.");
        nt.phase = PH_PREPARE;
        nt.attempt = 1;
    } else {
        uint32_t id = pt_core_new_id(a);
        write_assistant(a, id, m);
        nt.status = ST_DONE;
        nt.result = id;
        nt.outcome = m->stop == PT_STOP_REFUSAL ? PT_S("failed") : PT_S("completed");
        nt.reason = m->stop == PT_STOP_REFUSAL  ? PT_S("refusal")
                    : m->stop == PT_STOP_LENGTH ? PT_S("truncated")
                                                : PT_STR_NULL;
        nin.status = m->stop == PT_STOP_REFUSAL ? IN_FAILED : IN_DONE;
        input_changed = true;
        after(a, AFTER_RUN_DONE, m->stop == PT_STOP_REFUSAL ? "failed" : "completed",
              m->stop == PT_STOP_REFUSAL ? PT_S("the model refused") : PT_STR_NULL, 0);
    }
    pt_write_task(&a->cw, &nt);
    if (in && input_changed)
        pt_write_input(&a->cw, &nin);
    ok = pt_core_commit_end(a);
    if (!rl->http_done) {
        /* The stream ended on its final event before the transfer did. */
        pt_fx fx;
        memset(&fx, 0, sizeof fx);
        fx.kind = PT_FX_CANCEL;
        fx.id = pt_core_new_fx(a);
        fx.task = t->id;
        fx.cancel = rl->http_fx;
        pt_core_queue(a, &fx, PT_STR_NULL);
    }
    pt_run_free_live(a, t);
    return ok;
}

/* A partial answer from before a crash goes into the transcript as an
 * aborted assistant entry (never sent to the model), then the request is
 * sent again. */
static bool commit_rescued(pt_agent *a, task *t)
{
    pt_arena *sa = &a->scratch;
    pt_json *v = pt_json_parse(pt_buf_str(&t->rescued), sa, NULL);
    pt_msg m;
    pt_buf_reset(&t->rescued);
    if (!v || !pt_msg_read_json(v, &m, sa) || m.role != PT_ROLE_ASSISTANT || !m.nblocks) {
        pt_fx fx;
        memset(&fx, 0, sizeof fx);
        fx.kind = PT_FX_LIVE;
        fx.id = pt_core_new_fx(a);
        fx.task = t->id;
        return pt_core_queue(a, &fx, PT_STR_NULL);
    }
    m.stop = PT_STOP_ABORTED;
    m.error = PT_S("interrupted: the process stopped during this response");
    pt_core_commit_begin(a, t->id);
    a->after_live_del = t->id;
    write_assistant(a, pt_core_new_id(a), &m);
    return pt_core_commit_end(a);
}

bool pt_run_step(pt_agent *a, task *t)
{
    switch (t->phase) {
    case PH_PREPARE:
        return prepare(a, t, t->attempt ? t->attempt : 1);
    case PH_RETRY:
        if (t->woke) {
            t->woke = false;
            return prepare(a, t, t->attempt + 1);
        } else {
            pt_fx fx;
            memset(&fx, 0, sizeof fx);
            fx.kind = PT_FX_SLEEP;
            fx.id = pt_core_new_fx(a);
            fx.task = t->id;
            fx.until = t->until;
            t->fx = fx.id;
            return pt_core_queue(a, &fx, PT_STR_NULL);
        }
    case PH_REQUEST:
        if (t->rescued.n)
            return commit_rescued(a, t);
        if (!t->live)
            return start_request(a, t);
        if (t->live->finished)
            return classify(a, t);
        if (t->live->dirty) /* the trailing snapshot, once its time has come */
            return snapshot(t->live, pt_ai_stream_message(t->live->s), false);
        return false;
    }
    return false;
}
