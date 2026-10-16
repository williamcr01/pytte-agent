/* Permission rules (section 15.5): the gate with allow and deny rules, a
 * faux model calling stand-in tools; and the rules of the config files,
 * with the trust of a project's allow rules. */
#define _POSIX_C_SOURCE 200809L
#include "t.h"

#include "cli/cli.h"

#include <sys/stat.h>
#include <unistd.h>

/* ---- stand-in tools: they only count their calls ---- */

static int ran;

static void tool_run(void *ud, pt_rt *rt, const pt_fx *call)
{
    pt_rt_out out;
    (void)ud;
    ran++;
    memset(&out, 0, sizeof out);
    out.content = PT_S("ran");
    pt_rt_tool_done(rt, call->id, &out);
}

static void add_tool(pt_rt *rt, const char *name, bool read_only)
{
    pt_rt_tool t;
    memset(&t, 0, sizeof t);
    t.def.name = pt_strz(name);
    t.def.description = PT_S("A stand-in.");
    t.def.schema = PT_S("{\"type\":\"object\"}");
    t.def.replay = PT_REPLAY_UNSAFE;
    t.def.read_only = read_only;
    t.def.source = PT_S("test");
    t.run = tool_run;
    CHECK(pt_rt_tool_add(rt, &t));
}

typedef struct rule_def {
    bool deny;
    const char *spec;
    pt_rule_src src;
} rule_def;

static struct {
    bool asked;
    int ran;
    bool is_error;
    char text[400];
    char log[20000];
} res;

/* One call of tool with args (a JSON object) in mode perm, under rules
 * (NULL: none); trust: the project's allow rules apply. When dir is
 * given, the rules come from the config files instead (cli_perms_apply). */
static void run_call(const char *tool, const char *args, pt_perm_mode perm, const rule_def *rules, bool trust,
                     const char *dir)
{
    static char script[4096];
    pt_store *st = pt_store_memory_new();
    pt_http_driver *http;
    pt_opts o;
    pt_rt_cfg rc;
    pt_agent *a;
    pt_rt *rt;
    pt_session_cfg c;
    pt_identity me;
    pt_input in;
    pt_rt_result r;
    uint32_t i;
    snprintf(script, sizeof script,
             "{\"turns\":[{\"content\":[{\"type\":\"tool_call\",\"name\":\"%s\",\"args\":%s}]},"
             "{\"content\":[{\"type\":\"text\",\"text\":\"done\"}]}]}",
             tool, args);
    http = pt_http_faux_new(pt_strz(script));
    memset(&o, 0, sizeof o);
    o.alloc = T_AL;
    o.config.model = PT_S("faux/faux");
    o.blob = pt_rt_read_blob;
    o.blob_ud = st;
    a = pt_new(&o);
    memset(&rc, 0, sizeof rc);
    rc.store = st;
    rc.http = http;
    rt = pt_rt_new(&rc, a);
    add_tool(rt, "bash", false);
    add_tool(rt, "read", true);
    add_tool(rt, "web_fetch", false);
    add_tool(rt, "mcp__github__create_issue", false);
    add_tool(rt, "mcp__other__thing", false);
    pt_rt_set_perm(rt, perm);
    if (dir)
        CHECK(cli_perms_apply(rt, dir));
    for (i = 0; rules && rules[i].spec; i++)
        CHECK(pt_rt_rule_add(rt, rules[i].deny, pt_strz(rules[i].spec), rules[i].src));
    if (!dir)
        pt_rt_rules_trust(rt, trust);
    memset(&c, 0, sizeof c);
    c.cwd = PT_S("/tmp");
    c.host = PT_S("test");
    c.system = PT_S("Test.");
    c.tools = pt_rt_tool_defs(rt, &c.ntools);
    CHECK(pt_create(a, &c));
    memset(&me, 0, sizeof me);
    me.host = PT_S("test");
    CHECK(pt_open(a, &me, false));
    memset(&in, 0, sizeof in);
    in.text = PT_S("go");
    CHECK(pt_submit(a, &in));
    memset(&res, 0, sizeof res);
    ran = 0;
    while ((r = pt_rt_run(rt)) == PT_RT_ASK) {
        pt_ask_view v;
        res.asked = true;
        CHECK(pt_asks(a, &v, 1) == 1);
        CHECK(pt_approve(a, v.task, true, PT_STR_NULL));
    }
    CHECK(r == PT_RT_IDLE);
    for (i = 0; i < pt_entry_count(a); i++) {
        pt_entry_view v;
        if (pt_entry_at(a, i, &v) && strcmp(v.kind, "tool_result") == 0) {
            snprintf(res.text, sizeof res.text, "%.*s", (int)v.msg->text.n, v.msg->text.p);
            res.is_error = v.msg->is_error;
        }
    }
    {
        pt_str log = pt_store_memory_log(st);
        snprintf(res.log, sizeof res.log, "%.*s", (int)(log.n < sizeof res.log - 1 ? log.n : sizeof res.log - 1), log.p);
    }
    res.ran = ran;
    pt_rt_close(rt);
    pt_rt_free(rt);
    pt_free(a);
    http->free(http);
    pt_store_memory_free(st);
}

