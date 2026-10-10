#define _POSIX_C_SOURCE 200809L
#include "drv/drv.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void *libc_alloc(void *ud, void *p, size_t old, size_t n)
{
    (void)ud;
    (void)old;
    if (!n) {
        free(p);
        return NULL;
    }
    return realloc(p, n);
}

pt_allocator pt_sys_allocator(void)
{
    pt_allocator al;
    al.fn = libc_alloc;
    al.ud = NULL;
    return al;
}

int pt_sys_wait(pt_waitfd *fds, unsigned n, int timeout_ms)
{
    struct pollfd p[64];
    unsigned i;
    int r;
    if (n > 64)
        n = 64;
    for (i = 0; i < n; i++) {
        p[i].fd = fds[i].fd;
        p[i].events = POLLIN;
        p[i].revents = 0;
        fds[i].ready = false;
    }
    r = poll(p, n, timeout_ms);
    if (r < 0)
        return errno == EINTR ? 0 : -1;
    for (i = 0; i < n; i++)
        fds[i].ready = (p[i].revents & (POLLIN | POLLHUP | POLLERR)) != 0;
    return r;
}

int64_t pt_sys_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void pt_sys_sleep_ms(uint32_t ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
        ;
}

const char *pt_sys_env(void *ud, const char *name)
{
    (void)ud;
    return getenv(name);
}

char *pt_sys_read_file(const char *path, size_t *n)
{
    int fd = open(path, O_RDONLY);
    struct stat st;
    char *p = NULL;
    size_t len = 0, cap = 0;
    int error;
    if (fd < 0)
        return NULL;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0) {
        if ((uint64_t)st.st_size >= SIZE_MAX) {
            errno = EOVERFLOW;
            goto fail;
        }
        cap = (size_t)st.st_size;
    }
    p = (char *)malloc(cap + 1);
    if (!p) {
        errno = ENOMEM;
        goto fail;
    }
    for (;;) {
        ssize_t got;
        if (len == cap) {
            char c;
            char *np;
            size_t next;
            /* Probe EOF before growing an exactly-sized regular file. */
            do got = read(fd, &c, 1); while (got < 0 && errno == EINTR);
            if (got < 0)
                goto fail;
            if (!got)
                break;
            if (cap > (SIZE_MAX - 1) / 2) {
                errno = EOVERFLOW;
                goto fail;
            }
            next = cap < 4096 ? 4096 : cap * 2;
            np = (char *)realloc(p, next + 1);
            if (!np) {
                errno = ENOMEM;
                goto fail;
            }
            p = np;
            cap = next;
            p[len++] = c;
        }
        /* Bound each syscall; regular and streaming files share this path. */
        do got = read(fd, p + len, cap - len > 65536 ? 65536 : cap - len);
        while (got < 0 && errno == EINTR);
        if (got < 0)
            goto fail;
        len += (size_t)got;
        if (got == 0)
            break;
    }
    close(fd);
    p[len] = 0;
    *n = len;
    return p;
fail:
    error = errno;
    free(p);
    close(fd);
    errno = error;
    return NULL;
}

bool pt_sys_write_file(const char *path, const void *p, size_t n)
{
    FILE *f = fopen(path, "wb");
    bool ok;
    if (!f)
        return false;
    ok = fwrite(p, 1, n, f) == n;
    ok = fclose(f) == 0 && ok;
    return ok;
}

void pt_sys_random(void *ud, void *out, size_t n)
{
    FILE *f = fopen("/dev/urandom", "rb");
    (void)ud;
    if (!f || fread(out, 1, n, f) != n) {
        /* no randomness to be had: still unique enough for tokens */
        unsigned char *p = (unsigned char *)out;
        uint64_t x = (uint64_t)pt_sys_now_ms() ^ ((uint64_t)getpid() << 32);
        size_t i;
        for (i = 0; i < n; i++) {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            p[i] = (unsigned char)x;
        }
    }
    if (f)
        fclose(f);
}

void pt_sys_identity(char *host, size_t hcap, char *boot, size_t bcap, int64_t *pid)
{
    FILE *f;
    if (gethostname(host, hcap) != 0 || !host[0])
        snprintf(host, hcap, "unknown");
    host[hcap - 1] = 0;
    boot[0] = 0;
    f = fopen("/proc/sys/kernel/random/boot_id", "r");
    if (f) {
        if (fgets(boot, (int)bcap, f))
            boot[strcspn(boot, "\n")] = 0;
        fclose(f);
    }
    *pid = (int64_t)getpid();
}
