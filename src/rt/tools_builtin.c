/* The built-in tools (section 13.3): bash, bash_poll, read, write and
 * edit. Search and listing go through bash.
 *
 * A bash call that runs past its yield time ends with the output so far,
 * and its process goes on in the background, named by the call's task ID
 * (unique in the session, so a name from before a restart never finds a
 * new process). bash_poll reads what it printed since, waits for more,
 * or stops it. Background processes die with this process.
 *
 * read records a stamp (size, mtime, SHA-256) in the doc "reads:<path>";
 * edit refuses a file that changed since, so the model never edits text
 * it has not seen. */
#define _POSIX_C_SOURCE 200809L
#include "rt/rt_int.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHUNK 65536
#define READ_LIMIT 2000 /* lines a read shows unless it asks for more */
#define YIELD_S 10        /* seconds a bash call waits before it goes to the background */
#define POLL_S 10         /* seconds a bash_poll waits for the process to end */
#define POLL_MIN_S 5      /* ...at least, as in Codex: polling in a loop costs a request each */
#define HELD_MAX (1 << 20) /* output kept while no call reads it: the newest bytes */
#define BACKGROUND_MAX 16 /* processes in the background; past it, bash calls wait */

typedef struct job job;

typedef struct builtins {
    pt_rt *rt;
    job *jobs;
    char *io;   /* CHUNK bytes, reused by every call */
    pt_buf out; /* a call's text, reused */
    pt_buf doc;
    pt_arena ar; /* arguments of one call */
    const char *unset[16]; /* API key variables: never passed to tools */
} builtins;

struct job {
    job *next;
    builtins *b;
    uint32_t fx;      /* the call reading it; 0: in the background */
    uint32_t session; /* the bash call's task: its name in the background */
    uint32_t timer;   /* the timeout, then the SIGKILL after a stop */
    uint32_t yield;   /* when the reading call ends while it runs on */
    pt_proc *p;
    char *command;
    int out_fd, exit_fd;
    int64_t started, read_at; /* read_at: when the reading call began */
    uint32_t timeout_s;
    int status;
    bool timed_out, stopped, eof, ended;
    bool background; /* it went to the background once */
    bool noted;      /* its end is told, or need not be */
    bool polled;  /* the reading call is a bash_poll */
    bool told;    /* a bash_poll asked it to stop: not an error */
    pt_buf held;  /* output while in the background */
    uint64_t dropped; /* bytes cut from the front of held */
};

/* ---- shared ---- */

static void done_text(pt_rt *rt, uint32_t fx, pt_str text, bool error)
{
    pt_rt_out r;
    memset(&r, 0, sizeof r);
    r.content = text.p ? text : PT_S("");
    r.is_error = error;
    pt_rt_tool_done(rt, fx, &r);
}

static void fail(pt_rt *rt, uint32_t fx, const char *what, const char *path)
{
    char msg[1200];
    snprintf(msg, sizeof msg, "%s %s: %s", what, path, strerror(errno));
    done_text(rt, fx, pt_strz(msg), true);
}

static const pt_json *args_of(builtins *b, const pt_fx *fx)
{
    pt_arena_reset(&b->ar);
    return pt_json_parse(fx->args, &b->ar, NULL);
}

static const char *arg_str(builtins *b, const pt_json *args, const char *key)
{
    const pt_json *v = pt_json_get(args, key);
    return v && v->type == PT_J_STR ? pt_arena_cstr(&b->ar, pt_json_str(v, &b->ar)) : NULL;
}

/* An absolute path: relative ones are under the session's directory. */
static const char *resolve(builtins *b, const char *path)
{
    pt_str cwd = pt_session_cwd(pt_rt_agent(b->rt));
    pt_buf p;
    const char *z;
    if (path[0] == '/' || !cwd.n)
        return path;
    pt_buf_init(&p, pt_sys_allocator());
    pt_buf_puts(&p, cwd);
    pt_buf_putc(&p, '/');
    pt_buf_putz(&p, path);
    z = pt_arena_cstr(&b->ar, pt_buf_str(&p));
    pt_buf_free(&p);
    return z;
}

static void stamp_key(builtins *b, const char *path, pt_buf *key)
{
    (void)b;
    pt_buf_reset(key);
    pt_buf_putz(key, "reads:");
    pt_buf_putz(key, path);
}

/* {"size":N,"mtime":S,"hash":"..."} into b->doc */
static void stamp(builtins *b, const struct stat *st, const unsigned char hash[32])
{
    char hex[64];
    pt_jw w;
    pt_hex(hex, hash, 32);
    pt_buf_reset(&b->doc);
    pt_jw_init(&w, &b->doc);
    pt_jw_obj(&w);
    pt_jw_key(&w, "size");
    pt_jw_uint(&w, (uint64_t)st->st_size);
    pt_jw_key(&w, "mtime");
    pt_jw_int(&w, (int64_t)st->st_mtime);
    pt_jw_key(&w, "hash");
    pt_jw_str(&w, (pt_str){hex, 64});
    pt_jw_obj_end(&w);
}

