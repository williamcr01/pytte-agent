/* MCP servers from config files, the trust of a project's servers, OAuth
 * tokens, and the browser for logins.
 *
 *   "mcpServers": {
 *     "github": {"command": "npx", "args": ["-y", "@modelcontextprotocol/server-github"],
 *                "env": {"GITHUB_TOKEN": "${GITHUB_TOKEN}"}},
 *     "linear": {"url": "https://mcp.linear.app/mcp"}
 *   }
 *
 * The user's config.json runs its servers. A project's (.mcp.json, or
 * "mcpServers" in .pytte/config.json, in the working directory) start a
 * program, so they run only once trusted: trust.json, next to config.json,
 * keeps a hash of each trusted project's server config; any change asks
 * again. A project server replaces the user's of its name when it runs.
 *
 * ${VAR} and ${VAR:-default} in commands, arguments, env, URLs and
 * headers take the environment's values. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cli/cli.h"
#include "rt/mcp.h"

/* ---- reading ---- */

static void warnf(cli_mcp *m, const char *where, const char *name, const char *what)
{
    if (m->warn[0])
        return;
    if (name)
        snprintf(m->warn, sizeof m->warn, "%.300s: mcpServers.%.40s: %.200s", where, name, what);
    else
        snprintf(m->warn, sizeof m->warn, "%.300s: %.250s", where, what);
}

/* ${VAR}, ${VAR:-default}: the environment's value */
static const char *expand(pt_arena *a, pt_str s)
{
    pt_buf b;
    size_t i = 0;
    const char *out;
    if (!s.n || !memchr(s.p, '$', s.n))
        return pt_arena_cstr(a, s);
    pt_buf_init(&b, pt_sys_allocator());
    while (i < s.n) {
        size_t e, colon;
        if (s.p[i] != '$' || i + 1 >= s.n || s.p[i + 1] != '{') {
            pt_buf_putc(&b, s.p[i++]);
            continue;
        }
        for (e = i + 2; e < s.n && s.p[e] != '}'; e++)
            ;
        if (e >= s.n) {
            pt_buf_put(&b, s.p + i, s.n - i);
            break;
        }
        for (colon = i + 2; colon < e && !(s.p[colon] == ':' && colon + 1 < e && s.p[colon + 1] == '-'); colon++)
            ;
        {
            char name[128];
            const char *v;
            size_t nl = colon - i - 2;
            if (nl >= sizeof name)
                nl = sizeof name - 1;
            memcpy(name, s.p + i + 2, nl);
            name[nl] = 0;
            v = getenv(name);
            if (v && *v)
                pt_buf_putz(&b, v);
            else if (colon < e)
                pt_buf_put(&b, s.p + colon + 2, e - colon - 2);
        }
        i = e + 1;
    }
    out = b.oom ? NULL : pt_arena_cstr(a, pt_buf_str(&b));
    pt_buf_free(&b);
    return out;
}

static const char *str_of(cli_mcp *m, const pt_json *o, const char *key, pt_arena *tmp)
{
    const pt_json *v = pt_json_get(o, key);
    return pt_json_is(v, PT_J_STR) ? expand(&m->ar, pt_json_str(v, tmp)) : NULL;
}

static uint32_t num_of(const pt_json *o, const char *key)
{
    int64_t v = pt_json_geti(o, key, 0);
    return v > 0 && v < 86400 ? (uint32_t)v : 0;
}

