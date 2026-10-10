/* HTTP driver that answers from a faux script: no network. Delivers the
 * response in the script's chunk sizes, with its delays, and closes the
 * connection early where the script says "cut". */
#define _POSIX_C_SOURCE 200809L
#include "drv/drv.h"

#include <stdlib.h>
#include <string.h>

typedef struct fx {
    struct fx *next;
    pt_http_sink sink;
    char *body;
    size_t n, at;
    int status;
    uint32_t chunk, delay_ms;
    bool head_sent;
} fx;

typedef struct faux_drv {
    pt_http_driver vt;
    char *script;
    size_t script_n;
    fx *list;
    pt_faux_state st;
} faux_drv;

static void *start(pt_http_driver *vd, const pt_http_req *req, const pt_http_sink *sink)
{
    faux_drv *d = (faux_drv *)vd;
    pt_arena a;
    pt_faux_response r;
    fx *x = (fx *)calloc(1, sizeof *x);
    if (!x)
        return NULL;
    pt_arena_init(&a, pt_sys_allocator(), 0);
    if (!pt_faux_respond((pt_str){d->script, d->script_n}, req->body, &d->st, &r, &a)) {
        /* a broken script answers like a broken server */
        r.status = 500;
        r.body = PT_S("{\"class\":\"fatal\",\"message\":\"the faux script is not valid JSON\"}");
        r.chunk = 0;
        r.delay_ms = 0;
        r.cut = -1;
    }
    x->sink = *sink;
    x->status = r.status;
    x->n = r.cut >= 0 && (size_t)r.cut < r.body.n ? (size_t)r.cut : r.body.n;
    x->body = (char *)malloc(x->n + 1);
    if (x->body && x->n)
        memcpy(x->body, r.body.p, x->n);
    x->chunk = r.chunk;
    x->delay_ms = r.delay_ms;
    pt_arena_free(&a);
    if (!x->body) {
        free(x);
        return NULL;
    }
    x->next = d->list;
    d->list = x;
    return x;
}

static void unlink_fx(faux_drv *d, fx *x)
{
    fx **pp;
    for (pp = &d->list; *pp; pp = &(*pp)->next)
        if (*pp == x) {
            *pp = x->next;
            break;
        }
    free(x->body);
    free(x);
}

static void cancel(pt_http_driver *vd, void *h)
{
    unlink_fx((faux_drv *)vd, (fx *)h);
}

/* Each pump delivers one chunk of every active response. With nothing to
 * deliver it waits on fds (and only then on the timeout). */
static int pump(pt_http_driver *vd, int timeout_ms, pt_waitfd *fds, unsigned nfds)
{
    faux_drv *d = (faux_drv *)vd;
    fx *x = d->list, *next;
    int active = 0;
    if (nfds) {
        int t = timeout_ms;
        if (x && (t < 0 || (int)x->delay_ms < t))
            t = (int)x->delay_ms;
        pt_sys_wait(fds, nfds, t);
    } else if (x && x->delay_ms)
        pt_sys_sleep_ms(x->delay_ms);
    for (; x; x = next) {
        size_t step;
        next = x->next;
        if (!x->head_sent) {
            x->head_sent = true;
            if (x->sink.head)
                x->sink.head(x->sink.ud, x->status, PT_STR_NULL);
        }
        step = x->chunk && x->chunk < x->n - x->at ? x->chunk : x->n - x->at;
        if (step && x->sink.body)
            x->sink.body(x->sink.ud, x->body + x->at, step);
        x->at += step;
        if (x->at >= x->n) {
            if (x->sink.done)
                x->sink.done(x->sink.ud, NULL);
            unlink_fx(d, x);
        } else {
            active++;
        }
    }
    return active;
}

static void drv_free(pt_http_driver *vd)
{
    faux_drv *d = (faux_drv *)vd;
    while (d->list)
        unlink_fx(d, d->list);
    free(d->script);
    free(d);
}

pt_http_driver *pt_http_faux_new(pt_str script)
{
    faux_drv *d = (faux_drv *)calloc(1, sizeof *d);
    if (!d)
        return NULL;
    d->script = (char *)malloc(script.n + 1);
    if (!d->script) {
        free(d);
        return NULL;
    }
    memcpy(d->script, script.p, script.n);
    d->script[script.n] = 0;
    d->script_n = script.n;
    d->vt.start = start;
    d->vt.cancel = cancel;
    d->vt.pump = pump;
    d->vt.free = drv_free;
    return &d->vt;
}
