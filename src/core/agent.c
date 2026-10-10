/* The agent: public API, scheduler, effect queue and commits. */
#include "core/core.h"

#include <string.h>

/* ---- small helpers ---- */

static void put_err(char *dst, size_t cap, const char *msg, pt_str detail)
{
    size_t n = 0, i;
    for (i = 0; msg[i] && n + 1 < cap; i++)
        dst[n++] = msg[i];
    if (detail.n && n + 3 < cap) {
        if (n) {
            dst[n++] = ':';
            dst[n++] = ' ';
        }
        for (i = 0; i < detail.n && n + 1 < cap; i++)
            dst[n++] = detail.p[i];
    }
    dst[n] = 0;
}

void pt_core_seterr(pt_agent *a, const char *msg, pt_str detail)
{
    put_err(a->err, sizeof a->err, msg, detail);
}

void pt_core_emit(pt_agent *a, const pt_event *ev)
{
    if (a->o.on_event)
        a->o.on_event(a->o.ud, ev);
}

void pt_core_fail(pt_agent *a, pt_fatal_kind k, const char *msg, pt_str detail)
{
    pt_event ev;
    if (a->fatal)
        return;
    a->fatal = k;
    pt_core_seterr(a, msg, detail);
    memset(&ev, 0, sizeof ev);
    ev.kind = PT_E_FATAL;
    ev.error = pt_strz(a->err);
    pt_core_emit(a, &ev);
}

int64_t pt_core_now(pt_agent *a)
{
    return a->o.now_ms ? a->o.now_ms(a->o.ud) : 0;
}

uint32_t pt_core_new_id(pt_agent *a)
{
    return a->next_id++;
}

uint32_t pt_core_new_fx(pt_agent *a)
{
    return ++a->next_fx;
}

bool pt_core_grow(pt_agent *a, void **arr, uint32_t *cap, uint32_t need, size_t elem)
{
    uint32_t ncap;
    void *p;
    if (need <= *cap)
        return true;
    ncap = *cap ? *cap * 2 : 16;
    while (ncap < need)
        ncap *= 2;
    p = a->al.fn(a->al.ud, *arr, elem * *cap, elem * ncap);
    if (!p)
        return false;
    *arr = p;
    *cap = ncap;
    return true;
}

task *pt_core_task(pt_agent *a, uint32_t id)
{
    uint32_t i;
    for (i = 0; i < a->ntasks; i++)
        if (a->tasks[i]->id == id)
            return a->tasks[i];
    return NULL;
}

input *pt_core_input(pt_agent *a, uint32_t id)
{
    uint32_t i;
    for (i = 0; i < a->ninputs; i++)
        if (a->inputs[i]->id == id)
            return a->inputs[i];
    return NULL;
}

entry *pt_core_last_entry(pt_agent *a)
{
    return a->nentries ? &a->entries[a->nentries - 1] : NULL;
}

static void hex(char *out, const unsigned char *p, size_t n)
{
    static const char digits[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; i++) {
        out[2 * i] = digits[p[i] >> 4];
        out[2 * i + 1] = digits[p[i] & 15];
    }
}

/* ---- effect queue ---- */

bool pt_core_queue(pt_agent *a, const pt_fx *fx, pt_str bytes)
{
    uint32_t i;
    queued_fx *q;
    if (fx->kind == PT_FX_LIVE) { /* a newer snapshot replaces a queued one */
        for (i = 0; i < a->qlen; i++) {
            q = &a->q[(a->qhead + i) % PT_FXQ];
            if (q->fx.kind == PT_FX_LIVE && q->fx.task == fx->task) {
                pt_buf_reset(&q->bytes);
                pt_buf_puts(&q->bytes, bytes);
                return true;
            }
        }
    }
    if (a->qlen == PT_FXQ) {
        if (fx->kind == PT_FX_LIVE)
            return true; /* best effort */
        pt_core_fail(a, PT_FATAL_OOM, "effect queue full", PT_STR_NULL);
        return false;
    }
    q = &a->q[(a->qhead + a->qlen++) % PT_FXQ];
    q->fx = *fx;
    pt_buf_reset(&q->bytes);
    if (fx->kind == PT_FX_LIVE)
        pt_buf_puts(&q->bytes, bytes);
    return true;
}

/* ---- commits ---- */

