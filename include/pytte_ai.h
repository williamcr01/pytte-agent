/* pytte_ai.h: the AI layer (libpytte-ai).
 *
 * One message format and one event stream for every provider, modeled on
 * pi's packages/ai. Sans-I/O: pt_ai_build() describes an HTTP request, the
 * host performs it and feeds the response bytes to a pt_ai_stream, which
 * turns them into unified events and a final message. */
#ifndef PYTTE_AI_H
#define PYTTE_AI_H

#include "pytte_base.h"

/* ---- unified messages ------------------------------------------------------ */

typedef enum { PT_ROLE_SYSTEM, PT_ROLE_USER, PT_ROLE_ASSISTANT, PT_ROLE_TOOL_RESULT } pt_role;

typedef enum {
    PT_BLOCK_TEXT,      /* text */
    PT_BLOCK_THINKING,  /* text, signature, redacted */
    PT_BLOCK_TOOL_CALL, /* id, name, args (raw JSON text, exactly as received) */
    PT_BLOCK_IMAGE,     /* later */
    PT_BLOCK_OPAQUE     /* api + raw provider JSON, echoed back to the same api only */
} pt_block_kind;

typedef enum {
    PT_STOP_STOP,     /* the model finished its turn */
    PT_STOP_LENGTH,   /* hit max tokens */
    PT_STOP_TOOL_USE, /* wants tool results */
    PT_STOP_REFUSAL,  /* declined */
    PT_STOP_ERROR,    /* request or stream failed */
    PT_STOP_ABORTED   /* cancelled */
} pt_stop;

typedef struct pt_block {
    pt_block_kind kind;
    pt_str text;      /* text, thinking */
    pt_str signature; /* thinking; the encrypted data when redacted */
    bool redacted;
    pt_str id, name; /* tool call */
    pt_str args;     /* tool call: raw JSON text */
    pt_str api, raw; /* opaque */
} pt_block;

typedef struct pt_usage {
    uint64_t input, output, cache_read, cache_write, reasoning;
} pt_usage;

typedef struct pt_msg {
    pt_role role;
    pt_str text; /* system, user and tool_result content */
    /* assistant */
    pt_str api, provider, model, response_model, response_id;
    pt_block *blocks;
    uint32_t nblocks;
    pt_usage usage;
    pt_stop stop;
    pt_str raw_stop, error;
    int64_t timestamp, duration_ms;
    /* tool_result */
    pt_str call_id, name;
    bool is_error;
} pt_msg;

typedef struct pt_tool {
    pt_str name, description;
    pt_str schema; /* JSON Schema of the arguments, raw JSON */
} pt_tool;

typedef struct pt_ai_context {
    const pt_msg *msgs; /* the first system message is the system prompt */
    uint32_t nmsgs;
    const pt_tool *tools;
    uint32_t ntools;
} pt_ai_context;

const char *pt_stop_name(pt_stop s);
const char *pt_role_name(pt_role r);
/* Serialize / parse the unified JSON form (the transcript format). */
void pt_msg_write_json(pt_jw *w, const pt_msg *m);
bool pt_msg_read_json(const pt_json *v, pt_msg *m, pt_arena *a);

/* ---- providers and models ------------------------------------------------- */

typedef struct pt_header {
    const char *name, *value;
} pt_header;

typedef enum { PT_AUTH_NONE, PT_AUTH_X_API_KEY, PT_AUTH_BEARER } pt_auth_style;

/* Dialect quirks. 0 always means "inherit": a model's compat overrides its
 * provider's, which overrides the API's defaults. */
enum { PT_INHERIT = 0, PT_NO = 1, PT_YES = 2 };
enum { PT_MAXTOK_MAX_TOKENS = 1, PT_MAXTOK_MAX_COMPLETION_TOKENS = 2 };
enum { PT_SYSROLE_SYSTEM = 1, PT_SYSROLE_DEVELOPER = 2 };
enum { PT_RPARAM_NONE = 1, PT_RPARAM_EFFORT = 2, PT_RPARAM_OPENROUTER = 3 };

