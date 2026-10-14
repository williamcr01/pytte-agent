/* MCP servers end to end: this program is also the server (run with
 * --mcp-server MODE, over stdio). Sessions start it, a faux model calls
 * its tools, and the results are what the session recorded. */
#define _XOPEN_SOURCE 700
#include "t.h"

#include <arpa/inet.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#include <sys/stat.h>

#include "cli/cli.h"
#include "rt/mcp.h"
#include "rt/rt.h"

/* ---- the server ---- */

static void put(pt_buf *b)
{
    fwrite(b->p, 1, b->n, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    pt_buf_reset(b);
}

static void result(pt_buf *b, pt_str id, const char *json)
{
    pt_buf_putz(b, "{\"jsonrpc\":\"2.0\",\"id\":");
    pt_buf_puts(b, id);
    pt_buf_putz(b, ",\"result\":");
    pt_buf_putz(b, json);
    pt_buf_putc(b, '}');
    put(b);
}

static void text_result(pt_buf *b, pt_str id, const char *pre, pt_str text, bool error)
{
    pt_buf t;
    pt_buf_init(&t, pt_sys_allocator());
    pt_buf_putz(&t, pre);
    pt_buf_puts(&t, text);
    pt_buf_putz(b, "{\"jsonrpc\":\"2.0\",\"id\":");
    pt_buf_puts(b, id);
    pt_buf_putz(b, ",\"result\":{\"content\":[{\"type\":\"text\",\"text\":");
    pt_json_quote(b, pt_buf_str(&t));
    pt_buf_putz(b, error ? "}],\"isError\":true}}" : "}]}}");
    put(b);
    pt_buf_free(&t);
}

static int server(const char *mode)
{
    static char line[1 << 20];
    char roots[512] = "none";
    int cancelled = 0;
    bool extra = false;
    pt_buf b;
    if (!strcmp(mode, "crash_init")) {
        fprintf(stderr, "cannot start: no config\n");
        _exit(2);
    }
    pt_buf_init(&b, pt_sys_allocator());
    while (fgets(line, sizeof line, stdin)) {
        pt_arena ar;
        const pt_json *j, *params, *args;
        pt_str method, id, name;
        pt_arena_init(&ar, pt_sys_allocator(), 0);
        j = pt_json_parse(pt_str_trim(pt_strz(line)), &ar, NULL);
        method = pt_json_gets(j, "method", &ar);
        id = pt_json_get(j, "id") ? pt_json_get(j, "id")->raw : PT_STR_NULL;
        params = pt_json_get(j, "params");
        args = pt_json_get(params, "arguments");
        name = pt_json_gets(params, "name", &ar);
        if (!method.n) { /* an answer to our request */
            const pt_json *r = pt_json_get(pt_json_get(j, "result"), "roots");
            if (r && r->child)
                snprintf(roots, sizeof roots, "%.*s", (int)pt_json_gets(r->child, "uri", &ar).n,
                         pt_json_gets(r->child, "uri", &ar).p);
        } else if (pt_str_eqz(method, "initialize")) {
            if (strcmp(mode, "hang") != 0)
                result(&b, id,
                       "{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{\"tools\":{\"listChanged\":true},"
                       "\"prompts\":{},\"resources\":{}},\"serverInfo\":{\"name\":\"fake\",\"version\":\"1\"}}");
        } else if (pt_str_eqz(method, "notifications/initialized")) {
            fputs("{\"jsonrpc\":\"2.0\",\"id\":\"p1\",\"method\":\"ping\"}\n", stdout);
            fputs("{\"jsonrpc\":\"2.0\",\"id\":\"r1\",\"method\":\"roots/list\"}\n", stdout);
            fputs("not json: a stray line\n", stdout);
            fflush(stdout);
        } else if (pt_str_eqz(method, "notifications/cancelled")) {
            cancelled++;
        } else if (pt_str_eqz(method, "tools/list")) {
            if (!strcmp(mode, "list_error")) {
                pt_buf_putz(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
                pt_buf_puts(&b, id);
                pt_buf_putz(&b, ",\"error\":{\"code\":-32000,\"message\":\"rate limited\"}}");
                put(&b);
            } else if (!strcmp(mode, "fill"))
                result(&b, id,
                       "{\"tools\":[{\"name\":\"show\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"q\":{"
                       "\"type\":\"string\"},\"model_name\":{\"type\":\"string\"},\"session_id\":{\"type\":\"string\"}},"
                       "\"required\":[\"q\",\"session_id\"]}},{\"name\":\"plain\",\"inputSchema\":{\"type\":\"object\","
                       "\"properties\":{\"q\":{\"type\":\"string\"}}}}]}");
            else if (!pt_json_gets(params, "cursor", &ar).n)
                result(&b, id,
                       "{\"tools\":[{\"name\":\"echo\",\"description\":\"Says it back.\",\"inputSchema\":{\"type\":"
                       "\"object\",\"properties\":{\"text\":{\"type\":\"string\"}}},\"annotations\":{\"readOnlyHint\":"
                       "true}},{\"name\":\"slow\",\"inputSchema\":{\"type\":\"object\"}}],\"nextCursor\":\"2\"}");
            else if (extra)
                result(&b, id, "{\"tools\":[{\"name\":\"img\"},{\"name\":\"fail\"},{\"name\":\"crash\"},{\"name\":"
                               "\"stats\"},{\"name\":\"changes\"},{\"name\":\"extra\"}]}");
            else
                result(&b, id, "{\"tools\":[{\"name\":\"img\"},{\"name\":\"fail\"},{\"name\":\"crash\"},{\"name\":"
                               "\"stats\"},{\"name\":\"changes\"}]}");
        } else if (pt_str_eqz(method, "tools/call")) {
            if (!strcmp(mode, "fill")) {
                text_result(&b, id, "got: ", args ? args->raw : PT_STR_NULL, false);
            } else if (pt_str_eqz(name, "echo")) {
                text_result(&b, id, "echo: ", pt_json_gets(args, "text", &ar), false);
            } else if (pt_str_eqz(name, "slow")) {
                pt_str tok = pt_json_get(pt_json_get(params, "_meta"), "progressToken")->raw;
                printf("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/progress\",\"params\":{\"progressToken\":%.*s,"
                       "\"progress\":1,\"total\":2,\"message\":\"halfway\"}}\n",
                       (int)tok.n, tok.p);
                fflush(stdout);
            } else if (pt_str_eqz(name, "img")) {
                result(&b, id,
                       "{\"content\":[{\"type\":\"text\",\"text\":\"a picture\"},{\"type\":\"image\",\"data\":"
                       "\"iVBORw0KGgoAAAANSUhEUgAAAAIAAAADCAYAAAA=\",\"mimeType\":\"image/png\"}]}");
            } else if (pt_str_eqz(name, "fail")) {
                text_result(&b, id, "bad input", PT_STR_NULL, true);
            } else if (pt_str_eqz(name, "crash")) {
                fprintf(stderr, "boom\n");
                _exit(3);
            } else if (pt_str_eqz(name, "stats")) {
                char s[700];
                snprintf(s, sizeof s, "cancelled=%d roots=%s", cancelled, roots);
                text_result(&b, id, s, PT_STR_NULL, false);
            } else if (pt_str_eqz(name, "changes")) {
                extra = true;
                fputs("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/tools/list_changed\"}\n", stdout);
                text_result(&b, id, "changed", PT_STR_NULL, false);
            } else {
                pt_buf_putz(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
                pt_buf_puts(&b, id);
                pt_buf_putz(&b, ",\"error\":{\"code\":-32602,\"message\":\"Unknown tool\"}}");
                put(&b);
            }
        } else if (pt_str_eqz(method, "prompts/list")) {
            result(&b, id,
                   "{\"prompts\":[{\"name\":\"review\",\"description\":\"Review a file\",\"arguments\":[{\"name\":"
                   "\"file\",\"description\":\"which\",\"required\":true},{\"name\":\"focus\"}]}]}");
        } else if (pt_str_eqz(method, "prompts/get")) {
            if (!pt_str_eqz(name, "review")) {
                pt_buf_putz(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
                pt_buf_puts(&b, id);
                pt_buf_putz(&b, ",\"error\":{\"code\":-32602,\"message\":\"no such prompt\"}}");
                put(&b);
            } else {
                pt_buf t;
                pt_buf_init(&t, pt_sys_allocator());
                pt_buf_putz(&t, "Please review ");
                pt_buf_puts(&t, pt_json_gets(args, "file", &ar));
                pt_buf_putz(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
                pt_buf_puts(&b, id);
                pt_buf_putz(&b, ",\"result\":{\"messages\":[{\"role\":\"user\",\"content\":{\"type\":\"text\",\"text\":");
                pt_json_quote(&b, pt_buf_str(&t));
                pt_buf_putz(&b, "}}]}}");
                put(&b);
                pt_buf_free(&t);
            }
        } else if (pt_str_eqz(method, "resources/list")) {
            result(&b, id, "{\"resources\":[{\"uri\":\"note://one\",\"name\":\"one\",\"mimeType\":\"text/plain\"}]}");
        } else if (pt_str_eqz(method, "resources/read")) {
            pt_buf t;
            pt_buf_init(&t, pt_sys_allocator());
            pt_buf_putz(&t, "contents of ");
            pt_buf_puts(&t, pt_json_gets(params, "uri", &ar));
            pt_buf_putz(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
            pt_buf_puts(&b, id);
            pt_buf_putz(&b, ",\"result\":{\"contents\":[{\"uri\":\"note://one\",\"text\":");
            pt_json_quote(&b, pt_buf_str(&t));
            pt_buf_putz(&b, "}]}}");
            put(&b);
            pt_buf_free(&t);
        } else if (id.n) {
            pt_buf_putz(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
            pt_buf_puts(&b, id);
            pt_buf_putz(&b, ",\"error\":{\"code\":-32601,\"message\":\"Method not found\"}}");
            put(&b);
        }
        pt_arena_free(&ar);
    }
    _exit(0);
}

/* ---- sessions ---- */

static char self[PATH_MAX];
static char notes[8192];

static void on_note(void *ud, const char *server, const char *msg)
{
    size_t n = strlen(notes);
    (void)ud;
    snprintf(notes + n, sizeof notes - n, "[%s] %s\n", server ? server : "-", msg);
}

/* OAuth state the host keeps, and the browser */
static char saved[4096];
static int browser_fd = -1;
static char auth_url[2048];

static bool on_load(void *ud, const char *server, const char *url, pt_buf *out)
{
    (void)ud;
    (void)server;
    (void)url;
    if (!saved[0])
        return false;
    pt_buf_putz(out, saved);
    return true;
}

static void on_save(void *ud, const char *server, const char *url, pt_str json)
{
    (void)ud;
    (void)server;
    (void)url;
    snprintf(saved, sizeof saved, "%.*s", (int)json.n, json.p ? json.p : "");
}

static pt_str query(const char *url, const char *key)
{
    const char *q = strchr(url, '?'), *p;
    size_t kl = strlen(key);
    for (p = q; p && *p; p++)
        if ((*p == '?' || *p == '&') && !strncmp(p + 1, key, kl) && p[1 + kl] == '=') {
            const char *v = p + 2 + kl, *e = strchr(v, '&');
            return (pt_str){v, e ? (size_t)(e - v) : strlen(v)};
        }
    return PT_STR_NULL;
}

/* The browser: the user logs in, and it goes to the redirect URI. */
static void on_open(void *ud, const char *url)
{
    pt_str redirect = query(url, "redirect_uri"), state = query(url, "state");
    struct sockaddr_in sa;
    char req[512];
    int port;
    (void)ud;
    snprintf(auth_url, sizeof auth_url, "%s", url);
    /* http%3A%2F%2F127.0.0.1%3A<port>%2Fcallback */
    if (redirect.n < 30 || strncmp(redirect.p, "http%3A%2F%2F127.0.0.1%3A", 25))
        return;
    port = atoi(redirect.p + 25);
    browser_fd = socket(AF_INET, SOCK_STREAM, 0);
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons((uint16_t)port);
    if (connect(browser_fd, (struct sockaddr *)&sa, sizeof sa) < 0)
        return;
    snprintf(req, sizeof req, "GET /callback?code=thecode&state=%.*s HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n",
             (int)state.n, state.p);
    if (write(browser_fd, req, strlen(req)) < 0)
        return;
}

/* Wraps the faux model's driver, for remote servers' tests. */
static pt_http_driver *(*wrap)(pt_http_driver *faux);

typedef struct sess {
    pt_store *st;
    pt_http_driver *http;
    pt_agent *a;
    pt_rt *rt;
    bool asked;
} sess;

static void srv_cfg(pt_rt_mcp_server *c, const char *name, const char *mode)
{
    static const char *args[2][2];
    static int k;
    const char **a = args[k++ % 2];
    memset(c, 0, sizeof *c);
    a[0] = "--mcp-server";
    a[1] = mode;
    c->name = name;
    c->command = self;
    c->args = a;
    c->nargs = 2;
}

/* A tool the session's epoch offers besides the runtime's (as if from a
 * run before, its server not started yet); then not provisional. */
static const pt_tool_def *old_tool;

static void open_sess(sess *s, const char *script, const pt_rt_mcp_server *srv, uint32_t n, pt_perm_mode perm)
{
    pt_tool_def defs[32];
    pt_opts o;
    pt_rt_cfg rc;
    pt_session_cfg c;
    pt_identity me;
    pt_rt_mcp_host host;
    memset(s, 0, sizeof *s);
    notes[0] = 0;
    s->st = pt_store_memory_new();
    s->http = pt_http_faux_new(pt_strz(script));
    if (wrap)
        s->http = wrap(s->http);
    memset(&o, 0, sizeof o);
    o.alloc = T_AL;
    o.config.model = PT_S("faux/faux");
    o.blob = pt_rt_read_blob;
    o.blob_ud = s->st;
    s->a = pt_new(&o);
    memset(&rc, 0, sizeof rc);
    rc.store = s->st;
    rc.http = s->http;
    rc.session = "sid-1";
    s->rt = pt_rt_new(&rc, s->a);
    CHECK(pt_rt_add_builtins(s->rt));
    pt_rt_set_perm(s->rt, perm);
    memset(&host, 0, sizeof host);
    host.notify = on_note;
    host.auth_load = on_load;
    host.auth_save = on_save;
    host.open_url = on_open;
    CHECK(pt_rt_mcp_setup(s->rt, &host, srv, n));
    memset(&c, 0, sizeof c);
    c.cwd = PT_S("/tmp");
    c.host = PT_S("test");
    c.system = PT_S("Test.");
    c.tools = pt_rt_tool_defs(s->rt, &c.ntools);
    c.provisional = n > 0 && !old_tool;
    if (old_tool && c.ntools < 32) {
        memcpy(defs, c.tools, sizeof *defs * c.ntools);
        defs[c.ntools++] = *old_tool;
        c.tools = defs;
    }
    CHECK(pt_create(s->a, &c));
    memset(&me, 0, sizeof me);
    me.host = PT_S("test");
    CHECK(pt_open(s->a, &me, false));
}

static void say(sess *s, const char *text)
{
    pt_input in;
    pt_rt_result r;
    memset(&in, 0, sizeof in);
    in.text = pt_strz(text);
    CHECK(pt_submit(s->a, &in));
    while ((r = pt_rt_run(s->rt)) == PT_RT_ASK) {
        pt_ask_view v;
        s->asked = true;
        CHECK(pt_asks(s->a, &v, 1) == 1);
        CHECK(pt_approve(s->a, v.task, true, PT_STR_NULL));
    }
    CHECK(r == PT_RT_IDLE);
}

/* Step the runtime (an idle agent: servers only) until *flag or ms pass. */
static void wait_for(sess *s, const bool *flag, int ms)
{
    int i;
    for (i = 0; i < ms / 20 && !*flag; i++)
        pt_rt_step(s->rt, 20, NULL, 0);
}

static void close_sess(sess *s)
{
    pt_rt_close(s->rt);
    pt_rt_free(s->rt);
    pt_free(s->a);
    s->http->free(s->http);
    pt_store_memory_free(s->st);
}

static bool has(pt_str hay, const char *needle)
{
    size_t n = strlen(needle), i;
    for (i = 0; i + n <= hay.n; i++)
        if (!memcmp(hay.p + i, needle, n))
            return true;
    return false;
}

/* The tool result containing needle; its error flag and image. */
static bool tool_result(sess *s, const char *needle, bool *error, bool *image)
{
    uint32_t i;
    for (i = 0; i < pt_entry_count(s->a); i++) {
        pt_entry_view v;
        if (pt_entry_at(s->a, i, &v) && !strcmp(v.kind, "tool_result") && has(v.msg->text, needle)) {
            if (error)
                *error = v.msg->is_error;
            if (image)
                *image = v.msg->image.n > 0;
            return true;
        }
    }
    return false;
}

static pt_str epoch_raw(sess *s)
{
    pt_entry_view v;
    return pt_entry_at(s->a, 0, &v) ? v.raw : PT_STR_NULL;
}

static pt_rt_mcp_info info(sess *s, uint32_t k)
{
    pt_rt_mcp_info in[4];
    memset(in, 0, sizeof in);
    pt_rt_mcp_servers(s->rt, in, 4);
    return in[k];
}

#define CALL(name, args) "{\"type\":\"tool_call\",\"name\":\"" name "\",\"args\":" args "}"
#define TURN(...) "{\"content\":[" __VA_ARGS__ "]}"
#define DONE TURN("{\"type\":\"text\",\"text\":\"done\"}")

static void test_tools(void)
{
    sess s;
    pt_rt_mcp_server c;
    bool err = false, img = false;
    uint32_t i, nd;
    const pt_tool_def *defs;
    srv_cfg(&c, "t", "ok");
    c.trust_hints = true;
    open_sess(&s,
              "{\"turns\":[" TURN(CALL("mcp__t__echo", "{\"text\":\"hi\"}") "," CALL("mcp__t__img", "{}") "," CALL(
                  "mcp__t__fail", "{}") "," CALL("mcp__t__stats", "{}")) "," DONE "," TURN(CALL("mcp__t__changes",
                                                                                                   "{}")) "," DONE "]}",
              &c, 1, PT_PERM_AUTO);
    say(&s, "use them");
    /* the first request waited for the server: its tools were offered */
    CHECK(has(epoch_raw(&s), "mcp__t__echo") && has(epoch_raw(&s), "mcp__t__stats"));
    CHECK(has(epoch_raw(&s), "Says it back."));
    CHECK(!has(epoch_raw(&s), "\"provisional\""));
    CHECK(tool_result(&s, "echo: hi", &err, NULL) && !err);
    CHECK(tool_result(&s, "a picture", &err, &img) && !err && img);
    CHECK(tool_result(&s, "bad input", &err, NULL) && err);
    CHECK(tool_result(&s, "cancelled=0 roots=file:///tmp", &err, NULL) && !err);
    CHECK(has(pt_strz(notes), "[t] ready: 7 tools"));
    CHECK(info(&s, 0).state == PT_MCP_READY && info(&s, 0).ntools == 7);
    CHECK(info(&s, 0).nprompts == 1 && info(&s, 0).nresources == 1);
    CHECK(!strcmp(info(&s, 0).transport, "stdio"));
    /* its hint made echo read-only; the others are not */
    defs = pt_rt_tool_defs(s.rt, &nd);
    for (i = 0; i < nd; i++) {
        if (pt_str_eqz(defs[i].name, "mcp__t__echo"))
            CHECK(defs[i].read_only && pt_str_eqz(defs[i].source, "mcp:t"));
        if (pt_str_eqz(defs[i].name, "mcp__t__stats"))
            CHECK(!defs[i].read_only);
    }
    /* the list changed: listed again after the call */
    CHECK(!pt_rt_tool_has(s.rt, PT_S("mcp__t__extra")));
    say(&s, "change it");
    CHECK(tool_result(&s, "changed", &err, NULL) && !err);
    {
        int k;
        for (k = 0; k < 100 && !pt_rt_tool_has(s.rt, PT_S("mcp__t__extra")); k++)
            pt_rt_step(s.rt, 20, NULL, 0);
    }
    CHECK(pt_rt_tool_has(s.rt, PT_S("mcp__t__extra")));
    CHECK(info(&s, 0).ntools == 8);
    close_sess(&s);
}

/* Arguments the config fills in: hidden from the model, sent instead of its own. */
static void test_fill(void)
{
    sess s;
    pt_rt_mcp_server c;
    bool err = true;
    pt_str ep;
    srv_cfg(&c, "f", "fill");
    c.arguments = "{\"*\":{\"model_name\":\"{model_id}\",\"session_id\":\"s-{session}\"},"
                  "\"plain\":{\"fixed\":3,\"who\":\"{model} {nope}\"}}";
    open_sess(&s,
              "{\"turns\":[" TURN(CALL("mcp__f__show", "{\"q\":\"x\",\"model_name\":\"liar\"}") "," CALL(
                  "mcp__f__plain", "{\"q\":\"y\"}")) "," DONE "]}",
              &c, 1, PT_PERM_AUTO);
    say(&s, "go");
    ep = epoch_raw(&s);
    CHECK(has(ep, "mcp__f__show") && has(ep, "\"properties\":{\"q\":{\"type\":\"string\"}},\"required\":[\"q\"]"));
    CHECK(!has(ep, "model_name") && !has(ep, "session_id"));
    CHECK(tool_result(&s, "got: {\"q\":\"x\",\"model_name\":\"faux\",\"session_id\":\"s-sid-1\"}", &err, NULL) && !err);
    /* "*" only where the schema has it; the tool's own entry always */
    CHECK(tool_result(&s, "got: {\"q\":\"y\",\"fixed\":3,\"who\":\"faux/faux {nope}\"}", &err, NULL) && !err);
    close_sess(&s);
}

static void test_ask(void)
{
    sess s;
    pt_rt_mcp_server c;
    bool err = true;
    srv_cfg(&c, "t", "ok");
    open_sess(&s, "{\"turns\":[" TURN(CALL("mcp__t__echo", "{\"text\":\"x\"}")) "," DONE "]}", &c, 1, PT_PERM_ASK);
    say(&s, "go");
    CHECK(s.asked); /* no trust in hints: even echo asks */
    CHECK(tool_result(&s, "echo: x", &err, NULL) && !err);
    close_sess(&s);

    open_sess(&s, "{\"turns\":[" TURN(CALL("mcp__t__echo", "{\"text\":\"x\"}")) "," DONE "]}", &c, 1,
              PT_PERM_READ_ONLY);
    say(&s, "go");
    CHECK(tool_result(&s, "", &err, NULL) && err);
    CHECK(!tool_result(&s, "echo: x", NULL, NULL));
    close_sess(&s);
}

static void test_timeout(void)
{
    sess s;
    pt_rt_mcp_server c;
    bool err = false;
    srv_cfg(&c, "t", "ok");
    c.timeout_s = 1;
    open_sess(&s,
              "{\"turns\":[" TURN(CALL("mcp__t__slow", "{}")) "," DONE "," TURN(CALL("mcp__t__stats", "{}")) "," DONE
              "]}",
              &c, 1, PT_PERM_AUTO);
    say(&s, "slow");
    CHECK(tool_result(&s, "did not answer within 1 seconds", &err, NULL) && err);
    say(&s, "stats");
    CHECK(tool_result(&s, "cancelled=1", &err, NULL) && !err);
    close_sess(&s);
}

static void test_crash(void)
{
    sess s;
    pt_rt_mcp_server c;
    bool err = false;
    srv_cfg(&c, "t", "ok");
    open_sess(&s,
              "{\"turns\":[" TURN(CALL("mcp__t__crash", "{}")) "," DONE "," TURN(CALL("mcp__t__echo",
                                                                                     "{\"text\":\"again\"}")) "," DONE
              "]}",
              &c, 1, PT_PERM_AUTO);
    say(&s, "crash it");
    CHECK(tool_result(&s, "exited with status 3: boom", &err, NULL) && err);
    CHECK(info(&s, 0).state == PT_MCP_FAILED);
    CHECK(has(pt_strz(notes), "[t] stopped: exited with status 3: boom"));
    /* within the gap a call does not start it; a restart does */
    CHECK(pt_rt_mcp_restart(s.rt, PT_S("t")));
    CHECK(!pt_rt_mcp_restart(s.rt, PT_S("nope")));
    {
        int k;
        for (k = 0; k < 200 && info(&s, 0).state != PT_MCP_READY; k++)
            pt_rt_step(s.rt, 20, NULL, 0);
    }
    CHECK(info(&s, 0).state == PT_MCP_READY);
    say(&s, "again");
    CHECK(tool_result(&s, "echo: again", &err, NULL) && !err);
    close_sess(&s);
}

static void test_startup_failures(void)
{
    sess s;
    pt_rt_mcp_server c[2];
    bool err = false;
    srv_cfg(&c[0], "h", "hang");
    c[0].startup_s = 1;
    c[0].timeout_s = 1;
    srv_cfg(&c[1], "x", "crash_init");
    open_sess(&s, "{\"turns\":[" TURN(CALL("mcp__h__echo", "{}")) "," DONE "]}", c, 2, PT_PERM_AUTO);
    say(&s, "hello");
    /* the first request went on without them */
    CHECK(!has(epoch_raw(&s), "mcp__h__") && !has(epoch_raw(&s), "mcp__x__"));
    CHECK(has(pt_strz(notes), "[h] still starting after 1 s"));
    CHECK(has(pt_strz(notes), "[x] failed: exited with status 2: cannot start: no config"));
    /* the model was not offered it */
    CHECK(tool_result(&s, "Unknown tool: mcp__h__echo", &err, NULL) && err);
    CHECK(info(&s, 0).state == PT_MCP_STARTING && info(&s, 1).state == PT_MCP_FAILED);
    CHECK(!strcmp(info(&s, 1).error, "exited with status 2: cannot start: no config"));
    close_sess(&s);
}

/* A server that will not list its tools runs with none, and says why. */
static void test_list_error(void)
{
    sess s;
    pt_rt_mcp_server c;
    srv_cfg(&c, "e", "list_error");
    open_sess(&s, "{\"turns\":[" DONE "]}", &c, 1, PT_PERM_AUTO);
    say(&s, "hello");
    CHECK(!has(epoch_raw(&s), "mcp__e__"));
    CHECK(has(pt_strz(notes), "[e] listing tools: rate limited (MCP error -32000)"));
    CHECK(info(&s, 0).state == PT_MCP_READY);
    CHECK(!strcmp(info(&s, 0).error, "listing tools: rate limited (MCP error -32000)"));
    close_sess(&s);
}

/* A session from before offers the tool; its server hangs at start: the
 * call waits for it (asking first, as the server is unknown yet), then
 * times out. */
static void test_resumed(void)
{
    sess s;
    pt_rt_mcp_server c;
    pt_tool_def d;
    bool err = false;
    srv_cfg(&c, "h", "hang");
    c.timeout_s = 1;
    c.startup_s = 1;
    memset(&d, 0, sizeof d);
    d.name = PT_S("mcp__h__echo");
    d.description = PT_S("Says it back.");
    d.schema = PT_S("{\"type\":\"object\"}");
    old_tool = &d;
    open_sess(&s, "{\"turns\":[" TURN(CALL("mcp__h__echo", "{}")) "," DONE "]}", &c, 1, PT_PERM_ASK);
    old_tool = NULL;
    say(&s, "hello");
    CHECK(s.asked);
    CHECK(tool_result(&s, "did not answer within 1 seconds", &err, NULL) && err);
    close_sess(&s);
}

typedef struct got {
    bool done;
    char error[200];
    char text[500];
} got;

static void on_got(void *ud, const char *error, pt_str text)
{
    got *g = (got *)ud;
    g->done = true;
    snprintf(g->error, sizeof g->error, "%s", error ? error : "");
    snprintf(g->text, sizeof g->text, "%.*s", (int)text.n, text.p ? text.p : "");
}

static void test_prompts_resources(void)
{
    sess s;
    pt_rt_mcp_server c;
    const pt_rt_mcp_prompt *p;
    const pt_rt_mcp_resource *r;
    uint32_t n;
    got g;
    srv_cfg(&c, "docs", "ok");
    open_sess(&s, "{\"turns\":[" DONE "]}", &c, 1, PT_PERM_AUTO);
    say(&s, "hi");
    p = pt_rt_mcp_prompts(s.rt, &n);
    CHECK(n == 1);
    if (n == 1) {
        CHECK_STR(p[0].server, "docs");
        CHECK_STR(p[0].name, "review");
        CHECK_STR(p[0].description, "Review a file");
        CHECK(p[0].nargs == 2 && p[0].args[0].required && !p[0].args[1].required);
        CHECK_STR(p[0].args[0].name, "file");
    }
    r = pt_rt_mcp_resources(s.rt, &n);
    CHECK(n == 1);
    if (n == 1) {
        CHECK_STR(r[0].uri, "note://one");
        CHECK_STR(r[0].mime, "text/plain");
    }
    memset(&g, 0, sizeof g);
    CHECK(pt_rt_mcp_get_prompt(s.rt, PT_S("docs"), PT_S("review"), PT_S("{\"file\":\"a.c\"}"), on_got, &g));
    wait_for(&s, &g.done, 4000);
    CHECK(g.done && !g.error[0] && !strcmp(g.text, "Please review a.c"));
    memset(&g, 0, sizeof g);
    CHECK(pt_rt_mcp_get_prompt(s.rt, PT_S("docs"), PT_S("other"), PT_STR_NULL, on_got, &g));
    wait_for(&s, &g.done, 4000);
    CHECK(g.done && strstr(g.error, "no such prompt"));
    memset(&g, 0, sizeof g);
    CHECK(pt_rt_mcp_read(s.rt, PT_S("docs"), PT_S("note://one"), on_got, &g));
    wait_for(&s, &g.done, 4000);
    CHECK(g.done && !strcmp(g.text, "contents of note://one"));
    CHECK(!pt_rt_mcp_read(s.rt, PT_S("nope"), PT_S("note://one"), on_got, &g));
    close_sess(&s);
}

/* ---- a remote server: Streamable HTTP, behind OAuth ---- */

typedef struct rx {
    struct rx *next;
    pt_http_sink sink;
    int status;
    char headers[512];
    pt_buf body;
    size_t at;
    bool head_sent;
} rx;

static struct {
    pt_http_driver vt;
    pt_http_driver *faux;
    rx *list;
    const char *token;  /* the access token it takes */
    bool drop_once;     /* forget the session once */
    int sessions;       /* sessions started */
    bool bad_headers;   /* a request after initialize without them */
    char challenge[64]; /* PKCE, from the authorize URL */
    bool pkce_ok, resource_ok, registered;
    int refreshes;
} rs;

static const char *hdr(const pt_http_req *r, const char *name)
{
    uint32_t i;
    for (i = 0; i < r->nheaders; i++)
        if (pt_str_ieq(pt_strz(r->headers[i].name), pt_strz(name)))
            return r->headers[i].value;
    return NULL;
}

static bool form_is(pt_str body, const char *key, const char *value)
{
    char want[300];
    snprintf(want, sizeof want, "%s=%s", key, value);
    return has(body, want);
}

static pt_str form_get(pt_str body, const char *key)
{
    size_t kl = strlen(key), i;
    for (i = 0; i + kl < body.n; i++)
        if ((i == 0 || body.p[i - 1] == '&') && !memcmp(body.p + i, key, kl) && body.p[i + kl] == '=') {
            size_t e = i + kl + 1;
            while (e < body.n && body.p[e] != '&')
                e++;
            return (pt_str){body.p + i + kl + 1, e - i - kl - 1};
        }
    return PT_STR_NULL;
}

static void mcp_answer(const pt_http_req *req, rx *x)
{
    pt_arena ar;
    const pt_json *j;
    pt_str method, id;
    const char *auth = hdr(req, "Authorization"), *sid = hdr(req, "Mcp-Session-Id");
    char want[100];
    pt_arena_init(&ar, pt_sys_allocator(), 0);
    j = pt_json_parse(req->body, &ar, NULL);
    method = pt_json_gets(j, "method", &ar);
    id = pt_json_get(j, "id") ? pt_json_get(j, "id")->raw : PT_STR_NULL;
    snprintf(want, sizeof want, "Bearer %s", rs.token);
    if (!auth || strcmp(auth, want) != 0) {
        x->status = 401;
        snprintf(x->headers, sizeof x->headers,
                 "WWW-Authenticate: Bearer resource_metadata=\"https://mcp.test/.well-known/oauth-protected-resource/mcp\"\n");
    } else if (pt_str_eqz(method, "initialize")) {
        x->status = 200;
        snprintf(x->headers, sizeof x->headers, "Content-Type: application/json\nMcp-Session-Id: s%d\n", ++rs.sessions);
        pt_buf_putz(&x->body, "{\"jsonrpc\":\"2.0\",\"id\":");
        pt_buf_puts(&x->body, id);
        pt_buf_putz(&x->body, ",\"result\":{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{\"tools\":{}}}}");
    } else if (!sid || !hdr(req, "MCP-Protocol-Version")) {
        rs.bad_headers = true;
        x->status = 400;
    } else if (rs.drop_once) {
        rs.drop_once = false;
        x->status = 404;
    } else if (!id.n) {
        x->status = 202;
    } else {
        x->status = 200;
        snprintf(x->headers, sizeof x->headers, "Content-Type: text/event-stream\n");
        if (pt_str_eqz(method, "tools/list")) {
            pt_buf_putz(&x->body, "event: message\ndata: {\"jsonrpc\":\"2.0\",\"id\":");
            pt_buf_puts(&x->body, id);
            pt_buf_putz(&x->body, ",\"result\":{\"tools\":[{\"name\":\"hello\",\"inputSchema\":{\"type\":\"object\"}}]}}\n\n");
        } else {
            pt_buf_putz(&x->body, ": a comment\n\ndata: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/message\"}\n\n");
            pt_buf_putz(&x->body, "data: {\"jsonrpc\":\"2.0\",\"id\":");
            pt_buf_puts(&x->body, id);
            pt_buf_putz(&x->body, ",\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"hello from http\"}]}}\n\n");
        }
    }
    pt_arena_free(&ar);
}

static void *rs_start(pt_http_driver *d, const pt_http_req *req, const pt_http_sink *sink)
{
    rx *x;
    (void)d;
    if (!strncmp(req->url, "faux:", 5))
        return rs.faux->start(rs.faux, req, sink);
    x = (rx *)calloc(1, sizeof *x);
    x->sink = *sink;
    pt_buf_init(&x->body, pt_sys_allocator());
    x->status = 404;
    if (!strcmp(req->url, "https://mcp.test/mcp")) {
        mcp_answer(req, x);
    } else if (!strcmp(req->url, "https://mcp.test/.well-known/oauth-protected-resource/mcp")) {
        x->status = 200;
        pt_buf_putz(&x->body, "{\"resource\":\"https://mcp.test/mcp\",\"authorization_servers\":[\"https://auth.test\"]}");
    } else if (!strcmp(req->url, "https://auth.test/.well-known/oauth-authorization-server")) {
        x->status = 200;
        pt_buf_putz(&x->body, "{\"issuer\":\"https://auth.test\",\"authorization_endpoint\":\"https://auth.test/authorize\","
                              "\"token_endpoint\":\"https://auth.test/token\",\"registration_endpoint\":"
                              "\"https://auth.test/register\",\"code_challenge_methods_supported\":[\"S256\"]}");
    } else if (!strcmp(req->url, "https://auth.test/register")) {
        rs.registered = has(req->body, "\"redirect_uris\":[\"http://127.0.0.1:");
        x->status = 201;
        pt_buf_putz(&x->body, "{\"client_id\":\"cid1\"}");
    } else if (!strcmp(req->url, "https://auth.test/token")) {
        if (form_is(req->body, "grant_type", "authorization_code") && form_is(req->body, "code", "thecode") &&
            form_is(req->body, "client_id", "cid1")) {
            /* the verifier hashes to the challenge sent before */
            pt_str v = form_get(req->body, "code_verifier");
            unsigned char dg[32];
            pt_sha256 sh;
            pt_buf c;
            pt_sha256_init(&sh);
            pt_sha256_update(&sh, v.p, v.n);
            pt_sha256_final(&sh, dg);
            pt_buf_init(&c, pt_sys_allocator());
            pt_base64url(&c, (pt_str){(const char *)dg, 32});
            rs.pkce_ok = v.n >= 43 && pt_str_eqz(pt_buf_str(&c), rs.challenge);
            pt_buf_free(&c);
            rs.resource_ok = form_is(req->body, "resource", "https%3A%2F%2Fmcp.test%2Fmcp");
            x->status = 200;
            pt_buf_putz(&x->body, "{\"access_token\":\"tok1\",\"refresh_token\":\"ref1\",\"token_type\":\"Bearer\"}");
        } else if (form_is(req->body, "grant_type", "refresh_token") && form_is(req->body, "refresh_token", "ref1")) {
            rs.refreshes++;
            x->status = 200;
            pt_buf_putz(&x->body, "{\"access_token\":\"tok2\",\"token_type\":\"bearer\",\"expires_in\":3600}");
        } else {
            x->status = 400;
            pt_buf_putz(&x->body, "{\"error\":\"invalid_grant\"}");
        }
    }
    if (!x->headers[0] && x->body.n)
        snprintf(x->headers, sizeof x->headers, "Content-Type: application/json\n");
    x->next = rs.list;
    rs.list = x;
    return x;
}

static void rx_free(rx *x)
{
    rx **pp;
    for (pp = &rs.list; *pp; pp = &(*pp)->next)
        if (*pp == x) {
            *pp = x->next;
            break;
        }
    pt_buf_free(&x->body);
    free(x);
}

static void rs_cancel(pt_http_driver *d, void *h)
{
    rx *x;
    (void)d;
    for (x = rs.list; x; x = x->next)
        if (x == h) {
            rx_free(x);
            return;
        }
    rs.faux->cancel(rs.faux, h);
}

static int rs_pump(pt_http_driver *d, int timeout_ms, pt_waitfd *fds, unsigned nfds)
{
    rx *x, *next;
    int n = rs.faux->pump(rs.faux, rs.list ? 0 : timeout_ms, fds, nfds);
    (void)d;
    if (n < 0)
        return n;
    for (x = rs.list; x; x = next) {
        size_t step;
        next = x->next;
        if (!x->head_sent) {
            const char *h = x->headers;
            x->head_sent = true;
            while (*h) {
                const char *nl = strchr(h, '\n'), *colon = strchr(h, ':');
                if (colon && colon < nl && x->sink.header)
                    x->sink.header(x->sink.ud, pt_str_trim((pt_str){h, (size_t)(colon - h)}),
                                   pt_str_trim((pt_str){colon + 1, (size_t)(nl - colon - 1)}));
                h = nl + 1;
            }
            x->sink.head(x->sink.ud, x->status, PT_STR_NULL);
        }
        step = x->body.n - x->at < 11 ? x->body.n - x->at : 11;
        if (step)
            x->sink.body(x->sink.ud, x->body.p + x->at, step);
        x->at += step;
        if (x->at >= x->body.n) {
            x->sink.done(x->sink.ud, NULL);
            rx_free(x);
        } else {
            n++;
        }
    }
    return n;
}

static void rs_free(pt_http_driver *d)
{
    (void)d;
    while (rs.list)
        rx_free(rs.list);
    rs.faux->free(rs.faux);
}

static pt_http_driver *remote(pt_http_driver *faux)
{
    rs.faux = faux;
    rs.vt.start = rs_start;
    rs.vt.cancel = rs_cancel;
    rs.vt.pump = rs_pump;
    rs.vt.free = rs_free;
    return &rs.vt;
}

static int count_results(sess *s, const char *needle)
{
    uint32_t i;
    int n = 0;
    for (i = 0; i < pt_entry_count(s->a); i++) {
        pt_entry_view v;
        n += pt_entry_at(s->a, i, &v) && !strcmp(v.kind, "tool_result") && has(v.msg->text, needle);
    }
    return n;
}

static void wait_state(sess *s, pt_mcp_state st, int ms)
{
    int i;
    for (i = 0; i < ms / 20 && info(s, 0).state != st; i++)
        pt_rt_step(s->rt, 20, NULL, 0);
}

static void test_remote(void)
{
    sess s;
    pt_rt_mcp_server c;
    bool err = true;
    memset(&c, 0, sizeof c);
    c.name = "web";
    c.url = "https://mcp.test/mcp";
    wrap = remote;
    saved[0] = 0;
    rs.token = "tok1";

    /* no token: it needs a login; the session goes on without it */
    open_sess(&s, "{\"turns\":[" DONE "]}", &c, 1, PT_PERM_AUTO);
    say(&s, "hi");
    CHECK(info(&s, 0).state == PT_MCP_AUTH);
    CHECK(!strcmp(info(&s, 0).transport, "http"));
    CHECK(has(pt_strz(notes), "[web] the server asks for a login: log in with /mcp login web"));
    /* the login: discovery, registration, the browser, the token */
    CHECK(pt_rt_mcp_login(s.rt, PT_S("web")));
    {
        int i;
        for (i = 0; i < 200 && !auth_url[0]; i++)
            pt_rt_step(s.rt, 20, NULL, 0);
    }
    CHECK(rs.registered);
    CHECK(!strncmp(auth_url, "https://auth.test/authorize?response_type=code&client_id=cid1&", 62));
    CHECK(query(auth_url, "code_challenge_method").n == 4);
    CHECK(pt_str_eqz(query(auth_url, "resource"), "https%3A%2F%2Fmcp.test%2Fmcp"));
    snprintf(rs.challenge, sizeof rs.challenge, "%.*s", (int)query(auth_url, "code_challenge").n,
             query(auth_url, "code_challenge").p);
    wait_state(&s, PT_MCP_READY, 4000);
    CHECK(info(&s, 0).state == PT_MCP_READY && info(&s, 0).ntools == 1);
    CHECK(rs.pkce_ok && rs.resource_ok);
    CHECK(has(pt_strz(saved), "\"access_token\":\"tok1\"") && has(pt_strz(saved), "\"refresh_token\":\"ref1\""));
    CHECK(has(pt_strz(saved), "\"client_id\":\"cid1\""));
    CHECK(has(pt_strz(notes), "[web] logged in"));
    if (browser_fd >= 0) {
        char buf[512] = "";
        CHECK(read(browser_fd, buf, sizeof buf - 1) > 0 && strstr(buf, "HTTP/1.1 200") && strstr(buf, "Logged in"));
        close(browser_fd);
        browser_fd = -1;
    }
    close_sess(&s);

    /* the token saved: a session offers its tool; SSE answers; a lost
     * session starts again */
    open_sess(&s, "{\"turns\":[" TURN(CALL("mcp__web__hello", "{}")) "," DONE "," TURN(CALL("mcp__web__hello", "{}")) "," DONE "," TURN(CALL("mcp__web__hello", "{}")) "," DONE "]}", &c, 1, PT_PERM_AUTO);
    say(&s, "one");
    CHECK(has(epoch_raw(&s), "mcp__web__hello"));
    CHECK(tool_result(&s, "hello from http", &err, NULL) && !err);
    CHECK(!rs.bad_headers);
    rs.drop_once = true;
    say(&s, "two");
    CHECK(tool_result(&s, "ended its session", &err, NULL) && err);
    wait_state(&s, PT_MCP_READY, 4000);
    say(&s, "three");
    CHECK(count_results(&s, "hello from http") == 2);
    close_sess(&s);

    /* the token expired: refreshed once */
    rs.token = "tok2";
    open_sess(&s, "{\"turns\":[" TURN(CALL("mcp__web__hello", "{}")) "," DONE "]}", &c, 1, PT_PERM_AUTO);
    say(&s, "one");
    CHECK(rs.refreshes == 1);
    CHECK(tool_result(&s, "hello from http", &err, NULL) && !err);
    CHECK(has(pt_strz(saved), "\"access_token\":\"tok2\"") && has(pt_strz(saved), "\"refresh_token\":\"ref1\""));
    /* logged out: the tokens are gone */
    CHECK(pt_rt_mcp_logout(s.rt, PT_S("web")));
    CHECK(info(&s, 0).state == PT_MCP_AUTH);
    CHECK(!has(pt_strz(saved), "tok2") && has(pt_strz(saved), "cid1"));
    close_sess(&s);
    wrap = NULL;
}

/* Config: servers from the user's file run; a project's wait for trust,
 * which a change to them takes back. */
static void test_config(void)
{
    static cli_mcp m;
    char tmpl[] = "/tmp/pytte-mcp-XXXXXX", path[600], err[300];
    const char *dir = mkdtemp(tmpl);
    static const char user[] =
        "{\"model\": \"x\", \"mcpServers\": {"
        "\"a\": {\"command\": \"srv-${PT_T_NAME}\", \"args\": [\"--x=${PT_T_NOPE:-dflt}\", \"$plain\"], "
        "\"env\": {\"K\": \"${PT_T_NAME}\", \"N\": 3}, \"timeout\": 9, \"trust_hints\": true},"
        "\"b\": {\"type\": \"http\", \"url\": \"https://h.test/mcp\", \"headers\": {\"X-Key\": \"k-${PT_T_NAME}\"}, "
        "\"oauth\": {\"client_id\": \"cid\"}, \"arguments\": {\"*\": {\"k\": \"${PT_T_NAME}-{model}\", \"n\": [1]}}},"
        "\"off\": {\"command\": \"x\", \"disabled\": true}}}";
    static const char proj[] = "{\"mcpServers\": {\"p\": {\"command\": \"p\", \"cwd\": \"sub\"}, \"a\": {\"command\": \"pa\"}}}";
    if (!dir)
        return;
    setenv("PT_T_NAME", "nm", 1);
    unsetenv("PT_T_NOPE");
    setenv("XDG_CONFIG_HOME", dir, 1);
    snprintf(path, sizeof path, "%s/pytte", dir);
    mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/pytte/config.json", dir);
    CHECK(pt_sys_write_file(path, user, sizeof user - 1));
    snprintf(path, sizeof path, "%s/proj", dir);
    mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/proj/.mcp.json", dir);
    CHECK(pt_sys_write_file(path, proj, sizeof proj - 1));
    snprintf(path, sizeof path, "%s/proj", dir);

    cli_mcp_load(&m, path);
    CHECK(!m.warn[0]);
    CHECK(m.n == 2 && m.nuntrusted == 2 && !m.trusted && m.hash[0]);
    if (m.n == 2) {
        CHECK(!strcmp(m.run[0].name, "a") && !strcmp(m.run[0].command, "srv-nm"));
        CHECK(m.run[0].nargs == 2 && !strcmp(m.run[0].args[0], "--x=dflt") && !strcmp(m.run[0].args[1], "$plain"));
        CHECK(m.run[0].nenv == 2 && !strcmp(m.run[0].env[0], "K=nm") && !strcmp(m.run[0].env[1], "N=3"));
        CHECK(m.run[0].timeout_s == 9 && m.run[0].trust_hints && !m.run[0].cwd);
        CHECK(!strcmp(m.run[1].url, "https://h.test/mcp") && m.run[1].nheaders == 1);
        CHECK(!strcmp(m.run[1].headers[0].value, "k-nm") && !strcmp(m.run[1].client_id, "cid"));
        CHECK(m.run[1].arguments && !strcmp(m.run[1].arguments, "{\"*\":{\"k\":\"nm-{model}\",\"n\":[1]}}"));
        CHECK(!m.run[0].arguments);
    }
    CHECK(!strcmp(m.info[2].source, ".mcp.json") && !m.info[2].runs);
    CHECK(cli_mcp_trust(&m, true, err, sizeof err));
    cli_mcp_free(&m);

    cli_mcp_load(&m, path);
    CHECK(m.trusted && m.n == 3 && m.nuntrusted == 0);
    if (m.n == 3) { /* the project's a in place of the user's */
        char want[700];
        CHECK(!strcmp(m.run[0].name, "a") && !strcmp(m.run[0].command, "pa"));
        snprintf(want, sizeof want, "%s/sub", path);
        CHECK(!strcmp(m.run[2].name, "p") && !strcmp(m.run[2].cwd, want));
        snprintf(want, sizeof want, "%s", path);
        CHECK(!strcmp(m.run[0].cwd, want)); /* the project's directory */
    }
    cli_mcp_free(&m);

    /* changed: asks again */
    snprintf(path, sizeof path, "%s/proj/.mcp.json", dir);
    {
        static const char evil[] = "{\"mcpServers\": {\"p\": {\"command\": \"evil\"}}}";
        CHECK(pt_sys_write_file(path, evil, sizeof evil - 1));
    }
    snprintf(path, sizeof path, "%s/proj", dir);
    cli_mcp_load(&m, path);
    CHECK(!m.trusted && m.n == 2 && m.nuntrusted == 1);
    CHECK(cli_mcp_trust(&m, true, err, sizeof err) && cli_mcp_trust(&m, false, err, sizeof err));
    cli_mcp_free(&m);
    cli_mcp_load(&m, path);
    CHECK(!m.trusted);
    cli_mcp_free(&m);

    /* problems: warned, skipped */
    snprintf(path, sizeof path, "%s/pytte/config.json", dir);
    {
        static const char bad[] =
            "{\"mcpServers\": {\"a b\": {\"command\": \"x\"}, \"s\": {\"type\": \"sse\", \"url\": \"https://x\"}}}";
        CHECK(pt_sys_write_file(path, bad, sizeof bad - 1));
    }
    snprintf(path, sizeof path, "%s/proj", dir);
    cli_mcp_load(&m, path);
    CHECK(strstr(m.warn, "names are 1 to 32") != NULL);
    CHECK(m.n == 0);
    cli_mcp_free(&m);
    snprintf(path, sizeof path, "%s/pytte/config.json", dir);
    {
        static const char bad[] = "{\"mcpServers\": {\"s\": {\"url\": \"https://x\", \"arguments\": {\"*\": 3}}}}";
        CHECK(pt_sys_write_file(path, bad, sizeof bad - 1));
    }
    snprintf(path, sizeof path, "%s/proj", dir);
    cli_mcp_load(&m, path);
    CHECK(strstr(m.warn, "each tool's arguments is an object") != NULL);
    CHECK(m.n == 1 && !m.run[0].arguments); /* the server still runs, without them */
    cli_mcp_free(&m);
    {
        char cmd[700];
        snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
        CHECK(system(cmd) == 0);
    }
}

static void test_proto(void)
{
    pt_buf b;
    pt_arena ar;
    mcp_msg m;
    pt_buf_init(&b, pt_sys_allocator());
    pt_arena_init(&ar, pt_sys_allocator(), 0);
    mcp_tool_name(PT_S("my.srv"), PT_S("do it"), &b);
    CHECK_STR(pt_buf_str(&b), "mcp__my_srv__do_it");
    pt_buf_reset(&b);
    mcp_tool_name(PT_S("s"), PT_S("a_very_long_tool_name_that_goes_on_and_on_and_on_and_on_and_on"), &b);
    CHECK(b.n == 64 && b.p[55] == '_');
    CHECK(mcp_server_name_ok(PT_S("git-hub_2")) && !mcp_server_name_ok(PT_S("a b")) &&
          !mcp_server_name_ok(PT_STR_NULL));
    CHECK(mcp_parse(PT_S("{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{}}"), &ar, &m));
    CHECK(m.kind == MCP_RESPONSE && m.numeric && m.nid == 7);
    CHECK(mcp_parse(PT_S("{\"jsonrpc\":\"2.0\",\"id\":\"x\",\"error\":{\"code\":-1,\"message\":\"no\"}}"), &ar, &m));
    CHECK(m.is_error && !m.numeric && m.err_code == -1);
    CHECK_STR(m.err_message, "no");
    CHECK(mcp_parse(PT_S("{\"jsonrpc\":\"2.0\",\"method\":\"a/b\"}"), &ar, &m) && m.kind == MCP_NOTIFY);
    CHECK(!mcp_parse(PT_S("{\"jsonrpc\":\"1.0\",\"method\":\"a\"}"), &ar, &m));
    CHECK(!mcp_parse(PT_S("{\"jsonrpc\":\"2.0\",\"id\":1}"), &ar, &m));
    CHECK(!mcp_parse(PT_S("[1]"), &ar, &m));
    CHECK_STR(mcp_cut(PT_S("h\xc3\xa9llo"), 2), "h");
    pt_buf_free(&b);
    pt_arena_free(&ar);
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--mcp-server"))
        return server(argv[2]);
    if (!realpath(argv[0], self)) {
        perror("realpath");
        return 1;
    }
    RUN(test_proto);
    RUN(test_tools);
    RUN(test_fill);
    RUN(test_list_error);
    RUN(test_ask);
    RUN(test_timeout);
    RUN(test_crash);
    RUN(test_startup_failures);
    RUN(test_resumed);
    RUN(test_prompts_resources);
    RUN(test_remote);
    RUN(test_config);
    return t_done("test_mcp");
}
