/* The band: the few rows at the bottom of the terminal that pytte redraws
 * (sections 16.2.2, 16.2.3 and 16.2.6). Everything above it is scrollback
 * that is written once and never touched again.
 *
 * Only relative moves: up to the top of the band (CSI n A, CR), then down
 * row by row (CR LF, which scrolls when the band grows). The cursor is
 * placed by printing the cursor row's text up to it again, so the
 * terminal's own widths decide where it lands. */
#include "tui/tui.h"

#include <stdlib.h>
#include <string.h>

void tui_frame_init(tui_frame *f, pt_allocator al)
{
    memset(f, 0, sizeof *f);
    pt_buf_init(&f->rows, al);
}

void tui_frame_free(tui_frame *f)
{
    pt_buf_free(&f->rows);
}

void tui_frame_reset(tui_frame *f)
{
    pt_buf_reset(&f->rows);
    f->n = 0;
    f->cur_row = 0;
    f->cur_at = 0;
}

int tui_frame_row(tui_frame *f, pt_str styled, int cols)
{
    size_t k = tui_fit(styled, cols > 1 ? cols - 1 : 0, true);
    long nl = pt_str_find((pt_str){styled.p, k}, '\n');
    if (nl >= 0)
        k = (size_t)nl;
    pt_buf_put(&f->rows, styled.p, k);
    pt_buf_putc(&f->rows, '\n');
    return f->n++;
}

void tui_band_init(tui_band *b, int rows, int cols, pt_allocator al)
{
    memset(b, 0, sizeof *b);
    b->rows = rows;
    b->cols = cols;
    b->reflow = true;
    b->color = true;
    pt_buf_init(&b->out, al);
    pt_buf_init(&b->drawn, al);
}

void tui_band_free(tui_band *b)
{
    pt_buf_free(&b->out);
    pt_buf_free(&b->drawn);
}

/* The next row of rows (each followed by '\n'), from *at. */
static pt_str next_row(pt_str rows, size_t *at)
{
    size_t i = *at;
    pt_str r;
    while (i < rows.n && rows.p[i] != '\n')
        i++;
    r.p = rows.p + *at;
    r.n = i - *at;
    *at = i < rows.n ? i + 1 : i;
    return r;
}

static void up(tui_band *b, int n)
{
    if (n > 0) {
        pt_buf_putz(&b->out, "\x1b[");
        pt_buf_i64(&b->out, n);
        pt_buf_putc(&b->out, 'A');
    }
}

static void styled(tui_band *b, pt_str s)
{
    pt_buf_putz(&b->out, "\x1b[0m");
    tui_sgr(&b->out, s, b->color);
    pt_buf_putz(&b->out, "\x1b[0m");
}

/* From the top of the band, at column 0: draw f, then place the cursor. */
static void draw(tui_band *b, const tui_frame *f)
{
    pt_str nrows = pt_buf_str(&f->rows), orows = pt_buf_str(&b->drawn), cur_row = PT_STR_NULL, prefix;
    int old_h = b->clear ? 0 : b->h, i;
    size_t at = 0, oat = 0;
    if (b->clear)
        pt_buf_putz(&b->out, b->gap ? "\x1b[J\r\n" : "\x1b[J");
    b->clear = false;
    b->gap = false;
    for (i = 0; i < f->n; i++) {
        pt_str r = next_row(nrows, &at), o = i < old_h ? next_row(orows, &oat) : PT_STR_NULL;
        if (i > 0)
            pt_buf_putz(&b->out, "\r\n");
        /* the last row is always written when the band shrinks: CSI J
         * clears from the cursor, which must be past it */
        if (i >= old_h || !pt_str_eq(r, o) || (i == f->n - 1 && old_h > f->n)) {
            styled(b, r);
            pt_buf_putz(&b->out, "\x1b[K");
        }
        if (i == f->cur_row)
            cur_row = r;
    }
    if (old_h > f->n)
        pt_buf_putz(&b->out, "\x1b[J");
    up(b, f->n - 1 - f->cur_row);
    pt_buf_putc(&b->out, '\r');
    prefix.p = cur_row.p;
    prefix.n = f->cur_at < cur_row.n ? f->cur_at : cur_row.n;
    if (prefix.n)
        styled(b, prefix);
    pt_buf_reset(&b->drawn);
    pt_buf_puts(&b->drawn, nrows);
    b->h = f->n;
    b->cur = f->cur_row;
    b->up = f->cur_row;
    b->cur_at = prefix.n;
}

