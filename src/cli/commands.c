/* pytte's subcommands besides the TUI and print mode: log, ls,
 * update-catalog and mcp, and loading the MCP servers each mode runs. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cli/main.h"

/* ---- pytte log: read only; works while another process owns the session ---- */

static void print_entry(const pt_entry_view *v, bool tty)
{
    const pt_msg *m = v->msg;
    const char *dim = tty ? "\x1b[2m" : "", *reset = tty ? "\x1b[0m" : "";
    uint32_t i;
    if (strcmp(v->kind, "epoch") == 0) {
        printf("%s[epoch %u] %.*s%s\n\n", dim, v->epoch, (int)m->text.n, m->text.p, reset);
        if (v->summary.n)
            printf("%s[summary] %.*s%s\n\n", dim, (int)v->summary.n, v->summary.p, reset);
        if (v->files.n)
            printf("%s%.*s%s\n", dim, (int)v->files.n, v->files.p, reset);
    } else if (strcmp(v->kind, "user") == 0) {
        printf("%s> %.*s%s\n\n", tty ? "\x1b[1m" : "", (int)m->text.n, m->text.p, reset);
    } else if (strcmp(v->kind, "assistant") == 0) {
        if (m->stop == PT_STOP_ABORTED || m->stop == PT_STOP_ERROR)
            printf("%s[%s, not sent to the model]%s\n", dim, pt_stop_name(m->stop), reset);
        for (i = 0; i < m->nblocks; i++) {
            const pt_block *b = &m->blocks[i];
            if (b->kind == PT_BLOCK_TEXT)
                printf("%.*s\n", (int)b->text.n, b->text.p);
            else if (b->kind == PT_BLOCK_THINKING && b->text.n)
                printf("%s%.*s%s\n", dim, (int)b->text.n, b->text.p, reset);
            else if (b->kind == PT_BLOCK_TOOL_CALL)
                printf("-> %.*s %.*s\n", (int)b->name.n, b->name.p, (int)b->args.n, b->args.p);
        }
        printf("\n");
    } else if (strcmp(v->kind, "tool_result") == 0) {
        printf("%s<- %.*s%s: %.*s%s\n\n", dim, (int)m->name.n, m->name.p, m->is_error ? " (error)" : "",
               (int)m->text.n, m->text.p, reset);
    } else if (strcmp(v->kind, "error") == 0) {
        pt_arena ar;
        pt_json *j;
        pt_arena_init(&ar, pt_sys_allocator(), 0);
        j = pt_json_parse(v->raw, &ar, NULL);
        {
            const pt_json *d = pt_json_get(j, "data");
            pt_str msg = pt_json_gets(d, "message", &ar);
            printf("%s[error, attempt %lld] %.*s%s\n\n", dim, (long long)pt_json_geti(d, "attempt", 0),
                   (int)msg.n, msg.p, reset);
        }
        pt_arena_free(&ar);
    } else if (m) {
        printf("%s[%s] %.*s%s\n\n", dim, v->kind, (int)m->text.n, m->text.p, reset);
    }
}

#define LOG_CHUNK 65536

/* Load a log into a, a chunk at a time: memory for one chunk and the
 * longest line. 0, or 20 with the message printed. */
static int load_log(pt_agent *a, const char *id, const char *path)
{
    char *chunk = (char *)malloc(LOG_CHUNK);
    pt_buf line; /* a line that spans chunks */
    int fd = open(path, O_RDONLY | O_CLOEXEC), code = 0;
    struct stat st;
    uint64_t off = 0;
    bool torn = false;
    if (!chunk || fd < 0 || fstat(fd, &st) != 0) {
        fprintf(stderr, "pytte: cannot read %s: %s\n", path, chunk ? strerror(errno) : "out of memory");
        free(chunk);
        if (fd >= 0)
            close(fd);
        return 20;
    }
    pt_buf_init(&line, pt_sys_allocator());
    while (!torn && !code) {
        ssize_t got = read(fd, chunk, LOG_CHUNK);
        size_t i, from = 0;
        if (got < 0 && errno == EINTR)
            continue;
        if (got == 0)
            break;
        if (got < 0) {
            fprintf(stderr, "pytte: cannot read %s: %s\n", path, strerror(errno));
            code = 20;
            break;
        }
        for (i = 0; i < (size_t)got; i++) {
            pt_load_result r;
            pt_str l;
            if (chunk[i] != '\n')
                continue;
            if (line.n) {
                pt_buf_put(&line, chunk + from, i - from);
                l = pt_buf_str(&line);
            } else {
                l = (pt_str){chunk + from, i - from};
            }
            r = line.oom ? PT_LOAD_CORRUPT : pt_load_line(a, l, true, off + i + 1 >= (uint64_t)st.st_size);
            pt_buf_reset(&line);
            from = i + 1;
            if (r == PT_LOAD_CORRUPT) {
                fprintf(stderr, "pytte: %s: %s\n", id, line.oom ? "out of memory" : pt_error(a));
                code = 20;
                break;
            }
            if (r == PT_LOAD_TORN) { /* being written right now, or torn: not committed */
                torn = true;
                break;
            }
        }
        if (!torn && !code && from < (size_t)got)
            pt_buf_put(&line, chunk + from, (size_t)got - from);
        off += (uint64_t)got;
    }
    close(fd);
    free(chunk);
    pt_buf_free(&line);
    return code;
}