/* One server's config; false (with a warning) skips it. */
static bool parse_server(cli_mcp *m, const pt_json *e, const char *where, const char *dir, pt_rt_mcp_server *s,
                         pt_arena *tmp)
{
    pt_arena *a = &m->ar;
    const char *name = pt_arena_cstr(a, e->key), *type;
    const pt_json *args = pt_json_get(e, "args"), *env = pt_json_get(e, "env"), *hd = pt_json_get(e, "headers");
    const pt_json *oauth = pt_json_get(e, "oauth"), *v;
    memset(s, 0, sizeof *s);
    if (!mcp_server_name_ok(e->key)) {
        warnf(m, where, name, "names are 1 to 32 letters, digits, - and _");
        return false;
    }
    if (!pt_json_is(e, PT_J_OBJ)) {
        warnf(m, where, name, "not an object");
        return false;
    }
    if (pt_json_truthy(pt_json_get(e, "disabled")))
        return false;
    s->name = name;
    type = str_of(m, e, "type", tmp);
    s->url = str_of(m, e, "url", tmp);
    if (type && !strcmp(type, "sse")) {
        warnf(m, where, name, "the old HTTP+SSE transport is not supported: use the server's Streamable HTTP URL");
        return false;
    }
    if (type && strcmp(type, "stdio") && strcmp(type, "http") && strcmp(type, "streamable-http")) {
        warnf(m, where, name, "type is stdio or http");
        return false;
    }
    if (s->url && (!type || strcmp(type, "stdio"))) {
        if (strncmp(s->url, "https://", 8) && strncmp(s->url, "http://", 7)) {
            warnf(m, where, name, "url is not http(s)");
            return false;
        }
        if (hd && pt_json_is(hd, PT_J_OBJ)) {
            pt_header *h = (pt_header *)pt_arena_zalloc(a, sizeof *h * (hd->len ? hd->len : 1));
            for (v = h ? hd->child : NULL; v; v = v->next)
                if (pt_json_is(v, PT_J_STR)) {
                    h[s->nheaders].name = pt_arena_cstr(a, v->key);
                    h[s->nheaders++].value = expand(a, pt_json_str(v, tmp));
                }
            s->headers = h;
        }
    } else {
        s->url = NULL;
        s->command = str_of(m, e, "command", tmp);
        if (!s->command || !s->command[0]) {
            warnf(m, where, name, "needs a command or a url");
            return false;
        }
        if (pt_json_is(args, PT_J_ARR) && args->len) {
            const char **av = (const char **)pt_arena_zalloc(a, sizeof *av * args->len);
            for (v = av ? args->child : NULL; v; v = v->next)
                if (pt_json_is(v, PT_J_STR))
                    av[s->nargs++] = expand(a, pt_json_str(v, tmp));
            s->args = av;
        }
        if (pt_json_is(env, PT_J_OBJ) && env->len) {
            const char **ev = (const char **)pt_arena_zalloc(a, sizeof *ev * env->len);
            for (v = ev ? env->child : NULL; v; v = v->next) {
                pt_buf b;
                if (!pt_json_is(v, PT_J_STR) && v->type != PT_J_NUM)
                    continue;
                pt_buf_init(&b, pt_sys_allocator());
                pt_buf_puts(&b, v->key);
                pt_buf_putc(&b, '=');
                pt_buf_putz(&b, v->type == PT_J_NUM ? pt_arena_cstr(a, v->raw) : expand(a, pt_json_str(v, tmp)));
                ev[s->nenv++] = pt_arena_cstr(a, pt_buf_str(&b));
                pt_buf_free(&b);
            }
            s->env = ev;
        }
        s->cwd = str_of(m, e, "cwd", tmp);
        if (s->cwd && s->cwd[0] != '/' && dir) { /* relative to the project */
            pt_buf b;
            pt_buf_init(&b, pt_sys_allocator());
            pt_buf_putz(&b, dir);
            pt_buf_putc(&b, '/');
            pt_buf_putz(&b, s->cwd);
            s->cwd = pt_arena_cstr(a, pt_buf_str(&b));
            pt_buf_free(&b);
        } else if (!s->cwd && dir) {
            s->cwd = pt_arena_cstr(a, pt_strz(dir));
        }
    }
    s->client_id = str_of(m, e, "client_id", tmp);
    s->client_secret = str_of(m, e, "client_secret", tmp);
    s->scope = str_of(m, e, "scope", tmp);
    if (oauth) {
        if (!s->client_id)
            s->client_id = str_of(m, oauth, "client_id", tmp);
        if (!s->client_secret)
            s->client_secret = str_of(m, oauth, "client_secret", tmp);
        if (!s->scope)
            s->scope = str_of(m, oauth, "scope", tmp);
    }
    s->timeout_s = num_of(e, "timeout");
    s->startup_s = num_of(e, "startup_timeout");
    s->trust_hints = pt_json_truthy(pt_json_get(e, "trust_hints"));
    return !a->oom;
}

static bool add(cli_mcp *m, const pt_rt_mcp_server *s, const char *source, bool runs)
{
    uint32_t i;
    for (i = 0; i < m->nall; i++)
        if (!strcmp(m->all[i].name, s->name)) {
            if (!runs) /* a project's waiting for trust: the user's stays */
                break;
            m->all[i] = *s;
            m->info[i].source = source;
            m->info[i].runs = true;
            return true;
        }
    if (m->nall == CLI_MCP_MAX)
        return false;
    m->all[m->nall] = *s;
    m->info[m->nall].source = source;
    m->info[m->nall++].runs = runs;
    return true;
}

