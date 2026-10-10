/* The TUI's parts on their own: text, the key decoder, Markdown, the input
 * box. The screen tests are in test_screen.c. */
#include "t.h"

#include "cli/cli.h"
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
    CHECK(tui_cp_best(0x00B1) == 1 && tui_cp_max(0x00B1) == 1); /* ± is ambiguous: narrow... */
    tui_set_ambiguous_wide(true);
    CHECK(tui_cp_best(0x00B1) == 2 && tui_cp_max(0x00B1) == 2); /* ...unless set wide */
    CHECK(tui_cp_best('a') == 1 && tui_cp_best(0x0301) == 0);
    tui_set_ambiguous_wide(false);
    CHECK(tui_cp_best(0x2764) == 1 && tui_cp_max(0x2764) == 2); /* ❤ text by default */
    CHECK(tui_cp_best(0x200D) == 0);                           /* ZWJ */
    CHECK(tui_cp_best(0xFE0F) == 0);
    CHECK(tui_cp_best(0x1160) == 0); /* hangul medial vowel */
    CHECK(tui_cp_best(0x3400) == 2 && tui_cp_best(0x2FFFD) == 2);
    CHECK(tui_cp_max(0x10FFFD) == 1); /* private use is ambiguous */

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
    s = PT_S("a\xe2\x80\x8d" "bc\n");
    CHECK(tui_prev_cluster(s, 5) == 0); /* ZWJ can join an ASCII base. */
    CHECK(tui_prev_cluster(s, 6) == 5);
    CHECK(tui_prev_cluster(s, 7) == 6);
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

/* ---- keys ---- */

static void describe(pt_buf *out, const tui_ev *ev)
{
    char tmp[64];
    switch (ev->kind) {
    case TE_KEY:
        if (ev->key >= TK_ENTER)
            snprintf(tmp, sizeof tmp, "K%u", (unsigned)(ev->key - TK_ENTER));
        else
            snprintf(tmp, sizeof tmp, "K'%c'", (char)ev->key);
        pt_buf_putz(out, tmp);
        if (ev->mods) {
            snprintf(tmp, sizeof tmp, "+%d", ev->mods);
            pt_buf_putz(out, tmp);
        }
        break;
    case TE_TEXT:
    case TE_PASTE:
        pt_buf_putz(out, ev->kind == TE_TEXT ? "T[" : "P[");
        pt_buf_puts(out, ev->text);
        pt_buf_putc(out, ']');
        break;
    case TE_REPLY:
        snprintf(tmp, sizeof tmp, "R%d:%d:%d", ev->reply, ev->a, ev->b);
        pt_buf_putz(out, tmp);
        break;
    }
    pt_buf_putc(out, ' ');
}

/* Decode bytes fed in pieces (cut at the positions in cuts, ascending), with
 * a flush at the end; text events are merged, as the box sees them. */
static void decode(pt_buf *out, const char *in, size_t n, const size_t *cuts, int ncuts, bool cpr)
{
    tui_keys k;
    tui_ev ev;
    size_t at = 0;
    int c;
    pt_buf text;
    pt_buf_init(&text, T_AL);
    tui_keys_init(&k);
    k.want_cpr = cpr;
    pt_buf_reset(out);
    for (c = 0; c <= ncuts; c++) {
        size_t to = c < ncuts ? cuts[c] : n;
        bool last = c == ncuts;
        tui_keys_feed(&k, in + at, to - at);
        at = to;
        while (tui_keys_next(&k, &ev, last)) {
            if (ev.kind == TE_TEXT) {
                pt_buf_puts(&text, ev.text);
                continue;
            }
            if (text.n) {
                pt_buf_putz(out, "T[");
                pt_buf_puts(out, pt_buf_str(&text));
                pt_buf_putz(out, "] ");
                pt_buf_reset(&text);
            }
            describe(out, &ev);
        }
    }
    if (text.n) {
        pt_buf_putz(out, "T[");
        pt_buf_puts(out, pt_buf_str(&text));
        pt_buf_putz(out, "] ");
    }
    CHECK(!tui_keys_pending(&k));
    tui_keys_free(&k);
    pt_buf_free(&text);
}

