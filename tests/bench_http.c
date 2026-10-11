/* Full agent turns over real HTTP/TLS against a local replay server.
 * No credentials or provider network are used. The Python harness creates
 * the server and trust bundle. Counters cover portable core backing storage;
 * Massif/DHAT cover host, curl and TLS allocations as well. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__GLIBC__)
#include <malloc.h>
#endif

#include "cli/cli.h"
#include "core/core.h"
#include "base/arena.c"

typedef struct counters {
    size_t allocations, reallocations, frees, requested, live, peak;
} counters;

static void must(bool ok, const char *why)
{
    if (!ok) {
        fprintf(stderr, "bench_http: %s\n", why);
        exit(1);
    }
}

static void *count_alloc(void *ud, void *p, size_t old, size_t n)
{
    counters *c = (counters *)ud;
    void *out;
    bool resize = p != NULL;
    must(old <= c->live, "invalid allocator accounting");
    if (!n) {
        free(p);
        c->frees++;
        c->live -= old;
        return NULL;
    }
    out = realloc(p, n);
    must(out != NULL, "out of memory");
    if (resize)
        c->reallocations++;
    else
        c->allocations++;
    c->requested += n;
    c->live = c->live - old + n;
    if (c->live > c->peak)
        c->peak = c->live;
    return out;
}

static double now_ms(void)
{
    struct timespec ts;
    must(clock_gettime(CLOCK_MONOTONIC, &ts) == 0, "clock failed");
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

static void arena_usage(const pt_arena *ar, size_t *cap, size_t *used)
{
    const pt_arena_chunk *ch;
    for (ch = ar->head; ch; ch = ch->next) {
        *cap += ch->cap;
        *used += ch->used;
    }
}

static void snapshot(const char *phase, unsigned turn, const counters *c, const pt_agent *a,
                     double elapsed, double first, size_t bytes, const char *digest)
{
    size_t acap = 0, used = 0, bcap = 0;
    uint32_t i;
    task *t;
    if (a) {
        arena_usage(&a->perm, &acap, &used);
        arena_usage(&a->epoch, &acap, &used);
        arena_usage(&a->scratch, &acap, &used);
        arena_usage(&a->objects, &acap, &used);
        arena_usage(&a->reqs, &acap, &used);
        arena_usage(&a->subs_a, &acap, &used);
        arena_usage(&a->after_a, &acap, &used);
        for (i = 0; i < a->ntasks; i++) {
            arena_usage(&a->tasks[i]->a, &acap, &used);
            bcap += a->tasks[i]->rescued.cap + a->tasks[i]->gate_text.cap;
        }
        for (t = a->free_tasks; t; t = t->next_free) {
            arena_usage(&t->a, &acap, &used);
            bcap += t->rescued.cap + t->gate_text.cap;
        }
        for (i = 0; i < a->ninputs; i++)
            arena_usage(&a->inputs[i]->a, &acap, &used);
        bcap += a->commit.cap + a->live_out.cap;
        for (i = 0; i < PT_FXQ; i++)
            bcap += a->q[i].bytes.cap;
    }
    printf("{\"phase\":\"%s\",\"turn\":%u,\"elapsed_ms\":%.6f,\"first_text_ms\":%.6f,"
           "\"text_bytes\":%zu,\"core_allocations\":%zu,\"core_reallocations\":%zu,"
           "\"core_frees\":%zu,\"core_requested_bytes\":%zu,\"core_live_bytes\":%zu,"
           "\"core_peak_bytes\":%zu,\"retained_arena_capacity\":%zu,"
           "\"retained_arena_used\":%zu,\"retained_buffer_capacity\":%zu",
           phase, turn, elapsed, first, bytes, c->allocations, c->reallocations, c->frees,
           c->requested, c->live, c->peak, acap, used, bcap);
#if defined(__GLIBC__)
    {
        struct mallinfo2 m = mallinfo2();
        /* This includes allocator rounding and host/TLS storage; it is
         * distinct from requested core bytes and measured resident pages. */
        printf(",\"libc_live_bytes\":%zu,\"libc_retained_bytes\":%zu",
               m.uordblks + m.hblkhd, m.arena + m.hblkhd);
    }
