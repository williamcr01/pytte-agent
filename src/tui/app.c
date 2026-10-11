/* The TUI (section 16.2): the event loop, the display model built from the
 * agent's events, the band's contents, and the slash commands.
 *
 * Agent callbacks, keys and display updates run on the event-loop thread.
 * Session setup/recovery and durable writes use a bounded I/O worker.
 * Text goes to the scrollback once and is never touched again (R5): a
 * streaming answer is committed one complete source line at a time, and
 * the unfinished line is shown in the band as the live tail. */
#define _POSIX_C_SOURCE 200809L
#include "tui/tui.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cli/cli.h"

enum { TH_COLLAPSED, TH_FULL, TH_HIDDEN };
enum { PH_WAIT, PH_THINK, PH_WRITE, PH_RETRY, PH_TOOL, PH_COMPACT };
enum { LIVE_NONE, LIVE_TEXT, LIVE_THINK };
enum { MENU_NONE, MENU_CMD, MENU_ARG, MENU_SETTINGS };

#define MAX_ITEMS 400
#define DOUBLE_MS 1000

typedef struct syms {
    const char *prompt, *bullet, *think, *fail, *sel, *rail, *bar, *rule, *more, *ok, *tee, *elbow, *spin[10];
    int nspin;
} syms;

static const syms uni = {"❯", "●", "✻", "✗", "▸", "┃", "│", "─", "…", "✓", "├", "└",
                         {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"}, 10};
static const syms asc = {">", "*", "*", "x", ">", "|", "|", "-", "...", "ok", "|", "`", {"-", "\\", "|", "/"}, 4};

#define SPIN_MS 120  /* the activity row's frame: spinner and shimmer */
#define TOOL_FAIL 3  /* rows of a failed tool's output kept in the scrollback */
#define TOOL_DIFF 8  /* rows of an edit's diff */
#define TOOL_LIVE 5  /* rows of a running tool's output in the band */
#define TOOL_KEEP 4096 /* bytes of it kept for that */
#define RUN_MAX 8      /* running calls the band shows; more still run */

static const struct {
    const char *name, *args, *desc;
} cmds[] = {
    {"model", " [id]", "set the model of this session"},
    {"effort", " [level]", "set the reasoning effort of this session"},
    {"new", "", "start a new session"},
    {"resume", " [id]", "open another session from this directory"},
    {"status", "", "session, model, context and terminal"},
    {"compact", "", "summarize the conversation to free the context"},
    {"usage", "", "tokens used in this session, per model"},
    {"thinking", " [mode]", "how thinking is shown: collapsed, full, hidden"},
    {"settings", "", "user settings (saved)"},
    {"help", "", "commands and keys"},
    {"quit", "", "exit; it prints how to continue"},
};
#define NCMDS (sizeof cmds / sizeof cmds[0])

static const char *const levels[] = {"off", "minimal", "low", "medium", "high", "xhigh", "max"};
static const char *const th_names[] = {"collapsed", "full", "hidden"};
static const char *const sym_names[] = {"auto", "unicode", "ascii"};
static const char *const perm_names[] = {"ask", "auto", "read-only"};
static const char *const on_off[] = {"on", "off"};
#define VALUES(v) v, (int)(sizeof v / sizeof v[0])
/* the settings menu: config index, short name, and the values to cycle
 * through (none: edited as a command) */
static const struct {
    int k;
    const char *name;
    const char *const *vals;
    int nv;
} settings[] = {{CFG_THINKING, "thinking", VALUES(th_names)},
                {CFG_SYMBOLS, "symbols", VALUES(sym_names)},
                {CFG_MODEL, "model", NULL, 0},
                {CFG_REASONING, "reasoning", VALUES(levels)},
                {CFG_PAGER, "pager", NULL, 0},
                {CFG_PERMS, "permissions", VALUES(perm_names)},
                {CFG_COMPACT_MODEL, "compact-model", NULL, 0},
                {CFG_SKILLS, "skills", VALUES(on_off)}};
#define NSETTINGS (sizeof settings / sizeof settings[0])

typedef struct item {
    size_t at, n;      /* label in items_buf */
    size_t dat, dn;    /* description */
} item;

/* Tool calls in a row form a group (see group_add). */
#define GROUP_MAX 128 /* rows of a group; a longer run of calls makes more groups */
#define GROUP_LIVE 8  /* rows of an open group shown in the band */
enum { CAT_COMMAND, CAT_READ, CAT_WRITE, CAT_EDIT, CAT_OTHER, NCAT };
enum { G_OK, G_FAILED, G_TIMEOUT, G_DENIED, G_CANCELLED, G_INTERRUPTED, NBAD };

typedef struct grow {
    uint32_t at, n;   /* the row, styled, in grp */
    uint32_t dat, dn; /* its details, raw lines: a diff ("- ", "+ ") or a failure's last lines */
    uint8_t cat, bad;
    bool diff;
} grow;

/* A call that is running: its row, and the end of its output. */
typedef struct run_call {
    uint32_t task;
    int cat;
    char name[64];
    pt_buf id, head, out;
} run_call;

typedef struct queued_input {
    pt_buf text;
    bool command, submitted;
} queued_input;

typedef struct session_list {
    pt_worker *worker;
    char cwd[4096], ids[50][64];
    uint32_t n;
    bool pending, ready;
} session_list;

typedef struct config_job {
    pt_worker *worker;
    char path[4096], value[512], err[700];
    int key;
    bool active, ok;
    /* Pending values belong to the UI; the worker only reads value/key. */
    unsigned pending;
    char next[CFG_N][512];
} config_job;

typedef struct app {
    cli_opts o;
    cli_config *cfg;
    cli_session s;
    bool open; /* s holds a session */
    cli_open_job *opening;
    char fallback[64]; /* /resume restores the old session on failure */
    bool history_ready, drop_restored_draft;
    pt_buf pending_history, command;
    session_list *sessions;
    bool sessions_menu;
    bool skills_menu; /* the menu lists skills (read when it opens) */
    config_job *saving;
    bool pager_pending;
    char model[256];     /* shown */
    char model_ref[256]; /* o.model points here */
    bool created;        /* this process created the session */
    bool discarded;      /* ...and left it empty: closing deleted it */
    char err[700];
    tui_term *t;
    tui_keys keys;
    tui_band band;
    tui_frame frame;
    tui_input in;
    int rows, cols;
    bool ascii, color, kitty, cpr, answered;
    int start_col; /* CPR if already available before the first paint */
    const syms *sy;
    int thinking;
    /* output: styled lines for the scrollback, committed at the next paint */
    pt_buf lines;
    pt_buf tmp, tmp2, wrap, live_rows; /* scratch, reused */
    pt_arena ar;
    /* the display model */
    uint32_t seen;   /* entries already shown */
    uint32_t shown_epoch;
    pt_buf live;     /* the unfinished source line of the streaming block */
    int live_kind, live_col;
    tui_md md;
    bool streamed;   /* this attempt was streamed: do not print its entry again */
    bool started;    /* the current message has its blank line */
    bool printed;    /* ...and some content */
    int64_t think_at;
    bool running;
    int phase;
    int64_t run_at, retry_until;
    pt_buf retry_err;
    /* the running calls, in the order they started */
    run_call runs[RUN_MAX];
    int nruns;
    pt_buf diff;      /* scratch for the band, reused */
    /* the open group of finished calls: in the band until something else is shown */
    pt_buf grp;
    grow grows[GROUP_MAX];
    int ngrows;
    /* a tool call waiting for approval (refreshed from pt_asks) */
    pt_ask_view ask;
    uint32_t ask_task;
    int ask_sel;
    bool asked; /* the band shows the question */
    char ask_prefix[128]; /* what "allow this session" allows: "git", or "" for the whole tool */
    uint64_t ctx_used;
    uint32_t ctx_window;
    queued_input *queue;
    uint32_t nq, qcap;
    /* the menu */
    int menu;
    bool menu_closed;
    item items[MAX_ITEMS];
    int nitems, sel, label_w; /* label_w: the widest label shown, for the column */
    int menu_top;             /* the first row of the menu's window */
    pt_buf items_buf;
    int set_sel;
    /* double presses and hints */
    int64_t esc_at, ctrlc_at;
    /* pacing */
    bool dirty, urgent;
    int64_t last_paint, spin_at, winch_at, keys_at;
    int interval;
    bool draft_dirty;
    int64_t draft_at;
    char hist_path[4200];
    bool quit;
    bool keep_on_quit; /* /quit keeps even an empty session for resuming */
    int code;
} app;

static void group_close(app *a);

static void set_dirty(app *a)
{
    a->dirty = true;
}

/* ---- output to the scrollback ---- */

static int width(const app *a)
{
    return a->cols > 2 ? a->cols - 1 : 1;
}

/* Clean outside text, then each of its lines: style, the prefix on the
 * first line (indent spaces on the others), word-wrapped with a hanging
 * indent. */
static void out_text(app *a, pt_buf *out, int style, const char *prefix, int pstyle, pt_str raw, int indent)
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

static void out_z(app *a, int style, const char *z)
{
    group_close(a);
    out_text(a, &a->lines, style, NULL, 0, pt_strz(z), 0);
}

static void out_blank(pt_buf *out)
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
static pt_str call_gist(app *a, pt_str args)
{
    const pt_json *j, *c;
    pt_arena_reset(&a->ar);
    j = pt_json_parse(args, &a->ar, NULL);
    if (j && j->type == PT_J_OBJ)
        for (c = j->child; c; c = c->next)
            if (c->type == PT_J_STR)
                return pt_json_str(c, &a->ar);
    return args;
}

/* A call with all of its arguments, for the full view: "● name {...}". */
static void out_call(app *a, pt_buf *out, const pt_block *b)
{
    char head[160];
    snprintf(head, sizeof head, "%s %.*s ", a->sy->bullet, (int)(b->name.n > 60 ? 60 : b->name.n),
             b->name.p);
    out_text(a, out, 0, head, TUI_BOLD, b->args, 2);
}

/* How each builtin tool's row reads: asking, running, done, failed. */
static const struct {
    const char *name, *ask, *running, *done, *fail;
    int cat;
} verbs[] = {
    {"bash", "Run", "Running", "Ran", "Couldn't run", CAT_COMMAND},
    {"read", "Read", "Reading", "Read", "Couldn't read", CAT_READ},
    {"write", "Write", "Writing", "Wrote", "Couldn't write", CAT_WRITE},
    {"edit", "Edit", "Editing", "Edited", "Couldn't edit", CAT_EDIT},
};
#define NVERBS (sizeof verbs / sizeof verbs[0])

static int verb_of(pt_str name)
{
    int i;
    for (i = 0; i < (int)NVERBS; i++)
        if (pt_str_eqz(name, verbs[i].name))
            return i;
    return -1;
}

/* A call on one row: the verb (another tool: its name), then the gist of
 * its arguments, dim; in a command, the programs stand out. */
static void phrase(app *a, pt_buf *out, int vstyle, const char *verb, pt_str name, pt_str args)
{
    pt_str g = call_gist(a, args);
    bool cmd = pt_str_eqz(name, "bash"), at_cmd = true, in_word = false, prog = false;
    int col = 0, cur = -1;
    size_t k;
    tui_style(out, vstyle);
    if (verb)
        pt_buf_putz(out, verb);
    else
        pt_buf_put(out, name.p, name.n > 60 ? 60 : name.n);
    pt_buf_reset(&a->tmp);
    tui_clean(&a->tmp, g, &col);
    while (a->tmp.n && (a->tmp.p[a->tmp.n - 1] == '\n' || a->tmp.p[a->tmp.n - 1] == ' '))
        a->tmp.n--;
    if (a->tmp.n)
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
            st = prog ? 0 : TUI_DIM;
        }
        if (st != cur)
            tui_style(out, cur = st);
        pt_buf_putc(out, c);
    }
    tui_style(out, 0);
}

