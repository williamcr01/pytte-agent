/* pytte: the durable agent CLI.
 *
 *   pytte ask [options] [PROMPT]     new session, run until done
 *   pytte resume [ID] [options]      recover a session and continue it
 *   pytte log [ID] [--json]          print a session's transcript
 *
 * Sessions live in $XDG_STATE_HOME/pytte/sessions/<id>/ (by default
 * ~/.local/state/pytte/sessions). Kill the process at any point and
 * `pytte resume` continues from its last commit. */
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "rt/rt.h"

static const char usage[] =
    "usage: pytte ask [options] [PROMPT]   start a session (PROMPT from stdin if omitted)\n"
    "       pytte resume [ID] [options]    continue a session (default: the newest)\n"
    "       pytte log [ID] [--json]        print a session's transcript\n"
    "\n"
    "  --model REF         provider or provider/model-id (default: $PYTTE_MODEL; else the\n"
    "                      session's last model, or anthropic for a new one)\n"
    "  --reasoning LEVEL   off, minimal, low, medium, high, xhigh, max\n"
    "  --max-tokens N      default: the model's maximum that fits the context\n"
    "  --system TEXT       system prompt of a new session\n"
    "  --script FILE       faux provider script (default: $PYTTE_FAUX_SCRIPT)\n"
    "  --json              print each commit as a JSON line instead of the answer\n"
    "  --no-sync           do not fsync commits (survives crashes, not power loss)\n"
    "  --force             take over a session another host left open\n"
    "\n"
    "Exit codes: 0 done, 1 usage or setup, 10 the run failed, 20 storage error,\n"
    "130 interrupted (the session stays resumable).\n";

static const char default_system[] =
    "You are pytte, a small coding agent running in a terminal. Be concise.";

typedef struct opts {
    const char *cmd, *arg, *model, *system, *script;
    pt_reasoning reasoning;
    uint32_t max_tokens, retry_base_ms;
    bool json, no_sync, force;
} opts;

static void die(const char *msg, const char *arg)
{
    fprintf(stderr, "pytte: %s%s%s\n", msg, arg ? ": " : "", arg ? arg : "");
    exit(1);
}

static void parse_args(int argc, char **argv, opts *o)
{
    int i;
    memset(o, 0, sizeof o[0]);
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
#define TAKES(name) (strcmp(a, name) == 0 && (v || (die("missing value for", name), 0)) && ++i)
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0 || strcmp(a, "help") == 0) {
            fputs(usage, stdout);
            exit(0);
        } else if (strcmp(a, "--json") == 0) {
            o->json = true;
        } else if (strcmp(a, "--no-sync") == 0) {
            o->no_sync = true;
        } else if (strcmp(a, "--force") == 0) {
            o->force = true;
        } else if (TAKES("--model")) {
            o->model = v;
        } else if (TAKES("--system")) {
            o->system = v;
        } else if (TAKES("--script")) {
            o->script = v;
        } else if (TAKES("--reasoning")) {
            if (!pt_reasoning_parse(pt_strz(v), &o->reasoning))
                die("unknown reasoning level", v);
        } else if (TAKES("--max-tokens") || TAKES("--retry-base-ms")) {
            uint64_t n;
            if (!pt_str_to_u64(pt_strz(v), &n) || n == 0 || n > UINT32_MAX)
                die("bad number", v);
            if (strcmp(a, "--max-tokens") == 0)
                o->max_tokens = (uint32_t)n;
            else
                o->retry_base_ms = (uint32_t)n; /* for tests */
        } else if (a[0] == '-' && a[1]) {
            die("unknown option", a);
        } else if (!o->cmd) {
            o->cmd = a;
        } else if (!o->arg) {
            o->arg = a;
        } else {
            die("too many arguments", a);
        }
#undef TAKES
    }
    if (!o->model && getenv("PYTTE_MODEL") && *getenv("PYTTE_MODEL"))
        o->model = getenv("PYTTE_MODEL");
    if (!o->model && o->cmd && strcmp(o->cmd, "ask") == 0)
        o->model = "anthropic";
    if (!o->script)
        o->script = getenv("PYTTE_FAUX_SCRIPT");
}

/* ---- sessions on disk ---- */

