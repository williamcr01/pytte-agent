/* pytte.h: the durable agent core (libpytte).
 *
 * A sans-I/O task interpreter. The core never touches files, sockets or
 * clocks: it hands the host effects (append this commit, make this HTTP
 * request, sleep until then) through pt_poll(), and the host reports back
 * with pt_fx_*(). Every step is committed to the session log before its
 * effect starts, so a session survives the process dying at any point.
 *
 * The host loop:
 *
 *     pt_fx fx;
 *     for (;;) {
 *         while (pt_poll(a, &fx)) start_effect(&fx);   // async or blocking
 *         if (pt_idle(a) || pt_fatal(a)) break;
 *         wait_for_progress();   // calls pt_fx_head / pt_fx_data / pt_fx_done
 *     }
 *
 * Display events arrive through the on_event callback. */
#ifndef PYTTE_H
#define PYTTE_H

#include "pytte_ai.h"

#define PT_VERSION "0.1.0"

typedef struct pt_agent pt_agent;

/* ---- effects -------------------------------------------------------------- */

typedef enum {
    PT_FX_COMMIT = 1, /* append bytes to the log (fsync when sync); complete with pt_fx_done */
    PT_FX_HTTP,       /* perform http; stream with pt_fx_head, pt_fx_data, then pt_fx_done */
    PT_FX_SLEEP,      /* complete with pt_fx_done once the wall clock reaches until */
    PT_FX_LIVE,       /* best effort: replace the task's live file with bytes, or delete it
                         when bytes is empty. No completion. */
    PT_FX_CANCEL,     /* stop effect `cancel` (a request, a sleep, a tool); no completion */
    PT_FX_GATE,       /* may this tool call run? answer with pt_fx_gate */
    PT_FX_ASK,        /* ask the user about a tool call; answered by pt_approve, no completion */
    PT_FX_TOOL,       /* run a tool: pt_fx_data for its output as it comes, then pt_fx_tool */
    PT_FX_BLOB,       /* store bytes as a blob (section 6.5); complete with pt_fx_blob */
    PT_FX_EPOCH_MATERIALS /* an epoch's system prompt and tools (a new one, or a provisional one
                             being settled); complete with pt_fx_materials */
} pt_fx_kind;

/* What running a tool again after an unknown partial run does (section 13.4). */
typedef enum { PT_REPLAY_UNSAFE, PT_REPLAY_SAFE } pt_replay;

typedef struct pt_fx {
    pt_fx_kind kind;
    uint32_t id;   /* unique within the agent */
    uint32_t task; /* the task it serves; 0 for session commits */
    /* COMMIT: one line with its newline, valid until completed.
     * LIVE: the snapshot, valid until the next pt_poll.
     * BLOB: the contents, valid until completed.
     * EPOCH_MATERIALS: the current epoch's system prompt (to keep when the
     * host has nothing newer), valid until completed. */
    pt_str bytes;
    bool sync;                /* COMMIT */
    const pt_http_req *http;  /* HTTP: valid until completed or cancelled */
    int64_t until;            /* SLEEP: wall-clock ms */
    uint32_t cancel;          /* CANCEL */
    /* GATE, ASK, TOOL: valid until completed (ASK: until answered). */
    pt_str name, args;        /* the tool and its arguments (a JSON object) */
    pt_str call_id;           /* TOOL: stable across attempts */
    pt_str prompt;            /* ASK */
    uint32_t attempt;         /* TOOL: 2 and up when run again after a crash */
    pt_replay replay;         /* GATE, TOOL */
    uint32_t epoch;           /* EPOCH_MATERIALS: the new epoch's number */
} pt_fx;

/* ---- events (display only; no reply) ----------------------------------------- */

