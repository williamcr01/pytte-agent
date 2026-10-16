/* The band (16.2.2): what is drawn under the scrollback each paint, the
 * live tail, a question, the queue, the input, the menu and the status
 * row, laid out within the terminal's height. */
#define _POSIX_C_SOURCE 200809L
#include "tui/app.h"

static void row(app *a, pt_str styled)
{
    tui_frame_row(&a->frame, styled, a->cols);
}

static void blank_row(app *a)
{
    row(a, PT_STR_NULL);
}

/* A token count, short: 950, 12.3k, 200k, 1.2M. */
void app_tokens_short(char *out, size_t cap, uint64_t n)
{
    if (n < 1000)
        snprintf(out, cap, "%llu", (unsigned long long)n);
    else if (n < 10000)
        snprintf(out, cap, "%.1fk", (double)n / 1000);
    else if (n < 1000000)
        snprintf(out, cap, "%lluk", (unsigned long long)((n + 500) / 1000));
    else
        snprintf(out, cap, "%.1fM", (double)n / 1000000);
    n = strlen(out);
    if (n > 3 && strcmp(out + n - 3, ".0k") == 0) /* 2.0k: 2k */
        memmove(out + n - 3, "k", 2);
    else if (n > 3 && strcmp(out + n - 3, ".0M") == 0)
        memmove(out + n - 3, "M", 2);
}

/* Below the input, dim and still while a run goes on: the permission
 * mode, the model (without its provider), effort and context used; what the keys do while a menu
 * or a question is open; or why the run is waiting. */
static void status_row(app *a, int64_t now, int total, int sel)
{
    char line[600], eff[40];
    const char *model = strchr(a->model.name, '/');
    int n;
    pt_buf_reset(&a->tmp);
    pt_buf_putz(&a->tmp, "  ");
    tui_style(&a->tmp, TUI_DIM);
    if (now - a->pace.ctrlc_at < DOUBLE_MS) {
        pt_buf_putz(&a->tmp, "ctrl-c again to quit");
    } else if (a->run.active && now - a->pace.esc_at < DOUBLE_MS) {
        pt_buf_putz(&a->tmp, "esc again to interrupt");
    } else if (a->ask.shown) {
        pt_buf_putz(&a->tmp, a->ascii ? "1-3 or up/down enter · esc no · ctrl-c interrupt"
                                       : "1-3 or ↑↓ enter · esc no · ctrl-c interrupt");
    } else if (a->menu.kind == MENU_SETTINGS) { /* a value to cycle through, or one to type */
        pt_buf_putz(&a->tmp, a->ascii ? "up/down select · " : "↑↓ select · ");
        pt_buf_putz(&a->tmp, app_settings[a->menu.set_sel].vals            ? a->ascii ? "left/right or enter change"
                                                                                       : "←→ or enter change"
                             : app_settings[a->menu.set_sel].k == SET_TOOLS ? "enter open"
                                                                            : "enter edit");
        pt_buf_putz(&a->tmp, " · esc close");
    } else if (a->menu.kind == MENU_TOOLS) {
        uint32_t all, on = app_tools_on(a, &all);
        pt_buf_putz(&a->tmp, a->ascii ? "up/down select · space turns on or off · esc close"
                                       : "↑↓ select · space turns on or off · esc close");
        snprintf(line, sizeof line, " · %u of %u on", on, all);
        pt_buf_putz(&a->tmp, line);
    } else if (a->menu.kind != MENU_NONE) {
        pt_buf_putz(&a->tmp, a->ascii ? "up/down select · tab complete · enter run · esc close"
                                       : "↑↓ select · tab complete · enter run · esc close");
        if (total > 1) {
            snprintf(line, sizeof line, " · %d/%d", sel + 1, total);
            pt_buf_putz(&a->tmp, line);
        }
    } else if (a->run.active && a->run.phase == PH_RETRY) {
        snprintf(line, sizeof line, "retrying · %.*s", (int)(a->run.retry_err.n > 200 ? 200 : a->run.retry_err.n),
                 a->run.retry_err.p);
        pt_buf_putz(&a->tmp, line);
    } else {
        pt_buf_putz(&a->tmp, pt_perm_name(a->o.perm));
        pt_buf_putz(&a->tmp, " · ");
        pt_buf_putz(&a->tmp, model ? model + 1 : a->model.name[0] ? a->model.name : "no model");
        app_effort_label(a, eff, sizeof eff);
        n = snprintf(line, sizeof line, " · %s", eff);
        if (a->model.ctx_window) { /* the context used, of the model's window */
            char used[16], all[16];
            app_tokens_short(used, sizeof used, a->model.ctx_used);
            app_tokens_short(all, sizeof all, a->model.ctx_window);
            n += snprintf(line + n, sizeof line - (size_t)n, " · %s/%s", used, all);
        }
        if (a->run.nbg)
            snprintf(line + n, sizeof line - (size_t)n, " · %u in background (/ps)", a->run.nbg);
        pt_buf_putz(&a->tmp, line);
    }
    row(a, pt_buf_str(&a->tmp));
}

