/* In-memory state: the newest record of each ID (I3). Applying a commit and
 * recovering from the log are the same code: every commit is applied by
 * parsing its line, whether it was just written or read back after a crash. */
#include "core/core.h"

#include <string.h>

static const char *const task_status_names[] = {"pending", "running", "waiting", "done"};
static const char *const phase_names[] = {"prepare", "request", "retry", "tools",
                                          "call", "approve", "execute", "compact_wait", "restore"};
#define NPHASES 9
static const char *const kind_names[] = {"run", "tool", "compact", "rewind"};
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

/* {"path":p} or {"path":p,"blob":ref}: a file as it was (no blob: it did not exist). */
static void write_file(pt_jw *w, pt_str path, pt_str blob)
{
    pt_jw_obj(w);
    pt_jw_key(w, "path");
    pt_jw_str(w, path);
    if (blob.n) {
        pt_jw_key(w, "blob");
        pt_jw_str(w, blob);
    }
    pt_jw_obj_end(w);
}

static void write_state(pt_jw *w, const task *t)
{
    uint32_t i;
    pt_jw_key(w, "phase");
    pt_jw_strz(w, phase_names[t->phase]);
    if (t->kind == TASK_TOOL) {
        pt_jw_key(w, "entry");
        pt_jw_uint(w, t->entry);
        pt_jw_key(w, "call_id");
        pt_jw_str(w, t->call_id);
        if (t->args.n) {
            pt_jw_key(w, "args");
            pt_jw_raw(w, t->args);
        }
        if (t->phase == PH_APPROVE) {
            pt_jw_key(w, "prompt");
            pt_jw_str(w, t->prompt);
        } else if (t->phase == PH_EXECUTE) {
            if (t->rule.n) {
                pt_jw_key(w, "rule");
                pt_jw_str(w, t->rule);
            }
            pt_jw_key(w, "replay");
            pt_jw_strz(w, t->replay == PT_REPLAY_SAFE ? "safe" : "unsafe");
            pt_jw_key(w, "attempt");
            pt_jw_uint(w, t->attempt);
            if (t->snap_path.n) { /* before the call changes the file (section 8.6) */
                pt_jw_key(w, "snap");
                write_file(w, t->snap_path, t->snap_blob);
            }
        }
        return;
    }
    if (t->kind == TASK_REWIND) {
        pt_jw_key(w, "entry");
        pt_jw_uint(w, t->entry);
        pt_jw_key(w, "files");
        pt_jw_arr(w);
        for (i = 0; i < t->nfiles; i++)
            write_file(w, t->files[i].path, t->files[i].blob);
        pt_jw_arr_end(w);
        return;
    }
    if (t->phase == PH_COMPACT) {
        pt_jw_key(w, "compact");
        pt_jw_uint(w, t->compact);
        return;
    }
    pt_jw_key(w, "attempt");
    pt_jw_uint(w, t->attempt);
    if (t->phase == PH_REQUEST || t->kind == TASK_COMPACT) { /* a compaction's cutoff is fixed */
        pt_jw_key(w, "cutoff");
        pt_jw_uint(w, t->cutoff);
    }
    if (t->kind == TASK_COMPACT && t->keep) {
        pt_jw_key(w, "keep");
        pt_jw_uint(w, t->keep);
    }
    if (t->phase == PH_REQUEST) {
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
    } else if (t->phase == PH_TOOLS) {
        pt_jw_key(w, "assistant");
        pt_jw_uint(w, t->entry);
        pt_jw_key(w, "calls");
        pt_jw_arr(w);
        for (i = 0; i < t->ncalls; i++)
            pt_jw_uint(w, t->calls[i]);
        pt_jw_arr_end(w);
        pt_jw_key(w, "parallel");
        pt_jw_arr(w);
        for (i = 0; i < t->ncalls; i++)
            pt_jw_bool(w, t->parallel[i]);
        pt_jw_arr_end(w);
    }
}

