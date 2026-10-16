/* Deterministic simulation with exhaustive crash testing (section 22.2).
 *
 * A "world" holds what outlives a process: the store (the disk) and the
 * faux server. A "client" is one process: it opens the session, submits
 * its prompts (with request IDs, so submitting again after a crash is a
 * no-op), and runs until idle.
 *
 * For each scenario: run once without faults and record the transcript,
 * the input outcomes and every request sent. Then, for every commit n, run
 * again with commit n failing (torn at several points, or fully written
 * just before the process dies), restart clients until one finishes, and
 * check that the result is the same and that every request sent was one
 * the reference run sent too (a re-sent request has the same bytes).
 *
 * A scenario with real file tools (TOOLS_FILES) works in a directory that
 * outlives the clients as the disk does, and may rewind to one of its
 * prompts once idle: the files it leaves must be the same however often
 * the process died, including during the rewind itself. */
#define _POSIX_C_SOURCE 200809L
#include "t.h"

#include "core/core.h"
#include "rt/rt.h"

#include <dirent.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

static bool use_step; /* drive clients with pt_rt_step instead of pt_rt_run */

static bool w_stop(void *ud);

static pt_rt_result run_stepped(pt_rt *rt, void *w)
{
    pt_rt_result r;
    while ((r = pt_rt_step(rt, 100, NULL, 0)) == PT_RT_BUSY)
        w_stop(w);
    return r;
}

typedef struct rec_http { /* the faux server, remembering what it was asked */
    pt_http_driver vt;
    pt_http_driver *inner;
    char **bodies;
    uint32_t n, cap;
} rec_http;

static void *rh_start(pt_http_driver *d, const pt_http_req *req, const pt_http_sink *sink)
{
    rec_http *r = (rec_http *)d;
    if (r->n == r->cap) {
        r->cap = r->cap ? r->cap * 2 : 16;
        r->bodies = (char **)realloc(r->bodies, r->cap * sizeof *r->bodies);
    }
    r->bodies[r->n] = (char *)malloc(req->body.n + 1);
    memcpy(r->bodies[r->n], req->body.p, req->body.n);
    r->bodies[r->n++][req->body.n] = 0;
    return r->inner->start(r->inner, req, sink);
}

static void rh_cancel(pt_http_driver *d, void *h)
{
    rec_http *r = (rec_http *)d;
    r->inner->cancel(r->inner, h);
}

static int rh_pump(pt_http_driver *d, int timeout_ms, pt_waitfd *fds, unsigned nfds)
{
    rec_http *r = (rec_http *)d;
    return r->inner->pump(r->inner, timeout_ms, fds, nfds);
}

static void rh_free(pt_http_driver *d)
{
    rec_http *r = (rec_http *)d;
    uint32_t i;
    r->inner->free(r->inner);
    for (i = 0; i < r->n; i++)
        free(r->bodies[i]);
    free(r->bodies);
    free(r);
}

typedef struct scenario scenario;

typedef struct world {
    const scenario *sc;
    pt_agent *agent;  /* the current client's */
    uint32_t streamed; /* stream events of the first prompt's run */
    bool aborted;
    pt_store *store;
    pt_store *via; /* when set, clients use this store (wrapping store) */
    char err[512]; /* the last client's fatal error */
    rec_http *http;
    /* the fake tools: how often each unsafe call ran, and look's timers */
    struct {
        char id[64];
        uint32_t runs;
    } unsafe[16];
    uint32_t nunsafe;
    struct pend {
        struct world *w;
        uint32_t fx, timer;
        bool used, big;
    } pend[8];
    uint32_t most_looks; /* at once */
    int64_t now;
    uint64_t rng;
    char dir[128]; /* the files of a TOOLS_FILES scenario */
} world;

static uint32_t rnd(world *w)
{
    w->rng ^= w->rng << 13;
    w->rng ^= w->rng >> 7;
    w->rng ^= w->rng << 17;
    return (uint32_t)(w->rng >> 16);
}

static int64_t w_now(void *ud)
{
    return ++((world *)ud)->now;
}

static void w_sleep(void *ud, uint32_t ms)
{
    ((world *)ud)->now += ms;
}

static void w_random(void *ud, void *out, size_t n)
{
    unsigned char *p = (unsigned char *)out;
    size_t i;
    for (i = 0; i < n; i++)
        p[i] = (unsigned char)rnd((world *)ud);
}

static void world_init(world *w, const char *script, uint64_t seed)
{
    memset(w, 0, sizeof *w);
    w->store = pt_store_memory_new();
    w->http = (rec_http *)calloc(1, sizeof *w->http);
    w->http->inner = pt_http_faux_new(pt_strz(script));
    w->http->vt.start = rh_start;
    w->http->vt.cancel = rh_cancel;
    w->http->vt.pump = rh_pump;
    w->http->vt.free = rh_free;
    w->now = 1760000000000;
    w->rng = seed * 2654435761u + 88172645463325252ull;
    snprintf(w->dir, sizeof w->dir, "%s/pytte-sim-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    CHECK(mkdtemp(w->dir) != NULL);
}

static void world_free(world *w)
{
    DIR *d = opendir(w->dir);
    struct dirent *e;
    char path[400];
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        snprintf(path, sizeof path, "%s/%s", w->dir, e->d_name);
        unlink(path);
    }
    if (d)
        closedir(d);
    rmdir(w->dir);
    pt_store_memory_free(w->store);
    rh_free(&w->http->vt);
}

enum { ABORT_NONE, ABORT_STREAM, ABORT_RETRY, ABORT_TOOL, ABORT_COMPACT };
enum { TOOLS_NONE, TOOLS_AUTO, TOOLS_ASK, TOOLS_FILES };

/* The user interrupts the first prompt's run: after abort_after stream
 * events, or once it waits to retry. Each client decides again from what
 * it sees, as a user would after a crash lost an abort that never landed. */
struct scenario {
    const char *name, *script;
    const char *prompts[3];
    int abort;
    uint32_t abort_after;
    int tools; /* fake tools, run without asking or approved by the client */
    uint32_t window; /* context window (tokens), to make it compact */
    uint32_t together; /* the most looks the reference run has at once; 0: not checked */
    uint32_t steer;    /* bit i: prompt i is sent as a steer; the reference run is then one run */
    uint32_t rewind;   /* once idle, rewind to this prompt (1 is the first); 0: no */
};