static bool has(const char *hay, const char *needle)
{
    return strstr(hay, needle) != NULL;
}

#define U PT_RULE_USER
#define P PT_RULE_PROJECT

/* Run a bash line in ask mode under rules: true when it ran without asking. */
static bool quiet(const char *command, const rule_def *rules)
{
    char args[300];
    snprintf(args, sizeof args, "{\"command\":\"%s\"}", command);
    run_call("bash", args, PT_PERM_ASK, rules, false, NULL);
    return res.ran == 1 && !res.asked;
}

static void test_allow_bash(void)
{
    static const rule_def diff[] = {{false, "bash(git diff:*)", U}, {false, NULL, 0}};
    static const rule_def exact[] = {{false, "bash(git status)", U}, {false, NULL, 0}};
    static const rule_def two[] = {{false, "bash(git status)", U}, {false, "bash(make:*)", U}, {false, NULL, 0}};
    CHECK(!quiet("git diff", NULL)); /* no rules: it asks */
    CHECK(res.asked && res.ran == 1);
    CHECK(quiet("git diff", diff));
    CHECK(quiet("git diff HEAD~1 -- src", diff));
    CHECK(!quiet("git diffstat", diff)); /* words, not letters */
    CHECK(!quiet("git log", diff));
    /* every command of the line must be allowed */
    CHECK(!quiet("git diff; rm -rf x", diff));
    CHECK(!quiet("git diff && rm -rf x", diff));
    CHECK(!quiet("git diff | sh", diff));
    CHECK(!quiet("git diff \\n rm x", diff));
    CHECK(!quiet("git diff $(rm x)", diff));
    CHECK(!quiet("git diff > /etc/x", diff));
    CHECK(quiet("git status", exact));
    CHECK(!quiet("git status -s", exact));
    CHECK(!quiet("git statusx", exact));
    CHECK(quiet("git status && make test", two));
    CHECK(quiet("make -j4 | git status", two));
    CHECK(!quiet("git status && make test; rm x", two));
    /* the rule that allowed it is committed with the call */
    quiet("git diff", diff);
    CHECK(has(res.log, "\"rule\":\"allow bash(git diff:*) (user config)\""));
}

static void test_deny(void)
{
    static const rule_def both[] = {{false, "bash(git:*)", U}, {true, "bash(git push:*)", U}, {false, NULL, 0}};
    static const rule_def rm[] = {{true, "bash(rm -rf:*)", P}, {false, NULL, 0}};
    static const rule_def all[] = {{false, "bash", U}, {true, "bash(rm -rf:*)", U}, {false, NULL, 0}};
    CHECK(quiet("git status", both));
    CHECK(!quiet("git push origin main", both));
    run_call("bash", "{\"command\":\"git push\"}", PT_PERM_ASK, both, false, NULL);
    CHECK(!res.asked && res.ran == 0 && res.is_error);
    CHECK(has(res.text, "Permission denied") && has(res.text, "bash(git push:*) (user config)"));
    /* a deny rule matches any command of the line, and what is inside $() */
    run_call("bash", "{\"command\":\"ls; rm -rf build\"}", PT_PERM_AUTO, rm, false, NULL);
    CHECK(res.ran == 0 && has(res.text, "bash(rm -rf:*) (project config)"));
    run_call("bash", "{\"command\":\"echo $(rm -rf build)\"}", PT_PERM_AUTO, rm, false, NULL);
    CHECK(res.ran == 0);
    run_call("bash", "{\"command\":\"ls\"}", PT_PERM_AUTO, rm, false, NULL);
    CHECK(res.ran == 1 && !res.asked);
    /* deny beats the mode and a rule that allows everything */
    run_call("bash", "{\"command\":\"rm -rf /\"}", PT_PERM_ASK, all, false, NULL);
    CHECK(res.ran == 0 && !res.asked);
    run_call("bash", "{\"command\":\"echo hi\"}", PT_PERM_ASK, all, false, NULL);
    CHECK(res.ran == 1 && !res.asked);
    /* a project's deny applies though the project is not trusted */
    run_call("bash", "{\"command\":\"rm -rf x\"}", PT_PERM_AUTO, rm, false, NULL);
    CHECK(res.ran == 0);
}

