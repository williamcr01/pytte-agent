/* The real curl transport against an offline loopback server. */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "t.h"
#include "drv/drv.h"

typedef struct server {
    int fd;
    bool request_ok;
} server;

static void *serve(void *ud)
{
    server *s = (server *)ud;
    static const char response[] =
        "HTTP/1.1 100 Continue\r\n\r\n"
        "HTTP/1.1 503 Service Unavailable\r\nRetry-After: 7\r\n"
        "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
        "2\r\nhe\r\n3\r\nllo\r\n0\r\n\r\n";
    char buf[4096];
    size_t n = 0, at = 0;
    int fd = accept(s->fd, NULL, NULL);
    if (fd < 0)
        return NULL;
    while (n < sizeof buf - 1) {
        char *end;
        ssize_t got = recv(fd, buf + n, sizeof buf - n - 1, 0);
        if (got <= 0)
            break;
        n += (size_t)got;
        buf[n] = 0;
        end = strstr(buf, "\r\n\r\n");
        if (end && n >= (size_t)(end + 4 - buf) + 4) {
            s->request_ok = strncmp(buf, "PATCH /stream HTTP/1.1\r\n", 24) == 0 &&
                strstr(buf, "X-Pytte-Test: kept\r\n") != NULL &&
                strstr(buf, "Content-Length: 4\r\n") != NULL &&
                memcmp(end + 4, "a\0bc", 4) == 0;
            break;
        }
    }
    while (at < sizeof response - 1) {
        ssize_t sent = send(fd, response + at, sizeof response - 1 - at, 0);
        if (sent <= 0)
            break;
        at += (size_t)sent;
    }
    close(fd);
    return NULL;
}

typedef struct observer {
    pthread_t host;
    unsigned heads, done;
    int status;
    bool wrong_thread, error;
    pt_buf body;
    char retry[32];
    char *request_body;
} observer;

static void head(void *ud, int status, pt_str retry)
{
    observer *o = (observer *)ud;
    o->heads++;
    o->status = status;
    o->wrong_thread |= !pthread_equal(o->host, pthread_self());
    snprintf(o->retry, sizeof o->retry, "%.*s", (int)retry.n, retry.p);
}

static void body(void *ud, const char *p, size_t n)
{
    observer *o = (observer *)ud;
    o->wrong_thread |= !pthread_equal(o->host, pthread_self());
    pt_buf_put(&o->body, p, n);
}

static void done(void *ud, const char *err)
{
    observer *o = (observer *)ud;
    o->done++;
    o->error = err != NULL;
    o->wrong_thread |= !pthread_equal(o->host, pthread_self());
    free(o->request_body); /* Curl must stop borrowing it before done. */
    o->request_body = NULL;
}

static void test_many_waitfds(void)
{
    int idle[2], ready[2];
    pt_waitfd sys_fds[65], curl_fds[33];
    pt_http_driver *d;
    unsigned i;
    if (pipe(idle) != 0) {
        CHECK(false);
        return;
    }
    if (pipe(ready) != 0) {
        CHECK(false);
        close(idle[0]); close(idle[1]);
        return;
    }
    for (i = 0; i < 64; i++)
        sys_fds[i] = (pt_waitfd){idle[0], false};
    sys_fds[64] = (pt_waitfd){ready[0], false};
    for (i = 0; i < 32; i++)
        curl_fds[i] = (pt_waitfd){idle[0], false};
    curl_fds[32] = (pt_waitfd){ready[0], false};
    CHECK(write(ready[1], "r", 1) == 1);
    CHECK(pt_sys_wait(sys_fds, 65, 0) == 1);
    for (i = 0; i < 64; i++)
        CHECK(!sys_fds[i].ready);
    CHECK(sys_fds[64].ready);
    {
        pt_waitfd invalid;
        int bad = dup(idle[0]);
        CHECK(bad >= 0);
        if (bad >= 0) {
            close(bad);
            invalid = (pt_waitfd){bad, false};
            CHECK(pt_sys_wait(&invalid, 1, 0) == 1 && invalid.ready);
        }
    }
    d = pt_http_curl_new();
    CHECK(d != NULL);
    if (d) {
        CHECK(d->pump(d, 0, curl_fds, 33) == 0);
        for (i = 0; i < 32; i++)
            CHECK(!curl_fds[i].ready);
        CHECK(curl_fds[32].ready);
        d->free(d);
    }
    close(idle[0]); close(idle[1]);
    close(ready[0]); close(ready[1]);
}