typedef enum {
    PT_E_STREAM,    /* ai: a stream event of a running request */
    PT_E_COMMITTED, /* line: a commit was applied (the JSON, without newline) */
    PT_E_RETRY,     /* error, until: the run waits to try again */
    PT_E_RUN_DONE,  /* outcome, error: a run finished */
    PT_E_IDLE,      /* nothing left to do until a new input */
    PT_E_FATAL,     /* error: the agent stopped (see pt_fatal) */
    PT_E_TOOL_START,  /* name, args, call_id: a tool starts running */
    PT_E_TOOL_OUTPUT, /* data: output of a running tool */
    PT_E_ASK,         /* name, args, prompt: a tool call waits for pt_approve */
    PT_E_COMPACT      /* outcome: "started", "completed", "failed" (error): compaction */
} pt_event_kind;

typedef struct pt_event {
    pt_event_kind kind;
    uint32_t task;
    const pt_ai_event *ai;
    pt_str line;
    pt_str outcome; /* "completed", "failed" */
    pt_str error;   /* why, when something failed */
    int64_t until;
    pt_str name, args, call_id, prompt, data; /* tool events */
} pt_event;

/* Called from inside pt_poll and pt_fx_*. Must not call back into the
 * agent, except the read-only functions at the end of this header. */
typedef void (*pt_event_fn)(void *ud, const pt_event *ev);

/* ---- setup ------------------------------------------------------------------ */

/* Settings read live: they apply whenever a run next prepares a request. */
typedef struct pt_config {
    pt_str model; /* "provider/model-id" or "provider"; empty: the session's last */
    pt_str compact_model; /* the model that writes summaries; empty: model */
    pt_reasoning reasoning;
    uint32_t max_tokens;       /* 0: the model's maximum that fits the context */
    uint32_t retry_base_ms;    /* first backoff delay; 0: 1000 */
    uint32_t retry_max_ms;     /* backoff cap; 0: 300000 */
    uint32_t live_interval_ms; /* live file snapshots at most this often; 0: 250 */
    bool no_sync;              /* skip fsync: survives process crashes, not power loss */
    bool serial_tools;         /* run a round's tools one at a time even when all may run together */
    bool hide_thinking;        /* the frontend shows no reasoning text: requests leave it out */
    uint32_t context_window;   /* 0: the model's */
    uint32_t compact_reserve;  /* compact when fewer tokens are left; 0: 24576 (a quarter of small windows) */
    uint32_t compact_keep;     /* tokens of the newest turns a compaction keeps as they are;
                                * 0: 20000 (a fifth of small windows) */
} pt_config;

typedef struct pt_opts {
    pt_allocator alloc;
    int64_t (*now_ms)(void *ud); /* wall clock */
    void (*random)(void *ud, void *out, size_t n);
    /* The API key for a provider; false when there is none. The strings
     * must live as long as the agent. Never written anywhere. */
    bool (*credentials)(void *ud, const pt_provider *p, pt_credentials *out);
    pt_event_fn on_event;
    void *ud; /* passed to all of the above */
    /* A blob's contents ("sha256:<hex>"), into ar, while loading a log
     * whose large prompts are blobs. False when it cannot be read. */
    bool (*blob)(void *blob_ud, pt_str ref, pt_arena *ar, pt_str *out);
    void *blob_ud;
    const pt_ai_catalog *catalog; /* NULL: the built-in one */
    pt_config config;
    bool all_epochs; /* keep the entries of every epoch, not just the current one
                      * (for reading a log: such an agent must not run) */
} pt_opts;

pt_agent *pt_new(const pt_opts *o);
void pt_free(pt_agent *a);
/* Copied; strings are copied too. */
void pt_set_config(pt_agent *a, const pt_config *c);

/* ---- opening a session ----------------------------------------------------------
 * New session: pt_create, then pt_open. Existing session: feed the log with
 * pt_load_line, any live files with pt_load_live, then pt_load_done and
 * pt_open. Both queue commits; nothing else runs before they are done. */

/* A tool as the core knows it: frozen into each epoch. Running it is the
 * host's job (PT_FX_TOOL). */
typedef struct pt_tool_def {
    pt_str name, description;
    pt_str schema;   /* JSON Schema of the arguments, raw JSON */
    pt_replay replay;
    bool parallel;   /* may run at the same time as other parallel tools */
    bool read_only;  /* changes nothing: never asked about; its "path" is a file read */
    pt_str source;   /* who runs it: "builtin", ... */
} pt_tool_def;

