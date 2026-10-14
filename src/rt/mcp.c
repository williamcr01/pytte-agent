/* The MCP client (Model Context Protocol, 2025-06-18): servers over stdio
 * (child processes speaking newline-delimited JSON-RPC) or Streamable
 * HTTP (a POST per message; the answer is JSON or an event stream).
 *
 * At the first step every server starts: initialize, then tools/list
 * (paged), prompts/list and resources/list as it offers them. Its tools
 * register in the runtime as mcp__<server>__<tool>. While a server starts
 * it holds the materials of a provisional epoch (until it is ready, fails,
 * or its startup time passes), so a new session's first request offers
 * its tools. A call of a tool whose server is still starting waits for
 * it; one whose server died starts it again.
 *
 * Calls send a progress token (the call's effect ID); progress shows as
 * the call's output. Cancelling sends notifications/cancelled. The model
 * sees text and the first image of a result; long text is cut like any
 * tool's output.
 *
 * Arguments the config fills in (pt_rt_mcp_server.arguments) leave a
 * tool's schema when it is listed, and are added to each call's. */
#define _POSIX_C_SOURCE 200809L
#include "rt/mcp_int.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IO_CHUNK 65536
#define LINE_MAX_BYTES (64u << 20) /* one message over stdio */
#define BODY_MAX (64u << 20)       /* one HTTP response */
#define DESC_MAX 2048              /* bytes of a tool's description kept */
#define RESTART_GAP_MS 5000        /* a server that just failed is not started again sooner */

typedef enum { P_INIT, P_TOOLS, P_PROMPTS, P_RESOURCES, P_CALL, P_GET, P_READ } pkind;

typedef struct pend {
    int64_t id;
    pkind kind;
    srv *s;
    uint64_t gen;
    uint32_t fx;       /* P_CALL */
    pt_rt_mcp_done fn; /* P_GET, P_READ */
    void *ud;
} pend;

typedef struct mcall {
    struct mcall *next;
    srv *s;
    uint32_t fx;
    int64_t id; /* the request's, once sent; 0: waiting for the server */
    uint32_t timer;
    char *tool; /* the model's name for it, until sent */
    char *args;
} mcall;

typedef struct qmsg { /* a message waiting to be POSTed */
    struct qmsg *next;
    int64_t id;
    bool init;
    size_t n;
    char data[1];
} qmsg;

