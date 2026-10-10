/* The core on its own: log lines, recovery, and runs driven by a tiny
 * in-memory host that answers HTTP effects from a faux script. */
#include "t.h"

#include "core/core.h"

#define CHECK_SAME(a, b) check_same((a), (b), __LINE__)
static void check_same(pt_str a, pt_str b, int line)
{
    t_checks++;
    if (!pt_str_eq(a, b)) {
        t_failed++;
        fprintf(stderr, "test_core.c:%d: differs:\n%.*s\n---\n%.*s\n", line, (int)a.n, a.p, (int)b.n, b.p);
    }
}

/* ---- a minimal host ---- */

typedef struct host {
    pt_agent *a;
    pt_buf log;
    pt_buf live[16]; /* by task ID */
    const char *script;
    pt_faux_state faux;
    int64_t now;
    uint32_t commits, requests;
    pt_buf last_request;
    pt_buf text; /* streamed text */
    int retries, done, failed, aborted, idle;
    pt_fx held;      /* the request h_run stopped at */
    uint32_t cancelled; /* the last PT_FX_CANCEL's target */
    uint32_t stop_at_request; /* return before answering request n (1-based) */
    pt_str body;              /* answer with this SSE body instead of the faux script */
    uint32_t snapshots;       /* live files written */
    pt_buf longest;           /* the longest one */
} host;

static int64_t h_now(void *ud)
{
    return ((host *)ud)->now += 7;
}

static void h_random(void *ud, void *out, size_t n)
{
    (void)ud;
    memset(out, 0x5a, n);
}

static void h_event(void *ud, const pt_event *ev)
{
    host *h = (host *)ud;
    switch (ev->kind) {
    case PT_E_STREAM:
        if (ev->ai->kind == PT_EV_TEXT_DELTA)
            pt_buf_puts(&h->text, ev->ai->delta);
        break;
    case PT_E_RETRY:
        h->retries++;
        break;
    case PT_E_RUN_DONE:
        if (pt_str_eqz(ev->outcome, "completed"))
            h->done++;
        else if (pt_str_eqz(ev->outcome, "aborted"))
            h->aborted++;
        else
            h->failed++;
        break;
    case PT_E_IDLE:
        h->idle++;
        break;
    default:
        break;
    }
}

static pt_agent *h_agent(host *h)
{
    pt_opts o;
    memset(&o, 0, sizeof o);
    o.alloc = T_AL;
    o.now_ms = h_now;
    o.random = h_random;
    o.on_event = h_event;
    o.ud = h;
    o.config.model = PT_S("faux/faux");
    o.config.live_interval_ms = 1;
    h->a = pt_new(&o);
    return h->a;
}

static void h_init(host *h, const char *script)
{
    int i;
    memset(h, 0, sizeof *h);
    pt_buf_init(&h->log, T_AL);
    for (i = 0; i < 16; i++)
        pt_buf_init(&h->live[i], T_AL);
    pt_buf_init(&h->last_request, T_AL);
    pt_buf_init(&h->text, T_AL);
    pt_buf_init(&h->longest, T_AL);
    h->script = script;
    h->now = 1760000000000;
}

static void h_free(host *h)
{
    int i;
    pt_free(h->a);
    pt_buf_free(&h->log);
    for (i = 0; i < 16; i++)
        pt_buf_free(&h->live[i]);
    pt_buf_free(&h->last_request);
    pt_buf_free(&h->text);
    pt_buf_free(&h->longest);
}

static void answer(host *h, const pt_fx *fx)
{
    pt_arena ar;
    pt_faux_response r;
    size_t at = 0;
    pt_arena_init(&ar, T_AL, 0);
    pt_buf_reset(&h->last_request);
    pt_buf_puts(&h->last_request, fx->http->body);
    if (h->body.n) {
        r.status = 200;
        r.body = h->body;
        r.cut = -1;
    } else {
        CHECK(pt_faux_respond(pt_strz(h->script), fx->http->body, &h->faux, &r, &ar));
    }
    pt_fx_head(h->a, fx->id, r.status, PT_STR_NULL);
    if (r.cut >= 0 && (size_t)r.cut < r.body.n)
        r.body.n = (size_t)r.cut;
    while (at < r.body.n) { /* small pieces, to cross SSE boundaries */
        size_t step = r.body.n - at < 5 ? r.body.n - at : 5;
        pt_fx_data(h->a, fx->id, r.body.p + at, step);
        at += step;
        h->now += 20; /* a slow server */
    }
    pt_fx_done(h->a, fx->id, NULL);
    pt_arena_free(&ar);
}

