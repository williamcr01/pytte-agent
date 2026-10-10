/* The TUI's parts on their own: text, the key decoder, Markdown, the input
 * box. The screen tests are in test_screen.c. */
#include "t.h"

#include "tui/tui.h"

static pt_buf tb; /* scratch */

static pt_str clean(const char *in, size_t n)
{
    int col = 0;
    pt_buf_reset(&tb);
    tui_clean(&tb, (pt_str){in, n}, &col);
    return pt_buf_str(&tb);
}

static int wb(const char *s)
{
    return tui_width(pt_strz(s), false);
}

static int wm(const char *s)
{
    return tui_width(pt_strz(s), true);
}

static void test_utf8(void)
{
    uint32_t cp;
    char enc[4];
    CHECK(tui_utf8_dec("A", 1, &cp) == 1 && cp == 'A');
    CHECK(tui_utf8_dec("\xc3\xa9", 2, &cp) == 2 && cp == 0xE9);
    CHECK(tui_utf8_dec("\xe2\x82\xac", 3, &cp) == 3 && cp == 0x20AC);
    CHECK(tui_utf8_dec("\xf0\x9f\x98\x80", 4, &cp) == 4 && cp == 0x1F600);
    /* invalid: maximal bad prefixes */
    CHECK(tui_utf8_dec("\x80", 1, &cp) == 1 && cp == TUI_REPLACEMENT);
    CHECK(tui_utf8_dec("\xc0\xaf", 2, &cp) == 1 && cp == TUI_REPLACEMENT); /* overlong */
    CHECK(tui_utf8_dec("\xe0\x80\x80", 3, &cp) == 1 && cp == TUI_REPLACEMENT);
    CHECK(tui_utf8_dec("\xed\xa0\x80", 3, &cp) == 1 && cp == TUI_REPLACEMENT); /* surrogate */
    CHECK(tui_utf8_dec("\xf4\x90\x80\x80", 4, &cp) == 1 && cp == TUI_REPLACEMENT);
    CHECK(tui_utf8_dec("\xe2\x82", 2, &cp) == 2 && cp == TUI_REPLACEMENT); /* truncated */
    CHECK(tui_utf8_dec("\xe2\x82x", 3, &cp) == 2 && cp == TUI_REPLACEMENT);
    CHECK(tui_utf8_enc(0x1F600, enc) == 4 && memcmp(enc, "\xf0\x9f\x98\x80", 4) == 0);
    CHECK(tui_utf8_enc(0xE9, enc) == 2 && memcmp(enc, "\xc3\xa9", 2) == 0);
}

static void test_widths(void)
{
    CHECK(tui_cp_best('a') == 1 && tui_cp_max('a') == 1);
    CHECK(tui_cp_best(0x4E2D) == 2); /* 中 */
    CHECK(tui_cp_best(0x0301) == 0 && tui_cp_max(0x0301) == 0); /* combining acute */
    CHECK(tui_cp_best(0x1F600) == 2); /* 😀 */
    CHECK(tui_cp_best(0x00B1) == 1 && tui_cp_max(0x00B1) == 2); /* ± is ambiguous */
    CHECK(tui_cp_best(0x2764) == 1 && tui_cp_max(0x2764) == 2); /* ❤ text by default */
    CHECK(tui_cp_best(0x200D) == 0);                           /* ZWJ */
    CHECK(tui_cp_best(0xFE0F) == 0);
    CHECK(tui_cp_best(0x1160) == 0); /* hangul medial vowel */
    CHECK(tui_cp_best(0x3400) == 2 && tui_cp_best(0x2FFFD) == 2);
    CHECK(tui_cp_max(0x10FFFD) == 2); /* private use is ambiguous */

    CHECK(wb("hello") == 5 && wm("hello") == 5);
    CHECK(wb("中文") == 4);
    CHECK(wb("e\xcc\x81") == 1);                                /* e + combining acute */
    CHECK(wb("\xe2\x9d\xa4\xef\xb8\x8f") == 2);                 /* ❤️ with VS16 */
    CHECK(wb("\xf0\x9f\x87\xb8\xf0\x9f\x87\xaa") == 2);         /* 🇸🇪 */
    CHECK(wm("\xf0\x9f\x87\xb8\xf0\x9f\x87\xaa") == 4);
    /* 👨‍👩‍👧: one cluster, best 2, max the sum of its parts */
    CHECK(wb("\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7") == 2);
    CHECK(wm("\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7") == 6);
    CHECK(wb("\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd") == 2); /* 👍🏽 */
}

static void test_clusters(void)
{
    pt_str s = pt_strz("a\xcc\x81" "b\xf0\x9f\x87\xb8\xf0\x9f\x87\xaa\xf0\x9f\x87\xb8"
                       "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\n" "c");
    size_t i = 0, b[16];
    int n = 0;
    while (i < s.n && n < 16) {
        b[n++] = i;
        i = tui_next_cluster(s, i);
    }
    /* a+acute, b, a flag pair, a lone RI, man ZWJ woman, \n, c */
    CHECK(n == 7);
    CHECK(b[1] == 3 && b[2] == 4 && b[3] == 12 && b[4] == 16 && b[5] == 27 && b[6] == 28);
    while (n-- > 0) {
        CHECK(tui_prev_cluster(s, i) == b[n]);
        i = b[n];
    }
}

