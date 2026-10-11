/* The compact task (sections 8.3 and 12.3): a summary of the current epoch
 * becomes the first user content of a new one, followed by copies of the
 * newest turns, kept as they are.
 *
 *   prepare ──► request ──► done: epoch n+1 (and the waiting run → prepare)
 *      ▲          │
 *      └── retry ◄┘ (sleep)
 *
 * The summarization request is the epoch written out as plain text, so it
 * needs no tools and fits whatever model summarizes it. Like a run's, it
 * is built from committed state only and is the same when sent again. */
#include "core/core.h"

#include <string.h>

#define CUT 2048            /* bytes kept of each tool call's input and result */
#define SUMMARY_TOKENS 8192 /* at most, for the summary */
#define KEEP 20000          /* tokens of the newest turns kept as they are */
#define FILES 256           /* paths listed, of each kind */

static const char instructions[] =
    "You keep the summary that replaces the history of a long coding session, so that the work "
    "can go on in a fresh context window. Whoever continues sees only your summary and the newest "
    "messages, so it must stand on its own. Use this format:\n\n"
    "## Goal\nWhat the user wants to achieve.\n\n"
    "## Constraints and preferences\nRequirements and preferences the user stated.\n\n"
    "## Progress\n### Done\n### In progress\n### Blocked\n\n"
    "## Key decisions\nWhat was decided, and why.\n\n"
    "## Next steps\nWhat should happen next, in order.\n\n"
    "## Critical context\nNames, paths, commands, errors and numbers needed to continue.\n\n"
    "Keep every section; write \"(none)\" under an empty one. Be specific and concise. The files "
    "read and modified are listed separately: do not list them. Write only the summary.";

static const char ask[] = "\n</conversation>\n\nWrite the summary of the conversation above.";
static const char ask_update[] =
    "\n</conversation>\n\nUpdate the previous summary with the conversation above: keep what still "
    "holds, add what is new, move finished work to Done, and drop what no longer matters.";
static const char gap[] = "\n\n[... the middle of the session is left out ...]\n\n";

static bool sent_answer(const entry *e)
{
    return e->kind == ENT_ASSISTANT && e->msg.stop != PT_STOP_ERROR && e->msg.stop != PT_STOP_ABORTED;
}

/* An answer of this epoch's own: a compaction right after one, with only
 * the kept copies to summarize, would not make the context any smaller. */
bool pt_compact_possible(pt_agent *a)
{
    uint32_t i;
    for (i = 0; i < a->nentries; i++)
        if (sent_answer(&a->entries[i]) && !a->entries[i].kept)
            return true;
    return false;
}

static uint64_t keep_tokens(pt_agent *a)
{
    const pt_config *c = &a->o.config;
    pt_ai_target target;
    const char *why;
    uint64_t window = c->context_window;
    if (c->compact_keep)
        return c->compact_keep;
    if (!window && pt_ai_resolve(a->o.catalog, c->model.n ? c->model : a->last_model, &target, &a->scratch, &why))
        window = target.model.context_window;
    return window && window / 5 < KEEP ? window / 5 : KEEP;
}

/* The first entry kept as it is (0: none): the newest turns through
 * cutoff that fit in keep_tokens, starting at a user or assistant entry so
 * that no tool result is parted from its call. Something before it must
 * be left to summarize. */
static uint32_t choose_keep(pt_agent *a, uint32_t cutoff)
{
    uint64_t budget = keep_tokens(a) * 3, bytes = 0; /* about 3 bytes a token */
    uint32_t i = a->nentries, at = 0, k;
    while (i-- > 1) { /* entries[0] is the epoch */
        const entry *e = &a->entries[i];
        if (e->id > cutoff || !pt_core_sent(e))
            continue;
        bytes += pt_core_sent_bytes(a, e);
        if (bytes > budget)
            break;
        if (e->kind == ENT_USER || e->kind == ENT_ASSISTANT)
            at = i;
    }
    for (k = 1; at && k < at; k++)
        if (sent_answer(&a->entries[k]))
            return a->entries[at].id;
    return 0;
}

uint32_t pt_compact_write(pt_agent *a, uint32_t owner, uint32_t cutoff)
{
    task ct;
    memset(&ct, 0, sizeof ct);
    ct.id = pt_core_new_id(a);
    ct.kind = TASK_COMPACT;
    ct.owner = owner;
    ct.status = ST_PENDING;
    ct.phase = PH_PREPARE;
    ct.attempt = 1;
    ct.cutoff = cutoff;
    ct.keep = choose_keep(a, cutoff);
    pt_write_task(&a->cw, &ct);
    return ct.id;
}

