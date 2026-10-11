/* Transcript → unified context (step 1 of section 11.9). Pure: the same
 * entries always give the same context, so a re-sent request has the same
 * bytes as the original (prompt caching, preserved thinking). */
#include "core/core.h"

#include <string.h>

bool pt_core_sent(const entry *e)
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

/* About what e adds to a request: its content, not its log line (which
 * also holds ids, usage and JSON escapes). */
uint64_t pt_core_sent_bytes(const pt_agent *a, const entry *e)
{
    const pt_msg *m = &e->msg;
    uint64_t n = 16 + m->text.n + e->summary.n + e->files.n + e->request.n;
    uint32_t i;
    for (i = 0; i < m->nblocks; i++)
        n += 16 + m->blocks[i].text.n + m->blocks[i].args.n + m->blocks[i].signature.n / 4;
    if (e->kind == ENT_EPOCH)
        for (i = 0; i < a->ntools; i++)
            n += 32 + a->tools[i].name.n + a->tools[i].description.n + a->tools[i].schema.n;
    return n;
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

/* After a compaction, the summary opens the epoch as user content (section
 * 12.3), followed by the request that was being worked on, or by the
 * newest turns as they were. */
static const char intro[] =
    "This session was compacted to fit the context window. A summary of the conversation so far:\n\n";
static const char intro_kept[] = "This session was compacted to fit the context window. A summary of the "
                                 "conversation before the messages that follow:\n\n";
static const char working[] = "\n\nThe user's message being worked on when the session was compacted:\n\n";

static bool summary_msg(const entry *e, bool kept, pt_msg *m, pt_arena *ar)
{
    pt_str in = kept ? PT_S(intro_kept) : PT_S(intro);
    size_t n = in.n + e->summary.n + (e->files.n ? 2 + e->files.n : 0) +
               (e->request.n ? sizeof working - 1 + e->request.n : 0);
    char *p = (char *)pt_arena_alloc(ar, n ? n : 1), *q = p;
    if (!p)
        return false;
    memcpy(q, in.p, in.n);
    q += in.n;
    memcpy(q, e->summary.p, e->summary.n);
    q += e->summary.n;
    if (e->files.n) {
        memcpy(q, "\n\n", 2);
        memcpy(q + 2, e->files.p, e->files.n);
        q += 2 + e->files.n;
    }
    if (e->request.n) {
        memcpy(q, working, sizeof working - 1);
        q += sizeof working - 1;
        memcpy(q, e->request.p, e->request.n);
    }
    memset(m, 0, sizeof *m);
    m->role = PT_ROLE_USER;
    m->text = (pt_str){p, n};
    return true;
}

bool pt_core_context(pt_agent *a, uint32_t cutoff, pt_ai_context *out, pt_arena *ar)
{
    uint32_t i, n = 0;
    pt_msg *msgs = (pt_msg *)pt_arena_alloc(ar, sizeof(pt_msg) * (a->nentries + 1));
    if (!msgs)
        return false;
    for (i = 0; i < a->nentries && a->entries[i].id <= cutoff; i++) {
        const entry *e = &a->entries[i];
        if (e->kind == ENT_TOOL_RESULT || !pt_core_sent(e))
            continue; /* results are placed after their calls */
        msgs[n++] = e->msg;
        if (e->kind == ENT_EPOCH && e->summary.n &&
            !summary_msg(e, i + 1 < a->nentries && a->entries[i + 1].kept, &msgs[n++], ar))
            return false;
        if (e->kind == ENT_ASSISTANT)
            n = add_results(a, i, cutoff, msgs, n);
    }
    out->msgs = msgs;
    out->nmsgs = n;
    out->tools = a->tools;
    out->ntools = a->ntools;
    return true;
}
