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

/* ---- HTTP ----
 * A driver runs requests in the background of pump(). Callbacks happen
 * inside pump(): head once (before any body), body any number of times,
 * then done exactly once (error NULL on success). A cancelled request gets
 * no more callbacks. Callbacks must not cancel or start requests. */
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
    /* Wait up to timeout_ms for progress; returns the number of active
     * requests, or -1 on a driver failure. */
    int (*pump)(pt_http_driver *d, int timeout_ms);
    void (*free)(pt_http_driver *d);
};

pt_http_driver *pt_http_curl_new(void);
/* Answers every request from a faux script (see src/ai/faux.c). */
pt_http_driver *pt_http_faux_new(pt_str script);

#endif
