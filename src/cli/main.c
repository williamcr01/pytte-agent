/* pytte: the durable agent CLI (section 16.1).
 *
 *   pytte ["PROMPT"]            the TUI in a new session
 *   pytte -c / -r [ID]          the TUI on this directory's newest session / on ID
 *   pytte -p "PROMPT"           one-shot: run until idle and print the answer
 *   ... | pytte -p "PROMPT"     the same, with stdin as data for the prompt
 *   pytte log [ID] [--json]     print a session's transcript
 *   pytte approve ID TASK yes|no   answer a tool call waiting for approval
 *   pytte ls                    list sessions with their status
 *   pytte update-catalog        fetch the model catalog (models.dev, OpenRouter)
 *   pytte mcp [list|trust|untrust|login NAME|logout NAME]   MCP servers
 *
 * Kill the process at any point and `pytte -c` continues from its last
 * commit. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif

#include "cli/cli.h"

static const char usage[] =
    "usage: pytte [options] [PROMPT]      the TUI in a new session\n"
    "       pytte -c | -r [ID] [PROMPT]   continue this directory's newest session, or ID\n"
    "       pytte -p [options] [PROMPT]   print mode: run until done, print the answer\n"
    "                                     on stdout; tool calls, thinking and the\n"
    "                                     text between them go to stderr. Piped\n"
    "                                     stdin is the PROMPT, or data for it\n"
    "       pytte log [ID] [--json] [--all-epochs]\n"
    "                                     print a session's transcript (by default\n"
    "                                     since the last compaction)\n"
    "       pytte approve ID TASK yes|no  answer a tool call waiting for approval,\n"
    "                                     then go on as -p would\n"
    "       pytte ls                      list sessions, newest first: status, directory\n"
    "                                     and name (its first prompt)\n"
    "       pytte update-catalog          fetch the newest models from models.dev and\n"
    "                                     OpenRouter into ~/.cache/pytte/models.bin\n"
    "       pytte mcp [list]              MCP servers (mcpServers in config.json; a\n"
    "                                     project's in .mcp.json): their state, tools\n"
    "       pytte mcp trust | untrust     run this directory's project servers, or not\n"
    "       pytte mcp login | logout NAME log in to a remote server (in the browser)\n"
    "\n"
    "  -c, --continue          the newest session started in this directory\n"
    "  -r, --resume [ID]       that session, or pick from this directory's\n"
    "  -p, --print             no TUI (also when stdin or stdout is not a terminal)\n"
    "  -w, --worktree [NAME]   start in the git worktree <repo>/.pytte/worktrees/NAME\n"
    "                          on branch worktree-NAME, made if missing (default: a\n"
    "                          random name; --worktree=NAME, or -- before a PROMPT\n"
    "                          that could be a name)\n"
    "  -m, --model REF         provider/model-id (default: $PYTTE_MODEL, the config\n"
    "                          file; else the session's last, or " DEFAULT_MODEL ")\n"
    "  --effort LEVEL          off, minimal, low, medium, high, xhigh, max ($PYTTE_EFFORT)\n"
    "                          starts at medium unless configured otherwise\n"
    "  --compact-model REF     the model that writes compaction summaries (default:\n"
    "                          $PYTTE_COMPACT_MODEL, the config file, else the model)\n"
    "  --max-tokens N          default: the model's maximum that fits the context\n"
    "  --context-window N      compact as if the model's window were N tokens\n"
    "  --system-prompt TEXT    system prompt of a new session\n"
    "  --append-system-prompt TEXT\n"
    "  --output-format FMT     with -p: text (default), json (one object at the end),\n"
    "                          stream-json (every commit as a JSON line)\n"
    "  --permission-mode MODE  ask (default), auto or read-only: what tools that may\n"
    "                          change things do ($PYTTE_PERMISSION_MODE, permissions.mode)\n"
    "  --script FILE           faux provider script (default: $PYTTE_FAUX_SCRIPT)\n"
    "  --no-stdin              with -p: leave stdin unread (as ssh -n does), for\n"
    "                          loops and callers that keep it open\n"
    "  --no-sync               do not fsync commits (survives crashes, not power loss)\n"
    "  --no-mcp                start no MCP servers\n"
    "  --force                 take over a session another host left open\n"
    "\n"
    "Exit codes: 0 done, 1 usage or setup, 3 waiting for approval (-p), 10 the run\n"
    "failed or stdout could not be written, 20 storage error, 130 interrupted, 141\n"
    "stdout closed (the session stays resumable after either).\n";

enum { OUT_TEXT, OUT_JSON, OUT_STREAM };

typedef struct opts {
    cli_opts c;
    const char *cmd_log, *cmd_approve, *cmd_ls, *cmd_update, *cmd_mcp, *id, *prompt, *task, *answer;
    const char *mcp_args[2];
    bool print, cont, resume, json, all_epochs, model_flag, compact_flag, effort_flag, perm_flag;
    bool no_stdin, no_mcp;
    int out;
} opts;

static void load_mcp(opts *o, bool quiet);

static void die(const char *msg, const char *arg)
{
    fprintf(stderr, "pytte: %s%s%s\n", msg, arg ? ": " : "", arg ? arg : "");
    exit(1);
}

/* YYYYMMDD-HHMMSS-xxxx */
static bool looks_like_id(const char *s)
{
    return strlen(s) == 20 && s[8] == '-' && s[15] == '-' && s[0] >= '0' && s[0] <= '9';
}

