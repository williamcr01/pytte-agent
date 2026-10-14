/* The TUI app's private header: its state, and what its files share.
 *
 * app.c          setup and the event loop
 * app_view.c     scrollback output, tool-call groups, the streaming tail
 * app_band.c     the band's frame: live rows, the question, input, menu, status
 * app_menu.c     the menu and completion
 * app_cmd.c      settings, the model, and the slash commands
 * app_session.c  sessions, drafts, input history, the pager
 * app_input.c    keys, the input queue, MCP prompt and @-mention expansion
 *
 * Everything runs on the event-loop thread (see app.c). */
#ifndef PT_TUI_APP_H
#define PT_TUI_APP_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli/cli.h"
#include "tui/tui.h"

enum { TH_HIDDEN, TH_FULL };
enum { PH_WAIT, PH_THINK, PH_WRITE, PH_RETRY, PH_TOOL, PH_COMPACT };
enum { LIVE_NONE, LIVE_TEXT, LIVE_THINK };
enum { MENU_NONE, MENU_CMD, MENU_ARG, MENU_SETTINGS, MENU_TOOLS };

#define MAX_ITEMS 400
#define DOUBLE_MS 1000

/* The left edge is a column of marks, one per kind of row: the prompt and
 * what the user sent, thinking, notes, tool calls that worked or failed, a
 * question. Prose sits two columns in, beside them. */
typedef struct syms {
    const char *prompt, *think, *note, *ok, *fail, *ask, *sel, *bar, *more, *dot;
    const char *up, *down; /* tokens read and written */
    const char *join;      /* between calls of one round on a row */
    const char *on, *off;  /* a tool turned on or off (/tools) */
} syms;


#define ACCENT TUI_MAGENTA /* the prompt, the spinner, the selection */
#define MARGIN 2           /* where prose starts, after a mark and a space */
#define NAME_W 6           /* a tool's name column: "bash  ", "write " */

#define BLINK_MS 500 /* the activity mark: on for the first half of each second of the clock */
#define TOOL_FAIL 3  /* rows of a failed tool's output kept in the scrollback */
#define RUN_MAX 8      /* running calls the band shows; more still run */

/* The slash commands, for the menu and /help (app_cmd.c). */
typedef struct app_cmd_def {
    const char *name, *args, *desc;
} app_cmd_def;
extern const app_cmd_def app_cmds[];
extern const size_t app_ncmds;
#define NCMDS app_ncmds

extern const char *const app_levels[]; /* reasoning efforts, by PT_EFF_* bit */
extern const size_t app_nlevels;
#define NLEVELS app_nlevels
#define NTH (TH_FULL + 1)
extern const char *const app_th_names[]; /* by TH_*, as configured: NTH of them */

/* The settings menu: config index, short name, and the values to cycle
 * through (none: edited as a command). SET_TOOLS: the tools, which open
 * their own menu. */
#define SET_TOOLS CFG_N
typedef struct app_setting {
    int k;
    const char *name;
    const char *const *vals;
    int nv;
} app_setting;
extern const app_setting app_settings[];
extern const size_t app_nsettings;
#define NSETTINGS app_nsettings


typedef struct item {
    size_t at, n;      /* label in app_menu.buf */
    size_t dat, dn;    /* description */
} item;

/* Tool calls in a row form a group (see group_add); the calls of one
 * assistant message, a round, share a row. */
#define GROUP_MAX 128 /* calls of a group; a longer run of calls makes more groups */
#define GROUP_LIVE 8  /* rows of an open group shown in the band */
#define NO_ROUND UINT32_MAX /* a call whose message was not found: a row of its own */
enum { CAT_COMMAND, CAT_READ, CAT_WRITE, CAT_EDIT, CAT_OTHER, NCAT };
enum { G_OK, G_FAILED, G_TIMEOUT, G_DENIED, G_CANCELLED, G_INTERRUPTED, NBAD };

typedef struct grow {
    uint32_t at, n;   /* the call's part of its row, styled, in group.buf */
    uint32_t name;    /* ...of which the tool's name, and the space after it */
    uint32_t dat, dn; /* its details, raw lines: a failure's last lines */
    uint32_t round;   /* the entry of its message */
    uint8_t cat, bad;
} grow;

