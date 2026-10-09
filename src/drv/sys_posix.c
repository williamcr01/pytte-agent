#define _POSIX_C_SOURCE 200809L
#include "drv/drv.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

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
    FILE *f = fopen(path, "rb");
    char *p = NULL;
    size_t len = 0, cap = 0;
    if (!f)
        return NULL;
    for (;;) {
        size_t got;
        if (cap - len < 4096) {
            char *np;
            cap = cap ? cap * 2 : 8192;
            np = (char *)realloc(p, cap + 1);
            if (!np) {
                free(p);
                fclose(f);
                errno = ENOMEM;
                return NULL;
            }
            p = np;
        }
        got = fread(p + len, 1, cap - len, f);
        len += got;
        if (got == 0)
            break;
    }
    if (ferror(f)) {
        free(p);
        fclose(f);
        return NULL;
    }
    fclose(f);
    p[len] = 0;
    *n = len;
    return p;
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
