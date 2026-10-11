/* HTTP driver on the libcurl multi interface. */
#define _POSIX_C_SOURCE 200809L
#include "drv/drv.h"

#include <curl/curl.h>
#include <dirent.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
    bool directory_ca;
    char retry_after[32];
    char err[CURL_ERROR_SIZE];
} xfer;

typedef struct curl_drv {
    pt_http_driver vt;
    CURLM *m;
    CURL *first; /* Trust probe becomes the first transfer's easy handle. */
    xfer *list;
    /* Owned, contiguous trust paths; stable until the driver is freed.
     * Environment overrides are captured once, when the driver is created. */
    pt_buf trust;
    const char *ca_file, *ca_path, *ca_seed, *original_path;
    bool custom_file, custom_path;
} curl_drv;

static bool hash_cert_name(const char *name)
{
    unsigned i;
    if (strlen(name) != 10 || name[8] != '.' || name[9] != '0')
        return false;
    for (i = 0; i < 8; i++)
        if (!((name[i] >= '0' && name[i] <= '9') ||
              (name[i] >= 'a' && name[i] <= 'f')))
            return false;
    return true;
}

static bool proxy_configured(void)
{
    static const char *const vars[] = {"http_proxy", "https_proxy", "HTTPS_PROXY",
                                      "all_proxy", "ALL_PROXY"};
    unsigned i;
    for (i = 0; i < sizeof vars / sizeof vars[0]; i++) {
        const char *proxy = getenv(vars[i]);
        if (proxy && *proxy)
            return true;
    }
    return false;
}