void pt_core_commit_begin(pt_agent *a, uint32_t task_id)
{
    pt_log_begin(&a->commit, &a->cw, a->seq + 1, pt_core_now(a));
    a->commit_task = task_id;
    a->after = AFTER_NONE;
    a->after_live_del = 0;
    a->commit_close = false;
    pt_arena_reset(&a->after_a);
}

bool pt_core_commit_end(pt_agent *a)
{
    pt_fx fx;
    task *t;
    pt_log_end(&a->commit, &a->cw);
    if (a->commit.oom) {
        pt_core_fail(a, PT_FATAL_OOM, "out of memory", PT_STR_NULL);
        return false;
    }
    memset(&fx, 0, sizeof fx);
    fx.kind = PT_FX_COMMIT;
    fx.id = pt_core_new_fx(a);
    fx.task = a->commit_task;
    fx.bytes = pt_buf_str(&a->commit);
    fx.sync = !a->o.config.no_sync;
    if (!pt_core_queue(a, &fx, PT_STR_NULL))
        return false;
    a->commit_fx = fx.id;
    a->commit_out = true;
    t = a->commit_task ? pt_core_task(a, a->commit_task) : NULL;
    if (t)
        t->fx = fx.id;
    return true;
}

static void commit_landed(pt_agent *a, const char *error)
{
    task *t = a->commit_task ? pt_core_task(a, a->commit_task) : NULL;
    pt_str line = pt_buf_str(&a->commit);
    const pt_json *root;
    uint64_t seq;
    pt_event ev;
    a->commit_out = false;
    if (t)
        t->fx = 0;
    if (error) {
        pt_core_fail(a, PT_FATAL_STORAGE, "could not commit to the session log", pt_strz(error));
        return;
    }
    line.n--; /* the newline */
    pt_arena_reset(&a->scratch);
    root = pt_log_parse(line, &seq, &a->scratch);
    if (!root || seq != a->seq + 1 || !pt_state_apply(a, root)) {
        if (!a->fatal)
            pt_core_fail(a, PT_FATAL_STORAGE, "internal error: a commit did not apply", line);
        return;
    }
    a->seq = seq;
    memset(&ev, 0, sizeof ev);
    ev.kind = PT_E_COMMITTED;
    ev.task = a->commit_task;
    ev.line = line;
    pt_core_emit(a, &ev);
    if (a->after_live_del) {
        pt_fx fx;
        memset(&fx, 0, sizeof fx);
        fx.kind = PT_FX_LIVE;
        fx.id = pt_core_new_fx(a);
        fx.task = a->after_live_del;
        pt_core_queue(a, &fx, PT_STR_NULL);
    }
    if (a->after != AFTER_NONE) {
        memset(&ev, 0, sizeof ev);
        ev.kind = a->after == AFTER_RETRY ? PT_E_RETRY : PT_E_RUN_DONE;
        ev.task = a->commit_task;
        ev.outcome = a->after_outcome;
        ev.error = a->after_error;
        ev.until = a->after_until;
        pt_core_emit(a, &ev);
    }
    if (a->commit_close)
        a->closed = true;
}

/* ---- session writes ---- */

static void write_entry_head(pt_agent *a, uint32_t id, const char *kind, uint32_t epoch)
{
    pt_jw *w = &a->cw;
    pt_jw_obj(w);
    pt_jw_key(w, "t");
    pt_jw_strz(w, "entry");
    pt_jw_key(w, "id");
    pt_jw_uint(w, id);
    pt_jw_key(w, "kind");
    pt_jw_strz(w, kind);
    pt_jw_key(w, "epoch");
    pt_jw_uint(w, epoch);
    pt_jw_key(w, "data");
    pt_jw_obj(w);
}

void pt_core_entry_begin(pt_agent *a, uint32_t id, const char *kind)
{
    write_entry_head(a, id, kind, a->cur_epoch);
}

void pt_core_entry_end(pt_agent *a)
{
    pt_jw_obj_end(&a->cw);
    pt_jw_obj_end(&a->cw);
}

