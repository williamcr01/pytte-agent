/* Prompt files and skills (sections 14.1 and 14.2): the system prompt of a
 * new epoch, the skills index the model sees, and the skills a message
 * invokes with $name or /skill:name. */
#define _POSIX_C_SOURCE 200809L
#include "rt/rt_int.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Where prompt files and skills are looked up, in order: a later root wins. */
enum { R_HOME, R_CONFIG, R_CWD };
static const struct {
    int base;
    const char *dir;
    bool prompts; /* holds SYSTEM.md and APPEND_SYSTEM.md */
} roots[] = {
    {R_HOME, "/.agents", false},
    {R_CONFIG, "", true},
    {R_CWD, "/.agents", false},
    {R_CWD, "/.pytte", true},
};
#define NROOTS (sizeof roots / sizeof roots[0])

#define AGENTS_CAP 32768 /* bytes of AGENTS.md files; the outermost are cut first */
#define AGENTS_MAX 64    /* files: the user's, then directories from the git root down */

#define SKILL_OPEN "<skill name=\""
#define SKILL_CLOSE "\n</skill>"

/* Root i under cwd, or false when its base is not set. */
static bool root_dir(const pt_rt *rt, size_t i, pt_str cwd, char *out, size_t cap)
{
    const char *base = roots[i].base == R_HOME     ? rt->prompt.home
                       : roots[i].base == R_CONFIG ? rt->prompt.config_dir
                                                   : NULL;
    int n;
    if (roots[i].base == R_CWD)
        n = snprintf(out, cap, "%.*s%s", (int)cwd.n, cwd.p, roots[i].dir);
    else if (base && *base)
        n = snprintf(out, cap, "%s%s", base, roots[i].dir);
    else
        return false;
    return n > 0 && (size_t)n < cap;
}

static const char *dup_z(pt_arena *ar, const char *z)
{
    return z ? pt_arena_cstr(ar, pt_strz(z)) : NULL;
}

bool pt_rt_set_prompt(pt_rt *rt, const pt_rt_prompt *p)
{
    pt_arena_reset(&rt->prompt_ar);
    rt->prompt.base = dup_z(&rt->prompt_ar, p->base);
    rt->prompt.home = dup_z(&rt->prompt_ar, p->home);
    rt->prompt.config_dir = dup_z(&rt->prompt_ar, p->config_dir);
    rt->prompt.skills_auto = p->skills_auto;
    rt->have_prompt = !rt->prompt_ar.oom;
    return rt->have_prompt;
}

void pt_rt_skills_auto(pt_rt *rt, bool on)
{
    rt->prompt.skills_auto = on;
}

void pt_rt_prompt_free(pt_rt *rt)
{
    pt_arena_free(&rt->prompt_ar);
    pt_arena_free(&rt->skills_ar);
    free(rt->skills);
}

/* The file of the winning root that has it (malloc'd), or NULL. */
static char *read_prompt_file(pt_rt *rt, pt_str cwd, const char *name, size_t *n)
{
    char dir[4096], path[4200];
    size_t i;
    for (i = NROOTS; i-- > 0;) {
        char *text;
        if (!roots[i].prompts || !root_dir(rt, i, cwd, dir, sizeof dir))
            continue;
        snprintf(path, sizeof path, "%s/%s", dir, name);
        if ((text = pt_sys_read_file(path, n)))
            return text;
    }
    return NULL;
}

/* ---- SKILL.md ---- */

static bool name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
           c == '.';
}

static pt_str next_line(pt_str *s)
{
    long nl = pt_str_find(*s, '\n');
    pt_str line = {s->p, nl < 0 ? s->n : (size_t)nl};
    s->p += nl < 0 ? s->n : (size_t)nl + 1;
    s->n -= nl < 0 ? s->n : (size_t)nl + 1;
    if (line.n && line.p[line.n - 1] == '\r')
        line.n--;
    return line;
}

/* A YAML scalar as one line: quotes removed, runs of whitespace one space. */
static void put_scalar(pt_buf *b, pt_str v)
{
    size_t i, start = b->n;
    char q = 0;
    bool space = false;
    v = pt_str_trim(v);
    if (v.n >= 2 && (v.p[0] == '"' || v.p[0] == '\'') && v.p[v.n - 1] == v.p[0]) {
        q = v.p[0];
        v.p++;
        v.n -= 2;
    }
    for (i = 0; i < v.n; i++) {
        char c = v.p[i];
        if (q == '"' && c == '\\' && i + 1 < v.n)
            c = v.p[++i];
        else if (q == '\'' && c == '\'' && i + 1 < v.n && v.p[i + 1] == '\'')
            i++;
        if (c == ' ' || c == '\t' || c == '\n') {
            space = b->n > start;
            continue;
        }
        if (space)
            pt_buf_putc(b, ' ');
        space = false;
        pt_buf_putc(b, c);
    }
}

