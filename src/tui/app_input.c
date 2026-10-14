/* What the user types: keys, the queue of inputs waiting for the run,
 * and the MCP prompts and @-mentions an input waits for before it goes. */
#define _POSIX_C_SOURCE 200809L
#include "tui/app.h"

static bool queue_text(app *a, pt_str text, bool command)
{
    queued_input *q;
    if (a->queue.n == a->queue.cap) {
        uint32_t cap = a->queue.cap ? a->queue.cap * 2 : 8, i;
        queued_input *next;
        if (cap < a->queue.cap || SIZE_MAX / (size_t)cap < sizeof *next)
            return false;
        next = (queued_input *)realloc(a->queue.items, (size_t)cap * sizeof *next);
        if (!next)
            return false;
        for (i = a->queue.cap; i < cap; i++) {
            int k;
            memset(&next[i], 0, sizeof next[i]);
            pt_buf_init(&next[i].text, pt_sys_allocator());
            pt_buf_init(&next[i].names, pt_sys_allocator());
            for (k = 0; k < XPARTS; k++)
                pt_buf_init(&next[i].parts[k], pt_sys_allocator());
        }
        a->queue.items = next;
        a->queue.cap = cap;
    }
    q = &a->queue.items[a->queue.n];
    pt_buf_reset(&q->text);
    pt_buf_puts(&q->text, text);
    if (q->text.oom)
        return false;
    q->command = command;
    q->submitted = false;
    q->xid = 0;
    q->want = q->got = 0;
    q->failed = q->prompt = false;
    q->err[0] = 0;
    pt_buf_reset(&q->names);
    {
        int k;
        for (k = 0; k < XPARTS; k++)
            pt_buf_reset(&q->parts[k]);
    }
    a->queue.n++;
    return true;
}

static void queued_remove(app *a, uint32_t at)
{
    queued_input kept = a->queue.items[at];
    memmove(a->queue.items + at, a->queue.items + at + 1, (a->queue.n - at - 1) * sizeof *a->queue.items);
    a->queue.items[--a->queue.n] = kept;
    pt_buf_reset(&a->queue.items[a->queue.n].text);
}

static void x_done(void *ud, const char *error, pt_str text)
{
    xreq *x = (xreq *)ud, **pp;
    app *a = x->a;
    uint32_t i;
    for (pp = &a->queue.xs; *pp; pp = &(*pp)->next)
        if (*pp == x) {
            *pp = x->next;
            break;
        }
    for (i = 0; i < a->queue.n; i++) {
        queued_input *q = &a->queue.items[i];
        if (q->xid != x->id)
            continue;
        if (error && !q->failed) {
            q->failed = true;
            snprintf(q->err, sizeof q->err, "%s: %s", x->k == 0 && q->prompt ? "the MCP prompt" : "the mention",
                     error);
        } else if (!error) {
            pt_buf_puts(&q->parts[x->k], text);
        }
        if (++q->got >= q->want)
            a->queue.expanded = true;
        break;
    }
    free(x);
}

static xreq *xreq_new(app *a, queued_input *q, uint32_t k)
{
    xreq *x = (xreq *)calloc(1, sizeof *x);
    if (!x)
        return NULL;
    x->a = a;
    x->id = q->xid;
    x->k = k;
    x->next = a->queue.xs;
    a->queue.xs = x;
    return x;
}

void xreq_drop(app *a, xreq *x)
{
    xreq **pp;
    for (pp = &a->queue.xs; *pp; pp = &(*pp)->next)
        if (*pp == x) {
            *pp = x->next;
            break;
        }
    free(x);
}

/* "/server:prompt ..." for a configured server: an MCP prompt, not a command. */
static bool names_prompt(app *a, pt_str text)
{
    size_t i;
    long colon;
    if (!text.n || text.p[0] != '/')
        return false;
    for (i = 1; i < text.n && text.p[i] != ' ' && text.p[i] != '\n'; i++)
        ;
    colon = pt_str_find((pt_str){text.p + 1, i - 1}, ':');
    return colon > 0 && mcp_known(a, (pt_str){text.p + 1, (size_t)colon});
}

/* The prompt's arguments from what follows its command: name=value, or
 * by position; the last takes the rest of the text. */
