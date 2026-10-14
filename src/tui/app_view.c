/* The TUI's scrollback output and its display model, built from the
 * agent's events: entries, tool-call groups, and the streaming block,
 * committed one complete source line at a time (section 16.2). */
#define _POSIX_C_SOURCE 200809L
#include "tui/app.h"

/* Clean outside text, then each of its lines: style, the prefix on the
 * first line (indent spaces on the others), word-wrapped with a hanging
 * indent. */
void app_out_text(app *a, pt_buf *out, int style, const char *prefix, int pstyle, pt_str raw, int indent)
{
    pt_str s;
    size_t at = 0;
    int col = 0;
    bool first = true;
    pt_buf_reset(&a->tmp);
    tui_clean(&a->tmp, raw, &col);
    s = pt_buf_str(&a->tmp);
    if (s.n && s.p[s.n - 1] == '\n')
        s.n--;
    do {
        size_t e = at;
        int i;
        while (e < s.n && s.p[e] != '\n')
            e++;
        pt_buf_reset(&a->tmp2);
        if (first && prefix) {
            tui_style(&a->tmp2, pstyle);
            pt_buf_putz(&a->tmp2, prefix);
        } else {
            for (i = 0; i < indent; i++)
                pt_buf_putc(&a->tmp2, ' ');
        }
        tui_style(&a->tmp2, style);
        pt_buf_put(&a->tmp2, s.p + at, e - at);
        tui_wrap(out, pt_buf_str(&a->tmp2), width(a), indent, true, false);
        first = false;
        at = e + 1;
    } while (at <= s.n);
}

/* Outside text on a rail: every row, wrapped ones too, starts with the
 * rail and a space. At most `max` rows (0: all), then one that says how
 * many more there are. */
static void out_rail(app *a, pt_buf *out, const char *rail, int rstyle, int style, pt_str raw, int max)
{
    pt_str s, rows;
    size_t at = 0;
    int col = 0, total = 0, n = 0, w = width(a) - tui_width(pt_strz(rail), true) - 1;
    pt_buf_reset(&a->tmp);
    tui_clean(&a->tmp, raw, &col);
    s = pt_buf_str(&a->tmp);
    while (s.n && s.p[s.n - 1] == '\n')
        s.n--;
    pt_buf_reset(&a->wrap);
    do {
        size_t e = at;
        while (e < s.n && s.p[e] != '\n')
            e++;
        pt_buf_reset(&a->tmp2);
        tui_style(&a->tmp2, style);
        pt_buf_put(&a->tmp2, s.p + at, e - at);
        total += tui_wrap(&a->wrap, pt_buf_str(&a->tmp2), w > 1 ? w : 1, 0, true, false);
        at = e + 1;
    } while (at <= s.n);
    rows = pt_buf_str(&a->wrap);
    at = 0;
    while (at < rows.n) {
        size_t e = at;
        char more[64];
        while (e < rows.n && rows.p[e] != '\n')
            e++;
        tui_style(out, rstyle);
        pt_buf_putz(out, rail);
        pt_buf_putc(out, ' ');
        if (max && n == max && total > max + 1) {
            tui_style(out, TUI_DIM);
            snprintf(more, sizeof more, "%s %d more lines (ctrl+o)", a->sy->more, total - n);
            pt_buf_putz(out, more);
            pt_buf_putc(out, '\n');
            break;
        }
        pt_buf_put(out, rows.p + at, e + 1 - at);
        n++;
        at = e + 1;
    }
}

/* A line from pytte itself, after its mark: a dim note, or a red failure. */
void app_out_note(app *a, pt_buf *out, int style, pt_str raw)
{
    char mark[16];
    snprintf(mark, sizeof mark, "%s ", style & TUI_RED ? a->sy->fail : a->sy->note);
    app_out_text(a, out, style, mark, style, raw, MARGIN);
}

void app_out_z(app *a, int style, const char *z)
{
    app_group_close(a);
    app_out_note(a, &a->lines, style, pt_strz(z));
}

void app_out_blank(pt_buf *out)
{
    pt_buf_putc(out, '\n');
}

/* Markdown, one complete line at a time (the lines of text need not end
 * with a newline). */
static void out_md(app *a, pt_buf *out, tui_md *m, pt_str raw, int base)
{
    size_t at = 0;
    int col = 0;
    pt_str s;
    pt_buf_reset(&a->tmp2);
    tui_clean(&a->tmp2, raw, &col);
    s = pt_buf_str(&a->tmp2);
    while (at < s.n) {
        size_t e = at;
        while (e < s.n && s.p[e] != '\n')
            e++;
        tui_md_line(m, (pt_str){s.p + at, e - at}, a->cols, base, false, out);
        at = e + 1;
    }
}

/* The gist of a call's arguments: its first string member ("ls -la",
 * "src/a.c"), else the JSON as it came. */
pt_str app_call_gist(app *a, pt_str args)
{
    const pt_json *j, *c;
    int64_t session;
    pt_arena_reset(&a->ar);
    j = pt_json_parse(args, &a->ar, NULL);
    if (pt_json_int(pt_json_get(j, "session"), &session)) { /* bash_poll */
        char *g = (char *)pt_arena_alloc(&a->ar, 48);
        if (g) {
            snprintf(g, 48, "session %lld%s", (long long)session,
                     pt_json_truthy(pt_json_get(j, "stop")) ? " (stop)" : "");
            return pt_strz(g);
        }
    }
    if (j && j->type == PT_J_OBJ)
        for (c = j->child; c; c = c->next)
            if (c->type == PT_J_STR)
                return pt_json_str(c, &a->ar);
    return args;
}