static task *first_run(world *w)
{
    uint32_t i;
    for (i = 0; i < w->agent->ntasks; i++) {
        task *t = w->agent->tasks[i];
        input *in = pt_core_input(w->agent, t->input);
        if (in && pt_str_eqz(in->text, w->sc->prompts[0]))
            return t;
    }
    return NULL;
}

static void w_event(void *ud, const pt_event *ev)
{
    world *w = (world *)ud;
    task *t;
    if (ev->kind == PT_E_STREAM && w->sc->abort == ABORT_STREAM && (t = first_run(w)) && t->id == ev->task)
        w->streamed++;
}

/* Between steps (not inside the agent's callbacks). */
static bool w_stop(void *ud)
{
    world *w = (world *)ud;
    task *t = first_run(w);
    uint32_t i;
    if (w->sc->abort == ABORT_COMPACT) /* whichever run waits for a summary */
        for (i = 0, t = NULL; i < w->agent->ntasks; i++)
            if (w->agent->tasks[i]->phase == PH_COMPACT && w->agent->tasks[i]->status != ST_DONE)
                t = w->agent->tasks[i];
    if (!w->aborted && t && t->status != ST_DONE &&
        ((w->sc->abort == ABORT_STREAM && w->streamed >= w->sc->abort_after) ||
         (w->sc->abort == ABORT_RETRY && t->phase == PH_RETRY) ||
         (w->sc->abort == ABORT_TOOL && t->phase == PH_TOOLS) ||
         (w->sc->abort == ABORT_COMPACT && t->phase == PH_COMPACT))) {
        w->aborted = true;
        CHECK(pt_abort(w->agent));
    }
    return false;
}

static pt_str big_prompt(void);

/* Where needle starts in s, or NULL. */
static const char *find(pt_str s, const char *needle)
{
    size_t n = strlen(needle), i;
    for (i = 0; i + n <= s.n; i++)
        if (memcmp(s.p + i, needle, n) == 0)
            return s.p + i;
    return NULL;
}

/* ---- fake tools ----
 * count: unsafe, runs at once; how often each call ran is kept in the
 *        world, which outlives the process (an unsafe call runs at most once).
 *        Not parallel, so it never runs while a look does.
 * look:  safe and parallel; answers a little later, from a timer. Looking
 *        at "big" outputs 20000 bytes: past the cap, so into a blob. */

static void count_run(void *ud, pt_rt *rt, const pt_fx *fx)
{
    world *w = (world *)ud;
    pt_rt_out r;
    uint32_t i;
    for (i = 0; i < 8; i++)
        CHECK(!w->pend[i].used); /* runs alone */
    for (i = 0; i < w->nunsafe && !pt_str_eqz(fx->call_id, w->unsafe[i].id); i++)
        ;
    if (i == w->nunsafe && w->nunsafe < 16)
        snprintf(w->unsafe[w->nunsafe++].id, 64, "%.*s", (int)fx->call_id.n, fx->call_id.p);
    if (i < 16)
        w->unsafe[i].runs++;
    memset(&r, 0, sizeof r);
    r.content = PT_S("counted");
    pt_rt_tool_done(rt, fx->id, &r);
}

static void look_fire(void *ud, pt_rt *rt)
{
    struct pend *p = (struct pend *)ud;
    pt_rt_out r;
    p->used = false;
    memset(&r, 0, sizeof r);
    pt_rt_tool_output(rt, p->fx, "looking\n", 8);
    if (p->big) {
        char chunk[4000];
        int k;
        memset(chunk, 'x', sizeof chunk);
        for (k = 0; k < 5; k++) {
            chunk[0] = (char)('0' + k);
            pt_rt_tool_output(rt, p->fx, chunk, sizeof chunk);
        }
    }
    r.note = PT_S("looked");
    pt_rt_tool_done(rt, p->fx, &r);
}

static void look_run(void *ud, pt_rt *rt, const pt_fx *fx)
{
    world *w = (world *)ud;
    uint32_t i, n;
    for (i = 0; i < 8 && w->pend[i].used; i++)
        ;
    if (i == 8) {
        CHECK(!"too many looks at once");
        return;
    }
    w->pend[i].w = w;
    w->pend[i].fx = fx->id;
    w->pend[i].used = true;
    w->pend[i].big = find(fx->args, "\"big\"") != NULL;
    w->pend[i].timer = pt_rt_timer(rt, pt_rt_now(rt) + 5, look_fire, &w->pend[i]);
    for (i = 0, n = 0; i < 8; i++)
        n += w->pend[i].used;
    if (n > w->most_looks)
        w->most_looks = n;
}

static void look_cancel(void *ud, pt_rt *rt, uint32_t fx)
{
    world *w = (world *)ud;
    uint32_t i;
    for (i = 0; i < 8; i++)
        if (w->pend[i].used && w->pend[i].fx == fx) {
            pt_rt_out r;
            pt_rt_timer_cancel(rt, w->pend[i].timer);
            w->pend[i].used = false;
            memset(&r, 0, sizeof r);
            r.content = PT_S("stopped");
            r.is_error = true;
            pt_rt_tool_done(rt, fx, &r);
        }
}

static void add_tools(world *w, pt_rt *rt)
{
    pt_rt_tool t[2];
    memset(t, 0, sizeof t);
    t[0].def.name = PT_S("count");
    t[0].def.description = PT_S("Count.");
    t[0].def.schema = PT_S("{\"type\":\"object\"}");
    t[0].run = count_run;
    t[0].ud = w;
    t[1].def.name = PT_S("look");
    t[1].def.description = PT_S("Look.");
    t[1].def.schema = PT_S("{\"type\":\"object\",\"properties\":{\"at\":{\"type\":\"string\"}}}");
    t[1].def.replay = PT_REPLAY_SAFE;
    t[1].def.parallel = true;
    t[1].def.read_only = true;
    t[1].run = look_run;
    t[1].cancel = look_cancel;
    t[1].ud = w;
    CHECK(pt_rt_tool_add(rt, &t[0]));
    CHECK(pt_rt_tool_add(rt, &t[1]));
    pt_rt_set_perm(rt, w->sc->tools == TOOLS_ASK ? PT_PERM_ASK : PT_PERM_AUTO);
}

