/* The shared stream: response bytes → SSE → API → unified events, with the
 * partial message assembled here so APIs only translate wire events. */
#include "ai.h"

#include <string.h>

typedef struct sblock {
    pt_buf text; /* text, thinking, or tool call arguments */
    pt_buf sig;
    bool open;
} sblock;

struct pt_ai_stream {
    pt_ai_target t;
    pt_allocator al;
    pt_arena *a;      /* final message */
    pt_arena scratch; /* per SSE event */
    pt_arena own;     /* API state */
    pt_ai_on_event cb;
    void *ud;
    pt_msg msg;
    pt_block *blocks; /* grows with al until the end, then copied to a */
    sblock *sb;
    uint32_t cap, bcap; /* capacities of sb and blocks */
    pt_sse sse;
    void *api_state;
    int status;
    int64_t retry_after_ms;
    pt_buf errbody;
    bool started, finished;
    int64_t t0, now;
    pt_ai_error err;
};

static void emit(pt_ai_stream *s, pt_ev_kind kind, uint32_t index, pt_str delta)
{
    pt_ai_event ev;
    if (!s->cb)
        return;
    ev.kind = kind;
    ev.index = index;
    ev.delta = delta;
    ev.msg = &s->msg;
    s->cb(s->ud, &ev);
}

pt_ai_stream *pt_ai_stream_new(const pt_ai_target *t, pt_arena *a, pt_allocator al,
                               pt_ai_on_event on_event, void *ud, int64_t now_ms)
{
    pt_ai_stream *s = (pt_ai_stream *)al.fn(al.ud, NULL, 0, sizeof *s);
    if (!s)
        return NULL;
    memset(s, 0, sizeof *s);
    s->t = *t;
    s->al = al;
    s->a = a;
    s->cb = on_event;
    s->ud = ud;
    s->t0 = s->now = now_ms;
    s->retry_after_ms = -1;
    pt_arena_init(&s->scratch, al, 16384);
    pt_arena_init(&s->own, al, 4096);
    pt_sse_init(&s->sse, al);
    pt_buf_init(&s->errbody, al);
    s->msg.role = PT_ROLE_ASSISTANT;
    s->msg.api = pt_arena_dup(a, pt_strz(t->api->id));
    s->msg.provider = pt_arena_dup(a, pt_strz(t->provider->id));
    s->msg.model = pt_arena_dup(a, pt_strz(t->model.id));
    s->msg.timestamp = now_ms;
    s->msg.stop = PT_STOP_ERROR;
    if (t->api->state_size) {
        s->api_state = pt_arena_zalloc(&s->own, t->api->state_size);
        if (!s->api_state) {
            pt_ai_stream_free(s);
            return NULL;
        }
    }
    return s;
}

void pt_ai_stream_free(pt_ai_stream *s)
{
    uint32_t i;
    if (!s)
        return;
    for (i = 0; i < s->cap; i++) {
        pt_buf_free(&s->sb[i].text);
        pt_buf_free(&s->sb[i].sig);
    }
    if (s->sb)
        s->al.fn(s->al.ud, s->sb, sizeof(sblock) * s->cap, 0);
    if (s->blocks)
        s->al.fn(s->al.ud, s->blocks, sizeof(pt_block) * s->bcap, 0);
    pt_sse_free(&s->sse);
    pt_buf_free(&s->errbody);
    pt_arena_free(&s->scratch);
    pt_arena_free(&s->own);
    s->al.fn(s->al.ud, s, sizeof *s, 0);
}

/* ---- emit interface ---- */

pt_msg *pt_emit_msg(pt_ai_stream *s) { return &s->msg; }
const pt_ai_target *pt_emit_target(const pt_ai_stream *s) { return &s->t; }
pt_arena *pt_emit_arena(pt_ai_stream *s) { return s->a; }
pt_arena *pt_emit_scratch(pt_ai_stream *s) { return &s->scratch; }
pt_arena *pt_emit_own(pt_ai_stream *s) { return &s->own; }
bool pt_emit_started(const pt_ai_stream *s) { return s->started; }
bool pt_emit_finished(const pt_ai_stream *s) { return s->finished; }

void pt_emit_start(pt_ai_stream *s)
{
    if (s->started || s->finished)
        return;
    s->started = true;
    emit(s, PT_EV_START, 0, PT_STR_NULL);
}

static bool grow(pt_ai_stream *s)
{
    uint32_t ncap = s->cap ? s->cap * 2 : 8;
    pt_block *nb;
    sblock *ns;
    uint32_t i;
    if (s->bcap < ncap) {
        nb = (pt_block *)s->al.fn(s->al.ud, s->blocks, sizeof(pt_block) * s->bcap,
                                  sizeof(pt_block) * ncap);
        if (!nb)
            return false;
        s->blocks = nb;
        s->bcap = ncap;
        s->msg.blocks = nb;
    }
    nb = s->blocks;
    ns = (sblock *)s->al.fn(s->al.ud, s->sb, sizeof(sblock) * s->cap, sizeof(sblock) * ncap);
    if (!ns)
        return false;
    s->sb = ns;
    for (i = s->cap; i < ncap; i++) {
        memset(&nb[i], 0, sizeof nb[i]);
        memset(&ns[i], 0, sizeof ns[i]);
        pt_buf_init(&ns[i].text, s->al);
        pt_buf_init(&ns[i].sig, s->al);
    }
    s->cap = ncap;
    return true;
}