#endif
#if defined(__linux__)
    {
        char line[256], key[64];
        unsigned long n, rss = 0, pss = 0, priv = 0, anon = 0;
        FILE *f = fopen("/proc/self/smaps_rollup", "r");
        must(f != NULL, "cannot read smaps_rollup");
        while (fgets(line, sizeof line, f)) {
            if (sscanf(line, "%63s %lu kB", key, &n) != 2)
                continue;
            if (!strcmp(key, "Rss:")) rss = n;
            if (!strcmp(key, "Pss:")) pss = n;
            if (!strcmp(key, "Private_Clean:") || !strcmp(key, "Private_Dirty:")) priv += n;
            if (!strcmp(key, "Anonymous:")) anon = n;
        }
        must(!ferror(f), "cannot read memory snapshot");
        fclose(f);
        printf(",\"rss_kib\":%lu,\"pss_kib\":%lu,\"uss_kib\":%lu,\"anonymous_kib\":%lu",
               rss, pss, priv, anon);
    }
#endif
    if (getenv("PYTTE_BENCH_SMAPS") && !strcmp(phase, "response") && turn == 1) {
        char block[1024];
        size_t count;
        FILE *maps = fopen("/proc/self/smaps", "r");
        FILE *output = fopen(getenv("PYTTE_BENCH_SMAPS"), "w");
        must(maps && output, "cannot open mapping snapshot");
        while ((count = fread(block, 1, sizeof block, maps)) != 0)
            must(fwrite(block, 1, count, output) == count, "cannot write mapping snapshot");
        must(!ferror(maps), "cannot read mappings");
        fclose(maps);
        must(fclose(output) == 0, "cannot close mapping snapshot");
    }
    printf(",\"sha256\":\"%s\"}\n", digest);
    fflush(stdout);
}

typedef struct route {
    pt_http_driver vt;
    pt_http_driver *curl;
    const char *url;
} route;

static void *route_start(pt_http_driver *vd, const pt_http_req *req, const pt_http_sink *sink)
{
    route *r = (route *)vd;
    pt_http_req local = *req;
    local.url = r->url;
    return r->curl->start(r->curl, &local, sink);
}
static void route_cancel(pt_http_driver *vd, void *handle)
{
    route *r = (route *)vd;
    r->curl->cancel(r->curl, handle);
}
static int route_pump(pt_http_driver *vd, int timeout, pt_waitfd *fds, unsigned n)
{
    route *r = (route *)vd;
    return r->curl ? r->curl->pump(r->curl, timeout, fds, n) : pt_sys_wait(fds, n, timeout);
}

typedef struct observer {
    unsigned done;
    size_t bytes;
    double start, first, deadline;
    bool failed;
    pt_sha256 hash;
} observer;

static int64_t h_now(void *ud) { (void)ud; return pt_sys_now_ms(); }
static bool credentials(void *ud, const pt_provider *p, pt_credentials *out)
{
    (void)ud; (void)p;
    out->api_key = PT_S("offline-benchmark");
    return true;
}
static bool stop(void *ud) { return now_ms() > ((observer *)ud)->deadline; }
static void event(void *ud, const pt_event *ev)
{
    observer *o = (observer *)ud;
    if (ev->kind == PT_E_STREAM && ev->ai->kind == PT_EV_TEXT_DELTA) {
        if (!o->bytes)
            o->first = now_ms() - o->start;
        o->bytes += ev->ai->delta.n;
        pt_sha256_update(&o->hash, ev->ai->delta.p, ev->ai->delta.n);
    }
    if (ev->kind == PT_E_RUN_DONE) {
        o->done++;
        o->failed |= !pt_str_eqz(ev->outcome, "completed");
    }
}

static void sink_body(void *ud, const char *p, size_t n)
{
    observer *o = (observer *)ud;
    if (!o->bytes)
        o->first = now_ms() - o->start;
    o->bytes += n;
    pt_sha256_update(&o->hash, p, n);
}
static void sink_done(void *ud, const char *error)
{
    observer *o = (observer *)ud;
    o->done++;
    o->failed = error != NULL;
    if (error)
        fprintf(stderr, "transport: %s\n", error);
}

