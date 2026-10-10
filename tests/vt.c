/* A strict VT emulator for the TUI tests (section 22.5). Included by the
 * test programs.
 *
 * It models a screen grid plus scrollback, autowrap with the deferred-wrap
 * state, and exactly the escape vocabulary of section 16.2.1 (R2); any
 * other sequence is recorded in err. Switches:
 *   reflow    on resize, rewrap logical lines (kitty, VTE, ...) or cut and
 *             keep rows in place (xterm, st, the Linux console);
 *   width     its own width per code point, to play a terminal that
 *             disagrees with pytte;
 *   modes     kitty flag stack, bracketed paste, cursor, synchronized
 *             output; queries are answered into `replies`. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tui/tui.h"

typedef struct vt_cell {
    char s[24]; /* the code point plus any zero-width ones after it */
    unsigned char n;
    signed char w; /* 1 or 2; 0: the right half of a wide character */
    unsigned char style;
} vt_cell;

typedef struct vt_row {
    vt_cell *c;
    int n;        /* cells allocated */
    bool wrapped; /* continued on the next row by autowrap */
} vt_row;

typedef struct vt {
    int rows, cols;
    vt_row *screen;
    vt_row *sb; /* scrollback, oldest first */
    int nsb, sbcap;
    int r, c;
    bool pending; /* deferred wrap */
    int style;
    bool reflow;
    int (*width)(uint32_t cp);
    /* modes */
    int kitty; /* pushed flags (depth) */
    bool paste, hidden, sync;
    /* queries */
    bool answer, kitty_ok; /* answer DA1 (and the kitty query) at all */
    pt_buf replies;
    /* parser */
    char pend[64];
    int npend;
    char err[256];
    long writes;
} vt;

static int vt_width_best(uint32_t cp)
{
    return tui_cp_best(cp);
}

/* A terminal with older tables: pictographs and ambiguous characters are
 * one column, zero-width marks newer than its tables take one. */
static int vt_width_old(uint32_t cp) __attribute__((unused));
static int vt_width_old(uint32_t cp)
{
    int b = tui_cp_best(cp), m = tui_cp_max(cp);
    if (b == 2 && cp >= 0x1F000)
        return 1;
    if (b == 0 && m == 1)
        return 1;
    return b;
}

static void vt_fail(vt *v, const char *what, const char *seq, int n)
{
    if (!v->err[0])
        snprintf(v->err, sizeof v->err, "%s: \"%.*s\"", what, n, seq);
}

static void row_init(vt_row *row, int cols)
{
    int i;
    row->c = (vt_cell *)calloc((size_t)cols, sizeof(vt_cell));
    row->n = cols;
    row->wrapped = false;
    for (i = 0; i < cols; i++)
        row->c[i].w = 1;
}

static void row_free(vt_row *row)
{
    free(row->c);
    row->c = NULL;
}

static vt *vt_new(int rows, int cols, bool reflow)
{
    vt *v = (vt *)calloc(1, sizeof *v);
    int i;
    v->rows = rows;
    v->cols = cols;
    v->reflow = reflow;
    v->width = vt_width_best;
    v->answer = true;
    v->kitty_ok = true;
    v->screen = (vt_row *)calloc((size_t)rows, sizeof(vt_row));
    for (i = 0; i < rows; i++)
        row_init(&v->screen[i], cols);
    pt_buf_init(&v->replies, T_AL);
    return v;
}

static void vt_free(vt *v)
{
    int i;
    for (i = 0; i < v->rows; i++)
        row_free(&v->screen[i]);
    for (i = 0; i < v->nsb; i++)
        row_free(&v->sb[i]);
    free(v->screen);
    free(v->sb);
    pt_buf_free(&v->replies);
    free(v);
}

static void push_sb(vt *v, vt_row row)
{
    if (v->nsb == v->sbcap) {
        v->sbcap = v->sbcap ? v->sbcap * 2 : 64;
        v->sb = (vt_row *)realloc(v->sb, (size_t)v->sbcap * sizeof(vt_row));
    }
    v->sb[v->nsb++] = row;
}

static void scroll_up(vt *v)
{
    push_sb(v, v->screen[0]);
    memmove(v->screen, v->screen + 1, (size_t)(v->rows - 1) * sizeof(vt_row));
    row_init(&v->screen[v->rows - 1], v->cols);
}

static void linefeed(vt *v)
{
    if (v->r == v->rows - 1)
        scroll_up(v);
    else
        v->r++;
}

static void clear_cells(vt_row *row, int from, int to)
{
    int i;
    for (i = from; i < to && i < row->n; i++) {
        row->c[i].n = 0;
        row->c[i].w = 1;
        row->c[i].style = 0;
    }
}

