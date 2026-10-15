/* Internal interface of the core: in-memory state, the log codec, and what
 * the task handlers share. */
#ifndef PT_CORE_INTERNAL_H
#define PT_CORE_INTERNAL_H

#include "pytte.h"

/* ---- log lines (log.c) ---- */

/* {"seq":N,"ts":T,"w":[ ... records ... ],"crc":"xxxxxxxx"}\n
 * pt_log_begin writes up to the open array; the caller writes the records
 * with w; pt_log_end closes it with the checksum and the newline. */
void pt_log_begin(pt_buf *b, pt_jw *w, uint64_t seq, int64_t ts);
void pt_log_end(pt_buf *b, pt_jw *w);
/* Checks a line (without newline): checksum, JSON, fields. Returns the
 * parsed line, or NULL. */
const pt_json *pt_log_parse(pt_str line, uint64_t *seq, pt_arena *a);

/* ---- records ---- */

typedef enum { ENT_EPOCH, ENT_USER, ENT_ASSISTANT, ENT_TOOL_RESULT, ENT_NOTE, ENT_ERROR } ent_kind;

typedef struct entry {
    uint32_t id, epoch;
    ent_kind kind;
    pt_msg msg; /* epoch: the system prompt; notes: role system */
    pt_str summary, request; /* epoch after compaction: the summary, the request being worked on */
    pt_str files;            /* epoch after compaction: the files read and modified, as text */
    bool to_model;
    uint32_t kept; /* a copy of this entry of the epoch before (section 12.3) */
    bool provisional; /* epoch: its materials are settled before the first request */
    pt_str raw;
} entry;

typedef enum { TASK_RUN, TASK_TOOL, TASK_COMPACT } task_kind;
typedef enum { ST_PENDING, ST_RUNNING, ST_WAITING, ST_DONE } task_status;
/* run: prepare, request, retry, tools, compact_wait; tool: call, approve,
 * execute; compact: prepare, request, retry */
typedef enum {
    PH_PREPARE, PH_REQUEST, PH_RETRY, PH_TOOLS, PH_CALL, PH_APPROVE, PH_EXECUTE, PH_COMPACT
} task_phase;

typedef struct run_live run_live;
typedef struct tool_live tool_live;

/* A request in flight (run.c): the run's, or a compaction's. */
struct run_live {
    pt_agent *agent;
    uint32_t task;
    bool quiet;  /* a compaction: no stream events, no live file */
    pt_arena a;  /* request and final message */
    pt_ai_target target;
    pt_http_req req;
    pt_ai_stream *s;
    uint32_t http_fx;
    bool http_done, finished, cancel_delivered;
    struct run_live *pending_next;
    int64_t last_live;
    bool dirty; /* content arrived since the last snapshot */
    pt_buf snap;
};

typedef struct task {
    /* durable: the newest task record */
    uint32_t id, input, owner; /* owner: the run a tool or compact task belongs to */
    task_kind kind;
    task_status status;
    task_phase phase;
    uint32_t attempt, cutoff;
    uint32_t keep;      /* compact: the first entry kept as it is; 0: none */
    int64_t until;                                     /* retry */
    pt_str model; pt_reasoning reasoning; uint32_t max_tokens; /* request params */
    uint32_t entry;     /* run tools, tool: the assistant entry with the calls */
    uint32_t *calls, ncalls; /* run tools: its tool tasks, in call order */
    bool *parallel;     /* run tools: per call, whether it may run alongside others */
    pt_str call_id;     /* tool */
    pt_str args;        /* tool: arguments a gate rewrote; empty: as called */
    pt_str prompt;      /* tool approve */
    pt_replay replay;   /* tool execute */
    uint32_t compact;   /* run compact_wait: the compact task */
    bool abort;         /* an abort was requested (section 8.4) */
    pt_str outcome, reason; uint32_t result;           /* done */
    pt_arena a; /* owns the strings above */
    /* this process only */
    uint32_t fx;    /* outstanding commit, sleep, gate or tool effect */
    run_live *live; /* request in flight */
    tool_live *tl;  /* tool started in this process */
    pt_buf rescued; /* live file written before a crash */
    bool woke;      /* the retry sleep is over */
    bool recovered; /* the record was loaded, not committed by this process */
    /* A loaded tool intent: whether its owner closed the session (quit)
     * before anyone opened it again (reopened), or crashed. */
    bool quit, reopened;
    bool asked;     /* approve: the question went out */
    bool gated;     /* call: the gate answered (gate, gate_text) */
    pt_gate_kind gate;
    pt_buf gate_text;
    struct task *next_free;
} task;