static bool prompt_args(const pt_rt_mcp_prompt *p, pt_str rest, pt_buf *json, char *err, size_t cap)
{
    pt_str val[16];
    uint32_t i, k, nargs = p->nargs < 16 ? p->nargs : 16;
    size_t at = 0;
    memset(val, 0, sizeof val);
    rest = pt_str_trim(rest);
    /* name=value words first */
    while (at < rest.n) {
        size_t e = at, eq;
        bool named = false;
        while (e < rest.n && rest.p[e] != ' ' && rest.p[e] != '\n')
            e++;
        for (eq = at; eq < e && rest.p[eq] != '='; eq++)
            ;
        for (k = 0; eq < e && k < nargs; k++)
            if (pt_str_eq(p->args[k].name, (pt_str){rest.p + at, eq - at})) {
                pt_str v = {rest.p + eq + 1, e - eq - 1};
                if (v.n >= 2 && v.p[0] == '"' && v.p[v.n - 1] == '"')
                    v = (pt_str){v.p + 1, v.n - 2};
                val[k] = v;
                val[k].p = val[k].p ? val[k].p : "";
                named = true;
            }
        if (!named)
            break;
        at = e;
        while (at < rest.n && (rest.p[at] == ' ' || rest.p[at] == '\n'))
            at++;
    }
    /* then by position */
    for (k = 0; k < nargs && at < rest.n; k++) {
        size_t e = at;
        uint32_t later = 0;
        if (val[k].p)
            continue;
        for (i = k + 1; i < nargs; i++)
            later += !val[i].p;
        if (!later) { /* the last: the rest */
            e = rest.n;
        } else if (rest.p[at] == '"') {
            for (e = at + 1; e < rest.n && rest.p[e] != '"'; e++)
                ;
            val[k] = (pt_str){rest.p + at + 1, e - at - 1};
            at = e < rest.n ? e + 1 : e;
            while (at < rest.n && (rest.p[at] == ' ' || rest.p[at] == '\n'))
                at++;
            continue;
        } else {
            while (e < rest.n && rest.p[e] != ' ' && rest.p[e] != '\n')
                e++;
        }
        val[k] = (pt_str){rest.p + at, e - at};
        if (!later && val[k].n >= 2 && val[k].p[0] == '"' && val[k].p[val[k].n - 1] == '"')
            val[k] = (pt_str){val[k].p + 1, val[k].n - 2};
        at = e;
        while (at < rest.n && (rest.p[at] == ' ' || rest.p[at] == '\n'))
            at++;
    }
    if (at < rest.n) {
        snprintf(err, cap, "/%.*s:%.*s takes %u argument%s", (int)p->server.n, p->server.p, (int)p->name.n,
                 p->name.p, nargs, nargs == 1 ? "" : "s");
        return false;
    }
    pt_buf_putc(json, '{');
    for (k = 0; k < nargs; k++) {
        if (!val[k].p) {
            if (p->args[k].required) {
                snprintf(err, cap, "/%.*s:%.*s needs %.*s", (int)p->server.n, p->server.p, (int)p->name.n, p->name.p,
                         (int)p->args[k].name.n, p->args[k].name.p);
                return false;
            }
            continue;
        }
        if (json->n > 1)
            pt_buf_putc(json, ',');
        pt_json_quote(json, p->args[k].name);
        pt_buf_putc(json, ':');
        pt_json_quote(json, val[k]);
    }
    pt_buf_putc(json, '}');
    return !json->oom;
}

/* Ask for what q names: its prompt and its mentions. 0: nothing (or
 * asked: see want), -1: q->err says why it cannot go. */
