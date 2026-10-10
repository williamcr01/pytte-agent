/* In-memory state: the newest record of each ID (I3). Applying a commit and
 * recovering from the log are the same code: every commit is applied by
 * parsing its line, whether it was just written or read back after a crash. */
#include "core/core.h"

#include <string.h>

static const char *const task_status_names[] = {"pending", "running", "waiting", "done"};
static const char *const phase_names[] = {"prepare", "request", "retry"};
static const char *const input_status_names[] = {"queued", "placed", "done", "failed", "aborted"};
static const char *const ent_names[] = {"epoch", "user", "assistant", "tool_result", "note", "error"};

const char *pt_input_status_name(input_status s)
{
    return input_status_names[s];
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

static uint32_t getid(const pt_json *o, const char *k)
{
    int64_t v = pt_json_geti(o, k, 0);
    return v > 0 && v <= (int64_t)UINT32_MAX ? (uint32_t)v : 0;
}

/* ---- writing ---- */

void pt_write_task(pt_jw *w, const task *t)
{
    pt_jw_obj(w);
    pt_jw_key(w, "t");
    pt_jw_strz(w, "task");
    pt_jw_key(w, "id");
    pt_jw_uint(w, t->id);
    pt_jw_key(w, "kind");
    pt_jw_strz(w, "run");
    pt_jw_key(w, "status");
    pt_jw_strz(w, task_status_names[t->status]);
    pt_jw_key(w, "input");
    pt_jw_uint(w, t->input);
    if (t->status == ST_DONE) {
        pt_jw_key(w, "outcome");
        pt_jw_obj(w);
        pt_jw_key(w, "status");
        pt_jw_str(w, t->outcome);
        if (t->result) {
            pt_jw_key(w, "entry");
            pt_jw_uint(w, t->result);
        }
        if (t->reason.n) {
            pt_jw_key(w, "reason");
            pt_jw_str(w, t->reason);
        }
        pt_jw_obj_end(w);
    } else {
        pt_jw_key(w, "state");
        pt_jw_obj(w);
        pt_jw_key(w, "phase");
        pt_jw_strz(w, phase_names[t->phase]);
        pt_jw_key(w, "attempt");
        pt_jw_uint(w, t->attempt);
        if (t->phase == PH_REQUEST) {
            pt_jw_key(w, "cutoff");
            pt_jw_uint(w, t->cutoff);
            pt_jw_key(w, "params");
            pt_jw_obj(w);
            pt_jw_key(w, "model");
            pt_jw_str(w, t->model);
            pt_jw_key(w, "reasoning");
            pt_jw_strz(w, pt_reasoning_name(t->reasoning));
            pt_jw_key(w, "max_tokens");
            pt_jw_uint(w, t->max_tokens);
            pt_jw_obj_end(w);
        } else if (t->phase == PH_RETRY) {
            pt_jw_key(w, "until");
            pt_jw_int(w, t->until);
        }
        pt_jw_obj_end(w);
    }
    pt_jw_obj_end(w);
}

void pt_write_input(pt_jw *w, const input *in)
{
    pt_jw_obj(w);
    pt_jw_key(w, "t");
    pt_jw_strz(w, "input");
    pt_jw_key(w, "id");
    pt_jw_uint(w, in->id);
    if (in->req.n) {
        pt_jw_key(w, "req");
        pt_jw_str(w, in->req);
    }
    pt_jw_key(w, "kind");
    pt_jw_strz(w, "user");
    pt_jw_key(w, "text");
    pt_jw_str(w, in->text);
    pt_jw_key(w, "status");
    pt_jw_strz(w, input_status_names[in->status]);
    if (in->run) {
        pt_jw_key(w, "run");
        pt_jw_uint(w, in->run);
    }
    pt_jw_obj_end(w);
}

/* ---- applying ---- */

static bool read_task(const pt_json *r, task *t, pt_arena *ar)
{
    const pt_json *st = pt_json_get(r, "state"), *out = pt_json_get(r, "outcome");
    unsigned v;
    if (!pt_str_eqz(pt_json_gets(r, "kind", ar), "run"))
        return false;
    t->kind = TASK_RUN;
    if (!lookup(pt_json_gets(r, "status", ar), task_status_names, 4, &v))
        return false;
    t->status = (task_status)v;
    t->input = getid(r, "input");
    if (t->status == ST_DONE) {
        t->outcome = pt_json_gets(out, "status", ar);
        t->reason = pt_json_gets(out, "reason", ar);
        t->result = getid(out, "entry");
        return t->outcome.n > 0;
    }
    if (!lookup(pt_json_gets(st, "phase", ar), phase_names, 3, &v))
        return false;
    t->phase = (run_phase)v;
    t->attempt = getid(st, "attempt");
    if (t->phase == PH_REQUEST) {
        const pt_json *p = pt_json_get(st, "params");
        int64_t mt = pt_json_geti(p, "max_tokens", 0);
        t->cutoff = getid(st, "cutoff");
        t->model = pt_json_gets(p, "model", ar);
        if (!pt_reasoning_parse(pt_json_gets(p, "reasoning", ar), &t->reasoning))
            t->reasoning = PT_REASON_DEFAULT;
        t->max_tokens = mt > 0 && mt <= (int64_t)UINT32_MAX ? (uint32_t)mt : 0;
        if (!t->model.n)
            return false;
    } else if (t->phase == PH_RETRY) {
        t->until = pt_json_geti(st, "until", 0);
    }
    return t->attempt > 0;
}

static bool apply_task(pt_agent *a, const pt_json *r, uint32_t id)
{
    task *t = pt_core_task(a, id), nt;
    memset(&nt, 0, sizeof nt);
    /* Validate in temporary storage before replacing persistent strings. */
    if (!read_task(r, &nt, &a->scratch) || a->scratch.oom)
        return false;
    nt.id = id;
    if (nt.status != ST_DONE && nt.phase == PH_REQUEST && !pt_str_eq(nt.model, a->last_model))
        a->last_model = pt_arena_dup(&a->perm, nt.model);
    if (!t) {
        if (!pt_core_grow(a, (void **)&a->tasks, &a->tcap, a->ntasks + 1, sizeof(task *)))
            goto oom;
        t = (task *)pt_arena_zalloc(&a->objects, sizeof *t);
        if (!t)
            goto oom;
        pt_arena_init(&t->a, a->al, 128);
        pt_buf_init(&t->rescued, a->al);
        a->tasks[a->ntasks++] = t; /* IDs only grow, so this stays sorted */
    } else {
        pt_arena_reset(&t->a);
    }
    /* Replace the durable part; keep what belongs to this process. */
    {
        uint32_t fx = t->fx;
        run_live *live = t->live;
        pt_buf rescued = t->rescued;
        bool woke = t->woke;
        nt.a = t->a;
        if (nt.model.n)
            nt.model = pt_arena_dup(&nt.a, nt.model);
        if (nt.outcome.n)
            nt.outcome = pt_arena_dup(&nt.a, nt.outcome);
        if (nt.reason.n)
            nt.reason = pt_arena_dup(&nt.a, nt.reason);
        *t = nt;
        t->fx = fx;
        t->live = live;
        t->rescued = rescued;
        t->woke = woke;
    }
    if (t->a.oom || a->perm.oom)
        goto oom;
    return true;
oom:
    pt_core_fail(a, PT_FATAL_OOM, "out of memory", PT_STR_NULL);
    return false;
}

static bool apply_input(pt_agent *a, const pt_json *r, uint32_t id)
{
    input *in = pt_core_input(a, id);
    pt_str req, text;
    unsigned v;
    if (!lookup(pt_json_gets(r, "status", &a->scratch), input_status_names, 5, &v))
        return false;
    req = pt_json_gets(r, "req", &a->scratch);
    text = pt_json_gets(r, "text", &a->scratch);
    if (a->scratch.oom)
        goto oom;
    if (!in) {
        if (!pt_core_grow(a, (void **)&a->inputs, &a->icap, a->ninputs + 1, sizeof(input *)))
            goto oom;
        in = (input *)pt_arena_zalloc(&a->objects, sizeof *in);
        if (!in)
            goto oom;
        pt_arena_init(&in->a, a->al, 64);
        a->inputs[a->ninputs++] = in;
    }
    if (!pt_str_eq(in->req, req) || !pt_str_eq(in->text, text)) {
        pt_arena_reset(&in->a);
        in->req = req.n ? pt_arena_dup(&in->a, req) : PT_STR_NULL;
        in->text = text.n ? pt_arena_dup(&in->a, text) : PT_STR_NULL;
        if (in->a.oom)
            goto oom;
    }
    in->id = id;
    in->status = (input_status)v;
    in->run = getid(r, "run");
    return true;
oom:
    pt_core_fail(a, PT_FATAL_OOM, "out of memory", PT_STR_NULL);
    return false;
}

static bool apply_entry(pt_agent *a, const pt_json *r, uint32_t id)
{
    pt_arena *ea = &a->epoch, *sa = &a->scratch;
    const pt_json *d = pt_json_get(r, "data");
    unsigned k;
    entry *e;
    if (!lookup(pt_json_gets(r, "kind", sa), ent_names, 6, &k))
        return false;
    if (a->nentries && id <= a->entries[a->nentries - 1].id)
        return true; /* insert-only: never replaced */
    if (k == ENT_EPOCH) {
        /* A new epoch: older entries leave memory (they stay in the log). */
        a->nentries = 0;
        pt_arena_reset(ea);
        a->cur_epoch = getid(d, "n");
    }
    if (!pt_core_grow(a, (void **)&a->entries, &a->ecap, a->nentries + 1, sizeof(entry))) {
        pt_core_fail(a, PT_FATAL_OOM, "out of memory", PT_STR_NULL);
        return false;
    }
    e = &a->entries[a->nentries];
    memset(e, 0, sizeof *e);
    e->id = id;
    e->kind = (ent_kind)k;
    e->epoch = getid(r, "epoch");
    e->raw = pt_arena_dup(ea, r->raw);
    switch (e->kind) {
    case ENT_EPOCH:
        e->msg.role = PT_ROLE_SYSTEM;
        e->msg.text = pt_arena_dup(ea, pt_json_gets(d, "system", sa));
        break;
    case ENT_USER:
        e->msg.role = PT_ROLE_USER;
        e->msg.text = pt_arena_dup(ea, pt_json_gets(d, "content", sa));
        break;
    case ENT_ASSISTANT:
        if (!pt_msg_read_json(pt_json_get(d, "message"), &e->msg, ea))
            return false;
        break;
    case ENT_TOOL_RESULT:
        e->msg.role = PT_ROLE_TOOL_RESULT;
        e->msg.call_id = pt_arena_dup(ea, pt_json_gets(d, "call_id", sa));
        e->msg.name = pt_arena_dup(ea, pt_json_gets(d, "name", sa));
        e->msg.text = pt_arena_dup(ea, pt_json_gets(d, "content", sa));
        e->msg.is_error = pt_json_truthy(pt_json_get(d, "is_error"));
        break;
    case ENT_NOTE:
        e->msg.role = PT_ROLE_SYSTEM;
        e->msg.text = pt_arena_dup(ea, pt_json_gets(d, "text", sa));
        e->to_model = pt_json_truthy(pt_json_get(d, "to_model"));
        break;
    case ENT_ERROR:
        break;
    }
    if (ea->oom) {
        pt_core_fail(a, PT_FATAL_OOM, "out of memory", PT_STR_NULL);
        return false;
    }
    a->nentries++;
    return true;
}

static void apply_claim(pt_agent *a, const pt_json *r, uint32_t id)
{
    pt_arena *pa = &a->perm, *sa = &a->scratch;
    claim *c = &a->newest;
    c->id = id;
    c->event = pt_arena_dup(pa, pt_json_gets(r, "event", sa));
    c->token = pt_arena_dup(pa, pt_json_gets(r, "token", sa));
    c->host = pt_arena_dup(pa, pt_json_gets(r, "host", sa));
    c->boot = pt_arena_dup(pa, pt_json_gets(r, "boot", sa));
    c->pid = pt_json_geti(r, "pid", 0);
    c->opened = pt_json_geti(r, "opened", 0);
}

static bool apply_record(pt_agent *a, const pt_json *r)
{
    pt_str type = pt_json_gets(r, "t", &a->scratch);
    uint32_t id = getid(r, "id");
    bool ok = true;
    if (!pt_json_is(r, PT_J_OBJ) || !id)
        return false;
    if (pt_str_eqz(type, "session")) {
        if (pt_json_geti(r, "v", 0) != 1) {
            pt_core_seterr(a, "unsupported session version", PT_STR_NULL);
            return false;
        }
        a->have_session = true;
    } else if (pt_str_eqz(type, "entry")) {
        ok = apply_entry(a, r, id);
    } else if (pt_str_eqz(type, "task")) {
        ok = apply_task(a, r, id);
    } else if (pt_str_eqz(type, "input")) {
        ok = apply_input(a, r, id);
    } else if (pt_str_eqz(type, "claim")) {
        apply_claim(a, r, id);
    }
    /* Other types (docs, later kinds) are not needed here yet. */
    if (ok && id >= a->next_id)
        a->next_id = id + 1;
    return ok;
}

bool pt_state_apply(pt_agent *a, const pt_json *line)
{
    const pt_json *r;
    for (r = pt_json_get(line, "w")->child; r; r = r->next)
        if (!apply_record(a, r))
            return false;
    return true;
}
