/* The TUI on a strict emulated terminal (section 22.5): first the emulator
 * itself, then the band, then random sessions checked after every paint. */
#include "t.h"

#include "vt.c"

static pt_buf sb; /* scratch */

#define W(v, s) vt_write((v), (s), sizeof(s) - 1)

static pt_str row(vt *v, int r)
{
    vt_row_text(v, r, &sb);
    return pt_buf_str(&sb);
}

static pt_str lines(vt *v, int upto)
{
    vt_lines(v, upto, &sb);
    return pt_buf_str(&sb);
}

static void test_vt(void)
{
    vt *v = vt_new(4, 10, true);
    W(v, "hello\r\nworld");
    CHECK_STR(row(v, 0), "hello");
    CHECK_STR(row(v, 1), "world");
    CHECK(v->r == 1 && v->c == 5);
    /* deferred wrap: the 10th column keeps the cursor; CR LF after it adds no row */
    W(v, "\r\n0123456789\r\nnext");
    CHECK_STR(row(v, 2), "0123456789");
    CHECK_STR(row(v, 3), "next");
    /* autowrap marks the row; scrolling feeds the scrollback */
    W(v, "\r\nabcdefghijKLM\r\n");
    CHECK(v->nsb == 3);
    CHECK_STR(lines(v, 3), "hello\nworld\n0123456789\nnext\nabcdefghijKLM\n");
    /* up, erase */
    W(v, "\x1b[2A\rXY\x1b[K\x1b[J");
    CHECK_STR(row(v, 1), "XY");
    CHECK_STR(row(v, 2), "");
    CHECK(!v->err[0]);
    /* wide characters do not split at the edge */
    W(v, "\r\n123456789\xe4\xb8\xad");
    CHECK_STR(row(v, 2), "123456789");
    CHECK_STR(row(v, 3), "\xe4\xb8\xad");
    CHECK(!v->err[0]);
    vt_free(v);

    /* strict: anything outside the vocabulary is an error */
    v = vt_new(4, 10, true);
    W(v, "\x1b[3m");
    CHECK(strstr(v->err, "SGR") != NULL);
    vt_free(v);
    v = vt_new(4, 10, true);
    W(v, "\x1b[2;3H");
    CHECK(v->err[0]);
    vt_free(v);
    v = vt_new(4, 10, true);
    W(v, "\x1b[?1049h");
    CHECK(v->err[0]);
    vt_free(v);
    v = vt_new(4, 10, true);
    W(v, "a\tb");
    CHECK(strstr(v->err, "control") != NULL);
    vt_free(v);

    /* modes and replies */
    v = vt_new(4, 10, true);
    W(v, "\x1b[6n\x1b[?u\x1b[c\x1b[?2004h\x1b[>1u\x1b[?25l\x1b[?2026h");
    CHECK_STR(pt_buf_str(&v->replies), "\x1b[1;1R\x1b[?0u\x1b[?62;22c");
    CHECK(v->paste && v->kitty == 1 && v->hidden && v->sync);
    W(v, "\x1b[<u\x1b[?2004l\x1b[?25h\x1b[?2026l\x1b[0;1;2;31mx\x1b[0m");
    CHECK(!v->paste && v->kitty == 0 && !v->hidden && !v->sync && !v->err[0]);
    vt_free(v);
}

static void test_vt_resize(void)
{
    vt *v = vt_new(5, 10, true);
    /* reflow: narrowing rewraps, widening joins wrapped rows again */
    W(v, "abcdefghijklmno\r\nxyz\r\n> input");
    CHECK(v->r == 3 && v->c == 7);
    vt_resize(v, 5, 5);
    CHECK_STR(lines(v, v->rows), "abcdefghijklmno\nxyz\n> input\n");
    CHECK_STR(row(v, v->r), "ut");
    CHECK(v->c == 2);
    vt_resize(v, 5, 20);
    CHECK_STR(row(v, 0), "abcdefghijklmno");
    CHECK_STR(row(v, 2), "> input");
    CHECK(v->r == 2 && v->c == 7);
    /* shorter: rows go to the scrollback, the cursor stays on screen */
    vt_resize(v, 2, 20);
    CHECK(v->r == 1 && v->nsb == 1);
    vt_free(v);

    /* no reflow: rows are cut, and stay cut */
    v = vt_new(5, 10, false);
    W(v, "abcdefghij\r\n> input");
    vt_resize(v, 5, 4);
    CHECK_STR(row(v, 0), "abcd");
    CHECK(v->r == 1 && v->c == 3);
    vt_resize(v, 5, 10);
    CHECK_STR(row(v, 0), "abcd");
    vt_free(v);
}

int main(void)
{
    pt_buf_init(&sb, T_AL);
    RUN(test_vt);
    RUN(test_vt_resize);
    pt_buf_free(&sb);
    return t_done("test_screen");
}