/* pt_opts.blob for a reader: blob_ud is the session directory. The
 * owner's lock is left alone, as for the log. */
static bool read_blob(void *dir, pt_str ref, pt_arena *ar, pt_str *out)
{
    char path[4300];
    char *p;
    size_t n, i;
    if (ref.n != 71 || memcmp(ref.p, "sha256:", 7) != 0)
        return false;
    for (i = 7; i < ref.n; i++)
        if (!((ref.p[i] >= '0' && ref.p[i] <= '9') || (ref.p[i] >= 'a' && ref.p[i] <= 'f')))
            return false;
    snprintf(path, sizeof path, "%s/blobs/%.2s/%.62s", (const char *)dir, ref.p + 7, ref.p + 9);
    if (!(p = pt_sys_read_file(path, &n)))
        return false;
    *out = pt_arena_dup(ar, (pt_str){p, n});
    free(p);
    return !ar->oom;
}

int pytte_log(const opts *o)
{
    char root[4096], id[64], dir[4200], path[4300];
    pt_opts ao;
    pt_agent *a;
    pt_entry_view v;
    bool tty = isatty(STDOUT_FILENO);
    uint32_t i;
    int code;
    cli_sessions_dir(root, sizeof root);
    if (o->id)
        snprintf(id, sizeof id, "%s", o->id);
    else if (!cli_sessions_list(NULL, false, &id, 1))
        die("no sessions in", root);
    snprintf(dir, sizeof dir, "%s/%s", root, id);
    snprintf(path, sizeof path, "%s/log.jsonl", dir);
    memset(&ao, 0, sizeof ao);
    ao.alloc = pt_sys_allocator();
    ao.all_epochs = o->all_epochs;
    ao.blob = read_blob;
    ao.blob_ud = dir;
    a = pt_new(&ao);
    if (!a) {
        fprintf(stderr, "pytte: out of memory loading %s\n", path);
        return 20;
    }
    if ((code = load_log(a, id, path)) != 0) {
        pt_free(a);
        return code;
    }
    for (i = 0; pt_entry_at(a, i, &v); i++) {
        if (o->json) {
            fwrite(v.raw.p, 1, v.raw.n, stdout);
            fputc('\n', stdout);
        } else if (!(o->all_epochs && v.kept)) { /* printed as the original */
            print_entry(&v, tty);
        }
    }
    pt_free(a);
    return 0;
}

/* ---- pytte rewind (section 8.6) ---- */

typedef struct rewound {
    bool done, failed;
    char err[600];
} rewound;

static void rewind_event(void *ud, const pt_event *ev)
{
    rewound *r = (rewound *)ud;
    if (ev->kind != PT_E_REWIND)
        return;
    r->done = true;
    r->failed = pt_str_eqz(ev->outcome, "failed");
    snprintf(r->err, sizeof r->err, "%.*s", (int)ev->error.n, ev->error.p);
}

/* The first line of a message, cut to fit one line of the listing. */
static void first_line(pt_str text, char *out, size_t cap)
{
    size_t n = 0;
    while (n < text.n && text.p[n] != '\n')
        n++;
    if (n >= cap) {
        n = cap - 1;
        while (n && ((unsigned char)text.p[n] & 0xC0) == 0x80)
            n--;
    }
    memcpy(out, text.p, n);
    out[n] = 0;
}

