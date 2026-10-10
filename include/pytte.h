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
    PT_FX_CANCEL      /* stop effect `cancel` (an HTTP request); no completion */
} pt_fx_kind;

typedef struct pt_fx {
    pt_fx_kind kind;
    uint32_t id;   /* unique within the agent */
    uint32_t task; /* the task it serves; 0 for session commits */
    /* COMMIT: one line with its newline, valid until completed.
     * LIVE: the snapshot, valid until the next pt_poll. */
    pt_str bytes;
    bool sync;                /* COMMIT */
    const pt_http_req *http;  /* HTTP: valid until completed or cancelled */
    int64_t until;            /* SLEEP: wall-clock ms */
    uint32_t cancel;          /* CANCEL */
} pt_fx;

/* ---- events (display only; no reply) ----------------------------------------- */

typedef enum {
    PT_E_STREAM,    /* ai: a stream event of a running request */
    PT_E_COMMITTED, /* line: a commit was applied (the JSON, without newline) */
    PT_E_RETRY,     /* error, until: the run waits to try again */
    PT_E_RUN_DONE,  /* outcome, error: a run finished */
    PT_E_IDLE,      /* nothing left to do until a new input */
    PT_E_FATAL      /* error: the agent stopped (see pt_fatal) */
} pt_event_kind;

typedef struct pt_event {
    pt_event_kind kind;
    uint32_t task;
    const pt_ai_event *ai;
    pt_str line;
    pt_str outcome; /* "completed", "failed" */
    pt_str error;   /* why, when something failed */
    int64_t until;
} pt_event;

/* Called from inside pt_poll and pt_fx_*. Must not call back into the
 * agent, except the read-only functions at the end of this header. */
typedef void (*pt_event_fn)(void *ud, const pt_event *ev);

/* ---- setup ------------------------------------------------------------------ */

/* Settings read live: they apply whenever a run next prepares a request. */
typedef struct pt_config {
    pt_str model; /* "provider/model-id" or "provider"; empty: the session's last */
    pt_reasoning reasoning;
    uint32_t max_tokens;       /* 0: the model's maximum that fits the context */
    uint32_t retry_base_ms;    /* first backoff delay; 0: 1000 */
    uint32_t retry_max_ms;     /* backoff cap; 0: 300000 */
    uint32_t live_interval_ms; /* live file snapshots at most this often; 0: 250 */
    bool no_sync;              /* skip fsync: survives process crashes, not power loss */
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
    const pt_ai_catalog *catalog; /* NULL: the built-in one */
    pt_config config;
} pt_opts;

pt_agent *pt_new(const pt_opts *o);
void pt_free(pt_agent *a);
/* Copied; strings are copied too. */
void pt_set_config(pt_agent *a, const pt_config *c);

/* ---- opening a session ----------------------------------------------------------
 * New session: pt_create, then pt_open. Existing session: feed the log with
 * pt_load_line, any live files with pt_load_live, then pt_load_done and
 * pt_open. Both queue commits; nothing else runs before they are done. */

typedef struct pt_session_cfg {
    pt_str cwd, host;
    pt_str system; /* the system prompt, frozen into the first epoch */
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
} pt_input;

bool pt_submit(pt_agent *a, const pt_input *in);

/* ---- the loop ---------------------------------------------------------------------- */

/* The next effect to start: 1 = *out is set, 0 = nothing right now. */
int pt_poll(pt_agent *a, pt_fx *out);
void pt_fx_head(pt_agent *a, uint32_t fx, int status, pt_str retry_after);
void pt_fx_data(pt_agent *a, uint32_t fx, const void *p, size_t n);
/* error: NULL on success. A failed COMMIT stops the agent for good. */
void pt_fx_done(pt_agent *a, uint32_t fx, const char *error);
/* Nothing to do, and nothing outstanding, until a new input. */
bool pt_idle(const pt_agent *a);

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
} pt_entry_view;

/* Entries of the current epoch, in order. */
uint32_t pt_entry_count(const pt_agent *a);
bool pt_entry_at(const pt_agent *a, uint32_t i, pt_entry_view *out);

#endif
