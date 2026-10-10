/* Log loader and scheduler on hostile logs: never crashes, never loops.
 * The first byte picks the mode: raw lines, or each line wrapped as a
 * valid commit (seq and checksum fixed up), which gets past the checksum
 * and into the records. Then the agent runs a few steps against a host
 * that accepts every commit and fails every request. */
#include <stdlib.h>
#include <string.h>

#include "core/core.h"

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

static int64_t now_fn(void *ud)
{
    (void)ud;
    return 1760000000000;
}

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size);
int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
    pt_allocator al = {al_fn, NULL};
    pt_opts o;
    pt_agent *a;
    pt_buf line;
    pt_jw w;
    const char *p = (const char *)data + 1;
    size_t n = size ? size - 1 : 0, start = 0, i;
    bool wrap = size && (data[0] & 1);
    uint64_t seq = 1;
    int steps;
    pt_fx fx;
    memset(&o, 0, sizeof o);
    o.alloc = al;
    o.now_ms = now_fn;
    o.config.model = PT_S("faux");
    a = pt_new(&o);
    pt_buf_init(&line, al);
    for (i = 0; i <= n; i++) {
        pt_load_result r;
        pt_str l;
        if (i < n && p[i] != '\n')
            continue;
        l = (pt_str){p + start, i - start};
        if (wrap) {
            pt_log_begin(&line, &w, seq++, 1);
            pt_buf_puts(&line, l); /* the records, whatever they are */
            pt_log_end(&line, &w);
            l = (pt_str){line.p, line.n - 1};
        }
        r = pt_load_line(a, l, i < n, i == n);
        if (r != PT_LOAD_OK)
            break;
        start = i + 1;
    }
    if (pt_load_done(a)) {
        pt_identity me;
        memset(&me, 0, sizeof me);
        me.host = PT_S("fuzz");
        pt_open(a, &me, true);
        for (steps = 0; steps < 200 && pt_poll(a, &fx); steps++) {
            if (fx.kind == PT_FX_COMMIT || fx.kind == PT_FX_SLEEP)
                pt_fx_done(a, fx.id, NULL);
            else if (fx.kind == PT_FX_HTTP)
                pt_fx_done(a, fx.id, "no network in the fuzzer");
        }
    }
    pt_buf_free(&line);
    pt_free(a);
    return 0;
}
