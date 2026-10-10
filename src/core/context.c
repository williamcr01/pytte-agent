/* Transcript → unified context (step 1 of section 11.9). Pure: the same
 * entries always give the same context, so a re-sent request has the same
 * bytes as the original (prompt caching, preserved thinking). */
#include "core/core.h"

static bool sent(const entry *e)
{
    switch (e->kind) {
    case ENT_EPOCH:
    case ENT_USER:
    case ENT_TOOL_RESULT:
        return true;
    case ENT_ASSISTANT:
        return e->msg.stop != PT_STOP_ERROR && e->msg.stop != PT_STOP_ABORTED;
    case ENT_NOTE:
        return e->to_model;
    case ENT_ERROR:
        return false;
    }
    return false;
}

bool pt_core_context(pt_agent *a, uint32_t cutoff, pt_ai_context *out, pt_arena *ar)
{
    uint32_t i, n = 0;
    pt_msg *msgs = (pt_msg *)pt_arena_alloc(ar, sizeof(pt_msg) * (a->nentries ? a->nentries : 1));
    if (!msgs)
        return false;
    for (i = 0; i < a->nentries && a->entries[i].id <= cutoff; i++)
        if (sent(&a->entries[i]))
            msgs[n++] = a->entries[i].msg;
    out->msgs = msgs;
    out->nmsgs = n;
    out->tools = NULL; /* tools arrive with the tool task (M3) */
    out->ntools = 0;
    return true;
}
