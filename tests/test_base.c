#include "t.h"

static void test_arena(void)
{
    pt_arena a;
    int i;
    char *big;
    pt_str d;
    pt_arena_init(&a, T_AL, 256);
    for (i = 0; i < 1000; i++) {
        char *p = (char *)pt_arena_alloc(&a, (size_t)(i % 37) + 1);
        CHECK(p != NULL);
        CHECK(((uintptr_t)p & 15) == 0);
        memset(p, 'x', (size_t)(i % 37) + 1);
    }
    big = (char *)pt_arena_alloc(&a, 10000);
    CHECK(big != NULL);
    memset(big, 1, 10000);
    d = pt_arena_dup(&a, PT_S("hello"));
    CHECK_STR(d, "hello");
    CHECK(d.p[5] == 0);
    pt_arena_reset(&a);
    CHECK(pt_arena_alloc(&a, 8) != NULL);
    CHECK(!a.oom);
    pt_arena_free(&a);
}

static void test_buf(void)
{
    pt_buf b;
    pt_buf_init(&b, T_AL);
    pt_buf_u64(&b, 0);
    pt_buf_putc(&b, ' ');
    pt_buf_i64(&b, -42);
    pt_buf_putc(&b, ' ');
    pt_buf_i64(&b, INT64_MIN);
    pt_buf_putc(&b, ' ');
    pt_buf_u64(&b, UINT64_MAX);
    CHECK_STR(pt_buf_str(&b), "0 -42 -9223372036854775808 18446744073709551615");
    pt_buf_free(&b);
}

static void test_str(void)
{
    uint64_t v;
    CHECK(pt_str_icontains(PT_S("Prompt Is Too Long: 5"), PT_S("prompt is too long")));
    CHECK(!pt_str_contains(PT_S("abc"), PT_S("abcd")));
    CHECK(pt_str_ieq(PT_S("Retry-After"), PT_S("retry-after")));
    CHECK_STR(pt_str_trim(PT_S("  x y \r\n")), "x y");
    CHECK(pt_str_to_u64(PT_S("18446744073709551615"), &v) && v == UINT64_MAX);
    CHECK(!pt_str_to_u64(PT_S("18446744073709551616"), &v));
    CHECK(!pt_str_to_u64(PT_S("12a"), &v));
    CHECK(!pt_str_to_u64(PT_S(""), &v));
}

static void test_json_parse(void)
{
    pt_arena a;
    pt_json_err err;
    const char *src = " {\"a\": [1, -2.5e3, true, false, null], \"b\\u0041\": {\"c\": \"x\\ny\"},"
                      " \"e\": \"\\ud83d\\ude00 \\ud800 caf\xC3\xA9\", \"n\": -9223372036854775808} ";
    pt_json *v;
    const pt_json *arr, *b;
    int64_t i;
    pt_arena_init(&a, T_AL, 0);
    v = pt_json_parse(pt_strz(src), &a, &err);
    CHECK(v != NULL);
    if (!v) {
        fprintf(stderr, "parse error at %zu: %s\n", err.pos, err.msg);
        pt_arena_free(&a);
        return;
    }
    CHECK(v->type == PT_J_OBJ && v->len == 4);
    arr = pt_json_get(v, "a");
    CHECK(pt_json_is(arr, PT_J_ARR) && arr->len == 5);
    CHECK(pt_json_int(pt_json_at(arr, 0), &i) && i == 1);
    CHECK(!pt_json_int(pt_json_at(arr, 1), &i));
    CHECK_STR(pt_json_at(arr, 1)->raw, "-2.5e3");
    CHECK(pt_json_at(arr, 2)->type == PT_J_TRUE);
    CHECK(pt_json_at(arr, 4)->type == PT_J_NULL);
    CHECK(pt_json_at(arr, 5) == NULL);
    CHECK_STR(arr->raw, "[1, -2.5e3, true, false, null]");
    b = pt_json_get(v, "bA");
    CHECK(b != NULL);
    CHECK_STR(pt_json_gets(b, "c", &a), "x\ny");
    CHECK_STR(pt_json_gets(v, "e", &a), "\xF0\x9F\x98\x80 \xEF\xBF\xBD caf\xC3\xA9");
    CHECK(pt_json_int(pt_json_get(v, "n"), &i) && i == INT64_MIN);
    CHECK(pt_json_geti(v, "missing", 7) == 7);
    pt_arena_free(&a);
}

