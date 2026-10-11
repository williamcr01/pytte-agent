/* pytte: the durable agent CLI (section 16.1).
 *
 *   pytte ["PROMPT"]            the TUI in a new session
 *   pytte -c / -r [ID]          the TUI on this directory's newest session / on ID
 *   pytte -p "PROMPT"           one-shot: run until idle and print the answer
 *   pytte log [ID] [--json]     print a session's transcript
 *   pytte approve ID TASK yes|no   answer a tool call waiting for approval
 *   pytte ls                    list sessions with their status
 *
 * Kill the process at any point and `pytte -c` continues from its last
 * commit. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cli/cli.h"

static const char usage[] =
    "usage: pytte [options] [PROMPT]      the TUI in a new session\n"
    "       pytte -c | -r [ID] [PROMPT]   continue this directory's newest session, or ID\n"
    "       pytte -p [options] [PROMPT]   print mode: run until done, print the answer\n"
    "                                     (PROMPT from stdin when it is piped)\n"
    "       pytte log [ID] [--json] [--all-epochs]\n"
    "                                     print a session's transcript (by default\n"
    "                                     since the last compaction)\n"
    "       pytte approve ID TASK yes|no  answer a tool call waiting for approval,\n"
    "                                     then go on as -p would\n"
    "       pytte ls                      list sessions, newest first: status, directory\n"
    "                                     and first prompt\n"
    "\n"
    "  -c, --continue          the newest session started in this directory\n"
    "  -r, --resume [ID]       that session, or pick from this directory's\n"
    "  -p, --print             no TUI (also when stdin or stdout is not a terminal)\n"
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
    "  --no-sync               do not fsync commits (survives crashes, not power loss)\n"
    "  --force                 take over a session another host left open\n"
    "\n"
    "Exit codes: 0 done, 1 usage or setup, 3 waiting for approval (-p), 10 the run\n"
    "failed, 20 storage error, "
    "130 interrupted (the session stays resumable).\n";

enum { OUT_TEXT, OUT_JSON, OUT_STREAM };

typedef struct opts {
    cli_opts c;
    const char *cmd_log, *cmd_approve, *cmd_ls, *id, *prompt, *task, *answer;
    bool print, cont, resume, json, all_epochs, model_flag, compact_flag, effort_flag, perm_flag;
    int out;
} opts;

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
        } else if (strcmp(a, "--json") == 0) { /* log */
            o->json = true;
        } else if (strcmp(a, "--all-epochs") == 0) { /* log */
            o->all_epochs = true;
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
        } else if (o->cmd_ls) {
            die("ls takes no arguments", a);
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
    ok = pt_ai_resolve(pt_ai_builtin_catalog(), pt_strz(ref), &t, &a, &why);
    pt_arena_free(&a);
    if (!ok) {
        fprintf(stderr, "pytte: model %s (from %s): %s, e.g. %s\n", ref, src, why, DEFAULT_MODEL);
        exit(1);
    }
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
    if (o->compact_flag)
        cli_config_set(cfg, CFG_COMPACT_MODEL, o->c.compact_model, "flag");
    else if (cfg->val[CFG_COMPACT_MODEL][0])
        o->c.compact_model = cfg->val[CFG_COMPACT_MODEL];
    check_model(o->c.model, o->c.model == cfg->val[CFG_MODEL] ? cfg->src[CFG_MODEL] : "flag");
    check_model(o->c.compact_model, cfg->src[CFG_COMPACT_MODEL]);
}

/* ---- print mode ---- */

