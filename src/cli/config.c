/* Settings (section 19): built-in defaults, the user file, the project
 * file, then PYTTE_* variables; flags are applied by the caller. Settings
 * changed in the TUI are written back to the user file, one key at a time,
 * keeping every other key; the file is rewritten indented (19.4). */
#define _POSIX_C_SOURCE 200809L
#include "cli/cli.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *const keys[CFG_N] = {"model",     "reasoning",        "tui.thinking",     "tui.symbols",
                                        "tui.pager", "permissions.mode", "compaction.model", "skills.auto",
                                        "search.backend"};

void cli_config_dir(char *out, size_t cap)
{
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xdg && *xdg)
        snprintf(out, cap, "%s/pytte", xdg);
    else
        snprintf(out, cap, "%s/.config/pytte", home ? home : ".");
}

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

/* Applies the file's settings; the user file also gives the catalog's
 * providers. False when the file could not be read. */
static bool load_file(cli_config *c, const char *path, const char *src, char *warn, size_t cap)
{
    size_t n;
    char *text = pt_sys_read_file(path, &n);
    pt_arena ar;
    pt_json *j;
    pt_json_err e;
    int k;
    bool ok;
    if (!text)
        return false;
    pt_arena_init(&ar, pt_sys_allocator(), 0);
    j = pt_json_parse((pt_str){text, n}, &ar, &e);
    if (!j || !pt_json_is(j, PT_J_OBJ)) {
        if (!warn[0])
            snprintf(warn, cap, "%s: %s; ignored", path, j ? "not a JSON object" : e.msg);
    } else {
        for (k = 0; k < CFG_N; k++) {
            const pt_json *v = lookup(j, keys[k]);
            if (pt_json_is(v, PT_J_TRUE) || pt_json_is(v, PT_J_FALSE)) { /* "skills.auto": false */
                cli_config_set(c, k, pt_json_is(v, PT_J_TRUE) ? "on" : "off", src);
            } else if (v && pt_json_is(v, PT_J_STR)) {
                pt_str s = pt_json_str(v, &ar);
                if (!memchr(s.p, 0, s.n) && s.n < sizeof c->val[k]) {
                    snprintf(c->val[k], sizeof c->val[k], "%.*s", (int)s.n, s.p);
                    c->src[k] = src;
                }
            }
        }
        if (!warn[0])
            cli_perms_check(j, path, warn, cap);
        {
            const pt_json *tools = pt_json_get(j, "tools"), *m;
            for (m = pt_json_is(tools, PT_J_OBJ) ? tools->child : NULL; m; m = m->next)
                if ((pt_json_is(m, PT_J_TRUE) || pt_json_is(m, PT_J_FALSE)) &&
                    !cli_tool_set(c, m->key, pt_json_is(m, PT_J_TRUE), src) && !warn[0])
                    snprintf(warn, cap, "%s: tools.%.*s ignored", path, (int)(m->key.n > 100 ? 100 : m->key.n),
                             m->key.p);
        }
        if (strcmp(src, "user") == 0)
            cli_catalog_load(pt_json_get(j, "providers"), path, warn, cap);
        else if (pt_json_get(j, "providers") && !warn[0])
            snprintf(warn, cap, "%s: providers are read only from %s", path, c->user_path);
    }
    ok = j && pt_json_is(j, PT_J_OBJ);
    pt_arena_free(&ar);
    free(text);
    return ok;
}

