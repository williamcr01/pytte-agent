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
    uint32_t id;
    if (!a->next_id || a->next_id == UINT32_MAX) {
        pt_core_fail(a, PT_FATAL_OOM, "record ID space exhausted", PT_STR_NULL);
        return 0;
    }
    id = a->next_id++;
    return id;
}

uint32_t pt_core_new_fx(pt_agent *a)
{
    return ++a->fxq.next;
}

bool pt_core_grow(pt_agent *a, void **arr, uint32_t *cap, uint32_t need, size_t elem)
{
    uint32_t ncap;
    void *p;
    if (need <= *cap)
        return true;
    ncap = *cap ? *cap : 16;
    while (ncap < need) {
        if (ncap > UINT32_MAX / 2) {
            ncap = need;
            break;
        }
        ncap *= 2;
    }
    if (elem && ncap > SIZE_MAX / elem)
        return false;
    p = a->al.fn(a->al.ud, *arr, elem * *cap, elem * ncap);
    if (!p)
        return false;
    *arr = p;
    *cap = ncap;
    return true;
}

task *pt_core_task(pt_agent *a, uint32_t id)
{
    uint32_t lo = 0, hi = a->ntasks;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (a->tasks[mid]->id < id)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < a->ntasks && a->tasks[lo]->id == id ? a->tasks[lo] : NULL;
}

input *pt_core_input(pt_agent *a, uint32_t id)
{
    uint32_t lo = 0, hi = a->ninputs;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (a->inputs[mid]->id < id)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < a->ninputs && a->inputs[lo]->id == id ? a->inputs[lo] : NULL;
}

entry *pt_core_last_entry(pt_agent *a)
{
    return a->nentries ? &a->entries[a->nentries - 1] : NULL;
}

entry *pt_core_entry(pt_agent *a, uint32_t id)
{
    uint32_t lo = 0, hi = a->nentries;
    while (lo < hi) { /* entries are in ID order */
        uint32_t mid = lo + (hi - lo) / 2;
        if (a->entries[mid].id < id)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < a->nentries && a->entries[lo].id == id ? &a->entries[lo] : NULL;
}

void pt_core_doc_begin(pt_agent *a, pt_str key)
{
    pt_jw *w = &a->commit.w;
    uint32_t i, id = 0;
    for (i = 0; i < a->ndocs; i++)
        if (pt_str_eq(a->docs[i].key, key))
            id = a->docs[i].id;
    pt_jw_obj(w);
    pt_jw_key(w, "t");
    pt_jw_strz(w, "doc");
    pt_jw_key(w, "id");
    pt_jw_uint(w, id ? id : pt_core_new_id(a));
    pt_jw_key(w, "key");
    pt_jw_str(w, key);
    pt_jw_key(w, "value");
}

void pt_core_write_doc(pt_agent *a, pt_str key, pt_str value)
{
    pt_core_doc_begin(a, key);
    pt_jw_raw(&a->commit.w, value);
    pt_jw_obj_end(&a->commit.w);
}

void pt_core_after(pt_agent *a, after_kind k, const char *outcome, pt_str error, int64_t until)
{
    a->commit.after = k;
    a->commit.outcome = pt_arena_dup(&a->commit.ar, pt_strz(outcome));
    a->commit.error = pt_arena_dup(&a->commit.ar, error);
    a->commit.until = until;
}

void pt_core_cancel(pt_agent *a, uint32_t task_id, uint32_t fx_id)
{
    pt_fx fx;
    memset(&fx, 0, sizeof fx);
    fx.kind = PT_FX_CANCEL;
    fx.id = pt_core_new_fx(a);
    fx.task = task_id;
    fx.cancel = fx_id;
    pt_core_queue(a, &fx, PT_STR_NULL);
}

void pt_core_live_del(pt_agent *a, uint32_t task_id)
{
    pt_fx fx;
    memset(&fx, 0, sizeof fx);
    fx.kind = PT_FX_LIVE;
    fx.id = pt_core_new_fx(a);
    fx.task = task_id;
    pt_core_queue(a, &fx, PT_STR_NULL);
}

void pt_task_release(pt_agent *a, task *t)
{
    pt_run_free_live(a, t);
    pt_tool_free_live(a, t);
    pt_buf_reset(&t->rescued);
    pt_buf_reset(&t->gate_text);
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
        for (i = 0; i < a->fxq.len; i++) {
            q = &a->fxq.v[(a->fxq.head + i) % PT_FXQ];
            if (q->fx.kind == PT_FX_LIVE && q->fx.task == fx->task) {
                pt_buf_reset(&q->bytes);
                pt_buf_puts(&q->bytes, bytes);
                return true;
            }
        }
    }
    if (a->fxq.len == PT_FXQ) {
        if (fx->kind == PT_FX_LIVE)
            return true; /* best effort */
        pt_core_fail(a, PT_FATAL_OOM, "effect queue full", PT_STR_NULL);
        return false;
    }
    q = &a->fxq.v[(a->fxq.head + a->fxq.len++) % PT_FXQ];
    q->fx = *fx;
    pt_buf_reset(&q->bytes);
    if (fx->kind == PT_FX_LIVE)
        pt_buf_puts(&q->bytes, bytes);
    return true;
}