typedef enum { IN_QUEUED, IN_PLACED, IN_DONE, IN_FAILED, IN_ABORTED } input_status;
typedef enum { INK_USER, INK_APPROVE } input_kind;

typedef struct input {
    uint32_t id, run;
    input_kind kind;
    input_status status;
    uint32_t task; /* approve: the tool task */
    bool yes;      /* approve: the answer */
    bool note;     /* user: from pytte, not the user */
    pt_str req, text; /* approve: text is the reason */
    pt_str blob;      /* "sha256:…" when the log holds text as a blob */
    pt_arena a;
} input;

typedef struct doc {
    uint32_t id;
    pt_str key;  /* in perm */
    pt_buf value; /* raw JSON, replaced whole */
} doc;

/* The epoch's frozen tools: what the model is offered, and how they run. */
typedef struct tool_meta {
    pt_replay replay;
    bool parallel, read_only;
    const pt_json *schema; /* parsed, in the epoch arena */
} tool_meta;

typedef struct claim {
    uint32_t id;
    pt_str event, token, host, boot;
    int64_t pid, opened;
} claim;

/* Prompts this long are stored once, as a blob, and the log's records
 * refer to it (section 6.5): the input record is rewritten as it is
 * placed and finished, and the user entry repeats it. */
#define PT_BLOB_MIN 16384

typedef struct sub { /* a submission waiting for its commit */
    input_kind kind;
    pt_str text, req;
    uint32_t task; /* approve */
    bool yes;
    bool note;        /* from pytte, not the user */
    pt_str blob;      /* the stored text's ref, once PT_FX_BLOB completed */
    uint32_t blob_fx; /* the PT_FX_BLOB in flight */
    bool blob_tried;  /* done storing (blob empty: it failed; keep the text inline) */
} sub;

#define PT_FXQ 32

typedef struct queued_fx {
    pt_fx fx;
    pt_buf bytes; /* LIVE snapshots */
} queued_fx;

typedef enum { AFTER_NONE, AFTER_RETRY, AFTER_RUN_DONE, AFTER_COMPACT } after_kind;

/* The session writes the host requested, applied at the next step. */
typedef struct session_reqs {
    bool create, open, close;
    bool compact;       /* pt_compact: commit a compact task when no run is active */
    pt_session_cfg cfg; /* create's */
    pt_arena ar;        /* the strings of cfg, and of claims (below) */
} session_reqs;

/* Our claim on the session (section 6.8), and the newest claim seen. */
typedef struct claims {
    claim newest;    /* the newest claim record */
    uint32_t mine;   /* our claim's ID, once open */
    pt_identity me;  /* strings in req.ar */
    pt_str token, takeover;
    int64_t opened;
} claims;

/* The next epoch's materials (PT_FX_EPOCH_MATERIALS), for the compact
 * task `task`: asked for once its request is about to start. */
typedef struct epoch_mat {
    uint32_t task, fx;
    bool ready, keep; /* keep: the host had nothing newer */
    pt_buf system, tools, skills; /* tools, skills: JSON arrays, as frozen */
} epoch_mat;

/* Submissions waiting for their commit. */
typedef struct sub_list {
    sub *v;
    uint32_t n, cap;
    pt_arena ar;
} sub_list;

/* The commit in flight (at most one), and what happens when it lands. */
typedef struct commit_flight {
    pt_buf buf;
    pt_jw w; /* its records are written with this */
    uint32_t fx, task;
    bool out, close;
    uint32_t live_del; /* delete this task's live file when it lands */
    after_kind after;
    pt_str outcome, error; /* in ar */
    int64_t until;
    pt_arena ar;
} commit_flight;

/* Effects waiting for the host to take them (pt_poll). */
typedef struct fx_queue {
    queued_fx v[PT_FXQ];
    uint32_t head, len;
    uint32_t next; /* the next effect ID */
} fx_queue;

struct pt_agent {
    pt_opts o;
    pt_allocator al;
    pt_arena perm;    /* config strings, session header, claims */
    pt_arena epoch;   /* entries */
    pt_arena scratch; /* one step */
    pt_arena objects; /* task/input structs, until the agent is freed */

