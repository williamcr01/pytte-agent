/* The catalog: providers from config.json, models from update-catalog. */
#define _POSIX_C_SOURCE 200809L
#include "t.h"

#include "cli/cli.h"

#include <sys/stat.h>
#include <unistd.h>

static char dir[64], cache[200];
static pt_arena ar;
static char warn[600];

/* Load with config.json's "providers" given as text (NULL: none). */
static void load(const char *providers)
{
    pt_json_err e;
    pt_json *j = NULL;
    pt_arena_reset(&ar);
    if (providers)
        j = pt_json_parse(pt_strz(providers), &ar, &e);
    warn[0] = 0;
    cli_catalog_load(j, "config.json", warn, sizeof warn);
}

static bool resolve(const char *ref, pt_ai_target *t)
{
    const char *why;
    return pt_ai_resolve(cli_catalog(), pt_strz(ref), t, &ar, &why);
}

static void test_none(void)
{
    pt_ai_target t;
    load(NULL);
    CHECK(!warn[0]);
    /* nothing to add: the static table itself */
    CHECK(cli_catalog()->models == pt_ai_builtin_catalog()->models);
    CHECK(resolve("anthropic/claude-opus-5-5", &t) && t.known);
}

static void test_custom(void)
{
    pt_ai_target t;
    load("{\"groq\": {\"base_url\": \"https://api.groq.com/openai/v1\", \"context_window\": 131072,"
         "  \"compat\": {\"max_tokens_field\": \"max_completion_tokens\", \"stream_usage\": false},"
         "  \"headers\": {\"X-A\": \"b\"},"
         "  \"models\": {\"z-model\": {}, \"llama\": {\"name\": \"Llama\", \"images\": true,"
         "                \"cost\": {\"input\": 0.59, \"output\": 0.79}}}},"
         " \"local-llm\": {\"base_url\": \"http://localhost:11434/v1\", \"auth\": \"none\","
         "  \"reasoning\": true, \"efforts\": [\"none\", \"high\"],"
         "  \"models\": {\"qwen3\": {}, \"deep\": {\"efforts\": [\"max\"]}}}}");
    CHECK_STR(pt_strz(warn), "");
    CHECK(cli_catalog()->nproviders == pt_ai_builtin_catalog()->nproviders + 2);
    CHECK(resolve("groq/llama", &t) && t.known);
    CHECK(strcmp(t.provider->base_url, "https://api.groq.com/openai/v1") == 0);
    CHECK(strcmp(t.provider->key_env[0], "GROQ_API_KEY") == 0 && !t.provider->key_env[1]);
    CHECK(t.provider->auth == PT_AUTH_BEARER);
    CHECK(strcmp(t.provider->headers[0].name, "X-A") == 0 && strcmp(t.provider->headers[0].value, "b") == 0);
    CHECK(strcmp(pt_ai_api_id(t.api), "openai-completions") == 0);
    CHECK(strcmp(t.model.name, "Llama") == 0 && t.model.images);
    CHECK(t.model.context_window == 131072);
    CHECK(t.model.cost.input == 590000 && t.model.cost.output == 790000);
    CHECK(t.compat.max_tokens_field == PT_MAXTOK_MAX_COMPLETION_TOKENS && t.compat.stream_usage == PT_NO);
    CHECK(resolve("groq/z-model", &t) && t.known && strcmp(t.model.name, "z-model") == 0);
    CHECK(resolve("groq/unlisted", &t) && !t.known && t.model.context_window == 131072);
    CHECK(resolve("local-llm/qwen3", &t) && t.known && t.provider->auth == PT_AUTH_NONE);
    CHECK(strcmp(t.provider->key_env[0], "LOCAL_LLM_API_KEY") == 0);
    /* efforts: the model's, else the provider's */
    CHECK(t.efforts == (PT_EFF_OFF | PT_EFF_HIGH) && pt_ai_effort(&t, PT_REASON_LOW) == PT_REASON_HIGH);
    CHECK(resolve("local-llm/deep", &t) && t.efforts == PT_EFF_MAX);
    CHECK(resolve("local-llm/other", &t) && t.efforts == (PT_EFF_OFF | PT_EFF_HIGH));
    /* the built-in ones still resolve */
    CHECK(resolve("anthropic/claude-opus-5-5", &t) && t.known);
    CHECK(resolve("faux/faux", &t) && t.known);
}

