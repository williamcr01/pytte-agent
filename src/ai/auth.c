/* API key resolution. The host supplies the environment lookup. */
#include "ai.h"

bool pt_ai_find_key(const pt_provider *p, pt_env_fn env, void *ud, pt_credentials *out,
                    const char **which)
{
    int i;
    out->api_key = PT_STR_NULL;
    *which = NULL;
    if (p->auth == PT_AUTH_NONE)
        return true;
    for (i = 0; i < 4 && p->key_env[i]; i++) {
        const char *v = env ? env(ud, p->key_env[i]) : NULL;
        if (v && *v) {
            out->api_key = pt_strz(v);
            *which = p->key_env[i];
            return true;
        }
    }
    return false;
}
