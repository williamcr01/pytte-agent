/* The built-in tools (section 13.3): bash, read, write, edit and
 * read_output. Search and listing go through bash.
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
#define PAGE 16000      /* read_output's largest page: under OUT_CAP, so never cut */

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
    uint32_t fx, timer;
    pt_proc *p;
    int out_fd, exit_fd;
    int64_t started;
    uint32_t timeout_s;
    bool timed_out, stopped, eof;
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

static void job_end(job *j)
{
    builtins *b = j->b;
    pt_rt *rt = b->rt;
    job **pp;
    int status = 0;
    char note[160], details[160];
    size_t k = 0;
    pt_rt_out r;
    int64_t ms = pt_rt_now(rt) - j->started;
    pt_proc_reap(j->p, true, &status);
    if (j->out_fd >= 0)
        pt_rt_unwatch(rt, j->out_fd);
    if (j->exit_fd >= 0)
        pt_rt_unwatch(rt, j->exit_fd);
    if (j->timer)
        pt_rt_timer_cancel(rt, j->timer);
    for (pp = &b->jobs; *pp != j; pp = &(*pp)->next)
        ;
    *pp = j->next;
    note[0] = 0;
    if (j->timed_out)
        k += (size_t)snprintf(note + k, sizeof note - k, "\n[timed out after %us]", j->timeout_s);
    else if (j->stopped)
        k += (size_t)snprintf(note + k, sizeof note - k, "\n[stopped]");
    else if (status)
        k += (size_t)snprintf(note + k, sizeof note - k, "\n[exit code %d]", status);
    snprintf(details, sizeof details, "{\"exit\":%d,\"ms\":%lld}", status, (long long)(ms < 0 ? 0 : ms));
    memset(&r, 0, sizeof r);
    r.note = (pt_str){note, k};
    r.is_error = status != 0 || j->timed_out;
    r.details = pt_strz(details);
    {
        uint32_t fx = j->fx;
        pt_proc_free(j->p);
        free(j);
        pt_rt_tool_done(rt, fx, &r);
    }
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
        pt_rt_tool_output(b->rt, j->fx, b->io, (size_t)got);
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
        pt_rt_tool_output(j->b->rt, j->fx, j->b->io, (size_t)got);
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
    int64_t timeout = pt_json_geti(args, "timeout", 0);
    pt_str cwd = pt_session_cwd(pt_rt_agent(rt));
    char err[300], call_id[200], attempt[40];
    job *j;
    if (!cmd) {
        done_text(rt, fx->id, PT_S("command is required"), true);
        return;
    }
    if (!(j = (job *)calloc(1, sizeof *j))) {
        done_text(rt, fx->id, PT_S("out of memory"), true);
        return;
    }
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
        free(j);
        done_text(rt, fx->id, pt_strz(err), true);
        return;
    }
    j->b = b;
    j->fx = fx->id;
    j->started = pt_rt_now(rt);
    j->out_fd = pt_proc_out_fd(j->p);
    j->exit_fd = pt_proc_exit_fd(j->p);
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
}

static void bash_cancel(void *ud, pt_rt *rt, uint32_t fx)
{
    builtins *b = (builtins *)ud;
    job *j = find_job(b, fx);
    if (!j)
        return;
    j->stopped = true;
    if (rt->closing) { /* exiting: no time for a grace period */
        pt_proc_stop(j->p, true);
        job_end(j);
    } else {
        stop_job(j);
    }
}

/* ---- the table ---- */

/* ---- read_output ---- */

/* The blob of call_id's output, from its result in this epoch: false with
 * why set when there is none. */
static bool find_blob(builtins *b, pt_str call_id, char hex[65], uint64_t *size, const char **why)
{
    pt_agent *a = pt_rt_agent(b->rt);
    uint32_t i = pt_entry_count(a);
    *why = "No tool call with that ID in this conversation (since the last compaction).";
    while (i--) {
        pt_entry_view e;
        const pt_json *rec, *d;
        pt_str ref;
        size_t k;
        if (!pt_entry_at(a, i, &e) || !e.msg || strcmp(e.kind, "tool_result") != 0 ||
            !pt_str_eq(e.msg->call_id, call_id))
            continue;
        *why = "That call's output was not cut: all of it is in its result.";
        rec = pt_json_parse(e.raw, &b->ar, NULL);
        d = pt_json_get(pt_json_get(rec, "data"), "details");
        ref = pt_json_str(pt_json_get(d, "blob"), &b->ar);
        if (ref.n != 71 || memcmp(ref.p, "sha256:", 7) != 0)
            return false;
        for (k = 0; k < 64; k++) {
            char ch = ref.p[7 + k];
            if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
                return false;
            hex[k] = ch;
        }
        hex[64] = 0;
        *size = (uint64_t)pt_json_geti(d, "size", 0);
        return true;
    }
    return false;
}

