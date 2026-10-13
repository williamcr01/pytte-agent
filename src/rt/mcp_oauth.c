/* OAuth 2.1 for remote MCP servers (the 2025-06-18 authorization spec).
 *
 * A login: the server's protected-resource metadata (RFC 9728; its URL
 * from the 401's WWW-Authenticate, else well-known) names the
 * authorization server; its metadata (RFC 8414, else OpenID Connect
 * discovery) gives the endpoints. Without a configured client, one
 * registers dynamically (RFC 7591). The browser opens the authorization
 * page (PKCE S256, with the resource indicator of RFC 8707); the answer
 * comes back to a listener on 127.0.0.1 (RFC 8252), and the code is
 * exchanged for tokens. They are saved through the host, and the server
 * connects again.
 *
 * A 401 later refreshes the access token once, if there is a refresh
 * token; when that fails too, the server needs a login. */
#define _POSIX_C_SOURCE 200809L
#include "rt/mcp_int.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define LOGIN_MS (5 * 60 * 1000) /* the browser's answer */
#define REFRESH_GAP_MS 30000     /* no second refresh sooner: it did not help */
#define REQ_MAX 8192             /* the browser's request to the listener */

enum { F_IDLE, F_PRM, F_ASM, F_REG, F_BROWSER, F_TOKEN };

struct mauth {
    bool loaded;
    /* saved */
    char *access, *refresh, *client_id, *client_secret, *redirect;
    char *token_ep, *auth_ep, *reg_ep, *resource, *scope;
    /* from the last 401 */
    char *prm_url, *www_scope;
    char *bearer;
    int64_t refreshed_at;
    bool refreshing;
    /* a login */
    int step;
    char *issuer;
    char *cands[4]; /* metadata URLs to try, in order */
    int ncand, ci;
    char verifier[48], state[32];
    int lfd, cfd;
    char req[REQ_MAX];
    size_t nreq;
    uint32_t timer;
};

static void set(char **dst, pt_str v)
{
    char *p = NULL;
    if (v.n && (p = (char *)malloc(v.n + 1))) {
        memcpy(p, v.p, v.n);
        p[v.n] = 0;
    }
    free(*dst);
    *dst = p;
}

static void setz(char **dst, const char *z)
{
    set(dst, pt_strz(z));
}

static pt_str sz(const char *z)
{
    return z ? pt_strz(z) : PT_STR_NULL;
}

static mauth *auth_of(srv *s)
{
    if (!s->auth) {
        s->auth = (mauth *)calloc(1, sizeof *s->auth);
        if (s->auth)
            s->auth->lfd = s->auth->cfd = -1;
    }
    return s->auth;
}

/* ---- saving ---- */

static void put_field(pt_buf *b, const char *key, const char *v, bool *first)
{
    if (!v)
        return;
    pt_buf_putz(b, *first ? "{" : ",");
    *first = false;
    pt_json_quote(b, pt_strz(key));
    pt_buf_putc(b, ':');
    pt_json_quote(b, pt_strz(v));
}

static void save(srv *s)
{
    mauth *a = s->auth;
    pt_buf b;
    bool first = true;
    if (!s->m->host.auth_save)
        return;
    pt_buf_init(&b, pt_sys_allocator());
    put_field(&b, "access_token", a->access, &first);
    put_field(&b, "refresh_token", a->refresh, &first);
    put_field(&b, "client_id", a->client_id, &first);
    put_field(&b, "client_secret", a->client_secret, &first);
    put_field(&b, "redirect_uri", a->redirect, &first);
    put_field(&b, "token_endpoint", a->token_ep, &first);
    put_field(&b, "authorization_endpoint", a->auth_ep, &first);
    put_field(&b, "registration_endpoint", a->reg_ep, &first);
    put_field(&b, "resource", a->resource, &first);
    put_field(&b, "scope", a->scope, &first);
    if (!first)
        pt_buf_putc(&b, '}');
    if (!b.oom)
        s->m->host.auth_save(s->m->host.ud, s->name, s->url, pt_buf_str(&b));
    pt_buf_free(&b);
}

static void set_bearer(mauth *a)
{
    free(a->bearer);
    a->bearer = NULL;
    if (a->access && (a->bearer = (char *)malloc(strlen(a->access) + 8)))
        sprintf(a->bearer, "Bearer %s", a->access);
}

