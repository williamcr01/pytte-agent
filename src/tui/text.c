/* Text for the terminal: UTF-8, display widths, clusters, sanitizing,
 * styled text and wrapping (section 16.2.4). */
#include "tui/tui.h"

#include "tui/width.gen.inc"

enum { CLS_BASE, CLS_EXTEND, CLS_ZWJ, CLS_RI };

/* ---- UTF-8 ---- */

size_t tui_utf8_dec(const char *p, size_t n, uint32_t *cp)
{
    const unsigned char *s = (const unsigned char *)p;
    unsigned c = s[0], lo = 0x80, hi = 0xBF;
    size_t need, i;
    uint32_t v;
    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    if (c >= 0xC2 && c <= 0xDF) {
        need = 1;
        v = c & 0x1F;
    } else if (c >= 0xE0 && c <= 0xEF) {
        need = 2;
        v = c & 0x0F;
        if (c == 0xE0)
            lo = 0xA0; /* overlong */
        if (c == 0xED)
            hi = 0x9F; /* surrogates */
    } else if (c >= 0xF0 && c <= 0xF4) {
        need = 3;
        v = c & 0x07;
        if (c == 0xF0)
            lo = 0x90;
        if (c == 0xF4)
            hi = 0x8F; /* above U+10FFFF */
    } else {
        *cp = TUI_REPLACEMENT;
        return 1;
    }
    for (i = 1; i <= need; i++) {
        unsigned b;
        if (i >= n || (b = s[i]) < lo || b > hi) {
            *cp = TUI_REPLACEMENT; /* the maximal bad prefix */
            return i;
        }
        lo = 0x80;
        hi = 0xBF;
        v = v << 6 | (b & 0x3F);
    }
    *cp = v;
    return need + 1;
}