    entry *entries;
    uint32_t nentries, ecap;
    task **tasks;
    uint32_t ntasks, tcap;
    input **inputs;
    uint32_t ninputs, icap;
    pt_tool *tools;  /* the current epoch's, in the epoch arena */
    tool_meta *tool_meta;
    uint32_t ntools;
    doc *docs;
    uint32_t ndocs, dcap;
    pt_str cwd;      /* from the session header (perm) */
    pt_str prompt;   /* the host's prompt settings, from the header (perm) */
    task *free_tasks; /* finished tool tasks, kept for reuse */
    uint32_t next_id;
    uint64_t seq;    /* of the last applied commit */
    uint32_t cur_epoch;
    bool have_session, loaded;
    bool closing, closed;
    pt_str last_model; /* the newest model a run pinned, else the session's first (in perm) */

    session_reqs req;
    claims claims;
    epoch_mat mat;
    sub_list subs;
    commit_flight commit;
    fx_queue fxq;
    run_live *pending_http_free; /* requests retained until their cancel is delivered */
    pt_buf live_out; /* the LIVE snapshot handed out by pt_poll */

    uint32_t abort_run; /* pt_abort: the run to stop */

    pt_fatal_kind fatal;
    char err[512];
    bool idle_sent;
};

/* ---- agent.c ---- */
void pt_core_fail(pt_agent *a, pt_fatal_kind k, const char *msg, pt_str detail);
void pt_core_seterr(pt_agent *a, const char *msg, pt_str detail);
bool pt_core_queue(pt_agent *a, const pt_fx *fx, pt_str bytes);
void pt_core_emit(pt_agent *a, const pt_event *ev);
int64_t pt_core_now(pt_agent *a);
uint32_t pt_core_new_id(pt_agent *a);
uint32_t pt_core_new_fx(pt_agent *a);
/* Grow an array allocated with the agent's allocator to hold need items. */
bool pt_core_grow(pt_agent *a, void **arr, uint32_t *cap, uint32_t need, size_t elem);
/* Start a commit on behalf of task (0 = session); records are written
 * with a->commit.w; pt_core_commit_end queues it. */
void pt_core_commit_begin(pt_agent *a, uint32_t task);
bool pt_core_commit_end(pt_agent *a);
task *pt_core_task(pt_agent *a, uint32_t id);
input *pt_core_input(pt_agent *a, uint32_t id);
entry *pt_core_last_entry(pt_agent *a);
/* An entry record in the current epoch: write its data fields with a->commit.w
 * between these two. */
void pt_core_entry_begin(pt_agent *a, uint32_t id, const char *kind);
void pt_core_entry_end(pt_agent *a);
/* ...in a given epoch (a new epoch's own entry). */
void pt_core_entry_head(pt_agent *a, uint32_t id, const char *kind, uint32_t epoch);

/* Entry by ID in the current epoch, or NULL. */
entry *pt_core_entry(pt_agent *a, uint32_t id);
/* A doc record in the commit being written; value is valid JSON. */
void pt_core_write_doc(pt_agent *a, pt_str key, pt_str value);
/* ...or write the value with a->commit.w, then pt_jw_obj_end(&a->commit.w). */
void pt_core_doc_begin(pt_agent *a, pt_str key);
/* Remember what to tell the host after the commit lands. */
void pt_core_after(pt_agent *a, after_kind k, const char *outcome, pt_str error, int64_t until);
/* Queue a cancel of effect fx_id, on behalf of a task. */
void pt_core_cancel(pt_agent *a, uint32_t task_id, uint32_t fx_id);
/* Queue a delete of a task's live file. */
void pt_core_live_del(pt_agent *a, uint32_t task_id);

/* ---- state.c: records ---- */
bool pt_state_apply(pt_agent *a, const pt_json *line);
void pt_write_task(pt_jw *w, const task *t);
void pt_write_input(pt_jw *w, const input *in);
const char *pt_input_status_name(input_status s);
void pt_task_release(pt_agent *a, task *t); /* free its process-only state */

/* ---- context.c ---- */
/* The current epoch through entry `cutoff`, as a unified context. */
bool pt_core_context(pt_agent *a, uint32_t cutoff, pt_ai_context *out, pt_arena *ar);
bool pt_core_sent(const entry *e); /* part of the requests */
int pt_core_find_tool(pt_agent *a, pt_str name); /* in the current epoch; -1: none */
uint64_t pt_core_sent_bytes(const pt_agent *a, const entry *e);
uint64_t pt_core_estimate(const pt_agent *a, uint32_t cutoff);

