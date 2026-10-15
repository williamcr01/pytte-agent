/* Session store on a POSIX filesystem (section 6.1):
 *
 *   <dir>/LOCK        flock'd by the owner; says who holds it
 *   <dir>/log.jsonl   the log, appended with O_APPEND, fdatasync per commit
 *   <dir>/live/       t<task>.json snapshots, replaced with rename
 *   <dir>/blobs/      ab/cdef... (SHA-256), written to blobs/tmp first
 *   <dir>/procs/      g<id>: process groups the owner started that still run */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE /* flock */
#include "drv/drv.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct posix_store {
    pt_store vt;
    char dir[4096];
    int log_fd, lock_fd;
} posix_store;

static int fail(pt_store *s, const char *what, const char *path)
{
    snprintf(s->err, sizeof s->err, "%s %.300s: %.150s", what, path, strerror(errno));
    return -1;
}

static void path(const posix_store *s, char *out, size_t cap, const char *name)
{
    snprintf(out, cap, "%s/%s", s->dir, name);
}

static void live_path(const posix_store *s, char *out, size_t cap, uint32_t task, const char *ext)
{
    snprintf(out, cap, "%s/live/t%lu.json%s", s->dir, (unsigned long)task, ext);
}

static int sync_fd(int fd)
{
    return pt_sys_sync_fd(fd, true);
}

static int sync_dir(const char *dir)
{
    return pt_sys_sync_dir(dir, false);
}

/* 1: created, 0: already an actual directory, -1: error. */
static int ensure_directory(const char *dir)
{
    struct stat st;
    if (mkdir(dir, 0700) == 0)
        return 1;
    if (errno != EEXIST)
        return -1;
    if (lstat(dir, &st) != 0)
        return -1;
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    return 0;
}

static int require_regular_file(int fd)
{
    struct stat st;
    if (fstat(fd, &st) != 0)
        return -1;
    if (!S_ISREG(st.st_mode)) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static bool valid_blob_name(const char *hex)
{
    size_t i;
    if (!hex || strlen(hex) != 64)
        return false;
    for (i = 0; i < 64; i++)
        if (!((hex[i] >= '0' && hex[i] <= '9') || (hex[i] >= 'a' && hex[i] <= 'f')))
            return false;
    return hex[64] == 0;
}

static bool checked_offset(uint64_t off, off_t *out)
{
    off_t pos = (off_t)off;
    if (pos < 0 || (uint64_t)pos != off) {
        errno = EOVERFLOW;
        return false;
    }
    *out = pos;
    return true;
}

/* POSIX leaves counts greater than SSIZE_MAX implementation-defined. */
static int write_all(int fd, const void *p, size_t n)
{
    const char *c = (const char *)p;
    while (n) {
        size_t count = n > (size_t)SSIZE_MAX ? (size_t)SSIZE_MAX : n;
        ssize_t w = write(fd, c, count);
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0)
            return -1;
        if (w == 0) {
            errno = EIO;
            return -1;
        }
        c += (size_t)w;
        n -= (size_t)w;
    }
    return 0;
}