static void parse_args(int argc, char **argv, opts *o)
{
    int i;
    memset(o, 0, sizeof o[0]);
    o->c.reasoning = DEFAULT_EFFORT;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
#define TAKES(name) (strcmp(a, name) == 0 && (v || (die("missing value for", name), 0)) && ++i)
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            fputs(usage, stdout);
            exit(0);
        } else if (strcmp(a, "--version") == 0) {
            puts("pytte " PT_VERSION);
            exit(0);
        } else if (strcmp(a, "-p") == 0 || strcmp(a, "--print") == 0) {
            o->print = true;
        } else if (strcmp(a, "-c") == 0 || strcmp(a, "--continue") == 0) {
            o->cont = true;
        } else if (strcmp(a, "-r") == 0 || strcmp(a, "--resume") == 0) {
            o->resume = true;
            if (v && looks_like_id(v))
                o->id = argv[++i];
        } else if (strcmp(a, "-w") == 0 || strcmp(a, "--worktree") == 0) {
            o->c.worktree = ""; /* a random name */
            if (v && cli_worktree_name_ok(v))
                o->c.worktree = argv[++i];
        } else if (strncmp(a, "--worktree=", 11) == 0) {
            o->c.worktree = a + 11;
            if (!cli_worktree_name_ok(o->c.worktree))
                die("not a worktree name (letters, digits, - _ .)", o->c.worktree);
        } else if (strcmp(a, "--json") == 0) { /* log */
            o->json = true;
        } else if (strcmp(a, "--all-epochs") == 0) { /* log */
            o->all_epochs = true;
        } else if (strcmp(a, "--no-stdin") == 0) {
            o->no_stdin = true;
        } else if (strcmp(a, "--no-mcp") == 0) {
            o->no_mcp = true;
        } else if (strcmp(a, "--no-sync") == 0) {
            o->c.no_sync = true;
        } else if (strcmp(a, "--force") == 0) {
            o->c.force = true;
        } else if (TAKES("-m") || TAKES("--model")) {
            o->c.model = v;
            o->model_flag = true;
        } else if (TAKES("--compact-model")) {
            o->c.compact_model = v;
            o->compact_flag = true;
        } else if (TAKES("--system-prompt")) {
            o->c.system = v;
        } else if (TAKES("--append-system-prompt")) {
            o->c.append_system = v;
        } else if (TAKES("--script")) {
            o->c.script = v;
        } else if (TAKES("--effort")) {
            if (!cli_effort_parse(pt_strz(v), &o->c.reasoning))
                die("unknown effort level", v);
            o->effort_flag = true;
        } else if (TAKES("--permission-mode")) {
            if (!pt_perm_parse(pt_strz(v), &o->c.perm))
                die("unknown permission mode (ask, auto, read-only)", v);
            o->perm_flag = true;
        } else if (TAKES("--output-format")) {
            if (strcmp(v, "text") == 0)
                o->out = OUT_TEXT;
            else if (strcmp(v, "json") == 0)
                o->out = OUT_JSON;
            else if (strcmp(v, "stream-json") == 0)
                o->out = OUT_STREAM;
            else
                die("unknown output format", v);
        } else if (TAKES("--max-tokens") || TAKES("--retry-base-ms") || TAKES("--context-window")) {
            uint64_t n;
            if (!pt_str_to_u64(pt_strz(v), &n) || n == 0 || n > UINT32_MAX)
                die("bad number", v);
            if (strcmp(a, "--max-tokens") == 0)
                o->c.max_tokens = (uint32_t)n;
            else if (strcmp(a, "--context-window") == 0)
                o->c.context_window = (uint32_t)n;
            else
                o->c.retry_base_ms = (uint32_t)n; /* for tests */
        } else if (strcmp(a, "--") == 0 && v) {
            o->prompt = argv[++i];
        } else if (a[0] == '-' && a[1]) {
            die("unknown option", a);
        } else if (i == 1 && strcmp(a, "log") == 0) {
            o->cmd_log = a;
        } else if (i == 1 && strcmp(a, "ls") == 0) {
            o->cmd_ls = a;
        } else if (i == 1 && strcmp(a, "update-catalog") == 0) {
            o->cmd_update = a;
        } else if (i == 1 && strcmp(a, "mcp") == 0) {
            o->cmd_mcp = a;
        } else if (o->cmd_mcp) {
            if (o->mcp_args[1])
                die("too many arguments", a);
            o->mcp_args[o->mcp_args[0] ? 1 : 0] = a;
        } else if (i == 1 && strcmp(a, "approve") == 0) {
            o->cmd_approve = a;
        } else if ((o->cmd_log || o->cmd_approve) && !o->id) {
            o->id = a;
        } else if (o->cmd_approve && !o->task) {
            o->task = a;
        } else if (o->cmd_approve && !o->answer) {
            if (strcmp(a, "yes") != 0 && strcmp(a, "no") != 0)
                die("answer yes or no", a);
            o->answer = a;
        } else if (o->cmd_ls || o->cmd_update) {
            die(o->cmd_ls ? "ls takes no arguments" : "update-catalog takes no arguments", a);
        } else if (!o->prompt) {
            o->prompt = a;
        } else {
            die("too many arguments", a);
        }
#undef TAKES
    }
    if (o->cmd_approve && !o->answer)
        die("usage: pytte approve ID TASK yes|no", NULL);
    if (!o->c.script)
        o->c.script = getenv("PYTTE_FAUX_SCRIPT");
}

/* An unknown model stops pytte before a session exists to leave behind. */
static void check_model(const char *ref, const char *src)
{
    pt_arena a;
    pt_ai_target t;
    const char *why;
    bool ok;
    if (!ref || !*ref)
        return;
    pt_arena_init(&a, pt_sys_allocator(), 0);
    ok = pt_ai_resolve(cli_catalog(), pt_strz(ref), &t, &a, &why);
    pt_arena_free(&a);
    if (!ok) {
        fprintf(stderr, "pytte: model %s (from %s): %s, e.g. %s\n", ref, src, why, DEFAULT_MODEL);
        exit(1);
    }
}

/* Say on stderr when the model does not take the chosen effort level and
 * requests use another. */
static void effort_note(const char *ref, pt_reasoning want)
{
    pt_arena a;
    pt_ai_target t;
    const char *why;
    if (!ref || !*ref)
        return;
    pt_arena_init(&a, pt_sys_allocator(), 0);
    if (pt_ai_resolve(cli_catalog(), pt_strz(ref), &t, &a, &why) && pt_ai_effort(&t, want) != want)
        fprintf(stderr, "pytte: %s takes no effort %s; using %s\n", ref, pt_reasoning_name(want),
                pt_reasoning_name(pt_ai_effort(&t, want)));
    pt_arena_free(&a);
}

/* Model: a flag, else the environment; config files set new sessions'
 * defaults (an existing one keeps its last model). Effort: a flag, else
 * the layered settings, starting at an explicit medium. */
static void apply_config(opts *o, cli_config *cfg, bool create)
{
    bool env_or_new;
    env_or_new = create || strcmp(cfg->src[CFG_MODEL], "env") == 0;
    if (o->model_flag)
        cli_config_set(cfg, CFG_MODEL, o->c.model, "flag");
    else if (cfg->val[CFG_MODEL][0] && env_or_new)
        o->c.model = cfg->val[CFG_MODEL];
    else if (create)
        o->c.model = DEFAULT_MODEL;
    if (o->effort_flag) {
        cli_config_set(cfg, CFG_REASONING, pt_reasoning_name(o->c.reasoning), "flag");
    } else if (!cli_effort_parse(pt_strz(cfg->val[CFG_REASONING]), &o->c.reasoning)) {
        fprintf(stderr, "pytte: unknown effort level %s (from %s)\n", cfg->val[CFG_REASONING],
                cfg->src[CFG_REASONING]);
        exit(1);
    }
    if (o->perm_flag) {
        cli_config_set(cfg, CFG_PERMS, pt_perm_name(o->c.perm), "flag");
    } else if (!pt_perm_parse(pt_strz(cfg->val[CFG_PERMS]), &o->c.perm)) {
        fprintf(stderr, "pytte: unknown permission mode %s (from %s)\n", cfg->val[CFG_PERMS],
                cfg->src[CFG_PERMS]);
        exit(1);
    }
    if (strcmp(cfg->val[CFG_SKILLS], "on") != 0 && strcmp(cfg->val[CFG_SKILLS], "off") != 0) {
        fprintf(stderr, "pytte: skills.auto is on or off, not %s (from %s)\n", cfg->val[CFG_SKILLS],
                cfg->src[CFG_SKILLS]);
        exit(1);
    }
    o->c.manual_skills = strcmp(cfg->val[CFG_SKILLS], "off") == 0;
    if (o->compact_flag)
        cli_config_set(cfg, CFG_COMPACT_MODEL, o->c.compact_model, "flag");
    else if (cfg->val[CFG_COMPACT_MODEL][0])
        o->c.compact_model = cfg->val[CFG_COMPACT_MODEL];
    check_model(o->c.model, o->c.model == cfg->val[CFG_MODEL] ? cfg->src[CFG_MODEL] : "flag");
    check_model(o->c.compact_model, cfg->src[CFG_COMPACT_MODEL]);
}

/* ---- print mode ---- */

/* Where a message's text goes. stdout gets only answers: when it is not
 * a terminal, a message's text waits until the message is known to call
 * no tools (an answer, for stdout) or to call some (narration, for
 * stderr). */
enum { MSG_HOLD, MSG_OUT, MSG_ERR };

typedef struct ui {
    int out;
    bool tty_err, tty_out;
    bool text_nl, any_text, thinking_out;
    FILE *text_to; /* where the last text went */
    int msg;       /* MSG_*: the streaming message's text */
    int out_errno; /* stdout failed: EPIPE when its reader left */
    int runs_failed, runs_done;
    pt_buf held;          /* the message's text while MSG_HOLD */
    pt_buf result, error; /* for --output-format json */
} ui;