/* Above the input while a run is active: the blinking dot, what the run is
 * doing, its clock and the tokens it read and wrote, once there are some
 * ("• thinking · 12s · ↑ 12k ↓ 340"). */
static void activity_row(app *a, int64_t now)
{
    char what[120], el[32], tin[16], tout[16], tok[64] = "";
    uint64_t in = a->run.tok_in + a->run.req_in, out = app_run_out(a);
    if (a->ask.shown)
        snprintf(what, sizeof what, "waiting for you");
    else if (a->open && pt_rt_materials_waiting(a->s.rt))
        snprintf(what, sizeof what, "starting MCP servers");
    else if (a->run.phase == PH_RETRY) {
        int64_t left = (a->run.retry_until - now + 999) / 1000;
        snprintf(what, sizeof what, "retrying in %llds", (long long)(left > 0 ? left : 0));
    } else if (a->run.phase == PH_TOOL && a->run.ncalls == 1)
        snprintf(what, sizeof what, "running %s", a->run.calls[0].name);
    else if (a->run.phase == PH_TOOL && a->run.ncalls)
        snprintf(what, sizeof what, "running %d calls", a->run.ncalls);
    else
        snprintf(what, sizeof what, "%s", a->run.phase == PH_THINK     ? "thinking"
                                          : a->run.phase == PH_WRITE   ? "writing"
                                          : a->run.phase == PH_COMPACT ? "compacting"
                                                                   : "working");
    app_secs_short(el, sizeof el, app_run_clock(a, now));
    if (in || out) {
        app_tokens_short(tin, sizeof tin, in);
        app_tokens_short(tout, sizeof tout, out);
        snprintf(tok, sizeof tok, " · %s %s %s %s", a->sy->up, tin, a->sy->down, tout);
    }
    pt_buf_reset(&a->tmp);
    tui_style(&a->tmp, ACCENT | TUI_BOLD);
    pt_buf_putz(&a->tmp, app_blink(a, now));
    tui_style(&a->tmp, 0);
    pt_buf_putc(&a->tmp, ' ');
    pt_buf_putz(&a->tmp, what);
    tui_style(&a->tmp, TUI_DIM);
    pt_buf_putz(&a->tmp, " · ");
    pt_buf_putz(&a->tmp, el);
    pt_buf_putz(&a->tmp, tok);
    tui_style(&a->tmp, 0);
    row(a, pt_buf_str(&a->tmp));
}

/* A choice in a list under the input, the selection marked. */
static void pick(app *a, bool on)
{
    pt_buf_reset(&a->tmp);
    pt_buf_putz(&a->tmp, "  ");
    tui_style(&a->tmp, ACCENT | TUI_BOLD);
    pt_buf_putz(&a->tmp, on ? a->sy->sel : " ");
    tui_style(&a->tmp, on ? TUI_BOLD : 0);
    pt_buf_putc(&a->tmp, ' ');
}

