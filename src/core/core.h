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
    bool to_model;
    pt_str raw;
} entry;

typedef enum { TASK_RUN } task_kind;
typedef enum { ST_PENDING, ST_RUNNING, ST_WAITING, ST_DONE } task_status;
typedef enum { PH_PREPARE, PH_REQUEST, PH_RETRY } run_phase;

typedef struct run_live run_live;

typedef struct task {
    /* durable: the newest task record */
    uint32_t id, input;
    task_kind kind;
    task_status status;
    run_phase phase;
    uint32_t attempt, cutoff;
    int64_t until;                                     /* retry */
    pt_str model; pt_reasoning reasoning; uint32_t max_tokens; /* request params */
    pt_str outcome, reason; uint32_t result;           /* done */
    pt_arena a; /* owns the strings above */
    /* this process only */
    uint32_t fx;    /* outstanding commit or sleep; the HTTP request is in live */
    run_live *live; /* request in flight */
    pt_buf rescued; /* partial message from a live file written before a crash */
    bool woke;      /* the retry sleep is over */
} task;

typedef enum { IN_QUEUED, IN_PLACED, IN_DONE, IN_FAILED, IN_ABORTED } input_status;

typedef struct input {
    uint32_t id, run;
    input_status status;
    pt_str req, text;
    pt_arena a;
} input;

typedef struct claim {
    uint32_t id;
    pt_str event, token, host, boot;
    int64_t pid, opened;
} claim;

typedef struct sub { /* a submission waiting for its commit */
    pt_str text, req;
} sub;

#define PT_FXQ 32

typedef struct queued_fx {
    pt_fx fx;
    pt_buf bytes; /* LIVE snapshots */
} queued_fx;

typedef enum { AFTER_NONE, AFTER_RETRY, AFTER_RUN_DONE } after_kind;

struct pt_agent {
    pt_opts o;
    pt_allocator al;
    pt_arena perm;    /* config strings, session header, claims */
    pt_arena epoch;   /* entries */
    pt_arena scratch; /* one step */

    entry *entries;
    uint32_t nentries, ecap;
    task **tasks;
    uint32_t ntasks, tcap;
    input **inputs;
    uint32_t ninputs, icap;
    claim newest;    /* the newest claim record */
    uint32_t mine;   /* our claim's ID, once open */
    uint32_t next_id;
    uint64_t seq;    /* of the last applied commit */
    uint32_t cur_epoch;
    bool have_session, loaded;
    pt_str last_model; /* the newest model a run pinned (in perm) */

    /* requested session writes */
    bool want_create, want_open, want_close, closing, closed;
    pt_session_cfg create;
    pt_identity me;
    pt_str my_token, takeover;
    int64_t my_opened;
    pt_arena reqs; /* their strings */

    sub *subs;
    uint32_t nsubs, scap;
    pt_arena subs_a;

    /* the commit in flight (at most one) */
    pt_buf commit;
    pt_jw cw;
    uint32_t commit_fx, commit_task;
    bool commit_out, commit_close;
    uint32_t after_live_del; /* delete this task's live file when it lands */
    after_kind after;
    pt_str after_outcome, after_error; /* in after_a */
    int64_t after_until;
    pt_arena after_a;

    queued_fx q[PT_FXQ];
    uint32_t qhead, qlen;
    uint32_t next_fx;
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
 * with a->cw; pt_core_commit_end queues it. */
void pt_core_commit_begin(pt_agent *a, uint32_t task);
bool pt_core_commit_end(pt_agent *a);
task *pt_core_task(pt_agent *a, uint32_t id);
input *pt_core_input(pt_agent *a, uint32_t id);
entry *pt_core_last_entry(pt_agent *a);
/* An entry record in the current epoch: write its data fields with a->cw
 * between these two. */
void pt_core_entry_begin(pt_agent *a, uint32_t id, const char *kind);
void pt_core_entry_end(pt_agent *a);

/* ---- state.c: records ---- */
bool pt_state_apply(pt_agent *a, const pt_json *line);
void pt_write_task(pt_jw *w, const task *t);
void pt_write_input(pt_jw *w, const input *in);
const char *pt_input_status_name(input_status s);

/* ---- context.c ---- */
/* The current epoch through entry `cutoff`, as a unified context. */
bool pt_core_context(pt_agent *a, uint32_t cutoff, pt_ai_context *out, pt_arena *ar);

/* ---- run.c ---- */
/* Advance a run task; true when it queued something. */
bool pt_run_step(pt_agent *a, task *t);
void pt_run_free_live(pt_agent *a, task *t);
/* Commit the end of an interrupted run; true when it queued the commit.
 * False when the answer is complete: it is kept, and the abort waits for
 * what comes after it (the run may be done then). */
bool pt_run_abort(pt_agent *a, task *t);
/* HTTP progress of a request; false when fx is not one in flight. */
bool pt_run_http_head(pt_agent *a, uint32_t fx, int status, pt_str retry_after);
bool pt_run_http_data(pt_agent *a, uint32_t fx, const void *p, size_t n);
bool pt_run_http_done(pt_agent *a, uint32_t fx, const char *error);

#endif
