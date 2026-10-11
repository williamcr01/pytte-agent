/* Settings (section 19): built-in defaults, the user file, the project
 * file, then PYTTE_* variables; flags are applied by the caller. Settings
 * changed in the TUI are written back to the user file, one key at a time,
 * leaving every other byte of the file as it was (19.4). */
#define _POSIX_C_SOURCE 200809L
#include "cli/cli.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *const keys[CFG_N] = {"model",     "reasoning",        "tui.thinking",    "tui.symbols",
                                        "tui.pager", "permissions.mode", "compaction.model"};

const char *cli_config_key(int k)
{
    return keys[k];
}

bool cli_effort_parse(pt_str s, pt_reasoning *out)
{
    pt_reasoning r;
    if (!pt_reasoning_parse(s, &r) || r == PT_REASON_DEFAULT)
        return false;
    *out = r;
    return true;
}

void cli_config_set(cli_config *c, int k, const char *v, const char *src)
{
    snprintf(c->val[k], sizeof c->val[k], "%s", v);
    c->src[k] = src;
}

/* The member at a dotted path, or NULL. */
static const pt_json *lookup(const pt_json *j, const char *key)
{
    char part[64];
    while (j && *key) {
        const char *dot = strchr(key, '.');
        size_t n = dot ? (size_t)(dot - key) : strlen(key);
        if (n >= sizeof part)
            return NULL;
        memcpy(part, key, n);
        part[n] = 0;
        j = pt_json_is(j, PT_J_OBJ) ? pt_json_get(j, part) : NULL;
        key += n + (dot != NULL);
    }
    return j;
}

static void load_file(cli_config *c, const char *path, const char *src, char *warn, size_t cap)
{
    size_t n;
    char *text = pt_sys_read_file(path, &n);
    pt_arena ar;
    pt_json *j;
    pt_json_err e;
    int k;
    if (!text)
        return;
    pt_arena_init(&ar, pt_sys_allocator(), 0);
    j = pt_json_parse((pt_str){text, n}, &ar, &e);
    if (!j || !pt_json_is(j, PT_J_OBJ)) {
        if (!warn[0])
            snprintf(warn, cap, "%s: %s; ignored", path, j ? "not a JSON object" : e.msg);
    } else {
        for (k = 0; k < CFG_N; k++) {
            const pt_json *v = lookup(j, keys[k]);
            if (v && pt_json_is(v, PT_J_STR)) {
                pt_str s = pt_json_str(v, &ar);
                if (!memchr(s.p, 0, s.n) && s.n < sizeof c->val[k]) {
                    snprintf(c->val[k], sizeof c->val[k], "%.*s", (int)s.n, s.p);
                    c->src[k] = src;
                }
            }
        }
    }
    pt_arena_free(&ar);
    free(text);
}

void cli_config_load(cli_config *c, char *warn, size_t cap)
{
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME"), *e;
    int k;
    memset(c, 0, sizeof *c);
    warn[0] = 0;
    for (k = 0; k < CFG_N; k++)
        c->src[k] = "default";
    cli_config_set(c, CFG_REASONING, pt_reasoning_name(DEFAULT_EFFORT), "default");
    cli_config_set(c, CFG_THINKING, "collapsed", "default");
    cli_config_set(c, CFG_SYMBOLS, "auto", "default");
    cli_config_set(c, CFG_PERMS, "ask", "default");
    e = getenv("PAGER");
    cli_config_set(c, CFG_PAGER, e && *e ? e : "less -R", "default");
    if (xdg && *xdg)
        snprintf(c->user_path, sizeof c->user_path, "%s/pytte/config.json", xdg);
    else
        snprintf(c->user_path, sizeof c->user_path, "%s/.config/pytte/config.json", home ? home : ".");
    load_file(c, c->user_path, "user", warn, cap);
    load_file(c, ".pytte/config.json", "project", warn, cap);
    if ((e = getenv("PYTTE_MODEL")) && *e)
        cli_config_set(c, CFG_MODEL, e, "env");
    if ((e = getenv("PYTTE_COMPACT_MODEL")) && *e)
        cli_config_set(c, CFG_COMPACT_MODEL, e, "env");
    if ((e = getenv("PYTTE_EFFORT")) && *e)
        cli_config_set(c, CFG_REASONING, e, "env");
    if ((e = getenv("PYTTE_PERMISSION_MODE")) && *e)
        cli_config_set(c, CFG_PERMS, e, "env");
    if ((e = getenv("PYTTE_ASCII")) && strcmp(e, "1") == 0)
        cli_config_set(c, CFG_SYMBOLS, "ascii", "env");
}

/* ---- writing one key ---- */

static size_t off(pt_str src, pt_str inner)
{
    return (size_t)(inner.p - src.p);
}