/* ---- commits ---- */

void pt_core_commit_begin(pt_agent *a, uint32_t task_id)
{
    pt_log_begin(&a->commit.buf, &a->commit.w, a->seq + 1, pt_core_now(a));
    a->commit.task = task_id;
    a->commit.after = AFTER_NONE;
    a->commit.live_del = 0;
    a->commit.close = false;
    pt_arena_reset(&a->commit.ar);
}

bool pt_core_commit_end(pt_agent *a)
{
    pt_fx fx;
    task *t;
    pt_log_end(&a->commit.buf, &a->commit.w);
    if (a->commit.buf.oom) {
        pt_core_fail(a, PT_FATAL_OOM, "out of memory", PT_STR_NULL);
        return false;
    }
    memset(&fx, 0, sizeof fx);
    fx.kind = PT_FX_COMMIT;
    fx.id = pt_core_new_fx(a);
    fx.task = a->commit.task;
    fx.bytes = pt_buf_str(&a->commit.buf);
    fx.sync = !a->o.config.no_sync;
    if (!pt_core_queue(a, &fx, PT_STR_NULL))
        return false;
    a->commit.fx = fx.id;
    a->commit.out = true;
    t = a->commit.task ? pt_core_task(a, a->commit.task) : NULL;
    if (t)
        t->fx = fx.id;
    return true;
}

static void commit_landed(pt_agent *a, const char *error)
{
    task *t = a->commit.task ? pt_core_task(a, a->commit.task) : NULL;
    pt_str line = pt_buf_str(&a->commit.buf);
    const pt_json *root;
    uint64_t seq;
    pt_event ev;
    a->commit.out = false;
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
    ev.task = a->commit.task;
    ev.line = line;
    pt_core_emit(a, &ev);
    if (a->commit.live_del)
        pt_core_live_del(a, a->commit.live_del);
    if (a->commit.after != AFTER_NONE) {
        memset(&ev, 0, sizeof ev);
        ev.kind = a->commit.after == AFTER_RETRY ? PT_E_RETRY : a->commit.after == AFTER_COMPACT ? PT_E_COMPACT : PT_E_RUN_DONE;
        ev.task = a->commit.task;
        ev.outcome = a->commit.outcome;
        ev.error = a->commit.error;
        ev.until = a->commit.until;
        pt_core_emit(a, &ev);
    }
    if (a->commit.close)
        a->closed = true;
}

/* ---- session writes ---- */

