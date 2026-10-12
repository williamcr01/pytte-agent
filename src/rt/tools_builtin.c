/* The built-in tools (section 13.3): bash, bash_poll, read, write and
 * edit. grep, find and ls are in tools_search.c.
 *
 * A bash call that runs past its yield time ends with the output so far,
 * and its process goes on in the background, named by the call's task ID
 * (unique in the session, so a name from before a restart never finds a
 * new process). bash_poll reads what it printed since, waits for more,
 * or stops it. Background processes die with this process.
 *
 * read records a stamp (size, mtime, SHA-256) in the doc "reads:<path>";
 * edit refuses a file that changed since, so the model never edits text
 * it has not seen. read shows images too: their bytes go in a blob the
 * result names, and the model sees the image. */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "rt/rt_int.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHUNK 65536
#define READ_BYTES (50u * 1024u) /* text a read shows at most */
#define READ_LIMIT 2000 /* lines a read shows unless it asks for more */
#define IMAGE_MAX 3932160u /* bytes: 5 MB in base64, Anthropic's limit */
#define IMAGE_SIDE_MAX 8000u /* pixels */
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
    pt_buf spans; /* an edit's spans, reused */
    search *search; /* grep, find and ls */
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
    b->out.oom = b->spans.oom = false; /* a failed allocation fails only its call */
    return pt_json_parse(fx->args, &b->ar, NULL);
}

static const char *arg_str(builtins *b, const pt_json *args, const char *key)
{
    const pt_json *v = pt_json_get(args, key);
    return v && v->type == PT_J_STR ? pt_arena_cstr(&b->ar, pt_json_str(v, &b->ar)) : NULL;
}

/* An absolute path: relative ones are under the session's directory. Paths
 * are intentionally not confined to the session directory: the tool runner
 * trusts the agent, and absolute paths are part of the tool's behavior. */
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

/* Replace path atomically: a unique temporary file next to it, synced,
 * renamed over it. Keeps the old file's mode; new files honor the process
 * umask as for open(..., 0666). */
static bool write_atomic(const char *path, const char *p, size_t n, const struct stat *expected)
{
    char tmp[4200];
    struct stat st;
    bool existed = stat(path, &st) == 0;
    mode_t mode = existed ? st.st_mode & 07777 : 0666;
    int fd, saved;
    size_t off = 0;
    mode_t mask;
    if (snprintf(tmp, sizeof tmp, "%s.pytte-XXXXXX", path) >= (int)sizeof tmp) {
        errno = ENAMETOOLONG;
        return false;
    }
    fd = mkstemp(tmp);
    if (fd < 0)
        return false;
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0)
        goto fail;
    if (!existed) {
        mask = umask(0022);
        umask(mask);
        mode &= ~mask;
    }
    while (off < n) {
        ssize_t w = write(fd, p + off, n - off);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            goto fail;
        off += (size_t)w;
    }
    if (fchmod(fd, mode) != 0 || fsync(fd) != 0)
        goto fail;
    if (close(fd) != 0) {
        fd = -1;
        goto fail;
    }
    fd = -1;
    if (expected) {
        struct stat current;
        if (stat(path, &current) != 0 || current.st_size != expected->st_size ||
            current.st_mtime != expected->st_mtime) {
            errno = ESTALE;
            goto fail;
        }
    }
    /* A change after this check and before rename remains a small unavoidable
     * TOCTOU window without directory-level locking. */
    if (rename(tmp, path) == 0)
        return true;
fail:
    saved = errno;
    if (fd >= 0)
        close(fd);
    unlink(tmp);
    errno = saved;
    return false;
}

/* ---- read ---- */

/* The image types every provider takes, by their first bytes. */
static const char *image_type(const unsigned char *p, size_t n)
{
    if (n >= 8 && memcmp(p, "\x89PNG\r\n\x1a\n", 8) == 0)
        return "image/png";
    if (n >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF)
        return "image/jpeg";
    if (n >= 6 && (memcmp(p, "GIF87a", 6) == 0 || memcmp(p, "GIF89a", 6) == 0))
        return "image/gif";
    if (n >= 12 && memcmp(p, "RIFF", 4) == 0 && memcmp(p + 8, "WEBP", 4) == 0)
        return "image/webp";
    return NULL;
}

static uint32_t be16(const unsigned char *p)
{
    return (uint32_t)p[0] << 8 | p[1];
}

