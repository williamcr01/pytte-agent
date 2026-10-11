/* Session store on a POSIX filesystem (section 6.1):
 *
 *   <dir>/LOCK        flock'd by the owner; says who holds it
 *   <dir>/log.jsonl   the log, appended with O_APPEND, fdatasync per commit
 *   <dir>/live/       t<task>.json snapshots, replaced with rename
 *   <dir>/blobs/      ab/cdef... (SHA-256), written to blobs/tmp first */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE /* flock */
#include "drv/drv.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
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
#ifdef __APPLE__
    return fsync(fd);
#else
    return fdatasync(fd);
#endif
}

static int sync_dir(const char *dir)
{
    int fd = open(dir, O_RDONLY), r;
    if (fd < 0)
        return -1;
    r = fsync(fd);
    close(fd);
    return r;
}

static int p_size(pt_store *vs, uint64_t *out)
{
    posix_store *s = (posix_store *)vs;
    struct stat st;
    if (fstat(s->log_fd, &st) != 0)
        return fail(vs, "cannot stat", "log.jsonl");
    *out = (uint64_t)st.st_size;
    return 0;
}

static int p_append(pt_store *vs, const void *p, size_t n, bool sync)
{
    posix_store *s = (posix_store *)vs;
    ssize_t w;
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
    ssize_t r;
    do
        r = pread(fd, p, cap, (off_t)off);
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
    int n = snprintf(out, cap, "%s/blobs/%.2s/%s", s->dir, hex, hex + 2);
    return n > 0 && (size_t)n < cap;
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
    mkdir(dir, 0700);
    path(s, dir, sizeof dir, "blobs/tmp");
    mkdir(dir, 0700);
    pt_sys_random(NULL, r, sizeof r);
    snprintf(t->path, sizeof t->path, "%s/%02x%02x%02x%02x%02x%02x%02x%02x", dir, r[0], r[1], r[2], r[3], r[4],
             r[5], r[6], r[7]);
    t->fd = open(t->path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
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
    const char *c = (const char *)p;
    while (n) {
        ssize_t w = write(t->fd, c, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return fail(vs, "cannot write", t->path);
        c += w;
        n -= (size_t)w;
    }
    return 0;
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

static int p_blob_commit(pt_store *vs, void *h, const char *hex)
{
    posix_store *s = (posix_store *)vs;
    blob_tmp *t = (blob_tmp *)h;
    char dst[4200], dir[4200];
    struct stat st;
    blob_path(s, dst, sizeof dst, hex);
    snprintf(dir, sizeof dir, "%s/blobs/%.2s", s->dir, hex);
    if (stat(dst, &st) == 0) { /* already there: the same bytes */
        p_blob_abort(vs, h);
        return 0;
    }
    if (sync_fd(t->fd) != 0 || close(t->fd) != 0) {
        t->fd = -1;
        fail(vs, "cannot sync", t->path);
        unlink(t->path);
        free(t);
        return -1;
    }
    if ((mkdir(dir, 0700) != 0 && errno != EEXIST) || rename(t->path, dst) != 0 || sync_dir(dir) != 0) {
        fail(vs, "cannot store", dst);
        unlink(t->path);
        free(t);
        return -1;
    }
    free(t);
    return 0;
}

static int p_blob_read(pt_store *vs, const char *hex, uint64_t off, void *p, size_t cap, size_t *got)
{
    posix_store *s = (posix_store *)vs;
    char src[4200];
    int fd, r;
    blob_path(s, src, sizeof src, hex);
    if ((fd = open(src, O_RDONLY | O_CLOEXEC)) < 0)
        return fail(vs, "cannot open", src);
    r = read_at(fd, off, p, cap, got);
    close(fd);
    return r == 0 ? 0 : fail(vs, "cannot read", src);
}

static int p_blob_path(pt_store *vs, const char *hex, char *out, size_t cap)
{
    if (blob_path((posix_store *)vs, out, cap, hex))
        return 0;
    snprintf(vs->err, sizeof vs->err, "blob path too long");
    return -1;
}

static int p_truncate(pt_store *vs, uint64_t off)
{
    posix_store *s = (posix_store *)vs;
    if (ftruncate(s->log_fd, (off_t)off) != 0 || fsync(s->log_fd) != 0)
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
        int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        bool ok = fd >= 0 && write(fd, p, n) == (ssize_t)n;
        if (fd >= 0 && close(fd) != 0)
            ok = false;
        if (!ok)
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
    char dir[4200], p[4600];
    DIR *d;
    struct dirent *e;
    path(s, dir, sizeof dir, "live");
    d = opendir(dir);
    if (!d)
        return errno == ENOENT ? 0 : fail(vs, "cannot list", dir);
    while ((e = readdir(d))) {
        unsigned long task;
        char rest[16];
        size_t n;
        char *data;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (sscanf(e->d_name, "t%lu%15s", &task, rest) != 2)
            continue;
        if (strcmp(rest, ".json.tmp") == 0) { /* a write that never finished */
            unlink(p);
            continue;
        }
        if (strcmp(rest, ".json") != 0 || task == 0 || task > UINT32_MAX)
            continue;
        data = pt_sys_read_file(p, &n);
        if (data) {
            fn(ud, (uint32_t)task, (pt_str){data, n});
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
    if (!s) {
        snprintf(err, cap, "out of memory");
        return NULL;
    }
    s->log_fd = s->lock_fd = -1;
    snprintf(s->dir, sizeof s->dir, "%s", dir);
    if (create && mkdir(dir, 0700) != 0) {
        snprintf(err, cap, "cannot create %s: %s", dir, strerror(errno));
        goto fail;
    }
    path(s, p, sizeof p, "live");
    if (mkdir(p, 0700) != 0 && errno != EEXIST) {
        snprintf(err, cap, "cannot open session %s: %s", dir, strerror(errno));
        goto fail;
    }
    path(s, p, sizeof p, "LOCK");
    s->lock_fd = open(p, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (s->lock_fd < 0) {
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
        ok = ftruncate(s->lock_fd, 0) == 0 && pwrite(s->lock_fd, who, (size_t)n, 0) == n;
        (void)ok; /* only used in messages */
    }
    path(s, p, sizeof p, "log.jsonl");
    s->log_fd = open(p, O_RDWR | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
    if (s->log_fd < 0) {
        snprintf(err, cap, "cannot open %s: %s", p, strerror(errno));
        goto fail;
    }
    if (create) { /* the new directory and its files must survive power loss */
        char parent[4096];
        char *slash;
        snprintf(parent, sizeof parent, "%s", dir);
        slash = strrchr(parent, '/');
        if (slash && slash != parent)
            *slash = 0;
        if (sync_dir(dir) != 0 || (slash && sync_dir(parent) != 0)) {
            snprintf(err, cap, "cannot sync %s: %s", dir, strerror(errno));
            goto fail;
        }
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
    s->vt.close = p_close;
    return &s->vt;
fail:
    p_close(&s->vt);
    return NULL;
}
