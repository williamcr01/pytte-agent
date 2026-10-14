/* Internal interface of the AI layer: the API vtable and what APIs use to
 * build requests and drive a stream. */
#ifndef PT_AI_INTERNAL_H
#define PT_AI_INTERNAL_H

#include "pytte_ai.h"

struct pt_api_vt {
    const char *id; /* "anthropic-messages", "openai-completions", "faux" */

    /* Build the HTTP request for a context already in the target's view.
     * Pure. */
    bool (*build)(const pt_ai_target *t, const pt_ai_context *ctx, const pt_ai_opts *o,
                  const pt_credentials *cred, pt_http_req *out, pt_arena *a);

    /* Per-request stream state: state_size zeroed bytes. */
    size_t state_size;

    /* One SSE event in, unified events out (through pt_emit_*). Nonzero
     * stops feeding. */
    int (*feed)(pt_ai_stream *s, void *st, const pt_sse_event *ev);

    /* Clean end of the body without a terminal event yet: settle the final
     * message (done, or error for a cut stream). */
    void (*finish)(pt_ai_stream *s, void *st);

    /* Non-2xx response: class and message from the body. */
    void (*classify_error)(int status, pt_str body, pt_ai_error *out, pt_arena *a);

    pt_compat defaults;
    uint8_t efforts;      /* PT_EFF_* of models nothing else describes */
    uint32_t tool_id_max; /* longest tool call ID the API accepts; 0 = any */
};

extern const pt_api_vt pt_api_anthropic;
extern const pt_api_vt pt_api_openai;
extern const pt_api_vt pt_api_openai_responses;
extern const pt_api_vt pt_api_openai_codex;
extern const pt_api_vt pt_api_faux;

/* ---- what APIs use while streaming ---- */

pt_msg *pt_emit_msg(pt_ai_stream *s);
const pt_ai_target *pt_emit_target(const pt_ai_stream *s);
pt_arena *pt_emit_arena(pt_ai_stream *s);   /* lives as long as the final message */
pt_arena *pt_emit_scratch(pt_ai_stream *s); /* reset before every SSE event */
pt_arena *pt_emit_own(pt_ai_stream *s);     /* lives as long as the stream */
bool pt_emit_started(const pt_ai_stream *s);
void pt_emit_start(pt_ai_stream *s);
/* Open a new block (emits its *_start event). Returns its index, or -1. */
long pt_emit_block(pt_ai_stream *s, pt_block_kind kind, pt_str id, pt_str name);
void pt_emit_delta(pt_ai_stream *s, long idx, pt_str delta);
void pt_emit_signature(pt_ai_stream *s, long idx, pt_str sig);
/* Replace an open block's content (text, thinking or arguments) with the
 * server's final version, without an event. */
void pt_emit_set_content(pt_ai_stream *s, long idx, pt_str content);
void pt_emit_end(pt_ai_stream *s, long idx); /* emits *_end; no-op when closed */
bool pt_emit_is_open(const pt_ai_stream *s, long idx);
void pt_emit_end_all(pt_ai_stream *s);
/* A complete opaque block (no events). */
void pt_emit_opaque(pt_ai_stream *s, pt_str raw);
/* A complete redacted thinking block. */
void pt_emit_redacted(pt_ai_stream *s, pt_str data);
/* Terminal events. done uses msg->stop as set by the API. */
void pt_emit_done(pt_ai_stream *s);
void pt_emit_fail(pt_ai_stream *s, pt_err_class cls, pt_str message);
bool pt_emit_finished(const pt_ai_stream *s);

/* ---- request helpers ---- */

/* Start a request with room for `extra` headers beyond the provider's and
 * the caller's. */
bool pt_req_init(pt_http_req *r, const pt_ai_target *t, const pt_ai_opts *o, const char *path,
                 uint32_t extra, pt_arena *a);
void pt_req_header(pt_http_req *r, const char *name, const char *value);
/* Adds the provider's and the caller's extra headers (call last). */
void pt_req_extra_headers(pt_http_req *r, const pt_ai_target *t, const pt_ai_opts *o);
/* Finish the body; false when out of memory. */
bool pt_req_body(pt_http_req *r, pt_buf *b, pt_arena *a);

/* OpenAI's {"error":{"message","type","code"}} errors, shared by both of
 * its APIs (and OpenRouter); status 0 when there is no HTTP status. */
void pt_openai_error(const pt_json *err, pt_str fallback, int status, pt_ai_error *out, pt_arena *a);
/* OpenAI's reasoning effort names. */
const char *pt_openai_effort(pt_reasoning r);

/* Shared error classification: retryable statuses, overflow phrases. */
void pt_classify_status(int status, pt_str message, pt_ai_error *out);

/* Rewrite a tool call ID into [A-Za-z0-9_-]{1,max}; deterministic, so calls
 * and results stay matched. Returns id itself when it is already fine. */
pt_str pt_fix_tool_id(pt_str id, uint32_t max, pt_arena *a);

/* A tool result's image goes to the target: it has one, and the model
 * takes images. */
bool pt_ai_sends_image(const pt_ai_target *t, const pt_msg *m);
/* A tool result's text for the target: its own, and a note when it has an
 * image the model cannot be sent. */
pt_str pt_ai_result_text(const pt_ai_target *t, const pt_msg *m, pt_arena *a);
/* The image as a data URL string: "data:<mime>;base64,...". */
void pt_ai_image_url(pt_jw *w, const pt_msg *m, pt_arena *a);

#endif