/* Read only regular live snapshots, refusing symlinks at the final component. */
static char *read_live_file_at(int dirfd, const char *name, size_t *out_n)
{
    int fd = openat(dirfd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    char *p;
    size_t len = 0, cap;
    int error;
    if (fd < 0)
        return NULL;
    if (fstat(fd, &st) != 0)
        goto fail;
    if (!S_ISREG(st.st_mode) || st.st_size < 0 || (uintmax_t)st.st_size >= (uintmax_t)SIZE_MAX) {
        errno = EINVAL;
        goto fail;
    }
    cap = (size_t)st.st_size;
    if (cap < 4096)
        cap = 4096;
    p = (char *)malloc(cap + 1);
    if (!p) {
        errno = ENOMEM;
        goto fail;
    }
    for (;;) {
        ssize_t got;
        if (len == cap) {
            char c;
            char *np;
            size_t next;
            do got = read(fd, &c, 1); while (got < 0 && errno == EINTR);
            if (got < 0)
                goto fail_buf;
            if (!got)
                break;
            if (cap > (SIZE_MAX - 1) / 2) {
                errno = EOVERFLOW;
                goto fail_buf;
            }
            next = cap * 2;
            np = (char *)realloc(p, next + 1);
            if (!np) {
                errno = ENOMEM;
                goto fail_buf;
            }
            p = np;
            cap = next;
            p[len++] = c;
        }
        {
            size_t count = cap - len;
            if (count > 65536)
                count = 65536;
            if (count > (size_t)SSIZE_MAX)
                count = (size_t)SSIZE_MAX;
            do got = read(fd, p + len, count); while (got < 0 && errno == EINTR);
        }
        if (got < 0)
            goto fail_buf;
        len += (size_t)got;
        if (!got)
            break;
    }
    close(fd);
    p[len] = 0;
    *out_n = len;
    return p;
fail_buf:
    error = errno;
    free(p);
    errno = error;
fail:
    error = errno;
    close(fd);
    errno = error;
    return NULL;
}

static bool live_name(const char *name, uint32_t *task, bool *temporary)
{
    const unsigned char *p = (const unsigned char *)name;
    uint32_t value = 0;
    if (*p++ != 't' || *p < '0' || *p > '9')
        return false;
    do {
        uint32_t digit = (uint32_t)(*p - '0');
        if (value > (UINT32_MAX - digit) / 10u)
            return false;
        value = value * 10u + digit;
        p++;
    } while (*p >= '0' && *p <= '9');
    if (strcmp((const char *)p, ".json.tmp") == 0)
        *temporary = true;
    else if (strcmp((const char *)p, ".json") == 0)
        *temporary = false;
    else
        return false;
    *task = value;
    return true;
}

static int p_size(pt_store *vs, uint64_t *out)
{
    posix_store *s = (posix_store *)vs;
    struct stat st;
    if (fstat(s->log_fd, &st) != 0)
        return fail(vs, "cannot stat", "log.jsonl");
    if (st.st_size < 0) {
        errno = EOVERFLOW;
        return fail(vs, "cannot stat", "log.jsonl");
    }
    *out = (uint64_t)st.st_size;
    return 0;
}

static int p_append(pt_store *vs, const void *p, size_t n, bool sync)
{
    posix_store *s = (posix_store *)vs;
    ssize_t w;
    if (n > (size_t)SSIZE_MAX) {
        errno = EOVERFLOW;
        return fail(vs, "cannot append to", "log.jsonl");
    }
    do
        w = write(s->log_fd, p, n);
    while (w < 0 && errno == EINTR);
    if (w < 0)
        return fail(vs, "cannot append to", "log.jsonl");
    if ((size_t)w != n) {
        snprintf(vs->err, sizeof vs->err, "short write to log.jsonl (%ld of %lu bytes)", (long)w,
                 (unsigned long)n);
        return -1;
    }
    if (sync && sync_fd(s->log_fd) != 0)
        return fail(vs, "cannot sync", "log.jsonl");
    return 0;
}

static int read_at(int fd, uint64_t off, void *p, size_t cap, size_t *got)
{
    off_t pos;
    size_t count = cap > (size_t)SSIZE_MAX ? (size_t)SSIZE_MAX : cap;
    ssize_t r;
    if (!checked_offset(off, &pos))
        return -1;
    do
        r = pread(fd, p, count, pos);
    while (r < 0 && errno == EINTR);
    *got = r > 0 ? (size_t)r : 0;
    return r < 0 ? -1 : 0;
}

static int p_read(pt_store *vs, uint64_t off, void *p, size_t cap, size_t *got)
{
    posix_store *s = (posix_store *)vs;
    return read_at(s->log_fd, off, p, cap, got) == 0 ? 0 : fail(vs, "cannot read", "log.jsonl");
}

/* ---- blobs ---- */

typedef struct blob_tmp {
    int fd;
    char path[4300]; /* the store's directory plus blobs/tmp/<16 hex> */
} blob_tmp;

/* false when it does not fit */
static bool blob_path(const posix_store *s, char *out, size_t cap, const char *hex)
{
    int n;
    if (!valid_blob_name(hex)) {
        errno = EINVAL;
        return false;
    }
    n = snprintf(out, cap, "%s/blobs/%.2s/%s", s->dir, hex, hex + 2);
    if (n < 0 || (size_t)n >= cap) {
        errno = ENAMETOOLONG;
        return false;
    }
    return true;
}

static bool shard_dir_path(char *out, size_t cap, const char *root, const char *hex)
{
    size_t n = strlen(root);
    if (cap < 4 || n > cap - 4) {
        errno = ENAMETOOLONG;
        return false;
    }
    memcpy(out, root, n);
    out[n++] = '/';
    out[n++] = hex[0];
    out[n++] = hex[1];
    out[n] = 0;
    return true;
}

static void *p_blob_begin(pt_store *vs)
{
    posix_store *s = (posix_store *)vs;
    blob_tmp *t = (blob_tmp *)calloc(1, sizeof *t);
    unsigned char r[8];
    char dir[4200];
    if (!t) {
        snprintf(vs->err, sizeof vs->err, "out of memory");
        return NULL;
    }
    path(s, dir, sizeof dir, "blobs");
    {
        int made = ensure_directory(dir);
        if (made < 0 || (made && sync_dir(s->dir) != 0)) {
            fail(vs, "cannot create", dir);
            free(t);
            return NULL;
        }
    }
    path(s, dir, sizeof dir, "blobs/tmp");
    if (ensure_directory(dir) < 0) {
        fail(vs, "cannot create", dir);
        free(t);
        return NULL;
    }
    pt_sys_random(NULL, r, sizeof r);
    snprintf(t->path, sizeof t->path, "%s/%02x%02x%02x%02x%02x%02x%02x%02x", dir, r[0], r[1], r[2], r[3], r[4],
             r[5], r[6], r[7]);
    t->fd = open(t->path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (t->fd < 0) {
        fail(vs, "cannot create", t->path);
        free(t);
        return NULL;
    }
    return t;
}

static int p_blob_write(pt_store *vs, void *h, const void *p, size_t n)
{
    blob_tmp *t = (blob_tmp *)h;
    return write_all(t->fd, p, n) == 0 ? 0 : fail(vs, "cannot write", t->path);
}

static void p_blob_abort(pt_store *vs, void *h)
{
    blob_tmp *t = (blob_tmp *)h;
    (void)vs;
    if (!t)
        return;
    close(t->fd);
    unlink(t->path);
    free(t);
}

static int discard_blob(pt_store *vs, blob_tmp *t, const char *what, const char *path)
{
    int error = errno;
    if (t->fd >= 0)
        close(t->fd);
    unlink(t->path);
    free(t);
    errno = error;
    return fail(vs, what, path);
}

static int p_blob_commit(pt_store *vs, void *h, const char *hex)
{
    posix_store *s = (posix_store *)vs;
    blob_tmp *t = (blob_tmp *)h;
    char dst[4200], root[4200], dir[4200];
    struct stat st;
    int root_made, dir_made, sync_result, sync_error, close_result, close_error;
    if (!blob_path(s, dst, sizeof dst, hex))
        return discard_blob(vs, t, errno == EINVAL ? "bad blob name" : "blob path too long", "blob");
    path(s, root, sizeof root, "blobs");
    root_made = ensure_directory(root);
    if (root_made < 0)
        return discard_blob(vs, t, "cannot create", root);
    if (root_made && sync_dir(s->dir) != 0)
        return discard_blob(vs, t, "cannot sync", s->dir);
    if (!shard_dir_path(dir, sizeof dir, root, hex))
        return discard_blob(vs, t, "blob path too long", root);
    dir_made = ensure_directory(dir);
    if (dir_made < 0)
        return discard_blob(vs, t, "cannot create", dir);
    if (lstat(dst, &st) == 0) {
        if (S_ISREG(st.st_mode)) {
            /* Trust an existing regular hash-named file rather than rehashing
             * potentially large blobs on every commit. */
            p_blob_abort(vs, t);
            return 0;
        }
        if (unlink(dst) != 0)
            return discard_blob(vs, t, "cannot remove invalid blob", dst);
    } else if (errno != ENOENT) {
        return discard_blob(vs, t, "cannot inspect", dst);
    }

    sync_result = sync_fd(t->fd);
    sync_error = errno;
    close_result = close(t->fd); /* always close, even when syncing failed */
    close_error = errno;
    t->fd = -1;
    if (sync_result != 0 || close_result != 0) {
        errno = sync_result != 0 ? sync_error : close_error;
        return discard_blob(vs, t, "cannot sync", t->path);
    }

    if (rename(t->path, dst) != 0)
        return discard_blob(vs, t, "cannot store", dst);
    if (sync_dir(dir) != 0 || (dir_made && sync_dir(root) != 0)) {
        int error = errno;
        free(t);
        errno = error;
        return fail(vs, "cannot sync", dir);
    }
    free(t);
    return 0;
}

static int p_blob_read(pt_store *vs, const char *hex, uint64_t off, void *p, size_t cap, size_t *got)
{
    posix_store *s = (posix_store *)vs;
    char src[4200], root[4200], dir[4200];
    struct stat st;
    int fd, r;
    if (!blob_path(s, src, sizeof src, hex))
        return fail(vs, errno == EINVAL ? "bad blob name" : "blob path too long", "blob");
    path(s, root, sizeof root, "blobs");
    if (!shard_dir_path(dir, sizeof dir, root, hex))
        return fail(vs, "blob path too long", "blob");
    if (lstat(root, &st) != 0)
        return fail(vs, "cannot open", root);
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return fail(vs, "cannot open", root);
    }
    if (lstat(dir, &st) != 0)
        return fail(vs, "cannot open", dir);
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return fail(vs, "cannot open", dir);
    }
    if ((fd = open(src, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)) < 0)
        return fail(vs, "cannot open", src);
    if (require_regular_file(fd) != 0) {
        int error = errno;
        close(fd);
        errno = error;
        return fail(vs, "cannot read", src);
    }
    r = read_at(fd, off, p, cap, got);
    close(fd);
    return r == 0 ? 0 : fail(vs, "cannot read", src);
}

