/* The input box (section 16.2.8): the draft, editing keys, the kill
 * buffer, history, and the draft's layout in rows. */
#include "tui/tui.h"

#include <string.h>
#include <limits.h>

void tui_input_init(tui_input *in, pt_allocator al)
{
    memset(in, 0, sizeof *in);
    pt_buf_init(&in->text, al);
    pt_buf_init(&in->kill, al);
    pt_buf_init(&in->saved, al);
    pt_buf_init(&in->history_data, al);
    in->goal = -1;
}

void tui_input_free(tui_input *in)
{
    pt_allocator al = in->text.al;
    if (in->hist)
        al.fn(al.ud, in->hist, in->hcap * sizeof *in->hist, 0);
    if (in->rows)
        al.fn(al.ud, in->rows, (size_t)in->rcap * sizeof *in->rows, 0);
    pt_buf_free(&in->text);
    pt_buf_free(&in->kill);
    pt_buf_free(&in->saved);
    pt_buf_free(&in->history_data);
}

static pt_str draft(const tui_input *in)
{
    return pt_buf_str(&in->text);
}

void tui_input_set(tui_input *in, pt_str text)
{
    size_t old = in->text.n;
    pt_buf_reset(&in->text);
    if (!pt_buf_reserve(&in->text, text.n)) {
        in->text.n = old;
        return;
    }
    if (text.n)
        memmove(in->text.p, text.p, text.n);
    in->text.n = text.n;
    in->cur = in->text.n;
    in->goal = -1;
    in->layout_valid = false;
}

static void edited(tui_input *in, size_t at)
{
    /* next_cluster looks ahead by one UTF-8 scalar. Invalidating four
     * bytes earlier also covers inserting an extending/joining character. */
    at = at > 4 ? at - 4 : 0;
    if (at < in->dirty_from)
        in->dirty_from = at;
}

static void cut(tui_input *in, size_t from, size_t to, bool keep)
{
    if (to <= from)
        return;
    if (keep) {
        pt_buf_reset(&in->kill);
        pt_buf_put(&in->kill, in->text.p + from, to - from);
        if (in->kill.oom)
            return;
    }
    edited(in, from);
    memmove(in->text.p + from, in->text.p + to, in->text.n - to);
    in->text.n -= to - from;
    in->cur = from;
}