static void check_keys(const char *in, size_t n, const char *want, bool cpr, int line)
{
    pt_buf got;
    size_t a, b;
    pt_buf_init(&got, T_AL);
    decode(&got, in, n, NULL, 0, cpr);
    t_checks++;
    if (!pt_str_eqz(pt_buf_str(&got), want)) {
        t_failed++;
        fprintf(stderr, "test_tui.c:%d: decoded \"%.*s\", want \"%s\"\n", line, (int)got.n, got.p, want);
    }
    /* the same, split at every byte, and at every pair of bytes */
    for (a = 1; a < n; a++)
        for (b = a; b < n; b++) {
            size_t cuts[2];
            cuts[0] = a;
            cuts[1] = b;
            decode(&got, in, n, cuts, a == b ? 1 : 2, cpr);
            t_checks++;
            if (!pt_str_eqz(pt_buf_str(&got), want)) {
                t_failed++;
                fprintf(stderr, "test_tui.c:%d: split at %zu,%zu: \"%.*s\"\n", line, a, b, (int)got.n, got.p);
                a = n;
                break;
            }
        }
    pt_buf_free(&got);
}
#define KEYS(in, want) check_keys((in), sizeof(in) - 1, (want), false, __LINE__)

static void test_keys(void)
{
    /* K0 enter, 1 tab, 2 backspace, 3 esc, 4-7 up down left right, 8 home,
     * 9 end, 10 insert, 11 delete, 12 pgup, 13 pgdn; +1 shift +2 alt +4 ctrl */
    KEYS("hi", "T[hi] ");
    KEYS("h\xc3\xa9\xe4\xb8\xad", "T[h\xc3\xa9\xe4\xb8\xad] ");
    KEYS("\r", "K0 ");
    KEYS("\n", "K'j'+4 ");
    KEYS("\t", "K1 ");
    KEYS("\x7f\x08", "K2 K2 ");
    KEYS("\x03\x04\x01", "K'c'+4 K'd'+4 K'a'+4 ");
    KEYS("\x1b", "K3 ");
    KEYS("\x1b\x1b", "K3 K3 ");
    KEYS("\x1b[A\x1b[B\x1b[C\x1b[D", "K4 K5 K7 K6 ");
    KEYS("\x1bOA\x1bOD\x1bOH\x1bOF", "K4 K6 K8 K9 ");
    KEYS("\x1b[1;5C\x1b[1;3D\x1b[1;2A", "K7+4 K6+2 K4+1 ");
    KEYS("\x1b[H\x1b[F\x1b[1~\x1b[4~\x1b[7~\x1b[8~", "K8 K9 K8 K9 K8 K9 ");
    KEYS("\x1b[3~\x1b[5~\x1b[6~\x1b[3;5~\x1b[2~", "K11 K12 K13 K11+4 K10 ");
    KEYS("\x1b[Z", "K1+1 ");
    KEYS("\x1b" "b\x1b" "f\x1b\x7f\x1b\r\x1b" "B", "K'b'+2 K'f'+2 K2+2 K0+2 K'b'+3 ");
    /* kitty: Shift+Enter, Esc, Ctrl-C, Alt-b, a keypad key (ignored) */
    KEYS("\x1b[13;2u\x1b[27u\x1b[99;5u\x1b[98;3u\x1b[57399u", "K0+1 K3 K'c'+4 K'b'+2 ");
    KEYS("\x1b[13;2:1u\x1b[97;133u", "K0+1 K'a'+4 "); /* event type; num lock dropped */
    KEYS("\x1b[27;2;13~", "K0+1 "); /* xterm modifyOtherKeys */
    /* paste: verbatim, newlines normalized, cleaned; ends split anywhere */
    KEYS("a\x1b[200~x\r\ny\rz\x1b[A\x03\x1b[201~b", "T[a] P[x\ny\nz[A] T[b] ");
    KEYS("\x1b[200~\x1b[201~", "P[] ");
    /* replies */
    KEYS("\x1b[?62;22c\x1b[?1u", "R1:0:0 R2:1:0 ");
    KEYS("\x1b[5;1R", ""); /* no query outstanding: Shift+F3, dropped */
    check_keys("\x1b[12;40R", 8, "R3:12:40 ", true, __LINE__);
    /* skipped: OSC, DCS, focus, unknown */
    KEYS("\x1b]11;rgb:0/0/0\x07x\x1b]52;c;eA==\x1b\\y", "T[xy] ");
    KEYS("\x1bP1$r0m\x1b\\z\x1b[I\x1b[O\x1b[15~\x1bOP", "T[z] ");
    /* an invalid byte in text */
    KEYS("a\xff" "b", "T[a\xef\xbf\xbd" "b] ");
}

static void test_keys_esc_wait(void)
{
    tui_keys k;
    tui_ev ev;
    tui_keys_init(&k);
    tui_keys_feed(&k, "\x1b", 1);
    CHECK(!tui_keys_next(&k, &ev, false)); /* maybe a sequence: wait */
    CHECK(tui_keys_pending(&k));
    tui_keys_feed(&k, "[A", 2);
    CHECK(tui_keys_next(&k, &ev, false) && ev.kind == TE_KEY && ev.key == TK_UP);
    tui_keys_feed(&k, "\x1b", 1);
    CHECK(tui_keys_next(&k, &ev, true) && ev.key == TK_ESC); /* the timeout passed */
    CHECK(!tui_keys_pending(&k));
    tui_keys_free(&k);
}