int pytte_rewind(opts *o, cli_config *cfg)
{
    cli_session s;
    rewound rw;
    pt_rt_rewind *scan;
    const pt_rt_point *pts, *p = NULL;
    pt_rt_plan plan;
    pt_buf text;
    char err[700], line[160], dir[4300], newid[64], draft[4400];
    uint32_t n, i, at = 0;
    int rc;
    memset(&rw, 0, sizeof rw);
    apply_config(o, cfg, false);
    if ((rc = cli_session_open(&s, &o->c, o->id, rewind_event, &rw, err, sizeof err)) != 0) {
        fprintf(stderr, "pytte: %s\n", err);
        return rc;
    }
    if (!(scan = pt_rt_rewind_scan(s.rt, err, sizeof err))) {
        fprintf(stderr, "pytte: %s\n", err);
        cli_session_close(&s);
        return 20;
    }
    pts = pt_rt_rewind_points(scan, &n);
    if (o->at) {
        uint64_t v;
        if (!pt_str_to_u64(pt_strz(o->at), &v) || v > UINT32_MAX)
            v = 0;
        for (i = 0; i < n; i++)
            if (pts[i].entry == v)
                p = &pts[i];
    } else if (n && !pts[n - 1].queued) {
        p = &pts[n - 1];
    }
    if (!p || p->queued) {
        fprintf(stderr, "pytte: %s; the messages:\n", p ? "that message was queued behind a run and cannot be rewound to"
                                                     : o->at ? "no such user message" : "no message to rewind to");
        for (i = 0; i < n; i++) {
            first_line(pts[i].text, line, sizeof line);
            fprintf(stderr, "%6u  %s%s\n", pts[i].entry, line, pts[i].queued ? "  (queued: cannot)" : "");
        }
        pt_rt_rewind_free(scan);
        cli_session_close(&s);
        return 1;
    }
    at = p->entry;
    first_line(p->text, line, sizeof line);
    fprintf(stderr, "pytte: back to before entry %u: %s\n", at, line);
    pt_buf_init(&text, pt_sys_allocator());
    pt_buf_puts(&text, p->text);
    if (!pt_rt_rewind_plan(s.rt, scan, at, &plan)) {
        fprintf(stderr, "pytte: cannot plan the rewind\n");
        pt_buf_free(&text);
        pt_rt_rewind_free(scan);
        cli_session_close(&s);
        return 1;
    }
    if (!o->conversation_only) {
        for (i = 0; i < plan.n; i++)
            fprintf(stderr, "  %s %.*s%s\n", plan.files[i].blob.n ? "restore" : "remove ", (int)plan.files[i].path.n,
                    plan.files[i].path.p, plan.changed[i] ? "  (changed outside pytte)" : "");
        if (!plan.n)
            fprintf(stderr, "  no file differs from before it\n");
        if (plan.risky)
            fprintf(stderr, "pytte: %u call%s of bash or MCP tools since then cannot be undone\n", plan.risky,
                    plan.risky == 1 ? "" : "s");
        if (plan.nchanged && !o->yes) {
            char answer[16];
            if (!isatty(STDIN_FILENO)) {
                fprintf(stderr, "pytte: %u file%s changed outside pytte since pytte last wrote %s; --yes overwrites\n",
                        plan.nchanged, plan.nchanged == 1 ? "" : "s", plan.nchanged == 1 ? "it" : "them");
                rc = 1;
                goto out;
            }
            fprintf(stderr, "overwrite the %u file%s changed outside pytte? [y/N] ", plan.nchanged,
                    plan.nchanged == 1 ? "" : "s");
            if (!fgets(answer, sizeof answer, stdin) || (answer[0] != 'y' && answer[0] != 'Y')) {
                fprintf(stderr, "pytte: nothing was changed\n");
                rc = 1;
                goto out;
            }
        }
        if (plan.n) {
            if (!pt_rewind(s.a, at, plan.files, plan.n)) {
                fprintf(stderr, "pytte: %s (finish its work first: pytte -r %s)\n", pt_error(s.a), s.id);
                rc = 1;
                goto out;
            }
            if (pt_rt_run(s.rt) != PT_RT_IDLE || !rw.done) {
                fprintf(stderr, "pytte: %s\n", pt_fatal(s.a) ? pt_error(s.a) : "the rewind did not finish");
                rc = 20;
                goto out;
            }
            if (rw.failed) {
                fprintf(stderr, "pytte: not every file was restored: %s\n", rw.err);
                rc = 10;
                goto out;
            }
            fprintf(stderr, "pytte: restored %u file%s\n", plan.n, plan.n == 1 ? "" : "s");
        }
    }
    rc = 0;
    if (!o->files_only) {
        snprintf(dir, sizeof dir, "%s", s.dir);
        cli_session_close(&s);
        if (!cli_session_fork(dir, p->keep, newid, sizeof newid, err, sizeof err)) {
            fprintf(stderr, "pytte: %s\n", err);
            rc = 1;
            goto out_closed;
        }
        {
            char root[4096];
            cli_sessions_dir(root, sizeof root);
            snprintf(draft, sizeof draft, "%s/%s/draft", root, newid);
        }
        pt_sys_write_file(draft, text.p, text.n); /* the message is back in the input */
        printf("%s\n", newid);
        fprintf(stderr, "pytte: continue with: pytte -r %s\n", newid);
        goto out_closed;
    }
out:
    cli_session_close(&s);
out_closed:
    pt_rt_plan_free(&plan);
    pt_buf_free(&text);
    pt_rt_rewind_free(scan);
    return rc;
}

