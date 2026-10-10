/* Replays every response fixture under tests/fixtures/<provider>/ through
 * the stream, in pieces of several sizes, and checks:
 *   - every chunking gives the same events and the same final message;
 *   - the event stream follows the rules (start first, one terminal event);
 *   - the "expect" assertions in the fixture's header line;
 *   - the golden file <name>.expect.json (PT_UPDATE_FIXTURES=1 rewrites it).
 *
 * Fixture format: one JSON header line, then the raw response body.
 *   {"model": "anthropic/claude-opus-5-5", "status": 200, "retry_after": "",
 *    "expect": {"stop": "stop", "class": "none", "blocks": ["thinking", "text"]}}
 * An optional "api" names the API when it is not the provider's default. */
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>

#include "pytte_ai.h"
#include "t.h"

static char *read_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    char *p = NULL;
    long len;
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (len = ftell(f)) >= 0 && fseek(f, 0, SEEK_SET) == 0) {
        p = (char *)malloc((size_t)len + 1);
        if (p && fread(p, 1, (size_t)len, f) == (size_t)len) {
            p[len] = 0;
            *n = (size_t)len;
        } else {
            free(p);
            p = NULL;
        }
    }
    fclose(f);
    return p;
}

typedef struct run {
    pt_buf events; /* compact event log */
    int terminals, starts;
    bool bad_order;
    pt_ev_kind last;
    int repeat;
} run;

static void flush_repeat(run *r)
{
    if (r->repeat > 1) {
        pt_buf_putz(&r->events, " x");
        pt_buf_i64(&r->events, r->repeat);
    }
    r->repeat = 0;
}

static void on_event(void *ud, const pt_ai_event *ev)
{
    run *r = (run *)ud;
    if (r->terminals)
        r->bad_order = true; /* nothing may follow the terminal event */
    if (ev->kind == PT_EV_START)
        r->starts++;
    else if (ev->kind != PT_EV_ERROR && !r->starts)
        r->bad_order = true; /* everything but a bare error comes after start */
    if (ev->kind == PT_EV_DONE || ev->kind == PT_EV_ERROR)
        r->terminals++;
    if (r->repeat && ev->kind == r->last &&
        (ev->kind == PT_EV_TEXT_DELTA || ev->kind == PT_EV_THINKING_DELTA ||
         ev->kind == PT_EV_TOOLCALL_DELTA)) {
        r->repeat++;
        return;
    }
    flush_repeat(r);
    if (r->events.n)
        pt_buf_putc(&r->events, ' ');
    pt_buf_putz(&r->events, pt_ev_name(ev->kind));
    if (ev->kind != PT_EV_START && ev->kind != PT_EV_DONE && ev->kind != PT_EV_ERROR) {
        pt_buf_putc(&r->events, ':');
        pt_buf_u64(&r->events, ev->index);
    }
    r->last = ev->kind;
    r->repeat = 1;
}

/* Feed the body in pieces of `chunk` bytes (0 = all at once); return the
 * result as JSON: events, message, error. */