static volatile sig_atomic_t interrupted;

static void on_signal(int sig)
{
    (void)sig;
    interrupted = 1;
}

static bool should_stop(void *ud)
{
    (void)ud;
    return interrupted != 0;
}

/* The first stdout failure stops the run: nobody gets the answer. */
static void out_flush(ui *u)
{
    if ((fflush(stdout) != 0 || ferror(stdout)) && !u->out_errno) {
        u->out_errno = errno ? errno : EIO;
        interrupted = 1;
    }
}

static void end_text(ui *u)
{
    if (u->any_text && !u->text_nl)
        fputc('\n', u->text_to);
    u->any_text = false;
    u->text_nl = true;
    if (u->text_to == stdout)
        out_flush(u);
}

static void put_text(ui *u, FILE *f, pt_str s)
{
    if (!s.n)
        return;
    if (u->any_text && u->text_to != f)
        end_text(u);
    fwrite(s.p, 1, s.n, f);
    u->text_to = f;
    u->text_nl = s.p[s.n - 1] == '\n';
    u->any_text = true;
    if (f == stdout)
        out_flush(u);
    else
        fflush(f);
}

/* The held text goes to f, and so does the rest of the message. */
static void release(ui *u, FILE *f)
{
    put_text(u, f, pt_buf_str(&u->held));
    pt_buf_reset(&u->held);
    u->msg = f == stdout ? MSG_OUT : MSG_ERR;
}

static void on_stream(ui *u, const pt_ai_event *ev)
{
    const pt_msg *m = ev->msg;
    switch (ev->kind) {
    case PT_EV_START:
        u->msg = u->tty_out ? MSG_OUT : MSG_HOLD;
        break;
    case PT_EV_THINKING_DELTA:
        if (u->any_text)
            end_text(u);
        if (!u->thinking_out && u->tty_err)
            fputs("\x1b[2m", stderr);
        u->thinking_out = true;
        fwrite(ev->delta.p, 1, ev->delta.n, stderr);
        break;
    case PT_EV_THINKING_END:
        if (u->thinking_out)
            fputs(u->tty_err ? "\x1b[0m\n" : "\n", stderr);
        u->thinking_out = false;
        break;
    case PT_EV_TEXT_DELTA:
        if (u->msg == MSG_HOLD) {
            pt_buf_puts(&u->held, ev->delta);
            if (!u->held.oom)
                break;
            /* Out of memory: the text goes out now, as if it were the answer.
             * The buffer keeps what it had; a fresh one forgets the failure. */
            release(u, stdout);
            pt_buf_free(&u->held);
            pt_buf_init(&u->held, pt_sys_allocator());
        }
        put_text(u, u->msg == MSG_ERR ? stderr : stdout, ev->delta);
        break;
    case PT_EV_TOOLCALL_START:
        if (u->msg == MSG_HOLD)
            release(u, stderr);
        break;
    case PT_EV_TOOLCALL_END: {
        const pt_block *b = &m->blocks[ev->index];
        end_text(u);
        fprintf(stderr, "%s-> %.*s %.*s%s\n", u->tty_err ? "\x1b[36m" : "", (int)b->name.n, b->name.p,
                (int)b->args.n, b->args.p, u->tty_err ? "\x1b[0m" : "");
        break;
    }
    case PT_EV_DONE:
    case PT_EV_ERROR:
        if (u->thinking_out)
            fputs(u->tty_err ? "\x1b[0m\n" : "\n", stderr);
        u->thinking_out = false;
        /* A cut-off message is no answer: a retry or the error follows. */
        if (u->msg == MSG_HOLD)
            release(u, ev->kind == PT_EV_DONE ? stdout : stderr);
        end_text(u);
        break;
    default:
        break;
    }
}

static void on_event(void *ud, const pt_event *ev)
{
    ui *u = (ui *)ud;
    if (ev->kind == PT_E_RUN_DONE) {
        bool ok = pt_str_eqz(ev->outcome, "completed");
        ok ? u->runs_done++ : u->runs_failed++;
        pt_buf_reset(&u->error);
        if (!ok)
            pt_buf_puts(&u->error, ev->error);
    }
    if (u->out == OUT_JSON) {
        if (ev->kind == PT_E_STREAM && ev->ai->kind == PT_EV_START)
            pt_buf_reset(&u->result);
        if (ev->kind == PT_E_STREAM && ev->ai->kind == PT_EV_TEXT_DELTA)
            pt_buf_puts(&u->result, ev->ai->delta);
        return;
    }
    if (u->out == OUT_STREAM) {
        if (ev->kind == PT_E_COMMITTED) {
            fwrite(ev->line.p, 1, ev->line.n, stdout);
            fputc('\n', stdout);
            out_flush(u);
        }
        return;
    }
    switch (ev->kind) {
    case PT_E_STREAM:
        on_stream(u, ev->ai);
        break;
    case PT_E_RETRY: {
        int64_t wait = ev->until - pt_sys_now_ms();
        fprintf(stderr, "%sretrying in %lld.%llds: %.*s%s\n", u->tty_err ? "\x1b[33m" : "",
                (long long)(wait > 0 ? wait / 1000 : 0), (long long)(wait > 0 ? wait % 1000 / 100 : 0),
                (int)ev->error.n, ev->error.p, u->tty_err ? "\x1b[0m" : "");
        break;
    }
    case PT_E_RUN_DONE:
        if (!pt_str_eqz(ev->outcome, "completed"))
            fprintf(stderr, "%serror:%s %.*s\n", u->tty_err ? "\x1b[31m" : "", u->tty_err ? "\x1b[0m" : "",
                    (int)ev->error.n, ev->error.p);
        break;
    default:
        break;
    }
}

static void print_json(ui *u, const char *id, const char *status)
{
    pt_buf b;
    pt_jw w;
    pt_buf_init(&b, pt_sys_allocator());
    pt_jw_init(&w, &b);
    pt_jw_obj(&w);
    pt_jw_key(&w, "session");
    pt_jw_strz(&w, id);
    pt_jw_key(&w, "status");
    pt_jw_strz(&w, status);
    pt_jw_key(&w, "result");
    pt_jw_str(&w, pt_buf_str(&u->result));
    if (u->error.n) {
        pt_jw_key(&w, "error");
        pt_jw_str(&w, pt_buf_str(&u->error));
    }
    pt_jw_obj_end(&w);
    pt_buf_putc(&b, '\n');
    fwrite(b.p, 1, b.n, stdout);
    out_flush(u);
    pt_buf_free(&b);
}

#define STDIN_MAX ((size_t)64 << 20)

/* All of stdin into b. False, with why in err, when it cannot be read
 * whole: a prompt is never sent cut short. Says so on stderr when stdin
 * stays silent, so an idle pipe is not a mystery hang. */
static bool read_stdin(pt_buf *b, char *err, size_t cap)
{
    struct stat st;
    struct pollfd pfd;
    if (fstat(STDIN_FILENO, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 &&
        (uint64_t)st.st_size <= STDIN_MAX)
        pt_buf_reserve(b, (size_t)st.st_size + 1);
    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 3000) == 0)
        fputs("pytte: waiting for stdin (--no-stdin runs without it)\n", stderr);
    for (;;) {
        ssize_t got;
        if (b->n == b->cap && !pt_buf_reserve(b, 65536)) {
            snprintf(err, cap, "out of memory reading stdin");
            return false;
        }
        got = read(STDIN_FILENO, b->p + b->n, b->cap - b->n);
        if (got < 0 && errno == EINTR)
            continue;
        if (got < 0) {
            snprintf(err, cap, "cannot read stdin: %s", strerror(errno));
            return false;
        }
        if (got == 0)
            return true;
        b->n += (size_t)got;
        if (b->n > STDIN_MAX) {
            snprintf(err, cap, "stdin is over %u MiB", (unsigned)(STDIN_MAX >> 20));
            return false;
        }
    }
}

