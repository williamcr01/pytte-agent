/* HTTP driver on the libcurl multi interface. */
#define _POSIX_C_SOURCE 200809L
#include "drv/drv.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#ifndef PT_CURL_LINKED
#include <dlfcn.h>

/* Loading curl only when HTTP is needed keeps its TLS/HTTP dependency tree
 * out of startup and offline sessions. Each driver has one owner thread. */
static struct {
    CURLcode (*global_init)(long);
    void (*global_cleanup)(void);
    CURL *(*easy_init)(void);
    void (*easy_cleanup)(CURL *);
    CURLcode (*easy_setopt)(CURL *, CURLoption, ...);
    CURLcode (*easy_getinfo)(CURL *, CURLINFO, ...);
    const char *(*easy_strerror)(CURLcode);
    struct curl_slist *(*slist_append)(struct curl_slist *, const char *);
    void (*slist_free_all)(struct curl_slist *);
    CURLM *(*multi_init)(void);
    CURLMcode (*multi_cleanup)(CURLM *);
    CURLMcode (*multi_add_handle)(CURLM *, CURL *);
    CURLMcode (*multi_remove_handle)(CURLM *, CURL *);
    CURLMcode (*multi_perform)(CURLM *, int *);
    CURLMcode (*multi_poll)(CURLM *, struct curl_waitfd *, unsigned, int, int *);
    CURLMsg *(*multi_info_read)(CURLM *, int *);
} curl_api;
static void *curl_library;

static bool load_symbol(void *dst, size_t n, const char *name)
{
    void *sym = dlsym(curl_library, name);
    /* POSIX dlsym supplies callable function addresses. memcpy avoids an
     * ISO C object-to-function-pointer cast and keeps pedantic builds clean. */
    if (!sym || n != sizeof sym)
        return false;
    memcpy(dst, &sym, n);
    return true;
}

static bool curl_load(void)
{
    if (curl_library)
        return true;
#ifdef __APPLE__
    curl_library = dlopen("libcurl.4.dylib", RTLD_NOW | RTLD_LOCAL);
#else
    curl_library = dlopen("libcurl.so.4", RTLD_NOW | RTLD_LOCAL);
#endif
    if (!curl_library)
        return false;
#define LOAD(name) if (!load_symbol(&curl_api.name, sizeof curl_api.name, "curl_" #name)) goto fail
    LOAD(global_init);
    LOAD(global_cleanup);
    LOAD(easy_init);
    LOAD(easy_cleanup);
    LOAD(easy_setopt);
    LOAD(easy_getinfo);
    LOAD(easy_strerror);
    LOAD(slist_append);
    LOAD(slist_free_all);
    LOAD(multi_init);
    LOAD(multi_cleanup);
    LOAD(multi_add_handle);
    LOAD(multi_remove_handle);
    LOAD(multi_perform);
    LOAD(multi_poll);
    LOAD(multi_info_read);
#undef LOAD
    return true;
fail:
    dlclose(curl_library);
    curl_library = NULL;
    memset(&curl_api, 0, sizeof curl_api);
    return false;
}

#undef curl_easy_setopt
#undef curl_easy_getinfo
#define curl_global_init curl_api.global_init
#define curl_global_cleanup curl_api.global_cleanup
#define curl_easy_init curl_api.easy_init
#define curl_easy_cleanup curl_api.easy_cleanup
#define curl_easy_setopt curl_api.easy_setopt
#define curl_easy_getinfo curl_api.easy_getinfo
#define curl_easy_strerror curl_api.easy_strerror
#define curl_slist_append curl_api.slist_append
#define curl_slist_free_all curl_api.slist_free_all
#define curl_multi_init curl_api.multi_init
#define curl_multi_cleanup curl_api.multi_cleanup
#define curl_multi_add_handle curl_api.multi_add_handle
#define curl_multi_remove_handle curl_api.multi_remove_handle
#define curl_multi_perform curl_api.multi_perform
#define curl_multi_poll curl_api.multi_poll
#define curl_multi_info_read curl_api.multi_info_read
#else
static bool curl_load(void) { return true; }
#endif

static pthread_mutex_t curl_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned curl_users;