/* ---- the request ---- */

/* s, or its head and tail around a marker when it is longer than max. */
static void put_cut(pt_buf *b, pt_str s, size_t max)
{
    size_t h = max / 2, t = max / 2;
    if (s.n <= max) {
        pt_buf_puts(b, s);
        return;
    }
    while (h && ((unsigned char)s.p[h] & 0xC0) == 0x80) /* whole characters */
        h--;
    while (t && ((unsigned char)s.p[s.n - t] & 0xC0) == 0x80)
        t--;
    pt_buf_put(b, s.p, h);
    pt_buf_putz(b, "\n…[");
    pt_buf_u64(b, s.n - h - t);
    pt_buf_putz(b, " bytes omitted]…\n");
    pt_buf_put(b, s.p + s.n - t, t);
}

/* The entries before `end` as text. */
static void transcript(pt_agent *a, uint32_t end, pt_buf *b)
{
    uint32_t i, k;
    for (i = 0; i < a->nentries && a->entries[i].id < end; i++) {
        const entry *e = &a->entries[i];
        const pt_msg *m = &e->msg;
        switch (e->kind) {
        case ENT_EPOCH: /* its summary goes first, as the previous one */
            if (e->request.n) {
                pt_buf_putz(b, "[user]\n");
                pt_buf_puts(b, e->request);
                pt_buf_putz(b, "\n\n");
            }
            break;
        case ENT_USER:
            pt_buf_putz(b, "[user]\n");
            pt_buf_puts(b, m->text);
            pt_buf_putz(b, "\n\n");
            break;
        case ENT_ASSISTANT:
            if (!sent_answer(e))
                break;
            pt_buf_putz(b, "[assistant]\n");
            for (k = 0; k < m->nblocks; k++) {
                const pt_block *bl = &m->blocks[k];
                if (bl->kind == PT_BLOCK_TEXT) {
                    pt_buf_puts(b, bl->text);
                    pt_buf_putc(b, '\n');
                } else if (bl->kind == PT_BLOCK_TOOL_CALL) {
                    pt_buf_putz(b, "[tool call: ");
                    pt_buf_puts(b, bl->name);
                    pt_buf_putz(b, "] ");
                    put_cut(b, bl->args, CUT);
                    pt_buf_putc(b, '\n');
                }
            }
            pt_buf_putc(b, '\n');
            break;
        case ENT_TOOL_RESULT:
            pt_buf_putz(b, "[tool result: ");
            pt_buf_puts(b, m->name);
            pt_buf_putz(b, m->is_error ? ", error]\n" : "]\n");
            put_cut(b, m->text, CUT);
            pt_buf_putz(b, "\n\n");
            break;
        case ENT_NOTE:
            if (e->to_model) {
                pt_buf_putz(b, "[note]\n");
                pt_buf_puts(b, m->text);
                pt_buf_putz(b, "\n\n");
            }
            break;
        case ENT_ERROR:
            break;
        }
    }
}

/* The summarization request: the previous summary, if the epoch has one,
 * then the entries before t->keep as text; a transcript too long for the
 * window keeps its start (the goal) and its end (the newest work). */
