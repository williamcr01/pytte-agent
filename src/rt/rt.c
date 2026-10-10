#define _POSIX_C_SOURCE 200809L
#include "rt/rt.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct req {
    pt_rt *rt;
    uint32_t fx;
    void *h;
    bool done;
} req;

typedef struct sleeper {
    uint32_t fx;
    int64_t until;
} sleeper;

struct pt_rt {
    pt_rt_cfg c;
    pt_agent *a;
    uint64_t log_end; /* where our last commit ended (section 6.8.4) */
    req **reqs;
    uint32_t nreqs, rcap;
    sleeper *sleeps;
    uint32_t nsleeps, scap;
};

static int64_t sys_now(void *ud)
{
    (void)ud;
    return pt_sys_now_ms();
}

static void sys_sleep(void *ud, uint32_t ms)
{
    (void)ud;
    pt_sys_sleep_ms(ms);
}

pt_rt *pt_rt_new(const pt_rt_cfg *c, pt_agent *a)
{
    pt_rt *rt = (pt_rt *)calloc(1, sizeof *rt);
    if (!rt)
        return NULL;
    rt->c = *c;
    if (!rt->c.now)
        rt->c.now = sys_now;
    if (!rt->c.sleep)
        rt->c.sleep = sys_sleep;
    rt->a = a;
    return rt;
}

static void drop_req(pt_rt *rt, uint32_t i)
{
    req *r = rt->reqs[i];
    if (!r->done)
        rt->c.http->cancel(rt->c.http, r->h);
    free(r);
    rt->reqs[i] = rt->reqs[--rt->nreqs];
}

void pt_rt_free(pt_rt *rt)
{
    if (!rt)
        return;
    while (rt->nreqs)
        drop_req(rt, 0);
    free(rt->reqs);
    free(rt->sleeps);
    if (rt->c.store)
        rt->c.store->close(rt->c.store);
    free(rt);
}

pt_http_driver *pt_rt_http(const pt_rt *rt)
{
    return rt->c.http;
}

/* ---- recovery ---- */

static void on_live(void *ud, uint32_t task, pt_str bytes)
{
    pt_load_live((pt_agent *)ud, task, bytes);
}

bool pt_rt_recover(pt_rt *rt, char *err, size_t cap)
{
    pt_store *st = rt->c.store;
    pt_buf b;
    size_t start = 0, i;
    unsigned long line = 1;
    bool ok = true;
    pt_buf_init(&b, pt_sys_allocator());
    if (st->read_log(st, &b) != 0) {
        snprintf(err, cap, "%s", st->err);
        pt_buf_free(&b);
        return false;
    }
    rt->log_end = b.n;
    for (i = 0; i < b.n && ok; i++) {
        pt_load_result r;
        if (b.p[i] != '\n')
            continue;
        r = pt_load_line(rt->a, (pt_str){b.p + start, i - start}, true, i + 1 == b.n);
        if (r == PT_LOAD_CORRUPT) {
            snprintf(err, cap, "%s (line %lu, byte %lu)", pt_error(rt->a), line, (unsigned long)start);
            ok = false;
        } else if (r == PT_LOAD_TORN) {
            break;
        }
        start = i + 1;
        line++;
    }
    if (ok && start < b.n) { /* a torn last line: the commit never happened */
        if (st->truncate(st, start) != 0) {
            snprintf(err, cap, "%s", st->err);
            ok = false;
        }
        rt->log_end = start;
    }
    pt_buf_free(&b);
    if (ok && st->live_list(st, on_live, rt->a) != 0) {
        snprintf(err, cap, "%s", st->err);
        ok = false;
    }
    if (ok && !pt_load_done(rt->a)) {
        snprintf(err, cap, "%s", pt_error(rt->a));
        ok = false;
    }
    return ok;
}

/* ---- effects ---- */

static void sink_head(void *ud, int status, pt_str retry_after)
{
    req *r = (req *)ud;
    pt_fx_head(r->rt->a, r->fx, status, retry_after);
}

