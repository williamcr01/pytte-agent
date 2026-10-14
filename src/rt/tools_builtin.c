/* The built-in tools (section 13.3): read, write and edit, and the table
 * of them all. bash and bash_poll are in tools_bash.c, grep, find and ls
 * in tools_search.c, web_fetch in tools_web.c.
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

#define READ_BYTES (50u * 1024u) /* text a read shows at most */
#define READ_LIMIT 2000 /* lines a read shows unless it asks for more */
#define IMAGE_MAX 3932160u /* bytes: 5 MB in base64, Anthropic's limit */
#define IMAGE_SIDE_MAX 8000u /* pixels */

typedef struct builtins {
    rt_tool_kit k; /* lent to bash */
    bash *bash;    /* bash and bash_poll */
    pt_buf doc;
    pt_buf spans; /* an edit's spans, reused */
    search *search; /* grep, find and ls */
    web *web;       /* web_fetch */
} builtins;

/* ---- shared ---- */

void pt_rt_kit_done(pt_rt *rt, uint32_t fx, pt_str text, bool error)
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
    pt_rt_kit_done(rt, fx, pt_strz(msg), true);
}

const pt_json *pt_rt_kit_args(rt_tool_kit *k, const pt_fx *fx)
{
    pt_arena_reset(&k->ar);
    k->out.oom = false; /* a failed allocation fails only its call */
    return pt_json_parse(fx->args, &k->ar, NULL);
}

static const pt_json *args_of(builtins *b, const pt_fx *fx)
{
    b->spans.oom = false;
    return pt_rt_kit_args(&b->k, fx);
}

const char *pt_rt_kit_str(rt_tool_kit *k, const pt_json *args, const char *key)
{
    const pt_json *v = pt_json_get(args, key);
    return v && v->type == PT_J_STR ? pt_arena_cstr(&k->ar, pt_json_str(v, &k->ar)) : NULL;
}

/* An absolute path: relative ones are under the session's directory. Paths
 * are intentionally not confined to the session directory: the tool runner
 * trusts the agent, and absolute paths are part of the tool's behavior. */