/* Tool calls waiting for approval, with the command that answers each. */
static void print_asks(cli_session *s)
{
    pt_ask_view v[8];
    pt_arena ar;
    uint32_t n = pt_asks(s->a, v, 8), i;
    pt_arena_init(&ar, pt_sys_allocator(), 0);
    for (i = 0; i < n && i < 8; i++) {
        pt_str gist = pt_rt_gist(v[i].args, &ar);
        fprintf(stderr, "pytte: %.*s wants approval: %.*s\n", (int)v[i].name.n, v[i].name.p,
                (int)(gist.n > 300 ? 300 : gist.n), gist.p);
        fprintf(stderr, "  answer with: pytte approve %s %u yes|no\n", s->id, v[i].task);
    }
    pt_arena_free(&ar);
}

/* Run until idle; returns the exit code. */
static int print_mode(opts *o, const char *id, cli_config *cfg)
{
    cli_session s;
    ui u;
    pt_buf in_buf;
    pt_str prompt = PT_STR_NULL, data = PT_STR_NULL;
    char err[700];
    const char *status = "completed";
    int rc;
    memset(&u, 0, sizeof u);
    u.out = o->out;
    u.tty_err = isatty(STDERR_FILENO);
    u.tty_out = isatty(STDOUT_FILENO);
    u.text_nl = true;
    pt_buf_init(&u.held, pt_sys_allocator());
    pt_buf_init(&u.result, pt_sys_allocator());
    pt_buf_init(&u.error, pt_sys_allocator());
    pt_buf_init(&in_buf, pt_sys_allocator());
    /* Piped stdin is the prompt, or data that goes with it. */
    if (!o->cmd_approve && !o->no_stdin && !isatty(STDIN_FILENO)) {
        if (!read_stdin(&in_buf, err, sizeof err))
            die(err, NULL);
        data = pt_str_trim(pt_buf_str(&in_buf));
    }
    if (o->prompt)
        prompt = pt_strz(o->prompt);
    else {
        prompt = data;
        data = PT_STR_NULL;
    }
    if (!id && !prompt.n && !o->cmd_approve)
        die("no prompt (give one as an argument or on stdin)", NULL);
    apply_config(o, cfg, id == NULL);
    effort_note(o->c.model, o->c.reasoning);
    o->c.stop = should_stop;
    load_mcp(o, false);
    if ((rc = cli_session_open(&s, &o->c, id, on_event, &u, err, sizeof err)) != 0) {
        fprintf(stderr, "pytte: %s\n", err);
        return rc;
    }
    if (o->cmd_approve) {
        uint64_t task = 0;
        if (!pt_str_to_u64(pt_strz(o->task), &task) || task > UINT32_MAX ||
            !pt_approve(s.a, (uint32_t)task, strcmp(o->answer, "yes") == 0, PT_STR_NULL)) {
            fprintf(stderr, "pytte: task %s: %s\n", o->task, task ? pt_error(s.a) : "not a task number");
            cli_session_close(&s);
            return 1;
        }
    }
    if (prompt.n) {
        pt_input in;
        pt_buf text, joined;
        char in_dir[4200];
        pt_buf_init(&text, pt_sys_allocator());
        pt_buf_init(&joined, pt_sys_allocator());
        memset(&in, 0, sizeof in);
        in.text = prompt;
        switch (pt_rt_expand_skills(s.rt, prompt, &text, err, sizeof err)) {
        case -1:
            fprintf(stderr, "pytte: %s\n", err);
            snprintf(in_dir, sizeof in_dir, "%s", s.dir);
            cli_session_close(&s);
            if (!id) /* new, and nothing in it */
                cli_session_discard(in_dir);
            return 1;
        case 1:
            in.text = pt_buf_str(&text);
            break;
        }
        if (data.n) { /* the data first, then what to do with it */
            pt_buf_reserve(&joined, data.n + in.text.n + 32);
            pt_buf_putz(&joined, "<stdin>\n");
            pt_buf_puts(&joined, data);
            pt_buf_putz(&joined, "\n</stdin>\n\n");
            pt_buf_puts(&joined, in.text);
            in.text = pt_buf_str(&joined);
        }
        if (joined.oom || !pt_submit(s.a, &in)) {
            char dir[4200];
            fprintf(stderr, "pytte: %s\n", joined.oom ? "out of memory" : pt_error(s.a));
            snprintf(dir, sizeof dir, "%s", s.dir);
            cli_session_close(&s);
            if (!id)
                cli_session_discard(dir); /* discard a newly-created empty session */
            pt_buf_free(&text);
            pt_buf_free(&joined);
            return 1;
        }
        pt_buf_free(&text);
        pt_buf_free(&joined);
        cli_session_name_set(s.dir, prompt);
    }
    if (o->out == OUT_TEXT)
        fprintf(stderr, "%ssession %s%s\n", u.tty_err ? "\x1b[2m" : "", s.id, u.tty_err ? "\x1b[0m" : "");
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN); /* a closed stdout is an EPIPE out_flush sees */
    switch (pt_rt_run(s.rt)) {
    case PT_RT_IDLE:
        end_text(&u);
        cli_session_close(&s);
        rc = u.runs_failed ? 10 : 0;
        status = u.runs_failed ? "failed" : "completed";
        break;
    case PT_RT_STOPPED:
        end_text(&u);
        fprintf(stderr, "pytte: detached; continue with: pytte -r %s\n", s.id);
        cli_session_close(&s);
        rc = 130;
        status = "detached";
        break;
    case PT_RT_FATAL:
        end_text(&u);
        fprintf(stderr, "pytte: %s\n", pt_error(s.a));
        pt_buf_reset(&u.error);
        pt_buf_putz(&u.error, pt_error(s.a));
        if (pt_fatal(s.a) == PT_FATAL_CONFIG) {
            cli_session_close(&s); /* nothing is broken: a clean exit */
            rc = 1;
        } else {
            cli_session_free(&s);
            rc = 20;
        }
        status = "error";
        fprintf(stderr, "pytte: the session is safe up to its last commit; continue with: pytte -r %s\n", s.id);
        break;
    case PT_RT_ASK:
        end_text(&u);
        print_asks(&s);
        cli_session_close(&s);
        rc = 3;
        status = "waiting";
        break;
    default: /* STUCK; BUSY is not returned by pt_rt_run */
        fprintf(stderr, "pytte: internal error: the agent is stuck\n");
        cli_session_free(&s);
        rc = 20;
        status = "error";
        break;
    }
    if (o->out == OUT_JSON)
        print_json(&u, s.id, status);
    if (u.out_errno && (rc == 0 || rc == 130)) {
        /* An EPIPE is quiet, as a SIGPIPE would be; other failures are not. */
        if (u.out_errno != EPIPE)
            fprintf(stderr, "pytte: cannot write to stdout: %s\n", strerror(u.out_errno));
        rc = u.out_errno == EPIPE ? 141 : 10;
    }
    pt_buf_free(&u.held);
    pt_buf_free(&u.result);
    pt_buf_free(&u.error);
    pt_buf_free(&in_buf);
    return rc;
}

/* ---- log: read only; works while another process owns the session ---- */

