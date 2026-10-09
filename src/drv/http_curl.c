/* HTTP driver on the libcurl multi interface. */
#define _POSIX_C_SOURCE 200809L
#include "drv/drv.h"

#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>

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
        char *h = (char *)malloc(ln + lv + 3);
        struct curl_slist *nl;
        if (!h)
            continue;
        memcpy(h, req->headers[i].name, ln);
        memcpy(h + ln, ": ", 2);
        memcpy(h + ln + 2, req->headers[i].value, lv + 1);
        nl = curl_slist_append(x->hdrs, h);
        free(h);
        if (nl)
            x->hdrs = nl;
    }
    curl_easy_setopt(x->e, CURLOPT_URL, req->url);
    curl_easy_setopt(x->e, CURLOPT_CUSTOMREQUEST, req->method);
    curl_easy_setopt(x->e, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)req->body.n);
    curl_easy_setopt(x->e, CURLOPT_COPYPOSTFIELDS, req->body.n ? req->body.p : "");
    curl_easy_setopt(x->e, CURLOPT_HTTPHEADER, x->hdrs);
    curl_easy_setopt(x->e, CURLOPT_WRITEFUNCTION, on_write);
    curl_easy_setopt(x->e, CURLOPT_WRITEDATA, x);
    curl_easy_setopt(x->e, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(x->e, CURLOPT_HEADERDATA, x);
    curl_easy_setopt(x->e, CURLOPT_ERRORBUFFER, x->err);
    curl_easy_setopt(x->e, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(x->e, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(x->e, CURLOPT_CONNECTTIMEOUT, 30L);
    /* No limit on how long a response may take. A connection that delivers
     * nothing at all for five minutes is considered dead (providers send
     * keep-alive pings far more often than that). */
    curl_easy_setopt(x->e, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(x->e, CURLOPT_LOW_SPEED_TIME, 300L);
    curl_easy_setopt(x->e, CURLOPT_USERAGENT, "pytte/0");
    if (curl_multi_add_handle(d->m, x->e) != CURLM_OK) {
        curl_easy_cleanup(x->e);
        curl_slist_free_all(x->hdrs);
        free(x);
        return NULL;
    }
    x->next = d->list;
    d->list = x;
    return x;
}

static void cancel(pt_http_driver *vd, void *h)
{
    unlink_xfer((curl_drv *)vd, (xfer *)h);
}

static int pump(pt_http_driver *vd, int timeout_ms)
{
    curl_drv *d = (curl_drv *)vd;
    int running = 0, left;
    CURLMsg *msg;
    if (curl_multi_perform(d->m, &running) != CURLM_OK)
        return -1;
    if (running && curl_multi_poll(d->m, NULL, 0, timeout_ms, NULL) != CURLM_OK)
        return -1;
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
    curl_global_cleanup();
}

pt_http_driver *pt_http_curl_new(void)
{
    curl_drv *d;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        return NULL;
    d = (curl_drv *)calloc(1, sizeof *d);
    if (!d)
        return NULL;
    d->m = curl_multi_init();
    if (!d->m) {
        free(d);
        return NULL;
    }
    d->vt.start = start;
    d->vt.cancel = cancel;
    d->vt.pump = pump;
    d->vt.free = drv_free;
    return &d->vt;
}
