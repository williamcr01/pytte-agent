/* Opening sessions, for the CLI and the TUI. Sessions live in
 * $XDG_STATE_HOME/pytte/sessions/<id>/ (by default ~/.local/state/pytte/sessions).
 * HTTP goes through a router: faux:// URLs to the faux driver (tests),
 * everything else to curl. */
#define _POSIX_C_SOURCE 200809L
#include "cli/cli.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
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
    char err[700], note[700];
    char forked[64]; /* the fork's ID */
    char *history_text, *draft;
    size_t history_n, draft_n;
};

static void job_free(cli_open_job *job);

const char cli_default_system[] =
    "You are an expert assistant operating inside pytte, an agent harness. You help users with their "
    "requests with the tools you have at your disposal.\n\n"
    "Make independent tool calls together, in one response: calls that may run alongside others "
    "(bash, read, grep, find, ls) then run at the same time. Make a call that needs another's result "
    "in a later response.";

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

/* YYYYMMDD-HHMMSS-xxxx: sorts by time. The first two x are the millisecond
 * (in 32 ms steps, then the ms within the step), the other two are random:
 * -c and log pick the newest by ID, and sessions start ms apart. Two in the
 * same millisecond collide 1 time in 1024, and then creating one fails. */
static void new_session_id(char *out, size_t cap)
{
    static const char b32[] = "234567abcdefghijklmnopqrstuvwxyz"; /* in ASCII order */
    unsigned char r[2];
    struct timespec ts;
    struct tm tm;
    size_t n;
    long ms;
    clock_gettime(CLOCK_REALTIME, &ts);
    gmtime_r(&ts.tv_sec, &tm);
    n = strftime(out, cap, "%Y%m%d-%H%M%S-", &tm);
    pt_sys_random(NULL, r, sizeof r);
    if (n + 5 > cap)
        return;
    ms = ts.tv_nsec / 1000000;
    out[n++] = b32[ms / 32];
    out[n++] = b32[ms % 32];
    out[n++] = b32[r[0] & 31];
    out[n++] = b32[r[1] & 31];
    out[n] = 0;
}

bool cli_log_first_line(int fd, pt_buf *line)
{
    enum { CHUNK = 65536, MAX = 16 << 20 };
    pt_buf_reset(line);
    while (line->n < MAX && pt_buf_reserve(line, CHUNK)) {
        ssize_t got = pread(fd, line->p + line->n, CHUNK, (off_t)line->n);
        const char *nl = got > 0 ? (const char *)memchr(line->p + line->n, '\n', (size_t)got) : NULL;
        if (got <= 0)
            return false;
        if (nl) {
            line->n = (size_t)(nl - line->p);
            return true;
        }
        line->n += (size_t)got;
    }
    return false;
}

bool cli_session_cwd(const char *id, char *out, size_t cap)
{
    char root[4096], path[4200];
    int fd;
    pt_buf line;
    pt_arena ar;
    pt_json *j;
    bool ok = false;
    cli_sessions_dir(root, sizeof root);
    snprintf(path, sizeof path, "%s/%s/log.jsonl", root, id);
    if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0)
        return false;
    pt_buf_init(&line, pt_sys_allocator());
    pt_arena_init(&ar, pt_sys_allocator(), 0);
    if (cli_log_first_line(fd, &line) && (j = pt_json_parse(pt_buf_str(&line), &ar, NULL))) {
        const pt_json *w = pt_json_get(j, "w"), *r;
        for (r = w ? w->child : NULL; r; r = r->next)
            if (pt_str_eqz(pt_json_gets(r, "t", &ar), "session")) {
                pt_str cwd = pt_json_gets(r, "cwd", &ar);
                snprintf(out, cap, "%.*s", (int)cwd.n, cwd.p);
                ok = true;
                break;
            }
    }
    close(fd);
    pt_arena_free(&ar);
    pt_buf_free(&line);
    return ok;
}

