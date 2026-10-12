/* AI layer: model resolution, request building, cross-provider transforms,
 * the faux provider end to end, and message JSON round trips. */
#include "ai/ai.h" /* internal: pt_fix_tool_id */
#include "t.h"

static pt_arena A;

static pt_ai_target target(const char *ref)
{
    pt_ai_target t;
    const char *why;
    bool ok = pt_ai_resolve(pt_ai_builtin_catalog(), pt_strz(ref), &t, &A, &why);
    CHECK(ok);
    return t;
}

static pt_json *body_of(const pt_http_req *r)
{
    pt_json_err err;
    pt_json *v = pt_json_parse(r->body, &A, &err);
    if (!v)
        fprintf(stderr, "bad request body at %zu (%s): %.*s\n", err.pos, err.msg, (int)r->body.n, r->body.p);
    CHECK(v != NULL);
    return v;
}

static const char *header(const pt_http_req *r, const char *name)
{
    uint32_t i;
    for (i = 0; i < r->nheaders; i++)
        if (pt_str_ieq(pt_strz(r->headers[i].name), pt_strz(name)))
            return r->headers[i].value;
    return NULL;
}

static pt_str jget(const pt_json *o, const char *path)
{
    /* dotted path with numeric array indices: "messages.1.content.0.type" */
    char key[64];
    const char *p = path;
    const pt_json *v = o;
    while (*p && v) {
        size_t n = 0;
        while (p[n] && p[n] != '.')
            n++;
        memcpy(key, p, n);
        key[n] = 0;
        if (key[0] >= '0' && key[0] <= '9')
            v = pt_json_at(v, (uint32_t)atoi(key));
        else
            v = pt_json_get(v, key);
        p += n + (p[n] == '.');
    }
    if (!v)
        return PT_S("<missing>");
    return v->type == PT_J_STR ? pt_json_str(v, &A) : v->raw;
}

/* ---- a shared conversation ---- */

static pt_block blocks_a[3];
static pt_msg conv[7];
static pt_tool tools[1];

static pt_ai_context conversation(void)
{
    pt_ai_context c;
    memset(conv, 0, sizeof conv);
    memset(blocks_a, 0, sizeof blocks_a);
    conv[0].role = PT_ROLE_SYSTEM;
    conv[0].text = PT_S("You are pytte.");
    conv[1].role = PT_ROLE_USER;
    conv[1].text = PT_S("say hi with echo");
    blocks_a[0].kind = PT_BLOCK_THINKING;
    blocks_a[0].text = PT_S("Use echo.");
    blocks_a[0].signature = PT_S("SIG");
    blocks_a[1].kind = PT_BLOCK_TEXT;
    blocks_a[1].text = PT_S("Calling echo.");
    blocks_a[2].kind = PT_BLOCK_TOOL_CALL;
    blocks_a[2].id = PT_S("toolu_01ABC");
    blocks_a[2].name = PT_S("echo");
    blocks_a[2].args = PT_S("{\"text\": \"hi\"}");
    conv[2].role = PT_ROLE_ASSISTANT;
    conv[2].api = PT_S("anthropic-messages");
    conv[2].provider = PT_S("anthropic");
    conv[2].model = PT_S("claude-opus-5-5");
    conv[2].blocks = blocks_a;
    conv[2].nblocks = 3;
    conv[2].stop = PT_STOP_TOOL_USE;
    conv[3].role = PT_ROLE_TOOL_RESULT;
    conv[3].call_id = PT_S("toolu_01ABC");
    conv[3].name = PT_S("echo");
    conv[3].text = PT_S("hi");
    conv[4].role = PT_ROLE_SYSTEM;
    conv[4].text = PT_S("Be brief.");
    conv[5].role = PT_ROLE_USER;
    conv[5].text = PT_S("thanks");
    tools[0].name = PT_S("echo");
    tools[0].description = PT_S("Echo text back.");
    tools[0].schema = PT_S("{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}}}");
    c.msgs = conv;
    c.nmsgs = 6;
    c.tools = tools;
    c.ntools = 1;
    return c;
}