/* A duration, short: 5s, 2m 5s, 1h 2m. */
static void secs_short(char *out, size_t cap, int64_t ms)
{
    long long t = (long long)(ms < 0 ? 0 : ms / 1000);
    if (t < 60)
        snprintf(out, cap, "%llds", t);
    else if (t < 3600)
        snprintf(out, cap, "%lldm %llds", t / 60, t % 60);
    else
        snprintf(out, cap, "%lldh %lldm", t / 3600, t / 60 % 60);
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
 * Calls in a row, with nothing shown between them, form a group: a header
 * that counts them ("● 3 tool calls · 2 commands · 1 edit"), then a row
 * for each ("├ Ran make test", "└ Edited a.c +3 / -1") with its details
 * under it: an edit's diff, a failure's last lines. One call is just its
 * row. The open group is drawn in the band, and goes to the scrollback
 * whole once something else is shown, since the header must count calls
 * that have not happened yet. */

static const char *const cat_one[NCAT] = {"command", "read", "write", "edit", "other"};
static const char *const cat_many[NCAT] = {"commands", "reads", "writes", "edits", "others"};
static const char *const bad_names[NBAD] = {"", "failed", "timed out", "denied", "cancelled", "interrupted"};

static bool starts(pt_str s, const char *z)
{
    size_t n = strlen(z);
    return s.n >= n && memcmp(s.p, z, n) == 0;
}

/* The lines of s into grp, each after a prefix, while *left > 0; *left
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
            pt_buf_putz(&a->grp, prefix);
            pt_buf_put(&a->grp, s.p + at, e - at);
            pt_buf_putc(&a->grp, '\n');
        }
        (*left)--;
        at = e + 1;
    }
}

/* A finished call joins the open group: its row, such as "Ran make test",
 * "Exited 2 make" or "Denied bash rm -rf build", and its details. */
static void group_add(app *a, const pt_entry_view *v)
{
    static const char *const how[NBAD] = {"", "", "Timed out", "Denied", "Cancelled", "Interrupted"};
    const pt_msg *m = v->msg;
    const pt_block *call = find_call(a, m->call_id);
    const pt_json *data, *d, *j;
    pt_str text = m->text, args = call ? call->args : PT_S("");
    int64_t ms = -1, code = 0;
    bool has_code, timed = false, stats = false;
    unsigned add = 0, del = 0;
    int vi = verb_of(m->name), left = TOOL_DIFF;
    int nn = (int)(m->name.n > 60 ? 60 : m->name.n);
    char verb[160], tail[64];
    const char *vz = NULL;
    grow *g;
    if (a->ngrows == GROUP_MAX)
        group_close(a);
    g = &a->grows[a->ngrows];
    memset(g, 0, sizeof *g);
    g->cat = (uint8_t)(vi < 0 ? CAT_OTHER : verbs[vi].cat);
    pt_arena_reset(&a->ar);
    data = pt_json_get(pt_json_parse(v->raw, &a->ar, NULL), "data");
    d = pt_json_get(data, "details");
    has_code = pt_json_int(pt_json_get(d, "exit"), &code);
    if (!pt_json_int(pt_json_get(d, "ms"), &ms))
        ms = -1;
    if (g->cat == CAT_COMMAND) { /* bash's closing note: the row says it */
        pt_str last = last_lines(text, 1);
        timed = starts(last, "[timed out after ");
        if (timed || starts(last, "[exit code ") || starts(last, "[stopped]")) {
            text.n = (size_t)(last.p - text.p);
            while (text.n && text.p[text.n - 1] == '\n')
                text.n--;
        }
    }
    if (!m->is_error)
        g->bad = G_OK;
    else if (pt_json_get(data, "interrupted"))
        g->bad = G_INTERRUPTED;
    else if (starts(text, "The user aborted"))
        g->bad = G_CANCELLED;
    else if (starts(text, "Permission denied") || starts(text, "The user denied"))
        g->bad = G_DENIED;
    else
        g->bad = timed ? G_TIMEOUT : G_FAILED;
    /* the details, and what the row ends with */
    tail[0] = 0;
    g->dat = (uint32_t)a->grp.n;
    if (g->bad == G_OK && g->cat == CAT_EDIT) {
        pt_str old, new_;
        j = pt_json_parse(args, &a->ar, NULL);
        old = pt_json_gets(j, "old", &a->ar);
        new_ = pt_json_gets(j, "new", &a->ar);
        add = count_lines(new_);
        del = count_lines(old);
        stats = g->diff = true;
        put_lines(a, old, "- ", &left);
        put_lines(a, new_, "+ ", &left);
        if (left < 0) {
            snprintf(verb, sizeof verb, "%s %d more lines (ctrl+o)\n", a->sy->more, -left);
            pt_buf_putz(&a->grp, verb);
        }
    } else if (g->bad == G_OK && g->cat == CAT_WRITE) {
        j = pt_json_parse(args, &a->ar, NULL);
        add = count_lines(pt_json_gets(j, "content", &a->ar));
        stats = true;
    } else if (g->bad == G_OK && pt_str_eqz(m->name, "read")) {
        snprintf(tail, sizeof tail, " · %u lines", count_lines(text));
    } else if (g->bad == G_FAILED || g->bad == G_TIMEOUT) {
        left = TOOL_FAIL;
        put_lines(a, last_lines(text, TOOL_FAIL), "", &left);
    }
    g->dn = (uint32_t)(a->grp.n - g->dat);
    if (g->cat == CAT_COMMAND && ms >= 1000) {
        char took[32];
        secs_short(took, sizeof took, ms);
        snprintf(tail, sizeof tail, " · %s", took);
    }
    /* the row */
    if (g->bad == G_OK) {
        vz = vi < 0 ? NULL : verbs[vi].done;
    } else if (g->bad == G_FAILED && g->cat == CAT_COMMAND && has_code && code) {
        snprintf(verb, sizeof verb, "Exited %lld", (long long)code);
        vz = verb;
    } else if (g->bad == G_FAILED) {
        if (vi < 0)
            snprintf(verb, sizeof verb, "%.*s failed", nn, m->name.p);
        vz = vi < 0 ? verb : verbs[vi].fail;
    } else {
        if (g->bad == G_TIMEOUT)
            snprintf(verb, sizeof verb, "%s", how[g->bad]);
        else
            snprintf(verb, sizeof verb, "%s %.*s", how[g->bad], nn, m->name.p);
        vz = verb;
    }
    g->at = (uint32_t)a->grp.n;
    phrase(a, &a->grp, g->bad ? TUI_RED : 0, vz, m->name, args);
    if (stats) { /* "+3", "+3 / -1" */
        snprintf(verb, sizeof verb, " +%u", add);
        tui_style(&a->grp, TUI_GREEN);
        pt_buf_putz(&a->grp, verb);
        if (g->cat == CAT_EDIT) {
            snprintf(verb, sizeof verb, " -%u", del);
            tui_style(&a->grp, TUI_DIM);
            pt_buf_putz(&a->grp, " /");
            tui_style(&a->grp, TUI_RED);
            pt_buf_putz(&a->grp, verb);
        }
    } else if (tail[0]) {
        tui_style(&a->grp, TUI_DIM);
        pt_buf_putz(&a->grp, tail);
    }
    tui_style(&a->grp, 0);
    g->n = (uint32_t)(a->grp.n - g->at);
    a->ngrows++;
}

/* One row: a mark ("├", "●"), then styled text, cut to the width. */
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

/* A row's details, each line after `cont` (the tree's line, or spaces). */
static void group_details(app *a, pt_buf *out, const grow *g, const char *cont)
{
    pt_str s = {a->grp.p + g->dat, g->dn};
    size_t at = 0;
    while (at < s.n) {
        size_t e = at, k;
        int col = 0, style = TUI_DIM;
        pt_str line;
        while (e < s.n && s.p[e] != '\n')
            e++;
        line = (pt_str){s.p + at, e - at};
        if (g->diff && starts(line, "- "))
            style = TUI_RED;
        else if (g->diff && starts(line, "+ "))
            style = TUI_GREEN;
        pt_buf_reset(&a->tmp2);
        tui_style(&a->tmp2, TUI_DIM);
        pt_buf_putz(&a->tmp2, cont);
        tui_style(&a->tmp2, style);
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
    for (i = 0; i < a->nruns; i++)
        if (pt_str_eq(pt_buf_str(&a->runs[i].id), call_id)) {
            run_call r = a->runs[i];
            memmove(&a->runs[i], &a->runs[i + 1], (size_t)(a->nruns - i - 1) * sizeof r);
            a->runs[--a->nruns] = r;
            return;
        }
}

static bool runs_oom(const app *a)
{
    int i;
    for (i = 0; i < RUN_MAX; i++)
        if (a->runs[i].id.oom || a->runs[i].head.oom || a->runs[i].out.oom)
            return true;
    return false;
}

/* "● 3 tool calls · 2 commands · 1 edit · 1 failed": the kinds, most
 * first, then what went wrong. */
static void group_header(app *a, pt_buf *out, int bstyle, bool running)
{
    int cats[NCAT] = {0}, bads[NBAD] = {0}, i, total = a->ngrows;
    bool used[NCAT] = {false};
    char seg[64];
    size_t k;
    for (i = 0; i < a->ngrows; i++) {
        cats[a->grows[i].cat]++;
        bads[a->grows[i].bad]++;
    }
    for (i = 0; running && i < a->nruns; i++) {
        cats[a->runs[i].cat]++;
        total++;
    }
    pt_buf_reset(&a->tmp2);
    tui_style(&a->tmp2, bstyle);
    pt_buf_putz(&a->tmp2, a->sy->bullet);
    tui_style(&a->tmp2, 0);
    snprintf(seg, sizeof seg, " %d tool calls", total);
    pt_buf_putz(&a->tmp2, seg);
    tui_style(&a->tmp2, TUI_DIM);
    for (;;) {
        int best = -1, c;
        for (c = 0; c < NCAT; c++)
            if (cats[c] && !used[c] && (best < 0 || cats[c] > cats[best]))
                best = c;
        if (best < 0)
            break;
        used[best] = true;
        snprintf(seg, sizeof seg, " · %d %s", cats[best], cats[best] == 1 ? cat_one[best] : cat_many[best]);
        pt_buf_putz(&a->tmp2, seg);
    }
    for (i = G_FAILED; i < NBAD; i++)
        if (bads[i]) {
            tui_style(&a->tmp2, TUI_DIM);
            pt_buf_putz(&a->tmp2, " · ");
            tui_style(&a->tmp2, TUI_RED);
            snprintf(seg, sizeof seg, "%d %s", bads[i], bad_names[i]);
            pt_buf_putz(&a->tmp2, seg);
        }
    tui_style(&a->tmp2, 0);
    k = tui_fit(pt_buf_str(&a->tmp2), width(a), false);
    pt_buf_put(out, a->tmp2.p, k);
    pt_buf_putc(out, '\n');
}

/* The group: whole for the scrollback; live, for the band, with its
 * newest rows only, without details, and the running calls last (the
 * newest with the end of its output). */
static void group_render(app *a, pt_buf *out, bool live)
{
    bool running = live && a->nruns;
    int n = a->ngrows + (running ? a->nruns : 0), i, first = 0;
    bool single = n == 1, bad = false;
    char cont[16], more[48];
    if (!n)
        return;
    for (i = 0; i < a->ngrows; i++)
        bad = bad || a->grows[i].bad != G_OK;
    if (!single)
        group_header(a, out, running ? TUI_DIM : bad ? TUI_RED : TUI_GREEN, running);
    if (live && a->ngrows > GROUP_LIVE) {
        first = a->ngrows - GROUP_LIVE;
        snprintf(more, sizeof more, "%s %d more", a->sy->more, first);
        pt_buf_reset(&a->wrap);
        tui_style(&a->wrap, TUI_DIM);
        pt_buf_putz(&a->wrap, more);
        group_row(a, out, a->sy->tee, TUI_DIM, pt_buf_str(&a->wrap));
    }
    for (i = first; i < a->ngrows; i++) {
        const grow *g = &a->grows[i];
        bool last = i == a->ngrows - 1 && !running;
        if (g->at + g->n > a->grp.n || g->dat + g->dn > a->grp.n)
            break; /* out of memory: paint() says so */
        if (single)
            group_row(a, out, a->sy->bullet, g->bad ? TUI_RED : TUI_GREEN, (pt_str){a->grp.p + g->at, g->n});
        else
            group_row(a, out, last ? a->sy->elbow : a->sy->tee, TUI_DIM, (pt_str){a->grp.p + g->at, g->n});
        if (!live) { /* under the row's text */
            snprintf(cont, sizeof cont, "%s   ", single || last ? " " : a->sy->bar);
            group_details(a, out, g, cont);
        }
    }
    for (i = 0; running && i + 1 < a->nruns; i++)
        group_row(a, out, a->sy->tee, TUI_DIM, pt_buf_str(&a->runs[i].head));
    if (running) { /* the newest: its row, and the end of its output */
        const run_call *r = &a->runs[a->nruns - 1];
        pt_str o = pt_buf_str(&r->out), rows;
        size_t at = o.n;
        int lines = 0;
        group_row(a, out, single ? a->sy->bullet : a->sy->elbow, TUI_DIM, pt_buf_str(&r->head));
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
        snprintf(cont, sizeof cont, "  %s", a->sy->bar);
        out_rail(a, &a->diff, cont, TUI_DIM, TUI_DIM, (pt_str){o.p + at, o.n - at}, 0);
        rows = pt_buf_str(&a->diff); /* its last TOOL_LIVE rows */
        at = rows.n;
        for (lines = 0; at && lines <= TOOL_LIVE; at--)
            if (rows.p[at - 1] == '\n' && ++lines > TOOL_LIVE)
                break;
        pt_buf_put(out, rows.p + at, rows.n - at);
    }
}

/* The open group goes to the scrollback. */
static void group_close(app *a)
{
    if (!a->ngrows)
        return;
    out_blank(&a->lines);
    group_render(a, &a->lines, false);
    a->ngrows = 0;
    pt_buf_reset(&a->grp);
}

static void out_thinking(app *a, pt_buf *out, pt_str text, int64_t ms, bool full)
{
    char line[80];
    if (full || a->thinking == TH_FULL) {
        tui_md m;
        memset(&m, 0, sizeof m);
        m.ascii = a->ascii;
        out_md(a, out, &m, text, TUI_DIM);
    } else if (a->thinking == TH_COLLAPSED) {
        if (ms >= 1000)
            snprintf(line, sizeof line, "%s thought for %llds", a->sy->think, (long long)(ms / 1000));
        else
            snprintf(line, sizeof line, "%s thought", a->sy->think);
        out_text(a, out, TUI_DIM, NULL, 0, pt_strz(line), 0);
    }
}

static pt_str error_message(app *a, pt_str raw)
{
    pt_json *j;
    pt_arena_reset(&a->ar);
    j = pt_json_parse(raw, &a->ar, NULL);
    return pt_json_gets(pt_json_get(j, "data"), "message", &a->ar);
}

/* An entry from its record: on start, for entries not streamed, and in
 * the full view (Ctrl-O). */
static void out_entry(app *a, pt_buf *out, const pt_entry_view *v, bool full)
{
    const pt_msg *m = v->msg;
    uint32_t i;
    if (strcmp(v->kind, "epoch") == 0 && v->epoch > 1) {
        char line[120];
        out_blank(out);
        snprintf(line, sizeof line, "%s Compacted: the model continues from a summary%s", a->sy->think,
                 full ? ":" : " (ctrl+o shows it)");
        out_text(a, out, TUI_DIM, NULL, 0, pt_strz(line), 0);
        if (full)
            out_rail(a, out, a->sy->bar, TUI_DIM, TUI_DIM, v->summary, 0);
        if (full && v->files.n)
            out_rail(a, out, a->sy->bar, TUI_DIM, TUI_DIM, v->files, 0);
    } else if (strcmp(v->kind, "user") == 0) {
        pt_str text = m->text, name;
        char line[120];
        out_blank(out);
        while (pt_rt_skill_block(&text, &name)) { /* a skill's instructions: just its name */
            snprintf(line, sizeof line, "skill %.*s", (int)(name.n > 64 ? 64 : name.n), name.p);
            out_rail(a, out, a->sy->rail, TUI_DIM, TUI_DIM, pt_strz(line), 0);
        }
        if (text.n)
            out_rail(a, out, a->sy->rail, TUI_DIM, TUI_BOLD, text, 0);
    } else if (strcmp(v->kind, "assistant") == 0) {
        bool any = false;
        out_blank(out);
        for (i = 0; i < m->nblocks; i++) {
            const pt_block *b = &m->blocks[i];
            if (b->kind == PT_BLOCK_THINKING && (b->text.n || full)) {
                out_thinking(a, out, b->text, 0, full);
            } else if (b->kind == PT_BLOCK_TEXT) {
                tui_md md;
                memset(&md, 0, sizeof md);
                md.ascii = a->ascii;
                out_md(a, out, &md, b->text, 0);
                any = true;
            } else if (b->kind == PT_BLOCK_TOOL_CALL) {
                if (full) /* else it is shown with its result */
                    out_call(a, out, b);
                any = true;
            }
        }
        if (m->stop == PT_STOP_ABORTED || m->stop == PT_STOP_ERROR)
            out_text(a, out, TUI_DIM, NULL, 0, any ? PT_S("(interrupted)") : PT_S("(interrupted, nothing arrived)"), 0);
    } else if (strcmp(v->kind, "tool_result") == 0) { /* else it joins a group */
        out_rail(a, out, a->sy->bar, TUI_DIM, m->is_error ? TUI_RED | TUI_DIM : TUI_DIM, m->text, 0);
    } else if (strcmp(v->kind, "error") == 0) {
        char p[16];
        snprintf(p, sizeof p, "%s ", a->sy->fail);
        out_text(a, out, TUI_RED, p, TUI_RED, error_message(a, v->raw), 2);
    } else if (strcmp(v->kind, "note") == 0) {
        out_text(a, out, TUI_DIM, NULL, 0, m->text, 0);
    }
}

/* ---- the display model, from the agent's events ---- */

static void begin_msg(app *a)
{
    group_close(a);
    if (!a->started)
        out_blank(&a->lines);
    a->started = true;
}

/* A complete source line of the streaming block. */
static void live_line(app *a, pt_str line)
{
    if (a->live_kind == LIVE_TEXT) {
        begin_msg(a);
        tui_md_line(&a->md, line, a->cols, 0, false, &a->lines);
        a->printed = true;
    } else if (a->thinking == TH_FULL) {
        begin_msg(a);
        tui_md_line(&a->md, line, a->cols, TUI_DIM, false, &a->lines);
    }
}

static void live_add(app *a, pt_str delta)
{
    size_t at = 0, i = a->live.n;
    tui_clean(&a->live, delta, &a->live_col);
    /* Earlier bytes are the unfinished line, already checked for newlines. */
    for (; i < a->live.n; i++)
        if (a->live.p[i] == '\n') {
            live_line(a, (pt_str){a->live.p + at, i - at});
            at = i + 1;
        }
    if (at) {
        memmove(a->live.p, a->live.p + at, a->live.n - at);
        a->live.n -= at;
    }
}

/* The block ended: its unfinished line is complete now. */
static void live_end(app *a)
{
    if (a->live_kind != LIVE_NONE && a->live.n)
        live_line(a, pt_buf_str(&a->live));
    if (a->live_kind == LIVE_THINK && a->thinking == TH_COLLAPSED && !a->ngrows) { /* a group goes on over it */
        begin_msg(a);
        out_thinking(a, &a->lines, PT_STR_NULL, pt_sys_now_ms() - a->think_at, false);
    }
    pt_buf_reset(&a->live);
    a->live_kind = LIVE_NONE;
    a->live_col = 0;
}

static void live_begin(app *a, int kind)
{
    live_end(a);
    a->live_kind = kind;
    memset(&a->md, 0, sizeof a->md);
    a->md.ascii = a->ascii;
}

static void on_ai(app *a, const pt_ai_event *ev)
{
    switch (ev->kind) {
    case PT_EV_START:
        a->streamed = true;
        a->started = false;
        a->printed = false;
        a->phase = PH_WAIT;
        break;
    case PT_EV_THINKING_START:
        if (a->thinking == TH_FULL)
            group_close(a);
        live_begin(a, LIVE_THINK);
        a->think_at = pt_sys_now_ms();
        a->phase = PH_THINK;
        break;
    case PT_EV_TEXT_START: /* the calls before it are over */
        group_close(a);
        live_begin(a, LIVE_TEXT);
        a->phase = PH_WRITE;
        break;
    case PT_EV_THINKING_DELTA:
    case PT_EV_TEXT_DELTA:
        live_add(a, ev->delta);
        break;
    case PT_EV_THINKING_END:
    case PT_EV_TEXT_END:
    case PT_EV_DONE:
    case PT_EV_ERROR:
        live_end(a);
        break;
    case PT_EV_TOOLCALL_END: /* shown with its result, or in the band as it runs */
        live_end(a);
        a->printed = true;
        break;
    default:
        break;
    }
}

/* Whether an assistant entry shows anything outside the full view: text,
 * thinking (in a group, collapsed thinking is left out), or that it was
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
             (a->thinking == TH_FULL || (a->thinking == TH_COLLAPSED && !a->ngrows))))
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
    group_close(a);
    out_entry(a, &a->lines, v, false);
}

static void on_entries(app *a)
{
    uint32_t n = pt_entry_count(a->s.a), i;
    pt_entry_view v;
    if (n < a->seen || (n && a->seen && pt_entry_at(a->s.a, 0, &v) && v.epoch != a->shown_epoch))
        a->seen = 0; /* a new epoch */
    a->ctx_used = pt_context_tokens(a->s.a);
    if (n && pt_entry_at(a->s.a, 0, &v))
        a->shown_epoch = v.epoch;
    for (i = a->seen; i < n && pt_entry_at(a->s.a, i, &v); i++) {
        if (v.kept) /* a compaction's copy: on screen already, as the original */
            continue;
        if (strcmp(v.kind, "user") == 0) /* answers a key: no frame wait */
            a->urgent = true;
        if (strcmp(v.kind, "user") == 0 && a->nq) { /* a queued input was placed */
            queued_input first = a->queue[0];
            memmove(a->queue, a->queue + 1, (a->nq - 1) * sizeof *a->queue);
            a->queue[--a->nq] = first;
            pt_buf_reset(&a->queue[a->nq].text);
        }
        if (strcmp(v.kind, "tool_result") == 0 && v.msg) {
            run_done(a, v.msg->call_id);
            if (a->phase == PH_TOOL && !a->nruns)
                a->phase = PH_WAIT;
        }
        if (strcmp(v.kind, "assistant") == 0 && a->streamed) {
            /* already on screen; an interrupted partial gets its mark */
            live_end(a);
            if (v.msg->stop == PT_STOP_ABORTED || v.msg->stop == PT_STOP_ERROR) {
                begin_msg(a);
                out_z(a, TUI_DIM, a->printed ? "(interrupted)" : "(interrupted, nothing arrived)");
            }
            a->streamed = false;
            a->started = false;
            continue;
        }
        show_entry(a, &v);
    }
    a->seen = n;
}

static void on_event(void *ud, const pt_event *ev)
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
            a->phase = PH_COMPACT;
        else if (pt_str_eqz(ev->outcome, "failed")) {
            snprintf(line, sizeof line, "%s %.*s", a->sy->fail, (int)(ev->error.n > 500 ? 500 : ev->error.n),
                     ev->error.p);
            group_close(a);
            out_text(a, &a->lines, TUI_RED, NULL, 0, pt_strz(line), 2);
        }
        break;
    case PT_E_TOOL_START:
        a->phase = PH_TOOL;
        if (a->nruns < RUN_MAX) {
            run_call *r = &a->runs[a->nruns++];
            int vi = verb_of(ev->name);
            r->task = ev->task;
            r->cat = vi < 0 ? CAT_OTHER : verbs[vi].cat;
            snprintf(r->name, sizeof r->name, "%.*s", (int)(ev->name.n > 60 ? 60 : ev->name.n), ev->name.p);
            pt_buf_reset(&r->id);
            pt_buf_puts(&r->id, ev->call_id);
            pt_buf_reset(&r->out);
            pt_buf_reset(&r->head);
            phrase(a, &r->head, 0, vi < 0 ? NULL : verbs[vi].running, ev->name, ev->args);
        }
        break;
    case PT_E_TOOL_OUTPUT: {
        run_call *r = NULL;
        int i;
        for (i = 0; i < a->nruns && !r; i++)
            if (a->runs[i].task == ev->task)
                r = &a->runs[i];
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
        a->phase = PH_RETRY;
        a->retry_until = ev->until;
        pt_buf_reset(&a->retry_err);
        tui_clean(&a->retry_err, ev->error, &(int){0});
        break;
    case PT_E_RUN_DONE:
        group_close(a);
        if (pt_str_eqz(ev->outcome, "failed")) {
            snprintf(line, sizeof line, "%s %.*s", a->sy->fail, (int)(ev->error.n > 500 ? 500 : ev->error.n),
                     ev->error.p);
            out_text(a, &a->lines, TUI_RED, NULL, 0, pt_strz(line), 2);
        } else {
            /* What the spinner said, kept: how long the run took. */
            char took[32];
            int64_t ms = pt_sys_now_ms() - a->run_at;
            if (ms < 1000)
                snprintf(took, sizeof took, "<1s");
            else
                secs_short(took, sizeof took, ms);
            snprintf(line, sizeof line, "Worked for %s", took);
            out_blank(&a->lines);
            out_z(a, TUI_DIM, line);
        }
        a->run_at = pt_sys_now_ms(); /* a queued input runs next */
        break;
    case PT_E_FATAL:
        group_close(a);
        snprintf(line, sizeof line, "%s %.*s", a->sy->fail, (int)(ev->error.n > 500 ? 500 : ev->error.n),
                 ev->error.p);
        out_text(a, &a->lines, TUI_RED, NULL, 0, pt_strz(line), 2);
        break;
    default:
        break;
    }
    set_dirty(a);
}

