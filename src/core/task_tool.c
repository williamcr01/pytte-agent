/* The tool task: one tool call of a run (section 8.2).
 *
 *   call ──► (gate) ──► execute ──► done
 *     │        │ ask       ▲   recovery: safe → run again (attempt + 1)
 *     │        ▼           │             unsafe → an "interrupted" result
 *     │      approve ──────┘
 *     ▼        │ no
 *   error result ◄┘
 *
 * call checks the arguments against the epoch's frozen schema, then asks
 * the host's gate. The intent to execute is committed before the effect
 * starts; the result, its entry and the task's end are one commit. */
#include "core/core.h"

#include <string.h>

#define TAIL 4096 /* output kept for the live file and an interrupted result */

struct tool_live {
    uint32_t fx;   /* the TOOL effect */
    bool finished; /* pt_fx_tool arrived */
    pt_buf tail;   /* the newest output: at most 2 * TAIL bytes, at least the last TAIL */
    int64_t last_live;
    bool dirty, wrote_live;
    pt_buf snap;
    /* the result, copied: content, details, doc key, doc value, image, its type */
    pt_buf result;
    pt_arena fx_arena; /* fx payload survives durable task-record replacement */
    size_t at[6], n[6];
    bool is_error;
};

static const char aborted_text[] = "The user aborted this tool call.";
static const char interrupted_text[] =
    "This tool call was interrupted by a crash. It may or may not have taken effect. Check the "
    "current state before retrying.";

void pt_tool_free_live(pt_agent *a, task *t)
{
    tool_live *tl = t->tl;
    if (!tl)
        return;
    pt_buf_free(&tl->tail);
    pt_buf_free(&tl->snap);
    pt_buf_free(&tl->result);
    pt_arena_free(&tl->fx_arena);
    a->al.fn(a->al.ud, tl, sizeof *tl, 0);
    t->tl = NULL;
}

/* ---- lookups ---- */

static const pt_block *find_call(pt_agent *a, const task *t)
{
    entry *e = pt_core_entry(a, t->entry);
    uint32_t i;
    if (!e || e->kind != ENT_ASSISTANT)
        return NULL;
    for (i = 0; i < e->msg.nblocks; i++)
        if (e->msg.blocks[i].kind == PT_BLOCK_TOOL_CALL && pt_str_eq(e->msg.blocks[i].id, t->call_id))
            return &e->msg.blocks[i];
    return NULL;
}

int pt_core_find_tool(pt_agent *a, pt_str name)
{
    uint32_t i;
    for (i = 0; i < a->ntools; i++)
        if (pt_str_eq(a->tools[i].name, name))
            return (int)i;
    return -1;
}

static pt_str call_args(const task *t, const pt_block *call)
{
    return t->args.n ? t->args : call->args;
}

/* The output kept in a live file before a crash, or in this process. */
static pt_str tail_of(pt_agent *a, task *t)
{
    pt_str s = PT_STR_NULL;
    if (t->tl) {
        s = pt_buf_str(&t->tl->tail);
        if (s.n > TAIL) {
            s.p += s.n - TAIL;
            s.n = TAIL;
        }
    } else if (t->rescued.n) {
        pt_json *v = pt_json_parse(pt_buf_str(&t->rescued), &a->scratch, NULL);
        s = pt_json_gets(v, "output", &a->scratch);
    }
    while (s.n && ((unsigned char)*s.p & 0xC0) == 0x80) { /* start on a character */
        s.p++;
        s.n--;
    }
    return s;
}

/* ---- the end of a call ---- */

typedef struct ending {
    pt_str content, details, doc_key, doc_value, image, image_mime;
    bool is_error, interrupted, aborted;
    const input *consumed; /* an approve answer this commit uses up */
} ending;

static bool valid_json(pt_agent *a, pt_str s, bool object)
{
    pt_json *v = s.n ? pt_json_parse(s, &a->scratch, NULL) : NULL;
    return v && (!object || v->type == PT_J_OBJ);
}