static void send_msg(srv *s, pt_str msg, int64_t id, bool init);
static void srv_fail(srv *s, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
static void srv_stop(srv *s, const char *why);
static void srv_connect(srv *s, bool hold);
static void tools_apply(srv *s);
static void call_send(mcall *c);
static void call_end(mcall *c, pt_str text, bool error);

/* ---- small things ---- */

static char *dupz(pt_arena *a, pt_str s)
{
    return (char *)pt_arena_cstr(a, s);
}

void mcp_notify_user(srv *s, const char *fmt, ...)
{
    char msg[1200];
    va_list ap;
    if (!s->m->host.notify)
        return;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    s->m->host.notify(s->m->host.ud, s->name, msg);
}

static void release(srv *s)
{
    if (s->startup_timer)
        pt_rt_timer_cancel(s->m->rt, s->startup_timer);
    s->startup_timer = 0;
    if (s->holding) {
        s->holding = false;
        pt_rt_materials_hold(s->m->rt, false);
    }
}

static void startup_over(void *ud, pt_rt *rt)
{
    srv *s = (srv *)ud;
    (void)rt;
    s->startup_timer = 0;
    if (s->state == PT_MCP_STARTING)
        mcp_notify_user(s, "still starting after %u s: its tools are left out until the next session", s->startup_s);
    release(s);
}

static void list_reset(list *l)
{
    pt_arena_reset(&l->ar);
    l->n = 0;
}

static void *list_add(list *l, size_t elem)
{
    char *p;
    if (!pt_rt_grow(&l->v, &l->cap, l->n + 1, elem))
        return NULL;
    p = (char *)l->v + (size_t)l->n++ * elem;
    memset(p, 0, elem);
    return p;
}

static void list_swap(list *a, list *b)
{
    list t = *a;
    *a = *b;
    *b = t;
}

static void list_free(list *l)
{
    pt_arena_free(&l->ar);
    free(l->v);
    memset(l, 0, sizeof *l);
}

/* ---- pending requests ---- */

static pend *pend_add(srv *s, int64_t id, pkind kind)
{
    mcp *m = s->m;
    pend *p;
    if (!pt_rt_grow((void **)&m->pend, &m->pcap, m->npend + 1, sizeof *m->pend))
        return NULL;
    p = &m->pend[m->npend++];
    memset(p, 0, sizeof *p);
    p->id = id;
    p->kind = kind;
    p->s = s;
    p->gen = s->gen;
    return p;
}

static bool pend_take(mcp *m, srv *s, int64_t id, pend *out)
{
    uint32_t i;
    for (i = 0; i < m->npend; i++)
        if (m->pend[i].s == s && m->pend[i].id == id) {
            *out = m->pend[i];
            m->pend[i] = m->pend[--m->npend];
            return true;
        }
    return false;
}

static mcall *call_by_fx(mcp *m, uint32_t fx)
{
    mcall *c;
    for (c = m->calls; c; c = c->next)
        if (c->fx == fx)
            return c;
    return NULL;
}

static bool srv_busy(srv *s)
{
    mcall *c;
    for (c = s->m->calls; c; c = c->next)
        if (c->s == s)
            return true;
    return false;
}

/* What waited on a request that will not be answered. */
static void pend_fail(pend *p, const char *why)
{
    if (p->kind == P_CALL) {
        mcall *c = call_by_fx(p->s->m, p->fx);
        if (c)
            call_end(c, pt_strz(why), true);
    } else if ((p->kind == P_GET || p->kind == P_READ) && p->fn) {
        p->fn(p->ud, why, PT_STR_NULL);
    }
}

/* Fail every request of s; calls not sent yet wait on, unless all. */
static void fail_all(srv *s, const char *why, bool all)
{
    mcp *m = s->m;
    uint32_t i = 0;
    mcall *c, *next;
    while (i < m->npend)
        if (m->pend[i].s == s) {
            pend p = m->pend[i];
            m->pend[i] = m->pend[--m->npend];
            pend_fail(&p, why);
            i = 0;
        } else {
            i++;
        }
    for (c = m->calls; c; c = next) {
        next = c->next;
        if (c->s == s && (all || c->id))
            call_end(c, pt_strz(why), true);
    }
}

/* ---- sending ---- */

static int64_t request(srv *s, const char *method, pt_str params, pkind kind, pend **out)
{
    pt_buf b;
    int64_t id = ++s->next_id;
    pend *p = pend_add(s, id, kind);
    if (out)
        *out = p;
    if (!p)
        return 0;
    pt_buf_init(&b, pt_sys_allocator());
    mcp_request(&b, id, method, params);
    if (b.oom) {
        s->m->npend--;
        pt_buf_free(&b);
        return 0;
    }
    send_msg(s, pt_buf_str(&b), id, kind == P_INIT);
    pt_buf_free(&b);
    return id;
}

static void notify(srv *s, const char *method, pt_str params)
{
    pt_buf b;
    pt_buf_init(&b, pt_sys_allocator());
    mcp_notify(&b, method, params);
    if (!b.oom)
        send_msg(s, pt_buf_str(&b), 0, false);
    pt_buf_free(&b);
}

/* ---- stdio ---- */

static void flush_later(void *ud, pt_rt *rt);

static void stdio_flush(srv *s)
{
    while (s->outq.n) {
        long w = pt_proc_write(s->p, s->outq.p, s->outq.n);
        if (w < 0) {
            srv_fail(s, "its input closed: it stopped reading");
            return;
        }
        if (w == 0) { /* the pipe is full: try again soon */
            if (!s->flush_timer)
                s->flush_timer = pt_rt_timer(s->m->rt, pt_rt_now(s->m->rt) + 10, flush_later, s);
            return;
        }
        memmove(s->outq.p, s->outq.p + w, s->outq.n - (size_t)w);
        s->outq.n -= (size_t)w;
    }
}

static void flush_later(void *ud, pt_rt *rt)
{
    srv *s = (srv *)ud;
    (void)rt;
    s->flush_timer = 0;
    if (s->p)
        stdio_flush(s);
}

static void on_message(srv *s, pt_str json);

static void stdio_lines(srv *s, const char *p, size_t n)
{
    uint64_t gen = s->gen;
    while (n && s->p && s->gen == gen) {
        const char *nl = (const char *)memchr(p, '\n', n);
        size_t k = nl ? (size_t)(nl - p) : n;
        if (s->line.n + k > LINE_MAX_BYTES) {
            srv_fail(s, "sent a message longer than %u MiB", LINE_MAX_BYTES >> 20);
            return;
        }
        if (!nl) {
            pt_buf_put(&s->line, p, n);
            return;
        }
        if (s->line.n) {
            pt_buf_put(&s->line, p, k);
            if (s->line.oom) {
                srv_fail(s, "out of memory reading its output");
                return;
            }
            on_message(s, pt_buf_str(&s->line));
            pt_buf_reset(&s->line);
        } else {
            on_message(s, (pt_str){p, k});
        }
        p += k + 1;
        n -= k + 1;
    }
}

static void stdio_err(void *ud, pt_rt *rt);

static void stdio_died(srv *s)
{
    int status = 0;
    char tail[400] = "";
    size_t i, n, start;
    if (s->err_fd >= 0) /* what it said last, before its stdout closed */
        stdio_err(s, s->m->rt);
    n = s->errtail_n;
    pt_proc_reap(s->p, true, &status);
    /* the last lines of what it said on stderr */
    while (n && (s->errtail[n - 1] == '\n' || s->errtail[n - 1] == ' '))
        n--;
    start = n > 300 ? n - 300 : 0;
    for (i = start; i < n; i++)
        tail[i - start] = s->errtail[i] == '\n' ? ' ' : s->errtail[i];
    tail[n - start] = 0;
    srv_fail(s, "exited with status %d%s%s", status, tail[0] ? ": " : "", tail);
}

static void stdio_out(void *ud, pt_rt *rt)
{
    srv *s = (srv *)ud;
    char *io = s->m->io;
    (void)rt;
    while (s->p) {
        long r = pt_proc_read(s->p, io, IO_CHUNK);
        if (r < 0)
            return; /* nothing more now */
        if (r == 0) {
            stdio_died(s);
            return;
        }
        stdio_lines(s, io, (size_t)r);
    }
}

static void stdio_err(void *ud, pt_rt *rt)
{
    srv *s = (srv *)ud;
    char buf[4096];
    (void)rt;
    for (;;) {
        long r = s->p ? pt_proc_read_err(s->p, buf, sizeof buf) : 0;
        size_t k;
        if (r <= 0) {
            if (r == 0 && s->err_fd >= 0) { /* closed: stop watching */
                pt_rt_unwatch(s->m->rt, s->err_fd);
                s->err_fd = -1;
            }
            return;
        }
        k = (size_t)r;
        if (k >= sizeof s->errtail) {
            memcpy(s->errtail, buf + k - sizeof s->errtail, sizeof s->errtail);
            s->errtail_n = sizeof s->errtail;
        } else {
            if (s->errtail_n + k > sizeof s->errtail) {
                size_t drop = s->errtail_n + k - sizeof s->errtail;
                memmove(s->errtail, s->errtail + drop, s->errtail_n - drop);
                s->errtail_n -= drop;
            }
            memcpy(s->errtail + s->errtail_n, buf, k);
            s->errtail_n += k;
        }
    }
}

static void stdio_exit(void *ud, pt_rt *rt)
{
    srv *s = (srv *)ud;
    stdio_out(ud, rt); /* what it wrote before it ended */
    if (s->p)
        stdio_err(ud, rt);
    if (s->p)
        stdio_died(s);
}

static void stdio_close(srv *s)
{
    pt_rt *rt = s->m->rt;
    if (s->out_fd >= 0)
        pt_rt_unwatch(rt, s->out_fd);
    if (s->err_fd >= 0)
        pt_rt_unwatch(rt, s->err_fd);
    if (s->exit_fd >= 0)
        pt_rt_unwatch(rt, s->exit_fd);
    s->out_fd = s->err_fd = s->exit_fd = -1;
    if (s->flush_timer)
        pt_rt_timer_cancel(rt, s->flush_timer);
    s->flush_timer = 0;
    if (s->p) {
        pt_proc_close_in(s->p); /* EOF first: a polite server exits */
        pt_proc_free(s->p);     /* ...then SIGKILL to its group */
    }
    s->p = NULL;
    pt_buf_reset(&s->line);
    pt_buf_reset(&s->outq);
}

static bool stdio_start(srv *s)
{
    pt_rt *rt = s->m->rt;
    pt_proc_opts po;
    char err[300];
    const char *cwd = s->cwd;
    pt_str sc = pt_session_cwd(pt_rt_agent(rt));
    char dir[4096];
    if (!cwd && sc.n && sc.n < sizeof dir) {
        memcpy(dir, sc.p, sc.n);
        dir[sc.n] = 0;
        cwd = dir;
    }
    memset(&po, 0, sizeof po);
    po.stdin_pipe = true;
    po.split_err = true;
    s->errtail_n = 0;
    s->p = pt_proc_spawn(s->argv, cwd, s->m->unset, s->env, &po, err, sizeof err);
    if (!s->p) {
        srv_fail(s, "cannot start %s: %s", s->target, err);
        return false;
    }
    s->out_fd = pt_proc_out_fd(s->p);
    s->err_fd = pt_proc_err_fd(s->p);
    s->exit_fd = pt_proc_exit_fd(s->p);
    if (!pt_rt_watch(rt, s->out_fd, stdio_out, s) || !pt_rt_watch(rt, s->err_fd, stdio_err, s) ||
        (s->exit_fd >= 0 && !pt_rt_watch(rt, s->exit_fd, stdio_exit, s))) {
        srv_fail(s, "out of memory");
        return false;
    }
    return true;
}

/* ---- HTTP calls ---- */

static void h_header(void *ud, pt_str name, pt_str value)
{
    hcall *h = (hcall *)ud;
    size_t n;
    if (pt_str_ieq(name, PT_S("content-type"))) {
        n = value.n < sizeof h->ctype - 1 ? value.n : sizeof h->ctype - 1;
        memcpy(h->ctype, value.p, n);
        h->ctype[n] = 0;
    } else if (pt_str_ieq(name, PT_S("mcp-session-id"))) {
        n = value.n < sizeof h->session - 1 ? value.n : sizeof h->session - 1;
        memcpy(h->session, value.p, n);
        h->session[n] = 0;
    } else if (pt_str_ieq(name, PT_S("www-authenticate"))) {
        pt_buf_reset(&h->www);
        pt_buf_puts(&h->www, mcp_cut(value, 2048));
    }
}

static void h_head(void *ud, int status, pt_str retry_after)
{
    hcall *h = (hcall *)ud;
    (void)retry_after;
    h->status = status;
    h->sse_on = h->on_msg && status >= 200 && status < 300 && pt_str_starts(pt_strz(h->ctype), PT_S("text/event-stream"));
}

static int h_event(void *ud, const pt_sse_event *ev)
{
    hcall *h = (hcall *)ud;
    if ((!ev->event.n || pt_str_eqz(ev->event, "message")) && ev->data.n)
        h->on_msg(h, ev->data);
    return 0;
}

static void h_body(void *ud, const char *p, size_t n)
{
    hcall *h = (hcall *)ud;
    if (h->sse_on) {
        pt_sse_feed(&h->sse, p, n, h_event, h);
        return;
    }
    if (h->body.n + n > BODY_MAX) {
        h->too_big = true;
        return;
    }
    pt_buf_put(&h->body, p, n);
}

static void unlink_h(hcall *h)
{
    hcall **pp;
    for (pp = &h->m->hcalls; *pp; pp = &(*pp)->next)
        if (*pp == h) {
            *pp = h->next;
            break;
        }
}

static void h_fire(void *ud, pt_rt *rt)
{
    hcall *h = (hcall *)ud;
    (void)rt;
    h->timer = 0;
    /* off the list first: done may close what it belonged to */
    unlink_h(h);
    if (h->done)
        h->done(h);
    hcall_free(h);
}

static void h_done(void *ud, const char *error)
{
    hcall *h = (hcall *)ud;
    h->req = NULL;
    h->ended = true;
    if (h->sse_on)
        pt_sse_finish(&h->sse, h_event, h);
    if (error)
        snprintf(h->err, sizeof h->err, "%s", error);
    else if (h->too_big)
        snprintf(h->err, sizeof h->err, "the response is larger than %u MiB", BODY_MAX >> 20);
    h->timer = pt_rt_timer(h->m->rt, 0, h_fire, h);
    if (!h->timer) { /* out of memory: no done, but no leak */
        snprintf(h->err, sizeof h->err, "out of memory");
    }
}

hcall *hcall_new(mcp *m, srv *s, const char *method, pt_str url)
{
    hcall *h = (hcall *)calloc(1, sizeof *h);
    if (!h)
        return NULL;
    h->m = m;
    h->s = s;
    pt_arena_init(&h->ar, pt_sys_allocator(), 1024);
    pt_buf_init(&h->body_out, pt_sys_allocator());
    pt_buf_init(&h->body, pt_sys_allocator());
    pt_buf_init(&h->www, pt_sys_allocator());
    pt_sse_init(&h->sse, pt_sys_allocator());
    h->hr.method = method;
    h->hr.url = pt_arena_cstr(&h->ar, url);
    h->hr.headers = h->hdr;
    h->hr.hcap = sizeof h->hdr / sizeof h->hdr[0];
    h->hr.flags = PT_HTTP_DECOMPRESS;
    h->next = m->hcalls;
    m->hcalls = h;
    return h;
}

void hcall_header(hcall *h, const char *name, pt_str value)
{
    if (h->hr.nheaders < h->hr.hcap) {
        h->hdr[h->hr.nheaders].name = pt_arena_cstr(&h->ar, pt_strz(name));
        h->hdr[h->hr.nheaders++].value = pt_arena_cstr(&h->ar, value);
    }
}

void hcall_free(hcall *h)
{
    unlink_h(h);
    if (h->req)
        pt_rt_http_cancel(h->m->rt, h->req);
    if (h->timer)
        pt_rt_timer_cancel(h->m->rt, h->timer);
    pt_arena_free(&h->ar);
    pt_buf_free(&h->body_out);
    pt_buf_free(&h->body);
    pt_buf_free(&h->www);
    pt_sse_free(&h->sse);
    free(h);
}

bool hcall_start(hcall *h)
{
    pt_http_sink sink;
    if (h->ar.oom || h->body_out.oom) {
        hcall_free(h);
        return false;
    }
    h->hr.body = pt_buf_str(&h->body_out);
    memset(&sink, 0, sizeof sink);
    sink.ud = h;
    sink.head = h_head;
    sink.body = h_body;
    sink.done = h_done;
    sink.header = h_header;
    h->req = pt_rt_http_start(h->m->rt, &h->hr, &sink);
    if (!h->req) {
        hcall_free(h);
        return false;
    }
    return true;
}

/* ---- Streamable HTTP ---- */

static void post_msg(hcall *h, pt_str json)
{
    srv *s = h->s;
    if (s->gen == (uint64_t)(uintptr_t)h->ud)
        on_message(s, json);
}

static void post_done(hcall *h)
{
    srv *s = h->s;
    pend p;
    char msg[700];
    if (s->gen != (uint64_t)(uintptr_t)h->ud)
        return; /* sent before a restart */
    if (h->init && h->session[0])
        snprintf(s->session, sizeof s->session, "%s", h->session);
    if (h->err[0]) {
        snprintf(msg, sizeof msg, "%s: %s", s->url, h->err);
        if (h->init)
            srv_fail(s, "%s", msg);
        else if (h->msg_id && pend_take(s->m, s, h->msg_id, &p))
            pend_fail(&p, msg);
        return;
    }
    if (h->status == 401) {
        if (!mcp_auth_unauthorized(s, pt_buf_str(&h->www)))
            mcp_needs_login(s, "the server asks for a login");
        return;
    }
    if (h->status == 404 && s->session[0] && !h->init) {
        /* the session is gone (the server restarted): start a new one */
        s->session[0] = 0;
        fail_all(s, "the server ended its session; it was restarted: try again", false);
        mcp_reconnect(s);
        return;
    }
    if (h->status < 200 || h->status >= 300) {
        snprintf(msg, sizeof msg, "%s answered HTTP %d%s%.*s", s->url, h->status, h->body.n ? ": " : "",
                 (int)mcp_cut(pt_buf_str(&h->body), 300).n, h->body.p ? h->body.p : "");
        if (h->init)
            srv_fail(s, "%s", msg);
        else if (h->msg_id && pend_take(s->m, s, h->msg_id, &p))
            pend_fail(&p, msg);
        return;
    }
    if (!h->sse_on && h->body.n && pt_str_starts(pt_strz(h->ctype), PT_S("application/json")))
        on_message(s, pt_buf_str(&h->body));
    /* a request whose answer never came */
    if (s->gen == (uint64_t)(uintptr_t)h->ud && h->msg_id && pend_take(s->m, s, h->msg_id, &p)) {
        if (p.kind == P_INIT)
            srv_fail(s, "%s did not answer initialize", s->url);
        else
            pend_fail(&p, "the server sent no answer");
    }
}

static void post_flush(void *ud, pt_rt *rt)
{
    srv *s = (srv *)ud;
    const char *bearer;
    (void)rt;
    s->post_timer = 0;
    while (s->outbox) {
        qmsg *q = s->outbox;
        hcall *h;
        uint32_t i;
        s->outbox = q->next;
        h = hcall_new(s->m, s, "POST", pt_strz(s->url));
        if (!h) {
            free(q);
            srv_fail(s, "out of memory");
            return;
        }
        h->ud = (void *)(uintptr_t)s->gen;
        h->msg_id = q->id;
        h->init = q->init;
        h->on_msg = post_msg;
        h->done = post_done;
        pt_buf_put(&h->body_out, q->data, q->n);
        free(q);
        hcall_header(h, "Content-Type", PT_S("application/json"));
        hcall_header(h, "Accept", PT_S("application/json, text/event-stream"));
        if (s->version[0])
            hcall_header(h, "MCP-Protocol-Version", pt_strz(s->version));
        if (s->session[0])
            hcall_header(h, "Mcp-Session-Id", pt_strz(s->session));
        if ((bearer = mcp_auth_bearer(s)))
            hcall_header(h, "Authorization", pt_strz(bearer));
        for (i = 0; i < s->nheaders; i++)
            hcall_header(h, s->headers[i].name, pt_strz(s->headers[i].value));
        if (!hcall_start(h)) {
            srv_fail(s, "cannot start a request to %s", s->url);
            return;
        }
    }
}

static void http_close(srv *s)
{
    hcall *h, *next;
    for (h = s->m->hcalls; h; h = next) {
        next = h->next;
        if (h->s == s && h->done == post_done)
            hcall_free(h);
    }
    while (s->outbox) {
        qmsg *q = s->outbox;
        s->outbox = q->next;
        free(q);
    }
    if (s->post_timer)
        pt_rt_timer_cancel(s->m->rt, s->post_timer);
    s->post_timer = 0;
}

static void send_msg(srv *s, pt_str msg, int64_t id, bool init)
{
    if (!s->http) {
        if (!s->p)
            return;
        pt_buf_puts(&s->outq, msg);
        pt_buf_putc(&s->outq, '\n');
        if (s->outq.oom) {
            srv_fail(s, "out of memory");
            return;
        }
        if (!s->flush_timer)
            stdio_flush(s);
        return;
    }
    {
        qmsg *q = (qmsg *)malloc(sizeof *q + msg.n), **pp;
        if (!q) {
            srv_fail(s, "out of memory");
            return;
        }
        q->next = NULL;
        q->id = id;
        q->init = init;
        q->n = msg.n;
        memcpy(q->data, msg.p, msg.n);
        for (pp = &s->outbox; *pp; pp = &(*pp)->next)
            ;
        *pp = q;
        /* never from inside an HTTP callback: a timer starts the request */
        if (!s->post_timer)
            s->post_timer = pt_rt_timer(s->m->rt, 0, post_flush, s);
    }
}

/* ---- the server's life ---- */

static void srv_close(srv *s)
{
    if (s->http)
        http_close(s);
    else
        stdio_close(s);
    s->initialized = false;
    s->gen++;
}

static void set_failed(srv *s, pt_mcp_state st, const char *msg)
{
    snprintf(s->error, sizeof s->error, "%s", msg);
    srv_close(s);
    s->state = st;
    s->failed_at = pt_rt_now(s->m->rt);
    release(s);
    fail_all(s, msg, true);
}

static void srv_fail(srv *s, const char *fmt, ...)
{
    char msg[600];
    va_list ap;
    bool was_ready = s->state == PT_MCP_READY;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    set_failed(s, PT_MCP_FAILED, msg);
    mcp_notify_user(s, "%s%s", was_ready ? "stopped: " : "failed: ", msg);
}

void mcp_needs_login(srv *s, const char *why)
{
    char msg[300];
    snprintf(msg, sizeof msg, "%s: log in with /mcp login %s", why, s->name);
    set_failed(s, PT_MCP_AUTH, msg);
    mcp_notify_user(s, "%s", msg);
}

static void srv_stop(srv *s, const char *why)
{
    set_failed(s, PT_MCP_STOPPED, why);
}

static void srv_connect(srv *s, bool hold)
{
    pt_buf params;
    pt_rt *rt = s->m->rt;
    srv_close(s);
    s->state = PT_MCP_STARTING;
    s->error[0] = 0;
    s->session[0] = 0;
    s->version[0] = 0;
    s->cap_tools = s->cap_prompts = s->cap_resources = false;
    if (hold && !s->holding) {
        s->holding = true;
        pt_rt_materials_hold(rt, true);
        s->startup_timer = pt_rt_timer(rt, pt_rt_now(rt) + (int64_t)s->startup_s * 1000, startup_over, s);
    }
    if (s->http)
        mcp_auth_load(s);
    else if (!stdio_start(s))
        return;
    pt_buf_init(&params, pt_sys_allocator());
    mcp_init_params(&params);
    request(s, "initialize", pt_buf_str(&params), P_INIT, NULL);
    pt_buf_free(&params);
}

void mcp_reconnect(srv *s)
{
    fail_all(s, "the server is restarting", false);
    srv_connect(s, false);
}

/* ---- lists ---- */

static void list_request(srv *s, const char *method, pkind kind, pt_str cursor)
{
    pt_buf params;
    pt_buf_init(&params, pt_sys_allocator());
    if (cursor.n) {
        pt_buf_putz(&params, "{\"cursor\":");
        pt_json_quote(&params, cursor);
        pt_buf_putc(&params, '}');
    }
    request(s, method, pt_buf_str(&params), kind, NULL);
    pt_buf_free(&params);
}

static void ready(srv *s)
{
    bool first = s->state != PT_MCP_READY;
    s->state = PT_MCP_READY;
    s->error[0] = 0;
    if (first) {
        mcall *c, *next;
        uint32_t n = s->tools.n;
        release(s);
        mcp_notify_user(s, "ready: %u tool%s", n, n == 1 ? "" : "s");
        for (c = s->m->calls; c; c = next) { /* calls that waited for it */
            next = c->next;
            if (c->s == s && !c->id)
                call_send(c);
        }
    }
}

static void got_tools(srv *s, const pt_json *r)
{
    pt_arena *ma = &s->m->ar, *la = &s->tools_next.ar;
    const pt_json *t;
    pt_buf full;
    pt_buf_init(&full, pt_sys_allocator());
    for (t = pt_json_get(r, "tools") ? pt_json_get(r, "tools")->child : NULL; t; t = t->next) {
        const pt_json *schema = pt_json_get(t, "inputSchema"), *ann = pt_json_get(t, "annotations");
        pt_str name = pt_json_gets(t, "name", ma), desc = pt_json_gets(t, "description", ma);
        mtool *mt;
        uint32_t k;
        bool dup = false;
        if (!name.n)
            continue;
        pt_buf_reset(&full);
        mcp_tool_name(pt_strz(s->name), name, &full);
        for (k = 0; k < s->tools_next.n; k++)
            dup = dup || pt_str_eq(((mtool *)s->tools_next.v)[k].full, pt_buf_str(&full));
        if (dup || !(mt = (mtool *)list_add(&s->tools_next, sizeof *mt)))
            continue;
        if (!desc.n)
            desc = pt_json_gets(t, "title", ma);
        if (!desc.n)
            desc = pt_json_gets(ann, "title", ma);
        mt->name = pt_arena_dup(la, name);
        mt->full = pt_arena_dup(la, pt_buf_str(&full));
        mt->desc = pt_arena_dup(la, mcp_cut(desc.n ? desc : name, DESC_MAX));
        if (!pt_json_is(schema, PT_J_OBJ))
            schema = NULL;
        mt->schema = pt_arena_dup(la, schema ? schema->raw : PT_S("{\"type\":\"object\"}"));
        pt_buf_reset(&full);
        if (mcp_fill_of(s->fill, name, schema, &full)) {
            mt->fill = pt_arena_dup(la, pt_buf_str(&full));
            pt_buf_reset(&full);
            if (schema && mcp_schema_strip(schema, mt->fill, &full, ma))
                mt->schema = pt_arena_dup(la, pt_buf_str(&full));
        }
        mt->ro = s->trust && pt_json_truthy(pt_json_get(ann, "readOnlyHint"));
        mt->safe = s->trust && (mt->ro || pt_json_truthy(pt_json_get(ann, "idempotentHint")));
    }
    pt_buf_free(&full);
}

static void got_prompts(srv *s, const pt_json *r)
{
    pt_arena *ma = &s->m->ar, *la = &s->prompts_next.ar;
    const pt_json *t;
    for (t = pt_json_get(r, "prompts") ? pt_json_get(r, "prompts")->child : NULL; t; t = t->next) {
        const pt_json *args = pt_json_get(t, "arguments"), *a;
        pt_rt_mcp_prompt *p;
        pt_rt_mcp_arg *av;
        pt_str name = pt_json_gets(t, "name", ma), desc = pt_json_gets(t, "description", ma);
        uint32_t k = 0;
        if (!name.n || !(p = (pt_rt_mcp_prompt *)list_add(&s->prompts_next, sizeof *p)))
            continue;
        if (!desc.n)
            desc = pt_json_gets(t, "title", ma);
        p->server = pt_strz(s->name);
        p->name = pt_arena_dup(la, name);
        p->description = pt_arena_dup(la, mcp_cut(desc, 300));
        if (!pt_json_is(args, PT_J_ARR) || !args->len)
            continue;
        av = (pt_rt_mcp_arg *)pt_arena_zalloc(la, sizeof *av * args->len);
        for (a = av ? args->child : NULL; a; a = a->next) {
            av[k].name = pt_arena_dup(la, pt_json_gets(a, "name", ma));
            av[k].description = pt_arena_dup(la, mcp_cut(pt_json_gets(a, "description", ma), 200));
            av[k].required = pt_json_truthy(pt_json_get(a, "required"));
            k += av[k].name.n > 0;
        }
        p->args = av;
        p->nargs = k;
    }
}

static void got_resources(srv *s, const pt_json *r)
{
    pt_arena *ma = &s->m->ar, *la = &s->resources_next.ar;
    const pt_json *t;
    for (t = pt_json_get(r, "resources") ? pt_json_get(r, "resources")->child : NULL; t; t = t->next) {
        pt_rt_mcp_resource *x;
        pt_str uri = pt_json_gets(t, "uri", ma);
        if (!uri.n || s->resources_next.n >= 1000 || !(x = (pt_rt_mcp_resource *)list_add(&s->resources_next, sizeof *x)))
            continue;
        x->server = pt_strz(s->name);
        x->uri = pt_arena_dup(la, uri);
        x->name = pt_arena_dup(la, pt_json_gets(t, "name", ma));
        x->description = pt_arena_dup(la, mcp_cut(pt_json_gets(t, "description", ma), 300));
        x->mime = pt_arena_dup(la, pt_json_gets(t, "mimeType", ma));
    }
}

/* Register the listed tools in place of the old ones, unless calls run. */
static void tools_apply(srv *s)
{
    pt_rt *rt = s->m->rt;
    uint32_t i;
    if (srv_busy(s)) {
        s->tools_pending = true;
        return;
    }
    s->tools_pending = false;
    for (i = 0; i < s->tools.n; i++)
        pt_rt_tool_remove(rt, ((mtool *)s->tools.v)[i].full);
    list_swap(&s->tools, &s->tools_next);
    list_reset(&s->tools_next);
    for (i = 0; i < s->tools.n; i++) {
        mtool *mt = &((mtool *)s->tools.v)[i];
        pt_rt_tool t;
        memset(&t, 0, sizeof t);
        t.def.name = mt->full;
        t.def.description = mt->desc;
        t.def.schema = mt->schema;
        t.def.replay = mt->safe ? PT_REPLAY_SAFE : PT_REPLAY_UNSAFE;
        t.def.parallel = true;
        t.def.read_only = mt->ro;
        {
            char src[48];
            snprintf(src, sizeof src, "mcp:%s", s->name);
            t.def.source = pt_arena_dup(&s->tools.ar, pt_strz(src));
        }
        t.run = rt->fallback.run;
        t.cancel = rt->fallback.cancel;
        t.ud = s->m;
        if (!pt_rt_tool_add(rt, &t)) {
            srv_fail(s, "out of memory registering its tools");
            return;
        }
    }
}

/* ---- messages ---- */

static void server_request(srv *s, const mcp_msg *msg)
{
    pt_buf b;
    pt_buf_init(&b, pt_sys_allocator());
    if (pt_str_eqz(msg->method, "ping")) {
        mcp_reply(&b, msg->id, PT_STR_NULL);
    } else if (pt_str_eqz(msg->method, "roots/list")) {
        pt_str cwd = pt_session_cwd(pt_rt_agent(s->m->rt));
        pt_buf r;
        pt_buf_init(&r, pt_sys_allocator());
        pt_buf_putz(&r, "{\"roots\":[");
        if (cwd.n) {
            pt_buf u;
            pt_buf_init(&u, pt_sys_allocator());
            pt_buf_putz(&u, "file://");
            pt_buf_puts(&u, cwd);
            pt_buf_putz(&r, "{\"uri\":");
            pt_json_quote(&r, pt_buf_str(&u));
            pt_buf_putz(&r, ",\"name\":");
            pt_json_quote(&r, cwd);
            pt_buf_putc(&r, '}');
            pt_buf_free(&u);
        }
        pt_buf_putz(&r, "]}");
        mcp_reply(&b, msg->id, pt_buf_str(&r));
        pt_buf_free(&r);
    } else {
        mcp_reply_error(&b, msg->id, -32601, "pytte does not support this request");
    }
    if (!b.oom)
        send_msg(s, pt_buf_str(&b), 0, false);
    pt_buf_free(&b);
}

static void server_notify(srv *s, const mcp_msg *msg)
{
    pt_arena *a = &s->m->ar;
    if (pt_str_eqz(msg->method, "notifications/progress")) {
        int64_t tok;
        mcall *c;
        if (pt_json_int(pt_json_get(msg->params, "progressToken"), &tok) && tok > 0 && tok <= UINT32_MAX &&
            (c = call_by_fx(s->m, (uint32_t)tok)) && c->s == s) {
            const pt_json *pr = pt_json_get(msg->params, "progress"), *tot = pt_json_get(msg->params, "total");
            pt_str text = pt_json_gets(msg->params, "message", a);
            pt_buf b;
            pt_buf_init(&b, pt_sys_allocator());
            pt_buf_putz(&b, "progress");
            if (pr && pr->type == PT_J_NUM) {
                pt_buf_putc(&b, ' ');
                pt_buf_puts(&b, mcp_cut(pr->raw, 24));
                if (tot && tot->type == PT_J_NUM) {
                    pt_buf_putc(&b, '/');
                    pt_buf_puts(&b, mcp_cut(tot->raw, 24));
                }
            }
            if (text.n) {
                pt_buf_putz(&b, ": ");
                pt_buf_puts(&b, mcp_cut(text, 500));
            }
            pt_buf_putc(&b, '\n');
            if (!b.oom)
                pt_rt_tool_output(s->m->rt, c->fx, b.p, b.n);
            pt_buf_free(&b);
        }
    } else if (s->state == PT_MCP_READY && pt_str_eqz(msg->method, "notifications/tools/list_changed")) {
        list_reset(&s->tools_next);
        list_request(s, "tools/list", P_TOOLS, PT_STR_NULL);
    } else if (s->state == PT_MCP_READY && pt_str_eqz(msg->method, "notifications/prompts/list_changed")) {
        list_reset(&s->prompts_next);
        list_request(s, "prompts/list", P_PROMPTS, PT_STR_NULL);
    } else if (s->state == PT_MCP_READY && pt_str_eqz(msg->method, "notifications/resources/list_changed")) {
        list_reset(&s->resources_next);
        list_request(s, "resources/list", P_RESOURCES, PT_STR_NULL);
    }
}

static void got_init(srv *s, const pt_json *r)
{
    pt_arena *a = &s->m->ar;
    const pt_json *caps = pt_json_get(r, "capabilities");
    pt_str v = pt_json_gets(r, "protocolVersion", a);
    snprintf(s->version, sizeof s->version, "%.*s", (int)(v.n < 31 ? v.n : 31), v.p ? v.p : "");
    s->cap_tools = pt_json_get(caps, "tools") != NULL;
    s->cap_prompts = pt_json_get(caps, "prompts") != NULL;
    s->cap_resources = pt_json_get(caps, "resources") != NULL;
    s->initialized = true;
    notify(s, "notifications/initialized", PT_STR_NULL);
    if (s->cap_prompts) {
        list_reset(&s->prompts_next);
        list_request(s, "prompts/list", P_PROMPTS, PT_STR_NULL);
    }
    if (s->cap_resources) {
        list_reset(&s->resources_next);
        list_request(s, "resources/list", P_RESOURCES, PT_STR_NULL);
    }
    list_reset(&s->tools_next);
    if (s->cap_tools)
        list_request(s, "tools/list", P_TOOLS, PT_STR_NULL);
    else {
        tools_apply(s);
        ready(s);
    }
}

static void call_result(srv *s, mcall *c, const pt_json *r);

static void response(srv *s, const mcp_msg *msg)
{
    pt_arena *a = &s->m->ar;
    pend p;
    pt_str cursor;
    if (!msg->numeric || !pend_take(s->m, s, msg->nid, &p) || p.gen != s->gen)
        return; /* not ours, or from before a restart */
    if (msg->is_error) {
        char why[480]; /* 400 of the message and the code: "listing tools: " fits in s->error */
        snprintf(why, sizeof why, "%.*s (MCP error %lld)", (int)mcp_cut(msg->err_message, 400).n,
                 msg->err_message.p, (long long)msg->err_code);
        if (p.kind == P_INIT)
            srv_fail(s, "initialize: %s", why);
        else if (p.kind == P_TOOLS) { /* no tools, then; and why, so it is not a silent 0 */
            tools_apply(s);
            ready(s);
            snprintf(s->error, sizeof s->error, "listing tools: %s", why);
            mcp_notify_user(s, "%s", s->error);
        }
        else
            pend_fail(&p, why);
        return;
    }
    cursor = pt_json_gets(msg->result, "nextCursor", a);
    switch (p.kind) {
    case P_INIT:
        got_init(s, msg->result);
        break;
    case P_TOOLS:
        got_tools(s, msg->result);
        if (cursor.n) {
            list_request(s, "tools/list", P_TOOLS, cursor);
        } else {
            tools_apply(s);
            if (s->state == PT_MCP_STARTING)
                ready(s);
        }
        break;
    case P_PROMPTS:
        got_prompts(s, msg->result);
        if (cursor.n) {
            list_request(s, "prompts/list", P_PROMPTS, cursor);
        } else {
            list_swap(&s->prompts, &s->prompts_next);
            list_reset(&s->prompts_next);
        }
        break;
    case P_RESOURCES:
        got_resources(s, msg->result);
        if (cursor.n && s->resources_next.n < 1000) {
            list_request(s, "resources/list", P_RESOURCES, cursor);
        } else {
            list_swap(&s->resources, &s->resources_next);
            list_reset(&s->resources_next);
        }
        break;
    case P_CALL: {
        mcall *c = call_by_fx(s->m, p.fx);
        if (c)
            call_result(s, c, msg->result);
        break;
    }
    case P_GET:
    case P_READ: {
        pt_buf text;
        pt_buf_init(&text, pt_sys_allocator());
        if (p.kind == P_GET)
            mcp_prompt_text(msg->result, a, &text);
        else
            mcp_resource_text(msg->result, a, &text);
        if (p.fn)
            p.fn(p.ud, text.oom ? "out of memory" : NULL, pt_buf_str(&text));
        pt_buf_free(&text);
        break;
    }
    }
}

static void on_message(srv *s, pt_str json)
{
    mcp *m = s->m;
    mcp_msg msg;
    json = pt_str_trim(json);
    if (!json.n)
        return;
    if (json.p[0] == '[') { /* a batch (2025-03-26) */
        pt_arena tmp;
        const pt_json *arr, *e;
        pt_arena_init(&tmp, pt_sys_allocator(), 0);
        arr = pt_json_parse(json, &tmp, NULL);
        for (e = pt_json_is(arr, PT_J_ARR) ? arr->child : NULL; e; e = e->next)
            on_message(s, e->raw);
        pt_arena_free(&tmp);
        return;
    }
    pt_arena_reset(&m->ar);
    if (!mcp_parse(json, &m->ar, &msg))
        return; /* not JSON-RPC: a server's stray output */
    switch (msg.kind) {
    case MCP_RESPONSE:
        response(s, &msg);
        break;
    case MCP_REQUEST:
        server_request(s, &msg);
        break;
    case MCP_NOTIFY:
        server_notify(s, &msg);
        break;
    default:
        break;
    }
}

/* ---- tool calls ---- */

static void call_free(mcall *c)
{
    mcall **pp;
    for (pp = &c->s->m->calls; *pp; pp = &(*pp)->next)
        if (*pp == c) {
            *pp = c->next;
            break;
        }
    if (c->timer)
        pt_rt_timer_cancel(c->s->m->rt, c->timer);
    free(c->tool);
    free(c->args);
    free(c);
}

/* End a call with text; the tool list waiting on it may apply. */
static void call_end(mcall *c, pt_str text, bool error)
{
    srv *s = c->s;
    pt_rt *rt = s->m->rt;
    uint32_t fx = c->fx;
    pt_rt_out r;
    pend p;
    if (c->id)
        pend_take(s->m, s, c->id, &p);
    call_free(c);
    memset(&r, 0, sizeof r);
    r.content = text.n ? text : PT_S("(no output)");
    r.is_error = error;
    pt_rt_tool_done(rt, fx, &r);
    if (s->tools_pending && !srv_busy(s))
        tools_apply(s);
}

static void call_result(srv *s, mcall *c, const pt_json *res)
{
    pt_rt *rt = s->m->rt;
    pt_arena *a = &s->m->ar;
    pt_buf text, img;
    mcp_result mr;
    pt_rt_out r;
    char ref[72];
    uint32_t fx = c->fx;
    pt_buf_init(&text, pt_sys_allocator());
    pt_buf_init(&img, pt_sys_allocator());
    mcp_call_result(res, a, &text, &mr);
    memset(&r, 0, sizeof r);
    if (mr.image_b64.n) {
        const char *mime = NULL;
        if (mr.image_b64.n / 4 * 3 > PT_RT_IMAGE_MAX + 3 || !pt_base64_decode(mr.image_b64, &img) ||
            !(mime = pt_rt_image_type((const unsigned char *)img.p, img.n)) || !pt_rt_put_blob(rt, img.p, img.n, ref)) {
            if (text.n)
                pt_buf_putc(&text, '\n');
            pt_buf_putz(&text, "[an image (");
            pt_buf_puts(&text, mr.image_mime);
            pt_buf_putz(&text, ") the model cannot be shown]");
        } else {
            r.image = pt_strz(ref);
            r.image_mime = pt_strz(mime);
        }
    }
    if (c->id) {
        pend p;
        pend_take(s->m, s, c->id, &p);
    }
    c->id = 0;
    call_free(c);
    r.content = text.oom ? PT_S("out of memory") : text.n ? pt_buf_str(&text) : PT_S("(no output)");
    r.is_error = mr.is_error || text.oom;
    pt_rt_tool_done(rt, fx, &r);
    pt_buf_free(&text);
    pt_buf_free(&img);
    if (s->tools_pending && !srv_busy(s))
        tools_apply(s);
}

static void call_timeout(void *ud, pt_rt *rt)
{
    mcall *c = (mcall *)ud;
    char msg[200];
    (void)rt;
    c->timer = 0;
    if (c->id && c->s->initialized) {
        pt_buf b;
        pt_buf_init(&b, pt_sys_allocator());
        pt_buf_putz(&b, "{\"requestId\":");
        pt_buf_i64(&b, c->id);
        pt_buf_putz(&b, ",\"reason\":\"timed out\"}");
        notify(c->s, "notifications/cancelled", pt_buf_str(&b));
        pt_buf_free(&b);
    }
    snprintf(msg, sizeof msg, "the MCP server %s did not answer within %u seconds", c->s->name, c->s->timeout_s);
    call_end(c, pt_strz(msg), true);
}

static const mtool *find_tool(srv *s, pt_str full)
{
    uint32_t i;
    for (i = 0; i < s->tools.n; i++)
        if (pt_str_eq(((mtool *)s->tools.v)[i].full, full))
            return &((mtool *)s->tools.v)[i];
    return NULL;
}

static void call_send(mcall *c)
{
    srv *s = c->s;
    const mtool *t = find_tool(s, pt_strz(c->tool));
    pt_buf params;
    pend *p;
    char msg[300];
    if (!t) {
        snprintf(msg, sizeof msg, "tool_unavailable: the MCP server %s has no tool %s", s->name, c->tool);
        call_end(c, pt_strz(msg), true);
        return;
    }
    pt_buf_init(&params, pt_sys_allocator());
    pt_buf_putz(&params, "{\"name\":");
    pt_json_quote(&params, t->name);
    pt_buf_putz(&params, ",\"arguments\":");
    if (t->fill.n) {
        pt_arena tmp;
        pt_arena_init(&tmp, pt_sys_allocator(), 0);
        mcp_fill_args(t->fill, pt_strz(c->args), pt_session_model(pt_rt_agent(s->m->rt)), pt_strz(s->m->rt->session),
                      &params, &tmp);
        if (tmp.oom)
            params.oom = true;
        pt_arena_free(&tmp);
    } else
        pt_buf_putz(&params, c->args && c->args[0] ? c->args : "{}");
    pt_buf_putz(&params, ",\"_meta\":{\"progressToken\":");
    pt_buf_u64(&params, c->fx);
    pt_buf_putz(&params, "}}");
    if (params.oom) {
        pt_buf_free(&params);
        call_end(c, PT_S("out of memory"), true);
        return;
    }
    c->id = request(s, "tools/call", pt_buf_str(&params), P_CALL, &p);
    pt_buf_free(&params);
    if (!c->id || !p) {
        call_end(c, PT_S("out of memory"), true);
        return;
    }
    p->fx = c->fx;
}

static srv *srv_of_tool(mcp *m, pt_str name)
{
    uint32_t i;
    for (i = 0; i < m->nsrvs; i++)
        if (pt_str_starts(name, pt_strz(m->srvs[i]->prefix)))
            return m->srvs[i];
    return NULL;
}

static void t_run(void *ud, pt_rt *rt, const pt_fx *fx)
{
    mcp *m = (mcp *)ud;
    srv *s = srv_of_tool(m, fx->name);
    mcall *c;
    pt_rt_out r;
    char msg[800];
    memset(&r, 0, sizeof r);
    r.is_error = true;
    if (!s) {
        r.content = PT_S("tool_unavailable: this tool is not available here.");
        pt_rt_tool_done(rt, fx->id, &r);
        return;
    }
    if (s->state == PT_MCP_AUTH) {
        snprintf(msg, sizeof msg, "the MCP server %s needs a login first: the user can run /mcp login %s", s->name,
                 s->name);
        r.content = pt_strz(msg);
        pt_rt_tool_done(rt, fx->id, &r);
        return;
    }
    if ((s->state == PT_MCP_FAILED || s->state == PT_MCP_STOPPED) && pt_rt_now(rt) - s->failed_at < RESTART_GAP_MS) {
        snprintf(msg, sizeof msg, "the MCP server %s is not running: %s", s->name, s->error);
        r.content = pt_strz(msg);
        pt_rt_tool_done(rt, fx->id, &r);
        return;
    }
    c = (mcall *)calloc(1, sizeof *c);
    if (c) {
        c->tool = (char *)malloc(fx->name.n + 1);
        c->args = (char *)malloc(fx->args.n + 1);
    }
    if (!c || !c->tool || !c->args) {
        if (c) {
            free(c->tool);
            free(c->args);
        }
        free(c);
        r.content = PT_S("out of memory");
        pt_rt_tool_done(rt, fx->id, &r);
        return;
    }
    memcpy(c->tool, fx->name.p, fx->name.n);
    c->tool[fx->name.n] = 0;
    memcpy(c->args, fx->args.p, fx->args.n);
    c->args[fx->args.n] = 0;
    c->s = s;
    c->fx = fx->id;
    c->next = m->calls;
    m->calls = c;
    c->timer = pt_rt_timer(rt, pt_rt_now(rt) + (int64_t)s->timeout_s * 1000, call_timeout, c);
    if (s->state == PT_MCP_READY) {
        call_send(c);
    } else if (s->state != PT_MCP_STARTING) {
        pt_rt_tool_output(rt, fx->id, "starting the MCP server again\n", 30);
        srv_connect(s, false); /* the call waits for it */
    }
}

static void t_cancel(void *ud, pt_rt *rt, uint32_t fx)
{
    mcp *m = (mcp *)ud;
    mcall *c = call_by_fx(m, fx);
    (void)rt;
    if (!c)
        return;
    if (c->id && c->s->initialized) {
        pt_buf b;
        pt_buf_init(&b, pt_sys_allocator());
        pt_buf_putz(&b, "{\"requestId\":");
        pt_buf_i64(&b, c->id);
        pt_buf_putz(&b, ",\"reason\":\"cancelled by the user\"}");
        notify(c->s, "notifications/cancelled", pt_buf_str(&b));
        pt_buf_free(&b);
    }
    call_end(c, PT_S("cancelled"), true);
}

/* ---- setup and the public API ---- */

static void mcp_start(void *ud)
{
    mcp *m = (mcp *)ud;
    uint32_t i;
    for (i = 0; i < m->nsrvs; i++)
        srv_connect(m->srvs[i], true);
}

static void srv_free(srv *s)
{
    srv_close(s);
    release(s);
    mcp_auth_free(s);
    list_free(&s->tools);
    list_free(&s->tools_next);
    list_free(&s->prompts);
    list_free(&s->prompts_next);
    list_free(&s->resources);
    list_free(&s->resources_next);
    pt_buf_free(&s->line);
    pt_buf_free(&s->outq);
    pt_arena_free(&s->cfg);
    free(s);
}

static void mcp_free(void *ud)
{
    mcp *m = (mcp *)ud;
    uint32_t i;
    while (m->calls) /* stopped already; nothing should be left */
        call_free(m->calls);
    for (i = 0; i < m->nsrvs; i++)
        srv_free(m->srvs[i]);
    while (m->hcalls)
        hcall_free(m->hcalls);
    free(m->srvs);
    free(m->pend);
    free(m->io);
    free(m->unset);
    free(m->infos);
    free(m->agg_prompts);
    free(m->agg_resources);
    pt_arena_free(&m->ar);
    free(m);
}

static srv *srv_new(mcp *m, const pt_rt_mcp_server *c)
{
    srv *s = (srv *)calloc(1, sizeof *s);
    pt_arena *a;
    pt_buf t;
    uint32_t i;
    if (!s)
        return NULL;
    s->m = m;
    a = &s->cfg;
    pt_arena_init(a, pt_sys_allocator(), 1024);
    pt_arena_init(&s->tools.ar, pt_sys_allocator(), 0);
    pt_arena_init(&s->tools_next.ar, pt_sys_allocator(), 0);
    pt_arena_init(&s->prompts.ar, pt_sys_allocator(), 0);
    pt_arena_init(&s->prompts_next.ar, pt_sys_allocator(), 0);
    pt_arena_init(&s->resources.ar, pt_sys_allocator(), 0);
    pt_arena_init(&s->resources_next.ar, pt_sys_allocator(), 0);
    pt_buf_init(&s->line, pt_sys_allocator());
    pt_buf_init(&s->outq, pt_sys_allocator());
    s->out_fd = s->err_fd = s->exit_fd = -1;
    s->state = PT_MCP_STOPPED;
    s->name = dupz(a, pt_strz(c->name));
    s->http = c->url != NULL;
    s->url = c->url ? dupz(a, pt_strz(c->url)) : NULL;
    s->cwd = c->cwd ? dupz(a, pt_strz(c->cwd)) : NULL;
    s->client_id = c->client_id ? dupz(a, pt_strz(c->client_id)) : NULL;
    s->client_secret = c->client_secret ? dupz(a, pt_strz(c->client_secret)) : NULL;
    s->scope = c->scope ? dupz(a, pt_strz(c->scope)) : NULL;
    s->timeout_s = c->timeout_s ? c->timeout_s : 600;
    s->startup_s = c->startup_s ? c->startup_s : 15;
    s->trust = c->trust_hints;
    if (c->arguments) {
        pt_json_err e;
        const pt_json *f = pt_json_parse(pt_arena_dup(a, pt_strz(c->arguments)), a, &e);
        s->fill = pt_json_is(f, PT_J_OBJ) ? f : NULL;
    }
    snprintf(s->prefix, sizeof s->prefix, "mcp__%s__", s->name);
    /* sh finds the command on PATH: exec "$0" "$@" */
    s->argv = (const char **)pt_arena_zalloc(a, sizeof *s->argv * (c->nargs + 5));
    s->env = (const char **)pt_arena_zalloc(a, sizeof *s->env * (c->nenv + 1));
    s->headers = (pt_header *)pt_arena_zalloc(a, sizeof *s->headers * (c->nheaders + 1));
    if (!s->argv || !s->env || !s->headers) {
        srv_free(s);
        return NULL;
    }
    pt_buf_init(&t, pt_sys_allocator());
    if (!s->http) {
        s->argv[0] = "/bin/sh";
        s->argv[1] = "-c";
        s->argv[2] = "exec \"$0\" \"$@\"";
        s->argv[3] = dupz(a, pt_strz(c->command ? c->command : ""));
        pt_buf_putz(&t, c->command ? c->command : "");
        for (i = 0; i < c->nargs; i++) {
            s->argv[4 + i] = dupz(a, pt_strz(c->args[i]));
            pt_buf_putc(&t, ' ');
            pt_buf_putz(&t, c->args[i]);
        }
        for (i = 0; i < c->nenv; i++)
            s->env[i] = dupz(a, pt_strz(c->env[i]));
    } else {
        pt_buf_putz(&t, c->url);
        for (i = 0; i < c->nheaders; i++) {
            s->headers[i].name = dupz(a, pt_strz(c->headers[i].name));
            s->headers[i].value = dupz(a, pt_strz(c->headers[i].value));
        }
        s->nheaders = c->nheaders;
    }
    s->target = dupz(a, mcp_cut(pt_buf_str(&t), 400));
    pt_buf_free(&t);
    if (a->oom) {
        srv_free(s);
        return NULL;
    }
    return s;
}

static void add_srvs(mcp *m, const pt_rt_mcp_server *servers, uint32_t n, bool *ok)
{
    uint32_t i;
    srv **ns = (srv **)realloc(m->srvs, sizeof *m->srvs * (m->nsrvs + n));
    if (!ns) {
        *ok = false;
        return;
    }
    m->srvs = ns;
    for (i = 0; i < n; i++) {
        srv *s = srv_new(m, &servers[i]);
        if (!s) {
            *ok = false;
            return;
        }
        m->srvs[m->nsrvs++] = s;
        if (m->rt->mcp.started) /* added late: no epoch waits for it */
            srv_connect(s, false);
    }
}

bool pt_rt_mcp_setup(pt_rt *rt, const pt_rt_mcp_host *host, const pt_rt_mcp_server *servers, uint32_t n)
{
    const pt_ai_catalog *cat = rt->c.catalog ? rt->c.catalog : pt_ai_builtin_catalog();
    mcp *m = (mcp *)rt->mcp.ud;
    uint32_t i, k, nu = 0;
    bool ok = true;
    pt_rt_tool fb;
    if (!n)
        return true;
    if (m) {
        add_srvs(m, servers, n, &ok);
        return ok;
    }
    m = (mcp *)calloc(1, sizeof *m);
    if (!m)
        return false;
    m->rt = rt;
    if (host)
        m->host = *host;
    pt_arena_init(&m->ar, pt_sys_allocator(), 4096);
    m->io = (char *)malloc(IO_CHUNK);
    m->unset = (const char **)calloc((size_t)cat->nproviders * 4 + 1, sizeof *m->unset);
    if (!m->io || !m->unset) {
        mcp_free(m);
        return false;
    }
    for (i = 0; i < cat->nproviders; i++)
        for (k = 0; k < 4 && cat->providers[i].key_env[k]; k++)
            m->unset[nu++] = cat->providers[i].key_env[k];
    add_srvs(m, servers, n, &ok);
    if (!ok) {
        mcp_free(m);
        return false;
    }
    rt->mcp.ud = m;
    rt->mcp.free = mcp_free;
    rt->mcp.start = mcp_start;
    memset(&fb, 0, sizeof fb);
    fb.def.name = PT_S("mcp__");
    fb.run = t_run;
    fb.cancel = t_cancel;
    fb.ud = m;
    pt_rt_tool_fallback(rt, &fb);
    return true;
}

static srv *find_srv(pt_rt *rt, pt_str name)
{
    mcp *m = (mcp *)rt->mcp.ud;
    uint32_t i;
    for (i = 0; m && i < m->nsrvs; i++)
        if (pt_str_eqz(name, m->srvs[i]->name))
            return m->srvs[i];
    return NULL;
}

uint32_t pt_rt_mcp_servers(pt_rt *rt, pt_rt_mcp_info *out, uint32_t max)
{
    mcp *m = (mcp *)rt->mcp.ud;
    uint32_t i;
    for (i = 0; m && i < m->nsrvs && i < max; i++) {
        srv *s = m->srvs[i];
        out[i].name = s->name;
        out[i].transport = s->http ? "http" : "stdio";
        out[i].target = s->target;
        out[i].state = s->state;
        out[i].error = s->error;
        out[i].ntools = s->tools.n;
        out[i].nprompts = s->prompts.n;
        out[i].nresources = s->resources.n;
    }
    return m ? m->nsrvs : 0;
}

const pt_rt_mcp_prompt *pt_rt_mcp_prompts(pt_rt *rt, uint32_t *n)
{
    mcp *m = (mcp *)rt->mcp.ud;
    uint32_t i, k = 0;
    *n = 0;
    for (i = 0; m && i < m->nsrvs; i++) {
        srv *s = m->srvs[i];
        if (s->state != PT_MCP_READY || !s->prompts.n)
            continue;
        if (!pt_rt_grow((void **)&m->agg_prompts, &m->apcap, k + s->prompts.n, sizeof *m->agg_prompts))
            break;
        memcpy(m->agg_prompts + k, s->prompts.v, s->prompts.n * sizeof *m->agg_prompts);
        k += s->prompts.n;
    }
    *n = k;
    return m ? m->agg_prompts : NULL;
}

const pt_rt_mcp_resource *pt_rt_mcp_resources(pt_rt *rt, uint32_t *n)
{
    mcp *m = (mcp *)rt->mcp.ud;
    uint32_t i, k = 0;
    *n = 0;
    for (i = 0; m && i < m->nsrvs; i++) {
        srv *s = m->srvs[i];
        if (s->state != PT_MCP_READY || !s->resources.n)
            continue;
        if (!pt_rt_grow((void **)&m->agg_resources, &m->arcap, k + s->resources.n, sizeof *m->agg_resources))
            break;
        memcpy(m->agg_resources + k, s->resources.v, s->resources.n * sizeof *m->agg_resources);
        k += s->resources.n;
    }
    *n = k;
    return m ? m->agg_resources : NULL;
}

static bool ask(pt_rt *rt, pt_str server, const char *method, pt_str params, pkind kind, pt_rt_mcp_done fn,
                void *ud)
{
    srv *s = find_srv(rt, server);
    pend *p;
    if (!s || s->state != PT_MCP_READY)
        return false;
    if (!request(s, method, params, kind, &p) || !p)
        return false;
    p->fn = fn;
    p->ud = ud;
    return true;
}

bool pt_rt_mcp_get_prompt(pt_rt *rt, pt_str server, pt_str name, pt_str args, pt_rt_mcp_done fn, void *ud)
{
    pt_buf b;
    bool ok;
    pt_buf_init(&b, pt_sys_allocator());
    pt_buf_putz(&b, "{\"name\":");
    pt_json_quote(&b, name);
    if (args.n) {
        pt_buf_putz(&b, ",\"arguments\":");
        pt_buf_puts(&b, args);
    }
    pt_buf_putc(&b, '}');
    ok = !b.oom && ask(rt, server, "prompts/get", pt_buf_str(&b), P_GET, fn, ud);
    pt_buf_free(&b);
    return ok;
}

bool pt_rt_mcp_read(pt_rt *rt, pt_str server, pt_str uri, pt_rt_mcp_done fn, void *ud)
{
    pt_buf b;
    bool ok;
    pt_buf_init(&b, pt_sys_allocator());
    pt_buf_putz(&b, "{\"uri\":");
    pt_json_quote(&b, uri);
    pt_buf_putc(&b, '}');
    ok = !b.oom && ask(rt, server, "resources/read", pt_buf_str(&b), P_READ, fn, ud);
    pt_buf_free(&b);
    return ok;
}

bool pt_rt_mcp_login(pt_rt *rt, pt_str server)
{
    srv *s = find_srv(rt, server);
    return s && s->http && mcp_auth_login(s);
}

bool pt_rt_mcp_logout(pt_rt *rt, pt_str server)
{
    srv *s = find_srv(rt, server);
    if (!s || !s->http)
        return false;
    mcp_auth_logout(s);
    srv_stop(s, "logged out");
    s->state = PT_MCP_AUTH;
    snprintf(s->error, sizeof s->error, "logged out: log in with /mcp login %s", s->name);
    return true;
}

bool pt_rt_mcp_restart(pt_rt *rt, pt_str server)
{
    srv *s = find_srv(rt, server);
    if (!s)
        return false;
    fail_all(s, "the server was restarted", true);
    srv_connect(s, false);
    return true;
}