static pt_ev_kind start_kind(pt_block_kind k)
{
    return k == PT_BLOCK_THINKING ? PT_EV_THINKING_START
           : k == PT_BLOCK_TOOL_CALL ? PT_EV_TOOLCALL_START
                                     : PT_EV_TEXT_START;
}

long pt_emit_block(pt_ai_stream *s, pt_block_kind kind, pt_str id, pt_str name)
{
    uint32_t i;
    pt_block *b;
    if (s->finished)
        return -1;
    pt_emit_start(s);
    if (s->msg.nblocks == s->cap && !grow(s))
        return -1;
    i = s->msg.nblocks++;
    b = &s->blocks[i];
    memset(b, 0, sizeof *b);
    b->kind = kind;
    b->id = pt_arena_dup(s->a, id);
    b->name = pt_arena_dup(s->a, name);
    s->sb[i].open = true;
    pt_buf_reset(&s->sb[i].text);
    pt_buf_reset(&s->sb[i].sig);
    if (kind != PT_BLOCK_OPAQUE)
        emit(s, start_kind(kind), i, PT_STR_NULL);
    return (long)i;
}

bool pt_emit_is_open(const pt_ai_stream *s, long idx)
{
    return idx >= 0 && (uint32_t)idx < s->msg.nblocks && s->sb[idx].open;
}

/* Point the block's strings at the accumulation buffers (they may move). */
static void sync_block(pt_ai_stream *s, uint32_t i)
{
    pt_block *b = &s->blocks[i];
    if (b->kind == PT_BLOCK_TOOL_CALL)
        b->args = pt_buf_str(&s->sb[i].text);
    else if (b->kind != PT_BLOCK_OPAQUE)
        b->text = pt_buf_str(&s->sb[i].text);
    if (b->kind == PT_BLOCK_THINKING && !b->redacted)
        b->signature = pt_buf_str(&s->sb[i].sig);
}

void pt_emit_delta(pt_ai_stream *s, long idx, pt_str delta)
{
    pt_block *b;
    pt_ev_kind k;
    if (s->finished || !pt_emit_is_open(s, idx) || !delta.n)
        return;
    b = &s->blocks[idx];
    pt_buf_puts(&s->sb[idx].text, delta);
    sync_block(s, (uint32_t)idx);
    k = b->kind == PT_BLOCK_THINKING ? PT_EV_THINKING_DELTA
        : b->kind == PT_BLOCK_TOOL_CALL ? PT_EV_TOOLCALL_DELTA
                                        : PT_EV_TEXT_DELTA;
    emit(s, k, (uint32_t)idx, delta);
}

void pt_emit_signature(pt_ai_stream *s, long idx, pt_str sig)
{
    if (s->finished || !pt_emit_is_open(s, idx))
        return;
    pt_buf_puts(&s->sb[idx].sig, sig);
    sync_block(s, (uint32_t)idx);
}

void pt_emit_set_content(pt_ai_stream *s, long idx, pt_str content)
{
    if (s->finished || !pt_emit_is_open(s, idx))
        return;
    pt_buf_reset(&s->sb[idx].text);
    pt_buf_puts(&s->sb[idx].text, content);
    sync_block(s, (uint32_t)idx);
}

/* Copy a block's accumulated strings into the final arena. */
static void settle_block(pt_ai_stream *s, uint32_t i)
{
    pt_block *b = &s->blocks[i];
    sync_block(s, i);
    if (b->kind == PT_BLOCK_TOOL_CALL)
        b->args = pt_arena_dup(s->a, b->args);
    else if (b->kind != PT_BLOCK_OPAQUE)
        b->text = pt_arena_dup(s->a, b->text);
    if (b->kind == PT_BLOCK_THINKING && !b->redacted)
        b->signature = pt_arena_dup(s->a, b->signature);
    s->sb[i].open = false;
}

void pt_emit_end(pt_ai_stream *s, long idx)
{
    pt_block *b;
    pt_ev_kind k;
    if (s->finished || !pt_emit_is_open(s, idx))
        return;
    settle_block(s, (uint32_t)idx);
    b = &s->blocks[idx];
    k = b->kind == PT_BLOCK_THINKING ? PT_EV_THINKING_END
        : b->kind == PT_BLOCK_TOOL_CALL ? PT_EV_TOOLCALL_END
                                        : PT_EV_TEXT_END;
    if (b->kind != PT_BLOCK_OPAQUE)
        emit(s, k, (uint32_t)idx, PT_STR_NULL);
}

void pt_emit_end_all(pt_ai_stream *s)
{
    uint32_t i;
    for (i = 0; i < s->msg.nblocks; i++)
        pt_emit_end(s, (long)i);
}