static uint32_t le16(const unsigned char *p)
{
    return (uint32_t)p[1] << 8 | p[0];
}

static uint32_t le24(const unsigned char *p)
{
    return (uint32_t)p[2] << 16 | le16(p);
}

/* Width and height from the header; false when they cannot be found. */
static bool image_size(const unsigned char *p, size_t n, const char *mime, uint32_t *w, uint32_t *h)
{
    size_t i;
    if (strcmp(mime, "image/png") == 0) {
        if (n < 24)
            return false;
        *w = be16(p + 16) << 16 | be16(p + 18);
        *h = be16(p + 20) << 16 | be16(p + 22);
        return true;
    }
    if (strcmp(mime, "image/gif") == 0) {
        if (n < 10)
            return false;
        *w = le16(p + 6);
        *h = le16(p + 8);
        return true;
    }
    if (strcmp(mime, "image/webp") == 0) {
        if (n >= 30 && memcmp(p + 12, "VP8 ", 4) == 0) {
            *w = le16(p + 26) & 0x3FFF;
            *h = le16(p + 28) & 0x3FFF;
            return true;
        }
        if (n >= 25 && memcmp(p + 12, "VP8L", 4) == 0) {
            uint32_t v = le16(p + 21) | le16(p + 23) << 16;
            *w = (v & 0x3FFF) + 1;
            *h = ((v >> 14) & 0x3FFF) + 1;
            return true;
        }
        if (n >= 30 && memcmp(p + 12, "VP8X", 4) == 0) {
            *w = le24(p + 24) + 1;
            *h = le24(p + 27) + 1;
            return true;
        }
        return false;
    }
    /* JPEG: the first start-of-frame marker. */
    for (i = 2; i + 9 < n;) {
        unsigned m;
        if (p[i] != 0xFF) {
            i++;
            continue;
        }
        m = p[i + 1];
        if (m == 0xFF) {
            i++;
            continue;
        }
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD8)) { /* no length */
            i += 2;
            continue;
        }
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            *h = be16(p + i + 5);
            *w = be16(p + i + 7);
            return true;
        }
        i += 2 + be16(p + i + 2);
    }
    return false;
}

/* An image goes to the model as a blob the result names. Nothing scales
 * it here, so it must fit what providers take as it is. */
static void read_image(builtins *b, pt_rt *rt, const pt_fx *fx, int fd, const struct stat *st, const char *rel,
                       const char *mime)
{
    pt_buf *img = &b->out;
    char msg[600], ref[72];
    uint32_t w = 0, h = 0;
    pt_rt_out r;
    bool kept, huge;
    if ((uint64_t)st->st_size > IMAGE_MAX) {
        close(fd);
        snprintf(msg, sizeof msg,
                 "This image is %llu bytes; at most %u go to the model. Make a smaller copy (for example "
                 "with ImageMagick: magick in.png -resize 1568x1568 out.png) and read that.",
                 (unsigned long long)st->st_size, IMAGE_MAX);
        done_text(rt, fx->id, pt_strz(msg), true);
        return;
    }
    pt_buf_reset(img);
    /* Its size, once; one byte more shows that it grew while being read. */
    if (!pt_buf_reserve(img, (size_t)st->st_size + 1)) {
        close(fd);
        done_text(rt, fx->id, PT_S("out of memory"), true);
        return;
    }
    for (;;) {
        ssize_t got = read(fd, img->p + img->n, (size_t)st->st_size + 1 - img->n);
        if (got < 0 && errno == EINTR)
            continue;
        if (got < 0) {
            close(fd);
            fail(rt, fx->id, "cannot read", rel);
            return;
        }
        if (got == 0)
            break;
        img->n += (size_t)got;
        if (img->n > (size_t)st->st_size) {
            close(fd);
            done_text(rt, fx->id, PT_S("The image changed while being read; read it again."), true);
            return;
        }
    }
    close(fd);
    huge = image_size((const unsigned char *)img->p, img->n, mime, &w, &h) &&
           (w > IMAGE_SIDE_MAX || h > IMAGE_SIDE_MAX);
    kept = !huge && pt_rt_put_blob(rt, img->p, img->n, ref);
    if (huge)
        snprintf(msg, sizeof msg,
                 "This image is %ux%u pixels; providers take at most %u on a side. Make a smaller copy and "
                 "read that.",
                 w, h, IMAGE_SIDE_MAX);
    else if (w && h)
        snprintf(msg, sizeof msg, "Image %s (%s, %ux%u, %lu bytes).", rel, mime, w, h, (unsigned long)img->n);
    else
        snprintf(msg, sizeof msg, "Image %s (%s, %lu bytes).", rel, mime, (unsigned long)img->n);
    /* Megabytes the text reads never need: give them back. */
    pt_buf_free(img);
    pt_buf_init(img, pt_sys_allocator());
    if (huge) {
        done_text(rt, fx->id, pt_strz(msg), true);
        return;
    }
    if (!kept) {
        done_text(rt, fx->id,
                  PT_S("cannot keep the image: the session's store holds no blobs, or writing failed"), true);
        return;
    }
    memset(&r, 0, sizeof r);
    r.content = pt_strz(msg);
    r.whole = true;
    r.image = pt_strz(ref);
    r.image_mime = pt_strz(mime);
    pt_rt_tool_done(rt, fx->id, &r);
}

