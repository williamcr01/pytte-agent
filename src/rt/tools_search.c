/* The search tools (section 13.3): grep, find and ls. They change
 * nothing, so they are the tools of read-only mode, and they run in this
 * process: no shell, no ripgrep or fd needed.
 *
 * grep and find walk a tree depth first, in name order, with an explicit
 * stack. They skip .git and what .gitignore files ignore: those in the
 * tree, and those from the repository's root down to where the walk
 * starts. Patterns are globs (find, --glob) turned into POSIX extended
 * regular expressions; grep's pattern is one itself.
 *
 * Every result is bounded (matches, entries, bytes) and says when it was
 * cut and how to see more. */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "rt/rt_int.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define OUT_BYTES (50u * 1024u)     /* a result's text at most */
#define LINE_BYTES 500              /* of one line grep shows */
#define GREP_LIMIT 100              /* matches, unless asked */
#define CONTEXT_MAX 10              /* lines around a match */
#define FIND_LIMIT 1000             /* paths, unless asked */
#define LS_LIMIT 500                /* entries, unless asked */
#define VISIT_MAX 200000            /* entries a walk looks at */
#define FILE_MAX (8u * 1024u * 1024u) /* bytes of a file grep searches */
#define BINARY_PROBE 8192           /* a NUL in these first bytes: binary */
#define KEEP_MAX (1u << 20)         /* buffer capacity kept between calls */

typedef struct rule { /* one .gitignore line */
    regex_t re;
    size_t base; /* length of the directory it is relative to */
    uint32_t depth; /* of that directory in the walk; ancestors are 0 */
    bool neg, dir_only;
} rule;

struct search {
    pt_rt *rt;
    pt_arena ar; /* arguments of one call */
    pt_buf out;  /* the result */
    pt_buf file; /* a file grep reads */
    pt_buf path; /* the entry being looked at: an absolute path */
    pt_buf pend; /* the walk's stack: entries, depth then NUL-ended path */
    pt_buf ends; /* where each pending entry ends in pend (size_t) */
    pt_buf names; /* a directory's entries, NUL-ended */
    pt_buf order; /* ...and pointers to them, sorted (const char *) */
    pt_buf re;    /* a regular expression being built */
    rule *rules;
    uint32_t nrules, rcap;
};

typedef struct walk {
    search *s;
    size_t root;    /* length of the root's absolute path */
    pt_str shown;   /* how the root is shown: the path as given, or empty */
    uint64_t visited, unreadable;
    bool capped;    /* stopped at VISIT_MAX */
} walk;

/* ---- shared ---- */

static void done(pt_rt *rt, uint32_t fx, pt_str text, bool error)
{
    pt_rt_out r;
    memset(&r, 0, sizeof r);
    r.content = text.p ? text : PT_S("");
    r.is_error = error;
    r.whole = true;
    pt_rt_tool_done(rt, fx, &r);
}

static void fail(pt_rt *rt, uint32_t fx, const char *what, const char *path)
{
    char msg[1200];
    snprintf(msg, sizeof msg, "%s %s: %s", what, path, strerror(errno));
    done(rt, fx, pt_strz(msg), true);
}

/* Big buffers go back after a call; small ones are kept for the next. */
static void trim(pt_buf *b)
{
    if (b->cap > KEEP_MAX) {
        pt_buf_free(b);
        pt_buf_init(b, pt_sys_allocator());
    }
    pt_buf_reset(b);
}

/* A call starts afresh: a failed allocation before fails only its call. */
static const pt_json *begin(search *s, const pt_fx *fx)
{
    s->out.oom = s->file.oom = s->path.oom = s->pend.oom = s->ends.oom = false;
    s->names.oom = s->order.oom = s->re.oom = false;
    pt_arena_reset(&s->ar);
    return pt_json_parse(fx->args, &s->ar, NULL);
}

static void finish(search *s, pt_rt *rt, uint32_t fx)
{
    if (s->out.oom)
        done(rt, fx, PT_S("out of memory"), true);
    else
        done(rt, fx, pt_buf_str(&s->out), false);
    trim(&s->out);
    trim(&s->file);
    trim(&s->pend);
    trim(&s->ends);
    trim(&s->names);
    trim(&s->order);
}

static const char *arg_str(search *s, const pt_json *args, const char *key)
{
    const pt_json *v = pt_json_get(args, key);
    return v && v->type == PT_J_STR ? pt_arena_cstr(&s->ar, pt_json_str(v, &s->ar)) : NULL;
}

/* The absolute, resolved path of rel (relative ones are under the
 * session's directory) into out; false with errno set. */
static bool resolve(search *s, const char *rel, char out[PATH_MAX])
{
    pt_str cwd = pt_session_cwd(pt_rt_agent(s->rt));
    char joined[PATH_MAX + 2];
    if (rel[0] != '/' && cwd.n) {
        if (snprintf(joined, sizeof joined, "%.*s/%s", (int)cwd.n, cwd.p, rel) >= (int)sizeof joined) {
            errno = ENAMETOOLONG;
            return false;
        }
        rel = joined;
    }
    return realpath(rel, out) != NULL;
}

/* How a path under the root is shown: the root as given, then the rest. */
static void put_shown(pt_buf *out, const walk *w, const char *abs)
{
    const char *rest = abs + w->root;
    if (*rest == '/')
        rest++;
    if (w->shown.n) {
        pt_buf_puts(out, w->shown);
        if (*rest && w->shown.p[w->shown.n - 1] != '/')
            pt_buf_putc(out, '/');
    }
    pt_buf_putz(out, *rest || w->shown.n ? rest : ".");
}