void tui_band_paint(tui_band *b, const tui_frame *f)
{
    pt_buf_putz(&b->out, "\x1b[?2026h\x1b[?25l");
    up(b, b->up);
    pt_buf_putc(&b->out, '\r');
    draw(b, f);
    pt_buf_putz(&b->out, "\x1b[?25h\x1b[?2026l");
}

void tui_band_commit(tui_band *b, pt_str lines, const tui_frame *f)
{
    size_t at = 0;
    pt_buf_putz(&b->out, "\x1b[?2026h\x1b[?25l");
    up(b, b->up);
    pt_buf_putz(&b->out, "\r\x1b[J");
    while (at < lines.n) {
        styled(b, next_row(lines, &at));
        pt_buf_putz(&b->out, "\r\n");
    }
    b->h = 0;
    b->up = 0;
    b->clear = false;
    b->gap = false;
    draw(b, f);
    pt_buf_putz(&b->out, "\x1b[?25h\x1b[?2026l");
}

/* Rows from the cursor up to the top of the band after a resize to cols,
 * with the widest (max) or narrowest widths a terminal may use. */
static int rows_up(tui_band *b, int cols, bool max)
{
    pt_str rows = pt_buf_str(&b->drawn), r = PT_STR_NULL;
    size_t at = 0;
    int n = 0, i, col, next_w;
    /* Each row above the cursor was a line ended by our CR LF: it now takes
     * as many rows as it wraps to. The cursor's own row adds the rows its
     * text up to the cursor wraps to, and one more if the character after
     * the cursor no longer fits on the last of them. */
    for (i = 0; i <= b->cur; i++) {
        r = next_row(rows, &at);
        if (i < b->cur)
            n += tui_rows(r, cols, max, &col);
    }
    /* the character after the cursor: if it wraps, the cursor goes with it */
    next_w = tui_next_w(r, b->cur_at < r.n ? b->cur_at : r.n, max);
    r.n = b->cur_at < r.n ? b->cur_at : r.n;
    n += tui_rows(r, cols, max, &col) - 1;
    return n + (col + next_w > cols || col >= cols);
}

void tui_band_resize(tui_band *b, int rows, int cols)
{
    if (cols < 1)
        cols = 1;
    if (b->h && b->reflow) {
        /* Erring towards too far up: that costs a few transcript rows, while
         * too short would leave band rows behind. When the two answers
         * differ, the row above the new top may be the middle of a wrapped
         * line, which a later reflow would join to the band; an empty row
         * in between keeps them apart. */
        int lo = rows_up(b, cols, false), height = 0, col;
        pt_str all = pt_buf_str(&b->drawn);
        size_t at = 0;
        while (at < all.n)
            height += tui_rows(next_row(all, &at), cols, true, &col);
        b->up = rows_up(b, cols, true);
        /* Also when the band may now be taller than the screen: its top may
         * have gone into the scrollback, and the row above the screen be a
         * wrapped piece of it. */
        b->gap = b->gap || b->up != lo || height >= rows;
    } else {
        b->up = b->cur; /* rows are cut or kept in place, not rewrapped */
    }
    b->rows = rows;
    b->cols = cols;
    b->clear = true;
}

void tui_band_redraw(tui_band *b)
{
    b->clear = true;
}

void tui_band_erase(tui_band *b)
{
    up(b, b->up);
    pt_buf_putz(&b->out, "\r\x1b[J");
    b->h = 0;
    b->cur = 0;
    b->up = 0;
    b->clear = false;
}