typedef struct ui {
    int out;
    bool tty_err;
    bool text_nl, any_text, thinking_out;
    int runs_failed, runs_done;
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

static void end_text(ui *u)
{
    if (u->any_text && !u->text_nl)
        fputc('\n', stdout);
    u->any_text = false;
    u->text_nl = true;
    fflush(stdout);
}

static void on_stream(ui *u, const pt_ai_event *ev)
{
    const pt_msg *m = ev->msg;
    switch (ev->kind) {
    case PT_EV_THINKING_DELTA:
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
        fwrite(ev->delta.p, 1, ev->delta.n, stdout);
        u->text_nl = ev->delta.p[ev->delta.n - 1] == '\n';
        u->any_text = true;
        fflush(stdout);
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
            fflush(stdout);
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

static void print_json(const ui *u, const char *id, const char *status)
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
    pt_buf_free(&b);
}

static pt_str read_stdin(pt_buf *b)
{
    char tmp[4096];
    size_t got;
    while ((got = fread(tmp, 1, sizeof tmp, stdin)) > 0)
        pt_buf_put(b, tmp, got);
    return pt_str_trim(pt_buf_str(b));
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
    pt_str prompt = PT_STR_NULL;
    char err[700];
    const char *status = "completed";
    int rc;
    memset(&u, 0, sizeof u);
    u.out = o->out;
    u.tty_err = isatty(STDERR_FILENO);
    u.text_nl = true;
    pt_buf_init(&u.result, pt_sys_allocator());
    pt_buf_init(&u.error, pt_sys_allocator());
    pt_buf_init(&in_buf, pt_sys_allocator());
    if (o->prompt)
        prompt = pt_strz(o->prompt);
    else if (!isatty(STDIN_FILENO))
        prompt = read_stdin(&in_buf);
    if (!id && !prompt.n && !o->cmd_approve)
        die("no prompt (give one as an argument or on stdin)", NULL);
    apply_config(o, cfg, id == NULL);
    o->c.stop = should_stop;
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
        memset(&in, 0, sizeof in);
        in.text = prompt;
        if (!pt_submit(s.a, &in))
            die(pt_error(s.a), NULL);
    }
    if (o->out == OUT_TEXT)
        fprintf(stderr, "%ssession %s%s\n", u.tty_err ? "\x1b[2m" : "", s.id, u.tty_err ? "\x1b[0m" : "");
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
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
        if (got <= 0)
            break;
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
    else if (!cli_sessions_list(NULL, &id, 1))
        die("no sessions in", root);
    snprintf(dir, sizeof dir, "%s/%s", root, id);
    snprintf(path, sizeof path, "%s/log.jsonl", dir);
    memset(&ao, 0, sizeof ao);
    ao.alloc = pt_sys_allocator();
    ao.all_epochs = o->all_epochs;
    ao.blob = read_blob;
    ao.blob_ud = dir;
    a = pt_new(&ao);
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
            r->cwd = pt_json_gets(c, "cwd", &r->ar);
        else if (pt_str_eqz(t, "entry") && pt_str_eqz(pt_json_gets(c, "kind", &r->ar), "user") &&
                   !r->title.p) {
            const pt_json *content = pt_json_get(pt_json_get(c, "data"), "content");
            if (pt_json_is(content, PT_J_OBJ))
                r->title_blob = pt_json_gets(content, "blob", &r->ar);
            r->title = pt_json_str(content, &r->ar);
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

static int cmd_ls(void)
{
    char root[4096], path[4200], status[300], *peek;
    char(*ids)[64];
    const char *home = getenv("HOME");
    size_t hn = home ? strlen(home) : 0;
    uint32_t n, i;
    ls_row r;
    cli_sessions_dir(root, sizeof root);
    ids = (char(*)[64])malloc(4096 * sizeof *ids);
    peek = (char *)malloc(LS_PEEK);
    if (!ids || !peek)
        die("out of memory", NULL);
    n = cli_sessions_list(NULL, ids, 4096);
    pt_arena_init(&r.ar, pt_sys_allocator(), 0);
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
        got = pread(fd, peek, LS_PEEK, 0);
        if (got > 0)
            each_line(peek, (size_t)got, ls_head, &r);
        if (st.st_size > LS_PEEK) {
            got = pread(fd, peek, LS_PEEK, st.st_size - LS_PEEK);
            if (got > 0 && (t.p = (const char *)memchr(peek, '\n', (size_t)got)))
                each_line(t.p + 1, (size_t)got - (size_t)(t.p + 1 - peek), ls_tail, &r);
        } else if (got > 0) {
            each_line(peek, (size_t)got, ls_tail, &r);
        }
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
    free(peek);
    free(ids);
    return 0;
}

/* -r without an ID, in a terminal: pick from this directory's sessions. */
static bool pick_session(char *out, size_t cap)
{
    char ids[20][64], cwd[4096], line[64];
    uint32_t n, i;
    unsigned long k;
    if (!getcwd(cwd, sizeof cwd) || !(n = cli_sessions_list(cwd, ids, 20)))
        return false;
    for (i = 0; i < n; i++)
        fprintf(stderr, "%3u  %s\n", i + 1, ids[i]);
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
    tty = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    if (o.cont && (o.print || !tty)) {
        if (!getcwd(cwd, sizeof cwd) || !cli_sessions_list(cwd, &picked, 1))
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
    return tui_run(&o.c, &cfg, o.id, o.prompt);
}