static int newer_first(const void *x, const void *y)
{
    return -strcmp((const char *)x, (const char *)y);
}

uint32_t cli_sessions_list(const char *cwd, bool worktrees, char (*ids)[64], uint32_t max)
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
        if (!cwd || (cli_session_cwd(all[i], c, sizeof c) &&
                     (worktrees ? cli_session_cwd_under(c, cwd) : strcmp(c, cwd) == 0)))
            memcpy(ids[out++], all[i], 64);
    free(all);
    return out;
}

bool cli_session_cwd_under(const char *cwd, const char *base)
{
    size_t n = strlen(base);
    return strcmp(cwd, base) == 0 ||
           (strncmp(cwd, base, n) == 0 && strncmp(cwd + n, "/.pytte/worktrees/", 18) == 0 && cwd[n + 18]);
}

/* ---- names ---- */

/* The first line that is not blank, without control characters, cut on
 * a character to fit CLI_NAME_MAX bytes. */
static size_t name_of(pt_str text, char *out, size_t cap)
{
    size_t i = 0, n = 0, max = cap - 1 < CLI_NAME_MAX ? cap - 1 : CLI_NAME_MAX;
    while (i < text.n && (text.p[i] == ' ' || text.p[i] == '\t' || text.p[i] == '\n' || text.p[i] == '\r'))
        i++;
    for (; i < text.n && text.p[i] != '\n' && n < max; i++) {
        unsigned char c = (unsigned char)text.p[i];
        out[n++] = c < 0x20 || c == 0x7f ? ' ' : (char)c;
    }
    if (i < text.n && ((unsigned char)text.p[i] & 0xC0) == 0x80) { /* cut inside a character: drop it */
        while (n && ((unsigned char)out[n - 1] & 0xC0) == 0x80)
            n--;
        if (n)
            n--;
    }
    while (n && (out[n - 1] == ' ' || out[n - 1] == '\r'))
        n--;
    out[n] = 0;
    return n;
}

bool cli_session_name(const char *dir, char *out, size_t cap)
{
    char path[4300], buf[CLI_NAME_MAX + 8];
    ssize_t got;
    int fd;
    out[0] = 0;
    snprintf(path, sizeof path, "%s/name", dir);
    if ((fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)) < 0)
        return false;
    got = read(fd, buf, sizeof buf);
    close(fd);
    return got > 0 && name_of((pt_str){buf, (size_t)got}, out, cap) > 0;
}

void cli_session_name_set(const char *dir, pt_str prompt)
{
    char path[4300], name[CLI_NAME_MAX + 2];
    size_t n = name_of(prompt, name, sizeof name - 1);
    int fd;
    if (!n)
        return;
    name[n++] = '\n';
    snprintf(path, sizeof path, "%s/name", dir);
    /* the first prompt names it: a name already there stays */
    if ((fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600)) < 0)
        return;
    if (write(fd, name, n) != (ssize_t)n)
        unlink(path);
    close(fd);
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