void pt_write_task(pt_jw *w, const task *t)
{
    pt_jw_obj(w);
    pt_jw_key(w, "t");
    pt_jw_strz(w, "task");
    pt_jw_key(w, "id");
    pt_jw_uint(w, t->id);
    pt_jw_key(w, "kind");
    pt_jw_strz(w, kind_names[t->kind]);
    if (t->owner) {
        pt_jw_key(w, "owner");
        pt_jw_uint(w, t->owner);
    }
    pt_jw_key(w, "status");
    pt_jw_strz(w, task_status_names[t->status]);
    if (t->kind == TASK_RUN) {
        pt_jw_key(w, "input");
        pt_jw_uint(w, t->input);
    }
    if (t->abort && t->status != ST_DONE) {
        pt_jw_key(w, "abort");
        pt_jw_bool(w, true);
    }
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
        write_state(w, t);
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
    pt_jw_strz(w, in->kind == INK_APPROVE ? "approve" : "user");
    if (in->note) {
        pt_jw_key(w, "from");
        pt_jw_strz(w, "pytte");
    }
    if (in->steer) {
        pt_jw_key(w, "when_busy");
        pt_jw_strz(w, "steer");
    }
    if (in->kind == INK_APPROVE) {
        pt_jw_key(w, "task");
        pt_jw_uint(w, in->task);
        pt_jw_key(w, "answer");
        pt_jw_strz(w, in->yes ? "yes" : "no");
    }
    if (in->blob.n) {
        pt_jw_key(w, "blob");
        pt_jw_str(w, in->blob);
        pt_jw_key(w, "size");
        pt_jw_uint(w, in->text.n);
    } else {
        pt_jw_key(w, "text");
        pt_jw_str(w, in->text);
    }
    if (in->images.n) {
        pt_jw_key(w, "images");
        pt_jw_raw(w, in->images);
    }
    pt_jw_key(w, "status");
    pt_jw_strz(w, input_status_names[in->status]);
    if (in->run) {
        pt_jw_key(w, "run");
        pt_jw_uint(w, in->run);
    }
    pt_jw_obj_end(w);
}

/* ---- applying ---- */

/* The state of a rewind: the user entry, and the files to put back. */
static bool read_rewind(const pt_json *st, task *t, pt_arena *ar)
{
    const pt_json *files = pt_json_get(st, "files"), *f;
    uint32_t i = 0;
    t->entry = getid(st, "entry");
    if (!pt_json_is(files, PT_J_ARR) || !t->entry)
        return false;
#if SIZE_MAX <= UINT32_MAX
    if (files->len > SIZE_MAX / sizeof(pt_restore))
        return false;
#endif
    t->nfiles = files->len;
    t->files = (pt_restore *)pt_arena_zalloc(ar, sizeof(pt_restore) * (files->len ? files->len : 1));
    if (!t->files)
        return false;
    for (f = files->child; f; f = f->next, i++) {
        t->files[i].path = pt_json_gets(f, "path", ar);
        t->files[i].blob = pt_json_gets(f, "blob", ar);
        if (!pt_json_is(f, PT_J_OBJ) || !t->files[i].path.n)
            return false;
    }
    return true;
}