static int p_blob_path(pt_store *vs, const char *hex, char *out, size_t cap)
{
    if (blob_path((posix_store *)vs, out, cap, hex))
        return 0;
    snprintf(vs->err, sizeof vs->err, "%s", errno == EINVAL ? "bad blob name" : "blob path too long");
    return -1;
}

static int p_truncate(pt_store *vs, uint64_t off)
{
    posix_store *s = (posix_store *)vs;
    off_t pos;
    if (!checked_offset(off, &pos) || ftruncate(s->log_fd, pos) != 0 ||
        pt_sys_sync_fd(s->log_fd, false) != 0)
        return fail(vs, "cannot truncate", "log.jsonl");
    return 0;
}

static int p_live_put(pt_store *vs, uint32_t task, const void *p, size_t n)
{
    posix_store *s = (posix_store *)vs;
    char tmp[4200], dst[4200];
    live_path(s, tmp, sizeof tmp, task, ".tmp");
    live_path(s, dst, sizeof dst, task, "");
    {
        int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
        int saved_errno;
        if (fd < 0)
            return fail(vs, "cannot write", tmp);
        if (require_regular_file(fd) != 0) {
            saved_errno = errno;
            close(fd);
            errno = saved_errno;
            return fail(vs, "cannot write", tmp);
        }
        if (write_all(fd, p, n) != 0) {
            saved_errno = errno;
            close(fd);
            errno = saved_errno;
            return fail(vs, "cannot write", tmp);
        }
        if (close(fd) != 0)
            return fail(vs, "cannot write", tmp);
    }
    if (rename(tmp, dst) != 0)
        return fail(vs, "cannot rename", tmp);
    return 0;
}