/* ---- pytte ls ---- */

#define LS_PEEK 65536 /* ls reads this much of each log's head and tail */

/* The commit lines in p[0..n), whole lines only; fn returns false to stop. */
static void each_line(const char *p, size_t n, bool (*fn)(void *, pt_str), void *ud)
{
    const char *end = p + n, *nl;
    while (p < end && (nl = (const char *)memchr(p, '\n', (size_t)(end - p)))) {
        if (!fn(ud, (pt_str){p, (size_t)(nl - p)}))
            return;
        p = nl + 1;
    }
}

static bool contains(pt_str s, const char *needle)
{
    size_t n = strlen(needle), i;
    for (i = 0; i + n <= s.n; i++)
        if (s.p[i] == needle[0] && memcmp(s.p + i, needle, n) == 0)
            return true;
    return false;
}

typedef struct ls_row {
    pt_arena ar; /* reset per session */
    pt_str cwd, title, title_blob; /* title_blob: a large prompt's ref */
    const pt_json *claim;          /* the last claim seen */
} ls_row;

/* Head: the session record (cwd) and the first user entry (title). */
static bool ls_head(void *ud, pt_str line)
{
    ls_row *r = (ls_row *)ud;
    const pt_json *c;
    if (!contains(line, "\"t\":\"session\"") && !contains(line, "\"kind\":\"user\""))
        return true;
    c = pt_json_get(pt_json_parse(line, &r->ar, NULL), "w");
    for (c = c ? c->child : NULL; c; c = c->next) {
        pt_str t = pt_json_gets(c, "t", &r->ar);
        if (pt_str_eqz(t, "session"))
            r->cwd = pt_arena_dup(&r->ar, pt_json_gets(c, "cwd", &r->ar));
        else if (pt_str_eqz(t, "entry") && pt_str_eqz(pt_json_gets(c, "kind", &r->ar), "user") &&
                   !r->title.p) {
            const pt_json *content = pt_json_get(pt_json_get(c, "data"), "content");
            if (pt_json_is(content, PT_J_OBJ))
                r->title_blob = pt_json_gets(content, "blob", &r->ar);
            /* copied: the tail is read into the same buffer */
            r->title_blob = pt_arena_dup(&r->ar, r->title_blob);
            r->title = pt_arena_dup(&r->ar, pt_json_str(content, &r->ar));
        }
    }
    return !r->title.p;
}

/* Tail: the last claim (the tail starts mid-line; that line is skipped). */
static bool ls_tail(void *ud, pt_str line)
{
    ls_row *r = (ls_row *)ud;
    const pt_json *c;
    if (!contains(line, "\"t\":\"claim\""))
        return true;
    c = pt_json_get(pt_json_parse(line, &r->ar, NULL), "w");
    for (c = c ? c->child : NULL; c; c = c->next)
        if (pt_str_eqz(pt_json_gets(c, "t", &r->ar), "claim"))
            r->claim = c;
    return true;
}

/* From the last claim. Closed by its owner: idle (nothing left to do),
 * waiting (for a tool call's approval) or closed (a run is unfinished;
 * resuming continues it). running: an open claim of a live process on
 * this machine (the lock is left alone: taking it, even shared, could
 * refuse an owner starting that moment). crashed: an open claim of a
 * process that is gone. */