static bool read_task(const pt_json *r, task *t, pt_arena *ar)
{
    const pt_json *st = pt_json_get(r, "state"), *out = pt_json_get(r, "outcome");
    unsigned v;
    if (!lookup(pt_json_gets(r, "kind", ar), kind_names, 4, &v))
        return false;
    t->kind = (task_kind)v;
    if (!lookup(pt_json_gets(r, "status", ar), task_status_names, 4, &v))
        return false;
    t->status = (task_status)v;
    t->input = getid(r, "input");
    t->owner = getid(r, "owner");
    t->abort = pt_json_truthy(pt_json_get(r, "abort"));
    if (t->kind == TASK_TOOL && !t->owner)
        return false;
    if (t->status == ST_DONE) {
        t->outcome = pt_json_gets(out, "status", ar);
        t->reason = pt_json_gets(out, "reason", ar);
        t->result = getid(out, "entry");
        return t->outcome.n > 0;
    }
    if (!lookup(pt_json_gets(st, "phase", ar), phase_names, NPHASES, &v))
        return false;
    t->phase = (task_phase)v;
    if (t->kind == TASK_TOOL ? t->phase < PH_CALL || t->phase > PH_EXECUTE
        : t->kind == TASK_COMPACT ? t->phase > PH_RETRY
        : t->kind == TASK_REWIND  ? t->phase != PH_RESTORE
                                  : t->phase >= PH_CALL && t->phase != PH_COMPACT)
        return false;
    if (t->kind == TASK_REWIND)
        return read_rewind(st, t, ar);
    if (t->phase == PH_COMPACT)
        return (t->compact = getid(st, "compact")) != 0;
    t->attempt = getid(st, "attempt");
    if (t->kind == TASK_TOOL) {
        const pt_json *args = pt_json_get(st, "args");
        t->entry = getid(st, "entry");
        t->call_id = pt_json_gets(st, "call_id", ar);
        t->args = pt_json_is(args, PT_J_OBJ) ? args->raw : PT_STR_NULL;
        t->prompt = pt_json_gets(st, "prompt", ar);
        t->rule = pt_json_gets(st, "rule", ar);
        t->replay = pt_str_eqz(pt_json_gets(st, "replay", ar), "safe") ? PT_REPLAY_SAFE : PT_REPLAY_UNSAFE;
        if (pt_json_is(pt_json_get(st, "snap"), PT_J_OBJ)) {
            const pt_json *snap = pt_json_get(st, "snap");
            t->snap_path = pt_json_gets(snap, "path", ar);
            t->snap_blob = pt_json_gets(snap, "blob", ar);
            if (!t->snap_path.n)
                return false;
        }
        return t->entry > 0 && (t->phase != PH_EXECUTE || t->attempt > 0);
    }
    t->cutoff = getid(st, "cutoff");
    t->keep = getid(st, "keep");
    if (t->kind == TASK_COMPACT && (!t->cutoff || t->keep > t->cutoff))
        return false;
    if (t->phase == PH_REQUEST) {
        const pt_json *p = pt_json_get(st, "params");
        int64_t mt = pt_json_geti(p, "max_tokens", 0);
        t->model = pt_json_gets(p, "model", ar);
        if (!pt_reasoning_parse(pt_json_gets(p, "reasoning", ar), &t->reasoning))
            t->reasoning = PT_REASON_DEFAULT;
        t->max_tokens = mt > 0 && mt <= (int64_t)UINT32_MAX ? (uint32_t)mt : 0;
        if (!t->model.n)
            return false;
    } else if (t->phase == PH_RETRY) {
        t->until = pt_json_geti(st, "until", 0);
    } else if (t->phase == PH_TOOLS) {
        const pt_json *calls = pt_json_get(st, "calls"), *par = pt_json_get(st, "parallel"), *c;
        uint32_t i = 0;
        t->entry = getid(st, "assistant");
        if (!pt_json_is(calls, PT_J_ARR) || !pt_json_is(par, PT_J_ARR) || par->len != calls->len || !t->entry)
            return false;
#if SIZE_MAX <= UINT32_MAX
        if (calls->len > SIZE_MAX / sizeof(uint32_t))
            return false;
#endif
        t->ncalls = calls->len;
        t->calls = (uint32_t *)pt_arena_alloc(ar, sizeof(uint32_t) * (calls->len ? calls->len : 1));
        t->parallel = (bool *)pt_arena_alloc(ar, sizeof(bool) * (calls->len ? calls->len : 1));
        if (!t->calls || !t->parallel)
            return false;
        for (c = calls->child; c; c = c->next) {
            int64_t id;
            if (!pt_json_int(c, &id) || id <= 0 || id > (int64_t)UINT32_MAX)
                return false;
            t->calls[i++] = (uint32_t)id;
        }
        for (i = 0, c = par->child; c; c = c->next)
            t->parallel[i++] = pt_json_truthy(c);
    }
    return t->attempt > 0;
}

/* A task that is gone from memory: done tool tasks leave the table, and
 * their struct is reused by the next one. */