typedef struct pt_compat {
    uint8_t max_tokens_field;        /* PT_MAXTOK_* */
    uint8_t system_role;             /* PT_SYSROLE_* */
    uint8_t stream_usage;            /* stream_options.include_usage */
    uint8_t reasoning_param;         /* PT_RPARAM_* */
    uint8_t reasoning_delta;         /* reasoning text in delta.reasoning(_content) */
    uint8_t mid_conversation_system; /* system messages after the first, in place */
    uint8_t parallel_tool_calls;     /* send parallel_tool_calls: true */
    uint8_t fallbacks;               /* anthropic: server-side refusal fallbacks */
    uint8_t thinking_display;        /* anthropic: "summarized" thinking */
} pt_compat;

typedef struct pt_cost { /* micro-dollars per million tokens */
    uint64_t input, output, cache_read, cache_write;
} pt_cost;

typedef struct pt_provider {
    const char *id, *name, *base_url;
    const char *api;            /* default API of its models */
    const char *key_env[4];     /* env vars to try, in order; NULL-terminated */
    pt_auth_style auth;
    pt_header headers[4];       /* extra headers; NULL name ends the list */
    pt_compat compat;
    const char *default_model;  /* used when a reference names only the provider */
    uint32_t context_window, max_tokens; /* defaults for models not in the catalog */
    bool reasoning;             /* default for models not in the catalog */
} pt_provider;

typedef struct pt_model {
    const char *provider, *id, *name;
    const char *api; /* overrides the provider's when set */
    uint32_t context_window, max_tokens; /* 0 = unknown */
    bool reasoning, images;
    pt_cost cost;
    pt_compat compat;
} pt_model;

typedef struct pt_ai_catalog {
    const pt_provider *providers;
    uint32_t nproviders;
    const pt_model *models;
    uint32_t nmodels;
} pt_ai_catalog;

const pt_ai_catalog *pt_ai_builtin_catalog(void);

typedef struct pt_api_vt pt_api_vt;
const pt_api_vt *pt_ai_api(pt_str id);
const char *pt_ai_api_id(const pt_api_vt *api);

/* Everything needed to talk to one model. */
typedef struct pt_ai_target {
    const pt_provider *provider;
    pt_model model; /* from the catalog, or synthesized from provider defaults */
    const pt_api_vt *api;
    pt_compat compat; /* resolved: API defaults < provider < model */
    bool known;       /* model is in the catalog */
} pt_ai_target;

/* Resolve "provider/model-id" (split at the first slash) or "provider".
 * Returns false with *why set when the provider or its API is unknown. */
bool pt_ai_resolve(const pt_ai_catalog *cat, pt_str ref, pt_ai_target *out, pt_arena *a,
                   const char **why);

/* ---- auth ------------------------------------------------------------------ */

typedef struct pt_credentials {
    pt_str api_key;
} pt_credentials;

/* Looks up an environment variable; returns NULL when unset. */
typedef const char *(*pt_env_fn)(void *ud, const char *name);
/* The first set variable of the provider's key_env list. On success *which
 * names it. False when none is set (and the provider needs a key). */
bool pt_ai_find_key(const pt_provider *p, pt_env_fn env, void *ud, pt_credentials *out,
                    const char **which);

/* ---- requests ---------------------------------------------------------------- */

typedef enum {
    PT_REASON_DEFAULT,
    PT_REASON_OFF,
    PT_REASON_MINIMAL,
    PT_REASON_LOW,
    PT_REASON_MEDIUM,
    PT_REASON_HIGH,
    PT_REASON_XHIGH,
    PT_REASON_MAX
} pt_reasoning;

bool pt_reasoning_parse(pt_str s, pt_reasoning *out);
const char *pt_reasoning_name(pt_reasoning r);

typedef struct pt_ai_opts {
    uint32_t max_tokens;     /* 0: model maximum, clamped to what fits the context */
    uint32_t input_estimate; /* tokens; 0: estimated from the context */
    pt_reasoning reasoning;
    pt_str temperature; /* raw JSON number; empty = unset */
    bool no_cache;
    pt_str session_id;
    const pt_header *headers;
    uint32_t nheaders;
} pt_ai_opts;

typedef struct pt_http_req {
    const char *method, *url;
    pt_header *headers;
    uint32_t nheaders, hcap;
    pt_str body;
} pt_http_req;