static void build_create(pt_agent *a)
{
    pt_jw *w = &a->cw;
    const pt_session_cfg *c = &a->create;
    pt_core_commit_begin(a, 0);
    pt_jw_obj(w);
    pt_jw_key(w, "t");
    pt_jw_strz(w, "session");
    pt_jw_key(w, "id");
    pt_jw_uint(w, pt_core_new_id(a));
    pt_jw_key(w, "v");
    pt_jw_uint(w, 1);
    pt_jw_key(w, "created");
    pt_jw_int(w, pt_core_now(a));
    pt_jw_key(w, "cwd");
    pt_jw_str(w, c->cwd);
    pt_jw_key(w, "host");
    pt_jw_str(w, c->host);
    pt_jw_key(w, "pytte");
    pt_jw_strz(w, PT_VERSION);
    pt_jw_key(w, "forked_from");
    pt_jw_null(w);
    pt_jw_obj_end(w);
    write_entry_head(a, pt_core_new_id(a), "epoch", 1);
    pt_jw_key(w, "n");
    pt_jw_uint(w, 1);
    pt_jw_key(w, "system");
    pt_jw_str(w, c->system);
    pt_jw_key(w, "tools");
    pt_jw_raw(w, PT_S("[]"));
    pt_jw_key(w, "skills");
    pt_jw_raw(w, PT_S("[]"));
    pt_core_entry_end(a);
    a->want_create = false;
    pt_core_commit_end(a);
}

static void write_claim(pt_agent *a, const char *event)
{
    pt_jw *w = &a->cw;
    pt_jw_obj(w);
    pt_jw_key(w, "t");
    pt_jw_strz(w, "claim");
    pt_jw_key(w, "id");
    pt_jw_uint(w, a->mine);
    pt_jw_key(w, "event");
    pt_jw_strz(w, event);
    pt_jw_key(w, "token");
    pt_jw_str(w, a->my_token);
    pt_jw_key(w, "host");
    pt_jw_str(w, a->me.host);
    pt_jw_key(w, "boot");
    pt_jw_str(w, a->me.boot);
    pt_jw_key(w, "pid");
    pt_jw_int(w, a->me.pid);
    pt_jw_key(w, "opened");
    pt_jw_int(w, a->my_opened);
    if (*event == 'o' && a->takeover.n) {
        pt_jw_key(w, "takeover");
        pt_jw_str(w, a->takeover);
    }
    if (*event == 'c') {
        pt_jw_key(w, "closed");
        pt_jw_int(w, pt_core_now(a));
    }
    pt_jw_key(w, "pytte");
    pt_jw_strz(w, PT_VERSION);
    pt_jw_obj_end(w);
}

static void build_open(pt_agent *a)
{
    unsigned char r[8];
    char tok[16];
    memset(r, 0, sizeof r);
    if (a->o.random)
        a->o.random(a->o.ud, r, sizeof r);
    hex(tok, r, sizeof r);
    a->my_token = pt_arena_dup(&a->reqs, (pt_str){tok, 16});
    a->my_opened = pt_core_now(a);
    pt_core_commit_begin(a, 0);
    a->mine = pt_core_new_id(a);
    write_claim(a, "open");
    a->want_open = false;
    pt_core_commit_end(a);
}

static void build_close(pt_agent *a)
{
    pt_core_commit_begin(a, 0);
    write_claim(a, "close");
    a->want_close = false;
    a->commit_close = true;
    pt_core_commit_end(a);
}

/* ---- inputs ---- */

static bool active_run(pt_agent *a)
{
    uint32_t i;
    for (i = 0; i < a->ntasks; i++)
        if (a->tasks[i]->status != ST_DONE)
            return true;
    return false;
}

static input *first_queued(pt_agent *a)
{
    uint32_t i;
    for (i = 0; i < a->ninputs; i++)
        if (a->inputs[i]->status == IN_QUEUED)
            return a->inputs[i];
    return NULL;
}

/* An input becomes a user entry and a run task, in one commit. */
static void write_placement(pt_agent *a, input *in)
{
    task t;
    input placed = *in;
    pt_jw *w = &a->cw;
    uint32_t entry_id = pt_core_new_id(a), run_id = pt_core_new_id(a);
    placed.status = IN_PLACED;
    placed.run = run_id;
    pt_write_input(w, &placed);
    pt_core_entry_begin(a, entry_id, "user");
    pt_jw_key(w, "content");
    pt_jw_str(w, in->text);
    pt_jw_key(w, "input");
    pt_jw_uint(w, in->id);
    pt_core_entry_end(a);
    memset(&t, 0, sizeof t);
    t.id = run_id;
    t.input = in->id;
    t.status = ST_PENDING;
    t.phase = PH_PREPARE;
    t.attempt = 1;
    pt_write_task(w, &t);
}

