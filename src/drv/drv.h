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
/* Like pt_sys_read_file, but rejects non-regular files without blocking. */
char *pt_sys_read_regular_file(const char *path, size_t *n);
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
/* Hostname, boot ID (empty when unknown) and pid, for claim records.
 * A zero buffer capacity means no output is written to that buffer. */
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

/* ---- processes ----
 * A command in its own session and process group, with no terminal:
 * stdin is /dev/null, stdout and stderr go to one pipe (unless opts say
 * otherwise). */
typedef struct pt_proc pt_proc;
typedef struct pt_proc_opts {
    /* stdin is a pipe we write (pt_proc_write). SIGPIPE is then ignored in
     * this process, if it was not handled already: a write to a process
     * that died fails with EPIPE instead. */
    bool stdin_pipe;
    bool split_err; /* stderr goes to its own pipe (pt_proc_err_fd, pt_proc_read_err) */
} pt_proc_opts;
/* argv[0] is run with argv; env: the environment, minus the variables
 * named in unset, plus the NAME=value strings in add (both NULL-ended).
 * opts may be NULL. */
pt_proc *pt_proc_spawn(const char *const argv[], const char *cwd, const char *const unset[],
                       const char *const add[], const pt_proc_opts *opts, char *err, size_t cap);
int pt_proc_out_fd(const pt_proc *p);  /* readable when output (or EOF) waits */
int pt_proc_err_fd(const pt_proc *p);  /* split_err: readable when stderr waits; else -1 */
/* stdin_pipe: write without blocking. Returns bytes written (maybe fewer
 * than n, maybe 0 when the pipe is full), or -1 when the reader is gone. */
long pt_proc_write(pt_proc *p, const void *buf, size_t n);
/* Close stdin (the process sees EOF). */
void pt_proc_close_in(pt_proc *p);
int pt_proc_in_fd(const pt_proc *p); /* writable when room waits; -1 when closed */
long pt_proc_read_err(pt_proc *p, void *buf, size_t cap); /* like pt_proc_read */
int pt_proc_exit_fd(const pt_proc *p); /* readable once the process ended; -1 where unsupported */
/* Read output: > 0 bytes, 0 at the end, -1 nothing right now. */
long pt_proc_read(pt_proc *p, void *buf, size_t cap);
/* Collect the exit status, waiting when wait. status: the exit code, or
 * 128 + the signal. False when still running (wait false). */
bool pt_proc_reap(pt_proc *p, bool wait, int *status);
/* Signal the whole group: SIGTERM, or SIGKILL when hard. */
void pt_proc_stop(pt_proc *p, bool hard);
/* Kills the group if it still runs, reaps it, closes the pipe. */
void pt_proc_free(pt_proc *p);

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
    /* Optional: each response header, before head. Headers of an interim
     * (1xx) response may come first; keep the last value of a name. The
     * strings live only during the call. */
    void (*header)(void *ud, pt_str name, pt_str value);
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

/* Uses system libcurl and its certificate trust configuration. Captures
 * SSL_CERT_FILE / SSL_CERT_DIR at creation; copied paths live until free.
 * OpenSSL's hashed CA directory is preferred when available, with a verified
 * bundle retry before any request bytes are sent. Explicit files and proxy
 * configurations keep the bundle path. Driver use has one owner thread. */
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
    /* Up to cap bytes of the log from off; *got = 0 at its end. Recovery
     * reads in chunks, so a long log needs no memory of its size. */
    int (*read)(pt_store *s, uint64_t off, void *p, size_t cap, size_t *got);
    int (*truncate)(pt_store *s, uint64_t off);
    /* Live files: best effort, replaced atomically. */
    int (*live_put)(pt_store *s, uint32_t task, const void *p, size_t n);
    int (*live_del)(pt_store *s, uint32_t task);
    int (*live_list)(pt_store *s, void (*fn)(void *ud, uint32_t task, pt_str bytes), void *ud);
    /* Blobs (section 6.5): immutable, named by the SHA-256 of their bytes
     * (64 hex digits). Written in pieces, then committed: durable before
     * any record names it. A crash before that leaves only a temporary
     * file. NULL blob_begin: the store keeps no blobs. */
    void *(*blob_begin)(pt_store *s);
    int (*blob_write)(pt_store *s, void *h, const void *p, size_t n);
    int (*blob_commit)(pt_store *s, void *h, const char *hex); /* releases h */
    void (*blob_abort)(pt_store *s, void *h);
    /* Up to cap bytes from off; *got = 0 at its end. */
    int (*blob_read)(pt_store *s, const char *hex, uint64_t off, void *p, size_t cap, size_t *got);
    /* The committed blob's file, for tools to read: 0 with out set, or -1.
     * NULL: blobs are not files. */
    int (*blob_path)(pt_store *s, const char *hex, char *out, size_t cap);
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