static int p_live_del(pt_store *vs, uint32_t task)
{
    posix_store *s = (posix_store *)vs;
    char p[4200];
    live_path(s, p, sizeof p, task, "");
    if (unlink(p) != 0 && errno != ENOENT)
        return fail(vs, "cannot delete", p);
    return 0;
}

static int p_live_list(pt_store *vs, void (*fn)(void *ud, uint32_t task, pt_str bytes), void *ud)
{
    posix_store *s = (posix_store *)vs;
    char dir[4200];
    DIR *d;
    struct dirent *e;
    int fd;
    path(s, dir, sizeof dir, "live");
    fd = open(dir, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return errno == ENOENT ? 0 : fail(vs, "cannot list", dir);
    {
        struct stat st;
        if (fstat(fd, &st) != 0) {
            int error = errno;
            close(fd);
            errno = error;
            return fail(vs, "cannot list", dir);
        }
        if (!S_ISDIR(st.st_mode)) {
            close(fd);
            errno = ENOTDIR;
            return fail(vs, "cannot list", dir);
        }
    }
    d = fdopendir(fd);
    if (!d) {
        int error = errno;
        close(fd);
        errno = error;
        return fail(vs, "cannot list", dir);
    }
    while ((e = readdir(d))) {
        uint32_t task;
        bool temporary;
        size_t n;
        char *data;
        if (!live_name(e->d_name, &task, &temporary))
            continue;
        if (temporary) { /* a write that never finished */
            unlinkat(dirfd(d), e->d_name, 0);
            continue;
        }
        if (task == 0)
            continue;
        data = read_live_file_at(dirfd(d), e->d_name, &n);
        if (data) {
            fn(ud, task, (pt_str){data, n});
            free(data);
        }
    }
    closedir(d);
    return 0;
}

/* Blobs a dead owner was still writing: never committed, so never named. */
static void clear_blob_tmp(const posix_store *s)
{
    char dir[4200];
    DIR *d;
    struct dirent *e;
    path(s, dir, sizeof dir, "blobs/tmp");
    if (!(d = opendir(dir)))
        return;
    while ((e = readdir(d)))
        if (e->d_name[0] != '.')
            unlinkat(dirfd(d), e->d_name, 0);
    closedir(d);
}

/* ---- process groups ---- */

static void proc_path(const posix_store *s, char *out, size_t cap, int64_t id)
{
    snprintf(out, cap, "%s/procs/g%lld", s->dir, (long long)id);
}

static int p_proc_put(pt_store *vs, int64_t id, const void *p, size_t n)
{
    posix_store *s = (posix_store *)vs;
    char dir[4200], file[4200];
    int fd, r;
    path(s, dir, sizeof dir, "procs");
    if (ensure_directory(dir) < 0)
        return fail(vs, "cannot create", dir);
    proc_path(s, file, sizeof file, id);
    fd = open(file, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (fd < 0)
        return fail(vs, "cannot write", file);
    r = require_regular_file(fd) == 0 && write_all(fd, p, n) == 0 ? 0 : -1;
    if (close(fd) != 0)
        r = -1;
    if (r != 0) {
        int error = errno;
        unlink(file);
        errno = error;
        return fail(vs, "cannot write", file);
    }
    return 0;
}

static int p_proc_del(pt_store *vs, int64_t id)
{
    posix_store *s = (posix_store *)vs;
    char file[4200];
    proc_path(s, file, sizeof file, id);
    if (unlink(file) != 0 && errno != ENOENT)
        return fail(vs, "cannot delete", file);
    return 0;
}

static int p_proc_list(pt_store *vs, void (*fn)(void *ud, int64_t id, pt_str bytes), void *ud)
{
    posix_store *s = (posix_store *)vs;
    char dir[4200];
    DIR *d;
    struct dirent *e;
    path(s, dir, sizeof dir, "procs");
    if (!(d = opendir(dir)))
        return errno == ENOENT ? 0 : fail(vs, "cannot list", dir);
    while ((e = readdir(d))) {
        char *end, *data;
        long long id;
        size_t n;
        if (e->d_name[0] != 'g' || e->d_name[1] < '1' || e->d_name[1] > '9')
            continue;
        errno = 0;
        id = strtoll(e->d_name + 1, &end, 10);
        if (*end || errno)
            continue;
        data = read_live_file_at(dirfd(d), e->d_name, &n);
        if (data) {
            fn(ud, (int64_t)id, (pt_str){data, n});
            free(data);
        }
    }
    closedir(d);
    return 0;
}

static void p_close(pt_store *vs)
{
    posix_store *s = (posix_store *)vs;
    if (s->log_fd >= 0)
        close(s->log_fd);
    if (s->lock_fd >= 0)
        close(s->lock_fd); /* releases the flock */
    free(s);
}

pt_store *pt_store_posix_open(const char *dir, bool create, char *err, size_t cap)
{
    posix_store *s = (posix_store *)calloc(1, sizeof *s);
    char p[4200];
    size_t dir_len;
    bool dir_created = false;
    struct stat st;
    if (!s) {
        snprintf(err, cap, "out of memory");
        return NULL;
    }
    s->log_fd = s->lock_fd = -1;
    if (!dir || (dir_len = strlen(dir)) >= sizeof s->dir) {
        snprintf(err, cap, "session path too long");
        goto fail;
    }
    memcpy(s->dir, dir, dir_len + 1);
    while (dir_len > 1 && s->dir[dir_len - 1] == '/')
        s->dir[--dir_len] = 0;
    if (create) {
        if (mkdir(s->dir, 0700) != 0) {
            snprintf(err, cap, "cannot create %s: %s", s->dir, strerror(errno));
            goto fail;
        }
        dir_created = true;
    }
    if (lstat(s->dir, &st) != 0) {
        snprintf(err, cap, "cannot open session %s: %s", s->dir, strerror(errno));
        goto fail;
    }
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        snprintf(err, cap, "cannot open session %s: %s", s->dir, strerror(errno));
        goto fail;
    }
    path(s, p, sizeof p, "live");
    if (ensure_directory(p) < 0) {
        snprintf(err, cap, "cannot open session %s: %s", s->dir, strerror(errno));
        goto fail;
    }
    path(s, p, sizeof p, "LOCK");
    s->lock_fd = open(p, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (s->lock_fd < 0) {
        snprintf(err, cap, "cannot open %s: %s", p, strerror(errno));
        goto fail;
    }
    if (require_regular_file(s->lock_fd) != 0) {
        snprintf(err, cap, "cannot open %s: %s", p, strerror(errno));
        goto fail;
    }
    if (flock(s->lock_fd, LOCK_EX | LOCK_NB) != 0) {
        char holder[256] = "";
        ssize_t r = pread(s->lock_fd, holder, sizeof holder - 1, 0);
        holder[r > 0 ? r : 0] = 0;
        holder[strcspn(holder, "\n")] = 0;
        if (errno == EWOULDBLOCK || errno == EAGAIN)
            snprintf(err, cap, "the session is in use by %s", holder[0] ? holder : "another process");
        else
            snprintf(err, cap, "cannot lock %s: %s", p, strerror(errno));
        goto fail;
    }
    {
        char host[256], boot[64], who[400];
        int64_t pid;
        int n;
        bool ok;
        pt_sys_identity(host, sizeof host, boot, sizeof boot, &pid);
        n = snprintf(who, sizeof who, "%s pid %lld\n", host, (long long)pid);
        ok = n >= 0 && (size_t)n < sizeof who && ftruncate(s->lock_fd, 0) == 0 &&
             pwrite(s->lock_fd, who, (size_t)n, 0) == (ssize_t)n;
        (void)ok; /* only used in messages */
    }
    clear_blob_tmp(s); /* ours alone now */
    path(s, p, sizeof p, "log.jsonl");
    s->log_fd = open(p, O_RDWR | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (s->log_fd < 0) {
        snprintf(err, cap, "cannot open %s: %s", p, strerror(errno));
        goto fail;
    }
    if (require_regular_file(s->log_fd) != 0) {
        snprintf(err, cap, "cannot open %s: %s", p, strerror(errno));
        goto fail;
    }
    if (dir_created && (sync_dir(s->dir) != 0 || pt_sys_sync_parent(s->dir, false) != 0)) {
        snprintf(err, cap, "cannot sync %s: %s", s->dir, strerror(errno));
        goto fail;
    }
    s->vt.size = p_size;
    s->vt.append = p_append;
    s->vt.read = p_read;
    s->vt.blob_begin = p_blob_begin;
    s->vt.blob_write = p_blob_write;
    s->vt.blob_commit = p_blob_commit;
    s->vt.blob_abort = p_blob_abort;
    s->vt.blob_read = p_blob_read;
    s->vt.blob_path = p_blob_path;
    s->vt.truncate = p_truncate;
    s->vt.live_put = p_live_put;
    s->vt.live_del = p_live_del;
    s->vt.live_list = p_live_list;
    s->vt.proc_put = p_proc_put;
    s->vt.proc_del = p_proc_del;
    s->vt.proc_list = p_proc_list;
    s->vt.close = p_close;
    return &s->vt;
fail:
    p_close(&s->vt);
    return NULL;
}
