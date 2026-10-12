/* Inside the runtime: shared by rt.c (the loop), tools.c (the registry,
 * calls and the gate) and tools_builtin.c. */
#ifndef PT_RT_INT_H
#define PT_RT_INT_H

#include "rt/rt.h"

/* Tool output kept for the result (section 12.2): all of it up to OUT_CAP,
 * else the first OUT_HEAD and the last OUT_TAIL bytes. */
#define OUT_CAP 16384
#define OUT_HEAD 4096
#define OUT_TAIL 12288

typedef struct rt_req {
    pt_rt *rt;
    uint32_t fx;
    void *h;
    bool done;
} rt_req;

typedef struct rt_sleeper {
    uint32_t fx;
    int64_t until;
} rt_sleeper;

typedef struct rt_timer {
    uint32_t id;
    int64_t at;
    pt_rt_fn fn;
    void *ud;
} rt_timer;

typedef struct rt_watch {
    int fd;
    uint64_t generation;
    pt_rt_fn fn;
    void *ud;
} rt_watch;

/* A running tool call. Slots past ncalls keep their buffers for reuse. */
typedef struct rt_call {
    uint32_t fx, tool;
    int64_t started;
    pt_buf head, tail;
    uint64_t total;
    /* Output past OUT_CAP also goes, whole, to a blob (section 12.2). */
    void *blob;
    bool nospill; /* the store keeps no blobs, or writing one failed */
    bool output_oom; /* head/tail capture was incomplete */
    bool cancel_requested;
    pt_sha256 sha;
} rt_call;

typedef struct rt_rule {
    char *tool, *prefix;
} rt_rule;

struct pt_rt {
    pt_rt_cfg c;
    pt_agent *a;
    uint64_t log_end; /* where our last commit ended (section 6.8.4) */
    rt_req **reqs;
    uint32_t nreqs, rcap;
    rt_sleeper *sleeps;
    uint32_t nsleeps, scap;
    pt_worker *worker; /* borrowed; NULL keeps the blocking host behavior */
    pt_fx io;          /* borrowed effect bytes until the worker finishes */
    bool io_pending, io_ok;
    char io_error[512];
    char io_blob[72]; /* a stored BLOB's ref: "sha256:<hex>" */
    /* tools */
    pt_rt_tool *tools;
    pt_tool_def *defs;
    uint32_t ntools, tcap;
    rt_call *calls;
    uint32_t ncalls, ccap, cinit;
    rt_timer *timers;
    uint32_t ntimers, tmcap, next_timer;
    rt_watch *watches;
    uint32_t nwatches, wcap;
    uint64_t watch_generation;
    pt_perm_mode perm;
    rt_rule *rules;
    uint32_t nrules, rucap;
    bool closing, free_waiting;
    void *builtin; /* tools_builtin.c's state */
    void (*builtin_free)(void *ud);
    /* The agent is idle: true when it submitted something (a note that
     * background commands ended), so there is work again. */
    bool (*builtin_idle)(void *ud);
    /* prompt.c */
    pt_rt_prompt prompt;
    bool have_prompt;
    pt_arena prompt_ar; /* the strings of prompt, kept */
    pt_rt_skill *skills;
    uint32_t nskills, skcap;
    pt_arena skills_ar; /* their strings, until the next scan */
    pt_buf scratch; /* tool results, reused */
    pt_arena ar;    /* one gate or result at a time */
};

/* tools.c */
void pt_rt_gate(pt_rt *rt, const pt_fx *fx);
void pt_rt_run_tool(pt_rt *rt, const pt_fx *fx);
void pt_rt_cancel_tool(pt_rt *rt, uint32_t fx);
void pt_rt_tools_stop(pt_rt *rt); /* closing: stop running calls */
void pt_rt_tools_free(pt_rt *rt);
/* tools_search.c: grep, find and ls, with their state */
typedef struct search search;
search *pt_rt_search_new(pt_rt *rt);
void pt_rt_search_free(search *s);
void pt_rt_search_tools(search *s, pt_rt_tool t[3]);
/* prompt.c */
void pt_rt_prompt_free(pt_rt *rt);
/* Grow a malloc'd array to hold need items (doubling). */
bool pt_rt_grow(void **arr, uint32_t *cap, uint32_t need, size_t elem);

#endif