static void ls_status(const ls_row *r, char *out, size_t cap)
{
    char host[256], boot[64];
    int64_t me;
    pt_str event, h, b;
    pt_arena *ar = (pt_arena *)&r->ar;
    int64_t pid = pt_json_geti(r->claim, "pid", 0);
    if (!r->claim) {
        snprintf(out, cap, "crashed");
        return;
    }
    event = pt_json_gets(r->claim, "event", ar);
    if (pt_str_eqz(event, "close")) {
        pt_str state = pt_json_gets(r->claim, "state", ar);
        snprintf(out, cap, "%s", pt_str_eqz(state, "idle")      ? "idle"
                                 : pt_str_eqz(state, "waiting") ? "waiting"
                                                                : "closed");
        return;
    }
    pt_sys_identity(host, sizeof host, boot, sizeof boot, &me);
    h = pt_json_gets(r->claim, "host", ar);
    b = pt_json_gets(r->claim, "boot", ar);
    if (!pt_str_eqz(h, host))
        snprintf(out, cap, "open on %.*s", (int)h.n, h.p);
    else if (pt_str_eqz(b, boot) && pid > 0 && (kill((pid_t)pid, 0) == 0 || errno == EPERM))
        snprintf(out, cap, "running (pid %lld)", (long long)pid);
    else
        snprintf(out, cap, "crashed");
}

/* ---- pytte update-catalog ---- */

typedef struct fetch {
    pt_buf body;
    int status;
    bool done;
    char err[300];
} fetch;

static void fetch_head(void *ud, int status, pt_str retry_after)
{
    (void)retry_after;
    ((fetch *)ud)->status = status;
}

static void fetch_body(void *ud, const char *p, size_t n)
{
    fetch *f = (fetch *)ud;
    if (f->body.n + n > ((size_t)64 << 20)) /* models.dev is about 6 MiB */
        f->body.oom = true;
    else
        pt_buf_put(&f->body, p, n);
}

static void fetch_done(void *ud, const char *error)
{
    fetch *f = (fetch *)ud;
    f->done = true;
    if (error)
        snprintf(f->err, sizeof f->err, "%s", error);
}

/* GET url into f->body; false with f->err set. */
static bool fetch_url(pt_http_driver *d, const char *url, fetch *f)
{
    pt_header accept = {"Accept", "application/json"};
    pt_http_req req;
    pt_http_sink sink;
    memset(&req, 0, sizeof req);
    req.method = "GET";
    req.url = url;
    req.headers = &accept;
    req.nheaders = req.hcap = 1;
    sink.ud = f;
    sink.head = fetch_head;
    sink.body = fetch_body;
    sink.done = fetch_done;
    sink.header = NULL;
    if (!d->start(d, &req, &sink)) {
        snprintf(f->err, sizeof f->err, "cannot start the request");
        return false;
    }
    while (!f->done)
        if (d->pump(d, 1000, NULL, 0) < 0) {
            snprintf(f->err, sizeof f->err, "the HTTP driver failed");
            return false;
        }
    if (!f->err[0] && f->body.oom)
        snprintf(f->err, sizeof f->err, "response too large, or out of memory");
    else if (!f->err[0] && (f->status < 200 || f->status > 299))
        snprintf(f->err, sizeof f->err, "HTTP %d", f->status);
    return !f->err[0];
}

int pytte_update_catalog(void)
{
    static const char *const urls[2] = {"https://models.dev/api.json", "https://openrouter.ai/api/v1/models"};
    pt_http_driver *d = pt_http_curl_new();
    fetch f[2];
    pt_buf out;
    char path[4096], msg[600], err[600], updated[32];
    time_t now = time(NULL);
    struct tm tm;
    int i, rc = 1;
    if (!d)
        die("cannot start libcurl", NULL);
    memset(f, 0, sizeof f);
    pt_buf_init(&out, pt_sys_allocator());
    for (i = 0; i < 2; i++) {
        pt_buf_init(&f[i].body, pt_sys_allocator());
        if (!fetch_url(d, urls[i], &f[i])) {
            fprintf(stderr, "pytte: %s: %s\n", urls[i], f[i].err);
            goto done;
        }
    }
    gmtime_r(&now, &tm);
    strftime(updated, sizeof updated, "%Y-%m-%dT%H:%M:%SZ", &tm);
    cli_catalog_path(path, sizeof path);
    if (!cli_catalog_convert(pt_buf_str(&f[0].body), pt_buf_str(&f[1].body), updated, &out, msg, sizeof msg)) {
        fprintf(stderr, "pytte: %s\n", msg);
    } else if (!cli_write_atomic(path, pt_buf_str(&out), err, sizeof err)) {
        fprintf(stderr, "pytte: %s\n", err);
    } else {
        printf("wrote %s: %s\n", path, msg);
        rc = 0;
    }
done:
    for (i = 0; i < 2; i++)
        pt_buf_free(&f[i].body);
    pt_buf_free(&out);
    d->free(d);
    return rc;
}

