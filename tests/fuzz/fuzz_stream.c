/* Response streams: any bytes, for every API, end in exactly one terminal
 * event and a message that serializes to valid JSON. */
#include <stdlib.h>
#include <string.h>

#include "pytte_ai.h"

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

static void on_event(void *ud, const pt_ai_event *ev)
{
    int *terminals = (int *)ud;
    if (*terminals)
        abort(); /* nothing after the terminal event */
    if (ev->kind == PT_EV_DONE || ev->kind == PT_EV_ERROR)
        (*terminals)++;
}

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size);
int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
    static const char *const refs[] = {"anthropic", "openai", "openrouter/x/y", "faux"};
    pt_allocator al = {al_fn, NULL};
    pt_arena a;
    pt_ai_target t;
    pt_ai_stream *s;
    const char *why;
    int terminals = 0;
    size_t i = 0, chunk;
    pt_buf out;
    pt_jw w;
    if (size < 2)
        return 0;
    pt_arena_init(&a, al, 0);
    if (!pt_ai_resolve(pt_ai_builtin_catalog(), pt_strz(refs[data[0] % 4]), &t, &a, &why))
        abort();
    chunk = (size_t)(data[1] % 64) + 1;
    s = pt_ai_stream_new(&t, &a, al, on_event, &terminals, 0);
    pt_ai_stream_head(s, data[0] & 0x80 ? 500 : 200, PT_STR_NULL);
    data += 2;
    size -= 2;
    while (i < size) {
        size_t n = size - i < chunk ? size - i : chunk;
        pt_ai_stream_body(s, (const char *)data + i, n, 0);
        i += n;
    }
    pt_ai_stream_end(s, NULL, 0);
    if (terminals != 1)
        abort();
    pt_buf_init(&out, al);
    pt_jw_init(&w, &out);
    pt_msg_write_json(&w, pt_ai_stream_message(s));
    if (!out.oom && !pt_json_parse(pt_buf_str(&out), &a, NULL))
        abort();
    pt_buf_free(&out);
    pt_ai_stream_free(s);
    pt_arena_free(&a);
    return 0;
}
