/* MCP messages from a server: parsing never crashes, a tool or prompt
 * result becomes text whatever its shape, tool names stay short and in
 * [A-Za-z0-9_-], and the messages built for the wire parse back. */
#include <stdlib.h>
#include <string.h>

#include "../../src/rt/mcp_proto.c"

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

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size);
int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
    pt_allocator al = {al_fn, NULL};
    pt_str in = {(const char *)data, size};
    pt_arena a;
    pt_buf b;
    mcp_msg m;
    mcp_result r;
    size_t i, half = size / 2;
    pt_arena_init(&a, al, 0);
    pt_buf_init(&b, al);
    if (mcp_parse(in, &a, &m)) {
        mcp_call_result(m.result, &a, &b, &r);
        pt_buf_reset(&b);
        mcp_prompt_text(m.result, &a, &b);
        pt_buf_reset(&b);
        mcp_resource_text(m.result, &a, &b);
        pt_buf_reset(&b);
        mcp_call_result(m.params, &a, &b, &r);
    }
    /* names: the halves as server and tool */
    pt_buf_reset(&b);
    mcp_tool_name((pt_str){in.p, half}, (pt_str){in.p + half, size - half}, &b);
    if (!b.oom) {
        if (b.n > 64 || !pt_str_starts(pt_buf_str(&b), PT_S("mcp__")))
            abort();
        for (i = 0; i < b.n; i++)
            if (!strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-", b.p[i]))
                abort();
    }
    /* a request carrying the input as a quoted string parses back */
    pt_buf_reset(&b);
    {
        pt_buf p;
        pt_buf_init(&p, al);
        pt_buf_putz(&p, "{\"x\":");
        pt_json_quote(&p, in);
        pt_buf_putc(&p, '}');
        mcp_request(&b, 42, "tools/call", pt_buf_str(&p));
        if (!b.oom && !p.oom && (!mcp_parse(pt_buf_str(&b), &a, &m) || m.kind != MCP_REQUEST || m.nid != 42))
            abort();
        pt_buf_free(&p);
    }
    if (mcp_cut(in, half).n > half)
        abort();
    pt_buf_free(&b);
    pt_arena_free(&a);
    return 0;
}