static void t_read_output(void *ud, pt_rt *rt, const pt_fx *fx)
{
    builtins *b = (builtins *)ud;
    pt_store *st = rt->c.store;
    const pt_json *args = args_of(b, fx);
    const char *id = arg_str(b, args, "call_id"), *why;
    int64_t offset = pt_json_geti(args, "offset", 0), limit = pt_json_geti(args, "limit", PAGE);
    uint64_t size, end;
    size_t got = 0, n;
    char hex[65];
    if (!id) {
        done_text(rt, fx->id, PT_S("call_id is required"), true);
        return;
    }
    if (!st->blob_read || !find_blob(b, pt_strz(id), hex, &size, &why)) {
        done_text(rt, fx->id, st->blob_read ? pt_strz(why) : PT_S("This session keeps no tool output."), true);
        return;
    }
    if (offset < 0)
        offset = 0;
    if (limit < 1 || limit > PAGE)
        limit = PAGE;
    if ((uint64_t)offset >= size) {
        pt_buf_reset(&b->out);
        pt_buf_putz(&b->out, "(the output has ");
        pt_buf_u64(&b->out, size);
        pt_buf_putz(&b->out, " bytes)");
        done_text(rt, fx->id, pt_buf_str(&b->out), true);
        return;
    }
    if (st->blob_read(st, hex, (uint64_t)offset, b->io, (size_t)limit, &got) != 0) {
        done_text(rt, fx->id, pt_strz(st->err), true);
        return;
    }
    /* End on a whole UTF-8 character when more follows. */
    n = got;
    if ((uint64_t)offset + got < size) {
        size_t k = n, back = 0;
        while (k && back < 3 && ((unsigned char)b->io[k - 1] & 0xC0) == 0x80) {
            k--;
            back++;
        }
        if (k && ((unsigned char)b->io[k - 1] & 0xC0) == 0xC0) {
            unsigned char lead = (unsigned char)b->io[k - 1];
            size_t len = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
            if (back + 1 < len) /* the character runs past the page */
                n = k - 1;
        }
    }
    end = (uint64_t)offset + n;
    pt_buf_reset(&b->out);
    pt_buf_put(&b->out, b->io, n);
    if (end < size) {
        if (n && b->io[n - 1] != '\n')
            pt_buf_putc(&b->out, '\n');
        pt_buf_putz(&b->out, "…[bytes ");
        pt_buf_i64(&b->out, offset);
        pt_buf_putz(&b->out, "-");
        pt_buf_u64(&b->out, end);
        pt_buf_putz(&b->out, " of ");
        pt_buf_u64(&b->out, size);
        pt_buf_putz(&b->out, "; read on with offset ");
        pt_buf_u64(&b->out, end);
        pt_buf_putz(&b->out, "]");
    }
    done_text(rt, fx->id, pt_buf_str(&b->out), false);
}

static const char bash_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"command\":{\"type\":\"string\",\"description\":\"The command line\"},"
    "\"timeout\":{\"type\":\"integer\",\"minimum\":1,\"description\":\"Stop it after this many seconds (default: never)\"}},"
    "\"required\":[\"command\"],\"additionalProperties\":false}";
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
static const char read_output_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"call_id\":{\"type\":\"string\",\"description\":\"The ID of the tool call whose output was cut\"},"
    "\"offset\":{\"type\":\"integer\",\"minimum\":0,\"description\":\"First byte, from 0\"},"
    "\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":16000,\"description\":\"Bytes to show (default 16000)\"}},"
    "\"required\":[\"call_id\"],\"additionalProperties\":false}";

static void builtins_free(void *ud)
{
    builtins *b = (builtins *)ud;
    while (b->jobs) {
        b->jobs->stopped = true;
        pt_proc_stop(b->jobs->p, true);
        job_end(b->jobs);
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
    memset(t, 0, sizeof t);
    for (i = 0; i < 5; i++) {
        t[i].def.source = PT_S("builtin");
        t[i].ud = b;
    }
    t[0].def.name = PT_S("bash");
    t[0].def.description = PT_S(
        "Run a command line in the user's shell, in the session's directory, and return its output "
        "(stdout and stderr together). Each call starts fresh: cd and export do not carry over. stdin "
        "is empty, so nothing can prompt. Use it for searching (grep, find), listing (ls), builds, "
        "tests and git.");
    t[0].def.schema = PT_S(bash_schema);
    t[0].run = t_bash;
    t[0].cancel = bash_cancel;
    t[1].def.name = PT_S("read");
    t[1].def.description = PT_S(
        "Read a text file, with line numbers. Shows up to 2000 lines from offset; ask for more with "
        "offset and limit. Read a file before editing it.");
    t[1].def.schema = PT_S(read_schema);
    t[1].def.replay = PT_REPLAY_SAFE;
    t[1].def.parallel = true;
    t[1].read_only = true;
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
    t[4].def.name = PT_S("read_output");
    t[4].def.description = PT_S(
        "Page through the full output of an earlier tool call whose result was cut (it says bytes "
        "were omitted), by byte offset.");
    t[4].def.schema = PT_S(read_output_schema);
    t[4].def.replay = PT_REPLAY_SAFE;
    t[4].def.parallel = true;
    t[4].read_only = true;
    t[4].run = t_read_output;
    for (i = 0; i < 5; i++)
        if (!pt_rt_tool_add(rt, &t[i]))
            return false;
    return true;
}
