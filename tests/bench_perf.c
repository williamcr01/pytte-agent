/* Optimized offline workloads with backing-allocation counters. Including
 * arena.c here lets the benchmark inspect retained capacity without adding
 * instrumentation or counters to the production hot path. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core/core.h"
#include "tui/tui.h"
#include "base/arena.c"

typedef struct counts {
    size_t allocs, resizes, frees, requested, live, peak;
    size_t arena_cap, arena_used, buffer_cap;
} counts;

static void *count_alloc(void *ud, void *p, size_t old, size_t n)
{
    counts *c = (counts *)ud;
    void *out;
    if (!n) {
        free(p);
        c->frees++;
        c->live -= old;
        return NULL;
    }
    out = realloc(p, n);
    if (!out) {
        fprintf(stderr, "benchmark: out of memory\n");
        exit(1);
    }
    if (p)
        c->resizes++;
    else
        c->allocs++;
    c->requested += n;
    c->live = c->live - old + n;
    if (c->live > c->peak)
        c->peak = c->live;
    return out;
}

static void arena_usage(counts *c, const pt_arena *a)
{
    const pt_arena_chunk *ch;
    for (ch = a->head; ch; ch = ch->next) {
        c->arena_cap += ch->cap;
        c->arena_used += ch->used;
    }
}

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000 + (double)t.tv_nsec / 1000000;
}

static void must(bool ok)
{
    if (!ok) {
        fprintf(stderr, "benchmark: workload failed\n");
        exit(1);
    }
}

typedef struct host {
    pt_agent *a;
    pt_allocator al;
    pt_buf log, live, script;
    pt_arena response;
    int64_t now;
    pt_faux_state faux;
    uint64_t text_bytes;
    unsigned done;
} host;

static int64_t h_now(void *ud) { return ((host *)ud)->now; }
static void h_random(void *ud, void *p, size_t n) { (void)ud; memset(p, 0x5a, n); }
static void h_event(void *ud, const pt_event *ev)
{
    host *h = (host *)ud;
    if (ev->kind == PT_E_STREAM && ev->ai->kind == PT_EV_TEXT_DELTA)
        h->text_bytes += ev->ai->delta.n;
    if (ev->kind == PT_E_RUN_DONE) {
        must(pt_str_eqz(ev->outcome, "completed"));
        h->done++;
    }
}

static void h_run(host *h)
{
    pt_fx fx;
    unsigned guard = 0;
    while (pt_poll(h->a, &fx)) {
        must(++guard < 100000);
        if (fx.kind == PT_FX_COMMIT) {
            pt_buf_puts(&h->log, fx.bytes);
            must(!h->log.oom);
            pt_fx_done(h->a, fx.id, NULL);
        } else if (fx.kind == PT_FX_HTTP) {
            pt_faux_response r;
            size_t at = 0;
            pt_arena_reset(&h->response);
            /* The offline host replays the same response on each turn;
             * request construction still uses the full growing context. */
            must(pt_faux_respond(pt_buf_str(&h->script), PT_S("{\"turn\":0}"),
                                 &h->faux, &r, &h->response));
            pt_fx_head(h->a, fx.id, r.status, PT_STR_NULL);
            while (at < r.body.n) {
                size_t n = r.body.n - at < 16 ? r.body.n - at : 16;
                pt_fx_data(h->a, fx.id, r.body.p + at, n);
                at += n;
                h->now++;
            }
            pt_fx_done(h->a, fx.id, NULL);
        } else if (fx.kind == PT_FX_LIVE) {
            pt_buf_reset(&h->live);
            pt_buf_puts(&h->live, fx.bytes);
            must(!h->live.oom);
        } else if (fx.kind == PT_FX_SLEEP) {
            h->now = fx.until;
            pt_fx_done(h->a, fx.id, NULL);
        }
    }
    must(pt_idle(h->a) && !pt_fatal(h->a));
}

static void core_work(counts *c, size_t reply, unsigned turns)
{
    host h;
    pt_opts o;
    pt_session_cfg cfg;
    pt_identity me;
    pt_input in;
    size_t i;
    memset(&h, 0, sizeof h);
    h.al.fn = count_alloc;
    h.al.ud = c;
    h.now = 1760000000000;
    pt_buf_init(&h.log, h.al);
    pt_buf_init(&h.live, h.al);
    pt_buf_init(&h.script, h.al);
    pt_arena_init(&h.response, h.al, 4096);
    pt_buf_putz(&h.script, "{\"turns\":[{\"content\":[{\"type\":\"text\",\"text\":\"");
    for (i = 0; i < reply; i++)
        pt_buf_putc(&h.script, i % 64 == 63 ? ' ' : 'a');
    pt_buf_putz(&h.script, "\"}]}]}");
    must(!h.script.oom);
    memset(&o, 0, sizeof o);
    o.alloc = h.al;
    o.now_ms = h_now;
    o.random = h_random;
    o.on_event = h_event;
    o.ud = &h;
    o.config.model = PT_S("faux/faux");
    h.a = pt_new(&o);
    must(h.a != NULL);
    memset(&cfg, 0, sizeof cfg);
    cfg.cwd = PT_S("/offline/bench");
    cfg.host = PT_S("bench");
    cfg.system = PT_S("Be concise.");
    memset(&me, 0, sizeof me);
    me.host = cfg.host;
    me.boot = PT_S("boot");
    me.pid = 1;
    must(pt_create(h.a, &cfg) && pt_open(h.a, &me, false));
    h_run(&h);
    memset(&in, 0, sizeof in);
    in.text = PT_S("hello");
    for (i = 0; i < turns; i++) {
        must(pt_submit(h.a, &in));
        h_run(&h);
    }
    must(h.done == turns && h.text_bytes == reply * turns);
    arena_usage(c, &h.a->perm);
    arena_usage(c, &h.a->epoch);
    arena_usage(c, &h.a->scratch);
#ifndef PT_BENCH_LEGACY
    arena_usage(c, &h.a->objects);
#endif
    arena_usage(c, &h.a->reqs);
    arena_usage(c, &h.a->subs_a);
    arena_usage(c, &h.a->after_a);
    for (i = 0; i < h.a->ntasks; i++)
        arena_usage(c, &h.a->tasks[i]->a);
    for (i = 0; i < h.a->ninputs; i++)
        arena_usage(c, &h.a->inputs[i]->a);
    c->buffer_cap = h.a->commit.cap + h.a->live_out.cap;
    for (i = 0; i < PT_FXQ; i++)
        c->buffer_cap += h.a->q[i].bytes.cap;
    pt_free(h.a);
    pt_buf_free(&h.log);
    pt_buf_free(&h.live);
    pt_buf_free(&h.script);
    pt_arena_free(&h.response);
}