static void sessions_dir(char *out, size_t cap)
{
    const char *st = getenv("XDG_STATE_HOME"), *home = getenv("HOME");
    if (st && *st)
        snprintf(out, cap, "%s/pytte/sessions", st);
    else
        snprintf(out, cap, "%s/.local/state/pytte/sessions", home ? home : ".");
}

static void mkdirs(const char *path)
{
    char tmp[4096];
    char *p;
    snprintf(tmp, sizeof tmp, "%s", path);
    for (p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0700);
            *p = '/';
        }
    if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
        die("cannot create", path);
}

/* YYYYMMDD-HHMMSS-xxxx: sorts by time. */
static void new_session_id(char *out, size_t cap)
{
    static const char b32[] = "abcdefghijklmnopqrstuvwxyz234567";
    unsigned char r[4];
    time_t now = time(NULL);
    struct tm tm;
    size_t n;
    int i;
    gmtime_r(&now, &tm);
    n = strftime(out, cap, "%Y%m%d-%H%M%S-", &tm);
    pt_sys_random(NULL, r, sizeof r);
    for (i = 0; i < 4 && n + 1 < cap; i++)
        out[n++] = b32[r[i] & 31];
    out[n] = 0;
}

static bool newest_session(const char *root, char *out, size_t cap)
{
    DIR *d = opendir(root);
    struct dirent *e;
    out[0] = 0;
    if (!d)
        return false;
    while ((e = readdir(d)))
        if (e->d_name[0] != '.' && strlen(e->d_name) == 20 && strcmp(e->d_name, out) > 0)
            snprintf(out, cap, "%s", e->d_name);
    closedir(d);
    return out[0] != 0;
}

/* ---- HTTP: faux:// to the faux driver, everything else to curl ---- */

typedef struct route {
    pt_http_driver *d;
    void *h;
    pt_http_sink sink;
} route;

typedef struct router {
    pt_http_driver vt;
    pt_http_driver *curl, *faux;
} router;

static void r_head(void *ud, int status, pt_str retry_after)
{
    route *r = (route *)ud;
    if (r->sink.head)
        r->sink.head(r->sink.ud, status, retry_after);
}

static void r_body(void *ud, const char *p, size_t n)
{
    route *r = (route *)ud;
    if (r->sink.body)
        r->sink.body(r->sink.ud, p, n);
}

static void r_done(void *ud, const char *error)
{
    route *r = (route *)ud;
    if (r->sink.done)
        r->sink.done(r->sink.ud, error);
    free(r);
}

static void *r_start(pt_http_driver *vd, const pt_http_req *req, const pt_http_sink *sink)
{
    router *d = (router *)vd;
    route *r;
    pt_http_sink mine;
    bool faux = strncmp(req->url, "faux:", 5) == 0;
    if (faux && !d->faux)
        return NULL; /* no script */
    if (!faux && !d->curl && !(d->curl = pt_http_curl_new()))
        return NULL;
    r = (route *)calloc(1, sizeof *r);
    if (!r)
        return NULL;
    r->d = faux ? d->faux : d->curl;
    r->sink = *sink;
    mine.ud = r;
    mine.head = r_head;
    mine.body = r_body;
    mine.done = r_done;
    r->h = r->d->start(r->d, req, &mine);
    if (!r->h) {
        free(r);
        return NULL;
    }
    return r;
}

static void r_cancel(pt_http_driver *vd, void *h)
{
    route *r = (route *)h;
    (void)vd;
    r->d->cancel(r->d, r->h);
    free(r);
}

static int r_pump(pt_http_driver *vd, int timeout_ms)
{
    router *d = (router *)vd;
    int n = 0, m;
    if (d->faux && (m = d->faux->pump(d->faux, timeout_ms)) > 0)
        n += m;
    if (d->curl) {
        m = d->curl->pump(d->curl, n ? 0 : timeout_ms);
        if (m < 0)
            return -1;
        n += m;
    }
    return n;
}

static void r_free(pt_http_driver *vd)
{
    router *d = (router *)vd;
    if (d->curl)
        d->curl->free(d->curl);
    if (d->faux)
        d->faux->free(d->faux);
    free(d);
}

