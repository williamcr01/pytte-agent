/* Opening sessions, for the CLI and the TUI. Sessions live in
 * $XDG_STATE_HOME/pytte/sessions/<id>/ (by default ~/.local/state/pytte/sessions).
 * HTTP goes through a router: faux:// URLs to the faux driver (tests),
 * everything else to curl. */
#define _POSIX_C_SOURCE 200809L
#include "cli/cli.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct cli_open_job {
    cli_session *s;
    cli_opts o;
    pt_worker *worker;
    pt_buf args;
    const char *id;
    cli_session *previous; /* in args; owned exclusively after begin */
    pt_str previous_draft;
    pt_event_fn on_event;
    void *ud;
    int code;
    bool active, history, discard_previous;
    char err[700];
    char *history_text, *draft;
    size_t history_n, draft_n;
};

static void job_free(cli_open_job *job);

const char cli_default_system[] =
    "You are pytte, a small coding agent running in a terminal. Be concise. Use the tools to look "
    "before you answer about files, and read a file before editing it. Prefer small, reviewable "
    "changes.";

void cli_sessions_dir(char *out, size_t cap)
{
    const char *st = getenv("XDG_STATE_HOME"), *home = getenv("HOME");
    if (st && *st)
        snprintf(out, cap, "%s/pytte/sessions", st);
    else
        snprintf(out, cap, "%s/.local/state/pytte/sessions", home ? home : ".");
}

static bool mkdirs(const char *path)
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
    return mkdir(tmp, 0700) == 0 || errno == EEXIST;
}

/* YYYYMMDD-HHMMSS-xxxx: sorts by time. The first x is the time within the
 * second (in 32 ms steps), the other three are random. */
static void new_session_id(char *out, size_t cap)
{
    static const char b32[] = "234567abcdefghijklmnopqrstuvwxyz"; /* in ASCII order */
    unsigned char r[3];
    struct timespec ts;
    struct tm tm;
    size_t n;
    int i;
    clock_gettime(CLOCK_REALTIME, &ts);
    gmtime_r(&ts.tv_sec, &tm);
    n = strftime(out, cap, "%Y%m%d-%H%M%S-", &tm);
    pt_sys_random(NULL, r, sizeof r);
    if (n + 5 > cap)
        return;
    out[n++] = b32[ts.tv_nsec / 1000000 / 32];
    for (i = 0; i < 3; i++)
        out[n++] = b32[r[i] & 31];
    out[n] = 0;
}

bool cli_session_cwd(const char *id, char *out, size_t cap)
{
    char root[4096], path[4200], line[8192];
    FILE *f;
    size_t n;
    pt_arena ar;
    pt_json *j;
    bool ok = false;
    cli_sessions_dir(root, sizeof root);
    snprintf(path, sizeof path, "%s/%s/log.jsonl", root, id);
    if (!(f = fopen(path, "rb")))
        return false;
    n = fread(line, 1, sizeof line - 1, f);
    fclose(f);
    line[n] = 0;
    if (!strchr(line, '\n'))
        return false;
    *strchr(line, '\n') = 0;
    pt_arena_init(&ar, pt_sys_allocator(), 0);
    if ((j = pt_json_parse(pt_strz(line), &ar, NULL))) {
        const pt_json *w = pt_json_get(j, "w"), *r;
        for (r = w ? w->child : NULL; r; r = r->next)
            if (pt_str_eqz(pt_json_gets(r, "t", &ar), "session")) {
                pt_str cwd = pt_json_gets(r, "cwd", &ar);
                snprintf(out, cap, "%.*s", (int)cwd.n, cwd.p);
                ok = true;
                break;
            }
    }
    pt_arena_free(&ar);
    return ok;
}

static int newer_first(const void *x, const void *y)
{
    return -strcmp((const char *)x, (const char *)y);
}

uint32_t cli_sessions_list(const char *cwd, char (*ids)[64], uint32_t max)
{
    char root[4096], c[4096];
    char(*all)[64] = NULL;
    uint32_t n = 0, cap = 0, i, out = 0;
    DIR *d;
    struct dirent *e;
    cli_sessions_dir(root, sizeof root);
    if (!(d = opendir(root)))
        return 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || strlen(e->d_name) != 20)
            continue;
        if (n == cap) {
            char(*na)[64] = (char(*)[64])realloc(all, (cap = cap ? cap * 2 : 64) * sizeof *all);
            if (!na)
                break;
            all = na;
        }
        snprintf(all[n++], 64, "%.63s", e->d_name);
    }
    closedir(d);
    if (n)
        qsort(all, n, sizeof *all, newer_first);
    for (i = 0; i < n && out < max; i++)
        if (!cwd || (cli_session_cwd(all[i], c, sizeof c) && strcmp(c, cwd) == 0))
            memcpy(ids[out++], all[i], 64);
    free(all);
    return out;
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