/* A call with all of its arguments, for the full view: "· name {...}". */
static void out_call(app *a, pt_buf *out, const pt_block *b)
{
    char head[160];
    snprintf(head, sizeof head, "%s %.*s ", a->sy->note, (int)(b->name.n > 60 ? 60 : b->name.n), b->name.p);
    app_out_text(a, out, 0, head, TUI_BOLD, b->args, MARGIN);
}

/* The builtin tools: the name shown in a row, and the kind. */
static const struct {
    const char *name, *shown;
    int cat;
} tools[] = {
    {"bash", "bash", CAT_COMMAND}, {"bash_poll", "poll", CAT_COMMAND}, {"read", "read", CAT_READ},
    {"write", "write", CAT_WRITE}, {"edit", "edit", CAT_EDIT}, {"grep", "grep", CAT_READ},
    {"find", "find", CAT_READ}, {"ls", "ls", CAT_READ},
};
#define NTOOLS (sizeof tools / sizeof tools[0])

int app_tool_of(pt_str name)
{
    int i;
    for (i = 0; i < (int)NTOOLS; i++)
        if (pt_str_eqz(name, tools[i].name))
            return i;
    return -1;
}

/* A call on one row: the tool's name, dim, in a column, then the gist of
 * its arguments; in a command, the programs stand out and the rest is dim. */
void app_phrase(app *a, pt_buf *out, pt_str name, pt_str args)
{
    pt_str g = app_call_gist(a, args);
    bool cmd = pt_str_eqz(name, "bash"), at_cmd = true, in_word = false, prog = false;
    int col = 0, cur = -1, ti = app_tool_of(name), w;
    size_t k;
    tui_style(out, TUI_DIM);
    if (ti >= 0)
        name = pt_strz(tools[ti].shown);
    w = (int)(name.n > 60 ? 60 : name.n);
    pt_buf_put(out, name.p, (size_t)w);
    pt_buf_reset(&a->tmp);
    tui_clean(&a->tmp, g, &col);
    while (a->tmp.n && (a->tmp.p[a->tmp.n - 1] == '\n' || a->tmp.p[a->tmp.n - 1] == ' '))
        a->tmp.n--;
    for (w = w < NAME_W ? NAME_W - w : 1; a->tmp.n && w > 0; w--) /* to the column, or one space */
        pt_buf_putc(out, ' ');
    for (k = 0; k < a->tmp.n; k++) {
        char c = a->tmp.p[k] == '\n' ? ' ' : a->tmp.p[k];
        int st;
        if (cmd && (c == ';' || c == '|' || (c == '&' && (!k || (a->tmp.p[k - 1] != '>' && a->tmp.p[k - 1] != '<'))))) {
            st = 0; /* a program comes next */
            at_cmd = true;
            in_word = false;
        } else if (c == ' ') {
            st = cur < 0 ? TUI_DIM : cur;
            in_word = false;
        } else {
            if (!in_word) {
                in_word = true;
                prog = cmd && at_cmd && !strchr("<>(", c);
                at_cmd = false;
            }
            st = prog || !cmd ? 0 : TUI_DIM;
        }
        if (st != cur)
            tui_style(out, cur = st);
        pt_buf_putc(out, c);
    }
    tui_style(out, 0);
}

/* A duration, short: 5s, 2m 5s, 1h 2m. */
void app_secs_short(char *out, size_t cap, int64_t ms)
{
    long long t = (long long)(ms < 0 ? 0 : ms / 1000);
    if (t < 60)
        snprintf(out, cap, "%llds", t);
    else if (t < 3600)
        snprintf(out, cap, "%lldm %llds", t / 60, t % 60);
    else
        snprintf(out, cap, "%lldh %lldm", t / 3600, t / 60 % 60);
}

/* How long the run has worked: the time spent waiting for an answer to a
 * question does not count. */
int64_t app_run_clock(const app *a, int64_t now)
{
    return (a->run.wait_at ? a->run.wait_at : now) - a->run.at;
}

/* The activity mark blinks with the clock, lighting as its seconds change;
 * it stays lit while the clock stands still. */
const char *app_blink(const app *a, int64_t now)
{
    return a->run.wait_at || app_run_clock(a, now) / BLINK_MS % 2 == 0 ? a->sy->dot : " ";
}

/* The call a result answers: searched from the newest entry back. */
static const pt_block *find_call(app *a, pt_str call_id)
{
    uint32_t i = a->open ? pt_entry_count(a->s.a) : 0, k, looked = 0;
    pt_entry_view v;
    while (i-- && looked++ < 256)
        if (pt_entry_at(a->s.a, i, &v) && v.msg && strcmp(v.kind, "assistant") == 0)
            for (k = 0; k < v.msg->nblocks; k++)
                if (v.msg->blocks[k].kind == PT_BLOCK_TOOL_CALL && pt_str_eq(v.msg->blocks[k].id, call_id))
                    return &v.msg->blocks[k];
    return NULL;
}