static void print_entry(const pt_entry_view *v, bool tty)
{
    const pt_msg *m = v->msg;
    const char *dim = tty ? "\x1b[2m" : "", *reset = tty ? "\x1b[0m" : "";
    uint32_t i;
    if (strcmp(v->kind, "epoch") == 0) {
        printf("%s[epoch %u] %.*s%s\n\n", dim, v->epoch, (int)m->text.n, m->text.p, reset);
        if (v->summary.n)
            printf("%s[summary] %.*s%s\n\n", dim, (int)v->summary.n, v->summary.p, reset);
        if (v->files.n)
            printf("%s%.*s%s\n", dim, (int)v->files.n, v->files.p, reset);
    } else if (strcmp(v->kind, "user") == 0) {
        printf("%s> %.*s%s\n\n", tty ? "\x1b[1m" : "", (int)m->text.n, m->text.p, reset);
    } else if (strcmp(v->kind, "assistant") == 0) {
        if (m->stop == PT_STOP_ABORTED || m->stop == PT_STOP_ERROR)
            printf("%s[%s, not sent to the model]%s\n", dim, pt_stop_name(m->stop), reset);
        for (i = 0; i < m->nblocks; i++) {
            const pt_block *b = &m->blocks[i];
            if (b->kind == PT_BLOCK_TEXT)
                printf("%.*s\n", (int)b->text.n, b->text.p);
            else if (b->kind == PT_BLOCK_THINKING && b->text.n)
                printf("%s%.*s%s\n", dim, (int)b->text.n, b->text.p, reset);
            else if (b->kind == PT_BLOCK_TOOL_CALL)
                printf("-> %.*s %.*s\n", (int)b->name.n, b->name.p, (int)b->args.n, b->args.p);
        }
        printf("\n");
    } else if (strcmp(v->kind, "tool_result") == 0) {
        printf("%s<- %.*s%s: %.*s%s\n\n", dim, (int)m->name.n, m->name.p, m->is_error ? " (error)" : "",
               (int)m->text.n, m->text.p, reset);
    } else if (strcmp(v->kind, "error") == 0) {
        pt_arena ar;
        pt_json *j;
        pt_arena_init(&ar, pt_sys_allocator(), 0);
        j = pt_json_parse(v->raw, &ar, NULL);
        {
            const pt_json *d = pt_json_get(j, "data");
            pt_str msg = pt_json_gets(d, "message", &ar);
            printf("%s[error, attempt %lld] %.*s%s\n\n", dim, (long long)pt_json_geti(d, "attempt", 0),
                   (int)msg.n, msg.p, reset);
        }
        pt_arena_free(&ar);
    } else if (m) {
        printf("%s[%s] %.*s%s\n\n", dim, v->kind, (int)m->text.n, m->text.p, reset);
    }
}

#define LOG_CHUNK 65536

/* Load a log into a, a chunk at a time: memory for one chunk and the
 * longest line. 0, or 20 with the message printed. */
static int load_log(pt_agent *a, const char *id, const char *path)
{
    char *chunk = (char *)malloc(LOG_CHUNK);
    pt_buf line; /* a line that spans chunks */
    int fd = open(path, O_RDONLY | O_CLOEXEC), code = 0;
    struct stat st;
    uint64_t off = 0;
    bool torn = false;
    if (!chunk || fd < 0 || fstat(fd, &st) != 0) {
        fprintf(stderr, "pytte: cannot read %s: %s\n", path, chunk ? strerror(errno) : "out of memory");
        free(chunk);
        if (fd >= 0)
            close(fd);
        return 20;
    }
    pt_buf_init(&line, pt_sys_allocator());
    while (!torn && !code) {
        ssize_t got = read(fd, chunk, LOG_CHUNK);
        size_t i, from = 0;
        if (got < 0 && errno == EINTR)
            continue;
        if (got == 0)
            break;
        if (got < 0) {
            fprintf(stderr, "pytte: cannot read %s: %s\n", path, strerror(errno));
            code = 20;
            break;
        }
        for (i = 0; i < (size_t)got; i++) {
            pt_load_result r;
            pt_str l;
            if (chunk[i] != '\n')
                continue;
            if (line.n) {
                pt_buf_put(&line, chunk + from, i - from);
                l = pt_buf_str(&line);
            } else {
                l = (pt_str){chunk + from, i - from};
            }
            r = line.oom ? PT_LOAD_CORRUPT : pt_load_line(a, l, true, off + i + 1 >= (uint64_t)st.st_size);
            pt_buf_reset(&line);
            from = i + 1;
            if (r == PT_LOAD_CORRUPT) {
                fprintf(stderr, "pytte: %s: %s\n", id, line.oom ? "out of memory" : pt_error(a));
                code = 20;
                break;
            }
            if (r == PT_LOAD_TORN) { /* being written right now, or torn: not committed */
                torn = true;
                break;
            }
        }
        if (!torn && !code && from < (size_t)got)
            pt_buf_put(&line, chunk + from, (size_t)got - from);
        off += (uint64_t)got;
    }
    close(fd);
    free(chunk);
    pt_buf_free(&line);
    return code;
}

/* pt_opts.blob for a reader: blob_ud is the session directory. The
 * owner's lock is left alone, as for the log. */
static bool read_blob(void *dir, pt_str ref, pt_arena *ar, pt_str *out)
{
    char path[4300];
    char *p;
    size_t n, i;
    if (ref.n != 71 || memcmp(ref.p, "sha256:", 7) != 0)
        return false;
    for (i = 7; i < ref.n; i++)
        if (!((ref.p[i] >= '0' && ref.p[i] <= '9') || (ref.p[i] >= 'a' && ref.p[i] <= 'f')))
            return false;
    snprintf(path, sizeof path, "%s/blobs/%.2s/%.62s", (const char *)dir, ref.p + 7, ref.p + 9);
    if (!(p = pt_sys_read_file(path, &n)))
        return false;
    *out = pt_arena_dup(ar, (pt_str){p, n});
    free(p);
    return !ar->oom;
}

static int cmd_log(const opts *o)
{
    char root[4096], id[64], dir[4200], path[4300];
    pt_opts ao;
    pt_agent *a;
    pt_entry_view v;
    bool tty = isatty(STDOUT_FILENO);
    uint32_t i;
    int code;
    cli_sessions_dir(root, sizeof root);
    if (o->id)
        snprintf(id, sizeof id, "%s", o->id);
    else if (!cli_sessions_list(NULL, false, &id, 1))
        die("no sessions in", root);
    snprintf(dir, sizeof dir, "%s/%s", root, id);
    snprintf(path, sizeof path, "%s/log.jsonl", dir);
    memset(&ao, 0, sizeof ao);
    ao.alloc = pt_sys_allocator();
    ao.all_epochs = o->all_epochs;
    ao.blob = read_blob;
    ao.blob_ud = dir;
    a = pt_new(&ao);
    if (!a) {
        fprintf(stderr, "pytte: out of memory loading %s\n", path);
        return 20;
    }
    if ((code = load_log(a, id, path)) != 0) {
        pt_free(a);
        return code;
    }
    for (i = 0; pt_entry_at(a, i, &v); i++) {
        if (o->json) {
            fwrite(v.raw.p, 1, v.raw.n, stdout);
            fputc('\n', stdout);
        } else if (!(o->all_epochs && v.kept)) { /* printed as the original */
            print_entry(&v, tty);
        }
    }
    pt_free(a);
    return 0;
}

/* ---- ls ---- */

#define LS_PEEK 65536 /* ls reads this much of each log's head and tail */

/* The commit lines in p[0..n), whole lines only; fn returns false to stop. */
static void each_line(const char *p, size_t n, bool (*fn)(void *, pt_str), void *ud)
{
    const char *end = p + n, *nl;
    while (p < end && (nl = (const char *)memchr(p, '\n', (size_t)(end - p)))) {
        if (!fn(ud, (pt_str){p, (size_t)(nl - p)}))
            return;
        p = nl + 1;
    }
}