static int r_pump(pt_http_driver *vd, int timeout_ms, pt_waitfd *fds, unsigned nfds)
{
    router *d = (router *)vd;
    int n = 0, m;
    if (d->faux && (m = d->faux->pump(d->faux, d->curl ? 0 : timeout_ms, d->curl ? NULL : fds,
                                      d->curl ? 0 : nfds)) > 0)
        n += m;
    if (d->curl) {
        m = d->curl->pump(d->curl, n ? 0 : timeout_ms, fds, nfds);
        if (m < 0)
            return -1;
        n += m;
    } else if (!d->faux && nfds) {
        pt_sys_wait(fds, nfds, timeout_ms);
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

static pt_http_driver *router_new(const char *script, char *err, size_t cap)
{
    router *d = (router *)calloc(1, sizeof *d);
    if (!d) {
        snprintf(err, cap, "out of memory");
        return NULL;
    }
    if (script) {
        size_t n;
        char *src = pt_sys_read_file(script, &n);
        if (!src) {
            snprintf(err, cap, "cannot read script: %s", script);
            free(d);
            return NULL;
        }
        d->faux = pt_http_faux_new((pt_str){src, n});
        free(src);
    }
    d->vt.start = r_start;
    d->vt.cancel = r_cancel;
    d->vt.pump = r_pump;
    d->vt.free = r_free;
    return &d->vt;
}

/* ---- sessions ---- */

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

void cli_session_free(cli_session *s)
{
    cli_open_job *background = s->background;
    pt_rt_free(s->rt);
    pt_free(s->a);
    if (s->http)
        s->http->free(s->http);
    s->rt = NULL;
    s->a = NULL;
    s->http = NULL;
    s->background = NULL;
    job_free(background);
}

void cli_session_close(cli_session *s)
{
    if (s->rt)
        pt_rt_close(s->rt);
    cli_session_free(s);
}

void cli_session_discard(const char *dir)
{
    static const char *const files[] = {"log.jsonl", "LOCK", "draft"};
    char p[4300];
    size_t i;
    for (i = 0; i < sizeof files / sizeof files[0]; i++) {
        snprintf(p, sizeof p, "%s/%s", dir, files[i]);
        unlink(p);
    }
    snprintf(p, sizeof p, "%s/live", dir);
    rmdir(p);
    rmdir(dir);
}

static int fail(cli_session *s, int code, char *err, size_t cap, const char *what)
{
    if (what)
        snprintf(err, cap, "%s", what);
    cli_session_free(s);
    return code;
}

int cli_session_open(cli_session *s, const cli_opts *o, const char *id, pt_event_fn on_event,
                     void *ud, char *err, size_t cap)
{
    char root[4096], cwd[4096];
    pt_opts ao;
    pt_rt_cfg rc;
    pt_store *st;
    pt_identity me;
    bool create = id == NULL;
    memset(s, 0, sizeof *s);
    cli_sessions_dir(root, sizeof root);
    if (create) {
        if (!mkdirs(root)) {
            snprintf(err, cap, "cannot create %s: %s", root, strerror(errno));
            return 1;
        }
        new_session_id(s->id, sizeof s->id);
    } else {
        snprintf(s->id, sizeof s->id, "%s", id);
    }
    snprintf(s->dir, sizeof s->dir, "%s/%s", root, s->id);
    if (!(s->http = router_new(o->script, err, cap)))
        return 1;
    if (!(st = pt_store_posix_open(s->dir, create, err, cap)))
        return fail(s, 1, err, cap, NULL);
    memset(&ao, 0, sizeof ao);
    ao.alloc = pt_sys_allocator();
    ao.now_ms = sys_now;
    ao.random = pt_sys_random;
    ao.credentials = credentials;
    ao.on_event = on_event;
    ao.ud = ud;
    ao.blob = pt_rt_read_blob;
    ao.blob_ud = st;
    ao.config.model = o->model ? pt_strz(o->model) : PT_STR_NULL;
    ao.config.reasoning = o->reasoning;
    ao.config.max_tokens = o->max_tokens;
    ao.config.retry_base_ms = o->retry_base_ms;
    ao.config.context_window = o->context_window;
    ao.config.no_sync = o->no_sync;
    memset(&rc, 0, sizeof rc);
    rc.store = st;
    rc.http = s->http;
    rc.stop = o->stop;
    if (!(s->a = pt_new(&ao)) || !(s->rt = pt_rt_new(&rc, s->a))) {
        st->close(st);
        return fail(s, 1, err, cap, "out of memory");
    }
    if (!pt_rt_add_builtins(s->rt)) /* the store is the runtime's now */
        return fail(s, 1, err, cap, "out of memory");
    pt_rt_set_perm(s->rt, o->perm);
    memset(&me, 0, sizeof me);
    pt_sys_identity(s->host, sizeof s->host, s->boot, sizeof s->boot, &me.pid);
    me.host = pt_strz(s->host);
    me.boot = pt_strz(s->boot);
    if (create) {
        pt_session_cfg c;
        pt_buf sys;
        bool ok;
        pt_buf_init(&sys, pt_sys_allocator());
        pt_buf_putz(&sys, o->system ? o->system : cli_default_system);
        if (o->append_system) {
            pt_buf_putz(&sys, "\n\n");
            pt_buf_putz(&sys, o->append_system);
        }
        memset(&c, 0, sizeof c);
        c.cwd = pt_strz(getcwd(cwd, sizeof cwd) ? cwd : ".");
        c.host = me.host;
        c.system = pt_buf_str(&sys);
        c.tools = pt_rt_tool_defs(s->rt, &c.ntools);
        ok = pt_create(s->a, &c);
        pt_buf_free(&sys);
        if (!ok)
            return fail(s, 1, err, cap, pt_error(s->a));
    } else {
        char why[600];
        if (!pt_rt_recover(s->rt, why, sizeof why)) {
            snprintf(err, cap, "session %s: %s", s->id, why);
            return fail(s, 20, err, cap, NULL);
        }
    }
    if (!pt_open(s->a, &me, o->force)) {
        snprintf(err, cap, "session %s: %s", s->id, pt_error(s->a));
        return fail(s, 1, err, cap, NULL);
    }
    return 0;
}

static void job_event(void *ud, const pt_event *ev)
{
    cli_open_job *job = (cli_open_job *)ud;
    /* Setup/recovery cannot call into the UI from the worker. Once setup
     * is published, every core call is back on the host thread. */
    if (job->active && job->on_event)
        job->on_event(job->ud, ev);
}

static void job_open(void *ud)
{
    cli_open_job *job = (cli_open_job *)ud;
    char path[4300], picked[64], cwd[4096];
    const char *id = job->id;
    if (job->previous) {
        cli_session *s = job->previous;
        snprintf(path, sizeof path, "%s/draft", s->dir);
        if (job->previous_draft.n)
            pt_sys_write_file(path, job->previous_draft.p, job->previous_draft.n);
        else
            unlink(path);
        if (pt_fatal(s->a) == PT_FATAL_STORAGE)
            cli_session_free(s);
        else
            cli_session_close(s);
        if (job->discard_previous)
            cli_session_discard(s->dir);
        job->previous = NULL;
    }
    if (!id && job->o.continue_current) {
        if (!getcwd(cwd, sizeof cwd) || !cli_sessions_list(cwd, &picked, 1)) {
            snprintf(job->err, sizeof job->err, "no session to continue in this directory");
            job->code = 1;
            return;
        }
        id = picked;
    }
    job->code = cli_session_open(job->s, &job->o, id, job_event, job,
                                 job->err, sizeof job->err);
    if (job->code)
        return;
    if (id) {
        snprintf(path, sizeof path, "%s/draft", job->s->dir);
        job->draft = pt_sys_read_file(path, &job->draft_n);
    }
    if (job->history) {
        char root[4096], *slash;
        size_t i, lines = 0;
        cli_sessions_dir(root, sizeof root);
        if ((slash = strrchr(root, '/')))
            *slash = 0;
        snprintf(path, sizeof path, "%s/history", root);
        job->history_text = pt_sys_read_file(path, &job->history_n);
        for (i = 0; i < job->history_n; i++)
            lines += job->history_text[i] == '\n';
        if (lines > 2000) {
            size_t at = 0, skip = lines - 1000;
            char why[600];
            while (skip && at < job->history_n)
                skip -= job->history_text[at++] == '\n';
            cli_write_atomic(path, (pt_str){job->history_text + at, job->history_n - at}, why, sizeof why);
        }
    }
}

static void job_free(cli_open_job *job)
{
    if (!job)
        return;
    pt_worker_free(job->worker);
    pt_buf_free(&job->args);
    free(job->history_text);
    free(job->draft);
    free(job);
}

cli_open_job *cli_session_begin(cli_session *s, const cli_opts *o, const char *id,
                                bool history, pt_str draft, bool discard,
                                pt_event_fn on_event, void *ud,
                                char *err, size_t cap)
{
    cli_open_job *job;
    const char *values[5];
    size_t total = s->a ? sizeof(cli_session) : 0, i;
    if (draft.n > SIZE_MAX - total) {
        snprintf(err, cap, "session draft is too large");
        return NULL;
    }
    total += draft.n;
    values[0] = o->model;
    values[1] = o->system;
    values[2] = o->append_system;
    values[3] = o->script;
    values[4] = id;
    for (i = 0; i < 5; i++)
        if (values[i]) {
            size_t n = strlen(values[i]) + 1;
            if (n > SIZE_MAX - total) {
                snprintf(err, cap, "session options are too large");
                return NULL;
            }
            total += n;
        }
    job = (cli_open_job *)calloc(1, sizeof *job);
    if (!job) {
        snprintf(err, cap, "out of memory");
        return NULL;
    }
    pt_buf_init(&job->args, pt_sys_allocator());
    if (!pt_buf_reserve(&job->args, total))
        goto fail;
    if (s->a) {
        job->previous = (cli_session *)job->args.p;
        pt_buf_put(&job->args, s, sizeof *s);
    }
    job->previous_draft = (pt_str){draft.n ? job->args.p + job->args.n : NULL, draft.n};
    pt_buf_puts(&job->args, draft);
    for (i = 0; i < 5; i++)
        if (values[i]) {
            const char *v = job->args.p + job->args.n;
            pt_buf_put(&job->args, values[i], strlen(values[i]) + 1);
            values[i] = v;
        }
    job->o = *o;
    job->o.model = values[0];
    job->o.system = values[1];
    job->o.append_system = values[2];
    job->o.script = values[3];
    job->id = values[4];
    job->s = s;
    job->history = history;
    job->discard_previous = discard;
    job->on_event = on_event;
    job->ud = ud;
    job->worker = pt_worker_new();
    if (!job->worker)
        goto fail;
    if (job->previous) {
        if (s->background) {
            s->background->active = false;
            s->background->s = job->previous;
        }
        memset(s, 0, sizeof *s);
    }
    if (!pt_worker_start(job->worker, job_open, job)) {
        if (job->previous) {
            *s = *job->previous;
            if (s->background) {
                s->background->s = s;
                s->background->active = true;
            }
        }
        goto fail;
    }
    return job;
fail:
    snprintf(err, cap, "could not allocate the session I/O worker");
    job_free(job);
    return NULL;
}

int cli_session_job_fd(const cli_open_job *job) { return pt_worker_fd(job->worker); }

bool cli_session_finish(cli_open_job *job, int *code, char *err, size_t cap)
{
    if (!pt_worker_finish(job->worker))
        return false;
    *code = job->code;
    if (job->code) {
        snprintf(err, cap, "%s", job->err);
        job_free(job);
    } else {
        job->active = true;
        job->s->background = job;
        pt_rt_use_worker(job->s->rt, job->worker);
        pt_buf_free(&job->args); /* options are owned by core/driver now */
    }
    return true;
}

void cli_session_cancel(cli_open_job *job)
{
    while (!pt_worker_finish(job->worker)) {
        pt_waitfd fd;
        fd.fd = pt_worker_fd(job->worker);
        pt_sys_wait(&fd, 1, -1);
    }
    if (!job->code)
        cli_session_close(job->s);
    job_free(job);
}

char *cli_session_take_history(cli_session *s, size_t *n)
{
    cli_open_job *job = s->background;
    char *text = job ? job->history_text : NULL;
    *n = job ? job->history_n : 0;
    if (job) {
        job->history_text = NULL;
        job->history_n = 0;
    }
    return text;
}

char *cli_session_take_draft(cli_session *s, size_t *n)
{
    cli_open_job *job = s->background;
    char *text = job ? job->draft : NULL;
    *n = job ? job->draft_n : 0;
    if (job) {
        job->draft = NULL;
        job->draft_n = 0;
    }
    return text;
}