static void drop_task(pt_agent *a, task *t)
{
    uint32_t i;
    for (i = 0; i < a->ntasks && a->tasks[i] != t; i++)
        ;
    if (i == a->ntasks)
        return;
    memmove(a->tasks + i, a->tasks + i + 1, sizeof(task *) * (a->ntasks - i - 1));
    a->ntasks--;
    pt_task_release(a, t);
    pt_arena_reset(&t->a);
    t->next_free = a->free_tasks;
    a->free_tasks = t;
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
    if (nt.status == ST_DONE && nt.kind != TASK_RUN) { /* only runs are looked back at */
        if (t)
            drop_task(a, t);
        return true;
    }
    if (!t) {
        if (!pt_core_grow(a, (void **)&a->tasks, &a->tcap, a->ntasks + 1, sizeof(task *)))
            goto oom;
        if (a->free_tasks) {
            pt_arena ar;
            pt_buf rescued, gate_text;
            t = a->free_tasks;
            a->free_tasks = t->next_free;
            ar = t->a;
            rescued = t->rescued;
            gate_text = t->gate_text;
            memset(t, 0, sizeof *t);
            t->a = ar;
            t->rescued = rescued;
            t->gate_text = gate_text;
        } else {
            t = (task *)pt_arena_zalloc(&a->objects, sizeof *t);
            if (!t)
                goto oom;
            pt_arena_init(&t->a, a->al, 128);
            pt_buf_init(&t->rescued, a->al);
            pt_buf_init(&t->gate_text, a->al);
        }
        a->tasks[a->ntasks++] = t; /* IDs only grow, so this stays sorted */
    } else {
        pt_arena_reset(&t->a);
    }
    /* Replace the durable part; keep what belongs to this process. */
    {
        task keep = *t;
        nt.a = t->a;
        if (nt.model.n)
            nt.model = pt_arena_dup(&nt.a, nt.model);
        if (nt.outcome.n)
            nt.outcome = pt_arena_dup(&nt.a, nt.outcome);
        if (nt.reason.n)
            nt.reason = pt_arena_dup(&nt.a, nt.reason);
        if (nt.call_id.n)
            nt.call_id = pt_arena_dup(&nt.a, nt.call_id);
        if (nt.args.n)
            nt.args = pt_arena_dup(&nt.a, nt.args);
        if (nt.prompt.n)
            nt.prompt = pt_arena_dup(&nt.a, nt.prompt);
        if (nt.snap_path.n)
            nt.snap_path = pt_arena_dup(&nt.a, nt.snap_path);
        if (nt.snap_blob.n)
            nt.snap_blob = pt_arena_dup(&nt.a, nt.snap_blob);
        if (nt.nfiles) {
            uint32_t k;
            pt_restore *fl = (pt_restore *)pt_arena_alloc(&nt.a, sizeof(pt_restore) * nt.nfiles);
            if (fl)
                for (k = 0; k < nt.nfiles; k++) {
                    fl[k].path = pt_arena_dup(&nt.a, nt.files[k].path);
                    fl[k].blob = nt.files[k].blob.n ? pt_arena_dup(&nt.a, nt.files[k].blob) : PT_STR_NULL;
                }
            nt.files = fl;
        }
        if (nt.rule.n)
            nt.rule = pt_arena_dup(&nt.a, nt.rule);
        if (nt.ncalls) {
            uint32_t *c = (uint32_t *)pt_arena_alloc(&nt.a, sizeof(uint32_t) * nt.ncalls);
            bool *par = (bool *)pt_arena_alloc(&nt.a, sizeof(bool) * nt.ncalls);
            if (c)
                memcpy(c, nt.calls, sizeof(uint32_t) * nt.ncalls);
            if (par)
                memcpy(par, nt.parallel, sizeof(bool) * nt.ncalls);
            nt.calls = c;
            nt.parallel = par;
        }
        *t = nt;
        t->fx = keep.fx;
        t->live = keep.live;
        t->tl = keep.tl;
        t->rescued = keep.rescued;
        t->woke = keep.woke;
        t->recovered = !a->loaded; /* this record came from the log */
        t->asked = keep.asked && t->phase == PH_APPROVE;
        t->gated = keep.gated && t->phase == PH_CALL;
        t->gate = keep.gate;
        t->gate_text = keep.gate_text;
        t->next_free = NULL;
    }
    if (t->a.oom || a->perm.oom)
        goto oom;
    return true;
oom:
    pt_core_fail(a, PT_FATAL_OOM, "out of memory", PT_STR_NULL);
    return false;
}