static void test_bad(void)
{
    pt_ai_target t;
    load("{\"nourl\": {}, \"ok\": {\"base_url\": \"https://x.test/v1\"}}");
    CHECK(strstr(warn, "providers.nourl: base_url is required") != NULL);
    CHECK(!resolve("nourl/m", &t));
    CHECK(resolve("ok/m", &t));
    load("{\"openai\": {\"base_url\": \"https://evil.test\"}}");
    CHECK(strstr(warn, "a built-in provider") != NULL);
    CHECK(resolve("openai/gpt-5", &t) && strcmp(t.provider->base_url, "https://api.openai.com/v1") == 0);
    load("{\"x\": {\"base_url\": \"https://x.test\", \"api\": \"faux\"}}");
    CHECK(strstr(warn, "api is") != NULL);
    load("{\"a/b\": {\"base_url\": \"https://x.test\"}}");
    CHECK(strstr(warn, "letters, digits") != NULL);
    load("{\"x\": {\"base_url\": \"https://x.test\", \"efforts\": [\"huge\"]}}");
    CHECK(strstr(warn, "efforts is a list") != NULL);
    load("{\"x\": {\"base_url\": \"https://x.test\", \"bas_url\": 1}}");
    CHECK(strstr(warn, "unknown setting \"bas_url\"") != NULL);
    CHECK(resolve("x/m", &t));
}

/* A small models.dev and OpenRouter, written to models.bin. */
static const char dev[] =
    "{\"anthropic\": {\"models\": {"
    "\"claude-opus-4-5\": {\"name\": \"Opus 4.5\", \"tool_call\": true, \"reasoning\": true,"
    "  \"limit\": {\"context\": 200000, \"output\": 64000}, \"cost\": {\"input\": 5, \"output\": 25},"
    "  \"modalities\": {\"input\": [\"text\", \"image\"]}},"
    "\"claude-opus-4-6\": {\"tool_call\": true, \"reasoning\": true, \"reasoning_options\": ["
    "  {\"type\": \"effort\", \"values\": [\"low\", \"medium\", \"high\", \"max\", \"default\"]},"
    "  {\"type\": \"budget_tokens\", \"min\": 1024}]},"
    "\"claude-toggle\": {\"tool_call\": true, \"reasoning\": true, \"reasoning_options\": ["
    "  {\"type\": \"toggle\"}, {\"type\": \"effort\", \"values\": [\"low\", \"high\"]}]},"
    "\"claude-opus-5-5\": {\"name\": \"Renamed\", \"tool_call\": true},"
    "\"claude-new-9\": {\"name\": \"Claude New 9\", \"tool_call\": true, \"reasoning\": true,"
    "  \"limit\": {\"context\": 2000000}},"
    "\"claude-old\": {\"tool_call\": true, \"status\": \"deprecated\"},"
    "\"claude-notools\": {\"tool_call\": false}}},"
    " \"openai\": {\"models\": {\"gpt-realtime\": {\"tool_call\": true}}},"
    " \"groq\": {\"models\": {\"from-dev\": {\"name\": \"From models.dev\", \"tool_call\": true}}}}";
static const char openrouter[] =
    "{\"data\": [{\"id\": \"x/y\", \"name\": \"Y\", \"context_length\": 1000,"
    "  \"supported_parameters\": [\"tools\", \"reasoning\"],"
    "  \"pricing\": {\"prompt\": \"0.000003\", \"completion\": \"0.000015\"}},"
    " {\"id\": \"x/must\", \"supported_parameters\": [\"tools\", \"reasoning\"],"
    "  \"reasoning\": {\"mandatory\": true, \"supported_efforts\": [\"max\", \"high\"]}},"
    " {\"id\": \"x/maps\", \"supported_parameters\": [\"tools\", \"reasoning\"], \"reasoning\": {\"mandatory\": false}},"
    " {\"id\": \"x/notools\", \"supported_parameters\": []},"
    " {\"id\": \"x/router\", \"supported_parameters\": [\"tools\"], \"pricing\": {\"prompt\": \"-1\"}}]}";

static void update(pt_buf *out, char *msg, size_t cap)
{
    load("{\"groq\": {\"base_url\": \"https://api.groq.com/openai/v1\"}}");
    CHECK(cli_catalog_convert(pt_strz(dev), pt_strz(openrouter), "2026-10-13T00:00:00Z", out, msg, cap));
    CHECK(pt_sys_write_file(cache, out->p, out->n));
}