void pt_core_entry_head(pt_agent *a, uint32_t id, const char *kind, uint32_t epoch)
{
    pt_jw *w = &a->commit.w;
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
    pt_core_entry_head(a, id, kind, a->cur_epoch);
}

void pt_core_entry_end(pt_agent *a)
{
    pt_jw_obj_end(&a->commit.w);
    pt_jw_obj_end(&a->commit.w);
}

static void build_create(pt_agent *a)
{
    pt_jw *w = &a->commit.w;
    const pt_session_cfg *c = &a->req.cfg;
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
    if (a->o.config.model.n) { /* resuming before the first request keeps it */
        pt_jw_key(w, "model");
        pt_jw_str(w, a->o.config.model);
    }
    if (c->prompt.n) {
        pt_jw_key(w, "prompt");
        pt_jw_raw(w, c->prompt);
    }
    pt_jw_obj_end(w);
    pt_core_entry_head(a, pt_core_new_id(a), "epoch", 1);
    pt_jw_key(w, "n");
    pt_jw_uint(w, 1);
    pt_jw_key(w, "system");
    pt_jw_str(w, c->system);
    pt_jw_key(w, "tools");
    pt_core_write_tool_defs(w, c->tools, c->ntools);
    pt_jw_key(w, "skills");
    pt_jw_raw(w, c->skills.n ? c->skills : PT_S("[]"));
    if (c->provisional) {
        pt_jw_key(w, "provisional");
        pt_jw_bool(w, true);
    }
    pt_core_entry_end(a);
    a->req.create = false;
    pt_core_commit_end(a);
}

void pt_core_write_tool_defs(pt_jw *w, const pt_tool_def *defs, uint32_t n)
{
    uint32_t i;
    pt_jw_arr(w);
    for (i = 0; i < n; i++) {
        const pt_tool_def *d = &defs[i];
        pt_jw_obj(w);
        pt_jw_key(w, "name");
        pt_jw_str(w, d->name);
        pt_jw_key(w, "description");
        pt_jw_str(w, d->description);
        pt_jw_key(w, "input_schema");
        pt_jw_raw(w, d->schema);
        pt_jw_key(w, "replay");
        pt_jw_strz(w, d->replay == PT_REPLAY_SAFE ? "safe" : "unsafe");
        pt_jw_key(w, "parallel");
        pt_jw_bool(w, d->parallel);
        if (d->read_only) {
            pt_jw_key(w, "read_only");
            pt_jw_bool(w, true);
        }
        pt_jw_key(w, "source");
        pt_jw_str(w, d->source);
        pt_jw_obj_end(w);
    }
    pt_jw_arr_end(w);
}

/* What a closing owner leaves behind, for `pytte ls` (which reads only
 * the log's tail): idle (nothing to do), waiting (a tool call waits for
 * approval) or busy (a run that the next owner continues). */
static const char *close_state(pt_agent *a)
{
    uint32_t i;
    if (pt_asks(a, NULL, 0))
        return "waiting";
    for (i = 0; i < a->ntasks; i++)
        if (a->tasks[i]->status != ST_DONE)
            return "busy";
    for (i = 0; i < a->ninputs; i++)
        if (a->inputs[i]->status == IN_QUEUED && a->inputs[i]->kind == INK_USER)
            return "busy";
    return "idle";
}

