/* Rewind, the host's side (section 8.6).
 *
 * Before write or edit changes a file, the core asks to have it saved
 * (SNAPSHOT): its bytes go into a blob of the session, which the tool
 * task's intent then names. Rewinding reads the log back for those
 * snapshots (pt_rt_rewind_scan), works out which files differ from what
 * they were before a user message (pt_rt_rewind_plan), and the core's
 * rewind task puts them back one by one (RESTORE).
 *
 * The scan reads the log itself: done tool tasks leave the core's memory,
 * and a long session's older epochs are not kept there either. */
#define _POSIX_C_SOURCE 200809L
#include "rt/rt_int.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SNAPSHOT_MAX (256u << 20) /* bytes of one file: a snapshot is a copy in the session */

/* An absolute path: relative ones are under the session's directory, the
 * way the tools resolve them (the "reads:" stamps are keyed by the result). */
static bool resolve(pt_rt *rt, const char *path, pt_buf *out)
{
    pt_str cwd = pt_session_cwd(pt_rt_agent(rt));
    pt_buf_reset(out);
    if (path[0] != '/' && cwd.n) {
        pt_buf_puts(out, cwd);
        pt_buf_putc(out, '/');
    }
    pt_buf_putz(out, path);
    pt_buf_putc(out, 0);
    out->n--;
    return !out->oom;
}

/* ---- SNAPSHOT ---- */

static void snapshot_fail(pt_rt *rt, const pt_fx *fx, const char *path, const char *why)
{
    char msg[4400];
    snprintf(msg, sizeof msg, "%s: %s", path, why);
    pt_fx_snapshot(rt->a, fx->id, NULL, msg);
}

void pt_rt_snapshot(pt_rt *rt, const pt_fx *fx)
{
    pt_store *st = rt->c.store;
    pt_arena ar;
    const pt_json *args;
    pt_str rel;
    pt_buf path;
    pt_snapshot s;
    struct stat sb;
    char ref[72], chunk[16384];
    int fd;
    void *h;
    pt_sha256 sha;
    unsigned char d[32];
    uint64_t total = 0;
    pt_arena_init(&ar, pt_sys_allocator(), 1024);
    pt_buf_init(&path, pt_sys_allocator());
    args = pt_json_parse(fx->args, &ar, NULL);
    rel = pt_json_gets(args, "path", &ar);
    memset(&s, 0, sizeof s);
    if (!rel.n) { /* the call will say what is wrong; it changes no file */
        pt_fx_snapshot(rt->a, fx->id, &s, NULL);
        goto out;
    }
    if (!resolve(rt, pt_arena_cstr(&ar, rel), &path) || ar.oom) {
        pt_fx_snapshot(rt->a, fx->id, NULL, "out of memory");
        goto out;
    }
    s.path = pt_buf_str(&path);
    if (stat(path.p, &sb) != 0) {
        if (errno == ENOENT) { /* it will be created: rewinding removes it */
            pt_fx_snapshot(rt->a, fx->id, &s, NULL);
        } else {
            snapshot_fail(rt, fx, path.p, strerror(errno));
        }
        goto out;
    }
    if (!S_ISREG(sb.st_mode) || sb.st_size < 0 || (uint64_t)sb.st_size > SNAPSHOT_MAX) {
        snapshot_fail(rt, fx, path.p, S_ISREG(sb.st_mode) ? "too large to save" : "not a regular file");
        goto out;
    }
    if (!st->blob_begin || !(h = st->blob_begin(st))) {
        snapshot_fail(rt, fx, path.p, "the session keeps no blobs");
        goto out;
    }
    if ((fd = open(path.p, O_RDONLY | O_CLOEXEC)) < 0) {
        st->blob_abort(st, h);
        snapshot_fail(rt, fx, path.p, strerror(errno));
        goto out;
    }
    pt_sha256_init(&sha);
    for (;;) { /* in pieces: the file is not held whole */
        ssize_t got = read(fd, chunk, sizeof chunk);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0) {
            if (got < 0)
                total = UINT64_MAX;
            break;
        }
        total += (uint64_t)got;
        pt_sha256_update(&sha, chunk, (size_t)got);
        if (total > SNAPSHOT_MAX || st->blob_write(st, h, chunk, (size_t)got) != 0) {
            total = UINT64_MAX;
            break;
        }
    }
    close(fd);
    if (total == UINT64_MAX) {
        st->blob_abort(st, h);
        snapshot_fail(rt, fx, path.p, "could not be read and stored");
        goto out;
    }
    pt_sha256_final(&sha, d);
    memcpy(ref, "sha256:", 7);
    pt_hex(ref + 7, d, 32);
    ref[71] = 0;
    if (st->blob_commit(st, h, ref + 7) != 0) {
        snapshot_fail(rt, fx, path.p, st->err[0] ? st->err : "could not store it");
        goto out;
    }
    s.blob = pt_strz(ref);
    pt_fx_snapshot(rt->a, fx->id, &s, NULL);