/* The text of a blob ref, read through the host, into ar. */
static bool blob_text(pt_agent *a, pt_str ref, pt_arena *ar, pt_str *out)
{
    if (!a->o.blob || !a->o.blob(a->o.blob_ud, ref, ar, out)) {
        pt_core_seterr(a, "a blob the log refers to cannot be read", ref);
        return false;
    }
    return true;
}

static bool apply_input(pt_agent *a, const pt_json *r, uint32_t id)
{
    input *in = pt_core_input(a, id);
    pt_str req, text, blob;
    input_kind kind;
    unsigned v;
    if (!lookup(pt_json_gets(r, "status", &a->scratch), input_status_names, 5, &v))
        return false;
    req = pt_json_gets(r, "req", &a->scratch);
    text = pt_json_gets(r, "text", &a->scratch);
    blob = pt_json_gets(r, "blob", &a->scratch);
    kind = pt_str_eqz(pt_json_gets(r, "kind", &a->scratch), "approve") ? INK_APPROVE : INK_USER;
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
    if (blob.n) { /* the text is read once: rewrites of the record keep it */
        if (!pt_str_eq(in->blob, blob) || !pt_str_eq(in->req, req)) {
            pt_arena_reset(&in->a);
            in->req = req.n ? pt_arena_dup(&in->a, req) : PT_STR_NULL;
            in->blob = pt_arena_dup(&in->a, blob);
            if (!blob_text(a, blob, &in->a, &in->text))
                return false;
            if (in->a.oom)
                goto oom;
        }
    } else if (in->blob.n || !pt_str_eq(in->req, req) || !pt_str_eq(in->text, text)) {
        pt_arena_reset(&in->a);
        in->req = req.n ? pt_arena_dup(&in->a, req) : PT_STR_NULL;
        in->text = text.n ? pt_arena_dup(&in->a, text) : PT_STR_NULL;
        in->blob = PT_STR_NULL;
        if (in->a.oom)
            goto oom;
    }
    {
        const pt_json *im = pt_json_get(r, "images");
        pt_str raw = pt_json_is(im, PT_J_ARR) ? im->raw : PT_STR_NULL;
        if (!pt_str_eq(in->images, raw)) { /* logged once, then repeated by each rewrite */
            in->images = raw.n ? pt_arena_dup(&in->a, raw) : PT_STR_NULL;
            if (in->a.oom)
                goto oom;
        }
    }
    in->id = id;
    in->kind = kind;
    in->status = (input_status)v;
    in->run = getid(r, "run");
    in->task = getid(r, "task");
    in->yes = pt_str_eqz(pt_json_gets(r, "answer", &a->scratch), "yes");
    in->note = pt_str_eqz(pt_json_gets(r, "from", &a->scratch), "pytte");
    in->steer = pt_str_eqz(pt_json_gets(r, "when_busy", &a->scratch), "steer");
    return true;
oom:
    pt_core_fail(a, PT_FATAL_OOM, "out of memory", PT_STR_NULL);
    return false;
}

/* The epoch's tools: parsed once into the epoch arena, so building a
 * request or checking a call needs no parsing. */
static bool apply_tools(pt_agent *a, const pt_json *list)
{
    pt_arena *ea = &a->epoch;
    const pt_json *t;
    uint32_t n = 0;
    a->tools = NULL;
    a->tool_meta = NULL;
    a->ntools = 0;
    if (!pt_json_is(list, PT_J_ARR))
        return list == NULL;
    if (!list->len)
        return true;
    a->tools = (pt_tool *)pt_arena_zalloc(ea, sizeof(pt_tool) * list->len);
    a->tool_meta = (tool_meta *)pt_arena_zalloc(ea, sizeof(tool_meta) * list->len);
    if (!a->tools || !a->tool_meta)
        return false;
    for (t = list->child; t; t = t->next) {
        const pt_json *schema = pt_json_get(t, "input_schema");
        pt_tool *d = &a->tools[n];
        tool_meta *m = &a->tool_meta[n];
        if (!pt_json_is(t, PT_J_OBJ))
            return false;
        d->name = pt_arena_dup(ea, pt_json_gets(t, "name", &a->scratch));
        d->description = pt_arena_dup(ea, pt_json_gets(t, "description", &a->scratch));
        d->schema = pt_arena_dup(ea, schema ? schema->raw : PT_S("{\"type\":\"object\"}"));
        m->replay = pt_str_eqz(pt_json_gets(t, "replay", &a->scratch), "safe") ? PT_REPLAY_SAFE
                                                                              : PT_REPLAY_UNSAFE;
        m->parallel = pt_json_truthy(pt_json_get(t, "parallel"));
        m->read_only = pt_json_truthy(pt_json_get(t, "read_only"));
        m->snapshot = pt_json_truthy(pt_json_get(t, "snapshot"));
        m->schema = pt_json_parse(d->schema, ea, NULL);
        if (!d->name.n || (schema && !m->schema))
            return false;
        n++;
    }
    a->ntools = n;
    return !ea->oom;
}