/* The output has room for n more bytes. */
static bool room(const search *s, size_t n)
{
    return s->out.n + n <= OUT_BYTES;
}

/* ---- globs ---- */

static void re_char(pt_buf *re, char c)
{
    if (strchr(".^$+(){}|\\[]*?", c))
        pt_buf_putc(re, '\\');
    pt_buf_putc(re, c);
}

/* A glob as a POSIX extended regular expression: * and ? stay within a
 * name, ** crosses directories, [...] is a class ([!...] negated). */
static void glob_re(pt_buf *re, const char *g, size_t n)
{
    size_t i = 0;
    while (i < n) {
        char c = g[i];
        if (c == '*' && i + 1 < n && g[i + 1] == '*') {
            bool at_start = i == 0 || g[i - 1] == '/';
            i += 2;
            if (at_start && i < n && g[i] == '/') { /* "**" + "/": any directories, or none */
                pt_buf_putz(re, "(.*/)?");
                i++;
            } else {
                pt_buf_putz(re, ".*");
            }
        } else if (c == '*') {
            pt_buf_putz(re, "[^/]*");
            i++;
        } else if (c == '?') {
            pt_buf_putz(re, "[^/]");
            i++;
        } else if (c == '[') {
            size_t j = i + 1;
            if (j < n && (g[j] == '!' || g[j] == '^'))
                j++;
            if (j < n && g[j] == ']')
                j++;
            while (j < n && g[j] != ']')
                j++;
            if (j >= n) { /* no end: a plain [ */
                re_char(re, c);
                i++;
                continue;
            }
            pt_buf_putc(re, '[');
            i++;
            if (g[i] == '!' || g[i] == '^') {
                pt_buf_putc(re, '^');
                i++;
            }
            pt_buf_put(re, g + i, j - i);
            pt_buf_putc(re, ']');
            i = j + 1;
        } else if (c == '\\' && i + 1 < n) {
            re_char(re, g[i + 1]);
            i += 2;
        } else {
            re_char(re, c);
            i++;
        }
    }
}

/* Compile a glob: anchored ones match the whole path relative to their
 * directory, others the end of it (a name at any depth). */
static bool glob_compile(search *s, regex_t *out, const char *g, size_t n, bool anchored)
{
    pt_buf_reset(&s->re);
    pt_buf_putz(&s->re, anchored ? "^" : "^(.*/)?");
    glob_re(&s->re, g, n);
    pt_buf_putz(&s->re, "$");
    pt_buf_putc(&s->re, 0);
    return !s->re.oom && regcomp(out, s->re.p, REG_EXTENDED | REG_NOSUB) == 0;
}

/* ---- .gitignore ---- */

static void rules_pop(search *s, uint32_t depth)
{
    while (s->nrules && s->rules[s->nrules - 1].depth >= depth && s->rules[s->nrules - 1].depth)
        regfree(&s->rules[--s->nrules].re);
}

static void rules_free(search *s)
{
    while (s->nrules)
        regfree(&s->rules[--s->nrules].re);
}

/* Add the rules of dir/.gitignore, at depth. A line it cannot use is
 * skipped, as git does with patterns it does not understand. */
static void rules_load(search *s, const char *dir, size_t dlen, uint32_t depth)
{
    char path[PATH_MAX + 16];
    char *text, *line, *next;
    size_t n;
    if (snprintf(path, sizeof path, "%.*s/.gitignore", (int)dlen, dir) >= (int)sizeof path)
        return;
    if (!(text = pt_sys_read_regular_file(path, &n)))
        return;
    for (line = text; line < text + n; line = next) {
        char *end = (char *)memchr(line, '\n', (size_t)(text + n - line));
        rule r;
        size_t len;
        bool anchored;
        next = end ? end + 1 : text + n;
        len = (size_t)((end ? end : text + n) - line);
        while (len && (line[len - 1] == '\r' || line[len - 1] == ' ' || line[len - 1] == '\t'))
            len--;
        if (!len || line[0] == '#')
            continue;
        memset(&r, 0, sizeof r);
        if (line[0] == '!') {
            r.neg = true;
            line++;
            len--;
        }
        if (len && line[len - 1] == '/') {
            r.dir_only = true;
            len--;
        }
        anchored = len && memchr(line, '/', len) != NULL;
        if (len && line[0] == '/') {
            line++;
            len--;
        }
        if (!len || !pt_rt_grow((void **)&s->rules, &s->rcap, s->nrules + 1, sizeof *s->rules) ||
            !glob_compile(s, &r.re, line, len, anchored))
            continue;
        r.base = dlen;
        r.depth = depth;
        s->rules[s->nrules++] = r;
    }
    free(text);
}

/* The rules of the directories from the repository's root (the nearest
 * with .git) down to root's parent, at depth 0. Outside a repository:
 * none. */
static void rules_above(search *s, const char *root)
{
    char dir[PATH_MAX], probe[PATH_MAX + 8];
    size_t ends[256], n = 0, len = strlen(root), i;
    struct stat st;
    if (len >= sizeof dir)
        return;
    memcpy(dir, root, len + 1);
    for (;;) { /* up to the repository root, remembering each step */
        if (snprintf(probe, sizeof probe, "%s/.git", len ? dir : "") < (int)sizeof probe &&
            lstat(probe, &st) == 0)
            break;
        if (!len)
            return; /* not in a repository */
        while (len && dir[len - 1] != '/')
            len--;
        if (len)
            len--; /* the slash */
        dir[len] = 0;
        if (n == sizeof ends / sizeof *ends)
            return;
        ends[n++] = len;
    }
    for (i = n; i-- > 0;) /* the repository root first; root itself is loaded by the walk */
        rules_load(s, root, ends[i], 0);
}