static uint32_t count_lines(pt_str s)
{
    uint32_t n = 0;
    size_t i;
    while (s.n && s.p[s.n - 1] == '\n')
        s.n--;
    if (!s.n)
        return 0;
    for (i = 0; i < s.n; i++)
        n += s.p[i] == '\n';
    return n + 1;
}

/* The last n lines of s. */
static pt_str last_lines(pt_str s, uint32_t n)
{
    size_t at;
    while (s.n && s.p[s.n - 1] == '\n')
        s.n--;
    at = s.n;
    while (at && (n -= s.p[at - 1] == '\n') > 0)
        at--;
    return (pt_str){s.p + at, s.n - at};
}

/* ---- tool calls: groups (section 16.2) ----
 * Calls in a row, with nothing shown between them, form a group: one row
 * each, its mark in the left column, then the tool's name and the gist
 * ("✓ bash  make test · 3s", "✗ bash  make · exit 2", "✓ edit  a.c +3 -1"),
 * with a failure's last lines under the gist.
 * The open group is drawn in the band, and goes to the scrollback whole
 * once something else is shown. */

static const char *const bad_names[NBAD] = {"", "failed", "timed out", "denied", "cancelled", "interrupted"};

bool app_starts(pt_str s, const char *z)
{
    size_t n = strlen(z);
    return s.n >= n && memcmp(s.p, z, n) == 0;
}

/* The lines of s into group.buf, each after a prefix, while *left > 0; *left
 * counts down past 0 for the lines left out. */
static void put_lines(app *a, pt_str s, const char *prefix, int *left)
{
    size_t at = 0;
    while (s.n && s.p[s.n - 1] == '\n')
        s.n--;
    while (s.n && at <= s.n) {
        size_t e = at;
        while (e < s.n && s.p[e] != '\n')
            e++;
        if (*left > 0) {
            pt_buf_putz(&a->group.buf, prefix);
            pt_buf_put(&a->group.buf, s.p + at, e - at);
            pt_buf_putc(&a->group.buf, '\n');
        }
        (*left)--;
        at = e + 1;
    }
}

/* A finished call joins the open group: its row, such as "bash  make test",
 * "bash  make · exit 2" or "bash  rm -rf build · denied", and its details. */
static void group_add(app *a, const pt_entry_view *v)
{
    const pt_msg *m = v->msg;
    const pt_block *call = find_call(a, m->call_id);
    const pt_json *data, *d, *j;
    pt_str text = m->text, args = call ? call->args : PT_S("");
    int64_t ms = -1, code = 0, session = 0;
    bool has_code, timed = false, stats = false, on = false;
    unsigned add = 0, del = 0;
    int ti = app_tool_of(m->name), left = TOOL_FAIL;
    char seg[160], tail[64];
    grow *g;
    if (a->group.n == GROUP_MAX)
        app_group_close(a);
    g = &a->group.rows[a->group.n];
    memset(g, 0, sizeof *g);
    g->cat = (uint8_t)(ti < 0 ? CAT_OTHER : tools[ti].cat);
    pt_arena_reset(&a->ar);
    data = pt_json_get(pt_json_parse(v->raw, &a->ar, NULL), "data");
    d = pt_json_get(data, "details");
    has_code = pt_json_int(pt_json_get(d, "exit"), &code);
    on = pt_json_int(pt_json_get(d, "session"), &session); /* runs on in the background */
    if (!pt_json_int(pt_json_get(d, "ms"), &ms))
        ms = -1;
    if (g->cat == CAT_COMMAND) { /* bash's closing note: the row says it */
        pt_str last = last_lines(text, 1);
        timed = app_starts(last, "[timed out after ");
        if (timed || app_starts(last, "[exit code ") || app_starts(last, "[stopped]") || app_starts(last, "[ended]") ||
            app_starts(last, "[still running as session ")) {
            text.n = (size_t)(last.p - text.p);
            while (text.n && text.p[text.n - 1] == '\n')
                text.n--;
        }
    }
    if (!m->is_error)
        g->bad = G_OK;
    else if (pt_json_get(data, "interrupted"))
        g->bad = G_INTERRUPTED;
    else if (app_starts(text, "The user aborted"))
        g->bad = G_CANCELLED;
    else if (app_starts(text, "Permission denied") || app_starts(text, "The user denied"))
        g->bad = G_DENIED;
    else
        g->bad = timed ? G_TIMEOUT : G_FAILED;
    /* the details, and what the row ends with */
    tail[0] = 0;
    g->dat = (uint32_t)a->group.buf.n;
    if (g->bad == G_OK && g->cat == CAT_EDIT) {
        const pt_json *e;
        j = pt_json_parse(args, &a->ar, NULL);
        for (e = pt_json_is(pt_json_get(j, "edits"), PT_J_ARR) ? pt_json_get(j, "edits")->child : NULL; e;
             e = e->next) {
            add += count_lines(pt_json_gets(e, "new", &a->ar));
            del += count_lines(pt_json_gets(e, "old", &a->ar));
        }
        stats = true;
    } else if (g->bad == G_OK && g->cat == CAT_WRITE) {
        j = pt_json_parse(args, &a->ar, NULL);
        add = count_lines(pt_json_gets(j, "content", &a->ar));
        stats = true;
    } else if (g->bad == G_OK && pt_str_eqz(m->name, "read") && pt_json_get(data, "image")) {
        snprintf(tail, sizeof tail, " · image");
    } else if (g->bad == G_OK && pt_str_eqz(m->name, "read")) {
        snprintf(tail, sizeof tail, " · %u lines", count_lines(text));
    } else if (g->bad == G_FAILED || g->bad == G_TIMEOUT) {
        put_lines(a, last_lines(text, TOOL_FAIL), "", &left);
    }
    g->dn = (uint32_t)(a->group.buf.n - g->dat);
    if (on && g->bad == G_OK && pt_str_eqz(m->name, "bash")) {
        snprintf(tail, sizeof tail, " · still running as session %lld", (long long)session);
    } else if (on && g->bad == G_OK) {
        snprintf(tail, sizeof tail, " · still running");
    } else if (g->cat == CAT_COMMAND && ms >= 1000) {
        char took[32];
        app_secs_short(took, sizeof took, ms);
        snprintf(tail, sizeof tail, " · %s", took);
    }
    /* the row: the call, then what came of it */
    g->at = (uint32_t)a->group.buf.n;
    app_phrase(a, &a->group.buf, m->name, args);
    if (stats) { /* "+3", "+3 -1" */
        snprintf(seg, sizeof seg, " +%u", add);
        tui_style(&a->group.buf, TUI_GREEN);
        pt_buf_putz(&a->group.buf, seg);
        if (g->cat == CAT_EDIT) {
            snprintf(seg, sizeof seg, " -%u", del);
            tui_style(&a->group.buf, TUI_RED);
            pt_buf_putz(&a->group.buf, seg);
        }
    }
    if (g->bad) {
        if (g->bad == G_FAILED && g->cat == CAT_COMMAND && has_code && code)
            snprintf(seg, sizeof seg, "exit %lld", (long long)code);
        else
            snprintf(seg, sizeof seg, "%s", bad_names[g->bad]);
        tui_style(&a->group.buf, TUI_DIM);
        pt_buf_putz(&a->group.buf, " · ");
        tui_style(&a->group.buf, TUI_RED);
        pt_buf_putz(&a->group.buf, seg);
    }
    if (tail[0]) {
        tui_style(&a->group.buf, TUI_DIM);
        pt_buf_putz(&a->group.buf, tail);
    }
    tui_style(&a->group.buf, 0);
    g->n = (uint32_t)(a->group.buf.n - g->at);
    a->group.n++;
}