static void test_resolve(void)
{
    pt_ai_target t;
    const char *why;
    CHECK(!pt_ai_resolve(pt_ai_builtin_catalog(), PT_S("anthropic"), &t, &A, &why) && why);
    CHECK(!pt_ai_resolve(pt_ai_builtin_catalog(), PT_S("anthropic/"), &t, &A, &why) && why);
    t = target("anthropic/claude-opus-5-5");
    CHECK(t.known);
    CHECK(strcmp(t.model.id, "claude-opus-5-5") == 0);
    CHECK(t.compat.mid_conversation_system == PT_YES && t.compat.fallbacks == PT_YES);
    t = target("openrouter/acme/unlisted-1");
    CHECK(!t.known);
    CHECK(strcmp(t.model.id, "acme/unlisted-1") == 0);
    CHECK(strcmp(pt_ai_api_id(t.api), "openai-completions") == 0);
    CHECK(t.compat.reasoning_param == PT_RPARAM_OPENROUTER);
    CHECK(t.compat.system_role == PT_SYSROLE_SYSTEM); /* from the API defaults */
    t = target("openai/gpt-5");
    CHECK(t.known && t.compat.max_tokens_field == PT_MAXTOK_MAX_COMPLETION_TOKENS);
    CHECK(!pt_ai_resolve(pt_ai_builtin_catalog(), PT_S("nope/x"), &t, &A, &why) && why);
}

/* Resolution is a binary search: every model must be found as itself. */
static void test_catalog_sorted(void)
{
    const pt_ai_catalog *cat = pt_ai_builtin_catalog();
    char ref[128];
    uint32_t i;
    for (i = 0; i < cat->nmodels; i++) {
        const pt_model_entry *e = &cat->models[i];
        pt_ai_target t;
        const char *why;
        CHECK(e->provider < cat->nproviders);
        if (i) {
            const pt_model_entry *p = &cat->models[i - 1];
            CHECK(p->provider < e->provider || (p->provider == e->provider && strcmp(p->id, e->id) < 0));
        }
        snprintf(ref, sizeof ref, "%s/%s", cat->providers[e->provider].id, e->id);
        CHECK(pt_ai_resolve(cat, pt_strz(ref), &t, &A, &why) && t.known);
        CHECK(strcmp(t.model.id, e->id) == 0 && strcmp(t.model.name, e->name) == 0);
        CHECK(t.model.context_window == e->context_window && t.model.cost.input == e->cost.input);
    }
}

static const char *fake_env(void *ud, const char *name)
{
    (void)ud;
    return strcmp(name, "OPENAI_API_KEY") == 0 ? "sk-test" : NULL;
}

static void test_keys(void)
{
    pt_credentials c;
    const char *which;
    pt_ai_target t = target("openai/gpt-5");
    CHECK(pt_ai_find_key(t.provider, fake_env, NULL, &c, &which));
    CHECK_STR(c.api_key, "sk-test");
    CHECK(strcmp(which, "OPENAI_API_KEY") == 0);
    t = target("anthropic/claude-opus-5-5");
    CHECK(!pt_ai_find_key(t.provider, fake_env, NULL, &c, &which));
    t = target("faux/faux");
    CHECK(pt_ai_find_key(t.provider, fake_env, NULL, &c, &which) && !c.api_key.n);
}

