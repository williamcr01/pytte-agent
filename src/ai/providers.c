/* Built-in providers (data) and the API registry. */
#include "ai.h"

/* The order of providers[]: what the model table's rows name. */
enum { PROV_ANTHROPIC, PROV_OPENAI, PROV_OPENROUTER, PROV_FAUX };

static const pt_provider providers[] = {
    {
        .id = "anthropic",
        .name = "Anthropic",
        .base_url = "https://api.anthropic.com",
        .api = "anthropic-messages",
        .key_env = {"ANTHROPIC_API_KEY", NULL},
        .auth = PT_AUTH_X_API_KEY,
        .compat = {.thinking_display = PT_YES},
        .context_window = 200000,
        .max_tokens = 64000,
        .reasoning = true,
    },
    {
        .id = "openai",
        .name = "OpenAI",
        .base_url = "https://api.openai.com/v1",
        .api = "openai-responses",
        .key_env = {"OPENAI_API_KEY", NULL},
        .auth = PT_AUTH_BEARER,
        /* thinking_display: reasoning summaries on the Responses API; the
         * rest applies when OpenAI is used through Chat Completions. */
        .compat = {.max_tokens_field = PT_MAXTOK_MAX_COMPLETION_TOKENS,
                   .reasoning_param = PT_RPARAM_EFFORT,
                   .reasoning_delta = PT_NO,
                   .thinking_display = PT_YES},
        .context_window = 128000,
        .reasoning = true,
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
        .context_window = 128000,
    },
    {
        .id = "faux",
        .name = "Faux (scripted, for tests)",
        .base_url = "faux://local",
        .api = "faux",
        .auth = PT_AUTH_NONE,
        .context_window = 100000,
        .max_tokens = 4096,
        .reasoning = true,
    },
};

/* The models come from scripts/gen_models.py (make models). A model that is
 * not listed still works, with its provider's defaults. */
static const pt_model_entry models[] = {
#include "models.gen.inc"
    {PROV_FAUX, "faux", "Faux", true, false, 100000, 4096, {0, 0, 0, 0}, {0}},
};

static const pt_ai_catalog builtin = {
    providers, sizeof providers / sizeof providers[0],
    models, sizeof models / sizeof models[0],
};

const pt_ai_catalog *pt_ai_builtin_catalog(void)
{
    return &builtin;
}

static const pt_api_vt *const apis[] = {&pt_api_anthropic, &pt_api_openai, &pt_api_openai_responses,
                                        &pt_api_faux};

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