/* One commit: the tool_result entry, an optional doc, the task done. */
static bool finish(pt_agent *a, task *t, const pt_block *call, const ending *e)
{
    pt_jw *w = &a->commit.w;
    uint32_t id;
    task nt = *t;
    bool ok;
    pt_core_commit_begin(a, t->id);
    if (e->consumed) {
        input in = *e->consumed;
        in.status = IN_DONE;
        pt_write_input(w, &in);
    }
    id = pt_core_new_id(a);
    pt_core_entry_begin(a, id, "tool_result");
    pt_jw_key(w, "call_id");
    pt_jw_str(w, t->call_id);
    pt_jw_key(w, "name");
    pt_jw_str(w, call ? call->name : PT_STR_NULL);
    pt_jw_key(w, "content");
    pt_jw_str(w, e->content);
    pt_jw_key(w, "is_error");
    pt_jw_bool(w, e->is_error);
    if (e->details.n && valid_json(a, e->details, true)) {
        pt_jw_key(w, "details");
        pt_jw_raw(w, e->details);
    }
    if (e->interrupted) {
        pt_jw_key(w, "interrupted");
        pt_jw_bool(w, true);
    }
    if (e->image.n && e->image_mime.n) {
        pt_jw_key(w, "image");
        pt_jw_obj(w);
        pt_jw_key(w, "blob");
        pt_jw_str(w, e->image);
        pt_jw_key(w, "mime");
        pt_jw_str(w, e->image_mime);
        pt_jw_obj_end(w);
    }
    pt_core_entry_end(a);
    if (e->doc_key.n && valid_json(a, e->doc_value, false))
        pt_core_write_doc(a, e->doc_key, e->doc_value);
    nt.status = ST_DONE;
    nt.outcome = e->aborted ? PT_S("aborted") : e->is_error ? PT_S("failed") : PT_S("completed");
    nt.reason = PT_STR_NULL;
    nt.result = id;
    pt_write_task(w, &nt);
    if (t->phase == PH_EXECUTE && ((t->tl && t->tl->wrote_live) || t->recovered))
        a->commit.live_del = t->id;
    ok = pt_core_commit_end(a);
    pt_buf_reset(&t->rescued);
    pt_tool_free_live(a, t);
    return ok;
}

static bool finish_text(pt_agent *a, task *t, const pt_block *call, const char *text, pt_str detail)
{
    ending e;
    pt_buf b;
    bool ok;
    pt_buf_init(&b, a->al);
    pt_buf_putz(&b, text);
    pt_buf_puts(&b, detail);
    memset(&e, 0, sizeof e);
    e.content = pt_buf_str(&b);
    e.is_error = true;
    ok = !b.oom && finish(a, t, call, &e);
    if (b.oom)
        pt_core_fail(a, PT_FATAL_OOM, "out of memory", PT_STR_NULL);
    pt_buf_free(&b);
    return ok;
}

/* Aborted, or interrupted by a crash: the text and the output so far. */
static bool finish_cut(pt_agent *a, task *t, const pt_block *call, bool aborted, const input *consumed)
{
    ending e;
    pt_buf b;
    pt_str tail = tail_of(a, t);
    bool ok;
    pt_buf_init(&b, a->al);
    pt_buf_putz(&b, aborted ? aborted_text : interrupted_text);
    if (tail.n) {
        pt_buf_putz(&b, aborted ? "\n\nOutput before the abort:\n" : "\n\nThe last output before the crash:\n");
        pt_buf_puts(&b, tail);
    }
    memset(&e, 0, sizeof e);
    e.content = pt_buf_str(&b);
    e.is_error = true;
    e.aborted = aborted;
    e.interrupted = !aborted;
    e.consumed = consumed;
    ok = !b.oom && finish(a, t, call, &e);
    if (b.oom)
        pt_core_fail(a, PT_FATAL_OOM, "out of memory", PT_STR_NULL);
    pt_buf_free(&b);
    return ok;
}

/* ---- call: check, then ask the gate ---- */