static void put_cp(vt *v, uint32_t cp, const char *bytes, int nb)
{
    int w = v->width(cp);
    vt_row *row;
    vt_cell *cell;
    if (w == 0) { /* joins the previous cell */
        int c = v->pending ? v->c : v->c - 1;
        row = &v->screen[v->r];
        while (c > 0 && row->c[c].w == 0)
            c--;
        if (c >= 0 && row->c[c].n + nb < (int)sizeof row->c[c].s) {
            memcpy(row->c[c].s + row->c[c].n, bytes, (size_t)nb);
            row->c[c].n = (unsigned char)(row->c[c].n + nb);
        }
        return;
    }
    if (v->pending || v->c + w > v->cols) {
        v->screen[v->r].wrapped = true;
        v->pending = false;
        v->c = 0;
        linefeed(v);
    }
    row = &v->screen[v->r];
    cell = &row->c[v->c];
    memcpy(cell->s, bytes, (size_t)nb);
    cell->n = (unsigned char)nb;
    cell->w = (signed char)w;
    cell->style = (unsigned char)v->style;
    if (w == 2)
        row->c[v->c + 1].w = 0, row->c[v->c + 1].n = 0;
    if (v->c + w >= v->cols) {
        v->c = v->cols - 1;
        v->pending = true;
    } else {
        v->c += w;
    }
}

static bool sgr_ok(vt *v, const char *params, int n)
{
    int i = 0;
    while (i <= n) {
        long p = 0;
        int start = i;
        while (i < n && params[i] >= '0' && params[i] <= '9')
            p = p * 10 + (params[i++] - '0');
        if (i < n && params[i] != ';')
            return false;
        if (start == i)
            p = 0;
        if (p == 0)
            v->style = 0;
        else if (p == 1)
            v->style |= TUI_BOLD;
        else if (p == 2)
            v->style |= TUI_DIM;
        else if ((p >= 30 && p <= 37) || p == 39 || (p >= 90 && p <= 97))
            v->style = (v->style & ~TUI_COLOR_MASK) | (p == 39 ? 0 : (int)((p % 10) + 1) << 2 & TUI_COLOR_MASK);
        else
            return false; /* italics, underline, backgrounds, 256 colours: not ours */
        i++;
    }
    return true;
}

static void csi(vt *v, const char *s, int n)
{
    /* s: the bytes after "ESC [", final byte last */
    char fin = s[n - 1];
    const char *p = s;
    int np = n - 1;
    long num = 0;
    int i;
    if (fin == 'm') {
        if (!sgr_ok(v, p, np))
            vt_fail(v, "SGR outside the vocabulary", s, n);
        return;
    }
    if (np && p[0] == '?') {
        if (fin == 'u' && np == 1) { /* kitty query */
            if (v->answer && v->kitty_ok)
                pt_buf_putz(&v->replies, "\x1b[?0u");
            return;
        }
        if ((fin == 'h' || fin == 'l') && np > 1) {
            bool on = fin == 'h';
            long mode = strtol(p + 1, NULL, 10);
            if (mode == 25)
                v->hidden = !on;
            else if (mode == 2004)
                v->paste = on;
            else if (mode == 2026)
                v->sync = on;
            else
                vt_fail(v, "private mode outside the vocabulary", s, n);
            return;
        }
        vt_fail(v, "sequence outside the vocabulary", s, n);
        return;
    }
    if (np && p[0] == '>' && fin == 'u') {
        if (np == 2 && p[1] == '1')
            v->kitty++;
        else
            vt_fail(v, "kitty flags other than 1", s, n);
        return;
    }
    if (np && p[0] == '<' && fin == 'u' && np == 1) {
        if (v->kitty)
            v->kitty--;
        return;
    }
    for (i = 0; i < np; i++) {
        if (p[i] < '0' || p[i] > '9') {
            vt_fail(v, "sequence outside the vocabulary", s, n);
            return;
        }
        num = num * 10 + (p[i] - '0');
    }
    switch (fin) {
    case 'A':
        v->r -= (int)(num ? num : 1);
        if (v->r < 0)
            v->r = 0;
        v->pending = false;
        return;
    case 'K':
        if (num) {
            vt_fail(v, "erase other than to the end", s, n);
            return;
        }
        clear_cells(&v->screen[v->r], v->c, v->cols);
        v->screen[v->r].wrapped = false;
        return;
    case 'J':
        if (num) {
            vt_fail(v, "erase other than to the end", s, n);
            return;
        }
        clear_cells(&v->screen[v->r], v->c, v->cols);
        v->screen[v->r].wrapped = false;
        for (i = v->r + 1; i < v->rows; i++) {
            clear_cells(&v->screen[i], 0, v->cols);
            v->screen[i].wrapped = false;
        }
        return;
    case 'c':
        if (num == 0 && v->answer)
            pt_buf_putz(&v->replies, "\x1b[?62;22c");
        return;
    case 'n':
        if (num == 6) {
            char rep[32];
            if (v->answer) {
                snprintf(rep, sizeof rep, "\x1b[%d;%dR", v->r + 1, v->c + 1);
                pt_buf_putz(&v->replies, rep);
            }
            return;
        }
        break;
    }
    vt_fail(v, "sequence outside the vocabulary", s, n);
}

