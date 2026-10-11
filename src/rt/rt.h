/* The runtime: the reference host loop. Turns the core's effects into
 * driver calls (store, HTTP, timers) and feeds the results back. Used by
 * the CLI and by the simulation tests (with the memory store, the faux
 * HTTP driver and a fake clock). */
#ifndef PT_RT_H
#define PT_RT_H

#include "drv/drv.h"
#include "pytte.h"

typedef struct pt_rt pt_rt;

typedef struct pt_rt_cfg {
    pt_store *store;
    pt_http_driver *http;
    /* The clock; NULL: the system's. Must match the agent's now_ms. */
    int64_t (*now)(void *ud);
    void (*sleep)(void *ud, uint32_t ms);
    void *clock_ud;
    /* Polled between effects; true stops pt_rt_run. */
    bool (*stop)(void *ud);
    void *stop_ud;
} pt_rt_cfg;

pt_rt *pt_rt_new(const pt_rt_cfg *c, pt_agent *a);
/* Cancels outstanding requests and closes the store. The HTTP driver
 * stays the caller's to free (after this). */
void pt_rt_free(pt_rt *rt);
pt_http_driver *pt_rt_http(const pt_rt *rt);
/* Borrow an idle worker to perform durable writes/live snapshots. I/O
 * completion and every agent callback remain on the calling thread.
 * Configure between steps, before scheduling effects. The worker must
 * outlive rt; free/close finish any borrowed in-flight effect first. */
void pt_rt_use_worker(pt_rt *rt, pt_worker *worker);

/* Recovery (section 6.4): read the log into the agent, cut a torn tail,
 * hand over live files. False with err when the log is corrupt. */
bool pt_rt_recover(pt_rt *rt, char *err, size_t cap);

/* ---- tools (section 13) ----
 * The runtime keeps a registry of tools and answers the core's GATE and
 * TOOL effects. A tool's run starts a call; it reports output and its end
 * with pt_rt_tool_output and pt_rt_tool_done, at once or later from a
 * timer or an fd watch. */
typedef struct pt_rt_tool {
    pt_tool_def def;
    bool read_only; /* changes nothing: the gate never asks about it */
    void (*run)(void *ud, pt_rt *rt, const pt_fx *call);
    /* Stop a running call (abort, exit). It must still end with
     * pt_rt_tool_done, now or later. NULL: calls finish inside run. */
    void (*cancel)(void *ud, pt_rt *rt, uint32_t fx);
    void *ud;
} pt_rt_tool;

/* Copied (strings must outlive rt). False when out of memory. */
bool pt_rt_tool_add(pt_rt *rt, const pt_rt_tool *t);
/* bash, read, write, edit (tools_builtin.c). */
bool pt_rt_add_builtins(pt_rt *rt);
/* The registered tools, for a new session's epoch. */
const pt_tool_def *pt_rt_tool_defs(pt_rt *rt, uint32_t *n);

/* Output as it comes: shown live, kept for the result. */
void pt_rt_tool_output(pt_rt *rt, uint32_t fx, const void *p, size_t n);

typedef struct pt_rt_out {
    /* The text for the model. NULL p: the output reported so far, with
     * note appended. Long content is cut to its head and tail. */
    pt_str content, note;
    bool is_error;
    pt_str details; /* a JSON object, or empty */
    pt_str doc_key, doc_value;
} pt_rt_out;

void pt_rt_tool_done(pt_rt *rt, uint32_t fx, const pt_rt_out *r);
/* The agent the runtime drives (for a tool's lookups: pt_doc, the cwd). */
pt_agent *pt_rt_agent(pt_rt *rt);
/* pt_opts.blob for a store (blob_ud: the pt_store). */
bool pt_rt_read_blob(void *store, pt_str ref, pt_arena *ar, pt_str *out);

/* Event-loop hooks for calls that end later. A timer fires once at `at`
 * (wall-clock ms). A watch fires whenever fd is readable, until removed.
 * Callbacks may add and remove timers and watches. */
typedef void (*pt_rt_fn)(void *ud, pt_rt *rt);
uint32_t pt_rt_timer(pt_rt *rt, int64_t at, pt_rt_fn fn, void *ud);
void pt_rt_timer_cancel(pt_rt *rt, uint32_t id);
bool pt_rt_watch(pt_rt *rt, int fd, pt_rt_fn fn, void *ud);
void pt_rt_unwatch(pt_rt *rt, int fd);
int64_t pt_rt_now(pt_rt *rt);

/* ---- permissions (section 15.5) ---- */
typedef enum {
    PT_PERM_ASK,      /* tools that change things ask first */
    PT_PERM_AUTO,     /* everything runs */
    PT_PERM_READ_ONLY /* tools that change things are denied */
} pt_perm_mode;

bool pt_perm_parse(pt_str s, pt_perm_mode *out);
const char *pt_perm_name(pt_perm_mode m);
void pt_rt_set_perm(pt_rt *rt, pt_perm_mode mode);
/* Allow a tool from now on in this process: calls whose gist (the first
 * string argument) starts with prefix as a word, or every call when prefix
 * is empty. */
bool pt_rt_allow(pt_rt *rt, pt_str tool, pt_str prefix);
/* The first string argument of a call ("ls -la", "src/a.c"), or empty;
 * allocated in a. */
pt_str pt_rt_gist(pt_str args, pt_arena *a);

typedef enum {
    PT_RT_IDLE,    /* nothing left to do */
    PT_RT_STOPPED, /* cfg.stop said so */
    PT_RT_FATAL,   /* see pt_fatal / pt_error */
    PT_RT_STUCK,   /* nothing outstanding, yet not idle: a bug */
    PT_RT_BUSY,    /* pt_rt_step: work is outstanding */
    PT_RT_ASK      /* nothing can go on until a tool call is approved (pt_asks) */
} pt_rt_result;

/* Until idle, fatal or stopped. */
pt_rt_result pt_rt_run(pt_rt *rt);
/* One bounded step, for hosts that wait on more than the agent (a TUI):
 * start what the agent hands out, then wait up to timeout_ms (-1: no
 * limit) for HTTP progress, the next sleeper or one of fds (marked
 * ready), and start what that produced. When the agent handed out work
 * the step only looks (timeout 0), so the host shows that work at once.
 * Waits on fds even when idle or fatal; cfg.stop is not consulted. */
pt_rt_result pt_rt_step(pt_rt *rt, int timeout_ms, pt_waitfd *fds, unsigned nfds);
/* Clean exit: drop outstanding requests and commit the close claim.
 * Pending work stays pending for the next owner. */
bool pt_rt_close(pt_rt *rt);

#endif