/* Build the HTTP request for a context. Pure: the same inputs always give
 * the same bytes. Everything is allocated in a. */
bool pt_ai_build(const pt_ai_target *t, const pt_ai_context *ctx, const pt_ai_opts *o,
                 const pt_credentials *cred, pt_http_req *out, pt_arena *a);

/* The context as the target will see it: cross-provider transforms. */
bool pt_ai_target_view(const pt_ai_target *t, const pt_ai_context *in, pt_ai_context *out,
                       pt_arena *a);

/* max_tokens for a request, when the caller did not set one. 0 = omit. */
uint32_t pt_ai_auto_max_tokens(const pt_ai_target *t, const pt_ai_context *ctx,
                               uint32_t input_estimate);

/* ---- streaming ---------------------------------------------------------------- */

typedef enum {
    PT_EV_START,
    PT_EV_TEXT_START,
    PT_EV_TEXT_DELTA,
    PT_EV_TEXT_END,
    PT_EV_THINKING_START,
    PT_EV_THINKING_DELTA,
    PT_EV_THINKING_END,
    PT_EV_TOOLCALL_START,
    PT_EV_TOOLCALL_DELTA,
    PT_EV_TOOLCALL_END,
    PT_EV_DONE, /* final message, stop = stop, length, tool_use or refusal */
    PT_EV_ERROR /* final message, stop = error or aborted */
} pt_ev_kind;

const char *pt_ev_name(pt_ev_kind k);

typedef struct pt_ai_event {
    pt_ev_kind kind;
    uint32_t index;   /* block index for block events */
    pt_str delta;     /* delta events */
    const pt_msg *msg; /* the partial (or, for done/error, final) message */
} pt_ai_event;

typedef void (*pt_ai_on_event)(void *ud, const pt_ai_event *ev);

typedef enum {
    PT_ERR_NONE,
    PT_ERR_RETRYABLE, /* 429, 5xx, overloaded, connection errors, cut streams */
    PT_ERR_OVERFLOW,  /* the context is too long */
    PT_ERR_FATAL,     /* other 4xx: bad key, bad request */
    PT_ERR_ABORTED
} pt_err_class;

const char *pt_err_class_name(pt_err_class c);

typedef struct pt_ai_error {
    pt_err_class cls;
    int status;             /* HTTP status, 0 when none */
    int64_t retry_after_ms; /* -1 when not given */
    pt_str message;
} pt_ai_error;

typedef struct pt_ai_stream pt_ai_stream;

/* The final message's strings live in a; the stream's own state is freed by
 * pt_ai_stream_free. now_ms stamps the message (the core has no clock). */
pt_ai_stream *pt_ai_stream_new(const pt_ai_target *t, pt_arena *a, pt_allocator al,
                               pt_ai_on_event on_event, void *ud, int64_t now_ms);
/* Response status and the Retry-After header (seconds), before any body. */
void pt_ai_stream_head(pt_ai_stream *s, int status, pt_str retry_after);
void pt_ai_stream_body(pt_ai_stream *s, const char *p, size_t n, int64_t now_ms);
/* End of the response. transport_error != NULL means the connection
 * failed. Emits the terminal done/error event if none was emitted yet. */
void pt_ai_stream_end(pt_ai_stream *s, const char *transport_error, int64_t now_ms);
void pt_ai_stream_abort(pt_ai_stream *s, int64_t now_ms);
bool pt_ai_stream_finished(const pt_ai_stream *s);
const pt_msg *pt_ai_stream_message(const pt_ai_stream *s);
const pt_ai_error *pt_ai_stream_error(const pt_ai_stream *s);
void pt_ai_stream_free(pt_ai_stream *s);

/* ---- faux provider ------------------------------------------------------------ */

typedef struct pt_faux_response {
    int status;
    pt_str body;    /* SSE (or an error body) */
    uint32_t chunk; /* deliver in pieces of this many bytes; 0 = all at once */
    uint32_t delay_ms;
    int64_t cut;    /* close the connection after this many bytes; -1 = never */
} pt_faux_response;

/* The faux "server": answers a request built for the faux API from a script
 * (see tests/faux/). Pure; allocates in a. */
bool pt_faux_respond(pt_str script, pt_str request_body, pt_faux_response *out, pt_arena *a);

#endif
