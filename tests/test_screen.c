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

/* ---- the band ---- */

static uint64_t rng = 88172645463325252ull;

static uint32_t rnd(uint32_t n)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint32_t)(rng >> 16) % n;
}

static const char *const certain[] = {"alpha", "beta", "gamma", "x", "\xe4\xb8\xad\xe6\x96\x87", "e\xcc\x81te",
                                      "0123456789", "--", "\xc3\xa9t\xc3\xa9"};
static const char *const uncertain[] = {"\xf0\x9f\x98\x80", "\xe2\x9d\xa4\xef\xb8\x8f", "\xc2\xb1",
                                        "\xf0\x9f\x87\xb8\xf0\x9f\x87\xaa", "ok"};

static void random_text(pt_buf *b, int words, bool unc)
{
    int i;
    for (i = 0; i < words; i++) {
        if (i)
            pt_buf_putc(b, ' ');
        if (rnd(5) == 0)
            tui_style(b, (int)rnd(32));
        pt_buf_putz(b, unc && rnd(3) == 0 ? uncertain[rnd(5)] : certain[rnd(9)]);
    }
    if (rnd(2))
        tui_style(b, 0);
}

static void plain(pt_buf *out, pt_str s)
{
    size_t i;
    for (i = 0; i < s.n; i++) {
        if (s.p[i] == TUI_STY) {
            i++;
            continue;
        }
        pt_buf_putc(out, s.p[i]);
    }
}

static int run_id;

typedef struct bstate {
    vt *v;
    tui_band b;
    tui_frame f;
    pt_buf want;    /* the transcript, plain, one line each */
    bool exact;     /* the transcript must match exactly */
    size_t since;   /* want up to here may have lost lines to a resize */
    size_t settled; /* the transcript up to here may hold band rows (16.2.6) */
    bool unc;
    int ops;
} bstate;

static void random_frame(bstate *st)
{
    pt_buf t;
    int n = 1 + (int)rnd(5), i;
    size_t at = 0, k;
    pt_str row;
    pt_buf_init(&t, T_AL);
    tui_frame_reset(&st->f);
    for (i = 0; i < n; i++) {
        pt_buf_reset(&t);
        pt_buf_putz(&t, "#"); /* every band row starts with a mark */
        random_text(&t, (int)rnd(12), st->unc);
        tui_frame_row(&st->f, pt_buf_str(&t), st->b.cols);
    }
    st->f.cur_row = (int)rnd((uint32_t)n);
    for (i = 0; i <= st->f.cur_row; i++) {
        size_t j = at;
        while (st->f.rows.p[j] != '\n')
            j++;
        row.p = st->f.rows.p + at;
        row.n = j - at;
        at = j + 1;
    }
    /* the cursor on a cluster boundary, never inside a style switch */
    k = 0;
    {
        size_t stop = rnd((uint32_t)row.n + 1), i2 = 0;
        while (i2 < row.n && i2 < stop) {
            k = i2;
            i2 = row.p[i2] == TUI_STY ? i2 + 2 : tui_next_cluster(row, i2);
        }
        if (i2 <= stop)
            k = i2;
    }
    st->f.cur_at = k;
    pt_buf_free(&t);
}

static void flush_to_vt(bstate *st)
{
    vt_write(st->v, st->b.out.p, st->b.out.n);
    pt_buf_reset(&st->b.out);
}