/* The user says yes to every question. */
static bool approve_all(pt_agent *a)
{
    pt_ask_view v[8];
    uint32_t n = pt_asks(a, v, 8), i;
    for (i = 0; i < n && i < 8; i++)
        CHECK(pt_approve(a, v[i].task, true, PT_STR_NULL));
    return n > 0;
}

/* Rewind to the nth prompt: plan from the log, then let the core's task put
 * the files back. A plan with nothing in it is a rewind that already
 * happened (the files are as they were). */
static pt_rt_result rewind_to(world *w, pt_rt *rt, pt_agent *a, uint32_t nth)
{
    char err[512];
    pt_rt_rewind *r = pt_rt_rewind_scan(rt, err, sizeof err);
    const pt_rt_point *p;
    pt_rt_plan plan;
    uint32_t n = 0;
    pt_rt_result res = PT_RT_IDLE;
    CHECK(r != NULL);
    if (!r)
        return PT_RT_FATAL;
    p = pt_rt_rewind_points(r, &n);
    CHECK(n >= nth);
    if (n >= nth && pt_rt_rewind_plan(rt, r, p[nth - 1].entry, &plan)) {
        if (plan.n) {
            CHECK(pt_rewind(a, p[nth - 1].entry, plan.files, plan.n));
            res = use_step ? run_stepped(rt, w) : pt_rt_run(rt);
        }
        pt_rt_plan_free(&plan);
    }
    pt_rt_rewind_free(r);
    return res;
}

/* One process. Returns how its run loop ended. */
static pt_rt_result client(world *w, const scenario *sc)
{
    pt_opts o;
    pt_rt_cfg rc;
    pt_agent *a;
    pt_rt *rt;
    pt_identity me;
    pt_rt_result res = PT_RT_FATAL;
    char err[512];
    int i;
    memset(&o, 0, sizeof o);
    o.alloc = T_AL;
    o.now_ms = w_now;
    o.random = w_random;
    o.ud = w;
    o.blob = pt_rt_read_blob;
    o.blob_ud = w->store;
    o.on_event = w_event;
    o.config.model = PT_S("faux/faux");
    o.config.live_interval_ms = 1;
    o.config.context_window = sc->window;
    a = pt_new(&o);
    w->sc = sc;
    w->agent = a;
    w->streamed = 0;
    w->aborted = false;
    memset(&rc, 0, sizeof rc);
    rc.store = w->via ? w->via : w->store;
    rc.stop = w_stop;
    rc.stop_ud = w;
    rc.http = &w->http->vt;
    rc.now = w_now;
    rc.sleep = w_sleep;
    rc.clock_ud = w;
    rt = pt_rt_new(&rc, a);
    memset(w->pend, 0, sizeof w->pend); /* a crash took the timers along */
    if (sc->tools == TOOLS_FILES) {
        CHECK(pt_rt_add_builtins(rt));
        pt_rt_set_perm(rt, PT_PERM_AUTO);
    } else if (sc->tools) {
        add_tools(w, rt);
    }
    memset(&me, 0, sizeof me);
    me.host = PT_S("sim");
    me.pid = 1;
    if (pt_store_memory_log(w->store).n && !pt_rt_recover(rt, err, sizeof err) &&
        pt_store_memory_log(w->store).n) {
        fprintf(stderr, "recover: %s\n", err);
        CHECK(!"recovery failed");
        goto out;
    }
    if (!pt_store_memory_log(w->store).n) {
        /* nothing durable yet (or only a torn first line): create the session */
        pt_session_cfg c;
        memset(&c, 0, sizeof c);
        c.cwd = sc->tools == TOOLS_FILES ? pt_strz(w->dir) : PT_S("/sim");
        c.host = PT_S("sim");
        c.system = PT_S("You are being simulated.");
        c.tools = pt_rt_tool_defs(rt, &c.ntools);
        CHECK(pt_create(a, &c));
    }
    CHECK(pt_open(a, &me, false));
    for (i = 0; i < 3 && sc->prompts[i]; i++) {
        pt_input in;
        char req[8] = {'p', (char)('0' + i), 0};
        memset(&in, 0, sizeof in);
        in.text = strcmp(sc->prompts[i], "@big") == 0 ? big_prompt() : pt_strz(sc->prompts[i]);
        in.req = pt_strz(req);
        in.steer = (sc->steer >> i & 1) != 0;
        CHECK(pt_submit(a, &in));
    }
    do
        res = use_step ? run_stepped(rt, w) : pt_rt_run(rt);
    while (res == PT_RT_ASK && approve_all(a));
    if (res == PT_RT_IDLE && sc->rewind)
        res = rewind_to(w, rt, a, sc->rewind);
    if (res == PT_RT_IDLE && !pt_rt_close(rt))
        res = PT_RT_FATAL; /* the close commit failed: also a crash */
    snprintf(w->err, sizeof w->err, "%s", pt_error(a));
out:
    pt_rt_free(rt);
    pt_free(a);
    w->agent = NULL;
    return res;
}