static void build_submit(pt_agent *a)
{
    sub s = a->subs[0];
    input in;
    bool place = !active_run(a) && !first_queued(a);
    memset(&in, 0, sizeof in);
    pt_core_commit_begin(a, 0);
    in.id = pt_core_new_id(a);
    in.text = s.text;
    in.req = s.req;
    in.status = IN_QUEUED;
    if (place)
        write_placement(a, &in);
    else
        pt_write_input(&a->cw, &in);
    pt_core_commit_end(a);
    memmove(a->subs, a->subs + 1, sizeof(sub) * --a->nsubs);
    if (!a->nsubs)
        pt_arena_reset(&a->subs_a);
}

static void build_place(pt_agent *a, input *in)
{
    pt_core_commit_begin(a, 0);
    write_placement(a, in);
    pt_core_commit_end(a);
}

/* ---- scheduler ---- */

static bool step(pt_agent *a)
{
    uint32_t i;
    input *q;
    pt_arena_reset(&a->scratch);
    if (a->commit_out || a->fatal)
        return false;
    if (a->want_create) {
        build_create(a);
        return true;
    }
    if (!a->loaded)
        return false;
    if (a->want_open) {
        build_open(a);
        return true;
    }
    if (a->abort_run) {
        task *t = pt_core_task(a, a->abort_run);
        if (!t || t->status == ST_DONE) {
            a->abort_run = 0;
        } else if (pt_run_abort(a, t)) {
            a->abort_run = 0;
            return true;
        } else if (a->fatal) {
            return false;
        }
    }
    if (a->closing) {
        if (!a->want_close)
            return false;
        build_close(a);
        return true;
    }
    if (a->nsubs) {
        build_submit(a);
        return true;
    }
    if (!active_run(a) && (q = first_queued(a))) {
        build_place(a, q);
        return true;
    }
    for (i = 0; i < a->ntasks; i++) {
        task *t = a->tasks[i];
        if (t->status != ST_DONE && !t->fx && pt_run_step(a, t))
            return true;
        if (a->fatal || a->commit_out)
            return false;
    }
    return false;
}

int pt_poll(pt_agent *a, pt_fx *out)
{
    queued_fx *q;
    if (a->fatal)
        return 0;
    if (!a->qlen)
        step(a);
    if (a->fatal)
        return 0;
    if (!a->qlen) {
        if (pt_idle(a) && !a->idle_sent) {
            pt_event ev;
            a->idle_sent = true;
            memset(&ev, 0, sizeof ev);
            ev.kind = PT_E_IDLE;
            pt_core_emit(a, &ev);
        }
        return 0;
    }
    q = &a->q[a->qhead];
    a->qhead = (a->qhead + 1) % PT_FXQ;
    a->qlen--;
    *out = q->fx;
    if (out->kind == PT_FX_LIVE) {
        pt_buf_reset(&a->live_out);
        pt_buf_puts(&a->live_out, pt_buf_str(&q->bytes));
        out->bytes = pt_buf_str(&a->live_out);
    }
    if (out->kind != PT_FX_LIVE && out->kind != PT_FX_CANCEL)
        a->idle_sent = false;
    return 1;
}

bool pt_idle(const pt_agent *a)
{
    uint32_t i;
    if (a->fatal || a->commit_out || a->qlen || a->want_create || a->want_open || a->want_close ||
        a->nsubs || !a->loaded)
        return false;
    if (a->closing)
        return a->closed;
    for (i = 0; i < a->ntasks; i++)
        if (a->tasks[i]->status != ST_DONE)
            return false;
    for (i = 0; i < a->ninputs; i++)
        if (a->inputs[i]->status == IN_QUEUED)
            return false;
    return true;
}

/* ---- completions ---- */

void pt_fx_head(pt_agent *a, uint32_t fx, int status, pt_str retry_after)
{
    if (!a->fatal)
        pt_run_http_head(a, fx, status, retry_after);
}

void pt_fx_data(pt_agent *a, uint32_t fx, const void *p, size_t n)
{
    if (!a->fatal)
        pt_run_http_data(a, fx, p, n);
}

void pt_fx_done(pt_agent *a, uint32_t fx, const char *error)
{
    uint32_t i;
    if (a->fatal)
        return;
    if (a->commit_out && fx == a->commit_fx) {
        commit_landed(a, error);
        return;
    }
    if (pt_run_http_done(a, fx, error))
        return;
    for (i = 0; i < a->ntasks; i++) /* a retry sleep */
        if (a->tasks[i]->fx == fx) {
            a->tasks[i]->fx = 0;
            a->tasks[i]->woke = true;
        }
}

