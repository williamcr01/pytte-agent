/* The run task: answers one input, one request at a time (section 8.1).
 *
 *   prepare ──► request ──► done
 *      ▲          │  │
 *      │          │  └──► retry (sleep) ──► prepare, attempt + 1
 *      │          ▼
 *      ├──── tools (waiting on one tool task per call, task_tool.c)
 *      └──── compact_wait (waiting on a compact task, task_compact.c): when the
 *            context is over budget, or the server says it is too long
 *
 * prepare is pure; it pins the request's parameters in one commit.
 * request sends exactly what the pinned checkpoint describes, so a request
 * re-sent after a crash has the same bytes as the original. */
#include "core/core.h"

#include <string.h>

static void free_run_live(pt_agent *a, run_live *rl)
{
    pt_ai_stream_free(rl->s);
    pt_buf_free(&rl->snap);
    pt_arena_free(&rl->a);
    a->al.fn(a->al.ud, rl, sizeof *rl, 0);
}

void pt_run_free_live(pt_agent *a, task *t)
{
    run_live *rl = t->live;
    if (!rl)
        return;
    t->live = NULL;
    if (rl->http_fx && !rl->http_done) {
        /* The queued CANCEL may not be handed to the host until a later poll. */
        rl->pending_next = a->pending_http_free;
        a->pending_http_free = rl;
        return;
    }
    free_run_live(a, rl);
}

void pt_run_free_pending(pt_agent *a)
{
    while (a->pending_http_free) {
        run_live *rl = a->pending_http_free;
        a->pending_http_free = rl->pending_next;
        free_run_live(a, rl);
    }
}

void pt_run_cancel_delivered(pt_agent *a, uint32_t cancelled_fx)
{
    run_live *rl;
    for (rl = a->pending_http_free; rl; rl = rl->pending_next)
        if (rl->http_fx == cancelled_fx) {
            rl->cancel_delivered = true;
            return;
        }
}

static void free_cancelled_runs(pt_agent *a)
{
    run_live **p = &a->pending_http_free;
    while (*p) {
        run_live *rl = *p;
        if (rl->cancel_delivered) {
            *p = rl->pending_next;
            free_run_live(a, rl);
        } else {
            p = &rl->pending_next;
        }
    }
}

void pt_run_poll_begin(pt_agent *a)
{
    free_cancelled_runs(a);
}

bool pt_run_resolve(pt_agent *a, pt_str ref, pt_ai_target *out, pt_arena *ar)
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
    if (a->o.config.context_window)
        out->model.context_window = a->o.config.context_window;
    return true;
}

/* The provider's API key; a missing one stops the agent without a commit,
 * so the run can continue once it is set (or with another model). */
bool pt_run_credentials(pt_agent *a, const pt_provider *p, pt_credentials *cred)
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
        pt_buf_putz(&m, " or run /login");
    }
    pt_core_fail(a, PT_FATAL_CONFIG, "", pt_buf_str(&m));
    pt_buf_free(&m);
    return false;
}

/* ---- prepare ---- */

/* Room kept free for the next answer and one round of tool results. */
#define RESERVE 24576

/* Over budget: wait for a summary of the epoch instead of sending it. */
static bool over_budget(pt_agent *a, const pt_ai_target *target, uint64_t used)
{
    const pt_config *c = &a->o.config;
    uint64_t window = target->model.context_window, reserve;
    if (!window)
        return false;
    reserve = c->compact_reserve ? c->compact_reserve : window / 4 < RESERVE ? window / 4 : RESERVE;
    return used + reserve > window && pt_compact_possible(a);
}

static bool start_compaction(pt_agent *a, task *t, uint32_t cutoff)
{
    task nt = *t;
    pt_core_commit_begin(a, t->id);
    nt.compact = pt_compact_write(a, t->id, cutoff);
    nt.status = ST_WAITING;
    nt.phase = PH_COMPACT;
    pt_write_task(&a->commit.w, &nt);
    return pt_core_commit_end(a);
}

