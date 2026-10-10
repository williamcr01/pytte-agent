#define _POSIX_C_SOURCE 200809L
#include "rt/rt_int.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FDS 64

typedef rt_req req;
typedef rt_sleeper sleeper;

static bool finish_io(pt_rt *rt, bool wait);

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
    pt_buf_init(&rt->scratch, pt_sys_allocator());
    pt_arena_init(&rt->ar, pt_sys_allocator(), 4096);
    return rt;
}

pt_agent *pt_rt_agent(pt_rt *rt)
{
    return rt->a;
}

int64_t pt_rt_now(pt_rt *rt)
{
    return rt->c.now(rt->c.clock_ud);
}

bool pt_rt_grow(void **arr, uint32_t *cap, uint32_t need, size_t elem)
{
    uint32_t ncap = *cap ? *cap : 4;
    void *p;
    if (need <= *cap)
        return true;
    while (ncap < need)
        ncap *= 2;
    if (!(p = realloc(*arr, (size_t)ncap * elem)))
        return false;
    *arr = p;
    *cap = ncap;
    return true;
}

uint32_t pt_rt_timer(pt_rt *rt, int64_t at, pt_rt_fn fn, void *ud)
{
    rt_timer *t;
    if (!pt_rt_grow((void **)&rt->timers, &rt->tmcap, rt->ntimers + 1, sizeof *rt->timers))
        return 0;
    t = &rt->timers[rt->ntimers++];
    t->id = ++rt->next_timer;
    t->at = at;
    t->fn = fn;
    t->ud = ud;
    return t->id;
}

void pt_rt_timer_cancel(pt_rt *rt, uint32_t id)
{
    uint32_t i;
    for (i = 0; i < rt->ntimers; i++)
        if (rt->timers[i].id == id) {
            rt->timers[i] = rt->timers[--rt->ntimers];
            return;
        }
}

bool pt_rt_watch(pt_rt *rt, int fd, pt_rt_fn fn, void *ud)
{
    rt_watch *w;
    if (!pt_rt_grow((void **)&rt->watches, &rt->wcap, rt->nwatches + 1, sizeof *rt->watches))
        return false;
    w = &rt->watches[rt->nwatches++];
    w->fd = fd;
    w->fn = fn;
    w->ud = ud;
    return true;
}

void pt_rt_unwatch(pt_rt *rt, int fd)
{
    uint32_t i;
    for (i = 0; i < rt->nwatches; i++)
        if (rt->watches[i].fd == fd) {
            rt->watches[i] = rt->watches[--rt->nwatches];
            return;
        }
}

/* Fire timers that are due; returns how many. */
static uint32_t fire_timers(pt_rt *rt, int64_t now)
{
    uint32_t i = 0, n = 0;
    while (i < rt->ntimers)
        if (rt->timers[i].at <= now) {
            rt_timer t = rt->timers[i];
            rt->timers[i] = rt->timers[--rt->ntimers];
            t.fn(t.ud, rt);
            n++;
            i = 0; /* the callback may have changed the list */
        } else {
            i++;
        }
    return n;
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
    finish_io(rt, true);
    while (rt->nreqs)
        drop_req(rt, 0);
    rt->closing = true;
    pt_rt_tools_free(rt);
    free(rt->reqs);
    free(rt->sleeps);
    free(rt->timers);
    free(rt->watches);
    pt_buf_free(&rt->scratch);
    pt_arena_free(&rt->ar);
    if (rt->c.store)
        rt->c.store->close(rt->c.store);
    free(rt);
}

pt_http_driver *pt_rt_http(const pt_rt *rt)
{
    return rt->c.http;
}