/* Run until idle or fatal; false when stopped early at stop_at_request. */
static bool h_run(host *h)
{
    pt_fx fx;
    int guard = 0;
    while (guard++ < 100000) {
        if (!pt_poll(h->a, &fx)) {
            if (pt_fatal(h->a) || pt_idle(h->a))
                return true;
            CHECK(!"stuck: nothing to do but not idle");
            return true;
        }
        switch (fx.kind) {
        case PT_FX_COMMIT:
            pt_buf_puts(&h->log, fx.bytes);
            h->commits++;
            pt_fx_done(h->a, fx.id, NULL);
            break;
        case PT_FX_HTTP:
            if (++h->requests == h->stop_at_request) {
                h->held = fx;
                return false;
            }
            answer(h, &fx);
            break;
        case PT_FX_SLEEP:
            if (h->now < fx.until)
                h->now = fx.until;
            pt_fx_done(h->a, fx.id, NULL);
            break;
        case PT_FX_LIVE:
            if (fx.task < 16) {
                pt_buf_reset(&h->live[fx.task]);
                pt_buf_puts(&h->live[fx.task], fx.bytes);
            }
            h->snapshots += fx.bytes.n > 0;
            if (fx.bytes.n > h->longest.n) {
                pt_buf_reset(&h->longest);
                pt_buf_puts(&h->longest, fx.bytes);
            }
            break;
        case PT_FX_CANCEL:
            h->cancelled = fx.cancel;
            break;
        default: /* no tools here */
            CHECK(!"unexpected effect");
            return true;
        }
    }
    CHECK(!"runaway loop");
    return true;
}

static void h_create(host *h)
{
    pt_session_cfg c;
    pt_identity me;
    memset(&c, 0, sizeof c);
    c.cwd = PT_S("/tmp/proj");
    c.host = PT_S("testhost");
    c.system = PT_S("You are a test.");
    memset(&me, 0, sizeof me);
    me.host = PT_S("testhost");
    me.pid = 42;
    h_agent(h);
    CHECK(pt_create(h->a, &c));
    CHECK(pt_open(h->a, &me, false));
}

/* A fresh agent from the log (as after a crash), with the live files. */
static pt_load_result h_reload(host *h)
{
    size_t start = 0, i;
    pt_load_result r = PT_LOAD_OK;
    uint32_t t;
    pt_free(h->a);
    h_agent(h);
    for (i = 0; i <= h->log.n && r == PT_LOAD_OK; i++) {
        bool nl = i < h->log.n && h->log.p[i] == '\n';
        if (!nl && i < h->log.n)
            continue;
        if (i == h->log.n && start == h->log.n)
            break;
        r = pt_load_line(h->a, (pt_str){h->log.p + start, i - start}, nl, i + 1 >= h->log.n);
        if (r == PT_LOAD_TORN)
            h->log.n = start; /* truncate */
        start = i + 1;
    }
    if (r == PT_LOAD_CORRUPT)
        return r;
    for (t = 0; t < 16; t++)
        if (h->live[t].n)
            pt_load_live(h->a, t, pt_buf_str(&h->live[t]));
    CHECK(pt_load_done(h->a));
    return PT_LOAD_OK; /* a torn tail was repaired */
}

static void h_open(host *h, const char *hostname, bool force, bool expect)
{
    pt_identity me;
    memset(&me, 0, sizeof me);
    me.host = pt_strz(hostname);
    me.pid = 43;
    CHECK(pt_open(h->a, &me, force) == expect);
}

static void h_submit(host *h, const char *text, const char *req)
{
    pt_input in;
    memset(&in, 0, sizeof in);
    in.text = pt_strz(text);
    in.req = req ? pt_strz(req) : PT_STR_NULL;
    CHECK(pt_submit(h->a, &in));
}