/* ---- markdown ---- */

/* Styled rows made readable: {b} bold, {d} dim, {c} cyan, {} plain, ... */
static void readable(pt_buf *out, pt_str s)
{
    size_t i;
    for (i = 0; i < s.n; i++) {
        if (s.p[i] == TUI_STY && i + 1 < s.n) {
            int st = (unsigned char)s.p[++i] - 0x20;
            pt_buf_putc(out, '{');
            if (st & TUI_BOLD)
                pt_buf_putc(out, 'b');
            if (st & TUI_DIM)
                pt_buf_putc(out, 'd');
            if ((st & TUI_COLOR_MASK) == TUI_CYAN)
                pt_buf_putc(out, 'c');
            if ((st & TUI_COLOR_MASK) == TUI_RED)
                pt_buf_putc(out, 'r');
            pt_buf_putc(out, '}');
        } else {
            pt_buf_putc(out, s.p[i]);
        }
    }
}

static pt_str md(tui_md *m, const char *line, int cols, int base, bool band)
{
    pt_buf rows;
    pt_buf_init(&rows, T_AL);
    tui_md_line(m, pt_strz(line), cols, base, band, &rows);
    pt_buf_reset(&tb);
    readable(&tb, pt_buf_str(&rows));
    pt_buf_free(&rows);
    return pt_buf_str(&tb);
}

#define MD(line, want) CHECK_STR(md(&m, (line), 31, 0, false), (want))

static void test_md(void)
{
    tui_md m;
    memset(&m, 0, sizeof m);
    MD("Plain text.", "Plain text.\n");
    MD("", "\n");
    MD("A long paragraph that goes on and on past the edge.", "A long paragraph that goes on\nand on past the edge.\n");
    MD("Some **bold** and `code` here.", "Some {b}bold{} and {c}code{} here.\n");
    MD("An *em*, _em_, snake_case_x.", "An em, em, snake_case_x.\n");
    MD("Unclosed **bold and `tick", "Unclosed **bold and `tick\n");
    MD("2 * 3 * 4 = 24", "2 * 3 * 4 = 24\n");
    MD("**`x` inside**", "{b}{bc}x{b} inside{}\n");
    MD("See [the docs](https://x.io) now", "See the docs {d}https://x.io{} now\n");
    MD("[https://x.io](https://x.io)", "https://x.io\n");
    MD("Escaped \\*not em\\*", "Escaped *not em*\n");
    MD("# Title", "{b}Title\n");
    MD("### Sub ###", "{b}Sub\n");
    MD("#hashtag", "#hashtag\n");
    MD("- an item that wraps onto a second row", "\xe2\x80\xa2 an item that wraps onto a\n  second row\n");
    MD("  * nested", "  \xe2\x80\xa2 nested\n");
    MD("12. twelfth", "12. twelfth\n");
    MD("> quoted **text**", "{d}\xe2\x94\x82 {}quoted {b}text{}\n");
    MD("---", "{d}\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
               "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
               "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
               "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\n");
    MD("| a | b |", "| a | b |\n");
    /* fenced code: dim fences, lines as they are, no markdown inside */
    MD("```c", "{d}```c\n");
    CHECK(m.code);
    MD("int **p = `x`; // a long line that the terminal wraps by itself", "int **p = `x`; // a long line that the terminal wraps by itself\n");
    MD("~~~", "~~~\n");
    MD("```", "{d}```\n");
    CHECK(!m.code);
    /* in the band: code is cut into rows, and widths are pessimistic */
    MD("~~~~", "{d}~~~~\n");
    CHECK_STR(md(&m, "0123456789012345678901234567890123456789", 31, 0, true),
              "012345678901234567890123456789\n0123456789\n");
    MD("~~~~", "{d}~~~~\n");
    /* thinking: dim under everything */
    CHECK_STR(md(&m, "Let me **think**.", 31, TUI_DIM, false), "{d}Let me {bd}think{d}.\n");
    m.ascii = true;
    MD("- ascii", "- ascii\n");
}

/* ---- the input box ---- */

static tui_input_result press(tui_input *in, uint32_t key, int mods)
{
    tui_ev ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = TE_KEY;
    ev.key = key;
    ev.mods = mods;
    return tui_input_key(in, &ev, 20);
}