static void write_claim(pt_agent *a, const char *event)
{
    pt_jw *w = &a->commit.w;
    pt_jw_obj(w);
    pt_jw_key(w, "t");
    pt_jw_strz(w, "claim");
    pt_jw_key(w, "id");
    pt_jw_uint(w, a->claims.mine);
    pt_jw_key(w, "event");
    pt_jw_strz(w, event);
    pt_jw_key(w, "token");
    pt_jw_str(w, a->claims.token);
    pt_jw_key(w, "host");
    pt_jw_str(w, a->claims.me.host);
    pt_jw_key(w, "boot");
    pt_jw_str(w, a->claims.me.boot);
    pt_jw_key(w, "pid");
    pt_jw_int(w, a->claims.me.pid);
    pt_jw_key(w, "opened");
    pt_jw_int(w, a->claims.opened);
    if (*event == 'o' && a->claims.takeover.n) {
        pt_jw_key(w, "takeover");
        pt_jw_str(w, a->claims.takeover);
    }
    if (*event == 'c') {
        pt_jw_key(w, "closed");
        pt_jw_int(w, pt_core_now(a));
        pt_jw_key(w, "state");
        pt_jw_strz(w, close_state(a));
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
    a->claims.token = pt_arena_dup(&a->req.ar, (pt_str){tok, 16});
    a->claims.opened = pt_core_now(a);
    pt_core_commit_begin(a, 0);
    a->claims.mine = pt_core_new_id(a);
    write_claim(a, "open");
    a->req.open = false;
    pt_core_commit_end(a);
}

static void build_close(pt_agent *a)
{
    pt_core_commit_begin(a, 0);
    write_claim(a, "close");
    a->req.close = false;
    a->commit.close = true;
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
        if (a->inputs[i]->status == IN_QUEUED && a->inputs[i]->kind == INK_USER)
            return a->inputs[i];
    return NULL;
}

/* An input becomes a user entry and a run task, in one commit. */
static void write_placement(pt_agent *a, input *in)
{
    task t;
    input placed = *in;
    pt_jw *w = &a->commit.w;
    uint32_t entry_id = pt_core_new_id(a), run_id = pt_core_new_id(a);
    placed.status = IN_PLACED;
    placed.run = run_id;
    pt_write_input(w, &placed);
    pt_core_entry_begin(a, entry_id, "user");
    pt_jw_key(w, "content");
    if (in->blob.n) {
        pt_jw_obj(w);
        pt_jw_key(w, "blob");
        pt_jw_str(w, in->blob);
        pt_jw_key(w, "size");
        pt_jw_uint(w, in->text.n);
        pt_jw_obj_end(w);
    } else {
        pt_jw_str(w, in->text);
    }
    pt_jw_key(w, "input");
    pt_jw_uint(w, in->id);
    if (in->note) {
        pt_jw_key(w, "from");
        pt_jw_strz(w, "pytte");
    }
    pt_core_entry_end(a);
    memset(&t, 0, sizeof t);
    t.id = run_id;
    t.input = in->id;
    t.status = ST_PENDING;
    t.phase = PH_PREPARE;
    t.attempt = 1;
    pt_write_task(w, &t);
}

/* Commit the oldest submission: true when an effect was queued, false
 * while its text is still being stored as a blob. */
static bool build_submit(pt_agent *a)
{
    sub s = a->subs.v[0];
    input in;
    bool place = !active_run(a) && !first_queued(a);
    if (s.kind == INK_USER && s.text.n >= PT_BLOB_MIN && !s.blob_tried) {
        pt_fx fx;
        if (s.blob_fx)
            return false;
        memset(&fx, 0, sizeof fx);
        fx.kind = PT_FX_BLOB;
        fx.id = a->subs.v[0].blob_fx = pt_core_new_fx(a);
        fx.bytes = s.text; /* in subs.ar until this submission is committed */
        return pt_core_queue(a, &fx, PT_STR_NULL);
    }
    memset(&in, 0, sizeof in);
    pt_core_commit_begin(a, 0);
    in.id = pt_core_new_id(a);
    in.kind = s.kind;
    in.text = s.text;
    in.req = s.req;
    in.task = s.task;
    in.yes = s.yes;
    in.note = s.note;
    in.blob = s.blob;
    in.status = IN_QUEUED;
    if (place && s.kind == INK_USER)
        write_placement(a, &in);
    else
        pt_write_input(&a->commit.w, &in);
    pt_core_commit_end(a);
    memmove(a->subs.v, a->subs.v + 1, sizeof(sub) * --a->subs.n);
    if (!a->subs.n)
        pt_arena_reset(&a->subs.ar);
    return true;
}

void pt_fx_blob(pt_agent *a, uint32_t fx, pt_str ref)
{
    uint32_t i;
    for (i = 0; i < a->subs.n; i++)
        if (a->subs.v[i].blob_fx == fx) {
            a->subs.v[i].blob_fx = 0;
            a->subs.v[i].blob_tried = true;
            a->subs.v[i].blob = ref.n ? pt_arena_dup(&a->subs.ar, ref) : PT_STR_NULL;
            if (a->subs.ar.oom) /* inline, then */
                a->subs.v[i].blob = PT_STR_NULL;
            return;
        }
}

static void build_place(pt_agent *a, input *in)
{
    pt_core_commit_begin(a, 0);
    write_placement(a, in);
    pt_core_commit_end(a);
}

/* ---- scheduler ---- */

/* Whether a round's call may run alongside other calls. */
static bool tool_parallel(const task *run, uint32_t id)
{
    uint32_t i;
    for (i = 0; run && i < run->ncalls; i++)
        if (run->calls[i] == id)
            return run->parallel[i];
    return false;
}

static bool step(pt_agent *a)
{
    uint32_t i, round = 0;
    bool pending = false, exclusive = false; /* in round: a call not done; one that holds up the rest */
    input *q;
    pt_arena_reset(&a->scratch);
    if (a->commit.out || a->fatal)
        return false;
    if (a->req.create) {
        build_create(a);
        return true;
    }
    if (!a->loaded)
        return false;
    if (a->req.open) {
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
        /* Input accepted while the opening commit was pending must still
         * reach the log. Leave its run pending for the next host. */
        if (a->subs.n)
            return build_submit(a);
        if (!a->req.close) {
            a->closed = true;
            return false;
        }
        build_close(a);
        return true;
    }
    if (a->subs.n && build_submit(a))
        return true;
    if (a->req.compact) {
        a->req.compact = false;
        if (!active_run(a) && pt_compact_possible(a)) {
            entry *last = pt_core_last_entry(a);
            pt_core_commit_begin(a, 0);
            pt_compact_write(a, 0, last ? last->id : 0);
            pt_core_commit_end(a);
            return true;
        }
    }
    if (!active_run(a) && (q = first_queued(a))) {
        build_place(a, q);
        return true;
    }
    for (i = 0; i < a->ntasks; i++) {
        task *t = a->tasks[i];
        if (t->status == ST_DONE)
            continue;
        if (t->kind == TASK_TOOL) {
            /* A round's calls go in call order (section 7.5): a parallel
             * call runs alongside the parallel calls before it, the others
             * wait for every call before them, and hold up every call
             * after. Done calls have left the table. */
            bool par = tool_parallel(pt_core_task(a, t->owner), t->id);
            if (t->owner != round) {
                round = t->owner;
                pending = exclusive = false;
            }
            if (par ? exclusive : pending) {
                exclusive = true; /* holds up the rest of the round */
                continue;
            }
            pending = true;
            exclusive = exclusive || !par;
            if (!t->fx && pt_tool_step(a, t))
                return true;
        } else if (!t->fx && (t->kind == TASK_COMPACT ? pt_compact_step(a, t) : pt_run_step(a, t))) {
            return true;
        }
        if (a->fatal || a->commit.out)
            return false;
    }
    return false;
}

int pt_poll(pt_agent *a, pt_fx *out)
{
    queued_fx *q;
    pt_run_poll_begin(a);
    if (a->fatal)
        return 0;
    if (!a->fxq.len)
        step(a);
    if (a->fatal)
        return 0;
    if (!a->fxq.len) {
        if (pt_idle(a) && !a->idle_sent) {
            pt_event ev;
            a->idle_sent = true;
            memset(&ev, 0, sizeof ev);
            ev.kind = PT_E_IDLE;
            pt_core_emit(a, &ev);
        }
        return 0;
    }
    q = &a->fxq.v[a->fxq.head];
    a->fxq.head = (a->fxq.head + 1) % PT_FXQ;
    a->fxq.len--;
    *out = q->fx;
    if (out->kind == PT_FX_CANCEL)
        pt_run_cancel_delivered(a, out->cancel);
    if (out->kind == PT_FX_LIVE) { /* display-only snapshot: failure is best effort */
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
    if (a->fatal || a->commit.out || a->fxq.len || a->req.create || a->req.open || a->req.close ||
        a->subs.n || a->req.compact || !a->loaded)
        return false;
    if (a->closing)
        return a->closed;
    for (i = 0; i < a->ntasks; i++)
        if (a->tasks[i]->status != ST_DONE)
            return false;
    for (i = 0; i < a->ninputs; i++)
        if (a->inputs[i]->status == IN_QUEUED && a->inputs[i]->kind == INK_USER)
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
    if (!a->fatal && !pt_run_http_data(a, fx, p, n))
        pt_tool_data(a, fx, p, n);
}

void pt_fx_done(pt_agent *a, uint32_t fx, const char *error)
{
    uint32_t i;
    if (a->fatal)
        return;
    if (a->commit.out && fx == a->commit.fx) {
        commit_landed(a, error);
        return;
    }
    if (pt_run_http_done(a, fx, error))
        return;
    for (i = 0; i < a->ntasks; i++) /* a retry sleep */
        if (a->tasks[i]->fx == fx && a->tasks[i]->phase == PH_RETRY) {
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
    pt_arena_init(&a->epoch, a->al, 4096);
    pt_arena_init(&a->scratch, a->al, 8192);
    pt_arena_init(&a->objects, a->al, 1024);
    pt_arena_init(&a->req.ar, a->al, 1024);
    pt_arena_init(&a->subs.ar, a->al, 1024);
    pt_arena_init(&a->commit.ar, a->al, 1024);
    pt_buf_init(&a->commit.buf, a->al);
    pt_buf_init(&a->live_out, a->al);
    pt_buf_init(&a->mat.system, a->al);
    pt_buf_init(&a->mat.tools, a->al);
    pt_buf_init(&a->mat.skills, a->al);
    for (i = 0; i < PT_FXQ; i++)
        pt_buf_init(&a->fxq.v[i].bytes, a->al);
    a->next_id = 1;
    a->o.config.model = pt_arena_dup(&a->perm, o->config.model);
    a->o.config.compact_model = pt_arena_dup(&a->perm, o->config.compact_model);
    if (a->perm.oom) {
        pt_free(a);
        return NULL;
    }
    return a;
}

void pt_free(pt_agent *a)
{
    uint32_t i;
    if (!a)
        return;
    pt_run_free_pending(a);
    for (i = 0; i < a->ntasks; i++) {
        task *t = a->tasks[i];
        pt_task_release(a, t);
        pt_arena_free(&t->a);
        pt_buf_free(&t->rescued);
        pt_buf_free(&t->gate_text);
    }
    pt_run_free_pending(a);
    while (a->free_tasks) {
        task *t = a->free_tasks;
        a->free_tasks = t->next_free;
        pt_arena_free(&t->a);
        pt_buf_free(&t->rescued);
        pt_buf_free(&t->gate_text);
    }
    for (i = 0; i < a->ndocs; i++)
        pt_buf_free(&a->docs[i].value);
    a->al.fn(a->al.ud, a->docs, sizeof(doc) * a->dcap, 0);
    for (i = 0; i < a->ninputs; i++) {
        pt_arena_free(&a->inputs[i]->a);
    }
    a->al.fn(a->al.ud, a->tasks, sizeof(task *) * a->tcap, 0);
    a->al.fn(a->al.ud, a->inputs, sizeof(input *) * a->icap, 0);
    a->al.fn(a->al.ud, a->entries, sizeof(entry) * a->ecap, 0);
    a->al.fn(a->al.ud, a->subs.v, sizeof(sub) * a->subs.cap, 0);
    for (i = 0; i < PT_FXQ; i++)
        pt_buf_free(&a->fxq.v[i].bytes);
    pt_buf_free(&a->commit.buf);
    pt_buf_free(&a->live_out);
    pt_buf_free(&a->mat.system);
    pt_buf_free(&a->mat.tools);
    pt_buf_free(&a->mat.skills);
    pt_arena_free(&a->perm);
    pt_arena_free(&a->epoch);
    pt_arena_free(&a->scratch);
    pt_arena_free(&a->objects);
    pt_arena_free(&a->req.ar);
    pt_arena_free(&a->subs.ar);
    pt_arena_free(&a->commit.ar);
    a->al.fn(a->al.ud, a, sizeof *a, 0);
}

void pt_set_config(pt_agent *a, const pt_config *c)
{
    pt_str model = a->o.config.model, cm = a->o.config.compact_model;
    if (!pt_str_eq(model, c->model))
        model = pt_arena_dup(&a->perm, c->model);
    if (!pt_str_eq(cm, c->compact_model))
        cm = pt_arena_dup(&a->perm, c->compact_model);
    if (a->perm.oom) {
        pt_core_fail(a, PT_FATAL_OOM, "out of memory setting configuration", PT_STR_NULL);
        return;
    }
    a->o.config = *c;
    a->o.config.model = model;
    a->o.config.compact_model = cm;
}

bool pt_create(pt_agent *a, const pt_session_cfg *c)
{
    if (a->loaded || a->req.create) {
        pt_core_seterr(a, "the session already exists", PT_STR_NULL);
        return false;
    }
    if (!pt_core_raw_is(a, c->skills, PT_J_ARR) || !pt_core_raw_is(a, c->prompt, PT_J_OBJ)) {
        pt_core_seterr(a, "skills must be a JSON array and prompt an object", PT_STR_NULL);
        return false;
    }
    a->req.cfg.skills = pt_arena_dup(&a->req.ar, c->skills);
    a->req.cfg.prompt = pt_arena_dup(&a->req.ar, c->prompt);
    a->prompt = pt_arena_dup(&a->perm, c->prompt);
    a->req.cfg.cwd = pt_arena_dup(&a->req.ar, c->cwd);
    a->req.cfg.provisional = c->provisional;
    a->req.cfg.host = pt_arena_dup(&a->req.ar, c->host);
    a->req.cfg.system = pt_arena_dup(&a->req.ar, c->system);
    a->req.cfg.ntools = 0;
    if (c->ntools) {
        pt_tool_def *d = (pt_tool_def *)pt_arena_alloc(&a->req.ar, sizeof(pt_tool_def) * c->ntools);
        uint32_t i;
        if (!d) {
            pt_core_fail(a, PT_FATAL_OOM, "out of memory creating session", PT_STR_NULL);
            return false;
        }
        for (i = 0; i < c->ntools; i++) {
            d[i] = c->tools[i];
            d[i].name = pt_arena_dup(&a->req.ar, c->tools[i].name);
            d[i].description = pt_arena_dup(&a->req.ar, c->tools[i].description);
            d[i].schema = pt_arena_dup(&a->req.ar, c->tools[i].schema);
            d[i].source = pt_arena_dup(&a->req.ar, c->tools[i].source);
        }
        a->req.cfg.tools = d;
        a->req.cfg.ntools = c->ntools;
    }
    if (a->req.ar.oom || a->perm.oom) {
        pt_core_fail(a, PT_FATAL_OOM, "out of memory creating session", PT_STR_NULL);
        return false;
    }
    a->req.create = true;
    a->loaded = true;
    a->have_session = true;
    a->cur_epoch = 1;
    return true;
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
    if (t && t->status != ST_DONE && (t->phase == PH_REQUEST || t->phase == PH_EXECUTE)) {
        pt_buf_reset(&t->rescued);
        pt_buf_puts(&t->rescued, bytes);
    } else { /* stale: its commit landed, but the delete did not */
        pt_core_live_del(a, task_id);
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
    const claim *c = &a->claims.newest;
    if (!a->loaded) {
        pt_core_seterr(a, "no session loaded", PT_STR_NULL);
        return false;
    }
    a->claims.takeover = PT_STR_NULL;
    a->claims.quit = c->id && pt_str_eqz(c->event, "close");
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
        a->claims.takeover = pt_arena_dup(&a->req.ar, c->token);
    }
    a->claims.me.host = pt_arena_dup(&a->req.ar, me->host);
    a->claims.me.boot = pt_arena_dup(&a->req.ar, me->boot);
    a->claims.me.pid = me->pid;
    a->req.open = true;
    return !a->req.ar.oom;
}

void pt_close(pt_agent *a)
{
    if (a->closing)
        return;
    a->closing = true;
    if (a->claims.mine || a->req.open)
        a->req.close = true;
    else if (!a->req.create && !a->commit.out && !a->subs.n)
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
        for (i = 0; i < a->subs.n; i++)
            if (pt_str_eq(a->subs.v[i].req, in->req))
                return true;
    }
    if (!pt_core_grow(a, (void **)&a->subs.v, &a->subs.cap, a->subs.n + 1, sizeof(sub)))
        return false;
    {
        sub pending;
        memset(&pending, 0, sizeof pending);
        pending.text = pt_arena_dup(&a->subs.ar, in->text);
        pending.req = pt_arena_dup(&a->subs.ar, in->req);
        pending.note = in->note;
        if (a->subs.ar.oom) {
            pt_core_fail(a, PT_FATAL_OOM, "out of memory queueing submission", PT_STR_NULL);
            return false;
        }
        a->subs.v[a->subs.n++] = pending;
    }
    a->idle_sent = false;
    return true;
}

bool pt_compact(pt_agent *a)
{
    if (a->fatal || active_run(a) || a->req.compact || !pt_compact_possible(a))
        return false;
    a->req.compact = true;
    a->idle_sent = false;
    return true;
}

bool pt_abort(pt_agent *a)
{
    uint32_t i;
    for (i = 0; i < a->ntasks; i++)
        if (a->tasks[i]->status != ST_DONE &&
            (a->tasks[i]->kind == TASK_RUN || (a->tasks[i]->kind == TASK_COMPACT && !a->tasks[i]->owner))) {
            a->abort_run = a->tasks[i]->id;
            a->idle_sent = false;
            return true;
        }
    return false;
}

/* ---- reading ---- */

pt_str pt_doc(const pt_agent *a, pt_str key)
{
    uint32_t i;
    for (i = 0; i < a->ndocs; i++)
        if (pt_str_eq(a->docs[i].key, key))
            return pt_buf_str(&a->docs[i].value);
    return PT_STR_NULL;
}

pt_str pt_session_cwd(const pt_agent *a)
{
    return a->cwd.n ? a->cwd : a->req.cfg.cwd;
}

pt_str pt_session_model(const pt_agent *a)
{
    return a->last_model;
}

uint64_t pt_context_tokens(const pt_agent *a)
{
    return pt_core_estimate(a, UINT32_MAX);
}

pt_str pt_session_prompt(const pt_agent *a)
{
    return a->prompt;
}

bool pt_core_raw_is(pt_agent *a, pt_str raw, pt_jtype kind)
{
    const pt_json *j;
    if (!raw.n)
        return true;
    j = pt_json_parse(raw, &a->scratch, NULL);
    return j && pt_json_is(j, kind);
}

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
    out->summary = e->summary;
    out->files = e->files;
    out->kept = e->kept;
    return true;
}