/* The "mcpServers" of a JSON file's text; raw gets its text (for the hash). */
static const pt_json *servers_of(cli_mcp *m, const char *path, pt_arena *tmp, char **text)
{
    size_t n;
    const pt_json *j, *sv;
    pt_json_err e;
    *text = pt_sys_read_file(path, &n);
    if (!*text)
        return NULL;
    j = pt_json_parse((pt_str){*text, n}, tmp, &e);
    if (!pt_json_is(j, PT_J_OBJ)) {
        warnf(m, path, NULL, j ? "not a JSON object" : e.msg);
        return NULL;
    }
    sv = pt_json_get(j, "mcpServers");
    if (sv && !pt_json_is(sv, PT_J_OBJ)) {
        warnf(m, path, NULL, "mcpServers is not an object");
        return NULL;
    }
    return sv;
}

void cli_trust_path(char *out, size_t cap)
{
    char dir[4000];
    cli_config_dir(dir, sizeof dir);
    snprintf(out, cap, "%s/trust.json", dir);
}

void cli_mcp_auth_path(char *out, size_t cap)
{
    char dir[4000];
    cli_config_dir(dir, sizeof dir);
    snprintf(out, cap, "%s/mcp-auth.json", dir);
}

/* The member key of a JSON file (an object of objects or strings), raw. */
static bool file_get(const char *path, const char *key, pt_buf *out)
{
    size_t n;
    char *text = pt_sys_read_file(path, &n);
    pt_arena ar;
    const pt_json *j, *v;
    bool ok = false;
    if (!text)
        return false;
    pt_arena_init(&ar, pt_sys_allocator(), 0);
    j = pt_json_parse((pt_str){text, n}, &ar, NULL);
    if ((v = pt_json_get(j, key))) {
        if (pt_json_is(v, PT_J_STR))
            pt_buf_puts(out, pt_json_str(v, &ar));
        else
            pt_buf_puts(out, v->raw);
        ok = !out->oom;
    }
    pt_arena_free(&ar);
    free(text);
    return ok;
}

/* Set member key (raw JSON; empty: remove it), keeping the others. */
static bool file_put(const char *path, const char *key, pt_str raw, char *err, size_t cap)
{
    size_t n = 0;
    char *text = pt_sys_read_file(path, &n), dir[4096], *slash;
    pt_arena ar;
    const pt_json *j = NULL, *v;
    pt_buf b;
    bool ok, first = true;
    pt_arena_init(&ar, pt_sys_allocator(), 0);
    if (text) {
        j = pt_json_parse((pt_str){text, n}, &ar, NULL);
        if (!pt_json_is(j, PT_J_OBJ)) {
            snprintf(err, cap, "%s is not a JSON object; not changed", path);
            pt_arena_free(&ar);
            free(text);
            return false;
        }
    }
    pt_buf_init(&b, pt_sys_allocator());
    pt_buf_putc(&b, '{');
    for (v = j ? j->child : NULL; v; v = v->next) {
        if (pt_str_eqz(v->key, key))
            continue;
        pt_buf_putz(&b, first ? "\n  " : ",\n  ");
        first = false;
        pt_json_quote(&b, v->key);
        pt_buf_putz(&b, ": ");
        pt_buf_puts(&b, v->raw);
    }
    if (raw.n) {
        pt_buf_putz(&b, first ? "\n  " : ",\n  ");
        first = false;
        pt_json_quote(&b, pt_strz(key));
        pt_buf_putz(&b, ": ");
        pt_buf_puts(&b, raw);
    }
    pt_buf_putz(&b, first ? "}\n" : "\n}\n");
    snprintf(dir, sizeof dir, "%s", path);
    if ((slash = strrchr(dir, '/'))) {
        char *p;
        *slash = 0;
        for (p = dir + 1; *p; p++) /* mkdir -p */
            if (*p == '/') {
                *p = 0;
                mkdir(dir, 0700);
                *p = '/';
            }
        mkdir(dir, 0700);
    }
    ok = !b.oom && cli_write_atomic(path, pt_buf_str(&b), err, cap);
    pt_buf_free(&b);
    pt_arena_free(&ar);
    free(text);
    return ok;
}