static void replay(const pt_ai_target *t, int status, pt_str retry_after, pt_str body, size_t chunk,
                   pt_buf *out, run *r)
{
    pt_arena a;
    pt_ai_stream *s;
    size_t i = 0;
    pt_jw w;
    const pt_ai_error *e;
    pt_arena_init(&a, T_AL, 0);
    memset(r, 0, sizeof *r);
    pt_buf_init(&r->events, T_AL);
    s = pt_ai_stream_new(t, &a, T_AL, on_event, r, 0);
    pt_ai_stream_head(s, status, retry_after);
    if (!chunk)
        chunk = body.n ? body.n : 1;
    while (i < body.n) {
        size_t n = body.n - i < chunk ? body.n - i : chunk;
        pt_ai_stream_body(s, body.p + i, n, 0);
        i += n;
    }
    pt_ai_stream_end(s, NULL, 0);
    flush_repeat(r);
    e = pt_ai_stream_error(s);
    pt_jw_init(&w, out);
    pt_jw_obj(&w);
    pt_jw_key(&w, "events");
    pt_jw_str(&w, pt_buf_str(&r->events));
    pt_jw_key(&w, "message");
    pt_msg_write_json(&w, pt_ai_stream_message(s));
    pt_jw_key(&w, "error");
    pt_jw_obj(&w);
    pt_jw_key(&w, "class");
    pt_jw_strz(&w, pt_err_class_name(e->cls));
    pt_jw_key(&w, "status");
    pt_jw_int(&w, e->status);
    pt_jw_key(&w, "retry_after_ms");
    pt_jw_int(&w, e->cls == PT_ERR_NONE ? -1 : e->retry_after_ms);
    pt_jw_obj_end(&w);
    pt_jw_obj_end(&w);
    pt_buf_putc(out, '\n');
    pt_ai_stream_free(s);
    pt_buf_free(&r->events);
    pt_arena_free(&a);
}

static void check_expect(const char *name, const pt_json *expect, pt_str result, pt_arena *a)
{
    pt_json *res = pt_json_parse(result, a, NULL);
    const pt_json *msg = pt_json_get(res, "message"), *blocks, *b, *want;
    pt_str v;
    uint32_t i = 0;
    if (!expect)
        return;
    v = pt_json_gets(expect, "stop", a);
    if (v.n && !pt_str_eq(v, pt_json_gets(msg, "stop", a))) {
        fprintf(stderr, "%s: stop %.*s, want %.*s\n", name, (int)pt_json_gets(msg, "stop", a).n,
                pt_json_gets(msg, "stop", a).p, (int)v.n, v.p);
        CHECK(0);
    }
    v = pt_json_gets(expect, "class", a);
    if (v.n && !pt_str_eq(v, pt_json_gets(pt_json_get(res, "error"), "class", a))) {
        fprintf(stderr, "%s: error class mismatch, want %.*s\n", name, (int)v.n, v.p);
        CHECK(0);
    }
    want = pt_json_get(expect, "blocks");
    if (want) {
        blocks = pt_json_get(msg, "content");
        CHECK(blocks && blocks->len == want->len);
        for (b = want->child; b; b = b->next, i++) {
            pt_str got = pt_json_gets(pt_json_at(blocks, i), "type", a);
            if (!pt_str_eq(got, pt_json_str(b, a))) {
                fprintf(stderr, "%s: block %u is %.*s\n", name, i, (int)got.n, got.p);
                CHECK(0);
            }
        }
    }
    /* every tool call's arguments must parse, unless the fixture says not */
    if (!pt_json_truthy(pt_json_get(expect, "broken_args"))) {
        blocks = pt_json_get(msg, "content");
        for (b = blocks ? blocks->child : NULL; b; b = b->next)
            if (pt_str_eqz(pt_json_gets(b, "type", a), "tool_call")) {
                pt_str args = pt_json_gets(b, "arguments", a);
                if (!pt_json_parse(args, a, NULL)) {
                    fprintf(stderr, "%s: tool call arguments do not parse: %.*s\n", name,
                            (int)args.n, args.p);
                    CHECK(0);
                }
            }
    }
}

static int fixtures_run, fixtures_updated;