int main(int argc, char **argv)
{
    counters c = {0};
    route r;
    observer obs = {0};
    pt_agent *a = NULL;
    pt_rt *rt = NULL;
    pt_store *store = NULL;
    pt_worker *worker = NULL;
    unsigned turns, i;
    size_t expected;
    double t0;
    bool transport, expect_error;
    must(argc >= 4 && argc <= 6, "usage: bench/http URL TURNS TEXT_BYTES [SESSION_DIR | transport [error]]");
    turns = (unsigned)strtoul(argv[2], NULL, 10);
    expected = (size_t)strtoull(argv[3], NULL, 10);
    must(turns > 0 && turns <= 10000 && expected > 0, "invalid workload size");
    transport = argc >= 5 && !strcmp(argv[4], "transport");
    expect_error = argc == 6 && !strcmp(argv[5], "error");
    must(!expect_error || transport, "error mode requires transport");
    memset(&r, 0, sizeof r);
    r.vt.start = route_start;
    r.vt.cancel = route_cancel;
    r.vt.pump = route_pump;
    r.url = argv[1];
    if (!transport) {
        pt_opts opts;
        pt_session_cfg cfg;
        pt_identity me;
        pt_rt_cfg rcfg;
        char err[512];
        memset(&opts, 0, sizeof opts);
        opts.alloc = (pt_allocator){count_alloc, &c};
        opts.now_ms = h_now;
        opts.random = pt_sys_random;
        opts.credentials = credentials;
        opts.on_event = event;
        opts.ud = &obs;
        opts.config.model = PT_S(DEFAULT_MODEL);
        opts.config.context_window = 100000000;
        a = pt_new(&opts);
        must(a != NULL, "cannot create agent");
        store = argc >= 5 ? pt_store_posix_open(argv[4], true, err, sizeof err) : pt_store_memory_new();
        must(store != NULL, "cannot open store");
        memset(&rcfg, 0, sizeof rcfg);
        rcfg.store = store;
        rcfg.http = &r.vt;
        rcfg.stop = stop;
        rcfg.stop_ud = &obs;
        rt = pt_rt_new(&rcfg, a);
        worker = pt_worker_new();
        must(rt && worker && pt_rt_add_builtins(rt), "cannot initialize runtime");
        pt_rt_use_worker(rt, worker);
        memset(&cfg, 0, sizeof cfg);
        cfg.cwd = PT_S("/offline/benchmark");
        cfg.host = PT_S("benchmark");
        cfg.system = pt_strz(cli_default_system);
        cfg.tools = pt_rt_tool_defs(rt, &cfg.ntools);
        memset(&me, 0, sizeof me);
        me.host = cfg.host;
        me.boot = PT_S("benchmark");
        me.pid = 1;
        obs.deadline = now_ms() + 30000;
        must(pt_create(a, &cfg) && pt_open(a, &me, false), "cannot open agent");
        must(pt_rt_run(rt) == PT_RT_IDLE, "setup failed");
    }
    snapshot("startup", 0, &c, a, 0, 0, 0, "");
    t0 = now_ms();
    r.curl = pt_http_curl_new();
    must(r.curl != NULL, "cannot load curl");
    snapshot("curl_init", 0, &c, a, now_ms() - t0, 0, 0, "");
    for (i = 1; i <= turns; i++) {
        obs.bytes = 0;
        obs.first = 0;
        pt_sha256_init(&obs.hash);
        obs.start = now_ms();
        obs.deadline = obs.start + 30000;
        if (transport) {
            pt_http_req req;
            pt_http_sink sink = {&obs, NULL, sink_body, sink_done};
            memset(&req, 0, sizeof req);
            req.method = "POST";
            req.url = r.url;
            req.body = PT_S("{}");
            must(r.curl->start(r.curl, &req, &sink) != NULL, "cannot start transfer");
            while (obs.done < i && now_ms() < obs.deadline)
                must(r.curl->pump(r.curl, 10, NULL, 0) >= 0, "transport failed");
            must(obs.done == i && obs.failed == expect_error, "unexpected TLS result");
            if (!expect_error)
                must(obs.bytes == expected, "incomplete transport response");
        } else {
            pt_input in;
            memset(&in, 0, sizeof in);
            in.text = PT_S("hello");
            must(pt_submit(a, &in), "submit failed");
            must(pt_rt_run(rt) == PT_RT_IDLE && !pt_fatal(a), "agent run failed");
            must(obs.done == i && !obs.failed && obs.bytes == expected, "incomplete agent response");
        }
        {
            unsigned char hash[32];
            char digest[65];
            double elapsed = now_ms() - obs.start;
            pt_sha256_final(&obs.hash, hash);
            pt_hex(digest, hash, sizeof hash);
            digest[64] = 0;
            snapshot("response", i, &c, a, elapsed, obs.first, obs.bytes, digest);
        }
    }
    if (rt) {
        must(pt_rt_close(rt), "close failed");
        pt_rt_free(rt);
        pt_worker_free(worker);
        if (argc < 5)
            pt_store_memory_free(store);
        pt_free(a);
    }
    r.curl->free(r.curl);
    must(c.live == 0, "core leaked counted storage");
    snapshot("freed", turns, &c, NULL, 0, 0, 0, "");
    return 0;
}