/* "kind:text" of each entry, for comparisons. */
static void transcript(const pt_agent *a, pt_buf *out)
{
    uint32_t i, j;
    pt_entry_view v;
    pt_buf_reset(out);
    for (i = 0; pt_entry_at(a, i, &v); i++) {
        pt_buf_putz(out, v.kind);
        if (v.msg && v.msg->role == PT_ROLE_ASSISTANT) {
            pt_buf_putc(out, '/');
            pt_buf_putz(out, pt_stop_name(v.msg->stop));
            for (j = 0; j < v.msg->nblocks; j++) {
                pt_buf_putc(out, ':');
                pt_buf_puts(out, v.msg->blocks[j].kind == PT_BLOCK_TOOL_CALL ? v.msg->blocks[j].name
                                                                             : v.msg->blocks[j].text);
            }
        } else if (v.msg) {
            pt_buf_putc(out, ':');
            pt_buf_puts(out, v.msg->text);
        }
        pt_buf_putc(out, '\n');
    }
}

static const char hello[] =
    "{\"turns\":[{\"content\":[{\"type\":\"thinking\",\"text\":\"Short.\"},"
    "{\"type\":\"text\",\"text\":\"Hello there, from faux!\"}]}]}";

/* ---- tests ---- */

static void test_crc(void)
{
    CHECK(pt_crc32(0, "123456789", 9) == 0xCBF43926u);
    CHECK(pt_crc32(pt_crc32(0, "1234", 4), "56789", 5) == 0xCBF43926u);
    CHECK(pt_crc32(0, "", 0) == 0);
}

static void test_log_line(void)
{
    pt_buf b;
    pt_jw w;
    pt_arena ar;
    uint64_t seq = 0;
    pt_str line;
    size_t i;
    pt_buf_init(&b, T_AL);
    pt_arena_init(&ar, T_AL, 0);
    pt_log_begin(&b, &w, 7, 1760000000123);
    pt_jw_raw(&w, PT_S("{\"t\":\"doc\",\"id\":3}"));
    pt_log_end(&b, &w);
    CHECK(b.p[b.n - 1] == '\n');
    line = (pt_str){b.p, b.n - 1};
    CHECK(pt_str_starts(line, PT_S("{\"seq\":7,\"ts\":1760000000123,\"w\":[{\"t\":\"doc\",\"id\":3}],\"crc\":\"")));
    CHECK(pt_log_parse(line, &seq, &ar) != NULL && seq == 7);
    /* any flipped byte is caught */
    for (i = 0; i < line.n; i++) {
        b.p[i] ^= 0x20;
        CHECK(pt_log_parse(line, &seq, &ar) == NULL);
        b.p[i] ^= 0x20;
    }
    /* any prefix is rejected */
    for (i = 0; i < line.n; i++)
        CHECK(pt_log_parse((pt_str){line.p, i}, &seq, &ar) == NULL);
    pt_buf_free(&b);
    pt_arena_free(&ar);
}

static void test_ask(void)
{
    host h;
    pt_buf before, after;
    pt_entry_view v;
    h_init(&h, hello);
    pt_buf_init(&before, T_AL);
    pt_buf_init(&after, T_AL);
    h_create(&h);
    h_submit(&h, "hi", NULL);
    h_run(&h);
    CHECK(!pt_fatal(h.a));
    CHECK(h.done == 1 && h.failed == 0 && h.idle == 1);
    CHECK_STR(pt_buf_str(&h.text), "Hello there, from faux!");
    transcript(h.a, &before);
    CHECK_STR(pt_buf_str(&before), "epoch:You are a test.\nuser:hi\nassistant/stop:Short.:Hello there, from faux!\n");
    /* session, claim, placement, request, answer */
    CHECK(h.commits == 5);
    CHECK(pt_entry_at(h.a, 2, &v) && pt_str_contains(v.raw, PT_S("\"model\":\"faux\"")));
    /* the live file was deleted once the answer was committed */
    CHECK(h.live[6].n == 0);

    /* recovery gives the same state, and the next IDs continue */
    CHECK(h_reload(&h) == PT_LOAD_OK);
    transcript(h.a, &after);
    CHECK_SAME(pt_buf_str(&after), pt_buf_str(&before));
    CHECK(h.a->next_id == 8);
    CHECK(pt_idle(h.a));
    h_free(&h);
    pt_buf_free(&before);
    pt_buf_free(&after);
}

