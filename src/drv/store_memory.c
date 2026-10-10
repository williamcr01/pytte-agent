/* In-memory session store for tests and simulations. It outlives the
 * agents that use it, like a disk outlives a process, and it can fail an
 * append on purpose to simulate a crash at that point. */
#include "drv/drv.h"

#include <stdlib.h>
#include <string.h>

typedef struct live_file {
    uint32_t task;
    pt_buf b;
} live_file;

typedef struct mem_store {
    pt_store vt;
    pt_buf log;
    live_file *live;
    uint32_t nlive, cap;
    uint32_t appends, fail_in;
    long keep;
} mem_store;

static int fail(pt_store *s, const char *msg)
{
    size_t n = strlen(msg);
    if (n >= sizeof s->err)
        n = sizeof s->err - 1;
    memcpy(s->err, msg, n);
    s->err[n] = 0;
    return -1;
}

static int m_size(pt_store *s, uint64_t *out)
{
    *out = ((mem_store *)s)->log.n;
    return 0;
}

static int m_append(pt_store *vs, const void *p, size_t n, bool sync)
{
    mem_store *s = (mem_store *)vs;
    (void)sync;
    if (s->fail_in && --s->fail_in == 0) {
        size_t keep = s->keep < 0 || (size_t)s->keep > n ? n : (size_t)s->keep;
        pt_buf_put(&s->log, p, keep);
        return fail(vs, "simulated crash");
    }
    pt_buf_put(&s->log, p, n);
    if (s->log.oom)
        return fail(vs, "out of memory");
    s->appends++;
    return 0;
}

static int m_read_log(pt_store *vs, pt_buf *out)
{
    mem_store *s = (mem_store *)vs;
    pt_buf_puts(out, pt_buf_str(&s->log));
    return out->oom ? fail(vs, "out of memory") : 0;
}

static int m_truncate(pt_store *vs, uint64_t off)
{
    mem_store *s = (mem_store *)vs;
    if (off < s->log.n)
        s->log.n = (size_t)off;
    return 0;
}

static live_file *find_live(mem_store *s, uint32_t task)
{
    uint32_t i;
    for (i = 0; i < s->nlive; i++)
        if (s->live[i].task == task)
            return &s->live[i];
    return NULL;
}

static int m_live_put(pt_store *vs, uint32_t task, const void *p, size_t n)
{
    mem_store *s = (mem_store *)vs;
    live_file *f = find_live(s, task);
    if (!f) {
        if (s->nlive == s->cap) {
            uint32_t ncap = s->cap ? s->cap * 2 : 8;
            live_file *nl = (live_file *)realloc(s->live, ncap * sizeof *nl);
            if (!nl)
                return fail(vs, "out of memory");
            s->live = nl;
            s->cap = ncap;
        }
        f = &s->live[s->nlive++];
        f->task = task;
        pt_buf_init(&f->b, pt_sys_allocator());
    }
    pt_buf_reset(&f->b);
    pt_buf_put(&f->b, p, n);
    return 0;
}

static int m_live_del(pt_store *vs, uint32_t task)
{
    mem_store *s = (mem_store *)vs;
    live_file *f = find_live(s, task);
    if (f) {
        pt_buf_free(&f->b);
        *f = s->live[--s->nlive];
    }
    return 0;
}

static int m_live_list(pt_store *vs, void (*fn)(void *ud, uint32_t task, pt_str bytes), void *ud)
{
    mem_store *s = (mem_store *)vs;
    uint32_t i;
    for (i = 0; i < s->nlive; i++)
        fn(ud, s->live[i].task, pt_buf_str(&s->live[i].b));
    return 0;
}

static void m_close(pt_store *vs)
{
    (void)vs; /* the "disk" stays */
}

pt_store *pt_store_memory_new(void)
{
    mem_store *s = (mem_store *)calloc(1, sizeof *s);
    if (!s)
        return NULL;
    pt_buf_init(&s->log, pt_sys_allocator());
    s->vt.size = m_size;
    s->vt.append = m_append;
    s->vt.read_log = m_read_log;
    s->vt.truncate = m_truncate;
    s->vt.live_put = m_live_put;
    s->vt.live_del = m_live_del;
    s->vt.live_list = m_live_list;
    s->vt.close = m_close;
    return &s->vt;
}

void pt_store_memory_free(pt_store *vs)
{
    mem_store *s = (mem_store *)vs;
    uint32_t i;
    if (!s)
        return;
    for (i = 0; i < s->nlive; i++)
        pt_buf_free(&s->live[i].b);
    free(s->live);
    pt_buf_free(&s->log);
    free(s);
}

void pt_store_memory_fail(pt_store *vs, uint32_t nth, long keep)
{
    mem_store *s = (mem_store *)vs;
    s->fail_in = nth;
    s->keep = keep;
}

uint32_t pt_store_memory_appends(const pt_store *vs)
{
    return ((const mem_store *)vs)->appends;
}

pt_str pt_store_memory_log(const pt_store *vs)
{
    return pt_buf_str(&((const mem_store *)vs)->log);
}