static void hash_of(const char *p, size_t n, unsigned char out[32])
{
    pt_sha256 s;
    pt_sha256_init(&s);
    pt_sha256_update(&s, p, n);
    pt_sha256_final(&s, out);
}

static bool mkdirs(const char *path)
{
    char tmp[4096];
    char *p;
    snprintf(tmp, sizeof tmp, "%s", path);
    for (p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0777) != 0 && errno != EEXIST)
                return false;
            *p = '/';
        }
    return true;
}

/* Replace path atomically: a temporary file next to it, synced, renamed
 * over it. Keeps the old file's mode. */
static bool write_atomic(const char *path, const char *p, size_t n)
{
    char tmp[4200];
    struct stat st;
    mode_t mode = stat(path, &st) == 0 ? st.st_mode & 07777 : 0666;
    int fd, saved;
    size_t off = 0;
    snprintf(tmp, sizeof tmp, "%s.pytte-%ld.tmp", path, (long)getpid());
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0)
        return false;
    while (off < n) {
        ssize_t w = write(fd, p + off, n - off);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            goto fail;
        off += (size_t)w;
    }
    if (fchmod(fd, mode) != 0 || fsync(fd) != 0 || close(fd) != 0) {
        fd = -1;
        goto fail;
    }
    if (rename(tmp, path) == 0)
        return true;
    fd = -1;
fail:
    saved = errno;
    if (fd >= 0)
        close(fd);
    unlink(tmp);
    errno = saved;
    return false;
}

/* ---- read ---- */

static void t_read(void *ud, pt_rt *rt, const pt_fx *fx)
{
    builtins *b = (builtins *)ud;
    const pt_json *args = args_of(b, fx);
    const char *rel = arg_str(b, args, "path"), *path;
    int64_t offset = pt_json_geti(args, "offset", 1), limit = pt_json_geti(args, "limit", READ_LIMIT);
    uint64_t line = 1, after = 0;
    bool start = true, binary = false;
    unsigned char hash[32];
    pt_sha256 sh;
    struct stat st;
    pt_rt_out r;
    pt_buf key;
    int fd;
    if (!rel) {
        done_text(rt, fx->id, PT_S("path is required"), true);
        return;
    }
    path = resolve(b, rel);
    if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0) {
        fail(rt, fx->id, "cannot open", path);
        return;
    }
    if (fstat(fd, &st) != 0) {
        close(fd);
        fail(rt, fx->id, "cannot stat", path);
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        close(fd);
        done_text(rt, fx->id, PT_S("This is a directory; list it with bash (ls)."), true);
        return;
    }
    pt_sha256_init(&sh);
    pt_buf_reset(&b->out);
    for (;;) {
        ssize_t got = read(fd, b->io, CHUNK);
        const char *p = b->io, *end;
        if (got < 0 && errno == EINTR)
            continue;
        if (got < 0) {
            close(fd);
            fail(rt, fx->id, "cannot read", path);
            return;
        }
        if (got == 0)
            break;
        pt_sha256_update(&sh, b->io, (size_t)got);
        if (line == 1 && memchr(b->io, 0, (size_t)got))
            binary = true;
        end = p + got;
        while (p < end && !binary) {
            const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
            const char *stop = nl ? nl + 1 : end;
            bool shown = (int64_t)line >= offset && (int64_t)line < offset + limit;
            if (shown) {
                if (start) {
                    char num[24];
                    int k = snprintf(num, sizeof num, "%6llu\t", (unsigned long long)line);
                    pt_buf_put(&b->out, num, (size_t)k);
                }
                pt_buf_put(&b->out, p, (size_t)(stop - p));
            } else if (start && (int64_t)line >= offset + limit) {
                after++;
            }
            start = nl != NULL;
            if (nl)
                line++;
            p = stop;
        }
    }
    close(fd);
    pt_sha256_final(&sh, hash);
    if (binary) {
        pt_buf_reset(&b->out);
        pt_buf_putz(&b->out, "This is a binary file (");
        pt_buf_u64(&b->out, (uint64_t)st.st_size);
        pt_buf_putz(&b->out, " bytes); it is not shown.");
    } else if (!st.st_size) {
        pt_buf_putz(&b->out, "(the file is empty)");
    } else if ((int64_t)line - (start ? 1 : 0) < offset) {
        pt_buf_reset(&b->out);
        pt_buf_putz(&b->out, "(the file has ");
        pt_buf_u64(&b->out, line - (start ? 1 : 0));
        pt_buf_putz(&b->out, " lines)");
    } else if (after) {
        if (b->out.n && b->out.p[b->out.n - 1] != '\n')
            pt_buf_putc(&b->out, '\n');
        pt_buf_putz(&b->out, "… ");
        pt_buf_u64(&b->out, after);
        pt_buf_putz(&b->out, " more lines; read on with offset ");
        pt_buf_i64(&b->out, offset + limit);
    }
    stamp(b, &st, hash);
    pt_buf_init(&key, pt_sys_allocator());
    stamp_key(b, path, &key);
    memset(&r, 0, sizeof r);
    r.content = pt_buf_str(&b->out);
    r.doc_key = pt_buf_str(&key);
    r.doc_value = pt_buf_str(&b->doc);
    pt_rt_tool_done(rt, fx->id, &r);
    pt_buf_free(&key);
}