bool pt_compact_context(pt_agent *a, const task *t, pt_ai_context *out, pt_arena *ar)
{
    static const char prev_open[] = "<previous-summary>\n", prev_close[] = "\n</previous-summary>\n\n",
                      conv[] = "<conversation>\n";
    pt_ai_target target;
    pt_msg *msgs = (pt_msg *)pt_arena_zalloc(ar, sizeof(pt_msg) * 2);
    pt_str prev = a->nentries ? a->entries[0].summary : PT_STR_NULL, end = prev.n ? PT_S(ask_update) : PT_S(ask);
    uint64_t window, budget;
    pt_buf b;
    pt_str all;
    char *p;
    size_t n, head, tail, pre;
    if (!msgs || !pt_run_resolve(a, t->model, &target, ar))
        return false;
    window = target.model.context_window;
    /* about 3 bytes per token, leaving room for the summary */
    budget = window > (uint64_t)t->max_tokens + 2048 ? (window - t->max_tokens - 2048) * 3 : 0;
    budget = budget > prev.n ? budget - prev.n : 0;
    if (budget < 16384)
        budget = 16384;
    pt_buf_init(&b, a->al);
    transcript(a, t->keep ? t->keep : t->cutoff + 1, &b);
    all = pt_buf_str(&b);
    head = all.n;
    tail = 0;
    if (all.n > budget) { /* keep the start (the goal) and the most recent work */
        head = (size_t)(budget / 5);
        tail = (size_t)(budget - head);
        while (head && ((unsigned char)all.p[head] & 0xC0) == 0x80)
            head--;
        while (tail && ((unsigned char)all.p[all.n - tail] & 0xC0) == 0x80)
            tail--;
    }
    pre = (prev.n ? sizeof prev_open - 1 + prev.n + sizeof prev_close - 1 : 0) + sizeof conv - 1;
    n = pre + head + (tail ? sizeof gap - 1 + tail : 0) + end.n;
    p = b.oom ? NULL : (char *)pt_arena_alloc(ar, n);
    if (p) {
        char *q = p;
        if (prev.n) {
            memcpy(q, prev_open, sizeof prev_open - 1);
            q += sizeof prev_open - 1;
            memcpy(q, prev.p, prev.n);
            q += prev.n;
            memcpy(q, prev_close, sizeof prev_close - 1);
            q += sizeof prev_close - 1;
        }
        memcpy(q, conv, sizeof conv - 1);
        q += sizeof conv - 1;
        if (head)
            memcpy(q, all.p, head);
        q += head;
        if (tail) {
            memcpy(q, gap, sizeof gap - 1);
            q += sizeof gap - 1;
            memcpy(q, all.p + all.n - tail, tail);
            q += tail;
        }
        memcpy(q, end.p, end.n);
    }
    pt_buf_free(&b);
    if (!p)
        return false;
    msgs[0].role = PT_ROLE_SYSTEM;
    msgs[0].text = PT_S(instructions);
    msgs[1].role = PT_ROLE_USER;
    msgs[1].text = (pt_str){p, n};
    out->msgs = msgs;
    out->nmsgs = 2;
    out->tools = NULL;
    out->ntools = 0;
    return true;
}

/* ---- the steps ---- */

/* Pin the model and how long the summary may be. Pure, like a run's. */
static bool prepare(pt_agent *a, task *t, uint32_t attempt)
{
    pt_arena *sa = &a->scratch;
    const pt_config *c = &a->o.config;
    pt_ai_target target;
    pt_credentials cred;
    task nt = *t;
    pt_buf ref;
    bool ok;
    if (!pt_run_resolve(a, c->model, &target, sa) || !pt_run_credentials(a, target.provider, &cred))
        return false;
    pt_buf_init(&ref, a->al);
    pt_buf_putz(&ref, target.provider->id);
    pt_buf_putc(&ref, '/');
    pt_buf_putz(&ref, target.model.id);
    nt.status = ST_RUNNING;
    nt.phase = PH_REQUEST;
    nt.attempt = attempt;
    nt.model = pt_buf_str(&ref);
    nt.reasoning = c->reasoning;
    nt.max_tokens = target.model.max_tokens && target.model.max_tokens < SUMMARY_TOKENS
                        ? target.model.max_tokens
                        : SUMMARY_TOKENS;
    pt_core_commit_begin(a, t->id);
    pt_write_task(&a->cw, &nt);
    ok = pt_core_commit_end(a);
    pt_buf_free(&ref);
    return ok;
}

typedef enum { MAT_READY, MAT_ASKED, MAT_WAITING } mat_state;

/* The next epoch's materials for t (section 12.3), asked for once: when
 * the compaction starts its request, so they are as of this moment. */
static mat_state want_materials(pt_agent *a, task *t)
{
    pt_fx fx;
    if (a->mat_task == t->id && a->mat_ready)
        return MAT_READY;
    if (a->mat_task == t->id && a->mat_fx)
        return MAT_WAITING;
    a->mat_task = t->id;
    a->mat_ready = false;
    memset(&fx, 0, sizeof fx);
    fx.kind = PT_FX_EPOCH_MATERIALS;
    fx.id = a->mat_fx = pt_core_new_fx(a);
    fx.task = t->id;
    fx.epoch = a->cur_epoch + 1;
    fx.bytes = a->nentries ? a->entries[0].msg.text : PT_STR_NULL;
    pt_core_queue(a, &fx, PT_STR_NULL);
    return MAT_ASKED;
}