static void test_anthropic_request(void)
{
    pt_ai_target t = target("anthropic/claude-opus-5-5");
    pt_ai_context c = conversation();
    pt_ai_opts o;
    pt_credentials cred = {PT_S("sk-ant-test")};
    pt_http_req r;
    pt_json *b;
    memset(&o, 0, sizeof o);
    CHECK(pt_ai_build(&t, &c, &o, &cred, &r, &A));
    CHECK(strcmp(r.url, "https://api.anthropic.com/v1/messages") == 0);
    CHECK(header(&r, "x-api-key") && strcmp(header(&r, "x-api-key"), "sk-ant-test") == 0);
    CHECK(header(&r, "anthropic-version") && strcmp(header(&r, "anthropic-version"), "2023-06-01") == 0);
    CHECK(header(&r, "anthropic-beta") &&
          strcmp(header(&r, "anthropic-beta"), "server-side-fallback-2026-07-01") == 0);
    b = body_of(&r);
    CHECK_STR(jget(b, "model"), "claude-opus-5-5");
    CHECK_STR(jget(b, "max_tokens"), "128000");
    CHECK_STR(jget(b, "stream"), "true");
    CHECK_STR(jget(b, "cache_control.type"), "ephemeral");
    CHECK_STR(jget(b, "thinking.type"), "adaptive");
    CHECK_STR(jget(b, "thinking.display"), "summarized");
    CHECK_STR(jget(b, "output_config.effort"), "high");
    CHECK_STR(jget(b, "fallbacks"), "default");
    CHECK_STR(jget(b, "system"), "You are pytte.");
    CHECK_STR(jget(b, "tools.0.input_schema.type"), "object");
    CHECK_STR(jget(b, "tools.0.eager_input_streaming"), "true");
    CHECK_STR(jget(b, "messages.0.role"), "user");
    CHECK_STR(jget(b, "messages.0.content.0.text"), "say hi with echo");
    CHECK_STR(jget(b, "messages.1.role"), "assistant");
    CHECK_STR(jget(b, "messages.1.content.0.type"), "thinking");
    CHECK_STR(jget(b, "messages.1.content.0.signature"), "SIG");
    CHECK_STR(jget(b, "messages.1.content.2.type"), "tool_use");
    CHECK_STR(jget(b, "messages.1.content.2.input"), "{\"text\": \"hi\"}"); /* raw, unchanged */
    CHECK_STR(jget(b, "messages.2.content.0.type"), "tool_result");
    CHECK_STR(jget(b, "messages.2.content.0.tool_use_id"), "toolu_01ABC");
    CHECK_STR(jget(b, "messages.3.role"), "system"); /* in place on this model */
    CHECK_STR(jget(b, "messages.3.content"), "Be brief.");
    CHECK_STR(jget(b, "messages.4.content.0.text"), "thanks");
    CHECK(pt_json_get(b, "messages")->len == 5);

    /* building twice gives the same bytes */
    {
        pt_http_req r2;
        CHECK(pt_ai_build(&t, &c, &o, &cred, &r2, &A));
        CHECK(pt_str_eq(r.body, r2.body));
    }

    /* effort follows the reasoning level */
    o.reasoning = PT_REASON_MAX;
    o.max_tokens = 1000;
    CHECK(pt_ai_build(&t, &c, &o, &cred, &r, &A));
    b = body_of(&r);
    CHECK_STR(jget(b, "output_config.effort"), "max");
    CHECK_STR(jget(b, "max_tokens"), "1000");

    /* unread thinking: no summary, the model's default display */
    o.hide_thinking = true;
    CHECK(pt_ai_build(&t, &c, &o, &cred, &r, &A));
    b = body_of(&r);
    CHECK_STR(jget(b, "thinking.type"), "adaptive");
    CHECK(pt_json_get(pt_json_get(b, "thinking"), "display") == NULL);
}

static void test_anthropic_folding_and_bad_args(void)
{
    /* Haiku 4.5: no mid-conversation system messages, no reasoning */
    pt_ai_target t = target("anthropic/claude-haiku-4-5");
    pt_ai_context c = conversation();
    pt_http_req r;
    pt_json *b;
    blocks_a[2].args = PT_S("{\"text\": \"h"); /* cut off */
    CHECK(pt_ai_build(&t, &c, NULL, NULL, &r, &A));
    b = body_of(&r);
    CHECK(pt_json_get(b, "thinking") == NULL);
    CHECK(pt_json_get(b, "output_config") == NULL);
    CHECK(pt_json_get(b, "fallbacks") == NULL);
    CHECK(header(&r, "anthropic-beta") == NULL);
    CHECK_STR(jget(b, "max_tokens"), "64000");
    CHECK_STR(jget(b, "messages.1.content.2.input.INVALID_JSON"), "{\"text\": \"h");
    /* the system message is folded after the tool result, in the same user turn */
    CHECK_STR(jget(b, "messages.2.content.0.type"), "tool_result");
    CHECK_STR(jget(b, "messages.2.content.1.text"), "thanks");
    CHECK_STR(jget(b, "messages.2.content.2.text"), "<system>\nBe brief.\n</system>");
    CHECK(pt_json_get(b, "messages")->len == 3);
}

