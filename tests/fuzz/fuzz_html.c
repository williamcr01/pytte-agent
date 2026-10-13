/* HTML to text: never crashes, splitting the input anywhere gives the
 * same text, and the output is never far longer than the input. The first
 * byte picks the split; a URL resolve runs on the input as well. */
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

static void run(const char *p, size_t n, size_t split, pt_buf *out)
{
    pt_allocator al = {al_fn, NULL};
    pt_html h;
    pt_html_init(&h, al, out, PT_S("https://example.com/a/b?c"));
    pt_html_feed(&h, p, split);
    pt_html_feed(&h, p + split, n - split);
    pt_html_finish(&h);
    pt_html_free(&h);
}

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size);
int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
    pt_allocator al = {al_fn, NULL};
    pt_buf a, b, u;
    size_t split = size ? data[0] % (size + 1) : 0;
    pt_buf_init(&a, al);
    pt_buf_init(&b, al);
    pt_buf_init(&u, al);
    run((const char *)data, size, size, &a);
    run((const char *)data, size, split, &b);
    if (a.n != b.n || (a.n && memcmp(a.p, b.p, a.n) != 0))
        abort();
    /* links add their URL, lists their markers: bounded growth */
    if (a.n > 64 + size * 8)
        abort();
    pt_url_resolve(PT_S("https://example.com/a/b?c"), (pt_str){(const char *)data, size}, &u);
    pt_buf_free(&a);
    pt_buf_free(&b);
    pt_buf_free(&u);
    return 0;
}
