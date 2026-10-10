/* The input box (section 16.2.8): the draft, editing keys, the kill
 * buffer, history, and the draft's layout in rows. */
#include "tui/tui.h"

#include <string.h>

void tui_input_init(tui_input *in, pt_allocator al)
{
    memset(in, 0, sizeof *in);
    pt_buf_init(&in->text, al);
    pt_buf_init(&in->kill, al);
    pt_buf_init(&in->saved, al);
    in->goal = -1;
}

void tui_input_free(tui_input *in)
{
    uint32_t i;
    pt_allocator al = in->text.al;
    for (i = 0; i < in->nhist; i++)
        pt_buf_free(&in->hist[i]);
    if (in->hist)
        al.fn(al.ud, in->hist, in->hcap * sizeof *in->hist, 0);
    if (in->rows)
        al.fn(al.ud, in->rows, (size_t)in->rcap * sizeof *in->rows, 0);
    pt_buf_free(&in->text);
    pt_buf_free(&in->kill);
    pt_buf_free(&in->saved);
}

static pt_str draft(const tui_input *in)
{
    return pt_buf_str(&in->text);
}

void tui_input_set(tui_input *in, pt_str text)
{
    pt_buf_reset(&in->text);
    pt_buf_puts(&in->text, text);
    in->cur = in->text.n;
    in->goal = -1;
}

static void cut(tui_input *in, size_t from, size_t to, bool keep)
{
    if (to <= from)
        return;
    if (keep) {
        pt_buf_reset(&in->kill);
        pt_buf_put(&in->kill, in->text.p + from, to - from);
    }
    memmove(in->text.p + from, in->text.p + to, in->text.n - to);
    in->text.n -= to - from;
    in->cur = from;
}

void tui_input_insert(tui_input *in, pt_str clean)
{
    size_t n = in->text.n;
    if (!clean.n)
        return;
    pt_buf_put(&in->text, clean.p, clean.n); /* grow, then make room */
    if (in->text.n != n + clean.n)
        return; /* out of memory */
    memmove(in->text.p + in->cur + clean.n, in->text.p + in->cur, n - in->cur);
    memcpy(in->text.p + in->cur, clean.p, clean.n);
    in->cur += clean.n;
    in->goal = -1;
}

static bool word_char(const char *p)
{
    unsigned char c = (unsigned char)*p;
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80;
}

static size_t word_back(const tui_input *in)
{
    size_t i = in->cur;
    while (i > 0 && !word_char(in->text.p + tui_prev_cluster(draft(in), i)))
        i = tui_prev_cluster(draft(in), i);
    while (i > 0 && word_char(in->text.p + tui_prev_cluster(draft(in), i)))
        i = tui_prev_cluster(draft(in), i);
    return i;
}

static size_t word_fwd(const tui_input *in)
{
    size_t i = in->cur;
    while (i < in->text.n && !word_char(in->text.p + i))
        i = tui_next_cluster(draft(in), i);
    while (i < in->text.n && word_char(in->text.p + i))
        i = tui_next_cluster(draft(in), i);
    return i;
}

static size_t line_start(const tui_input *in)
{
    size_t i = in->cur;
    while (i > 0 && in->text.p[i - 1] != '\n')
        i--;
    return i;
}

static size_t line_end(const tui_input *in)
{
    size_t i = in->cur;
    while (i < in->text.n && in->text.p[i] != '\n')
        i++;
    return i;
}

/* The byte offset in row r closest to column col (not past it). */
static size_t at_col(const tui_input *in, int r, int col)
{
    const tui_irow *row = &in->rows[r];
    size_t i = row->start;
    int c = 0;
    while (i < row->end) {
        size_t j = tui_next_cluster(draft(in), i);
        int w = tui_cluster_w(draft(in), i, j, true);
        if (c + w > col)
            break;
        c += w;
        i = j;
    }
    return i;
}