/* A call that is running: its row, and how many lines it has printed. */
typedef struct run_call {
    uint32_t task;
    char name[64];
    pt_buf id, head;
    size_t gist;    /* where the gist starts in head: before it, the tool's name */
    uint32_t round; /* as in grow */
    uint64_t lines; /* ended lines of output */
    bool open;      /* and one more is under way */
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
    const char *wins[CFG_N]; /* where a value that wins at the next start comes from: "env", "project" */
    /* Tools turned on or off, waiting to be saved: one entry per tool, its
     * newest state. key SET_TOOLS: the worker writes tool. */
    struct tool_save {
        char name[128];
        bool on, now; /* now: the session had sent nothing, so its first request gets it */
        const char *wins;
    } tq[CLI_TOOLS_MAX], tool;
    uint32_t ntq;
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
    /* Tokens the run read and wrote, by the provider's count: the requests
     * done, and the one streaming, whose output is guessed from its bytes
     * until its count arrives. */
    uint64_t tok_in, tok_out, req_in, req_out, req_bytes;
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
    bool pinned;     /* chosen (flag, environment, /model, /settings model): sessions opened next use it too */
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
    bool tools_pending;  /* the tools menu opens once the session has */
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
    cli_oauth *login; /* a /login oauth waiting for the browser */
    bool pager_pending;
    tui_term *t;
    tui_keys keys;
    tui_band band;
    tui_frame frame;
    int above; /* the band's rows above the input last paint, blanks included */
    tui_input in;
    int rows, cols;
    app_caps caps;
    bool ascii, color;
    const syms *sy;
    int thinking;
    /* output: styled lines for the scrollback, committed at the next paint */
    pt_buf lines;
    pt_buf tmp, tmp2, wrap, live_rows; /* scratch, reused */
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

static inline void set_dirty(app *a)
{
    a->pace.dirty = true;
}

/* The width text wraps to: one column short of the edge. */
static inline int width(const app *a)
{
    return a->cols > 2 ? a->cols - 1 : 1;
}

/* app_view.c: scrollback output and the display model built from the agent's events */
void app_out_text(app *a, pt_buf *out, int style, const char *prefix, int pstyle, pt_str raw, int indent);
void app_out_note(app *a, pt_buf *out, int style, pt_str raw);
void app_out_z(app *a, int style, const char *z);
void app_out_blank(pt_buf *out);
pt_str app_call_gist(app *a, pt_str args);
int app_tool_of(pt_str name);
void app_phrase(app *a, pt_buf *out, pt_str name, pt_str args, size_t *gist);
void app_secs_short(char *out, size_t cap, int64_t ms);
int64_t app_run_clock(const app *a, int64_t now);
void app_run_restart(app *a, int64_t now);
uint64_t app_run_out(const app *a);
const char *app_blink(const app *a, int64_t now);
bool app_starts(pt_str s, const char *z);
bool app_runs_oom(const app *a);
void app_group_render(app *a, pt_buf *out, bool live);
void app_group_close(app *a);
void app_out_entry(app *a, pt_buf *out, const pt_entry_view *v, bool full);
void app_live_end(app *a);
void app_on_event(void *ud, const pt_event *ev);
void app_out_tail(app *a);

/* app_band.c: the band: the frame under the scrollback, and painting it */
void app_tokens_short(char *out, size_t cap, uint64_t n);
bool app_asking(app *a);
void app_flush_out(app *a);
void app_paint(app *a);

/* app_menu.c: the menu under the input, and completing from it */
int app_find_setting(pt_str name);
bool app_menu_takes_arg(pt_str cmd);
pt_str app_item_label(const app *a, int i);
const pt_rt_mcp_prompt *app_find_prompt(app *a, pt_str cmd);
size_t app_secret_at(pt_str d);
void app_menu_update(app *a);
bool app_complete(app *a);

/* app_cmd.c: settings, the model, and the slash commands */
void app_effort_label(const app *a, char *out, size_t cap);
void app_apply_symbols(app *a);
void app_apply_thinking(app *a);
void app_finish_setting(app *a);
/* A /login oauth that ended: say how. */
void app_finish_login(app *a);
bool app_save_setting(app *a, int k, const char *v);
void app_update_model(app *a);
void app_set_config(app *a);
void app_model_for(app *a, const char *id, bool fork);
uint32_t app_bg_running(app *a);
void app_mcp_note(void *ud, const char *server, const char *msg);
void app_show_notes(app *a);
void app_run_command(app *a, pt_str line);
/* /tools: the registered tools, on or off */
void app_tools_list(app *a);
void app_tool_set(app *a, pt_str name, bool on);
void app_tools_open(app *a);
uint32_t app_tools_on(app *a, uint32_t *all);
bool app_mcp_known(app *a, pt_str name);

/* app_session.c: opening, switching and closing sessions; drafts, history, the pager */
void app_save_draft(app *a);
bool app_session_empty(app *a);
bool app_open_session(app *a, const char *id);
void app_close_session(app *a);
void app_switch_session(app *a, const char *id);
void app_suspend(app *a);
void app_resume(app *a);
void app_pager(app *a);
void app_history_append(app *a, pt_str text);
void app_history_load(app *a);
void app_finish_session(app *a);

/* app_input.c: what is typed: keys, the input queue, MCP prompts and @-mentions */
void app_xreq_drop(app *a, xreq *x);
void app_queued_submit(app *a);
void app_submit(app *a);
void app_on_key(app *a, const tui_ev *ev);

#endif