/* One row: a mark in the left column, then styled text, cut to the width. */
static void group_row(app *a, pt_buf *out, const char *mark, int mstyle, pt_str styled)
{
    size_t k;
    pt_buf_reset(&a->tmp2);
    tui_style(&a->tmp2, mstyle);
    pt_buf_putz(&a->tmp2, mark);
    tui_style(&a->tmp2, 0);
    pt_buf_putc(&a->tmp2, ' ');
    pt_buf_puts(&a->tmp2, styled);
    k = tui_fit(pt_buf_str(&a->tmp2), width(a), false);
    pt_buf_put(out, a->tmp2.p, k);
    pt_buf_putc(out, '\n');
}

/* Under a row's gist: the column its details start at. */
static const char detail_pad[] = "        "; /* MARGIN + NAME_W */

/* A row's details, each line under the gist. */
static void group_details(app *a, pt_buf *out, const grow *g)
{
    pt_str s = {a->group.buf.p + g->dat, g->dn};
    size_t at = 0;
    while (at < s.n) {
        size_t e = at, k;
        int col = 0;
        pt_str line;
        while (e < s.n && s.p[e] != '\n')
            e++;
        line = (pt_str){s.p + at, e - at};
        pt_buf_reset(&a->tmp2);
        pt_buf_putz(&a->tmp2, detail_pad);
        tui_style(&a->tmp2, TUI_DIM);
        pt_buf_reset(&a->tmp);
        tui_clean(&a->tmp, line, &col);
        pt_buf_puts(&a->tmp2, pt_buf_str(&a->tmp));
        tui_style(&a->tmp2, 0);
        k = tui_fit(pt_buf_str(&a->tmp2), width(a), false);
        pt_buf_put(out, a->tmp2.p, k);
        pt_buf_putc(out, '\n');
        at = e + 1;
    }
}

/* A running call ended: its slot goes to the end, buffers and all. */
static void run_done(app *a, pt_str call_id)
{
    int i;
    for (i = 0; i < a->run.ncalls; i++)
        if (pt_str_eq(pt_buf_str(&a->run.calls[i].id), call_id)) {
            run_call r = a->run.calls[i];
            memmove(&a->run.calls[i], &a->run.calls[i + 1], (size_t)(a->run.ncalls - i - 1) * sizeof r);
            a->run.calls[--a->run.ncalls] = r;
            return;
        }
}

bool app_runs_oom(const app *a)
{
    int i;
    for (i = 0; i < RUN_MAX; i++)
        if (a->run.calls[i].id.oom || a->run.calls[i].head.oom || a->run.calls[i].out.oom)
            return true;
    return false;
}

/* The group: whole for the scrollback; live, for the band, with its
 * newest rows only, without details, and the running calls last, marked
 * by the blinking dot (the newest with the end of its output). */