out:
    pt_buf_free(&path);
    pt_arena_free(&ar);
}

/* ---- RESTORE ---- */

void pt_rt_restore(pt_rt *rt, const pt_fx *fx)
{
    pt_buf path;
    pt_arena ar;
    pt_str bytes = PT_STR_NULL;
    const char *err = NULL;
    pt_buf_init(&path, pt_sys_allocator());
    pt_buf_puts(&path, fx->path);
    pt_buf_putc(&path, 0);
    path.n--;
    if (path.oom) {
        pt_fx_done(rt->a, fx->id, "out of memory");
        pt_buf_free(&path);
        return;
    }
    if (!fx->blob.n) { /* it did not exist */
        if (unlink(path.p) != 0 && errno != ENOENT)
            err = strerror(errno);
    } else {
        pt_arena_init(&ar, pt_sys_allocator(), 4096);
        if (!pt_rt_read_blob(rt->c.store, fx->blob, &ar, &bytes))
            err = "its saved copy cannot be read";
        else if (!pt_rt_mkdirs(path.p) || !pt_rt_write_atomic(path.p, bytes.p ? bytes.p : "", bytes.n, NULL))
            err = strerror(errno);
        pt_arena_free(&ar);
    }
    pt_fx_done(rt->a, fx->id, err);
    pt_buf_free(&path);
}

/* ---- the scan: what the log says ---- */

typedef struct snap_rec {
    uint32_t entry; /* the assistant message that made the call */
    pt_str path, blob;
} snap_rec;

struct pt_rt_rewind {
    pt_rt_point *points;
    uint32_t npoints, pcap;
    snap_rec *snaps;
    uint32_t nsnaps, scap;
    uint32_t *risky; /* entries of results of calls rewind cannot undo, in order */
    uint32_t nrisky, rcap;
    uint32_t *inputs; /* inputs seen in earlier lines */
    uint32_t ninputs, icap;
    pt_arena ar; /* strings of all of the above */
    pt_arena tmp; /* one line */
    pt_rt *rt;
};

static bool grow(void **arr, uint32_t *cap, uint32_t need, size_t elem)
{
    void *p;
    uint32_t ncap = *cap ? *cap * 2 : 32;
    if (need <= *cap)
        return true;
    if (ncap < need)
        ncap = need;
    if (ncap > SIZE_MAX / elem || !(p = realloc(*arr, (size_t)ncap * elem)))
        return false;
    *arr = p;
    *cap = ncap;
    return true;
}

static bool seen_input(const pt_rt_rewind *r, uint32_t id)
{
    uint32_t i;
    for (i = 0; i < r->ninputs; i++)
        if (r->inputs[i] == id)
            return true;
    return false;
}

/* Whether a call of this tool may have changed things rewind does not track. */
static bool is_risky(pt_rt *rt, pt_str name)
{
    uint32_t i;
    for (i = 0; i < rt->ntools; i++)
        if (pt_str_eq(rt->tools[i].def.name, name))
            return !rt->tools[i].def.read_only && !rt->tools[i].def.snapshot && !rt->tools[i].no_ask;
    return pt_str_starts(name, PT_S("mcp__")); /* a server that is not running */
}

static uint32_t id_of(const pt_json *o, const char *key)
{
    int64_t v = pt_json_geti(o, key, 0);
    return v > 0 && v <= (int64_t)UINT32_MAX ? (uint32_t)v : 0;
}