/* Text: the lines from offset, at most limit of them and READ_BYTES of
 * output, cut at a whole line, then where to read on. */
static void t_read(void *ud, pt_rt *rt, const pt_fx *fx)
{
    builtins *b = (builtins *)ud;
    const pt_json *args = args_of(b, fx);
    const char *rel = arg_str(b, args, "path"), *path, *mime;
    int64_t offset = pt_json_geti(args, "offset", 1), limit = pt_json_geti(args, "limit", READ_LIMIT);
    int64_t end_line;
    uint64_t line = 1, after = 0, lines;
    uint64_t full_at = 0;  /* the first line not shown for want of room */
    uint64_t cut_line = 0; /* a line longer than READ_BYTES, shown cut */
    size_t line_at = 0;    /* where the line being shown starts in out */
    bool start = true, binary = false, first = true;
    unsigned char hash[32];
    pt_sha256 sh;
    struct stat st;
    char resolved[4096];
    pt_rt_out r;
    pt_buf key;
    int fd;
    if (!rel) {
        done_text(rt, fx->id, PT_S("path is required"), true);
        return;
    }
    path = resolve(b, rel);
    if (!realpath(path, resolved)) {
        fail(rt, fx->id, "cannot resolve", path);
        return;
    }
    path = resolved;
    if (offset < 1)
        offset = 1;
    if (limit < 1)
        limit = 1;
    end_line = limit > INT64_MAX - offset ? INT64_MAX : offset + limit;
    if ((fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK)) < 0) {
        fail(rt, fx->id, "cannot open", path);
        return;
    }
    if (fstat(fd, &st) != 0) {
        close(fd);
        fail(rt, fx->id, "cannot stat", path);
        return;
    }
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        done_text(rt, fx->id, PT_S("This is not a regular file; read only regular files."), true);
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
        if (first && (mime = image_type((const unsigned char *)b->io, (size_t)got)) != NULL) {
            if (lseek(fd, 0, SEEK_SET) != 0) {
                close(fd);
                fail(rt, fx->id, "cannot read", path);
                return;
            }
            read_image(b, rt, fx, fd, &st, rel, mime);
            return;
        }
        first = false;
        pt_sha256_update(&sh, b->io, (size_t)got);
        if (line == 1 && memchr(b->io, 0, (size_t)got))
            binary = true;
        end = p + got;
        while (p < end && !binary) {
            const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
            const char *stop = nl ? nl + 1 : end;
            size_t len = (size_t)(stop - p);
            if (line >= (uint64_t)offset && line < (uint64_t)end_line && !full_at) {
                if (start) {
                    char num[24];
                    int k = snprintf(num, sizeof num, "%6llu\t", (unsigned long long)line);
                    line_at = b->out.n;
                    pt_buf_put(&b->out, num, (size_t)k);
                }
                if (b->out.n + len <= READ_BYTES) {
                    pt_buf_put(&b->out, p, len);
                } else if (line == (uint64_t)offset) {
                    /* The first line alone is too long: its start, ending
                     * on a whole character. */
                    size_t take = READ_BYTES > b->out.n ? READ_BYTES - b->out.n : 0;
                    while (take && ((unsigned char)p[take] & 0xC0) == 0x80)
                        take--;
                    pt_buf_put(&b->out, p, take);
                    cut_line = line;
                    full_at = line + 1;
                } else { /* the output ends before this line */
                    b->out.n = line_at;
                    full_at = line;
                }
            } else if (start && line >= (uint64_t)end_line) {
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
    lines = line - (start ? 1 : 0);
    if (binary) {
        pt_buf_reset(&b->out);
        pt_buf_putz(&b->out, "This is a binary file (");
        pt_buf_u64(&b->out, (uint64_t)st.st_size);
        pt_buf_putz(&b->out, " bytes); it is not shown.");
    } else if (!st.st_size) {
        pt_buf_putz(&b->out, "(the file is empty)");
    } else if ((int64_t)lines < offset) {
        pt_buf_reset(&b->out);
        pt_buf_putz(&b->out, "(the file has ");
        pt_buf_u64(&b->out, lines);
        pt_buf_putz(&b->out, " lines)");
    } else if (full_at || after) {
        if (b->out.n && b->out.p[b->out.n - 1] != '\n')
            pt_buf_putc(&b->out, '\n');
        if (cut_line) {
            pt_buf_putz(&b->out, "… line ");
            pt_buf_u64(&b->out, cut_line);
            pt_buf_putz(&b->out, " is longer than 50 KiB; only its start is shown");
            if (cut_line < lines) {
                pt_buf_putz(&b->out, ". Read on with offset ");
                pt_buf_u64(&b->out, full_at);
            }
        } else if (full_at) {
            pt_buf_putz(&b->out, "… output cut at 50 KiB before line ");
            pt_buf_u64(&b->out, full_at);
            pt_buf_putz(&b->out, " of ");
            pt_buf_u64(&b->out, lines);
            pt_buf_putz(&b->out, "; read on with offset ");
            pt_buf_u64(&b->out, full_at);
        } else {
            pt_buf_putz(&b->out, "… ");
            pt_buf_u64(&b->out, after);
            pt_buf_putz(&b->out, " more lines; read on with offset ");
            pt_buf_i64(&b->out, end_line);
        }
    }
    if (b->out.oom) {
        done_text(rt, fx->id, PT_S("out of memory"), true);
        return;
    }
    stamp(b, &st, hash);
    pt_buf_init(&key, pt_sys_allocator());
    stamp_key(b, path, &key);
    memset(&r, 0, sizeof r);
    r.content = pt_buf_str(&b->out);
    r.whole = true;
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
    if (!write_atomic(path, text.p, text.n, NULL)) {
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

/* Where one edit replaces text in the file as it was read. */
typedef struct span {
    size_t at, len;
    uint32_t edit;
} span;

static int span_cmp(const void *x, const void *y)
{
    const span *a = (const span *)x, *b = (const span *)y;
    return a->at < b->at ? -1 : a->at > b->at;
}

/* The non-overlapping occurrences of needle, as spans of edit k (into
 * b->spans); returns how many. */
static uint64_t find_all(builtins *b, pt_str hay, pt_str needle, uint32_t k)
{
    uint64_t n = 0;
    size_t i = 0;
    while (i + needle.n <= hay.n) {
        const char *at = (const char *)memchr(hay.p + i, needle.p[0], hay.n - i - needle.n + 1);
        if (!at)
            break;
        i = (size_t)(at - hay.p);
        if (memcmp(at, needle.p, needle.n) == 0) {
            span s;
            s.at = i;
            s.len = needle.n;
            s.edit = k;
            pt_buf_put(&b->spans, &s, sizeof s);
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

/* "edit 2: " when there are several, so the model knows which. */
static int edit_name(char *out, size_t cap, uint32_t k, uint32_t n)
{
    return n > 1 ? snprintf(out, cap, "edit %u: ", k + 1) : (out[0] = 0, 0);
}

/* Check the edits; their old and new strings into olds and news. */
static bool edits_of(builtins *b, const pt_json *list, pt_str *olds, pt_str *news, bool *all, uint32_t n,
                     char *msg, size_t cap)
{
    const pt_json *e;
    uint32_t k = 0;
    for (e = list->child; e; e = e->next, k++) {
        const pt_json *jo = pt_json_get(e, "old"), *jn = pt_json_get(e, "new");
        int at = edit_name(msg, cap, k, n);
        if (!pt_json_is(e, PT_J_OBJ) || !jo || !jn || jo->type != PT_J_STR || jn->type != PT_J_STR) {
            snprintf(msg + at, cap - (size_t)at, "old and new are required");
            return false;
        }
        olds[k] = pt_json_str(jo, &b->ar);
        news[k] = pt_json_str(jn, &b->ar);
        all[k] = pt_json_truthy(pt_json_get(e, "all"));
        if (!olds[k].n) {
            snprintf(msg + at, cap - (size_t)at, "old must not be empty; use write to create a file");
            return false;
        }
        if (pt_str_eq(olds[k], news[k])) {
            snprintf(msg + at, cap - (size_t)at, "old and new are the same");
            return false;
        }
    }
    return true;
}

static void t_edit(void *ud, pt_rt *rt, const pt_fx *fx)
{
    builtins *b = (builtins *)ud;
    const pt_json *args = args_of(b, fx), *list = pt_json_get(args, "edits"), *e;
    const char *rel = arg_str(b, args, "path"), *path, *why;
    pt_str *olds, *news, file;
    bool *all;
    const span *sp;
    char *src;
    size_t n, i, nspans, size, at;
    uint32_t nedits = 0, k;
    uint64_t found, total = 0;
    unsigned char hash[32];
    struct stat st, current;
    char resolved[4096];
    pt_rt_out r;
    pt_buf key;
    char msg[4300];
    if (!rel || !pt_json_is(list, PT_J_ARR) || !list->child) {
        done_text(rt, fx->id, PT_S("path and at least one edit are required"), true);
        return;
    }
    for (e = list->child; e; e = e->next)
        nedits++;
    olds = (pt_str *)pt_arena_alloc(&b->ar, nedits * sizeof *olds);
    news = (pt_str *)pt_arena_alloc(&b->ar, nedits * sizeof *news);
    all = (bool *)pt_arena_alloc(&b->ar, nedits * sizeof *all);
    if (!olds || !news || !all) {
        done_text(rt, fx->id, PT_S("out of memory"), true);
        return;
    }
    if (!edits_of(b, list, olds, news, all, nedits, msg, sizeof msg)) {
        done_text(rt, fx->id, pt_strz(msg), true);
        return;
    }
    path = resolve(b, rel);
    if (stat(path, &st) != 0) {
        fail(rt, fx->id, "cannot stat", path);
        return;
    }
    if (!S_ISREG(st.st_mode)) {
        done_text(rt, fx->id, PT_S("This is not a regular file; edit only regular files."), true);
        return;
    }
    /* Resolve symlink destinations once: editing replaces the target, not
     * the link itself. */
    if (!realpath(path, resolved)) {
        fail(rt, fx->id, "cannot resolve", path);
        return;
    }
    path = resolved;
    if (!(src = pt_sys_read_regular_file(path, &n))) {
        fail(rt, fx->id, "cannot read", path);
        return;
    }
    file = (pt_str){src, n};
    if (!fresh(b, path, file, &why)) {
        free(src);
        done_text(rt, fx->id, pt_strz(why), true);
        return;
    }
    /* Every old is matched against the file as read, never against
     * another edit's result. */
    pt_buf_reset(&b->spans);
    for (k = 0; k < nedits; k++) {
        int nm = edit_name(msg, sizeof msg, k, nedits);
        found = find_all(b, file, olds[k], k);
        if (!found || (found > 1 && !all[k])) {
            free(src);
            snprintf(msg + nm, sizeof msg - (size_t)nm,
                     found ? "old matches %llu times. Give more context so it matches once, or set all: true."
                           : "old does not match the file. Copy it exactly, with its whitespace.",
                     (unsigned long long)found);
            done_text(rt, fx->id, pt_strz(msg), true);
            return;
        }
        total += found;
    }
    if (b->spans.oom) {
        free(src);
        done_text(rt, fx->id, PT_S("out of memory"), true);
        return;
    }
    sp = (const span *)b->spans.p;
    nspans = b->spans.n / sizeof *sp;
    qsort(b->spans.p, nspans, sizeof *sp, span_cmp);
    size = n;
    for (i = 0; i < nspans; i++) {
        size_t len = news[sp[i].edit].n;
        if (i && sp[i].at < sp[i - 1].at + sp[i - 1].len) {
            free(src);
            snprintf(msg, sizeof msg, "edits %u and %u overlap. Merge them into one edit.",
                     (sp[i - 1].edit < sp[i].edit ? sp[i - 1].edit : sp[i].edit) + 1,
                     (sp[i - 1].edit < sp[i].edit ? sp[i].edit : sp[i - 1].edit) + 1);
            done_text(rt, fx->id, pt_strz(msg), true);
            return;
        }
        size -= sp[i].len;
        if (len > SIZE_MAX - size) {
            free(src);
            done_text(rt, fx->id, PT_S("the edited file would be too large"), true);
            return;
        }
        size += len;
    }
    pt_buf_reset(&b->out);
    pt_buf_reserve(&b->out, size);
    for (i = 0, at = 0; i < nspans; i++) {
        pt_buf_put(&b->out, src + at, sp[i].at - at);
        pt_buf_puts(&b->out, news[sp[i].edit]);
        at = sp[i].at + sp[i].len;
    }
    pt_buf_put(&b->out, src + at, n - at);
    free(src);
    if (b->out.oom) {
        done_text(rt, fx->id, PT_S("out of memory"), true);
        return;
    }
    if (stat(path, &current) != 0 || current.st_size != st.st_size || current.st_mtime != st.st_mtime) {
        done_text(rt, fx->id, PT_S("The file was modified while editing. Read it again, then edit it."), true);
        return;
    }
    if (!write_atomic(path, b->out.p, b->out.n, &st) || stat(path, &st) != 0) {
        fail(rt, fx->id, "cannot write", path);
        return;
    }
    hash_of(b->out.p, b->out.n, hash);
    stamp(b, &st, hash);
    if (nedits > 1)
        snprintf(msg, sizeof msg, "Edited %s: %u edits replaced %llu occurrences.", rel, nedits,
                 (unsigned long long)total);
    else
        snprintf(msg, sizeof msg, "Edited %s: replaced %llu occurrence%s.", rel, (unsigned long long)total,
                 total == 1 ? "" : "s");
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
    if (!j->timer)
        pt_proc_stop(j->p, true); /* Preserve the kill guarantee on timer OOM. */
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
        if (!j->timer) {
            j->timed_out = true;
            j->stopped = true;
            pt_proc_stop(j->p, true);
            job_end(j);
            return;
        }
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
    "\"edits\":{\"type\":\"array\",\"minItems\":1,\"items\":{\"type\":\"object\",\"properties\":{"
    "\"old\":{\"type\":\"string\",\"description\":\"Exact text to replace, as the file has it before this call\"},"
    "\"new\":{\"type\":\"string\"},"
    "\"all\":{\"type\":\"boolean\",\"description\":\"Replace every match instead of exactly one\"}},"
    "\"required\":[\"old\",\"new\"],\"additionalProperties\":false}}},"
    "\"required\":[\"path\",\"edits\"],\"additionalProperties\":false}";
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
    pt_buf_free(&b->spans);
    pt_arena_free(&b->ar);
    pt_rt_search_free(b->search);
    free(b);
}

bool pt_rt_add_builtins(pt_rt *rt)
{
    builtins *b = (builtins *)calloc(1, sizeof *b);
    const pt_ai_catalog *cat = pt_ai_builtin_catalog();
    pt_rt_tool t[8];
    uint32_t i, k, n = 0;
    if (!b || !(b->io = (char *)malloc(CHUNK)) || !(b->search = pt_rt_search_new(rt))) {
        if (b)
            free(b->io);
        free(b);
        return false;
    }
    b->rt = rt;
    pt_buf_init(&b->out, pt_sys_allocator());
    pt_buf_init(&b->doc, pt_sys_allocator());
    pt_buf_init(&b->spans, pt_sys_allocator());
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
        "Read a text file, with line numbers: up to 2000 lines or 50 KiB from offset, whichever comes "
        "first; the result says where to read on. Read a file before editing it. An image (PNG, JPEG, "
        "GIF, WebP) is shown to you as an image.");
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
        "Replace text in a file. Each edit's old must match exactly once (or set all), whitespace "
        "included. Every old is matched against the file as it was before this call, and edits must "
        "not overlap: make all changes to one file in one call. The file must have been read since it "
        "last changed.");
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
    t[4].no_ask = true; /* it can stop a session: not read-only */
    t[4].run = t_bash_poll;
    t[4].cancel = bash_cancel;
    pt_rt_search_tools(b->search, t + 5);
    for (i = 0; i < 8; i++)
        if (!pt_rt_tool_add(rt, &t[i]))
            return false;
    return true;
}