/* ---- write ---- */

static void t_write(void *ud, pt_rt *rt, const pt_fx *fx)
{
    builtins *b = (builtins *)ud;
    const pt_json *args = args_of(b, fx);
    const pt_json *content = pt_json_get(args, "content");
    const char *rel = arg_str(b, args, "path"), *path;
    pt_str text;
    unsigned char hash[32];
    struct stat st;
    bool existed;
    pt_rt_out r;
    pt_buf key;
    char msg[4300];
    if (!rel || !content || content->type != PT_J_STR) {
        done_text(rt, fx->id, PT_S("path and content are required"), true);
        return;
    }
    text = pt_json_str(content, &b->ar);
    path = resolve(b, rel);
    existed = stat(path, &st) == 0;
    if (!mkdirs(path)) {
        fail(rt, fx->id, "cannot create the directories of", path);
        return;
    }
    if (!write_atomic(path, text.p, text.n)) {
        fail(rt, fx->id, "cannot write", path);
        return;
    }
    if (stat(path, &st) != 0) {
        fail(rt, fx->id, "cannot stat", path);
        return;
    }
    hash_of(text.p, text.n, hash);
    stamp(b, &st, hash);
    snprintf(msg, sizeof msg, "%s %s (%lu bytes).", existed ? "Wrote" : "Created", rel, (unsigned long)text.n);
    pt_buf_init(&key, pt_sys_allocator());
    stamp_key(b, path, &key);
    memset(&r, 0, sizeof r);
    r.content = pt_strz(msg);
    r.doc_key = pt_buf_str(&key);
    r.doc_value = pt_buf_str(&b->doc);
    pt_rt_tool_done(rt, fx->id, &r);
    pt_buf_free(&key);
}

/* ---- edit ---- */

/* Count non-overlapping occurrences of needle. */
static uint64_t count(pt_str hay, pt_str needle)
{
    uint64_t n = 0;
    size_t i = 0;
    while (i + needle.n <= hay.n) {
        const char *at = (const char *)memchr(hay.p + i, needle.p[0], hay.n - i - needle.n + 1);
        if (!at)
            break;
        i = (size_t)(at - hay.p);
        if (memcmp(at, needle.p, needle.n) == 0) {
            n++;
            i += needle.n;
        } else {
            i++;
        }
    }
    return n;
}

static bool fresh(builtins *b, const char *path, pt_str now, const char **why)
{
    pt_buf key;
    pt_str v;
    const pt_json *j;
    unsigned char hash[32];
    char hex[64];
    pt_buf_init(&key, pt_sys_allocator());
    stamp_key(b, path, &key);
    v = pt_doc(pt_rt_agent(b->rt), pt_buf_str(&key));
    pt_buf_free(&key);
    if (!v.n) {
        *why = "Read the file first (with read), then edit it.";
        return false;
    }
    j = pt_json_parse(v, &b->ar, NULL);
    hash_of(now.p, now.n, hash);
    pt_hex(hex, hash, 32);
    if (!pt_str_eq(pt_json_gets(j, "hash", &b->ar), (pt_str){hex, 64})) {
        *why = "The file was modified since you last read it. Read it again, then edit it.";
        return false;
    }
    return true;
}