static int expand_start(app *a, queued_input *q)
{
    pt_str t = pt_buf_str(&q->text);
    pt_rt_mcp_info in[CLI_MCP_MAX];
    uint32_t ns = a->open && a->mcp ? pt_rt_mcp_servers(a->s.rt, in, CLI_MCP_MAX) : 0, i;
    size_t at;
    q->xid = ++a->queue.next_xid;
    q->want = q->got = 0;
    if (ns > CLI_MCP_MAX)
        ns = CLI_MCP_MAX;
    if (names_prompt(a, t)) {
        const pt_rt_mcp_prompt *p = find_prompt(a, t);
        size_t e;
        pt_buf args;
        xreq *x;
        bool ok;
        for (e = 1; e < t.n && t.p[e] != ' ' && t.p[e] != '\n'; e++)
            ;
        if (!p) {
            long colon = pt_str_find((pt_str){t.p + 1, e - 1}, ':');
            const char *why = "has no such prompt";
            for (i = 0; i < ns; i++)
                if (pt_str_eqz((pt_str){t.p + 1, (size_t)colon}, in[i].name) && in[i].state != PT_MCP_READY)
                    why = "is not ready (/mcp says why)";
            snprintf(q->err, sizeof q->err, "the MCP server %.*s %s: %.*s", (int)colon, t.p + 1, why,
                     (int)(e > 100 ? 100 : e), t.p);
            return -1;
        }
        pt_buf_init(&args, pt_sys_allocator());
        if (!prompt_args(p, (pt_str){t.p + e, t.n - e}, &args, q->err, sizeof q->err)) {
            pt_buf_free(&args);
            return -1;
        }
        x = xreq_new(a, q, 0);
        ok = x && pt_rt_mcp_get_prompt(a->s.rt, p->server, p->name, pt_buf_str(&args), x_done, x);
        pt_buf_free(&args);
        if (!ok) {
            if (x)
                xreq_drop(a, x);
            snprintf(q->err, sizeof q->err, "cannot ask the MCP server for the prompt");
            return -1;
        }
        q->prompt = true;
        q->want = 1;
    }
    /* @server:uri, at a word's start, for a ready server */
    for (at = 0; at < t.n; at++) {
        size_t e;
        long colon;
        pt_str tok, server, uri;
        if (t.p[at] != '@' || (at && t.p[at - 1] != ' ' && t.p[at - 1] != '\n' && t.p[at - 1] != '\t'))
            continue;
        for (e = at + 1; e < t.n && t.p[e] != ' ' && t.p[e] != '\n' && t.p[e] != '\t'; e++)
            ;
        tok = (pt_str){t.p + at + 1, e - at - 1};
        while (tok.n && strchr(".,;!?)", tok.p[tok.n - 1]))
            tok.n--;
        if ((colon = pt_str_find(tok, ':')) <= 0)
            continue;
        server = (pt_str){tok.p, (size_t)colon};
        uri = (pt_str){tok.p + colon + 1, tok.n - (size_t)colon - 1};
        for (i = 0; i < ns && !pt_str_eqz(server, in[i].name); i++)
            ;
        if (i == ns || !uri.n)
            continue;
        if (in[i].state != PT_MCP_READY) {
            snprintf(q->err, sizeof q->err, "the MCP server %s is not ready (/mcp says why): @%.*s", in[i].name,
                     (int)(tok.n > 200 ? 200 : tok.n), tok.p);
            return -1;
        }
        {
            xreq *x;
            pt_buf_reset(&a->tmp2);
            pt_buf_puts(&a->tmp2, server);
            pt_buf_putc(&a->tmp2, '\t');
            pt_buf_puts(&a->tmp2, uri);
            pt_buf_putc(&a->tmp2, '\n');
            if (pt_str_contains(pt_buf_str(&q->names), pt_buf_str(&a->tmp2)))
                continue; /* named twice: once */
            if (q->want == XPARTS) {
                snprintf(q->err, sizeof q->err, "at most %d mentions in a message", XPARTS - 1);
                return -1;
            }
            pt_buf_puts(&q->names, pt_buf_str(&a->tmp2));
            x = xreq_new(a, q, q->want);
            if (!x || !pt_rt_mcp_read(a->s.rt, server, uri, x_done, x)) {
                if (x)
                    xreq_drop(a, x);
                snprintf(q->err, sizeof q->err, "cannot ask the MCP server for @%.*s",
                         (int)(tok.n > 200 ? 200 : tok.n), tok.p);
                return -1;
            }
            q->want++;
        }
    }
    return 0;
}

/* What q sends: base (the prompt's text, or what was typed), then each
 * mention's contents. */
