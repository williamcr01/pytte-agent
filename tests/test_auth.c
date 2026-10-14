/* API keys and logins saved by /login: auth.json next to config.json. */
#define _POSIX_C_SOURCE 200809L
#include "t.h"

#include "cli/cli.h"

#include <sys/stat.h>
#include <unistd.h>

static char dir[64], path[200];

static void put(const char *text)
{
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static void test_valid(void)
{
    CHECK(cli_auth_valid(PT_S("sk-ant-abc_123")));
    CHECK(!cli_auth_valid(PT_S("")));
    CHECK(!cli_auth_valid(PT_S("sk ant")));
    CHECK(!cli_auth_valid(PT_S("sk\nant")));
    CHECK(!cli_auth_valid(PT_S("sk-\xc3\xa9")));
}

static void test_load_and_save(void)
{
    char err[300];
    const char *old;
    size_t n;
    char *text;
    struct stat st;
    put("{\n  \"openai\": {\"key\": \"sk-open\"},\n  \"bad\": {\"key\": \"has space\"},\n  \"other\": 1\n}\n");
    old = cli_auth_key("openai");
    CHECK(old && strcmp(old, "sk-open") == 0);
    CHECK(!cli_auth_key("bad"));
    CHECK(!cli_auth_key("anthropic"));
    /* a new key: every other member is kept */
    CHECK(cli_auth_save("anthropic", PT_S("sk-ant"), err, sizeof err));
    CHECK(!err[0]);
    CHECK(strcmp(cli_auth_key("anthropic"), "sk-ant") == 0);
    text = pt_sys_read_file(path, &n);
    CHECK_STR(((pt_str){text, text ? n : 0}),
              "{\n  \"openai\": {\n    \"key\": \"sk-open\"\n  },\n  \"bad\": {\n    \"key\": \"has space\"\n  },\n"
              "  \"other\": 1,\n  \"anthropic\": {\n    \"key\": \"sk-ant\"\n  }\n}\n");
    free(text);
    /* replaced: the old string stays valid */
    CHECK(cli_auth_save("openai", PT_S("sk-new"), err, sizeof err));
    CHECK(strcmp(cli_auth_key("openai"), "sk-new") == 0);
    CHECK(strcmp(old, "sk-open") == 0);
    CHECK(!cli_auth_save("openai", PT_S("two words"), err, sizeof err));
    CHECK(!cli_auth_save("a.b", PT_S("k"), err, sizeof err));
    /* a file that does not parse is never overwritten */
    put("{oops");
    CHECK(!cli_auth_save("openai", PT_S("sk-x"), err, sizeof err));
    text = pt_sys_read_file(path, &n);
    CHECK_STR(((pt_str){text, text ? n : 0}), "{oops");
    free(text);
    CHECK(strcmp(cli_auth_key("openai"), "sk-new") == 0);
    /* a new file is private */
    unlink(path);
    CHECK(cli_auth_save("openai", PT_S("sk-y"), err, sizeof err));
    CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
}

/* Subscription logins share the file with keys. */
static void test_logins(void)
{
    char err[300];
    cli_login l, old;
    size_t n;
    char *text;
    put("{\"anthropic\": {\"key\": \"sk-ant\"},\n"
        " \"openai-codex\": {\"access\": \"a1\", \"refresh\": \"r1\", \"expires\": 1000, \"account_id\": \"acc\"}}\n");
    CHECK(cli_auth_login("openai-codex", true, &l));
    CHECK(strcmp(l.access, "a1") == 0 && strcmp(l.refresh, "r1") == 0 && strcmp(l.account_id, "acc") == 0);
    CHECK(l.expires == 1000);
    CHECK(!cli_auth_login("anthropic", false, &l));
    old = l;
    l.access = "a2";
    l.refresh = "r2";
    l.expires = 2000;
    CHECK(cli_auth_login_save("openai-codex", &l, err, sizeof err));
    CHECK(cli_auth_login("openai-codex", false, &l) && strcmp(l.access, "a2") == 0 && l.expires == 2000);
    CHECK(strcmp(old.access, "a1") == 0); /* replaced: the old strings stay valid */
    text = pt_sys_read_file(path, &n);
    CHECK(text && strstr(text, "\"key\": \"sk-ant\"") && strstr(text, "\"refresh\": \"r2\"") &&
          strstr(text, "\"expires\": 2000"));
    free(text);
    /* another pytte refreshed it: reload sees the file's */
    put("{\"openai-codex\": {\"access\": \"a3\", \"refresh\": \"r3\", \"expires\": 3000}}\n");
    CHECK(cli_auth_login("openai-codex", false, &l) && strcmp(l.access, "a2") == 0);
    CHECK(cli_auth_login("openai-codex", true, &l) && strcmp(l.access, "a3") == 0 && !l.account_id[0]);
    /* not saved: still used by this process */
    put("{oops");
    l.access = "a4";
    CHECK(!cli_auth_login_save("openai-codex", &l, err, sizeof err) && err[0]);
    CHECK(cli_auth_login("openai-codex", false, &l) && strcmp(l.access, "a4") == 0);
    l.access = "two words";
    CHECK(!cli_auth_login_save("openai-codex", &l, err, sizeof err));
}

int main(void)
{
    char cmd[200];
    snprintf(dir, sizeof dir, "/tmp/pytte-auth-XXXXXX");
    if (!mkdtemp(dir))
        return 1;
    setenv("XDG_CONFIG_HOME", dir, 1);
    snprintf(path, sizeof path, "%s/pytte", dir);
    mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/pytte/auth.json", dir);
    RUN(test_valid);
    RUN(test_load_and_save);
    RUN(test_logins);
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd) != 0)
        fprintf(stderr, "cannot remove %s\n", dir);
    return t_done("test_auth");
}