static bool curl_acquire(void)
{
    bool ok = true;
    pthread_mutex_lock(&curl_lock);
    if (!curl_users) {
        ok = curl_load();
        if (ok && curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
            ok = false;
    }
    if (ok)
        curl_users++;
    pthread_mutex_unlock(&curl_lock);
    return ok;
}

static void curl_release(void)
{
    pthread_mutex_lock(&curl_lock);
    if (--curl_users == 0) {
        curl_global_cleanup();
        /* Keep a successfully loaded module for the process lifetime.
         * global_cleanup does not join resolver threads; dlclose here
         * could unmap code still executing after cancellation. */
    }
    pthread_mutex_unlock(&curl_lock);
}

typedef struct xfer {
    struct xfer *next;
    CURL *e;
    struct curl_slist *hdrs;
    pt_http_sink sink;
    bool head_sent;
    char retry_after[32];
    char err[CURL_ERROR_SIZE];
} xfer;

typedef struct curl_drv {
    pt_http_driver vt;
    CURLM *m;
    xfer *list;
} curl_drv;

static void send_head(xfer *x)
{
    long status = 0;
    if (x->head_sent)
        return;
    x->head_sent = true;
    curl_easy_getinfo(x->e, CURLINFO_RESPONSE_CODE, &status);
    if (x->sink.head)
        x->sink.head(x->sink.ud, (int)status, pt_strz(x->retry_after));
}

static size_t on_write(char *p, size_t size, size_t n, void *ud)
{
    xfer *x = (xfer *)ud;
    send_head(x);
    if (x->sink.body)
        x->sink.body(x->sink.ud, p, size * n);
    return size * n;
}

static size_t on_header(char *p, size_t size, size_t n, void *ud)
{
    xfer *x = (xfer *)ud;
    pt_str line = {p, size * n};
    static const pt_str name = {"retry-after:", 12};
    if (pt_str_starts(line, PT_S("HTTP/"))) {
        x->retry_after[0] = 0; /* a new response (after a 100 Continue) */
    } else if (line.n > name.n && pt_str_ieq((pt_str){line.p, name.n}, name)) {
        pt_str v = pt_str_trim((pt_str){line.p + name.n, line.n - name.n});
        if (v.n < sizeof x->retry_after) {
            memcpy(x->retry_after, v.p, v.n);
            x->retry_after[v.n] = 0;
        }
    }
    return size * n;
}

static void unlink_xfer(curl_drv *d, xfer *x)
{
    xfer **pp;
    for (pp = &d->list; *pp; pp = &(*pp)->next)
        if (*pp == x) {
            *pp = x->next;
            break;
        }
    curl_multi_remove_handle(d->m, x->e);
    curl_easy_cleanup(x->e);
    curl_slist_free_all(x->hdrs);
    free(x);
}

static void *start(pt_http_driver *vd, const pt_http_req *req, const pt_http_sink *sink)
{
    curl_drv *d = (curl_drv *)vd;
    xfer *x = (xfer *)calloc(1, sizeof *x);
    uint32_t i;
    size_t hcap = 0;
    char *h = NULL;
    if (!x)
        return NULL;
    x->e = curl_easy_init();
    if (!x->e) {
        free(x);
        return NULL;
    }
    x->sink = *sink;
    for (i = 0; i < req->nheaders; i++) {
        size_t ln = strlen(req->headers[i].name), lv = strlen(req->headers[i].value);
        if (ln > SIZE_MAX - 3 || lv > SIZE_MAX - ln - 3)
            goto fail;
        if (ln + lv + 3 > hcap)
            hcap = ln + lv + 3;
    }
    if (hcap && !(h = (char *)malloc(hcap)))
        goto fail;
    for (i = 0; i < req->nheaders; i++) {
        size_t ln = strlen(req->headers[i].name), lv = strlen(req->headers[i].value);
        struct curl_slist *nl;
        memcpy(h, req->headers[i].name, ln);
        memcpy(h + ln, ": ", 2);
        memcpy(h + ln + 2, req->headers[i].value, lv + 1);
        nl = curl_slist_append(x->hdrs, h);
        if (!nl)
            goto fail;
        x->hdrs = nl;
    }
    free(h);
    h = NULL;
    if ((curl_off_t)req->body.n < 0 || (size_t)(curl_off_t)req->body.n != req->body.n)
        goto fail;
#define SET(option, value) if (curl_easy_setopt(x->e, option, value) != CURLE_OK) goto fail
    SET(CURLOPT_URL, req->url);
    SET(CURLOPT_CUSTOMREQUEST, req->method);
    SET(CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)req->body.n);
    /* The HTTP contract keeps request bytes alive until done/cancel. Curl
     * borrows them too, avoiding a second full request-body allocation. */
    SET(CURLOPT_POSTFIELDS, req->body.n ? req->body.p : "");
    SET(CURLOPT_HTTPHEADER, x->hdrs);
    SET(CURLOPT_WRITEFUNCTION, on_write);
    SET(CURLOPT_WRITEDATA, x);
    SET(CURLOPT_HEADERFUNCTION, on_header);
    SET(CURLOPT_HEADERDATA, x);
    SET(CURLOPT_ERRORBUFFER, x->err);
    SET(CURLOPT_NOSIGNAL, 1L);
    SET(CURLOPT_TCP_KEEPALIVE, 1L);
    SET(CURLOPT_CONNECTTIMEOUT, 30L);
    /* No limit on how long a response may take. A connection that delivers
     * nothing at all for five minutes is considered dead (providers send
     * keep-alive pings far more often than that). */
    SET(CURLOPT_LOW_SPEED_LIMIT, 1L);
    SET(CURLOPT_LOW_SPEED_TIME, 300L);
    SET(CURLOPT_USERAGENT, "pytte/0");
#undef SET
    if (curl_multi_add_handle(d->m, x->e) != CURLM_OK)
        goto fail;
    x->next = d->list;
    d->list = x;
    return x;
fail:
    free(h);
    curl_easy_cleanup(x->e);
    curl_slist_free_all(x->hdrs);
    free(x);
    return NULL;
}