static void expand_text(queued_input *q, pt_str base, pt_buf *out)
{
    uint32_t k = q->prompt ? 1 : 0;
    pt_str names = pt_buf_str(&q->names);
    size_t at = 0;
    pt_buf_puts(out, base);
    for (; k < q->want && at < names.n; k++) {
        size_t e = at, tab;
        while (e < names.n && names.p[e] != '\n')
            e++;
        for (tab = at; tab < e && names.p[tab] != '\t'; tab++)
            ;
        pt_buf_putz(out, "\n\n<resource server=\"");
        pt_buf_put(out, names.p + at, tab - at);
        pt_buf_putz(out, "\" uri=\"");
        pt_buf_put(out, names.p + tab + 1, e - tab - 1);
        pt_buf_putz(out, "\">\n");
        pt_buf_puts(out, pt_buf_str(&q->parts[k]));
        pt_buf_putz(out, "\n</resource>");
        at = e + 1;
    }
}

void queued_submit(app *a)
{
    uint32_t i = 0;
    while (a->open && i < a->queue.n) {
        queued_input *q = &a->queue.items[i];
        if (q->submitted || (a->quit && q->command)) {
            i++;
        } else if (q->command) {
            pt_buf_reset(&a->command);
            pt_buf_puts(&a->command, pt_buf_str(&q->text));
            if (a->command.oom) {
                out_z(a, TUI_RED, "out of memory queuing a command");
                return;
            }
            queued_remove(a, i);
            run_command(a, pt_buf_str(&a->command));
            menu_update(a);
        } else {
            pt_input in;
            pt_buf text, full;
            char err[600];
            bool ok;
            if (!q->xid && expand_start(a, q) < 0) {
                out_z(a, TUI_RED, q->err);
                queued_remove(a, i);
                continue;
            }
            if (q->got < q->want)
                break; /* what comes after it waits for its fetches */
            if (q->failed) {
                out_z(a, TUI_RED, q->err);
                queued_remove(a, i);
                continue;
            }
            memset(&in, 0, sizeof in);
            in.text = pt_buf_str(&q->text);
            pt_buf_init(&full, pt_sys_allocator());
            pt_buf_init(&text, pt_sys_allocator());
            /* skills in what was typed; not in what servers sent */
            switch (q->prompt ? 0 : pt_rt_expand_skills(a->s.rt, in.text, &text, err, sizeof err)) {
            case -1: /* not sent: it is still in the history */
                out_z(a, TUI_RED, err);
                queued_remove(a, i);
                pt_buf_free(&text);
                pt_buf_free(&full);
                continue;
            case 1:
                in.text = pt_buf_str(&text);
                break;
            }
            if (q->want) {
                expand_text(q, q->prompt ? pt_buf_str(&q->parts[0]) : in.text, &full);
                in.text = pt_buf_str(&full);
            }
            ok = !full.oom && pt_submit(a->s.a, &in);
            pt_buf_free(&text);
            pt_buf_free(&full);
            if (!ok) {
                out_z(a, TUI_RED, pt_error(a->s.a));
                return;
            }
            if (!a->sess.named) { /* the first prompt names the session */
                cli_session_name_set(a->s.dir, pt_buf_str(&q->text));
                a->sess.named = true;
            }
            q->submitted = true;
            i++;
        }
    }
}

void submit(app *a)
{
    pt_str text = pt_str_trim(pt_buf_str(&a->in.text));
    bool command;
    if (!text.n)
        return;
    if (secret_at(text)) {
        /* the key stays out of the history */
    } else if (a->hist.ready) {
        if (!tui_hist_add(&a->in, text))
            out_z(a, TUI_RED, "out of memory adding to input history");
        history_append(a, text);
    } else {
        tui_hist_line(&a->hist.pending, text);
    }
    command = text.p[0] == '/' && pt_str_find(text, '\n') < 0 && !pt_str_starts(text, PT_S("/skill:")) &&
              !names_prompt(a, text);
    if (command && (!a->sess.opening || secret_at(text))) { /* a key is never queued */
        pt_buf_reset(&a->command);
        pt_buf_puts(&a->command, text);
        if (a->command.oom) {
            out_z(a, TUI_RED, "out of memory queuing a command");
            return;
        }
        tui_input_set(&a->in, PT_STR_NULL);
        a->menu.kind = MENU_NONE;
        a->menu.closed = false;
        run_command(a, pt_buf_str(&a->command));
        menu_update(a);
        a->draft.dirty = true;
        return;
    }
    if (!a->open && !a->sess.opening) {
        out_z(a, TUI_RED, "no session");
        return;
    }
    if (!queue_text(a, text, command)) {
        out_z(a, TUI_RED, "out of memory queuing input");
        return;
    }
    tui_input_set(&a->in, PT_STR_NULL);
    queued_submit(a);
    a->draft.dirty = true;
}