static void menu_row(app *a, int i)
{
    bool on = i == a->menu.sel;
    pick(a, on);
    if (a->menu.kind == MENU_SETTINGS && app_settings[i].k == SET_TOOLS) {
        uint32_t all, n = app_tools_on(a, &all);
        char v[600];
        snprintf(v, sizeof v, "%-14s", app_settings[i].name);
        pt_buf_putz(&a->tmp, v);
        tui_style(&a->tmp, TUI_CYAN);
        snprintf(v, sizeof v, "%u of %u on", n, all);
        pt_buf_putz(&a->tmp, v);
    } else if (a->menu.kind == MENU_SETTINGS) {
        int k = app_settings[i].k;
        char v[600];
        snprintf(v, sizeof v, "%-14s", app_settings[i].name);
        pt_buf_putz(&a->tmp, v);
        tui_style(&a->tmp, TUI_CYAN);
        pt_buf_putz(&a->tmp, a->cfg->val[k][0] ? a->cfg->val[k] : "none");
        if (strcmp(a->cfg->src[k], "default") != 0 && strcmp(a->cfg->src[k], "user") != 0) {
            tui_style(&a->tmp, TUI_DIM);
            snprintf(v, sizeof v, "  (set by %s)", a->cfg->src[k]);
            pt_buf_putz(&a->tmp, v);
        }
    } else if (a->menu.kind == MENU_TOOLS) { /* "● read   Read a text file, with line numbers" */
        pt_str name = app_item_label(a, i);
        bool tool_on = cli_tool_on(a->cfg, name);
        int col = 0, pad = a->menu.label_w - tui_width(name, true);
        tui_style(&a->tmp, tool_on ? TUI_GREEN | TUI_BOLD : TUI_DIM);
        pt_buf_putz(&a->tmp, tool_on ? a->sy->on : a->sy->off);
        tui_style(&a->tmp, tool_on ? (on ? TUI_BOLD : 0) : TUI_DIM);
        pt_buf_putc(&a->tmp, ' ');
        tui_clean(&a->tmp, name, &col);
        tui_style(&a->tmp, TUI_DIM);
        for (pad = pad > 0 ? pad + 2 : 2; pad > 0; pad--)
            pt_buf_putc(&a->tmp, ' ');
        tui_clean(&a->tmp, (pt_str){a->menu.buf.p + a->menu.items[i].dat, a->menu.items[i].dn}, &col);
    } else {
        int col = 0;
        if (a->menu.kind == MENU_CMD)
            pt_buf_putc(&a->tmp, '/');
        tui_clean(&a->tmp, app_item_label(a, i), &col);
        if (a->menu.items[i].dn) {
            int pad = a->menu.label_w - tui_width(app_item_label(a, i), true);
            tui_style(&a->tmp, TUI_DIM);
            for (pad = pad > 0 ? pad + 2 : 2; pad > 0; pad--)
                pt_buf_putc(&a->tmp, ' ');
            tui_clean(&a->tmp, (pt_str){a->menu.buf.p + a->menu.items[i].dat, a->menu.items[i].dn}, &col);
        }
    }
    row(a, pt_buf_str(&a->tmp));
}

/* The first tool call waiting for approval, if any. */
bool app_asking(app *a)
{
    if (!a->open || a->sess.opening || !pt_asks(a->s.a, &a->ask.view, 1))
        return false;
    if (a->ask.view.task != a->ask.task) { /* a new question */
        pt_str g = app_call_gist(a, a->ask.view.args);
        size_t n = 0;
        a->ask.task = a->ask.view.task;
        a->ask.sel = 0;
        a->ask.call_n = 0;
        /* bash: allow commands with the same first word; web_fetch: the
         * same host; others: the tool */
        if (pt_str_eqz(a->ask.view.name, "bash"))
            while (n < g.n && n + 1 < sizeof a->ask.prefix && !strchr(" \t\n;&|<>()`$", g.p[n]))
                n++;
        if (pt_str_eqz(a->ask.view.name, "web_fetch")) {
            g = pt_rt_url_host(g);
            n = g.n < sizeof a->ask.prefix ? g.n : 0;
        }
        memcpy(a->ask.prefix, g.p, n);
        a->ask.prefix[n] = 0;
    }
    return true;
}