/* What must come out the same however often the process died. */
static void outcome(world *w, pt_buf *out)
{
    pt_opts o;
    pt_rt_cfg rc;
    pt_agent *a;
    pt_rt *rt;
    pt_entry_view v;
    char err[512];
    uint32_t i, j;
    memset(&o, 0, sizeof o);
    o.alloc = T_AL;
    o.now_ms = w_now;
    o.ud = w;
    o.blob = pt_rt_read_blob;
    o.blob_ud = w->store;
    a = pt_new(&o);
    memset(&rc, 0, sizeof rc);
    rc.store = w->store;
    rt = pt_rt_new(&rc, a);
    pt_buf_reset(out);
    if (!pt_rt_recover(rt, err, sizeof err)) {
        pt_buf_putz(out, "unrecoverable: ");
        pt_buf_putz(out, err);
    }
    for (i = 0; pt_entry_at(a, i, &v); i++) {
        if (!v.msg)
            continue; /* error entries: when errors happen depends on the crashes */
        if (v.msg->role == PT_ROLE_ASSISTANT &&
            (v.msg->stop == PT_STOP_ERROR || v.msg->stop == PT_STOP_ABORTED))
            continue; /* partial answers: never sent to the model */
        pt_buf_putz(out, v.kind);
        pt_buf_putz(out, v.kept ? "(kept):" : ":");
        if (v.msg->role == PT_ROLE_ASSISTANT) {
            pt_buf_putz(out, pt_stop_name(v.msg->stop));
            for (j = 0; j < v.msg->nblocks; j++) {
                pt_buf_putc(out, '|');
                pt_buf_puts(out, v.msg->blocks[j].kind == PT_BLOCK_TOOL_CALL ? v.msg->blocks[j].name
                                                                             : v.msg->blocks[j].text);
            }
        } else if (strcmp(v.kind, "epoch") == 0) {
            pt_entry_view *ev = &v;
            pt_buf_puts(out, ev->msg->text);
            if (a->entries[i].summary.n) {
                pt_buf_putz(out, " / summary: ");
                pt_buf_puts(out, a->entries[i].summary);
                pt_buf_putz(out, " / request: ");
                pt_buf_puts(out, a->entries[i].request);
            }
        } else if (strcmp(v.kind, "tool_result") == 0) {
            /* an unsafe call cut short by a crash says "interrupted" */
            const char *ref = find(v.raw, "\"blob\":\"sha256:");
            pt_buf_puts(out, v.msg->name);
            if (ref) { /* the whole output, read back from the blob */
                char hex[65], back[32];
                size_t got = 0;
                uint64_t total = 0;
                snprintf(hex, sizeof hex, "%.64s", ref + 15);
                while (w->store->blob_read(w->store, hex, total, back, sizeof back, &got) == 0 && got)
                    total += got;
                pt_buf_putz(out, " blob ");
                pt_buf_u64(out, total);
            }
        } else {
            pt_buf_puts(out, v.msg->text);
            if (find(v.raw, "\"blob\":\"sha256:"))
                pt_buf_putz(out, " (in a blob)");
        }
        pt_buf_putc(out, '\n');
    }
    for (i = 0; i < a->ninputs; i++) {
        pt_buf_putz(out, "input ");
        pt_buf_puts(out, a->inputs[i]->text);
        pt_buf_putz(out, ": ");
        pt_buf_putz(out, pt_input_status_name(a->inputs[i]->status));
        pt_buf_putc(out, '\n');
    }
    for (i = 0; i < a->ntasks; i++) {
        pt_buf_putz(out, "run: ");
        pt_buf_puts(out, a->tasks[i]->outcome);
        if (a->tasks[i]->reason.n) {
            pt_buf_putz(out, ", ");
            pt_buf_puts(out, a->tasks[i]->reason);
        }
        pt_buf_putc(out, '\n');
    }
    {
        const pt_str log = pt_store_memory_log(w->store);
        pt_buf_putz(out, "log ends with newline: ");
        pt_buf_putz(out, log.n && log.p[log.n - 1] == '\n' ? "yes" : "no");
        pt_buf_putc(out, '\n');
    }
    pt_buf_putz(out, pt_idle(a) ? "idle\n" : "not idle\n");
    if (w->sc && w->sc->tools == TOOLS_FILES) {
        static const char *const names[] = {"a.txt", "b.txt", "c.txt"};
        for (i = 0; i < 3; i++) {
            char path[400], text[64];
            FILE *f;
            size_t n = 0;
            snprintf(path, sizeof path, "%s/%s", w->dir, names[i]);
            f = fopen(path, "rb");
            if (f) {
                n = fread(text, 1, sizeof text - 1, f);
                fclose(f);
            }
            text[n] = 0;
            pt_buf_putz(out, names[i]);
            pt_buf_putz(out, f ? ": " : ": (absent)");
            if (f)
                pt_buf_putz(out, text);
            pt_buf_putc(out, '\n');
        }
    }
    pt_rt_free(rt);
    pt_free(a);
}

/* Run clients until one ends idle. */
static bool converge(world *w, const scenario *sc, uint32_t *clients)
{
    uint32_t i;
    for (i = 0; i < 50; i++) {
        pt_rt_result r = client(w, sc);
        (*clients)++;
        if (r == PT_RT_IDLE)
            return true;
        CHECK(r == PT_RT_FATAL);
        if (r != PT_RT_FATAL)
            return false;
    }
    return false;
}

static bool known_request(const rec_http *ref, const char *body)
{
    uint32_t i;
    for (i = 0; i < ref->n; i++)
        if (strcmp(ref->bodies[i], body) == 0)
            return true;
    return false;
}

static int64_t field_of(const char *body, const char *key)
{
    pt_arena ar;
    int64_t t;
    pt_arena_init(&ar, T_AL, 0);
    t = pt_json_geti(pt_json_parse(pt_strz(body), &ar, NULL), key, -1);
    pt_arena_free(&ar);
    return t;
}

static void check_against(const scenario *sc, world *w, const pt_buf *want, const rec_http *ref,
                          const char *how, uint32_t n)
{
    pt_buf got;
    uint32_t i;
    pt_buf_init(&got, T_AL);
    outcome(w, &got);
    for (i = 0; i < w->nunsafe; i++) {
        t_checks++;
        if (w->unsafe[i].runs > 1) {
            t_failed++;
            fprintf(stderr, "%s: crash at commit %u (%s): unsafe call %s ran %u times\n", sc->name, n, how,
                    w->unsafe[i].id, w->unsafe[i].runs);
        }
    }
    t_checks++;
    if (!pt_str_eq(pt_buf_str(&got), pt_buf_str(want))) {
        t_failed++;
        fprintf(stderr, "%s: crash at commit %u (%s): outcome differs:\n%.*s---\n%.*s\n", sc->name, n,
                how, (int)got.n, got.p, (int)want->n, want->p);
        if (getenv("SIM_LOG"))
            fprintf(stderr, "%.*s", (int)pt_store_memory_log(w->store).n, pt_store_memory_log(w->store).p);
    }
    /* A request is built from committed state only, so every request with
     * the same history (the same number of messages) has the same bytes;
     * the first turn's must match the reference run's exactly. */
    for (i = 0; i < w->http->n; i++) {
        const char *bi = w->http->bodies[i];
        int64_t ti = field_of(bi, "turn"), mi = field_of(bi, "messages");
        bool ci = strstr(bi, "\"compact\":true") != NULL;
        uint32_t j;
        t_checks++;
        if (ti == 0 && !known_request(ref, bi)) {
            t_failed++;
            fprintf(stderr, "%s: crash at commit %u (%s): a first request unlike the reference's:\n%s\n",
                    sc->name, n, how, bi);
        }
        for (j = 0; j < i; j++) {
            t_checks++;
            const char *bj = w->http->bodies[j];
            /* the same point of the same epoch: turn, messages, and whether it is a summary */
            if (field_of(bj, "messages") == mi && field_of(bj, "turn") == ti &&
                (strstr(bj, "\"compact\":true") != NULL) == ci && strcmp(bj, bi) != 0) {
                t_failed++;
                fprintf(stderr, "%s: crash at commit %u (%s): a history of %lld messages was sent with different bytes:\n%s\n%s\n",
                        sc->name, n, how, (long long)mi, w->http->bodies[j], bi);
                break;
            }
        }
    }
    pt_buf_free(&got);
}