static void test_read_only(void)
{
    static const rule_def allow[] = {{false, "bash", U}, {false, NULL, 0}};
    static const rule_def deny[] = {{true, "read(*.env)", U}, {false, NULL, 0}};
    /* read-only stays read-only: an allow rule does not let bash run */
    run_call("bash", "{\"command\":\"ls\"}", PT_PERM_READ_ONLY, allow, false, NULL);
    CHECK(res.ran == 0 && !res.asked && has(res.text, "read-only"));
    run_call("read", "{\"path\":\"a.c\"}", PT_PERM_READ_ONLY, allow, false, NULL);
    CHECK(res.ran == 1);
    /* a deny rule stops even a read-only tool */
    run_call("read", "{\"path\":\"prod.env\"}", PT_PERM_ASK, deny, false, NULL);
    CHECK(res.ran == 0 && has(res.text, "read(*.env)"));
    run_call("read", "{\"path\":\"prod.txt\"}", PT_PERM_ASK, deny, false, NULL);
    CHECK(res.ran == 1);
}

static void test_project_trust(void)
{
    static const rule_def proj[] = {{false, "bash(make:*)", P}, {false, NULL, 0}};
    run_call("bash", "{\"command\":\"make test\"}", PT_PERM_ASK, proj, false, NULL);
    CHECK(res.asked && res.ran == 1 && !has(res.log, "\"rule\""));
    run_call("bash", "{\"command\":\"make test\"}", PT_PERM_ASK, proj, true, NULL);
    CHECK(!res.asked && res.ran == 1 && has(res.log, "allow bash(make:*) (project config)"));
}

static void test_tools_and_hosts(void)
{
    static const rule_def web[] = {{false, "web_fetch(docs.python.org)", U}, {true, "web_fetch(*.internal)", U},
                                   {false, "web_fetch(*.internal)", U}, {false, NULL, 0}};
    static const rule_def mcp[] = {{false, "mcp__github__*", U}, {false, NULL, 0}};
    run_call("web_fetch", "{\"url\":\"https://docs.python.org/3/\"}", PT_PERM_ASK, web, false, NULL);
    CHECK(!res.asked && res.ran == 1);
    run_call("web_fetch", "{\"url\":\"https://DOCS.python.org:8443/x\"}", PT_PERM_ASK, web, false, NULL);
    CHECK(!res.asked && res.ran == 1);
    run_call("web_fetch", "{\"url\":\"https://example.com/\"}", PT_PERM_ASK, web, false, NULL);
    CHECK(res.asked);
    run_call("web_fetch", "{\"url\":\"http://wiki.internal/x\"}", PT_PERM_ASK, web, false, NULL);
    CHECK(res.ran == 0 && !res.asked); /* deny beats the allow of the same pattern */
    run_call("mcp__github__create_issue", "{\"title\":\"t\"}", PT_PERM_ASK, mcp, false, NULL);
    CHECK(!res.asked && res.ran == 1);
    run_call("mcp__other__thing", "{\"title\":\"t\"}", PT_PERM_ASK, mcp, false, NULL);
    CHECK(res.asked);
}

static void test_spec(void)
{
    const char *why = "";
    CHECK(pt_rule_check(PT_S("bash"), &why));
    CHECK(pt_rule_check(PT_S("bash(git diff:*)"), &why));
    CHECK(pt_rule_check(PT_S("mcp__github__*"), &why));
    CHECK(pt_rule_check(PT_S(" web_fetch(a.test) "), &why));
    CHECK(!pt_rule_check(PT_S(""), &why));
    CHECK(!pt_rule_check(PT_S("(x)"), &why));
    CHECK(!pt_rule_check(PT_S("bash("), &why));
    CHECK(!pt_rule_check(PT_S("bash()"), &why));
    CHECK(!pt_rule_check(PT_S("bash(x) y"), &why));
    CHECK(!pt_rule_check(PT_S("ba sh"), &why));
    CHECK(why[0]);
}