static bool check_args(pt_agent *a, task *t, const pt_block *call, int k, pt_str args)
{
    pt_json_err jerr;
    pt_json *v = pt_json_parse(args, &a->scratch, &jerr);
    char err[300];
    if (!v) {
        pt_buf b;
        bool ok;
        pt_buf_init(&b, a->al);
        pt_buf_putz(&b, "The arguments are not valid JSON (");
        pt_buf_putz(&b, jerr.msg ? jerr.msg : "parse error");
        pt_buf_putz(&b, " at byte ");
        pt_buf_u64(&b, jerr.pos);
        pt_buf_putz(&b, "). Send a complete JSON object; split large content into smaller calls.");
        ok = finish_text(a, t, call, "", pt_buf_str(&b));
        pt_buf_free(&b);
        (void)ok;
        return false;
    }
    if (v->type != PT_J_OBJ) {
        finish_text(a, t, call, "The arguments must be a JSON object.", PT_STR_NULL);
        return false;
    }
    if (a->tool_meta[k].schema && !pt_schema_check(a->tool_meta[k].schema, v, &a->scratch, err, sizeof err)) {
        finish_text(a, t, call, "Invalid arguments: ", pt_strz(err));
        return false;
    }
    return true;
}

static bool call_phase(pt_agent *a, task *t, const pt_block *call)
{
    int k = pt_core_find_tool(a, call->name);
    pt_fx fx;
    if (k < 0) {
        pt_buf b;
        uint32_t i;
        bool ok;
        pt_buf_init(&b, a->al);
        pt_buf_putz(&b, "Unknown tool: ");
        pt_buf_puts(&b, call->name);
        pt_buf_putz(&b, a->ntools ? ". The tools are: " : ". No tools are available.");
        for (i = 0; i < a->ntools; i++) {
            if (i)
                pt_buf_putz(&b, ", ");
            pt_buf_puts(&b, a->tools[i].name);
        }
        ok = finish_text(a, t, call, "", pt_buf_str(&b));
        pt_buf_free(&b);
        return ok;
    }
    if (!check_args(a, t, call, k, call_args(t, call)))
        return !a->fatal; /* the error result was queued */
    memset(&fx, 0, sizeof fx);
    fx.kind = PT_FX_GATE;
    fx.id = pt_core_new_fx(a);
    fx.task = t->id;
    fx.name = call->name;
    fx.args = call_args(t, call);
    fx.call_id = t->call_id;
    fx.replay = a->tool_meta[k].replay;
    t->fx = fx.id;
    return pt_core_queue(a, &fx, PT_STR_NULL);
}

static bool commit_phase(pt_agent *a, task *t, task_phase ph, pt_str args, pt_str prompt, uint32_t attempt,
                         const input *consumed)
{
    task nt = *t;
    int k;
    pt_core_commit_begin(a, t->id);
    if (consumed) {
        input in = *consumed;
        in.status = IN_DONE;
        pt_write_input(&a->commit.w, &in);
    }
    nt.phase = ph;
    nt.status = ph == PH_APPROVE ? ST_WAITING : ST_RUNNING;
    nt.args = args;
    nt.prompt = prompt;
    nt.attempt = attempt;
    if (ph == PH_EXECUTE && attempt == 1) { /* the policy at the time of the intent */
        const pt_block *call = find_call(a, t);
        k = call ? pt_core_find_tool(a, call->name) : -1;
        nt.replay = k >= 0 ? a->tool_meta[k].replay : PT_REPLAY_UNSAFE;
    }
    pt_write_task(&a->commit.w, &nt);
    return pt_core_commit_end(a);
}

static bool gated(pt_agent *a, task *t, const pt_block *call)
{
    pt_str text = pt_buf_str(&t->gate_text);
    int k = pt_core_find_tool(a, call->name);
    t->gated = false;
    switch (t->gate) {
    case PT_GATE_DENY:
        return finish_text(a, t, call, text.n ? "Permission denied: " : "Permission denied.", text);
    case PT_GATE_ASK:
        return commit_phase(a, t, PH_APPROVE, t->args, text, 0, NULL);
    case PT_GATE_REWRITE:
        if (k < 0 || !check_args(a, t, call, k, text))
            return !a->fatal;
        return commit_phase(a, t, PH_EXECUTE, text, PT_STR_NULL, 1, NULL);
    case PT_GATE_ALLOW:
        break;
    }
    return commit_phase(a, t, PH_EXECUTE, t->args, PT_STR_NULL, 1, NULL);
}