static void run_fixture(const char *dir, const char *file)
{
    static const size_t chunks[] = {0, 1, 2, 3, 7, 64};
    char path[1024], gold[1024];
    size_t n, i, gn = 0, nl;
    char *data = read_file((snprintf(path, sizeof path, "%s/%s", dir, file), path), &n);
    char *golden;
    pt_arena a;
    pt_json *meta;
    pt_ai_target t;
    const char *why;
    pt_buf first;
    pt_str body;
    if (!data) {
        CHECK(0);
        return;
    }
    fixtures_run++;
    pt_arena_init(&a, T_AL, 0);
    for (nl = 0; nl < n && data[nl] != '\n'; nl++)
        ;
    meta = pt_json_parse((pt_str){data, nl}, &a, NULL);
    CHECK(meta != NULL);
    body.p = data + (nl < n ? nl + 1 : n);
    body.n = n - (size_t)(body.p - data);
    if (!meta || !pt_ai_resolve(pt_ai_builtin_catalog(), pt_json_gets(meta, "model", &a), &t, &a, &why)) {
        fprintf(stderr, "%s: bad header line\n", path);
        CHECK(0);
        free(data);
        pt_arena_free(&a);
        return;
    }
    if (pt_json_get(meta, "api") && !pt_ai_retarget(&t, pt_json_gets(meta, "api", &a))) {
        fprintf(stderr, "%s: unknown api\n", path);
        CHECK(0);
    }
    pt_buf_init(&first, T_AL);
    for (i = 0; i < sizeof chunks / sizeof chunks[0]; i++) {
        pt_buf out;
        run r;
        pt_buf_init(&out, T_AL);
        replay(&t, (int)pt_json_geti(meta, "status", 200), pt_json_gets(meta, "retry_after", &a), body,
               chunks[i], &out, &r);
        if (r.terminals != 1 || r.bad_order) {
            fprintf(stderr, "%s: bad event order (chunk %zu): %d terminal events\n", path, chunks[i],
                    r.terminals);
            CHECK(0);
        }
        if (i == 0) {
            pt_buf_puts(&first, pt_buf_str(&out));
        } else if (!pt_str_eq(pt_buf_str(&first), pt_buf_str(&out))) {
            fprintf(stderr, "%s: chunk size %zu changes the result:\n%.*s\nvs\n%.*s\n", path, chunks[i],
                    (int)first.n, first.p, (int)out.n, out.p);
            CHECK(0);
        }
        pt_buf_free(&out);
    }
    check_expect(path, pt_json_get(meta, "expect"), pt_buf_str(&first), &a);

    snprintf(gold, sizeof gold, "%s/%.*s.expect.json", dir, (int)(strlen(file) - 4), file);
    golden = read_file(gold, &gn);
    if (getenv("PT_UPDATE_FIXTURES")) {
        if (!golden || !pt_str_eq((pt_str){golden, gn}, pt_buf_str(&first))) {
            FILE *f = fopen(gold, "wb");
            CHECK(f != NULL);
            if (f) {
                fwrite(first.p, 1, first.n, f);
                fclose(f);
                fprintf(stderr, "    updated %s\n", gold);
                fixtures_updated++;
            }
        }
    } else if (!golden) {
        fprintf(stderr, "%s: missing golden file (make fixtures-update)\n", gold);
        CHECK(0);
    } else if (!pt_str_eq((pt_str){golden, gn}, pt_buf_str(&first))) {
        fprintf(stderr, "%s: result differs from the golden file:\n%.*s\n", path, (int)first.n, first.p);
        CHECK(0);
    }
    free(golden);
    pt_buf_free(&first);
    free(data);
    pt_arena_free(&a);
}

static int by_name(const void *x, const void *y)
{
    return strcmp(*(char *const *)x, *(char *const *)y);
}

static void run_dir(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    char *names[512];
    size_t n = 0, i;
    if (!d)
        return;
    while ((e = readdir(d)) && n < 512) {
        size_t l = strlen(e->d_name);
        if (l > 4 && strcmp(e->d_name + l - 4, ".sse") == 0)
            names[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, n, sizeof names[0], by_name);
    for (i = 0; i < n; i++) {
        fprintf(stderr, "  %s/%s\n", dir, names[i]);
        run_fixture(dir, names[i]);
        free(names[i]);
    }
}

int main(void)
{
    run_dir("tests/fixtures/anthropic");
    run_dir("tests/fixtures/openai");
    run_dir("tests/fixtures/openrouter");
    fprintf(stderr, "%d fixtures, %d golden files updated\n", fixtures_run, fixtures_updated);
    CHECK(fixtures_run > 0);
    return t_done("test_fixtures");
}