static void test_openai_request(void)
{
    pt_ai_target t = target("openai/gpt-5");
    CHECK(strcmp(pt_ai_api_id(t.api), "openai-responses") == 0);
    CHECK(pt_ai_retarget(&t, PT_S("openai-completions")));
    pt_ai_context c = conversation();
    pt_ai_opts o;
    pt_credentials cred = {PT_S("sk-test")};
    pt_http_req r;
    pt_json *b;
    memset(&o, 0, sizeof o);
    CHECK(pt_ai_build(&t, &c, &o, &cred, &r, &A));
    CHECK(strcmp(r.url, "https://api.openai.com/v1/chat/completions") == 0);
    CHECK(header(&r, "authorization") && strcmp(header(&r, "authorization"), "Bearer sk-test") == 0);
    b = body_of(&r);
    CHECK_STR(jget(b, "stream_options.include_usage"), "true");
    CHECK_STR(jget(b, "max_completion_tokens"), "128000");
    CHECK(pt_json_get(b, "reasoning_effort") == NULL); /* default: server decides */
    CHECK_STR(jget(b, "messages.0.role"), "system");
    CHECK_STR(jget(b, "messages.1.role"), "user");
    CHECK_STR(jget(b, "messages.2.role"), "assistant");
    /* Anthropic thinking seen by OpenAI: plain text, joined with the text */
    CHECK_STR(jget(b, "messages.2.content"), "<thinking>\nUse echo.\n</thinking>Calling echo.");
    CHECK_STR(jget(b, "messages.2.tool_calls.0.id"), "toolu_01ABC");
    CHECK_STR(jget(b, "messages.2.tool_calls.0.function.arguments"), "{\"text\": \"hi\"}");
    CHECK_STR(jget(b, "messages.3.role"), "tool");
    CHECK_STR(jget(b, "messages.3.tool_call_id"), "toolu_01ABC");
    CHECK_STR(jget(b, "messages.4.role"), "system");
    CHECK_STR(jget(b, "messages.5.content"), "thanks");
    CHECK_STR(jget(b, "tools.0.function.parameters.type"), "object");

    o.reasoning = PT_REASON_LOW;
    CHECK(pt_ai_build(&t, &c, &o, &cred, &r, &A));
    CHECK_STR(jget(body_of(&r), "reasoning_effort"), "low");

    t = target("openrouter/acme/unlisted-1");
    o.reasoning = PT_REASON_HIGH;
    CHECK(pt_ai_build(&t, &c, &o, &cred, &r, &A));
    b = body_of(&r);
    CHECK(strcmp(r.url, "https://openrouter.ai/api/v1/chat/completions") == 0);
    CHECK(header(&r, "X-Title") != NULL);
    CHECK_STR(jget(b, "reasoning.effort"), "high");
    CHECK(pt_json_get(b, "max_tokens") == NULL); /* unknown model: omitted */
}

static void test_responses_request(void)
{
    /* history from the same API: the reasoning item goes back whole */
    pt_ai_target t = target("openai/gpt-5");
    pt_ai_context c = conversation();
    pt_ai_opts o;
    pt_credentials cred = {PT_S("sk-test")};
    pt_http_req r;
    pt_json *b;
    pt_str item = PT_S("{\"id\":\"rs_1\",\"type\":\"reasoning\",\"summary\":[],\"encrypted_content\":\"ENC\"}");
    memset(&o, 0, sizeof o);
    CHECK(strcmp(pt_ai_api_id(t.api), "openai-responses") == 0);
    conv[2].api = PT_S("openai-responses");
    conv[2].provider = PT_S("openai");
    blocks_a[0].signature = item;
    blocks_a[2].id = PT_S("call_1");
    conv[3].call_id = PT_S("call_1");
    CHECK(pt_ai_build(&t, &c, &o, &cred, &r, &A));
    CHECK(strcmp(r.url, "https://api.openai.com/v1/responses") == 0);
    CHECK(header(&r, "authorization") && strcmp(header(&r, "authorization"), "Bearer sk-test") == 0);
    b = body_of(&r);
    CHECK_STR(jget(b, "store"), "false");
    CHECK_STR(jget(b, "instructions"), "You are pytte.");
    CHECK_STR(jget(b, "max_output_tokens"), "128000");
    CHECK_STR(jget(b, "reasoning.summary"), "auto");
    CHECK(pt_json_get(pt_json_get(b, "reasoning"), "effort") == NULL); /* server default */
    CHECK_STR(jget(b, "include.0"), "reasoning.encrypted_content");
    CHECK_STR(jget(b, "input.0.role"), "user");
    CHECK_STR(jget(b, "input.0.content.0.type"), "input_text");
    CHECK_STR(jget(b, "input.1"), "{\"id\":\"rs_1\",\"type\":\"reasoning\",\"summary\":[],\"encrypted_content\":\"ENC\"}");
    CHECK_STR(jget(b, "input.2.type"), "message");
    CHECK_STR(jget(b, "input.2.content.0.type"), "output_text");
    CHECK_STR(jget(b, "input.2.content.0.text"), "Calling echo.");
    CHECK_STR(jget(b, "input.3.type"), "function_call");
    CHECK_STR(jget(b, "input.3.call_id"), "call_1");
    CHECK_STR(jget(b, "input.3.arguments"), "{\"text\": \"hi\"}");
    CHECK_STR(jget(b, "input.4.type"), "function_call_output");
    CHECK_STR(jget(b, "input.4.output"), "hi");
    CHECK_STR(jget(b, "input.5.role"), "developer");
    CHECK_STR(jget(b, "input.6.content.0.text"), "thanks");
    CHECK_STR(jget(b, "tools.0.type"), "function");
    CHECK_STR(jget(b, "tools.0.name"), "echo");
    CHECK_STR(jget(b, "tools.0.parameters.type"), "object");

    o.reasoning = PT_REASON_HIGH;
    CHECK(pt_ai_build(&t, &c, &o, &cred, &r, &A));
    CHECK_STR(jget(body_of(&r), "reasoning.effort"), "high");

    /* unread thinking: no summary */
    o.hide_thinking = true;
    CHECK(pt_ai_build(&t, &c, &o, &cred, &r, &A));
    b = body_of(&r);
    CHECK_STR(jget(b, "reasoning.effort"), "high");
    CHECK(pt_json_get(pt_json_get(b, "reasoning"), "summary") == NULL);
    o.hide_thinking = false;

    /* history from Anthropic: its thinking becomes text, signatures stay home */
    c = conversation();
    o.reasoning = PT_REASON_DEFAULT;
    CHECK(pt_ai_build(&t, &c, &o, &cred, &r, &A));
    b = body_of(&r);
    CHECK_STR(jget(b, "input.1.content.0.text"), "<thinking>\nUse echo.\n</thinking>");
    CHECK(!pt_str_contains(r.body, PT_S("SIG")));

    /* a model without reasoning gets no reasoning fields */
    t = target("openai/gpt-4.1");
    CHECK(pt_ai_build(&t, &c, &o, &cred, &r, &A));
    b = body_of(&r);
    CHECK(pt_json_get(b, "reasoning") == NULL && pt_json_get(b, "include") == NULL);
}