static pt_http_driver *router_new(const char *script)
{
    router *d = (router *)calloc(1, sizeof *d);
    if (!d)
        die("out of memory", NULL);
    if (script) {
        size_t n;
        char *src = pt_sys_read_file(script, &n);
        if (!src)
            die("cannot read script", script);
        d->faux = pt_http_faux_new((pt_str){src, n});
        free(src);
    }
    d->vt.start = r_start;
    d->vt.cancel = r_cancel;
    d->vt.pump = r_pump;
    d->vt.free = r_free;
    return &d->vt;
}

/* ---- display ---- */

typedef struct ui {
    bool json, tty_err;
    bool text_nl, any_text, thinking_out;
    int runs_failed, runs_done;
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
    if (u->json) {
        if (ev->kind == PT_E_COMMITTED) {
            fwrite(ev->line.p, 1, ev->line.n, stdout);
            fputc('\n', stdout);
            fflush(stdout);
        }
        if (ev->kind == PT_E_RUN_DONE)
            pt_str_eqz(ev->outcome, "completed") ? u->runs_done++ : u->runs_failed++;
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
        if (pt_str_eqz(ev->outcome, "completed")) {
            u->runs_done++;
        } else {
            u->runs_failed++;
            fprintf(stderr, "%serror:%s %.*s\n", u->tty_err ? "\x1b[31m" : "", u->tty_err ? "\x1b[0m" : "",
                    (int)ev->error.n, ev->error.p);
        }
        break;
    default:
        break;
    }
}

/* ---- running a session ---- */

static int64_t sys_now(void *ud)
{
    (void)ud;
    return pt_sys_now_ms();
}

static bool credentials(void *ud, const pt_provider *p, pt_credentials *out)
{
    const char *which;
    (void)ud;
    return pt_ai_find_key(p, pt_sys_env, NULL, out, &which);
}

typedef struct session {
    char id[64], dir[4200];
    pt_agent *a;
    pt_rt *rt;
    ui u;
} session;

static void start(session *s, const opts *o, bool create)
{
    char root[4096], err[600];
    pt_opts ao;
    pt_rt_cfg rc;
    pt_store *st;
    sessions_dir(root, sizeof root);
    if (create) {
        mkdirs(root);
        new_session_id(s->id, sizeof s->id);
    } else if (o->arg) {
        snprintf(s->id, sizeof s->id, "%s", o->arg);
    } else if (!newest_session(root, s->id, sizeof s->id)) {
        die("no sessions in", root);
    }
    snprintf(s->dir, sizeof s->dir, "%s/%s", root, s->id);
    st = pt_store_posix_open(s->dir, create, err, sizeof err);
    if (!st) {
        fprintf(stderr, "pytte: %s\n", err);
        exit(1);
    }
    memset(&s->u, 0, sizeof s->u);
    s->u.json = o->json;
    s->u.tty_err = isatty(STDERR_FILENO);
    s->u.text_nl = true;
    memset(&ao, 0, sizeof ao);
    ao.alloc = pt_sys_allocator();
    ao.now_ms = sys_now;
    ao.random = pt_sys_random;
    ao.credentials = credentials;
    ao.on_event = on_event;
    ao.ud = &s->u;
    ao.config.model = o->model ? pt_strz(o->model) : PT_STR_NULL;
    ao.config.reasoning = o->reasoning;
    ao.config.max_tokens = o->max_tokens;
    ao.config.retry_base_ms = o->retry_base_ms;
    ao.config.no_sync = o->no_sync;
    s->a = pt_new(&ao);
    memset(&rc, 0, sizeof rc);
    rc.store = st;
    rc.http = router_new(o->script);
    rc.stop = should_stop;
    s->rt = s->a ? pt_rt_new(&rc, s->a) : NULL;
    if (!s->rt)
        die("out of memory", NULL);
}

static void finish_http(session *s, pt_http_driver *http)
{
    pt_rt_free(s->rt);
    pt_free(s->a);
    http->free(http);
}

/* Run until idle; returns the exit code. */
static int run(session *s, pt_http_driver *http)
{
    pt_rt_result r;
    int rc = 0;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    r = pt_rt_run(s->rt);
    end_text(&s->u);
    switch (r) {
    case PT_RT_IDLE:
        pt_rt_close(s->rt);
        rc = s->u.runs_failed ? 10 : 0;
        break;
    case PT_RT_STOPPED:
        pt_rt_close(s->rt);
        fprintf(stderr, "pytte: detached; continue with: pytte resume %s\n", s->id);
        rc = 130;
        break;
    case PT_RT_FATAL:
        fprintf(stderr, "pytte: %s\n", pt_error(s->a));
        if (pt_fatal(s->a) == PT_FATAL_CONFIG) {
            pt_rt_close(s->rt); /* nothing is broken: a clean exit */
            rc = 1;
        } else {
            rc = 20;
        }
        fprintf(stderr, "pytte: the session is safe up to its last commit; continue with: pytte resume %s\n",
                s->id);
        break;
    case PT_RT_STUCK:
        fprintf(stderr, "pytte: internal error: the agent is stuck\n");
        rc = 20;
        break;
    }
    finish_http(s, http);
    return rc;
}