static bool prepare(pt_agent *a, task *t, uint32_t attempt)
{
    pt_arena *sa = &a->scratch;
    const pt_config *c = &a->o.config;
    pt_ai_target target;
    pt_ai_context ctx;
    pt_credentials cred;
    entry *last = pt_core_last_entry(a);
    task nt = *t;
    uint64_t used;
    pt_buf ref;
    bool ok;
    if (a->nentries && a->entries[0].kind == ENT_EPOCH && a->entries[0].provisional)
        return pt_compact_settle(a, t);
    if (!pt_run_resolve(a, c->model, &target, sa) || !pt_run_credentials(a, target.provider, &cred))
        return false;
    nt.cutoff = last ? last->id : 0;
    used = pt_core_estimate(a, nt.cutoff);
    if (over_budget(a, &target, used))
        return start_compaction(a, t, nt.cutoff);
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
    nt.max_tokens = c->max_tokens ? c->max_tokens : pt_ai_auto_max_tokens(&target, &ctx, used < UINT32_MAX ? (uint32_t)(used ? used : 1) : UINT32_MAX);
    pt_core_commit_begin(a, t->id);
    pt_write_task(&a->commit.w, &nt);
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
    if (rl->quiet) {
        rl->finished = rl->finished || ev->kind == PT_EV_DONE || ev->kind == PT_EV_ERROR;
        return;
    }
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

bool pt_run_start(pt_agent *a, task *t)
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
    rl->quiet = t->kind == TASK_COMPACT;
    pt_arena_init(&rl->a, a->al, 4096);
    pt_buf_init(&rl->snap, a->al);
    t->live = rl;
    if (!pt_run_resolve(a, t->model, &rl->target, &rl->a))
        goto fail;
    memset(&cred, 0, sizeof cred);
    if (!pt_run_credentials(a, rl->target.provider, &cred))
        goto fail;
    memset(&opts, 0, sizeof opts);
    opts.max_tokens = t->max_tokens;
    opts.reasoning = t->reasoning;
    opts.hide_thinking = a->o.config.hide_thinking;
    if (!(t->kind == TASK_COMPACT ? pt_compact_context(a, t, &ctx, &rl->a)
                                  : pt_core_context(a, t->cutoff, &ctx, &rl->a)) ||
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

int64_t pt_run_backoff(pt_agent *a, uint32_t attempt, int64_t retry_after_ms)
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

/* The usage doc: token totals per model, {"provider/model":{"input":..,
 * "output":..,"cache_read":..,"cache_write":..,"requests":..}}. Replaced
 * whole, like every doc; small, since it has a member per model used. */
void pt_run_add_usage(pt_agent *a, const pt_msg *m, pt_str model)
{
    static const char *const keys[] = {"input", "output", "cache_read", "cache_write", "requests"};
    const pt_usage *u = &m->usage;
    const pt_json *old, *c;
    pt_jw *w = &a->commit.w;
    uint64_t add[5];
    bool found = false;
    unsigned k;
    if (!model.n || (!u->input && !u->output))
        return;
    add[0] = u->input;
    add[1] = u->output;
    add[2] = u->cache_read;
    add[3] = u->cache_write;
    add[4] = 1;
    old = pt_json_parse(pt_doc(a, PT_S("usage")), &a->scratch, NULL);
    pt_core_doc_begin(a, PT_S("usage"));
    pt_jw_obj(w);
    for (c = pt_json_is(old, PT_J_OBJ) ? old->child : NULL; c; c = c->next) {
        bool mine = pt_str_eq(c->key, model);
        found = found || mine;
        pt_jw_keys(w, c->key);
        pt_jw_obj(w);
        for (k = 0; k < 5; k++) {
            pt_jw_key(w, keys[k]);
            pt_jw_uint(w, (uint64_t)pt_json_geti(c, keys[k], 0) + (mine ? add[k] : 0));
        }
        pt_jw_obj_end(w);
    }
    if (!found) {
        pt_jw_keys(w, model);
        pt_jw_obj(w);
        for (k = 0; k < 5; k++) {
            pt_jw_key(w, keys[k]);
            pt_jw_uint(w, add[k]);
        }
        pt_jw_obj_end(w);
    }
    pt_jw_obj_end(w);
    pt_jw_obj_end(w);
}

void pt_run_write_assistant(pt_agent *a, uint32_t id, const pt_msg *m)
{
    pt_core_entry_begin(a, id, "assistant");
    pt_jw_key(&a->commit.w, "message");
    pt_msg_write_json(&a->commit.w, m);
    pt_core_entry_end(a);
}

void pt_run_write_error(pt_agent *a, uint32_t attempt, const pt_ai_error *e, int64_t retry_at)
{
    pt_jw *w = &a->commit.w;
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

void pt_run_write_tool_error(pt_agent *a, uint32_t id, pt_str call_id, pt_str name, pt_str text)
{
    pt_jw *w = &a->commit.w;
    pt_core_entry_begin(a, id, "tool_result");
    pt_jw_key(w, "call_id");
    pt_jw_str(w, call_id);
    pt_jw_key(w, "name");
    pt_jw_str(w, name);
    pt_jw_key(w, "content");
    pt_jw_str(w, text);
    pt_jw_key(w, "is_error");
    pt_jw_bool(w, true);
    pt_core_entry_end(a);
}

static bool tool_parallel(pt_agent *a, pt_str name)
{
    uint32_t i;
    for (i = 0; i < a->ntools; i++)
        if (pt_str_eq(a->tools[i].name, name))
            return a->tool_meta[i].parallel;
    return false;
}

/* The assistant entry and a tool task per call, in one commit: a call
 * never exists without its task, or the reverse. */
static bool write_round(pt_agent *a, task *nt, const pt_msg *m, uint32_t calls)
{
    uint32_t aid = pt_core_new_id(a), i, k = 0;
    nt->calls = (uint32_t *)pt_arena_alloc(&a->scratch, sizeof(uint32_t) * calls);
    nt->parallel = (bool *)pt_arena_alloc(&a->scratch, sizeof(bool) * calls);
    if (!nt->calls || !nt->parallel || a->scratch.oom) {
        pt_core_fail(a, PT_FATAL_OOM, "out of memory building tool round", PT_STR_NULL);
        return false;
    }
    pt_run_write_assistant(a, aid, m);
    for (i = 0; i < m->nblocks; i++) {
        const pt_block *b = &m->blocks[i];
        task tt;
        if (b->kind != PT_BLOCK_TOOL_CALL)
            continue;
        memset(&tt, 0, sizeof tt);
        tt.id = pt_core_new_id(a);
        tt.kind = TASK_TOOL;
        tt.owner = nt->id;
        tt.status = ST_PENDING;
        tt.phase = PH_CALL;
        tt.entry = aid;
        tt.call_id = b->id;
        pt_write_task(&a->commit.w, &tt);
        nt->parallel[k] = !a->o.config.serial_tools && tool_parallel(a, b->name);
        nt->calls[k++] = tt.id;
    }
    nt->ncalls = k;
    nt->entry = aid;
    nt->status = ST_WAITING;
    nt->phase = PH_TOOLS;
    return true;
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
    a->commit.live_del = t->id;
    if (in)
        nin = *in;
    else
        memset(&nin, 0, sizeof nin);

    if (m->stop == PT_STOP_ERROR || m->stop == PT_STOP_ABORTED) {
        if (m->nblocks) /* keep what arrived, for the record; never sent again */
            pt_run_write_assistant(a, pt_core_new_id(a), m);
        if (e->cls == PT_ERR_RETRYABLE) {
            int64_t until = pt_core_now(a) + pt_run_backoff(a, t->attempt, e->retry_after_ms);
            pt_run_write_error(a, t->attempt, e, until);
            nt.phase = PH_RETRY;
            nt.until = until;
            pt_core_after(a, AFTER_RETRY, "", e->message, until);
        } else if (e->cls == PT_ERR_OVERFLOW && pt_compact_possible(a)) {
            /* Too long after all: summarize, then send again. Once the
             * epoch is new there is nothing to summarize, so a second
             * overflow fails the run. */
            entry *last = pt_core_last_entry(a);
            pt_run_write_error(a, t->attempt, e, 0);
            nt.compact = pt_compact_write(a, t->id, last ? last->id : 0);
            nt.status = ST_WAITING;
            nt.phase = PH_COMPACT;
        } else {
            pt_run_write_error(a, t->attempt, e, 0);
            nt.status = ST_DONE;
            nt.outcome = PT_S("failed");
            nt.reason = e->message;
            nt.result = 0;
            nin.status = IN_FAILED;
            input_changed = true;
            pt_core_after(a, AFTER_RUN_DONE, "failed", e->message, 0);
        }
    } else if (calls && m->stop == PT_STOP_TOOL_USE) {
        if (!write_round(a, &nt, m, calls))
            return false;
    } else if (calls && m->stop == PT_STOP_LENGTH) {
        /* A call may be cut off mid-input: run none of them. */
        pt_run_write_assistant(a, pt_core_new_id(a), m);
        for (i = 0; i < m->nblocks; i++)
            if (m->blocks[i].kind == PT_BLOCK_TOOL_CALL)
                pt_run_write_tool_error(a, pt_core_new_id(a), m->blocks[i].id, m->blocks[i].name,
                                        PT_S("The tool call was cut off by max_tokens. Retry in smaller pieces."));
        nt.phase = PH_PREPARE;
        nt.attempt = 1;
    } else {
        uint32_t id = pt_core_new_id(a);
        pt_run_write_assistant(a, id, m);
        nt.status = ST_DONE;
        nt.result = id;
        nt.outcome = m->stop == PT_STOP_REFUSAL ? PT_S("failed") : PT_S("completed");
        nt.reason = m->stop == PT_STOP_REFUSAL  ? PT_S("refusal")
                    : m->stop == PT_STOP_LENGTH ? PT_S("truncated")
                                                : PT_STR_NULL;
        nin.status = m->stop == PT_STOP_REFUSAL ? IN_FAILED : IN_DONE;
        input_changed = true;
        pt_core_after(a, AFTER_RUN_DONE, m->stop == PT_STOP_REFUSAL ? "failed" : "completed",
              m->stop == PT_STOP_REFUSAL ? PT_S("the model refused") : PT_STR_NULL, 0);
    }
    if (a->fatal)
        return false; /* discard this incomplete write-ahead batch */
    pt_write_task(&a->commit.w, &nt);
    if (in && input_changed)
        pt_write_input(&a->commit.w, &nin);
    pt_run_add_usage(a, m, t->model);
    ok = pt_core_commit_end(a);
    if (!rl->http_done) /* the stream ended on its final event before the transfer did */
        pt_core_cancel(a, t->id, rl->http_fx);
    pt_run_free_live(a, t);
    return ok;
}

/* The partial message in a live file from before a crash, if it has any. */
static bool read_rescued(pt_agent *a, task *t, pt_msg *m)
{
    pt_arena *sa = &a->scratch;
    pt_json *v = pt_json_parse(pt_buf_str(&t->rescued), sa, NULL);
    pt_buf_reset(&t->rescued);
    if (!v || !pt_msg_read_json(v, m, sa) || m->role != PT_ROLE_ASSISTANT || !m->nblocks)
        return false;
    m->stop = PT_STOP_ABORTED;
    m->error = PT_S("interrupted: the process stopped during this response");
    return true;
}

/* A partial answer from before a crash goes into the transcript as an
 * aborted assistant entry (never sent to the model), then the request is
 * sent again. */
static bool commit_rescued(pt_agent *a, task *t)
{
    pt_msg m;
    if (!read_rescued(a, t, &m)) {
        pt_core_live_del(a, t->id);
        return true;
    }
    pt_core_commit_begin(a, t->id);
    a->commit.live_del = t->id;
    pt_run_write_assistant(a, pt_core_new_id(a), &m);
    return pt_core_commit_end(a);
}

static bool has_tools(pt_agent *a, const task *t)
{
    uint32_t i;
    for (i = 0; i < a->ntasks; i++)
        if (a->tasks[i]->kind == TASK_TOOL && a->tasks[i]->owner == t->id)
            return true;
    return false;
}

/* The run ends aborted: one commit with the partial answer (if any), the
 * run done(aborted) and its input aborted. Then the request or the retry
 * sleep is cancelled. */
static bool end_aborted(pt_agent *a, task *t)
{
    input *in = pt_core_input(a, t->input), nin;
    task nt = *t;
    uint32_t sleeping = t->phase == PH_RETRY ? t->fx : 0, compacting = 0;
    task *ct = t->phase == PH_COMPACT ? pt_core_task(a, t->compact) : NULL;
    pt_msg m;
    bool ok;
    pt_core_commit_begin(a, t->id);
    if (ct)
        compacting = pt_compact_abort(a, ct);
    if (t->live) {
        m = *pt_ai_stream_message(t->live->s);
        if (m.nblocks) {
            m.stop = PT_STOP_ABORTED;
            m.error = PT_S("interrupted by the user");
            pt_run_write_assistant(a, pt_core_new_id(a), &m);
        }
    } else if (t->rescued.n && read_rescued(a, t, &m)) {
        pt_run_write_assistant(a, pt_core_new_id(a), &m);
    }
    if (t->phase == PH_REQUEST)
        a->commit.live_del = t->id;
    nt.status = ST_DONE;
    nt.outcome = PT_S("aborted");
    nt.reason = PT_S("interrupted by the user");
    nt.result = 0;
    pt_write_task(&a->commit.w, &nt);
    if (in) {
        nin = *in;
        nin.status = IN_ABORTED;
        pt_write_input(&a->commit.w, &nin);
    }
    pt_core_after(a, AFTER_RUN_DONE, "aborted", PT_S("interrupted by the user"), 0);
    ok = pt_core_commit_end(a);
    if (t->live) {
        if (!t->live->http_done)
            pt_core_cancel(a, t->id, t->live->http_fx);
        pt_run_free_live(a, t);
    }
    if (sleeping)
        pt_core_cancel(a, t->id, sleeping);
    if (compacting)
        pt_core_cancel(a, ct->id, compacting);
    t->woke = false;
    return ok;
}

/* With tool tasks running, the abort is two steps (section 8.4): first
 * the decision is committed (calls not started end now, running ones are
 * marked and cancelled), then each call commits its end, and the run ends
 * once they all have. */
bool pt_run_abort(pt_agent *a, task *t)
{
    task nt = *t;
    uint32_t i;
    bool ok;
    if (t->kind == TASK_COMPACT) { /* one the user asked for: it just ends */
        uint32_t fx;
        pt_core_commit_begin(a, t->id);
        fx = pt_compact_abort(a, t);
        pt_core_after(a, AFTER_COMPACT, "failed", PT_S("interrupted by the user"), 0);
        ok = pt_core_commit_end(a);
        if (fx)
            pt_core_cancel(a, t->id, fx);
        return ok;
    }
    if (t->live && t->live->finished)
        return false;
    if (t->phase != PH_TOOLS || !has_tools(a, t))
        return end_aborted(a, t);
    if (t->abort)
        return true; /* already decided */
    pt_core_commit_begin(a, t->id);
    nt.abort = true;
    pt_write_task(&a->commit.w, &nt);
    for (i = 0; i < a->ntasks; i++)
        if (a->tasks[i]->kind == TASK_TOOL && a->tasks[i]->owner == t->id)
            pt_tool_abort(a, a->tasks[i]);
    ok = pt_core_commit_end(a);
    for (i = 0; i < a->ntasks; i++) {
        task *tt = a->tasks[i];
        if (tt->kind == TASK_TOOL && tt->owner == t->id && tt->tl && !pt_tool_finished(tt))
            pt_core_cancel(a, tt->id, pt_tool_fx(tt));
    }
    return ok;
}

static bool tools_done(pt_agent *a, const task *t)
{
    uint32_t i;
    for (i = 0; i < t->ncalls; i++)
        if (pt_core_task(a, t->calls[i]))
            return false;
    return true;
}

bool pt_run_sleep(pt_agent *a, task *t)
{
    pt_fx fx;
    memset(&fx, 0, sizeof fx);
    fx.kind = PT_FX_SLEEP;
    fx.id = pt_core_new_fx(a);
    fx.task = t->id;
    fx.until = t->until;
    t->fx = fx.id;
    return pt_core_queue(a, &fx, PT_STR_NULL);
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
        }
        return pt_run_sleep(a, t);
    case PH_TOOLS:
        if (!tools_done(a, t))
            return false;
        if (t->abort)
            return end_aborted(a, t);
        return prepare(a, t, 1); /* pure: folded into the next request's commit */
    case PH_REQUEST:
        if (t->rescued.n)
            return commit_rescued(a, t);
        if (!t->live)
            return pt_run_start(a, t);
        if (t->live->finished)
            return classify(a, t);
        if (t->live->dirty) /* the trailing snapshot, once its time has come */
            return snapshot(t->live, pt_ai_stream_message(t->live->s), false);
        return false;
    default:
        return false;
    }
}