/* A tool result's image: in the result for Anthropic and Responses, in a
 * user message after the tool messages for Chat Completions, and a note
 * for a model that takes no images. */
static void test_images(void)
{
    pt_ai_target t = target("anthropic/claude-opus-5-5");
    pt_ai_context c = conversation();
    pt_http_req r;
    pt_json *b;
    conv[3].image = PT_S("abcd");
    conv[3].image_mime = PT_S("image/png");
    CHECK(t.model.images);
    CHECK(pt_ai_build(&t, &c, NULL, NULL, &r, &A));
    b = body_of(&r);
    CHECK_STR(jget(b, "messages.2.content.0.type"), "tool_result");
    CHECK_STR(jget(b, "messages.2.content.0.content.0.text"), "hi");
    CHECK_STR(jget(b, "messages.2.content.0.content.1.type"), "image");
    CHECK_STR(jget(b, "messages.2.content.0.content.1.source.type"), "base64");
    CHECK_STR(jget(b, "messages.2.content.0.content.1.source.media_type"), "image/png");
    CHECK_STR(jget(b, "messages.2.content.0.content.1.source.data"), "YWJjZA==");

    t.model.images = false;
    CHECK(pt_ai_build(&t, &c, NULL, NULL, &r, &A));
    b = body_of(&r);
    CHECK_STR(jget(b, "messages.2.content.0.content"),
              "hi\n[an image was omitted: this model does not take images]");
    CHECK(!pt_str_contains(r.body, PT_S("YWJjZA")));

    t = target("openai/gpt-5");
    CHECK(pt_ai_retarget(&t, PT_S("openai-completions")));
    CHECK(pt_ai_build(&t, &c, NULL, NULL, &r, &A));
    b = body_of(&r);
    CHECK_STR(jget(b, "messages.3.role"), "tool");
    CHECK_STR(jget(b, "messages.3.content"), "hi");
    CHECK_STR(jget(b, "messages.4.role"), "user");
    CHECK_STR(jget(b, "messages.4.content.1.type"), "image_url");
    CHECK_STR(jget(b, "messages.4.content.1.image_url.url"), "data:image/png;base64,YWJjZA==");
    CHECK_STR(jget(b, "messages.5.role"), "system");

    t = target("openai/gpt-5");
    conv[2].api = PT_S("openai-responses");
    conv[2].provider = PT_S("openai");
    blocks_a[0].signature = PT_S("{\"id\":\"rs_1\",\"type\":\"reasoning\",\"summary\":[]}");
    CHECK(pt_ai_build(&t, &c, NULL, NULL, &r, &A));
    b = body_of(&r);
    CHECK_STR(jget(b, "input.4.type"), "function_call_output");
    CHECK_STR(jget(b, "input.4.output.0.type"), "input_text");
    CHECK_STR(jget(b, "input.4.output.0.text"), "hi");
    CHECK_STR(jget(b, "input.4.output.1.type"), "input_image");
    CHECK_STR(jget(b, "input.4.output.1.image_url"), "data:image/png;base64,YWJjZA==");
}