static bool apply_doc(pt_agent *a, const pt_json *r, uint32_t id)
{
    pt_str key = pt_json_gets(r, "key", &a->scratch);
    const pt_json *v = pt_json_get(r, "value");
    doc *d = NULL;
    uint32_t i;
    if (!key.n || !v)
        return false;
    for (i = 0; i < a->ndocs; i++)
        if (pt_str_eq(a->docs[i].key, key)) {
            d = &a->docs[i];
            break;
        }
    if (!d) {
        if (!pt_core_grow(a, (void **)&a->docs, &a->dcap, a->ndocs + 1, sizeof(doc)))
            goto oom;
        d = &a->docs[a->ndocs];
        d->key = pt_arena_dup(&a->perm, key);
        pt_buf_init(&d->value, a->al);
        a->ndocs++;
    }
    d->id = id;
    pt_buf_reset(&d->value);
    pt_buf_puts(&d->value, v->raw);
    if (!d->value.oom && !a->perm.oom)
        return true;
oom:
    pt_core_fail(a, PT_FATAL_OOM, "out of memory", PT_STR_NULL);
    return false;
}

/* An epoch's "files" ({"read": [...], "modified": [...]}) as the text that
 * follows its summary. */
static pt_str files_text(const pt_json *f, pt_arena *ea, pt_arena *sa)
{
    static const char *const keys[] = {"read", "modified"};
    static const char *const heads[] = {"Files read:", "Files modified:"};
    const pt_json *c;
    size_t n = 0;
    char *p, *q;
    unsigned k;
    for (k = 0; k < 2; k++)
        for (c = pt_json_is(pt_json_get(f, keys[k]), PT_J_ARR) ? pt_json_get(f, keys[k])->child : NULL; c;
             c = c->next)
            n += 3 + pt_json_str(c, sa).n;
    if (!n)
        return PT_STR_NULL;
    n += 64;
    if (!(p = q = (char *)pt_arena_alloc(ea, n)))
        return PT_STR_NULL;
    for (k = 0; k < 2; k++) {
        const pt_json *arr = pt_json_get(f, keys[k]);
        if (!pt_json_is(arr, PT_J_ARR) || !arr->child)
            continue;
        if (q > p)
            *q++ = '\n';
        memcpy(q, heads[k], strlen(heads[k]));
        q += strlen(heads[k]);
        for (c = arr->child; c; c = c->next) {
            pt_str s = pt_json_str(c, sa);
            memcpy(q, "\n- ", 3);
            q += 3;
            memcpy(q, s.p, s.n);
            q += s.n;
        }
        *q++ = '\n';
    }
    return (pt_str){p, (size_t)(q - p)};
}