typedef struct frontmatter {
    pt_buf name, description;
    bool manual;
    pt_str body;
} frontmatter;

/* The frontmatter's name, description and disable-model-invocation, and
 * the body after it. Values may continue on indented lines, as block
 * scalars (| and >) do. False when the text has no frontmatter. */
static bool parse_skill(pt_str text, frontmatter *f)
{
    pt_str s = text, line;
    pt_buf *val = NULL;
    pt_buf_reset(&f->name);
    pt_buf_reset(&f->description);
    f->manual = false;
    if (!pt_str_eqz(pt_str_trim(next_line(&s)), "---"))
        return false;
    while (s.n) {
        long colon;
        line = next_line(&s);
        if (pt_str_eqz(pt_str_trim(line), "---")) {
            f->body = pt_str_trim(s);
            return true;
        }
        if (line.n && (line.p[0] == ' ' || line.p[0] == '\t')) { /* a continuation */
            if (val) {
                if (val->n)
                    pt_buf_putc(val, ' ');
                put_scalar(val, line);
            }
            continue;
        }
        val = NULL;
        if ((colon = pt_str_find(line, ':')) < 0)
            continue;
        {
            pt_str key = pt_str_trim((pt_str){line.p, (size_t)colon});
            pt_str v = pt_str_trim((pt_str){line.p + colon + 1, line.n - (size_t)colon - 1});
            if (v.n && (v.p[0] == '|' || v.p[0] == '>'))
                v = PT_STR_NULL; /* a block scalar: the indented lines follow */
            if (pt_str_eqz(key, "name"))
                val = &f->name;
            else if (pt_str_eqz(key, "description"))
                val = &f->description;
            else if (pt_str_eqz(key, "disable-model-invocation"))
                f->manual = pt_str_eqz(v, "true");
            if (val)
                put_scalar(val, v);
        }
    }
    return false; /* never closed */
}

static bool valid_name(pt_str n)
{
    size_t i;
    if (!n.n || n.n > 64)
        return false;
    for (i = 0; i < n.n; i++)
        if (!name_char(n.p[i]))
            return false;
    return true;
}

static int find_skill(const pt_rt *rt, pt_str name)
{
    uint32_t i;
    for (i = 0; i < rt->nskills; i++)
        if (pt_str_eq(rt->skills[i].name, name))
            return (int)i;
    return -1;
}

/* One skill directory: add or replace the skill it holds. */
static void add_skill(pt_rt *rt, frontmatter *f, const char *dir, const char *entry)
{
    char path[4600];
    size_t n;
    char *text;
    pt_rt_skill sk;
    int at = snprintf(path, sizeof path, "%s/%s/SKILL.md", dir, entry);
    if (at < 0 || (size_t)at >= sizeof path || !(text = pt_sys_read_file(path, &n)))
        return;
    if (parse_skill((pt_str){text, n}, f) && f->description.n) {
        pt_str name = f->name.n ? pt_buf_str(&f->name) : pt_strz(entry);
        if (valid_name(name)) {
            sk.name = pt_arena_dup(&rt->skills_ar, name);
            sk.description = pt_arena_dup(&rt->skills_ar, pt_buf_str(&f->description));
            sk.path = pt_arena_dup(&rt->skills_ar, pt_strz(path));
            sk.manual = f->manual;
            if ((at = find_skill(rt, sk.name)) >= 0)
                rt->skills[at] = sk;
            else if (pt_rt_grow((void **)&rt->skills, &rt->skcap, rt->nskills + 1, sizeof *rt->skills))
                rt->skills[rt->nskills++] = sk;
        }
    }
    free(text);
}

static int by_name(const void *x, const void *y)
{
    const pt_rt_skill *a = (const pt_rt_skill *)x, *b = (const pt_rt_skill *)y;
    size_t n = a->name.n < b->name.n ? a->name.n : b->name.n;
    int c = memcmp(a->name.p, b->name.p, n);
    return c ? c : (a->name.n > b->name.n) - (a->name.n < b->name.n);
}