static bool trust_init(curl_drv *d, CURL *e)
{
    const char *file = getenv("SSL_CERT_FILE"), *path = getenv("SSL_CERT_DIR");
    char *default_file = NULL, *default_path = NULL;
    struct curl_tlssessioninfo *tls = NULL;
    size_t fn, pn, size, seed_at;
    bool directory = false;
    d->custom_file = file && *file;
    d->custom_path = path && *path;
    /* version_info queries every optional dependency's version and faults
     * otherwise unused pages. These three queries use only the easy handle. */
    if (curl_easy_getinfo(e, CURLINFO_CAINFO, &default_file) != CURLE_OK ||
        curl_easy_getinfo(e, CURLINFO_CAPATH, &default_path) != CURLE_OK ||
        curl_easy_getinfo(e, CURLINFO_TLS_SSL_PTR, &tls) != CURLE_OK)
        return false;
    if (!d->custom_file)
        file = default_file;
    if (!d->custom_path)
        path = default_path;
#ifndef __APPLE__
    /* Some Unix curl packages configure only the bundle, while the native
     * OpenSSL store also has /etc/ssl/certs. Use that standard system store
     * only for these system bundle locations, never for an explicit file. */
    if (!d->custom_file && !d->custom_path && !path && file &&
        (!strcmp(file, "/etc/ssl/certs/ca-certificates.crt") ||
         !strcmp(file, "/etc/ssl/cert.pem"))) {
        struct stat st;
        if (stat(file, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 && access(file, R_OK) == 0)
            path = "/etc/ssl/certs";
    }
    /* OpenSSL looks up hashed directory entries on demand. Preserve native
     * trust handling on other backends/platforms and explicit CA files. */
    directory = !d->custom_file && path && *path && tls &&
                tls->backend == CURLSSLBACKEND_OPENSSL && !proxy_configured();
    /* Proxy CONNECT requests can precede the origin TLS handshake. Keep
     * the original bundle there so fallback never replays a sent request. */
#endif
    fn = file ? strlen(file) : 0;
    pn = path ? strlen(path) : 0;
    if (fn > SIZE_MAX - 2 || pn > (SIZE_MAX - fn - 2) / 2 ||
        fn + pn * 2 + 2 > SIZE_MAX - 12)
        return false;
    size = fn + pn * 2 + 14;
    /* Reserve before taking any pointers into the buffer. No trust-path
     * allocation or directory scan occurs in the transfer/stream loop. */
    if (!pt_buf_reserve(&d->trust, size))
        return false;
    pt_buf_put(&d->trust, file ? file : "", fn + 1);
    d->ca_file = file ? d->trust.p : NULL;
    pt_buf_put(&d->trust, path ? path : "", pn + 1);
    d->ca_path = path ? d->trust.p + fn + 1 : NULL;
    /* The fallback restores curl's original directory, which can be NULL.
     * Default paths from getinfo are static; explicit paths are owned here. */
    d->original_path = d->custom_path ? d->ca_path : default_path;
    seed_at = d->trust.n;
    if (directory) {
        DIR *dir = opendir(path);
        struct dirent *entry;
        if (!dir)
            return true; /* Keep libcurl's original CA configuration. */
        while ((entry = readdir(dir))) {
            struct stat st;
            char *seed = d->trust.p + seed_at;
            if (!hash_cert_name(entry->d_name))
                continue;
            memcpy(seed, path, pn);
            seed[pn] = '/';
            memcpy(seed + pn + 1, entry->d_name, 11);
            if (stat(seed, &st) == 0 && S_ISREG(st.st_mode) && access(seed, R_OK) == 0) {
                /* Use one certificate from the same trusted directory as
                 * CAINFO: curl 8.22 restores its full bundle for NULL CAINFO.
                 * All other roots remain available through CAPATH. */
                d->ca_seed = seed;
                d->trust.n += pn + 12;
                break;
            }
        }
        closedir(dir);
    }
    return true;
}

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
    x->e = d->first ? d->first : curl_easy_init();
    d->first = NULL;
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
    {
        curl_off_t size = (curl_off_t)req->body.n;
        if (size < 0 || (size_t)size != req->body.n)
            goto fail;
    }
#define SET(option, value) if (curl_easy_setopt(x->e, option, value) != CURLE_OK) goto fail
    SET(CURLOPT_URL, req->url);
    if (d->ca_seed && proxy_configured())
        d->ca_seed = NULL; /* libcurl can read changed proxy variables later. */
    if (d->ca_seed || d->custom_file)
        SET(CURLOPT_CAINFO, d->ca_seed ? d->ca_seed : d->ca_file);
    if (d->ca_seed || d->custom_path)
        SET(CURLOPT_CAPATH, d->ca_path);
    x->directory_ca = d->ca_seed != NULL;
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

static bool retry_ca_file(curl_drv *d, xfer *x, CURLcode result)
{
    long sent = 0;
    curl_off_t uploaded = 0;
    if (!x->directory_ca || x->head_sent ||
        (result != CURLE_PEER_FAILED_VERIFICATION && result != CURLE_SSL_CACERT_BADFILE))
        return false;
    if (curl_easy_getinfo(x->e, CURLINFO_REQUEST_SIZE, &sent) != CURLE_OK || sent ||
        curl_easy_getinfo(x->e, CURLINFO_SIZE_UPLOAD_T, &uploaded) != CURLE_OK || uploaded)
        return false;
    x->directory_ca = false;
    /* A distro's CA directory can be incomplete or stale. Retry the original
     * bundle once, with verification still enabled. These failures occur
     * before the HTTPS request is sent; request bytes remain borrowed. */
    if (curl_multi_remove_handle(d->m, x->e) != CURLM_OK ||
        curl_easy_setopt(x->e, CURLOPT_CAINFO, d->ca_file) != CURLE_OK ||
        curl_easy_setopt(x->e, CURLOPT_CAPATH, d->original_path) != CURLE_OK ||
        curl_multi_add_handle(d->m, x->e) != CURLM_OK)
        return false;
    d->ca_seed = NULL; /* Reuse verified bundle connections on later turns. */
    x->err[0] = 0;
    return true;
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
        CURLcode result;
        if (msg->msg != CURLMSG_DONE)
            continue;
        result = msg->data.result; /* Removing a handle invalidates msg. */
        for (x = d->list; x && x->e != msg->easy_handle; x = x->next)
            ;
        if (!x)
            continue;
        if (retry_ca_file(d, x, result)) {
            running++; /* The retried transfer starts on the next pump. */
            continue;
        }
        if (result == CURLE_OK) {
            send_head(x);
            if (x->sink.done)
                x->sink.done(x->sink.ud, NULL);
        } else if (x->sink.done) {
            x->sink.done(x->sink.ud, x->err[0] ? x->err : curl_easy_strerror(result));
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
    if (d->first)
        curl_easy_cleanup(d->first);
    pt_buf_free(&d->trust);
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
    pt_buf_init(&d->trust, pt_sys_allocator());
    d->first = curl_easy_init();
    if (!d->first || !trust_init(d, d->first)) {
        if (d->first)
            curl_easy_cleanup(d->first);
        pt_buf_free(&d->trust);
        free(d);
        curl_release();
        return NULL;
    }
    d->m = curl_multi_init();
    if (!d->m) {
        curl_easy_cleanup(d->first);
        pt_buf_free(&d->trust);
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