void pt_fx_materials(pt_agent *a, uint32_t fx, const pt_materials *m)
{
    pt_jw w;
    if (!fx || fx != a->mat_fx)
        return; /* the compaction ended meanwhile */
    a->mat_fx = 0;
    a->mat_ready = true;
    a->mat_keep = !m;
    pt_buf_reset(&a->mat_system);
    pt_buf_reset(&a->mat_tools);
    if (m) {
        pt_buf_puts(&a->mat_system, m->system);
        pt_jw_init(&w, &a->mat_tools);
        pt_core_write_tool_defs(&w, m->tools, m->ntools);
        if (a->mat_system.oom || a->mat_tools.oom)
            a->mat_keep = true; /* the current epoch's, then */
    }
}

static void materials_done(pt_agent *a)
{
    a->mat_task = a->mat_fx = 0;
    a->mat_ready = false;
}

/* The tools array of the current epoch, as it was frozen. */
static pt_str epoch_tools(pt_agent *a)
{
    const pt_json *r = a->nentries ? pt_json_parse(a->entries[0].raw, &a->scratch, NULL) : NULL;
    const pt_json *tools = pt_json_get(pt_json_get(r, "data"), "tools");
    return pt_json_is(tools, PT_J_ARR) ? tools->raw : PT_S("[]");
}

/* ---- the files read and modified ---- */

typedef struct file_list {
    pt_str read[FILES], mod[FILES];
    uint32_t nread, nmod;
} file_list;

static bool listed(const pt_str *l, uint32_t n, pt_str path)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        if (pt_str_eq(l[i], path))
            return true;
    return false;
}

/* Append path unless listed; a full list drops its oldest. */
static void add_path(pt_str *l, uint32_t *n, pt_str path)
{
    if (!path.n || memchr(path.p, '\n', path.n) || listed(l, *n, path))
        return;
    if (*n == FILES)
        memmove(l, l + 1, sizeof *l * --*n);
    l[(*n)++] = path;
}

/* The paths of the previous lists, then of the summarized tool calls with
 * a "path" argument: a call of a read-only tool reads it, any other may
 * change it. Code, not the model, keeps these, so none is forgotten. */
static file_list *collect_files(pt_agent *a, const task *t)
{
    pt_arena *sa = &a->scratch;
    file_list *f = (file_list *)pt_arena_zalloc(sa, sizeof *f);
    const pt_json *r, *prev, *c;
    uint32_t i, k, end = t->keep ? t->keep : t->cutoff + 1;
    if (!f || !a->nentries)
        return f;
    r = pt_json_parse(a->entries[0].raw, sa, NULL);
    prev = pt_json_get(pt_json_get(r, "data"), "files");
    for (c = pt_json_is(pt_json_get(prev, "read"), PT_J_ARR) ? pt_json_get(prev, "read")->child : NULL; c; c = c->next)
        add_path(f->read, &f->nread, pt_json_str(c, sa));
    for (c = pt_json_is(pt_json_get(prev, "modified"), PT_J_ARR) ? pt_json_get(prev, "modified")->child : NULL; c;
         c = c->next)
        add_path(f->mod, &f->nmod, pt_json_str(c, sa));
    for (i = 1; i < a->nentries && a->entries[i].id < end; i++) {
        const pt_msg *m = &a->entries[i].msg;
        if (!sent_answer(&a->entries[i]))
            continue;
        for (k = 0; k < m->nblocks; k++) {
            int tool;
            pt_str path;
            if (m->blocks[k].kind != PT_BLOCK_TOOL_CALL || (tool = pt_core_find_tool(a, m->blocks[k].name)) < 0)
                continue;
            path = pt_json_gets(pt_json_parse(m->blocks[k].args, sa, NULL), "path", sa);
            if (a->tool_meta[tool].read_only)
                add_path(f->read, &f->nread, path);
            else
                add_path(f->mod, &f->nmod, path);
        }
    }
    return f;
}