static void test_torn_tail(void)
{
    host h;
    pt_buf full, want, got;
    size_t last_start, cut;
    pt_buf_init(&full, T_AL);
    pt_buf_init(&want, T_AL);
    pt_buf_init(&got, T_AL);
    h_init(&h, hello);
    h_create(&h);
    h_submit(&h, "hi", NULL);
    h_run(&h);
    pt_buf_puts(&full, pt_buf_str(&h.log));
    for (last_start = full.n - 1; last_start > 0 && full.p[last_start - 1] != '\n'; last_start--)
        ;
    /* the state without the last commit */
    pt_buf_reset(&h.log);
    pt_buf_put(&h.log, full.p, last_start);
    CHECK(h_reload(&h) == PT_LOAD_OK);
    transcript(h.a, &want);
    /* every cut inside the last line loads as that state */
    for (cut = last_start; cut < full.n; cut++) {
        pt_buf_reset(&h.log);
        pt_buf_put(&h.log, full.p, cut);
        CHECK(h_reload(&h) == PT_LOAD_OK);
        CHECK(h.log.n == last_start);
        transcript(h.a, &got);
        CHECK_SAME(pt_buf_str(&got), pt_buf_str(&want));
    }
    /* damage before the end is corruption, not a torn tail */
    pt_buf_reset(&h.log);
    pt_buf_puts(&h.log, pt_buf_str(&full));
    h.log.p[10] = 'X';
    CHECK(h_reload(&h) == PT_LOAD_CORRUPT);
    h_free(&h);
    pt_buf_free(&full);
    pt_buf_free(&want);
    pt_buf_free(&got);
}

static void test_retry(void)
{
    static const char script[] =
        "{\"turns\":[[{\"status\":529,\"body\":{\"class\":\"retryable\",\"message\":\"overloaded\"}},"
        "{\"content\":[{\"type\":\"text\",\"text\":\"cut off her\"}],\"cut\":60},"
        "{\"content\":[{\"type\":\"text\",\"text\":\"Third time lucky.\"}]}]]}";
    host h;
    pt_buf t;
    pt_buf_init(&t, T_AL);
    h_init(&h, script);
    h_create(&h);
    h_submit(&h, "go", NULL);
    h_run(&h);
    CHECK(h.retries == 2 && h.done == 1);
    transcript(h.a, &t);
    CHECK(pt_str_contains(pt_buf_str(&t), PT_S("error\nassistant/stop:Third time lucky.\n")));
    CHECK(h.now >= 1760000000000 + 750 + 1500); /* backoff slept */
    h_free(&h);
    pt_buf_free(&t);
}

static void test_tool_call_without_tools(void)
{
    static const char script[] =
        "{\"turns\":[{\"content\":[{\"type\":\"tool_call\",\"name\":\"echo\",\"args\":{\"text\":\"hi\"}}]},"
        "{\"content\":[{\"type\":\"text\",\"text\":\"No tools, then.\"}]}]}";
    host h;
    pt_buf t;
    pt_buf_init(&t, T_AL);
    h_init(&h, script);
    h_create(&h);
    h_submit(&h, "echo hi", NULL);
    h_run(&h);
    CHECK(h.done == 1);
    transcript(h.a, &t);
    CHECK(pt_str_contains(pt_buf_str(&t),
                          PT_S("assistant/tool_use:echo\ntool_result:Unknown tool: echo. No tools are available.")));
    CHECK(pt_str_contains(pt_buf_str(&t), PT_S("assistant/stop:No tools, then.\n")));
    h_free(&h);
    pt_buf_free(&t);
}

static void test_fatal_error(void)
{
    static const char script[] =
        "{\"turns\":[{\"status\":401,\"body\":{\"class\":\"fatal\",\"message\":\"bad key\"}}]}";
    host h;
    uint32_t i;
    h_init(&h, script);
    h_create(&h);
    h_submit(&h, "x", NULL);
    h_run(&h);
    CHECK(h.failed == 1 && h.done == 0 && pt_idle(h.a));
    for (i = 0; i < h.a->ninputs; i++)
        CHECK(h.a->inputs[i]->status == IN_FAILED);
    h_free(&h);
}

