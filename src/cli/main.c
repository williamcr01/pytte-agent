/* pytte: the durable agent CLI (section 16.1).
 *
 *   pytte ["PROMPT"]            the TUI in a new session
 *   pytte -c / -r [ID]          the TUI on this directory's newest session / on ID
 *   pytte -p "PROMPT"           one-shot: run until idle and print the answer
 *   pytte log [ID] [--json]     print a session's transcript
 *
 * Kill the process at any point and `pytte -c` continues from its last
 * commit. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cli/cli.h"

static const char usage[] =
    "usage: pytte [options] [PROMPT]      the TUI in a new session\n"
    "       pytte -c | -r [ID] [PROMPT]   continue this directory's newest session, or ID\n"
    "       pytte -p [options] [PROMPT]   print mode: run until done, print the answer\n"
    "                                     (PROMPT from stdin when it is piped)\n"
    "       pytte log [ID] [--json]       print a session's transcript\n"
    "\n"
    "  -c, --continue          the newest session started in this directory\n"
    "  -r, --resume [ID]       that session, or pick from this directory's\n"
    "  -p, --print             no TUI (also when stdin or stdout is not a terminal)\n"
    "  -m, --model REF         provider/model-id (default: $PYTTE_MODEL, the config\n"
    "                          file; else the session's last, or " DEFAULT_MODEL ")\n"
    "  --effort LEVEL          off, minimal, low, medium, high, xhigh, max ($PYTTE_EFFORT)\n"
    "  --max-tokens N          default: the model's maximum that fits the context\n"
    "  --system-prompt TEXT    system prompt of a new session\n"
    "  --append-system-prompt TEXT\n"
    "  --output-format FMT     with -p: text (default), json (one object at the end),\n"
    "                          stream-json (every commit as a JSON line)\n"
    "  --script FILE           faux provider script (default: $PYTTE_FAUX_SCRIPT)\n"
    "  --no-sync               do not fsync commits (survives crashes, not power loss)\n"
    "  --force                 take over a session another host left open\n"
    "\n"
    "Exit codes: 0 done, 1 usage or setup, 10 the run failed, 20 storage error,\n"
    "130 interrupted (the session stays resumable).\n";

enum { OUT_TEXT, OUT_JSON, OUT_STREAM };

typedef struct opts {
    cli_opts c;
    const char *cmd_log, *id, *prompt;
    bool print, cont, resume, json, model_flag, effort_flag;
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
        } else if (strcmp(a, "--no-sync") == 0) {
            o->c.no_sync = true;
        } else if (strcmp(a, "--force") == 0) {
            o->c.force = true;
        } else if (TAKES("-m") || TAKES("--model")) {
            o->c.model = v;
            o->model_flag = true;
        } else if (TAKES("--system-prompt")) {
            o->c.system = v;
        } else if (TAKES("--append-system-prompt")) {
            o->c.append_system = v;
        } else if (TAKES("--script")) {
            o->c.script = v;
        } else if (TAKES("--effort")) {
            if (!pt_reasoning_parse(pt_strz(v), &o->c.reasoning))
                die("unknown effort level", v);
            o->effort_flag = true;
        } else if (TAKES("--output-format")) {
            if (strcmp(v, "text") == 0)
                o->out = OUT_TEXT;
            else if (strcmp(v, "json") == 0)
                o->out = OUT_JSON;
            else if (strcmp(v, "stream-json") == 0)
                o->out = OUT_STREAM;
            else
                die("unknown output format", v);
        } else if (TAKES("--max-tokens") || TAKES("--retry-base-ms")) {
            uint64_t n;
            if (!pt_str_to_u64(pt_strz(v), &n) || n == 0 || n > UINT32_MAX)
                die("bad number", v);
            if (strcmp(a, "--max-tokens") == 0)
                o->c.max_tokens = (uint32_t)n;
            else
                o->c.retry_base_ms = (uint32_t)n; /* for tests */
        } else if (strcmp(a, "--") == 0 && v) {
            o->prompt = argv[++i];
        } else if (a[0] == '-' && a[1]) {
            die("unknown option", a);
        } else if (i == 1 && strcmp(a, "log") == 0) {
            o->cmd_log = a;
        } else if (o->cmd_log && !o->id) {
            o->id = a;
        } else if (!o->prompt) {
            o->prompt = a;
        } else {
            die("too many arguments", a);
        }
