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

const char cli_default_system[] =
    "You are pytte, a small coding agent running in a terminal. Be concise.";

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
        snprintf(all[n++], 64, "%s", e->d_name);
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
    pt_rt_free(s->rt);
    pt_free(s->a);
    if (s->http)
        s->http->free(s->http);
    s->rt = NULL;
    s->a = NULL;
    s->http = NULL;
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
    ao.config.model = o->model ? pt_strz(o->model) : PT_STR_NULL;
    ao.config.reasoning = o->reasoning;
    ao.config.max_tokens = o->max_tokens;
    ao.config.retry_base_ms = o->retry_base_ms;
    ao.config.no_sync = o->no_sync;
    memset(&rc, 0, sizeof rc);
    rc.store = st;
    rc.http = s->http;
    rc.stop = o->stop;
    if (!(s->a = pt_new(&ao)) || !(s->rt = pt_rt_new(&rc, s->a))) {
        st->close(st);
        return fail(s, 1, err, cap, "out of memory");
    }
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