/* Whether the entry at abs is ignored: the last rule that matches says. */
static bool ignored(search *s, const char *abs, size_t len, bool is_dir)
{
    uint32_t i = s->nrules;
    const char *name = abs + len;
    while (name > abs && name[-1] != '/')
        name--;
    if (strcmp(name, ".git") == 0)
        return true;
    while (i-- > 0) {
        const rule *r = &s->rules[i];
        if (r->dir_only && !is_dir)
            continue;
        if (len > r->base && regexec(&r->re, abs + r->base + 1, 0, NULL, 0) == 0)
            return !r->neg;
    }
    return false;
}

/* ---- the walk ---- */

typedef struct pend_head {
    uint32_t depth;
} pend_head;

static void push(search *s, const char *abs, size_t len, uint32_t depth)
{
    pend_head h;
    size_t end;
    h.depth = depth;
    pt_buf_put(&s->pend, &h, sizeof h);
    pt_buf_put(&s->pend, abs, len);
    pt_buf_putc(&s->pend, 0);
    end = s->pend.n;
    pt_buf_put(&s->ends, &end, sizeof end);
}

/* The newest pending entry into s->path; false when there is none. */
static bool pop(search *s, uint32_t *depth)
{
    size_t n = s->ends.n / sizeof(size_t), end, start;
    pend_head h;
    if (!n || s->pend.oom || s->ends.oom)
        return false;
    memcpy(&end, s->ends.p + (n - 1) * sizeof end, sizeof end);
    s->ends.n -= sizeof end;
    start = 0; /* where the one below it ends */
    if (n > 1)
        memcpy(&start, s->ends.p + (n - 2) * sizeof start, sizeof start);
    memcpy(&h, s->pend.p + start, sizeof h);
    pt_buf_reset(&s->path);
    pt_buf_put(&s->path, s->pend.p + start + sizeof h, end - start - sizeof h); /* with its NUL */
    s->pend.n = start;
    *depth = h.depth;
    return !s->path.oom;
}

static int name_cmp(const void *x, const void *y)
{
    return strcmp(*(const char *const *)x, *(const char *const *)y);
}

/* The entries of the directory in s->path, sorted, into s->order; returns
 * how many, or -1 when it cannot be read. */
static long list_dir(search *s)
{
    DIR *d = opendir(s->path.p);
    struct dirent *e;
    size_t i, n = 0;
    const char *p;
    if (!d)
        return -1;
    pt_buf_reset(&s->names);
    pt_buf_reset(&s->order);
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        pt_buf_putz(&s->names, e->d_name);
        pt_buf_putc(&s->names, 0);
        n++;
    }
    closedir(d);
    if (s->names.oom || !pt_buf_reserve(&s->order, n * sizeof p))
        return -1;
    /* Pointers only once names stops growing. */
    for (i = 0, p = s->names.p; i < n; i++, p += strlen(p) + 1)
        memcpy(s->order.p + i * sizeof p, &p, sizeof p);
    s->order.n = n * sizeof p;
    qsort(s->order.p, n, sizeof p, name_cmp);
    return (long)n;
}

static const char *nth(const search *s, size_t i)
{
    const char *p;
    memcpy(&p, s->order.p + i * sizeof p, sizeof p);
    return p;
}

/* Visit the tree under the root (in s->path): fn sees each entry not
 * ignored, the root's files too (with depth 0 for a file root). fn
 * returns false to stop. */
typedef bool (*visit_fn)(void *ud, walk *w, const char *abs, size_t len, bool is_dir);

static void walk_tree(walk *w, visit_fn fn, void *ud)
{
    search *s = w->s;
    uint32_t depth;
    struct stat st;
    push(s, s->path.p, s->path.n - 1, 0);
    rules_above(s, s->path.p);
    while (pop(s, &depth)) {
        size_t len = s->path.n - 1, i;
        long n;
        bool is_dir;
        if (++w->visited > VISIT_MAX) {
            w->capped = true;
            break;
        }
        if (lstat(s->path.p, &st) != 0) {
            w->unreadable++;
            continue;
        }
        is_dir = S_ISDIR(st.st_mode);
        if (depth) {
            rules_pop(s, depth);
            if (ignored(s, s->path.p, len, is_dir))
                continue;
        }
        if (!S_ISREG(st.st_mode) && !is_dir) /* links are not followed: no loops */
            continue;
        if (!fn(ud, w, s->path.p, len, is_dir))
            break;
        if (!is_dir)
            continue;
        if ((n = list_dir(s)) < 0) {
            if (s->names.oom || s->order.oom)
                break;
            w->unreadable++;
            continue;
        }
        rules_load(s, s->path.p, len, depth); /* the root's (depth 0) stay for the whole walk */
        /* Children, last first: the stack hands them out in name order. */
        s->path.n = len; /* without its NUL: names go after it */
        for (i = (size_t)n; i-- > 0;) {
            pt_buf_putc(&s->path, '/');
            pt_buf_putz(&s->path, nth(s, i));
            push(s, s->path.p, s->path.n, depth + 1);
            s->path.n = len;
        }
        if (s->pend.oom || s->ends.oom || s->path.oom)
            break;
    }
    /* A walk cut short for want of memory is an error, not a result. */
    if (s->pend.oom || s->ends.oom || s->path.oom || s->names.oom || s->order.oom)
        s->out.oom = true;
    rules_free(s);
    pt_buf_reset(&s->pend);
    pt_buf_reset(&s->ends);
}

