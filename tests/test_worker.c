/* A held durable write must not block the host or publish a commit early. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <pthread.h>
#include <unistd.h>

#include "t.h"
#include "rt/rt.h"

typedef struct job {
    int gate[2];
    unsigned n;
} job;

static void held(void *ud)
{
    job *j = (job *)ud;
    char c;
    ssize_t n;
    do n = read(j->gate[0], &c, 1); while (n < 0 && errno == EINTR);
    if (n == 1)
        j->n++;
}

static void test_worker(void)
{
    pt_worker *w = pt_worker_new();
    job j;
    pt_waitfd fd;
    unsigned i;
    memset(&j, 0, sizeof j);
    CHECK(w != NULL);
    if (!w)
        return;
    CHECK(pipe(j.gate) == 0);
    fd.fd = pt_worker_fd(w);
    for (i = 0; i < 100; i++) {
        CHECK(pt_worker_start(w, held, &j));
        CHECK(!pt_worker_start(w, held, &j));
        CHECK(!pt_worker_finish(w));
        CHECK(write(j.gate[1], "g", 1) == 1);
        CHECK(pt_sys_wait(&fd, 1, 2000) == 1 && fd.ready);
        CHECK(pt_worker_finish(w));
        CHECK(j.n == i + 1);
        CHECK(!pt_worker_finish(w));
    }
    pt_worker_free(w);
    close(j.gate[0]);
    close(j.gate[1]);
}

typedef struct held_store {
    pt_store vt;
    pt_store *inner;
    int began[2], gate[2];
    bool first;
} held_store;

static int h_size(pt_store *vs, uint64_t *out)
{
    held_store *s = (held_store *)vs;
    return s->inner->size(s->inner, out);
}

static int h_append(pt_store *vs, const void *p, size_t n, bool sync)
{
    held_store *s = (held_store *)vs;
    int rc;
    if (s->first) {
        char c;
        ssize_t got;
        s->first = false;
        (void)!write(s->began[1], "b", 1);
        do got = read(s->gate[0], &c, 1); while (got < 0 && errno == EINTR);
        if (got != 1)
            return -1;
    }
    rc = s->inner->append(s->inner, p, n, sync);
    if (rc)
        snprintf(vs->err, sizeof vs->err, "%s", s->inner->err);
    return rc;
}

static int h_read(pt_store *vs, uint64_t off, void *p, size_t cap, size_t *got)
{
    held_store *s = (held_store *)vs;
    return s->inner->read(s->inner, off, p, cap, got);
}
static int h_truncate(pt_store *vs, uint64_t off)
{
    held_store *s = (held_store *)vs;
    return s->inner->truncate(s->inner, off);
}
static int h_live_put(pt_store *vs, uint32_t task, const void *p, size_t n)
{
    held_store *s = (held_store *)vs;
    return s->inner->live_put(s->inner, task, p, n);
}
static int h_live_del(pt_store *vs, uint32_t task)
{
    held_store *s = (held_store *)vs;
    return s->inner->live_del(s->inner, task);
}
static int h_live_list(pt_store *vs, void (*fn)(void *, uint32_t, pt_str), void *ud)
{
    held_store *s = (held_store *)vs;
    return s->inner->live_list(s->inner, fn, ud);
}
static void h_close(pt_store *vs) { (void)vs; }

typedef struct observer {
    pthread_t host;
    unsigned commits, done, bytes;
    bool wrong_thread;
} observer;

static void event(void *ud, const pt_event *ev)
{
    observer *o = (observer *)ud;
    if (!pthread_equal(o->host, pthread_self()))
        o->wrong_thread = true;
    if (ev->kind == PT_E_COMMITTED)
        o->commits++;
    if (ev->kind == PT_E_RUN_DONE)
        o->done++;
    if (ev->kind == PT_E_STREAM && ev->ai->kind == PT_EV_TEXT_DELTA)
        o->bytes += (unsigned)ev->ai->delta.n;
}

static void test_runtime(bool fail, bool close_early)
{
    held_store store;
    observer obs;
    pt_opts opts;
    pt_rt_cfg cfg;
    pt_session_cfg session;
    pt_identity me;
    pt_input input;
    pt_waitfd fd;
    pt_http_driver *http;
    pt_worker *worker;
    pt_agent *a;
    pt_rt *rt;
    int input_fd[2];
    char c;
    memset(&store, 0, sizeof store);
    memset(&obs, 0, sizeof obs);
    memset(&opts, 0, sizeof opts);
    memset(&cfg, 0, sizeof cfg);
    memset(&session, 0, sizeof session);
    memset(&me, 0, sizeof me);
    memset(&input, 0, sizeof input);
    obs.host = pthread_self();
    store.inner = pt_store_memory_new();
    CHECK(store.inner != NULL);
    CHECK(pipe(store.began) == 0 && pipe(store.gate) == 0 && pipe(input_fd) == 0);
    store.first = true;
    store.vt.size = h_size;
    store.vt.append = h_append;
    store.vt.read = h_read;
    store.vt.truncate = h_truncate;
    store.vt.live_put = h_live_put;
    store.vt.live_del = h_live_del;
    store.vt.live_list = h_live_list;
    store.vt.close = h_close;
    if (fail)
        pt_store_memory_fail(store.inner, 1, 0);
    http = pt_http_faux_new(PT_S("{\"turns\":[{\"content\":[{\"type\":\"text\",\"text\":\"hello\"}]}]}"));
    worker = pt_worker_new();
    CHECK(http != NULL && worker != NULL);
    opts.alloc = T_AL;
    opts.config.model = PT_S("faux/faux");
    opts.on_event = event;
    opts.ud = &obs;
    a = pt_new(&opts);
    cfg.store = &store.vt;
    cfg.http = http;
    rt = pt_rt_new(&cfg, a);
    CHECK(a != NULL && rt != NULL);
    pt_rt_use_worker(rt, worker);
    session.cwd = PT_S("/offline");
    session.host = PT_S("host");
    session.system = PT_S("be concise");
    me.host = session.host;
    me.boot = PT_S("boot");
    me.pid = 1;
    CHECK(pt_create(a, &session) && pt_open(a, &me, false));
    CHECK(pt_rt_step(rt, 0, NULL, 0) == PT_RT_BUSY);
    fd.fd = store.began[0];
    CHECK(pt_sys_wait(&fd, 1, 2000) == 1 && read(fd.fd, &c, 1) == 1);
    CHECK(obs.commits == 0 && pt_store_memory_log(store.inner).n == 0);
    input.text = PT_S("hi");
    CHECK(pt_submit(a, &input));
    CHECK(write(input_fd[1], "k", 1) == 1);
    fd.fd = input_fd[0];
    CHECK(pt_rt_step(rt, 1000, &fd, 1) == PT_RT_BUSY);
    CHECK(fd.ready && read(fd.fd, &c, 1) == 1);
    CHECK(obs.commits == 0); /* input woke while append is still held */
    CHECK(write(store.gate[1], "g", 1) == 1);
    if (fail) {
        CHECK(pt_rt_run(rt) == PT_RT_FATAL);
        CHECK(pt_fatal(a) == PT_FATAL_STORAGE && obs.commits == 0);
    } else if (close_early) {
        pt_str log;
        unsigned i;
        for (i = 0; i < 1111; i++) {
            input.text = PT_S("queued before quitting");
            CHECK(pt_submit(a, &input));
        }
        CHECK(pt_rt_close(rt));
        log = pt_store_memory_log(store.inner);
        CHECK(pt_str_contains(log, PT_S("\"text\":\"hi\"")));
        CHECK(pt_str_contains(log, PT_S("\"event\":\"close\"")));
        CHECK(obs.commits == 1115 && obs.done == 0 && obs.bytes == 0);
        CHECK(pt_closed(a));
    } else {
        CHECK(pt_rt_run(rt) == PT_RT_IDLE);
        CHECK(obs.commits >= 4 && obs.done == 1 && obs.bytes == 5);
        CHECK(pt_rt_close(rt));
    }
    CHECK(!obs.wrong_thread);
    pt_rt_free(rt);
    pt_free(a);
    http->free(http);
    pt_worker_free(worker);
    pt_store_memory_free(store.inner);
    close(store.gate[0]); close(store.gate[1]);
    close(store.began[0]); close(store.began[1]);
    close(input_fd[0]); close(input_fd[1]);
}

int main(void)
{
    RUN(test_worker);
    test_runtime(false, false);
    test_runtime(true, false);
    test_runtime(false, true);
    return t_done("test_worker");
}