static void exhaustive(const scenario *sc)
{
    world ref, w;
    pt_buf want;
    uint32_t k, n, m, clients = 0, crashes = 0;
    const long keeps[] = {0, 1, 17, 60, 200, -1};
    pt_buf_init(&want, T_AL);

    world_init(&ref, sc->script, 1);
    CHECK(converge(&ref, sc, &clients));
    CHECK(clients == 1);
    CHECK(!sc->together || ref.most_looks == sc->together);
    k = pt_store_memory_appends(ref.store);
    outcome(&ref, &want);
    if (sc->steer) { /* every steer joined the first run */
        const char *at = find(pt_buf_str(&want), "run: ");
        CHECK(at && !find((pt_str){at + 1, (size_t)(want.p + want.n - (at + 1))}, "run: "));
    }
    if (getenv("SIM_SHOW"))
        fprintf(stderr, "%s:\n%.*s", sc->name, (int)want.n, want.p);
    if (getenv("SIM_LOG") && strcmp(getenv("SIM_LOG"), sc->name) == 0)
        fprintf(stderr, "%.*s", (int)pt_store_memory_log(ref.store).n, pt_store_memory_log(ref.store).p);

    for (n = 1; n <= k; n++) {
        for (m = 0; m < sizeof keeps / sizeof *keeps; m++) {
            char how[32];
            world_init(&w, sc->script, n * 31 + m);
            pt_store_memory_fail(w.store, n, keeps[m]);
            clients = 0;
            CHECK(converge(&w, sc, &clients));
            CHECK(clients == 2); /* exactly one crash */
            snprintf(how, sizeof how, "keep %ld", keeps[m]);
            check_against(sc, &w, &want, ref.http, how, n);
            world_free(&w);
            crashes++;
        }
    }

    /* chaos: many crashes per run, at random commits and tear points */
    for (n = 0; n < 60; n++) {
        world_init(&w, sc->script, 1000 + n);
        clients = 0;
        for (m = 0; m < 50; m++) {
            pt_rt_result r;
            if (m < 8)
                pt_store_memory_fail(w.store, 1 + rnd(&w) % 4, (long)(rnd(&w) % 300) - 50);
            else
                pt_store_memory_fail(w.store, 0, 0);
            r = client(&w, sc);
            if (r == PT_RT_IDLE)
                break;
            crashes++;
        }
        CHECK(m < 50);
        check_against(sc, &w, &want, ref.http, "chaos", 0);
        world_free(&w);
    }
    fprintf(stderr, "    %s: %u commits, %u crashed runs\n", sc->name, k, crashes);
    world_free(&ref);
    pt_buf_free(&want);
}

#define LONG                                                                                       \
    "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut "  \
    "labore et dolore magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco "     \
    "laboris nisi ut aliquip ex ea commodo consequat. Duis aute irure dolor in reprehenderit."

/* "@big" in a scenario's prompts: past PT_BLOB_MIN, so stored as a blob. */
static pt_str big_prompt(void)
{
    static char text[20000];
    size_t i;
    if (!text[0])
        for (i = 0; i < sizeof text; i++)
            text[i] = i % 64 == 63 ? '\n' : (char)('a' + (i / 64) % 26);
    return (pt_str){text, sizeof text};
}

