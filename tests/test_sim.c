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
 * the reference run sent too (a re-sent request has the same bytes). */
#include "t.h"

#include "core/core.h"
#include "rt/rt.h"

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

static int rh_pump(pt_http_driver *d, int timeout_ms)
{
    rec_http *r = (rec_http *)d;
    return r->inner->pump(r->inner, timeout_ms);
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

typedef struct world {
    pt_store *store;
    pt_store *via; /* when set, clients use this store (wrapping store) */
    char err[512]; /* the last client's fatal error */
    rec_http *http;
    int64_t now;
    uint64_t rng;
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
}

static void world_free(world *w)
{
    pt_store_memory_free(w->store);
    rh_free(&w->http->vt);
}

typedef struct scenario {
    const char *name, *script;
    const char *prompts[3];
} scenario;

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
    o.config.model = PT_S("faux");
    o.config.live_interval_ms = 1;
    a = pt_new(&o);
    memset(&rc, 0, sizeof rc);
    rc.store = w->via ? w->via : w->store;
    rc.http = &w->http->vt;
    rc.now = w_now;
    rc.sleep = w_sleep;
    rc.clock_ud = w;
    rt = pt_rt_new(&rc, a);
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
        c.cwd = PT_S("/sim");
        c.host = PT_S("sim");
        c.system = PT_S("You are being simulated.");
        CHECK(pt_create(a, &c));
    }
    CHECK(pt_open(a, &me, false));
    for (i = 0; i < 3 && sc->prompts[i]; i++) {
        pt_input in;
        char req[8] = {'p', (char)('0' + i), 0};
        memset(&in, 0, sizeof in);
        in.text = pt_strz(sc->prompts[i]);
        in.req = pt_strz(req);
        CHECK(pt_submit(a, &in));
    }
    res = pt_rt_run(rt);
    if (res == PT_RT_IDLE && !pt_rt_close(rt))
        res = PT_RT_FATAL; /* the close commit failed: also a crash */
    snprintf(w->err, sizeof w->err, "%s", pt_error(a));
out:
    pt_rt_free(rt);
    pt_free(a);
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
        pt_buf_putc(out, ':');
        if (v.msg->role == PT_ROLE_ASSISTANT) {
            pt_buf_putz(out, pt_stop_name(v.msg->stop));
            for (j = 0; j < v.msg->nblocks; j++) {
                pt_buf_putc(out, '|');
                pt_buf_puts(out, v.msg->blocks[j].kind == PT_BLOCK_TOOL_CALL ? v.msg->blocks[j].name
                                                                             : v.msg->blocks[j].text);
            }
        } else {
            pt_buf_puts(out, v.msg->text);
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

static int64_t turn_of(const char *body)
{
    pt_arena ar;
    int64_t t;
    pt_arena_init(&ar, T_AL, 0);
    t = pt_json_geti(pt_json_parse(pt_strz(body), &ar, NULL), "turn", -1);
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
    t_checks++;
    if (!pt_str_eq(pt_buf_str(&got), pt_buf_str(want))) {
        t_failed++;
        fprintf(stderr, "%s: crash at commit %u (%s): outcome differs:\n%.*s---\n%.*s\n", sc->name, n,
                how, (int)got.n, got.p, (int)want->n, want->p);
    }
    /* A request is built from committed state only, so every request for
     * the same turn (the same committed history) has the same bytes; the
     * first turn's must match the reference run's exactly. */
    for (i = 0; i < w->http->n; i++) {
        const char *bi = w->http->bodies[i];
        int64_t ti = turn_of(bi);
        uint32_t j;
        t_checks++;
        if (ti == 0 && !known_request(ref, bi)) {
            t_failed++;
            fprintf(stderr, "%s: crash at commit %u (%s): a first request unlike the reference's:\n%s\n",
                    sc->name, n, how, bi);
        }
        for (j = 0; j < i; j++) {
            t_checks++;
            if (turn_of(w->http->bodies[j]) == ti && strcmp(w->http->bodies[j], bi) != 0) {
                t_failed++;
                fprintf(stderr, "%s: crash at commit %u (%s): turn %lld was sent with different bytes:\n%s\n%s\n",
                        sc->name, n, how, (long long)ti, w->http->bodies[j], bi);
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
    k = pt_store_memory_appends(ref.store);
    outcome(&ref, &want);

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

static const scenario scenarios[] = {
    {"answer",
     "{\"chunk\":7,\"turns\":[{\"content\":[{\"type\":\"thinking\",\"text\":\"Easy.\"},"
     "{\"type\":\"text\",\"text\":\"The answer is 450, since 25 times 18 is 450.\"}]}]}",
     {"what is 25*18?", NULL, NULL}},
    {"retry",
     "{\"chunk\":7,\"turns\":[[{\"status\":529,\"body\":{\"class\":\"retryable\",\"message\":\"overloaded\"}},"
     "{\"content\":[{\"type\":\"text\",\"text\":\"This stream gets cut off somewhere in the middle\"}],"
     "\"cut\":150},"
     "{\"content\":[{\"type\":\"text\",\"text\":\"Third time lucky.\"}]}]]}",
     {"go", NULL, NULL}},
    {"tool_round",
     "{\"chunk\":9,\"turns\":[{\"content\":[{\"type\":\"text\",\"text\":\"Let me look.\"},"
     "{\"type\":\"tool_call\",\"name\":\"bash\",\"args\":{\"cmd\":\"ls\"}}]},"
     "{\"content\":[{\"type\":\"text\",\"text\":\"No tools here, so I will just answer.\"}]}]}",
     {"list the files", NULL, NULL}},
    {"fatal",
     "{\"turns\":[{\"status\":401,\"body\":{\"class\":\"fatal\",\"message\":\"invalid key\"}}]}",
     {"hello?", NULL, NULL}},
    {"length",
     "{\"chunk\":5,\"turns\":[{\"content\":[{\"type\":\"text\",\"text\":\"This is cut by max_tok\"}],"
     "\"stop\":\"length\"}]}",
     {"write a novel", NULL, NULL}},
    {"two_inputs",
     "{\"chunk\":6,\"turns\":[{\"content\":[{\"type\":\"text\",\"text\":\"First answer.\"}]},"
     "{\"content\":[{\"type\":\"text\",\"text\":\"Second answer.\"}]}]}",
     {"first question", "second question", NULL}},
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
FWD(read_log, (pt_store * s, pt_buf *out), (x->inner, out))
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
        x.vt.read_log = in_read_log;
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
    return t_done("test_sim");
}