/* ---- the band ---- */

static int find_setting(pt_str name)
{
    size_t i;
    if (pt_str_eqz(name, "effort"))
        name = PT_S("reasoning");
    for (i = 0; i < NSETTINGS; i++)
        if (pt_str_eqz(name, settings[i].name) || pt_str_eqz(name, cli_config_key(settings[i].k)))
            return (int)i;
    return -1;
}

static bool menu_takes_arg(pt_str cmd)
{
    return pt_str_eqz(cmd, "model") || pt_str_eqz(cmd, "effort") || pt_str_eqz(cmd, "thinking") ||
           pt_str_eqz(cmd, "resume") || pt_str_eqz(cmd, "settings");
}

static void add_item(app *a, const char *label, size_t ln, const char *desc)
{
    item *it;
    if (a->nitems >= MAX_ITEMS)
        return;
    it = &a->items[a->nitems++];
    it->at = a->items_buf.n;
    it->n = ln;
    pt_buf_put(&a->items_buf, label, ln);
    it->dat = a->items_buf.n;
    it->dn = desc ? strlen(desc) : 0;
    if (desc)
        pt_buf_putz(&a->items_buf, desc);
}

static pt_str item_label(const app *a, int i)
{
    return (pt_str){a->items_buf.p + a->items[i].at, a->items[i].n};
}

static void add_models(app *a, pt_str q)
{
    const pt_ai_catalog *cat = pt_ai_builtin_catalog();
    char ref[256];
    uint32_t i;
    for (i = 0; i < cat->nmodels; i++) {
        int n = snprintf(ref, sizeof ref, "%s/%s", cat->models[i].provider, cat->models[i].id);
        if (n > 0 && (size_t)n < sizeof ref && pt_str_icontains(pt_strz(ref), q))
            add_item(a, ref, (size_t)n, cat->models[i].name);
    }
}

/* Skills whose invocation (lead and name: "$name", "skill:name") starts
 * with q. */