static void enter(app *a)
{
    pt_str d = pt_buf_str(&a->in.text);
    if (d.n && d.p[d.n - 1] == '\\' && a->in.cur == d.n) { /* a trailing \ is a newline */
        a->in.text.n--;
        a->in.cur--;
        tui_input_insert(&a->in, PT_S("\n"));
        return;
    }
    if (a->menu.kind == MENU_CMD && a->menu.n) {
        pt_str label = item_label(a, a->menu.sel);
        const pt_rt_mcp_prompt *p = find_prompt(a, label);
        uint32_t k;
        bool needs = false;
        for (k = 0; p && k < p->nargs; k++)
            needs = needs || p->args[k].required;
        if ((menu_takes_arg(label) && !pt_str_eqz(label, "settings")) || needs) {
            /* /model and friends: show their arguments first */
            if (complete(a))
                return;
        } else {
            complete(a);
        }
    } else if (a->menu.kind == MENU_ARG && a->menu.n) {
        bool skill = a->menu.of_skills || a->menu.of_mentions;
        pt_str label = item_label(a, a->menu.sel);
        bool sub = pt_str_starts(d, PT_S("/mcp ")) && (pt_str_eqz(label, "login") || pt_str_eqz(label, "logout") ||
                                                      pt_str_eqz(label, "restart"));
        complete(a);
        if (skill || sub)
            return; /* a $skill or @mention picked: the message goes on; /mcp login: then the name */
    }
    submit(a);
}

static void settings_key(app *a, const tui_ev *ev)
{
    int k = app_settings[a->menu.set_sel].k, i, dir = ev->key == TK_LEFT ? -1 : 1;
    const char *const *vals = app_settings[a->menu.set_sel].vals;
    int nv = app_settings[a->menu.set_sel].nv;
    char d[600];
    if (ev->key == TK_UP || ev->key == TK_DOWN) {
        a->menu.set_sel = (a->menu.set_sel + (ev->key == TK_UP ? -1 : 1) + (int)NSETTINGS) % (int)NSETTINGS;
        return;
    }
    if (ev->key == TK_ESC || (ev->key == 'c' && ev->mods == TM_CTRL)) {
        a->menu.kind = MENU_NONE;
        return;
    }
    if (ev->key != TK_LEFT && ev->key != TK_RIGHT && ev->key != TK_ENTER)
        return;
    if (vals) {
        for (i = 0; i < nv && strcmp(a->cfg->val[k], vals[i]) != 0; i++)
            ;
        save_setting(a, k, vals[((i < nv ? i : 0) + dir + nv) % nv]);
    } else { /* model, pager, compact-model: edit as a command */
        snprintf(d, sizeof d, "/settings %s %s", app_settings[a->menu.set_sel].name, k == CFG_PAGER ? a->cfg->val[k] : "");
        a->menu.kind = MENU_NONE;
        a->menu.closed = false;
        tui_input_set(&a->in, pt_strz(d));
        menu_update(a);
    }
}

static void interrupt(app *a)
{
    if (a->open && pt_abort(a->s.a))
        a->pace.esc_at = 0;
}

/* Answer the question: 0 yes, 1 yes and allow alike, 2 no. */
static void answer(app *a, int choice)
{
    if (choice == 1 && !pt_rt_allow(a->s.rt, a->ask.view.name, pt_strz(a->ask.prefix)))
        out_z(a, TUI_RED, "out of memory: allowed this once only");
    if (!pt_approve(a->s.a, a->ask.view.task, choice != 2, PT_STR_NULL))
        out_z(a, TUI_RED, pt_error(a->s.a));
    a->ask.task = 0;
    set_dirty(a);
}