static bool contains(pt_str s, const char *needle)
{
    size_t n = strlen(needle), i;
    for (i = 0; i + n <= s.n; i++)
        if (s.p[i] == needle[0] && memcmp(s.p + i, needle, n) == 0)
            return true;
    return false;
}

typedef struct ls_row {
    pt_arena ar; /* reset per session */
    pt_str cwd, title, title_blob; /* title_blob: a large prompt's ref */
    const pt_json *claim;          /* the last claim seen */
} ls_row;

/* Head: the session record (cwd) and the first user entry (title). */
static bool ls_head(void *ud, pt_str line)
{
    ls_row *r = (ls_row *)ud;
    const pt_json *c;
    if (!contains(line, "\"t\":\"session\"") && !contains(line, "\"kind\":\"user\""))
        return true;
    c = pt_json_get(pt_json_parse(line, &r->ar, NULL), "w");
    for (c = c ? c->child : NULL; c; c = c->next) {
        pt_str t = pt_json_gets(c, "t", &r->ar);
        if (pt_str_eqz(t, "session"))
            r->cwd = pt_arena_dup(&r->ar, pt_json_gets(c, "cwd", &r->ar));
        else if (pt_str_eqz(t, "entry") && pt_str_eqz(pt_json_gets(c, "kind", &r->ar), "user") &&
                   !r->title.p) {
            const pt_json *content = pt_json_get(pt_json_get(c, "data"), "content");
            if (pt_json_is(content, PT_J_OBJ))
                r->title_blob = pt_json_gets(content, "blob", &r->ar);
            /* copied: the tail is read into the same buffer */
            r->title_blob = pt_arena_dup(&r->ar, r->title_blob);
            r->title = pt_arena_dup(&r->ar, pt_json_str(content, &r->ar));
        }
    }
    return !r->title.p;
}

/* Tail: the last claim (the tail starts mid-line; that line is skipped). */
static bool ls_tail(void *ud, pt_str line)
{
    ls_row *r = (ls_row *)ud;
    const pt_json *c;
    if (!contains(line, "\"t\":\"claim\""))
        return true;
    c = pt_json_get(pt_json_parse(line, &r->ar, NULL), "w");
    for (c = c ? c->child : NULL; c; c = c->next)
        if (pt_str_eqz(pt_json_gets(c, "t", &r->ar), "claim"))
            r->claim = c;
    return true;
}

/* From the last claim. Closed by its owner: idle (nothing left to do),
 * waiting (for a tool call's approval) or closed (a run is unfinished;
 * resuming continues it). running: an open claim of a live process on
 * this machine (the lock is left alone: taking it, even shared, could
 * refuse an owner starting that moment). crashed: an open claim of a
 * process that is gone. */
static void ls_status(const ls_row *r, char *out, size_t cap)
{
    char host[256], boot[64];
    int64_t me;
    pt_str event, h, b;
    pt_arena *ar = (pt_arena *)&r->ar;
    int64_t pid = pt_json_geti(r->claim, "pid", 0);
    if (!r->claim) {
        snprintf(out, cap, "crashed");
        return;
    }
    event = pt_json_gets(r->claim, "event", ar);
    if (pt_str_eqz(event, "close")) {
        pt_str state = pt_json_gets(r->claim, "state", ar);
        snprintf(out, cap, "%s", pt_str_eqz(state, "idle")      ? "idle"
                                 : pt_str_eqz(state, "waiting") ? "waiting"
                                                                : "closed");
        return;
    }
    pt_sys_identity(host, sizeof host, boot, sizeof boot, &me);
    h = pt_json_gets(r->claim, "host", ar);
    b = pt_json_gets(r->claim, "boot", ar);
    if (!pt_str_eqz(h, host))
        snprintf(out, cap, "open on %.*s", (int)h.n, h.p);
    else if (pt_str_eqz(b, boot) && pid > 0 && (kill((pid_t)pid, 0) == 0 || errno == EPERM))
        snprintf(out, cap, "running (pid %lld)", (long long)pid);
    else
        snprintf(out, cap, "crashed");
}

/* ---- pytte update-catalog ---- */

typedef struct fetch {
    pt_buf body;
    int status;
    bool done;
    char err[300];
} fetch;

static void fetch_head(void *ud, int status, pt_str retry_after)
{
    (void)retry_after;
    ((fetch *)ud)->status = status;
}

static void fetch_body(void *ud, const char *p, size_t n)
{
    fetch *f = (fetch *)ud;
    if (f->body.n + n > ((size_t)64 << 20)) /* models.dev is about 6 MiB */
        f->body.oom = true;
    else
        pt_buf_put(&f->body, p, n);
}

static void fetch_done(void *ud, const char *error)
{
    fetch *f = (fetch *)ud;
    f->done = true;
    if (error)
        snprintf(f->err, sizeof f->err, "%s", error);
}

/* GET url into f->body; false with f->err set. */
static bool fetch_url(pt_http_driver *d, const char *url, fetch *f)
{
    pt_header accept = {"Accept", "application/json"};
    pt_http_req req;
    pt_http_sink sink;
    memset(&req, 0, sizeof req);
    req.method = "GET";
    req.url = url;
    req.headers = &accept;
    req.nheaders = req.hcap = 1;
    sink.ud = f;
    sink.head = fetch_head;
    sink.body = fetch_body;
    sink.done = fetch_done;
    sink.header = NULL;
    if (!d->start(d, &req, &sink)) {
        snprintf(f->err, sizeof f->err, "cannot start the request");
        return false;
    }
    while (!f->done)
        if (d->pump(d, 1000, NULL, 0) < 0) {
            snprintf(f->err, sizeof f->err, "the HTTP driver failed");
            return false;
        }
    if (!f->err[0] && f->body.oom)
        snprintf(f->err, sizeof f->err, "response too large, or out of memory");
    else if (!f->err[0] && (f->status < 200 || f->status > 299))
        snprintf(f->err, sizeof f->err, "HTTP %d", f->status);
    return !f->err[0];
}

static int cmd_update_catalog(void)
{
    static const char *const urls[2] = {"https://models.dev/api.json", "https://openrouter.ai/api/v1/models"};
    pt_http_driver *d = pt_http_curl_new();
    fetch f[2];
    pt_buf out;
    char path[4096], msg[600], err[600], updated[32];
    time_t now = time(NULL);
    struct tm tm;
    int i, rc = 1;
    if (!d)
        die("cannot start libcurl", NULL);
    memset(f, 0, sizeof f);
    pt_buf_init(&out, pt_sys_allocator());
    for (i = 0; i < 2; i++) {
        pt_buf_init(&f[i].body, pt_sys_allocator());
        if (!fetch_url(d, urls[i], &f[i])) {
            fprintf(stderr, "pytte: %s: %s\n", urls[i], f[i].err);
            goto done;
        }
    }
    gmtime_r(&now, &tm);
    strftime(updated, sizeof updated, "%Y-%m-%dT%H:%M:%SZ", &tm);
    cli_catalog_path(path, sizeof path);
    if (!cli_catalog_convert(pt_buf_str(&f[0].body), pt_buf_str(&f[1].body), updated, &out, msg, sizeof msg)) {
        fprintf(stderr, "pytte: %s\n", msg);
    } else if (!cli_write_atomic(path, pt_buf_str(&out), err, sizeof err)) {
        fprintf(stderr, "pytte: %s\n", err);
    } else {
        printf("wrote %s: %s\n", path, msg);
        rc = 0;
    }
done:
    for (i = 0; i < 2; i++)
        pt_buf_free(&f[i].body);
    pt_buf_free(&out);
    d->free(d);
    return rc;
}