static void test_tool_ids(void)
{
    pt_str id = PT_S("call:with.bad/chars-and-a-very-long-tail-0123456789");
    pt_str a = pt_fix_tool_id(id, 40, &A), b = pt_fix_tool_id(id, 40, &A), c;
    size_t i;
    CHECK(a.n == 40 && pt_str_eq(a, b));
    for (i = 0; i < a.n; i++)
        CHECK(a.p[i] != ':' && a.p[i] != '.' && a.p[i] != '/');
    c = pt_fix_tool_id(PT_S("call:with.bad/chars-and-a-very-long-tail-0123456780"), 40, &A);
    CHECK(!pt_str_eq(a, c)); /* differs only in the tail, still distinct */
    CHECK(pt_fix_tool_id(PT_S("toolu_01ABC"), 40, &A).p == (const char *)0 ||
          pt_str_eqz(pt_fix_tool_id(PT_S("toolu_01ABC"), 40, &A), "toolu_01ABC"));
}

static void test_cross_provider_view(void)
{
    /* An OpenAI assistant turn with a long call ID and an opaque block,
     * sent to Anthropic: the opaque block is dropped, the ID is rewritten
     * the same way in the call and in its result. */
    pt_block bl[2];
    pt_msg m[3];
    pt_ai_context in, out;
    pt_ai_target t = target("anthropic/claude-opus-5-5");
    memset(bl, 0, sizeof bl);
    memset(m, 0, sizeof m);
    bl[0].kind = PT_BLOCK_OPAQUE;
    bl[0].api = PT_S("openai-completions");
    bl[0].raw = PT_S("{\"reasoning_details\":[]}");
    bl[1].kind = PT_BLOCK_TOOL_CALL;
    bl[1].id = PT_S("call.with.dots|and|pipes");
    bl[1].name = PT_S("echo");
    bl[1].args = PT_S("{}");
    m[0].role = PT_ROLE_USER;
    m[0].text = PT_S("hi");
    m[1].role = PT_ROLE_ASSISTANT;
    m[1].api = PT_S("openai-completions");
    m[1].provider = PT_S("openrouter");
    m[1].blocks = bl;
    m[1].nblocks = 2;
    m[2].role = PT_ROLE_TOOL_RESULT;
    m[2].call_id = bl[1].id;
    m[2].text = PT_S("ok");
    in.msgs = m;
    in.nmsgs = 3;
    in.tools = NULL;
    in.ntools = 0;
    CHECK(pt_ai_target_view(&t, &in, &out, &A));
    CHECK(out.nmsgs == 3);
    CHECK(out.msgs[1].nblocks == 1);
    CHECK(out.msgs[1].blocks[0].kind == PT_BLOCK_TOOL_CALL);
    CHECK(pt_str_eq(out.msgs[1].blocks[0].id, out.msgs[2].call_id));
    CHECK(pt_str_find(out.msgs[2].call_id, '.') < 0 && pt_str_find(out.msgs[2].call_id, '|') < 0);
    /* the input is untouched */
    CHECK(m[1].nblocks == 2 && pt_str_eqz(m[2].call_id, "call.with.dots|and|pipes"));
}

/* ---- faux, end to end ---- */

typedef struct sink {
    pt_buf text, log;
} sink;

static void on_event(void *ud, const pt_ai_event *ev)
{
    sink *k = (sink *)ud;
    if (ev->kind == PT_EV_TEXT_DELTA)
        pt_buf_puts(&k->text, ev->delta);
    if (ev->kind != PT_EV_TEXT_DELTA && ev->kind != PT_EV_THINKING_DELTA && ev->kind != PT_EV_TOOLCALL_DELTA) {
        if (k->log.n)
            pt_buf_putc(&k->log, ' ');
        pt_buf_putz(&k->log, pt_ev_name(ev->kind));
    }
}

static const char *script =
    "{\"chunk\": 5, \"turns\": ["
    "{\"content\": [{\"type\": \"thinking\", \"text\": \"hmm\"},"
    "               {\"type\": \"text\", \"text\": \"Calling echo \u2192 now\"},"
    "               {\"type\": \"tool_call\", \"name\": \"echo\", \"args\": {\"text\": \"hi\"}}]},"
    "{\"status\": 529, \"body\": {\"class\": \"retryable\", \"message\": \"overloaded\"}},"
    "{\"content\": [{\"type\": \"text\", \"text\": \"partial answer\"}], \"cut\": 150},"
    "{\"content\": [{\"type\": \"text\", \"text\": \"oops\"}], \"error\": \"boom\", \"class\": \"fatal\"}"
    "]}";