static void r_header(void *ud, pt_str name, pt_str value)
{
    route *r = (route *)ud;
    if (r->sink.header)
        r->sink.header(r->sink.ud, name, value);
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
    mine.header = r_header;
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
    const char *which, *key;
    (void)ud;
    if (pt_ai_find_key(p, pt_sys_env, NULL, out, &which))
        return true;
    if (!(key = cli_auth_key(p->id)))
        return false;
    out->api_key = pt_strz(key);
    return true;
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
    static const char *const files[] = {"log.jsonl", "LOCK", "draft", "name"};
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
    ao.catalog = cli_catalog();
    ao.on_event = on_event;
    ao.ud = ud;
    ao.blob = pt_rt_read_blob;
    ao.blob_ud = st;
    ao.config.model = o->model ? pt_strz(o->model) : PT_STR_NULL;
    ao.config.compact_model = o->compact_model ? pt_strz(o->compact_model) : PT_STR_NULL;
    ao.config.reasoning = o->reasoning;
    ao.config.max_tokens = o->max_tokens;
    ao.config.retry_base_ms = o->retry_base_ms;
    ao.config.context_window = o->context_window;
    ao.config.no_sync = o->no_sync;
    memset(&rc, 0, sizeof rc);
    rc.store = st;
    rc.http = s->http;
    rc.stop = o->stop;
    rc.catalog = cli_catalog();
    if (!(s->a = pt_new(&ao)) || !(s->rt = pt_rt_new(&rc, s->a))) {
        st->close(st);
        return fail(s, 1, err, cap, "out of memory");
    }
    if (!pt_rt_add_builtins(s->rt)) /* the store is the runtime's now */
        return fail(s, 1, err, cap, "out of memory");
    pt_rt_set_perm(s->rt, o->perm);
    if (o->nmcp && !pt_rt_mcp_setup(s->rt, &o->mcp_host, o->mcp, o->nmcp))
        return fail(s, 1, err, cap, "out of memory");
    {
        pt_rt_prompt pr;
        char conf[4096];
        cli_config_dir(conf, sizeof conf);
        pr.base = cli_default_system;
        pr.home = getenv("HOME");
        pr.config_dir = conf;
        pr.skills_auto = !o->manual_skills;
        if (!pt_rt_set_prompt(s->rt, &pr))
            return fail(s, 1, err, cap, "out of memory");
    }
    memset(&me, 0, sizeof me);
    pt_sys_identity(s->host, sizeof s->host, s->boot, sizeof s->boot, &me.pid);
    me.host = pt_strz(s->host);
    me.boot = pt_strz(s->boot);
    if (create) {
        pt_session_cfg c;
        pt_buf sys, skills, prompt;
        pt_jw w;
        bool ok;
        pt_buf_init(&sys, pt_sys_allocator());
        pt_buf_init(&skills, pt_sys_allocator());
        pt_buf_init(&prompt, pt_sys_allocator());
        /* The flags are the session's own: kept to rebuild later epochs. */
        if (o->system || o->append_system) {
            pt_jw_init(&w, &prompt);
            pt_jw_obj(&w);
            if (o->system) {
                pt_jw_key(&w, "system");
                pt_jw_strz(&w, o->system);
            }
            if (o->append_system) {
                pt_jw_key(&w, "append");
                pt_jw_strz(&w, o->append_system);
            }
            pt_jw_obj_end(&w);
        }
        memset(&c, 0, sizeof c);
        c.cwd = pt_strz(getcwd(cwd, sizeof cwd) ? cwd : ".");
        c.host = me.host;
        c.prompt = pt_buf_str(&prompt);
        ok = !prompt.oom && pt_rt_build_prompt(s->rt, c.cwd, c.prompt, &sys, &skills);
        c.system = pt_buf_str(&sys);
        c.skills = pt_buf_str(&skills);
        c.tools = pt_rt_tool_defs(s->rt, &c.ntools);
        c.provisional = o->nmcp > 0; /* settled once the servers listed their tools */
        ok = ok && pt_create(s->a, &c);
        pt_buf_free(&sys);
        pt_buf_free(&skills);
        pt_buf_free(&prompt);
        if (!ok)
            return fail(s, 1, err, cap, pt_error(s->a)[0] ? pt_error(s->a) : "out of memory");
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

/* ---- /fork: a copy of a closed session ---- */

static bool sync_dir(const char *dir)
{
    int fd = open(dir, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    bool ok = fd >= 0 && fsync(fd) == 0;
    if (fd >= 0)
        close(fd);
    return ok;
}

/* The whole file, synced. A file that is not there is copied as nothing
 * unless it must be. */
static bool copy_file(const char *from, const char *to, bool must)
{
    char buf[16384];
    int in = open(from, O_RDONLY | O_CLOEXEC | O_NOFOLLOW), out;
    bool ok = true;
    if (in < 0)
        return !must && errno == ENOENT;
    if ((out = open(to, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600)) < 0) {
        close(in);
        return false;
    }
    while (ok) {
        ssize_t n = read(in, buf, sizeof buf), at = 0;
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            ok = n == 0;
            break;
        }
        while (ok && at < n) {
            ssize_t w = write(out, buf + at, (size_t)(n - at));
            if (w < 0 && errno != EINTR)
                ok = false;
            else if (w > 0)
                at += w;
        }
    }
    ok = ok && fsync(out) == 0;
    ok = close(out) == 0 && ok;
    close(in);
    return ok;
}

/* blobs/ab/cdef...: written once and never changed, so shared by links
 * (copied where the file system cannot link). blobs/tmp is not copied. */
static bool copy_blobs(const char *from, const char *to)
{
    char src[4700], dst[4700];
    DIR *top, *sub;
    struct dirent *e, *f;
    bool ok = true;
    snprintf(src, sizeof src, "%s/blobs", from);
    if (!(top = opendir(src)))
        return errno == ENOENT;
    snprintf(dst, sizeof dst, "%s/blobs", to);
    if (mkdir(dst, 0700) != 0) {
        closedir(top);
        return false;
    }
    while (ok && (e = readdir(top))) {
        if (strlen(e->d_name) != 2 || e->d_name[0] == '.')
            continue;
        snprintf(src, sizeof src, "%.4096s/blobs/%.255s", from, e->d_name);
        snprintf(dst, sizeof dst, "%.4096s/blobs/%.255s", to, e->d_name);
        if (!(sub = opendir(src)) || mkdir(dst, 0700) != 0) {
            ok = false;
            if (sub)
                closedir(sub);
            break;
        }
        while (ok && (f = readdir(sub))) {
            if (f->d_name[0] == '.')
                continue;
            snprintf(src, sizeof src, "%.4096s/blobs/%.255s/%.255s", from, e->d_name, f->d_name);
            snprintf(dst, sizeof dst, "%.4096s/blobs/%.255s/%.255s", to, e->d_name, f->d_name);
            ok = link(src, dst) == 0 || copy_file(src, dst, true);
        }
        closedir(sub);
        snprintf(dst, sizeof dst, "%.4096s/blobs/%.255s", to, e->d_name);
        ok = ok && sync_dir(dst);
    }
    closedir(top);
    snprintf(dst, sizeof dst, "%s/blobs", to);
    return ok && sync_dir(dst);
}

/* Remove a fork that could not be made: blobs/, then the rest. */
static void remove_fork(const char *dir)
{
    char p[4700];
    DIR *top, *sub;
    struct dirent *e, *f;
    snprintf(p, sizeof p, "%s/blobs", dir);
    if ((top = opendir(p))) {
        while ((e = readdir(top))) {
            if (e->d_name[0] == '.')
                continue;
            snprintf(p, sizeof p, "%.4096s/blobs/%.255s", dir, e->d_name);
            if ((sub = opendir(p))) {
                while ((f = readdir(sub)))
                    if (f->d_name[0] != '.') {
                        snprintf(p, sizeof p, "%.4096s/blobs/%.255s/%.255s", dir, e->d_name, f->d_name);
                        unlink(p);
                    }
                closedir(sub);
            }
            snprintf(p, sizeof p, "%.4096s/blobs/%.255s", dir, e->d_name);
            rmdir(p);
        }
        closedir(top);
        snprintf(p, sizeof p, "%s/blobs", dir);
        rmdir(p);
    }
    cli_session_discard(dir);
}

/* Copy the closed session in from to a new one (job->forked): its log,
 * blobs, name and draft. The copy's log is the original's, so it opens
 * as the original would, with every entry and setting. */
static bool fork_session(cli_open_job *job, const char *from)
{
    static const char *const files[] = {"name", "draft", "log.jsonl"}; /* the log last */
    char root[4096], to[4200], a[4400], b[4400];
    size_t i;
    bool ok;
    cli_sessions_dir(root, sizeof root);
    new_session_id(job->forked, sizeof job->forked);
    snprintf(to, sizeof to, "%s/%s", root, job->forked);
    if (mkdir(to, 0700) != 0) {
        snprintf(job->err, sizeof job->err, "cannot create %.400s: %s", to, strerror(errno));
        return false;
    }
    ok = copy_blobs(from, to);
    for (i = 0; ok && i < sizeof files / sizeof files[0]; i++) {
        snprintf(a, sizeof a, "%s/%s", from, files[i]);
        snprintf(b, sizeof b, "%s/%s", to, files[i]);
        ok = copy_file(a, b, i == 2);
    }
    ok = ok && sync_dir(to) && sync_dir(root);
    if (!ok) {
        snprintf(job->err, sizeof job->err, "cannot fork the session: %s", strerror(errno));
        remove_fork(to);
    }
    return ok;
}

/* ---- /worktree ---- */

extern char **environ;

/* git with args, in the working directory: its exit status (-1: it did
 * not run), and the start of what it printed (both streams) in out. */
static int git(const char *const *argv, char *out, size_t cap)
{
    posix_spawn_file_actions_t fa;
    int p[2], st = -1;
    pid_t pid;
    size_t n = 0;
    out[0] = 0;
    if (pipe(p) != 0)
        return -1;
    fcntl(p[0], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, p[1], 1);
    posix_spawn_file_actions_adddup2(&fa, p[1], 2);
    posix_spawn_file_actions_addclose(&fa, p[1]);
    if (posix_spawnp(&pid, "git", &fa, NULL, (char *const *)argv, environ) != 0)
        pid = -1;
    posix_spawn_file_actions_destroy(&fa);
    close(p[1]);
    for (;;) { /* to the end: git must not block on a full pipe */
        char buf[1024];
        ssize_t got = read(p[0], buf, sizeof buf);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            break;
        if (n + 1 < cap) {
            size_t take = (size_t)got < cap - 1 - n ? (size_t)got : cap - 1 - n;
            memcpy(out + n, buf, take);
            n += take;
        }
    }
    close(p[0]);
    out[n] = 0;
    if (pid < 0)
        return -1;
    while (waitpid(pid, &st, 0) < 0)
        if (errno != EINTR)
            return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

bool cli_worktree_name_ok(const char *name)
{
    size_t i, n = strlen(name);
    if (!n || n > 64 || name[0] == '.' || name[0] == '-' || strstr(name, ".."))
        return false;
    for (i = 0; i < n; i++)
        if (!((name[i] >= 'a' && name[i] <= 'z') || (name[i] >= 'A' && name[i] <= 'Z') ||
              (name[i] >= '0' && name[i] <= '9') || name[i] == '-' || name[i] == '_' || name[i] == '.'))
            return false;
    return true;
}

static void first_line(char *s)
{
    s[strcspn(s, "\n")] = 0;
}

/* See cli.h. */
bool cli_worktree_enter(const char *want, char *note, size_t ncap, char *err, size_t ecap)
{
    static const char b32[] = "234567abcdefghijklmnopqrstuvwxyz";
    const char *rev[] = {"git", "rev-parse", "--path-format=absolute", "--git-common-dir", NULL};
    char name[72], root[4096], dir[4200], path[4300], branch[96], out[1024];
    char *slash;
    struct stat st;
    bool made = false;
    snprintf(name, sizeof name, "%s", want ? want : "");
    if (!name[0]) {
        unsigned char r[6];
        size_t i;
        pt_sys_random(NULL, r, sizeof r);
        memcpy(name, "wt-", 3);
        for (i = 0; i < sizeof r; i++)
            name[3 + i] = b32[r[i] & 31];
        name[3 + sizeof r] = 0;
    }
    if (!cli_worktree_name_ok(name)) {
        snprintf(err, ecap, "not a worktree name: %s (letters, digits, - _ .)", name);
        return false;
    }
    /* the main checkout's: from a worktree, the new one is its sibling */
    if (git(rev, root, sizeof root) != 0) {
        first_line(root);
        snprintf(err, ecap, "not in a git repository%s%.300s", root[0] ? ": " : "", root);
        return false;
    }
    first_line(root);
    if (!(slash = strrchr(root, '/')) || strcmp(slash, "/.git") != 0) {
        snprintf(err, ecap, "worktrees need a checkout with .git in it (git dir: %.400s)", root);
        return false;
    }
    *slash = 0;
    snprintf(dir, sizeof dir, "%s/.pytte/worktrees", root);
    snprintf(path, sizeof path, "%s/%s", dir, name);
    snprintf(branch, sizeof branch, "worktree-%s", name);
    if (stat(path, &st) != 0) {
        const char *add[] = {"git", "worktree", "add", "-b", branch, path, NULL};
        const char *attach[] = {"git", "worktree", "add", path, branch, NULL};
        char ignore[4300];
        if (!mkdirs(dir)) {
            snprintf(err, ecap, "cannot create %.400s: %s", dir, strerror(errno));
            return false;
        }
        snprintf(ignore, sizeof ignore, "%s/.gitignore", dir); /* the worktrees are not the checkout's files */
        if (stat(ignore, &st) != 0)
            pt_sys_write_file(ignore, "*\n", 2);
        if (git(add, out, sizeof out) != 0 && (!strstr(out, "already exists") || git(attach, out, sizeof out) != 0)) {
            first_line(out);
            snprintf(err, ecap, "git worktree add: %.300s", out[0] ? out : "failed");
            return false;
        }
        made = true;
    } else if (!S_ISDIR(st.st_mode)) {
        snprintf(err, ecap, "%.400s is not a directory", path);
        return false;
    }
    if (chdir(path) != 0) {
        snprintf(err, ecap, "cannot enter %.400s: %s", path, strerror(errno));
        return false;
    }
    if (made)
        snprintf(note, ncap, "made worktree %.400s on branch %s", path, branch);
    else
        snprintf(note, ncap, "in worktree %.400s", path);
    return true;
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
    /* first: when there is no worktree, the previous session is kept
     * (even an empty one) for the host to open again */
    bool entered = !job->o.worktree ||
                   cli_worktree_enter(job->o.worktree, job->note, sizeof job->note, job->err, sizeof job->err);
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
        if (job->discard_previous && entered)
            cli_session_discard(s->dir);
        job->previous = NULL;
        if (job->o.fork && !job->discard_previous) {
            if (!fork_session(job, s->dir)) {
                job->code = 1;
                return;
            }
            snprintf(job->note, sizeof job->note, "forked from %s", s->id);
            id = job->forked;
        }
    }
    if (!entered) {
        job->code = 1;
        return;
    }
    if (!id && job->o.continue_current) {
        if (!getcwd(cwd, sizeof cwd) || !cli_sessions_list(cwd, false, &picked, 1)) {
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
    const char *values[6];
    const char *failure = "could not allocate the session I/O worker";
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
    values[5] = o->worktree;
    for (i = 0; i < 6; i++)
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
    for (i = 0; i < 6; i++)
        if (values[i]) {
            const char *v = job->args.p + job->args.n;
            pt_buf_put(&job->args, values[i], strlen(values[i]) + 1);
            values[i] = v;
        }
    if (job->args.oom) {
        failure = "out of memory copying session arguments";
        goto fail;
    }
    job->o = *o;
    job->o.model = values[0];
    job->o.system = values[1];
    job->o.append_system = values[2];
    job->o.script = values[3];
    job->id = values[4];
    job->o.worktree = values[5]; /* for job_open only: args are freed after */
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
    snprintf(err, cap, "%s", failure);
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

const char *cli_session_note(const cli_session *s)
{
    return s->background ? s->background->note : "";
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