static void sink_body(void *ud, const char *p, size_t n)
{
    req *r = (req *)ud;
    pt_fx_data(r->rt->a, r->fx, p, n);
}

static void sink_done(void *ud, const char *error)
{
    req *r = (req *)ud;
    r->done = true;
    pt_fx_done(r->rt->a, r->fx, error);
}

static void commit(pt_rt *rt, const pt_fx *fx)
{
    pt_store *st = rt->c.store;
    uint64_t size;
    if (st->size(st, &size) != 0) {
        pt_fx_done(rt->a, fx->id, st->err);
        return;
    }
    if (size != rt->log_end) {
        pt_fx_done(rt->a, fx->id, "the log was modified by another writer");
        return;
    }
    if (st->append(st, fx->bytes.p, fx->bytes.n, fx->sync) != 0) {
        pt_fx_done(rt->a, fx->id, st->err);
        return;
    }
    rt->log_end += fx->bytes.n;
    pt_fx_done(rt->a, fx->id, NULL);
}

static void start_http(pt_rt *rt, const pt_fx *fx)
{
    req *r;
    pt_http_sink sink;
    if (rt->nreqs == rt->rcap) {
        uint32_t ncap = rt->rcap ? rt->rcap * 2 : 8;
        req **nr = (req **)realloc(rt->reqs, ncap * sizeof *nr);
        if (!nr) {
            pt_fx_done(rt->a, fx->id, "out of memory");
            return;
        }
        rt->reqs = nr;
        rt->rcap = ncap;
    }
    r = (req *)calloc(1, sizeof *r);
    if (!r) {
        pt_fx_done(rt->a, fx->id, "out of memory");
        return;
    }
    r->rt = rt;
    r->fx = fx->id;
    sink.ud = r;
    sink.head = sink_head;
    sink.body = sink_body;
    sink.done = sink_done;
    r->h = rt->c.http->start(rt->c.http, fx->http, &sink);
    if (!r->h) {
        free(r);
        pt_fx_done(rt->a, fx->id, "could not start the request");
        return;
    }
    rt->reqs[rt->nreqs++] = r;
}

static void handle(pt_rt *rt, const pt_fx *fx)
{
    uint32_t i;
    switch (fx->kind) {
    case PT_FX_COMMIT:
        commit(rt, fx);
        break;
    case PT_FX_HTTP:
        start_http(rt, fx);
        break;
    case PT_FX_SLEEP:
        if (rt->nsleeps == rt->scap) {
            sleeper *ns = (sleeper *)realloc(rt->sleeps, (rt->scap ? rt->scap * 2 : 4) * sizeof *ns);
            if (!ns) {
                pt_fx_done(rt->a, fx->id, NULL); /* wake early rather than never */
                break;
            }
            rt->sleeps = ns;
            rt->scap = rt->scap ? rt->scap * 2 : 4;
        }
        rt->sleeps[rt->nsleeps].fx = fx->id;
        rt->sleeps[rt->nsleeps++].until = fx->until;
        break;
    case PT_FX_LIVE:
        if (fx->bytes.n)
            rt->c.store->live_put(rt->c.store, fx->task, fx->bytes.p, fx->bytes.n);
        else
            rt->c.store->live_del(rt->c.store, fx->task);
        break;
    case PT_FX_CANCEL:
        for (i = 0; i < rt->nreqs; i++)
            if (rt->reqs[i]->fx == fx->cancel) {
                drop_req(rt, i);
                break;
            }
        for (i = 0; i < rt->nsleeps; i++)
            if (rt->sleeps[i].fx == fx->cancel) {
                rt->sleeps[i] = rt->sleeps[--rt->nsleeps];
                break;
            }
        break;
    }
}

static void sweep(pt_rt *rt)
{
    uint32_t i = 0;
    while (i < rt->nreqs)
        if (rt->reqs[i]->done)
            drop_req(rt, i);
        else
            i++;
}