void app_group_render(app *a, pt_buf *out, bool live)
{
    bool running = live && a->run.ncalls;
    int i, first = 0;
    const char *mark = app_blink(a, pt_sys_now_ms());
    char more[48];
    if (!a->group.n && !running)
        return;
    if (live && a->group.n > GROUP_LIVE) {
        first = a->group.n - GROUP_LIVE;
        snprintf(more, sizeof more, "%s %d more", a->sy->more, first);
        pt_buf_reset(&a->wrap);
        tui_style(&a->wrap, TUI_DIM);
        pt_buf_putz(&a->wrap, more);
        group_row(a, out, " ", 0, pt_buf_str(&a->wrap));
    }
    for (i = first; i < a->group.n; i++) {
        const grow *g = &a->group.rows[i];
        if (g->at + g->n > a->group.buf.n || g->dat + g->dn > a->group.buf.n)
            break; /* out of memory: app_paint() says so */
        group_row(a, out, g->bad ? a->sy->fail : a->sy->ok, g->bad ? TUI_RED : TUI_GREEN | TUI_DIM,
                  (pt_str){a->group.buf.p + g->at, g->n});
        if (!live)
            group_details(a, out, g);
    }
    for (i = 0; running && i < a->run.ncalls; i++)
        group_row(a, out, mark, ACCENT | TUI_BOLD, pt_buf_str(&a->run.calls[i].head));
    if (running) { /* the end of the newest one's output */
        const run_call *r = &a->run.calls[a->run.ncalls - 1];
        pt_str o = pt_buf_str(&r->out), rows;
        size_t at = o.n;
        int lines = 0;
        while (at && o.p[at - 1] == '\n')
            at--;
        o.n = at;
        while (at && lines <= TOOL_LIVE)
            lines += o.p[--at] == '\n';
        if (at < o.n && o.p[at] == '\n')
            at++;
        while (at < o.n && ((unsigned char)o.p[at] & 0xC0) == 0x80)
            at++;
        if (!o.n)
            return;
        pt_buf_reset(&a->diff);
        out_rail(a, &a->diff, detail_pad + 1, 0, TUI_DIM, (pt_str){o.p + at, o.n - at}, 0);
        rows = pt_buf_str(&a->diff); /* its last TOOL_LIVE rows */
        at = rows.n;
        for (lines = 0; at && lines <= TOOL_LIVE; at--)
            if (rows.p[at - 1] == '\n' && ++lines > TOOL_LIVE)
                break;
        pt_buf_put(out, rows.p + at, rows.n - at);
    }
}

/* The open group goes to the scrollback. */
void app_group_close(app *a)
{
    if (!a->group.n)
        return;
    app_out_blank(&a->lines);
    app_group_render(a, &a->lines, false);
    a->group.n = 0;
    pt_buf_reset(&a->group.buf);
}

/* Thinking in full, or hidden: only that it happened, and for how long. */
static void out_thinking(app *a, pt_buf *out, pt_str text, int64_t ms, bool full)
{
    char line[80], took[32];
    if (full || a->thinking == TH_FULL) {
        tui_md m;
        memset(&m, 0, sizeof m);
        m.ascii = a->ascii;
        m.margin = MARGIN;
        out_md(a, out, &m, text, TUI_DIM);
        return;
    }
    if (ms < 0) /* from the log, which does not time thinking */
        took[0] = 0;
    else if (ms < 1000)
        snprintf(took, sizeof took, " for <1s");
    else {
        snprintf(took, sizeof took, " for ");
        app_secs_short(took + 5, sizeof took - 5, ms);
    }
    snprintf(line, sizeof line, "%s thought%s", a->sy->think, took);
    app_out_text(a, out, TUI_DIM, NULL, 0, pt_strz(line), 0);
}

static pt_str error_message(app *a, pt_str raw)
{
    pt_json *j;
    pt_arena_reset(&a->ar);
    j = pt_json_parse(raw, &a->ar, NULL);
    return pt_json_gets(pt_json_get(j, "data"), "message", &a->ar);
}

/* A user entry pytte wrote (a note that background commands ended). The
 * key cannot come from the text: quotes in a JSON string are escaped. */
/* Where the resources an @-mention added start: blocks to the end of the
 * text, each "\n\n<resource server=\"s\" uri=\"u\">\n...\n</resource>";
 * text.n when there are none. */
static size_t resources_at(pt_str text)
{
    static const char head[] = "\n\n<resource server=\"", tail[] = "\n</resource>";
    size_t at;
    for (at = 0; at + sizeof head - 1 <= text.n; at++) {
        size_t k = at;
        if (memcmp(text.p + at, head, sizeof head - 1) != 0)
            continue;
        for (;;) { /* blocks, one after another, to the end */
            size_t e;
            if (k == text.n)
                return at;
            if (k + sizeof head - 1 > text.n || memcmp(text.p + k, head, sizeof head - 1) != 0)
                break;
            for (e = k + sizeof head - 1; e + sizeof tail - 1 <= text.n; e++)
                if (!memcmp(text.p + e, tail, sizeof tail - 1) &&
                    (e + sizeof tail - 1 == text.n ||
                     (e + sizeof tail - 1 + sizeof head - 1 <= text.n &&
                      !memcmp(text.p + e + sizeof tail - 1, head, sizeof head - 1))))
                    break;
            if (e + sizeof tail - 1 > text.n)
                break;
            k = e + sizeof tail - 1;
        }
    }
    return text.n;
}