static const scenario scenarios[] = {
    {"answer",
     "{\"chunk\":7,\"turns\":[{\"content\":[{\"type\":\"thinking\",\"text\":\"Easy.\"},"
     "{\"type\":\"text\",\"text\":\"The answer is 450, since 25 times 18 is 450.\"}]}]}",
     {"what is 25*18?", NULL, NULL}, ABORT_NONE, 0, TOOLS_NONE, 0, 0, 0},
    {"retry",
     "{\"chunk\":7,\"turns\":[[{\"status\":529,\"body\":{\"class\":\"retryable\",\"message\":\"overloaded\"}},"
     "{\"content\":[{\"type\":\"text\",\"text\":\"This stream gets cut off somewhere in the middle\"}],"
     "\"cut\":150},"
     "{\"content\":[{\"type\":\"text\",\"text\":\"Third time lucky.\"}]}]]}",
     {"go", NULL, NULL}, ABORT_NONE, 0, TOOLS_NONE, 0, 0, 0},
    {"tool_round",
     "{\"chunk\":9,\"turns\":[{\"content\":[{\"type\":\"text\",\"text\":\"Let me look.\"},"
     "{\"type\":\"tool_call\",\"name\":\"bash\",\"args\":{\"cmd\":\"ls\"}}]},"
     "{\"content\":[{\"type\":\"text\",\"text\":\"No tools here, so I will just answer.\"}]}]}",
     {"list the files", NULL, NULL}, ABORT_NONE, 0, TOOLS_NONE, 0, 0, 0},
    {"fatal",
     "{\"turns\":[{\"status\":401,\"body\":{\"class\":\"fatal\",\"message\":\"invalid key\"}}]}",
     {"hello?", NULL, NULL}, ABORT_NONE, 0, TOOLS_NONE, 0, 0, 0},
    {"length",
     "{\"chunk\":5,\"turns\":[{\"content\":[{\"type\":\"text\",\"text\":\"This is cut by max_tok\"}],"
     "\"stop\":\"length\"}]}",
     {"write a novel", NULL, NULL}, ABORT_NONE, 0, TOOLS_NONE, 0, 0, 0},
    {"two_inputs",
     "{\"chunk\":6,\"turns\":[{\"content\":[{\"type\":\"text\",\"text\":\"First answer.\"}]},"
     "{\"content\":[{\"type\":\"text\",\"text\":\"Second answer.\"}]}]}",
     {"first question", "second question", NULL}, ABORT_NONE, 0, TOOLS_NONE, 0, 0, 0},
    {"abort_stream",
     "{\"chunk\":5,\"turns\":[{\"content\":[{\"type\":\"thinking\",\"text\":\"Long.\"},"
     "{\"type\":\"text\",\"text\":\"A long answer that the user interrupts somewhere in the middle.\"}]}]}",
     {"first question", "second question", NULL}, ABORT_STREAM, 6, TOOLS_NONE, 0, 0, 0},
    {"abort_retry",
     "{\"turns\":[{\"status\":529,\"body\":{\"class\":\"retryable\",\"message\":\"overloaded\"}}]}",
     {"go", NULL, NULL}, ABORT_RETRY, 0, TOOLS_NONE, 0, 0, 0},
    {"tools",
     "{\"chunk\":11,\"turns\":[{\"content\":[{\"type\":\"text\",\"text\":\"Count, then look.\"},"
     "{\"type\":\"tool_call\",\"name\":\"count\",\"args\":{}},"
     "{\"type\":\"tool_call\",\"name\":\"look\",\"args\":{\"at\":\"a\"}}]},"
     "{\"content\":[{\"type\":\"tool_call\",\"name\":\"look\",\"args\":{\"at\":\"b\"}},"
     "{\"type\":\"tool_call\",\"name\":\"look\",\"args\":{\"at\":7}},"
     "{\"type\":\"tool_call\",\"name\":\"nope\",\"args\":{}}]},"
     "{\"content\":[{\"type\":\"tool_call\",\"name\":\"count\",\"args\":{}}]},"
     "{\"content\":[{\"type\":\"text\",\"text\":\"Counted twice and looked.\"}]}]}",
     {"count and look", NULL, NULL}, ABORT_NONE, 0, TOOLS_AUTO, 0, 0, 0},
    /* Looks run together, count alone: the round goes in three batches. */
    {"batches",
     "{\"chunk\":11,\"turns\":[{\"content\":[{\"type\":\"tool_call\",\"name\":\"look\",\"args\":{\"at\":\"a\"}},"
     "{\"type\":\"tool_call\",\"name\":\"look\",\"args\":{\"at\":\"b\"}},"
     "{\"type\":\"tool_call\",\"name\":\"count\",\"args\":{}},"
     "{\"type\":\"tool_call\",\"name\":\"look\",\"args\":{\"at\":\"c\"}},"
     "{\"type\":\"tool_call\",\"name\":\"look\",\"args\":{\"at\":\"d\"}},"
     "{\"type\":\"tool_call\",\"name\":\"look\",\"args\":{\"at\":\"e\"}}]},"
     "{\"content\":[{\"type\":\"text\",\"text\":\"Looked around a count.\"}]}]}",
     {"look, count, look", NULL, NULL}, ABORT_NONE, 0, TOOLS_AUTO, 0, 3, 0},
    {"approve",
     "{\"chunk\":11,\"turns\":[{\"content\":[{\"type\":\"tool_call\",\"name\":\"look\",\"args\":{}},"
     "{\"type\":\"tool_call\",\"name\":\"count\",\"args\":{}}]},"
     "{\"content\":[{\"type\":\"text\",\"text\":\"Approved.\"}]}]}",
     {"count, if I may", "and again", NULL}, ABORT_NONE, 0, TOOLS_ASK, 0, 0, 0},
    {"abort_tool",
     "{\"chunk\":11,\"turns\":[{\"content\":[{\"type\":\"tool_call\",\"name\":\"look\",\"args\":{}},"
     "{\"type\":\"tool_call\",\"name\":\"look\",\"args\":{}}]},"
     "{\"content\":[{\"type\":\"text\",\"text\":\"Next.\"}]}]}",
     {"look twice", "then this", NULL}, ABORT_TOOL, 0, TOOLS_AUTO, 0, 0, 0},
    {"big_prompt",
     "{\"chunk\":9,\"turns\":[{\"content\":[{\"type\":\"text\",\"text\":\"A long read.\"}]},"
     "{\"content\":[{\"type\":\"text\",\"text\":\"And a short one.\"}]}]}",
     {"@big", "short", NULL}, ABORT_NONE, 0, TOOLS_NONE, 0, 0, 0},
    {"big_output",
     "{\"chunk\":11,\"turns\":[{\"content\":[{\"type\":\"tool_call\",\"name\":\"look\",\"args\":{\"at\":\"big\"}}]},"
     "{\"content\":[{\"type\":\"text\",\"text\":\"That was long.\"}]}]}",
     {"look at it all", NULL, NULL}, ABORT_NONE, 0, TOOLS_AUTO, 0, 0, 0},
    {"compact",
     "{\"chunk\":40,\"turns\":["
     "{\"content\":[{\"type\":\"text\",\"text\":\"" LONG "\"}]},"
     "{\"content\":[{\"type\":\"text\",\"text\":\"Second. " LONG "\"}]},"
     "{\"content\":[{\"type\":\"text\",\"text\":\"Third. " LONG "\"}]},"
     "{\"content\":[{\"type\":\"text\",\"text\":\"Fourth, after a summary.\"}]}],"
     "\"compact\":{\"content\":[{\"type\":\"text\",\"text\":\"Three questions so far.\"}]}}",
     {"first question", "second question", "third question"}, ABORT_NONE, 0, TOOLS_NONE, 300, 0, 0},
    /* Between tool rounds, keeping the newest two. The window leaves room
     * for count's longer result when a crash interrupts it. */
    {"compact_midrun",
     "{\"chunk\":40,\"turns\":["
     "{\"content\":[{\"type\":\"text\",\"text\":\"" LONG "\"},"
     "{\"type\":\"tool_call\",\"name\":\"look\",\"args\":{\"at\":\"a\"}}]},"
     "{\"content\":[{\"type\":\"text\",\"text\":\"Again.\"},"
     "{\"type\":\"tool_call\",\"name\":\"look\",\"args\":{\"at\":\"b\"}}]},"
     "{\"content\":[{\"type\":\"tool_call\",\"name\":\"count\",\"args\":{}}]},"
     "{\"content\":[{\"type\":\"text\",\"text\":\"Looked twice and counted.\"}]}],"
     "\"compact\":{\"content\":[{\"type\":\"text\",\"text\":\"Looking at things.\"}]}}",
     {"look around", NULL, NULL}, ABORT_NONE, 0, TOOLS_AUTO, 440, 0, 0},
    {"overflow",
     "{\"chunk\":40,\"turns\":["
     "{\"content\":[{\"type\":\"text\",\"text\":\"" LONG LONG LONG "\"}]},"
     "{\"context_limit\":900,\"content\":[{\"type\":\"text\",\"text\":\"After the summary.\"}]}],"
     "\"compact\":[{\"status\":529,\"body\":{\"class\":\"retryable\",\"message\":\"busy\"}},"
     "{\"content\":[{\"type\":\"text\",\"text\":\"One short answer.\"}]}]}",
     {"first", "second", NULL}, ABORT_NONE, 0, TOOLS_NONE, 0, 0, 0},
    /* A steer waits for the tool round, then joins the next request. */
    {"steer_tool",
     "{\"chunk\":11,\"turns\":[{\"content\":[{\"type\":\"text\",\"text\":\"Looking.\"},"
     "{\"type\":\"tool_call\",\"name\":\"look\",\"args\":{\"at\":\"a\"}}]},"
     "{\"content\":[{\"type\":\"text\",\"text\":\"Looked, and heard the steer.\"}]}]}",
     {"look at a", "actually, be brief", NULL}, ABORT_NONE, 0, TOOLS_AUTO, 0, 0, 2},
    /* A run that ends with an answer starts the steer's turn in the same run. */
    {"steer_answer",
     "{\"chunk\":6,\"turns\":[{\"content\":[{\"type\":\"text\",\"text\":\"First answer.\"}]},"
     "{\"content\":[{\"type\":\"text\",\"text\":\"Answer to the steer.\"}]}]}",
     {"first question", "and one more thing", NULL}, ABORT_NONE, 0, TOOLS_NONE, 0, 0, 2},
    /* Two prompts' worth of file changes, then back to before the second:
     * a.txt as the first left it, c.txt gone. */
    {"rewind",
     "{\"chunk\":11,\"turns\":["
     "{\"content\":[{\"type\":\"tool_call\",\"name\":\"write\",\"args\":{\"path\":\"a.txt\",\"content\":\"one\\n\"}},"
     "{\"type\":\"tool_call\",\"name\":\"write\",\"args\":{\"path\":\"b.txt\",\"content\":\"bee\\n\"}}]},"
     "{\"content\":[{\"type\":\"text\",\"text\":\"Two files.\"}]},"
     "{\"content\":[{\"type\":\"tool_call\",\"name\":\"write\",\"args\":{\"path\":\"a.txt\",\"content\":\"two\\n\"}},"
     "{\"type\":\"tool_call\",\"name\":\"write\",\"args\":{\"path\":\"c.txt\",\"content\":\"sea\\n\"}}]},"
     "{\"content\":[{\"type\":\"tool_call\",\"name\":\"edit\",\"args\":{\"path\":\"a.txt\","
     "\"edits\":[{\"old\":\"two\",\"new\":\"three\"}]}},"
     "{\"type\":\"tool_call\",\"name\":\"write\",\"args\":{\"path\":\"b.txt\",\"content\":\"bee, again\\n\"}}]},"
     "{\"content\":[{\"type\":\"text\",\"text\":\"Changed them.\"}]}]}",
     {"make files", "change them", NULL}, ABORT_NONE, 0, TOOLS_FILES, 0, 0, 0, 2},
    {"abort_compact",
     "{\"chunk\":40,\"turns\":["
     "{\"content\":[{\"type\":\"text\",\"text\":\"" LONG "\"}]},"
     "{\"content\":[{\"type\":\"text\",\"text\":\"Second. " LONG "\"}]},"
     "{\"content\":[{\"type\":\"text\",\"text\":\"Third.\"}]}],"
     "\"compact\":{\"content\":[{\"type\":\"text\",\"text\":\"Never used.\"}]}}",
     {"one", "two", "three"}, ABORT_COMPACT, 0, TOOLS_NONE, 300, 0, 0},
};