/* Run turn n of the script (by faking n assistant messages in the context). */
static const pt_msg *faux_turn(uint32_t n, sink *k, const pt_ai_error **err)
{
    pt_ai_target t = target("faux/faux");
    pt_msg msgs[8];
    pt_ai_context c;
    pt_http_req r;
    pt_faux_response resp;
    pt_ai_stream *s;
    uint32_t i;
    size_t at = 0, len;
    memset(msgs, 0, sizeof msgs);
    msgs[0].role = PT_ROLE_USER;
    msgs[0].text = PT_S("go");
    for (i = 1; i <= n; i++)
        msgs[i].role = PT_ROLE_ASSISTANT;
    c.msgs = msgs;
    c.nmsgs = n + 1;
    c.tools = NULL;
    c.ntools = 0;
    CHECK(pt_ai_build(&t, &c, NULL, NULL, &r, &A));
    CHECK(pt_faux_respond(pt_strz(script), r.body, NULL, &resp, &A));
    pt_buf_init(&k->text, T_AL);
    pt_buf_init(&k->log, T_AL);
    s = pt_ai_stream_new(&t, &A, T_AL, on_event, k, 1000);
    pt_ai_stream_head(s, resp.status, PT_STR_NULL);
    len = resp.cut >= 0 && (size_t)resp.cut < resp.body.n ? (size_t)resp.cut : resp.body.n;
    while (at < len) {
        size_t step = resp.chunk && resp.chunk < len - at ? resp.chunk : len - at;
        pt_ai_stream_body(s, resp.body.p + at, step, 1000 + (int64_t)at);
        at += step;
    }
    pt_ai_stream_end(s, NULL, 5000);
    {
        /* copy out what we need before freeing the stream */
        static pt_ai_error e;
        static pt_msg m;
        e = *pt_ai_stream_error(s);
        m = *pt_ai_stream_message(s);
        pt_ai_stream_free(s);
        *err = &e;
        return &m;
    }
}

static void test_faux(void)
{
    sink k;
    const pt_ai_error *e;
    const pt_msg *m = faux_turn(0, &k, &e);
    CHECK(m->stop == PT_STOP_TOOL_USE && e->cls == PT_ERR_NONE);
    CHECK(m->nblocks == 3);
    CHECK_STR(m->blocks[0].text, "hmm");
    CHECK_STR(m->blocks[0].signature, "faux-signature");
    CHECK_STR(pt_buf_str(&k.text), "Calling echo \xE2\x86\x92 now");
    CHECK_STR(m->blocks[2].args, "{\"text\": \"hi\"}");
    CHECK_STR(m->blocks[2].name, "echo");
    /* done arrives with the last body chunk; the duration is taken there */
    CHECK(m->timestamp == 1000 && m->duration_ms > 0 && m->duration_ms < 4000);
    CHECK_STR(pt_buf_str(&k.log), "start thinking_start thinking_end text_start text_end toolcall_start "
                                  "toolcall_end done");
    pt_buf_free(&k.text);
    pt_buf_free(&k.log);

    m = faux_turn(1, &k, &e);
    CHECK(m->stop == PT_STOP_ERROR && e->cls == PT_ERR_RETRYABLE && e->status == 529);
    CHECK_STR(pt_buf_str(&k.log), "error");
    pt_buf_free(&k.text);
    pt_buf_free(&k.log);

    m = faux_turn(2, &k, &e); /* cut mid-stream */
    CHECK(m->stop == PT_STOP_ERROR && e->cls == PT_ERR_RETRYABLE);
    CHECK(m->nblocks == 1);
    CHECK_STR(m->blocks[0].text, "partia"); /* the deltas that arrived before the cut */
    pt_buf_free(&k.text);
    pt_buf_free(&k.log);

    m = faux_turn(3, &k, &e); /* error event after content */
    CHECK(m->stop == PT_STOP_ERROR && e->cls == PT_ERR_FATAL);
    CHECK_STR(m->error, "boom");
    CHECK_STR(m->blocks[0].text, "oops");
    pt_buf_free(&k.text);
    pt_buf_free(&k.log);

    m = faux_turn(4, &k, &e); /* past the end of the script */
    CHECK(m->stop == PT_STOP_STOP);
    pt_buf_free(&k.text);
    pt_buf_free(&k.log);
}