void pt_fx_gate(pt_agent *a, uint32_t fx, const pt_gate *g)
{
    uint32_t i;
    if (a->fatal)
        return;
    for (i = 0; i < a->ntasks; i++) {
        task *t = a->tasks[i];
        if (t->kind == TASK_TOOL && t->fx == fx && t->phase == PH_CALL) {
            t->fx = 0;
            t->gated = true;
            t->gate = g->kind;
            pt_buf_reset(&t->gate_text);
            pt_buf_puts(&t->gate_text, g->kind == PT_GATE_REWRITE ? g->args : g->text);
            if (t->gate_text.oom)
                pt_core_fail(a, PT_FATAL_OOM, "out of memory", PT_STR_NULL);
            a->idle_sent = false;
            return;
        }
    }
}

/* ---- approve ---- */

static const input *answer_for(pt_agent *a, uint32_t task_id)
{
    uint32_t i;
    for (i = 0; i < a->ninputs; i++) {
        const input *in = a->inputs[i];
        if (in->kind == INK_APPROVE && in->task == task_id && in->status == IN_QUEUED)
            return in;
    }
    return NULL;
}

static bool approve_phase(pt_agent *a, task *t, const pt_block *call)
{
    const input *in = answer_for(a, t->id);
    pt_fx fx;
    pt_event ev;
    if (in) {
        if (in->yes)
            return commit_phase(a, t, PH_EXECUTE, t->args, PT_STR_NULL, 1, in);
        {
            ending e;
            pt_buf b;
            bool ok;
            pt_buf_init(&b, a->al);
            pt_buf_putz(&b, "The user denied this tool call");
            if (in->text.n) {
                pt_buf_putz(&b, ": ");
                pt_buf_puts(&b, in->text);
            }
            pt_buf_putc(&b, '.');
            memset(&e, 0, sizeof e);
            e.content = pt_buf_str(&b);
            e.is_error = true;
            e.consumed = in;
            ok = finish(a, t, call, &e);
            pt_buf_free(&b);
            return ok;
        }
    }
    if (t->asked)
        return false;
    t->asked = true;
    memset(&fx, 0, sizeof fx);
    fx.kind = PT_FX_ASK;
    fx.id = pt_core_new_fx(a);
    fx.task = t->id;
    fx.name = call->name;
    fx.args = call_args(t, call);
    fx.call_id = t->call_id;
    fx.prompt = t->prompt;
    memset(&ev, 0, sizeof ev);
    ev.kind = PT_E_ASK;
    ev.task = t->id;
    ev.name = fx.name;
    ev.args = fx.args;
    ev.call_id = fx.call_id;
    ev.prompt = fx.prompt;
    pt_core_emit(a, &ev);
    return pt_core_queue(a, &fx, PT_STR_NULL);
}

bool pt_approve(pt_agent *a, uint32_t task_id, bool yes, pt_str reason)
{
    task *t = pt_core_task(a, task_id);
    uint32_t i;
    if (a->fatal || !t || t->kind != TASK_TOOL || t->phase != PH_APPROVE || t->status == ST_DONE) {
        pt_core_seterr(a, "that tool call is not waiting for approval", PT_STR_NULL);
        return false;
    }
    if (answer_for(a, task_id))
        return true;
    for (i = 0; i < a->subs.n; i++)
        if (a->subs.v[i].kind == INK_APPROVE && a->subs.v[i].task == task_id)
            return true;
    if (!pt_core_grow(a, (void **)&a->subs.v, &a->subs.cap, a->subs.n + 1, sizeof(sub)))
        return false;
    {
        sub pending;
        memset(&pending, 0, sizeof pending);
        pending.kind = INK_APPROVE;
        pending.task = task_id;
        pending.yes = yes;
        pending.text = pt_arena_dup(&a->subs.ar, reason);
        if (a->subs.ar.oom) {
            pt_core_fail(a, PT_FATAL_OOM, "out of memory queueing approval", PT_STR_NULL);
            return false;
        }
        a->subs.v[a->subs.n++] = pending;
    }
    a->idle_sent = false;
    return true;
}