/* ---- the config files ---- */

static char root[128], home[160], proj[160];

static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static char *slurp(const char *path)
{
    size_t n;
    return pt_sys_read_file(path, &n);
}

static void test_files(void)
{
    char path[400], err[400];
    pt_rt_rule_info in[16];
    char *text;
    snprintf(root, sizeof root, "/tmp/pytte-perms-XXXXXX");
    CHECK(mkdtemp(root) != NULL);
    snprintf(home, sizeof home, "%s/home", root);
    snprintf(proj, sizeof proj, "%s/proj", root);
    snprintf(path, sizeof path, "%s/home/pytte", root);
    mkdir(home, 0700);
    mkdir(path, 0700);
    mkdir(proj, 0700);
    snprintf(path, sizeof path, "%s/.pytte", proj);
    mkdir(path, 0700);
    CHECK(setenv("XDG_CONFIG_HOME", home, 1) == 0);
    snprintf(path, sizeof path, "%s/pytte/config.json", home);
    put(path, "{\"model\": \"faux/faux\", \"permissions\": {\"allow\": [\"bash(git status)\", \"bash(echo:*)\"], "
              "\"deny\": [\"bash(rm -rf:*)\"]}}\n");
    snprintf(path, sizeof path, "%s/.pytte/config.json", proj);
    put(path, "{\"permissions\": {\"allow\": [\"bash(make:*)\"], \"deny\": [\"bash(curl:*)\"]}}\n");

    /* both files apply; the project's allow does not, untrusted */
    run_call("bash", "{\"command\":\"git status\"}", PT_PERM_ASK, NULL, false, proj);
    CHECK(!res.asked && res.ran == 1 && has(res.log, "allow bash(git status) (user config)"));
    run_call("bash", "{\"command\":\"rm -rf x\"}", PT_PERM_AUTO, NULL, false, proj);
    CHECK(res.ran == 0 && has(res.text, "(user config)"));
    run_call("bash", "{\"command\":\"curl example.com\"}", PT_PERM_AUTO, NULL, false, proj);
    CHECK(res.ran == 0 && has(res.text, "(project config)")); /* a project's deny applies at once */
    run_call("bash", "{\"command\":\"make test\"}", PT_PERM_ASK, NULL, false, proj);
    CHECK(res.asked && res.ran == 1);

    /* trusting applies it; changing the list asks again */
    CHECK(cli_perms_trust(proj, true, err, sizeof err));
    run_call("bash", "{\"command\":\"make test\"}", PT_PERM_ASK, NULL, false, proj);
    CHECK(!res.asked && res.ran == 1 && has(res.log, "allow bash(make:*) (project config)"));
    snprintf(path, sizeof path, "%s/.pytte/config.json", proj);
    put(path, "{\"permissions\": {\"allow\": [\"bash(make:*)\", \"bash(sh:*)\"]}}\n");
    run_call("bash", "{\"command\":\"make test\"}", PT_PERM_ASK, NULL, false, proj);
    CHECK(res.asked);
    CHECK(cli_perms_trust(proj, true, err, sizeof err));
    CHECK(cli_perms_trust(proj, false, err, sizeof err));
    run_call("bash", "{\"command\":\"make test\"}", PT_PERM_ASK, NULL, false, proj);
    CHECK(res.asked);
    CHECK(cli_perms_trust(proj, true, err, sizeof err));
    put(path, "{}\n");
    CHECK(!cli_perms_trust(proj, true, err, sizeof err)); /* nothing to trust */
    CHECK(has(err, "no allow rules"));

    /* "always allow": saved to the user file, once, keeping its other keys */
    snprintf(path, sizeof path, "%s/pytte/config.json", home);
    CHECK(cli_perms_allow_save(PT_S("bash(ls:*)"), err, sizeof err));
    CHECK(cli_perms_allow_save(PT_S("bash(ls:*)"), err, sizeof err));
    text = slurp(path);
    CHECK(text && has(text, "\"model\": \"faux/faux\"") && has(text, "\"bash(ls:*)\"") && has(text, "\"bash(git status)\""));
    CHECK(text && strstr(strstr(text, "bash(ls:*)") + 1, "bash(ls:*)") == NULL);
    free(text);
    run_call("bash", "{\"command\":\"ls -la\"}", PT_PERM_ASK, NULL, false, proj);
    CHECK(!res.asked && res.ran == 1);
    snprintf(path, sizeof path, "%s/pytte/config.json", home);
    put(path, "{\"permissions\": 3}\n");
    CHECK(!cli_perms_allow_save(PT_S("bash(ls:*)"), err, sizeof err)); /* not an object */
    put(path, "{\"model\": \"x\"}\n");
    CHECK(cli_perms_allow_save(PT_S("bash(ls:*)"), err, sizeof err)); /* no list yet: made */
    text = slurp(path);
    CHECK(text && has(text, "bash(ls:*)") && has(text, "\"model\""));
    free(text);
    put(path, "{\"permissions\": {\"allow\": 3}}\n");
    CHECK(!cli_perms_allow_save(PT_S("bash(ls:*)"), err, sizeof err));
    put(path, "not json");
    CHECK(!cli_perms_allow_save(PT_S("bash(ls:*)"), err, sizeof err));
    text = slurp(path);
    CHECK(text && strcmp(text, "not json") == 0); /* never overwritten */
    free(text);

    /* the rules in force, with their sources */
    {
        pt_store *st = pt_store_memory_new();
        pt_opts o;
        pt_rt_cfg rc;
        pt_agent *a;
        pt_rt *rt;
        uint32_t n;
        put(path, "{\"permissions\": {\"allow\": [\"bash(a:*)\", 7, \"bad(\"], \"deny\": [\"bash(b)\"]}}\n");
        snprintf(path, sizeof path, "%s/.pytte/config.json", proj);
        put(path, "{\"permissions\": {\"allow\": [\"bash(c)\"]}}\n");
        memset(&o, 0, sizeof o);
        o.alloc = T_AL;
        o.config.model = PT_S("faux/faux");
        a = pt_new(&o);
        memset(&rc, 0, sizeof rc);
        rc.store = st;
        rt = pt_rt_new(&rc, a);
        CHECK(pt_rt_allow(rt, PT_S("bash"), PT_S("echo")));
        CHECK(cli_perms_apply(rt, proj));
        n = pt_rt_rules(rt, in, 16);
        CHECK(n == 4); /* the bad entries are skipped */
        CHECK_STR(in[0].spec, "bash(echo:*)");
        CHECK(in[0].src == PT_RULE_SESSION && in[0].active);
        CHECK_STR(in[1].spec, "bash(b)");
        CHECK(in[1].deny && in[1].src == PT_RULE_USER);
        CHECK_STR(in[2].spec, "bash(a:*)");
        CHECK_STR(in[3].spec, "bash(c)");
        CHECK(in[3].src == PT_RULE_PROJECT && !in[3].active);
        CHECK(cli_perms_apply(rt, proj)); /* again: the same, not twice */
        CHECK(pt_rt_rules(rt, in, 16) == 4);
        pt_rt_close(rt);
        pt_rt_free(rt);
        pt_free(a);
        pt_store_memory_free(st);
    }
    {
        char cmd[200];
        snprintf(cmd, sizeof cmd, "rm -rf %s", root);
        (void)!system(cmd);
    }
}