typedef struct pt_session_cfg {
    pt_str cwd, host;
    pt_str system; /* the system prompt, frozen into the first epoch */
    const pt_tool_def *tools; /* ...with these tools */
    uint32_t ntools;
    pt_str skills; /* ...and this skills index: a JSON array, or empty */
    /* The host's own prompt settings, kept in the session header to
     * rebuild later epochs' system prompts from (pt_session_prompt): a
     * JSON object, or empty. */
    pt_str prompt;
    /* The first epoch's materials are settled later: before the first
     * request, the core asks for them (EPOCH_MATERIALS) and commits them,
     * so tools the host is still discovering (MCP servers) make it in.
     * Submitted inputs are committed meanwhile; their run waits. */
    bool provisional;
} pt_session_cfg;

bool pt_create(pt_agent *a, const pt_session_cfg *c);

typedef enum {
    PT_LOAD_OK,
    PT_LOAD_TORN,   /* truncate the log at the start of this line and stop */
    PT_LOAD_CORRUPT /* refuse to open: a bad line that is not the last */
} pt_load_result;

/* One line without its newline. complete: the line ended with a newline.
 * last: no more bytes follow it. */
pt_load_result pt_load_line(pt_agent *a, pt_str line, bool complete, bool last);
/* The live file of a task, from before a crash. */
void pt_load_live(pt_agent *a, uint32_t task, pt_str bytes);
/* False (see pt_error) when the log holds no valid session. */
bool pt_load_done(pt_agent *a);

typedef struct pt_identity {
    pt_str host; /* hostname or device ID */
    pt_str boot; /* boot ID, when the platform has one */
    int64_t pid;
} pt_identity;

/* Take ownership: commits an "open" claim. Refuses (false, see pt_error)
 * when the newest claim is an open one from another host, unless force. */
bool pt_open(pt_agent *a, const pt_identity *me, bool force);
/* Clean exit: commits a "close" claim and starts nothing new. Pending work
 * stays pending, for the next owner. pt_closed() says when it is done. */
void pt_close(pt_agent *a);
bool pt_closed(const pt_agent *a);

/* ---- work ----------------------------------------------------------------------- */

typedef struct pt_input {
    pt_str text;
    pt_str req; /* optional request ID: submitting it twice is a no-op */
    bool note;  /* pytte wrote it, not the user: its entry says "from":"pytte" */
} pt_input;

bool pt_submit(pt_agent *a, const pt_input *in);

/* Interrupt the active run (section 8.4): what has arrived of its answer
 * is committed as an aborted assistant entry, and the run and its input
 * end "aborted". Unlike exiting, this is a durable decision. Queued inputs
 * stay queued. False when no run is active. */
bool pt_abort(pt_agent *a);

/* Answer a tool call that waits for approval (PT_E_ASK). The answer is
 * committed, so it survives restarts. False when the task is not waiting. */
bool pt_approve(pt_agent *a, uint32_t task, bool yes, pt_str reason);

/* Compact now (section 12.3): summarize the current epoch into a new one.
 * False when a run is active or nothing in this epoch came from the model. */
bool pt_compact(pt_agent *a);

/* ---- the loop ---------------------------------------------------------------------- */

/* The next effect to start: 1 = *out is set, 0 = nothing right now. */
int pt_poll(pt_agent *a, pt_fx *out);
void pt_fx_head(pt_agent *a, uint32_t fx, int status, pt_str retry_after);
void pt_fx_data(pt_agent *a, uint32_t fx, const void *p, size_t n);
/* error: NULL on success. A failed COMMIT stops the agent for good. */
void pt_fx_done(pt_agent *a, uint32_t fx, const char *error);
/* Nothing to do, and nothing outstanding, until a new input. */
bool pt_idle(const pt_agent *a);

typedef enum { PT_GATE_ALLOW, PT_GATE_DENY, PT_GATE_ASK, PT_GATE_REWRITE } pt_gate_kind;

typedef struct pt_gate {
    pt_gate_kind kind;
    pt_str text; /* DENY: why; ASK: the question */
    pt_str args; /* REWRITE: the arguments to run with instead (a JSON object) */
} pt_gate;

