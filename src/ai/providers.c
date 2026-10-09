/* Built-in providers (data) and the API registry. */
#include "ai.h"

static const pt_provider providers[] = {
    {
        .id = "anthropic",
        .name = "Anthropic",
        .base_url = "https://api.anthropic.com",
        .api = "anthropic-messages",
        .key_env = {"ANTHROPIC_API_KEY", NULL},
        .auth = PT_AUTH_X_API_KEY,
        .compat = {.thinking_display = PT_YES},
        .default_model = "claude-opus-5-5",
        .context_window = 200000,
        .max_tokens = 64000,
        .reasoning = true,
    },
    {
        .id = "openai",
        .name = "OpenAI",
        .base_url = "https://api.openai.com/v1",
        .api = "openai-completions",
        .key_env = {"OPENAI_API_KEY", NULL},
        .auth = PT_AUTH_BEARER,
        .compat = {.max_tokens_field = PT_MAXTOK_MAX_COMPLETION_TOKENS,
                   .reasoning_param = PT_RPARAM_EFFORT,
                   .reasoning_delta = PT_NO},
        .default_model = "gpt-5",
        .context_window = 128000,
    },
    {
        .id = "openrouter",
        .name = "OpenRouter",
        .base_url = "https://openrouter.ai/api/v1",
        .api = "openai-completions",
        .key_env = {"OPENROUTER_API_KEY", NULL},
        .auth = PT_AUTH_BEARER,
        .headers = {{"X-Title", "pytte"}},
        .compat = {.max_tokens_field = PT_MAXTOK_MAX_TOKENS,
                   .reasoning_param = PT_RPARAM_OPENROUTER,
                   .reasoning_delta = PT_YES},
        .default_model = "openai/gpt-5-mini",
        .context_window = 128000,
    },
    {
        .id = "faux",
        .name = "Faux (scripted, for tests)",
        .base_url = "faux://local",
        .api = "faux",
        .auth = PT_AUTH_NONE,
        .default_model = "faux",
        .context_window = 100000,
        .max_tokens = 4096,
        .reasoning = true,
    },
};

/* Context window, max output, reasoning, images; prices where known, in
 * micro-dollars per million tokens (input, output, cache read, cache write). */
static const pt_model models[] = {
    {"anthropic", "claude-opus-5-5", "Claude Opus 5.5", NULL, 1000000, 128000, true, true,
     {4000000, 20000000, 200000, 5000000},
     {.mid_conversation_system = PT_YES, .fallbacks = PT_YES}},
    {"anthropic", "claude-opus-5", "Claude Opus 5", NULL, 1000000, 128000, true, true,
     {5000000, 25000000, 500000, 6250000},
     {.mid_conversation_system = PT_YES, .fallbacks = PT_YES}},
    {"anthropic", "claude-fable-5-1", "Claude Fable 5.1", NULL, 1000000, 128000, true, true,
     {10000000, 50000000, 250000, 12500000},
     {.mid_conversation_system = PT_YES}},
    {"anthropic", "claude-sonnet-5-5", "Claude Sonnet 5.5", NULL, 1000000, 128000, true, true,
     {0, 0, 0, 0}, {.mid_conversation_system = PT_YES}},
    {"anthropic", "claude-sonnet-5", "Claude Sonnet 5", NULL, 1000000, 128000, true, true,
     {0, 0, 0, 0}, {0}},
    {"anthropic", "claude-haiku-5-5", "Claude Haiku 5.5", NULL, 1000000, 128000, true, true,
     {0, 0, 0, 0}, {0}},
    {"anthropic", "claude-opus-4-8", "Claude Opus 4.8", NULL, 1000000, 128000, true, true,
     {5000000, 25000000, 500000, 6250000},
     {.mid_conversation_system = PT_YES}},
    {"anthropic", "claude-haiku-4-5", "Claude Haiku 4.5", NULL, 200000, 64000, false, true,
     {0, 0, 0, 0}, {0}},

    {"openai", "gpt-5", "GPT-5", NULL, 400000, 128000, true, true, {0, 0, 0, 0}, {0}},
    {"openai", "gpt-5-mini", "GPT-5 mini", NULL, 400000, 128000, true, true, {0, 0, 0, 0}, {0}},
    {"openai", "gpt-4.1", "GPT-4.1", NULL, 1047576, 32768, false, true, {0, 0, 0, 0}, {0}},
    {"openai", "gpt-4o-mini", "GPT-4o mini", NULL, 128000, 16384, false, true, {0, 0, 0, 0}, {0}},

    {"faux", "faux", "Faux", NULL, 100000, 4096, true, false, {0, 0, 0, 0}, {0}},
};

static const pt_ai_catalog builtin = {
    providers, sizeof providers / sizeof providers[0],
    models, sizeof models / sizeof models[0],
};

const pt_ai_catalog *pt_ai_builtin_catalog(void)
{
    return &builtin;
}

static const pt_api_vt *const apis[] = {&pt_api_anthropic, &pt_api_openai, &pt_api_faux};

const pt_api_vt *pt_ai_api(pt_str id)
{
    size_t i;
    for (i = 0; i < sizeof apis / sizeof apis[0]; i++)
        if (pt_str_eqz(id, apis[i]->id))
            return apis[i];
    return NULL;
}

const char *pt_ai_api_id(const pt_api_vt *api)
{
    return api->id;
}