tui_input_result tui_input_key(tui_input *in, const tui_ev *ev, int width)
{
    uint32_t k = ev->key;
    int m = ev->mods;
    bool ctrl = (m & TM_CTRL) != 0, alt = (m & TM_ALT) != 0;
    if (ev->kind == TE_TEXT || ev->kind == TE_PASTE) {
        tui_input_insert(in, ev->text);
        return TI_EDITED;
    }
    if (ev->kind != TE_KEY)
        return TI_IGNORED;
    if (k == TK_UP || k == TK_DOWN) {
        int r;
        tui_input_layout(in, width, 1 << 30);
        r = in->cur_row + (k == TK_UP ? -1 : 1);
        if (r < 0)
            return TI_TOP;
        if (r >= in->nrows)
            return TI_BOTTOM;
        if (in->goal < 0)
            in->goal = in->cur_col;
        in->cur = at_col(in, r, in->goal);
        return TI_MOVED;
    }
    in->goal = -1;
    if ((k == TK_ENTER && (m & (TM_SHIFT | TM_ALT))) || (k == 'j' && ctrl && !alt)) {
        tui_input_insert(in, PT_S("\n"));
        return TI_EDITED;
    }
    if ((k == TK_LEFT && !ctrl && !alt) || (k == 'b' && ctrl && !alt)) {
        in->cur = tui_prev_cluster(draft(in), in->cur);
        return TI_MOVED;
    }
    if ((k == TK_RIGHT && !ctrl && !alt) || (k == 'f' && ctrl && !alt)) {
        in->cur = tui_next_cluster(draft(in), in->cur);
        return TI_MOVED;
    }
    if (((k == TK_LEFT) && (ctrl || alt)) || (k == 'b' && alt && !ctrl)) {
        in->cur = word_back(in);
        return TI_MOVED;
    }
    if (((k == TK_RIGHT) && (ctrl || alt)) || (k == 'f' && alt && !ctrl)) {
        in->cur = word_fwd(in);
        return TI_MOVED;
    }
    if (k == TK_HOME || (k == 'a' && ctrl)) {
        in->cur = line_start(in);
        return TI_MOVED;
    }
    if (k == TK_END || (k == 'e' && ctrl)) {
        in->cur = line_end(in);
        return TI_MOVED;
    }
    if (k == TK_BACKSPACE && !alt && !ctrl) {
        cut(in, tui_prev_cluster(draft(in), in->cur), in->cur, false);
        return TI_EDITED;
    }
    if (k == TK_DELETE || (k == 'd' && ctrl && in->text.n)) {
        cut(in, in->cur, tui_next_cluster(draft(in), in->cur), false);
        return TI_EDITED;
    }
    if ((k == 'w' && ctrl) || (k == TK_BACKSPACE && (alt || ctrl))) {
        cut(in, word_back(in), in->cur, true);
        return TI_EDITED;
    }
    if (k == 'd' && alt && !ctrl) {
        size_t from = in->cur;
        cut(in, from, word_fwd(in), true);
        return TI_EDITED;
    }
    if (k == 'k' && ctrl) {
        size_t e = line_end(in);
        cut(in, in->cur, e == in->cur && e < in->text.n ? e + 1 : e, true);
        return TI_EDITED;
    }
    if (k == 'u' && ctrl) {
        cut(in, line_start(in), in->cur, true);
        return TI_EDITED;
    }
    if (k == 'y' && ctrl) {
        tui_input_insert(in, pt_buf_str(&in->kill));
        return TI_EDITED;
    }
    return TI_IGNORED;
}

/* ---- layout ---- */

static void add_row(tui_input *in, size_t start, size_t end, size_t next)
{
    if (in->nrows == in->rcap) {
        int ncap = in->rcap ? in->rcap * 2 : 16;
        pt_allocator al = in->text.al;
        tui_irow *nr = (tui_irow *)al.fn(al.ud, in->rows, (size_t)in->rcap * sizeof *nr, (size_t)ncap * sizeof *nr);
        if (!nr)
            return;
        in->rows = nr;
        in->rcap = ncap;
    }
    in->rows[in->nrows].start = start;
    in->rows[in->nrows].end = end;
    in->rows[in->nrows].next = next;
    in->nrows++;
}