void mcp_auth_load(srv *s)
{
    mauth *a = auth_of(s);
    pt_buf b;
    pt_arena ar;
    const pt_json *j;
    if (!a || a->loaded || !s->m->host.auth_load)
        return;
    a->loaded = true;
    pt_buf_init(&b, pt_sys_allocator());
    pt_arena_init(&ar, pt_sys_allocator(), 0);
    if (s->m->host.auth_load(s->m->host.ud, s->name, s->url, &b) && !b.oom &&
        pt_json_is(j = pt_json_parse(pt_buf_str(&b), &ar, NULL), PT_J_OBJ)) {
        set(&a->access, pt_json_gets(j, "access_token", &ar));
        set(&a->refresh, pt_json_gets(j, "refresh_token", &ar));
        set(&a->client_id, pt_json_gets(j, "client_id", &ar));
        set(&a->client_secret, pt_json_gets(j, "client_secret", &ar));
        set(&a->redirect, pt_json_gets(j, "redirect_uri", &ar));
        set(&a->token_ep, pt_json_gets(j, "token_endpoint", &ar));
        set(&a->auth_ep, pt_json_gets(j, "authorization_endpoint", &ar));
        set(&a->reg_ep, pt_json_gets(j, "registration_endpoint", &ar));
        set(&a->resource, pt_json_gets(j, "resource", &ar));
        set(&a->scope, pt_json_gets(j, "scope", &ar));
        set_bearer(a);
    }
    pt_arena_free(&ar);
    pt_buf_free(&b);
}

const char *mcp_auth_bearer(srv *s)
{
    return s->auth ? s->auth->bearer : NULL;
}

/* ---- URLs and forms ---- */

static void enc(pt_buf *b, pt_str v)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t i;
    for (i = 0; i < v.n; i++) {
        unsigned char c = (unsigned char)v.p[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
            c == '_' || c == '~') {
            pt_buf_putc(b, (char)c);
        } else {
            pt_buf_putc(b, '%');
            pt_buf_putc(b, hex[c >> 4]);
            pt_buf_putc(b, hex[c & 15]);
        }
    }
}

/* key=value (&-joined), skipped when value is empty */
static void param(pt_buf *b, const char *key, pt_str v)
{
    if (!v.n)
        return;
    if (b->n && b->p[b->n - 1] != '?')
        pt_buf_putc(b, '&');
    pt_buf_putz(b, key);
    pt_buf_putc(b, '=');
    enc(b, v);
}