#undef TAKES
    }
    if (!o->c.script)
        o->c.script = getenv("PYTTE_FAUX_SCRIPT");
}

/* Model and effort: a flag, else the environment; the config files only
 * set the defaults of new sessions (an existing one keeps its last). */
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
    env_or_new = create || strcmp(cfg->src[CFG_REASONING], "env") == 0;
    if (o->effort_flag) {
        cli_config_set(cfg, CFG_REASONING, pt_reasoning_name(o->c.reasoning), "flag");
    } else if (cfg->val[CFG_REASONING][0] && env_or_new) {
        if (!pt_reasoning_parse(pt_strz(cfg->val[CFG_REASONING]), &o->c.reasoning))
            fprintf(stderr, "pytte: unknown effort level %s (from %s); ignored\n",
                    cfg->val[CFG_REASONING], cfg->src[CFG_REASONING]);
    }
    if (o->c.model && *o->c.model) { /* before a session exists to leave behind */
        pt_arena a;
        pt_ai_target t;
        const char *why;
        bool ok;
        pt_arena_init(&a, pt_sys_allocator(), 0);
        ok = pt_ai_resolve(pt_ai_builtin_catalog(), pt_strz(o->c.model), &t, &a, &why);
        pt_arena_free(&a);
        if (!ok) {
            fprintf(stderr, "pytte: model %s (from %s): %s, e.g. %s\n", o->c.model,
                    o->c.model == cfg->val[CFG_MODEL] ? cfg->src[CFG_MODEL] : "flag", why, DEFAULT_MODEL);
            exit(1);
        }
    }
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
    if (!id && !prompt.n)
        die("no prompt (give one as an argument or on stdin)", NULL);
    apply_config(o, cfg, id == NULL);
    o->c.stop = should_stop;
    if ((rc = cli_session_open(&s, &o->c, id, on_event, &u, err, sizeof err)) != 0) {
        fprintf(stderr, "pytte: %s\n", err);
        return rc;
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

static int cmd_log(const opts *o)
{
    char root[4096], id[64], path[4200];
    char *src;
    size_t n, start = 0, i;
    pt_opts ao;
    pt_agent *a;
    pt_entry_view v;
    bool tty = isatty(STDOUT_FILENO);
    cli_sessions_dir(root, sizeof root);
    if (o->id)
        snprintf(id, sizeof id, "%s", o->id);
    else if (!cli_sessions_list(NULL, &id, 1))
        die("no sessions in", root);
    snprintf(path, sizeof path, "%s/%s/log.jsonl", root, id);
    src = pt_sys_read_file(path, &n);
    if (!src)
        die("cannot read", path);
    memset(&ao, 0, sizeof ao);
    ao.alloc = pt_sys_allocator();
    a = pt_new(&ao);
    for (i = 0; i < n; i++) {
        pt_load_result r;
        if (src[i] != '\n')
            continue;
        r = pt_load_line(a, (pt_str){src + start, i - start}, true, i + 1 == n);
        if (r == PT_LOAD_CORRUPT) {
            fprintf(stderr, "pytte: %s: %s\n", id, pt_error(a));
            return 20;
        }
        if (r == PT_LOAD_TORN) /* being written right now, or torn: not committed */
            break;
        start = i + 1;
    }
    for (i = 0; pt_entry_at(a, (uint32_t)i, &v); i++) {
        if (o->json) {
            fwrite(v.raw.p, 1, v.raw.n, stdout);
            fputc('\n', stdout);
        } else {
            print_entry(&v, tty);
        }
    }
    pt_free(a);
    free(src);
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