static void test_queue_and_req(void)
{
    static const char script[] = "{\"turns\":[{\"content\":[{\"type\":\"text\",\"text\":\"one\"}]},"
                                 "{\"content\":[{\"type\":\"text\",\"text\":\"two\"}]}]}";
    host h;
    pt_buf t;
    pt_buf_init(&t, T_AL);
    h_init(&h, script);
    h_create(&h);
    h_submit(&h, "first", "r1");
    h_submit(&h, "second", "r2");
    h_submit(&h, "first again", "r1"); /* same request ID: ignored */
    h_run(&h);
    CHECK(h.done == 2);
    transcript(h.a, &t);
    CHECK_STR(pt_buf_str(&t), "epoch:You are a test.\nuser:first\nassistant/stop:one\nuser:second\nassistant/stop:two\n");
    /* after recovery the request IDs are still known */
    CHECK(h_reload(&h) == PT_LOAD_OK);
    h_open(&h, "testhost", false, true);
    h_submit(&h, "again", "r2");
    h_run(&h);
    CHECK(h.done == 2);
    h_free(&h);
    pt_buf_free(&t);
}

static void test_resend_is_identical(void)
{
    host h;
    pt_buf first, t;
    pt_buf_init(&first, T_AL);
    pt_buf_init(&t, T_AL);
    h_init(&h, hello);
    h_create(&h);
    h_submit(&h, "hi", NULL);
    h.stop_at_request = 1;
    CHECK(!h_run(&h)); /* "crash" with the request in flight */
    /* fake a live file with a partial answer, as written mid-stream */
    pt_buf_putz(&h.live[6], "{\"role\":\"assistant\",\"provider\":\"faux\",\"content\":[{\"type\":\"text\","
                            "\"text\":\"Hel\"}],\"usage\":{\"input\":0,\"output\":0},\"stop\":\"error\"}");
    CHECK(h_reload(&h) == PT_LOAD_OK);
    h_open(&h, "testhost", false, true);
    h.stop_at_request = 0;
    h_run(&h);
    pt_buf_puts(&first, pt_buf_str(&h.last_request));
    CHECK(h.done == 1);
    transcript(h.a, &t);
    CHECK(pt_str_contains(pt_buf_str(&t), PT_S("user:hi\nassistant/aborted:Hel\nassistant/stop:Short.:Hello")));
    CHECK(h.live[6].n == 0);
    /* the re-sent request matches a request that never crashed */
    h_free(&h);
    h_init(&h, hello);
    h_create(&h);
    h_submit(&h, "hi", NULL);
    h_run(&h);
    CHECK_SAME(pt_buf_str(&first), pt_buf_str(&h.last_request));
    h_free(&h);
    pt_buf_free(&first);
    pt_buf_free(&t);
}

static bool fake_key(void *ud, const pt_provider *p, pt_credentials *out)
{
    (void)ud;
    (void)p;
    out->api_key = PT_S("sk-test");
    return true;
}

/* Live snapshots keep up with a real provider's stream. */
static void test_live_snapshots(void)
{
    host h;
    size_t n;
    char *sse;
    FILE *f = fopen("tests/fixtures/anthropic/live_text.sse", "rb");
    pt_config c;
    CHECK(f != NULL);
    if (!f)
        return;
    sse = (char *)malloc(1 << 16);
    n = fread(sse, 1, 1 << 16, f);
    fclose(f);
    h_init(&h, hello);
    h.body = pt_str_trim((pt_str){strchr(sse, '\n') + 1, n - (size_t)(strchr(sse, '\n') + 1 - sse)});
    h_create(&h);
    h.a->o.credentials = fake_key;
    memset(&c, 0, sizeof c);
    c.model = PT_S("anthropic/claude-haiku-5-5");
    pt_set_config(h.a, &c);
    h_submit(&h, "hi", NULL);
    h_run(&h);
    CHECK(h.done == 1);
    CHECK(pt_str_contains(pt_buf_str(&h.longest), PT_S("\"type\":\"text\",\"text\":\"")));
    h_free(&h);
    free(sse);
}

/* Feed the held request the first `upto` bytes of its faux answer. */
static void feed_held(host *h, size_t upto)
{
    pt_arena ar;
    pt_faux_response r;
    pt_arena_init(&ar, T_AL, 0);
    CHECK(pt_faux_respond(pt_strz(h->script), h->held.http->body, &h->faux, &r, &ar));
    pt_fx_head(h->a, h->held.id, r.status, PT_STR_NULL);
    pt_fx_data(h->a, h->held.id, r.body.p, upto < r.body.n ? upto : r.body.n);
    pt_arena_free(&ar);
}