/* ---- run.c ---- */
/* Advance a run task; true when it queued something. */
bool pt_run_step(pt_agent *a, task *t);
void pt_run_free_live(pt_agent *a, task *t);
void pt_run_cancel_delivered(pt_agent *a, uint32_t cancelled_fx);
void pt_run_free_pending(pt_agent *a);
void pt_run_poll_begin(pt_agent *a);
/* Commit the end of an interrupted run; true when it queued the commit.
 * False when the answer is complete: it is kept, and the abort waits for
 * what comes after it (the run may be done then). */
bool pt_run_abort(pt_agent *a, task *t);
/* HTTP progress of a request; false when fx is not one in flight. */
bool pt_run_http_head(pt_agent *a, uint32_t fx, int status, pt_str retry_after);
bool pt_run_http_data(pt_agent *a, uint32_t fx, const void *p, size_t n);
bool pt_run_http_done(pt_agent *a, uint32_t fx, const char *error);
/* A tool round is done: begin the next request (or end an aborted run). */
void pt_run_write_tool_error(pt_agent *a, uint32_t id, pt_str call_id, pt_str name, pt_str text);
void pt_run_write_assistant(pt_agent *a, uint32_t id, const pt_msg *m);

/* Shared with task_compact.c: the model, its key, the backoff, and requests. */
bool pt_run_resolve(pt_agent *a, pt_str ref, pt_ai_target *out, pt_arena *ar);
bool pt_run_credentials(pt_agent *a, const pt_provider *p, pt_credentials *cred);
int64_t pt_run_backoff(pt_agent *a, uint32_t attempt, int64_t retry_after_ms);
/* Send t's request (t->live): the context through t->cutoff, or for a
 * compact task its summarization request. */
bool pt_run_start(pt_agent *a, task *t);
/* The retry sleep of a run or compact task in PH_RETRY. */
bool pt_run_sleep(pt_agent *a, task *t);
void pt_run_write_error(pt_agent *a, uint32_t attempt, const pt_ai_error *e, int64_t retry_at);
/* Record the tokens of a message in the usage doc (in the open commit). */
void pt_run_add_usage(pt_agent *a, const pt_msg *m, pt_str model);

/* The tools array of an epoch entry (agent.c). */
void pt_core_write_tool_defs(pt_jw *w, const pt_tool_def *defs, uint32_t n);
/* Empty raw, or JSON of that type (parsed in scratch). */
bool pt_core_raw_is(pt_agent *a, pt_str raw, pt_jtype kind);

/* ---- task_compact.c ---- */
/* True when the epoch holds an answer from the model, so a summary can
 * make it smaller (and compaction cannot repeat without progress). */
bool pt_compact_possible(pt_agent *a);
/* Write a new compact task into the open commit; returns its ID. */
uint32_t pt_compact_write(pt_agent *a, uint32_t owner, uint32_t cutoff);
bool pt_compact_step(pt_agent *a, task *t);
/* The summarization request of a compact task. */
bool pt_compact_context(pt_agent *a, const task *t, pt_ai_context *out, pt_arena *ar);
/* The request of a compact task finished (t->live->finished). */
bool pt_compact_finish(pt_agent *a, task *t);
/* A provisional epoch (pt_session_cfg.provisional) asks the host for its
 * materials, then commits them as a "materials" record: true when it
 * queued something. Run t waits in prepare meanwhile. */
bool pt_compact_settle(pt_agent *a, task *t);
/* Abort a compaction: done(aborted) into the open commit; returns the
 * effect to cancel after the commit is queued, or 0. */
uint32_t pt_compact_abort(pt_agent *a, task *t);

/* ---- task_tool.c ---- */
bool pt_tool_step(pt_agent *a, task *t);
void pt_tool_free_live(pt_agent *a, task *t);
/* Commit an "aborted" result now, or mark the task so its own commit
 * says so: written into the run's abort commit. Returns the effect to
 * cancel once the commit is queued, or 0. */
uint32_t pt_tool_abort(pt_agent *a, task *t);
bool pt_tool_data(pt_agent *a, uint32_t fx, const void *p, size_t n);
bool pt_tool_finished(const task *t); /* its effect ended (t->tl set) */
uint32_t pt_tool_fx(const task *t);

#endif