/* Wake due sleepers; returns how many. */
static uint32_t wake(pt_rt *rt, int64_t now)
{
    uint32_t i = 0, n = 0;
    while (i < rt->nsleeps)
        if (rt->sleeps[i].until <= now) {
            uint32_t fx = rt->sleeps[i].fx;
            rt->sleeps[i] = rt->sleeps[--rt->nsleeps];
            pt_fx_done(rt->a, fx, NULL);
            n++;
        } else {
            i++;
        }
    return n;
}

static int64_t next_wake(const pt_rt *rt)
{
    int64_t t = INT64_MAX;
    uint32_t i;
    for (i = 0; i < rt->nsleeps; i++)
        if (rt->sleeps[i].until < t)
            t = rt->sleeps[i].until;
    return t;
}

static unsigned drain(pt_rt *rt)
{
    pt_fx fx;
    unsigned n = 0;
    while (pt_poll(rt->a, &fx)) {
        handle(rt, &fx);
        n++;
    }
    return n;
}

static void http_failed(pt_rt *rt)
{
    uint32_t i;
    for (i = 0; i < rt->nreqs; i++)
        if (!rt->reqs[i]->done) {
            rt->reqs[i]->done = true;
            rt->c.http->cancel(rt->c.http, rt->reqs[i]->h);
            pt_fx_done(rt->a, rt->reqs[i]->fx, "HTTP driver failure");
        }
}

/* Wait once: for a due sleeper, HTTP progress, one of fds, or timeout_ms. */
static void wait_once(pt_rt *rt, int timeout_ms, pt_waitfd *fds, unsigned nfds)
{
    int64_t now = rt->c.now(rt->c.clock_ud), until, wait = timeout_ms;
    if (wake(rt, now))
        return;
    until = next_wake(rt);
    if (until != INT64_MAX && (wait < 0 || until - now < wait))
        wait = until - now;
    if (wait > INT32_MAX)
        wait = INT32_MAX;
    if (rt->nreqs || nfds) {
        if (rt->c.http->pump(rt->c.http, (int)wait, fds, nfds) < 0) {
            http_failed(rt);
            /* still wait on the host's fds (a terminal, a signal pipe),
             * without spinning on a driver that keeps failing */
            if (nfds)
                pt_sys_wait(fds, nfds, wait < 0 || wait > 1000 ? 1000 : (int)wait);
        }
        sweep(rt);
    } else if (rt->nsleeps && wait > 0) {
        rt->c.sleep(rt->c.clock_ud, (uint32_t)wait);
    }
    wake(rt, rt->c.now(rt->c.clock_ud));
}

static pt_rt_result status(pt_rt *rt)
{
    if (pt_fatal(rt->a))
        return PT_RT_FATAL;
    if (pt_idle(rt->a))
        return PT_RT_IDLE;
    if (!rt->nreqs && !rt->nsleeps)
        return PT_RT_STUCK;
    return PT_RT_BUSY;
}

pt_rt_result pt_rt_step(pt_rt *rt, int timeout_ms, pt_waitfd *fds, unsigned nfds)
{
    /* Work handed out (a commit, a request) changed what the host shows:
     * look once without waiting, so the host paints it now, not after
     * the timeout. */
    unsigned n = drain(rt);
    wait_once(rt, n ? 0 : timeout_ms, fds, nfds);
    drain(rt);
    return status(rt);
}

pt_rt_result pt_rt_run(pt_rt *rt)
{
    for (;;) {
        pt_rt_result r;
        drain(rt);
        r = status(rt);
        if (r != PT_RT_BUSY)
            return r;
        if (rt->c.stop && rt->c.stop(rt->c.stop_ud))
            return PT_RT_STOPPED;
        wait_once(rt, 100, NULL, 0);
    }
}

bool pt_rt_close(pt_rt *rt)
{
    pt_fx fx;
    int guard = 0;
    while (rt->nreqs)
        drop_req(rt, 0);
    rt->nsleeps = 0;
    pt_close(rt->a);
    while (!pt_closed(rt->a) && !pt_fatal(rt->a) && guard++ < 1000)
        while (pt_poll(rt->a, &fx))
            if (fx.kind == PT_FX_COMMIT || fx.kind == PT_FX_LIVE)
                handle(rt, &fx);
    return pt_closed(rt->a);
}