static void test_abort(void)
{
    pt_ai_target t = target("faux/faux");
    sink k;
    pt_ai_stream *s;
    pt_buf_init(&k.text, T_AL);
    pt_buf_init(&k.log, T_AL);
    s = pt_ai_stream_new(&t, &A, T_AL, on_event, &k, 0);
    pt_ai_stream_head(s, 200, PT_STR_NULL);
    {
        const char *body = "event: start\ndata: {}\n\nevent: block\ndata: {\"type\":\"text\"}\n\n"
                           "event: delta\ndata: {\"text\":\"par\"}\n\n";
        pt_ai_stream_body(s, body, strlen(body), 1);
    }
    pt_ai_stream_abort(s, 2);
    pt_ai_stream_end(s, NULL, 3); /* no second terminal event */
    CHECK(pt_ai_stream_message(s)->stop == PT_STOP_ABORTED);
    CHECK(pt_ai_stream_error(s)->cls == PT_ERR_ABORTED);
    CHECK_STR(pt_ai_stream_message(s)->blocks[0].text, "par");
    CHECK_STR(pt_buf_str(&k.log), "start text_start error");
    pt_ai_stream_free(s);
    pt_buf_free(&k.text);
    pt_buf_free(&k.log);
}

static void test_transport_error(void)
{
    pt_ai_target t = target("anthropic/claude-opus-5-5");
    pt_ai_stream *s = pt_ai_stream_new(&t, &A, T_AL, NULL, NULL, 0);
    pt_ai_stream_end(s, "Could not resolve host", 10);
    CHECK(pt_ai_stream_error(s)->cls == PT_ERR_RETRYABLE);
    CHECK_STR(pt_ai_stream_message(s)->error, "connection error: Could not resolve host");
    pt_ai_stream_free(s);
}

static void test_msg_roundtrip(void)
{
    pt_ai_context c = conversation();
    pt_block extra[4];
    pt_msg m = conv[2], back;
    pt_buf b1, b2;
    pt_jw w;
    pt_json *v;
    memcpy(extra, blocks_a, sizeof blocks_a);
    memset(&extra[3], 0, sizeof extra[3]);
    extra[3].kind = PT_BLOCK_OPAQUE;
    extra[3].api = PT_S("anthropic-messages");
    extra[3].raw = PT_S("{\"type\": \"fallback\", \"n\": [1, 2.50]}");
    m.blocks = extra;
    m.nblocks = 4;
    m.usage.input = 10;
    m.usage.cache_read = 5;
    m.raw_stop = PT_S("tool_use");
    m.timestamp = 1760000000000;
    m.duration_ms = 1234;
    (void)c;
    pt_buf_init(&b1, T_AL);
    pt_buf_init(&b2, T_AL);
    pt_jw_init(&w, &b1);
    pt_msg_write_json(&w, &m);
    v = pt_json_parse(pt_buf_str(&b1), &A, NULL);
    CHECK(v && pt_msg_read_json(v, &back, &A));
    pt_jw_init(&w, &b2);
    pt_msg_write_json(&w, &back);
    CHECK(pt_str_eq(pt_buf_str(&b1), pt_buf_str(&b2)));
    CHECK_STR(back.blocks[3].raw, "{\"type\": \"fallback\", \"n\": [1, 2.50]}"); /* byte for byte */
    CHECK_STR(back.blocks[2].args, "{\"text\": \"hi\"}");
    pt_buf_free(&b1);
    pt_buf_free(&b2);
}

static void test_auto_max_tokens(void)
{
    pt_ai_target t = target("anthropic/claude-haiku-4-5"); /* 200K window, 64K out */
    pt_ai_context c = conversation();
    CHECK(pt_ai_auto_max_tokens(&t, &c, 0) == 64000);
    CHECK(pt_ai_auto_max_tokens(&t, &c, 150000) == 200000 - 150000 - 4096);
    CHECK(pt_ai_auto_max_tokens(&t, &c, 199000) == 1);
    t = target("openrouter/some/model"); /* unknown max: omitted */
    CHECK(pt_ai_auto_max_tokens(&t, &c, 0) == 0);
}

int main(void)
{
    pt_arena_init(&A, T_AL, 0);
    RUN(test_resolve);
    RUN(test_catalog_sorted);
    RUN(test_keys);
    RUN(test_anthropic_request);
    RUN(test_anthropic_folding_and_bad_args);
    RUN(test_openai_request);
    RUN(test_responses_request);
    RUN(test_images);
    RUN(test_tool_ids);
    RUN(test_cross_provider_view);
    RUN(test_faux);
    RUN(test_abort);
    RUN(test_transport_error);
    RUN(test_msg_roundtrip);
    RUN(test_auto_max_tokens);
    pt_arena_free(&A);
    return t_done("test_ai");
}
