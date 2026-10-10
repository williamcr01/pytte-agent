/* Model references: "provider/model-id" → target. */
#include "ai.h"

static uint8_t pick(uint8_t model, uint8_t provider, uint8_t api)
{
    return model ? model : provider ? provider : api;
}

static pt_compat merge(const pt_compat *m, const pt_compat *p, const pt_compat *d)
{
    pt_compat c;
    c.max_tokens_field = pick(m->max_tokens_field, p->max_tokens_field, d->max_tokens_field);
    c.system_role = pick(m->system_role, p->system_role, d->system_role);
    c.stream_usage = pick(m->stream_usage, p->stream_usage, d->stream_usage);
    c.reasoning_param = pick(m->reasoning_param, p->reasoning_param, d->reasoning_param);
    c.reasoning_delta = pick(m->reasoning_delta, p->reasoning_delta, d->reasoning_delta);
    c.mid_conversation_system =
        pick(m->mid_conversation_system, p->mid_conversation_system, d->mid_conversation_system);
    c.parallel_tool_calls = pick(m->parallel_tool_calls, p->parallel_tool_calls, d->parallel_tool_calls);
    c.fallbacks = pick(m->fallbacks, p->fallbacks, d->fallbacks);
    c.thinking_display = pick(m->thinking_display, p->thinking_display, d->thinking_display);
    return c;
}

bool pt_ai_resolve(const pt_ai_catalog *cat, pt_str ref, pt_ai_target *out, pt_arena *a,
                   const char **why)
{
    pt_str pid = ref, mid = {"", 0};
    long slash = pt_str_find(ref, '/');
    const pt_provider *p = NULL;
    const pt_model *m = NULL;
    const char *api;
    uint32_t i;
    static const pt_compat none;

    *why = NULL;
    if (slash >= 0) {
        pid.n = (size_t)slash;
        mid.p = ref.p + slash + 1;
        mid.n = ref.n - (size_t)slash - 1;
    }
    for (i = 0; i < cat->nproviders; i++)
        if (pt_str_eqz(pid, cat->providers[i].id))
            p = &cat->providers[i];
    if (!p) {
        *why = "unknown provider";
        return false;
    }
    if (!mid.n) {
        *why = "no model given: use provider/model-id";
        return false;
    }
    for (i = 0; i < cat->nmodels; i++)
        if (pt_str_eqz(pid, cat->models[i].provider) && pt_str_eqz(mid, cat->models[i].id))
            m = &cat->models[i];

    out->provider = p;
    out->known = m != NULL;
    if (m) {
        out->model = *m;
    } else {
        /* Not in the catalog: allowed, with the provider's defaults. */
        pt_model *d = &out->model;
        *d = (pt_model){0};
        d->provider = p->id;
        d->id = pt_arena_cstr(a, mid);
        d->name = d->id;
        d->context_window = p->context_window;
        d->max_tokens = p->max_tokens;
        d->reasoning = p->reasoning;
    }
    api = out->model.api ? out->model.api : p->api;
    out->api = pt_ai_api(pt_strz(api));
    if (!out->api) {
        *why = "unknown API";
        return false;
    }
    out->compat = merge(m ? &m->compat : &none, &p->compat, &out->api->defaults);
    return !a->oom;
}

bool pt_ai_retarget(pt_ai_target *t, pt_str api_id)
{
    const pt_api_vt *api = pt_ai_api(api_id);
    if (!api)
        return false;
    t->api = api;
    t->model.api = api->id;
    t->compat = merge(&t->model.compat, &t->provider->compat, &api->defaults);
    return true;
}