uint32_t pt_asks(const pt_agent *ca, pt_ask_view *out, uint32_t max)
{
    pt_agent *a = (pt_agent *)ca; /* lookups only */
    uint32_t i, n = 0;
    for (i = 0; i < a->ntasks; i++) {
        task *t = a->tasks[i];
        const pt_block *call;
        if (t->kind != TASK_TOOL || t->phase != PH_APPROVE || t->status == ST_DONE || answer_for(a, t->id))
            continue;
        if (n < max && (call = find_call(a, t))) {
            out[n].task = t->id;
            out[n].name = call->name;
            out[n].args = call_args(t, call);
            out[n].prompt = t->prompt;
        }
        n++;
    }
    return n;
}

/* ---- execute ---- */

static bool snapshot(pt_agent *a, task *t, bool force)
{
    tool_live *tl = t->tl;
    int64_t now = pt_core_now(a);
    uint32_t every = a->o.config.live_interval_ms ? a->o.config.live_interval_ms : 250;
    pt_jw w;
    pt_fx fx;
    if (!force && tl->last_live && now - tl->last_live < (int64_t)every) {
        tl->dirty = true;
        return false;
    }
    tl->last_live = now ? now : 1;
    tl->dirty = false;
    tl->wrote_live = true;
    pt_buf_reset(&tl->snap);
    pt_jw_init(&w, &tl->snap);
    pt_jw_obj(&w);
    pt_jw_key(&w, "output");
    pt_jw_str(&w, tail_of(a, t));
    pt_jw_obj_end(&w);
    memset(&fx, 0, sizeof fx);
    fx.kind = PT_FX_LIVE;
    fx.id = pt_core_new_fx(a);
    fx.task = t->id;
    if (tl->snap.oom) {
        pt_core_fail(a, PT_FATAL_OOM, "out of memory building tool snapshot", PT_STR_NULL);
        return false;
    }
    return pt_core_queue(a, &fx, pt_buf_str(&tl->snap));
}

static bool start(pt_agent *a, task *t, const pt_block *call)
{
    tool_live *tl = (tool_live *)a->al.fn(a->al.ud, NULL, 0, sizeof *tl);
    pt_fx fx;
    pt_event ev;
    if (!tl) {
        pt_core_fail(a, PT_FATAL_OOM, "out of memory", PT_STR_NULL);
        return false;
    }
    memset(tl, 0, sizeof *tl);
    pt_buf_init(&tl->tail, a->al);
    pt_buf_init(&tl->snap, a->al);
    pt_buf_init(&tl->result, a->al);
    pt_arena_init(&tl->fx_arena, a->al, 128);
    t->tl = tl;
    memset(&fx, 0, sizeof fx);
    fx.kind = PT_FX_TOOL;
    fx.id = pt_core_new_fx(a);
    fx.task = t->id;
    fx.name = call->name;
    fx.args = pt_arena_dup(&tl->fx_arena, call_args(t, call));
    if (tl->fx_arena.oom) {
        pt_core_fail(a, PT_FATAL_OOM, "out of memory retaining tool arguments", PT_STR_NULL);
        return false;
    }
    fx.call_id = t->call_id;
    fx.attempt = t->attempt;
    fx.replay = t->replay;
    tl->fx = fx.id;
    memset(&ev, 0, sizeof ev);
    ev.kind = PT_E_TOOL_START;
    ev.task = t->id;
    ev.name = fx.name;
    ev.args = fx.args;
    ev.call_id = fx.call_id;
    pt_core_emit(a, &ev);
    return pt_core_queue(a, &fx, PT_STR_NULL);
}