size_t tui_utf8_enc(uint32_t cp, char out[4])
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | cp >> 6);
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | cp >> 12);
        out[1] = (char)(0x80 | (cp >> 6 & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | cp >> 18);
    out[1] = (char)(0x80 | (cp >> 12 & 0x3F));
    out[2] = (char)(0x80 | (cp >> 6 & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* ---- widths ---- */

static bool amb_wide;

void tui_set_ambiguous_wide(bool wide)
{
    amb_wide = wide;
}

bool tui_ambiguous_wide(void) { return amb_wide; }

/* bits: best (0-1), max (2-3), class (4-5), pictographic (6), min (7-8),
 * East Asian ambiguous (9) */
static unsigned props(uint32_t cp)
{
    size_t lo = 0, hi = sizeof tui_wtab / sizeof *tui_wtab;
    if (cp >= 0x20 && cp < 0x7F)
        return 1 | 1 << 2 | 1 << 7;
    if (cp >= 0x110000)
        return 1 | 2 << 2 | 1 << 7;
    while (hi - lo > 1) { /* the last entry starting at or before cp */
        size_t mid = (lo + hi) / 2;
        if ((tui_wtab[mid] & 0x1FFFFF) <= cp)
            lo = mid;
        else
            hi = mid;
    }
    if (amb_wide && tui_wtab[lo] >> 30 & 1)
        return (tui_wtab[lo] >> 21 & 0x70) | 2 | 2 << 2 | 2 << 7;
    return tui_wtab[lo] >> 21;
}

int tui_cp_best(uint32_t cp)
{
    return (int)(props(cp) & 3);
}

int tui_cp_max(uint32_t cp)
{
    return (int)(props(cp) >> 2 & 3);
}

static int cls(unsigned p)
{
    return (int)(p >> 4 & 3);
}

/* ---- clusters ---- */

size_t tui_next_cluster(pt_str s, size_t i)
{
    uint32_t cp;
    size_t j;
    bool joining, ri;
    unsigned p;
    if (i >= s.n)
        return s.n;
    j = i + tui_utf8_dec(s.p + i, s.n - i, &cp);
    if (cp < 0x20)
        return j; /* newlines and style switches stand alone */
    p = props(cp);
    joining = cls(p) == CLS_ZWJ;
    ri = cls(p) == CLS_RI;
    while (j < s.n) {
        size_t k = j + tui_utf8_dec(s.p + j, s.n - j, &cp);
        int c;
        if (cp < 0x20)
            break;
        c = cls(props(cp));
        if (joining) /* the part after a ZWJ joins the cluster */
            joining = c == CLS_ZWJ;
        else if (c == CLS_ZWJ)
            joining = true;
        else if (ri && c == CLS_RI)
            ri = false; /* a flag is a pair */
        else if (c != CLS_EXTEND)
            break;
        ri = ri && c == CLS_RI;
        j = k;
    }
    return j;
}

size_t tui_prev_cluster(pt_str s, size_t i)
{
    size_t start = i, prev;
    if (i == 0)
        return 0;
    /* Two adjacent ASCII bytes cannot hide a Unicode joiner before the
     * last character. Keep the full forward walk for mixed clusters. */
    if ((unsigned char)s.p[i - 1] < 0x80 &&
        ((unsigned char)s.p[i - 1] < 0x20 || i < 2 || (unsigned char)s.p[i - 2] < 0x80))
        return i - 1;
    /* From the start of the line, which is always a boundary. */
    while (start > 0 && s.p[start - 1] != '\n')
        start--;
    if (start == i)
        return i - 1; /* the newline itself */
    prev = start;
    while (start < i) {
        prev = start;
        start = tui_next_cluster(s, start);
    }
    return prev;
}

int tui_cluster_wmin(pt_str s, size_t i, size_t j)
{
    int w = 0;
    bool first = true;
    while (i < j) {
        uint32_t cp;
        unsigned p;
        i += tui_utf8_dec(s.p + i, j - i, &cp);
        p = props(cp);
        if (cls(p) == CLS_ZWJ)
            break;
        if (first || !(cls(p) == CLS_EXTEND && (p & 3) == 2))
            w += (int)(p >> 7 & 3);
        first = false;
    }
    return w;
}

int tui_cluster_w(pt_str s, size_t i, size_t j, bool max)
{
    int best = 0, mx = 0, ris = 0;
    bool joined = false, vs16 = false, pict = false, first = true;
    while (i < j) {
        uint32_t cp;
        unsigned p;
        i += tui_utf8_dec(s.p + i, j - i, &cp);
        p = props(cp);
        if (first)
            pict = (p >> 6 & 1) != 0;
        else if (cls(p) == CLS_EXTEND && (p & 3) == 2)
            p &= ~3u; /* an emoji modifier: part of the base's two columns */
        first = false;
        if (cp == 0xFE0F)
            vs16 = true;
        if (cls(p) == CLS_ZWJ)
            joined = true;
        if (cls(p) == CLS_RI)
            ris++;
        if (!joined)
            best += (int)(p & 3);
        mx += (int)(p >> 2 & 3);
    }
    if ((vs16 && pict) || ris == 2) {
        best = 2;
        if (mx < 2)
            mx = 2;
    }
    return max ? mx : best;
}

/* ---- layout ---- */

/* The width of the code point at s[i] when placed alone, at its least:
 * parts after a ZWJ and emoji modifiers take nothing. *joined carries the
 * ZWJ state from one code point to the next. */
static int cp_min(pt_str s, size_t *i, bool *joined)
{
    uint32_t cp;
    unsigned p;
    int w;
    *i += tui_utf8_dec(s.p + *i, s.n - *i, &cp);
    p = props(cp);
    w = *joined || (cls(p) == CLS_EXTEND && (p & 3) == 2) ? 0 : (int)(p >> 7 & 3);
    *joined = cls(p) == CLS_ZWJ;
    return w;
}

int tui_rows(pt_str s, int cols, bool max, int *endcol)
{
    size_t i = 0;
    int rows = 1, col = 0;
    bool joined = false;
    while (i < s.n) {
        size_t j = i;
        int w;
        if (s.p[i] == TUI_STY) {
            i += 2;
            continue;
        }
        if (max) {
            j = tui_next_cluster(s, i);
            w = tui_cluster_w(s, i, j, true);
        } else {
            w = cp_min(s, &j, &joined);
        }
        if (col + w > cols && col > 0) {
            rows++;
            col = 0;
        }
        col += w;
        i = j;
    }
    *endcol = col;
    return rows;
}

int tui_next_w(pt_str s, size_t i, bool max)
{
    bool joined = false;
    while (i < s.n && s.p[i] == TUI_STY)
        i += 2;
    if (i >= s.n)
        return 0;
    if (max)
        return tui_cluster_w(s, i, tui_next_cluster(s, i), true);
    return cp_min(s, &i, &joined);
}

/* ---- sanitizing ---- */

void tui_clean(pt_buf *out, pt_str in, int *col)
{
    size_t i = 0;
    while (i < in.n) {
        unsigned char c = (unsigned char)in.p[i];
        uint32_t cp;
        char enc[4];
        size_t used;
        if (c < 0x80) {
            i++;
            if (c == '\n') {
                pt_buf_putc(out, '\n');
                *col = 0;
            } else if (c == '\t') {
                do
                    pt_buf_putc(out, ' ');
                while (++*col % 4);
            } else if (c >= 0x20 && c != 0x7F) {
                pt_buf_putc(out, (char)c);
                ++*col;
            } /* other controls, \r included, are dropped */
            continue;
        }
        used = tui_utf8_dec(in.p + i, in.n - i, &cp);
        i += used;
        if (cp <= 0x9F || (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2066 && cp <= 0x2069))
            continue; /* C1, bidi overrides and isolates */
        pt_buf_put(out, enc, tui_utf8_enc(cp, enc));
        *col += tui_cp_best(cp);
    }
}

/* ---- styled text ---- */

void tui_style(pt_buf *b, int style)
{
    pt_buf_putc(b, TUI_STY);
    pt_buf_putc(b, (char)(0x20 + style));
}

int tui_style_at(pt_str s, int style)
{
    size_t i;
    for (i = 0; i + 1 < s.n; i++)
        if (s.p[i] == TUI_STY)
            style = (unsigned char)s.p[++i] - 0x20;
    return style;
}

int tui_width(pt_str s, bool max)
{
    size_t i = 0;
    int w = 0;
    while (i < s.n) {
        size_t j;
        if (s.p[i] == TUI_STY) {
            i += 2;
            continue;
        }
        j = tui_next_cluster(s, i);
        w += tui_cluster_w(s, i, j, max);
        i = j;
    }
    return w;
}

size_t tui_fit(pt_str s, int cols, bool max)
{
    size_t i = 0;
    int w = 0;
    while (i < s.n) {
        size_t j;
        int cw;
        if (s.p[i] == TUI_STY) {
            i += 2;
            continue;
        }
        j = tui_next_cluster(s, i);
        cw = tui_cluster_w(s, i, j, max);
        if (w + cw > cols)
            break;
        w += cw;
        i = j;
    }
    return i > s.n ? s.n : i;
}

static void put_row(pt_buf *out, pt_str s, size_t from, size_t to, int indent, int style)
{
    while (indent-- > 0)
        pt_buf_putc(out, ' ');
    if (style)
        tui_style(out, style);
    pt_buf_put(out, s.p + from, to - from);
    pt_buf_putc(out, '\n');
}

int tui_wrap(pt_buf *out, pt_str s, int cols, int indent, bool words, bool max)
{
    size_t pos = 0;
    int rows = 0, style = 0;
    for (;;) {
        int avail = cols - (rows ? indent : 0), w = 0;
        size_t i = pos, space = 0, end, next;
        bool word = false; /* a break needs a word before it, not just indentation */
        if (avail < 1)
            avail = 1;
        while (i < s.n) {
            size_t j;
            int cw;
            if (s.p[i] == TUI_STY) {
                i += 2;
                continue;
            }
            j = tui_next_cluster(s, i);
            cw = tui_cluster_w(s, i, j, max);
            if (w + cw > avail)
                break;
            if (s.p[i] != ' ')
                word = true;
            else if (words && word)
                space = i;
            w += cw;
            i = j;
        }
        if (i >= s.n) {
            put_row(out, s, pos, s.n, rows ? indent : 0, style);
            return rows + 1;
        }
        if (words && word && s.p[i] == ' ')
            end = i;
        else if (words && space)
            end = space;
        else
            end = i > pos ? i : tui_next_cluster(s, pos); /* at least one cluster */
        next = end;
        while (words && next < s.n && s.p[next] == ' ')
            next++;
        put_row(out, s, pos, end, rows ? indent : 0, style);
        rows++;
        style = tui_style_at((pt_str){s.p + pos, next - pos}, style);
        pos = next;
        if (pos >= s.n)
            return rows;
    }
}

void tui_sgr(pt_buf *out, pt_str s, bool color)
{
    static const char *const colors[] = {"", ";31", ";32", ";36", ";35", ";34", "", ""};
    size_t i = 0, run = 0;
    while (i < s.n) {
        int st;
        if (s.p[i] != TUI_STY || i + 1 >= s.n) {
            i++;
            continue;
        }
        pt_buf_put(out, s.p + run, i - run);
        st = (unsigned char)s.p[i + 1] - 0x20;
        pt_buf_putz(out, "\x1b[0");
        if (st & TUI_BOLD)
            pt_buf_putz(out, ";1");
        if (st & TUI_DIM)
            pt_buf_putz(out, ";2");
        if (color)
            pt_buf_putz(out, colors[(st & TUI_COLOR_MASK) >> 2]);
        pt_buf_putc(out, 'm');
        i += 2;
        run = i;
    }
    pt_buf_put(out, s.p + run, s.n - run);
}
