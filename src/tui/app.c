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

/* The left edge is a column of marks, one per kind of row: the prompt and
 * what the user sent, thinking, notes, tool calls that worked or failed, a
 * question. Prose sits two columns in, beside them. */
typedef struct syms {
    const char *prompt, *think, *note, *ok, *fail, *ask, *sel, *bar, *more, *dot;
} syms;

static const syms uni = {"›", "∴", "·", "✓", "✗", "?", "›", "│", "…", "•"};
static const syms asc = {">", "~", "-", "+", "x", "?", ">", "|", "...", "*"};

#define ACCENT TUI_MAGENTA /* the prompt, the spinner, the selection */
#define MARGIN 2           /* where prose starts, after a mark and a space */
#define NAME_W 6           /* a tool's name column: "bash  ", "write " */

#define BLINK_MS 500 /* the activity mark: on for the first half of each second of the clock */
#define TOOL_FAIL 3  /* rows of a failed tool's output kept in the scrollback */
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
    {"fork", "", "continue in a copy of this session"},
    {"worktree", " [name]", "a new session in a git worktree (also /wt)"},
    {"status", "", "session, model, context and terminal"},
    {"compact", "", "summarize the conversation to free the context"},
    {"ps", "", "background commands"},
    {"stop", "", "stop the background commands"},
    {"usage", "", "tokens used in this session, per model"},
    {"thinking", " [mode]", "how thinking is shown: collapsed, full, hidden"},
    {"login", " [provider] [key]", "save an API key to auth.json"},
    {"mcp", " [login|logout|restart name] [trust|untrust]", "MCP servers: their state, logins, trust"},
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
    uint32_t dat, dn; /* its details, raw lines: a failure's last lines */
    uint8_t cat, bad;
} grow;

/* A call that is running: its row, and the end of its output. */
typedef struct run_call {
    uint32_t task;
    char name[64];
    pt_buf id, head, out;
} run_call;

/* An input that names an MCP prompt (/server:prompt) or resources
 * (@server:uri) waits for them, then goes as the text they make. */
#define XPARTS 9 /* the prompt, and up to 8 resources */

typedef struct queued_input {
    pt_buf text;
    bool command, submitted;
    uint64_t xid;       /* its fetches; 0: not started */
    uint32_t want, got; /* parts asked for, and answered */
    bool failed, prompt; /* prompt: parts[0] is the prompt's text */
    char err[300];
    pt_buf parts[XPARTS];
    pt_buf names; /* "server\turi\n" of each resource part */
} queued_input;

/* A fetch on its way: its answer goes to the queued input of id. */
typedef struct xreq {
    struct xreq *next;
    struct app *a;
    uint64_t id;
    uint32_t k;
} xreq;