static void add_skills(app *a, const char *lead, pt_str q, bool rescan)
{
    uint32_t i, n = 0;
    const pt_rt_skill *sk = a->open ? pt_rt_skills(a->s.rt, rescan, &n) : NULL;
    char label[80];
    for (i = 0; i < n; i++) {
        int ln = snprintf(label, sizeof label, "%s%.*s", lead, (int)sk[i].name.n, sk[i].name.p);
        if (ln > 0 && (size_t)ln < sizeof label && pt_str_starts(pt_strz(label), q)) {
            pt_buf_reset(&a->tmp);
            pt_buf_puts(&a->tmp, sk[i].description);
            pt_buf_putc(&a->tmp, 0);
            add_item(a, label, (size_t)ln, a->tmp.p);
        }
    }
    a->skills_menu = true;
}

/* The word being typed: after the last space or newline. */
static pt_str last_word(pt_str d)
{
    size_t at = d.n;
    while (at && d.p[at - 1] != ' ' && d.p[at - 1] != '\n' && d.p[at - 1] != '\t')
        at--;
    return (pt_str){d.p + at, d.n - at};
}

static void add_list(app *a, const char *const *v, size_t n, pt_str q)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (pt_str_starts(pt_strz(v[i]), q))
            add_item(a, v[i], strlen(v[i]), NULL);
}

/* The menu follows the draft: commands after "/", then their arguments. */
static void list_sessions(void *ud)
{
    session_list *list = (session_list *)ud;
    list->n = cli_sessions_list(list->cwd, list->ids, 50);
}

static session_list *get_sessions(app *a, bool refresh)
{
    session_list *list = a->sessions;
    if (!list) {
        list = (session_list *)calloc(1, sizeof *list);
        if (!list)
            return NULL;
        list->worker = pt_worker_new();
        if (!list->worker || !getcwd(list->cwd, sizeof list->cwd)) {
            pt_worker_free(list->worker);
            free(list);
            return NULL;
        }
        a->sessions = list;
    }
    if (refresh && !list->pending)
        list->ready = false;
    if (!list->ready && !list->pending)
        list->pending = pt_worker_start(list->worker, list_sessions, list);
    return list->ready ? list : NULL;
}

static void menu_update(app *a)
{
    bool was_sessions = a->sessions_menu, was_skills = a->skills_menu;
    pt_str d = pt_buf_str(&a->in.text), cmd, arg, word = last_word(d);
    long sp;
    int old = a->nitems;
    a->nitems = 0;
    pt_buf_reset(&a->items_buf);
    a->sessions_menu = a->skills_menu = false;
    if (a->menu == MENU_SETTINGS)
        return;
    a->menu = MENU_NONE;
    if (!a->menu_closed && word.n && word.p[0] == '$') { /* $skill anywhere */
        add_skills(a, "$", word, !was_skills);
        a->menu = a->nitems ? MENU_ARG : MENU_NONE;
        if (a->nitems != old || a->sel >= a->nitems)
            a->sel = a->menu_top = 0;
        return;
    }
    if (a->menu_closed || !d.n || d.p[0] != '/' || pt_str_find(d, '\n') >= 0)
        return;
    d.p++;
    d.n--;
    sp = pt_str_find(d, ' ');
    if (sp < 0) {
        size_t i;
        a->menu = MENU_CMD;
        for (i = 0; i < NCMDS; i++)
            if (pt_str_starts(pt_strz(cmds[i].name), d))
                add_item(a, cmds[i].name, strlen(cmds[i].name), cmds[i].desc);
        add_skills(a, "skill:", d, !was_skills);
    } else {
        cmd = (pt_str){d.p, (size_t)sp};
        arg = (pt_str){d.p + sp + 1, d.n - (size_t)sp - 1};
        a->menu = MENU_ARG;
        if (pt_str_eqz(cmd, "settings")) { /* "/settings model <id>" etc. */
            long sp2 = pt_str_find(arg, ' ');
            if (sp2 >= 0) {
                int st = find_setting((pt_str){arg.p, (size_t)sp2});
                arg = (pt_str){arg.p + sp2 + 1, arg.n - (size_t)sp2 - 1};
                cmd = PT_STR_NULL;
                if (st >= 0 && settings[st].vals)
                    add_list(a, settings[st].vals, (size_t)settings[st].nv, arg);
                else if (st >= 0 && settings[st].k != CFG_PAGER)
                    cmd = PT_S("model");
            } else {
                size_t i;
                for (i = 0; i < NSETTINGS; i++)
                    if (pt_str_starts(pt_strz(settings[i].name), arg))
                        add_item(a, settings[i].name, strlen(settings[i].name), cli_config_key(settings[i].k));
                cmd = PT_STR_NULL;
            }
        }
        if (pt_str_eqz(cmd, "model")) {
            add_models(a, arg);
        } else if (pt_str_eqz(cmd, "effort")) {
            add_list(a, levels, sizeof levels / sizeof levels[0], arg);
        } else if (pt_str_eqz(cmd, "thinking")) {
            add_list(a, th_names, 3, arg);
        } else if (pt_str_eqz(cmd, "resume")) {
            session_list *list = get_sessions(a, !was_sessions);
            uint32_t i;
            a->sessions_menu = true;
            for (i = 0; list && i < list->n; i++)
                if (pt_str_starts(pt_strz(list->ids[i]), arg) &&
                    (!a->open || strcmp(list->ids[i], a->s.id) != 0))
                    add_item(a, list->ids[i], strlen(list->ids[i]), NULL);
        }
        if (!a->nitems)
            a->menu = MENU_NONE;
    }
    if (a->nitems != old || a->sel >= a->nitems)
        a->sel = a->menu_top = 0;
}

static void row(app *a, pt_str styled)
{
    tui_frame_row(&a->frame, styled, a->cols);
}

/* The activity row: a spinner, then what the run is doing with a bright
 * band sweeping across the word, then the time and the hint. */
static void spinner_row(app *a, int64_t now)
{
    char line[400];
    const char *what = a->phase == PH_THINK     ? "Thinking"
                       : a->phase == PH_WRITE   ? "Writing"
                       : a->phase == PH_COMPACT ? "Compacting"
                                                : "Waiting";
    char running[80];
    int64_t frame = now / SPIN_MS;
    char el[32];
    const char *spin = a->sy->spin[frame % a->sy->nspin];
    const char *hint = now - a->esc_at < DOUBLE_MS ? "esc again to interrupt" : "esc esc to interrupt";
    int n, i, at;
    if (a->phase == PH_TOOL && a->nruns == 1) {
        snprintf(running, sizeof running, "Running %s", a->runs[0].name);
        what = running;
    } else if (a->phase == PH_TOOL && a->nruns) {
        snprintf(running, sizeof running, "Running %d calls", a->nruns);
        what = running;
    }
    pt_buf_reset(&a->tmp);
    tui_style(&a->tmp, TUI_BOLD);
    pt_buf_putz(&a->tmp, spin);
    pt_buf_putc(&a->tmp, ' ');
    if (a->phase == PH_RETRY) {
        int64_t left = (a->retry_until - now + 999) / 1000;
        tui_style(&a->tmp, TUI_DIM);
        snprintf(line, sizeof line, "Retrying in %llds · %s · %.*s", (long long)(left > 0 ? left : 0), hint,
                 (int)(a->retry_err.n > 200 ? 200 : a->retry_err.n), a->retry_err.p);
        pt_buf_putz(&a->tmp, line);
    } else {
        n = (int)strlen(what);
        at = (int)(frame % (n + 8)) - 3; /* the band: three letters, then a pause */
        for (i = 0; i < n; i++) {
            if (i == 0 || i == at || i == at + 3)
                tui_style(&a->tmp, i >= at && i < at + 3 ? 0 : TUI_DIM);
            pt_buf_putc(&a->tmp, what[i]);
        }
        tui_style(&a->tmp, TUI_DIM);
        secs_short(el, sizeof el, now - a->run_at);
        snprintf(line, sizeof line, " · %s · %s", el, hint);
        pt_buf_putz(&a->tmp, line);
    }
    row(a, pt_buf_str(&a->tmp));
}

static void blank_row(app *a)
{
    row(a, PT_STR_NULL);
}

/* A token count, short: 950, 12.3k, 200k, 1.2M. */
static void tokens_short(char *out, size_t cap, uint64_t n)
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

/* Below the input: the model (without its provider) and effort, or what
 * the keys do while a menu is open. */
static void status_row(app *a, int64_t now, int total, int sel)
{
    char line[600];
    const char *model = strchr(a->model, '/');
    int n;
    pt_buf_reset(&a->tmp);
    if (now - a->ctrlc_at < DOUBLE_MS) {
        tui_style(&a->tmp, TUI_DIM);
        pt_buf_putz(&a->tmp, "ctrl-c again to quit");
    } else if (a->asked) {
        tui_style(&a->tmp, TUI_DIM);
        pt_buf_putz(&a->tmp, "1-3 or ↑↓ enter · esc no · ctrl-c interrupt");
    } else if (a->menu == MENU_SETTINGS) {
        tui_style(&a->tmp, TUI_DIM);
        pt_buf_putz(&a->tmp, "↑↓ select · ←→ change · enter edit · esc close");
    } else if (a->menu != MENU_NONE) {
        tui_style(&a->tmp, TUI_DIM);
        snprintf(line, sizeof line, "↑↓ select · tab complete · enter run · esc close%s", total > 1 ? " · " : "");
        pt_buf_putz(&a->tmp, line);
        if (total > 1) {
            snprintf(line, sizeof line, "%d/%d", sel + 1, total);
            pt_buf_putz(&a->tmp, line);
        }
    } else {
        tui_style(&a->tmp, TUI_BOLD);
        pt_buf_putz(&a->tmp, model ? model + 1 : a->model[0] ? a->model : "no model");
        tui_style(&a->tmp, TUI_DIM);
        n = snprintf(line, sizeof line, " · %s", pt_reasoning_name(a->o.reasoning));
        if (a->ctx_window) { /* the context used, of the model's window */
            char used[16], all[16];
            tokens_short(used, sizeof used, a->ctx_used);
            tokens_short(all, sizeof all, a->ctx_window);
            snprintf(line + n, sizeof line - (size_t)n, " · %s/%s", used, all);
        }
        pt_buf_putz(&a->tmp, line);
    }
    row(a, pt_buf_str(&a->tmp));
}

static void menu_row(app *a, int i)
{
    bool on = i == a->sel;
    pt_buf_reset(&a->tmp);
    tui_style(&a->tmp, TUI_BOLD);
    pt_buf_putz(&a->tmp, on ? a->sy->sel : " ");
    pt_buf_putc(&a->tmp, ' ');
    if (a->menu == MENU_SETTINGS) {
        int k = settings[i].k;
        char v[600];
        tui_style(&a->tmp, on ? TUI_BOLD : 0);
        snprintf(v, sizeof v, "%-10s ", settings[i].name);
        pt_buf_putz(&a->tmp, v);
        tui_style(&a->tmp, TUI_CYAN);
        pt_buf_putz(&a->tmp, a->cfg->val[k][0] ? a->cfg->val[k] : "(none)");
        if (strcmp(a->cfg->src[k], "default") != 0 && strcmp(a->cfg->src[k], "user") != 0) {
            tui_style(&a->tmp, TUI_DIM);
            snprintf(v, sizeof v, "  (set by %s)", a->cfg->src[k]);
            pt_buf_putz(&a->tmp, v);
        }
    } else {
        int col = 0;
        tui_style(&a->tmp, on ? TUI_BOLD : 0);
        if (a->menu == MENU_CMD)
            pt_buf_putc(&a->tmp, '/');
        tui_clean(&a->tmp, item_label(a, i), &col);
        if (a->items[i].dn) {
            int pad = a->label_w - tui_width(item_label(a, i), true);
            tui_style(&a->tmp, TUI_DIM);
            for (pad = pad > 0 ? pad + 2 : 2; pad > 0; pad--)
                pt_buf_putc(&a->tmp, ' ');
            tui_clean(&a->tmp, (pt_str){a->items_buf.p + a->items[i].dat, a->items[i].dn}, &col);
        }
    }
    row(a, pt_buf_str(&a->tmp));
}

/* The first tool call waiting for approval, if any. */
static bool asking(app *a)
{
    if (!a->open || a->opening || !pt_asks(a->s.a, &a->ask, 1))
        return false;
    if (a->ask.task != a->ask_task) { /* a new question */
        pt_str g = call_gist(a, a->ask.args);
        size_t n = 0;
        a->ask_task = a->ask.task;
        a->ask_sel = 0;
        /* bash: allow commands with the same first word; others: the tool */
        if (pt_str_eqz(a->ask.name, "bash"))
            while (n < g.n && n + 1 < sizeof a->ask_prefix && !strchr(" \t\n;&|<>()`$", g.p[n]))
                n++;
        memcpy(a->ask_prefix, g.p, n);
        a->ask_prefix[n] = 0;
    }
    return true;
}

static void ask_row(app *a, int i)
{
    char line[300];
    bool on = i == a->ask_sel;
    pt_buf_reset(&a->tmp);
    tui_style(&a->tmp, TUI_BOLD);
    pt_buf_putz(&a->tmp, on ? a->sy->sel : " ");
    tui_style(&a->tmp, on ? TUI_BOLD : 0);
    if (i == 0)
        snprintf(line, sizeof line, " 1. Yes");
    else if (i == 1 && a->ask_prefix[0])
        snprintf(line, sizeof line, " 2. Yes, and allow %.*s \"%s\" this session", (int)a->ask.name.n,
                 a->ask.name.p, a->ask_prefix);
    else if (i == 1)
        snprintf(line, sizeof line, " 2. Yes, and allow %.*s this session", (int)a->ask.name.n, a->ask.name.p);
    else
        snprintf(line, sizeof line, " 3. No (esc)");
    pt_buf_putz(&a->tmp, line);
    row(a, pt_buf_str(&a->tmp));
}

