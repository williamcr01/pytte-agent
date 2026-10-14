/* MCP inside the runtime: the protocol codec (mcp_proto.c, pure: no I/O,
 * so the fuzzer links it alone), the client (mcp.c), its transports
 * (mcp_io.c) and OAuth for remote servers (mcp_oauth.c). */
#ifndef PT_RT_MCP_H
#define PT_RT_MCP_H

#include "pytte_base.h"

/* The protocol version this client speaks; the server may answer with an
 * older one it supports. */
#define MCP_VERSION "2025-06-18"

/* ---- JSON-RPC ---- */

typedef enum { MCP_BAD, MCP_RESPONSE, MCP_REQUEST, MCP_NOTIFY } mcp_kind;

typedef struct mcp_msg {
    mcp_kind kind;
    pt_str id;   /* raw JSON of the id (requests, responses) */
    int64_t nid; /* ...as a number, when it is one (ours always are) */
    bool numeric;
    pt_str method;
    const pt_json *params, *result;
    bool is_error;      /* a response with "error" */
    int64_t err_code;
    pt_str err_message; /* decoded, in the arena */
} mcp_msg;

/* One message (a JSON object). False when it is not a JSON-RPC 2.0 one. */
bool mcp_parse(pt_str json, pt_arena *a, mcp_msg *out);
/* A message for the wire, without the newline: params is raw JSON, or
 * empty for none. */
void mcp_request(pt_buf *b, int64_t id, const char *method, pt_str params);
void mcp_notify(pt_buf *b, const char *method, pt_str params);
void mcp_reply(pt_buf *b, pt_str id, pt_str result);
void mcp_reply_error(pt_buf *b, pt_str id, int code, const char *message);
/* initialize's params: this client, its version and capabilities */
void mcp_init_params(pt_buf *b);

/* ---- names ---- */

/* "mcp__<server>__<tool>": what the model calls it. Characters outside
 * [A-Za-z0-9_-] become _; past 64 bytes the tool's part is cut and a hash
 * of the whole keeps names apart. */
void mcp_tool_name(pt_str server, pt_str tool, pt_buf *out);
/* A server name config may use: 1 to 32 of [A-Za-z0-9_-]. */
bool mcp_server_name_ok(pt_str name);

/* ---- content ---- */

/* A tools/call result: its text blocks joined (resources and links
 * described, structuredContent when there is no text), and the first
 * image's base64 data and MIME type. */
typedef struct mcp_result {
    pt_str image_b64, image_mime;
    bool is_error;
    uint32_t images; /* how many there were (one is shown) */
} mcp_result;
void mcp_call_result(const pt_json *result, pt_arena *a, pt_buf *text, mcp_result *out);
/* prompts/get: its messages as one text for the user's turn. */
void mcp_prompt_text(const pt_json *result, pt_arena *a, pt_buf *text);
/* resources/read: the contents, as text (binary ones described). */
void mcp_resource_text(const pt_json *result, pt_arena *a, pt_buf *text);

/* At most n bytes of s, ending on a whole UTF-8 character. */
pt_str mcp_cut(pt_str s, size_t n);

/* ---- arguments the config fills in (pt_rt_mcp_server.arguments) ---- */

/* Those of tool, from fill ({"<tool>" or "*": {...}}; NULL: none): its own
 * entry's, and the "*" entry's its schema has (properties) and its own
 * does not set, as a JSON object into out. False (out empty) when none. */
bool mcp_fill_of(const pt_json *fill, pt_str tool, const pt_json *schema, pt_buf *out);
/* schema (an object) without the arguments of fill (a JSON object), in
 * properties and required, into out. False when fill does not parse. */
bool mcp_schema_strip(const pt_json *schema, pt_str fill, pt_buf *out, pt_arena *tmp);
/* args (the model's, a JSON object; anything else counts as none) with
 * fill's in place of its own, as a JSON object into out. In fill's
 * strings, {model} becomes model ("provider/model-id"), {model_id} the
 * part after the provider, {session} session. */
void mcp_fill_args(pt_str fill, pt_str args, pt_str model, pt_str session, pt_buf *out, pt_arena *tmp);

#endif
