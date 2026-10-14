/* Subscription logins (/login oauth): Sign in with ChatGPT, for the
 * openai-codex provider.
 *
 * The flow is the Codex CLI's (and pi's): OAuth 2 with PKCE S256 at
 * auth.openai.com, the browser sent back to http://localhost:1455/auth/callback,
 * where a listener takes the code. When the browser cannot reach this
 * machine (pytte over ssh), the address it ended on can be pasted instead.
 * The code is exchanged for tokens; the access token is a JWT that names
 * the ChatGPT account. Tokens are saved in auth.json and refreshed shortly
 * before they expire. */
#define _POSIX_C_SOURCE 200809L
#include "cli/cli.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CLIENT_ID "app_EMoamEEZ73f0CkXaXp7hrann"
#define AUTHORIZE_URL "https://auth.openai.com/oauth/authorize"
#define TOKEN_URL "https://auth.openai.com/oauth/token"
#define CALLBACK_PORT 1455
#define CALLBACK_PATH "/auth/callback"
#define REDIRECT_URI "http://localhost:1455" CALLBACK_PATH
#define SCOPE "openid profile email offline_access"
#define JWT_CLAIM "https://api.openai.com/auth"

#define LOGIN_MS (5 * 60 * 1000) /* the browser's answer */
#define HTTP_MS 30000            /* a token request */
#define REQ_MAX 8192             /* the browser's request to the listener */
#define REFRESH_MARGIN_MS (5 * 60 * 1000) /* refresh this long before expiry */

/* ---- helpers ---- */

static void enc(pt_buf *b, pt_str v)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t i;
    for (i = 0; i < v.n; i++) {
        unsigned char c = (unsigned char)v.p[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
            c == '_' || c == '~') {
            pt_buf_putc(b, (char)c);
        } else {
            pt_buf_putc(b, '%');
            pt_buf_putc(b, hex[c >> 4]);
            pt_buf_putc(b, hex[c & 15]);
        }
    }
}