/* After every operation: the band as drawn, the cursor, the transcript. */
static void check_screen(bstate *st, const char *op)
{
    vt *v = st->v;
    pt_buf want, got;
    size_t at = 0;
    int top = v->r - st->f.cur_row, i;
    pt_str cur_prefix = PT_STR_NULL;
    char ctx[96];
    snprintf(ctx, sizeof ctx, "run %d op %d (%s), %dx%d %s", run_id, st->ops, op, v->cols, v->rows,
             v->reflow ? "reflow" : "no reflow");
    pt_buf_init(&want, T_AL);
    pt_buf_init(&got, T_AL);
    if (v->err[0]) {
        fprintf(stderr, "%s: %s\n", ctx, v->err);
        CHECK(!"emulator error");
        v->err[0] = 0;
    }
    CHECK(top >= 0);
    if (top < 0)
        top = 0;
    for (i = 0; i < st->f.n; i++) {
        size_t j = at;
        pt_str r;
        while (st->f.rows.p[j] != '\n')
            j++;
        r.p = st->f.rows.p + at;
        r.n = j - at;
        at = j + 1;
        if (i == st->f.cur_row)
            cur_prefix = (pt_str){r.p, st->f.cur_at};
        pt_buf_reset(&want);
        plain(&want, r);
        vt_row_text(v, top + i, &got);
        t_checks++;
        if (top + i >= v->rows || !pt_str_eq(pt_buf_str(&got), pt_buf_str(&want))) {
            t_failed++;
            fprintf(stderr, "%s: band row %d is \"%.*s\", want \"%.*s\"\n", ctx, i, (int)got.n, got.p,
                    (int)want.n, want.p);
            break;
        }
    }
    for (i = top + st->f.n; i < v->rows; i++) {
        vt_row_text(v, i, &got);
        CHECK(got.n == 0);
    }
    /* the cursor is where the terminal's own widths put it */
    {
        int c = 0;
        size_t k = 0;
        while (k < cur_prefix.n) {
            uint32_t cp;
            if (cur_prefix.p[k] == TUI_STY) {
                k += 2;
                continue;
            }
            k += tui_utf8_dec(cur_prefix.p + k, cur_prefix.n - k, &cp);
            c += v->width(cp);
        }
        t_checks++;
        if (v->r != top + st->f.cur_row || v->c != c) {
            t_failed++;
            fprintf(stderr, "%s: cursor at %d,%d, want %d,%d\n", ctx, v->r, v->c, top + st->f.cur_row, c);
        }
    }
    /* the transcript: all of it, or (after a lossy resize) a prefix of each
     * line in order, and never a band row */
    vt_lines(v, top, &got);
    if (st->exact) {
        t_checks++;
        if (!pt_str_eq(pt_buf_str(&got), pt_buf_str(&st->want))) {
            t_failed++;
            fprintf(stderr, "%s: transcript differs:\n%.*s---\n%.*s\n", ctx, (int)got.n, got.p,
                    (int)st->want.n, st->want.p);
            st->exact = false;
        }
    } else {
        /* A resize may erase rows just above the band (16.2.6): then a line
         * can lose its end, and the next one may join it. What must hold:
         * no band row ever reaches the transcript, and what was committed
         * since the last resize is all there. */
        pt_str g = pt_buf_str(&got), tail = {st->want.p + st->since, st->want.n - st->since};
        t_checks += 2;
        if (g.n > st->settled && pt_str_contains((pt_str){g.p + st->settled, g.n - st->settled}, PT_S("#"))) {
            t_failed++;
            fprintf(stderr, "%s: a band row in the transcript:\n%.*s\n", ctx, (int)g.n, g.p);
        }
        if (g.n < tail.n || (tail.n && memcmp(g.p + g.n - tail.n, tail.p, tail.n) != 0)) {
            t_failed++;
            fprintf(stderr, "%s: lines committed since the resize are missing:\n%.*s---\n%.*s\n", ctx,
                    (int)g.n, g.p, (int)tail.n, tail.p);
        }
    }
    pt_buf_free(&want);
    pt_buf_free(&got);
}