static task *find_running(pt_agent *a, uint32_t fx)
{
    uint32_t i;
    for (i = 0; i < a->ntasks; i++)
        if (a->tasks[i]->tl && a->tasks[i]->tl->fx == fx && !a->tasks[i]->tl->finished)
            return a->tasks[i];
    return NULL;
}

bool pt_tool_data(pt_agent *a, uint32_t fx, const void *p, size_t n)
{
    task *t = find_running(a, fx);
    tool_live *tl;
    pt_event ev;
    if (!t)
        return false;
    tl = t->tl;
    if (n >= TAIL) { /* only the newest TAIL bytes matter */
        pt_buf_reset(&tl->tail);
        pt_buf_put(&tl->tail, (const char *)p + n - TAIL, TAIL);
    } else {
        if (tl->tail.n + n > 2 * TAIL) {
            size_t keep = TAIL - n;
            memmove(tl->tail.p, tl->tail.p + tl->tail.n - keep, keep);
            tl->tail.n = keep;
        }
        pt_buf_put(&tl->tail, p, n);
    }
    if (tl->tail.oom) {
        pt_core_fail(a, PT_FATAL_OOM, "out of memory retaining tool output", PT_STR_NULL);
        return false;
    }
    memset(&ev, 0, sizeof ev);
    ev.kind = PT_E_TOOL_OUTPUT;
    ev.task = t->id;
    ev.data = (pt_str){(const char *)p, n};
    pt_core_emit(a, &ev);
    snapshot(a, t, false);
    return true;
}

void pt_fx_tool(pt_agent *a, uint32_t fx, const pt_tool_result *r)
{
    task *t;
    tool_live *tl;
    const pt_str parts[6] = {r->content, r->details, r->doc_key, r->doc_value, r->image, r->image_mime};
    size_t total = 0;
    int i;
    if (a->fatal || !(t = find_running(a, fx)))
        return;
    tl = t->tl;
    for (i = 0; i < 6; i++) {
        if (parts[i].n > SIZE_MAX - total) {
            pt_core_fail(a, PT_FATAL_OOM, "tool result is too large", PT_STR_NULL);
            return;
        }
        total += parts[i].n;
    }
    pt_buf_reset(&tl->result);
    if (!pt_buf_reserve(&tl->result, total)) {
        pt_core_fail(a, PT_FATAL_OOM, "out of memory", PT_STR_NULL);
        return;
    }
    for (i = 0; i < 6; i++) {
        tl->at[i] = tl->result.n;
        tl->n[i] = parts[i].n;
        pt_buf_puts(&tl->result, parts[i]);
    }
    if (tl->result.oom) {
        pt_core_fail(a, PT_FATAL_OOM, "out of memory copying tool result", PT_STR_NULL);
        return;
    }
    tl->is_error = r->is_error;
    tl->finished = true;
    a->idle_sent = false;
}

static bool commit_result(pt_agent *a, task *t, const pt_block *call, bool aborted)
{
    tool_live *tl = t->tl;
    ending e;
    pt_buf b;
    bool ok;
    memset(&e, 0, sizeof e);
    e.content = tl->n[0] ? (pt_str){tl->result.p + tl->at[0], tl->n[0]} : PT_STR_NULL;
    e.details = tl->n[1] ? (pt_str){tl->result.p + tl->at[1], tl->n[1]} : PT_STR_NULL;
    e.doc_key = tl->n[2] ? (pt_str){tl->result.p + tl->at[2], tl->n[2]} : PT_STR_NULL;
    e.doc_value = tl->n[3] ? (pt_str){tl->result.p + tl->at[3], tl->n[3]} : PT_STR_NULL;
    e.is_error = tl->is_error;
    if (!aborted) {
        e.image = tl->n[4] ? (pt_str){tl->result.p + tl->at[4], tl->n[4]} : PT_STR_NULL;
        e.image_mime = tl->n[5] ? (pt_str){tl->result.p + tl->at[5], tl->n[5]} : PT_STR_NULL;
        return finish(a, t, call, &e);
    }
    pt_buf_init(&b, a->al);
    pt_buf_putz(&b, aborted_text);
    if (e.content.n) {
        pt_buf_putz(&b, "\n\nOutput before the abort:\n");
        pt_buf_puts(&b, e.content);
    }
    e.content = pt_buf_str(&b);
    e.is_error = true;
    e.aborted = true;
    ok = !b.oom && finish(a, t, call, &e);
    pt_buf_free(&b);
    return ok;
}

