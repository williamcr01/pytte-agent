/* Opening, switching and closing sessions, with the draft and input
 * history they carry; the pager (Ctrl-O) and suspending. */
#define _POSIX_C_SOURCE 200809L
#include "tui/app.h"

#include <fcntl.h>
#include <unistd.h>

static void draft_path(const app *a, char *out, size_t cap)
{
    snprintf(out, cap, "%s/draft", a->s.dir);
}

void save_draft(app *a)
{
    char p[4300];
    if (!a->open)
        return;
    draft_path(a, p, sizeof p);
    if (a->in.text.n && !secret_at(pt_buf_str(&a->in.text)))
        pt_sys_write_file(p, a->in.text.p, a->in.text.n);
    else
        unlink(p);
    a->draft.dirty = false;
    a->draft.at = pt_sys_now_ms();
}

static void reset_display(app *a)
{
    uint32_t i;
    group_close(a); /* the old session's, to the scrollback */
    pt_buf_reset(&a->view.live);
    a->view.kind = LIVE_NONE;
    a->view.streamed = a->view.started = a->view.printed = a->run.active = false;
    a->model.ctx_used = 0;
    for (i = 0; i < a->queue.n; i++)
        pt_buf_reset(&a->queue.items[i].text);
    a->queue.n = 0;
}

bool session_empty(app *a)
{
    uint32_t i, n;
    pt_entry_view v;
    if (!a->sess.created || a->in.text.n)
        return false;
    for (i = 0; i < a->queue.n; i++)
        if (!a->queue.items[i].command)
            return false;
    n = pt_entry_count(a->s.a);
    for (i = 0; i < n && pt_entry_at(a->s.a, i, &v); i++)
        if (strcmp(v.kind, "epoch") != 0)
            return false;
    return true;
}

bool open_session(app *a, const char *id)
{
    a->sess.opening = cli_session_begin(&a->s, &a->o, id, !a->hist.ready,
                                    a->open && !secret_at(pt_buf_str(&a->in.text)) ? pt_buf_str(&a->in.text)
                                                                                   : PT_STR_NULL,
                                    a->open && session_empty(a),
                                    on_event, a, a->sess.err, sizeof a->sess.err);
    if (!a->sess.opening) {
        group_close(a);
        out_note(a, &a->lines, TUI_RED, pt_strz(a->sess.err));
        a->code = 1;
        return false;
    }
    a->open = false;
    a->sess.created = id == NULL && !a->o.continue_current;
    a->sess.discarded = false;
    a->draft.drop_restored = false;
    reset_display(a);
    update_model(a);
    return true;
}

/* Close the session; an empty one this process made is discarded unless
 * /quit asked to keep it for resuming. */
void close_session(app *a)
{
    bool empty;
    while (a->sess.opening) {
        pt_waitfd fd;
        fd.fd = cli_session_job_fd(a->sess.opening);
        pt_sys_wait(&fd, 1, -1);
        finish_session(a);
    }
    if (!a->open)
        return;
    while (a->queue.xs) /* the runtime goes: their answers never come */
        xreq_drop(a, a->queue.xs);
    empty = !a->sess.keep_on_quit && session_empty(a);
    save_draft(a);
    if (pt_fatal(a->s.a) == PT_FATAL_STORAGE)
        cli_session_free(&a->s);
    else
        cli_session_close(&a->s);
    a->sess.discarded = empty && !a->in.text.n;
    if (a->sess.discarded) /* nothing was ever said: leave no session behind */
        cli_session_discard(a->s.dir);
    a->open = false;
}

/* /new, /resume: close this one, open the other; back to this one if
 * that fails. */
void switch_session(app *a, const char *id)
{
    char back[64];
    snprintf(back, sizeof back, "%s", a->s.id);
    a->o.continue_current = false;
    snprintf(a->sess.fallback, sizeof a->sess.fallback, "%s", back);
    a->sess.fallback_created = a->sess.created;
    if (!open_session(a, id)) {
        a->sess.fallback[0] = 0;
        a->code = 0; /* allocation failed before the old session was handed off */
        return;
    }
    pt_buf_reset(&a->lines); /* the old session's last lines: cleared anyway */
    tui_band_clear(&a->band);
    a->pace.urgent = true;
    tui_input_set(&a->in, PT_STR_NULL);
    a->code = 0;
}

void suspend(app *a)
{
    tui_band_erase(&a->band);
    flush_out(a);
    tui_term_suspend(a->t);
}

void resume(app *a)
{
    tui_term_resume(a->t);
    tui_term_size(a->t, &a->rows, &a->cols);
    a->band.rows = a->rows;
    a->band.cols = a->cols;
    a->pace.urgent = true;
}