bool app_ask_rule(const app *a, char *out, size_t cap)
{
    bool bash = pt_str_eqz(a->ask.view.name, "bash"), web = pt_str_eqz(a->ask.view.name, "web_fetch");
    int n;
    if ((bash || web) && !a->ask.prefix[0])
        return false;
    if (bash)
        n = snprintf(out, cap, "bash(%s:*)", a->ask.prefix);
    else if (web)
        n = snprintf(out, cap, "web_fetch(%s)", a->ask.prefix);
    else
        n = snprintf(out, cap, "%.*s", (int)a->ask.view.name.n, a->ask.view.name.p);
    return n > 0 && (size_t)n < cap;
}

static void ask_row(app *a, int i)
{
    char line[300], rule[200];
    pick(a, i == a->ask.sel);
    if (i == 0)
        snprintf(line, sizeof line, "1  yes");
    else if (i == 1 && a->ask.prefix[0])
        snprintf(line, sizeof line, "2  yes, and allow %.*s \"%s\" this session", (int)a->ask.view.name.n,
                 a->ask.view.name.p, a->ask.prefix);
    else if (i == 1)
        snprintf(line, sizeof line, "2  yes, and allow %.*s this session", (int)a->ask.view.name.n, a->ask.view.name.p);
    else if (i == 2)
        snprintf(line, sizeof line, "3  no (esc)");
    else if (app_ask_rule(a, rule, sizeof rule))
        snprintf(line, sizeof line, "4  yes, and always allow %.150s (saved to the config)", rule);
    else
        snprintf(line, sizeof line, "4  always allow: no rule fits this call");
    pt_buf_putz(&a->tmp, line);
    row(a, pt_buf_str(&a->tmp));
}

/* The call a question asks about, whole, wrapped to the width ("? bash
 * make test", its later rows under the gist), into ask.call: built once
 * for each question and width. */
static void ask_call(app *a)
{
    int col = 0;
    size_t k;
    if (a->ask.call_n && a->ask.call_w == width(a))
        return;
    pt_buf_reset(&a->tmp2);
    tui_style(&a->tmp2, ACCENT | TUI_BOLD);
    pt_buf_putz(&a->tmp2, a->sy->ask);
    tui_style(&a->tmp2, 0);
    pt_buf_putc(&a->tmp2, ' ');
    if (app_tool_of(a->ask.view.name) < 0 && a->ask.view.prompt.n) { /* another tool: its name and what it asks */
        size_t at = a->tmp2.n;
        tui_style(&a->tmp2, TUI_DIM);
        pt_buf_put(&a->tmp2, a->ask.view.name.p, a->ask.view.name.n > 60 ? 60 : a->ask.view.name.n);
        tui_style(&a->tmp2, 0);
        pt_buf_putc(&a->tmp2, ' ');
        tui_clean(&a->tmp2, a->ask.view.prompt, &col);
        for (k = at; k < a->tmp2.n; k++)
            if (a->tmp2.p[k] == '\n')
                a->tmp2.p[k] = ' ';
    } else {
        app_phrase(a, &a->tmp2, a->ask.view.name, a->ask.view.args, NULL, 0);
    }
    pt_buf_reset(&a->ask.call);
    a->ask.call_n = tui_wrap(&a->ask.call, pt_buf_str(&a->tmp2), width(a), MARGIN, false, false);
    a->ask.call_w = width(a);
}

/* The rows a question wants: a blank, the call, the four choices. */
static int ask_want(app *a)
{
    ask_call(a);
    return 5 + (a->ask.call_n < ASK_LINES ? a->ask.call_n : ASK_LINES);
}

/* The question in n rows: a blank row while there is room, the call
 * (when it is cut, its last row says how many more there are), then the
 * choices. */