int pytte_ls(void)
{
    char root[4096], path[4200], status[300], name[CLI_NAME_MAX + 1], *peek;
    char(*ids)[64];
    const char *home = getenv("HOME");
    size_t hn = home ? strlen(home) : 0;
    uint32_t n, i;
    ls_row r;
    pt_buf first; /* each log's first line, reused */
    cli_sessions_dir(root, sizeof root);
    ids = (char(*)[64])malloc(4096 * sizeof *ids);
    peek = (char *)malloc(LS_PEEK);
    if (!ids || !peek)
        die("out of memory", NULL);
    n = cli_sessions_list(NULL, false, ids, 4096);
    pt_arena_init(&r.ar, pt_sys_allocator(), 0);
    pt_buf_init(&first, pt_sys_allocator());
    for (i = 0; i < n; i++) {
        struct stat st;
        ssize_t got;
        int fd;
        size_t k;
        pt_str t;
        snprintf(path, sizeof path, "%s/%s/log.jsonl", root, ids[i]);
        if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0 || fstat(fd, &st) != 0) {
            if (fd >= 0)
                close(fd);
            continue;
        }
        pt_arena_reset(&r.ar);
        r.title_blob = PT_STR_NULL;
        r.cwd = r.title = PT_STR_NULL;
        r.claim = NULL;
        /* the head: the first line whole (however long its system prompt),
         * then up to LS_PEEK after it */
        if (cli_log_first_line(fd, &first)) {
            ls_head(&r, pt_buf_str(&first));
            got = pread(fd, peek, LS_PEEK, (off_t)first.n + 1);
            if (got > 0 && !r.title.p)
                each_line(peek, (size_t)got, ls_head, &r);
        }
        got = pread(fd, peek, LS_PEEK, st.st_size > LS_PEEK ? st.st_size - LS_PEEK : 0);
        if (got > 0 && st.st_size > LS_PEEK && (t.p = (const char *)memchr(peek, '\n', (size_t)got)))
            each_line(t.p + 1, (size_t)got - (size_t)(t.p + 1 - peek), ls_tail, &r);
        else if (got > 0 && st.st_size <= LS_PEEK)
            each_line(peek, (size_t)got, ls_tail, &r);
        close(fd);
        if (r.title_blob.n == 71) { /* a large prompt: its first bytes */
            char bp[4400];
            int bf;
            snprintf(bp, sizeof bp, "%s/%s/blobs/%.2s/%.62s", root, ids[i], r.title_blob.p + 7,
                     r.title_blob.p + 9);
            if ((bf = open(bp, O_RDONLY | O_CLOEXEC)) >= 0) {
                char *head = (char *)pt_arena_alloc(&r.ar, 256);
                got = head ? pread(bf, head, 256, 0) : -1;
                if (got > 0)
                    r.title = (pt_str){head, (size_t)got};
                close(bf);
            }
        }
        snprintf(path, sizeof path, "%s/%s", root, ids[i]);
        if (cli_session_name(path, name, sizeof name))
            r.title = pt_strz(name);
        ls_status(&r, status, sizeof status);
        /* the first line of the first prompt, cut on a character */
        t = r.title.p ? r.title : PT_S("(no prompts)");
        if (!r.title.p)
            r.title = t;
        for (k = 0; k < t.n && t.p[k] != '\n'; k++)
            ;
        t.n = k;
        if (t.n > 60) {
            t.n = 60;
            while (t.n && ((unsigned char)t.p[t.n] & 0xC0) == 0x80)
                t.n--;
        }
        if (home && hn > 1 && r.cwd.n >= hn && memcmp(r.cwd.p, home, hn) == 0 &&
            (r.cwd.n == hn || r.cwd.p[hn] == '/'))
            printf("%s  %-18s  ~%.*s  %.*s%s\n", ids[i], status, (int)(r.cwd.n - hn), r.cwd.p + hn, (int)t.n,
                   t.p, t.n < r.title.n ? "…" : "");
        else
            printf("%s  %-18s  %.*s  %.*s%s\n", ids[i], status, (int)r.cwd.n, r.cwd.p, (int)t.n, t.p,
                   t.n < r.title.n ? "…" : "");
    }
    pt_arena_free(&r.ar);
    pt_buf_free(&first);
    free(peek);
    free(ids);
    return 0;
}