static void band_run(bool reflow, bool disagree, bool unc, bool sticky, uint64_t seed, int ops)
{
    bstate st;
    pt_buf lines, t;
    int op;
    rng = seed;
    run_id++;
    if (getenv("VT_RUN") && run_id != atoi(getenv("VT_RUN")))
        return;
    memset(&st, 0, sizeof st);
    st.v = vt_new(20, 40, reflow);
    if (disagree)
        st.v->width = vt_width_old;
    st.v->sticky = sticky;
    tui_band_init(&st.b, 20, 40, T_AL);
    st.b.reflow = reflow;
    tui_frame_init(&st.f, T_AL);
    pt_buf_init(&st.want, T_AL);
    pt_buf_init(&lines, T_AL);
    pt_buf_init(&t, T_AL);
    st.exact = reflow && !disagree && !unc;
    st.unc = unc;
    random_frame(&st);
    tui_band_paint(&st.b, &st.f);
    flush_to_vt(&st);
    check_screen(&st, "first paint");
    for (op = 0; op < ops; op++) {
        uint32_t k = rnd(10);
        const char *name;
        st.ops = op;
        if (k < 4) {
            name = "paint";
            if (rnd(3))
                random_frame(&st);
            tui_band_paint(&st.b, &st.f);
        } else if (k < 7) {
            int n = (int)rnd(4), i;
            name = "commit";
            pt_buf_reset(&lines);
            for (i = 0; i < n; i++) {
                pt_buf_reset(&t);
                random_text(&t, (int)rnd(20), unc);
                pt_buf_puts(&lines, pt_buf_str(&t));
                pt_buf_putc(&lines, '\n');
                plain(&st.want, pt_buf_str(&t));
                pt_buf_putc(&st.want, '\n');
            }
            if (rnd(2))
                random_frame(&st);
            tui_band_commit(&st.b, pt_buf_str(&lines), &st.f);
        } else if (k < 9) {
            int rows = 12 + (int)rnd(19), cols = 10 + (int)rnd(60);
            name = "resize";
            if (getenv("VT_DEBUG") && op == atoi(getenv("VT_DEBUG")) && run_id == atoi(getenv("VT_RUN"))) {
                int r;
                fprintf(stderr, "before: cursor %d,%d band up %d cur %d\n", st.v->r, st.v->c, st.b.up, st.b.cur);
                for (r = 0; r < st.v->rows; r++) {
                    vt_row_text(st.v, r, &t);
                    fprintf(stderr, "%2d%c|%.*s|\n", r, st.v->screen[r].wrapped ? 'w' : ' ', (int)t.n, t.p);
                }
                pt_buf_reset(&t);
            }
            vt_resize(st.v, rows, cols);
            tui_band_resize(&st.b, rows, cols);
            if (st.b.gap && st.v->r >= st.b.up)
                pt_buf_putc(&st.want, '\n'); /* the band leaves an empty row (16.2.6) */
            if (st.v->r < st.b.up) {
                /* The old band rewrapped taller than the screen: its top went
                 * into the scrollback, out of reach. Accepted (16.2.6); what
                 * follows must still be right. */
                vt_lines(st.v, 0, &t);
                st.settled = t.n;
                st.exact = false;
                pt_buf_reset(&t);
            }
            if (getenv("VT_DEBUG") && op == atoi(getenv("VT_DEBUG")) && run_id == atoi(getenv("VT_RUN"))) {
                int r;
                fprintf(stderr, "after %dx%d: cursor %d,%d band up %d\n", cols, rows, st.v->r, st.v->c, st.b.up);
                for (r = 0; r < st.v->rows; r++) {
                    vt_row_text(st.v, r, &t);
                    fprintf(stderr, "%2d%c|%.*s|\n", r, st.v->screen[r].wrapped ? 'w' : ' ', (int)t.n, t.p);
                }
                pt_buf_reset(&t);
            }
            st.since = st.want.n;
            random_frame(&st); /* laid out for the new size */
            tui_band_paint(&st.b, &st.f);
        } else {
            name = "redraw";
            tui_band_redraw(&st.b);
            tui_band_paint(&st.b, &st.f);
        }
        flush_to_vt(&st);
        if (!reflow)
            st.exact = false; /* cut rows stay cut */
        if (getenv("VT_TRACE"))
            fprintf(stderr, "op %d %s: cursor %d,%d of %dx%d, band up %d, %d rows, sb %d\n", op, name, st.v->r,
                    st.v->c, st.v->cols, st.v->rows, st.b.up, st.f.n, st.v->nsb);
        check_screen(&st, name);
        if (t_failed > 20)
            break;
    }
    /* exit: erase the band, the transcript is all that is left */
    tui_band_erase(&st.b);
    flush_to_vt(&st);
    CHECK(st.v->r >= 0 && !st.v->err[0]);
    tui_band_free(&st.b);
    tui_frame_free(&st.f);
    pt_buf_free(&st.want);
    pt_buf_free(&lines);
    pt_buf_free(&t);
    vt_free(st.v);
}

static void test_band(void)
{
    uint64_t seed, seeds = getenv("BAND_SEEDS") ? (uint64_t)atoi(getenv("BAND_SEEDS")) : 16;
    for (seed = 1; seed <= seeds; seed++) {
        band_run(true, false, false, false, seed, 300);  /* exact */
        band_run(true, false, true, false, seed, 300);   /* uncertain widths */
        band_run(true, true, true, false, seed, 300);    /* a terminal that disagrees */
        band_run(true, false, false, true, seed, 300);   /* tmux: erased rows keep their width */
        band_run(false, false, false, false, seed, 300); /* no reflow */
        band_run(false, true, true, false, seed, 300);
    }
}

int main(void)
{
    pt_buf_init(&sb, T_AL);
    RUN(test_vt);
    RUN(test_vt_resize);
    RUN(test_band);
    pt_buf_free(&sb);
    return t_done("test_screen");
}