static void cancel(pt_http_driver *vd, void *h)
{
    unlink_xfer((curl_drv *)vd, (xfer *)h);
}

static int pump(pt_http_driver *vd, int timeout_ms, pt_waitfd *fds, unsigned nfds)
{
    curl_drv *d = (curl_drv *)vd;
    struct curl_waitfd w[32];
    int running = 0, left;
    unsigned i;
    CURLMsg *msg;
    if (nfds > 32)
        nfds = 32;
    if (timeout_ms < 0) /* no limit: curl_multi_poll takes no -1 */
        timeout_ms = 3600 * 1000;
    for (i = 0; i < nfds; i++) {
        w[i].fd = fds[i].fd;
        w[i].events = CURL_WAIT_POLLIN;
        w[i].revents = 0;
        fds[i].ready = false;
    }
    if (curl_multi_perform(d->m, &running) != CURLM_OK)
        return -1;
    /* With no transfers, curl_multi_poll (7.66+) still waits on the extra
     * fds and the timeout. */
    if ((running || nfds) && curl_multi_poll(d->m, nfds ? w : NULL, nfds, timeout_ms, NULL) != CURLM_OK)
        return -1;
    for (i = 0; i < nfds; i++)
        fds[i].ready = (w[i].revents & CURL_WAIT_POLLIN) != 0;
    if (curl_multi_perform(d->m, &running) != CURLM_OK)
        return -1;
    while ((msg = curl_multi_info_read(d->m, &left))) {
        xfer *x = NULL;
        if (msg->msg != CURLMSG_DONE)
            continue;
        for (x = d->list; x && x->e != msg->easy_handle; x = x->next)
            ;
        if (!x)
            continue;
        if (msg->data.result == CURLE_OK) {
            send_head(x);
            if (x->sink.done)
                x->sink.done(x->sink.ud, NULL);
        } else if (x->sink.done) {
            x->sink.done(x->sink.ud, x->err[0] ? x->err : curl_easy_strerror(msg->data.result));
        }
        unlink_xfer(d, x);
    }
    return running;
}

static void drv_free(pt_http_driver *vd)
{
    curl_drv *d = (curl_drv *)vd;
    while (d->list)
        unlink_xfer(d, d->list);
    curl_multi_cleanup(d->m);
    free(d);
    curl_release();
}

pt_http_driver *pt_http_curl_new(void)
{
    curl_drv *d;
    if (!curl_acquire())
        return NULL;
    d = (curl_drv *)calloc(1, sizeof *d);
    if (!d) {
        curl_release();
        return NULL;
    }
    d->m = curl_multi_init();
    if (!d->m) {
        free(d);
        curl_release();
        return NULL;
    }
    d->vt.start = start;
    d->vt.cancel = cancel;
    d->vt.pump = pump;
    d->vt.free = drv_free;
    return &d->vt;
}