/* The answer to a GATE. Strings are copied. */
void pt_fx_gate(pt_agent *a, uint32_t fx, const pt_gate *g);

typedef struct pt_tool_result {
    pt_str content;  /* what the model sees */
    bool is_error;
    pt_str details;  /* a JSON object for humans and tools (exit code, duration, blob), or empty */
    /* Optionally replace one doc in the same commit (a JSON value). */
    pt_str doc_key, doc_value;
    /* Optionally an image the model sees after content: a stored blob's
     * ref ("sha256:<hex>", durable before this call) and its MIME type. */
    pt_str image, image_mime;
} pt_tool_result;

/* A TOOL ended (also after PT_FX_CANCEL: with what it produced). Copied. */
void pt_fx_tool(pt_agent *a, uint32_t fx, const pt_tool_result *r);

/* What a new epoch freezes (section 12.3): built by the host when the
 * core asks. Copied. */
typedef struct pt_materials {
    pt_str system;
    const pt_tool_def *tools;
    uint32_t ntools;
    pt_str skills; /* a JSON array, or empty */
} pt_materials;

/* The answer to EPOCH_MATERIALS; NULL keeps the current epoch's. */
void pt_fx_materials(pt_agent *a, uint32_t fx, const pt_materials *m);

/* A BLOB is durable: ref is "sha256:<hex>" (copied), or empty when it
 * could not be stored (the bytes then stay inline in the log). */
void pt_fx_blob(pt_agent *a, uint32_t fx, pt_str ref);

typedef enum {
    PT_FATAL_NONE,
    PT_FATAL_STORAGE, /* a commit failed: the session is poisoned until reopened */
    PT_FATAL_CONFIG,  /* no API key, unknown model: fix and resume; nothing was lost */
    PT_FATAL_OOM
} pt_fatal_kind;

pt_fatal_kind pt_fatal(const pt_agent *a);
/* Why the agent stopped, or why the last call failed. */
const char *pt_error(const pt_agent *a);

/* ---- reading state ---------------------------------------------------------------- */

typedef struct pt_entry_view {
    uint32_t id, epoch;
    const char *kind;  /* epoch, user, assistant, tool_result, note, error */
    const pt_msg *msg; /* NULL for error entries */
    pt_str raw;        /* the record */
    pt_str summary;    /* epoch after a compaction: what the model continues from */
    pt_str files;      /* ...and the files read and modified, as text */
    uint32_t kept;     /* after a compaction: a copy of entry `kept` of the epoch
                        * before (already shown, if the viewer saw that epoch) */
} pt_entry_view;

/* Tool calls waiting for pt_approve, in task order; returns how many
 * (up to max are written). Strings live until the next commit applies. */
typedef struct pt_ask_view {
    uint32_t task;
    pt_str name, args, prompt;
} pt_ask_view;
uint32_t pt_asks(const pt_agent *a, pt_ask_view *out, uint32_t max);

/* A doc's value (raw JSON), or empty. Lives until the next commit applies. */
pt_str pt_doc(const pt_agent *a, pt_str key);
/* The working directory the session was started in. */
pt_str pt_session_cwd(const pt_agent *a);
/* The model the newest run asked for ("provider/model-id"); before the
 * first, the one the session was created with (pt_config.model), or
 * empty. Lives until the next commit applies. */
pt_str pt_session_model(const pt_agent *a);
/* The host's prompt settings from the session header (pt_session_cfg.prompt),
 * or empty. */
pt_str pt_session_prompt(const pt_agent *a);

/* About how many tokens the current epoch's context takes, as compaction
 * counts it: the newest answer's request and output as the provider
 * counted them, plus an estimate of what came after. Before any answer
 * that is the system prompt, tools and skills index, and what was sent. */
uint64_t pt_context_tokens(const pt_agent *a);

/* Entries of the current epoch, in order. */
uint32_t pt_entry_count(const pt_agent *a);
bool pt_entry_at(const pt_agent *a, uint32_t i, pt_entry_view *out);

#endif