/* ---- pytte mcp, and the servers every mode loads ---- */

/* The servers configured for this directory: static, not on main's stack
 * (it is some 20 KiB), for the life of the process. */
static cli_mcp mcp;

/* Print mode: what the servers say goes to stderr, but "ready". */
static void mcp_note(void *ud, const char *server, const char *msg)
{
    (void)ud;
    if (strncmp(msg, "ready:", 6) != 0)
        fprintf(stderr, "pytte: mcp %s: %s\n", server ? server : "", msg);
}

cli_mcp *load_mcp(opts *o, bool quiet)
{
    char cwd[4096];
    if (o->no_mcp)
        return NULL;
    cli_mcp_load(&mcp, getcwd(cwd, sizeof cwd) ? cwd : ".");
    if (mcp.warn[0])
        fprintf(stderr, "pytte: %s\n", mcp.warn);
    if (mcp.nuntrusted && !quiet)
        fprintf(stderr, "pytte: this project's MCP servers are not trusted, so they do not run; "
                        "`pytte mcp trust` runs them\n");
    o->c.mcp = mcp.run;
    o->c.nmcp = mcp.n;
    cli_mcp_host(&o->c.mcp_host, mcp_note, NULL);
    return &mcp;
}

static int64_t bare_now(void *ud)
{
    (void)ud;
    return pt_sys_now_ms();
}

typedef struct bare {
    pt_agent *a;
    pt_rt *rt;
    pt_store *st;
    pt_http_driver *http;
    bool login_ended;
} bare;

static void bare_note(void *ud, const char *server, const char *msg)
{
    bare *b = (bare *)ud;
    if (!strncmp(msg, "logged in", 9) || !strncmp(msg, "login failed", 12))
        b->login_ended = true;
    fprintf(stderr, "%s: %s\n", server ? server : "mcp", msg);
}

/* A runtime without a session, for servers only. */
static bool bare_open(bare *b, const pt_rt_mcp_server *srv, uint32_t n)
{
    pt_opts ao;
    pt_rt_cfg rc;
    pt_rt_mcp_host h;
    memset(b, 0, sizeof *b);
    memset(&ao, 0, sizeof ao);
    ao.alloc = pt_sys_allocator();
    ao.now_ms = bare_now;
    ao.random = pt_sys_random;
    ao.catalog = cli_catalog();
    memset(&rc, 0, sizeof rc);
    rc.store = b->st = pt_store_memory_new();
    rc.http = b->http = pt_http_curl_new();
    rc.catalog = cli_catalog();
    if (!b->st || !b->http || !(b->a = pt_new(&ao)) || !(b->rt = pt_rt_new(&rc, b->a)))
        return false;
    cli_mcp_host(&h, bare_note, b);
    return pt_rt_mcp_setup(b->rt, &h, srv, n);
}

static void bare_close(bare *b)
{
    if (b->rt)
        pt_rt_free(b->rt);
    if (b->a)
        pt_free(b->a);
    if (b->http)
        b->http->free(b->http);
    if (b->st)
        pt_store_memory_free(b->st);
}

/* Step until no server starts, or ms pass. */
static void bare_settle(bare *b, int64_t ms)
{
    int64_t end = pt_sys_now_ms() + ms;
    pt_rt_mcp_info in[CLI_MCP_MAX];
    while (pt_sys_now_ms() < end) {
        uint32_t i, n;
        bool starting = false;
        pt_rt_step(b->rt, 100, NULL, 0);
        n = pt_rt_mcp_servers(b->rt, in, CLI_MCP_MAX);
        for (i = 0; i < n && i < CLI_MCP_MAX; i++)
            starting = starting || in[i].state == PT_MCP_STARTING;
        if (!starting)
            return;
    }
}