static bool execute_phase(pt_agent *a, task *t, const pt_block *call, bool aborted)
{
    int k;
    if (t->tl) {
        if (t->tl->finished)
            return commit_result(a, t, call, aborted);
        if (t->tl->dirty) /* the trailing snapshot, once its time has come */
            return snapshot(a, t, false);
        return false;
    }
    if (aborted)
        return finish_cut(a, t, call, true, NULL);
    if (t->recovered) {
        /* The process died after the intent: run it again only if both the
         * stored and the current policy say that is harmless. */
        k = pt_core_find_tool(a, call->name);
        if (t->replay == PT_REPLAY_SAFE && k >= 0 && a->tool_meta[k].replay == PT_REPLAY_SAFE)
            return commit_phase(a, t, PH_EXECUTE, t->args, PT_STR_NULL, t->attempt + 1, NULL);
        return finish_cut(a, t, call, false, NULL);
    }
    return start(a, t, call);
}

bool pt_tool_step(pt_agent *a, task *t)
{
    task *run = pt_core_task(a, t->owner);
    bool aborted = t->abort || (run && run->abort);
    const pt_block *call = find_call(a, t);
    if (!call)
        return finish_text(a, t, NULL, "The tool call is missing from the transcript.", PT_STR_NULL);
    switch (t->phase) {
    case PH_CALL:
        if (aborted)
            return finish_cut(a, t, call, true, NULL);
        return t->gated ? gated(a, t, call) : call_phase(a, t, call);
    case PH_APPROVE:
        if (aborted)
            return finish_cut(a, t, call, true, answer_for(a, t->id));
        return approve_phase(a, t, call);
    case PH_EXECUTE:
        return execute_phase(a, t, call, aborted);
    default:
        return false;
    }
}

/* Inside the run's abort commit. A call whose effect started keeps its
 * own commit (it may have done something; its result is recorded), marked
 * aborted; any other ends now. */
uint32_t pt_tool_abort(pt_agent *a, task *t)
{
    task nt = *t;
    const pt_block *call;
    pt_jw *w = &a->commit.w;
    uint32_t id;
    if (t->tl) {
        nt.abort = true;
        pt_write_task(w, &nt);
        return t->tl->finished ? 0 : t->tl->fx;
    }
    call = find_call(a, t);
    {
        pt_str tail = t->phase == PH_EXECUTE ? tail_of(a, t) : PT_STR_NULL;
        id = pt_core_new_id(a);
        pt_core_entry_begin(a, id, "tool_result");
        pt_jw_key(w, "call_id");
        pt_jw_str(w, t->call_id);
        pt_jw_key(w, "name");
        pt_jw_str(w, call ? call->name : PT_STR_NULL);
        pt_jw_key(w, "content");
        if (tail.n) {
            pt_buf *b = &t->gate_text; /* free scratch: the gate answered long ago */
            pt_buf_reset(b);
            pt_buf_putz(b, aborted_text);
            pt_buf_putz(b, "\n\nOutput before the abort:\n");
            pt_buf_puts(b, tail);
            pt_jw_str(w, pt_buf_str(b));
        } else {
            pt_jw_strz(w, aborted_text);
        }
        pt_jw_key(w, "is_error");
        pt_jw_bool(w, true);
        pt_core_entry_end(a);
    }
    nt.status = ST_DONE;
    nt.outcome = PT_S("aborted");
    nt.reason = PT_STR_NULL;
    nt.result = id;
    pt_write_task(w, &nt);
    return 0;
}

bool pt_tool_finished(const task *t)
{
    return t->tl && t->tl->finished;
}

uint32_t pt_tool_fx(const task *t)
{
    return t->tl ? t->tl->fx : 0;
}