static void t_edit(void *ud, pt_rt *rt, const pt_fx *fx)
{
    builtins *b = (builtins *)ud;
    const pt_json *args = args_of(b, fx), *jo = pt_json_get(args, "old"), *jn = pt_json_get(args, "new");
    const char *rel = arg_str(b, args, "path"), *path, *why;
    bool all = pt_json_truthy(pt_json_get(args, "all"));
    pt_str old, new_, file;
    char *src;
    size_t n, i = 0;
    uint64_t found;
    unsigned char hash[32];
    struct stat st;
    pt_rt_out r;
    pt_buf key;
    char msg[4300];
    if (!rel || !jo || !jn || jo->type != PT_J_STR || jn->type != PT_J_STR) {
        done_text(rt, fx->id, PT_S("path, old and new are required"), true);
        return;
    }
    old = pt_json_str(jo, &b->ar);
    new_ = pt_json_str(jn, &b->ar);
    if (!old.n) {
        done_text(rt, fx->id, PT_S("old must not be empty; use write to create a file"), true);
        return;
    }
    if (pt_str_eq(old, new_)) {
        done_text(rt, fx->id, PT_S("old and new are the same"), true);
        return;
    }
    path = resolve(b, rel);
    if (!(src = pt_sys_read_file(path, &n))) {
        fail(rt, fx->id, "cannot read", path);
        return;
    }
    file = (pt_str){src, n};
    if (!fresh(b, path, file, &why)) {
        free(src);
        done_text(rt, fx->id, pt_strz(why), true);
        return;
    }
    found = count(file, old);
    if (!found || (found > 1 && !all)) {
        free(src);
        snprintf(msg, sizeof msg,
                 found ? "old matches %llu times. Give more context so it matches once, or set all: true."
                       : "old does not match the file. Copy it exactly, with its whitespace.",
                 (unsigned long long)found);
        done_text(rt, fx->id, pt_strz(msg), true);
        return;
    }
    pt_buf_reset(&b->out);
    pt_buf_reserve(&b->out, n - (size_t)found * old.n + (size_t)found * new_.n);
    while (i < n) {
        if (i + old.n <= n && memcmp(src + i, old.p, old.n) == 0) {
            pt_buf_puts(&b->out, new_);
            i += old.n;
        } else {
            const char *nx = (const char *)memchr(src + i + 1, old.p[0], n - i - 1);
            size_t to = nx ? (size_t)(nx - src) : n;
            pt_buf_put(&b->out, src + i, to - i);
            i = to;
        }
    }
    free(src);
    if (b->out.oom) {
        done_text(rt, fx->id, PT_S("out of memory"), true);
        return;
    }
    if (!write_atomic(path, b->out.p, b->out.n) || stat(path, &st) != 0) {
        fail(rt, fx->id, "cannot write", path);
        return;
    }
    hash_of(b->out.p, b->out.n, hash);
    stamp(b, &st, hash);
    snprintf(msg, sizeof msg, "Edited %s: replaced %llu occurrence%s.", rel, (unsigned long long)found,
             found == 1 ? "" : "s");
    pt_buf_init(&key, pt_sys_allocator());
    stamp_key(b, path, &key);
    memset(&r, 0, sizeof r);
    r.content = pt_strz(msg);
    r.doc_key = pt_buf_str(&key);
    r.doc_value = pt_buf_str(&b->doc);
    pt_rt_tool_done(rt, fx->id, &r);
    pt_buf_free(&key);
}

/* ---- bash ---- */

static job *find_job(builtins *b, uint32_t fx)
{
    job *j;
    for (j = b->jobs; j && j->fx != fx; j = j->next)
        ;
    return j;
}

static uint32_t background_count(const builtins *b)
{
    const job *j;
    uint32_t n = 0;
    for (j = b->jobs; j; j = j->next)
        n += !j->fx;
    return n;
}

/* Output goes to the reading call, else into held: its newest HELD_MAX
 * bytes, trimmed once it holds twice that (amortized O(n)). */
static void emit(job *j, const char *p, size_t n)
{
    pt_buf *h = &j->held;
    if (j->fx) {
        pt_rt_tool_output(j->b->rt, j->fx, p, n);
        return;
    }
    if (n >= HELD_MAX) {
        j->dropped += h->n + n - HELD_MAX;
        pt_buf_reset(h);
        pt_buf_put(h, p + n - HELD_MAX, HELD_MAX);
        return;
    }
    if (h->n + n > 2 * HELD_MAX) {
        size_t k = HELD_MAX - n;
        j->dropped += h->n - k;
        memmove(h->p, h->p + h->n - k, k);
        h->n = k;
    }
    pt_buf_put(h, p, n);
}

static void unwatch(job *j)
{
    pt_rt *rt = j->b->rt;
    if (j->out_fd >= 0)
        pt_rt_unwatch(rt, j->out_fd);
    if (j->exit_fd >= 0)
        pt_rt_unwatch(rt, j->exit_fd);
    j->out_fd = j->exit_fd = -1;
    if (j->timer)
        pt_rt_timer_cancel(rt, j->timer);
    j->timer = 0;
}

static void job_free(job *j)
{
    job **pp;
    for (pp = &j->b->jobs; *pp != j; pp = &(*pp)->next)
        ;
    *pp = j->next;
    unwatch(j);
    if (j->yield)
        pt_rt_timer_cancel(j->b->rt, j->yield);
    pt_proc_free(j->p);
    pt_buf_free(&j->held);
    free(j->command);
    free(j);
}