static void scan_skills(pt_rt *rt, pt_str cwd)
{
    char dir[4200];
    frontmatter f;
    size_t i;
    rt->nskills = 0;
    pt_arena_reset(&rt->skills_ar);
    pt_buf_init(&f.name, pt_sys_allocator());
    pt_buf_init(&f.description, pt_sys_allocator());
    for (i = 0; i < NROOTS; i++) {
        DIR *d;
        struct dirent *e;
        size_t n;
        if (!root_dir(rt, i, cwd, dir, sizeof dir - 8))
            continue;
        n = strlen(dir);
        memcpy(dir + n, "/skills", 8);
        if (!(d = opendir(dir)))
            continue;
        while ((e = readdir(d)))
            if (e->d_name[0] != '.')
                add_skill(rt, &f, dir, e->d_name);
        closedir(d);
    }
    pt_buf_free(&f.name);
    pt_buf_free(&f.description);
    if (rt->nskills)
        qsort(rt->skills, rt->nskills, sizeof *rt->skills, by_name);
}

const pt_rt_skill *pt_rt_skills(pt_rt *rt, bool rescan, uint32_t *n)
{
    if (rescan && rt->have_prompt)
        scan_skills(rt, pt_session_cwd(rt->a));
    *n = rt->nskills;
    return rt->skills;
}

/* ---- AGENTS.md ---- */

static void put_part(pt_buf *b, pt_str part)
{
    part = pt_str_trim(part);
    if (!part.n)
        return;
    if (b->n)
        pt_buf_putz(b, "\n\n");
    pt_buf_puts(b, part);
}


typedef struct agents_file {
    char path[4200];
    char *text;
    size_t n, keep;
} agents_file;

/* Read only enough to enforce the total prompt cap. The extra byte lets the
 * caller detect truncation without loading an unbounded instruction file. */
static char *read_bounded(const char *path, size_t cap, size_t *n)
{
    char *p = (char *)malloc(cap + 1);
    size_t used = 0;
    int fd;
    if (!p)
        return NULL;
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        free(p);
        return NULL;
    }
    {
        struct stat st;
        if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
            close(fd);
            free(p);
            return NULL;
        }
    }
    while (used < cap + 1) {
        ssize_t got = read(fd, p + used, cap + 1 - used);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            break;
        used += (size_t)got;
    }
    close(fd);
    *n = used;
    return p;
}

/* dir's AGENTS.md, else its CLAUDE.md. */
static bool read_agents(agents_file *f, const char *dir, size_t dn)
{
    static const char *const names[] = {"AGENTS.md", "CLAUDE.md"};
    size_t i;
    for (i = 0; i < 2; i++) {
        int k = snprintf(f->path, sizeof f->path, "%.*s/%s", (int)dn, dir, names[i]);
        if (k > 0 && (size_t)k < sizeof f->path && (f->text = read_bounded(f->path, AGENTS_CAP, &f->n)))
            return true;
    }
    return false;
}

/* The length of cwd's prefix that holds .git, or of cwd itself outside a
 * repository. */
static size_t git_root(pt_str cwd)
{
    char path[4200];
    size_t n = cwd.n;
    struct stat st;
    while (n) {
        int k = snprintf(path, sizeof path, "%.*s/.git", (int)n, cwd.p);
        if (k > 0 && (size_t)k < sizeof path && stat(path, &st) == 0)
            return n;
        while (n && cwd.p[n - 1] != '/')
            n--;
        if (n) /* the slash: "/a/b" -> "/a"; the root "/" is not walked into */
            n--;
    }
    return cwd.n;
}

/* The user's AGENTS.md, then each directory's from the git root down to
 * cwd, each under its path. Over AGENTS_CAP the outermost are cut first:
 * the nearest instructions matter most. */