static int cmd_ls(void)
{
    char root[4096], path[4200], status[300], name[CLI_NAME_MAX + 1], *peek;
    char(*ids)[64];
    const char *home = getenv("HOME");
    size_t hn = home ? strlen(home) : 0;
    uint32_t n, i;
    ls_row r;
    pt_buf first; /* each log's first line, reused */
    cli_sessions_dir(root, sizeof root);
    ids = (char(*)[64])malloc(4096 * sizeof *ids);
    peek = (char *)malloc(LS_PEEK);
    if (!ids || !peek)
        die("out of memory", NULL);
    n = cli_sessions_list(NULL, false, ids, 4096);
    pt_arena_init(&r.ar, pt_sys_allocator(), 0);
    pt_buf_init(&first, pt_sys_allocator());
    for (i = 0; i < n; i++) {
        struct stat st;
        ssize_t got;
        int fd;
        size_t k;
        pt_str t;
        snprintf(path, sizeof path, "%s/%s/log.jsonl", root, ids[i]);
        if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0 || fstat(fd, &st) != 0) {
            if (fd >= 0)
                close(fd);
            continue;
        }
        pt_arena_reset(&r.ar);
        r.title_blob = PT_STR_NULL;
        r.cwd = r.title = PT_STR_NULL;
        r.claim = NULL;
        /* the head: the first line whole (however long its system prompt),
         * then up to LS_PEEK after it */
        if (cli_log_first_line(fd, &first)) {
            ls_head(&r, pt_buf_str(&first));
            got = pread(fd, peek, LS_PEEK, (off_t)first.n + 1);
            if (got > 0 && !r.title.p)
                each_line(peek, (size_t)got, ls_head, &r);
        }
        got = pread(fd, peek, LS_PEEK, st.st_size > LS_PEEK ? st.st_size - LS_PEEK : 0);
        if (got > 0 && st.st_size > LS_PEEK && (t.p = (const char *)memchr(peek, '\n', (size_t)got)))
            each_line(t.p + 1, (size_t)got - (size_t)(t.p + 1 - peek), ls_tail, &r);
        else if (got > 0 && st.st_size <= LS_PEEK)
            each_line(peek, (size_t)got, ls_tail, &r);
        close(fd);
        if (r.title_blob.n == 71) { /* a large prompt: its first bytes */
            char bp[4400];
            int bf;
            snprintf(bp, sizeof bp, "%s/%s/blobs/%.2s/%.62s", root, ids[i], r.title_blob.p + 7,
                     r.title_blob.p + 9);
            if ((bf = open(bp, O_RDONLY | O_CLOEXEC)) >= 0) {
                char *head = (char *)pt_arena_alloc(&r.ar, 256);
                got = head ? pread(bf, head, 256, 0) : -1;
                if (got > 0)
                    r.title = (pt_str){head, (size_t)got};
                close(bf);
            }
        }
        snprintf(path, sizeof path, "%s/%s", root, ids[i]);
        if (cli_session_name(path, name, sizeof name))
            r.title = pt_strz(name);
        ls_status(&r, status, sizeof status);
        /* the first line of the first prompt, cut on a character */
        t = r.title.p ? r.title : PT_S("(no prompts)");
        if (!r.title.p)
            r.title = t;
        for (k = 0; k < t.n && t.p[k] != '\n'; k++)
            ;
        t.n = k;
        if (t.n > 60) {
            t.n = 60;
            while (t.n && ((unsigned char)t.p[t.n] & 0xC0) == 0x80)
                t.n--;
        }
        if (home && hn > 1 && r.cwd.n >= hn && memcmp(r.cwd.p, home, hn) == 0 &&
            (r.cwd.n == hn || r.cwd.p[hn] == '/'))
            printf("%s  %-18s  ~%.*s  %.*s%s\n", ids[i], status, (int)(r.cwd.n - hn), r.cwd.p + hn, (int)t.n,
                   t.p, t.n < r.title.n ? "…" : "");
        else
            printf("%s  %-18s  %.*s  %.*s%s\n", ids[i], status, (int)r.cwd.n, r.cwd.p, (int)t.n, t.p,
                   t.n < r.title.n ? "…" : "");
    }
    pt_arena_free(&r.ar);
    pt_buf_free(&first);
    free(peek);
    free(ids);
    return 0;
}

/* ---- MCP servers ---- */

static cli_mcp mcp;

/* Print mode: what the servers say goes to stderr, but "ready". */
static void mcp_note(void *ud, const char *server, const char *msg)
{
    (void)ud;
    if (strncmp(msg, "ready:", 6) != 0)
        fprintf(stderr, "pytte: mcp %s: %s\n", server ? server : "", msg);
}

/* This directory's servers into o (unless --no-mcp), warning about those
 * waiting for trust. */
static void load_mcp(opts *o, bool quiet)
{
    char cwd[4096];
    if (o->no_mcp)
        return;
    cli_mcp_load(&mcp, getcwd(cwd, sizeof cwd) ? cwd : ".");
    if (mcp.warn[0])
        fprintf(stderr, "pytte: %s\n", mcp.warn);
    if (mcp.nuntrusted && !quiet)
        fprintf(stderr, "pytte: this project's MCP servers are not trusted, so they do not run; "
                        "`pytte mcp trust` runs them\n");
    o->c.mcp = mcp.run;
    o->c.nmcp = mcp.n;
    cli_mcp_host(&o->c.mcp_host, mcp_note, NULL);
}

static int64_t bare_now(void *ud)
{
    (void)ud;
    return pt_sys_now_ms();
}

typedef struct bare {
    pt_agent *a;
    pt_rt *rt;
    pt_store *st;
    pt_http_driver *http;
    bool login_ended;
} bare;

static void bare_note(void *ud, const char *server, const char *msg)
{
    bare *b = (bare *)ud;
    if (!strncmp(msg, "logged in", 9) || !strncmp(msg, "login failed", 12))
        b->login_ended = true;
    fprintf(stderr, "%s: %s\n", server ? server : "mcp", msg);
}

/* A runtime without a session, for servers only. */
static bool bare_open(bare *b, const pt_rt_mcp_server *srv, uint32_t n)
{
    pt_opts ao;
    pt_rt_cfg rc;
    pt_rt_mcp_host h;
    memset(b, 0, sizeof *b);
    memset(&ao, 0, sizeof ao);
    ao.alloc = pt_sys_allocator();
    ao.now_ms = bare_now;
    ao.random = pt_sys_random;
    ao.catalog = cli_catalog();
    memset(&rc, 0, sizeof rc);
    rc.store = b->st = pt_store_memory_new();
    rc.http = b->http = pt_http_curl_new();
    rc.catalog = cli_catalog();
    if (!b->st || !b->http || !(b->a = pt_new(&ao)) || !(b->rt = pt_rt_new(&rc, b->a)))
        return false;
    cli_mcp_host(&h, bare_note, b);
    return pt_rt_mcp_setup(b->rt, &h, srv, n);
}

static void bare_close(bare *b)
{
    if (b->rt)
        pt_rt_free(b->rt);
    if (b->a)
        pt_free(b->a);
    if (b->http)
        b->http->free(b->http);
    if (b->st)
        pt_store_memory_free(b->st);
}

/* Step until no server starts, or ms pass. */
static void bare_settle(bare *b, int64_t ms)
{
    int64_t end = pt_sys_now_ms() + ms;
    pt_rt_mcp_info in[CLI_MCP_MAX];
    while (pt_sys_now_ms() < end) {
        uint32_t i, n;
        bool starting = false;
        pt_rt_step(b->rt, 100, NULL, 0);
        n = pt_rt_mcp_servers(b->rt, in, CLI_MCP_MAX);
        for (i = 0; i < n && i < CLI_MCP_MAX; i++)
            starting = starting || in[i].state == PT_MCP_STARTING;
        if (!starting)
            return;
    }
}

