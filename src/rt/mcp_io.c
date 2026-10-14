/* The MCP client's transports: stdio (a child process speaking
 * newline-delimited JSON-RPC on its stdin/stdout; stderr's tail is kept to
 * say why it died) and Streamable HTTP (a POST per message, answered with
 * JSON or an event stream), with the HTTP calls OAuth uses too.
 *
 * What arrives goes to mcp_on_message; what fails, to mcp_srv_fail or the
 * request that waited (mcp.c). */
#define _POSIX_C_SOURCE 200809L
#include "rt/mcp_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_MAX_BYTES (64u << 20) /* one message over stdio */
#define BODY_MAX (64u << 20)       /* one HTTP response */

typedef struct qmsg { /* a message waiting to be POSTed */
    struct qmsg *next;
    int64_t id;
    bool init;
    size_t n;
    char data[1];
} qmsg;

/* ---- stdio ---- */

static void flush_later(void *ud, pt_rt *rt);

static void stdio_flush(srv *s)
{
    while (s->outq.n) {
        long w = pt_proc_write(s->p, s->outq.p, s->outq.n);
        if (w < 0) {
            mcp_srv_fail(s, "its input closed: it stopped reading");
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

static void stdio_lines(srv *s, const char *p, size_t n)
{
    uint64_t gen = s->gen;
    while (n && s->p && s->gen == gen) {
        const char *nl = (const char *)memchr(p, '\n', n);
        size_t k = nl ? (size_t)(nl - p) : n;
        if (s->line.n + k > LINE_MAX_BYTES) {
            mcp_srv_fail(s, "sent a message longer than %u MiB", LINE_MAX_BYTES >> 20);
            return;
        }
        if (!nl) {
            pt_buf_put(&s->line, p, n);
            return;
        }
        if (s->line.n) {
            pt_buf_put(&s->line, p, k);
            if (s->line.oom) {
                mcp_srv_fail(s, "out of memory reading its output");
                return;
            }
            mcp_on_message(s, pt_buf_str(&s->line));
            pt_buf_reset(&s->line);
        } else {
            mcp_on_message(s, (pt_str){p, k});
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
    mcp_srv_fail(s, "exited with status %d%s%s", status, tail[0] ? ": " : "", tail);
}

static void stdio_out(void *ud, pt_rt *rt)
{
    srv *s = (srv *)ud;
    char *io = s->m->io;
    (void)rt;
    while (s->p) {
        long r = pt_proc_read(s->p, io, MCP_IO_CHUNK);
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

bool mcp_stdio_start(srv *s)
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
        mcp_srv_fail(s, "cannot start %s: %s", s->target, err);
        return false;
    }
    s->out_fd = pt_proc_out_fd(s->p);
    s->err_fd = pt_proc_err_fd(s->p);
    s->exit_fd = pt_proc_exit_fd(s->p);
    if (!pt_rt_watch(rt, s->out_fd, stdio_out, s) || !pt_rt_watch(rt, s->err_fd, stdio_err, s) ||
        (s->exit_fd >= 0 && !pt_rt_watch(rt, s->exit_fd, stdio_exit, s))) {
        mcp_srv_fail(s, "out of memory");
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
        mcp_on_message(s, json);
}

static void post_done(hcall *h)
{
    srv *s = h->s;
    char msg[700];
    if (s->gen != (uint64_t)(uintptr_t)h->ud)
        return; /* sent before a restart */
    if (h->init && h->session[0])
        snprintf(s->session, sizeof s->session, "%s", h->session);
    if (h->err[0]) {
        snprintf(msg, sizeof msg, "%s: %s", s->url, h->err);
        if (h->init)
            mcp_srv_fail(s, "%s", msg);
        else if (h->msg_id)
            mcp_request_failed(s, h->msg_id, msg);
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
        mcp_fail_all(s, "the server ended its session; it was restarted: try again", false);
        mcp_reconnect(s);
        return;
    }
    if (h->status < 200 || h->status >= 300) {
        snprintf(msg, sizeof msg, "%s answered HTTP %d%s%.*s", s->url, h->status, h->body.n ? ": " : "",
                 (int)mcp_cut(pt_buf_str(&h->body), 300).n, h->body.p ? h->body.p : "");
        if (h->init)
            mcp_srv_fail(s, "%s", msg);
        else if (h->msg_id)
            mcp_request_failed(s, h->msg_id, msg);
        return;
    }
    if (!h->sse_on && h->body.n && pt_str_starts(pt_strz(h->ctype), PT_S("application/json")))
        mcp_on_message(s, pt_buf_str(&h->body));
    /* a request whose answer never came */
    if (s->gen == (uint64_t)(uintptr_t)h->ud && h->msg_id)
        mcp_request_unanswered(s, h->msg_id);
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
            mcp_srv_fail(s, "out of memory");
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
            mcp_srv_fail(s, "cannot start a request to %s", s->url);
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

void mcp_send(srv *s, pt_str msg, int64_t id, bool init)
{
    if (!s->http) {
        if (!s->p)
            return;
        pt_buf_puts(&s->outq, msg);
        pt_buf_putc(&s->outq, '\n');
        if (s->outq.oom) {
            mcp_srv_fail(s, "out of memory");
            return;
        }
        if (!s->flush_timer)
            stdio_flush(s);
        return;
    }
    {
        qmsg *q = (qmsg *)malloc(sizeof *q + msg.n), **pp;
        if (!q) {
            mcp_srv_fail(s, "out of memory");
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

void mcp_io_close(srv *s)
{
    if (s->http)
        http_close(s);
    else
        stdio_close(s);
}