/* "&key=value" (no "&" first), value encoded. */
static void param(pt_buf *b, const char *key, pt_str v)
{
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

/* A query string's value for key, decoded, into out; false when absent. */
static bool query_get(pt_str q, const char *key, pt_buf *out)
{
    size_t kn = strlen(key);
    pt_buf_reset(out);
    while (q.n) {
        long amp = pt_str_find(q, '&');
        pt_str part = {q.p, amp < 0 ? q.n : (size_t)amp};
        if (part.n > kn && memcmp(part.p, key, kn) == 0 && part.p[kn] == '=') {
            size_t i;
            for (i = kn + 1; i < part.n; i++) {
                int h, l;
                if (part.p[i] == '%' && i + 2 < part.n && (h = unhex(part.p[i + 1])) >= 0 &&
                    (l = unhex(part.p[i + 2])) >= 0) {
                    pt_buf_putc(out, (char)(h * 16 + l));
                    i += 2;
                } else {
                    pt_buf_putc(out, part.p[i] == '+' ? ' ' : part.p[i]);
                }
            }
            return !out->oom;
        }
        if (amp < 0)
            break;
        q.p += amp + 1;
        q.n -= (size_t)amp + 1;
    }
    return false;
}

/* The ChatGPT account a JWT access token is for, into out; false when it
 * names none. */
static bool jwt_account(pt_str token, pt_buf *out)
{
    long a = pt_str_find(token, '.'), b;
    pt_str payload;
    pt_buf raw;
    pt_arena ar;
    pt_str id;
    bool ok = false;
    pt_buf_reset(out);
    if (a < 0)
        return false;
    payload = (pt_str){token.p + a + 1, token.n - (size_t)a - 1};
    if ((b = pt_str_find(payload, '.')) < 0)
        return false;
    payload.n = (size_t)b;
    pt_buf_init(&raw, pt_sys_allocator());
    pt_arena_init(&ar, pt_sys_allocator(), 0);
    if (pt_base64_decode(payload, &raw) && !raw.oom) {
        const pt_json *j = pt_json_parse(pt_buf_str(&raw), &ar, NULL);
        id = pt_json_gets(pt_json_get(j, JWT_CLAIM), "chatgpt_account_id", &ar);
        if (id.n) {
            pt_buf_puts(out, id);
            ok = !out->oom;
        }
    }
    pt_arena_free(&ar);
    pt_buf_free(&raw);
    return ok;
}

/* ---- token requests ---- */

typedef struct post {
    int status;
    bool done;
    char err[300];
    pt_buf body;
} post;

static void post_head(void *ud, int status, pt_str retry_after)
{
    (void)retry_after;
    ((post *)ud)->status = status;
}

static void post_body(void *ud, const char *p, size_t n)
{
    post *r = (post *)ud;
    if (r->body.n + n > ((size_t)1 << 20))
        r->body.oom = true;
    else
        pt_buf_put(&r->body, p, n);
}

static void post_done(void *ud, const char *error)
{
    post *r = (post *)ud;
    r->done = true;
    if (error)
        snprintf(r->err, sizeof r->err, "%s", error);
}

/* POST a form to the token endpoint, blocking up to HTTP_MS; the answer's
 * tokens into l (strings in ar). False with err. */
static bool token_post(pt_str form, cli_login *l, pt_arena *ar, char *err, size_t cap)
{
    pt_header h[2] = {{"Content-Type", "application/x-www-form-urlencoded"}, {"Accept", "application/json"}};
    pt_http_driver *d = pt_http_curl_new();
    pt_http_req req;
    pt_http_sink sink;
    post r;
    void *handle;
    int64_t end = pt_sys_now_ms() + HTTP_MS;
    const pt_json *j;
    pt_buf acct;
    pt_str access, refresh;
    bool ok = false;
    if (!d) {
        snprintf(err, cap, "cannot start the HTTP driver");
        return false;
    }
    memset(&r, 0, sizeof r);
    pt_buf_init(&r.body, pt_sys_allocator());
    memset(&req, 0, sizeof req);
    req.method = "POST";
    req.url = TOKEN_URL;
    req.headers = h;
    req.nheaders = req.hcap = 2;
    req.body = form;
    sink.ud = &r;
    sink.head = post_head;
    sink.body = post_body;
    sink.done = post_done;
    sink.header = NULL;
    if (!(handle = d->start(d, &req, &sink))) {
        snprintf(err, cap, "cannot start the request");
        goto done;
    }
    while (!r.done && pt_sys_now_ms() < end)
        if (d->pump(d, 500, NULL, 0) < 0) {
            snprintf(r.err, sizeof r.err, "the HTTP driver failed");
            break;
        }
    if (!r.done) {
        d->cancel(d, handle);
        snprintf(err, cap, "%s", r.err[0] ? r.err : "auth.openai.com did not answer");
        goto done;
    }
    if (r.err[0]) {
        snprintf(err, cap, "%s", r.err);
        goto done;
    }
    j = pt_json_parse(pt_buf_str(&r.body), ar, NULL);
    if (r.status < 200 || r.status > 299) {
        pt_str why = pt_json_gets(j, "error_description", ar);
        if (!why.n)
            why = pt_json_gets(j, "error", ar);
        if (!why.n)
            why = pt_json_gets(pt_json_get(j, "error"), "message", ar);
        snprintf(err, cap, "HTTP %d%s%.*s", r.status, why.n ? ": " : "", (int)(why.n > 200 ? 200 : why.n), why.p);
        goto done;
    }
    access = pt_json_gets(j, "access_token", ar);
    refresh = pt_json_gets(j, "refresh_token", ar);
    if (!access.n) {
        snprintf(err, cap, "the answer has no access token");
        goto done;
    }
    pt_buf_init(&acct, pt_sys_allocator());
    if (!jwt_account(access, &acct)) {
        snprintf(err, cap, "the token names no ChatGPT account");
        pt_buf_free(&acct);
        goto done;
    }
    l->access = pt_arena_cstr(ar, access);
    if (refresh.n) /* a refresh may keep the old one */
        l->refresh = pt_arena_cstr(ar, refresh);
    l->account_id = pt_arena_cstr(ar, pt_buf_str(&acct));
    l->expires = pt_sys_now_ms() + pt_json_geti(j, "expires_in", 3600) * 1000;
    pt_buf_free(&acct);
    ok = l->access && l->account_id && !ar->oom;
    if (!ok)
        snprintf(err, cap, "out of memory");
done:
    pt_buf_free(&r.body);
    d->free(d);
    return ok;
}

/* ---- refreshing ---- */

static pthread_mutex_t refresh_lock = PTHREAD_MUTEX_INITIALIZER;

bool cli_oauth_credentials(const char *provider, pt_credentials *out, char *why, size_t cap)
{
    cli_login l;
    bool ok = true;
    why[0] = 0;
    pthread_mutex_lock(&refresh_lock); /* one refresh at a time: it replaces the refresh token */
    if (!cli_auth_login(provider, false, &l)) {
        snprintf(why, cap, "not logged in to %s: run /login oauth %s", provider, provider);
        ok = false;
    } else if (l.expires - pt_sys_now_ms() < REFRESH_MARGIN_MS &&
               (!cli_auth_login(provider, true, &l) || l.expires - pt_sys_now_ms() < REFRESH_MARGIN_MS)) {
        pt_arena ar;
        pt_buf form;
        cli_login fresh = l;
        char err[400];
        pt_arena_init(&ar, pt_sys_allocator(), 0);
        pt_buf_init(&form, pt_sys_allocator());
        param(&form, "grant_type", PT_S("refresh_token"));
        param(&form, "refresh_token", pt_strz(l.refresh));
        param(&form, "client_id", PT_S(CLIENT_ID));
        if (!form.oom && token_post(pt_buf_str(&form), &fresh, &ar, err, sizeof err)) {
            cli_auth_login_save(provider, &fresh, err, sizeof err); /* kept in memory even when not saved */
            cli_auth_login(provider, false, &l);
        } else if (l.expires <= pt_sys_now_ms()) {
            snprintf(why, cap, "the %s login could not be refreshed (%s): run /login oauth %s", provider,
                     form.oom ? "out of memory" : err, provider);
            ok = false;
        } /* else: the old token still works for a while */
        pt_buf_free(&form);
        pt_arena_free(&ar);
    }
    pthread_mutex_unlock(&refresh_lock);
    if (ok) {
        out->api_key = pt_strz(l.access);
        out->account_id = pt_strz(l.account_id);
    }
    return ok;
}

/* ---- logging in ---- */

struct cli_oauth {
    pt_worker *w;
    int lfd;     /* the listener; -1 when the port was taken */
    int wake[2]; /* pasted addresses come through it; closing it cancels */
    char provider[64];
    char verifier[48], state[40];
    bool ok;
    char msg[600];
};

static void reply(int fd, int status, const char *text)
{
    char buf[1024];
    int n = snprintf(buf, sizeof buf,
                     "HTTP/1.1 %d %s\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n"
                     "Cache-Control: no-store\r\n\r\n<!doctype html><title>pytte</title>"
                     "<body style=\"font-family:sans-serif;margin:3em\"><p>%s</p></body>",
                     status, status == 200 ? "OK" : status == 400 ? "Bad Request" : "Not Found", text);
    if (n > 0 && write(fd, buf, (size_t)n) < 0) {
        /* the browser went away: nothing to tell it */
    }
}

/* The code and state from a callback's query, or from what was pasted: the
 * address, "code#state", a query, or the code alone. */
static void parse_answer(pt_str in, pt_buf *code, pt_buf *state, pt_buf *error)
{
    long q;
    in = pt_str_trim(in);
    pt_buf_reset(code);
    pt_buf_reset(state);
    pt_buf_reset(error);
    if ((q = pt_str_find(in, '?')) >= 0 || pt_str_contains(in, PT_S("code="))) {
        pt_str query = q >= 0 ? (pt_str){in.p + q + 1, in.n - (size_t)q - 1} : in;
        long hash = pt_str_find(query, '#');
        if (hash >= 0)
            query.n = (size_t)hash;
        query_get(query, "code", code);
        query_get(query, "state", state);
        query_get(query, "error", error);
    } else if ((q = pt_str_find(in, '#')) >= 0) {
        pt_buf_put(code, in.p, (size_t)q);
        pt_buf_put(state, in.p + q + 1, in.n - (size_t)q - 1);
    } else {
        pt_buf_puts(code, in);
    }
}

/* Exchange the code; the login's result into l->ok and l->msg. */
static void exchange(cli_oauth *l, pt_str code)
{
    pt_arena ar;
    pt_buf form;
    cli_login t;
    char err[400];
    pt_arena_init(&ar, pt_sys_allocator(), 0);
    pt_buf_init(&form, pt_sys_allocator());
    memset(&t, 0, sizeof t);
    param(&form, "grant_type", PT_S("authorization_code"));
    param(&form, "client_id", PT_S(CLIENT_ID));
    param(&form, "code", code);
    param(&form, "code_verifier", pt_strz(l->verifier));
    param(&form, "redirect_uri", PT_S(REDIRECT_URI));
    if (form.oom) {
        snprintf(l->msg, sizeof l->msg, "login failed: out of memory");
    } else if (!token_post(pt_buf_str(&form), &t, &ar, err, sizeof err)) {
        snprintf(l->msg, sizeof l->msg, "login failed: %s", err);
    } else if (!t.refresh) {
        snprintf(l->msg, sizeof l->msg, "login failed: the answer has no refresh token");
    } else if (!cli_auth_login_save(l->provider, &t, err, sizeof err)) {
        snprintf(l->msg, sizeof l->msg, "logged in to %s for this run only: %s", l->provider, err);
        l->ok = true;
    } else {
        char path[4200];
        cli_auth_path(path, sizeof path);
        snprintf(l->msg, sizeof l->msg, "logged in to %s; saved to %.500s", l->provider, path);
        l->ok = true;
    }
    pt_buf_free(&form);
    pt_arena_free(&ar);
}

/* Read one request from the browser (up to REQ_MAX, a few seconds). True
 * when it carried the login's answer (into code; the caller answers the
 * browser once the code is exchanged), or ended the login with an error
 * (l->msg). Other requests are answered here. */
static bool on_conn(cli_oauth *l, int fd, pt_buf *code)
{
    char req[REQ_MAX];
    size_t n = 0;
    pt_str line, target;
    pt_buf state, error;
    long sp;
    bool done = false;
    int64_t end = pt_sys_now_ms() + 5000;
    while (n < sizeof req - 1) {
        struct pollfd p;
        ssize_t got;
        int left = (int)(end - pt_sys_now_ms());
        p.fd = fd;
        p.events = POLLIN;
        if (left <= 0 || poll(&p, 1, left) <= 0)
            break;
        if ((got = read(fd, req + n, sizeof req - 1 - n)) <= 0)
            break;
        n += (size_t)got;
        req[n] = 0;
        if (strstr(req, "\r\n\r\n") || strstr(req, "\n\n"))
            break;
    }
    line = (pt_str){req, 0};
    while (line.n < n && req[line.n] != '\r' && req[line.n] != '\n')
        line.n++;
    if (!pt_str_starts(line, PT_S("GET ")) || (sp = pt_str_find((pt_str){line.p + 4, line.n - 4}, ' ')) < 0) {
        reply(fd, 404, "Not found.");
        return false;
    }
    target = (pt_str){line.p + 4, (size_t)sp};
    if (!pt_str_starts(target, PT_S(CALLBACK_PATH))) { /* favicon.ico and such */
        reply(fd, 404, "Not found.");
        return false;
    }
    pt_buf_init(&state, pt_sys_allocator());
    pt_buf_init(&error, pt_sys_allocator());
    parse_answer(target, code, &state, &error);
    if (!pt_str_eqz(pt_buf_str(&state), l->state)) {
        reply(fd, 400, "This login is not pytte's, or it is an old one.");
    } else if (error.n || !code->n) {
        reply(fd, 400, "The login did not finish. Go back to pytte to try again.");
        snprintf(l->msg, sizeof l->msg, "login failed: %.*s", (int)(error.n > 200 ? 200 : error.n),
                 error.n ? error.p : "no code");
        pt_buf_reset(code);
        done = true;
    } else {
        done = true;
    }
    pt_buf_free(&state);
    pt_buf_free(&error);
    return done;
}

/* The worker: wait for the browser's answer or a pasted one, then
 * exchange the code. */
static void login_job(void *ud)
{
    cli_oauth *l = (cli_oauth *)ud;
    int64_t end = pt_sys_now_ms() + LOGIN_MS;
    pt_buf code, state, error;
    pt_buf_init(&code, pt_sys_allocator());
    pt_buf_init(&state, pt_sys_allocator());
    pt_buf_init(&error, pt_sys_allocator());
    snprintf(l->msg, sizeof l->msg, "login failed: no answer from the browser in 5 minutes");
    for (;;) {
        struct pollfd p[2];
        int left = (int)(end - pt_sys_now_ms()), np = 1;
        p[0].fd = l->wake[0];
        p[0].events = POLLIN;
        if (l->lfd >= 0) {
            p[1].fd = l->lfd;
            p[1].events = POLLIN;
            np = 2;
        }
        if (left <= 0)
            break;
        if (poll(p, (nfds_t)np, left) < 0) {
            if (errno == EINTR)
                continue;
            snprintf(l->msg, sizeof l->msg, "login failed: %s", strerror(errno));
            break;
        }
        if (p[0].revents) {
            char in[4096];
            ssize_t n = read(l->wake[0], in, sizeof in);
            if (n <= 0) {
                snprintf(l->msg, sizeof l->msg, "login cancelled");
                break;
            }
            parse_answer((pt_str){in, (size_t)n}, &code, &state, &error);
            if (error.n) {
                snprintf(l->msg, sizeof l->msg, "login failed: %.*s", (int)(error.n > 200 ? 200 : error.n), error.p);
                break;
            }
            if (state.n && !pt_str_eqz(pt_buf_str(&state), l->state)) {
                snprintf(l->msg, sizeof l->msg, "login failed: that address is from another login");
                break;
            }
            if (code.n) {
                exchange(l, pt_buf_str(&code));
                break;
            }
        }
        if (np == 2 && p[1].revents) {
            int fd = accept(l->lfd, NULL, NULL);
            bool done;
            if (fd < 0)
                continue;
            done = on_conn(l, fd, &code);
            if (done && code.n) {
                exchange(l, pt_buf_str(&code));
                reply(fd, l->ok ? 200 : 400,
                      l->ok ? "Signed in to pytte. You can close this tab."
                            : "The login did not finish. Go back to pytte to see why.");
            }
            close(fd);
            if (done)
                break;
        }
    }
    pt_buf_free(&code);
    pt_buf_free(&state);
    pt_buf_free(&error);
}

static int listen_local(void)
{
    struct sockaddr_in sa;
    int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    if (fd < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(CALLBACK_PORT);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(fd, 4) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

cli_oauth *cli_oauth_begin(const char *provider, char *url, size_t ucap, char *err, size_t cap)
{
    unsigned char rnd[32], digest[32];
    pt_sha256 h;
    pt_buf b;
    cli_oauth *l;
    size_t i;
    if (strcmp(provider, "openai-codex") != 0) {
        snprintf(err, cap, "no login for %s", provider);
        return NULL;
    }
    if (!(l = (cli_oauth *)calloc(1, sizeof *l))) {
        snprintf(err, cap, "out of memory");
        return NULL;
    }
    l->lfd = l->wake[0] = l->wake[1] = -1;
    snprintf(l->provider, sizeof l->provider, "%s", provider);
    pt_buf_init(&b, pt_sys_allocator());
    /* PKCE: a random verifier, its SHA-256 as the challenge */
    pt_sys_random(NULL, rnd, sizeof rnd);
    pt_base64url(&b, (pt_str){(const char *)rnd, sizeof rnd});
    snprintf(l->verifier, sizeof l->verifier, "%.*s", (int)b.n, b.p);
    pt_sys_random(NULL, rnd, 16);
    for (i = 0; i < 16; i++)
        snprintf(l->state + 2 * i, 3, "%02x", rnd[i]);
    pt_sha256_init(&h);
    pt_sha256_update(&h, l->verifier, strlen(l->verifier));
    pt_sha256_final(&h, digest);
    pt_buf_reset(&b);
    pt_base64url(&b, (pt_str){(const char *)digest, sizeof digest});
    {
        pt_buf u;
        pt_buf_init(&u, pt_sys_allocator());
        pt_buf_putz(&u, AUTHORIZE_URL "?");
        param(&u, "response_type", PT_S("code"));
        param(&u, "client_id", PT_S(CLIENT_ID));
        param(&u, "redirect_uri", PT_S(REDIRECT_URI));
        param(&u, "scope", PT_S(SCOPE));
        param(&u, "code_challenge", pt_buf_str(&b));
        param(&u, "code_challenge_method", PT_S("S256"));
        param(&u, "state", pt_strz(l->state));
        param(&u, "id_token_add_organizations", PT_S("true"));
        param(&u, "codex_cli_simplified_flow", PT_S("true"));
        param(&u, "originator", PT_S("pytte"));
        if (u.oom || u.n >= ucap) {
            snprintf(err, cap, "out of memory");
            pt_buf_free(&u);
            goto fail;
        }
        memcpy(url, u.p, u.n);
        url[u.n] = 0;
        pt_buf_free(&u);
    }
    if (pipe(l->wake) < 0) {
        snprintf(err, cap, "cannot make a pipe: %s", strerror(errno));
        goto fail;
    }
    fcntl(l->wake[0], F_SETFD, FD_CLOEXEC);
    fcntl(l->wake[1], F_SETFD, FD_CLOEXEC);
    l->lfd = listen_local(); /* taken (the Codex CLI logging in?): paste the address instead */
    if (!(l->w = pt_worker_new()) || !pt_worker_start(l->w, login_job, l)) {
        snprintf(err, cap, "cannot start the login");
        goto fail;
    }
    pt_buf_free(&b);
    cli_open_url(url);
    return l;
fail:
    pt_buf_free(&b);
    cli_oauth_free(l);
    return NULL;
}

bool cli_oauth_listening(const cli_oauth *l)
{
    return l->lfd >= 0;
}

int cli_oauth_fd(const cli_oauth *l)
{
    return pt_worker_fd(l->w);
}

bool cli_oauth_paste(cli_oauth *l, pt_str answer)
{
    if (l->wake[1] < 0 || answer.n >= 4096)
        return false;
    return write(l->wake[1], answer.p, answer.n) == (ssize_t)answer.n;
}

bool cli_oauth_done(cli_oauth *l, bool *ok, char *msg, size_t cap)
{
    if (!pt_worker_finish(l->w))
        return false;
    *ok = l->ok;
    snprintf(msg, cap, "%s", l->msg);
    return true;
}

void cli_oauth_free(cli_oauth *l)
{
    if (!l)
        return;
    if (l->wake[1] >= 0)
        close(l->wake[1]); /* the job sees the end, and stops */
    if (l->w)
        pt_worker_free(l->w);
    if (l->wake[0] >= 0)
        close(l->wake[0]);
    if (l->lfd >= 0)
        close(l->lfd);
    free(l);
}
