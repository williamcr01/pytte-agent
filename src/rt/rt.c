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
    if (c->session)
        snprintf(rt->session, sizeof rt->session, "%s", c->session);
    rt->c.session = NULL;
    pt_buf_init(&rt->scratch, pt_sys_allocator());
    pt_buf_init(&rt->mat.cur, pt_sys_allocator());
    pt_arena_init(&rt->ar, pt_sys_allocator(), 4096);
    pt_arena_init(&rt->prompt.ar, pt_sys_allocator(), 0);
    pt_arena_init(&rt->skills.ar, pt_sys_allocator(), 0);
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
    while (ncap < need) {
        if (ncap > UINT32_MAX / 2) {
            ncap = need;
            break;
        }
        ncap *= 2;
    }
    if (elem && (size_t)ncap > SIZE_MAX / elem)
        return false;
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
    w->generation = ++rt->watch_generation;
    if (!w->generation)
        w->generation = ++rt->watch_generation;
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
    if (!rt->closing) {
        finish_io(rt, true);
        while (rt->nreqs)
            drop_req(rt, 0);
        rt->closing = true;
    }
    pt_rt_tools_stop(rt);
    if (rt->ncalls) {
        /* Async tool completions still borrow rt. Keep its state alive until
         * the final pt_rt_tool_done arrives; callbacks are no longer pumped. */
        rt->free_waiting = true;
        return;
    }
    rt->free_waiting = false;
    if (rt->mcp.free)
        rt->mcp.free(rt->mcp.ud);
    rt->mcp.free = NULL;
    pt_rt_tools_free(rt);
    pt_rt_prompt_free(rt);
    pt_buf_free(&rt->mat.cur);
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
    rt->io.worker = worker;
}

/* ---- recovery ---- */

static void on_live(void *ud, uint32_t task, pt_str bytes)
{
    pt_load_live((pt_agent *)ud, task, bytes);
}

/* A dead owner's process group: stopped if it is still the same group,
 * forgotten unless it ran on another host. */
static void on_proc(void *ud, int64_t id, pt_str mark)
{
    pt_store *st = (pt_store *)ud;
    if (pt_proc_stop_marked(mark) >= 0)
        st->proc_del(st, id);
}

void pt_rt_stop_orphans(pt_rt *rt)
{
    pt_store *st = rt->c.store;
    if (st && st->proc_list)
        st->proc_list(st, on_proc, st);
}

void pt_rt_proc_started(pt_rt *rt, int64_t group, pt_str mark)
{
    pt_store *st = rt->c.store;
    if (st && st->proc_put && mark.n)
        st->proc_put(st, group, mark.p, mark.n); /* best effort */
}

void pt_rt_proc_ended(pt_rt *rt, int64_t group)
{
    pt_store *st = rt->c.store;
    if (st && st->proc_del)
        st->proc_del(st, group);
}

/* The log is read in chunks: memory for one chunk and the longest line,
 * however long the session has grown (open question 7). */
#define READ_CHUNK 65536

