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

/* Recovery (section 6.4): read the log into the agent, cut a torn tail,
 * hand over live files. False with err when the log is corrupt. */
bool pt_rt_recover(pt_rt *rt, char *err, size_t cap);

typedef enum {
    PT_RT_IDLE,    /* nothing left to do */
    PT_RT_STOPPED, /* cfg.stop said so */
    PT_RT_FATAL,   /* see pt_fatal / pt_error */
    PT_RT_STUCK,   /* nothing outstanding, yet not idle: a bug */
    PT_RT_BUSY     /* pt_rt_step: work is outstanding */
} pt_rt_result;

/* Until idle, fatal or stopped. */
pt_rt_result pt_rt_run(pt_rt *rt);
/* One bounded step, for hosts that wait on more than the agent (a TUI):
 * start what the agent hands out, then wait up to timeout_ms (-1: no
 * limit) for HTTP progress, the next sleeper or one of fds (marked
 * ready), and start what that produced. Waits on fds even when idle or
 * fatal; cfg.stop is not consulted. */
pt_rt_result pt_rt_step(pt_rt *rt, int timeout_ms, pt_waitfd *fds, unsigned nfds);
/* Clean exit: drop outstanding requests and commit the close claim.
 * Pending work stays pending for the next owner. */
bool pt_rt_close(pt_rt *rt);

#endif