/* The reading call ends: with how the process ended, which frees it, or
 * with where it runs on, which leaves it in the background. */
static void job_report(job *j)
{
    pt_rt *rt = j->b->rt;
    uint32_t fx = j->fx;
    char note[200], details[160];
    size_t k = 0;
    pt_rt_out r;
    int64_t ms = pt_rt_now(rt) - (j->polled ? j->read_at : j->started);
    if (ms < 0)
        ms = 0;
    if (j->yield)
        pt_rt_timer_cancel(rt, j->yield);
    j->yield = 0;
    note[0] = 0;
    memset(&r, 0, sizeof r);
    if (!j->ended) {
        j->background = true;
        k += (size_t)snprintf(note, sizeof note,
                              j->polled ? "\n[still running as session %u]"
                                        : "\n[still running as session %u; bash_poll reads on]",
                              j->session);
        snprintf(details, sizeof details, "{\"session\":%u,\"ms\":%lld}", j->session, (long long)ms);
    } else {
        if (j->timed_out)
            k += (size_t)snprintf(note, sizeof note, "\n[timed out after %us]", j->timeout_s);
        else if (j->stopped)
            k += (size_t)snprintf(note, sizeof note, "\n[stopped]");
        else if (j->status)
            k += (size_t)snprintf(note, sizeof note, "\n[exit code %d]", j->status);
        else if (j->polled)
            k += (size_t)snprintf(note, sizeof note, "\n[ended]");
        snprintf(details, sizeof details, "{\"exit\":%d,\"ms\":%lld}", j->status, (long long)ms);
        r.is_error = j->timed_out || (j->status != 0 && !j->told);
    }
    r.note = (pt_str){note, k};
    r.details = pt_strz(details);
    j->fx = 0;
    j->polled = false;
    if (j->ended)
        job_free(j);
    pt_rt_tool_done(rt, fx, &r);
}

/* The process ended (and its output with it). */
static void job_end(job *j)
{
    pt_proc_reap(j->p, true, &j->status);
    unwatch(j);
    j->ended = true;
    if (j->fx)
        job_report(j);
}

/* Output waits: read what is there (bounded, so keys stay responsive). */
static void job_drain(job *j)
{
    builtins *b = j->b;
    size_t total = 0;
    while (total < 4 * CHUNK) {
        long got = pt_proc_read(j->p, b->io, CHUNK);
        if (got < 0)
            return;
        if (got == 0) {
            j->eof = true;
            if (j->out_fd >= 0)
                pt_rt_unwatch(b->rt, j->out_fd);
            j->out_fd = -1;
            return;
        }
        emit(j, b->io, (size_t)got);
        total += (size_t)got;
    }
}

static void on_output(void *ud, pt_rt *rt)
{
    job *j = (job *)ud;
    int status;
    (void)rt;
    job_drain(j);
    /* Without a pidfd, the end of the output is the end of the call. */
    if (j->eof && (j->exit_fd < 0 || pt_proc_reap(j->p, false, &status)))
        job_end(j);
}

/* The process ended: take what output is left and end the call, even if
 * a background child still holds the pipe. */
static void on_exit_fd(void *ud, pt_rt *rt)
{
    job *j = (job *)ud;
    int status;
    (void)rt;
    if (!pt_proc_reap(j->p, false, &status))
        return;
    pt_rt_unwatch(j->b->rt, j->exit_fd);
    j->exit_fd = -1;
    while (!j->eof) {
        long got = pt_proc_read(j->p, j->b->io, CHUNK);
        if (got <= 0)
            break;
        emit(j, j->b->io, (size_t)got);
    }
    job_end(j);
}

static void on_kill(void *ud, pt_rt *rt)
{
    job *j = (job *)ud;
    (void)rt;
    j->timer = 0;
    pt_proc_stop(j->p, true);
}

/* SIGTERM to the group, and SIGKILL two seconds later. */
static void stop_job(job *j)
{
    pt_rt *rt = j->b->rt;
    pt_proc_stop(j->p, false);
    if (j->timer)
        pt_rt_timer_cancel(rt, j->timer);
    j->timer = pt_rt_timer(rt, pt_rt_now(rt) + 2000, on_kill, j);
}

static void on_timeout(void *ud, pt_rt *rt)
{
    job *j = (job *)ud;
    (void)rt;
    j->timer = 0;
    j->timed_out = true;
    stop_job(j);
}

/* The reading call has waited long enough: it ends, the process runs on. */
static void on_yield(void *ud, pt_rt *rt)
{
    job *j = (job *)ud;
    (void)rt;
    j->yield = 0;
    if (j->fx && !j->ended)
        job_report(j);
}