bool cli_json_set(pt_str src, const char *key, pt_str value, pt_buf *out, char *err, size_t cap)
{
    pt_arena ar;
    pt_json *root;
    const pt_json *obj, *m = NULL;
    pt_json_err e;
    const char *k = key;
    char part[64];
    bool ok = false;
    pt_arena_init(&ar, pt_sys_allocator(), 0);
    if (!pt_str_trim(src).n)
        src = PT_S("{}\n");
    root = pt_json_parse(src, &ar, &e);
    if (!root || !pt_json_is(root, PT_J_OBJ)) {
        snprintf(err, cap, "%s", root ? "not a JSON object" : e.msg);
        goto done;
    }
    /* Walk the path as far as it exists. */
    obj = root;
    for (;;) {
        const char *dot = strchr(k, '.');
        size_t n = dot ? (size_t)(dot - k) : strlen(k);
        if (n >= sizeof part)
            goto done;
        memcpy(part, k, n);
        part[n] = 0;
        m = pt_json_get(obj, part);
        if (!m || !dot)
            break;
        if (!pt_json_is(m, PT_J_OBJ)) {
            snprintf(err, cap, "%.*s is not an object", (int)(k - key + n), key);
            goto done;
        }
        obj = m;
        k = dot + 1;
    }
    pt_buf_reset(out);
    if (m) { /* replace the value */
        pt_buf_put(out, src.p, off(src, m->raw));
        pt_buf_puts(out, value);
        pt_buf_put(out, m->raw.p + m->raw.n, src.n - off(src, m->raw) - m->raw.n);
    } else { /* add "rest": value, nesting what is missing */
        const pt_json *last = obj->child;
        size_t at, depth = 0;
        while (last && last->next)
            last = last->next;
        /* after the last member, or after the opening brace */
        at = last ? off(src, last->raw) + last->raw.n : off(src, obj->raw) + 1;
        pt_buf_put(out, src.p, at);
        pt_buf_putz(out, last ? ", " : "");
        for (;;) {
            const char *dot = strchr(k, '.');
            size_t n = dot ? (size_t)(dot - k) : strlen(k);
            pt_json_quote(out, (pt_str){k, n});
            pt_buf_putz(out, ": ");
            if (!dot)
                break;
            pt_buf_putc(out, '{');
            depth++;
            k = dot + 1;
        }
        pt_buf_puts(out, value);
        while (depth--)
            pt_buf_putc(out, '}');
        pt_buf_put(out, src.p + at, src.n - at);
    }
    ok = true;
done:
    pt_arena_free(&ar);
    return ok;
}

bool cli_write_atomic(const char *path, pt_str bytes, char *err, size_t cap)
{
    char tmp[4200], dir[4096];
    struct stat st;
    mode_t mode = stat(path, &st) == 0 ? (st.st_mode & 07777) : 0600;
    char *slash;
    int fd;
    size_t done = 0;
    snprintf(dir, sizeof dir, "%s", path);
    if ((slash = strrchr(dir, '/'))) { /* create the directory (0700) */
        char *p;
        *slash = 0;
        for (p = dir + 1; *p; p++)
            if (*p == '/') {
                *p = 0;
                mkdir(dir, 0700);
                *p = '/';
            }
        mkdir(dir, 0700);
    }
    snprintf(tmp, sizeof tmp, "%s.tmp%ld", path, (long)getpid());
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        snprintf(err, cap, "%s: %s", tmp, strerror(errno));
        return false;
    }
    while (done < bytes.n) {
        ssize_t w = write(fd, bytes.p + done, bytes.n - done);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            break;
        done += (size_t)w;
    }
    if (done < bytes.n || fchmod(fd, mode) != 0 || fsync(fd) != 0) {
        snprintf(err, cap, "%s: %s", tmp, strerror(errno));
        close(fd);
        unlink(tmp);
        return false;
    }
    close(fd);
    if (rename(tmp, path) != 0) {
        snprintf(err, cap, "%s: %s", path, strerror(errno));
        unlink(tmp);
        return false;
    }
    return true;
}

bool cli_config_save(cli_config *c, int k, const char *v, char *err, size_t cap)
{
    /* an environment variable, a flag or the project file still wins */
    if (strcmp(c->src[k], "default") == 0 || strcmp(c->src[k], "user") == 0)
        cli_config_set(c, k, v, "user");
    return cli_config_write(c->user_path, k, v, err, cap);
}

bool cli_config_write(const char *path, int k, const char *v, char *err, size_t cap)
{
    size_t n = 0;
    char *text;
    pt_buf value, out;
    bool ok;
    errno = 0;
    text = pt_sys_read_file(path, &n);
    if (!text && errno != ENOENT) {
        snprintf(err, cap, "cannot read %s: %s", path, strerror(errno));
        return false;
    }
    pt_buf_init(&value, pt_sys_allocator());
    pt_buf_init(&out, pt_sys_allocator());
    pt_json_quote(&value, pt_strz(v));
    if (value.oom) {
        snprintf(err, cap, "out of memory saving setting");
        ok = false;
        goto done;
    }
    ok = cli_json_set(text ? (pt_str){text, n} : PT_S("{}\n"), keys[k], pt_buf_str(&value), &out, err,
                      cap);
    if (!ok) { /* never overwrite a file that does not parse */
        char why[256];
        snprintf(why, sizeof why, "%s", err);
        snprintf(err, cap, "not saved, %s: %s", path, why);
    } else {
        ok = cli_write_atomic(path, pt_buf_str(&out), err, cap);
    }
done:
    pt_buf_free(&value);
    pt_buf_free(&out);
    free(text);
    return ok;
}