static bool is_note(const pt_entry_view *v)
{
    static const char key[] = "\"from\":\"pytte\"";
    size_t i, k = sizeof key - 1;
    if (strcmp(v->kind, "user") != 0)
        return false;
    for (i = 0; i + k <= v->raw.n; i++)
        if (memcmp(v->raw.p + i, key, k) == 0)
            return true;
    return false;
}

/* An entry from its record: on start, for entries not streamed, and in
 * the full view (Ctrl-O). */
void app_out_entry(app *a, pt_buf *out, const pt_entry_view *v, bool full)
{
    const pt_msg *m = v->msg;
    uint32_t i;
    if (strcmp(v->kind, "epoch") == 0 && v->epoch > 1) {
        char line[120];
        app_out_blank(out);
        snprintf(line, sizeof line, "compacted: the model continues from a summary%s",
                 full ? ":" : " (ctrl+o shows it)");
        app_out_note(a, out, TUI_DIM, pt_strz(line));
        if (full)
            out_rail(a, out, a->sy->bar, TUI_DIM, TUI_DIM, v->summary, 0);
        if (full && v->files.n)
            out_rail(a, out, a->sy->bar, TUI_DIM, TUI_DIM, v->files, 0);
    } else if (is_note(v) && !full) { /* its "- session ..." lines */
        pt_str text = m->text;
        size_t at = 0, e;
        app_out_blank(out);
        while (at < text.n) {
            for (e = at; e < text.n && text.p[e] != '\n'; e++)
                ;
            if (e - at > 2 && text.p[at] == '-' && text.p[at + 1] == ' ')
                app_out_note(a, out, TUI_DIM, (pt_str){text.p + at + 2, e - at - 2});
            at = e + 1;
        }
    } else if (strcmp(v->kind, "user") == 0) { /* as it looked at the prompt */
        pt_str text = m->text, name;
        char line[120], mark[16];
        app_out_blank(out);
        snprintf(mark, sizeof mark, "%s ", a->sy->prompt);
        size_t res;
        while (pt_rt_skill_block(&text, &name)) { /* a skill's instructions: just its name */
            snprintf(line, sizeof line, "skill %.*s", (int)(name.n > 64 ? 64 : name.n), name.p);
            app_out_text(a, out, TUI_DIM, mark, ACCENT, pt_strz(line), MARGIN);
        }
        res = resources_at(text);
        if (res)
            app_out_text(a, out, TUI_BOLD, mark, ACCENT | TUI_BOLD, (pt_str){text.p, res}, MARGIN);
        while (res < text.n) { /* an @-mention's contents: just which */
            const char *p = text.p + res + 20, *end = text.p + text.n, *q, *u, *close;
            for (q = p; q < end && *q != '"'; q++)
                ;
            u = q + 7 < end ? q + 7 : end; /* after ` uri="` */
            for (close = u; close < end && *close != '"'; close++)
                ;
            snprintf(line, sizeof line, "resource %.*s:%.*s", (int)(q - p > 40 ? 40 : q - p), p,
                     (int)(close - u > 70 ? 70 : close - u), u);
            app_out_text(a, out, TUI_DIM, mark, ACCENT, pt_strz(line), MARGIN);
            {
                pt_str rest = {text.p + res + 2, text.n - res - 2};
                size_t nx = resources_at(rest);
                res = nx == rest.n ? text.n : res + 2 + nx;
            }
        }
    } else if (strcmp(v->kind, "assistant") == 0) {
        bool any = false;
        app_out_blank(out);
        for (i = 0; i < m->nblocks; i++) {
            const pt_block *b = &m->blocks[i];
            if (b->kind == PT_BLOCK_THINKING && (b->text.n || full)) {
                out_thinking(a, out, b->text, -1, full);
            } else if (b->kind == PT_BLOCK_TEXT) {
                tui_md md;
                memset(&md, 0, sizeof md);
                md.ascii = a->ascii;
                md.margin = MARGIN;
                out_md(a, out, &md, b->text, 0);
                any = true;
            } else if (b->kind == PT_BLOCK_TOOL_CALL) {
                if (full) /* else it is shown with its result */
                    out_call(a, out, b);
                any = true;
            }
        }
        if (m->stop == PT_STOP_ABORTED || m->stop == PT_STOP_ERROR)
            app_out_note(a, out, TUI_DIM, any ? PT_S("interrupted") : PT_S("interrupted, nothing arrived"));
    } else if (strcmp(v->kind, "tool_result") == 0) { /* else it joins a group */
        out_rail(a, out, a->sy->bar, TUI_DIM, m->is_error ? TUI_RED | TUI_DIM : TUI_DIM, m->text, 0);
    } else if (strcmp(v->kind, "error") == 0) {
        app_out_note(a, out, TUI_RED, error_message(a, v->raw));
    } else if (strcmp(v->kind, "note") == 0) {
        app_out_note(a, out, TUI_DIM, m->text);
    }
}

static void begin_msg(app *a)
{
    app_group_close(a);
    if (!a->view.started)
        app_out_blank(&a->lines);
    a->view.started = true;
}