/* Keys while a tool call waits: true when taken. */
static bool ask_key(app *a, const tui_ev *ev)
{
    if (!asking(a))
        return false;
    if (ev->kind == TE_TEXT) {
        if (ev->text.n == 1 && ev->text.p[0] >= '1' && ev->text.p[0] <= '3')
            answer(a, ev->text.p[0] - '1');
        return true; /* typing waits for the answer */
    }
    if (ev->kind != TE_KEY || (ev->mods && ev->key != TK_ENTER))
        return false;
    if (ev->key == TK_UP || ev->key == TK_DOWN)
        a->ask.sel = (a->ask.sel + (ev->key == TK_UP ? 2 : 1)) % 3;
    else if (ev->key == TK_ENTER)
        answer(a, a->ask.sel);
    else if (ev->key == TK_ESC)
        answer(a, 2);
    else
        return false;
    return true;
}

void on_key(app *a, const tui_ev *ev)
{
    int64_t now = pt_sys_now_ms();
    uint32_t k = ev->key;
    bool ctrl = ev->mods == TM_CTRL;
    tui_input_result r;
    size_t before = a->in.text.n;
    a->pace.urgent = true;
    if (ask_key(a, ev))
        return;
    if (ev->kind == TE_TEXT || ev->kind == TE_PASTE) {
        if (a->menu.kind == MENU_SETTINGS)
            a->menu.kind = MENU_NONE;
        tui_input_insert(&a->in, ev->text);
        a->menu.closed = false;
        a->draft.dirty = true;
        menu_update(a);
        return;
    }
    if (ev->kind != TE_KEY)
        return;
    if (a->menu.kind == MENU_SETTINGS) {
        settings_key(a, ev);
        return;
    }
    if (k == TK_ENTER && !ev->mods) {
        enter(a);
        return;
    }
    if (k == TK_TAB && !ev->mods) {
        complete(a);
        return;
    }
    if (k == TK_ESC) {
        if (a->menu.kind != MENU_NONE) {
            a->menu.closed = true;
            menu_update(a);
        } else if (a->run.active) {
            if (now - a->pace.esc_at < DOUBLE_MS)
                interrupt(a);
            else
                a->pace.esc_at = now;
        }
        return;
    }
    if (ctrl && k == 'c') {
        if (a->sess.opening)
            a->draft.drop_restored = true;
        if (a->in.text.n) {
            tui_input_set(&a->in, PT_STR_NULL);
            a->draft.dirty = true;
            menu_update(a);
        } else if (a->run.active) {
            interrupt(a);
        } else if (now - a->pace.ctrlc_at < DOUBLE_MS) {
            a->quit = true;
        } else {
            a->pace.ctrlc_at = now;
        }
        return;
    }
    if (ctrl && k == 'd' && !a->in.text.n) {
        a->quit = true;
        return;
    }
    if (ctrl && k == 'l') {
        tui_band_redraw(&a->band);
        return;
    }
    if (ctrl && k == 'z') {
        suspend(a);
        tui_term_stop_self();
        resume(a);
        return;
    }
    if (ctrl && k == 't') {
        save_setting(a, CFG_THINKING, app_th_names[(a->thinking + 1) % NTH]);
        return;
    }
    if (ctrl && k == 'o') {
        if (a->sess.opening)
            a->pager_pending = true;
        else
            pager(a);
        return;
    }
    if ((k == TK_UP || k == TK_DOWN) && !ev->mods && a->menu.kind != MENU_NONE && a->menu.n) {
        a->menu.sel = (a->menu.sel + (k == TK_UP ? -1 : 1) + a->menu.n) % a->menu.n;
        return;
    }
    r = tui_input_key(&a->in, ev, width(a) > 3 ? width(a) - 2 : 1);
    if (r == TI_TOP || r == TI_BOTTOM) {
        if (tui_hist_move(&a->in, r == TI_TOP ? -1 : 1)) {
            a->menu.closed = true; /* a recalled command does not open the menu */
            menu_update(a);
        }
        return;
    }
    if (r == TI_EDITED || a->in.text.n != before) {
        a->menu.closed = false;
        a->draft.dirty = true;
        menu_update(a);
    }
}