/* One commit line, at byte offset `at` of the log. */
static bool scan_line(pt_rt_rewind *r, pt_str line, uint64_t at)
{
    const pt_json *root = pt_json_parse(line, &r->tmp, NULL), *rec;
    if (!root || !pt_json_is(pt_json_get(root, "w"), PT_J_ARR))
        return false;
    for (rec = pt_json_get(root, "w")->child; rec; rec = rec->next) {
        pt_str type = pt_json_gets(rec, "t", &r->tmp), kind = pt_json_gets(rec, "kind", &r->tmp);
        const pt_json *data = pt_json_get(rec, "data"), *st = pt_json_get(rec, "state");
        if (pt_str_eqz(type, "entry") && pt_str_eqz(kind, "user") && !id_of(rec, "kept") &&
            !pt_str_eqz(pt_json_gets(data, "from", &r->tmp), "pytte")) {
            const pt_json *c = pt_json_get(data, "content");
            pt_rt_point *p;
            if (!grow((void **)&r->points, &r->pcap, r->npoints + 1, sizeof *r->points))
                return false;
            p = &r->points[r->npoints];
            memset(p, 0, sizeof *p);
            p->entry = id_of(rec, "id");
            p->keep = at;
            p->queued = seen_input(r, id_of(data, "input"));
            if (pt_json_is(c, PT_J_OBJ)) { /* a long prompt is a blob */
                if (!pt_rt_read_blob(r->rt->c.store, pt_json_gets(c, "blob", &r->tmp), &r->ar, &p->text))
                    return false;
            } else {
                p->text = pt_arena_dup(&r->ar, pt_json_str(c, &r->tmp));
            }
            if (!p->entry || r->ar.oom)
                return false;
            r->npoints++;
        } else if (pt_str_eqz(type, "entry") && pt_str_eqz(kind, "tool_result")) {
            if (is_risky(r->rt, pt_json_gets(data, "name", &r->tmp))) {
                if (!grow((void **)&r->risky, &r->rcap, r->nrisky + 1, sizeof *r->risky))
                    return false;
                r->risky[r->nrisky++] = id_of(rec, "id");
            }
        } else if (pt_str_eqz(type, "task") && pt_str_eqz(kind, "tool") && pt_json_is(st, PT_J_OBJ) &&
                   pt_str_eqz(pt_json_gets(st, "phase", &r->tmp), "execute") &&
                   pt_json_geti(st, "attempt", 0) == 1 && pt_json_is(pt_json_get(st, "snap"), PT_J_OBJ)) {
            const pt_json *snap = pt_json_get(st, "snap");
            snap_rec *s;
            if (!grow((void **)&r->snaps, &r->scap, r->nsnaps + 1, sizeof *r->snaps))
                return false;
            s = &r->snaps[r->nsnaps++];
            s->entry = id_of(st, "entry");
            s->path = pt_arena_dup(&r->ar, pt_json_gets(snap, "path", &r->tmp));
            s->blob = pt_arena_dup(&r->ar, pt_json_gets(snap, "blob", &r->tmp));
            if (!s->entry || !s->path.n || r->ar.oom)
                return false;
        }
    }
    /* inputs of this line count as earlier for the lines that follow */
    for (rec = pt_json_get(root, "w")->child; rec; rec = rec->next) {
        uint32_t id = id_of(rec, "id");
        if (pt_str_eqz(pt_json_gets(rec, "t", &r->tmp), "input") &&
            pt_str_eqz(pt_json_gets(rec, "kind", &r->tmp), "user") && id && !seen_input(r, id)) {
            if (!grow((void **)&r->inputs, &r->icap, r->ninputs + 1, sizeof *r->inputs))
                return false;
            r->inputs[r->ninputs++] = id;
        }
    }
    return !r->tmp.oom;
}

void pt_rt_rewind_free(pt_rt_rewind *r)
{
    if (!r)
        return;
    free(r->points);
    free(r->snaps);
    free(r->risky);
    free(r->inputs);
    pt_arena_free(&r->ar);
    pt_arena_free(&r->tmp);
    free(r);
}

pt_rt_rewind *pt_rt_rewind_scan(pt_rt *rt, char *err, size_t cap)
{
    pt_store *st = rt->c.store;
    pt_rt_rewind *r = (pt_rt_rewind *)calloc(1, sizeof *r);
    pt_buf line;
    char chunk[16384];
    uint64_t off = 0, start = 0;
    size_t got;
    bool ok = true;
    if (!r) {
        snprintf(err, cap, "out of memory");
        return NULL;
    }
    r->rt = rt;
    pt_arena_init(&r->ar, pt_sys_allocator(), 4096);
    pt_arena_init(&r->tmp, pt_sys_allocator(), 4096);
    pt_buf_init(&line, pt_sys_allocator());
    while (ok && st->read(st, off, chunk, sizeof chunk, &got) == 0 && got) {
        size_t i = 0;
        while (ok && i < got) {
            const char *nl = (const char *)memchr(chunk + i, '\n', got - i);
            size_t take = nl ? (size_t)(nl - (chunk + i)) : got - i;
            pt_buf_put(&line, chunk + i, take);
            i += take;
            if (nl) {
                i++;
                pt_arena_reset(&r->tmp);
                ok = !line.oom && scan_line(r, pt_buf_str(&line), start);
                start = off + i;
                pt_buf_reset(&line);
            }
        }
        off += got;
    }
    pt_buf_free(&line);
    if (!ok) {
        snprintf(err, cap, "cannot read the session's log");
        pt_rt_rewind_free(r);
        return NULL;
    }
    return r;
}

const pt_rt_point *pt_rt_rewind_points(const pt_rt_rewind *r, uint32_t *n)
{
    *n = r->npoints;
    return r->points;
}