static const char *shell(void)
{
    static const char *const ok[] = {"/bash", "/zsh", "/sh", "/dash", "/ksh"};
    const char *sh = getenv("SHELL");
    size_t i, n;
    if (sh && *sh == '/')
        for (i = 0; i < sizeof ok / sizeof *ok; i++) {
            n = strlen(ok[i]);
            if (strlen(sh) >= n && strcmp(sh + strlen(sh) - n, ok[i]) == 0)
                return sh;
        }
    return "/bin/sh";
}

static void t_bash(void *ud, pt_rt *rt, const pt_fx *fx)
{
    builtins *b = (builtins *)ud;
    const pt_json *args = args_of(b, fx);
    const char *cmd = arg_str(b, args, "command"), *argv[4], *add[4];
    int64_t timeout = pt_json_geti(args, "timeout", 0), yield = pt_json_geti(args, "yield", YIELD_S);
    pt_str cwd = pt_session_cwd(pt_rt_agent(rt));
    char err[300], call_id[200], attempt[40];
    job *j;
    if (!cmd) {
        done_text(rt, fx->id, PT_S("command is required"), true);
        return;
    }
    if (!(j = (job *)calloc(1, sizeof *j)) || !(j->command = (char *)malloc(strlen(cmd) + 1))) {
        free(j);
        done_text(rt, fx->id, PT_S("out of memory"), true);
        return;
    }
    memcpy(j->command, cmd, strlen(cmd) + 1);
    argv[0] = shell();
    argv[1] = "-c";
    argv[2] = cmd;
    argv[3] = NULL;
    snprintf(call_id, sizeof call_id, "PYTTE_CALL_ID=%.*s", (int)(fx->call_id.n > 150 ? 150 : fx->call_id.n),
             fx->call_id.p);
    snprintf(attempt, sizeof attempt, "PYTTE_ATTEMPT=%u", fx->attempt);
    add[0] = call_id;
    add[1] = attempt;
    add[2] = "PYTTE_TOOL=bash";
    add[3] = NULL;
    j->p = pt_proc_spawn(argv, cwd.n ? pt_arena_cstr(&b->ar, cwd) : NULL, b->unset, add, err, sizeof err);
    if (!j->p) {
        free(j->command);
        free(j);
        done_text(rt, fx->id, pt_strz(err), true);
        return;
    }
    j->b = b;
    j->fx = fx->id;
    j->session = fx->task;
    j->started = j->read_at = pt_rt_now(rt);
    j->out_fd = pt_proc_out_fd(j->p);
    j->exit_fd = pt_proc_exit_fd(j->p);
    pt_buf_init(&j->held, pt_sys_allocator());
    j->next = b->jobs;
    b->jobs = j;
    if (!pt_rt_watch(rt, j->out_fd, on_output, j) ||
        (j->exit_fd >= 0 && !pt_rt_watch(rt, j->exit_fd, on_exit_fd, j))) {
        j->stopped = true;
        pt_proc_stop(j->p, true);
        job_end(j);
        return;
    }
    if (timeout > 0) {
        j->timeout_s = timeout > 86400 ? 86400 : (uint32_t)timeout;
        j->timer = pt_rt_timer(rt, j->started + (int64_t)j->timeout_s * 1000, on_timeout, j);
    }
    if (yield > 0 && background_count(b) < BACKGROUND_MAX)
        j->yield = pt_rt_timer(rt, j->started + (yield > 86400 ? 86400 : yield) * 1000, on_yield, j);
}

/* Abort: a bash call stops its process; a bash_poll only stops reading,
 * and the process runs on in the background. */
static void bash_cancel(void *ud, pt_rt *rt, uint32_t fx)
{
    builtins *b = (builtins *)ud;
    job *j = find_job(b, fx);
    if (!j)
        return;
    if (j->polled) {
        job_report(j);
        return;
    }
    j->stopped = true;
    if (rt->closing) { /* exiting: no time for a grace period */
        pt_proc_stop(j->p, true);
        job_end(j);
    } else {
        stop_job(j);
    }
}

