#include "pytte_base.h"

#include <string.h>

#define ALIGN 16u

struct pt_arena_chunk {
    pt_arena_chunk *next;
    size_t cap, used;
};

static size_t round_up(size_t n)
{
    return (n + (ALIGN - 1)) & ~(size_t)(ALIGN - 1);
}

static char *chunk_data(pt_arena_chunk *c)
{
    return (char *)c + round_up(sizeof(pt_arena_chunk));
}

void pt_arena_init(pt_arena *a, pt_allocator al, size_t chunk)
{
    a->al = al;
    a->head = NULL;
    a->chunk = chunk ? chunk : 16384;
    a->oom = false;
}

void *pt_arena_alloc(pt_arena *a, size_t n)
{
    pt_arena_chunk *c = a->head;
    size_t need = round_up(n ? n : 1);
    if (need < n) { /* overflow */
        a->oom = true;
        return NULL;
    }
    if (!c || c->cap - c->used < need) {
        size_t cap = need > a->chunk ? need : a->chunk;
        size_t total = round_up(sizeof(pt_arena_chunk)) + cap;
        pt_arena_chunk *nc;
        if (total < cap) {
            a->oom = true;
            return NULL;
        }
        nc = (pt_arena_chunk *)a->al.fn(a->al.ud, NULL, 0, total);
        if (!nc) {
            a->oom = true;
            return NULL;
        }
        nc->cap = cap;
        nc->used = 0;
        /* A big one-off allocation goes behind the current chunk, so the
         * free space left in that chunk is not wasted. */
        if (c && need > a->chunk) {
            nc->next = c->next;
            c->next = nc;
            nc->used = need;
            return chunk_data(nc);
        }
        nc->next = c;
        a->head = c = nc;
    }
    {
        void *p = chunk_data(c) + c->used;
        c->used += need;
        return p;
    }
}

void *pt_arena_zalloc(pt_arena *a, size_t n)
{
    void *p = pt_arena_alloc(a, n);
    if (p)
        memset(p, 0, n);
    return p;
}

pt_str pt_arena_dup(pt_arena *a, pt_str s)
{
    pt_str r = {"", 0};
    char *p = (char *)pt_arena_alloc(a, s.n + 1);
    if (!p)
        return r;
    if (s.n)
        memcpy(p, s.p, s.n);
    p[s.n] = 0;
    r.p = p;
    r.n = s.n;
    return r;
}

const char *pt_arena_cstr(pt_arena *a, pt_str s)
{
    return pt_arena_dup(a, s).p;
}

static size_t chunk_total(pt_arena_chunk *c)
{
    return round_up(sizeof(pt_arena_chunk)) + c->cap;
}

void pt_arena_reset(pt_arena *a)
{
    pt_arena_chunk *c = a->head, *keep = NULL;
    /* keep the last (oldest) regular chunk, free the rest */
    while (c) {
        pt_arena_chunk *next = c->next;
        if (!next && c->cap == a->chunk) {
            keep = c;
            break;
        }
        a->al.fn(a->al.ud, c, chunk_total(c), 0);
        c = next;
    }
    a->head = keep;
    if (keep)
        keep->used = 0;
    a->oom = false;
}

void pt_arena_free(pt_arena *a)
{
    pt_arena_chunk *c = a->head;
    while (c) {
        pt_arena_chunk *next = c->next;
        a->al.fn(a->al.ud, c, chunk_total(c), 0);
        c = next;
    }
    a->head = NULL;
}