bool pt_rt_recover(pt_rt *rt, char *err, size_t cap)
{
    pt_store *st = rt->c.store;
    char *chunk = (char *)malloc(READ_CHUNK);
    pt_buf line; /* a line that spans chunks */
    uint64_t size = 0, off = 0, start = 0; /* start: where the current line begins */
    unsigned long lineno = 1;
    bool ok = true, torn = false;
    pt_buf_init(&line, pt_sys_allocator());
    if (!chunk) {
        snprintf(err, cap, "out of memory reading the log");
        return false;
    }
    if (st->size(st, &size) != 0) {
        snprintf(err, cap, "%s", st->err);
        ok = false;
    } else {
        rt->log_end = size;
    }
    while (ok && !torn && off < size) {
        size_t got, i, from = 0;
        if (st->read(st, off, chunk, READ_CHUNK, &got) != 0) {
            snprintf(err, cap, "%s", st->err);
            ok = false;
            break;
        }
        if (!got) { /* shorter than it said */
            size = off;
            break;
        }
        for (i = 0; i < got && ok; i++) {
            pt_load_result r;
            pt_str l;
            if (chunk[i] != '\n')
                continue;
            if (line.n) {
                pt_buf_put(&line, chunk + from, i - from);
                l = pt_buf_str(&line);
            } else {
                l = (pt_str){chunk + from, i - from};
            }
            r = line.oom ? PT_LOAD_CORRUPT : pt_load_line(rt->a, l, true, off + i + 1 == size);
            pt_buf_reset(&line);
            if (r == PT_LOAD_CORRUPT) {
                snprintf(err, cap, "%s (line %lu, byte %llu)", line.oom ? "out of memory" : pt_error(rt->a),
                         lineno, (unsigned long long)start);
                ok = false;
            } else if (r == PT_LOAD_TORN) {
                torn = true;
                break;
            } else {
                start = off + i + 1;
                from = i + 1;
                lineno++;
            }
        }
        if (ok && !torn && from < got)
            pt_buf_put(&line, chunk + from, got - from);
        off += got;
    }
    free(chunk);
    pt_buf_free(&line);
    if (ok && start < size) { /* a torn last line: the commit never happened */
        if (st->truncate(st, start) != 0) {
            snprintf(err, cap, "%s", st->err);
            ok = false;
        }
        rt->log_end = start;
    }
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
    const pt_fx *fx = &rt->io.fx;
    uint64_t size;
    rt->io.ok = false;
    rt->io.error[0] = 0;
    if (fx->kind == PT_FX_BLOB) {
        static const char digits[] = "0123456789abcdef";
        unsigned char d[32];
        pt_sha256 sh;
        void *h;
        int i;
        if (!st->blob_begin || !(h = st->blob_begin(st)))
            return;
        if (st->blob_write(st, h, fx->bytes.p, fx->bytes.n) != 0) {
            st->blob_abort(st, h);
            return;
        }
        pt_sha256_init(&sh);
        pt_sha256_update(&sh, fx->bytes.p, fx->bytes.n);
        pt_sha256_final(&sh, d);
        memcpy(rt->io.blob, "sha256:", 7);
        for (i = 0; i < 32; i++) {
            rt->io.blob[7 + 2 * i] = digits[d[i] >> 4];
            rt->io.blob[8 + 2 * i] = digits[d[i] & 15];
        }
        rt->io.blob[71] = 0;
        rt->io.ok = st->blob_commit(st, h, rt->io.blob + 7) == 0;
        return;
    }
    if (fx->kind == PT_FX_LIVE) {
        if (fx->bytes.n)
            st->live_put(st, fx->task, fx->bytes.p, fx->bytes.n);
        else
            st->live_del(st, fx->task);
        rt->io.ok = true; /* snapshots remain best effort */
        return;
    }
    if (st->size(st, &size) != 0) {
        snprintf(rt->io.error, sizeof rt->io.error, "%s", st->err);
        return;
    }
    if (size != rt->log_end) {
        snprintf(rt->io.error, sizeof rt->io.error, "the log was modified by another writer");
        return;
    }
    if (st->append(st, fx->bytes.p, fx->bytes.n, fx->sync) != 0) {
        snprintf(rt->io.error, sizeof rt->io.error, "%s", st->err);
        return;
    }
    rt->io.ok = true;
}

static void io_done(pt_rt *rt)
{
    rt->io.pending = false;
    if (rt->io.fx.kind == PT_FX_COMMIT) {
        if (rt->io.ok)
            rt->log_end += rt->io.fx.bytes.n;
        pt_fx_done(rt->a, rt->io.fx.id, rt->io.ok ? NULL : rt->io.error);
    } else if (rt->io.fx.kind == PT_FX_BLOB) {
        pt_fx_blob(rt->a, rt->io.fx.id, rt->io.ok ? pt_strz(rt->io.blob) : PT_STR_NULL);
    }
}

static bool finish_io(pt_rt *rt, bool wait)
{
    if (!rt->io.pending)
        return false;
    while (!pt_worker_finish(rt->io.worker)) {
        pt_waitfd fd;
        if (!wait)
            return false;
        fd.fd = pt_worker_fd(rt->io.worker);
        pt_sys_wait(&fd, 1, -1);
    }
    io_done(rt);
    return true;
}