void pt_emit_opaque(pt_ai_stream *s, pt_str raw)
{
    long i = pt_emit_block(s, PT_BLOCK_OPAQUE, PT_STR_NULL, PT_STR_NULL);
    if (i < 0)
        return;
    s->blocks[i].api = pt_arena_dup(s->a, pt_strz(s->t.api->id));
    s->blocks[i].raw = pt_arena_dup(s->a, raw);
    s->sb[i].open = false;
}

void pt_emit_redacted(pt_ai_stream *s, pt_str data)
{
    long i = pt_emit_block(s, PT_BLOCK_THINKING, PT_STR_NULL, PT_STR_NULL);
    if (i < 0)
        return;
    s->blocks[i].redacted = true;
    s->blocks[i].signature = pt_arena_dup(s->a, data);
    pt_emit_end(s, i);
}

/* Freeze the message into the final arena. Open blocks keep their partial
 * content without end events. */
static void settle(pt_ai_stream *s)
{
    uint32_t i;
    pt_block *final;
    for (i = 0; i < s->msg.nblocks; i++)
        if (s->sb[i].open)
            settle_block(s, i);
    final = (pt_block *)pt_arena_alloc(s->a, sizeof(pt_block) * (s->msg.nblocks ? s->msg.nblocks : 1));
    if (final && s->msg.nblocks)
        memcpy(final, s->blocks, sizeof(pt_block) * s->msg.nblocks);
    s->msg.blocks = final;
    if (!final)
        s->msg.nblocks = 0;
    s->msg.duration_ms = s->now - s->t0;
    s->finished = true;
}

void pt_emit_done(pt_ai_stream *s)
{
    if (s->finished)
        return;
    pt_emit_start(s);
    pt_emit_end_all(s);
    settle(s);
    s->err.cls = PT_ERR_NONE;
    emit(s, PT_EV_DONE, 0, PT_STR_NULL);
}

void pt_emit_fail(pt_ai_stream *s, pt_err_class cls, pt_str message)
{
    if (s->finished)
        return;
    settle(s);
    s->msg.stop = cls == PT_ERR_ABORTED ? PT_STOP_ABORTED : PT_STOP_ERROR;
    s->msg.error = pt_arena_dup(s->a, message);
    s->err.cls = cls;
    s->err.message = s->msg.error;
    s->err.status = s->status;
    s->err.retry_after_ms = s->retry_after_ms;
    emit(s, PT_EV_ERROR, 0, PT_STR_NULL);
}

/* ---- host side ---- */

void pt_ai_stream_head(pt_ai_stream *s, int status, pt_str retry_after)
{
    uint64_t secs;
    s->status = status;
    if (pt_str_to_u64(pt_str_trim(retry_after), &secs) && secs < (uint64_t)INT64_MAX / 1000)
        s->retry_after_ms = (int64_t)secs * 1000;
}

static bool ok_status(int status)
{
    return status >= 200 && status < 300;
}

static int on_sse(void *ud, const pt_sse_event *ev)
{
    pt_ai_stream *s = (pt_ai_stream *)ud;
    int r;
    pt_arena_reset(&s->scratch);
    r = s->t.api->feed(s, s->api_state, ev);
    return s->finished ? 1 : r;
}

void pt_ai_stream_body(pt_ai_stream *s, const char *p, size_t n, int64_t now_ms)
{
    if (s->finished)
        return;
    s->now = now_ms;
    if (!ok_status(s->status)) {
        pt_buf_put(&s->errbody, p, n);
        return;
    }
    pt_sse_feed(&s->sse, p, n, on_sse, s);
}

void pt_ai_stream_end(pt_ai_stream *s, const char *transport_error, int64_t now_ms)
{
    if (s->finished)
        return;
    s->now = now_ms;
    if (transport_error) {
        pt_buf msg;
        pt_buf_init(&msg, s->al);
        pt_buf_putz(&msg, "connection error: ");
        pt_buf_putz(&msg, transport_error);
        pt_emit_fail(s, PT_ERR_RETRYABLE, pt_buf_str(&msg));
        pt_buf_free(&msg);
        return;
    }
    if (!ok_status(s->status)) {
        pt_ai_error e;
        memset(&e, 0, sizeof e);
        s->t.api->classify_error(s->status, pt_buf_str(&s->errbody), &e, &s->scratch);
        pt_emit_fail(s, e.cls, e.message);
        return;
    }
    if (pt_sse_finish(&s->sse, on_sse, s) == 0 && !s->finished)
        s->t.api->finish(s, s->api_state);
    if (!s->finished)
        pt_emit_fail(s, PT_ERR_RETRYABLE, PT_S("stream ended before the response was complete"));
}

void pt_ai_stream_abort(pt_ai_stream *s, int64_t now_ms)
{
    if (s->finished)
        return;
    s->now = now_ms;
    pt_emit_fail(s, PT_ERR_ABORTED, PT_S("aborted"));
}

bool pt_ai_stream_finished(const pt_ai_stream *s)
{
    return s->finished;
}

const pt_msg *pt_ai_stream_message(const pt_ai_stream *s)
{
    return &s->msg;
}

const pt_ai_error *pt_ai_stream_error(const pt_ai_stream *s)
{
    return &s->err;
}