static void write_files(pt_jw *w, const file_list *f)
{
    uint32_t i, nread = 0;
    for (i = 0; i < f->nread; i++)
        nread += !listed(f->mod, f->nmod, f->read[i]);
    if (!nread && !f->nmod)
        return;
    pt_jw_key(w, "files");
    pt_jw_obj(w);
    pt_jw_key(w, "read");
    pt_jw_arr(w);
    for (i = 0; i < f->nread; i++)
        if (!listed(f->mod, f->nmod, f->read[i])) /* modified says it all */
            pt_jw_str(w, f->read[i]);
    pt_jw_arr_end(w);
    pt_jw_key(w, "modified");
    pt_jw_arr(w);
    for (i = 0; i < f->nmod; i++)
        pt_jw_str(w, f->mod[i]);
    pt_jw_arr_end(w);
    pt_jw_obj_end(w);
}

/* Whether a user entry is among the kept ones (the run's own, if so). */
static bool keeps_user(pt_agent *a, const task *t)
{
    uint32_t i;
    for (i = 0; t->keep && i < a->nentries && a->entries[i].id <= t->cutoff; i++)
        if (a->entries[i].id >= t->keep && a->entries[i].kind == ENT_USER)
            return true;
    return false;
}

/* Copies of the kept entries, after the new epoch entry: new IDs, the same
 * data, and "kept" naming the original. */
static bool write_kept(pt_agent *a, const task *t)
{
    pt_jw *w = &a->cw;
    uint32_t i;
    for (i = 0; t->keep && i < a->nentries && a->entries[i].id <= t->cutoff; i++) {
        const entry *e = &a->entries[i];
        const pt_json *r, *d;
        if (e->id < t->keep || e->kind == ENT_EPOCH || !pt_core_sent(e))
            continue;
        r = pt_json_parse(e->raw, &a->scratch, NULL);
        if (!(d = pt_json_get(r, "data")))
            return false; /* out of memory: it was parsed once already */
        pt_jw_obj(w);
        pt_jw_key(w, "t");
        pt_jw_strz(w, "entry");
        pt_jw_key(w, "id");
        pt_jw_uint(w, pt_core_new_id(a));
        pt_jw_key(w, "kind");
        pt_jw_str(w, pt_json_gets(r, "kind", &a->scratch));
        pt_jw_key(w, "epoch");
        pt_jw_uint(w, a->cur_epoch + 1);
        pt_jw_key(w, "kept");
        pt_jw_uint(w, e->id);
        pt_jw_key(w, "data");
        pt_jw_raw(w, d->raw);
        pt_jw_obj_end(w);
    }
    return true;
}

static void end_owner(pt_agent *a, task *run, bool ok, pt_str why)
{
    task nt;
    input *in;
    if (!run || run->status == ST_DONE || run->phase != PH_COMPACT)
        return;
    nt = *run;
    if (ok) {
        nt.status = ST_RUNNING;
        nt.phase = PH_PREPARE;
        nt.attempt = 1;
        pt_write_task(&a->cw, &nt);
        return;
    }
    nt.status = ST_DONE;
    nt.outcome = PT_S("failed");
    nt.reason = why;
    nt.result = 0;
    pt_write_task(&a->cw, &nt);
    if ((in = pt_core_input(a, run->input))) {
        input nin = *in;
        nin.status = IN_FAILED;
        pt_write_input(&a->cw, &nin);
    }
    pt_core_after(a, AFTER_RUN_DONE, "failed", why, 0);
}

