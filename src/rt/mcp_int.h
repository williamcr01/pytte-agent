/* Inside the MCP client: shared by mcp.c (servers, protocol, tools),
 * mcp_io.c (the transports) and mcp_oauth.c (logging in to remote
 * servers). */
#ifndef PT_RT_MCP_INT_H
#define PT_RT_MCP_INT_H

#include "rt/mcp.h"
#include "rt/rt_int.h"

typedef struct mcp mcp;
typedef struct srv srv;
typedef struct mauth mauth;

/* ---- HTTP calls: one request, its response collected ----
 * done runs from a timer after the request ended (so it may start
 * others); the call is freed after it returns. With on_msg, an event
 * stream's messages go there as they arrive. */
typedef struct hcall hcall;
struct hcall {
    hcall *next;
    mcp *m;
    srv *s;
    void *req;
    uint32_t timer;
    pt_arena ar; /* the URL and headers */
    pt_http_req hr;
    pt_header hdr[16];
    pt_buf body_out, body;
    int status;
    char ctype[128], session[256];
    pt_buf www; /* WWW-Authenticate */
    pt_sse sse;
    bool sse_on, too_big, ended;
    char err[300];
    int64_t msg_id; /* the JSON-RPC request it carries; 0: none */
    bool init;      /* ...which is initialize */
    bool oauth;     /* part of a login or refresh */
    void (*on_msg)(hcall *h, pt_str json);
    void (*done)(hcall *h);
    void *ud;
};

/* A call to url (method "GET" or "POST"), its headers added with
 * hcall_header before hcall_start. */
hcall *hcall_new(mcp *m, srv *s, const char *method, pt_str url);
void hcall_header(hcall *h, const char *name, pt_str value);
/* False (and h freed) when it cannot start; done is not called then. */
bool hcall_start(hcall *h);
void hcall_free(hcall *h); /* cancels it */

/* ---- servers ---- */

typedef struct list {
    pt_arena ar;
    void *v;
    uint32_t n, cap;
} list;

typedef struct mtool {
    pt_str name; /* the server's */
    pt_str full; /* mcp__server__name: the model's */
    pt_str desc, schema; /* schema: without the arguments pytte fills in */
    pt_str fill;         /* those arguments: a JSON object, or empty */
    bool ro, safe;
} mtool;

struct srv {
    mcp *m;
    /* config, in cfg */
    pt_arena cfg;
    const char *name, *cwd, *url, *target, *client_id, *client_secret, *scope;
    const char **argv, **env; /* NULL-ended */
    pt_header *headers;
    uint32_t nheaders, timeout_s, startup_s;
    bool trust, http;
    const pt_json *fill; /* "arguments": {"<tool>" or "*": {...}}; NULL: none */
    char prefix[48]; /* "mcp__<name>__" */
    pt_mcp_state state;
    char error[600];
    uint64_t gen; /* a connection's number: what was sent before a restart is gone */
    /* stdio */
    pt_proc *p;
    int out_fd, err_fd, exit_fd;
    pt_buf line, outq;
    uint32_t flush_timer;
    char errtail[1024];
    size_t errtail_n;
    /* http */
    char session[256], version[32];
    struct qmsg *outbox;
    uint32_t post_timer;
    mauth *auth;
    /* protocol */
    int64_t next_id;
    bool cap_tools, cap_prompts, cap_resources, initialized;
    list tools, tools_next, prompts, prompts_next, resources, resources_next;
    bool tools_pending; /* a new list waits for running calls to end */
    bool holding;       /* holds the materials while starting */
    uint32_t startup_timer;
    int64_t failed_at;
};

struct mcp {
    pt_rt *rt;
    pt_rt_mcp_host host;
    srv **srvs;
    uint32_t nsrvs;
    struct pend *pend;
    uint32_t npend, pcap;
    struct mcall *calls;
    hcall *hcalls;
    char *io; /* reads, reused */
    pt_arena ar; /* one message */
    const char **unset; /* API key variables, NULL-ended */
    /* what the getters hand out */
    pt_rt_mcp_info *infos;
    pt_rt_mcp_prompt *agg_prompts;
    pt_rt_mcp_resource *agg_resources;
    uint32_t icap, apcap, arcap;
};

#define MCP_IO_CHUNK 65536 /* mcp.io: one read */

/* ---- mcp.c: the protocol side, for the transports ---- */
/* The server failed: it closes, and what waited on it fails. */
void mcp_srv_fail(srv *s, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
/* Fail every request of s; calls not sent yet wait on, unless all. */
void mcp_fail_all(srv *s, const char *why, bool all);
/* One JSON-RPC message from the server. */
void mcp_on_message(srv *s, pt_str json);
/* The request id of s will not be answered: why, or no answer came. */
void mcp_request_failed(srv *s, int64_t id, const char *why);
void mcp_request_unanswered(srv *s, int64_t id);

void mcp_notify_user(srv *s, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
/* Connect again (after a login): what was outstanding fails. */
void mcp_reconnect(srv *s);
/* The server needs a login: say so, fail what waits. */
void mcp_needs_login(srv *s, const char *why);

/* ---- the transports (mcp_io.c) ---- */
/* Spawn a stdio server and watch its pipes; false: it failed (said so). */
bool mcp_stdio_start(srv *s);
/* Send one message: written to stdin, or POSTed from a timer. */
void mcp_send(srv *s, pt_str msg, int64_t id, bool init);
/* Close the transport: the process, or the requests in flight. */
void mcp_io_close(srv *s);

/* ---- OAuth (mcp_oauth.c) ---- */
void mcp_auth_load(srv *s);
/* "Bearer <token>", or NULL. */
const char *mcp_auth_bearer(srv *s);
/* A 401 came: its WWW-Authenticate. True when a refresh started (then the
 * server reconnects, or needs a login); false: it needs a login now. */
bool mcp_auth_unauthorized(srv *s, pt_str www);
bool mcp_auth_login(srv *s);
void mcp_auth_logout(srv *s);
void mcp_auth_free(srv *s);

#endif
