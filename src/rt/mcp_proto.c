/* The MCP wire format: JSON-RPC 2.0 messages, tool names, and the content
 * of results as text for the model. Pure: no I/O, no libc allocation. */
#include "rt/mcp.h"

#include <string.h>

/* ---- JSON-RPC ---- */

bool mcp_parse(pt_str json, pt_arena *a, mcp_msg *out)
{
    const pt_json *j = pt_json_parse(json, a, NULL), *id, *err;
    memset(out, 0, sizeof *out);
    if (!pt_json_is(j, PT_J_OBJ) || !pt_str_eqz(pt_json_gets(j, "jsonrpc", a), "2.0"))
        return false;
    id = pt_json_get(j, "id");
    out->method = pt_json_gets(j, "method", a);
    out->params = pt_json_get(j, "params");
    if (id && (id->type == PT_J_NUM || id->type == PT_J_STR)) {
        out->id = id->raw;
        out->numeric = pt_json_int(id, &out->nid);
    } else if (id && id->type != PT_J_NULL) {
        return false;
    }
    if (out->method.n) {
        out->kind = id && id->type != PT_J_NULL ? MCP_REQUEST : MCP_NOTIFY;
        return true;
    }
    if (!out->id.n)
        return false;
    out->kind = MCP_RESPONSE;
    out->result = pt_json_get(j, "result");
    if ((err = pt_json_get(j, "error"))) {
        out->is_error = true;
        out->err_code = pt_json_geti(err, "code", 0);
        out->err_message = pt_json_gets(err, "message", a);
        if (!out->err_message.n)
            out->err_message = PT_S("error");
    } else if (!out->result) {
        return false;
    }
    return true;
}

static void head(pt_buf *b)
{
    pt_buf_putz(b, "{\"jsonrpc\":\"2.0\"");
}

void mcp_request(pt_buf *b, int64_t id, const char *method, pt_str params)
{
    head(b);
    pt_buf_putz(b, ",\"id\":");
    pt_buf_i64(b, id);
    pt_buf_putz(b, ",\"method\":");
    pt_json_quote(b, pt_strz(method));
    if (params.n) {
        pt_buf_putz(b, ",\"params\":");
        pt_buf_puts(b, params);
    }
    pt_buf_putc(b, '}');
}

void mcp_notify(pt_buf *b, const char *method, pt_str params)
{
    head(b);
    pt_buf_putz(b, ",\"method\":");
    pt_json_quote(b, pt_strz(method));
    if (params.n) {
        pt_buf_putz(b, ",\"params\":");
        pt_buf_puts(b, params);
    }
    pt_buf_putc(b, '}');
}

void mcp_reply(pt_buf *b, pt_str id, pt_str result)
{
    head(b);
    pt_buf_putz(b, ",\"id\":");
    pt_buf_puts(b, id);
    pt_buf_putz(b, ",\"result\":");
    pt_buf_puts(b, result.n ? result : PT_S("{}"));
    pt_buf_putc(b, '}');
}

void mcp_reply_error(pt_buf *b, pt_str id, int code, const char *message)
{
    head(b);
    pt_buf_putz(b, ",\"id\":");
    pt_buf_puts(b, id);
    pt_buf_putz(b, ",\"error\":{\"code\":");
    pt_buf_i64(b, code);
    pt_buf_putz(b, ",\"message\":");
    pt_json_quote(b, pt_strz(message));
    pt_buf_putz(b, "}}");
}

void mcp_init_params(pt_buf *b)
{
    pt_buf_putz(b, "{\"protocolVersion\":\"" MCP_VERSION "\",\"capabilities\":{\"roots\":{\"listChanged\":false}},"
                   "\"clientInfo\":{\"name\":\"pytte\",\"title\":\"pytte\",\"version\":\"0.1.0\"}}");
}

/* ---- names ---- */

static bool name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

bool mcp_server_name_ok(pt_str name)
{
    size_t i;
    if (!name.n || name.n > 32)
        return false;
    for (i = 0; i < name.n; i++)
        if (!name_char(name.p[i]))
            return false;
    return true;
}

void mcp_tool_name(pt_str server, pt_str tool, pt_buf *out)
{
    size_t start = out->n, i;
    pt_buf_putz(out, "mcp__");
    for (i = 0; i < server.n; i++)
        pt_buf_putc(out, name_char(server.p[i]) ? server.p[i] : '_');
    pt_buf_putz(out, "__");
    for (i = 0; i < tool.n; i++)
        pt_buf_putc(out, name_char(tool.p[i]) ? tool.p[i] : '_');
    if (out->oom || out->n - start <= 64)
        return;
    { /* cut, and tell cut names apart by a hash of the whole */
        unsigned char d[32];
        char hex[8];
        pt_sha256 sh;
        pt_sha256_init(&sh);
        pt_sha256_update(&sh, out->p + start, out->n - start);
        pt_sha256_final(&sh, d);
        pt_hex(hex, d, 4);
        out->n = start + 64 - 9;
        pt_buf_putc(out, '_');
        pt_buf_put(out, hex, 8);
    }
}

/* ---- content ---- */

pt_str mcp_cut(pt_str s, size_t n)
{
    if (s.n <= n)
        return s;
    while (n && ((unsigned char)s.p[n] & 0xC0) == 0x80)
        n--;
    return (pt_str){s.p, n};
}