static pt_str read_stdin(void)
{
    pt_buf b;
    char tmp[4096];
    size_t got;
    pt_buf_init(&b, pt_sys_allocator());
    while ((got = fread(tmp, 1, sizeof tmp, stdin)) > 0)
        pt_buf_put(&b, tmp, got);
    return pt_str_trim(pt_buf_str(&b)); /* lives for the run */
}

static void identity(pt_identity *me, char *host, size_t hcap, char *boot, size_t bcap)
{
    memset(me, 0, sizeof *me);
    pt_sys_identity(host, hcap, boot, bcap, &me->pid);
    me->host = pt_strz(host);
    me->boot = pt_strz(boot);
}

static int cmd_ask(const opts *o)
{
    session s;
    pt_session_cfg c;
    pt_identity me;
    pt_input in;
    char host[256], boot[64], cwd[4096];
    pt_str prompt;
    if (o->arg)
        prompt = pt_strz(o->arg);
    else if (!isatty(STDIN_FILENO))
        prompt = read_stdin();
    else
        die("no prompt (give one as an argument or on stdin)", NULL);
    if (!prompt.n)
        die("empty prompt", NULL);
    start(&s, o, true);
    identity(&me, host, sizeof host, boot, sizeof boot);
    memset(&c, 0, sizeof c);
    c.cwd = pt_strz(getcwd(cwd, sizeof cwd) ? cwd : ".");
    c.host = me.host;
    c.system = pt_strz(o->system ? o->system : default_system);
    memset(&in, 0, sizeof in);
    in.text = prompt;
    in.req = PT_S("ask");
    if (!pt_create(s.a, &c) || !pt_open(s.a, &me, false) || !pt_submit(s.a, &in))
        die(pt_error(s.a), NULL);
    if (!o->json)
        fprintf(stderr, "%ssession %s%s\n", s.u.tty_err ? "\x1b[2m" : "", s.id, s.u.tty_err ? "\x1b[0m" : "");
    return run(&s, pt_rt_http(s.rt));
}

static int cmd_resume(const opts *o)
{
    session s;
    pt_identity me;
    char host[256], boot[64], err[600];
    start(&s, o, false);
    if (!pt_rt_recover(s.rt, err, sizeof err)) {
        pt_http_driver *http = pt_rt_http(s.rt);
        fprintf(stderr, "pytte: session %s: %s\n", s.id, err);
        finish_http(&s, http);
        return 20;
    }
    identity(&me, host, sizeof host, boot, sizeof boot);
    if (!pt_open(s.a, &me, o->force)) {
        pt_http_driver *http = pt_rt_http(s.rt);
        fprintf(stderr, "pytte: session %s: %s\n", s.id, pt_error(s.a));
        finish_http(&s, http);
        return 1;
    }
    if (!o->json)
        fprintf(stderr, "%sresuming %s%s\n", s.u.tty_err ? "\x1b[2m" : "", s.id, s.u.tty_err ? "\x1b[0m" : "");
    return run(&s, pt_rt_http(s.rt));
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
    sessions_dir(root, sizeof root);
    if (o->arg)
        snprintf(id, sizeof id, "%s", o->arg);
    else if (!newest_session(root, id, sizeof id))
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

int main(int argc, char **argv)
{
    opts o;
    parse_args(argc, argv, &o);
    if (!o.cmd) {
        fputs(usage, stderr);
        return 1;
    }
    if (strcmp(o.cmd, "ask") == 0)
        return cmd_ask(&o);
    if (strcmp(o.cmd, "resume") == 0)
        return cmd_resume(&o);
    if (strcmp(o.cmd, "log") == 0)
        return cmd_log(&o);
    die("unknown command", o.cmd);
    return 1;
}
