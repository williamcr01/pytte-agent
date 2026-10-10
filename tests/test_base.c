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

static void sha_hex(const char *in, size_t n, size_t split, char out[65])
{
    pt_sha256 s;
    unsigned char d[32];
    pt_sha256_init(&s);
    pt_sha256_update(&s, in, split);
    pt_sha256_update(&s, in + split, n - split);
    pt_sha256_final(&s, d);
    pt_hex(out, d, 32);
    out[64] = 0;
}

static void test_sha256(void)
{
    char h[65], big[1000];
    size_t i;
    sha_hex("", 0, 0, h);
    CHECK(strcmp(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);
    sha_hex("abc", 3, 1, h);
    CHECK(strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
    sha_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, 55, h);
    CHECK(strcmp(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0);
    memset(big, 'a', sizeof big);
    for (i = 0; i <= sizeof big; i += 37) { /* any split gives the same digest */
        char h2[65];
        sha_hex(big, sizeof big, i, h2);
        if (i == 0)
            memcpy(h, h2, sizeof h);
        CHECK(strcmp(h, h2) == 0);
    }
    CHECK(strcmp(h, "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3") == 0);
}

static bool schema_ok(const char *schema, const char *value, const char *want_err)
{
    pt_arena a;
    char err[200];
    bool ok;
    pt_arena_init(&a, T_AL, 0);
    ok = pt_schema_check(pt_json_parse(pt_strz(schema), &a, NULL), pt_json_parse(pt_strz(value), &a, NULL),
                         &a, err, sizeof err);
    if (want_err && strcmp(err, want_err) != 0) {
        fprintf(stderr, "schema error: \"%s\", want \"%s\"\n", err, want_err);
        ok = !ok;
    }
    pt_arena_free(&a);
    return ok;
}

static void test_schema(void)
{
    const char *s = "{\"type\":\"object\",\"properties\":{\"cmd\":{\"type\":\"string\",\"minLength\":1},"
                    "\"timeout\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":600},"
                    "\"mode\":{\"enum\":[\"a\",\"b\"]},\"tags\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}},"
                    "\"required\":[\"cmd\"],\"additionalProperties\":false}";
    CHECK(schema_ok(s, "{\"cmd\":\"ls\"}", ""));
    CHECK(schema_ok(s, "{\"cmd\":\"ls\",\"timeout\":5,\"mode\":\"b\",\"tags\":[\"x\"]}", ""));
    CHECK(!schema_ok(s, "{}", "$: missing required property cmd"));
    CHECK(!schema_ok(s, "[]", "$: expected object, got array"));
    CHECK(!schema_ok(s, "{\"cmd\":1}", "$.cmd: expected string, got number"));
    CHECK(!schema_ok(s, "{\"cmd\":\"\"}", "$.cmd: too short"));
    CHECK(!schema_ok(s, "{\"cmd\":\"x\",\"timeout\":0}", "$.timeout: must be at least 1"));
    CHECK(!schema_ok(s, "{\"cmd\":\"x\",\"timeout\":1.5}", "$.timeout: expected integer, got number"));
    CHECK(!schema_ok(s, "{\"cmd\":\"x\",\"mode\":\"c\"}", "$.mode: must be one of [\"a\",\"b\"]"));
    CHECK(!schema_ok(s, "{\"cmd\":\"x\",\"tags\":[\"y\",2]}", "$.tags[1]: expected string, got number"));
    CHECK(!schema_ok(s, "{\"cmd\":\"x\",\"other\":2}", "$: unknown property other"));
    CHECK(schema_ok("{\"type\":[\"string\",\"null\"]}", "null", ""));
    CHECK(schema_ok("true", "{\"anything\":1}", ""));
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
    RUN(test_sha256);
    RUN(test_schema);
    return t_done("test_base");
}