/* Another writer appends to the log behind the owner's back (shared
 * storage, a bug): the size check before each commit stops the owner, and
 * the next open cuts the foreign line off as a torn tail. */
typedef struct intruder {
    pt_store vt;
    pt_store *inner;
    uint32_t at, calls;
} intruder;

static int in_size(pt_store *s, uint64_t *out)
{
    intruder *x = (intruder *)s;
    if (++x->calls == x->at) {
        static const char junk[] = "{\"seq\":99,\"from\":\"another writer\"}\n";
        x->inner->append(x->inner, junk, sizeof junk - 1, false);
    }
    return x->inner->size(x->inner, out);
}

#define FWD(name, args, call)                                                   \
    static int in_##name args                                                   \
    {                                                                           \
        intruder *x = (intruder *)s;                                            \
        int r = x->inner->name call;                                            \
        memcpy(s->err, x->inner->err, sizeof s->err);                           \
        return r;                                                               \
    }
FWD(append, (pt_store * s, const void *p, size_t n, bool sync), (x->inner, p, n, sync))
FWD(read, (pt_store * s, uint64_t off, void *p, size_t cap, size_t *got), (x->inner, off, p, cap, got))
FWD(truncate, (pt_store * s, uint64_t off), (x->inner, off))
FWD(live_put, (pt_store * s, uint32_t t, const void *p, size_t n), (x->inner, t, p, n))
FWD(live_del, (pt_store * s, uint32_t t), (x->inner, t))
FWD(live_list, (pt_store * s, void (*fn)(void *, uint32_t, pt_str), void *ud), (x->inner, fn, ud))

static void in_close(pt_store *s)
{
    (void)s;
}

static void test_another_writer(void)
{
    const scenario *sc = &scenarios[2];
    world ref, w;
    pt_buf want;
    intruder x;
    uint32_t clients = 0, at, k;
    pt_buf_init(&want, T_AL);
    world_init(&ref, sc->script, 1);
    CHECK(converge(&ref, sc, &clients));
    outcome(&ref, &want);
    k = pt_store_memory_appends(ref.store);
    for (at = 1; at <= k; at++) {
        world_init(&w, sc->script, at);
        memset(&x, 0, sizeof x);
        x.inner = w.store;
        x.at = at;
        x.vt.size = in_size;
        x.vt.append = in_append;
        x.vt.read = in_read;
        x.vt.truncate = in_truncate;
        x.vt.live_put = in_live_put;
        x.vt.live_del = in_live_del;
        x.vt.live_list = in_live_list;
        x.vt.close = in_close;
        w.via = &x.vt;
        CHECK(client(&w, sc) == PT_RT_FATAL);
        CHECK(strstr(w.err, "modified by another writer") != NULL);
        w.via = NULL;
        clients = 0;
        CHECK(converge(&w, sc, &clients));
        check_against(sc, &w, &want, ref.http, "another writer", at);
        world_free(&w);
    }
    world_free(&ref);
    pt_buf_free(&want);
}

