/* The rewind task (section 8.6): put files back as they were before a user
 * message.
 *
 *   restore ──► done
 *
 * The task is committed with the whole list of files and the blobs to put
 * back. Each is restored by a RESTORE effect, one after the other; nothing
 * is committed in between. A restore writes the whole file (or removes it),
 * so a crash only means doing it again: the task resumes from its first
 * file. The end is one commit: the task done, and the read stamps of the
 * files cleared, because the model has not seen them as they are now. */
#include "core/core.h"

#include <string.h>

/* The commit that starts the task. */
bool pt_rewind_write(pt_agent *a)
{
    task t;
    memset(&t, 0, sizeof t);
    t.id = pt_core_new_id(a);
    t.kind = TASK_REWIND;
    t.status = ST_RUNNING;
    t.phase = PH_RESTORE;
    t.entry = a->req.rewind.entry;
    t.files = a->req.rewind.files;
    t.nfiles = a->req.rewind.n;
    if (!t.id)
        return false;
    pt_core_commit_begin(a, 0);
    pt_write_task(&a->commit.w, &t);
    return pt_core_commit_end(a);
}

/* The end: done, and "reads:<path>" emptied for each file. */
static bool finish(pt_agent *a, task *t)
{
    task nt = *t;
    pt_buf key;
    uint32_t i;
    bool ok;
    pt_buf_init(&key, a->al);
    pt_core_commit_begin(a, t->id);
    for (i = 0; i < t->nfiles; i++) {
        pt_buf_reset(&key);
        pt_buf_putz(&key, "reads:");
        pt_buf_puts(&key, t->files[i].path);
        pt_core_write_doc(a, pt_buf_str(&key), PT_S("{}"));
    }
    nt.status = ST_DONE;
    nt.outcome = t->reason.n ? PT_S("failed") : PT_S("completed");
    pt_write_task(&a->commit.w, &nt);
    pt_core_after(a, AFTER_REWIND, t->reason.n ? "failed" : "completed", t->reason, 0);
    if (key.oom) {
        pt_core_fail(a, PT_FATAL_OOM, "out of memory", PT_STR_NULL);
        pt_buf_free(&key);
        return false;
    }
    pt_buf_free(&key);
    ok = pt_core_commit_end(a);
    return ok;
}

bool pt_rewind_step(pt_agent *a, task *t)
{
    pt_fx fx;
    if (t->next >= t->nfiles)
        return finish(a, t);
    memset(&fx, 0, sizeof fx);
    fx.kind = PT_FX_RESTORE;
    fx.id = pt_core_new_fx(a);
    fx.task = t->id;
    fx.path = t->files[t->next].path;
    fx.blob = t->files[t->next].blob;
    t->fx = fx.id;
    return pt_core_queue(a, &fx, PT_STR_NULL);
}

bool pt_rewind_done(pt_agent *a, uint32_t fx, const char *error)
{
    uint32_t i;
    for (i = 0; i < a->ntasks; i++) {
        task *t = a->tasks[i];
        if (t->kind != TASK_REWIND || t->fx != fx)
            continue;
        t->fx = 0;
        if (error && !t->reason.n) { /* the first failure; the others still get their turn */
            pt_buf b;
            pt_buf_init(&b, a->al);
            pt_buf_puts(&b, t->files[t->next].path);
            pt_buf_putz(&b, ": ");
            pt_buf_putz(&b, error);
            if (b.oom)
                pt_core_fail(a, PT_FATAL_OOM, "out of memory", PT_STR_NULL);
            else
                t->reason = pt_arena_dup(&t->a, pt_buf_str(&b));
            pt_buf_free(&b);
        }
        t->next++;
        a->idle_sent = false;
        return true;
    }
    return false;
}
