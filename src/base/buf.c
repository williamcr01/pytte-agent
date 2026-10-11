#include "pytte_base.h"

#include <string.h>

void pt_buf_init(pt_buf *b, pt_allocator al)
{
    b->p = NULL;
    b->n = b->cap = 0;
    b->al = al;
    b->oom = false;
}

bool pt_buf_reserve(pt_buf *b, size_t extra)
{
    size_t cap;
    char *np;
    if (b->oom)
        return false;
    if (b->cap - b->n >= extra)
        return true;
    if (b->n + extra < b->n) {
        b->oom = true;
        return false;
    }
    cap = b->cap ? b->cap : 64;
    while (cap < b->n + extra) {
        if (cap * 2 < cap) {
            b->oom = true;
            return false;
        }
        cap *= 2;
    }
    np = (char *)b->al.fn(b->al.ud, b->p, b->cap, cap);
    if (!np) {
        b->oom = true;
        return false;
    }
    b->p = np;
    b->cap = cap;
    return true;
}

void pt_buf_put(pt_buf *b, const void *p, size_t n)
{
    size_t off = 0;
    bool alias = false;
    if (!n)
        return;
    /* Integer address comparisons avoid undefined relational pointer comparisons. */
    if (b->p && (uintptr_t)p >= (uintptr_t)b->p &&
        (uintptr_t)p - (uintptr_t)b->p <= b->cap &&
        n <= b->cap - ((uintptr_t)p - (uintptr_t)b->p)) {
        alias = true;
        off = (size_t)((uintptr_t)p - (uintptr_t)b->p);
    }
    if (!pt_buf_reserve(b, n))
        return;
    if (alias)
        p = b->p + off;
    memmove(b->p + b->n, p, n);
    b->n += n;
}

void pt_buf_puts(pt_buf *b, pt_str s)
{
    pt_buf_put(b, s.p, s.n);
}

void pt_buf_putz(pt_buf *b, const char *z)
{
    pt_buf_puts(b, pt_strz(z));
}

void pt_buf_putc(pt_buf *b, char c)
{
    if (b->n < b->cap) {
        /* No reserve needed, and a single stack-local byte cannot alias
         * the buffer's allocation. */
        b->p[b->n++] = c;
        return;
    }
    pt_buf_put(b, &c, 1);
}

void pt_buf_u64(pt_buf *b, uint64_t v)
{
    char tmp[20];
    size_t i = sizeof tmp;
    do {
        tmp[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    pt_buf_put(b, tmp + i, sizeof tmp - i);
}

void pt_buf_i64(pt_buf *b, int64_t v)
{
    if (v < 0) {
        pt_buf_putc(b, '-');
        pt_buf_u64(b, (uint64_t)0 - (uint64_t)v);
    } else {
        pt_buf_u64(b, (uint64_t)v);
    }
}

pt_str pt_buf_str(const pt_buf *b)
{
    pt_str s;
    s.p = b->p ? b->p : "";
    s.n = b->n;
    return s;
}

void pt_buf_reset(pt_buf *b)
{
    b->n = 0;
}

void pt_buf_free(pt_buf *b)
{
    if (b->p)
        b->al.fn(b->al.ud, b->p, b->cap, 0);
    b->p = NULL;
    b->n = b->cap = 0;
}