typedef struct session_list {
    pt_worker *worker;
    char cwd[4096], ids[50][64];
    char names[50][CLI_NAME_MAX + 80]; /* the name, after "<worktree> · " for one in a worktree */
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

/* The session this TUI shows, and how it got there. */
typedef struct app_sess {
    cli_open_job *opening;
    char fallback[64]; /* /resume restores the old session on failure */
    bool fallback_created;
    bool named;        /* it has a name, or no longer needs one (see cli_session_name) */
    bool created;      /* this process created the session */
    bool discarded;    /* ...and left it empty: closing deleted it */
    bool keep_on_quit; /* /quit keeps even an empty session for resuming */
    char start_dir[4096]; /* where pytte started: /resume lists its sessions and its worktrees' */
    char err[700];
} app_sess;

/* The input history file; lines typed before the session opened wait in
 * pending. */
typedef struct app_hist {
    bool ready;
    pt_buf pending;
    char path[4200];
} app_hist;

/* The draft, saved to the session a second after it changes. */
typedef struct app_draft {
    bool dirty;
    bool drop_restored; /* the draft the session kept is not put back */
    int64_t at;
} app_draft;

/* The display model: which entries are shown, and the streaming block. */
typedef struct app_view {
    uint32_t seen;   /* entries already shown */
    uint32_t epoch;  /* ...of this epoch */
    pt_buf live;     /* the unfinished source line of the streaming block */
    int kind, col;   /* LIVE_*, and the column cleaning reached */
    tui_md md;
    bool streamed;   /* this attempt was streamed: do not print its entry again */
    bool started;    /* the current message has its blank line */
    bool printed;    /* ...and some content */
    int64_t think_at;
} app_view;

/* The run going on: what it does, its clock, and its running calls. */
typedef struct app_run {
    bool active;
    int phase;
    int64_t at, retry_until;
    int64_t wait_at; /* the run waits for an answer since then: its clock stands still */
    int64_t tick;    /* the clock's half second last painted */
    pt_buf retry_err;
    run_call calls[RUN_MAX]; /* in the order they started */
    int ncalls;
    uint32_t nbg; /* background commands still running, in the footer */
} app_run;

/* The open group of finished calls: in the band until something else is shown. */
typedef struct app_group {
    pt_buf buf;
    grow rows[GROUP_MAX];
    int n;
} app_group;

/* A tool call waiting for approval (refreshed from pt_asks). */
typedef struct app_ask {
    pt_ask_view view;
    uint32_t task;
    int sel;
    bool shown; /* the band shows the question */
    char prefix[128]; /* what "allow this session" allows: "git", or "" for the whole tool */
} app_ask;

/* The model in use, and what the catalog says of it. */
typedef struct app_model {
    char name[256]; /* shown */
    char ref[256];  /* o.model points here */
    uint64_t ctx_used;
    uint32_t ctx_window;
    uint8_t efforts; /* the model's levels (PT_EFF_*, resolved) */
    bool reasons;    /* the model reasons: effort means something */
} app_model;

/* Inputs waiting to be sent, and the MCP fetches they wait for. */
typedef struct app_queue {
    queued_input *items;
    uint32_t n, cap;
    xreq *xs; /* fetches on their way */
    uint64_t next_xid;
    bool expanded; /* a fetch ended: the queue may go on */
} app_queue;

/* The menu under the input. */
typedef struct app_menu {
    int kind; /* MENU_* */
    bool closed;
    item items[MAX_ITEMS];
    int n, sel, label_w; /* label_w: the widest label shown, for the column */
    int top;             /* the first row of the menu's window */
    pt_buf buf;          /* the items' labels and descriptions */
    int set_sel;         /* the selection in the settings menu */
    bool of_mentions;    /* it lists @-mentions */
    bool of_sessions;    /* ...sessions (/resume) */
    bool of_skills;      /* ...skills (read when it opens) */
    session_list *sessions;
} app_menu;

/* When to paint, and double presses. */
typedef struct app_pace {
    bool dirty, urgent;
    int64_t last_paint, winch_at, keys_at;
    int interval;
    int64_t esc_at, ctrlc_at;
} app_pace;

/* What the terminal answered. */
typedef struct app_caps {
    bool kitty, cpr, answered;
    int start_col; /* CPR if already available before the first paint */
} app_caps;

typedef struct app {
    cli_opts o;
    cli_config *cfg;
    cli_mcp *mcp;     /* the configured servers; NULL: --no-mcp */
    pt_buf mcp_notes; /* what servers said, shown at the next pass */
    cli_session s;
    bool open; /* s holds a session */
    app_sess sess;
    app_hist hist;
    app_draft draft;
    pt_buf command;
    config_job *saving;
    bool pager_pending;
    tui_term *t;
    tui_keys keys;
    tui_band band;
    tui_frame frame;
    tui_input in;
    int rows, cols;
    app_caps caps;
    bool ascii, color;
    const syms *sy;
    int thinking;
    /* output: styled lines for the scrollback, committed at the next paint */
    pt_buf lines;
    pt_buf tmp, tmp2, wrap, live_rows, diff; /* scratch, reused */
    pt_arena ar;
    app_view view;
    app_run run;
    app_group group;
    app_ask ask;
    app_model model;
    app_queue queue;
    app_menu menu;
    app_pace pace;
    bool quit;
    int code;
} app;

static void group_close(app *a);
static void xreq_drop(app *a, xreq *x);
static bool mcp_known(app *a, pt_str name);

static void set_dirty(app *a)
{
    a->pace.dirty = true;
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

/* A line from pytte itself, after its mark: a dim note, or a red failure. */
static void out_note(app *a, pt_buf *out, int style, pt_str raw)
{
    char mark[16];
    snprintf(mark, sizeof mark, "%s ", style & TUI_RED ? a->sy->fail : a->sy->note);
    out_text(a, out, style, mark, style, raw, MARGIN);
}

static void out_z(app *a, int style, const char *z)
{
    group_close(a);
    out_note(a, &a->lines, style, pt_strz(z));
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
    out_text(a, out, 0, head, TUI_BOLD, b->args, MARGIN);
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

static int tool_of(pt_str name)
{
    int i;
    for (i = 0; i < (int)NTOOLS; i++)
        if (pt_str_eqz(name, tools[i].name))
            return i;
    return -1;
}

/* A call on one row: the tool's name, dim, in a column, then the gist of
 * its arguments; in a command, the programs stand out and the rest is dim. */
static void phrase(app *a, pt_buf *out, pt_str name, pt_str args)
{
    pt_str g = call_gist(a, args);
    bool cmd = pt_str_eqz(name, "bash"), at_cmd = true, in_word = false, prog = false;
    int col = 0, cur = -1, ti = tool_of(name), w;
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

/* How long the run has worked: the time spent waiting for an answer to a
 * question does not count. */
static int64_t run_clock(const app *a, int64_t now)
{
    return (a->run.wait_at ? a->run.wait_at : now) - a->run.at;
}

/* The activity mark blinks with the clock, lighting as its seconds change;
 * it stays lit while the clock stands still. */
static const char *blink(const app *a, int64_t now)
{
    return a->run.wait_at || run_clock(a, now) / BLINK_MS % 2 == 0 ? a->sy->dot : " ";
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
    int ti = tool_of(m->name), left = TOOL_FAIL;
    char seg[160], tail[64];
    grow *g;
    if (a->group.n == GROUP_MAX)
        group_close(a);
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
        timed = starts(last, "[timed out after ");
        if (timed || starts(last, "[exit code ") || starts(last, "[stopped]") || starts(last, "[ended]") ||
            starts(last, "[still running as session ")) {
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
        secs_short(took, sizeof took, ms);
        snprintf(tail, sizeof tail, " · %s", took);
    }
    /* the row: the call, then what came of it */
    g->at = (uint32_t)a->group.buf.n;
    phrase(a, &a->group.buf, m->name, args);
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

static bool runs_oom(const app *a)
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
static void group_render(app *a, pt_buf *out, bool live)
{
    bool running = live && a->run.ncalls;
    int i, first = 0;
    const char *mark = blink(a, pt_sys_now_ms());
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
            break; /* out of memory: paint() says so */
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
static void group_close(app *a)
{
    if (!a->group.n)
        return;
    out_blank(&a->lines);
    group_render(a, &a->lines, false);
    a->group.n = 0;
    pt_buf_reset(&a->group.buf);
}

static void out_thinking(app *a, pt_buf *out, pt_str text, int64_t ms, bool full)
{
    char line[80];
    if (full || a->thinking == TH_FULL) {
        tui_md m;
        memset(&m, 0, sizeof m);
        m.ascii = a->ascii;
        m.margin = MARGIN;
        out_md(a, out, &m, text, TUI_DIM);
    } else if (a->thinking == TH_COLLAPSED) {
        char took[32];
        if (ms < 0) /* from the log, which does not time thinking */
            took[0] = 0;
        else if (ms < 1000)
            snprintf(took, sizeof took, " for <1s");
        else {
            snprintf(took, sizeof took, " for ");
            secs_short(took + 5, sizeof took - 5, ms);
        }
        snprintf(line, sizeof line, "%s thought%s", a->sy->think, took);
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
static void out_entry(app *a, pt_buf *out, const pt_entry_view *v, bool full)
{
    const pt_msg *m = v->msg;
    uint32_t i;
    if (strcmp(v->kind, "epoch") == 0 && v->epoch > 1) {
        char line[120];
        out_blank(out);
        snprintf(line, sizeof line, "compacted: the model continues from a summary%s",
                 full ? ":" : " (ctrl+o shows it)");
        out_note(a, out, TUI_DIM, pt_strz(line));
        if (full)
            out_rail(a, out, a->sy->bar, TUI_DIM, TUI_DIM, v->summary, 0);
        if (full && v->files.n)
            out_rail(a, out, a->sy->bar, TUI_DIM, TUI_DIM, v->files, 0);
    } else if (is_note(v) && !full) { /* its "- session ..." lines */
        pt_str text = m->text;
        size_t at = 0, e;
        out_blank(out);
        while (at < text.n) {
            for (e = at; e < text.n && text.p[e] != '\n'; e++)
                ;
            if (e - at > 2 && text.p[at] == '-' && text.p[at + 1] == ' ')
                out_note(a, out, TUI_DIM, (pt_str){text.p + at + 2, e - at - 2});
            at = e + 1;
        }
    } else if (strcmp(v->kind, "user") == 0) { /* as it looked at the prompt */
        pt_str text = m->text, name;
        char line[120], mark[16];
        out_blank(out);
        snprintf(mark, sizeof mark, "%s ", a->sy->prompt);
        size_t res;
        while (pt_rt_skill_block(&text, &name)) { /* a skill's instructions: just its name */
            snprintf(line, sizeof line, "skill %.*s", (int)(name.n > 64 ? 64 : name.n), name.p);
            out_text(a, out, TUI_DIM, mark, ACCENT, pt_strz(line), MARGIN);
        }
        res = resources_at(text);
        if (res)
            out_text(a, out, TUI_BOLD, mark, ACCENT | TUI_BOLD, (pt_str){text.p, res}, MARGIN);
        while (res < text.n) { /* an @-mention's contents: just which */
            const char *p = text.p + res + 20, *end = text.p + text.n, *q, *u, *close;
            for (q = p; q < end && *q != '"'; q++)
                ;
            u = q + 7 < end ? q + 7 : end; /* after ` uri="` */
            for (close = u; close < end && *close != '"'; close++)
                ;
            snprintf(line, sizeof line, "resource %.*s:%.*s", (int)(q - p > 40 ? 40 : q - p), p,
                     (int)(close - u > 70 ? 70 : close - u), u);
            out_text(a, out, TUI_DIM, mark, ACCENT, pt_strz(line), MARGIN);
            {
                pt_str rest = {text.p + res + 2, text.n - res - 2};
                size_t nx = resources_at(rest);
                res = nx == rest.n ? text.n : res + 2 + nx;
            }
        }
    } else if (strcmp(v->kind, "assistant") == 0) {
        bool any = false;
        out_blank(out);
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
            out_note(a, out, TUI_DIM, any ? PT_S("interrupted") : PT_S("interrupted, nothing arrived"));
    } else if (strcmp(v->kind, "tool_result") == 0) { /* else it joins a group */
        out_rail(a, out, a->sy->bar, TUI_DIM, m->is_error ? TUI_RED | TUI_DIM : TUI_DIM, m->text, 0);
    } else if (strcmp(v->kind, "error") == 0) {
        out_note(a, out, TUI_RED, error_message(a, v->raw));
    } else if (strcmp(v->kind, "note") == 0) {
        out_note(a, out, TUI_DIM, m->text);
    }
}

/* ---- the display model, from the agent's events ---- */

static void begin_msg(app *a)
{
    group_close(a);
    if (!a->view.started)
        out_blank(&a->lines);
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
static void live_end(app *a)
{
    if (a->view.kind != LIVE_NONE && a->view.live.n)
        live_line(a, pt_buf_str(&a->view.live));
    if (a->view.kind == LIVE_THINK && a->thinking == TH_COLLAPSED && !a->group.n) { /* a group goes on over it */
        begin_msg(a);
        out_thinking(a, &a->lines, PT_STR_NULL, pt_sys_now_ms() - a->view.think_at, false);
    }
    pt_buf_reset(&a->view.live);
    a->view.kind = LIVE_NONE;
    a->view.col = 0;
}

static void live_begin(app *a, int kind)
{
    live_end(a);
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
            group_close(a);
        live_begin(a, LIVE_THINK);
        a->view.think_at = pt_sys_now_ms();
        a->run.phase = PH_THINK;
        break;
    case PT_EV_TEXT_START: /* the calls before it are over */
        group_close(a);
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
        live_end(a);
        break;
    case PT_EV_TOOLCALL_END: /* shown with its result, or in the band as it runs */
        live_end(a);
        a->view.printed = true;
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
             (a->thinking == TH_FULL || (a->thinking == TH_COLLAPSED && !a->group.n))))
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
            live_end(a);
            if (v.msg->stop == PT_STOP_ABORTED || v.msg->stop == PT_STOP_ERROR) {
                begin_msg(a);
                out_z(a, TUI_DIM, a->view.printed ? "interrupted" : "interrupted, nothing arrived");
            }
            a->view.streamed = false;
            a->view.started = false;
            continue;
        }
        show_entry(a, &v);
    }
    a->view.seen = n;
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
            a->run.phase = PH_COMPACT;
        else if (pt_str_eqz(ev->outcome, "failed")) {
            group_close(a);
            out_note(a, &a->lines, TUI_RED, ev->error);
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
            phrase(a, &r->head, ev->name, ev->args);
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
        group_close(a);
        if (pt_str_eqz(ev->outcome, "failed")) {
            out_note(a, &a->lines, TUI_RED, ev->error);
        } else {
            /* What the clock said, kept: how long the run took. */
            char took[32];
            int64_t ms = run_clock(a, pt_sys_now_ms());
            if (ms < 1000)
                snprintf(took, sizeof took, "<1s");
            else
                secs_short(took, sizeof took, ms);
            snprintf(line, sizeof line, "worked for %s", took);
            out_blank(&a->lines);
            out_z(a, TUI_DIM, line);
        }
        a->run.at = pt_sys_now_ms(); /* a queued input runs next */
        a->run.wait_at = 0;
        break;
    case PT_E_FATAL:
        group_close(a);
        out_note(a, &a->lines, TUI_RED, ev->error);
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
    if (a->menu.n >= MAX_ITEMS)
        return;
    it = &a->menu.items[a->menu.n++];
    it->at = a->menu.buf.n;
    it->n = ln;
    pt_buf_put(&a->menu.buf, label, ln);
    it->dat = a->menu.buf.n;
    it->dn = desc ? strlen(desc) : 0;
    if (desc)
        pt_buf_putz(&a->menu.buf, desc);
}

static pt_str item_label(const app *a, int i)
{
    return (pt_str){a->menu.buf.p + a->menu.items[i].at, a->menu.items[i].n};
}

/* The level requests use: the chosen one, moved to one the model takes. */
static pt_reasoning effort_used(const app *a, pt_reasoning want)
{
    pt_ai_target t;
    memset(&t, 0, sizeof t);
    t.model.reasoning = a->model.reasons;
    t.efforts = a->model.efforts;
    return pt_ai_effort(&t, want);
}

/* "max", or "max→high" when the model does not take max. */
static void effort_label(const app *a, char *out, size_t cap)
{
    pt_reasoning used = effort_used(a, a->o.reasoning);
    if (used == a->o.reasoning)
        snprintf(out, cap, "%s", pt_reasoning_name(used));
    else
        snprintf(out, cap, "%s%s%s", pt_reasoning_name(a->o.reasoning), a->ascii ? "->" : "→",
                 pt_reasoning_name(used));
}

static void add_models(app *a, pt_str q)
{
    const pt_ai_catalog *cat = cli_catalog();
    char ref[256];
    uint32_t i;
    for (i = 0; i < cat->nmodels; i++) {
        int n = snprintf(ref, sizeof ref, "%s/%s", cat->providers[cat->models[i].provider].id,
                         cat->models[i].id);
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
    a->menu.of_skills = true;
}

/* MCP prompts whose command ("server:name") starts with q. */
static void add_prompts(app *a, pt_str q)
{
    uint32_t i, k, n = 0;
    const pt_rt_mcp_prompt *p = a->open ? pt_rt_mcp_prompts(a->s.rt, &n) : NULL;
    char label[160];
    for (i = 0; i < n; i++) {
        int ln = snprintf(label, sizeof label, "%.*s:%.*s", (int)p[i].server.n, p[i].server.p, (int)p[i].name.n,
                          p[i].name.p);
        if (ln <= 0 || (size_t)ln >= sizeof label || !pt_str_starts(pt_strz(label), q))
            continue;
        pt_buf_reset(&a->tmp);
        pt_buf_puts(&a->tmp, p[i].description.n ? p[i].description : PT_S("MCP prompt"));
        for (k = 0; k < p[i].nargs; k++) {
            pt_buf_putz(&a->tmp, k ? ", " : " · ");
            pt_buf_puts(&a->tmp, p[i].args[k].name);
            if (!p[i].args[k].required)
                pt_buf_putc(&a->tmp, '?');
        }
        pt_buf_putc(&a->tmp, 0);
        add_item(a, label, (size_t)ln, a->tmp.p);
    }
}

/* The prompt a command names ("/server:name ..." or "server:name"). */
static const pt_rt_mcp_prompt *find_prompt(app *a, pt_str cmd)
{
    uint32_t i, n = 0;
    const pt_rt_mcp_prompt *p = a->open ? pt_rt_mcp_prompts(a->s.rt, &n) : NULL;
    long colon;
    if (cmd.n && cmd.p[0] == '/')
        cmd = (pt_str){cmd.p + 1, cmd.n - 1};
    for (i = 0; i < cmd.n && cmd.p[i] != ' ' && cmd.p[i] != '\n'; i++)
        ;
    cmd.n = i;
    if ((colon = pt_str_find(cmd, ':')) <= 0)
        return NULL;
    for (i = 0; i < n; i++)
        if (pt_str_eq(p[i].server, (pt_str){cmd.p, (size_t)colon}) &&
            pt_str_eq(p[i].name, (pt_str){cmd.p + colon + 1, cmd.n - (size_t)colon - 1}))
            return &p[i];
    return NULL;
}

/* Resources whose mention ("@server:uri") starts with q, or whose name
 * has q's text. */
static void add_mentions(app *a, pt_str q)
{
    uint32_t i, n = 0;
    const pt_rt_mcp_resource *r = a->open ? pt_rt_mcp_resources(a->s.rt, &n) : NULL;
    pt_str rest = {q.p + 1, q.n - 1};
    for (i = 0; i < n && a->menu.n < MAX_ITEMS; i++) {
        pt_buf_reset(&a->tmp2);
        pt_buf_putc(&a->tmp2, '@');
        pt_buf_puts(&a->tmp2, r[i].server);
        pt_buf_putc(&a->tmp2, ':');
        pt_buf_puts(&a->tmp2, r[i].uri);
        if (a->tmp2.oom || a->tmp2.n > 300 || memchr(a->tmp2.p, ' ', a->tmp2.n))
            continue;
        if (!pt_str_starts(pt_buf_str(&a->tmp2), q) && !(rest.n && pt_str_icontains(r[i].name, rest)))
            continue;
        pt_buf_reset(&a->tmp);
        pt_buf_puts(&a->tmp, r[i].name);
        if (r[i].description.n) {
            pt_buf_putz(&a->tmp, r[i].name.n ? " · " : "");
            pt_buf_puts(&a->tmp, r[i].description);
        }
        pt_buf_putc(&a->tmp, 0);
        add_item(a, a->tmp2.p, a->tmp2.n, a->tmp.p);
    }
}

static const char *const mcp_subs[] = {"login", "logout", "restart", "trust", "untrust"};

/* "/login <provider> <key>": where the key starts, or 0. The key is shown
 * masked and never reaches the input history or a saved draft. */
static size_t secret_at(pt_str d)
{
    long sp;
    if (!pt_str_starts(d, PT_S("/login ")))
        return 0;
    sp = pt_str_find((pt_str){d.p + 7, d.n - 7}, ' ');
    return sp < 0 ? 0 : 7 + (size_t)sp + 1;
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
    char root[4096], dir[4200], cwd[4096], name[CLI_NAME_MAX + 1];
    uint32_t i;
    list->n = cli_sessions_list(list->cwd, true, list->ids, 50);
    cli_sessions_dir(root, sizeof root);
    for (i = 0; i < list->n; i++) {
        const char *wt = "";
        snprintf(dir, sizeof dir, "%s/%s", root, list->ids[i]);
        cli_session_name(dir, name, sizeof name);
        if (cli_session_cwd(list->ids[i], cwd, sizeof cwd) && strcmp(cwd, list->cwd) != 0)
            wt = strrchr(cwd, '/') + 1; /* under list->cwd: there is a slash */
        snprintf(list->names[i], sizeof list->names[i], "%.64s%s%s", wt, wt[0] && name[0] ? " · " : "", name);
    }
}

static session_list *get_sessions(app *a, bool refresh)
{
    session_list *list = a->menu.sessions;
    if (!list) {
        list = (session_list *)calloc(1, sizeof *list);
        if (!list)
            return NULL;
        list->worker = pt_worker_new();
        if (!list->worker) {
            free(list);
            return NULL;
        }
        snprintf(list->cwd, sizeof list->cwd, "%s", a->sess.start_dir);
        a->menu.sessions = list;
    }
    if (refresh && !list->pending)
        list->ready = false;
    if (!list->ready && !list->pending)
        list->pending = pt_worker_start(list->worker, list_sessions, list);
    return list->ready ? list : NULL;
}

static void menu_update(app *a)
{
    bool was_sessions = a->menu.of_sessions, was_skills = a->menu.of_skills;
    pt_str d = pt_buf_str(&a->in.text), cmd, arg, word = last_word(d);
    long sp;
    int old = a->menu.n;
    a->menu.n = 0;
    pt_buf_reset(&a->menu.buf);
    a->menu.of_sessions = a->menu.of_skills = a->menu.of_mentions = false;
    if (a->menu.kind == MENU_SETTINGS)
        return;
    a->menu.kind = MENU_NONE;
    if (!a->menu.closed && word.n && word.p[0] == '@') { /* @server:uri anywhere */
        add_mentions(a, word);
        a->menu.kind = a->menu.n ? MENU_ARG : MENU_NONE;
        a->menu.of_mentions = a->menu.n > 0;
        if (a->menu.n != old || a->menu.sel >= a->menu.n)
            a->menu.sel = a->menu.top = 0;
        return;
    }
    if (!a->menu.closed && word.n && word.p[0] == '$') { /* $skill anywhere */
        add_skills(a, "$", word, !was_skills);
        a->menu.kind = a->menu.n ? MENU_ARG : MENU_NONE;
        if (a->menu.n != old || a->menu.sel >= a->menu.n)
            a->menu.sel = a->menu.top = 0;
        return;
    }
    if (a->menu.closed || !d.n || d.p[0] != '/' || pt_str_find(d, '\n') >= 0)
        return;
    d.p++;
    d.n--;
    sp = pt_str_find(d, ' ');
    if (sp < 0) {
        size_t i;
        a->menu.kind = MENU_CMD;
        for (i = 0; i < NCMDS; i++)
            if (pt_str_starts(pt_strz(cmds[i].name), d))
                add_item(a, cmds[i].name, strlen(cmds[i].name), cmds[i].desc);
        add_skills(a, "skill:", d, !was_skills);
        add_prompts(a, d);
    } else {
        cmd = (pt_str){d.p, (size_t)sp};
        arg = (pt_str){d.p + sp + 1, d.n - (size_t)sp - 1};
        a->menu.kind = MENU_ARG;
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
            size_t i;
            for (i = 0; i < sizeof levels / sizeof levels[0]; i++) /* those the model takes */
                if ((!a->model.reasons || !a->model.efforts || a->model.efforts & 1u << i) && pt_str_starts(pt_strz(levels[i]), arg))
                    add_item(a, levels[i], strlen(levels[i]), NULL);
        } else if (pt_str_eqz(cmd, "thinking")) {
            add_list(a, th_names, 3, arg);
        } else if (pt_str_eqz(cmd, "login") && pt_str_find(arg, ' ') < 0) {
            const pt_ai_catalog *cat = cli_catalog();
            uint32_t i;
            for (i = 0; i < cat->nproviders; i++)
                if (cat->providers[i].auth != PT_AUTH_NONE && pt_str_starts(pt_strz(cat->providers[i].id), arg))
                    add_item(a, cat->providers[i].id, strlen(cat->providers[i].id), cat->providers[i].name);
        } else if (pt_str_eqz(cmd, "mcp")) {
            long sp2 = pt_str_find(arg, ' ');
            if (sp2 < 0) {
                add_list(a, mcp_subs, sizeof mcp_subs / sizeof mcp_subs[0], arg);
            } else if (a->open && pt_str_find((pt_str){arg.p + sp2 + 1, arg.n - (size_t)sp2 - 1}, ' ') < 0 &&
                       !pt_str_starts(arg, PT_S("trust")) && !pt_str_starts(arg, PT_S("untrust"))) {
                pt_rt_mcp_info in[CLI_MCP_MAX];
                pt_str name = {arg.p + sp2 + 1, arg.n - (size_t)sp2 - 1};
                uint32_t i, n = pt_rt_mcp_servers(a->s.rt, in, CLI_MCP_MAX);
                for (i = 0; i < n && i < CLI_MCP_MAX; i++)
                    if (pt_str_starts(pt_strz(in[i].name), name))
                        add_item(a, in[i].name, strlen(in[i].name), in[i].target);
            }
        } else if (pt_str_eqz(cmd, "resume")) {
            session_list *list = get_sessions(a, !was_sessions);
            uint32_t i;
            a->menu.of_sessions = true;
            for (i = 0; list && i < list->n; i++)
                if ((pt_str_starts(pt_strz(list->ids[i]), arg) || pt_str_icontains(pt_strz(list->names[i]), arg)) &&
                    (!a->open || strcmp(list->ids[i], a->s.id) != 0))
                    add_item(a, list->ids[i], strlen(list->ids[i]), list->names[i]);
        }
        if (!a->menu.n)
            a->menu.kind = MENU_NONE;
    }
    if (a->menu.n != old || a->menu.sel >= a->menu.n)
        a->menu.sel = a->menu.top = 0;
}

static void row(app *a, pt_str styled)
{
    tui_frame_row(&a->frame, styled, a->cols);
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
        pt_buf_putz(&a->tmp, "1-3 or ↑↓ enter · esc no · ctrl-c interrupt");
    } else if (a->menu.kind == MENU_SETTINGS) {
        pt_buf_putz(&a->tmp, "↑↓ select · ←→ change · enter edit · esc close");
    } else if (a->menu.kind != MENU_NONE) {
        pt_buf_putz(&a->tmp, "↑↓ select · tab complete · enter run · esc close");
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
        effort_label(a, eff, sizeof eff);
        n = snprintf(line, sizeof line, " · %s", eff);
        if (a->model.ctx_window) { /* the context used, of the model's window */
            char used[16], all[16];
            tokens_short(used, sizeof used, a->model.ctx_used);
            tokens_short(all, sizeof all, a->model.ctx_window);
            n += snprintf(line + n, sizeof line - (size_t)n, " · %s/%s", used, all);
        }
        if (a->run.nbg)
            snprintf(line + n, sizeof line - (size_t)n, " · %u in background (/ps)", a->run.nbg);
        pt_buf_putz(&a->tmp, line);
    }
    row(a, pt_buf_str(&a->tmp));
}

/* Above the input while a run is active: the blinking dot, what the run is
 * doing, and its clock ("• thinking · 12s"). Only the clock moves. */
static void activity_row(app *a, int64_t now)
{
    char what[120], el[32];
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
    secs_short(el, sizeof el, run_clock(a, now));
    pt_buf_reset(&a->tmp);
    tui_style(&a->tmp, ACCENT | TUI_BOLD);
    pt_buf_putz(&a->tmp, blink(a, now));
    tui_style(&a->tmp, 0);
    pt_buf_putc(&a->tmp, ' ');
    pt_buf_putz(&a->tmp, what);
    tui_style(&a->tmp, TUI_DIM);
    pt_buf_putz(&a->tmp, " · ");
    pt_buf_putz(&a->tmp, el);
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
    if (a->menu.kind == MENU_SETTINGS) {
        int k = settings[i].k;
        char v[600];
        snprintf(v, sizeof v, "%-14s", settings[i].name);
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
        if (a->menu.kind == MENU_CMD)
            pt_buf_putc(&a->tmp, '/');
        tui_clean(&a->tmp, item_label(a, i), &col);
        if (a->menu.items[i].dn) {
            int pad = a->menu.label_w - tui_width(item_label(a, i), true);
            tui_style(&a->tmp, TUI_DIM);
            for (pad = pad > 0 ? pad + 2 : 2; pad > 0; pad--)
                pt_buf_putc(&a->tmp, ' ');
            tui_clean(&a->tmp, (pt_str){a->menu.buf.p + a->menu.items[i].dat, a->menu.items[i].dn}, &col);
        }
    }
    row(a, pt_buf_str(&a->tmp));
}

/* The first tool call waiting for approval, if any. */
static bool asking(app *a)
{
    if (!a->open || a->sess.opening || !pt_asks(a->s.a, &a->ask.view, 1))
        return false;
    if (a->ask.view.task != a->ask.task) { /* a new question */
        pt_str g = call_gist(a, a->ask.view.args);
        size_t n = 0;
        a->ask.task = a->ask.view.task;
        a->ask.sel = 0;
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

static void ask_row(app *a, int i)
{
    char line[300];
    pick(a, i == a->ask.sel);
    if (i == 0)
        snprintf(line, sizeof line, "1  yes");
    else if (i == 1 && a->ask.prefix[0])
        snprintf(line, sizeof line, "2  yes, and allow %.*s \"%s\" this session", (int)a->ask.view.name.n,
                 a->ask.view.name.p, a->ask.prefix);
    else if (i == 1)
        snprintf(line, sizeof line, "2  yes, and allow %.*s this session", (int)a->ask.view.name.n, a->ask.view.name.p);
    else
        snprintf(line, sizeof line, "3  no (esc)");
    pt_buf_putz(&a->tmp, line);
    row(a, pt_buf_str(&a->tmp));
}

/* The question: a blank row while there is room, the call on one row
 * ("? bash  make test"), then the choices. */
static void ask_rows(app *a, int n)
{
    int col = 0, i;
    size_t k;
    if (n > 4) {
        blank_row(a);
        n--;
    }
    pt_buf_reset(&a->tmp2);
    tui_style(&a->tmp2, ACCENT | TUI_BOLD);
    pt_buf_putz(&a->tmp2, a->sy->ask);
    tui_style(&a->tmp2, 0);
    pt_buf_putc(&a->tmp2, ' ');
    if (tool_of(a->ask.view.name) < 0 && a->ask.view.prompt.n) { /* another tool: its name and what it asks */
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
        phrase(a, &a->tmp2, a->ask.view.name, a->ask.view.args);
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
    int avail = a->rows - 2, w = width(a), show, live_n = 0, nmenu, nq, st, gap, act, i, first, top, nask;
    size_t secret = secret_at(pt_buf_str(&a->in.text));
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
        group_render(a, live_rows, true);
    }
    if (a->view.kind != LIVE_NONE && a->view.live.n) {
        tui_md copy = a->view.md;
        if (!a->view.started) /* where the message's blank line will be */
            pt_buf_putc(live_rows, '\n');
        tui_md_line(&copy, pt_buf_str(&a->view.live), a->cols, a->view.kind == LIVE_THINK ? TUI_DIM : 0, true,
                    live_rows);
    }
    for (i = 0; (size_t)i < live_rows->n; i++)
        live_n += live_rows->p[i] == '\n';
    a->ask.shown = asking(a);
    if (a->run.active && a->ask.shown && !a->run.wait_at) /* the clock stands still while the user decides */
        a->run.wait_at = now;
    else if (a->run.wait_at && !a->ask.shown) {
        a->run.at += now - a->run.wait_at;
        a->run.wait_at = 0;
    }
    nask = a->ask.shown ? 5 : 0;
    act = a->run.active ? 1 : 0;
    nq = a->run.active || (a->queue.n && a->queue.items[0].xid && a->queue.items[0].got < a->queue.items[0].want)
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
    /* the input box after a blank row and the activity, the prompt's mark
     * on its first row */
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
            int lw = tui_width(item_label(a, i), true);
            a->menu.label_w = lw > a->menu.label_w ? (lw > 40 ? 40 : lw) : a->menu.label_w;
        }
        for (i = start; i < start + nmenu && i < total; i++)
            menu_row(a, i);
        a->menu.sel = save;
    }
    if (st)
        status_row(a, now, total, sel);
}

static void flush_out(app *a)
{
    int ms = tui_term_write(a->t, pt_buf_str(&a->band.out));
    pt_buf_reset(&a->band.out);
    /* writes that block (a slow link) slow the stream down */
    if (ms > 50 && a->pace.interval < 500)
        a->pace.interval *= 2;
    else if (ms < 10 && a->pace.interval > 33)
        a->pace.interval /= 2;
}

static void paint(app *a)
{
    int64_t now = pt_sys_now_ms();
    build_frame(a, now);
    if (a->in.text.oom || a->in.kill.oom || a->in.saved.oom || a->frame.rows.oom || a->lines.oom ||
        a->view.live.oom || a->tmp.oom || a->tmp2.oom || a->wrap.oom || a->menu.buf.oom || a->group.buf.oom ||
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
    a->pace.dirty = a->pace.urgent = false;
    a->pace.last_paint = now;
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
    a->view.md.ascii = a->ascii;
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
            out_note(a, &a->lines, TUI_RED, pt_strz(job->err));
        } else {
            for (i = 0; i < NSETTINGS && settings[i].k != job->key; i++)
                ;
            snprintf(msg, sizeof msg, "saved %s: %s", i < NSETTINGS ? settings[i].name : "setting",
                     job->value[0] ? job->value : "(none)");
            out_z(a, TUI_DIM, msg);
        }
        a->pace.urgent = true;
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
        !pt_ai_resolve(cli_catalog(), pt_strz(v), &t, &a->ar, &why)) {
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
    if (k == CFG_THINKING) {
        apply_thinking(a);
        set_config(a);
    }
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
    a->model.name[0] = 0;
    if (a->o.model)
        snprintf(a->model.name, sizeof a->model.name, "%s", a->o.model);
    while (n-- > 0 && pt_entry_at(a->s.a, n, &v))
        if (strcmp(v.kind, "assistant") == 0 && v.msg->model.n) {
            if (!a->model.name[0])
                snprintf(a->model.name, sizeof a->model.name, "%.*s/%.*s", (int)v.msg->provider.n, v.msg->provider.p,
                         (int)v.msg->model.n, v.msg->model.p);
            break;
        }
    a->model.ctx_used = a->open ? pt_context_tokens(a->s.a) : 0;
    a->model.ctx_window = 0;
    a->model.efforts = 0;
    a->model.reasons = false;
    pt_arena_reset(&a->ar);
    if (a->model.name[0] && pt_ai_resolve(cli_catalog(), pt_strz(a->model.name), &t, &a->ar, &why)) {
        a->model.ctx_window = t.model.context_window;
        a->model.efforts = t.efforts;
        a->model.reasons = t.model.reasoning;
    }
}

static void set_config(app *a)
{
    pt_config c;
    memset(&c, 0, sizeof c);
    c.model = a->o.model ? pt_strz(a->o.model) : PT_STR_NULL;
    c.compact_model = a->o.compact_model ? pt_strz(a->o.compact_model) : PT_STR_NULL;
    c.reasoning = a->o.reasoning;
    c.hide_thinking = a->thinking != TH_FULL; /* collapsed shows only how long */
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
    if (a->in.text.n && !secret_at(pt_buf_str(&a->in.text)))
        pt_sys_write_file(p, a->in.text.p, a->in.text.n);
    else
        unlink(p);
    a->draft.dirty = false;
    a->draft.at = pt_sys_now_ms();
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
    a->view.seen = n;
    if (n && pt_entry_at(a->s.a, 0, &v)) /* else the next commit looks like a new epoch */
        a->view.epoch = v.epoch;
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

static bool session_empty(app *a)
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

static bool open_session(app *a, const char *id)
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
static void finish_session(app *a);

static void close_session(app *a)
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
static void switch_session(app *a, const char *id)
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
    a->pace.urgent = true;
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

/* Background commands still running. */
static uint32_t bg_running(app *a)
{
    pt_rt_background v[32];
    uint32_t n = a->open ? pt_rt_backgrounds(a->s.rt, v, 32) : 0, i, k = 0;
    for (i = 0; i < n && i < 32; i++)
        k += !v[i].ended;
    return k;
}

/* "session 8 · running 1m 4s · make dev", newest first. */
static void cmd_ps(app *a)
{
    pt_rt_background v[32];
    uint32_t n = a->open ? pt_rt_backgrounds(a->s.rt, v, 32) : 0, i;
    char line[300], took[32];
    if (!n) {
        out_z(a, TUI_DIM, "no background commands");
        return;
    }
    for (i = 0; i < n && i < 32; i++) {
        secs_short(took, sizeof took, pt_sys_now_ms() - v[i].started);
        if (v[i].ended)
            snprintf(line, sizeof line, "session %u · ended (exit %d), not read yet · %.200s", v[i].session,
                     v[i].status, v[i].command);
        else
            snprintf(line, sizeof line, "session %u · running %s · %.200s", v[i].session, took, v[i].command);
        out_note(a, &a->lines, TUI_DIM, pt_strz(line));
    }
}

static void cmd_status(app *a)
{
    char line[5000], name[CLI_NAME_MAX + 1], eff[40];
    pt_str cwd = a->open ? pt_session_cwd(a->s.a) : PT_STR_NULL;
    snprintf(line, sizeof line, "session %s, owned by pid %ld\nstore %s", a->s.id, (long)getpid(), a->s.dir);
    out_z(a, TUI_DIM, line);
    if (cli_session_name(a->s.dir, name, sizeof name)) {
        snprintf(line, sizeof line, "name %s", name);
        out_z(a, TUI_DIM, line);
    }
    snprintf(line, sizeof line, "directory %.*s", (int)(cwd.n < 4096 ? cwd.n : 4096), cwd.p ? cwd.p : "");
    out_z(a, TUI_DIM, line);
    effort_label(a, eff, sizeof eff);
    snprintf(line, sizeof line, "model %s · effort %s", a->model.name[0] ? a->model.name : "(none)", eff);
    out_z(a, TUI_DIM, line);
    if (a->model.ctx_window)
        snprintf(line, sizeof line, "context %llu of %u tokens (%u%%)", (unsigned long long)a->model.ctx_used, a->model.ctx_window,
                 (unsigned)(a->model.ctx_used * 100 / a->model.ctx_window));
    else
        snprintf(line, sizeof line, "context %llu tokens", (unsigned long long)a->model.ctx_used);
    out_z(a, TUI_DIM, line);
    snprintf(line, sizeof line, "terminal %dx%d · %s · %s · kitty keys %s · %s · %s symbols", a->cols, a->rows,
             getenv("TERM") ? getenv("TERM") : "(no TERM)", a->band.reflow ? "reflows" : "does not reflow",
             a->caps.kitty ? "on" : "off", a->caps.answered ? "answers queries" : "no query replies",
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
        if (pt_ai_resolve(cli_catalog(), c->key, &t, &a->ar, &why) && t.known && t.model.cost.input) {
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
    if (a->mcp && a->mcp->nall)
        out_z(a, TUI_DIM, "/server:prompt [args] sends an MCP server's prompt; @server:uri adds a resource to a message");
    out_z(a, TUI_DIM,
          "enter send · shift+enter, alt+enter or ctrl-j newline · esc esc interrupt · ctrl-c clear, "
          "interrupt or quit · ctrl-d quit · ctrl-t thinking · ctrl-o details · ctrl-z suspend · ctrl-l redraw");
}

/* "/login": where each provider's key comes from. "/login <provider>":
 * ready for the key. "/login <provider> <key>": save it. */
static void cmd_login(app *a, pt_str arg)
{
    const pt_ai_catalog *cat = cli_catalog();
    const pt_provider *p = NULL;
    pt_str name = arg, key = PT_STR_NULL;
    long sp = pt_str_find(arg, ' ');
    char msg[4400], err[600], path[4200];
    uint32_t i;
    if (sp >= 0) {
        name.n = (size_t)sp;
        key = pt_str_trim((pt_str){arg.p + sp + 1, arg.n - (size_t)sp - 1});
    }
    for (i = 0; i < cat->nproviders; i++)
        if (cat->providers[i].auth != PT_AUTH_NONE && pt_str_eqz(name, cat->providers[i].id))
            p = &cat->providers[i];
    if (!arg.n) {
        for (i = 0; i < cat->nproviders; i++) {
            const pt_provider *q = &cat->providers[i];
            const char *env = q->key_env[0] ? getenv(q->key_env[0]) : NULL;
            if (q->auth == PT_AUTH_NONE)
                continue;
            snprintf(msg, sizeof msg, "%s: %s%s", q->id,
                     env && *env ? "from " : cli_auth_key(q->id) ? "saved in auth.json" : "no key",
                     env && *env ? q->key_env[0] : "");
            out_z(a, TUI_DIM, msg);
        }
        out_z(a, TUI_DIM, "/login <provider> saves a key; a key in the environment comes first");
        return;
    }
    if (!p) {
        snprintf(msg, sizeof msg, "not a provider: %.*s", (int)(name.n > 100 ? 100 : name.n), name.p);
        out_z(a, TUI_RED, msg);
        return;
    }
    if (!key.n) {
        snprintf(msg, sizeof msg, "/login %s ", p->id);
        tui_input_set(&a->in, pt_strz(msg));
        snprintf(msg, sizeof msg, "paste the %s API key and press enter", p->name);
        out_z(a, TUI_DIM, msg);
        return;
    }
    if (!cli_auth_valid(key)) {
        out_z(a, TUI_RED, "not an API key: it has spaces or control characters");
        return;
    }
    if (!cli_auth_save(p->id, key, err, sizeof err)) {
        out_z(a, TUI_RED, err);
        return;
    }
    cli_auth_path(path, sizeof path);
    snprintf(msg, sizeof msg, "saved the %s key to %s", p->name, path);
    out_z(a, TUI_DIM, msg);
    if (err[0])
        out_z(a, TUI_RED, err);
    if (p->key_env[0] && getenv(p->key_env[0]) && *getenv(p->key_env[0])) {
        snprintf(msg, sizeof msg, "%s is set in the environment and is used instead", p->key_env[0]);
        out_z(a, TUI_DIM, msg);
    }
}

/* ---- /mcp ---- */

/* From a server, inside a step: shown at the loop's next pass. */
static void mcp_note(void *ud, const char *server, const char *msg)
{
    app *a = (app *)ud;
    pt_buf_putz(&a->mcp_notes, server ? server : "");
    pt_buf_putc(&a->mcp_notes, '\t');
    pt_buf_putz(&a->mcp_notes, msg);
    pt_buf_putc(&a->mcp_notes, '\n');
    set_dirty(a);
}

static void show_notes(app *a)
{
    pt_str all = pt_buf_str(&a->mcp_notes);
    size_t at = 0;
    if (!all.n)
        return;
    while (at < all.n) {
        size_t e = at, tab;
        char line[1200];
        pt_str msg;
        while (e < all.n && all.p[e] != '\n')
            e++;
        for (tab = at; tab < e && all.p[tab] != '\t'; tab++)
            ;
        msg = (pt_str){all.p + tab + 1, e > tab ? e - tab - 1 : 0};
        snprintf(line, sizeof line, "mcp %.*s: %.*s", (int)(tab - at), all.p + at, (int)msg.n, msg.p);
        out_z(a,
              starts(msg, "failed") || starts(msg, "stopped") || starts(msg, "login failed") ||
                      pt_str_contains(msg, PT_S("log in with"))
                  ? TUI_RED
                  : TUI_DIM,
              line);
        at = e + 1;
    }
    pt_buf_reset(&a->mcp_notes);
    a->pace.urgent = true;
}

static const char *mcp_state_name(pt_mcp_state st)
{
    switch (st) {
    case PT_MCP_STARTING:
        return "starting";
    case PT_MCP_READY:
        return "ready";
    case PT_MCP_FAILED:
        return "failed";
    case PT_MCP_AUTH:
        return "needs login";
    default:
        return "stopped";
    }
}

static void cmd_mcp(app *a, pt_str arg)
{
    pt_rt_mcp_info in[CLI_MCP_MAX];
    pt_str sub = arg, name = PT_STR_NULL;
    long sp = pt_str_find(arg, ' ');
    char msg[1200], err[700];
    uint32_t i, n;
    if (sp >= 0) {
        sub.n = (size_t)sp;
        name = pt_str_trim((pt_str){arg.p + sp + 1, arg.n - (size_t)sp - 1});
    }
    if (!a->mcp) {
        out_z(a, TUI_DIM, "MCP servers are off (--no-mcp)");
        return;
    }
    n = a->open ? pt_rt_mcp_servers(a->s.rt, in, CLI_MCP_MAX) : 0;
    if (n > CLI_MCP_MAX)
        n = CLI_MCP_MAX;
    if (!sub.n) {
        if (!a->mcp->nall) {
            out_z(a, TUI_DIM, "no MCP servers: add them to mcpServers in ~/.config/pytte/config.json, or to "
                              ".mcp.json in a project");
            return;
        }
        for (i = 0; i < n; i++) {
            int k = snprintf(msg, sizeof msg, "%s · %s · %s", in[i].name, mcp_state_name(in[i].state), in[i].target);
            if (in[i].state == PT_MCP_READY)
                snprintf(msg + k, sizeof msg - (size_t)k, " · %u tools, %u prompts, %u resources", in[i].ntools,
                         in[i].nprompts, in[i].nresources);
            else if (in[i].error[0])
                snprintf(msg + k, sizeof msg - (size_t)k, " · %s", in[i].error);
            out_z(a, in[i].state == PT_MCP_FAILED || in[i].state == PT_MCP_AUTH ? TUI_RED : TUI_DIM, msg);
        }
        for (i = 0; i < a->mcp->nall; i++)
            if (!a->mcp->info[i].runs) {
                snprintf(msg, sizeof msg, "%s · not trusted (%s): /mcp trust runs this project's servers",
                         a->mcp->all[i].name, a->mcp->info[i].source);
                out_z(a, TUI_DIM, msg);
            }
        return;
    }
    if (pt_str_eqz(sub, "trust") || pt_str_eqz(sub, "untrust")) {
        bool on = sub.p[0] == 't';
        pt_rt_mcp_server add[CLI_MCP_MAX];
        uint32_t k = 0;
        if (!cli_mcp_trust(a->mcp, on, err, sizeof err)) {
            out_z(a, TUI_RED, err);
            return;
        }
        if (!on) {
            out_z(a, TUI_DIM, "no longer trusting this project's MCP servers: new sessions start without them");
            cli_mcp_free(a->mcp);
            cli_mcp_load(a->mcp, a->mcp->project);
            a->o.nmcp = a->mcp->n;
            return;
        }
        {
            char dir[4096];
            snprintf(dir, sizeof dir, "%s", a->mcp->project);
            cli_mcp_free(a->mcp);
            cli_mcp_load(a->mcp, dir);
        }
        a->o.mcp = a->mcp->run;
        a->o.nmcp = a->mcp->n;
        /* those not running yet start now */
        for (i = 0; i < a->mcp->n; i++) {
            uint32_t j;
            for (j = 0; j < n && strcmp(in[j].name, a->mcp->run[i].name) != 0; j++)
                ;
            if (j == n)
                add[k++] = a->mcp->run[i];
        }
        if (k && a->open && !pt_rt_mcp_setup(a->s.rt, &a->o.mcp_host, add, k)) {
            out_z(a, TUI_RED, "out of memory starting them");
            return;
        }
        snprintf(msg, sizeof msg, "trusted this project's MCP servers%s", k ? ": starting them; their tools come with the next session (/new) or after /compact" : "");
        out_z(a, TUI_DIM, msg);
        return;
    }
    if (!pt_str_eqz(sub, "login") && !pt_str_eqz(sub, "logout") && !pt_str_eqz(sub, "restart")) {
        out_z(a, TUI_RED, "/mcp [login|logout|restart NAME] [trust|untrust]");
        return;
    }
    if (!name.n) {
        out_z(a, TUI_RED, "which server?");
        return;
    }
    for (i = 0; i < n && !pt_str_eqz(name, in[i].name); i++)
        ;
    if (i == n) {
        snprintf(msg, sizeof msg, "no MCP server %.*s%s", (int)(name.n > 64 ? 64 : name.n), name.p,
                 mcp_known(a, name) ? " runs: /mcp trust runs this project's servers" : "");
        out_z(a, TUI_RED, msg);
        return;
    }
    if (pt_str_eqz(sub, "restart")) {
        pt_rt_mcp_restart(a->s.rt, name);
        snprintf(msg, sizeof msg, "restarting %s", in[i].name);
    } else if (strcmp(in[i].transport, "http") != 0) {
        snprintf(msg, sizeof msg, "%s is not a remote server: only those log in", in[i].name);
        out_z(a, TUI_RED, msg);
        return;
    } else if (pt_str_eqz(sub, "login")) {
        pt_rt_mcp_login(a->s.rt, name); /* it says what goes on */
        return;
    } else {
        pt_rt_mcp_logout(a->s.rt, name);
        snprintf(msg, sizeof msg, "logged out of %s", in[i].name);
    }
    out_z(a, TUI_DIM, msg);
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
            if (!pt_ai_resolve(cli_catalog(), arg, &t, &a->ar, &why)) {
                snprintf(msg, sizeof msg, "%s: %s", v, why ? why : "unknown");
                out_z(a, TUI_RED, msg);
                return;
            }
            snprintf(a->model.ref, sizeof a->model.ref, "%.255s", v);
            a->o.model = a->model.ref;
            set_config(a);
        }
        snprintf(msg, sizeof msg, "%smodel: %s", arg.n ? "switched " : "",
                 a->model.name[0] ? a->model.name : "(the session's last)");
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
        {
            char eff[40];
            effort_label(a, eff, sizeof eff);
            snprintf(msg, sizeof msg, "%seffort: %s%s", arg.n ? "switched " : "", eff,
                     effort_used(a, a->o.reasoning) != a->o.reasoning ? " (the nearest this model takes)" : "");
        }
        out_z(a, TUI_DIM, msg);
    } else if (pt_str_eqz(cmd, "new")) {
        switch_session(a, NULL);
    } else if (pt_str_eqz(cmd, "resume")) {
        if (!arg.n) {
            tui_input_set(&a->in, PT_S("/resume "));
            a->menu.closed = false;
            menu_update(a);
            return;
        }
        switch_session(a, v);
    } else if (pt_str_eqz(cmd, "fork")) {
        if (!a->open)
            out_z(a, TUI_RED, "no session to fork");
        else if (session_empty(a))
            out_z(a, TUI_DIM, "nothing to fork yet");
        else if (a->run.active)
            out_z(a, TUI_RED, "/fork waits for the run to end (esc esc stops it)");
        else {
            a->o.fork = true;
            switch_session(a, NULL);
            a->o.fork = false;
            a->sess.created = false; /* a copy: never discarded as empty */
        }
    } else if (pt_str_eqz(cmd, "worktree") || pt_str_eqz(cmd, "wt")) {
        if (arg.n && !cli_worktree_name_ok(v)) {
            snprintf(msg, sizeof msg, "not a worktree name: %.100s (letters, digits, - _ .)", v);
            out_z(a, TUI_RED, msg);
            return;
        }
        a->o.worktree = v; /* copied by the open */
        switch_session(a, NULL);
        a->o.worktree = NULL;
    } else if (pt_str_eqz(cmd, "status")) {
        cmd_status(a);
    } else if (pt_str_eqz(cmd, "compact")) {
        if (!a->open || !pt_compact(a->s.a))
            out_z(a, TUI_DIM, "nothing to compact yet");
    } else if (pt_str_eqz(cmd, "usage")) {
        cmd_usage(a);
    } else if (pt_str_eqz(cmd, "ps")) {
        cmd_ps(a);
    } else if (pt_str_eqz(cmd, "stop")) {
        uint32_t n = a->open ? pt_rt_backgrounds_stop(a->s.rt) : 0;
        char said[64];
        snprintf(said, sizeof said, n ? "stopping %u background command%s" : "no background commands running", n,
                 n == 1 ? "" : "s");
        out_z(a, TUI_DIM, said);
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
            a->menu.kind = MENU_SETTINGS;
            a->menu.set_sel = a->menu.top = 0;
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
    } else if (pt_str_eqz(cmd, "login")) {
        cmd_login(a, arg);
    } else if (pt_str_eqz(cmd, "mcp")) {
        cmd_mcp(a, arg);
    } else if (pt_str_eqz(cmd, "help")) {
        cmd_help(a);
    } else if (pt_str_eqz(cmd, "quit") || pt_str_eqz(cmd, "exit")) {
        a->sess.keep_on_quit = true;
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

static void history_load(app *a)
{
    char root[4096], *slash;
    cli_sessions_dir(root, sizeof root);
    if ((slash = strrchr(root, '/')))
        *slash = 0;
    snprintf(a->hist.path, sizeof a->hist.path, "%s/history", root);
}

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

/* ---- MCP: prompts and @-mentions in what is sent ---- */

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

static void xreq_drop(app *a, xreq *x)
{
    xreq **pp;
    for (pp = &a->queue.xs; *pp; pp = &(*pp)->next)
        if (*pp == x) {
            *pp = x->next;
            break;
        }
    free(x);
}

/* The configured server of that name: running, or waiting for trust. */
static bool mcp_known(app *a, pt_str name)
{
    uint32_t i;
    for (i = 0; a->mcp && i < a->mcp->nall; i++)
        if (pt_str_eqz(name, a->mcp->all[i].name))
            return true;
    return false;
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

static void queued_submit(app *a)
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

static void finish_session(app *a)
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

static void submit(app *a)
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

/* Tab: complete the selected menu item. Returns true when it changed
 * the draft. */
static bool complete(app *a)
{
    pt_str d = pt_buf_str(&a->in.text), label;
    pt_buf nd;
    bool changed;
    if (a->menu.kind == MENU_NONE || a->menu.kind == MENU_SETTINGS || !a->menu.n)
        return false;
    label = item_label(a, a->menu.sel);
    pt_buf_init(&nd, pt_sys_allocator());
    if (a->menu.kind == MENU_CMD) {
        size_t i;
        pt_buf_putc(&nd, '/');
        pt_buf_puts(&nd, label);
        for (i = 0; i < NCMDS; i++)
            if (pt_str_eqz(label, cmds[i].name) && cmds[i].args[0])
                pt_buf_putc(&nd, ' ');
        if (pt_str_starts(label, PT_S("skill:"))) /* then what it is for */
            pt_buf_putc(&nd, ' ');
        else if (find_prompt(a, label) && find_prompt(a, label)->nargs) /* then its arguments */
            pt_buf_putc(&nd, ' ');
    } else if (a->menu.of_sessions) { /* the whole argument: names have spaces */
        pt_buf_puts(&nd, PT_S("/resume "));
        pt_buf_puts(&nd, label);
    } else { /* replace the last word */
        pt_buf_put(&nd, d.p, (size_t)(last_word(d).p - d.p));
        pt_buf_puts(&nd, label);
        if ((label.n && (label.p[0] == '$' || label.p[0] == '@')) || pt_str_starts(d, PT_S("/login ")) ||
            (pt_str_starts(d, PT_S("/mcp ")) && pt_str_find((pt_str){d.p + 5, d.n - 5}, ' ') < 0 &&
             !pt_str_eqz(label, "trust") && !pt_str_eqz(label, "untrust"))) /* then the key, the name */
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
    int k = settings[a->menu.set_sel].k, i, dir = ev->key == TK_LEFT ? -1 : 1;
    const char *const *vals = settings[a->menu.set_sel].vals;
    int nv = settings[a->menu.set_sel].nv;
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
        snprintf(d, sizeof d, "/settings %s %s", settings[a->menu.set_sel].name, k == CFG_PAGER ? a->cfg->val[k] : "");
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

static void on_key(app *a, const tui_ev *ev)
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
        save_setting(a, CFG_THINKING, th_names[(a->thinking + 1) % 3]);
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
        due = now + BLINK_MS - run_clock(a, now) % BLINK_MS;
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
            on_key(a, &ev);
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
        pt_buf_free(&a->run.calls[i].out);
    }
    pt_buf_free(&a->diff);
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
        cli_mcp_host(&a->o.mcp_host, mcp_note, a);
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
    apply_symbols(a);
    apply_thinking(a);
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
        pt_buf_init(&a->run.calls[slot].out, pt_sys_allocator());
    }
    pt_buf_init(&a->diff, pt_sys_allocator());
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
    history_load(a);
    if (!getcwd(a->sess.start_dir, sizeof a->sess.start_dir))
        a->sess.start_dir[0] = 0;
    if (id)
        snprintf(picked, sizeof picked, "%s", id);
    if (!open_session(a, id ? picked : NULL)) {
        startup_free(a);
        fprintf(stderr, "pytte: %s\n", a->sess.err);
        return a->code ? a->code : 1;
    }
    a->o.worktree = NULL; /* -w: this open only; copied by it */
    set_config(a);
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
        out_text(a, &first, TUI_DIM, "pytte", ACCENT | TUI_BOLD, pt_strz(head), 0);
        pt_buf_puts(&first, pt_buf_str(&a->lines));
        pt_buf_reset(&a->lines);
        pt_buf_puts(&a->lines, pt_buf_str(&first));
        pt_buf_free(&first);
    }
    if (a->mcp && a->mcp->warn[0])
        out_z(a, TUI_RED, a->mcp->warn);
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
        out_z(a, TUI_DIM, line);
    }
    if (prompt && *prompt) {
        a->draft.drop_restored = true;
        tui_input_set(&a->in, pt_strz(prompt));
        submit(a);
    }
    menu_update(a);
    paint(a);
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
            finish_session(a);
        } else if (a->open)
            pt_rt_step(a->s.rt, timeout_ms(a, now), fds, 5);
        else
            pt_sys_wait(fds, 5, timeout_ms(a, now));
        finish_setting(a);
        show_notes(a);
        if (a->queue.expanded) {
            a->queue.expanded = false;
            queued_submit(a);
            set_dirty(a);
        }
        if (a->menu.sessions && a->menu.sessions->pending && pt_worker_finish(a->menu.sessions->worker)) {
            a->menu.sessions->pending = false;
            a->menu.sessions->ready = true;
            menu_update(a);
            a->pace.urgent = true;
        }
        now = pt_sys_now_ms();
        {
            uint32_t bg = bg_running(a);
            if (bg != a->run.nbg) {
                a->run.nbg = bg;
                set_dirty(a);
            }
        }
        a->run.active = a->open && !pt_idle(a->s.a) && !pt_fatal(a->s.a);
        if (a->run.active != was) {
            set_dirty(a);
            if (a->run.active) {
                a->run.at = now;
                a->run.wait_at = 0;
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
                suspend(a);
                tui_term_stop_self();
                resume(a);
            } else if (bits & TS_CONT) {
                resume(a); /* stopped by someone else; the shell may have reset the modes */
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
        if (a->run.active && !a->run.wait_at && run_clock(a, now) / BLINK_MS != a->run.tick) {
            a->run.tick = run_clock(a, now) / BLINK_MS;
            set_dirty(a);
        }
        if (now - a->pace.esc_at < DOUBLE_MS + 50 || now - a->pace.ctrlc_at < DOUBLE_MS + 50)
            set_dirty(a);
        if (a->draft.dirty && now - a->draft.at >= 1000)
            save_draft(a);
        if (a->quit)
            break;
        if (a->pace.urgent || (a->pace.dirty && now - a->pace.last_paint >= a->pace.interval))
            paint(a);
    }
    /* quit: the close claim, then the terminal. What was shown of an
     * unfinished answer stays (pytte -c picks the run up again). */
    if (a->code != 129 && ((a->view.kind != LIVE_NONE && a->view.live.n) || a->group.n)) {
        if (a->view.kind != LIVE_NONE && a->view.live.n) {
            live_end(a);
            out_z(a, TUI_DIM, "(detached)");
        }
        group_close(a); /* the band is erased next */
        a->menu.kind = MENU_NONE;
        paint(a);
    }
    a->quit = true;
    while (a->sess.opening) {
        pt_waitfd fd;
        fd.fd = cli_session_job_fd(a->sess.opening);
        pt_sys_wait(&fd, 1, -1);
        finish_session(a);
    }
    snprintf(kept, sizeof kept, "%.63s", a->open ? a->s.id : "");
    close_session(a);
    while (a->saving && (a->saving->active || a->saving->pending)) {
        pt_waitfd fd;
        finish_setting(a);
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
    flush_out(a);
    if (a->code != 129 && kept[0] && !a->sess.discarded) { /* how to come back to it */
        char last[200];
        pt_buf_reset(&a->lines);
        out_blank(&a->lines);
        snprintf(last, sizeof last, "to continue this session: pytte -r %s", kept);
        out_z(a, TUI_DIM, last);
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
        pt_buf_free(&a->run.calls[slot].out);
    }
    pt_buf_free(&a->diff);
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