static void put_agents(pt_rt *rt, pt_str cwd, pt_buf *b)
{
    agents_file *files = (agents_file *)calloc(AGENTS_MAX, sizeof *files);
    size_t nf = 0, at, left = AGENTS_CAP, i;
    char dir[4096];
    if (!files)
        return;
    if (rt->prompt.config_dir && *rt->prompt.config_dir) {
        snprintf(dir, sizeof dir, "%s", rt->prompt.config_dir);
        if (read_agents(&files[nf], dir, strlen(dir)))
            nf++;
    }
    for (at = git_root(cwd); nf < AGENTS_MAX; at++) {
        if ((at == cwd.n || cwd.p[at] == '/') && read_agents(&files[nf], cwd.p, at))
            nf++;
        if (at >= cwd.n)
            break;
    }
    for (i = nf; i-- > 0;) {
        files[i].keep = files[i].n < left ? files[i].n : left;
        while (files[i].keep && files[i].keep < files[i].n &&
               ((unsigned char)files[i].text[files[i].keep] & 0xC0) == 0x80)
            files[i].keep--; /* not inside a UTF-8 character */
        left -= files[i].keep;
    }
    for (i = 0; i < nf; i++) {
        if (!i)
            put_part(b, PT_S("# Instructions from AGENTS.md files\n\n"
                             "From the user's file, then from the project's outermost directory to "
                             "the innermost: where they disagree, the later one wins."));
        pt_buf_putz(b, "\n\n## ");
        pt_buf_putz(b, files[i].path);
        pt_buf_putz(b, "\n\n");
        pt_buf_puts(b, pt_str_trim((pt_str){files[i].text, files[i].keep}));
        if (files[i].keep < files[i].n) {
            char note[120];
            snprintf(note, sizeof note, "\n\n[cut to %zu of %zu bytes: AGENTS.md files are limited to %u KiB]",
                     files[i].keep, files[i].n, AGENTS_CAP / 1024);
            pt_buf_putz(b, note);
        }
        free(files[i].text);
    }
    free(files);
}

/* ---- the system prompt ---- */

bool pt_rt_build_prompt(pt_rt *rt, pt_str cwd, pt_str prompt, pt_buf *system, pt_buf *skills)
{
    pt_arena ar;
    const pt_json *j;
    pt_str sys, append;
    char *file;
    size_t n;
    uint32_t i, listed = 0;
    pt_jw w;
    bool ok;
    pt_arena_init(&ar, pt_sys_allocator(), 0);
    j = prompt.n ? pt_json_parse(prompt, &ar, NULL) : NULL;
    sys = pt_json_gets(j, "system", &ar);
    append = pt_json_gets(j, "append", &ar);
    if (sys.n) {
        put_part(system, sys);
    } else if ((file = read_prompt_file(rt, cwd, "SYSTEM.md", &n))) {
        put_part(system, (pt_str){file, n});
        free(file);
    } else {
        put_part(system, pt_strz(rt->prompt.base ? rt->prompt.base : ""));
    }
    if ((file = read_prompt_file(rt, cwd, "APPEND_SYSTEM.md", &n))) {
        put_part(system, (pt_str){file, n});
        free(file);
    }
    put_agents(rt, cwd, system);
    put_part(system, append);
    pt_arena_free(&ar);

    scan_skills(rt, cwd);
    pt_jw_init(&w, skills);
    pt_jw_arr(&w);
    for (i = 0; rt->prompt.skills_auto && i < rt->nskills; i++) {
        const pt_rt_skill *sk = &rt->skills[i];
        if (sk->manual)
            continue;
        if (!listed++)
            put_part(system, PT_S("# Skills\n\n"
                                  "Skills are instructions for particular tasks, kept in files. When a task "
                                  "matches a skill's description, read its file with the read tool before you "
                                  "start, and follow it. Paths in a skill are relative to its directory.\n\n"
                                  "<available_skills>"));
        pt_buf_putz(system, "\n- ");
        pt_buf_puts(system, sk->name);
        pt_buf_putz(system, ": ");
        pt_buf_puts(system, sk->description);
        pt_buf_putz(system, " (");
        pt_buf_puts(system, sk->path);
        pt_buf_putc(system, ')');
        pt_jw_obj(&w);
        pt_jw_key(&w, "name");
        pt_jw_str(&w, sk->name);
        pt_jw_key(&w, "description");
        pt_jw_str(&w, sk->description);
        pt_jw_key(&w, "path");
        pt_jw_str(&w, sk->path);
        pt_jw_obj_end(&w);
    }
    if (listed)
        pt_buf_putz(system, "\n</available_skills>");
    pt_jw_arr_end(&w);
    ok = !system->oom && !skills->oom;
    return ok;
}

/* ---- invoking skills ---- */