static void test_clean(void)
{
    static const char hostile[] =
        "ok\x1b]52;c;ZXZpbA==\x07 \x1b[2J\x9b" "31m\xc2\x9b" "x\x7f\r\n"
        "\xe2\x80\xae" "evil\xe2\x81\xa6" "\xff\xfe" "end\x00!";
    pt_str c = clean(hostile, sizeof hostile - 1);
    size_t i;
    for (i = 0; i < c.n; i++) {
        unsigned char ch = (unsigned char)c.p[i];
        CHECK(ch >= 0x20 || ch == '\n');
        CHECK(ch != 0x7F);
    }
    CHECK_STR(c, "ok]52;c;ZXZpbA== [2J\xef\xbf\xbd" "31mx\nevil\xef\xbf\xbd\xef\xbf\xbd" "end!");
    CHECK_STR(clean("a\tb\tc", 5), "a   b   c");
    CHECK_STR(clean("abcd\tx\n\ty", 10), "abcd    x\n    y");
    CHECK_STR(clean("one\rtwo\r\n", 9), "onetwo\n");
    CHECK_STR(clean("\xe4\xb8\xad\tx", 5), "\xe4\xb8\xad  x"); /* 中 is two columns */
}

static void test_styled(void)
{
    pt_buf s, out;
    pt_buf_init(&s, T_AL);
    pt_buf_init(&out, T_AL);
    pt_buf_putz(&s, "plain ");
    tui_style(&s, TUI_BOLD);
    pt_buf_putz(&s, "bold");
    tui_style(&s, 0);
    CHECK(tui_width(pt_buf_str(&s), false) == 10);
    CHECK(tui_style_at(pt_buf_str(&s), 0) == 0);
    CHECK(tui_style_at((pt_str){s.p, 8}, 0) == TUI_BOLD);
    tui_sgr(&out, pt_buf_str(&s), true);
    CHECK_STR(pt_buf_str(&out), "plain \x1b[0;1mbold\x1b[0m");
    pt_buf_reset(&s);
    tui_style(&s, TUI_DIM | TUI_RED);
    pt_buf_putz(&s, "x");
    pt_buf_reset(&out);
    tui_sgr(&out, pt_buf_str(&s), true);
    CHECK_STR(pt_buf_str(&out), "\x1b[0;2;31mx");
    pt_buf_reset(&out);
    tui_sgr(&out, pt_buf_str(&s), false);
    CHECK_STR(pt_buf_str(&out), "\x1b[0;2mx");
    pt_buf_free(&s);
    pt_buf_free(&out);
}

static pt_str wrap(const char *s, int cols, int indent, bool words)
{
    pt_buf_reset(&tb);
    tui_wrap(&tb, pt_strz(s), cols, indent, words, false);
    return pt_buf_str(&tb);
}

static void test_wrap(void)
{
    pt_buf s;
    CHECK_STR(wrap("", 10, 0, true), "\n");
    CHECK_STR(wrap("short", 10, 0, true), "short\n");
    CHECK_STR(wrap("the quick brown fox jumps", 10, 0, true), "the quick\nbrown fox\njumps\n");
    CHECK_STR(wrap("the quick brown fox", 10, 2, true), "the quick\n  brown\n  fox\n");
    CHECK_STR(wrap("abcdefghijkl", 5, 0, true), "abcde\nfghij\nkl\n");
    CHECK_STR(wrap("abcdefghijkl", 5, 0, false), "abcde\nfghij\nkl\n");
    CHECK_STR(wrap("ab cd", 2, 0, true), "ab\ncd\n");
    CHECK_STR(wrap("  indented text", 9, 0, true), "  indente\nd text\n"); /* never breaks in the indent */
    CHECK_STR(wrap("  two words", 7, 0, true), "  two\nwords\n");
    CHECK_STR(wrap("\xe4\xb8\xad\xe6\x96\x87\xe4\xb8\xad", 5, 0, false), "\xe4\xb8\xad\xe6\x96\x87\n\xe4\xb8\xad\n");
    CHECK_STR(wrap("\xe4\xb8\xad", 1, 0, false), "\xe4\xb8\xad\n"); /* too wide: one cluster anyway */
    CHECK_STR(wrap("word  ", 4, 0, true), "word\n");
    /* the style carries over to the next row */
    pt_buf_init(&s, T_AL);
    pt_buf_putz(&s, "a ");
    tui_style(&s, TUI_BOLD);
    pt_buf_putz(&s, "bold words here");
    pt_buf_reset(&tb);
    CHECK(tui_wrap(&tb, pt_buf_str(&s), 8, 0, true, false) == 3);
    CHECK_STR(pt_buf_str(&tb), "a \x01!bold\n\x01!words\n\x01!here\n");
    pt_buf_free(&s);
    CHECK(tui_fit(pt_strz("\xe4\xb8\xad\xe6\x96\x87"), 3, false) == 3);
    CHECK(tui_fit(pt_strz("abc"), 10, false) == 3);
}

int main(void)
{
    pt_buf_init(&tb, T_AL);
    RUN(test_utf8);
    RUN(test_widths);
    RUN(test_clusters);
    RUN(test_clean);
    RUN(test_styled);
    RUN(test_wrap);
    pt_buf_free(&tb);
    return t_done("test_tui");
}