/* The summary arrived (a new epoch), or the request failed. */
bool pt_compact_finish(pt_agent *a, task *t)
{
    run_live *rl = t->live;
    const pt_msg *m = pt_ai_stream_message(rl->s);
    const pt_ai_error *e = pt_ai_stream_error(rl->s);
    task *run = t->owner ? pt_core_task(a, t->owner) : NULL;
    input *in = run ? pt_core_input(a, run->input) : NULL;
    task nt = *t;
    pt_buf summary, why;
    file_list *files;
    pt_jw *w = &a->cw;
    uint32_t i;
    bool ok;
    pt_buf_init(&summary, a->al);
    pt_buf_init(&why, a->al);
    for (i = 0; i < m->nblocks; i++)
        if (m->blocks[i].kind == PT_BLOCK_TEXT)
            pt_buf_puts(&summary, m->blocks[i].text);
    pt_core_commit_begin(a, t->id);
    if ((m->stop == PT_STOP_STOP || m->stop == PT_STOP_LENGTH) && summary.n) {
        uint32_t id = pt_core_new_id(a);
        bool fresh = a->mat_task == t->id && a->mat_ready && !a->mat_keep;
        pt_core_entry_head(a, id, "epoch", a->cur_epoch + 1);
        pt_jw_key(w, "n");
        pt_jw_uint(w, a->cur_epoch + 1);
        pt_jw_key(w, "system");
        pt_jw_str(w, fresh ? pt_buf_str(&a->mat_system) : a->nentries ? a->entries[0].msg.text : PT_STR_NULL);
        pt_jw_key(w, "tools");
        pt_jw_raw(w, fresh ? pt_buf_str(&a->mat_tools) : epoch_tools(a));
        pt_jw_key(w, "skills");
        pt_jw_raw(w, PT_S("[]"));
        pt_jw_key(w, "summary");
        pt_jw_str(w, pt_buf_str(&summary));
        if ((files = collect_files(a, t)))
            write_files(w, files);
        else
            pt_core_fail(a, PT_FATAL_OOM, "out of memory", PT_STR_NULL);
        if (in && in->text.n && !keeps_user(a, t)) {
            pt_jw_key(w, "request");
            pt_jw_str(w, in->text);
        }
        pt_jw_key(w, "compacted");
        pt_jw_uint(w, t->cutoff);
        pt_core_entry_end(a);
        if (!write_kept(a, t))
            pt_core_fail(a, PT_FATAL_OOM, "out of memory", PT_STR_NULL);
        nt.status = ST_DONE;
        nt.outcome = PT_S("completed");
        nt.result = id;
        pt_write_task(w, &nt);
        end_owner(a, run, true, PT_STR_NULL);
        if (!run)
            pt_core_after(a, AFTER_COMPACT, "completed", PT_STR_NULL, 0);
    } else if (m->stop != PT_STOP_STOP && m->stop != PT_STOP_LENGTH && e->cls == PT_ERR_RETRYABLE) {
        int64_t until = pt_core_now(a) + pt_run_backoff(a, t->attempt, e->retry_after_ms);
        pt_run_write_error(a, t->attempt, e, until);
        nt.phase = PH_RETRY;
        nt.until = until;
        pt_core_after(a, AFTER_RETRY, "", e->message, until);
    } else {
        pt_buf_putz(&why, "compaction failed: ");
        if (e->message.n)
            pt_buf_puts(&why, e->message);
        else
            pt_buf_putz(&why, m->stop == PT_STOP_REFUSAL ? "the model refused" : "the summary was empty");
        if (e->cls != PT_ERR_NONE)
            pt_run_write_error(a, t->attempt, e, 0);
        nt.status = ST_DONE;
        nt.outcome = PT_S("failed");
        nt.reason = pt_buf_str(&why);
        pt_write_task(w, &nt);
        end_owner(a, run, false, pt_buf_str(&why));
        if (!run)
            pt_core_after(a, AFTER_COMPACT, "failed", pt_buf_str(&why), 0);
    }
    pt_run_add_usage(a, m, t->model);
    ok = !summary.oom && !why.oom && pt_core_commit_end(a);
    if (nt.status == ST_DONE)
        materials_done(a);
    pt_buf_free(&summary);
    pt_buf_free(&why);
    if (!rl->http_done)
        pt_core_cancel(a, t->id, rl->http_fx);
    pt_run_free_live(a, t);
    return ok;
}

bool pt_compact_step(pt_agent *a, task *t)
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
    case PH_REQUEST:
        if (!t->live) {
            pt_event ev;
            mat_state ms = want_materials(a, t);
            if (ms != MAT_READY)
                return ms == MAT_ASKED;
            memset(&ev, 0, sizeof ev);
            ev.kind = PT_E_COMPACT;
            ev.task = t->id;
            ev.outcome = PT_S("started");
            pt_core_emit(a, &ev);
            return pt_run_start(a, t);
        }
        if (t->live->finished)
            return pt_compact_finish(a, t);
        return false;
    default:
        return false;
    }
}

uint32_t pt_compact_abort(pt_agent *a, task *t)
{
    task nt = *t;
    uint32_t fx = t->live && !t->live->http_done ? t->live->http_fx : t->phase == PH_RETRY ? t->fx : 0;
    nt.status = ST_DONE;
    nt.outcome = PT_S("aborted");
    nt.reason = PT_S("interrupted by the user");
    nt.result = 0;
    pt_write_task(&a->cw, &nt);
    if (a->mat_task == t->id)
        materials_done(a);
    return fx;
}