pt_fatal_kind pt_fatal(const pt_agent *a)
{
    return a->fatal;
}

const char *pt_error(const pt_agent *a)
{
    return a->err;
}

/* ---- lifecycle ---- */

pt_agent *pt_new(const pt_opts *o)
{
    pt_agent *a = (pt_agent *)o->alloc.fn(o->alloc.ud, NULL, 0, sizeof *a);
    uint32_t i;
    if (!a)
        return NULL;
    memset(a, 0, sizeof *a);
    a->o = *o;
    a->al = o->alloc;
    if (!a->o.catalog)
        a->o.catalog = pt_ai_builtin_catalog();
    pt_arena_init(&a->perm, a->al, 4096);
    pt_arena_init(&a->epoch, a->al, 65536);
    pt_arena_init(&a->scratch, a->al, 65536);
    pt_arena_init(&a->reqs, a->al, 1024);
    pt_arena_init(&a->subs_a, a->al, 1024);
    pt_arena_init(&a->after_a, a->al, 1024);
    pt_buf_init(&a->commit, a->al);
    pt_buf_init(&a->live_out, a->al);
    for (i = 0; i < PT_FXQ; i++)
        pt_buf_init(&a->q[i].bytes, a->al);
    a->next_id = 1;
    pt_set_config(a, &o->config);
    return a;
}

void pt_free(pt_agent *a)
{
    uint32_t i;
    if (!a)
        return;
    for (i = 0; i < a->ntasks; i++) {
        task *t = a->tasks[i];
        pt_run_free_live(a, t);
        pt_arena_free(&t->a);
        pt_buf_free(&t->rescued);
        a->al.fn(a->al.ud, t, sizeof *t, 0);
    }
    for (i = 0; i < a->ninputs; i++) {
        pt_arena_free(&a->inputs[i]->a);
        a->al.fn(a->al.ud, a->inputs[i], sizeof(input), 0);
    }
    a->al.fn(a->al.ud, a->tasks, sizeof(task *) * a->tcap, 0);
    a->al.fn(a->al.ud, a->inputs, sizeof(input *) * a->icap, 0);
    a->al.fn(a->al.ud, a->entries, sizeof(entry) * a->ecap, 0);
    a->al.fn(a->al.ud, a->subs, sizeof(sub) * a->scap, 0);
    for (i = 0; i < PT_FXQ; i++)
        pt_buf_free(&a->q[i].bytes);
    pt_buf_free(&a->commit);
    pt_buf_free(&a->live_out);
    pt_arena_free(&a->perm);
    pt_arena_free(&a->epoch);
    pt_arena_free(&a->scratch);
    pt_arena_free(&a->reqs);
    pt_arena_free(&a->subs_a);
    pt_arena_free(&a->after_a);
    a->al.fn(a->al.ud, a, sizeof *a, 0);
}

void pt_set_config(pt_agent *a, const pt_config *c)
{
    a->o.config = *c;
    a->o.config.model = pt_arena_dup(&a->perm, c->model);
}

bool pt_create(pt_agent *a, const pt_session_cfg *c)
{
    if (a->loaded || a->want_create) {
        pt_core_seterr(a, "the session already exists", PT_STR_NULL);
        return false;
    }
    a->create.cwd = pt_arena_dup(&a->reqs, c->cwd);
    a->create.host = pt_arena_dup(&a->reqs, c->host);
    a->create.system = pt_arena_dup(&a->reqs, c->system);
    a->want_create = true;
    a->loaded = true;
    a->have_session = true;
    a->cur_epoch = 1;
    return !a->reqs.oom;
}

pt_load_result pt_load_line(pt_agent *a, pt_str line, bool complete, bool last)
{
    const pt_json *root;
    uint64_t seq;
    if (!complete)
        return PT_LOAD_TORN;
    pt_arena_reset(&a->scratch);
    root = pt_log_parse(line, &seq, &a->scratch);
    if (!root) {
        if (last)
            return PT_LOAD_TORN;
        pt_core_seterr(a, "corrupt log: a damaged line before the end", PT_STR_NULL);
        return PT_LOAD_CORRUPT;
    }
    if (seq != a->seq + 1) {
        pt_core_seterr(a, "corrupt log: commits out of sequence", PT_STR_NULL);
        return PT_LOAD_CORRUPT;
    }
    if (!pt_state_apply(a, root)) {
        if (!a->err[0])
            pt_core_seterr(a, "corrupt log: a record this version does not understand", line);
        return PT_LOAD_CORRUPT;
    }
    a->seq = seq;
    return PT_LOAD_OK;
}