void cli_config_load(cli_config *c, char *warn, size_t cap)
{
    const char *e;
    char dir[4000];
    int k;
    memset(c, 0, sizeof *c);
    warn[0] = 0;
    for (k = 0; k < CFG_N; k++)
        c->src[k] = "default";
    cli_config_set(c, CFG_REASONING, pt_reasoning_name(DEFAULT_EFFORT), "default");
    cli_config_set(c, CFG_THINKING, "hidden", "default");
    cli_config_set(c, CFG_SYMBOLS, "auto", "default");
    cli_config_set(c, CFG_PERMS, "ask", "default");
    cli_config_set(c, CFG_SKILLS, "on", "default");
    e = getenv("PAGER");
    cli_config_set(c, CFG_PAGER, e && *e ? e : "less -R", "default");
    cli_config_dir(dir, sizeof dir);
    snprintf(c->user_path, sizeof c->user_path, "%s/config.json", dir);
    if (!load_file(c, c->user_path, "user", warn, cap))
        cli_catalog_load(NULL, c->user_path, warn, cap);
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

/* v indented by depth levels of two spaces, scalars copied as they were.
 * Empty containers stay on one line. */
static void pretty(pt_buf *b, const pt_json *v, int depth)
{
    const pt_json *m;
    char open, close;
    int i;
    if (!pt_json_is(v, PT_J_OBJ) && !pt_json_is(v, PT_J_ARR)) {
        pt_buf_puts(b, v->raw);
        return;
    }
    open = pt_json_is(v, PT_J_OBJ) ? '{' : '[';
    close = open == '{' ? '}' : ']';
    pt_buf_putc(b, open);
    for (m = v->child; m; m = m->next) {
        pt_buf_putz(b, m == v->child ? "\n" : ",\n");
        for (i = 0; i <= depth; i++)
            pt_buf_putz(b, "  ");
        if (open == '{') {
            pt_json_quote(b, m->key);
            pt_buf_putz(b, ": ");
        }
        pretty(b, m, depth + 1);
    }
    if (v->child) {
        pt_buf_putc(b, '\n');
        for (i = 0; i < depth; i++)
            pt_buf_putz(b, "  ");
    }
    pt_buf_putc(b, close);
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
    if (out->oom) {
        snprintf(err, cap, "out of memory building setting");
        goto done;
    }
    /* Indent the result. It parses: value is JSON and src parsed. */
    {
        pt_arena ar2;
        pt_buf b;
        pt_arena_init(&ar2, pt_sys_allocator(), 0);
        pt_buf_init(&b, pt_sys_allocator());
        root = pt_json_parse(pt_buf_str(out), &ar2, &e);
        if (!root) {
            snprintf(err, cap, "not a JSON value: %.*s", (int)value.n, value.p);
        } else {
            pretty(&b, root, 0);
            pt_buf_putc(&b, '\n');
            if (b.oom) {
                snprintf(err, cap, "out of memory building setting");
            } else {
                pt_buf_reset(out);
                pt_buf_puts(out, pt_buf_str(&b));
                ok = !out->oom;
                if (!ok)
                    snprintf(err, cap, "out of memory building setting");
            }
        }
        pt_buf_free(&b);
        pt_arena_free(&ar2);
    }
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
    if (done < bytes.n || fchmod(fd, mode) != 0 || pt_sys_sync_fd(fd, false) != 0) {
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
    /* Else a power loss may undo the rename; but the file has changed, so
     * a failure here is no failure to write it. */
    (void)pt_sys_sync_parent(path, true);
    return true;
}

bool cli_config_save(cli_config *c, int k, const char *v, char *err, size_t cap)
{
    /* an environment variable, a flag or the project file still wins */
    if (strcmp(c->src[k], "default") == 0 || strcmp(c->src[k], "user") == 0)
        cli_config_set(c, k, v, "user");
    return cli_config_write(c->user_path, k, v, err, cap);
}

/* Set key to value (JSON) in the file at path. */
static bool write_key(const char *path, const char *key, pt_str value, char *err, size_t cap)
{
    size_t n = 0;
    char *text;
    pt_buf out;
    bool ok;
    errno = 0;
    text = pt_sys_read_file(path, &n);
    if (!text && errno != ENOENT) {
        snprintf(err, cap, "cannot read %s: %s", path, strerror(errno));
        return false;
    }
    pt_buf_init(&out, pt_sys_allocator());
    ok = cli_json_set(text ? (pt_str){text, n} : PT_S("{}\n"), key, value, &out, err, cap);
    if (!ok) { /* never overwrite a file that does not parse */
        char why[256];
        snprintf(why, sizeof why, "%s", err);
        snprintf(err, cap, "not saved, %s: %s", path, why);
    } else {
        ok = cli_write_atomic(path, pt_buf_str(&out), err, cap);
    }
    pt_buf_free(&out);
    free(text);
    return ok;
}

bool cli_config_write(const char *path, int k, const char *v, char *err, size_t cap)
{
    pt_buf value;
    bool ok;
    pt_buf_init(&value, pt_sys_allocator());
    pt_json_quote(&value, pt_strz(v));
    if ((ok = !value.oom))
        ok = write_key(path, keys[k], pt_buf_str(&value), err, cap);
    else
        snprintf(err, cap, "out of memory saving setting");
    pt_buf_free(&value);
    return ok;
}

/* ---- tools ---- */

static const char *const tools_off_by_default[] = {"web_fetch", "web_search"};
/* A tool that only serves another is on and off with it. */
static const struct {
    const char *tool, *follower;
} tool_pairs[] = {{"bash", "bash_poll"}};

const char *cli_tool_leader(pt_str name)
{
    size_t i;
    for (i = 0; i < sizeof tool_pairs / sizeof tool_pairs[0]; i++)
        if (pt_str_eqz(name, tool_pairs[i].follower))
            return tool_pairs[i].tool;
    return NULL;
}

const char *cli_tool_follower(pt_str name)
{
    size_t i;
    for (i = 0; i < sizeof tool_pairs / sizeof tool_pairs[0]; i++)
        if (pt_str_eqz(name, tool_pairs[i].tool))
            return tool_pairs[i].follower;
    return NULL;
}

const cli_tool_cfg *cli_tool_entry(const cli_config *c, pt_str name)
{
    uint32_t i;
    for (i = 0; i < c->ntools; i++)
        if (pt_str_eqz(name, c->tools[i].name))
            return &c->tools[i];
    return NULL;
}

bool cli_tool_on(const cli_config *c, pt_str name)
{
    const cli_tool_cfg *t = cli_tool_entry(c, name);
    size_t i;
    if (cli_tool_leader(name))
        return cli_tool_on(c, pt_strz(cli_tool_leader(name)));
    if (t)
        return t->on;
    for (i = 0; i < sizeof tools_off_by_default / sizeof tools_off_by_default[0]; i++)
        if (pt_str_eqz(name, tools_off_by_default[i]))
            return false;
    return true;
}

bool cli_tool_set(cli_config *c, pt_str name, bool on, const char *src)
{
    cli_tool_cfg *t = (cli_tool_cfg *)cli_tool_entry(c, name);
    if (!name.n || name.n >= sizeof c->tools[0].name || memchr(name.p, '.', name.n) || memchr(name.p, 0, name.n))
        return false;
    if (!t) {
        if (c->ntools == CLI_TOOLS_MAX)
            return false;
        t = &c->tools[c->ntools++];
        memcpy(t->name, name.p, name.n);
        t->name[name.n] = 0;
    }
    t->on = on;
    t->src = src;
    return true;
}

/* Append "name\n"; CLI_TOOLS_OFF_CAP holds every name there can be. */
static void put_name(char *out, size_t *n, const char *name)
{
    size_t len = strlen(name);
    if (*n + len + 2 > CLI_TOOLS_OFF_CAP)
        return;
    memcpy(out + *n, name, len);
    out[*n + len] = '\n';
    *n += len + 1;
    out[*n] = 0;
}

void cli_tools_off(const cli_config *c, char out[CLI_TOOLS_OFF_CAP])
{
    size_t i, n = 0;
    out[0] = 0;
    for (i = 0; i < sizeof tools_off_by_default / sizeof tools_off_by_default[0]; i++)
        if (!cli_tool_entry(c, pt_strz(tools_off_by_default[i])))
            put_name(out, &n, tools_off_by_default[i]);
    for (i = 0; i < c->ntools; i++)
        if (!c->tools[i].on)
            put_name(out, &n, c->tools[i].name);
    for (i = 0; i < sizeof tool_pairs / sizeof tool_pairs[0]; i++)
        if (!cli_tool_on(c, pt_strz(tool_pairs[i].tool)))
            put_name(out, &n, tool_pairs[i].follower);
}

bool cli_config_write_tool(const char *path, const char *name, bool on, char *err, size_t cap)
{
    char key[160];
    if (strchr(name, '.') || snprintf(key, sizeof key, "tools.%s", name) >= (int)sizeof key) {
        snprintf(err, cap, "not saved: %.100s cannot be a config key", name);
        return false;
    }
    return write_key(path, key, on ? PT_S("true") : PT_S("false"), err, cap);
}

/* ---- API keys (auth.json) ---- */

/* A key, kept for the life of the process: a replaced key stays reachable
 * through older, so a pointer handed out never dangles. */
typedef struct auth_key {
    struct auth_key *older;
    char key[1];
} auth_key;

static struct {
    char provider[64];
    auth_key *key;
} auth[16];
static int nauth;

/* A login, kept like a key: a replaced one stays reachable. */
typedef struct auth_login {
    struct auth_login *older;
    cli_login l; /* its strings point into data */
    char data[1];
} auth_login;

static struct {
    char provider[64];
    auth_login *login;
} logins[8];
static int nlogins;
static bool auth_loaded;
static pthread_mutex_t auth_lock = PTHREAD_MUTEX_INITIALIZER;

void cli_auth_path(char *out, size_t cap)
{
    char dir[4000];
    cli_config_dir(dir, sizeof dir);
    snprintf(out, cap, "%s/auth.json", dir);
}

bool cli_auth_valid(pt_str key)
{
    size_t i;
    if (!key.n || key.n > 4096)
        return false;
    for (i = 0; i < key.n; i++)
        if ((unsigned char)key.p[i] <= ' ' || (unsigned char)key.p[i] >= 0x7f)
            return false;
    return true;
}

/* Under auth_lock. False when out of memory or slots. */
static bool auth_put(const char *provider, pt_str key)
{
    auth_key *k;
    int i;
    if (strlen(provider) >= sizeof auth[0].provider)
        return false;
    for (i = 0; i < nauth && strcmp(auth[i].provider, provider) != 0; i++)
        ;
    if (i == nauth && nauth == (int)(sizeof auth / sizeof auth[0]))
        return false;
    if (!(k = (auth_key *)malloc(sizeof *k + key.n)))
        return false;
    memcpy(k->key, key.p, key.n);
    k->key[key.n] = 0;
    if (i == nauth) {
        snprintf(auth[nauth].provider, sizeof auth[0].provider, "%s", provider);
        auth[nauth++].key = NULL;
    }
    k->older = auth[i].key;
    auth[i].key = k;
    return true;
}

/* A login's token: printable ASCII without spaces, 1 to 16384 bytes
 * (access tokens are JWTs, longer than keys). */
static bool login_valid(pt_str s)
{
    size_t i;
    if (!s.n || s.n > 16384)
        return false;
    for (i = 0; i < s.n; i++)
        if ((unsigned char)s.p[i] <= ' ' || (unsigned char)s.p[i] >= 0x7f)
            return false;
    return true;
}

/* Under auth_lock. False when out of memory or slots, or the strings are
 * not tokens. */
static bool login_put(const char *provider, pt_str access, pt_str refresh, pt_str account, int64_t expires)
{
    auth_login *k;
    char *d;
    int i;
    if (strlen(provider) >= sizeof logins[0].provider || !login_valid(access) || !login_valid(refresh) ||
        (account.n && !login_valid(account)))
        return false;
    for (i = 0; i < nlogins && strcmp(logins[i].provider, provider) != 0; i++)
        ;
    if (i == nlogins && nlogins == (int)(sizeof logins / sizeof logins[0]))
        return false;
    if (!(k = (auth_login *)malloc(sizeof *k + access.n + refresh.n + account.n + 3)))
        return false;
    d = k->data;
    k->l.access = d;
    memcpy(d, access.p, access.n);
    d += access.n;
    *d++ = 0;
    k->l.refresh = d;
    memcpy(d, refresh.p, refresh.n);
    d += refresh.n;
    *d++ = 0;
    k->l.account_id = d;
    memcpy(d, account.p, account.n);
    d[account.n] = 0;
    k->l.expires = expires;
    if (i == nlogins) {
        snprintf(logins[nlogins].provider, sizeof logins[0].provider, "%s", provider);
        logins[nlogins++].login = NULL;
    }
    k->older = logins[i].login;
    logins[i].login = k;
    return true;
}

/* Under auth_lock: a login's member of auth.json, when it holds one. */
static void login_load(const char *provider, const pt_json *v, pt_arena *ar)
{
    const pt_json *access = pt_json_get(v, "access"), *refresh = pt_json_get(v, "refresh");
    int64_t expires = pt_json_geti(v, "expires", 0);
    if (!pt_json_is(access, PT_J_STR) || !pt_json_is(refresh, PT_J_STR))
        return;
    login_put(provider, pt_json_str(access, ar), pt_json_str(refresh, ar), pt_json_gets(v, "account_id", ar),
              expires);
}

/* Under auth_lock: {"anthropic": {"key": "..."}, "openai-codex": {"access":
 * ...}, ...}. A file that cannot be read or parsed gives no keys. */
static void auth_load(void)
{
    char path[4200];
    size_t n;
    char *text;
    pt_arena ar;
    const pt_json *j, *m;
    pt_json_err e;
    auth_loaded = true;
    cli_auth_path(path, sizeof path);
    if (!(text = pt_sys_read_file(path, &n)))
        return;
    pt_arena_init(&ar, pt_sys_allocator(), 0);
    j = pt_json_parse((pt_str){text, n}, &ar, &e);
    for (m = pt_json_is(j, PT_J_OBJ) ? j->child : NULL; m; m = m->next) {
        const pt_json *v = pt_json_is(m, PT_J_OBJ) ? pt_json_get(m, "key") : NULL;
        char provider[64];
        pt_str key;
        if (!pt_json_is(m, PT_J_OBJ) || m->key.n >= sizeof provider || memchr(m->key.p, 0, m->key.n))
            continue;
        snprintf(provider, sizeof provider, "%.*s", (int)m->key.n, m->key.p);
        login_load(provider, m, &ar);
        if (!pt_json_is(v, PT_J_STR))
            continue;
        key = pt_json_str(v, &ar);
        if (!cli_auth_valid(key))
            continue;
        auth_put(provider, key);
    }
    pt_arena_free(&ar);
    free(text);
}

const char *cli_auth_key(const char *provider)
{
    const char *key = NULL;
    int i;
    pthread_mutex_lock(&auth_lock);
    if (!auth_loaded)
        auth_load();
    for (i = 0; i < nauth; i++)
        if (strcmp(auth[i].provider, provider) == 0)
            key = auth[i].key->key;
    pthread_mutex_unlock(&auth_lock);
    return key;
}

bool cli_auth_save(const char *provider, pt_str key, char *err, size_t cap)
{
    char path[4200], dotted[80];
    size_t n = 0;
    char *text;
    pt_buf value, out;
    bool ok = false;
    if (!cli_auth_valid(key)) {
        snprintf(err, cap, "not an API key");
        return false;
    }
    if (strlen(provider) >= sizeof auth[0].provider || strchr(provider, '.')) {
        snprintf(err, cap, "not a provider: %s", provider);
        return false;
    }
    cli_auth_path(path, sizeof path);
    snprintf(dotted, sizeof dotted, "%s.key", provider);
    pthread_mutex_lock(&auth_lock);
    if (!auth_loaded)
        auth_load();
    errno = 0;
    text = pt_sys_read_file(path, &n);
    if (!text && errno != ENOENT) {
        snprintf(err, cap, "cannot read %s: %s", path, strerror(errno));
        pthread_mutex_unlock(&auth_lock);
        return false;
    }
    pt_buf_init(&value, pt_sys_allocator());
    pt_buf_init(&out, pt_sys_allocator());
    pt_json_quote(&value, key);
    if (value.oom) {
        snprintf(err, cap, "out of memory saving the key");
    } else if (!cli_json_set(text ? (pt_str){text, n} : PT_S("{}\n"), dotted, pt_buf_str(&value), &out, err,
                             cap)) { /* never overwrite a file that does not parse */
        char why[256];
        snprintf(why, sizeof why, "%s", err);
        snprintf(err, cap, "not saved, %s: %s", path, why);
    } else if (cli_write_atomic(path, pt_buf_str(&out), err, cap)) {
        ok = true;
        if (!auth_put(provider, key)) /* saved; this process still has the old one */
            snprintf(err, cap, "saved; restart pytte to use it");
        else
            err[0] = 0;
    }
    pthread_mutex_unlock(&auth_lock);
    pt_buf_free(&value);
    pt_buf_free(&out);
    free(text);
    return ok;
}

const char *const cli_search_backends[2] = {"exa", "parallel"};

const char *cli_search_env(const char *backend)
{
    return strcmp(backend, "exa") == 0 ? "EXA_API_KEY" : strcmp(backend, "parallel") == 0 ? "PARALLEL_API_KEY" : NULL;
}

/* "search:<backend>", the member of auth.json. */
static bool search_slot(const char *backend, char out[32])
{
    return cli_search_env(backend) && snprintf(out, 32, "search:%s", backend) < 32;
}

const char *cli_search_saved(const char *backend)
{
    char slot[32];
    return search_slot(backend, slot) ? cli_auth_key(slot) : NULL;
}

bool cli_search_save(const char *backend, pt_str key, char *err, size_t cap)
{
    char slot[32];
    if (!search_slot(backend, slot)) {
        snprintf(err, cap, "not a search backend: %s", backend);
        return false;
    }
    return cli_auth_save(slot, key, err, cap);
}

bool cli_search_key(void *ud, const char *backend, char *out, size_t cap)
{
    const char *env = cli_search_env(backend), *e, *saved;
    (void)ud;
    if (!env)
        return false;
    e = getenv(env);
    if (e && *e && strlen(e) < cap) {
        memcpy(out, e, strlen(e) + 1);
        return true;
    }
    saved = cli_search_saved(backend);
    if (!saved || strlen(saved) >= cap)
        return false;
    memcpy(out, saved, strlen(saved) + 1);
    return true;
}

bool cli_auth_login(const char *provider, bool reload, cli_login *out)
{
    bool ok = false;
    int i;
    pthread_mutex_lock(&auth_lock);
    if (!auth_loaded)
        auth_load();
    else if (reload) { /* another pytte may have refreshed it */
        char path[4200];
        size_t n;
        char *text;
        cli_auth_path(path, sizeof path);
        if ((text = pt_sys_read_file(path, &n))) {
            pt_arena ar;
            const pt_json *v;
            pt_arena_init(&ar, pt_sys_allocator(), 0);
            v = pt_json_get(pt_json_parse((pt_str){text, n}, &ar, NULL), provider);
            for (i = 0; i < nlogins && strcmp(logins[i].provider, provider) != 0; i++)
                ;
            if (pt_json_is(v, PT_J_OBJ) &&
                (i == nlogins || !pt_str_eqz(pt_json_gets(v, "access", &ar), logins[i].login->l.access)))
                login_load(provider, v, &ar);
            pt_arena_free(&ar);
            free(text);
        }
    }
    for (i = 0; i < nlogins; i++)
        if (strcmp(logins[i].provider, provider) == 0) {
            *out = logins[i].login->l;
            ok = true;
        }
    pthread_mutex_unlock(&auth_lock);
    return ok;
}

bool cli_auth_login_save(const char *provider, const cli_login *l, char *err, size_t cap)
{
    char path[4200], num[32];
    size_t n = 0;
    char *text;
    pt_buf value, out;
    bool ok = false;
    if (strlen(provider) >= sizeof logins[0].provider || strchr(provider, '.')) {
        snprintf(err, cap, "not a provider: %s", provider);
        return false;
    }
    if (!login_valid(pt_strz(l->access)) || !login_valid(pt_strz(l->refresh))) {
        snprintf(err, cap, "the login's tokens are not valid");
        return false;
    }
    cli_auth_path(path, sizeof path);
    pthread_mutex_lock(&auth_lock);
    if (!auth_loaded)
        auth_load();
    errno = 0;
    text = pt_sys_read_file(path, &n);
    if (!text && errno != ENOENT) {
        snprintf(err, cap, "cannot read %s: %s", path, strerror(errno));
        pthread_mutex_unlock(&auth_lock);
        return false;
    }
    pt_buf_init(&value, pt_sys_allocator());
    pt_buf_init(&out, pt_sys_allocator());
    pt_buf_putz(&value, "{\"access\": ");
    pt_json_quote(&value, pt_strz(l->access));
    pt_buf_putz(&value, ", \"refresh\": ");
    pt_json_quote(&value, pt_strz(l->refresh));
    snprintf(num, sizeof num, "%lld", (long long)l->expires);
    pt_buf_putz(&value, ", \"expires\": ");
    pt_buf_putz(&value, num);
    if (l->account_id && l->account_id[0]) {
        pt_buf_putz(&value, ", \"account_id\": ");
        pt_json_quote(&value, pt_strz(l->account_id));
    }
    pt_buf_putc(&value, '}');
    if (value.oom) {
        snprintf(err, cap, "out of memory saving the login");
    } else if (!cli_json_set(text ? (pt_str){text, n} : PT_S("{}\n"), provider, pt_buf_str(&value), &out, err,
                             cap)) { /* never overwrite a file that does not parse */
        char why[256];
        snprintf(why, sizeof why, "%s", err);
        snprintf(err, cap, "not saved, %s: %s", path, why);
    } else if (cli_write_atomic(path, pt_buf_str(&out), err, cap)) {
        ok = true;
        err[0] = 0;
    }
    /* Used from now on even when not saved: a refresh may have replaced
     * the refresh token, so the old login would not work again. */
    if (!login_put(provider, pt_strz(l->access), pt_strz(l->refresh), pt_strz(l->account_id ? l->account_id : ""),
                   l->expires) &&
        ok)
        snprintf(err, cap, "saved; restart pytte to use it");
    pthread_mutex_unlock(&auth_lock);
    pt_buf_free(&value);
    pt_buf_free(&out);
    free(text);
    return ok;
}