/* A complete source line of the streaming block. */
static void live_line(app *a, pt_str line)
{
    if (a->view.kind == LIVE_TEXT) {
        begin_msg(a);
        tui_md_line(&a->view.md, line, a->cols, 0, false, &a->lines);
        a->view.printed = true;
    } else if (a->thinking == TH_FULL) {
        begin_msg(a);
        tui_md_line(&a->view.md, line, a->cols, TUI_DIM, false, &a->lines);
    }
}

static void live_add(app *a, pt_str delta)
{
    size_t at = 0, i = a->view.live.n;
    tui_clean(&a->view.live, delta, &a->view.col);
    /* Earlier bytes are the unfinished line, already checked for newlines. */
    for (; i < a->view.live.n; i++)
        if (a->view.live.p[i] == '\n') {
            live_line(a, (pt_str){a->view.live.p + at, i - at});
            at = i + 1;
        }
    if (at) {
        memmove(a->view.live.p, a->view.live.p + at, a->view.live.n - at);
        a->view.live.n -= at;
    }
}

/* The block ended: its unfinished line is complete now. */
void app_live_end(app *a)
{
    if (a->view.kind != LIVE_NONE && a->view.live.n)
        live_line(a, pt_buf_str(&a->view.live));
    if (a->view.kind == LIVE_THINK && a->thinking == TH_HIDDEN && !a->group.n) { /* a group goes on over it */
        begin_msg(a);
        out_thinking(a, &a->lines, PT_STR_NULL, pt_sys_now_ms() - a->view.think_at, false);
    }
    pt_buf_reset(&a->view.live);
    a->view.kind = LIVE_NONE;
    a->view.col = 0;
}

static void live_begin(app *a, int kind)
{
    app_live_end(a);
    a->view.kind = kind;
    memset(&a->view.md, 0, sizeof a->view.md);
    a->view.md.ascii = a->ascii;
    a->view.md.margin = MARGIN;
}

static void on_ai(app *a, const pt_ai_event *ev)
{
    switch (ev->kind) {
    case PT_EV_START:
        a->view.streamed = true;
        a->view.started = false;
        a->view.printed = false;
        a->run.phase = PH_WAIT;
        break;
    case PT_EV_THINKING_START:
        if (a->thinking == TH_FULL)
            app_group_close(a);
        live_begin(a, LIVE_THINK);
        a->view.think_at = pt_sys_now_ms();
        a->run.phase = PH_THINK;
        break;
    case PT_EV_TEXT_START: /* the calls before it are over */
        app_group_close(a);
        live_begin(a, LIVE_TEXT);
        a->run.phase = PH_WRITE;
        break;
    case PT_EV_THINKING_DELTA:
    case PT_EV_TEXT_DELTA:
        live_add(a, ev->delta);
        break;
    case PT_EV_THINKING_END:
    case PT_EV_TEXT_END:
    case PT_EV_DONE:
    case PT_EV_ERROR:
        app_live_end(a);
        break;
    case PT_EV_TOOLCALL_END: /* shown with its result, or in the band as it runs */
        app_live_end(a);
        a->view.printed = true;
        break;
    default:
        break;
    }
}

/* Whether an assistant entry shows anything outside the full view: text,
 * thinking (in a group, hidden thinking is left out), or that it was
 * cut off. With only calls, a group goes on over it. */
static bool shows(app *a, const pt_entry_view *v)
{
    const pt_msg *m = v->msg;
    uint32_t i;
    if (m->stop == PT_STOP_ABORTED || m->stop == PT_STOP_ERROR)
        return true;
    for (i = 0; i < m->nblocks; i++)
        if ((m->blocks[i].kind == PT_BLOCK_TEXT) ||
            (m->blocks[i].kind == PT_BLOCK_THINKING && m->blocks[i].text.n &&
             (a->thinking == TH_FULL || !a->group.n)))
            return true;
    return false;
}

/* An entry from its record, outside the full view: a result joins the
 * open group, anything else shown closes it. */
static void show_entry(app *a, const pt_entry_view *v)
{
    if (strcmp(v->kind, "tool_result") == 0) {
        group_add(a, v);
        return;
    }
    if (strcmp(v->kind, "assistant") == 0 && !shows(a, v))
        return;
    app_group_close(a);
    app_out_entry(a, &a->lines, v, false);
}

static void on_entries(app *a)
{
    uint32_t n = pt_entry_count(a->s.a), i;
    pt_entry_view v;
    if (n < a->view.seen || (n && a->view.seen && pt_entry_at(a->s.a, 0, &v) && v.epoch != a->view.epoch))
        a->view.seen = 0; /* a new epoch */
    a->model.ctx_used = pt_context_tokens(a->s.a);
    if (n && pt_entry_at(a->s.a, 0, &v))
        a->view.epoch = v.epoch;
    for (i = a->view.seen; i < n && pt_entry_at(a->s.a, i, &v); i++) {
        if (v.kept) /* a compaction's copy: on screen already, as the original */
            continue;
        if (strcmp(v.kind, "user") == 0) /* answers a key: no frame wait */
            a->pace.urgent = true;
        if (strcmp(v.kind, "user") == 0 && a->queue.n && !is_note(&v)) { /* a queued input was placed */
            queued_input first = a->queue.items[0];
            memmove(a->queue.items, a->queue.items + 1, (a->queue.n - 1) * sizeof *a->queue.items);
            a->queue.items[--a->queue.n] = first;
            pt_buf_reset(&a->queue.items[a->queue.n].text);
        }
        if (strcmp(v.kind, "tool_result") == 0 && v.msg) {
            run_done(a, v.msg->call_id);
            if (a->run.phase == PH_TOOL && !a->run.ncalls)
                a->run.phase = PH_WAIT;
        }
        if (strcmp(v.kind, "assistant") == 0 && a->view.streamed) {
            /* already on screen; an interrupted partial gets its mark */
            app_live_end(a);
            if (v.msg->stop == PT_STOP_ABORTED || v.msg->stop == PT_STOP_ERROR) {
                begin_msg(a);
                app_out_z(a, TUI_DIM, a->view.printed ? "interrupted" : "interrupted, nothing arrived");
            }
            a->view.streamed = false;
            a->view.started = false;
            continue;
        }
        show_entry(a, &v);
    }
    a->view.seen = n;
}