static bool apply_entry(pt_agent *a, const pt_json *r, uint32_t id)
{
    pt_arena *ea = &a->epoch, *sa = &a->scratch;
    const pt_json *d = pt_json_get(r, "data");
    unsigned k;
    entry *e;
    if (!lookup(pt_json_gets(r, "kind", sa), ent_names, 6, &k))
        return false;
    if (a->nentries && id <= a->entries[a->nentries - 1].id) {
        pt_core_seterr(a, "corrupt log: entry IDs are not strictly increasing", PT_STR_NULL);
        return false;
    }
    if (k == ENT_EPOCH) {
        /* A new epoch: older entries leave memory (they stay in the log). */
        if (!a->o.all_epochs) {
            a->nentries = 0;
            pt_arena_reset(ea);
        }
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
    e->kept = getid(r, "kept");
    e->raw = pt_arena_dup(ea, r->raw);
    switch (e->kind) {
    case ENT_EPOCH:
        e->msg.role = PT_ROLE_SYSTEM;
        e->msg.text = pt_arena_dup(ea, pt_json_gets(d, "system", sa));
        e->summary = pt_arena_dup(ea, pt_json_gets(d, "summary", sa));
        e->request = pt_arena_dup(ea, pt_json_gets(d, "request", sa));
        e->files = files_text(pt_json_get(d, "files"), ea, sa);
        e->provisional = pt_json_truthy(pt_json_get(d, "provisional"));
        if (!apply_tools(a, pt_json_get(d, "tools")))
            return false;
        break;
    case ENT_USER: {
        const pt_json *c = pt_json_get(d, "content");
        e->msg.role = PT_ROLE_USER;
        if (pt_json_is(c, PT_J_OBJ)) { /* a blob: its input has the text */
            pt_str ref = pt_json_gets(c, "blob", sa);
            input *in = pt_core_input(a, getid(d, "input"));
            if (in && pt_str_eq(in->blob, ref))
                e->msg.text = pt_arena_dup(ea, in->text);
            else if (!blob_text(a, ref, ea, &e->msg.text))
                return false;
        } else {
            e->msg.text = pt_arena_dup(ea, pt_json_str(c, sa));
        }
        if (pt_json_is(pt_json_get(d, "images"), PT_J_ARR)) { /* one message each, bytes from blobs */
            const pt_json *list = pt_json_get(d, "images"), *im;
            uint32_t n = 0, i;
            while (pt_json_at(list, n))
                n++;
            e->imgs = (pt_msg *)pt_arena_zalloc(ea, sizeof(pt_msg) * (n ? n : 1));
            if (!e->imgs)
                goto oom;
            for (i = 0; i < n; i++) {
                pt_msg *m = &e->imgs[i];
                pt_str name = pt_json_gets(pt_json_at(list, i), "name", sa);
                static const char lead[] = "[image "; /* PT_INPUT_IMAGES is a digit */
                size_t lead_n = sizeof lead - 1, at = lead_n + 3;
                char *p = (char *)pt_arena_alloc(ea, at + name.n + 1);
                im = pt_json_at(list, i);
                if (!p)
                    goto oom;
                memcpy(p, lead, lead_n);
                p[lead_n] = (char)('1' + i % 9);
                p[lead_n + 1] = ':';
                p[lead_n + 2] = ' ';
                if (name.n)
                    memcpy(p + at, name.p, name.n);
                p[at + name.n] = ']';
                m->role = PT_ROLE_USER;
                m->text = (pt_str){p, at + name.n + 1};
                m->image_mime = pt_arena_dup(ea, pt_json_gets(im, "mime", sa));
                if (!blob_text(a, pt_json_gets(im, "blob", sa), ea, &m->image))
                    return false;
            }
            e->nimgs = n;
        }
        break;
    }
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
        if (pt_json_is(pt_json_get(d, "image"), PT_J_OBJ)) { /* its bytes are a blob */
            const pt_json *im = pt_json_get(d, "image");
            e->msg.image_mime = pt_arena_dup(ea, pt_json_gets(im, "mime", sa));
            if (!blob_text(a, pt_json_gets(im, "blob", sa), ea, &e->msg.image))
                return false;
        }
        break;
    case ENT_NOTE:
        e->msg.role = PT_ROLE_SYSTEM;
        e->msg.text = pt_arena_dup(ea, pt_json_gets(d, "text", sa));
        e->to_model = pt_json_truthy(pt_json_get(d, "to_model"));
        break;
    case ENT_ERROR:
        break;
    }
    if (ea->oom)
        goto oom;
    a->nentries++;
    return true;
oom:
    pt_core_fail(a, PT_FATAL_OOM, "out of memory", PT_STR_NULL);
    return false;
}

/* The settled materials of a provisional epoch: they replace its entry's
 * system prompt, tools and skills, as if the entry had always held them. */
static bool apply_materials(pt_agent *a, const pt_json *r)
{
    pt_arena *ea = &a->epoch, *sa = &a->scratch;
    const pt_json *d = pt_json_get(r, "data");
    entry *e = a->nentries ? &a->entries[0] : NULL;
    pt_buf raw;
    pt_jw w;
    if (!pt_json_is(d, PT_J_OBJ) || (uint32_t)pt_json_geti(r, "epoch", 0) != a->cur_epoch || !e ||
        e->kind != ENT_EPOCH)
        return true; /* an older epoch's: already replaced */
    if (!apply_tools(a, pt_json_get(d, "tools")))
        return false;
    pt_buf_init(&raw, a->al);
    pt_jw_init(&w, &raw);
    pt_jw_obj(&w);
    pt_jw_key(&w, "t");
    pt_jw_strz(&w, "entry");
    pt_jw_key(&w, "id");
    pt_jw_uint(&w, e->id);
    pt_jw_key(&w, "kind");
    pt_jw_strz(&w, "epoch");
    pt_jw_key(&w, "epoch");
    pt_jw_uint(&w, a->cur_epoch);
    pt_jw_key(&w, "data");
    pt_jw_raw(&w, d->raw);
    pt_jw_obj_end(&w);
    e->raw = pt_arena_dup(ea, pt_buf_str(&raw));
    e->msg.text = pt_arena_dup(ea, pt_json_gets(d, "system", sa));
    e->provisional = false;
    if (raw.oom || ea->oom) {
        pt_buf_free(&raw);
        pt_core_fail(a, PT_FATAL_OOM, "out of memory", PT_STR_NULL);
        return false;
    }
    pt_buf_free(&raw);
    return true;
}

static void apply_claim(pt_agent *a, const pt_json *r, uint32_t id)
{
    pt_arena *pa = &a->perm, *sa = &a->scratch;
    claim *c = &a->claims.newest;
    uint32_t i;
    c->id = id;
    c->event = pt_arena_dup(pa, pt_json_gets(r, "event", sa));
    c->token = pt_arena_dup(pa, pt_json_gets(r, "token", sa));
    c->host = pt_arena_dup(pa, pt_json_gets(r, "host", sa));
    c->boot = pt_arena_dup(pa, pt_json_gets(r, "boot", sa));
    c->pid = pt_json_geti(r, "pid", 0);
    c->opened = pt_json_geti(r, "opened", 0);
    /* How each tool call cut off so far ended: by the first claim after
     * its intent (a task record resets both). */
    for (i = 0; i < a->ntasks; i++) {
        task *t = a->tasks[i];
        if (t->kind != TASK_TOOL || t->phase != PH_EXECUTE || t->reopened)
            continue;
        if (pt_str_eqz(c->event, "close"))
            t->quit = true;
        else if (pt_str_eqz(c->event, "open"))
            t->reopened = true;
    }
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
        a->cwd = pt_arena_dup(&a->perm, pt_json_gets(r, "cwd", &a->scratch));
        if (pt_json_is(pt_json_get(r, "prompt"), PT_J_OBJ))
            a->prompt = pt_arena_dup(&a->perm, pt_json_get(r, "prompt")->raw);
        /* the model it was created with, until a run pins one */
        if (!a->last_model.n)
            a->last_model = pt_arena_dup(&a->perm, pt_json_gets(r, "model", &a->scratch));
    } else if (pt_str_eqz(type, "entry")) {
        ok = apply_entry(a, r, id);
    } else if (pt_str_eqz(type, "task")) {
        ok = apply_task(a, r, id);
    } else if (pt_str_eqz(type, "input")) {
        ok = apply_input(a, r, id);
    } else if (pt_str_eqz(type, "claim")) {
        apply_claim(a, r, id);
    } else if (pt_str_eqz(type, "doc")) {
        ok = apply_doc(a, r, id);
    } else if (pt_str_eqz(type, "materials")) {
        ok = apply_materials(a, r);
    }
    /* Other types are not needed here. */
    if (ok && id >= a->next_id) {
        if (id == UINT32_MAX) {
            pt_core_fail(a, PT_FATAL_OOM, "record ID space exhausted", PT_STR_NULL);
            return false;
        }
        a->next_id = id + 1;
    }
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