void pt_rt_use_worker(pt_rt *rt, pt_worker *worker)
{
    rt->worker = worker;
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

/* Called on the worker for the interactive host. It touches only the
 * store and this job's result; core completion happens on the host thread. */
static void store_io(void *ud)
{
    pt_rt *rt = (pt_rt *)ud;
    pt_store *st = rt->c.store;
    const pt_fx *fx = &rt->io;
    uint64_t size;
    rt->io_ok = false;
    rt->io_error[0] = 0;
    if (fx->kind == PT_FX_LIVE) {
        if (fx->bytes.n)
            st->live_put(st, fx->task, fx->bytes.p, fx->bytes.n);
        else
            st->live_del(st, fx->task);
        rt->io_ok = true; /* snapshots remain best effort */
        return;
    }
    if (st->size(st, &size) != 0) {
        snprintf(rt->io_error, sizeof rt->io_error, "%s", st->err);
        return;
    }
    if (size != rt->log_end) {
        snprintf(rt->io_error, sizeof rt->io_error, "the log was modified by another writer");
        return;
    }
    if (st->append(st, fx->bytes.p, fx->bytes.n, fx->sync) != 0) {
        snprintf(rt->io_error, sizeof rt->io_error, "%s", st->err);
        return;
    }
    rt->io_ok = true;
}

static void io_done(pt_rt *rt)
{
    rt->io_pending = false;
    if (rt->io.kind == PT_FX_COMMIT) {
        if (rt->io_ok)
            rt->log_end += rt->io.bytes.n;
        pt_fx_done(rt->a, rt->io.id, rt->io_ok ? NULL : rt->io_error);
    }
}

static bool finish_io(pt_rt *rt, bool wait)
{
    if (!rt->io_pending)
        return false;
    while (!pt_worker_finish(rt->worker)) {
        pt_waitfd fd;
        if (!wait)
            return false;
        fd.fd = pt_worker_fd(rt->worker);
        pt_sys_wait(&fd, 1, -1);
    }
    io_done(rt);
    return true;
}

static void start_io(pt_rt *rt, const pt_fx *fx)
{
    rt->io = *fx;
    if (!rt->worker) {
        store_io(rt);
        io_done(rt);
    } else if (pt_worker_start(rt->worker, store_io, rt)) {
        rt->io_pending = true;
    } else if (fx->kind == PT_FX_COMMIT) {
        pt_fx_done(rt->a, fx->id, "could not schedule the durable write");
    }
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
        start_io(rt, fx);
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
        start_io(rt, fx);
        break;
    case PT_FX_GATE:
        pt_rt_gate(rt, fx);
        break;
    case PT_FX_ASK: /* the frontend shows PT_E_ASK and calls pt_approve */
        break;
    case PT_FX_TOOL:
        pt_rt_run_tool(rt, fx);
        break;
    case PT_FX_CANCEL:
        pt_rt_cancel_tool(rt, fx->cancel);
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
    for (i = 0; i < rt->ntimers; i++)
        if (rt->timers[i].at < t)
            t = rt->timers[i].at;
    return t;
}

static unsigned drain(pt_rt *rt)
{
    pt_fx fx;
    unsigned n = 0;
    /* LIVE bytes last until the next poll; COMMIT bytes until completion.
     * Holding the poll while I/O is pending safely borrows both, so there
     * is no snapshot/commit allocation or copy on the worker path. */
    while (!rt->io_pending && pt_poll(rt->a, &fx)) {
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

/* Watches whose fd is ready: called after the wait, by fd, since a
 * callback may change the list. */
static void fire_watches(pt_rt *rt, const pt_waitfd *all, unsigned from, unsigned to)
{
    unsigned i;
    uint32_t j;
    for (i = from; i < to; i++) {
        if (!all[i].ready)
            continue;
        for (j = 0; j < rt->nwatches; j++)
            if (rt->watches[j].fd == all[i].fd) {
                rt->watches[j].fn(rt->watches[j].ud, rt);
                break;
            }
    }
}

/* Wait once: for a due sleeper or timer, HTTP progress, a watched fd, one
 * of fds, or timeout_ms. */
static void wait_once(pt_rt *rt, int timeout_ms, pt_waitfd *fds, unsigned nfds)
{
    int64_t now = rt->c.now(rt->c.clock_ud), until, wait = timeout_ms;
    pt_waitfd all[MAX_FDS];
    unsigned i, total = nfds, first_watch;
    uint32_t j;
    if (total > MAX_FDS / 2)
        total = MAX_FDS / 2;
    for (i = 0; i < nfds; i++)
        fds[i].ready = false;
    for (i = 0; i < total; i++)
        all[i] = fds[i];
    if (rt->io_pending) {
        all[total].fd = pt_worker_fd(rt->worker);
        all[total++].ready = false;
    }
    first_watch = total;
    for (j = 0; j < rt->nwatches && total < MAX_FDS; j++) {
        all[total].fd = rt->watches[j].fd;
        all[total++].ready = false;
    }
    if (wake(rt, now) + fire_timers(rt, now))
        return;
    until = next_wake(rt);
    if (until != INT64_MAX && (wait < 0 || until - now < wait))
        wait = until - now;
    if (wait > INT32_MAX)
        wait = INT32_MAX;
    if (rt->nreqs || total) {
        if (rt->c.http->pump(rt->c.http, (int)wait, all, total) < 0) {
            http_failed(rt);
            /* still wait on the host's fds (a terminal, a signal pipe),
             * without spinning on a driver that keeps failing */
            if (total)
                pt_sys_wait(all, total, wait < 0 || wait > 1000 ? 1000 : (int)wait);
        }
        for (i = 0; i < nfds && i < MAX_FDS / 2; i++)
            fds[i].ready = all[i].ready;
        sweep(rt);
        fire_watches(rt, all, first_watch, total);
    } else if ((rt->nsleeps || rt->ntimers) && wait > 0) {
        rt->c.sleep(rt->c.clock_ud, (uint32_t)wait);
    }
    finish_io(rt, false);
    now = rt->c.now(rt->c.clock_ud);
    wake(rt, now);
    fire_timers(rt, now);
}

static pt_rt_result status(pt_rt *rt)
{
    if (pt_fatal(rt->a))
        return PT_RT_FATAL;
    if (pt_idle(rt->a))
        return PT_RT_IDLE;
    if (!rt->nreqs && !rt->nsleeps && !rt->io_pending && !rt->ncalls && !rt->ntimers && !rt->nwatches)
        return pt_asks(rt->a, NULL, 0) ? PT_RT_ASK : PT_RT_STUCK;
    return PT_RT_BUSY;
}

pt_rt_result pt_rt_step(pt_rt *rt, int timeout_ms, pt_waitfd *fds, unsigned nfds)
{
    /* Work handed out (a commit, a request) changed what the host shows:
     * look once without waiting, so the host paints it now, not after
     * the timeout. */
    unsigned n = finish_io(rt, false) ? 1 : 0;
    n += drain(rt);
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
    while (rt->nreqs)
        drop_req(rt, 0);
    rt->nsleeps = 0;
    rt->closing = true; /* running tools are stopped; their calls stay pending */
    pt_rt_tools_stop(rt);
    /* Land an in-flight claim before deciding whether it needs a close. */
    finish_io(rt, true);
    pt_close(rt->a);
    while (!pt_closed(rt->a) && !pt_fatal(rt->a)) {
        bool progress = false;
        finish_io(rt, true);
        while (!rt->io_pending && pt_poll(rt->a, &fx)) {
            progress = true;
            if (fx.kind == PT_FX_COMMIT || fx.kind == PT_FX_LIVE)
                handle(rt, &fx);
        }
        if (!progress && !rt->io_pending)
            break; /* a stuck core; never silently drop a large input queue */
    }
    finish_io(rt, true);
    return pt_closed(rt->a);
}
