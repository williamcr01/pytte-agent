/* Inside the runtime: shared by rt.c (the loop), tools.c (the registry,
 * calls and the gate), prompt.c, mcp*.c and the builtin tools
 * (tools_builtin.c, tools_bash.c, tools_search.c, tools_web.c, tools_websearch.c). */
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

/* rt_call.tool of a call the fallback runs */
#define RT_FALLBACK UINT32_MAX

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

/* A permission rule (section 15.5): spec is "tool" or "tool(pattern)",
 * tool and pat point into the same allocation (pat NULL: any call). */
typedef enum { RULE_ANY, RULE_PREFIX, RULE_GLOB } rt_rule_mode;
typedef struct rt_rule {
    char *spec;
    const char *tool, *pat;
    rt_rule_mode mode;
    bool deny;
    pt_rule_src src;
} rt_rule;

/* The effect being stored: on the worker, or inline without one. */
typedef struct rt_io {
    pt_worker *worker; /* borrowed; NULL keeps the blocking host behavior */
    pt_fx fx;          /* borrowed effect bytes until the worker finishes */
    bool pending, ok;
    char error[512];
    char blob[72]; /* a stored BLOB's ref: "sha256:<hex>" */
} rt_io;

/* prompt.c: the prompt settings, kept. */
typedef struct rt_prompt {
    pt_rt_prompt v;
    bool have;
    pt_arena ar; /* the strings of v */
} rt_prompt;

/* prompt.c: the skills found by the last scan. */
typedef struct rt_skills {
    pt_rt_skill *v;
    uint32_t n, cap;
    pt_arena ar; /* their strings, until the next scan */
} rt_skills;

/* EPOCH_MATERIALS waits while held (pt_rt_materials_hold). */
typedef struct rt_mat {
    uint32_t holds;
    uint32_t fx; /* the held effect; 0: none */
    pt_buf cur;  /* ...and its epoch's system prompt */
} rt_mat;

/* tools_builtin.c's state. */
typedef struct rt_builtin {
    void *ud;
    void (*free)(void *ud);
    /* The agent is idle: true when it submitted something (a note that
     * background commands ended), so there is work again. */
    bool (*idle)(void *ud);
    /* A tool call ends: append to its result a line per background
     * command that ended untold since. */
    void (*ended)(void *ud, pt_buf *out);
    /* Background commands still running. */
    bool (*running)(void *ud);
} rt_builtin;

/* mcp.c's state. */
typedef struct rt_mcp {
    void *ud;
    void (*free)(void *ud);
    void (*start)(void *ud); /* on the host thread, at the first step */
    bool started;
} rt_mcp;

struct pt_rt {
    pt_rt_cfg c;
    pt_agent *a;
    uint64_t log_end; /* where our last commit ended (section 6.8.4) */
    rt_req **reqs;
    uint32_t nreqs, rcap;
    rt_sleeper *sleeps;
    uint32_t nsleeps, scap;
    rt_io io;
    char session[64]; /* c.session's copy */
    /* tools */
    pt_rt_tool *tools;
    pt_tool_def *defs, *on; /* every tool's, and those of the tools on (pt_rt_tool_defs) */
    uint32_t ntools, tcap;
    char **off; /* names of the tools turned off, registered or not */
    uint32_t noff, ocap;
    pt_rt_tool fallback; /* def.name: the prefix it claims; empty: none */
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
    bool project_trusted; /* a project's allow rules apply */
    bool closing, free_waiting;
    rt_builtin builtin;
    rt_prompt prompt;
    rt_skills skills;
    pt_buf scratch; /* tool results, reused */
    pt_arena ar;    /* one gate or result at a time */
    uint32_t nhttp; /* requests of tools and MCP (pt_rt_http_start) in flight */
    rt_mat mat;
    rt_mcp mcp;
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
/* What the builtin tools share: tools_builtin.c owns it, tools_bash.c
 * borrows it. Tools run one call at a time, on the host thread. */
#define RT_TOOL_IO 65536
typedef struct rt_tool_kit {
    pt_rt *rt;
    char *io;           /* RT_TOOL_IO bytes, reused by every call */
    pt_buf out;         /* a call's text, reused */
    pt_arena ar;        /* arguments of one call */
    const char **unset; /* API key variables, NULL-ended: never passed to tools */
} rt_tool_kit;
/* tools_builtin.c: call fx ends with text, an error or not. */
void pt_rt_kit_done(pt_rt *rt, uint32_t fx, pt_str text, bool error);
/* A call's arguments, parsed into k->ar (reset first). A failed
 * allocation fails only its call: k->out's is forgotten. */
const pt_json *pt_rt_kit_args(rt_tool_kit *k, const pt_fx *fx);
/* A string argument, NUL-ended in k->ar; NULL when missing or not one. */
const char *pt_rt_kit_str(rt_tool_kit *k, const pt_json *args, const char *key);
/* tools_bash.c: bash and bash_poll, with their processes; k outlives it. */
typedef struct bash bash;
bash *pt_rt_bash_new(rt_tool_kit *k);
void pt_rt_bash_free(bash *b); /* stops its processes */
void pt_rt_bash_tools(bash *b, pt_rt_tool t[2]);
/* The agent is idle: submit a note for background commands that ended
 * unread; true when it did. */
bool pt_rt_bash_idle(bash *b);
/* A tool result is being written: append a line to out for each
 * background command that ended untold, and count it told. */
void pt_rt_bash_ended(bash *b, pt_buf *out);
bool pt_rt_bash_running(const bash *b);
uint32_t pt_rt_bash_backgrounds(const bash *b, pt_rt_background *out, uint32_t max);
uint32_t pt_rt_bash_stop_all(bash *b);
/* tools_web.c: web_fetch, with its state */
typedef struct web web;
web *pt_rt_web_new(pt_rt *rt);
void pt_rt_web_free(web *w);
void pt_rt_web_tool(web *w, pt_rt_tool *t);
/* tools_websearch.c: web_search, with its state */
typedef struct wsearch wsearch;
/* The variables that hold search keys, NULL-ended: never passed to tools,
 * like the providers' key variables. */
extern const char *const pt_rt_search_env[3];
wsearch *pt_rt_wsearch_new(pt_rt *rt);
void pt_rt_wsearch_free(wsearch *w);
void pt_rt_wsearch_tool(wsearch *w, pt_rt_tool *t);
/* tools_builtin.c: images by their first bytes (NULL: not one providers
 * take), and their size in pixels from the header */
#define PT_RT_IMAGE_MAX 3932160u /* bytes: 5 MB in base64, Anthropic's limit */
#define PT_RT_IMAGE_SIDE_MAX 8000u
/* What read shows of a text file at most, and a @path mention adds. */
#define PT_RT_READ_BYTES (50u * 1024u)
#define PT_RT_READ_LINES 2000
const char *pt_rt_image_type(const unsigned char *p, size_t n);
bool pt_rt_image_size(const unsigned char *p, size_t n, const char *mime, uint32_t *w, uint32_t *h);
/* prompt.c */
void pt_rt_prompt_free(pt_rt *rt);
/* Grow a malloc'd array to hold need items (doubling). */
bool pt_rt_grow(void **arr, uint32_t *cap, uint32_t need, size_t elem);

#endif