static void test_http(void)
{
    server s;
    struct sockaddr_in addr;
    struct timeval limit = {3, 0};
    socklen_t len = sizeof addr;
    pthread_t thread;
    pt_http_driver *d, *other;
    pt_http_req req;
    pt_header hdrs[] = {{"X-Pytte-Test", "kept"}, {"Expect", ""}};
    pt_http_sink sink;
    pt_waitfd fd;
    observer o;
    char url[128];
    int wake[2], port;
    int64_t end;
    void *handle;
    memset(&s, 0, sizeof s);
    memset(&addr, 0, sizeof addr);
    s.fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(s.fd >= 0);
    if (s.fd < 0)
        return;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(s.fd, (struct sockaddr *)&addr, sizeof addr) == 0);
    CHECK(getsockname(s.fd, (struct sockaddr *)&addr, &len) == 0);
    CHECK(listen(s.fd, 1) == 0);
    CHECK(setsockopt(s.fd, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof limit) == 0);
    port = ntohs(addr.sin_port);
    CHECK(pthread_create(&thread, NULL, serve, &s) == 0);
    d = pt_http_curl_new();
    other = pt_http_curl_new();
    CHECK(d != NULL && other != NULL);
    if (!d || !other)
        exit(1);
    memset(&o, 0, sizeof o);
    o.host = pthread_self();
    pt_buf_init(&o.body, T_AL);
    o.request_body = (char *)malloc(4);
    CHECK(o.request_body != NULL);
    memcpy(o.request_body, "a\0bc", 4);
    sink = (pt_http_sink){&o, head, body, done, NULL};
    memset(&req, 0, sizeof req);
    snprintf(url, sizeof url, "http://127.0.0.1:%d/stream", port);
    req.url = url;
    req.method = "PATCH";
    req.body = (pt_str){o.request_body, 4};
    req.headers = hdrs;
    req.nheaders = 2;
    handle = d->start(d, &req, &sink);
    CHECK(handle != NULL);
    d->cancel(d, handle);
    CHECK(o.done == 0 && o.heads == 0);
    handle = other->start(other, &req, &sink);
    CHECK(handle != NULL);
    d->free(d); /* The second driver retains libcurl and global state. */
    end = pt_sys_now_ms() + 3000;
    while (!o.done && pt_sys_now_ms() < end)
        CHECK(other->pump(other, 50, NULL, 0) >= 0);
    CHECK(o.done == 1 && o.heads == 1 && o.status == 503 && !o.error);
    CHECK(strcmp(o.retry, "7") == 0 && !o.wrong_thread);
    CHECK_STR(pt_buf_str(&o.body), "hello");
    pthread_join(thread, NULL);
    CHECK(s.request_ok);
    close(s.fd);
    CHECK(pipe(wake) == 0 && write(wake[1], "w", 1) == 1);
    fd.fd = wake[0];
    CHECK(other->pump(other, 1000, &fd, 1) == 0 && fd.ready);
    close(wake[0]); close(wake[1]);
    other->free(other);
    pt_buf_free(&o.body);
    d = pt_http_curl_new(); /* Reopening after the last driver's global cleanup. */
    CHECK(d != NULL);
    if (d)
        d->free(d);
}

int main(void)
{
    RUN(test_many_waitfds);
    RUN(test_http);
    return t_done("test_http");
}