static const char *state_name(pt_mcp_state st)
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

static const pt_rt_mcp_server *find_mcp(const char *name)
{
    uint32_t i;
    for (i = 0; i < mcp.n; i++)
        if (!strcmp(mcp.run[i].name, name))
            return &mcp.run[i];
    return NULL;
}

static int cmd_mcp(opts *o)
{
    const char *sub = o->mcp_args[0] ? o->mcp_args[0] : "list", *name = o->mcp_args[1];
    char err[700];
    bare b;
    int rc = 0;
    o->no_mcp = false;
    load_mcp(o, true);
    if (!strcmp(sub, "trust") || !strcmp(sub, "untrust")) {
        if (name)
            die("takes no name: it is about this directory's servers", name);
        if (!cli_mcp_trust(&mcp, sub[0] == 't', err, sizeof err))
            die(err, NULL);
        printf("%s this directory's MCP servers (%s)\n", sub[0] == 't' ? "trusted" : "no longer trusting",
               mcp.project);
        return 0;
    }
    if (!strcmp(sub, "list")) {
        pt_rt_mcp_info in[CLI_MCP_MAX];
        uint32_t i, k = 0;
        if (name)
            die("list takes no name", name);
        if (!mcp.nall) {
            printf("no MCP servers: add them to mcpServers in %s, or to .mcp.json in a project\n",
                   pt_strz(getenv("HOME")).n ? "~/.config/pytte/config.json" : "config.json");
            return 0;
        }
        if (mcp.n) {
            if (!bare_open(&b, mcp.run, mcp.n))
                die("out of memory", NULL);
            bare_settle(&b, 30000);
            pt_rt_mcp_servers(b.rt, in, CLI_MCP_MAX);
        }
        for (i = 0; i < mcp.nall; i++) {
            const pt_rt_mcp_server *s = &mcp.all[i];
            printf("%-20s %-5s %-18s %s\n", s->name, s->url ? "http" : "stdio", mcp.info[i].source,
                   s->url ? s->url : s->command);
            if (!mcp.info[i].runs) {
                printf("%20s not trusted: `pytte mcp trust` runs this project's servers\n", "");
                continue;
            }
            printf("%20s %s", "", state_name(in[k].state));
            if (in[k].state == PT_MCP_READY)
                printf(": %u tools, %u prompts, %u resources", in[k].ntools, in[k].nprompts, in[k].nresources);
            else if (in[k].error && in[k].error[0])
                printf(": %s", in[k].error);
            printf("\n");
            k++;
        }
        if (mcp.n)
            bare_close(&b);
        return 0;
    }
    if (strcmp(sub, "login") && strcmp(sub, "logout"))
        die("usage: pytte mcp [list|trust|untrust|login NAME|logout NAME]", NULL);
    if (!name)
        die("which server?", NULL);
    if (!find_mcp(name))
        die("no such MCP server (or not trusted)", name);
    if (!find_mcp(name)->url)
        die("not a remote server: only those log in", name);
    if (!bare_open(&b, find_mcp(name), 1))
        die("out of memory", NULL);
    if (!strcmp(sub, "logout")) {
        pt_rt_mcp_logout(b.rt, pt_strz(name));
        printf("logged out of %s\n", name);
    } else {
        pt_rt_mcp_info in;
        int64_t end;
        bare_settle(&b, 30000);
        pt_rt_mcp_servers(b.rt, &in, 1);
        if (in.state == PT_MCP_READY) {
            printf("%s works already: %u tools\n", name, in.ntools);
        } else {
            pt_rt_mcp_login(b.rt, pt_strz(name));
            end = pt_sys_now_ms() + 6 * 60 * 1000;
            while (!b.login_ended && pt_sys_now_ms() < end)
                pt_rt_step(b.rt, 100, NULL, 0);
            bare_settle(&b, 30000);
            pt_rt_mcp_servers(b.rt, &in, 1);
            if (in.state == PT_MCP_READY)
                printf("%s: logged in, %u tools\n", name, in.ntools);
            else {
                fprintf(stderr, "pytte: %s: %s\n", name, in.error && in.error[0] ? in.error : "the login failed");
                rc = 1;
            }
        }
    }
    bare_close(&b);
    return rc;
}

/* -r without an ID, in a terminal: pick from this directory's sessions. */
static bool pick_session(char *out, size_t cap)
{
    char ids[20][64], cwd[4096], line[64], root[4096];
    uint32_t n, i;
    cli_sessions_dir(root, sizeof root);
    unsigned long k;
    if (!getcwd(cwd, sizeof cwd) || !(n = cli_sessions_list(cwd, false, ids, 20)))
        return false;
    for (i = 0; i < n; i++) {
        char dir[4200], name[CLI_NAME_MAX + 1];
        snprintf(dir, sizeof dir, "%s/%s", root, ids[i]);
        cli_session_name(dir, name, sizeof name);
        fprintf(stderr, "%3u  %s  %s\n", i + 1, ids[i], name);
    }
    fprintf(stderr, "session [1]: ");
    if (!fgets(line, sizeof line, stdin))
        return false;
    k = line[0] == '\n' ? 1 : strtoul(line, NULL, 10);
    if (k < 1 || k > n)
        die("no such session", NULL);
    snprintf(out, cap, "%s", ids[k - 1]);
    return true;
}

int main(int argc, char **argv)
{
    opts o;
    cli_config cfg;
    char warn[600], picked[64], cwd[4096];
    const char *term = getenv("TERM");
    bool tty;
#ifdef M_ARENA_MAX
    /* One heap: a worker thread's first malloc would otherwise reserve an
     * arena of its own, resident for the life of the process. */
    mallopt(M_ARENA_MAX, 1);
#endif
    parse_args(argc, argv, &o);
    if (o.cmd_log)
        return cmd_log(&o);
    if (o.cmd_ls)
        return cmd_ls();
    if (o.cmd_approve) {
        cli_config_load(&cfg, warn, sizeof warn);
        return print_mode(&o, o.id, &cfg);
    }
    cli_config_load(&cfg, warn, sizeof warn);
    if (warn[0])
        fprintf(stderr, "pytte: %s\n", warn);
    if (o.cmd_update)
        return cmd_update_catalog();
    if (o.cmd_mcp)
        return cmd_mcp(&o);
    tty = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    if (o.c.worktree && (o.cont || o.resume))
        die("-w starts a new session: not with -c or -r", NULL);
    if (o.c.worktree && (o.print || !tty)) { /* the TUI's open goes in, and says so */
        char note[700], err[700];
        if (!cli_worktree_enter(o.c.worktree, note, sizeof note, err, sizeof err))
            die(err, NULL);
        fprintf(stderr, "pytte: %s\n", note);
        o.c.worktree = NULL;
    }
    if (o.cont && (o.print || !tty)) {
        if (!getcwd(cwd, sizeof cwd) || !cli_sessions_list(cwd, false, &picked, 1))
            die("no session to continue in this directory", NULL);
        o.id = picked;
    } else if (o.resume && !o.id) {
        if (o.print || !tty)
            die("--resume needs a session ID here", NULL);
        if (!pick_session(picked, sizeof picked))
            die("no sessions in this directory", NULL);
        o.id = picked;
    }
    if (o.print || !tty)
        return print_mode(&o, o.id, &cfg);
    if (term && strcmp(term, "dumb") == 0)
        die("TERM=dumb is not supported yet; use -p", NULL);
    o.c.continue_current = o.cont;
    apply_config(&o, &cfg, o.id == NULL && !o.cont);
    load_mcp(&o, true); /* the TUI says it, and asks */
    return tui_run(&o.c, &cfg, o.no_mcp ? NULL : &mcp, o.id, o.prompt);
}