/* A server that holds every request and, like curl_multi_poll, sleeps
 * out the whole timeout while nothing arrives. */
typedef struct held_http {
    pt_http_driver vt;
    int n;
} held_http;

static void *held_start(pt_http_driver *d, const pt_http_req *req, const pt_http_sink *sink)
{
    (void)req;
    (void)sink;
    return &((held_http *)d)->n;
}

static void held_cancel(pt_http_driver *d, void *h)
{
    (void)d;
    (void)h;
}

static int held_pump(pt_http_driver *d, int timeout_ms, pt_waitfd *fds, unsigned nfds)
{
    (void)fds;
    (void)nfds;
    if (timeout_ms > 0)
        poll(NULL, 0, timeout_ms > 3000 ? 3000 : timeout_ms);
    ((held_http *)d)->n++;
    return 1; /* the one held request */
}

static void held_free(pt_http_driver *d)
{
    (void)d;
}

/* pt_rt_step reaches the same end as pt_rt_run, and wakes on extra fds. */
static void test_step(void)
{
    size_t i;
    for (i = 0; i < sizeof scenarios / sizeof *scenarios; i++) {
        const scenario *sc = &scenarios[i];
        world ref, w;
        pt_buf want, got;
        uint32_t clients = 0;
        pt_buf_init(&want, T_AL);
        pt_buf_init(&got, T_AL);
        world_init(&ref, sc->script, 1);
        CHECK(converge(&ref, sc, &clients));
        outcome(&ref, &want);
        world_init(&w, sc->script, 1);
        use_step = true;
        clients = 0;
        CHECK(converge(&w, sc, &clients));
        use_step = false;
        outcome(&w, &got);
        CHECK(pt_str_eq(pt_buf_str(&got), pt_buf_str(&want)));
        world_free(&ref);
        world_free(&w);
        pt_buf_free(&want);
        pt_buf_free(&got);
    }
    {
        world w;
        pt_opts o;
        pt_rt_cfg rc;
        pt_agent *a;
        pt_rt *rt;
        pt_session_cfg c;
        pt_identity me;
        pt_waitfd fd;
        int p[2];
        char ch;
        CHECK(pipe(p) == 0);
        world_init(&w, "{\"turns\":[]}", 1);
        memset(&o, 0, sizeof o);
        o.alloc = T_AL;
        o.now_ms = w_now;
        o.ud = &w;
        o.blob = pt_rt_read_blob;
        o.blob_ud = w.store;
        a = pt_new(&o);
        memset(&rc, 0, sizeof rc);
        rc.store = w.store;
        rc.http = &w.http->vt;
        rc.now = w_now;
        rc.sleep = w_sleep;
        rc.clock_ud = &w;
        rt = pt_rt_new(&rc, a);
        memset(&c, 0, sizeof c);
        memset(&me, 0, sizeof me);
        me.host = PT_S("sim");
        CHECK(pt_create(a, &c));
        CHECK(pt_open(a, &me, false));
        fd.fd = p[0];
        CHECK(pt_rt_step(rt, 0, &fd, 1) == PT_RT_IDLE);
        CHECK(!fd.ready);
        CHECK(write(p[1], "x", 1) == 1);
        CHECK(pt_rt_step(rt, 5000, &fd, 1) == PT_RT_IDLE); /* returns at once */
        CHECK(fd.ready);
        CHECK(read(p[0], &ch, 1) == 1);
        CHECK(pt_rt_step(rt, 10, &fd, 1) == PT_RT_IDLE);
        CHECK(!fd.ready);
        pt_rt_free(rt);
        pt_free(a);
        world_free(&w);
        close(p[0]);
        close(p[1]);
    }
    {
        /* A step that handed out work returns without waiting: a host (the
         * TUI) shows what that work committed at once, not after its
         * timeout. Here: the input is committed and the request started,
         * the server says nothing, and the step still returns at once. */
        world w;
        pt_opts o;
        pt_rt_cfg rc;
        pt_agent *a;
        pt_rt *rt;
        pt_session_cfg c;
        pt_identity me;
        pt_input in;
        pt_entry_view v;
        held_http held = {{held_start, held_cancel, held_pump, held_free}, 0};
        int64_t t0;
        uint32_t k;
        bool user = false;
        world_init(&w, "{\"turns\":[]}", 1);
        memset(&o, 0, sizeof o);
        o.alloc = T_AL;
        o.now_ms = w_now;
        o.ud = &w;
        o.blob = pt_rt_read_blob;
        o.blob_ud = w.store;
        o.config.model = PT_S("faux/faux");
        a = pt_new(&o);
        memset(&rc, 0, sizeof rc);
        rc.store = w.store;
        rc.http = &held.vt;
        rc.now = w_now;
        rc.sleep = w_sleep;
        rc.clock_ud = &w;
        rt = pt_rt_new(&rc, a);
        memset(&c, 0, sizeof c);
        memset(&me, 0, sizeof me);
        me.host = PT_S("sim");
        CHECK(pt_create(a, &c));
        CHECK(pt_open(a, &me, false));
        memset(&in, 0, sizeof in);
        in.text = PT_S("hello");
        CHECK(pt_submit(a, &in));
        t0 = pt_sys_now_ms();
        CHECK(pt_rt_step(rt, 2000, NULL, 0) == PT_RT_BUSY);
        CHECK(pt_sys_now_ms() - t0 < 500);
        for (k = 0; k < pt_entry_count(a); k++)
            if (pt_entry_at(a, k, &v) && strcmp(v.kind, "user") == 0)
                user = true;
        CHECK(user);
        CHECK(held.n >= 1); /* the request was started and pumped once */
        pt_rt_free(rt);
        pt_free(a);
        world_free(&w);
    }
}

static void test_scenarios(void)
{
    size_t i;
    for (i = 0; i < sizeof scenarios / sizeof *scenarios; i++)
        exhaustive(&scenarios[i]);
}

int main(void)
{
    RUN(test_scenarios);
    RUN(test_another_writer);
    RUN(test_step);
    return t_done("test_sim");
}