void tui_input_insert(tui_input *in, pt_str clean)
{
    size_t n = in->text.n;
    size_t alias = 0;
    bool own = false;
    if (!clean.n)
        return;
    if (in->text.p && (uintptr_t)clean.p >= (uintptr_t)in->text.p) {
        uintptr_t off = (uintptr_t)clean.p - (uintptr_t)in->text.p;
        if (off <= n && clean.n <= n - (size_t)off) {
            alias = (size_t)off;
            own = true;
        }
    }
    if (!pt_buf_reserve(&in->text, clean.n))
        return; /* out of memory */
    edited(in, in->cur);
    memmove(in->text.p + in->cur + clean.n, in->text.p + in->cur, n - in->cur);
    if (!own) {
        memcpy(in->text.p + in->cur, clean.p, clean.n);
    } else if (alias >= in->cur) {
        memmove(in->text.p + in->cur, in->text.p + alias + clean.n, clean.n);
    } else {
        size_t left = in->cur - alias;
        if (left > clean.n)
            left = clean.n;
        if (left < clean.n)
            memmove(in->text.p + in->cur + left, in->text.p + in->cur + clean.n, clean.n - left);
        memmove(in->text.p + in->cur, in->text.p + alias, left);
    }
    in->text.n = n + clean.n;
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
        if (!tui_input_layout(in, width, 1 << 30))
            return TI_IGNORED;
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
    if ((k == TK_ENTER && (m & TM_SHIFT)) || (k == 'j' && ctrl && !alt)) {
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

static bool add_row(tui_input *in, size_t start, size_t end, size_t examined)
{
    if (in->nrows == in->rcap) {
        int ncap;
        pt_allocator al = in->text.al;
        tui_irow *nr;
        if (in->rcap > INT_MAX / 2)
            goto fail;
        ncap = in->rcap ? in->rcap * 2 : 4;
        if (SIZE_MAX / (size_t)ncap < sizeof *nr)
            goto fail;
        nr = (tui_irow *)al.fn(al.ud, in->rows, (size_t)in->rcap * sizeof *nr, (size_t)ncap * sizeof *nr);
        if (!nr)
            goto fail;
        in->rows = nr;
        in->rcap = ncap;
    }
    in->rows[in->nrows].start = start;
    in->rows[in->nrows].end = end;
    in->rows[in->nrows].examined = examined;
    in->nrows++;
    return true;
fail:
    in->text.oom = true;
    in->layout_valid = false;
    return false;
}

bool tui_input_layout(tui_input *in, int width, int show)
{
    pt_str s = draft(in);
    size_t pos = 0;
    int lo, hi;
    bool wrap = true;
    if (width < 2)
        width = 2;
    if (show < 1)
        show = 1;
    if (!in->layout_valid || in->layout_width != width ||
        in->layout_ambiguous != tui_ambiguous_wide()) {
        in->nrows = 0;
    } else if (in->dirty_from == SIZE_MAX) {
        wrap = false;
    } else {
        /* Examination endpoints are nondecreasing. Everything before the
         * first row that examined an edited byte remains a valid prefix,
         * even when word wrapping had looked beyond that row's end. */
        lo = 0;
        hi = in->nrows;
        while (lo < hi) {
            int mid = lo + (hi - lo) / 2;
            if (in->rows[mid].examined < in->dirty_from)
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo == in->nrows)
            lo--;
        pos = in->rows[lo].start;
        in->nrows = lo;
    }
    while (wrap) {
        size_t i = pos, space = 0, examined = pos, next;
        int w = 0;
        bool word = false;
        while (i < s.n && s.p[i] != '\n') {
            size_t j = tui_next_cluster(s, i);
            int cw = tui_cluster_w(s, i, j, true);
            examined = j;
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
            if (!add_row(in, pos, s.n, s.n))
                return false;
            break;
        }
        if (examined < i + 1)
            examined = i + 1;
        if (s.p[i] == '\n') {
            next = i + 1;
            if (!add_row(in, pos, i, examined))
                return false;
        } else if (s.p[i] == ' ' && word) { /* the break falls on a space: it goes */
            next = i + 1;
            if (!add_row(in, pos, i, examined))
                return false;
        } else if (space) { /* break after the space: it stays on this row */
            next = space + 1;
            if (!add_row(in, pos, next, examined))
                return false;
        } else {
            next = i > pos ? i : tui_next_cluster(s, pos);
            if (!add_row(in, pos, next, examined))
                return false;
        }
        pos = next;
    }
    in->layout_valid = true;
    in->layout_width = width;
    in->layout_ambiguous = tui_ambiguous_wide();
    in->dirty_from = SIZE_MAX;
    /* the cursor: on the last row that starts at or before it */
    lo = 0;
    hi = in->nrows;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (in->rows[mid].start <= in->cur)
            lo = mid + 1;
        else
            hi = mid;
    }
    in->cur_row = lo ? lo - 1 : 0;
    in->cur_col = tui_width((pt_str){s.p + in->rows[in->cur_row].start, in->cur - in->rows[in->cur_row].start}, true);
    if (in->cur_row < in->scroll)
        in->scroll = in->cur_row;
    if (in->cur_row >= in->scroll + show)
        in->scroll = in->cur_row - show + 1;
    if (in->scroll > in->nrows - show)
        in->scroll = in->nrows - show > 0 ? in->nrows - show : 0;
    if (in->scroll < 0)
        in->scroll = 0;
    return true;
}

/* ---- history ---- */

pt_str tui_hist_at(const tui_input *in, uint32_t index)
{
    if (index >= in->nhist)
        return PT_STR_NULL;
    return (pt_str){in->history_data.p + in->hist[index].at, in->hist[index].n};
}

static bool hist_reserve(tui_input *in, uint32_t count)
{
    pt_allocator al = in->text.al;
    if (count > in->hcap) {
        uint32_t ncap = in->hcap ? in->hcap : 64;
        tui_hentry *nh;
        while (ncap < count) {
            if (ncap > UINT32_MAX / 2)
                return false;
            ncap *= 2;
        }
        if (SIZE_MAX / (size_t)ncap < sizeof *nh)
            return false;
        nh = (tui_hentry *)al.fn(al.ud, in->hist, (size_t)in->hcap * sizeof *nh,
                                (size_t)ncap * sizeof *nh);
        if (!nh)
            return false;
        in->hist = nh;
        in->hcap = ncap;
    }
    return true;
}

bool tui_hist_add(tui_input *in, pt_str entry)
{
    size_t at = in->history_data.n;
    size_t offset = 0;
    bool own = false;
    if (!entry.n || (in->nhist && pt_str_eq(tui_hist_at(in, in->nhist - 1), entry))) {
        in->hpos = in->nhist;
        return true;
    }
    if (in->nhist == UINT32_MAX || !hist_reserve(in, in->nhist + 1))
        return false;
    if (in->history_data.p && (uintptr_t)entry.p >= (uintptr_t)in->history_data.p) {
        uintptr_t off = (uintptr_t)entry.p - (uintptr_t)in->history_data.p;
        if (off <= at && entry.n <= at - (size_t)off) {
            offset = (size_t)off;
            own = true;
        }
    }
    if (!pt_buf_reserve(&in->history_data, entry.n))
        return false;
    if (own)
        entry.p = in->history_data.p + offset;
    memcpy(in->history_data.p + at, entry.p, entry.n);
    in->history_data.n += entry.n;
    in->hist[in->nhist].at = at;
    in->hist[in->nhist].n = entry.n;
    in->nhist++;
    in->hpos = in->nhist;
    return true;
}

bool tui_hist_move(tui_input *in, int dir)
{
    uint32_t to;
    if (dir < 0 ? in->hpos == 0 : in->hpos >= in->nhist)
        return false;
    if (in->hpos == in->nhist) { /* leaving the draft: keep it */
        pt_buf_reset(&in->saved);
        pt_buf_puts(&in->saved, draft(in));
        if (in->saved.oom)
            return false;
    }
    to = dir < 0 ? in->hpos - 1 : in->hpos + 1;
    in->hpos = to;
    tui_input_set(in, to == in->nhist ? pt_buf_str(&in->saved) : tui_hist_at(in, to));
    if (dir < 0) /* older: the cursor at the end of the first row, so ↑ goes on */
        in->cur = (size_t)(pt_str_find(draft(in), '\n') >= 0 ? pt_str_find(draft(in), '\n') : (long)in->text.n);
    return true;
}

void tui_hist_line(pt_buf *out, pt_str entry)
{
    pt_json_quote(out, entry);
    pt_buf_putc(out, '\n');
}

bool tui_hist_load(tui_input *in, pt_str file, uint32_t max)
{
    size_t at = 0, start;
    size_t lines = 0, skip;
    pt_arena ar;
    pt_buf clean;
    bool ok = true;
    for (start = 0; start < file.n; start++)
        if (file.p[start] == '\n')
            lines++;
    if (file.n && file.p[file.n - 1] != '\n')
        lines++;
    skip = lines > max ? lines - max : 0;
    if (lines - skip > UINT32_MAX - in->nhist ||
        !hist_reserve(in, in->nhist + (uint32_t)(lines - skip)))
        return false;
    while (skip && at < file.n)
        skip -= file.p[at++] == '\n';
    if (!pt_buf_reserve(&in->history_data, file.n - at))
        return false;
    pt_arena_init(&ar, in->text.al, 4096);
    pt_buf_init(&clean, in->text.al);
    while (at < file.n) {
        size_t e = at;
        pt_json *v;
        while (e < file.n && file.p[e] != '\n')
            e++;
        if ((v = pt_json_parse((pt_str){file.p + at, e - at}, &ar, NULL)) && pt_json_is(v, PT_J_STR)) {
            int col = 0;
            pt_buf_reset(&clean);
            tui_clean(&clean, pt_json_str(v, &ar), &col);
            if (clean.oom || ar.oom || !tui_hist_add(in, pt_buf_str(&clean))) {
                ok = false;
                break;
            }
        }
        if (ar.oom) {
            ok = false;
            break;
        }
        pt_arena_reset(&ar);
        at = e + 1;
    }
    pt_arena_free(&ar);
    pt_buf_free(&clean);
    in->hpos = in->nhist;
    return ok;
}