void tui_input_layout(tui_input *in, int width, int show)
{
    pt_str s = draft(in);
    size_t pos = 0;
    int r;
    if (width < 2)
        width = 2;
    in->nrows = 0;
    for (;;) {
        size_t i = pos, space = 0;
        int w = 0;
        bool word = false;
        while (i < s.n && s.p[i] != '\n') {
            size_t j = tui_next_cluster(s, i);
            int cw = tui_cluster_w(s, i, j, true);
            /* the cursor may sit after the last column, so a full row leaves one */
            if (w + cw > width - 1)
                break;
            if (s.p[i] == ' ' && word)
                space = i;
            else if (s.p[i] != ' ')
                word = true;
            w += cw;
            i = j;
        }
        if (i >= s.n) {
            add_row(in, pos, s.n, s.n);
            break;
        }
        if (s.p[i] == '\n') {
            add_row(in, pos, i, i + 1);
        } else if (s.p[i] == ' ' && word) { /* the break falls on a space: it goes */
            add_row(in, pos, i, i + 1);
        } else if (space) { /* break after the space: it stays on this row */
            add_row(in, pos, space + 1, space + 1);
        } else {
            add_row(in, pos, i > pos ? i : tui_next_cluster(s, pos), i > pos ? i : tui_next_cluster(s, pos));
        }
        pos = in->rows[in->nrows - 1].next;
    }
    /* the cursor: on the last row that starts at or before it */
    in->cur_row = 0;
    for (r = in->nrows - 1; r > 0; r--)
        if (in->rows[r].start <= in->cur) {
            in->cur_row = r;
            break;
        }
    in->cur_col = tui_width((pt_str){s.p + in->rows[in->cur_row].start, in->cur - in->rows[in->cur_row].start}, true);
    if (in->cur_row < in->scroll)
        in->scroll = in->cur_row;
    if (in->cur_row >= in->scroll + show)
        in->scroll = in->cur_row - show + 1;
    if (in->scroll > in->nrows - show)
        in->scroll = in->nrows - show > 0 ? in->nrows - show : 0;
    if (in->scroll < 0)
        in->scroll = 0;
}

/* ---- history ---- */

void tui_hist_add(tui_input *in, pt_str entry)
{
    pt_allocator al = in->text.al;
    if (!entry.n || (in->nhist && pt_str_eq(pt_buf_str(&in->hist[in->nhist - 1]), entry))) {
        in->hpos = in->nhist;
        return;
    }
    if (in->nhist == in->hcap) {
        uint32_t ncap = in->hcap ? in->hcap * 2 : 64;
        pt_buf *nh = (pt_buf *)al.fn(al.ud, in->hist, in->hcap * sizeof *nh, ncap * sizeof *nh);
        if (!nh)
            return;
        in->hist = nh;
        in->hcap = ncap;
    }
    pt_buf_init(&in->hist[in->nhist], al);
    pt_buf_puts(&in->hist[in->nhist], entry);
    in->nhist++;
    in->hpos = in->nhist;
}

bool tui_hist_move(tui_input *in, int dir)
{
    uint32_t to;
    if (dir < 0 ? in->hpos == 0 : in->hpos >= in->nhist)
        return false;
    if (in->hpos == in->nhist) { /* leaving the draft: keep it */
        pt_buf_reset(&in->saved);
        pt_buf_puts(&in->saved, draft(in));
    }
    to = dir < 0 ? in->hpos - 1 : in->hpos + 1;
    in->hpos = to;
    tui_input_set(in, to == in->nhist ? pt_buf_str(&in->saved) : pt_buf_str(&in->hist[to]));
    if (dir < 0) /* older: the cursor at the end of the first row, so ↑ goes on */
        in->cur = (size_t)(pt_str_find(draft(in), '\n') >= 0 ? pt_str_find(draft(in), '\n') : (long)in->text.n);
    return true;
}

void tui_hist_line(pt_buf *out, pt_str entry)
{
    pt_json_quote(out, entry);
    pt_buf_putc(out, '\n');
}

void tui_hist_load(tui_input *in, pt_str file, uint32_t max)
{
    size_t at = 0, start;
    uint32_t lines = 0, skip;
    pt_arena ar;
    for (start = 0; start < file.n; start++)
        if (file.p[start] == '\n')
            lines++;
    skip = lines > max ? lines - max : 0;
    pt_arena_init(&ar, in->text.al, 4096);
    while (at < file.n) {
        size_t e = at;
        pt_json *v;
        while (e < file.n && file.p[e] != '\n')
            e++;
        if (skip) {
            skip--;
        } else if ((v = pt_json_parse((pt_str){file.p + at, e - at}, &ar, NULL)) && pt_json_is(v, PT_J_STR)) {
            pt_buf clean;
            int col = 0;
            pt_buf_init(&clean, in->text.al);
            tui_clean(&clean, pt_json_str(v, &ar), &col);
            tui_hist_add(in, pt_buf_str(&clean));
            pt_buf_free(&clean);
        }
        pt_arena_reset(&ar);
        at = e + 1;
    }
    pt_arena_free(&ar);
    in->hpos = in->nhist;
}