static void test_input(void)
{
    tui_input in;
    pt_buf f;
    tui_input_init(&in, T_AL);
    tui_input_insert(&in, PT_S("hello world"));
    CHECK(press(&in, 'w', TM_CTRL) == TI_EDITED);
    CHECK_STR(pt_buf_str(&in.text), "hello ");
    CHECK_STR(pt_buf_str(&in.kill), "world");
    press(&in, 'a', TM_CTRL);
    press(&in, 'y', TM_CTRL);
    CHECK_STR(pt_buf_str(&in.text), "worldhello ");
    press(&in, TK_END, 0);
    tui_input_insert(&in, PT_S("e\xcc\x81"));
    press(&in, TK_BACKSPACE, 0); /* a whole cluster */
    CHECK_STR(pt_buf_str(&in.text), "worldhello ");
    press(&in, TK_ENTER, TM_SHIFT);
    tui_input_insert(&in, PT_S("second"));
    CHECK_STR(pt_buf_str(&in.text), "worldhello \nsecond");
    tui_input_layout(&in, 20, 8);
    CHECK(in.nrows == 2 && in.cur_row == 1 && in.cur_col == 6);
    CHECK(press(&in, TK_UP, 0) == TI_MOVED && in.cur == 6);
    CHECK(press(&in, TK_UP, 0) == TI_TOP);
    press(&in, 'u', TM_CTRL); /* the cursor was after "worldh" */
    CHECK_STR(pt_buf_str(&in.text), "ello \nsecond");
    /* word wrap at the width, the cursor's row and scrolling */
    tui_input_set(&in, PT_S("aaaa bbbb cccc dddd eeee ffff"));
    tui_input_layout(&in, 10, 2);
    CHECK(in.nrows == 3 && in.rows[1].start == 10 && in.cur_row == 2 && in.scroll == 1);
    /* history: browsing keeps the draft */
    tui_hist_add(&in, PT_S("one"));
    tui_hist_add(&in, PT_S("two\nlines"));
    tui_input_set(&in, PT_S("draft"));
    CHECK(tui_hist_move(&in, -1));
    CHECK_STR(pt_buf_str(&in.text), "two\nlines");
    CHECK(tui_hist_move(&in, -1) && !tui_hist_move(&in, -1));
    CHECK(tui_hist_move(&in, 1) && tui_hist_move(&in, 1));
    CHECK_STR(pt_buf_str(&in.text), "draft");
    tui_input_free(&in);
    /* the history file */
    pt_buf_init(&f, T_AL);
    tui_hist_line(&f, PT_S("a \"quoted\"\nentry"));
    pt_buf_putz(&f, "not json\n\"\\u001b[2J\"\n");
    tui_input_init(&in, T_AL);
    tui_hist_load(&in, pt_buf_str(&f), 1000);
    CHECK(in.nhist == 2);
    CHECK_STR(tui_hist_at(&in, 0), "a \"quoted\"\nentry");
    CHECK_STR(tui_hist_at(&in, 1), "[2J"); /* cleaned */
    tui_input_free(&in);
    pt_buf_free(&f);
}

/* Settings written back to the user file: one key changes (19.4). */
static pt_str set(const char *src, const char *key, const char *value, bool *ok)
{
    char err[200];
    pt_buf_reset(&tb);
    *ok = cli_json_set(pt_strz(src), key, pt_strz(value), &tb, err, sizeof err);
    return pt_buf_str(&tb);
}

static void test_config_set(void)
{
    bool ok;
    CHECK_STR(set("{\n  \"model\": \"x\",\n  \"tui\": {\"thinking\": \"collapsed\"}\n}\n", "tui.thinking", "\"full\"", &ok),
              "{\n  \"model\": \"x\",\n  \"tui\": {\"thinking\": \"full\"}\n}\n");
    CHECK(ok);
    CHECK_STR(set("{\"model\": \"x\"}", "tui.pager", "\"less\"", &ok), "{\"model\": \"x\", \"tui\": {\"pager\": \"less\"}}");
    CHECK(ok);
    CHECK_STR(set("{ \"tui\": {} }", "tui.symbols", "\"ascii\"", &ok), "{ \"tui\": {\"symbols\": \"ascii\"} }");
    CHECK(ok);
    CHECK_STR(set("{\"tui\": {\"a\": 1}}", "tui.b", "2", &ok), "{\"tui\": {\"a\": 1, \"b\": 2}}");
    CHECK(ok);
    CHECK_STR(set("", "model", "\"m\"", &ok), "{\"model\": \"m\"}\n");
    CHECK(ok);
    set("{\"model\": \"x\",}", "model", "\"y\"", &ok); /* does not parse: never written */
    CHECK(!ok);
    set("[1]", "model", "\"y\"", &ok);
    CHECK(!ok);
    set("{\"tui\": 3}", "tui.pager", "\"y\"", &ok);
    CHECK(!ok);
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
    RUN(test_keys);
    RUN(test_keys_esc_wait);
    RUN(test_md);
    RUN(test_input);
    RUN(test_config_set);
    pt_buf_free(&tb);
    return t_done("test_tui");
}