void cli_mcp_load(cli_mcp *m, const char *cwd)
{
    char user[4096], trust[4096], proj1[4200], proj2[4200];
    char *t_user = NULL, *t1 = NULL, *t2 = NULL;
    pt_arena tmp;
    const pt_json *su, *s1, *s2, *e;
    pt_rt_mcp_server s;
    uint32_t i;
    pt_buf have;
    memset(m, 0, sizeof *m);
    pt_arena_init(&m->ar, pt_sys_allocator(), 4096);
    pt_arena_init(&tmp, pt_sys_allocator(), 0);
    snprintf(m->project, sizeof m->project, "%s", cwd ? cwd : ".");
    {
        char dir[4000];
        cli_config_dir(dir, sizeof dir);
        snprintf(user, sizeof user, "%s/config.json", dir);
    }
    snprintf(proj1, sizeof proj1, "%s/.mcp.json", m->project);
    snprintf(proj2, sizeof proj2, "%s/.pytte/config.json", m->project);
    su = servers_of(m, user, &tmp, &t_user);
    for (e = su ? su->child : NULL; e; e = e->next)
        if (parse_server(m, e, user, NULL, &s, &tmp) && !add(m, &s, "user", true))
            warnf(m, user, NULL, "too many MCP servers");
    /* the project's: their hash, then whether it is trusted */
    s1 = servers_of(m, proj1, &tmp, &t1);
    s2 = servers_of(m, proj2, &tmp, &t2);
    if ((s1 && s1->len) || (s2 && s2->len)) {
        pt_sha256 sh;
        unsigned char d[32];
        pt_sha256_init(&sh);
        pt_sha256_update(&sh, m->project, strlen(m->project) + 1);
        if (s1)
            pt_sha256_update(&sh, s1->raw.p, s1->raw.n);
        pt_sha256_update(&sh, "", 1);
        if (s2)
            pt_sha256_update(&sh, s2->raw.p, s2->raw.n);
        pt_sha256_final(&sh, d);
        pt_hex(m->hash, d, 32);
        m->hash[64] = 0;
        cli_trust_path(trust, sizeof trust);
        pt_buf_init(&have, pt_sys_allocator());
        m->trusted = file_get(trust, m->project, &have) && pt_str_eqz(pt_buf_str(&have), m->hash);
        pt_buf_free(&have);
        for (e = s1 ? s1->child : NULL; e; e = e->next)
            if (parse_server(m, e, proj1, m->project, &s, &tmp) && !add(m, &s, ".mcp.json", m->trusted))
                warnf(m, proj1, NULL, "too many MCP servers");
        for (e = s2 ? s2->child : NULL; e; e = e->next)
            if (parse_server(m, e, proj2, m->project, &s, &tmp) && !add(m, &s, ".pytte/config.json", m->trusted))
                warnf(m, proj2, NULL, "too many MCP servers");
    }
    for (i = 0; i < m->nall; i++) {
        if (m->info[i].runs)
            m->run[m->n++] = m->all[i];
        else
            m->nuntrusted++;
    }
    pt_arena_free(&tmp);
    free(t_user);
    free(t1);
    free(t2);
}

bool cli_mcp_trust(const cli_mcp *m, bool on, char *err, size_t cap)
{
    char path[4096];
    pt_buf v;
    bool ok;
    if (!m->hash[0]) {
        snprintf(err, cap, "this directory has no MCP servers to trust");
        return false;
    }
    cli_trust_path(path, sizeof path);
    pt_buf_init(&v, pt_sys_allocator());
    if (on)
        pt_json_quote(&v, pt_strz(m->hash));
    ok = file_put(path, m->project, pt_buf_str(&v), err, cap);
    pt_buf_free(&v);
    return ok;
}

void cli_mcp_free(cli_mcp *m)
{
    pt_arena_free(&m->ar);
}

/* ---- the host's side ---- */

static bool auth_load(void *ud, const char *server, const char *url, pt_buf *out)
{
    char path[4096];
    (void)ud;
    (void)server;
    cli_mcp_auth_path(path, sizeof path);
    return file_get(path, url, out);
}

static void auth_save(void *ud, const char *server, const char *url, pt_str json)
{
    char path[4096], err[300];
    (void)ud;
    (void)server;
    cli_mcp_auth_path(path, sizeof path);
    file_put(path, url, json, err, sizeof err);
}

/* xdg-open, or open on macOS; detached, quiet. */
static void open_url(void *ud, const char *url)
{
    pid_t pid;
    (void)ud;
    if (!url || (strncmp(url, "https://", 8) && strncmp(url, "http://", 7)))
        return; /* only web pages */
    pid = fork();
    if (pid == 0) {
        int fd = open("/dev/null", O_RDWR);
        setsid();
        if (fd >= 0) {
            dup2(fd, 0);
            dup2(fd, 1);
            dup2(fd, 2);
        }
        if (fork() == 0) {
#ifdef __APPLE__
            execlp("open", "open", url, (char *)NULL);
#else
            execlp("xdg-open", "xdg-open", url, (char *)NULL);
#endif
            _exit(127);
        }
        _exit(0);
    }
    if (pid > 0)
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
            ;
}

void cli_mcp_host(pt_rt_mcp_host *h, void (*notify)(void *ud, const char *server, const char *msg), void *ud)
{
    memset(h, 0, sizeof *h);
    h->ud = ud;
    h->notify = notify;
    h->open_url = open_url;
    h->auth_load = auth_load;
    h->auth_save = auth_save;
}