static const char *state_name(pt_mcp_state st)
{
    switch (st) {
    case PT_MCP_STARTING:
        return "starting";
    case PT_MCP_READY:
        return "ready";
    case PT_MCP_FAILED:
        return "failed";
    case PT_MCP_AUTH:
        return "needs login";
    default:
        return "stopped";
    }
}

static const pt_rt_mcp_server *find_mcp(const char *name)
{
    uint32_t i;
    for (i = 0; i < mcp.n; i++)
        if (!strcmp(mcp.run[i].name, name))
            return &mcp.run[i];
    return NULL;
}

int pytte_mcp(opts *o)
{
    const char *sub = o->mcp_args[0] ? o->mcp_args[0] : "list", *name = o->mcp_args[1];
    char err[700];
    bare b;
    int rc = 0;
    o->no_mcp = false;
    load_mcp(o, true);
    if (!strcmp(sub, "trust") || !strcmp(sub, "untrust")) {
        if (name)
            die("takes no name: it is about this directory's servers", name);
        if (!cli_mcp_trust(&mcp, sub[0] == 't', err, sizeof err))
            die(err, NULL);
        printf("%s this directory's MCP servers (%s)\n", sub[0] == 't' ? "trusted" : "no longer trusting",
               mcp.project);
        return 0;
    }
    if (!strcmp(sub, "list")) {
        pt_rt_mcp_info in[CLI_MCP_MAX];
        uint32_t i, k = 0;
        if (name)
            die("list takes no name", name);
        if (!mcp.nall) {
            printf("no MCP servers: add them to mcpServers in %s, or to .mcp.json in a project\n",
                   pt_strz(getenv("HOME")).n ? "~/.config/pytte/config.json" : "config.json");
            return 0;
        }
        if (mcp.n) {
            if (!bare_open(&b, mcp.run, mcp.n))
                die("out of memory", NULL);
            bare_settle(&b, 30000);
            pt_rt_mcp_servers(b.rt, in, CLI_MCP_MAX);
        }
        for (i = 0; i < mcp.nall; i++) {
            const pt_rt_mcp_server *s = &mcp.all[i];
            printf("%-20s %-5s %-18s %s\n", s->name, s->url ? "http" : "stdio", mcp.info[i].source,
                   s->url ? s->url : s->command);
            if (!mcp.info[i].runs) {
                printf("%20s not trusted: `pytte mcp trust` runs this project's servers\n", "");
                continue;
            }
            printf("%20s %s", "", state_name(in[k].state));
            if (in[k].state == PT_MCP_READY)
                printf(": %u tools, %u prompts, %u resources", in[k].ntools, in[k].nprompts, in[k].nresources);
            else if (in[k].error && in[k].error[0])
                printf(": %s", in[k].error);
            printf("\n");
            k++;
        }
        if (mcp.n)
            bare_close(&b);
        return 0;
    }
    if (strcmp(sub, "login") && strcmp(sub, "logout"))
        die("usage: pytte mcp [list|trust|untrust|login NAME|logout NAME]", NULL);
    if (!name)
        die("which server?", NULL);
    if (!find_mcp(name))
        die("no such MCP server (or not trusted)", name);
    if (!find_mcp(name)->url)
        die("not a remote server: only those log in", name);
    if (!bare_open(&b, find_mcp(name), 1))
        die("out of memory", NULL);
    if (!strcmp(sub, "logout")) {
        pt_rt_mcp_logout(b.rt, pt_strz(name));
        printf("logged out of %s\n", name);
    } else {
        pt_rt_mcp_info in;
        int64_t end;
        bare_settle(&b, 30000);
        pt_rt_mcp_servers(b.rt, &in, 1);
        if (in.state == PT_MCP_READY) {
            printf("%s works already: %u tools\n", name, in.ntools);
        } else {
            pt_rt_mcp_login(b.rt, pt_strz(name));
            end = pt_sys_now_ms() + 6 * 60 * 1000;
            while (!b.login_ended && pt_sys_now_ms() < end)
                pt_rt_step(b.rt, 100, NULL, 0);
            bare_settle(&b, 30000);
            pt_rt_mcp_servers(b.rt, &in, 1);
            if (in.state == PT_MCP_READY)
                printf("%s: logged in, %u tools\n", name, in.ntools);
            else {
                fprintf(stderr, "pytte: %s: %s\n", name, in.error && in.error[0] ? in.error : "the login failed");
                rc = 1;
            }
        }
    }
    bare_close(&b);
    return rc;
}