static void vt_write(vt *v, const char *p, size_t n)
{
    size_t i = 0;
    v->writes++;
    while (i < n) {
        unsigned char ch;
        if (v->npend) { /* an escape sequence or UTF-8 in progress */
            if (v->npend >= (int)sizeof v->pend - 1) {
                vt_fail(v, "overlong sequence", v->pend, v->npend);
                v->npend = 0;
                continue;
            }
            v->pend[v->npend++] = p[i++];
            if (v->pend[0] == 0x1b) {
                if (v->npend == 2 && v->pend[1] != '[') {
                    vt_fail(v, "escape outside the vocabulary", v->pend, v->npend);
                    v->npend = 0;
                } else if (v->npend > 2 && v->pend[v->npend - 1] >= 0x40 && v->pend[v->npend - 1] <= 0x7E) {
                    csi(v, v->pend + 2, v->npend - 2);
                    v->npend = 0;
                }
            } else {
                uint32_t cp;
                size_t used = tui_utf8_dec(v->pend, (size_t)v->npend, &cp);
                unsigned char lead = (unsigned char)v->pend[0];
                int need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
                if (v->npend >= need || used < (size_t)v->npend) {
                    put_cp(v, cp, v->pend, (int)used);
                    if (cp == TUI_REPLACEMENT && memcmp(v->pend, "\xef\xbf\xbd", 3) != 0)
                        vt_fail(v, "invalid UTF-8", v->pend, v->npend);
                    v->npend = 0;
                }
            }
            continue;
        }
        ch = (unsigned char)p[i++];
        if (ch == '\r') {
            v->c = 0;
            v->pending = false;
        } else if (ch == '\n') {
            /* OPOST/ONLCR is on, but the TUI always writes CR LF itself */
            v->pending = false;
            linefeed(v);
        } else if (ch == 0x1b || ch >= 0x80) {
            v->pend[v->npend++] = (char)ch;
        } else if (ch < 0x20 || ch == 0x7F) {
            char seq[1];
            seq[0] = (char)ch;
            vt_fail(v, "control byte", seq, 1);
        } else {
            char b = (char)ch;
            put_cp(v, ch, &b, 1);
        }
    }
}

/* ---- reading ---- */

static void row_text(const vt_row *row, int cols, pt_buf *out)
{
    int i, end = 0;
    for (i = 0; i < cols && i < row->n; i++)
        if (row->c[i].n)
            end = i + 1;
    for (i = 0; i < end; i++) {
        if (row->c[i].w == 0)
            continue;
        if (row->c[i].n)
            pt_buf_put(out, row->c[i].s, row->c[i].n);
        else
            pt_buf_putc(out, ' ');
    }
}

static void vt_row_text(vt *v, int r, pt_buf *out)
{
    pt_buf_reset(out);
    row_text(&v->screen[r], v->cols, out);
}

/* The logical lines (wrapped rows joined) of the scrollback and of the
 * screen rows above `upto`, each followed by '\n'. */
static void vt_lines(vt *v, int upto, pt_buf *out)
{
    int i;
    pt_buf_reset(out);
    for (i = 0; i < v->nsb + upto; i++) {
        const vt_row *row = i < v->nsb ? &v->sb[i] : &v->screen[i - v->nsb];
        row_text(row, row->n, out);
        if (!row->wrapped)
            pt_buf_putc(out, '\n');
    }
}

/* ---- resize ---- */

typedef struct vt_line {
    vt_cell *c;
    int n, cap;
} vt_line;

static void line_push(vt_line *l, const vt_cell *c)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->c = (vt_cell *)realloc(l->c, (size_t)l->cap * sizeof(vt_cell));
    }
    l->c[l->n++] = *c;
}