static void ask_rows(app *a, int n)
{
    pt_str all = pt_buf_str(&a->ask.call);
    int i, choices = n - 1 < 4 ? n - 1 : 4, room = n - choices, total = a->ask.call_n, show;
    size_t at = 0;
    if (room > (total < ASK_LINES ? total : ASK_LINES)) {
        blank_row(a);
        room--;
    }
    show = total < room ? total : room;
    for (i = 0; i < show && at < all.n; i++) {
        size_t e = at;
        char more[64];
        while (e < all.n && all.p[e] != '\n')
            e++;
        if (i == show - 1 && show < total && show > 1) {
            pt_buf_reset(&a->tmp);
            pt_buf_putz(&a->tmp, "  ");
            tui_style(&a->tmp, TUI_DIM);
            snprintf(more, sizeof more, "%s %d more lines", a->sy->more, total - i);
            pt_buf_putz(&a->tmp, more);
            row(a, pt_buf_str(&a->tmp));
            break;
        }
        row(a, (pt_str){all.p + at, e - at});
        at = e + 1;
    }
    for (i = 0; i < choices; i++)
        ask_row(a, i);
}

/* Build the band (16.2.2) within its height budget. */
static void build_frame(app *a, int64_t now)
{
    int avail = a->rows - 2, w = width(a), show, live_n = 0, nmenu, nq, st, gap, act, i, first, top, nask, pad = 0;
    size_t secret = app_secret_at(pt_buf_str(&a->in.text));
    int total = a->menu.kind == MENU_SETTINGS ? (int)NSETTINGS : a->menu.n;
    int sel = a->menu.kind == MENU_SETTINGS ? a->menu.set_sel : a->menu.sel;
    bool tiny = a->cols < 20 || a->rows < 4;
    pt_buf *live_rows = &a->live_rows;
    tui_frame_reset(&a->frame);
    if (avail < 1)
        avail = 1;
    /* the pieces */
    pt_buf_reset(live_rows);
    if (a->group.n || a->run.ncalls) { /* the open group */
        pt_buf_putc(live_rows, '\n');
        app_group_render(a, live_rows, true);
    }
    if (a->view.live.n && (a->view.kind == LIVE_TEXT || (a->view.kind == LIVE_THINK && a->thinking == TH_FULL))) {
        tui_md copy = a->view.md;
        if (!a->view.started) /* where the message's blank line will be */
            pt_buf_putc(live_rows, '\n');
        tui_md_line(&copy, pt_buf_str(&a->view.live), a->cols, a->view.kind == LIVE_THINK ? TUI_DIM : 0, true,
                    live_rows);
    }
    for (i = 0; (size_t)i < live_rows->n; i++)
        live_n += live_rows->p[i] == '\n';
    a->ask.shown = app_asking(a);
    if (a->run.active && a->ask.shown && !a->run.wait_at) /* the clock stands still while the user decides */
        a->run.wait_at = now;
    else if (a->run.wait_at && !a->ask.shown) {
        a->run.at += now - a->run.wait_at;
        a->run.wait_at = 0;
    }
    nask = a->ask.shown ? ask_want(a) : 0;
    act = a->run.active ? 1 : 0;
    /* what waits: for the run, for the session to open, or for its fetches */
    nq = a->run.active || a->sess.opening ||
                 (a->queue.n && a->queue.items[0].xid && a->queue.items[0].got < a->queue.items[0].want)
             ? (int)(a->queue.n > 3 ? 4 : a->queue.n)
             : 0;
    nmenu = a->menu.kind == MENU_SETTINGS ? (int)NSETTINGS : a->menu.kind != MENU_NONE ? a->menu.n : 0;
    if (nmenu > 8)
        nmenu = 8;
    if (nask)
        nmenu = 0;
    st = gap = 1;
    show = a->rows / 3 < 8 ? a->rows / 3 : 8;
    if (show < 1)
        show = 1;
    tui_input_layout(&a->in, w > 3 ? w - 2 : 1, show);
    show = a->in.nrows < show ? a->in.nrows : show;
    if (tiny) {
        live_n = nq = nmenu = st = gap = act = 0;
        show = 1;
        nask = nask ? 1 : 0;
    }
    /* drop parts until it fits */
    while (live_n + nask + nq + gap + act + show + nmenu + st > avail) {
        int over = live_n + nask + nq + gap + act + show + nmenu + st - avail;
        if (nmenu) {
            nmenu = nmenu > over ? nmenu - over : 0;
        } else if (nq > 1) {
            nq = 1;
        } else if (live_n) {
            live_n = live_n > over ? live_n - over : 0;
        } else if (gap) {
            gap = 0;
        } else if (act) {
            act = 0;
        } else if (st) {
            st = 0;
        } else if (nq) {
            nq = 0;
        } else if (show > 1) {
            show = show > over + 1 ? show - over : 1;
        } else if (nask > 1) {
            nask = nask > over + 1 ? nask - over : 1;
        } else {
            break;
        }
    }
    tui_input_layout(&a->in, w > 3 ? w - 2 : 1, show);
    /* Rows above the input leave blanks behind (a call's output that ends,
     * the activity, a queued input that runs), so the input does not jump
     * up: they go only as the scrollback takes their place, at this commit.
     * Not after a resize or a redraw, which start the band over. */
    if (!tiny && a->band.h && !a->band.clear) {
        int committed = 0, room = avail - (live_n + nask + nq + gap + act + show + nmenu + st);
        for (i = 0; (size_t)i < a->lines.n; i++)
            committed += a->lines.p[i] == '\n';
        if (a->lines.n && a->lines.p[a->lines.n - 1] != '\n')
            committed++;
        pad = a->above - committed - (live_n + nask + nq + gap + act);
        if (pad > room)
            pad = room;
        if (pad < 0)
            pad = 0;
    }
    a->above = live_n + nask + nq + gap + act + pad;
    /* live tail: its last rows */
    {
        pt_str all = pt_buf_str(live_rows);
        size_t at = 0;
        int nlive = 0, skip;
        for (i = 0; (size_t)i < all.n; i++)
            nlive += all.p[i] == '\n';
        skip = nlive - live_n;
        while (at < all.n) {
            size_t e = at;
            while (e < all.n && all.p[e] != '\n')
                e++;
            if (skip-- <= 0)
                row(a, (pt_str){all.p + at, e - at});
            at = e + 1;
        }
    }
    if (nask)
        ask_rows(a, nask);
    for (i = 0; i < nq; i++) {
        char more[48];
        pt_buf_reset(&a->tmp);
        tui_style(&a->tmp, TUI_DIM);
        if (i == nq - 1 && (uint32_t)nq < a->queue.n) {
            snprintf(more, sizeof more, "  +%u more queued", a->queue.n - (uint32_t)i);
            pt_buf_putz(&a->tmp, more);
        } else {
            int col = 0;
            pt_str q = pt_buf_str(&a->queue.items[i].text);
            long nl = pt_str_find(q, '\n');
            pt_buf_putz(&a->tmp, a->queue.items[i].xid && a->queue.items[i].got < a->queue.items[i].want ? "  fetching · "
                                                                                       : "  queued · ");
            if (nl >= 0)
                q.n = (size_t)nl;
            tui_clean(&a->tmp, q, &col);
        }
        row(a, pt_buf_str(&a->tmp));
    }
    /* the input box after the blanks, a blank row and the activity, the
     * prompt's mark on its first row */
    for (i = 0; i < pad; i++)
        blank_row(a);
    if (gap)
        blank_row(a);
    if (act)
        activity_row(a, now);
    first = a->frame.n;
    top = a->in.scroll;
    for (i = top; i < a->in.nrows && i < top + show; i++) {
        const tui_irow *r = &a->in.rows[i];
        size_t pre;
        pt_buf_reset(&a->tmp);
        tui_style(&a->tmp, ACCENT | TUI_BOLD);
        pt_buf_putz(&a->tmp, i == 0 ? a->sy->prompt : " ");
        tui_style(&a->tmp, 0);
        pt_buf_putc(&a->tmp, ' ');
        pre = a->tmp.n;
        pt_buf_put(&a->tmp, a->in.text.p + r->start, r->end - r->start);
        if (secret && !a->tmp.oom) { /* same width: the key is printable ASCII */
            size_t j;
            for (j = r->start > secret ? r->start : secret; j < r->end; j++)
                if (a->in.text.p[j] > ' ' && a->in.text.p[j] < 0x7f)
                    a->tmp.p[pre + j - r->start] = '*';
        }
        row(a, pt_buf_str(&a->tmp));
        if (i == a->in.cur_row) {
            a->frame.cur_row = a->frame.n - 1;
            a->frame.cur_at = pre + (a->in.cur - r->start);
        }
    }
    if (a->frame.n == first) { /* nothing laid out: an empty prompt */
        pt_buf_reset(&a->tmp);
        tui_style(&a->tmp, ACCENT | TUI_BOLD);
        pt_buf_putz(&a->tmp, a->sy->prompt);
        tui_style(&a->tmp, 0);
        pt_buf_putc(&a->tmp, ' ');
        row(a, pt_buf_str(&a->tmp));
        a->frame.cur_row = a->frame.n - 1;
        a->frame.cur_at = a->tmp.n;
    }
    /* the menu: a window around the selection */
    if (nmenu) {
        int start, save = a->menu.sel;
        if (sel < a->menu.top)
            a->menu.top = sel;
        if (sel >= a->menu.top + nmenu)
            a->menu.top = sel - nmenu + 1;
        if (a->menu.top > total - nmenu)
            a->menu.top = total - nmenu > 0 ? total - nmenu : 0;
        start = a->menu.top;
        a->menu.sel = sel;
        a->menu.label_w = 0;
        for (i = start; a->menu.kind != MENU_SETTINGS && i < start + nmenu && i < total; i++) {
            int lw = tui_width(app_item_label(a, i), true);
            a->menu.label_w = lw > a->menu.label_w ? (lw > 40 ? 40 : lw) : a->menu.label_w;
        }
        for (i = start; i < start + nmenu && i < total; i++)
            menu_row(a, i);
        a->menu.sel = save;
    }
    if (st)
        status_row(a, now, total, sel);
}