static int unhex(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* The value of key in a query (a=b&c=d), decoded into out. */
static bool query_get(pt_str q, const char *key, pt_buf *out)
{
    size_t kl = strlen(key), i = 0;
    while (i < q.n) {
        size_t e = i;
        while (e < q.n && q.p[e] != '&')
            e++;
        if (e - i > kl && q.p[i + kl] == '=' && !memcmp(q.p + i, key, kl)) {
            size_t k;
            pt_buf_reset(out);
            for (k = i + kl + 1; k < e; k++) {
                int h, l;
                if (q.p[k] == '+')
                    pt_buf_putc(out, ' ');
                else if (q.p[k] == '%' && k + 2 < e + 1 && (h = unhex(q.p[k + 1])) >= 0 && (l = unhex(q.p[k + 2])) >= 0) {
                    pt_buf_putc(out, (char)(h * 16 + l));
                    k += 2;
                } else
                    pt_buf_putc(out, q.p[k]);
            }
            return !out->oom;
        }
        i = e + 1;
    }
    return false;
}

/* scheme://authority, and the path after it (no query, no trailing /) */
static void url_split(pt_str url, pt_str *origin, pt_str *path)
{
    size_t i = 0, e;
    long sch = -1;
    for (i = 0; i + 2 < url.n; i++)
        if (url.p[i] == ':' && url.p[i + 1] == '/' && url.p[i + 2] == '/') {
            sch = (long)i;
            break;
        }
    i = sch < 0 ? 0 : (size_t)sch + 3;
    while (i < url.n && url.p[i] != '/' && url.p[i] != '?' && url.p[i] != '#')
        i++;
    *origin = (pt_str){url.p, i};
    e = i;
    while (e < url.n && url.p[e] != '?' && url.p[e] != '#')
        e++;
    while (e > i && url.p[e - 1] == '/')
        e--;
    *path = (pt_str){url.p + i, e - i};
}

static char *join3(pt_str a, const char *b, pt_str c)
{
    size_t n = a.n + strlen(b) + c.n;
    char *p = (char *)malloc(n + 1);
    if (p)
        snprintf(p, n + 1, "%.*s%s%.*s", (int)a.n, a.p, b, (int)c.n, c.p);
    return p;
}

/* A parameter of a WWW-Authenticate challenge: key="v" or key=v. */
static pt_str www_param(pt_str www, const char *key)
{
    size_t kl = strlen(key), i;
    for (i = 0; i + kl < www.n; i++) {
        size_t e;
        if ((i && www.p[i - 1] != ' ' && www.p[i - 1] != ',') || www.p[i + kl] != '=' ||
            !pt_str_ieq((pt_str){www.p + i, kl}, pt_strz(key)))
            continue;
        i += kl + 1;
        if (i < www.n && www.p[i] == '"') {
            e = ++i;
            while (e < www.n && www.p[e] != '"')
                e++;
        } else {
            e = i;
            while (e < www.n && www.p[e] != ',' && www.p[e] != ' ')
                e++;
        }
        return (pt_str){www.p + i, e - i};
    }
    return PT_STR_NULL;
}

/* ---- the flow ---- */

static void flow_end(srv *s)
{
    mauth *a = s->auth;
    hcall *h, *next;
    int i;
    if (!a)
        return;
    for (h = s->m->hcalls; h; h = next) {
        next = h->next;
        if (h->s == s && h->oauth)
            hcall_free(h);
    }
    if (a->timer)
        pt_rt_timer_cancel(s->m->rt, a->timer);
    a->timer = 0;
    if (a->cfd >= 0) {
        pt_rt_unwatch(s->m->rt, a->cfd);
        close(a->cfd);
    }
    if (a->lfd >= 0) {
        pt_rt_unwatch(s->m->rt, a->lfd);
        close(a->lfd);
    }
    a->lfd = a->cfd = -1;
    for (i = 0; i < a->ncand; i++) {
        free(a->cands[i]);
        a->cands[i] = NULL;
    }
    a->ncand = a->ci = 0;
    a->step = F_IDLE;
    a->refreshing = false;
}

static void login_fail(srv *s, const char *fmt, const char *detail)
{
    char msg[600];
    snprintf(msg, sizeof msg, fmt, detail ? detail : "");
    flow_end(s);
    mcp_notify_user(s, "login failed: %s", msg);
    if (s->state != PT_MCP_READY)
        mcp_needs_login(s, "the login failed");
}

static hcall *oauth_call(srv *s, const char *method, const char *url, void (*done)(hcall *))
{
    hcall *h = hcall_new(s->m, s, method, pt_strz(url));
    if (!h)
        return NULL;
    h->oauth = true;
    h->done = done;
    hcall_header(h, "Accept", PT_S("application/json"));
    return h;
}

static void start_call(srv *s, hcall *h)
{
    if (!h || !hcall_start(h))
        login_fail(s, "cannot start a request%s", NULL);
}

static bool json_ok(hcall *h, pt_arena *ar, const pt_json **out)
{
    if (h->err[0] || h->status < 200 || h->status >= 300)
        return false;
    *out = pt_json_parse(pt_buf_str(&h->body), ar, NULL);
    return pt_json_is(*out, PT_J_OBJ);
}

static void asm_next(srv *s);
static void after_metadata(srv *s);

static void add_cand(mauth *a, char *url)
{
    if (url && a->ncand < 4)
        a->cands[a->ncand++] = url;
    else
        free(url);
}

static void clear_cands(mauth *a)
{
    int i;
    for (i = 0; i < a->ncand; i++) {
        free(a->cands[i]);
        a->cands[i] = NULL;
    }
    a->ncand = a->ci = 0;
}

static void asm_start(srv *s)
{
    mauth *a = s->auth;
    pt_str origin, path;
    url_split(sz(a->issuer), &origin, &path);
    clear_cands(a);
    if (path.n) {
        add_cand(a, join3(origin, "/.well-known/oauth-authorization-server", path));
        add_cand(a, join3(origin, "/.well-known/openid-configuration", path));
        add_cand(a, join3((pt_str){origin.p, origin.n + path.n}, "/.well-known/openid-configuration", PT_STR_NULL));
    } else {
        add_cand(a, join3(origin, "/.well-known/oauth-authorization-server", PT_STR_NULL));
        add_cand(a, join3(origin, "/.well-known/openid-configuration", PT_STR_NULL));
    }
    a->step = F_ASM;
    asm_next(s);
}

static void prm_next(srv *s);

static void prm_done(hcall *h)
{
    srv *s = h->s;
    mauth *a = s->auth;
    pt_arena ar;
    const pt_json *j, *as;
    pt_arena_init(&ar, pt_sys_allocator(), 0);
    if (json_ok(h, &ar, &j) && pt_json_is(as = pt_json_get(j, "authorization_servers"), PT_J_ARR) && as->child) {
        const pt_json *sc = pt_json_get(j, "scopes_supported"), *e;
        pt_str res = pt_json_gets(j, "resource", &ar);
        set(&a->issuer, pt_json_str(as->child, &ar));
        set(&a->resource, res.n ? res : pt_strz(s->url));
        if (!s->scope && !a->www_scope && pt_json_is(sc, PT_J_ARR)) {
            pt_buf b;
            pt_buf_init(&b, pt_sys_allocator());
            for (e = sc->child; e; e = e->next) {
                if (b.n)
                    pt_buf_putc(&b, ' ');
                pt_buf_puts(&b, pt_json_str(e, &ar));
            }
            set(&a->scope, pt_buf_str(&b));
            pt_buf_free(&b);
        }
        pt_arena_free(&ar);
        asm_start(s);
        return;
    }
    pt_arena_free(&ar);
    prm_next(s);
}

static void prm_next(srv *s)
{
    mauth *a = s->auth;
    if (a->ci < a->ncand) {
        start_call(s, oauth_call(s, "GET", a->cands[a->ci++], prm_done));
        return;
    }
    /* no metadata (servers of 2025-03-26): the server's origin authorizes */
    {
        pt_str origin, path;
        url_split(pt_strz(s->url), &origin, &path);
        set(&a->issuer, origin);
        setz(&a->resource, s->url);
        asm_start(s);
    }
}

static void asm_done(hcall *h)
{
    srv *s = h->s;
    mauth *a = s->auth;
    pt_arena ar;
    const pt_json *j;
    pt_arena_init(&ar, pt_sys_allocator(), 0);
    if (json_ok(h, &ar, &j)) {
        const pt_json *m = pt_json_get(j, "code_challenge_methods_supported"), *e;
        bool s256 = !pt_json_is(m, PT_J_ARR);
        for (e = s256 ? NULL : m->child; e; e = e->next)
            s256 = s256 || pt_str_eqz(pt_json_str(e, &ar), "S256");
        if (!s256) {
            pt_arena_free(&ar);
            login_fail(s, "the authorization server does not support PKCE with S256%s", NULL);
            return;
        }
        set(&a->auth_ep, pt_json_gets(j, "authorization_endpoint", &ar));
        set(&a->token_ep, pt_json_gets(j, "token_endpoint", &ar));
        set(&a->reg_ep, pt_json_gets(j, "registration_endpoint", &ar));
        pt_arena_free(&ar);
        if (!a->auth_ep || !a->token_ep) {
            login_fail(s, "the authorization server's metadata has no endpoints%s", NULL);
            return;
        }
        after_metadata(s);
        return;
    }
    pt_arena_free(&ar);
    asm_next(s);
}

static void asm_next(srv *s)
{
    mauth *a = s->auth;
    pt_str origin, path;
    if (a->ci < a->ncand) {
        start_call(s, oauth_call(s, "GET", a->cands[a->ci++], asm_done));
        return;
    }
    /* no metadata: the default endpoints (2025-03-26) */
    url_split(sz(a->issuer), &origin, &path);
    free(a->auth_ep);
    free(a->token_ep);
    free(a->reg_ep);
    a->auth_ep = join3(origin, "/authorize", PT_STR_NULL);
    a->token_ep = join3(origin, "/token", PT_STR_NULL);
    a->reg_ep = join3(origin, "/register", PT_STR_NULL);
    if (!a->auth_ep || !a->token_ep) {
        login_fail(s, "out of memory%s", NULL);
        return;
    }
    after_metadata(s);
}

/* ---- the browser's answer ---- */

static void token_request(srv *s, pt_str code);

static void reply(int fd, int status, const char *text)
{
    char buf[1024];
    int n = snprintf(buf, sizeof buf,
                     "HTTP/1.1 %d %s\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n"
                     "Cache-Control: no-store\r\n\r\n<!doctype html><title>pytte</title>"
                     "<body style=\"font-family:sans-serif;margin:3em\"><p>%s</p></body>",
                     status, status == 200 ? "OK" : "Not Found", text);
    if (n > 0 && write(fd, buf, (size_t)n) < 0) {
        /* the browser went away: nothing to tell it */
    }
}

static void conn_close(srv *s)
{
    mauth *a = s->auth;
    if (a->cfd >= 0) {
        pt_rt_unwatch(s->m->rt, a->cfd);
        close(a->cfd);
    }
    a->cfd = -1;
    a->nreq = 0;
}

static void on_request(srv *s)
{
    mauth *a = s->auth;
    pt_str line = {a->req, 0}, target, q = PT_STR_NULL;
    pt_buf v;
    long sp;
    while (line.n < a->nreq && a->req[line.n] != '\r' && a->req[line.n] != '\n')
        line.n++;
    if (!pt_str_starts(line, PT_S("GET ")) || (sp = pt_str_find((pt_str){line.p + 4, line.n - 4}, ' ')) < 0) {
        reply(a->cfd, 404, "Not found.");
        conn_close(s);
        return;
    }
    target = (pt_str){line.p + 4, (size_t)sp};
    if (!pt_str_starts(target, PT_S("/callback"))) { /* favicon.ico and such */
        reply(a->cfd, 404, "Not found.");
        conn_close(s);
        return;
    }
    if (pt_str_find(target, '?') >= 0) {
        long k = pt_str_find(target, '?');
        q = (pt_str){target.p + k + 1, target.n - (size_t)k - 1};
    }
    pt_buf_init(&v, pt_sys_allocator());
    if (!query_get(q, "state", &v) || !pt_str_eqz(pt_buf_str(&v), a->state)) {
        reply(a->cfd, 404, "This login is not pytte's, or it is an old one.");
        conn_close(s);
        pt_buf_free(&v);
        return;
    }
    if (query_get(q, "error", &v)) {
        char why[300];
        snprintf(why, sizeof why, "%.*s", (int)mcp_cut(pt_buf_str(&v), 200).n, v.p);
        reply(a->cfd, 200, "The login failed. You can close this tab.");
        conn_close(s);
        pt_buf_free(&v);
        login_fail(s, "the authorization server said %s", why);
        return;
    }
    if (!query_get(q, "code", &v) || !v.n) {
        reply(a->cfd, 404, "No code came.");
        conn_close(s);
        pt_buf_free(&v);
        return;
    }
    reply(a->cfd, 200, "Logged in to the MCP server. You can close this tab and go back to pytte.");
    conn_close(s);
    /* one answer: stop listening */
    pt_rt_unwatch(s->m->rt, a->lfd);
    close(a->lfd);
    a->lfd = -1;
    token_request(s, pt_buf_str(&v));
    pt_buf_free(&v);
}

static void on_conn(void *ud, pt_rt *rt)
{
    srv *s = (srv *)ud;
    mauth *a = s->auth;
    (void)rt;
    for (;;) {
        ssize_t r = read(a->cfd, a->req + a->nreq, sizeof a->req - 1 - a->nreq);
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            return;
        if (r <= 0) {
            conn_close(s);
            return;
        }
        a->nreq += (size_t)r;
        a->req[a->nreq] = 0;
        if (strstr(a->req, "\r\n\r\n") || strstr(a->req, "\n\n") || a->nreq == sizeof a->req - 1) {
            on_request(s);
            return;
        }
    }
}

static void on_accept(void *ud, pt_rt *rt)
{
    srv *s = (srv *)ud;
    mauth *a = s->auth;
    int fd = accept(a->lfd, NULL, NULL);
    if (fd < 0)
        return;
    if (a->cfd >= 0) { /* one at a time */
        close(fd);
        return;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    a->cfd = fd;
    a->nreq = 0;
    if (!pt_rt_watch(rt, fd, on_conn, s))
        conn_close(s);
}

/* A listener on 127.0.0.1: port (0: any). Its port, or 0. */
static int listen_on(mauth *a, int port)
{
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;
    int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    if (fd < 0)
        return 0;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(fd, 4) < 0 ||
        getsockname(fd, (struct sockaddr *)&sa, &sl) < 0) {
        close(fd);
        return 0;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    a->lfd = fd;
    return ntohs(sa.sin_port);
}

static int redirect_port(const char *redirect)
{
    const char *p;
    if (!redirect || strncmp(redirect, "http://127.0.0.1:", 17))
        return 0;
    p = redirect + 17;
    return atoi(p);
}

static void login_timeout(void *ud, pt_rt *rt)
{
    srv *s = (srv *)ud;
    (void)rt;
    s->auth->timer = 0;
    login_fail(s, "no answer came from the browser within 5 minutes%s", NULL);
}

static void authorize(srv *s)
{
    mauth *a = s->auth;
    unsigned char r[32], d[32];
    pt_buf v, u;
    pt_sha256 sh;
    const char *client = s->client_id ? s->client_id : a->client_id;
    const char *scope = s->scope ? s->scope : a->www_scope ? a->www_scope : a->scope;
    pt_sys_random(NULL, r, sizeof r);
    pt_buf_init(&v, pt_sys_allocator());
    pt_base64url(&v, (pt_str){(const char *)r, 32});
    snprintf(a->verifier, sizeof a->verifier, "%.*s", (int)v.n, v.p);
    pt_sys_random(NULL, r, 16);
    pt_buf_reset(&v);
    pt_base64url(&v, (pt_str){(const char *)r, 16});
    snprintf(a->state, sizeof a->state, "%.*s", (int)v.n, v.p);
    pt_sha256_init(&sh);
    pt_sha256_update(&sh, a->verifier, strlen(a->verifier));
    pt_sha256_final(&sh, d);
    pt_buf_reset(&v);
    pt_base64url(&v, (pt_str){(const char *)d, 32});
    pt_buf_init(&u, pt_sys_allocator());
    pt_buf_putz(&u, a->auth_ep);
    pt_buf_putc(&u, strchr(a->auth_ep, '?') ? '&' : '?');
    param(&u, "response_type", PT_S("code"));
    param(&u, "client_id", sz(client));
    param(&u, "redirect_uri", sz(a->redirect));
    param(&u, "code_challenge", pt_buf_str(&v));
    param(&u, "code_challenge_method", PT_S("S256"));
    param(&u, "state", pt_strz(a->state));
    param(&u, "resource", sz(a->resource));
    param(&u, "scope", sz(scope));
    pt_buf_free(&v);
    if (u.oom) {
        pt_buf_free(&u);
        login_fail(s, "out of memory%s", NULL);
        return;
    }
    a->step = F_BROWSER;
    a->timer = pt_rt_timer(s->m->rt, pt_rt_now(s->m->rt) + LOGIN_MS, login_timeout, s);
    {
        char *url = (char *)malloc(u.n + 1);
        if (url) {
            memcpy(url, u.p, u.n);
            url[u.n] = 0;
            mcp_notify_user(s, "log in in the browser; if it did not open, go to %.900s", url);
            if (s->m->host.open_url)
                s->m->host.open_url(s->m->host.ud, url);
            free(url);
        }
    }
    pt_buf_free(&u);
}

static void reg_done(hcall *h)
{
    srv *s = h->s;
    mauth *a = s->auth;
    pt_arena ar;
    const pt_json *j;
    pt_str id;
    pt_arena_init(&ar, pt_sys_allocator(), 0);
    if (!json_ok(h, &ar, &j) || !(id = pt_json_gets(j, "client_id", &ar)).n) {
        char why[400];
        if (h->err[0])
            snprintf(why, sizeof why, "%s", h->err);
        else
            snprintf(why, sizeof why, "HTTP %d %.*s", h->status, (int)mcp_cut(pt_buf_str(&h->body), 200).n,
                     h->body.p ? h->body.p : "");
        pt_arena_free(&ar);
        login_fail(s, "registering a client failed (%s); set client_id in the server's config", why);
        return;
    }
    set(&a->client_id, id);
    set(&a->client_secret, pt_json_gets(j, "client_secret", &ar));
    pt_arena_free(&ar);
    save(s);
    authorize(s);
}

static void after_metadata(srv *s)
{
    mauth *a = s->auth;
    int want = redirect_port(a->redirect), port = 0;
    char uri[64];
    clear_cands(a);
    /* the port registered before, so the client stays valid */
    if (want)
        port = listen_on(a, want);
    if (!port)
        port = listen_on(a, 0);
    if (!port) {
        login_fail(s, "cannot listen on 127.0.0.1 for the browser's answer%s", NULL);
        return;
    }
    if (!pt_rt_watch(s->m->rt, a->lfd, on_accept, s)) {
        login_fail(s, "out of memory%s", NULL);
        return;
    }
    snprintf(uri, sizeof uri, "http://127.0.0.1:%d/callback", port);
    if (!s->client_id && (port != want || !a->client_id)) {
        hcall *h;
        pt_buf *b;
        setz(&a->redirect, uri);
        free(a->client_id);
        free(a->client_secret);
        a->client_id = a->client_secret = NULL;
        if (!a->reg_ep) {
            login_fail(s, "the authorization server does not register clients: set client_id in the server's config%s",
                       NULL);
            return;
        }
        a->step = F_REG;
        h = oauth_call(s, "POST", a->reg_ep, reg_done);
        if (!h) {
            login_fail(s, "out of memory%s", NULL);
            return;
        }
        hcall_header(h, "Content-Type", PT_S("application/json"));
        b = &h->body_out;
        pt_buf_putz(b, "{\"client_name\":\"pytte\",\"redirect_uris\":[");
        pt_json_quote(b, pt_strz(uri));
        pt_buf_putz(b, "],\"grant_types\":[\"authorization_code\",\"refresh_token\"],"
                       "\"response_types\":[\"code\"],\"token_endpoint_auth_method\":\"none\"");
        if (s->scope || a->scope) {
            pt_buf_putz(b, ",\"scope\":");
            pt_json_quote(b, pt_strz(s->scope ? s->scope : a->scope));
        }
        pt_buf_putc(b, '}');
        start_call(s, h);
        return;
    }
    setz(&a->redirect, uri);
    authorize(s);
}

/* ---- tokens ---- */

static bool got_tokens(srv *s, hcall *h)
{
    mauth *a = s->auth;
    pt_arena ar;
    const pt_json *j;
    pt_str tok, type;
    pt_arena_init(&ar, pt_sys_allocator(), 0);
    if (!json_ok(h, &ar, &j) || !(tok = pt_json_gets(j, "access_token", &ar)).n) {
        pt_arena_free(&ar);
        return false;
    }
    type = pt_json_gets(j, "token_type", &ar);
    if (type.n && !pt_str_ieq(type, PT_S("bearer"))) {
        pt_arena_free(&ar);
        return false;
    }
    set(&a->access, tok);
    if (pt_json_gets(j, "refresh_token", &ar).n) /* else the old one stays */
        set(&a->refresh, pt_json_gets(j, "refresh_token", &ar));
    pt_arena_free(&ar);
    set_bearer(a);
    save(s);
    return true;
}

static void form_auth(srv *s, hcall *h)
{
    mauth *a = s->auth;
    const char *client = s->client_id ? s->client_id : a->client_id;
    const char *secret = s->client_id ? s->client_secret : a->client_secret;
    param(&h->body_out, "client_id", sz(client));
    param(&h->body_out, "client_secret", sz(secret));
    param(&h->body_out, "resource", sz(a->resource));
    hcall_header(h, "Content-Type", PT_S("application/x-www-form-urlencoded"));
}

static void token_done(hcall *h)
{
    srv *s = h->s;
    if (!got_tokens(s, h)) {
        char why[300];
        if (h->err[0])
            snprintf(why, sizeof why, "%s", h->err);
        else
            snprintf(why, sizeof why, "HTTP %d %.*s", h->status, (int)mcp_cut(pt_buf_str(&h->body), 200).n,
                     h->body.p ? h->body.p : "");
        login_fail(s, "the token request failed: %s", why);
        return;
    }
    flow_end(s);
    mcp_notify_user(s, "logged in");
    mcp_reconnect(s);
}

static void token_request(srv *s, pt_str code)
{
    mauth *a = s->auth;
    hcall *h;
    if (a->timer)
        pt_rt_timer_cancel(s->m->rt, a->timer);
    a->timer = 0;
    a->step = F_TOKEN;
    h = oauth_call(s, "POST", a->token_ep, token_done);
    if (!h) {
        login_fail(s, "out of memory%s", NULL);
        return;
    }
    param(&h->body_out, "grant_type", PT_S("authorization_code"));
    param(&h->body_out, "code", code);
    param(&h->body_out, "redirect_uri", sz(a->redirect));
    param(&h->body_out, "code_verifier", pt_strz(a->verifier));
    form_auth(s, h);
    start_call(s, h);
}

static void refresh_done(hcall *h)
{
    srv *s = h->s;
    mauth *a = s->auth;
    a->refreshing = false;
    if (got_tokens(s, h)) {
        mcp_reconnect(s);
        return;
    }
    if (h->status >= 400 && h->status < 500) { /* the grant is gone */
        free(a->refresh);
        free(a->access);
        a->refresh = a->access = NULL;
        set_bearer(a);
        save(s);
    }
    mcp_needs_login(s, "its login expired");
}

bool mcp_auth_unauthorized(srv *s, pt_str www)
{
    mauth *a = auth_of(s);
    pt_rt *rt = s->m->rt;
    pt_str v;
    hcall *h;
    if (!a)
        return false;
    if ((v = www_param(www, "resource_metadata")).n)
        set(&a->prm_url, v);
    if ((v = www_param(www, "scope")).n)
        set(&a->www_scope, v);
    if (a->refreshing)
        return true;
    if (!a->refresh || !a->token_ep || a->step != F_IDLE ||
        (a->refreshed_at && pt_rt_now(rt) - a->refreshed_at < REFRESH_GAP_MS))
        return false;
    h = oauth_call(s, "POST", a->token_ep, refresh_done);
    if (!h)
        return false;
    param(&h->body_out, "grant_type", PT_S("refresh_token"));
    param(&h->body_out, "refresh_token", pt_strz(a->refresh));
    form_auth(s, h);
    if (!hcall_start(h))
        return false;
    a->refreshing = true;
    a->refreshed_at = pt_rt_now(rt);
    return true;
}

bool mcp_auth_login(srv *s)
{
    mauth *a = auth_of(s);
    pt_str origin, path;
    if (!a)
        return false;
    mcp_auth_load(s);
    flow_end(s);
    clear_cands(a);
    a->step = F_PRM;
    url_split(pt_strz(s->url), &origin, &path);
    if (a->prm_url)
        add_cand(a, join3(pt_strz(a->prm_url), "", PT_STR_NULL));
    if (path.n)
        add_cand(a, join3(origin, "/.well-known/oauth-protected-resource", path));
    add_cand(a, join3(origin, "/.well-known/oauth-protected-resource", PT_STR_NULL));
    mcp_notify_user(s, "logging in...");
    prm_next(s);
    return true;
}

void mcp_auth_logout(srv *s)
{
    mauth *a = auth_of(s);
    if (!a)
        return;
    mcp_auth_load(s);
    flow_end(s);
    free(a->access);
    free(a->refresh);
    a->access = a->refresh = NULL;
    set_bearer(a);
    save(s);
}

void mcp_auth_free(srv *s)
{
    mauth *a = s->auth;
    if (!a)
        return;
    flow_end(s);
    free(a->access);
    free(a->refresh);
    free(a->client_id);
    free(a->client_secret);
    free(a->redirect);
    free(a->token_ep);
    free(a->auth_ep);
    free(a->reg_ep);
    free(a->resource);
    free(a->scope);
    free(a->prm_url);
    free(a->www_scope);
    free(a->bearer);
    free(a->issuer);
    free(a);
    s->auth = NULL;
}