static const char *resolve(builtins *b, const char *path)
{
    pt_str cwd = pt_session_cwd(pt_rt_agent(b->k.rt));
    pt_buf p;
    const char *z;
    if (path[0] == '/' || !cwd.n)
        return path;
    pt_buf_init(&p, pt_sys_allocator());
    pt_buf_puts(&p, cwd);
    pt_buf_putc(&p, '/');
    pt_buf_putz(&p, path);
    z = pt_arena_cstr(&b->k.ar, pt_buf_str(&p));
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
const char *pt_rt_image_type(const unsigned char *p, size_t n)
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
bool pt_rt_image_size(const unsigned char *p, size_t n, const char *mime, uint32_t *w, uint32_t *h)
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
    pt_buf *img = &b->k.out;
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
        pt_rt_kit_done(rt, fx->id, pt_strz(msg), true);
        return;
    }
    pt_buf_reset(img);
    /* Its size, once; one byte more shows that it grew while being read. */
    if (!pt_buf_reserve(img, (size_t)st->st_size + 1)) {
        close(fd);
        pt_rt_kit_done(rt, fx->id, PT_S("out of memory"), true);
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
            pt_rt_kit_done(rt, fx->id, PT_S("The image changed while being read; read it again."), true);
            return;
        }
    }
    close(fd);
    huge = pt_rt_image_size((const unsigned char *)img->p, img->n, mime, &w, &h) &&
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
        pt_rt_kit_done(rt, fx->id, pt_strz(msg), true);
        return;
    }
    if (!kept) {
        pt_rt_kit_done(rt, fx->id,
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
    const char *rel = pt_rt_kit_str(&b->k, args, "path"), *path, *mime;
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
        pt_rt_kit_done(rt, fx->id, PT_S("path is required"), true);
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
        pt_rt_kit_done(rt, fx->id, PT_S("This is not a regular file; read only regular files."), true);
        return;
    }
    pt_sha256_init(&sh);
    pt_buf_reset(&b->k.out);
    for (;;) {
        ssize_t got = read(fd, b->k.io, RT_TOOL_IO);
        const char *p = b->k.io, *end;
        if (got < 0 && errno == EINTR)
            continue;
        if (got < 0) {
            close(fd);
            fail(rt, fx->id, "cannot read", path);
            return;
        }
        if (got == 0)
            break;
        if (first && (mime = pt_rt_image_type((const unsigned char *)b->k.io, (size_t)got)) != NULL) {
            if (lseek(fd, 0, SEEK_SET) != 0) {
                close(fd);
                fail(rt, fx->id, "cannot read", path);
                return;
            }
            read_image(b, rt, fx, fd, &st, rel, mime);
            return;
        }
        first = false;
        pt_sha256_update(&sh, b->k.io, (size_t)got);
        if (line == 1 && memchr(b->k.io, 0, (size_t)got))
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
                    line_at = b->k.out.n;
                    pt_buf_put(&b->k.out, num, (size_t)k);
                }
                if (b->k.out.n + len <= READ_BYTES) {
                    pt_buf_put(&b->k.out, p, len);
                } else if (line == (uint64_t)offset) {
                    /* The first line alone is too long: its start, ending
                     * on a whole character. */
                    size_t take = READ_BYTES > b->k.out.n ? READ_BYTES - b->k.out.n : 0;
                    while (take && ((unsigned char)p[take] & 0xC0) == 0x80)
                        take--;
                    pt_buf_put(&b->k.out, p, take);
                    cut_line = line;
                    full_at = line + 1;
                } else { /* the output ends before this line */
                    b->k.out.n = line_at;
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
        pt_buf_reset(&b->k.out);
        pt_buf_putz(&b->k.out, "This is a binary file (");
        pt_buf_u64(&b->k.out, (uint64_t)st.st_size);
        pt_buf_putz(&b->k.out, " bytes); it is not shown.");
    } else if (!st.st_size) {
        pt_buf_putz(&b->k.out, "(the file is empty)");
    } else if ((int64_t)lines < offset) {
        pt_buf_reset(&b->k.out);
        pt_buf_putz(&b->k.out, "(the file has ");
        pt_buf_u64(&b->k.out, lines);
        pt_buf_putz(&b->k.out, " lines)");
    } else if (full_at || after) {
        if (b->k.out.n && b->k.out.p[b->k.out.n - 1] != '\n')
            pt_buf_putc(&b->k.out, '\n');
        if (cut_line) {
            pt_buf_putz(&b->k.out, "… line ");
            pt_buf_u64(&b->k.out, cut_line);
            pt_buf_putz(&b->k.out, " is longer than 50 KiB; only its start is shown");
            if (cut_line < lines) {
                pt_buf_putz(&b->k.out, ". Read on with offset ");
                pt_buf_u64(&b->k.out, full_at);
            }
        } else if (full_at) {
            pt_buf_putz(&b->k.out, "… output cut at 50 KiB before line ");
            pt_buf_u64(&b->k.out, full_at);
            pt_buf_putz(&b->k.out, " of ");
            pt_buf_u64(&b->k.out, lines);
            pt_buf_putz(&b->k.out, "; read on with offset ");
            pt_buf_u64(&b->k.out, full_at);
        } else {
            pt_buf_putz(&b->k.out, "… ");
            pt_buf_u64(&b->k.out, after);
            pt_buf_putz(&b->k.out, " more lines; read on with offset ");
            pt_buf_i64(&b->k.out, end_line);
        }
    }
    if (b->k.out.oom) {
        pt_rt_kit_done(rt, fx->id, PT_S("out of memory"), true);
        return;
    }
    stamp(b, &st, hash);
    pt_buf_init(&key, pt_sys_allocator());
    stamp_key(b, path, &key);
    memset(&r, 0, sizeof r);
    r.content = pt_buf_str(&b->k.out);
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
    const char *rel = pt_rt_kit_str(&b->k, args, "path"), *path;
    pt_str text;
    unsigned char hash[32];
    struct stat st;
    bool existed;
    pt_rt_out r;
    pt_buf key;
    char msg[4300];
    if (!rel || !content || content->type != PT_J_STR) {
        pt_rt_kit_done(rt, fx->id, PT_S("path and content are required"), true);
        return;
    }
    text = pt_json_str(content, &b->k.ar);
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
    v = pt_doc(pt_rt_agent(b->k.rt), pt_buf_str(&key));
    pt_buf_free(&key);
    if (!v.n) {
        *why = "Read the file first (with read), then edit it.";
        return false;
    }
    j = pt_json_parse(v, &b->k.ar, NULL);
    hash_of(now.p, now.n, hash);
    pt_hex(hex, hash, 32);
    if (!pt_str_eq(pt_json_gets(j, "hash", &b->k.ar), (pt_str){hex, 64})) {
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
        olds[k] = pt_json_str(jo, &b->k.ar);
        news[k] = pt_json_str(jn, &b->k.ar);
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
    const char *rel = pt_rt_kit_str(&b->k, args, "path"), *path, *why;
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
        pt_rt_kit_done(rt, fx->id, PT_S("path and at least one edit are required"), true);
        return;
    }
    for (e = list->child; e; e = e->next)
        nedits++;
    olds = (pt_str *)pt_arena_alloc(&b->k.ar, nedits * sizeof *olds);
    news = (pt_str *)pt_arena_alloc(&b->k.ar, nedits * sizeof *news);
    all = (bool *)pt_arena_alloc(&b->k.ar, nedits * sizeof *all);
    if (!olds || !news || !all) {
        pt_rt_kit_done(rt, fx->id, PT_S("out of memory"), true);
        return;
    }
    if (!edits_of(b, list, olds, news, all, nedits, msg, sizeof msg)) {
        pt_rt_kit_done(rt, fx->id, pt_strz(msg), true);
        return;
    }
    path = resolve(b, rel);
    if (stat(path, &st) != 0) {
        fail(rt, fx->id, "cannot stat", path);
        return;
    }
    if (!S_ISREG(st.st_mode)) {
        pt_rt_kit_done(rt, fx->id, PT_S("This is not a regular file; edit only regular files."), true);
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
        pt_rt_kit_done(rt, fx->id, pt_strz(why), true);
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
            pt_rt_kit_done(rt, fx->id, pt_strz(msg), true);
            return;
        }
        total += found;
    }
    if (b->spans.oom) {
        free(src);
        pt_rt_kit_done(rt, fx->id, PT_S("out of memory"), true);
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
            pt_rt_kit_done(rt, fx->id, pt_strz(msg), true);
            return;
        }
        size -= sp[i].len;
        if (len > SIZE_MAX - size) {
            free(src);
            pt_rt_kit_done(rt, fx->id, PT_S("the edited file would be too large"), true);
            return;
        }
        size += len;
    }
    pt_buf_reset(&b->k.out);
    pt_buf_reserve(&b->k.out, size);
    for (i = 0, at = 0; i < nspans; i++) {
        pt_buf_put(&b->k.out, src + at, sp[i].at - at);
        pt_buf_puts(&b->k.out, news[sp[i].edit]);
        at = sp[i].at + sp[i].len;
    }
    pt_buf_put(&b->k.out, src + at, n - at);
    free(src);
    if (b->k.out.oom) {
        pt_rt_kit_done(rt, fx->id, PT_S("out of memory"), true);
        return;
    }
    if (stat(path, &current) != 0 || current.st_size != st.st_size || current.st_mtime != st.st_mtime) {
        pt_rt_kit_done(rt, fx->id, PT_S("The file was modified while editing. Read it again, then edit it."), true);
        return;
    }
    if (!write_atomic(path, b->k.out.p, b->k.out.n, &st) || stat(path, &st) != 0) {
        fail(rt, fx->id, "cannot write", path);
        return;
    }
    hash_of(b->k.out.p, b->k.out.n, hash);
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