/* The question: the call on one row ("● Run make test"), then the choices. */
static void ask_rows(app *a, int n)
{
    int col = 0, vi = verb_of(a->ask.name), i;
    size_t k;
    pt_buf_reset(&a->tmp2);
    tui_style(&a->tmp2, TUI_BOLD);
    pt_buf_putz(&a->tmp2, a->sy->bullet);
    tui_style(&a->tmp2, 0);
    pt_buf_putc(&a->tmp2, ' ');
    if (vi < 0 && a->ask.prompt.n) { /* another tool: its name and what it asks */
        size_t at = a->tmp2.n;
        pt_buf_put(&a->tmp2, a->ask.name.p, a->ask.name.n > 60 ? 60 : a->ask.name.n);
        pt_buf_putc(&a->tmp2, ' ');
        tui_clean(&a->tmp2, a->ask.prompt, &col);
        for (k = at; k < a->tmp2.n; k++)
            if (a->tmp2.p[k] == '\n')
                a->tmp2.p[k] = ' ';
    } else {
        phrase(a, &a->tmp2, 0, vi < 0 ? NULL : verbs[vi].ask, a->ask.name, a->ask.args);
    }
    k = tui_fit(pt_buf_str(&a->tmp2), width(a), false);
    a->tmp2.n = k;
    row(a, pt_buf_str(&a->tmp2));
    for (i = 0; i < 3 && i + 1 < n; i++)
        ask_row(a, i);
}