static void test_abort(void)
{
    host h;
    pt_buf t, before;
    uint32_t i;
    pt_buf_init(&t, T_AL);
    pt_buf_init(&before, T_AL);
    h_init(&h, hello);
    h_create(&h);
    h_run(&h);
    CHECK(!pt_abort(h.a)); /* nothing is running */

    /* mid-stream: the partial answer is kept, marked aborted */
    h_submit(&h, "hi", NULL);
    h.stop_at_request = 1;
    CHECK(!h_run(&h));
    feed_held(&h, 330);
    CHECK(h.text.n > 0 && h.text.n < 23);
    CHECK(pt_abort(h.a));
    h_run(&h);
    CHECK(h.aborted == 1 && h.done == 0 && h.failed == 0 && pt_idle(h.a));
    CHECK(h.cancelled == h.held.id);
    transcript(h.a, &t);
    CHECK(pt_str_contains(pt_buf_str(&t), PT_S("user:hi\nassistant/aborted:Short.:Hel")));
    CHECK(h.a->tasks[0]->status == ST_DONE && pt_str_eqz(h.a->tasks[0]->outcome, "aborted"));
    for (i = 0; i < h.a->ninputs; i++)
        CHECK(h.a->inputs[i]->status == IN_ABORTED);
    for (i = 0; i < 16; i++)
        CHECK(h.live[i].n == 0);
    /* the rest of the cancelled response is ignored */
    pt_fx_data(h.a, h.held.id, "data: x\n\n", 9);
    pt_fx_done(h.a, h.held.id, NULL);
    CHECK(!pt_fatal(h.a) && pt_idle(h.a));
    /* the same after recovery */
    transcript(h.a, &before);
    CHECK(h_reload(&h) == PT_LOAD_OK);
    transcript(h.a, &t);
    CHECK_SAME(pt_buf_str(&t), pt_buf_str(&before));
    h_open(&h, "testhost", false, true);

    /* the next input runs; the aborted answer is not sent to the model */
    h.stop_at_request = 0;
    h_submit(&h, "again", NULL);
    h_run(&h);
    CHECK(h.done == 1);
    CHECK(!pt_str_contains(pt_buf_str(&h.last_request), PT_S("Hel\"")));
    CHECK(pt_str_contains(pt_buf_str(&h.last_request), PT_S("{\"role\":\"user\",\"text\":\"hi\"}")));

    /* an answer that has fully arrived is kept */
    h_submit(&h, "once more", NULL);
    h.stop_at_request = 3;
    CHECK(!h_run(&h));
    feed_held(&h, 100000);
    CHECK(pt_abort(h.a));
    h_run(&h);
    CHECK(h.done == 2 && h.aborted == 1);
    h_free(&h);
    pt_buf_free(&t);
    pt_buf_free(&before);
}

static void test_claims(void)
{
    host h;
    pt_buf log;
    pt_buf_init(&log, T_AL);
    h_init(&h, hello);
    h_create(&h);
    h_run(&h);
    /* the owner crashed: same host continues */
    CHECK(h_reload(&h) == PT_LOAD_OK);
    h_open(&h, "testhost", false, true);
    h_run(&h);
    /* another host is refused while the claim is open... */
    CHECK(h_reload(&h) == PT_LOAD_OK);
    h_open(&h, "otherhost", false, false);
    CHECK(pt_str_contains(pt_strz(pt_error(h.a)), PT_S("last opened on testhost (pid 43)")));
    /* ...unless forced, which records the takeover */
    h_open(&h, "otherhost", true, true);
    h_run(&h);
    CHECK(pt_str_contains(pt_buf_str(&h.log), PT_S("\"takeover\":\"5a5a5a5a5a5a5a5a\"")));
    /* after a clean close anyone may open */
    pt_close(h.a);
    h_run(&h);
    CHECK(pt_closed(h.a));
    CHECK(h_reload(&h) == PT_LOAD_OK);
    h_open(&h, "thirdhost", false, true);
    h_free(&h);
    pt_buf_free(&log);
}

int main(void)
{
    RUN(test_crc);
    RUN(test_log_line);
    RUN(test_ask);
    RUN(test_torn_tail);
    RUN(test_retry);
    RUN(test_tool_call_without_tools);
    RUN(test_fatal_error);
    RUN(test_queue_and_req);
    RUN(test_resend_is_identical);
    RUN(test_claims);
    RUN(test_abort);
    RUN(test_live_snapshots);
    return t_done("test_core");
}