/* Shared notes at the end of a walk's result. */
static void walk_notes(search *s, const walk *w)
{
    if (w->capped) {
        pt_buf_putz(&s->out, "\n[stopped after looking at ");
        pt_buf_u64(&s->out, VISIT_MAX);
        pt_buf_putz(&s->out, " entries: search a smaller directory]");
    }
    if (w->unreadable) {
        pt_buf_putz(&s->out, "\n[");
        pt_buf_u64(&s->out, w->unreadable);
        pt_buf_putz(&s->out, w->unreadable == 1 ? " entry could not be read]" : " entries could not be read]");
    }
}

/* The root of a walk from the "path" argument into s->path and w. */
static bool walk_root(search *s, pt_rt *rt, const pt_fx *fx, const pt_json *args, walk *w, struct stat *st)
{
    const char *rel = arg_str(s, args, "path");
    char abs[PATH_MAX];
    memset(w, 0, sizeof *w);
    w->s = s;
    if (!rel || !*rel)
        rel = ".";
    if (!resolve(s, rel, abs)) {
        fail(rt, fx->id, "cannot resolve", rel);
        return false;
    }
    if (stat(abs, st) != 0) {
        fail(rt, fx->id, "cannot stat", rel);
        return false;
    }
    pt_buf_reset(&s->path);
    pt_buf_putz(&s->path, abs);
    pt_buf_putc(&s->path, 0);
    w->root = strlen(abs);
    if (strcmp(rel, ".") != 0) {
        w->shown = pt_strz(rel);
        while (w->shown.n > 1 && w->shown.p[w->shown.n - 1] == '/')
            w->shown.n--;
    }
    return !s->path.oom;
}

/* A path filter: a glob, matched against the name, or against the path
 * under the root when it has a slash. */
static bool filter_compile(search *s, regex_t *re, const char *glob)
{
    size_t n = strlen(glob);
    bool slash = memchr(glob, '/', n) != NULL;
    if (glob[0] == '/') {
        glob++;
        n--;
    }
    return glob_compile(s, re, glob, n, slash);
}

static bool filter_match(const regex_t *re, const walk *w, const char *abs)
{
    const char *rest = abs + w->root;
    if (*rest == '/')
        rest++;
    if (!*rest) { /* the root itself, a file: its name */
        rest = abs + w->root;
        while (rest > abs && rest[-1] != '/')
            rest--;
    }
    return regexec(re, rest, 0, NULL, 0) == 0;
}

/* ---- find ---- */

typedef struct find_state {
    search *s;
    regex_t re;
    int64_t limit, found;
    bool full; /* stopped: limit or bytes */
} find_state;

static bool find_visit(void *ud, walk *w, const char *abs, size_t len, bool is_dir)
{
    find_state *f = (find_state *)ud;
    search *s = f->s;
    size_t mark = s->out.n;
    (void)len;
    if (abs[w->root] == 0) /* the root itself is not a result */
        return true;
    if (!filter_match(&f->re, w, abs))
        return true;
    if (f->found == f->limit) {
        f->full = true;
        return false;
    }
    put_shown(&s->out, w, abs);
    if (is_dir)
        pt_buf_putc(&s->out, '/');
    pt_buf_putc(&s->out, '\n');
    if (s->out.n > OUT_BYTES) {
        s->out.n = mark;
        f->full = true;
        return false;
    }
    f->found++;
    return true;
}

static void t_find(void *ud, pt_rt *rt, const pt_fx *fx)
{
    search *s = (search *)ud;
    const pt_json *args;
    const char *pattern;
    find_state f;
    struct stat st;
    walk w;
    args = begin(s, fx);
    pattern = arg_str(s, args, "pattern");
    memset(&f, 0, sizeof f);
    f.s = s;
    f.limit = pt_json_geti(args, "limit", FIND_LIMIT);
    if (f.limit < 1)
        f.limit = 1;
    if (!pattern || !*pattern) {
        done(rt, fx->id, PT_S("pattern is required"), true);
        return;
    }
    if (!walk_root(s, rt, fx, args, &w, &st))
        return;
    if (!S_ISDIR(st.st_mode)) {
        done(rt, fx->id, PT_S("path is not a directory; find searches directories"), true);
        return;
    }
    if (!filter_compile(s, &f.re, pattern)) {
        done(rt, fx->id, PT_S("pattern is not a glob find can use"), true);
        return;
    }
    pt_buf_reset(&s->out);
    walk_tree(&w, find_visit, &f);
    regfree(&f.re);
    if (!f.found)
        pt_buf_putz(&s->out, "No files match.");
    else
        s->out.n--; /* the last newline */
    if (f.full) {
        pt_buf_putz(&s->out, "\n[stopped at ");
        pt_buf_u64(&s->out, (uint64_t)f.found);
        pt_buf_putz(&s->out, f.found == f.limit ? " paths: the limit. Narrow the pattern or the path, or raise limit]"
                                                : " paths: 50 KiB. Narrow the pattern or the path]");
    }
    walk_notes(s, &w);
    finish(s, rt, fx->id);
}

/* ---- grep ---- */

/* Text every match contains, found fast (Horspool): only lines that have
 * it go to the regular expression. */