static void test_json_invalid(void)
{
    static const char *bad[] = {
        "", " ", "{", "}", "[1,]", "{\"a\":1,}", "01", "1.", "-", ".5", "+1", "1e", "1e+",
        "\"\\x\"", "\"\x01\"", "\"\xC3\x28\"", "\"\xED\xA0\x80\"", "\"\xF4\x90\x80\x80\"",
        "\"\xC0\xAF\"", "nul", "tru", "[1] 2", "\"\\u12\"", "\"abc", "{\"a\" 1}", "{1:2}",
        "[1 2]", "/*c*/1", "NaN", "[\"a\",]", "{\"a\":}", "\"\\u00zz\"",
    };
    pt_arena a;
    size_t i;
    char deep[200];
    pt_arena_init(&a, T_AL, 0);
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        pt_json_err err;
        pt_json *v = pt_json_parse(pt_strz(bad[i]), &a, &err);
        if (v)
            fprintf(stderr, "accepted invalid JSON: %s\n", bad[i]);
        CHECK(v == NULL && err.msg != NULL);
    }
    /* depth: 64 nested arrays are fine, 65 are not */
    memset(deep, '[', 64);
    memset(deep + 64, ']', 64);
    CHECK(pt_json_parse((pt_str){deep, 128}, &a, NULL) != NULL);
    memset(deep, '[', 65);
    memset(deep + 65, ']', 65);
    CHECK(pt_json_parse((pt_str){deep, 130}, &a, NULL) == NULL);
    pt_arena_free(&a);
}

static void test_json_write(void)
{
    pt_buf b;
    pt_jw w;
    pt_arena a;
    pt_json *v;
    pt_buf_init(&b, T_AL);
    pt_arena_init(&a, T_AL, 0);
    pt_jw_init(&w, &b);
    pt_jw_obj(&w);
    pt_jw_key(&w, "s");
    pt_jw_str(&w, PT_S("q\"b\\n\n\t\x01 \xC3\xA9 \xFF end"));
    pt_jw_key(&w, "arr");
    pt_jw_arr(&w);
    pt_jw_int(&w, -1);
    pt_jw_uint(&w, 2);
    pt_jw_bool(&w, true);
    pt_jw_null(&w);
    pt_jw_obj(&w);
    pt_jw_obj_end(&w);
    pt_jw_arr(&w);
    pt_jw_arr_end(&w);
    pt_jw_arr_end(&w);
    pt_jw_key(&w, "raw");
    pt_jw_raw(&w, PT_S("{\"k\": [1, 2]}"));
    pt_jw_obj_end(&w);
    CHECK_STR(pt_buf_str(&b), "{\"s\":\"q\\\"b\\\\n\\n\\t\\u0001 \xC3\xA9 \xEF\xBF\xBD end\","
                              "\"arr\":[-1,2,true,null,{},[]],\"raw\":{\"k\": [1, 2]}}");
    v = pt_json_parse(pt_buf_str(&b), &a, NULL);
    CHECK(v != NULL);
    CHECK_STR(pt_json_gets(v, "s", &a), "q\"b\\n\n\t\x01 \xC3\xA9 \xEF\xBF\xBD end");
    pt_buf_free(&b);
    pt_arena_free(&a);
}

/* ---- SSE ---- */

typedef struct collected {
    pt_buf out;
    int n;
} collected;

static int collect(void *ud, const pt_sse_event *ev)
{
    collected *c = (collected *)ud;
    pt_buf_putc(&c->out, '[');
    pt_buf_puts(&c->out, ev->event);
    pt_buf_putc(&c->out, '|');
    pt_buf_puts(&c->out, ev->data);
    pt_buf_putc(&c->out, '|');
    pt_buf_puts(&c->out, ev->id);
    pt_buf_putc(&c->out, ']');
    c->n++;
    return 0;
}

static void sse_run(const char *in, size_t split, collected *c)
{
    pt_sse s;
    size_t n = strlen(in);
    pt_sse_init(&s, T_AL);
    pt_buf_init(&c->out, T_AL);
    c->n = 0;
    if (split > n)
        split = n;
    pt_sse_feed(&s, in, split, collect, c);
    pt_sse_feed(&s, in + split, n - split, collect, c);
    pt_sse_finish(&s, collect, c);
    pt_sse_free(&s);
}

static void test_sse(void)
{
    const char *in = ": comment\r\n"
                     "event: message_start\r\n"
                     "data: {\"a\":1}\r\n"
                     "\r\n"
                     "data:no-space\n"
                     "data:  two spaces\n"
                     "id: 7\n"
                     "\n"
                     "event: lonely\n"
                     "\n"
                     "event: cr\r"
                     "data: x\r"
                     "\r"
                     "retry: 10\n"
                     "data\n"
                     "\n"
                     "data: tail";
    const char *want = "[message_start|{\"a\":1}|][|no-space\n two spaces|7][cr|x|7][||7][|tail|7]";
    size_t split, n = strlen(in);
    for (split = 0; split <= n; split++) {
        collected c;
        sse_run(in, split, &c);
        if (!pt_str_eqz(pt_buf_str(&c.out), want))
            fprintf(stderr, "split %zu: %.*s\n", split, (int)c.out.n, c.out.p);
        CHECK(pt_str_eqz(pt_buf_str(&c.out), want));
        pt_buf_free(&c.out);
    }
}

int main(void)
{
    RUN(test_arena);
    RUN(test_buf);
    RUN(test_str);
    RUN(test_json_parse);
    RUN(test_json_invalid);
    RUN(test_json_write);
    RUN(test_sse);
    return t_done("test_base");
}