void pt_load_live(pt_agent *a, uint32_t task_id, pt_str bytes)
{
    task *t = pt_core_task(a, task_id);
    if (t && t->status != ST_DONE && t->phase == PH_REQUEST) {
        pt_buf_reset(&t->rescued);
        pt_buf_puts(&t->rescued, bytes);
    } else { /* stale: its commit landed, but the delete did not */
        pt_fx fx;
        memset(&fx, 0, sizeof fx);
        fx.kind = PT_FX_LIVE;
        fx.id = pt_core_new_fx(a);
        fx.task = task_id;
        pt_core_queue(a, &fx, PT_STR_NULL);
    }
}

bool pt_load_done(pt_agent *a)
{
    if (!a->have_session) {
        pt_core_seterr(a, "the log holds no session", PT_STR_NULL);
        return false;
    }
    a->loaded = true;
    return true;
}

bool pt_open(pt_agent *a, const pt_identity *me, bool force)
{
    const claim *c = &a->newest;
    if (!a->loaded) {
        pt_core_seterr(a, "no session loaded", PT_STR_NULL);
        return false;
    }
    a->takeover = PT_STR_NULL;
    if (c->id && pt_str_eqz(c->event, "open") && !pt_str_eq(c->host, me->host)) {
        if (!force) {
            pt_buf b;
            pt_buf_init(&b, a->al);
            pt_buf_putz(&b, "last opened on ");
            pt_buf_puts(&b, c->host);
            pt_buf_putz(&b, " (pid ");
            pt_buf_i64(&b, c->pid);
            pt_buf_putz(&b, ") and not closed; it may still be running there. Use --force to take over");
            pt_core_seterr(a, b.oom ? "the session is claimed by another host" : "", pt_buf_str(&b));
            pt_buf_free(&b);
            return false;
        }
        a->takeover = pt_arena_dup(&a->reqs, c->token);
    }
    a->me.host = pt_arena_dup(&a->reqs, me->host);
    a->me.boot = pt_arena_dup(&a->reqs, me->boot);
    a->me.pid = me->pid;
    a->want_open = true;
    return !a->reqs.oom;
}

void pt_close(pt_agent *a)
{
    if (a->closing)
        return;
    a->closing = true;
    if (a->mine && !a->want_open)
        a->want_close = true;
    else
        a->closed = true;
}

bool pt_closed(const pt_agent *a)
{
    return a->closed;
}

bool pt_submit(pt_agent *a, const pt_input *in)
{
    uint32_t i;
    if (a->closing || a->fatal) {
        pt_core_seterr(a, "the session is closing", PT_STR_NULL);
        return false;
    }
    if (in->req.n) { /* idempotent: a known request ID is a no-op */
        for (i = 0; i < a->ninputs; i++)
            if (pt_str_eq(a->inputs[i]->req, in->req))
                return true;
        for (i = 0; i < a->nsubs; i++)
            if (pt_str_eq(a->subs[i].req, in->req))
                return true;
    }
    if (!pt_core_grow(a, (void **)&a->subs, &a->scap, a->nsubs + 1, sizeof(sub)))
        return false;
    a->subs[a->nsubs].text = pt_arena_dup(&a->subs_a, in->text);
    a->subs[a->nsubs].req = pt_arena_dup(&a->subs_a, in->req);
    a->nsubs++;
    a->idle_sent = false;
    return !a->subs_a.oom;
}

bool pt_abort(pt_agent *a)
{
    uint32_t i;
    for (i = 0; i < a->ntasks; i++)
        if (a->tasks[i]->status != ST_DONE) {
            a->abort_run = a->tasks[i]->id;
            a->idle_sent = false;
            return true;
        }
    return false;
}

/* ---- reading ---- */

uint32_t pt_entry_count(const pt_agent *a)
{
    return a->nentries;
}

bool pt_entry_at(const pt_agent *a, uint32_t i, pt_entry_view *out)
{
    static const char *const names[] = {"epoch", "user", "assistant", "tool_result", "note", "error"};
    const entry *e;
    if (i >= a->nentries)
        return false;
    e = &a->entries[i];
    out->id = e->id;
    out->epoch = e->epoch;
    out->kind = names[e->kind];
    out->msg = e->kind == ENT_ERROR ? NULL : &e->msg;
    out->raw = e->raw;
    return true;
}