void app_on_event(void *ud, const pt_event *ev)
{
    app *a = (app *)ud;
    char line[600];
    switch (ev->kind) {
    case PT_E_STREAM:
        on_ai(a, ev->ai);
        break;
    case PT_E_COMMITTED:
        on_entries(a);
        break;
    case PT_E_COMPACT:
        if (pt_str_eqz(ev->outcome, "started"))
            a->run.phase = PH_COMPACT;
        else if (pt_str_eqz(ev->outcome, "failed")) {
            app_group_close(a);
            app_out_note(a, &a->lines, TUI_RED, ev->error);
        }
        break;
    case PT_E_TOOL_START:
        a->run.phase = PH_TOOL;
        if (a->run.ncalls < RUN_MAX) {
            run_call *r = &a->run.calls[a->run.ncalls++];
            r->task = ev->task;
            snprintf(r->name, sizeof r->name, "%.*s", (int)(ev->name.n > 60 ? 60 : ev->name.n), ev->name.p);
            pt_buf_reset(&r->id);
            pt_buf_puts(&r->id, ev->call_id);
            pt_buf_reset(&r->out);
            pt_buf_reset(&r->head);
            app_phrase(a, &r->head, ev->name, ev->args);
        }
        break;
    case PT_E_TOOL_OUTPUT: {
        run_call *r = NULL;
        int i;
        for (i = 0; i < a->run.ncalls && !r; i++)
            if (a->run.calls[i].task == ev->task)
                r = &a->run.calls[i];
        if (!r)
            break;
        if (ev->data.n >= TOOL_KEEP) {
            pt_buf_reset(&r->out);
            pt_buf_put(&r->out, ev->data.p + ev->data.n - TOOL_KEEP, TOOL_KEEP);
        } else {
            if (r->out.n + ev->data.n > 2 * TOOL_KEEP) { /* keep the end: amortized O(n) */
                size_t k = TOOL_KEEP - ev->data.n;
                memmove(r->out.p, r->out.p + r->out.n - k, k);
                r->out.n = k;
            }
            pt_buf_puts(&r->out, ev->data);
        }
        break;
    }
    case PT_E_RETRY:
        a->run.phase = PH_RETRY;
        a->run.retry_until = ev->until;
        pt_buf_reset(&a->run.retry_err);
        tui_clean(&a->run.retry_err, ev->error, &(int){0});
        break;
    case PT_E_RUN_DONE:
        app_group_close(a);
        if (pt_str_eqz(ev->outcome, "failed")) {
            app_out_note(a, &a->lines, TUI_RED, ev->error);
        } else {
            /* What the clock said, kept: how long the run took. */
            char took[32];
            int64_t ms = app_run_clock(a, pt_sys_now_ms());
            if (ms < 1000)
                snprintf(took, sizeof took, "<1s");
            else
                app_secs_short(took, sizeof took, ms);
            snprintf(line, sizeof line, "worked for %s", took);
            app_out_blank(&a->lines);
            app_out_z(a, TUI_DIM, line);
        }
        a->run.at = pt_sys_now_ms(); /* a queued input runs next */
        a->run.wait_at = 0;
        break;
    case PT_E_FATAL:
        app_group_close(a);
        app_out_note(a, &a->lines, TUI_RED, ev->error);
        break;
    default:
        break;
    }
    set_dirty(a);
}

/* The transcript from the third-newest user entry on (16.2.5). */
void app_out_tail(app *a)
{
    uint32_t n = pt_entry_count(a->s.a), i, from = 0, users = 0, earlier = 0;
    pt_entry_view v;
    char line[300];
    for (i = n; i-- > 0;)
        if (pt_entry_at(a->s.a, i, &v) && strcmp(v.kind, "user") == 0 && ++users == 3) {
            from = i;
            break;
        }
    for (i = 0; i < from; i++)
        if (pt_entry_at(a->s.a, i, &v) && strcmp(v.kind, "epoch") != 0)
            earlier++;
    if (earlier) {
        snprintf(line, sizeof line, "%u earlier entries · pytte log shows them", earlier);
        app_out_z(a, TUI_DIM, line);
    }
    for (i = from; i < n && pt_entry_at(a->s.a, i, &v); i++)
        show_entry(a, &v);
    a->view.seen = n;
    if (n && pt_entry_at(a->s.a, 0, &v)) /* else the next commit looks like a new epoch */
        a->view.epoch = v.epoch;
}