void pager(app *a)
{
    char path[] = "/tmp/pytte-view-XXXXXX", cmd[1200];
    uint32_t i, n;
    pt_entry_view v;
    pt_buf styled, out;
    int fd;
    if (!a->open || (fd = mkstemp(path)) < 0)
        return;
    pt_buf_init(&styled, pt_sys_allocator());
    pt_buf_init(&out, pt_sys_allocator());
    n = pt_entry_count(a->s.a);
    for (i = 0; i < n && pt_entry_at(a->s.a, i, &v); i++)
        out_entry(a, &styled, &v, true);
    tui_sgr(&out, pt_buf_str(&styled), a->color);
    if (write(fd, out.p, out.n) != (ssize_t)out.n)
        out.n = 0;
    close(fd);
    snprintf(cmd, sizeof cmd, "%s '%s'", a->cfg->val[CFG_PAGER], path);
    suspend(a);
    if (system(cmd) != 0) {
        /* a pager that fails says why itself */
    }
    unlink(path);
    resume(a);
    pt_buf_free(&styled);
    pt_buf_free(&out);
}

void history_append(app *a, pt_str text)
{
    int fd;
    if (!a->hist.path[0])
        return;
    pt_buf_reset(&a->hist.pending);
    tui_hist_line(&a->hist.pending, text);
    if (a->hist.pending.oom)
        return; /* best effort; never append a truncated JSON string */
    fd = open(a->hist.path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd >= 0) {
        if (write(fd, a->hist.pending.p, a->hist.pending.n) != (ssize_t)a->hist.pending.n) {
            /* best effort */
        }
        close(fd);
    }
    pt_buf_reset(&a->hist.pending);
}

void history_load(app *a)
{
    char root[4096], *slash;
    cli_sessions_dir(root, sizeof root);
    if ((slash = strrchr(root, '/')))
        *slash = 0;
    snprintf(a->hist.path, sizeof a->hist.path, "%s/history", root);
}

void finish_session(app *a)
{
    int rc;
    char *text;
    size_t n;
    if (!a->sess.opening || !cli_session_finish(a->sess.opening, &rc, a->sess.err, sizeof a->sess.err))
        return;
    a->sess.opening = NULL;
    if (rc) {
        group_close(a);
        out_note(a, &a->lines, TUI_RED, pt_strz(a->sess.err));
        a->pace.urgent = true;
        if (a->sess.fallback[0]) {
            char back[64];
            snprintf(back, sizeof back, "%s", a->sess.fallback);
            a->sess.fallback[0] = 0;
            if (open_session(a, back)) {
                a->sess.created = a->sess.fallback_created; /* still discarded if nothing is said */
                return;
            }
        }
        a->code = rc;
        a->quit = true;
        return;
    }
    a->open = true;
    a->sess.fallback[0] = 0;
    {
        char name[CLI_NAME_MAX + 1], dir[4096];
        pt_str cwd = pt_session_cwd(a->s.a);
        uint32_t i, ne = pt_entry_count(a->s.a);
        pt_entry_view v;
        /* a session from before names, or whose prompts are compacted away,
         * is not named after a later one */
        a->sess.named = cli_session_name(a->s.dir, name, sizeof name);
        for (i = 0; i < ne && !a->sess.named && pt_entry_at(a->s.a, i, &v); i++)
            a->sess.named = strcmp(v.kind, "user") == 0 || (strcmp(v.kind, "epoch") == 0 && v.epoch > 1);
        /* new sessions start here: in the session's directory */
        if (cwd.n && cwd.n < sizeof dir) {
            memcpy(dir, cwd.p, cwd.n);
            dir[cwd.n] = 0;
            (void)!chdir(dir);
        }
    }
    if (!a->hist.ready) {
        text = cli_session_take_history(&a->s, &n);
        if (text && !tui_hist_load(&a->in, (pt_str){text, n}, 1000))
            out_z(a, TUI_RED, "out of memory loading input history");
        free(text);
        if (!tui_hist_load(&a->in, pt_buf_str(&a->hist.pending), UINT32_MAX))
            out_z(a, TUI_RED, "out of memory loading input history");
        if (a->hist.pending.n) {
            int fd = open(a->hist.path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
            if (fd >= 0) {
                (void)!write(fd, a->hist.pending.p, a->hist.pending.n);
                close(fd);
            }
        }
        pt_buf_reset(&a->hist.pending);
        a->hist.ready = true;
    }
    text = cli_session_take_draft(&a->s, &n);
    if (text && !a->draft.drop_restored) {
        size_t cur = a->in.cur, prefix;
        int col = 0;
        pt_buf_reset(&a->wrap);
        tui_clean(&a->wrap, (pt_str){text, n}, &col);
        prefix = a->wrap.n;
        pt_buf_puts(&a->wrap, pt_buf_str(&a->in.text));
        if (!a->wrap.oom) {
            tui_input_set(&a->in, pt_buf_str(&a->wrap));
            if (!a->in.text.oom)
                a->in.cur = prefix + cur;
        }
    }
    free(text);
    set_config(a);
    out_tail(a);
    if (cli_session_note(&a->s)[0])
        out_z(a, TUI_DIM, cli_session_note(&a->s));
    queued_submit(a);
    if (a->pager_pending && !a->quit && a->open) {
        a->pager_pending = false;
        pager(a);
    }
    a->pace.urgent = true;
}