static void t_bash_poll(void *ud, pt_rt *rt, const pt_fx *fx)
{
    builtins *b = (builtins *)ud;
    const pt_json *args = args_of(b, fx);
    int64_t session = pt_json_geti(args, "session", 0), wait = pt_json_geti(args, "wait", POLL_S);
    bool stop = pt_json_truthy(pt_json_get(args, "stop"));
    char msg[200];
    size_t k = 0; /* held bytes before the first whole line */
    job *j;
    for (j = b->jobs; j && (int64_t)j->session != session; j = j->next)
        ;
    if (!j) {
        snprintf(msg, sizeof msg,
                 "No session %lld: it ended and was read already, or pytte restarted since.", (long long)session);
        done_text(rt, fx->id, pt_strz(msg), true);
        return;
    }
    if (j->fx) {
        snprintf(msg, sizeof msg, "Another call is reading session %lld.", (long long)session);
        done_text(rt, fx->id, pt_strz(msg), true);
        return;
    }
    j->fx = fx->id;
    j->polled = true;
    j->read_at = pt_rt_now(rt);
    if (j->dropped) { /* from the first whole line */
        const char *nl = (const char *)memchr(j->held.p, '\n', j->held.n);
        k = nl ? (size_t)(nl + 1 - j->held.p) : 0;
        snprintf(msg, sizeof msg, "[%llu earlier bytes dropped]\n", (unsigned long long)(j->dropped + k));
        pt_rt_tool_output(rt, fx->id, msg, strlen(msg));
        j->dropped = 0;
    }
    if (j->held.n > k)
        pt_rt_tool_output(rt, fx->id, j->held.p + k, j->held.n - k);
    pt_buf_reset(&j->held);
    if (j->ended) {
        job_report(j);
    } else if (stop) { /* ends within two seconds */
        j->stopped = j->told = true;
        stop_job(j);
    } else {
        wait = wait < POLL_MIN_S ? POLL_MIN_S : wait > 86400 ? 86400 : wait;
        j->yield = pt_rt_timer(rt, j->read_at + wait * 1000, on_yield, j);
    }
}

/* Idle, with background commands ended and unread: submit a note that
 * starts a turn, so the model reads them and carries on without the
 * user asking. It is an input like any other, committed before it runs,
 * so a crash cannot lose or repeat it. */
static bool builtins_idle(void *ud)
{
    builtins *b = (builtins *)ud;
    pt_buf *t = &b->out;
    pt_input in;
    char line[400], req[48];
    job *j;
    uint32_t n = 0, first = 0;
    bool ok;
    pt_buf_reset(t);
    pt_buf_putz(t, "[pytte] Background commands you started have ended (the user did not write this "
                   "message):\n");
    for (j = b->jobs; j; j = j->next) {
        if (!j->background || !j->ended || j->noted || j->fx)
            continue;
        j->noted = true;
        if (!n++ || j->session < first)
            first = j->session;
        snprintf(line, sizeof line, "- session %u ended with %s %d: %.300s%s\n", j->session,
                 j->timed_out ? "a timeout, exit code" : "exit code", j->status, j->command,
                 strlen(j->command) > 300 ? "…" : "");
        pt_buf_putz(t, line);
    }
    if (!n)
        return false;
    pt_buf_putz(t, "Read what they printed with bash_poll, then carry on with what they were for. If "
                   "nothing is left to do, say so in a sentence.");
    snprintf(req, sizeof req, "background:%u", first);
    memset(&in, 0, sizeof in);
    in.text = pt_buf_str(t);
    in.req = pt_strz(req);
    in.note = true;
    ok = !t->oom && pt_submit(pt_rt_agent(b->rt), &in);
    return ok;
}

uint32_t pt_rt_backgrounds(pt_rt *rt, pt_rt_background *out, uint32_t max)
{
    builtins *b = (builtins *)rt->builtin;
    const job *j;
    uint32_t n = 0;
    for (j = b ? b->jobs : NULL; j; j = j->next) {
        if (!j->background)
            continue;
        if (n < max) {
            out[n].session = j->session;
            out[n].command = j->command;
            out[n].started = j->started;
            out[n].ended = j->ended;
            out[n].status = j->status;
        }
        n++;
    }
    return n;
}

uint32_t pt_rt_backgrounds_stop(pt_rt *rt)
{
    builtins *b = (builtins *)rt->builtin;
    job *j;
    uint32_t n = 0;
    for (j = b ? b->jobs : NULL; j; j = j->next)
        if (j->background && !j->ended && !j->stopped) {
            j->stopped = j->told = j->noted = true; /* the user stopped it: no note */
            stop_job(j);
            n++;
        }
    return n;
}

/* ---- the table ---- */

static const char bash_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"command\":{\"type\":\"string\",\"description\":\"The command line\"},"
    "\"timeout\":{\"type\":\"integer\",\"minimum\":1,\"description\":\"Stop it after this many seconds (default: never)\"},"
    "\"yield\":{\"type\":\"integer\",\"minimum\":0,\"description\":\"Return after this many seconds if it still runs, "
    "leaving it in the background (default 10; 0: wait for it to end)\"}},"
    "\"required\":[\"command\"],\"additionalProperties\":false}";
static const char bash_poll_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"session\":{\"type\":\"integer\",\"minimum\":1,\"description\":\"The session a bash call left running\"},"
    "\"wait\":{\"type\":\"integer\",\"minimum\":0,\"description\":\"Seconds to wait for it to end (default 10, at least 5)\"},"
    "\"stop\":{\"type\":\"boolean\",\"description\":\"Stop it (SIGTERM, then SIGKILL after 2s)\"}},"
    "\"required\":[\"session\"],\"additionalProperties\":false}";
