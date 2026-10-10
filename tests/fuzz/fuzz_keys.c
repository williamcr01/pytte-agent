/* The key decoder: never crashes, only clean text comes out, and splitting
 * the input anywhere gives the same events. */
#include <stdlib.h>
#include <string.h>

#include "tui/tui.h"

static void run(const char *p, size_t n, size_t split, pt_buf *out)
{
    tui_keys k;
    tui_ev ev;
    int part;
    bool text = false;
    tui_keys_init(&k);
    k.want_cpr = n && p[0] == 'R';
    for (part = 0; part < 2; part++) {
        if (part == 0)
            tui_keys_feed(&k, p, split);
        else
            tui_keys_feed(&k, p + split, n - split);
        while (tui_keys_next(&k, &ev, part == 1)) {
            size_t i;
            if (ev.kind == TE_TEXT || ev.kind == TE_PASTE)
                for (i = 0; i < ev.text.n; i++)
                    if (((unsigned char)ev.text.p[i] < 0x20 && ev.text.p[i] != '\n') || ev.text.p[i] == 0x7f)
                        abort();
            if (ev.kind == TE_TEXT) { /* runs of text may be cut differently */
                if (!text)
                    pt_buf_putc(out, 'T');
                text = true;
                pt_buf_puts(out, ev.text);
                continue;
            }
            text = false;
            pt_buf_putc(out, (char)('0' + ev.kind));
            pt_buf_u64(out, ev.key);
            pt_buf_u64(out, (uint64_t)ev.mods);
            pt_buf_u64(out, ev.reply);
            pt_buf_u64(out, (uint64_t)ev.a);
            pt_buf_u64(out, (uint64_t)ev.b);
            pt_buf_puts(out, ev.text);
        }
    }
    if (tui_keys_pending(&k) && !k.paste)
        abort(); /* a flush decodes everything */
    tui_keys_free(&k);
}

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size);
int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
    pt_buf a, b;
    size_t split = size ? data[0] % (size + 1) : 0;
    tui_keys tmp; /* for its allocator */
    tui_keys_init(&tmp);
    pt_buf_init(&a, tmp.in.al);
    pt_buf_init(&b, tmp.in.al);
    run((const char *)data, size, size, &a);
    run((const char *)data, size, split, &b);
    if (a.n != b.n || (a.n && memcmp(a.p, b.p, a.n) != 0))
        abort();
    pt_buf_free(&a);
    pt_buf_free(&b);
    tui_keys_free(&tmp);
    return 0;
}