/* Build the band (16.2.2) within its height budget. */
static void build_frame(app *a, int64_t now)
{
    int avail = a->rows - 2, w = width(a), show, live_n = 0, nmenu, nq, act, st, gaps, i, first, top, nask;
    int total = a->menu == MENU_SETTINGS ? (int)NSETTINGS : a->nitems;
    int sel = a->menu == MENU_SETTINGS ? a->set_sel : a->sel;
    bool tiny = a->cols < 20 || a->rows < 4;
    pt_buf *live_rows = &a->live_rows;
    tui_frame_reset(&a->frame);
    if (avail < 1)
        avail = 1;
    /* the pieces */
    pt_buf_reset(live_rows);
    if (a->ngrows || a->nruns) { /* the open group */
        pt_buf_putc(live_rows, '\n');
        group_render(a, live_rows, true);
    }
    if (a->live_kind != LIVE_NONE && a->live.n) {
        tui_md copy = a->md;
        tui_md_line(&copy, pt_buf_str(&a->live), a->cols, a->live_kind == LIVE_THINK ? TUI_DIM : 0, true,
                    live_rows);
    }
    for (i = 0; (size_t)i < live_rows->n; i++)
        live_n += live_rows->p[i] == '\n';
    a->asked = asking(a);
    nask = a->asked ? 4 : 0;
    act = a->running && !nask ? 1 : 0;
    nq = a->running ? (int)(a->nq > 3 ? 4 : a->nq) : 0;
    nmenu = a->menu == MENU_SETTINGS ? (int)NSETTINGS : a->menu != MENU_NONE ? a->nitems : 0;
    if (nmenu > 8)
        nmenu = 8;
    if (nask)
        nmenu = 0;
    st = 1;
    gaps = 2;
    show = a->rows / 3 < 8 ? a->rows / 3 : 8;
    if (show < 1)
        show = 1;
    tui_input_layout(&a->in, w > 3 ? w - 2 : 1, show);
    show = a->in.nrows < show ? a->in.nrows : show;
    if (tiny) {
        live_n = act = nq = nmenu = st = gaps = 0;
        show = 1;
        nask = nask ? 1 : 0;
    }
    /* drop parts until it fits */
    while (live_n + nask + act + nq + show + nmenu + st + gaps > avail) {
        int over = live_n + nask + act + nq + show + nmenu + st + gaps - avail;
        if (nmenu) {
            nmenu = nmenu > over ? nmenu - over : 0;
        } else if (nq > 1) {
            nq = 1;
        } else if (live_n) {
            live_n = live_n > over ? live_n - over : 0;
        } else if (gaps) {
            gaps = 0;
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
    if (act)
        spinner_row(a, now);
    for (i = 0; i < nq; i++) {
        char more[48];
        pt_buf_reset(&a->tmp);
        tui_style(&a->tmp, TUI_DIM);
        if (i == nq - 1 && (uint32_t)nq < a->nq) {
            snprintf(more, sizeof more, "  +%u more queued", a->nq - (uint32_t)i);
            pt_buf_putz(&a->tmp, more);
        } else {
            int col = 0;
            pt_str q = pt_buf_str(&a->queue[i].text);
            long nl = pt_str_find(q, '\n');
            pt_buf_putz(&a->tmp, "  queued: ");
            if (nl >= 0)
                q.n = (size_t)nl;
            tui_clean(&a->tmp, q, &col);
        }
        row(a, pt_buf_str(&a->tmp));
    }
    /* the input box, with a blank row above and below */
    if (gaps)
        blank_row(a);
    first = a->frame.n;
    top = a->in.scroll;
    for (i = top; i < a->in.nrows && i < top + show; i++) {
        const tui_irow *r = &a->in.rows[i];
        size_t pre;
        pt_buf_reset(&a->tmp);
        tui_style(&a->tmp, TUI_BOLD);
        pt_buf_putz(&a->tmp, a->sy->rail);
        tui_style(&a->tmp, 0);
        pt_buf_putc(&a->tmp, ' ');
        pre = a->tmp.n;
        pt_buf_put(&a->tmp, a->in.text.p + r->start, r->end - r->start);
        row(a, pt_buf_str(&a->tmp));
        if (i == a->in.cur_row) {
            a->frame.cur_row = a->frame.n - 1;
            a->frame.cur_at = pre + (a->in.cur - r->start);
        }
    }
    if (a->frame.n == first) { /* nothing laid out: an empty prompt */
        pt_buf_reset(&a->tmp);
        tui_style(&a->tmp, TUI_BOLD);
        pt_buf_putz(&a->tmp, a->sy->rail);
        tui_style(&a->tmp, 0);
        pt_buf_putc(&a->tmp, ' ');
        row(a, pt_buf_str(&a->tmp));
        a->frame.cur_row = a->frame.n - 1;
        a->frame.cur_at = a->tmp.n;
    }
    /* the menu: a window around the selection */
    if (nmenu) {
        int start, save = a->sel;
        if (sel < a->menu_top)
            a->menu_top = sel;
        if (sel >= a->menu_top + nmenu)
            a->menu_top = sel - nmenu + 1;
        if (a->menu_top > total - nmenu)
            a->menu_top = total - nmenu > 0 ? total - nmenu : 0;
        start = a->menu_top;
        a->sel = sel;
        a->label_w = 0;
        for (i = start; a->menu != MENU_SETTINGS && i < start + nmenu && i < total; i++) {
            int lw = tui_width(item_label(a, i), true);
            a->label_w = lw > a->label_w ? (lw > 40 ? 40 : lw) : a->label_w;
        }
        for (i = start; i < start + nmenu && i < total; i++)
            menu_row(a, i);
        a->sel = save;
    }
    if (gaps)
        blank_row(a);
    if (st)
        status_row(a, now, total, sel);
}

static void flush_out(app *a)
{
    int ms = tui_term_write(a->t, pt_buf_str(&a->band.out));
    pt_buf_reset(&a->band.out);
    /* writes that block (a slow link) slow the stream down */
    if (ms > 50 && a->interval < 500)
        a->interval *= 2;
    else if (ms < 10 && a->interval > 33)
        a->interval /= 2;
}

static void paint(app *a)
{
    int64_t now = pt_sys_now_ms();
    build_frame(a, now);
    if (a->in.text.oom || a->in.kill.oom || a->in.saved.oom || a->frame.rows.oom || a->lines.oom ||
        a->live.oom || a->tmp.oom || a->tmp2.oom || a->wrap.oom || a->items_buf.oom || a->grp.oom ||
        runs_oom(a) || a->live_rows.oom || a->diff.oom) {
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
    flush_out(a);
    a->dirty = a->urgent = false;
    a->last_paint = now;
}

/* ---- settings, model, sessions ---- */

static void set_config(app *a);

static void apply_symbols(app *a)
{
    const char *v = a->cfg->val[CFG_SYMBOLS], *term = getenv("TERM");
    const char *loc = getenv("LC_ALL");
    bool utf8;
    if (!loc || !*loc)
        loc = getenv("LC_CTYPE");
    if (!loc || !*loc)
        loc = getenv("LANG");
    utf8 = loc && (strstr(loc, "UTF-8") || strstr(loc, "utf-8") || strstr(loc, "UTF8") || strstr(loc, "utf8"));
    if (strcmp(v, "ascii") == 0)
        a->ascii = true;
    else if (strcmp(v, "unicode") == 0)
        a->ascii = false;
    else
        a->ascii = !term || !utf8 || strncmp(term, "linux", 5) == 0 || strncmp(term, "vt", 2) == 0;
    a->sy = a->ascii ? &asc : &uni;
    a->md.ascii = a->ascii;
}

static void apply_thinking(app *a)
{
    int i;
    a->thinking = TH_COLLAPSED;
    for (i = 0; i < 3; i++)
        if (strcmp(a->cfg->val[CFG_THINKING], th_names[i]) == 0)
            a->thinking = i;
}

static void write_setting(void *ud)
{
    config_job *job = (config_job *)ud;
    job->ok = cli_config_write(job->path, job->key, job->value, job->err, sizeof job->err);
}

static void start_setting(app *a)
{
    config_job *job = a->saving;
    int k;
    if (!job || job->active || !job->pending)
        return;
    for (k = 0; k < CFG_N; k++)
        if (job->pending & (1u << k))
            break;
    job->key = k;
    memcpy(job->value, job->next[k], sizeof job->value);
    job->pending &= ~(1u << k);
    job->active = pt_worker_start(job->worker, write_setting, job);
    if (!job->active)
        out_z(a, TUI_RED, "not saved: could not schedule the settings write");
}

static void finish_setting(app *a)
{
    config_job *job = a->saving;
    if (job && job->active && pt_worker_finish(job->worker)) {
        char msg[700];
        unsigned i;
        job->active = false;
        if (!job->ok) {
            group_close(a);
            out_text(a, &a->lines, TUI_RED, NULL, 0, pt_strz(job->err), 2);
        } else {
            for (i = 0; i < NSETTINGS && settings[i].k != job->key; i++)
                ;
            snprintf(msg, sizeof msg, "saved %s: %s", i < NSETTINGS ? settings[i].name : "setting",
                     job->value[0] ? job->value : "(none)");
            out_z(a, TUI_DIM, msg);
        }
        a->urgent = true;
    }
    start_setting(a);
}

/* Apply now and queue a durable save; report success after it lands. */
static bool save_setting(app *a, int k, const char *v)
{
    char err[700];
    bool ok = true;
    pt_ai_target t;
    const char *why = NULL;
    pt_arena_reset(&a->ar);
    if ((k == CFG_MODEL || (k == CFG_COMPACT_MODEL && *v)) &&
        !pt_ai_resolve(pt_ai_builtin_catalog(), pt_strz(v), &t, &a->ar, &why)) {
        snprintf(err, sizeof err, "%s: %s", v, why ? why : "unknown");
        out_z(a, TUI_RED, err);
        return false;
    }
    if (strcmp(a->cfg->src[k], "default") == 0 || strcmp(a->cfg->src[k], "user") == 0)
        cli_config_set(a->cfg, k, v, "user");
    if (!a->saving) {
        config_job *job = (config_job *)calloc(1, sizeof *job);
        if (job) {
            job->worker = pt_worker_new();
            snprintf(job->path, sizeof job->path, "%s", a->cfg->user_path);
            if (!job->worker) {
                free(job);
                job = NULL;
            }
        }
        a->saving = job;
    }
    if (!a->saving) {
        out_z(a, TUI_RED, "not saved: could not allocate the settings worker");
        ok = false;
    } else {
        snprintf(a->saving->next[k], sizeof a->saving->next[k], "%s", v);
        a->saving->pending |= 1u << k;
        start_setting(a);
    }
    if (k == CFG_THINKING)
        apply_thinking(a);
    if (k == CFG_SYMBOLS)
        apply_symbols(a);
    if (k == CFG_PERMS && pt_perm_parse(pt_strz(a->cfg->val[k]), &a->o.perm) && a->open)
        pt_rt_set_perm(a->s.rt, a->o.perm);
    if (k == CFG_SKILLS) { /* from the next epoch on: compaction or a new session */
        a->o.manual_skills = strcmp(a->cfg->val[k], "off") == 0;
        if (a->open)
            pt_rt_skills_auto(a->s.rt, !a->o.manual_skills);
    }
    if (k == CFG_COMPACT_MODEL) {
        a->o.compact_model = a->cfg->val[k][0] ? a->cfg->val[k] : NULL;
        set_config(a);
    }
    if (k == CFG_REASONING) {
        pt_reasoning r;
        if (cli_effort_parse(pt_strz(a->cfg->val[k]), &r)) {
            a->o.reasoning = r;
            set_config(a);
        }
    }
    return ok;
}

/* The model shown: the configured one, else the session's last. */
static void update_model(app *a)
{
    uint32_t n = a->open ? pt_entry_count(a->s.a) : 0;
    pt_entry_view v;
    pt_ai_target t;
    const char *why;
    a->model[0] = 0;
    if (a->o.model)
        snprintf(a->model, sizeof a->model, "%s", a->o.model);
    while (n-- > 0 && pt_entry_at(a->s.a, n, &v))
        if (strcmp(v.kind, "assistant") == 0 && v.msg->model.n) {
            if (!a->model[0])
                snprintf(a->model, sizeof a->model, "%.*s/%.*s", (int)v.msg->provider.n, v.msg->provider.p,
                         (int)v.msg->model.n, v.msg->model.p);
            break;
        }
    a->ctx_used = a->open ? pt_context_tokens(a->s.a) : 0;
    a->ctx_window = 0;
    pt_arena_reset(&a->ar);
    if (a->model[0] && pt_ai_resolve(pt_ai_builtin_catalog(), pt_strz(a->model), &t, &a->ar, &why))
        a->ctx_window = t.model.context_window;
}

static void set_config(app *a)
{
    pt_config c;
    memset(&c, 0, sizeof c);
    c.model = a->o.model ? pt_strz(a->o.model) : PT_STR_NULL;
    c.compact_model = a->o.compact_model ? pt_strz(a->o.compact_model) : PT_STR_NULL;
    c.reasoning = a->o.reasoning;
    c.max_tokens = a->o.max_tokens;
    c.retry_base_ms = a->o.retry_base_ms;
    c.context_window = a->o.context_window;
    c.no_sync = a->o.no_sync;
    if (a->open)
        pt_set_config(a->s.a, &c);
    update_model(a);
}

static void draft_path(const app *a, char *out, size_t cap)
{
    snprintf(out, cap, "%s/draft", a->s.dir);
}

static void save_draft(app *a)
{
    char p[4300];
    if (!a->open)
        return;
    draft_path(a, p, sizeof p);
    if (a->in.text.n)
        pt_sys_write_file(p, a->in.text.p, a->in.text.n);
    else
        unlink(p);
    a->draft_dirty = false;
    a->draft_at = pt_sys_now_ms();
}

/* The transcript from the third-newest user entry on (16.2.5). */
static void out_tail(app *a)
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
        out_z(a, TUI_DIM, line);
    }
    for (i = from; i < n && pt_entry_at(a->s.a, i, &v); i++)
        show_entry(a, &v);
    a->seen = n;
    if (n && pt_entry_at(a->s.a, 0, &v)) /* else the next commit looks like a new epoch */
        a->shown_epoch = v.epoch;
}

static void reset_display(app *a)
{
    uint32_t i;
    group_close(a); /* the old session's, to the scrollback */
    pt_buf_reset(&a->live);
    a->live_kind = LIVE_NONE;
    a->streamed = a->started = a->printed = a->running = false;
    a->ctx_used = 0;
    for (i = 0; i < a->nq; i++)
        pt_buf_reset(&a->queue[i].text);
    a->nq = 0;
}

static bool session_empty(app *a)
{
    uint32_t i, n;
    pt_entry_view v;
    if (!a->created || a->in.text.n)
        return false;
    for (i = 0; i < a->nq; i++)
        if (!a->queue[i].command)
            return false;
    n = pt_entry_count(a->s.a);
    for (i = 0; i < n && pt_entry_at(a->s.a, i, &v); i++)
        if (strcmp(v.kind, "epoch") != 0)
            return false;
    return true;
}

static bool open_session(app *a, const char *id)
{
    a->opening = cli_session_begin(&a->s, &a->o, id, !a->history_ready,
                                    a->open ? pt_buf_str(&a->in.text) : PT_STR_NULL,
                                    a->open && session_empty(a),
                                    on_event, a, a->err, sizeof a->err);
    if (!a->opening) {
        group_close(a);
        out_text(a, &a->lines, TUI_RED, NULL, 0, pt_strz(a->err), 2);
        a->code = 1;
        return false;
    }
    a->open = false;
    a->created = id == NULL && !a->o.continue_current;
    a->discarded = false;
    a->drop_restored_draft = false;
    reset_display(a);
    update_model(a);
    return true;
}

/* Close the session; an empty one this process made is discarded unless
 * /quit asked to keep it for resuming. */
static void finish_session(app *a);

static void close_session(app *a)
{
    bool empty;
    while (a->opening) {
        pt_waitfd fd;
        fd.fd = cli_session_job_fd(a->opening);
        pt_sys_wait(&fd, 1, -1);
        finish_session(a);
    }
    if (!a->open)
        return;
    empty = !a->keep_on_quit && session_empty(a);
    save_draft(a);
    if (pt_fatal(a->s.a) == PT_FATAL_STORAGE)
        cli_session_free(&a->s);
    else
        cli_session_close(&a->s);
    a->discarded = empty && !a->in.text.n;
    if (a->discarded) /* nothing was ever said: leave no session behind */
        cli_session_discard(a->s.dir);
    a->open = false;
}

/* /new, /resume: close this one, open the other; back to this one if
 * that fails. */
static void switch_session(app *a, const char *id)
{
    char back[64];
    snprintf(back, sizeof back, "%s", a->s.id);
    a->o.continue_current = false;
    snprintf(a->fallback, sizeof a->fallback, "%s", back);
    if (!open_session(a, id)) {
        a->fallback[0] = 0;
        a->code = 0; /* allocation failed before the old session was handed off */
        return;
    }
    tui_input_set(&a->in, PT_STR_NULL);
    a->code = 0;
}

/* ---- the pager (Ctrl-O) and suspending ---- */

static void suspend(app *a)
{
    tui_band_erase(&a->band);
    flush_out(a);
    tui_term_suspend(a->t);
}

static void resume(app *a)
{
    tui_term_resume(a->t);
    tui_term_size(a->t, &a->rows, &a->cols);
    a->band.rows = a->rows;
    a->band.cols = a->cols;
    a->urgent = true;
}

static void pager(app *a)
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

/* ---- commands ---- */

static void cmd_status(app *a)
{
    char line[5000];
    snprintf(line, sizeof line, "session %s, owned by pid %ld\nstore %s", a->s.id, (long)getpid(), a->s.dir);
    out_z(a, TUI_DIM, line);
    snprintf(line, sizeof line, "model %s · effort %s", a->model[0] ? a->model : "(none)",
             pt_reasoning_name(a->o.reasoning));
    out_z(a, TUI_DIM, line);
    if (a->ctx_window)
        snprintf(line, sizeof line, "context %llu of %u tokens (%u%%)", (unsigned long long)a->ctx_used, a->ctx_window,
                 (unsigned)(a->ctx_used * 100 / a->ctx_window));
    else
        snprintf(line, sizeof line, "context %llu tokens", (unsigned long long)a->ctx_used);
    out_z(a, TUI_DIM, line);
    snprintf(line, sizeof line, "terminal %dx%d · %s · %s · kitty keys %s · %s · %s symbols", a->cols, a->rows,
             getenv("TERM") ? getenv("TERM") : "(no TERM)", a->band.reflow ? "reflows" : "does not reflow",
             a->kitty ? "on" : "off", a->answered ? "answers queries" : "no query replies",
             a->ascii ? "ascii" : "unicode");
    out_z(a, TUI_DIM, line);
}

/* The usage doc: tokens per model, and what they cost where prices are known. */
static void cmd_usage(app *a)
{
    const pt_json *u, *c;
    char line[600], in[16], out[16], cache[16];
    pt_arena_reset(&a->ar);
    u = a->open ? pt_json_parse(pt_doc(a->s.a, PT_S("usage")), &a->ar, NULL) : NULL;
    if (!pt_json_is(u, PT_J_OBJ) || !u->len) {
        out_z(a, TUI_DIM, "no tokens used yet");
        return;
    }
    for (c = u->child; c; c = c->next) {
        uint64_t ti = (uint64_t)pt_json_geti(c, "input", 0), to = (uint64_t)pt_json_geti(c, "output", 0);
        uint64_t cr = (uint64_t)pt_json_geti(c, "cache_read", 0), cw = (uint64_t)pt_json_geti(c, "cache_write", 0);
        pt_ai_target t;
        const char *why;
        int n;
        tokens_short(in, sizeof in, ti + cr + cw);
        tokens_short(out, sizeof out, to);
        tokens_short(cache, sizeof cache, cr);
        n = snprintf(line, sizeof line, "%.*s: %s in (%s from cache) · %s out · %lld requests", (int)c->key.n,
                     c->key.p, in, cache, out, (long long)pt_json_geti(c, "requests", 0));
        if (pt_ai_resolve(pt_ai_builtin_catalog(), c->key, &t, &a->ar, &why) && t.known && t.model.cost.input) {
            const pt_cost *k = &t.model.cost; /* micro-dollars per million tokens */
            double usd = ((double)ti * (double)k->input + (double)to * (double)k->output +
                          (double)cr * (double)k->cache_read + (double)cw * (double)k->cache_write) / 1e12;
            snprintf(line + n, sizeof line - (size_t)n, " · $%.2f", usd);
        }
        out_z(a, TUI_DIM, line);
    }
}

static void cmd_help(app *a)
{
    size_t i;
    char line[200];
    for (i = 0; i < NCMDS; i++) {
        snprintf(line, sizeof line, "/%s%s  %s", cmds[i].name, cmds[i].args, cmds[i].desc);
        out_z(a, TUI_DIM, line);
    }
    out_z(a, TUI_DIM, "$name anywhere in a message, or /skill:name first, gives the model a skill's instructions");
    out_z(a, TUI_DIM,
          "enter send · shift+enter, alt+enter or ctrl-j newline · esc esc interrupt · ctrl-c clear, "
          "interrupt or quit · ctrl-d quit · ctrl-t thinking · ctrl-o details · ctrl-z suspend · ctrl-l redraw");
}

static bool valid_setting(int s, const char *v)
{
    int i;
    for (i = 0; i < settings[s].nv; i++)
        if (strcmp(v, settings[s].vals[i]) == 0)
            return true;
    return !settings[s].vals;
}

static void run_command(app *a, pt_str line)
{
    pt_str cmd = line, arg = PT_S("");
    long sp = pt_str_find(line, ' ');
    char v[600], msg[700];
    if (sp >= 0) {
        cmd.n = (size_t)sp;
        arg = pt_str_trim((pt_str){line.p + sp + 1, line.n - (size_t)sp - 1});
    }
    cmd.p++;
    cmd.n--;
    snprintf(v, sizeof v, "%.*s", (int)(arg.n < sizeof v ? arg.n : sizeof v - 1), arg.p);
    group_close(a);
    out_blank(&a->lines); /* the command itself is not echoed: only what it did */
    if (pt_str_eqz(cmd, "model")) {
        if (arg.n) {
            pt_ai_target t;
            const char *why = NULL;
            pt_arena_reset(&a->ar);
            if (!pt_ai_resolve(pt_ai_builtin_catalog(), arg, &t, &a->ar, &why)) {
                snprintf(msg, sizeof msg, "%s: %s", v, why ? why : "unknown");
                out_z(a, TUI_RED, msg);
                return;
            }
            snprintf(a->model_ref, sizeof a->model_ref, "%.255s", v);
            a->o.model = a->model_ref;
            set_config(a);
        }
        snprintf(msg, sizeof msg, "%smodel: %s", arg.n ? "switched " : "",
                 a->model[0] ? a->model : "(the session's last)");
        out_z(a, TUI_DIM, msg);
    } else if (pt_str_eqz(cmd, "effort")) {
        if (arg.n) {
            pt_reasoning r;
            if (!cli_effort_parse(arg, &r)) {
                out_z(a, TUI_RED, "levels: off, minimal, low, medium, high, xhigh, max");
                return;
            }
            a->o.reasoning = r;
            set_config(a);
        }
        snprintf(msg, sizeof msg, "%seffort: %s", arg.n ? "switched " : "", pt_reasoning_name(a->o.reasoning));
        out_z(a, TUI_DIM, msg);
    } else if (pt_str_eqz(cmd, "new")) {
        switch_session(a, NULL);
    } else if (pt_str_eqz(cmd, "resume")) {
        if (!arg.n) {
            tui_input_set(&a->in, PT_S("/resume "));
            a->menu_closed = false;
            menu_update(a);
            return;
        }
        switch_session(a, v);
    } else if (pt_str_eqz(cmd, "status")) {
        cmd_status(a);
    } else if (pt_str_eqz(cmd, "compact")) {
        if (!a->open || !pt_compact(a->s.a))
            out_z(a, TUI_DIM, "nothing to compact yet");
    } else if (pt_str_eqz(cmd, "usage")) {
        cmd_usage(a);
    } else if (pt_str_eqz(cmd, "thinking")) {
        if (arg.n) {
            if (!valid_setting(find_setting(PT_S("thinking")), v)) {
                out_z(a, TUI_RED, "modes: collapsed, full, hidden");
                return;
            }
            if (!save_setting(a, CFG_THINKING, v))
                return;
        }
        snprintf(msg, sizeof msg, "%sthinking: %s", arg.n ? "switched " : "", th_names[a->thinking]);
        out_z(a, TUI_DIM, msg);
    } else if (pt_str_eqz(cmd, "settings")) {
        long sp2 = pt_str_find(arg, ' ');
        int s;
        if (!arg.n) {
            a->menu = MENU_SETTINGS;
            a->set_sel = a->menu_top = 0;
            return;
        }
        s = find_setting((pt_str){arg.p, sp2 >= 0 ? (size_t)sp2 : arg.n});
        if (s < 0) {
            size_t i;
            int n = snprintf(msg, sizeof msg, "settings:");
            for (i = 0; i < NSETTINGS && n > 0 && (size_t)n < sizeof msg; i++)
                n += snprintf(msg + n, sizeof msg - (size_t)n, "%s %s", i ? "," : "", settings[i].name);
            out_z(a, TUI_RED, msg);
            return;
        }
        if (sp2 >= 0) {
            pt_str val = pt_str_trim((pt_str){arg.p + sp2 + 1, arg.n - (size_t)sp2 - 1});
            snprintf(v, sizeof v, "%.*s", (int)(val.n < sizeof v ? val.n : sizeof v - 1), val.p);
            if (!valid_setting(s, v)) {
                snprintf(msg, sizeof msg, "not a value for %s: %s", settings[s].name, v);
                out_z(a, TUI_RED, msg);
                return;
            }
            if (!save_setting(a, settings[s].k, v))
                return;
        }
        if (sp2 >= 0) /* saved: the value, and where it was saved */
            snprintf(msg, sizeof msg, "saving %s: %s", settings[s].name,
                     a->cfg->val[settings[s].k][0] ? a->cfg->val[settings[s].k] : "(none)");
        else
            snprintf(msg, sizeof msg, "%s: %s (%s)", settings[s].name,
                     a->cfg->val[settings[s].k][0] ? a->cfg->val[settings[s].k] : "(none)",
                     a->cfg->src[settings[s].k]);
        out_z(a, TUI_DIM, msg);
    } else if (pt_str_eqz(cmd, "help")) {
        cmd_help(a);
    } else if (pt_str_eqz(cmd, "quit") || pt_str_eqz(cmd, "exit")) {
        a->keep_on_quit = true;
        a->quit = true;
    } else {
        snprintf(msg, sizeof msg, "unknown command /%.*s; /help lists them", (int)(cmd.n > 100 ? 100 : cmd.n), cmd.p);
        out_z(a, TUI_RED, msg);
    }
}

/* ---- keys ---- */

static void history_append(app *a, pt_str text)
{
    int fd;
    if (!a->hist_path[0])
        return;
    pt_buf_reset(&a->pending_history);
    tui_hist_line(&a->pending_history, text);
    if (a->pending_history.oom)
        return; /* best effort; never append a truncated JSON string */
    fd = open(a->hist_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd >= 0) {
        if (write(fd, a->pending_history.p, a->pending_history.n) != (ssize_t)a->pending_history.n) {
            /* best effort */
        }
        close(fd);
    }
    pt_buf_reset(&a->pending_history);
}

static void history_load(app *a)
{
    char root[4096], *slash;
    cli_sessions_dir(root, sizeof root);
    if ((slash = strrchr(root, '/')))
        *slash = 0;
    snprintf(a->hist_path, sizeof a->hist_path, "%s/history", root);
}

static bool queue_text(app *a, pt_str text, bool command)
{
    queued_input *q;
    if (a->nq == a->qcap) {
        uint32_t cap = a->qcap ? a->qcap * 2 : 8, i;
        queued_input *next;
        if (cap < a->qcap || SIZE_MAX / (size_t)cap < sizeof *next)
            return false;
        next = (queued_input *)realloc(a->queue, (size_t)cap * sizeof *next);
        if (!next)
            return false;
        for (i = a->qcap; i < cap; i++) {
            pt_buf_init(&next[i].text, pt_sys_allocator());
            next[i].command = next[i].submitted = false;
        }
        a->queue = next;
        a->qcap = cap;
    }
    q = &a->queue[a->nq];
    pt_buf_reset(&q->text);
    pt_buf_puts(&q->text, text);
    if (q->text.oom)
        return false;
    q->command = command;
    q->submitted = false;
    a->nq++;
    return true;
}

static void queued_remove(app *a, uint32_t at)
{
    queued_input kept = a->queue[at];
    memmove(a->queue + at, a->queue + at + 1, (a->nq - at - 1) * sizeof *a->queue);
    a->queue[--a->nq] = kept;
    pt_buf_reset(&a->queue[a->nq].text);
}

static void queued_submit(app *a)
{
    uint32_t i = 0;
    while (a->open && i < a->nq) {
        queued_input *q = &a->queue[i];
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
            pt_buf text;
            char err[600];
            bool ok;
            memset(&in, 0, sizeof in);
            in.text = pt_buf_str(&q->text);
            pt_buf_init(&text, pt_sys_allocator());
            switch (pt_rt_expand_skills(a->s.rt, in.text, &text, err, sizeof err)) {
            case -1: /* not sent: it is still in the history */
                out_z(a, TUI_RED, err);
                queued_remove(a, i);
                pt_buf_free(&text);
                continue;
            case 1:
                in.text = pt_buf_str(&text);
                break;
            }
            ok = pt_submit(a->s.a, &in);
            pt_buf_free(&text);
            if (!ok) {
                out_z(a, TUI_RED, pt_error(a->s.a));
                return;
            }
            q->submitted = true;
            i++;
        }
    }
}

