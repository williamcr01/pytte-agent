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

/* The tool results after an assistant entry with calls, in call order
 * (section 9.2): parallel calls may finish in any order, and the request
 * bytes must not depend on it. Results are the entries that follow it up
 * to the next user or assistant entry. */
static uint32_t add_results(pt_agent *a, uint32_t i, uint32_t cutoff, pt_msg *msgs, uint32_t n)
{
    const pt_msg *am = &a->entries[i].msg;
    uint32_t b, j, end = i + 1;
    while (end < a->nentries && a->entries[end].id <= cutoff && a->entries[end].kind != ENT_USER &&
           a->entries[end].kind != ENT_ASSISTANT && a->entries[end].kind != ENT_EPOCH)
        end++;
    for (b = 0; b < am->nblocks; b++) {
        if (am->blocks[b].kind != PT_BLOCK_TOOL_CALL)
            continue;
        for (j = i + 1; j < end; j++)
            if (a->entries[j].kind == ENT_TOOL_RESULT &&
                pt_str_eq(a->entries[j].msg.call_id, am->blocks[b].id)) {
                msgs[n++] = a->entries[j].msg;
                break;
            }
    }
    return n;
}

bool pt_core_context(pt_agent *a, uint32_t cutoff, pt_ai_context *out, pt_arena *ar)
{
    uint32_t i, n = 0;
    pt_msg *msgs = (pt_msg *)pt_arena_alloc(ar, sizeof(pt_msg) * (a->nentries ? a->nentries : 1));
    if (!msgs)
        return false;
    for (i = 0; i < a->nentries && a->entries[i].id <= cutoff; i++) {
        const entry *e = &a->entries[i];
        if (e->kind == ENT_TOOL_RESULT || !sent(e))
            continue; /* results are placed after their calls */
        msgs[n++] = e->msg;
        if (e->kind == ENT_ASSISTANT)
            n = add_results(a, i, cutoff, msgs, n);
    }
    out->msgs = msgs;
    out->nmsgs = n;
    out->tools = a->tools;
    out->ntools = a->ntools;
    return true;
}