static void test_check(void)
{
    pt_arena ar;
    char warn[300];
    pt_arena_init(&ar, pt_sys_allocator(), 0);
    CHECK(cli_perms_check(pt_json_parse(PT_S("{}"), &ar, NULL), "c.json", warn, sizeof warn));
    CHECK(cli_perms_check(pt_json_parse(PT_S("{\"permissions\":{\"allow\":[\"bash\"]}}"), &ar, NULL), "c.json", warn, sizeof warn));
    warn[0] = 0;
    CHECK(!cli_perms_check(pt_json_parse(PT_S("{\"permissions\":{\"deny\":[\"bash(\"]}}"), &ar, NULL), "c.json", warn, sizeof warn));
    CHECK(has(warn, "c.json: permissions.deny") && has(warn, "skipped"));
    CHECK(!cli_perms_check(pt_json_parse(PT_S("{\"permissions\":{\"allow\":\"bash\"}}"), &ar, NULL), "c.json", warn, sizeof warn));
    CHECK(!cli_perms_check(pt_json_parse(PT_S("{\"permissions\":{\"allow\":[1]}}"), &ar, NULL), "c.json", warn, sizeof warn));
    pt_arena_free(&ar);
}

int main(void)
{
    RUN(test_spec);
    RUN(test_allow_bash);
    RUN(test_deny);
    RUN(test_read_only);
    RUN(test_project_trust);
    RUN(test_tools_and_hosts);
    RUN(test_check);
    RUN(test_files);
    return t_done("test_perms");
}