static void history_work(counts *c)
{
    pt_allocator al = {count_alloc, c};
    tui_input in;
    pt_buf file;
    unsigned i;
    char line[80];
    tui_input_init(&in, al);
    pt_buf_init(&file, al);
    for (i = 0; i < 1000; i++) {
        snprintf(line, sizeof line, "history entry %u: inspect the source", i);
        tui_hist_line(&file, pt_strz(line));
    }
    tui_hist_load(&in, pt_buf_str(&file), 1000);
    must(in.nhist == 1000);
    c->buffer_cap = in.text.cap + in.saved.cap + in.kill.cap;
#ifndef PT_BENCH_LEGACY
    c->buffer_cap += in.history_data.cap + in.hcap * sizeof *in.hist;
#else
    c->buffer_cap += in.hcap * sizeof *in.hist;
    for (i = 0; i < in.nhist; i++)
        c->buffer_cap += in.hist[i].cap;
#endif
    tui_input_free(&in);
    pt_buf_free(&file);
}

static void layout_work(counts *c)
{
    pt_allocator al = {count_alloc, c};
    tui_input in;
    unsigned i;
    tui_input_init(&in, al);
    for (i = 0; i < 1024; i++)
        tui_input_insert(&in, PT_S("a source line of text to wrap across a terminal row\n"));
    for (i = 0; i < 500; i++) {
        tui_input_insert(&in, PT_S("x"));
        tui_input_layout(&in, 76, 8);
        tui_input_layout(&in, 76, 8);
    }
    must(in.nrows >= 1024 && in.cur == in.text.n);
    c->buffer_cap = in.text.cap + in.saved.cap + in.kill.cap + (size_t)in.rcap * sizeof *in.rows;
    tui_input_free(&in);
}

static void editing_work(counts *c)
{
    pt_allocator al = {count_alloc, c};
    tui_input in;
    tui_ev key;
    unsigned i;
    tui_input_init(&in, al);
    memset(&key, 0, sizeof key);
    key.kind = TE_KEY;
    key.key = TK_BACKSPACE;
    for (i = 0; i < 1024; i++)
        tui_input_insert(&in, PT_S("a single logical line of text that wraps across the terminal "));
    for (i = 0; i < 500; i++) {
        must(tui_input_key(&in, &key, 76) == TI_EDITED);
        tui_input_layout(&in, 76, 8);
    }
    must(in.cur == in.text.n);
    c->buffer_cap = in.text.cap + (size_t)in.rcap * sizeof *in.rows;
    tui_input_free(&in);
}

int main(int argc, char **argv)
{
    const char *name = argc > 1 ? argv[1] : "core";
    unsigned iterations = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 10, i;
    counts c;
    double start, elapsed;
    memset(&c, 0, sizeof c);
    must(iterations != 0);
    start = now_ms();
    for (i = 0; i < iterations; i++) {
        c.arena_cap = c.arena_used = c.buffer_cap = 0;
        if (strcmp(name, "history") == 0)
            history_work(&c);
        else if (strcmp(name, "editing") == 0)
            editing_work(&c);
        else if (strcmp(name, "layout") == 0)
            layout_work(&c);
        else
            core_work(&c, strcmp(name, "stream") == 0 ? 65536 : 512,
                      strcmp(name, "conversation") == 0 ? 100 : 1);
        must(c.live == 0);
    }
    elapsed = now_ms() - start;
    printf("{\"workload\":\"%s\",\"iterations\":%u,\"elapsed_ms\":%.3f,"
           "\"allocations\":%lu,\"reallocations\":%lu,\"requested_bytes\":%lu,"
           "\"peak_live_bytes\":%lu,\"retained_arena_capacity\":%lu,"
           "\"retained_arena_used\":%lu,\"retained_buffer_capacity\":%lu}\n",
           name, iterations, elapsed, (unsigned long)c.allocs, (unsigned long)c.resizes,
           (unsigned long)c.requested, (unsigned long)c.peak, (unsigned long)c.arena_cap,
           (unsigned long)c.arena_used, (unsigned long)c.buffer_cap);
    return 0;
}