typedef struct needle {
    unsigned char *p;
    size_t n;
    size_t skip[256];
    unsigned char fold[256]; /* case folding, or the identity */
} needle;

typedef struct grep_state {
    search *s;
    regex_t re, glob;
    bool have_glob;
    needle lit; /* lit.n == 0: none */
    int64_t limit, found, context;
    uint64_t big, binary; /* files skipped */
    bool full, files_only;
} grep_state;

static bool ere_special(char c)
{
    return strchr(".[]()*+?{}|^$\\", c) != NULL;
}

/* The longest text every match of an ERE contains, into out: a run of
 * plain characters outside groups, classes and alternations. A character
 * a quantifier may drop (*, ?, {) is not part of it. Empty when there is
 * none (any | at the top makes every part optional). */
static void required_text(const char *re, pt_buf *out)
{
    size_t best_at = 0, best_n = 0, run_at = 0, i = 0, n = strlen(re);
    int depth = 0;
    pt_buf_reset(out);
    for (i = 0; i < n; i++) /* an alternation at the top: nothing is required */
        if (re[i] == '\\')
            i++;
        else if (re[i] == '(')
            depth++;
        else if (re[i] == ')')
            depth--;
        else if (re[i] == '|' && depth <= 0)
            return;
    depth = 0;
    for (i = 0; i < n;) {
        char c = re[i];
        bool plain = false;
        size_t next = i + 1;
        if (c == '\\' && i + 1 < n && ere_special(re[i + 1])) {
            plain = true;
            c = re[i + 1];
            next = i + 2;
        } else if (!ere_special(c)) {
            plain = true;
        }
        if (plain && next < n && (re[next] == '*' || re[next] == '?' || re[next] == '{'))
            plain = false; /* optional: ends the run */
        if (plain) {
            pt_buf_putc(out, c);
            if (next < n && re[next] == '+') { /* one at least, then anything */
                plain = false;
                next++;
            }
        }
        if (!plain) { /* the run ends: keep the longest */
            if (out->n - run_at > best_n) {
                best_at = run_at;
                best_n = out->n - run_at;
            }
            if (c == '[') { /* skip the class */
                next = i + 1;
                if (next < n && re[next] == '^')
                    next++;
                if (next < n && re[next] == ']')
                    next++;
                while (next < n && re[next] != ']')
                    next++;
                next++;
            } else if (c == '(') { /* skip the group */
                depth = 1;
                for (next = i + 1; next < n && depth; next++)
                    if (re[next] == '\\')
                        next++;
                    else if (re[next] == '(')
                        depth++;
                    else if (re[next] == ')')
                        depth--;
            } else if (c == '\\' && i + 1 < n) {
                next = i + 2; /* \w, \1 and the like */
            }
            if (next < n && (re[next] == '*' || re[next] == '?' || re[next] == '+'))
                next++;
            else if (next < n && re[next] == '{')
                while (next < n && re[next++] != '}')
                    ;
            run_at = out->n;
        }
        i = next;
    }
    if (out->n - run_at > best_n) {
        best_at = run_at;
        best_n = out->n - run_at;
    }
    memmove(out->p, out->p + best_at, best_n);
    out->n = best_n;
}

static void needle_init(needle *nd, unsigned char *p, size_t n, bool icase)
{
    size_t i;
    for (i = 0; i < 256; i++) {
        nd->fold[i] = (unsigned char)(icase && i >= 'A' && i <= 'Z' ? i + 32 : i);
        nd->skip[i] = n;
    }
    for (i = 0; i < n; i++)
        p[i] = nd->fold[p[i]];
    for (i = 0; i + 1 < n; i++)
        nd->skip[p[i]] = n - 1 - i;
    if (icase) /* both cases of a letter skip alike */
        for (i = 'A'; i <= 'Z'; i++)
            nd->skip[i] = nd->skip[i + 32];
    nd->p = p;
    nd->n = n;
}

/* The first occurrence of the needle in [p, end), or NULL. */
static const char *needle_find(const needle *nd, const char *p, const char *end)
{
    const unsigned char *h = (const unsigned char *)p, *e = (const unsigned char *)end;
    size_t m = nd->n, k;
    if (m == 1) { /* one byte: memchr, for each case */
        const unsigned char *a = (const unsigned char *)memchr(h, nd->p[0], (size_t)(e - h)), *b = NULL;
        if (nd->fold['A'] == 'a' && nd->p[0] >= 'a' && nd->p[0] <= 'z')
            b = (const unsigned char *)memchr(h, nd->p[0] - 32, (size_t)(e - h));
        return (const char *)(a && b ? (a < b ? a : b) : a ? a : b);
    }
    while ((size_t)(e - h) >= m) {
        unsigned char last = nd->fold[h[m - 1]];
        if (last == nd->p[m - 1]) {
            for (k = 0; k + 1 < m && nd->fold[h[k]] == nd->p[k]; k++)
                ;
            if (k + 1 == m)
                return (const char *)h;
        }
        h += nd->skip[h[m - 1]];
    }
    return NULL;
}

/* The start of the next line from p (inside [base, end)) with a match,
 * or NULL. */