static void sep(pt_buf *b, size_t start)
{
    if (b->n > start && b->p[b->n - 1] != '\n')
        pt_buf_putc(b, '\n');
}

/* A resource's contents ({uri, mimeType, text | blob}) as text. */
static void resource(const pt_json *r, pt_arena *a, pt_buf *b, size_t start)
{
    pt_str uri = pt_json_gets(r, "uri", a), mime = pt_json_gets(r, "mimeType", a);
    const pt_json *text = pt_json_get(r, "text"), *blob = pt_json_get(r, "blob");
    sep(b, start);
    if (pt_json_is(text, PT_J_STR)) {
        pt_buf_putz(b, "[resource ");
        pt_buf_puts(b, uri);
        if (mime.n) {
            pt_buf_putz(b, " (");
            pt_buf_puts(b, mime);
            pt_buf_putc(b, ')');
        }
        pt_buf_putz(b, "]\n");
        pt_buf_puts(b, pt_json_str(text, a));
    } else {
        pt_buf_putz(b, "[binary resource ");
        pt_buf_puts(b, uri);
        if (mime.n) {
            pt_buf_putz(b, " (");
            pt_buf_puts(b, mime);
            pt_buf_putc(b, ')');
        }
        if (pt_json_is(blob, PT_J_STR)) {
            pt_buf_putz(b, ", about ");
            pt_buf_u64(b, (uint64_t)(blob->raw.n / 4 * 3));
            pt_buf_putz(b, " bytes");
        }
        pt_buf_putc(b, ']');
    }
}

/* One content block of a result or message. */
static void block(const pt_json *c, pt_arena *a, pt_buf *b, size_t start, mcp_result *res)
{
    pt_str type = pt_json_gets(c, "type", a);
    if (pt_str_eqz(type, "text")) {
        sep(b, start);
        pt_buf_puts(b, pt_json_gets(c, "text", a));
    } else if (pt_str_eqz(type, "image")) {
        pt_str mime = pt_json_gets(c, "mimeType", a);
        if (res && !res->images++) {
            res->image_b64 = pt_json_gets(c, "data", a);
            res->image_mime = mime;
        } else {
            sep(b, start);
            pt_buf_putz(b, "[an image (");
            pt_buf_puts(b, mime);
            pt_buf_putz(b, ") not shown]");
        }
    } else if (pt_str_eqz(type, "audio")) {
        sep(b, start);
        pt_buf_putz(b, "[audio (");
        pt_buf_puts(b, pt_json_gets(c, "mimeType", a));
        pt_buf_putz(b, ") not shown]");
    } else if (pt_str_eqz(type, "resource")) {
        resource(pt_json_get(c, "resource"), a, b, start);
    } else if (pt_str_eqz(type, "resource_link")) {
        pt_str d = pt_json_gets(c, "description", a);
        sep(b, start);
        pt_buf_putz(b, "[resource link ");
        pt_buf_puts(b, pt_json_gets(c, "uri", a));
        if (pt_json_gets(c, "name", a).n) {
            pt_buf_putz(b, ": ");
            pt_buf_puts(b, pt_json_gets(c, "name", a));
        }
        if (d.n) {
            pt_buf_putz(b, " - ");
            pt_buf_puts(b, d);
        }
        pt_buf_putc(b, ']');
    }
}

void mcp_call_result(const pt_json *result, pt_arena *a, pt_buf *text, mcp_result *out)
{
    const pt_json *content = pt_json_get(result, "content"), *c, *sc;
    size_t start = text->n;
    memset(out, 0, sizeof *out);
    out->is_error = pt_json_truthy(pt_json_get(result, "isError"));
    for (c = pt_json_is(content, PT_J_ARR) ? content->child : NULL; c; c = c->next)
        block(c, a, text, start, out);
    sc = pt_json_get(result, "structuredContent");
    if (text->n == start && sc) /* only structured: the JSON is the text */
        pt_buf_puts(text, sc->raw);
}

void mcp_prompt_text(const pt_json *result, pt_arena *a, pt_buf *text)
{
    const pt_json *msgs = pt_json_get(result, "messages"), *m;
    size_t start = text->n;
    for (m = pt_json_is(msgs, PT_J_ARR) ? msgs->child : NULL; m; m = m->next) {
        pt_str role = pt_json_gets(m, "role", a);
        const pt_json *c = pt_json_get(m, "content");
        size_t at;
        if (text->n > start)
            pt_buf_putz(text, "\n\n");
        at = text->n;
        if (pt_str_eqz(role, "assistant"))
            pt_buf_putz(text, "[assistant]\n");
        if (pt_json_is(c, PT_J_ARR)) {
            const pt_json *k;
            for (k = c->child; k; k = k->next)
                block(k, a, text, at, NULL);
        } else {
            block(c, a, text, at, NULL);
        }
    }
}

void mcp_resource_text(const pt_json *result, pt_arena *a, pt_buf *text)
{
    const pt_json *contents = pt_json_get(result, "contents"), *c;
    size_t start = text->n;
    for (c = pt_json_is(contents, PT_J_ARR) ? contents->child : NULL; c; c = c->next) {
        const pt_json *t = pt_json_get(c, "text");
        if (text->n > start)
            pt_buf_putz(text, "\n\n");
        if (pt_json_is(t, PT_J_STR)) {
            pt_buf_puts(text, pt_json_str(t, a));
        } else {
            size_t at = text->n;
            resource(c, a, text, at);
        }
    }
}