static void test_convert(void)
{
    pt_ai_target t;
    pt_buf out;
    char msg[300];
    pt_buf_init(&out, pt_sys_allocator());
    update(&out, msg, sizeof msg);
    CHECK_STR(pt_strz(msg), "anthropic 5, openai 0, openrouter 3, groq 1");
    load("{\"groq\": {\"base_url\": \"https://api.groq.com/openai/v1\"}}");
    CHECK_STR(pt_strz(warn), "");
    CHECK(resolve("anthropic/claude-opus-4-5", &t) && t.known);
    CHECK(!t.model.reasoning); /* no adaptive thinking before 4.6 */
    CHECK(t.model.images && t.model.context_window == 200000 && t.model.max_tokens == 64000);
    CHECK(t.model.cost.input == 5000000 && t.model.cost.output == 25000000);
    CHECK(resolve("anthropic/claude-opus-4-6", &t) && t.model.reasoning);
    CHECK(resolve("anthropic/claude-old", &t) && !t.known);
    CHECK(resolve("anthropic/claude-notools", &t) && !t.known);
    CHECK(resolve("openrouter/x/y", &t) && t.known && t.model.reasoning && t.model.context_window == 1000);
    CHECK(t.model.cost.input == 3000000 && t.model.cost.output == 15000000);
    CHECK(resolve("openrouter/x/router", &t) && !t.known);
    /* efforts: what the source lists; toggle and not mandatory allow off */
    CHECK(resolve("anthropic/claude-opus-4-6", &t) &&
          t.model.efforts == (PT_EFF_LOW | PT_EFF_MEDIUM | PT_EFF_HIGH | PT_EFF_MAX));
    CHECK(resolve("anthropic/claude-toggle", &t) && t.model.efforts == (PT_EFF_OFF | PT_EFF_LOW | PT_EFF_HIGH));
    CHECK(resolve("anthropic/claude-opus-4-5", &t) && t.model.efforts == 0);
    CHECK(resolve("openrouter/x/must", &t) && t.model.efforts == (PT_EFF_HIGH | PT_EFF_MAX));
    CHECK(resolve("openrouter/x/maps", &t) &&
          t.model.efforts == (PT_EFF_OFF | PT_EFF_LOW | PT_EFF_MEDIUM | PT_EFF_HIGH));
    CHECK(resolve("openrouter/x/y", &t) && t.model.efforts == 0);
    CHECK(!cli_catalog_convert(PT_S("[]"), PT_S("{}"), "", &out, msg, sizeof msg));
    unlink(cache);
    pt_buf_free(&out);
}

static void test_fetched(void)
{
    pt_ai_target t;
    pt_buf out;
    char msg[300];
    pt_buf_init(&out, pt_sys_allocator());
    update(&out, msg, sizeof msg);
    load("{\"groq\": {\"base_url\": \"https://api.groq.com/openai/v1\"}}");
    CHECK(resolve("anthropic/claude-new-9", &t) && t.known && t.model.context_window == 2000000);
    /* fetched values win; compat flags stay the binary's */
    CHECK(resolve("anthropic/claude-opus-5-5", &t) && strcmp(t.model.name, "Renamed") == 0);
    CHECK(t.model.compat.mid_conversation_system == PT_YES && t.model.compat.fallbacks == PT_YES);
    /* built-in models the fetch lacks are kept */
    CHECK(resolve("anthropic/claude-sonnet-5-5", &t) && t.known);
    CHECK(resolve("openai/gpt-4.1", &t) && t.known);
    CHECK(resolve("faux/faux", &t) && t.known);
    CHECK(resolve("groq/from-dev", &t) && t.known && strcmp(t.model.name, "From models.dev") == 0);
    /* config.json's models replace the fetched ones */
    load("{\"groq\": {\"base_url\": \"https://api.groq.com/openai/v1\", \"models\": [\"mine\"]}}");
    CHECK(resolve("groq/mine", &t) && t.known);
    CHECK(resolve("groq/from-dev", &t) && !t.known);
    /* a file of another build, or cut short, is ignored */
    out.p[8] ^= 1; /* the version */
    CHECK(pt_sys_write_file(cache, out.p, out.n));
    load(NULL);
    CHECK(strstr(warn, "update-catalog") != NULL);
    CHECK(resolve("anthropic/claude-opus-5-5", &t) && strcmp(t.model.name, "Claude Opus 5.5") == 0);
    CHECK(cli_catalog()->models == pt_ai_builtin_catalog()->models);
    out.p[8] ^= 1;
    CHECK(pt_sys_write_file(cache, out.p, out.n - 1));
    load(NULL);
    CHECK(strstr(warn, "update-catalog") != NULL);
    unlink(cache);
    pt_buf_free(&out);
}

int main(void)
{
    char cmd[200];
    snprintf(dir, sizeof dir, "/tmp/pytte-providers-XXXXXX");
    if (!mkdtemp(dir))
        return 1;
    setenv("XDG_CACHE_HOME", dir, 1);
    snprintf(cache, sizeof cache, "%s/pytte", dir);
    mkdir(cache, 0700);
    snprintf(cache, sizeof cache, "%s/pytte/models.bin", dir);
    pt_arena_init(&ar, pt_sys_allocator(), 0);
    RUN(test_none);
    RUN(test_custom);
    RUN(test_bad);
    RUN(test_fetched);
    RUN(test_convert);
    pt_arena_free(&ar);
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd) != 0)
        fprintf(stderr, "cannot remove %s\n", dir);
    return t_done("test_providers");
}