static char *next_match(grep_state *g, char *base, char *p, char *end)
{
    regmatch_t m;
    while (p < end) {
        if (g->lit.n) { /* only lines with the needle */
            char *at = (char *)needle_find(&g->lit, p, end), *ls, *le;
            char saved;
            bool hit;
            if (!at)
                return NULL;
            for (ls = at; ls > p && ls[-1] != '\n'; ls--)
                ;
            le = (char *)memchr(at, '\n', (size_t)(end - at));
            if (!le)
                le = end;
            saved = *le;
            *le = 0; /* the line alone */
            hit = regexec(&g->re, ls, 0, NULL, ls > base && ls[-1] != '\n' ? REG_NOTBOL : 0) == 0;
            *le = saved;
            if (hit)
                return ls;
            p = le + 1;
            continue;
        }
        /* The rest at once: REG_NEWLINE keeps a match on one line. It
         * stops at a NUL; the search goes on after one. */
        if (regexec(&g->re, p, 1, &m, p > base && p[-1] != '\n' ? REG_NOTBOL : 0) == 0) {
            char *at = p + m.rm_so;
            if (at >= end)
                return NULL;
            while (at > base && at[-1] != '\n')
                at--;
            return at;
        }
        p = (char *)memchr(p, 0, (size_t)(end - p));
        if (!p)
            return NULL;
        p++;
    }
    return NULL;
}

/* One output line: "path:N:text" (a match) or "path-N-text" (context),
 * text cut at LINE_BYTES. False when the output is full. */
static bool put_line(grep_state *g, walk *w, const char *abs, uint64_t n, const char *p, size_t len, char sep)
{
    search *s = g->s;
    size_t mark = s->out.n;
    bool cut = len > LINE_BYTES;
    if (cut) {
        len = LINE_BYTES;
        while (len && ((unsigned char)p[len] & 0xC0) == 0x80)
            len--;
    }
    put_shown(&s->out, w, abs);
    pt_buf_putc(&s->out, sep);
    pt_buf_u64(&s->out, n);
    pt_buf_putc(&s->out, sep);
    pt_buf_put(&s->out, p, len);
    if (cut)
        pt_buf_putz(&s->out, "…");
    pt_buf_putc(&s->out, '\n');
    if (!room(s, 0)) {
        s->out.n = mark;
        g->full = true;
        return false;
    }
    return true;
}

/* Read a file into s->file, NUL-ended. 0: read; 1: skipped; -1: error. */
static int load(grep_state *g, const char *abs)
{
    search *s = g->s;
    struct stat st;
    int fd = open(abs, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return -1;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        return 1;
    }
    if ((uint64_t)st.st_size > FILE_MAX) {
        close(fd);
        g->big++;
        return 1;
    }
    pt_buf_reset(&s->file);
    if (!pt_buf_reserve(&s->file, (size_t)st.st_size + 1)) {
        close(fd);
        return -1;
    }
    for (;;) {
        ssize_t got = read(fd, s->file.p + s->file.n, (size_t)st.st_size - s->file.n);
        if (got < 0 && errno == EINTR)
            continue;
        if (got < 0) {
            close(fd);
            return -1;
        }
        s->file.n += (size_t)got;
        if (got == 0 || s->file.n == (size_t)st.st_size)
            break; /* what it held when looked at: a file that grows is read to its old size */
    }
    close(fd);
    s->file.p[s->file.n] = 0;
    if (memchr(s->file.p, 0, s->file.n < BINARY_PROBE ? s->file.n : BINARY_PROBE)) {
        g->binary++;
        return 1;
    }
    return 0;
}

/* The start of the line before the one at p. */
static const char *line_before(const char *base, const char *p)
{
    p--; /* its newline */
    while (p > base && p[-1] != '\n')
        p--;
    return p;
}

/* The line at p: its end (its newline, or end). */
static char *line_end(char *p, char *end)
{
    char *nl = (char *)memchr(p, '\n', (size_t)(end - p));
    return nl ? nl : end;
}

static bool grep_visit(void *ud, walk *w, const char *abs, size_t len, bool is_dir)
{
    grep_state *g = (grep_state *)ud;
    search *s = g->s;
    char *base, *p, *end, *shown_to; /* shown_to: lines before it are in the output */
    uint64_t line = 1, after = 0;
    bool any = false;
    int r;
    (void)len;
    if (is_dir || (g->have_glob && !filter_match(&g->glob, w, abs)))
        return true;
    if ((r = load(g, abs)) != 0) {
        w->unreadable += r < 0;
        return true;
    }
    base = p = shown_to = s->file.p;
    end = p + s->file.n;
    while (p < end) {
        char *hit = next_match(g, base, p, end), *stop;
        /* the lines after the last match, up to context of them */
        while (after && p < end && (!hit || p < hit)) {
            stop = line_end(p, end);
            if (!put_line(g, w, abs, line, p, (size_t)(stop - p), '-'))
                return false;
            after--;
            line++;
            p = shown_to = stop < end ? stop + 1 : end;
        }
        if (!hit)
            break;
        while (p < hit) { /* count the lines skipped */
            char *nl = (char *)memchr(p, '\n', (size_t)(hit - p));
            if (!nl)
                break;
            line++;
            p = nl + 1;
        }
        p = hit;
        if (g->found == g->limit) {
            g->full = true;
            return false;
        }
        if (g->files_only) {
            size_t mark = s->out.n;
            put_shown(&s->out, w, abs);
            pt_buf_putc(&s->out, '\n');
            if (!room(s, 0)) {
                s->out.n = mark;
                g->full = true;
                return false;
            }
            g->found++;
            return true;
        }
        if (g->context) { /* the lines before it, not shown yet */
            const char *q = p;
            uint64_t k = 0, at;
            while (k < (uint64_t)g->context && q > shown_to) {
                q = line_before(base, q);
                k++;
            }
            if ((any || s->out.n) && (q != shown_to || !any))
                pt_buf_putz(&s->out, "--\n");
            for (at = line - k; q < p; at++) {
                const char *e = (const char *)memchr(q, '\n', (size_t)(p - q));
                if (!put_line(g, w, abs, at, q, (size_t)(e - q), '-'))
                    return false;
                q = e + 1;
            }
        }
        stop = line_end(p, end);
        if (!put_line(g, w, abs, line, p, (size_t)(stop - p), ':'))
            return false;
        g->found++;
        any = true;
        after = (uint64_t)g->context;
        line++;
        p = shown_to = stop < end ? stop + 1 : end;
    }
    return true;
}