/* The $name at s[i] when it names a skill, else -1. */
static int dollar_skill(const pt_rt *rt, pt_str s, size_t i, size_t *len)
{
    size_t j = i + 1;
    if (s.p[i] != '$' || (i && s.p[i - 1] != ' ' && s.p[i - 1] != '\t' && s.p[i - 1] != '\n'))
        return -1;
    while (j < s.n && name_char(s.p[j]))
        j++;
    while (j > i + 1 && s.p[j - 1] == '.') /* the end of a sentence */
        j--;
    *len = j - i;
    return find_skill(rt, (pt_str){s.p + i + 1, j - i - 1});
}

static bool put_block(pt_buf *out, const pt_rt_skill *sk, char *err, size_t cap)
{
    frontmatter f;
    char path[4200];
    size_t n;
    char *text;
    bool ok;
    snprintf(path, sizeof path, "%.*s", (int)sk->path.n, sk->path.p);
    if (!(text = pt_sys_read_file(path, &n))) {
        snprintf(err, cap, "skill %.*s: cannot read %s", (int)sk->name.n, sk->name.p, path);
        return false;
    }
    pt_buf_init(&f.name, pt_sys_allocator());
    pt_buf_init(&f.description, pt_sys_allocator());
    ok = parse_skill((pt_str){text, n}, &f);
    if (ok) {
        pt_buf_putz(out, SKILL_OPEN);
        pt_buf_puts(out, sk->name);
        pt_buf_putz(out, "\" path=\"");
        pt_buf_puts(out, sk->path);
        pt_buf_putz(out, "\">\n");
        pt_buf_puts(out, f.body);
        pt_buf_putz(out, SKILL_CLOSE "\n\n");
    } else {
        snprintf(err, cap, "skill %.*s: %s has no frontmatter", (int)sk->name.n, sk->name.p, path);
    }
    pt_buf_free(&f.name);
    pt_buf_free(&f.description);
    free(text);
    return ok;
}

int pt_rt_expand_skills(pt_rt *rt, pt_str text, pt_buf *out, char *err, size_t cap)
{
    int picked[16], np = 0, k;
    size_t i, len, start = out->n;
    pt_str rest = text;
    uint32_t nskills;
    pt_rt_skills(rt, true, &nskills);
    if (pt_str_starts(text, PT_S("/skill:"))) {
        pt_str name = {text.p + 7, 0};
        while (7 + name.n < text.n && name_char(name.p[name.n]))
            name.n++;
        if ((k = find_skill(rt, name)) < 0) {
            snprintf(err, cap, "no skill named %.*s", (int)(name.n > 64 ? 64 : name.n), name.p);
            return -1;
        }
        picked[np++] = k;
        rest = pt_str_trim((pt_str){name.p + name.n, text.n - 7 - name.n});
    }
    for (i = 0; i < rest.n && np < 16; i++) {
        int j, dup = 0;
        if ((k = dollar_skill(rt, rest, i, &len)) < 0)
            continue;
        for (j = 0; j < np; j++)
            dup |= picked[j] == k;
        if (!dup)
            picked[np++] = k;
        i += len - 1;
    }
    if (!np)
        return 0;
    for (k = 0; k < np; k++)
        if (!put_block(out, &rt->skills[picked[k]], err, cap)) {
            out->n = start;
            return -1;
        }
    pt_buf_puts(out, rest);
    if (out->oom) {
        snprintf(err, cap, "out of memory");
        out->n = start;
        return -1;
    }
    return 1;
}

bool pt_rt_skill_block(pt_str *text, pt_str *name)
{
    pt_str t = *text, end;
    long q;
    if (!pt_str_starts(t, PT_S(SKILL_OPEN)))
        return false;
    t.p += sizeof SKILL_OPEN - 1;
    t.n -= sizeof SKILL_OPEN - 1;
    if ((q = pt_str_find(t, '"')) < 0)
        return false;
    *name = (pt_str){t.p, (size_t)q};
    for (end = t; end.n >= sizeof SKILL_CLOSE - 1; end.p++, end.n--)
        if (memcmp(end.p, SKILL_CLOSE, sizeof SKILL_CLOSE - 1) == 0 &&
            (end.n == sizeof SKILL_CLOSE - 1 || end.p[sizeof SKILL_CLOSE - 1] == '\n')) {
            end.p += sizeof SKILL_CLOSE - 1;
            end.n -= sizeof SKILL_CLOSE - 1;
            *text = pt_str_trim(end);
            return true;
        }
    return false;
}