static void vt_resize(vt *v, int rows, int cols)
{
    int i, j, total = v->nsb + v->rows, cur = v->nsb + v->r;
    vt_row *all = (vt_row *)malloc((size_t)total * sizeof(vt_row));
    vt_row *out = NULL;
    int nout = 0, capout = 0, cur_out = 0, cur_col = 0, bottom, top, last;
    if (v->nsb)
        memcpy(all, v->sb, (size_t)v->nsb * sizeof(vt_row));
    memcpy(all + v->nsb, v->screen, (size_t)v->rows * sizeof(vt_row));
    free(v->sb);
    free(v->screen);
    v->sb = NULL;
    v->nsb = v->sbcap = 0;
    /* the last row that matters: content or the cursor */
    for (last = total - 1; last > cur; last--) {
        bool blank = true;
        for (j = 0; j < all[last].n && blank; j++)
            blank = !all[last].c[j].n;
        if (!blank)
            break;
    }
#define OUT_ROW()                                                                   \
    do {                                                                            \
        if (nout == capout) {                                                       \
            capout = capout ? capout * 2 : 64;                                      \
            out = (vt_row *)realloc(out, (size_t)capout * sizeof(vt_row));          \
        }                                                                           \
        row_init(&out[nout++], cols);                                               \
    } while (0)
    if (v->reflow) {
        i = 0;
        while (i <= last) {
            vt_line l = {NULL, 0, 0};
            int k, col = 0, offset = -1, lw = 0;
            bool placed = false;
            /* a logical line: rows joined while wrapped */
            for (;;) {
                int end = all[i].n;
                if (all[i].wrapped) /* padding left by a wide character that did not fit */
                    while (end > 0 && !all[i].c[end - 1].n && all[i].c[end - 1].w == 1)
                        end--;
                if (i == cur) /* the cursor: so many columns of content into the line */
                    offset = lw + v->c;
                for (j = 0; j < end; j++)
                    if (all[i].c[j].w != 0) {
                        line_push(&l, &all[i].c[j]);
                        lw += all[i].c[j].w;
                    }
                if (!all[i].wrapped || i == last)
                    break;
                i++;
            }
            i++;
            while (l.n && !l.c[l.n - 1].n) /* trailing blanks */
                l.n--;
            OUT_ROW();
            lw = 0;
            for (k = 0; k < l.n; k++) {
                int w = l.c[k].w;
                if (col + w > cols) {
                    out[nout - 1].wrapped = true;
                    OUT_ROW();
                    col = 0;
                }
                if (offset >= 0 && !placed && lw >= offset) { /* it moves with its cell */
                    cur_out = nout - 1;
                    cur_col = col;
                    placed = true;
                }
                out[nout - 1].c[col] = l.c[k];
                if (w == 2 && col + 1 < cols)
                    out[nout - 1].c[col + 1].w = 0;
                col += w;
                lw += w;
            }
            if (offset >= 0 && !placed) { /* past the end of the content */
                col += offset - lw;
                while (col >= cols) {
                    out[nout - 1].wrapped = true;
                    OUT_ROW();
                    col -= cols;
                }
                cur_out = nout - 1;
                cur_col = col;
            }
            free(l.c);
        }
    } else {
        for (i = 0; i <= last; i++) {
            OUT_ROW();
            for (j = 0; j < cols && j < all[i].n; j++)
                out[nout - 1].c[j] = all[i].c[j];
            if (j < all[i].n && j > 0 && out[nout - 1].c[j - 1].w == 2)
                clear_cells(&out[nout - 1], j - 1, j); /* a wide character cut in half */
            out[nout - 1].wrapped = all[i].wrapped;
        }
        cur_out = cur;
        cur_col = v->c < cols ? v->c : cols - 1;
    }
#undef OUT_ROW
    for (i = 0; i < total; i++)
        row_free(&all[i]);
    free(all);
    /* the screen ends at the cursor or the last content below it */
    bottom = nout - 1 > cur_out ? nout - 1 : cur_out;
    top = bottom - rows + 1;
    if (top > cur_out)
        top = cur_out; /* the cursor stays on screen; rows below it may not */
    if (top < 0)
        top = 0;
    v->rows = rows;
    v->cols = cols;
    v->screen = (vt_row *)calloc((size_t)rows, sizeof(vt_row));
    for (i = 0; i < top; i++)
        push_sb(v, out[i]);
    for (i = 0; i < rows; i++) {
        if (top + i < nout)
            v->screen[i] = out[top + i];
        else
            row_init(&v->screen[i], cols);
    }
    for (i = top + rows; i < nout; i++)
        row_free(&out[i]);
    free(out);
    v->r = cur_out - top;
    v->c = cur_col;
    v->pending = false;
}