static void t_grep(void *ud, pt_rt *rt, const pt_fx *fx)
{
    search *s = (search *)ud;
    const pt_json *args;
    const char *pattern, *glob;
    grep_state g;
    struct stat st;
    walk w;
    int flags = REG_EXTENDED | REG_NEWLINE, rc; /* a match stays on its line */
    bool literal, icase;
    pt_str text;
    args = begin(s, fx);
    pattern = arg_str(s, args, "pattern");
    glob = arg_str(s, args, "glob");
    memset(&g, 0, sizeof g);
    g.s = s;
    g.limit = pt_json_geti(args, "limit", GREP_LIMIT);
    g.context = pt_json_geti(args, "context", 0);
    g.files_only = pt_json_truthy(pt_json_get(args, "files_only"));
    if (g.limit < 1)
        g.limit = 1;
    if (g.context < 0)
        g.context = 0;
    if (g.context > CONTEXT_MAX)
        g.context = CONTEXT_MAX;
    if (!pattern || !*pattern) {
        done(rt, fx->id, PT_S("pattern is required"), true);
        return;
    }
    icase = pt_json_truthy(pt_json_get(args, "ignore_case"));
    literal = pt_json_truthy(pt_json_get(args, "literal"));
    if (icase)
        flags |= REG_ICASE;
    /* The needle first: the whole pattern, or the text it requires. */
    if (literal) {
        text = pt_arena_dup(&s->ar, pt_strz(pattern));
    } else {
        required_text(pattern, &s->re);
        text = pt_arena_dup(&s->ar, pt_buf_str(&s->re));
    }
    if (text.n && text.p && !s->ar.oom)
        needle_init(&g.lit, (unsigned char *)text.p, text.n, icase);
    pt_buf_reset(&s->re);
    if (literal) {
        const char *c;
        for (c = pattern; *c; c++)
            re_char(&s->re, *c);
    } else {
        pt_buf_putz(&s->re, pattern);
    }
    pt_buf_putc(&s->re, 0);
    if (s->re.oom) {
        done(rt, fx->id, PT_S("out of memory"), true);
        return;
    }
    if ((rc = regcomp(&g.re, s->re.p, flags)) != 0) {
        char msg[300], why[200];
        regerror(rc, &g.re, why, sizeof why);
        snprintf(msg, sizeof msg, "pattern is not a POSIX extended regular expression: %s", why);
        done(rt, fx->id, pt_strz(msg), true);
        return;
    }
    if (glob && *glob) {
        if (!filter_compile(s, &g.glob, glob)) {
            regfree(&g.re);
            done(rt, fx->id, PT_S("glob is not a glob grep can use"), true);
            return;
        }
        g.have_glob = true;
    }
    if (walk_root(s, rt, fx, args, &w, &st)) {
        pt_buf_reset(&s->out);
        walk_tree(&w, grep_visit, &g);
        if (!g.found)
            pt_buf_putz(&s->out, "No matches.");
        else if (s->out.n && s->out.p[s->out.n - 1] == '\n')
            s->out.n--;
        if (g.full) {
            pt_buf_putz(&s->out, "\n[stopped at ");
            pt_buf_u64(&s->out, (uint64_t)g.found);
            pt_buf_putz(&s->out, g.files_only ? " files" : " matches");
            pt_buf_putz(&s->out, g.found == g.limit ? ": the limit. Narrow the search, or raise limit]"
                                                    : ": 50 KiB. Narrow the search]");
        }
        if (g.big) {
            pt_buf_putz(&s->out, "\n[");
            pt_buf_u64(&s->out, g.big);
            pt_buf_putz(&s->out, g.big == 1 ? " file over 8 MiB was not searched]" : " files over 8 MiB were not searched]");
        }
        walk_notes(s, &w);
        finish(s, rt, fx->id);
    }
    regfree(&g.re);
    if (g.have_glob)
        regfree(&g.glob);
}

/* ---- ls ---- */