/* ---- the plan: what to put back ---- */

/* The SHA-256 of a file as 64 hex digits: 1; 0 when it is not there; -1
 * when it cannot be read. */
static int file_hash(const char *path, char hex[64])
{
    char chunk[16384];
    unsigned char d[32];
    pt_sha256 sha;
    int fd = open(path, O_RDONLY | O_CLOEXEC), rc = 1;
    if (fd < 0)
        return errno == ENOENT ? 0 : -1;
    pt_sha256_init(&sha);
    for (;;) {
        ssize_t got = read(fd, chunk, sizeof chunk);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0) {
            rc = got < 0 ? -1 : 1;
            break;
        }
        pt_sha256_update(&sha, chunk, (size_t)got);
    }
    close(fd);
    pt_sha256_final(&sha, d);
    pt_hex(hex, d, 32);
    return rc;
}

/* Whether the file is no longer as pytte last left it: the stamp of its
 * last read or write (doc "reads:<path>") has a hash the file lacks. No
 * stamp, or one without a hash (a rewind cleared it): unknown, so not. */
static bool changed_outside(pt_rt *rt, pt_str path, pt_arena *ar)
{
    pt_buf key;
    pt_str v;
    const pt_json *j;
    pt_str hash;
    char cur[64], z[4300];
    int rc;
    pt_buf_init(&key, pt_sys_allocator());
    pt_buf_putz(&key, "reads:");
    pt_buf_puts(&key, path);
    v = pt_doc(rt->a, pt_buf_str(&key));
    pt_buf_free(&key);
    j = v.n ? pt_json_parse(v, ar, NULL) : NULL;
    hash = pt_json_gets(j, "hash", ar);
    if (hash.n != 64 || path.n >= sizeof z)
        return false;
    memcpy(z, path.p, path.n);
    z[path.n] = 0;
    rc = file_hash(z, cur);
    return rc <= 0 || memcmp(cur, hash.p, 64) != 0;
}

void pt_rt_plan_free(pt_rt_plan *p)
{
    pt_arena_free(&p->ar);
    memset(p, 0, sizeof *p);
}

bool pt_rt_rewind_plan(pt_rt *rt, const pt_rt_rewind *r, uint32_t entry, pt_rt_plan *out)
{
    pt_arena tmp;
    uint32_t i, j, k = 0, nseen = 0;
    pt_str *seen;
    bool found = false;
    memset(out, 0, sizeof *out);
    pt_arena_init(&out->ar, pt_sys_allocator(), 4096);
    pt_arena_init(&tmp, pt_sys_allocator(), 1024);
    for (i = 0; i < r->npoints; i++)
        found = found || r->points[i].entry == entry;
    if (!found)
        return false;
    for (i = 0; i < r->nrisky; i++)
        out->risky += r->risky[i] > entry;
    out->files = (pt_restore *)pt_arena_zalloc(&out->ar, sizeof(pt_restore) * (r->nsnaps ? r->nsnaps : 1));
    out->changed = (bool *)pt_arena_zalloc(&out->ar, sizeof(bool) * (r->nsnaps ? r->nsnaps : 1));
    seen = (pt_str *)pt_arena_zalloc(&tmp, sizeof(pt_str) * (r->nsnaps ? r->nsnaps : 1));
    if (!out->files || !out->changed || !seen)
        goto fail;
    for (i = 0; i < r->nsnaps; i++) { /* the oldest snapshot of each file after the message */
        const snap_rec *s = &r->snaps[i];
        char z[4300], hex[64];
        bool dup = false;
        int cur;
        if (s->entry <= entry)
            continue;
        for (j = 0; j < nseen && !dup; j++)
            dup = pt_str_eq(seen[j], s->path);
        if (dup || s->path.n >= sizeof z)
            continue;
        seen[nseen++] = s->path; /* seen, whether or not it needs restoring */
        memcpy(z, s->path.p, s->path.n);
        z[s->path.n] = 0;
        cur = file_hash(z, hex);
        if (cur == 0 && !s->blob.n)
            continue; /* it is not there, as it was not */
        if (cur == 1 && s->blob.n == 71 && memcmp(s->blob.p + 7, hex, 64) == 0)
            continue; /* it is as it was */
        out->files[k].path = pt_arena_dup(&out->ar, s->path);
        out->files[k].blob = pt_arena_dup(&out->ar, s->blob);
        out->changed[k] = changed_outside(rt, s->path, &tmp);
        out->nchanged += out->changed[k];
        k++;
    }
    out->n = k;
    if (out->ar.oom)
        goto fail;
    pt_arena_free(&tmp);
    return true;
fail:
    pt_arena_free(&tmp);
    pt_rt_plan_free(out);
    return false;
}
