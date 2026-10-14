/* The TUI (section 16.2): setup and the event loop. The rest of the app
 * is split by concern; app.h lists its files.
 *
 * Agent callbacks, keys and display updates run on the event-loop thread.
 * Session setup/recovery and durable writes use a bounded I/O worker.
 * Text goes to the scrollback once and is never touched again (R5): a
 * streaming answer is committed one complete source line at a time, and
 * the unfinished line is shown in the band as the live tail. */
#define _POSIX_C_SOURCE 200809L
#include "tui/app.h"

#include <unistd.h>

static bool reflows(void)
{
    const char *e = getenv("PYTTE_REFLOW"), *term = getenv("TERM");
    if (e && *e)
        return strcmp(e, "0") != 0;
    if (term && (strncmp(term, "linux", 5) == 0 || strncmp(term, "st", 2) == 0 || strncmp(term, "rxvt", 4) == 0))
        return false;
    if (getenv("XTERM_VERSION") && !getenv("TMUX") && !getenv("STY"))
        return false;
    return true;
}

/* Replies are ordinary input events. A terminal that does not answer must
 * never hold up painting or typing; late keyboard negotiation still works. */
static void on_reply(app *a, const tui_ev *ev)
{
    if (ev->reply == TR_DA1) {
        a->caps.answered = true;
        a->keys.want_cpr = false;
    } else if (ev->reply == TR_KITTY && !a->caps.kitty) {
        a->caps.kitty = true;
        tui_term_kitty(a->t, true);
    } else if (ev->reply == TR_CPR) {
        a->caps.cpr = true;
        a->caps.start_col = ev->b;
    }
}

static int timeout_ms(app *a, int64_t now)
{
    int64_t t = -1, due;
#define AT(when)                                  \
    do {                                          \
        int64_t d_ = (when) - now;                \
        if (d_ < 0)                               \
            d_ = 0;                               \
        if (t < 0 || d_ < t)                      \
            t = d_;                               \
    } while (0)
    if (a->pace.urgent)
        AT(now);
    if (a->pace.dirty)
        AT(a->pace.last_paint + a->pace.interval);
    if (tui_keys_pending(&a->keys))
        AT(a->pace.keys_at + 30);
    if (a->run.active && !a->run.wait_at) { /* the clock's next half second */
        due = now + BLINK_MS - app_run_clock(a, now) % BLINK_MS;
        AT(due);
    }
    if (now - a->pace.esc_at < DOUBLE_MS)
        AT(a->pace.esc_at + DOUBLE_MS);
    if (now - a->pace.ctrlc_at < DOUBLE_MS)
        AT(a->pace.ctrlc_at + DOUBLE_MS);
    if (a->pace.winch_at)
        AT(a->pace.winch_at);
    if (a->draft.dirty)
        AT(a->draft.at + 1000);
#undef AT
    return t > 60000 ? 60000 : (int)t;
}

static void read_keys(app *a, bool flush)
{
    tui_ev ev;
    while (tui_keys_next(&a->keys, &ev, flush) && !a->quit)
        if (ev.kind == TE_REPLY)
            on_reply(a, &ev);
        else
            app_on_key(a, &ev);
}

static int quit_code(int sig)
{
    return 128 + sig;
}

/* Startup can fail before the band/session are initialized. Release every
 * buffer and arena initialized by tui_run, then restore/free the terminal. */
static void startup_free(app *a)
{
    int i;
    tui_term_free(a->t);
    tui_keys_free(&a->keys);
    tui_input_free(&a->in);
    tui_frame_free(&a->frame);
    pt_buf_free(&a->lines);
    pt_buf_free(&a->tmp);
    pt_buf_free(&a->tmp2);
    pt_buf_free(&a->wrap);
    pt_buf_free(&a->live_rows);
    pt_buf_free(&a->view.live);
    pt_buf_free(&a->run.retry_err);
    for (i = 0; i < RUN_MAX; i++) {
        pt_buf_free(&a->run.calls[i].id);
        pt_buf_free(&a->run.calls[i].head);
    }
    pt_buf_free(&a->group.buf);
    pt_buf_free(&a->menu.buf);
    pt_buf_free(&a->hist.pending);
    pt_buf_free(&a->command);
    pt_arena_free(&a->ar);
}