static void t_ls(void *ud, pt_rt *rt, const pt_fx *fx)
{
    search *s = (search *)ud;
    const pt_json *args;
    int64_t limit;
    struct stat st;
    walk w;
    long n, i;
    args = begin(s, fx);
    limit = pt_json_geti(args, "limit", LS_LIMIT);
    if (limit < 1)
        limit = 1;
    if (!walk_root(s, rt, fx, args, &w, &st))
        return;
    if (!S_ISDIR(st.st_mode)) {
        done(rt, fx->id, PT_S("path is not a directory; read reads files"), true);
        return;
    }
    if ((n = list_dir(s)) < 0) {
        fail(rt, fx->id, "cannot list", s->path.p);
        return;
    }
    pt_buf_reset(&s->out);
    for (i = 0; i < n && i < limit; i++) {
        const char *name = nth(s, (size_t)i);
        size_t at = s->path.n - 1, mark = s->out.n;
        bool dir;
        s->path.n = at;
        pt_buf_putc(&s->path, '/');
        pt_buf_putz(&s->path, name);
        pt_buf_putc(&s->path, 0);
        dir = stat(s->path.p, &st) == 0 && S_ISDIR(st.st_mode); /* a link to a directory too */
        s->path.n = at + 1;
        s->path.p[at] = 0;
        pt_buf_putz(&s->out, name);
        if (dir)
            pt_buf_putc(&s->out, '/');
        pt_buf_putc(&s->out, '\n');
        if (!room(s, 0)) {
            s->out.n = mark;
            break;
        }
    }
    if (!n)
        pt_buf_putz(&s->out, "(empty directory)");
    else
        s->out.n--;
    if (i < n) {
        pt_buf_putz(&s->out, "\n[");
        pt_buf_u64(&s->out, (uint64_t)(n - i));
        pt_buf_putz(&s->out, " more entries; ");
        pt_buf_putz(&s->out, i == limit ? "raise limit, or use find]" : "the output is at 50 KiB: use find]");
    }
    finish(s, rt, fx->id);
}

/* ---- the table ---- */

static const char grep_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"pattern\":{\"type\":\"string\",\"description\":\"A POSIX extended regular expression, matched per line\"},"
    "\"path\":{\"type\":\"string\",\"description\":\"A directory to search, or one file (default: the session's directory)\"},"
    "\"glob\":{\"type\":\"string\",\"description\":\"Only files matching this glob: *.c, or src/**/*.h with a slash\"},"
    "\"ignore_case\":{\"type\":\"boolean\"},"
    "\"literal\":{\"type\":\"boolean\",\"description\":\"The pattern is plain text, not a regular expression\"},"
    "\"context\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":10,\"description\":\"Lines to show before and after each match\"},"
    "\"files_only\":{\"type\":\"boolean\",\"description\":\"List the files with a match instead of the lines\"},"
    "\"limit\":{\"type\":\"integer\",\"minimum\":1,\"description\":\"Matches (or files) to show at most (default 100)\"}},"
    "\"required\":[\"pattern\"],\"additionalProperties\":false}";
static const char find_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"pattern\":{\"type\":\"string\",\"description\":\"A glob: *.c matches names at any depth; src/**/*.h, with a "
    "slash, the path under path\"},"
    "\"path\":{\"type\":\"string\",\"description\":\"The directory to search (default: the session's directory)\"},"
    "\"limit\":{\"type\":\"integer\",\"minimum\":1,\"description\":\"Paths to show at most (default 1000)\"}},"
    "\"required\":[\"pattern\"],\"additionalProperties\":false}";
static const char ls_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\",\"description\":\"The directory (default: the session's directory)\"},"
    "\"limit\":{\"type\":\"integer\",\"minimum\":1,\"description\":\"Entries to show at most (default 500)\"}},"
    "\"additionalProperties\":false}";

search *pt_rt_search_new(pt_rt *rt)
{
    search *s = (search *)calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->rt = rt;
    pt_arena_init(&s->ar, pt_sys_allocator(), 4096);
    pt_buf_init(&s->out, pt_sys_allocator());
    pt_buf_init(&s->file, pt_sys_allocator());
    pt_buf_init(&s->path, pt_sys_allocator());
    pt_buf_init(&s->pend, pt_sys_allocator());
    pt_buf_init(&s->ends, pt_sys_allocator());
    pt_buf_init(&s->names, pt_sys_allocator());
    pt_buf_init(&s->order, pt_sys_allocator());
    pt_buf_init(&s->re, pt_sys_allocator());
    return s;
}

void pt_rt_search_free(search *s)
{
    if (!s)
        return;
    rules_free(s);
    free(s->rules);
    pt_arena_free(&s->ar);
    pt_buf_free(&s->out);
    pt_buf_free(&s->file);
    pt_buf_free(&s->path);
    pt_buf_free(&s->pend);
    pt_buf_free(&s->ends);
    pt_buf_free(&s->names);
    pt_buf_free(&s->order);
    pt_buf_free(&s->re);
    free(s);
}

void pt_rt_search_tools(search *s, pt_rt_tool t[3])
{
    uint32_t i;
    memset(t, 0, 3 * sizeof *t);
    for (i = 0; i < 3; i++) {
        t[i].def.source = PT_S("builtin");
        t[i].def.replay = PT_REPLAY_SAFE;
        t[i].def.parallel = true;
        t[i].def.read_only = true;
        t[i].ud = s;
    }
    t[0].def.name = PT_S("grep");
    t[0].def.description = PT_S(
        "Search file contents for a pattern; shows path:line:text for each match. Searches a directory "
        "recursively (skipping .git, what .gitignore ignores, binary files and files over 8 MiB) or one "
        "file. Up to limit matches and 50 KiB; the result says when it was cut.");
    t[0].def.schema = PT_S(grep_schema);
    t[0].run = t_grep;
    t[1].def.name = PT_S("find");
    t[1].def.description = PT_S(
        "Find files and directories by glob under a directory, skipping .git and what .gitignore "
        "ignores. Directories end with /. Up to limit paths and 50 KiB; the result says when it was cut.");
    t[1].def.schema = PT_S(find_schema);
    t[1].run = t_find;
    t[2].def.name = PT_S("ls");
    t[2].def.description = PT_S(
        "List a directory's entries in name order, dotfiles included; directories end with /. Up to "
        "limit entries.");
    t[2].def.schema = PT_S(ls_schema);
    t[2].run = t_ls;
}
