/* Platform drivers used by hosts (the CLI, tests). Not part of the portable
 * core: these may use libc, POSIX and libcurl. */
#ifndef PT_DRV_H
#define PT_DRV_H

#include "pytte_ai.h"

/* ---- system ---- */

pt_allocator pt_sys_allocator(void);
int64_t pt_sys_now_ms(void); /* wall clock */
void pt_sys_sleep_ms(uint32_t ms);
/* pt_env_fn over getenv. */
const char *pt_sys_env(void *ud, const char *name);
/* Whole file, NUL-terminated, malloc'd. NULL on error (errno set). */
char *pt_sys_read_file(const char *path, size_t *n);
bool pt_sys_write_file(const char *path, const void *p, size_t n);
/* Extra file descriptors to wake on while waiting (stdin, a signal pipe):
 * ready is set when fd can be read without blocking. */
typedef struct pt_waitfd {
    int fd;
    bool ready;
} pt_waitfd;
/* poll() for readability: wait up to timeout_ms (-1: forever) for one of
 * fds. Returns how many are ready, or -1 (errno set; EINTR is 0). */
int pt_sys_wait(pt_waitfd *fds, unsigned n, int timeout_ms);
/* Random bytes from the OS. ud is unused (fits pt_opts.random). */
void pt_sys_random(void *ud, void *out, size_t n);
/* Hostname, boot ID (empty when unknown) and pid, for claim records. */
void pt_sys_identity(char *host, size_t hcap, char *boot, size_t bcap, int64_t *pid);

/* One reusable blocking-I/O worker. start borrows fn/ud and their reachable
 * storage until finish returns true (or free joins the job). At most one
 * job is outstanding. finish is nonblocking and publishes its results to
 * the caller; fd wakes the host poll loop. Agent/UI code stays on the host
 * thread. free waits for an executing job and releases the worker. */
typedef struct pt_worker pt_worker;
pt_worker *pt_worker_new(void);
bool pt_worker_start(pt_worker *w, void (*fn)(void *), void *ud);
bool pt_worker_finish(pt_worker *w);
int pt_worker_fd(const pt_worker *w);
void pt_worker_free(pt_worker *w);

/* ---- HTTP ----
 * A driver runs requests in the background of pump(). Callbacks happen
 * inside pump(): head once (before any body), body any number of times,
 * then done exactly once (error NULL on success). A cancelled request gets
 * no more callbacks. Callbacks must not cancel or start requests. Request
 * body bytes are borrowed until done or cancel returns; other request
 * fields need only live through start. A sink may release body storage in
 * done. All calls for one driver belong to its owning host thread. */
typedef struct pt_http_sink {
    void *ud;
    void (*head)(void *ud, int status, pt_str retry_after);
    void (*body)(void *ud, const char *p, size_t n);
    void (*done)(void *ud, const char *error);
} pt_http_sink;

typedef struct pt_http_driver pt_http_driver;
struct pt_http_driver {
    void *(*start)(pt_http_driver *d, const pt_http_req *req, const pt_http_sink *sink);
    void (*cancel)(pt_http_driver *d, void *handle);
    /* Wait up to timeout_ms for progress or for one of fds to become
     * readable (marked ready; it waits on fds even with no requests).
     * Returns the number of active requests, or -1 on a driver failure. */
    int (*pump)(pt_http_driver *d, int timeout_ms, pt_waitfd *fds, unsigned nfds);
    void (*free)(pt_http_driver *d);
};

pt_http_driver *pt_http_curl_new(void);
/* Answers every request from a faux script (see src/ai/faux.c). */
pt_http_driver *pt_http_faux_new(pt_str script);

/* ---- storage ----
 * A session's log, plus its live files. Functions return 0, or -1 with
 * err set. */
typedef struct pt_store pt_store;
struct pt_store {
    int (*size)(pt_store *s, uint64_t *out);
    /* One write at the end of the log; fsync when sync. A failure may leave
     * part of the bytes behind (the next open repairs the tail). */
    int (*append)(pt_store *s, const void *p, size_t n, bool sync);
    int (*read_log)(pt_store *s, pt_buf *out);
    int (*truncate)(pt_store *s, uint64_t off);
    /* Live files: best effort, replaced atomically. */
    int (*live_put)(pt_store *s, uint32_t task, const void *p, size_t n);
    int (*live_del)(pt_store *s, uint32_t task);
    int (*live_list)(pt_store *s, void (*fn)(void *ud, uint32_t task, pt_str bytes), void *ud);
    void (*close)(pt_store *s);
    char err[512];
};

/* The session directory dir (created with mode 0700 when create). Holds an
 * exclusive flock on dir/LOCK until closed; when another process holds it,
 * fails and says who. */
pt_store *pt_store_posix_open(const char *dir, bool create, char *err, size_t cap);

/* In memory, for tests: survives its users (close does nothing), and can
 * fail an append on purpose. */
pt_store *pt_store_memory_new(void);
void pt_store_memory_free(pt_store *s);
/* The nth append from now (1 = the next) fails: it writes keep bytes
 * first (-1: all of them, as if the process died right after). */
void pt_store_memory_fail(pt_store *s, uint32_t nth, long keep);
uint32_t pt_store_memory_appends(const pt_store *s); /* successful ones */
pt_str pt_store_memory_log(const pt_store *s);

#endif
