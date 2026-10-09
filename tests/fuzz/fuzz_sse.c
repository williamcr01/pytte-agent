/* SSE parser: never crashes, and splitting the input anywhere gives the
 * same events. */
#include <stdlib.h>
#include <string.h>

#include "pytte_base.h"

static void *al_fn(void *ud, void *p, size_t old, size_t n)
{
    (void)ud;
    (void)old;
    if (!n) {
        free(p);
        return NULL;
    }
    return realloc(p, n);
}

static int collect(void *ud, const pt_sse_event *ev)
{
    pt_buf *b = (pt_buf *)ud;
    pt_buf_u64(b, ev->event.n);
    pt_buf_puts(b, ev->event);
    pt_buf_u64(b, ev->data.n);
    pt_buf_puts(b, ev->data);
    pt_buf_u64(b, ev->id.n);
    pt_buf_puts(b, ev->id);
    return 0;
}

static void run(const char *p, size_t n, size_t split, pt_buf *out)
{
    pt_allocator al = {al_fn, NULL};
    pt_sse s;
    pt_sse_init(&s, al);
    pt_sse_feed(&s, p, split, collect, out);
    pt_sse_feed(&s, p + split, n - split, collect, out);
    pt_sse_finish(&s, collect, out);
    pt_sse_free(&s);
}

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size);
int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
    pt_allocator al = {al_fn, NULL};
    pt_buf a, b;
    size_t split = size ? data[0] % (size + 1) : 0;
    pt_buf_init(&a, al);
    pt_buf_init(&b, al);
    run((const char *)data, size, size, &a);
    run((const char *)data, size, split, &b);
    if (a.n != b.n || (a.n && memcmp(a.p, b.p, a.n) != 0))
        abort();
    pt_buf_free(&a);
    pt_buf_free(&b);
    return 0;
}