static void finish_session(app *a)
{
    int rc;
    char *text;
    size_t n;
    if (!a->opening || !cli_session_finish(a->opening, &rc, a->err, sizeof a->err))
        return;
    a->opening = NULL;
    if (rc) {
        group_close(a);
        out_text(a, &a->lines, TUI_RED, NULL, 0, pt_strz(a->err), 2);
        a->urgent = true;
        if (a->fallback[0]) {
            char back[64];
            snprintf(back, sizeof back, "%s", a->fallback);
            a->fallback[0] = 0;
            if (open_session(a, back))
                return;
        }
        a->code = rc;
        a->quit = true;
        return;
    }
    a->open = true;
    a->fallback[0] = 0;
    if (!a->history_ready) {
        text = cli_session_take_history(&a->s, &n);
        if (text && !tui_hist_load(&a->in, (pt_str){text, n}, 1000))
            out_z(a, TUI_RED, "out of memory loading input history");
        free(text);
        if (!tui_hist_load(&a->in, pt_buf_str(&a->pending_history), UINT32_MAX))
            out_z(a, TUI_RED, "out of memory loading input history");
        if (a->pending_history.n) {
            int fd = open(a->hist_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
            if (fd >= 0) {
                (void)!write(fd, a->pending_history.p, a->pending_history.n);
                close(fd);
            }
        }
        pt_buf_reset(&a->pending_history);
        a->history_ready = true;
    }
    text = cli_session_take_draft(&a->s, &n);
    if (text && !a->drop_restored_draft) {
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
    queued_submit(a);
    if (a->pager_pending && !a->quit && a->open) {
        a->pager_pending = false;
        pager(a);
    }
    a->urgent = true;
}

static void submit(app *a)
{
    pt_str text = pt_str_trim(pt_buf_str(&a->in.text));
    bool command;
    if (!text.n)
        return;
    if (a->history_ready) {
        if (!tui_hist_add(&a->in, text))
            out_z(a, TUI_RED, "out of memory adding to input history");
        history_append(a, text);
    } else {
        tui_hist_line(&a->pending_history, text);
    }
    command = text.p[0] == '/' && pt_str_find(text, '\n') < 0 && !pt_str_starts(text, PT_S("/skill:"));
    if (command && !a->opening) {
        pt_buf_reset(&a->command);
        pt_buf_puts(&a->command, text);
        if (a->command.oom) {
            out_z(a, TUI_RED, "out of memory queuing a command");
            return;
        }
        tui_input_set(&a->in, PT_STR_NULL);
        a->menu = MENU_NONE;
        a->menu_closed = false;
        run_command(a, pt_buf_str(&a->command));
        menu_update(a);
        a->draft_dirty = true;
        return;
    }
    if (!a->open && !a->opening) {
        out_z(a, TUI_RED, "no session");
        return;
    }
    if (!queue_text(a, text, command)) {
        out_z(a, TUI_RED, "out of memory queuing input");
        return;
    }
    tui_input_set(&a->in, PT_STR_NULL);
    queued_submit(a);
    a->draft_dirty = true;
}

/* Tab: complete the selected menu item. Returns true when it changed
 * the draft. */
static bool complete(app *a)
{
    pt_str d = pt_buf_str(&a->in.text), label;
    pt_buf nd;
    bool changed;
    if (a->menu == MENU_NONE || a->menu == MENU_SETTINGS || !a->nitems)
        return false;
    label = item_label(a, a->sel);
    pt_buf_init(&nd, pt_sys_allocator());
    if (a->menu == MENU_CMD) {
        size_t i;
        pt_buf_putc(&nd, '/');
        pt_buf_puts(&nd, label);
        for (i = 0; i < NCMDS; i++)
            if (pt_str_eqz(label, cmds[i].name) && cmds[i].args[0])
                pt_buf_putc(&nd, ' ');
        if (pt_str_starts(label, PT_S("skill:"))) /* then what it is for */
            pt_buf_putc(&nd, ' ');
    } else { /* replace the last word */
        pt_buf_put(&nd, d.p, (size_t)(last_word(d).p - d.p));
        pt_buf_puts(&nd, label);
        if (label.n && label.p[0] == '$')
            pt_buf_putc(&nd, ' ');
    }
    changed = !pt_str_eq(pt_buf_str(&nd), d);
    if (changed)
        tui_input_set(&a->in, pt_buf_str(&nd));
    pt_buf_free(&nd);
    menu_update(a);
    return changed;
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
    if (a->menu == MENU_CMD && a->nitems) {
        pt_str label = item_label(a, a->sel);
        if (menu_takes_arg(label) && !pt_str_eqz(label, "settings")) {
            /* /model and friends: show their arguments first */
            if (complete(a))
                return;
        } else {
            complete(a);
        }
    } else if (a->menu == MENU_ARG && a->nitems) {
        bool skill = a->skills_menu;
        complete(a);
        if (skill)
            return; /* a $skill picked: the message goes on */
    }
    submit(a);
}

static void settings_key(app *a, const tui_ev *ev)
{
    int k = settings[a->set_sel].k, i, dir = ev->key == TK_LEFT ? -1 : 1;
    const char *const *vals = settings[a->set_sel].vals;
    int nv = settings[a->set_sel].nv;
    char d[600];
    if (ev->key == TK_UP || ev->key == TK_DOWN) {
        a->set_sel = (a->set_sel + (ev->key == TK_UP ? -1 : 1) + (int)NSETTINGS) % (int)NSETTINGS;
        return;
    }
    if (ev->key == TK_ESC || (ev->key == 'c' && ev->mods == TM_CTRL)) {
        a->menu = MENU_NONE;
        return;
    }
    if (ev->key != TK_LEFT && ev->key != TK_RIGHT && ev->key != TK_ENTER)
        return;
    if (vals) {
        for (i = 0; i < nv && strcmp(a->cfg->val[k], vals[i]) != 0; i++)
            ;
        save_setting(a, k, vals[((i < nv ? i : 0) + dir + nv) % nv]);
    } else { /* model, pager, compact-model: edit as a command */
        snprintf(d, sizeof d, "/settings %s %s", settings[a->set_sel].name, k == CFG_PAGER ? a->cfg->val[k] : "");
        a->menu = MENU_NONE;
        a->menu_closed = false;
        tui_input_set(&a->in, pt_strz(d));
        menu_update(a);
    }
}

static void interrupt(app *a)
{
    if (a->open && pt_abort(a->s.a))
        a->esc_at = 0;
}

/* Answer the question: 0 yes, 1 yes and allow alike, 2 no. */
static void answer(app *a, int choice)
{
    if (choice == 1 && !pt_rt_allow(a->s.rt, a->ask.name, pt_strz(a->ask_prefix)))
        out_z(a, TUI_RED, "out of memory: allowed this once only");
    if (!pt_approve(a->s.a, a->ask.task, choice != 2, PT_STR_NULL))
        out_z(a, TUI_RED, pt_error(a->s.a));
    a->ask_task = 0;
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
        a->ask_sel = (a->ask_sel + (ev->key == TK_UP ? 2 : 1)) % 3;
    else if (ev->key == TK_ENTER)
        answer(a, a->ask_sel);
    else if (ev->key == TK_ESC)
        answer(a, 2);
    else
        return false;
    return true;
}

static void on_key(app *a, const tui_ev *ev)
{
    int64_t now = pt_sys_now_ms();
    uint32_t k = ev->key;
    bool ctrl = ev->mods == TM_CTRL;
    tui_input_result r;
    size_t before = a->in.text.n;
    a->urgent = true;
    if (ask_key(a, ev))
        return;
    if (ev->kind == TE_TEXT || ev->kind == TE_PASTE) {
        if (a->menu == MENU_SETTINGS)
            a->menu = MENU_NONE;
        tui_input_insert(&a->in, ev->text);
        a->menu_closed = false;
        a->draft_dirty = true;
        menu_update(a);
        return;
    }
    if (ev->kind != TE_KEY)
        return;
    if (a->menu == MENU_SETTINGS) {
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
        if (a->menu != MENU_NONE) {
            a->menu_closed = true;
            menu_update(a);
        } else if (a->running) {
            if (now - a->esc_at < DOUBLE_MS)
                interrupt(a);
            else
                a->esc_at = now;
        }
        return;
    }
    if (ctrl && k == 'c') {
        if (a->opening)
            a->drop_restored_draft = true;
        if (a->in.text.n) {
            tui_input_set(&a->in, PT_STR_NULL);
            a->draft_dirty = true;
            menu_update(a);
        } else if (a->running) {
            interrupt(a);
        } else if (now - a->ctrlc_at < DOUBLE_MS) {
            a->quit = true;
        } else {
            a->ctrlc_at = now;
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
        save_setting(a, CFG_THINKING, th_names[(a->thinking + 1) % 3]);
        return;
    }
    if (ctrl && k == 'o') {
        if (a->opening)
            a->pager_pending = true;
        else
            pager(a);
        return;
    }
    if ((k == TK_UP || k == TK_DOWN) && !ev->mods && a->menu != MENU_NONE && a->nitems) {
        a->sel = (a->sel + (k == TK_UP ? -1 : 1) + a->nitems) % a->nitems;
        return;
    }
    r = tui_input_key(&a->in, ev, width(a) > 3 ? width(a) - 2 : 1);
    if (r == TI_TOP || r == TI_BOTTOM) {
        if (tui_hist_move(&a->in, r == TI_TOP ? -1 : 1)) {
            a->menu_closed = true; /* a recalled command does not open the menu */
            menu_update(a);
        }
        return;
    }
    if (r == TI_EDITED || a->in.text.n != before) {
        a->menu_closed = false;
        a->draft_dirty = true;
        menu_update(a);
    }
}

/* ---- setup and the loop ---- */

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
        a->answered = true;
        a->keys.want_cpr = false;
    } else if (ev->reply == TR_KITTY && !a->kitty) {
        a->kitty = true;
        tui_term_kitty(a->t, true);
    } else if (ev->reply == TR_CPR) {
        a->cpr = true;
        a->start_col = ev->b;
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
    if (a->urgent)
        AT(now);
    if (a->dirty)
        AT(a->last_paint + a->interval);
    if (tui_keys_pending(&a->keys))
        AT(a->keys_at + 30);
    if (a->running) {
        due = a->spin_at + SPIN_MS;
        AT(due);
    }
    if (now - a->esc_at < DOUBLE_MS)
        AT(a->esc_at + DOUBLE_MS);
    if (now - a->ctrlc_at < DOUBLE_MS)
        AT(a->ctrlc_at + DOUBLE_MS);
    if (a->winch_at)
        AT(a->winch_at);
    if (a->draft_dirty)
        AT(a->draft_at + 1000);
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
            on_key(a, &ev);
}

static int quit_code(int sig)
{
    return 128 + sig;
}

int tui_run(const cli_opts *o, cli_config *cfg, const char *id, const char *prompt)
{
    app a;
    const char *term = getenv("TERM"), *amb = getenv("PYTTE_AMBIGUOUS");
    int sig = 0, slot;
    char picked[64] = "", kept[64];
    memset(&a, 0, sizeof a);
    a.o = *o;
    a.cfg = cfg;
    a.interval = 33;
    a.esc_at = a.ctrlc_at = -DOUBLE_MS;
    /* the watchdog first: before the store is opened */
    if (!(a.t = tui_term_new())) {
        fprintf(stderr, "pytte: not a terminal\n");
        return 1;
    }
    if (o->model)
        snprintf(a.model_ref, sizeof a.model_ref, "%s", o->model);
    a.o.model = o->model ? a.model_ref : NULL;
    tui_set_ambiguous_wide(amb && strcmp(amb, "wide") == 0);
    a.color = !getenv("NO_COLOR") || !*getenv("NO_COLOR");
    apply_symbols(&a);
    apply_thinking(&a);
    tui_keys_init(&a.keys);
    a.keys.want_cpr = true;
    tui_input_init(&a.in, pt_sys_allocator());
    tui_frame_init(&a.frame, pt_sys_allocator());
    pt_buf_init(&a.lines, pt_sys_allocator());
    pt_buf_init(&a.tmp, pt_sys_allocator());
    pt_buf_init(&a.tmp2, pt_sys_allocator());
    pt_buf_init(&a.wrap, pt_sys_allocator());
    pt_buf_init(&a.live_rows, pt_sys_allocator());
    pt_buf_init(&a.live, pt_sys_allocator());
    pt_buf_init(&a.retry_err, pt_sys_allocator());
    for (slot = 0; slot < RUN_MAX; slot++) {
        pt_buf_init(&a.runs[slot].id, pt_sys_allocator());
        pt_buf_init(&a.runs[slot].head, pt_sys_allocator());
        pt_buf_init(&a.runs[slot].out, pt_sys_allocator());
    }
    pt_buf_init(&a.diff, pt_sys_allocator());
    pt_buf_init(&a.grp, pt_sys_allocator());
    pt_buf_init(&a.items_buf, pt_sys_allocator());
    pt_buf_init(&a.pending_history, pt_sys_allocator());
    pt_buf_init(&a.command, pt_sys_allocator());
    pt_arena_init(&a.ar, pt_sys_allocator(), 0);
    if (!tui_term_start(a.t, !term || (strncmp(term, "linux", 5) != 0 && strncmp(term, "vt", 2) != 0))) {
        tui_term_free(a.t);
        fprintf(stderr, "pytte: cannot set up the terminal\n");
        return 1;
    }
    tui_term_size(a.t, &a.rows, &a.cols);
    history_load(&a);
    if (id)
        snprintf(picked, sizeof picked, "%s", id);
    if (!open_session(&a, id ? picked : NULL)) {
        tui_term_free(a.t);
        fprintf(stderr, "pytte: %s\n", a.err);
        return a.code ? a.code : 1;
    }
    set_config(&a);
    tui_band_init(&a.band, a.rows, a.cols, pt_sys_allocator());
    a.band.reflow = reflows();
    a.band.color = a.color;
    {
        pt_waitfd fd;
        char buf[4096];
        long n;
        fd.fd = tui_term_in(a.t);
        if (pt_sys_wait(&fd, 1, 0) > 0 && fd.ready &&
            (n = tui_term_read(a.t, buf, sizeof buf)) > 0) {
            tui_keys_feed(&a.keys, buf, (size_t)n);
            read_keys(&a, false);
        }
        /* Without an immediate CPR, conservatively preserve a preceding
         * partial line. Its eventual reply cannot move the painted band. */
        if (a.start_col != 1)
            tui_term_write(a.t, PT_S("\r\n"));
    }
    {
        char head[100];
        pt_buf first;
        pt_buf_init(&first, pt_sys_allocator());
        snprintf(head, sizeof head, " v%s · /help for commands", PT_VERSION);
        out_text(&a, &first, TUI_DIM, "pytte", TUI_BOLD, pt_strz(head), 0);
        pt_buf_puts(&first, pt_buf_str(&a.lines));
        pt_buf_reset(&a.lines);
        pt_buf_puts(&a.lines, pt_buf_str(&first));
        pt_buf_free(&first);
    }
    if (prompt && *prompt) {
        a.drop_restored_draft = true;
        tui_input_set(&a.in, pt_strz(prompt));
        submit(&a);
    }
    menu_update(&a);
    paint(&a);
    while (!a.quit) {
        pt_waitfd fds[5];
        int64_t now = pt_sys_now_ms();
        int bits;
        bool was = a.running;
        fds[0].fd = tui_term_in(a.t);
        fds[1].fd = tui_term_sigfd(a.t);
        fds[2].fd = a.opening ? cli_session_job_fd(a.opening) : -1;
        fds[3].fd = a.sessions && a.sessions->pending ? pt_worker_fd(a.sessions->worker) : -1;
        fds[4].fd = a.saving && a.saving->active ? pt_worker_fd(a.saving->worker) : -1;
        if (a.opening) {
            pt_sys_wait(fds, 5, timeout_ms(&a, now));
            finish_session(&a);
        } else if (a.open)
            pt_rt_step(a.s.rt, timeout_ms(&a, now), fds, 5);
        else
            pt_sys_wait(fds, 5, timeout_ms(&a, now));
        finish_setting(&a);
        if (a.sessions && a.sessions->pending && pt_worker_finish(a.sessions->worker)) {
            a.sessions->pending = false;
            a.sessions->ready = true;
            menu_update(&a);
            a.urgent = true;
        }
        now = pt_sys_now_ms();
        a.running = a.open && !pt_idle(a.s.a) && !pt_fatal(a.s.a);
        if (a.running != was) {
            set_dirty(&a);
            if (a.running) {
                a.run_at = now;
                a.phase = PH_WAIT;
            }
        }
        if (fds[1].ready) {
            bits = tui_term_signals(a.t, &sig);
            if (bits & TS_HUP) {
                tui_term_gone(a.t);
                a.code = 129;
                break;
            }
            if (bits & TS_QUIT) {
                a.code = quit_code(sig);
                break;
            }
            if (bits & TS_TSTP) {
                suspend(&a);
                tui_term_stop_self();
                resume(&a);
            } else if (bits & TS_CONT) {
                resume(&a); /* stopped by someone else; the shell may have reset the modes */
                tui_band_redraw(&a.band);
            }
            if (bits & TS_WINCH)
                a.winch_at = now ? now : 1;
        }
        if (fds[0].ready) {
            char buf[4096];
            long n = tui_term_read(a.t, buf, sizeof buf);
            if (n == 0) { /* the terminal is gone */
                tui_term_gone(a.t);
                a.code = 129;
                break;
            }
            if (n > 0) {
                tui_keys_feed(&a.keys, buf, (size_t)n);
                a.keys_at = now;
                read_keys(&a, false);
            }
        }
        if (tui_keys_pending(&a.keys) && now - a.keys_at >= 30)
            read_keys(&a, true);
        if (a.winch_at && now >= a.winch_at) {
            int r, c;
            a.winch_at = 0;
            tui_term_size(a.t, &r, &c);
            if (r != a.rows || c != a.cols) {
                tui_band_resize(&a.band, r, c);
                a.rows = r;
                a.cols = c;
                a.urgent = true;
            }
        }
        if (a.running && now - a.spin_at >= SPIN_MS) {
            a.spin_at = now;
            set_dirty(&a);
        }
        if (now - a.esc_at < DOUBLE_MS + 50 || now - a.ctrlc_at < DOUBLE_MS + 50)
            set_dirty(&a);
        if (a.draft_dirty && now - a.draft_at >= 1000)
            save_draft(&a);
        if (a.quit)
            break;
        if (a.urgent || (a.dirty && now - a.last_paint >= a.interval))
            paint(&a);
    }
    /* quit: the close claim, then the terminal. What was shown of an
     * unfinished answer stays (pytte -c picks the run up again). */
    if (a.code != 129 && ((a.live_kind != LIVE_NONE && a.live.n) || a.ngrows)) {
        if (a.live_kind != LIVE_NONE && a.live.n) {
            live_end(&a);
            out_z(&a, TUI_DIM, "(detached)");
        }
        group_close(&a); /* the band is erased next */
        a.menu = MENU_NONE;
        paint(&a);
    }
    a.quit = true;
    while (a.opening) {
        pt_waitfd fd;
        fd.fd = cli_session_job_fd(a.opening);
        pt_sys_wait(&fd, 1, -1);
        finish_session(&a);
    }
    snprintf(kept, sizeof kept, "%s", a.open ? a.s.id : "");
    close_session(&a);
    while (a.saving && (a.saving->active || a.saving->pending)) {
        pt_waitfd fd;
        finish_setting(&a);
        if (!a.saving->active)
            continue;
        fd.fd = pt_worker_fd(a.saving->worker);
        pt_sys_wait(&fd, 1, -1);
    }
    if (a.saving) {
        pt_worker_free(a.saving->worker);
        free(a.saving);
    }
    if (a.sessions) {
        pt_worker_free(a.sessions->worker);
        free(a.sessions);
    }
    tui_band_erase(&a.band);
    flush_out(&a);
    if (a.code != 129 && kept[0] && !a.discarded) { /* how to come back to it */
        char last[200];
        pt_buf_reset(&a.lines);
        out_blank(&a.lines);
        snprintf(last, sizeof last, "to continue this session: pytte -r %s", kept);
        out_z(&a, TUI_DIM, last);
        pt_buf_reset(&a.tmp);
        tui_sgr(&a.tmp, pt_buf_str(&a.lines), a.color);
        tui_term_write(a.t, pt_buf_str(&a.tmp)); /* OPOST makes \n a new line */
    }
    tui_term_free(a.t);
    tui_keys_free(&a.keys);
    tui_input_free(&a.in);
    tui_frame_free(&a.frame);
    tui_band_free(&a.band);
    pt_buf_free(&a.lines);
    pt_buf_free(&a.tmp);
    pt_buf_free(&a.tmp2);
    pt_buf_free(&a.live_rows);
    pt_buf_free(&a.wrap);
    pt_buf_free(&a.live);
    pt_buf_free(&a.retry_err);
    for (slot = 0; slot < RUN_MAX; slot++) {
        pt_buf_free(&a.runs[slot].id);
        pt_buf_free(&a.runs[slot].head);
        pt_buf_free(&a.runs[slot].out);
    }
    pt_buf_free(&a.diff);
    pt_buf_free(&a.grp);
    pt_buf_free(&a.items_buf);
    pt_buf_free(&a.pending_history);
    pt_buf_free(&a.command);
    {
        uint32_t i;
        for (i = 0; i < a.qcap; i++)
            pt_buf_free(&a.queue[i].text);
        free(a.queue);
    }
    pt_arena_free(&a.ar);
    return a.code;
}