void app_flush_out(app *a)
{
    int ms = tui_term_write(a->t, pt_buf_str(&a->band.out));
    pt_buf_reset(&a->band.out);
    /* writes that block (a slow link) slow the stream down */
    if (ms > 50 && a->pace.interval < 500)
        a->pace.interval *= 2;
    else if (ms < 10 && a->pace.interval > 33)
        a->pace.interval /= 2;
}

void app_paint(app *a)
{
    int64_t now = pt_sys_now_ms();
    build_frame(a, now);
    if (a->in.text.oom || a->in.kill.oom || a->in.saved.oom || a->frame.rows.oom || a->lines.oom ||
        a->view.live.oom || a->tmp.oom || a->tmp2.oom || a->wrap.oom || a->menu.buf.oom || a->group.buf.oom ||
        app_runs_oom(a) || a->live_rows.oom) {
        if (!a->quit)
            fprintf(stderr, "pytte: out of memory drawing the terminal\n");
        a->code = 1;
        a->quit = true;
        return;
    }
    if (a->lines.n)
        tui_band_commit(&a->band, pt_buf_str(&a->lines), &a->frame);
    else
        tui_band_paint(&a->band, &a->frame);
    if (a->band.out.oom || a->band.drawn.oom) {
        fprintf(stderr, "pytte: out of memory drawing the terminal\n");
        a->code = 1;
        a->quit = true;
        return;
    }
    pt_buf_reset(&a->lines);
    app_flush_out(a);
    a->pace.dirty = a->pace.urgent = false;
    a->pace.last_paint = now;
}