static const char read_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\"},"
    "\"offset\":{\"type\":\"integer\",\"minimum\":1,\"description\":\"First line, from 1\"},"
    "\"limit\":{\"type\":\"integer\",\"minimum\":1,\"description\":\"Lines to show (default 2000)\"}},"
    "\"required\":[\"path\"],\"additionalProperties\":false}";
static const char write_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},"
    "\"required\":[\"path\",\"content\"],\"additionalProperties\":false}";
static const char edit_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\"},"
    "\"old\":{\"type\":\"string\",\"description\":\"Exact text to replace\"},"
    "\"new\":{\"type\":\"string\"},"
    "\"all\":{\"type\":\"boolean\",\"description\":\"Replace every match instead of exactly one\"}},"
    "\"required\":[\"path\",\"old\",\"new\"],\"additionalProperties\":false}";
static void builtins_free(void *ud)
{
    builtins *b = (builtins *)ud;
    while (b->jobs) { /* a reading call gets its result; the background just goes */
        job *j = b->jobs;
        j->stopped = true;
        pt_proc_stop(j->p, true);
        if (j->fx)
            job_end(j);
        else
            job_free(j);
    }
    free(b->io);
    pt_buf_free(&b->out);
    pt_buf_free(&b->doc);
    pt_arena_free(&b->ar);
    free(b);
}

bool pt_rt_add_builtins(pt_rt *rt)
{
    builtins *b = (builtins *)calloc(1, sizeof *b);
    const pt_ai_catalog *cat = pt_ai_builtin_catalog();
    pt_rt_tool t[5];
    uint32_t i, k, n = 0;
    if (!b || !(b->io = (char *)malloc(CHUNK))) {
        free(b);
        return false;
    }
    b->rt = rt;
    pt_buf_init(&b->out, pt_sys_allocator());
    pt_buf_init(&b->doc, pt_sys_allocator());
    pt_arena_init(&b->ar, pt_sys_allocator(), 4096);
    for (i = 0; i < cat->nproviders; i++)
        for (k = 0; k < 4 && cat->providers[i].key_env[k] && n + 1 < 16; k++)
            b->unset[n++] = cat->providers[i].key_env[k];
    rt->builtin = b;
    rt->builtin_free = builtins_free;
    rt->builtin_idle = builtins_idle;
    memset(t, 0, sizeof t);
    for (i = 0; i < 5; i++) {
        t[i].def.source = PT_S("builtin");
        t[i].ud = b;
    }
    t[0].def.name = PT_S("bash");
    t[0].def.description = PT_S(
        "Run a command line in the user's shell, in the session's directory, and return its output "
        "(stdout and stderr together). Each call starts fresh: cd and export do not carry over. stdin "
        "is empty, so nothing can prompt. A command still running after yield seconds keeps running "
        "in the background as a session: work on, then read it with bash_poll.");
    t[0].def.schema = PT_S(bash_schema);
    t[0].def.parallel = true;
    t[0].run = t_bash;
    t[0].cancel = bash_cancel;
    t[1].def.name = PT_S("read");
    t[1].def.description = PT_S(
        "Read a text file, with line numbers. Shows up to 2000 lines from offset; ask for more with "
        "offset and limit. Read a file before editing it.");
    t[1].def.schema = PT_S(read_schema);
    t[1].def.replay = PT_REPLAY_SAFE;
    t[1].def.parallel = true;
    t[1].def.read_only = true;
    t[1].run = t_read;
    t[2].def.name = PT_S("write");
    t[2].def.description = PT_S("Write a whole file, creating it and its directories if needed.");
    t[2].def.schema = PT_S(write_schema);
    t[2].def.replay = PT_REPLAY_SAFE;
    t[2].run = t_write;
    t[3].def.name = PT_S("edit");
    t[3].def.description = PT_S(
        "Replace text in a file: old must match exactly once (or set all), whitespace included. The "
        "file must have been read since it last changed.");
    t[3].def.schema = PT_S(edit_schema);
    t[3].run = t_edit;
    t[4].def.name = PT_S("bash_poll");
    t[4].def.description = PT_S(
        "Read what a background bash session printed since it was last read, waiting up to wait "
        "seconds for it to end; or stop it. Its result says whether it still runs. Before you end "
        "your turn, poll the sessions you started until they end, unless they are meant to keep "
        "running (a server, a watcher); then say that they still run.");
    t[4].def.schema = PT_S(bash_poll_schema);
    t[4].def.replay = PT_REPLAY_SAFE;
    t[4].def.parallel = true;
    t[4].def.read_only = true;
    t[4].run = t_bash_poll;
    t[4].cancel = bash_cancel;
    for (i = 0; i < 5; i++)
        if (!pt_rt_tool_add(rt, &t[i]))
            return false;
    return true;
}