int tui_run(const cli_opts *o, cli_config *cfg, cli_mcp *mcp, const char *id, const char *prompt)
{
    app st, *a = &st;
    const char *term = getenv("TERM"), *amb = getenv("PYTTE_AMBIGUOUS");
    int sig = 0, slot;
    char picked[64] = "", kept[64];
    memset(a, 0, sizeof *a);
    a->o = *o;
    a->cfg = cfg;
    a->mcp = mcp;
    pt_buf_init(&a->mcp_notes, pt_sys_allocator());
    if (mcp)
        cli_mcp_host(&a->o.mcp_host, app_mcp_note, a);
    a->pace.interval = 33;
    a->pace.esc_at = a->pace.ctrlc_at = -DOUBLE_MS;
    /* the watchdog first: before the store is opened */
    if (!(a->t = tui_term_new())) {
        fprintf(stderr, "pytte: not a terminal\n");
        return 1;
    }
    if (o->model)
        snprintf(a->model.ref, sizeof a->model.ref, "%s", o->model);
    a->o.model = o->model ? a->model.ref : NULL;
    tui_set_ambiguous_wide(amb && strcmp(amb, "wide") == 0);
    a->color = !getenv("NO_COLOR") || !*getenv("NO_COLOR");
    app_apply_symbols(a);
    app_apply_thinking(a);
    tui_keys_init(&a->keys);
    a->keys.want_cpr = true;
    tui_input_init(&a->in, pt_sys_allocator());
    tui_frame_init(&a->frame, pt_sys_allocator());
    pt_buf_init(&a->lines, pt_sys_allocator());
    pt_buf_init(&a->tmp, pt_sys_allocator());
    pt_buf_init(&a->tmp2, pt_sys_allocator());
    pt_buf_init(&a->wrap, pt_sys_allocator());
    pt_buf_init(&a->live_rows, pt_sys_allocator());
    pt_buf_init(&a->view.live, pt_sys_allocator());
    pt_buf_init(&a->run.retry_err, pt_sys_allocator());
    for (slot = 0; slot < RUN_MAX; slot++) {
        pt_buf_init(&a->run.calls[slot].id, pt_sys_allocator());
        pt_buf_init(&a->run.calls[slot].head, pt_sys_allocator());
    }
    pt_buf_init(&a->group.buf, pt_sys_allocator());
    pt_buf_init(&a->menu.buf, pt_sys_allocator());
    pt_buf_init(&a->hist.pending, pt_sys_allocator());
    pt_buf_init(&a->command, pt_sys_allocator());
    pt_arena_init(&a->ar, pt_sys_allocator(), 0);
    if (!tui_term_start(a->t, !term || (strncmp(term, "linux", 5) != 0 && strncmp(term, "vt", 2) != 0))) {
        startup_free(a);
        fprintf(stderr, "pytte: cannot set up the terminal\n");
        return 1;
    }
    tui_term_size(a->t, &a->rows, &a->cols);
    app_history_load(a);
    if (!getcwd(a->sess.start_dir, sizeof a->sess.start_dir))
        a->sess.start_dir[0] = 0;
    if (id)
        snprintf(picked, sizeof picked, "%s", id);
    if (!app_open_session(a, id ? picked : NULL)) {
        startup_free(a);
        fprintf(stderr, "pytte: %s\n", a->sess.err);
        return a->code ? a->code : 1;
    }
    a->o.worktree = NULL; /* -w: this open only; copied by it */
    app_set_config(a);
    tui_band_init(&a->band, a->rows, a->cols, pt_sys_allocator());
    a->band.reflow = reflows();
    a->band.color = a->color;
    {
        pt_waitfd fd;
        char buf[4096];
        long n;
        fd.fd = tui_term_in(a->t);
        if (pt_sys_wait(&fd, 1, 0) > 0 && fd.ready &&
            (n = tui_term_read(a->t, buf, sizeof buf)) > 0) {
            tui_keys_feed(&a->keys, buf, (size_t)n);
            read_keys(a, false);
        }
        /* Without an immediate CPR, conservatively preserve a preceding
         * partial line. Its eventual reply cannot move the painted band. */
        if (a->caps.start_col != 1)
            tui_term_write(a->t, PT_S("\r\n"));
    }
    {
        char head[100];
        pt_buf first;
        pt_buf_init(&first, pt_sys_allocator());
        snprintf(head, sizeof head, " %s · /help for commands", PT_VERSION);
        app_out_text(a, &first, TUI_DIM, "pytte", ACCENT | TUI_BOLD, pt_strz(head), 0);
        pt_buf_puts(&first, pt_buf_str(&a->lines));
        pt_buf_reset(&a->lines);
        pt_buf_puts(&a->lines, pt_buf_str(&first));
        pt_buf_free(&first);
    }
    if (a->mcp && a->mcp->warn[0])
        app_out_z(a, TUI_RED, a->mcp->warn);
    if (a->mcp && a->mcp->nuntrusted) { /* the trust flow: say what would run */
        char names[400] = "", line[700];
        uint32_t i;
        size_t k = 0;
        for (i = 0; i < a->mcp->nall; i++)
            if (!a->mcp->info[i].runs && k < sizeof names - 40)
                k += (size_t)snprintf(names + k, sizeof names - k, "%s%s (%s: %s)", k ? ", " : "",
                                      a->mcp->all[i].name, a->mcp->info[i].source,
                                      a->mcp->all[i].url ? a->mcp->all[i].url : a->mcp->all[i].command);
        snprintf(line, sizeof line,
                 "this project has MCP servers that are not trusted, so they do not run: %s. "
                 "/mcp trust runs them (programs on this machine: trust only what you know)",
                 names);
        app_out_z(a, TUI_DIM, line);
    }
    if (prompt && *prompt) {
        a->draft.drop_restored = true;
        tui_input_set(&a->in, pt_strz(prompt));
        app_submit(a);
    }
    app_menu_update(a);
    app_paint(a);
    while (!a->quit) {
        pt_waitfd fds[5];
        int64_t now = pt_sys_now_ms();
        int bits;
        bool was = a->run.active;
        fds[0].fd = tui_term_in(a->t);
        fds[1].fd = tui_term_sigfd(a->t);
        fds[2].fd = a->sess.opening ? cli_session_job_fd(a->sess.opening) : -1;
        fds[3].fd = a->menu.sessions && a->menu.sessions->pending ? pt_worker_fd(a->menu.sessions->worker) : -1;
        fds[4].fd = a->saving && a->saving->active ? pt_worker_fd(a->saving->worker) : -1;
        if (a->sess.opening) {
            pt_sys_wait(fds, 5, timeout_ms(a, now));
            app_finish_session(a);
        } else if (a->open)
            pt_rt_step(a->s.rt, timeout_ms(a, now), fds, 5);
        else
            pt_sys_wait(fds, 5, timeout_ms(a, now));
        app_finish_setting(a);
        app_show_notes(a);
        if (a->queue.expanded) {
            a->queue.expanded = false;
            app_queued_submit(a);
            set_dirty(a);
        }
        if (a->menu.sessions && a->menu.sessions->pending && pt_worker_finish(a->menu.sessions->worker)) {
            a->menu.sessions->pending = false;
            a->menu.sessions->ready = true;
            app_menu_update(a);
            a->pace.urgent = true;
        }
        now = pt_sys_now_ms();
        {
            uint32_t bg = app_bg_running(a);
            if (bg != a->run.nbg) {
                a->run.nbg = bg;
                set_dirty(a);
            }
        }
        a->run.active = a->open && !pt_idle(a->s.a) && !pt_fatal(a->s.a);
        if (a->run.active != was) {
            set_dirty(a);
            if (a->run.active) {
                app_run_restart(a, now);
                a->run.phase = PH_WAIT;
            }
        }
        if (fds[1].ready) {
            bits = tui_term_signals(a->t, &sig);
            if (bits & TS_HUP) {
                tui_term_gone(a->t);
                a->code = 129;
                break;
            }
            if (bits & TS_QUIT) {
                a->code = quit_code(sig);
                break;
            }
            if (bits & TS_TSTP) {
                app_suspend(a);
                tui_term_stop_self();
                app_resume(a);
            } else if (bits & TS_CONT) {
                app_resume(a); /* stopped by someone else; the shell may have reset the modes */
                tui_band_redraw(&a->band);
            }
            if (bits & TS_WINCH)
                a->pace.winch_at = now ? now : 1;
        }
        if (fds[0].ready) {
            char buf[4096];
            long n = tui_term_read(a->t, buf, sizeof buf);
            if (n == 0) { /* the terminal is gone */
                tui_term_gone(a->t);
                a->code = 129;
                break;
            }
            if (n > 0) {
                tui_keys_feed(&a->keys, buf, (size_t)n);
                a->pace.keys_at = now;
                read_keys(a, false);
            }
        }
        if (tui_keys_pending(&a->keys) && now - a->pace.keys_at >= 30)
            read_keys(a, true);
        if (a->pace.winch_at && now >= a->pace.winch_at) {
            int r, c;
            a->pace.winch_at = 0;
            tui_term_size(a->t, &r, &c);
            if (r != a->rows || c != a->cols) {
                tui_band_resize(&a->band, r, c);
                a->rows = r;
                a->cols = c;
                a->pace.urgent = true;
            }
        }
        if (a->run.active && !a->run.wait_at && app_run_clock(a, now) / BLINK_MS != a->run.tick) {
            a->run.tick = app_run_clock(a, now) / BLINK_MS;
            set_dirty(a);
        }
        if (now - a->pace.esc_at < DOUBLE_MS + 50 || now - a->pace.ctrlc_at < DOUBLE_MS + 50)
            set_dirty(a);
        if (a->draft.dirty && now - a->draft.at >= 1000)
            app_save_draft(a);
        if (a->quit)
            break;
        if (a->pace.urgent || (a->pace.dirty && now - a->pace.last_paint >= a->pace.interval))
            app_paint(a);
    }
    /* quit: the close claim, then the terminal. What was shown of an
     * unfinished answer stays (pytte -c picks the run up again). */
    if (a->code != 129 && ((a->view.kind != LIVE_NONE && a->view.live.n) || a->group.n)) {
        if (a->view.kind != LIVE_NONE && a->view.live.n) {
            app_live_end(a);
            app_out_z(a, TUI_DIM, "(detached)");
        }
        app_group_close(a); /* the band is erased next */
        a->menu.kind = MENU_NONE;
        app_paint(a);
    }
    a->quit = true;
    while (a->sess.opening) {
        pt_waitfd fd;
        fd.fd = cli_session_job_fd(a->sess.opening);
        pt_sys_wait(&fd, 1, -1);
        app_finish_session(a);
    }
    snprintf(kept, sizeof kept, "%.63s", a->open ? a->s.id : "");
    app_close_session(a);
    while (a->saving && (a->saving->active || a->saving->pending)) {
        pt_waitfd fd;
        app_finish_setting(a);
        if (!a->saving->active)
            continue;
        fd.fd = pt_worker_fd(a->saving->worker);
        pt_sys_wait(&fd, 1, -1);
    }
    if (a->saving) {
        pt_worker_free(a->saving->worker);
        free(a->saving);
    }
    if (a->menu.sessions) {
        pt_worker_free(a->menu.sessions->worker);
        free(a->menu.sessions);
    }
    tui_band_erase(&a->band);
    app_flush_out(a);
    if (a->code != 129 && kept[0] && !a->sess.discarded) { /* how to come back to it */
        char last[200];
        pt_buf_reset(&a->lines);
        app_out_blank(&a->lines);
        snprintf(last, sizeof last, "to continue this session: pytte -r %s", kept);
        app_out_z(a, TUI_DIM, last);
        pt_buf_reset(&a->tmp);
        tui_sgr(&a->tmp, pt_buf_str(&a->lines), a->color);
        tui_term_write(a->t, pt_buf_str(&a->tmp)); /* OPOST makes \n a new line */
    }
    tui_term_free(a->t);
    tui_keys_free(&a->keys);
    tui_input_free(&a->in);
    tui_frame_free(&a->frame);
    tui_band_free(&a->band);
    pt_buf_free(&a->lines);
    pt_buf_free(&a->tmp);
    pt_buf_free(&a->tmp2);
    pt_buf_free(&a->live_rows);
    pt_buf_free(&a->wrap);
    pt_buf_free(&a->view.live);
    pt_buf_free(&a->run.retry_err);
    for (slot = 0; slot < RUN_MAX; slot++) {
        pt_buf_free(&a->run.calls[slot].id);
        pt_buf_free(&a->run.calls[slot].head);
    }
    pt_buf_free(&a->group.buf);
    pt_buf_free(&a->menu.buf);
    pt_buf_free(&a->hist.pending);
    pt_buf_free(&a->command);
    {
        uint32_t i;
        for (i = 0; i < a->queue.cap; i++) {
            int k;
            pt_buf_free(&a->queue.items[i].text);
            pt_buf_free(&a->queue.items[i].names);
            for (k = 0; k < XPARTS; k++)
                pt_buf_free(&a->queue.items[i].parts[k]);
        }
        free(a->queue.items);
        pt_buf_free(&a->mcp_notes);
    }
    pt_arena_free(&a->ar);
    return a->code;
}