static void start_io(pt_rt *rt, const pt_fx *fx)
{
    rt->io.fx = *fx;
    if (!rt->io.worker) {
        store_io(rt);
        io_done(rt);
    } else if (pt_worker_start(rt->io.worker, store_io, rt)) {
        rt->io.pending = true;
    } else if (fx->kind == PT_FX_COMMIT) {
        pt_fx_done(rt->a, fx->id, "could not schedule the durable write");
    } else if (fx->kind == PT_FX_BLOB) {
        pt_fx_blob(rt->a, fx->id, PT_STR_NULL); /* kept inline */
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
    sink.header = NULL;
    r->h = rt->c.http->start(rt->c.http, fx->http, &sink);
    if (!r->h) {
        free(r);
        pt_fx_done(rt->a, fx->id, "could not start the request");
        return;
    }
    rt->reqs[rt->nreqs++] = r;
}

/* ---- HTTP for tools ---- */

typedef struct rt_hreq {
    pt_rt *rt;
    void *h;
    pt_http_sink sink;
} rt_hreq;

static void h_head(void *ud, int status, pt_str retry_after)
{
    rt_hreq *r = (rt_hreq *)ud;
    if (r->sink.head)
        r->sink.head(r->sink.ud, status, retry_after);
}

static void h_body(void *ud, const char *p, size_t n)
{
    rt_hreq *r = (rt_hreq *)ud;
    if (r->sink.body)
        r->sink.body(r->sink.ud, p, n);
}

static void h_header(void *ud, pt_str name, pt_str value)
{
    rt_hreq *r = (rt_hreq *)ud;
    if (r->sink.header)
        r->sink.header(r->sink.ud, name, value);
}

static void h_done(void *ud, const char *error)
{
    rt_hreq *r = (rt_hreq *)ud;
    r->rt->nhttp--;
    if (r->sink.done)
        r->sink.done(r->sink.ud, error);
    free(r);
}

void *pt_rt_http_start(pt_rt *rt, const pt_http_req *hr, const pt_http_sink *sink)
{
    rt_hreq *r = (rt_hreq *)calloc(1, sizeof *r);
    pt_http_sink mine;
    if (!r)
        return NULL;
    r->rt = rt;
    r->sink = *sink;
    mine.ud = r;
    mine.head = h_head;
    mine.body = h_body;
    mine.done = h_done;
    mine.header = h_header;
    if (!(r->h = rt->c.http->start(rt->c.http, hr, &mine))) {
        free(r);
        return NULL;
    }
    rt->nhttp++;
    return r;
}

void pt_rt_http_cancel(pt_rt *rt, void *h)
{
    rt_hreq *r = (rt_hreq *)h;
    if (!r)
        return;
    rt->c.http->cancel(rt->c.http, r->h);
    rt->nhttp--;
    free(r);
}

/* ---- epoch materials ---- */

static void answer_materials(pt_rt *rt, uint32_t id, pt_str current)
{
    /* The tools as registered now; the system prompt and skills built
     * again from their files, or kept when nothing builds them. */
    pt_materials m;
    pt_buf sys, skills;
    bool built;
    pt_buf_init(&sys, pt_sys_allocator());
    pt_buf_init(&skills, pt_sys_allocator());
    built = rt->prompt.have &&
            pt_rt_build_prompt(rt, pt_session_cwd(rt->a), pt_session_prompt(rt->a), &sys, &skills);
    memset(&m, 0, sizeof m);
    m.system = built ? pt_buf_str(&sys) : current;
    m.skills = built ? pt_buf_str(&skills) : PT_STR_NULL;
    m.tools = pt_rt_tool_defs(rt, &m.ntools);
    pt_fx_materials(rt->a, id, rt->prompt.have && !built ? NULL : &m);
    pt_buf_free(&sys);
    pt_buf_free(&skills);
}

void pt_rt_materials_hold(pt_rt *rt, bool on)
{
    if (on) {
        rt->mat.holds++;
        return;
    }
    if (rt->mat.holds && !--rt->mat.holds && rt->mat.fx) {
        uint32_t id = rt->mat.fx;
        rt->mat.fx = 0;
        /* the tools as they are now; the prompt rebuilt, or kept */
        answer_materials(rt, id, rt->mat.cur.oom ? PT_STR_NULL : pt_buf_str(&rt->mat.cur));
        pt_buf_reset(&rt->mat.cur);
    }
}

bool pt_rt_materials_waiting(const pt_rt *rt)
{
    return rt->mat.fx != 0;
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
    case PT_FX_BLOB:
        start_io(rt, fx);
        break;
    case PT_FX_EPOCH_MATERIALS:
        if (rt->mat.holds) {
            rt->mat.fx = fx->id; /* answered by the last release */
            pt_buf_reset(&rt->mat.cur);
            pt_buf_puts(&rt->mat.cur, fx->bytes);
        }
        else
            answer_materials(rt, fx->id, fx->bytes);
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
    while (!rt->io.pending && pt_poll(rt->a, &fx)) {
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
static void fire_watches(pt_rt *rt, const pt_waitfd *all, const uint64_t *generations, unsigned from,
                         unsigned to)
{
    unsigned i;
    uint32_t j;
    for (i = from; i < to; i++) {
        if (!all[i].ready)
            continue;
        for (j = 0; j < rt->nwatches; j++)
            if (rt->watches[j].fd == all[i].fd && rt->watches[j].generation == generations[i]) {
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
    uint64_t watch_generations[MAX_FDS] = {0};
    unsigned i, total = nfds, first_watch;
    uint32_t j;
    if (total > MAX_FDS / 2)
        total = MAX_FDS / 2;
    for (i = 0; i < nfds; i++)
        fds[i].ready = false;
    for (i = 0; i < total; i++)
        all[i] = fds[i];
    if (rt->io.pending) {
        all[total].fd = pt_worker_fd(rt->io.worker);
        all[total++].ready = false;
    }
    first_watch = total;
    for (j = 0; j < rt->nwatches && total < MAX_FDS; j++) {
        all[total].fd = rt->watches[j].fd;
        watch_generations[total] = rt->watches[j].generation;
        all[total++].ready = false;
    }
    if (wake(rt, now) + fire_timers(rt, now))
        return;
    until = next_wake(rt);
    if (until != INT64_MAX && (wait < 0 || until - now < wait))
        wait = until - now;
    if (wait > INT32_MAX)
        wait = INT32_MAX;
    if (rt->nreqs || rt->nhttp || total) {
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
        fire_watches(rt, all, watch_generations, first_watch, total);
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
        return !rt->closing && rt->builtin.idle && rt->builtin.idle(rt->builtin.ud) ? PT_RT_BUSY : PT_RT_IDLE;
    /* Watches, timers and requests outside a tool call (MCP servers, a
     * login) live on; only what the agent waits for keeps it from asking. */
    if (!rt->nreqs && !rt->nsleeps && !rt->io.pending && !rt->ncalls && !rt->mat.fx) {
        if (pt_asks(rt->a, NULL, 0))
            return PT_RT_ASK;
        if (!rt->ntimers && !rt->nwatches && !rt->nhttp)
            return PT_RT_STUCK;
    }
    return PT_RT_BUSY;
}

/* MCP servers start on the host thread, once its loop runs. */
static void start_mcp(pt_rt *rt)
{
    if (!rt->mcp.started && rt->mcp.start) {
        rt->mcp.started = true;
        rt->mcp.start(rt->mcp.ud);
    }
}

pt_rt_result pt_rt_step(pt_rt *rt, int timeout_ms, pt_waitfd *fds, unsigned nfds)
{
    start_mcp(rt);
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
    start_mcp(rt);
    for (;;) {
        pt_rt_result r;
        drain(rt);
        r = status(rt);
        /* Idle with background commands running: wait for them, so the
         * note that one ended wakes the model, as it would in a TUI. */
        if (r == PT_RT_IDLE && rt->builtin.running && rt->builtin.running(rt->builtin.ud))
            r = PT_RT_BUSY;
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
        while (!rt->io.pending && pt_poll(rt->a, &fx)) {
            progress = true;
            if (fx.kind == PT_FX_COMMIT || fx.kind == PT_FX_LIVE)
                handle(rt, &fx);
        }
        if (!progress && !rt->io.pending)
            break; /* a stuck core; never silently drop a large input queue */
    }
    finish_io(rt, true);
    return pt_closed(rt->a);
}

bool pt_rt_read_blob(void *store, pt_str ref, pt_arena *ar, pt_str *out)
{
    pt_store *st = (pt_store *)store;
    char hex[65], chunk[16384];
    pt_buf b;
    uint64_t off = 0;
    size_t got, i;
    bool ok = false;
    if (!st->blob_read || ref.n != 71 || memcmp(ref.p, "sha256:", 7) != 0)
        return false;
    for (i = 0; i < 64; i++) {
        char c = ref.p[7 + i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
        hex[i] = c;
    }
    hex[64] = 0;
    pt_buf_init(&b, pt_sys_allocator());
    while ((ok = st->blob_read(st, hex, off, chunk, sizeof chunk, &got) == 0) && got) {
        pt_buf_put(&b, chunk, got);
        off += got;
    }
    ok = ok && !b.oom;
    if (ok) {
        *out = pt_arena_dup(ar, pt_buf_str(&b));
        ok = !ar->oom;
    }
    pt_buf_free(&b);
    return ok;
}