/* ---- background commands (tools_bash.c) ---- */

static bool builtins_idle(void *ud)
{
    return pt_rt_bash_idle(((builtins *)ud)->bash);
}

uint32_t pt_rt_backgrounds(pt_rt *rt, pt_rt_background *out, uint32_t max)
{
    builtins *b = (builtins *)rt->builtin.ud;
    return b ? pt_rt_bash_backgrounds(b->bash, out, max) : 0;
}

uint32_t pt_rt_backgrounds_stop(pt_rt *rt)
{
    builtins *b = (builtins *)rt->builtin.ud;
    return b ? pt_rt_bash_stop_all(b->bash) : 0;
}

/* ---- the table ---- */

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
    pt_rt_bash_free(b->bash); /* first: it uses io */
    free(b->k.io);
    free(b->k.unset);
    pt_buf_free(&b->k.out);
    pt_buf_free(&b->doc);
    pt_buf_free(&b->spans);
    pt_arena_free(&b->k.ar);
    pt_rt_search_free(b->search);
    pt_rt_web_free(b->web);
    free(b);
}

bool pt_rt_add_builtins(pt_rt *rt)
{
    builtins *b = (builtins *)calloc(1, sizeof *b);
    const pt_ai_catalog *cat = rt->c.catalog ? rt->c.catalog : pt_ai_builtin_catalog();
    pt_rt_tool t[9], bt[2];
    uint32_t i, k, n = 0;
    if (!b || !(b->k.io = (char *)malloc(RT_TOOL_IO)) ||
        !(b->k.unset = (const char **)calloc((size_t)cat->nproviders * 4 + 1, sizeof *b->k.unset)) ||
        !(b->search = pt_rt_search_new(rt)) || !(b->web = pt_rt_web_new(rt)) ||
        !(b->bash = pt_rt_bash_new(&b->k))) {
        if (b) {
            free(b->k.io);
            free(b->k.unset);
            pt_rt_search_free(b->search);
            pt_rt_web_free(b->web);
        }
        free(b);
        return false;
    }
    b->k.rt = rt;
    pt_buf_init(&b->k.out, pt_sys_allocator());
    pt_buf_init(&b->doc, pt_sys_allocator());
    pt_buf_init(&b->spans, pt_sys_allocator());
    pt_arena_init(&b->k.ar, pt_sys_allocator(), 4096);
    for (i = 0; i < cat->nproviders; i++)
        for (k = 0; k < 4 && cat->providers[i].key_env[k]; k++)
            b->k.unset[n++] = cat->providers[i].key_env[k];
    rt->builtin.ud = b;
    rt->builtin.free = builtins_free;
    rt->builtin.idle = builtins_idle;
    memset(t, 0, sizeof t);
    for (i = 1; i < 4; i++) { /* read, write, edit */
        t[i].def.source = PT_S("builtin");
        t[i].ud = b;
    }
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
    pt_rt_bash_tools(b->bash, bt);
    t[0] = bt[0];
    t[4] = bt[1];
    pt_rt_search_tools(b->search, t + 5);
    pt_rt_web_tool(b->web, t + 8);
    for (i = 0; i < 9; i++)
        if (!pt_rt_tool_add(rt, &t[i]))
            return false;
    return true;
}
