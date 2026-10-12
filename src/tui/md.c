/* Markdown, one source line at a time (section 16.2.5). Lines are complete
 * when rendered, so nothing printed ever needs to be taken back; the only
 * state between lines is whether a fenced code block is open.
 *
 * Shown: headings bold; paragraphs word-wrapped; lists with a bullet and a
 * hanging indent; quotes with a bar; rules; fences dim; code lines as they
 * are. Inline, within one line: **strong** bold, *em* and _em_ without the
 * markers (no italics, R9), `code` cyan, [text](url) as text and a dim URL.
 * An unclosed delimiter stays as it is. */
#include "tui/tui.h"

#include <string.h>

static bool is_space(char c)
{
    return c == ' ' || c == '\t';
}

static bool is_alnum(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (unsigned char)c >= 0x80;
}

static bool is_punct(char c)
{
    return (c > ' ' && c < '0') || (c > '9' && c < 'A') || (c > 'Z' && c < 'a') || (c > 'z' && c <= '~');
}

/* Where the closing delimiter d (n characters) is, from i; -1 if none. A
 * closer follows a non-space; for '_' it must not be inside a word. */
static long closer(pt_str s, size_t i, const char *d, size_t n)
{
    for (; i + n <= s.n; i++) {
        if (s.p[i] == '\\') {
            i++;
            continue;
        }
        if (memcmp(s.p + i, d, n) != 0 || is_space(s.p[i - 1]))
            continue;
        if (d[0] == '_' && i + n < s.n && is_alnum(s.p[i + n]))
            continue;
        if (n == 1 && i + 1 < s.n && s.p[i + 1] == d[0])
            continue; /* part of a ** run */
        return (long)i;
    }
    return -1;
}

static void inline_md(pt_buf *out, pt_str s, int base)
{
    size_t i = 0;
    while (i < s.n) {
        char c = s.p[i];
        if (c == '\\' && i + 1 < s.n && is_punct(s.p[i + 1])) {
            pt_buf_putc(out, s.p[i + 1]);
            i += 2;
            continue;
        }
        if (c == '`') {
            size_t run = 0, j;
            while (i + run < s.n && s.p[i + run] == '`')
                run++;
            for (j = i + run; j + run <= s.n; j++) { /* a closing run of the same length */
                size_t k = 0;
                while (j + k < s.n && s.p[j + k] == '`')
                    k++;
                if (k == run)
                    break;
                if (k)
                    j += k - 1;
            }
            if (j + run <= s.n) {
                tui_style(out, (base & ~TUI_COLOR_MASK) | TUI_CYAN);
                pt_buf_put(out, s.p + i + run, j - i - run);
                tui_style(out, base);
                i = j + run;
            } else {
                pt_buf_put(out, s.p + i, run);
                i += run;
            }
            continue;
        }
        if ((c == '*' || c == '_') && i + 2 < s.n && s.p[i + 1] == c && !is_space(s.p[i + 2]) &&
            (c == '*' || i == 0 || !is_alnum(s.p[i - 1]))) {
            char d[2];
            long e;
            d[0] = d[1] = c;
            e = closer(s, i + 2, d, 2);
            if (e > (long)i + 2) {
                tui_style(out, base | TUI_BOLD);
                inline_md(out, (pt_str){s.p + i + 2, (size_t)e - i - 2}, base | TUI_BOLD);
                tui_style(out, base);
                i = (size_t)e + 2;
                continue;
            }
        }
        if ((c == '*' || c == '_') && i + 1 < s.n && !is_space(s.p[i + 1]) && s.p[i + 1] != c &&
            (c == '*' || i == 0 || !is_alnum(s.p[i - 1]))) {
            long e = closer(s, i + 1, &c, 1);
            if (e > (long)i + 1) { /* emphasis: the markers go, no style (R9) */
                inline_md(out, (pt_str){s.p + i + 1, (size_t)e - i - 1}, base);
                i = (size_t)e + 1;
                continue;
            }
        }
        if (c == '[') {
            long mid = pt_str_find((pt_str){s.p + i, s.n - i}, ']');
            if (mid > 0 && i + (size_t)mid + 1 < s.n && s.p[i + mid + 1] == '(') {
                pt_str text = {s.p + i + 1, (size_t)mid - 1}, rest = {s.p + i + mid + 2, s.n - i - mid - 2};
                long end = pt_str_find(rest, ')');
                if (end >= 0) {
                    pt_str url = {rest.p, (size_t)end};
                    inline_md(out, text, base);
                    if (!pt_str_eq(url, text) && url.n) {
                        pt_buf_putc(out, ' ');
                        tui_style(out, base | TUI_DIM);
                        pt_buf_puts(out, url);
                        tui_style(out, base);
                    }
                    i = (size_t)(rest.p - s.p) + (size_t)end + 1;
                    continue;
                }
            }
        }
        pt_buf_putc(out, c);
        i++;
    }
}

/* Leading spaces (columns; tabs are already expanded). */
static size_t indent_of(pt_str s)
{
    size_t i = 0;
    while (i < s.n && s.p[i] == ' ')
        i++;
    return i;
}

/* A fence: 3+ backticks or tildes after at most 3 spaces. */
static bool fence(pt_str s, char *ch, int *len)
{
    size_t i = indent_of(s), n = 0;
    if (i > 3 || i >= s.n || (s.p[i] != '`' && s.p[i] != '~'))
        return false;
    while (i + n < s.n && s.p[i + n] == s.p[i])
        n++;
    if (n < 3)
        return false;
    if (s.p[i] == '`' && pt_str_find((pt_str){s.p + i + n, s.n - i - n}, '`') >= 0)
        return false; /* a backtick fence's info string has no backticks */
    *ch = s.p[i];
    *len = (int)n;
    return true;
}

static bool closing_fence(const tui_md *m, pt_str s)
{
    char ch;
    int len;
    size_t i;
    if (!fence(s, &ch, &len) || ch != m->fch || len < m->flen)
        return false;
    for (i = indent_of(s) + (size_t)len; i < s.n; i++)
        if (!is_space(s.p[i]))
            return false;
    return true;
}

static bool rule(pt_str s)
{
    size_t i = indent_of(s);
    int n = 0;
    char c;
    if (i > 3 || i >= s.n)
        return false;
    c = s.p[i];
    if (c != '-' && c != '*' && c != '_')
        return false;
    for (; i < s.n; i++) {
        if (s.p[i] == c)
            n++;
        else if (!is_space(s.p[i]))
            return false;
    }
    return n >= 3;
}

/* Styled text wrapped at width; rows after the first start with cont (both
 * prefixes are styled and of the same width). */
static int emit(pt_buf *out, pt_str first, pt_str cont, int pw, pt_str text, int width, bool band, bool words)
{
    pt_buf rows;
    size_t at = 0;
    int n = 0;
    pt_buf_init(&rows, out->al);
    tui_wrap(&rows, text, width - pw > 1 ? width - pw : 1, 0, words, band);
    if (rows.oom) {
        pt_buf_free(&rows);
        return -1;
    }
    while (at < rows.n) {
        size_t e = at;
        while (e < rows.n && rows.p[e] != '\n')
            e++;
        pt_buf_puts(out, n ? cont : first);
        pt_buf_put(out, rows.p + at, e - at);
        pt_buf_putc(out, '\n'); /* tui_wrap may leave its final row unterminated */
        at = e < rows.n ? e + 1 : e;
        n++;
    }
    pt_buf_free(&rows);
    return out->oom ? -1 : n;
}

static const char spaces[] = "        "; /* margins */

static int plain_row(pt_buf *out, int margin, pt_str s, int base)
{
    if (s.n)
        pt_buf_put(out, spaces, (size_t)margin);
    if (base)
        tui_style(out, base);
    pt_buf_puts(out, s);
    pt_buf_putc(out, '\n');
    return 1;
}

int tui_md_line(tui_md *m, pt_str line, int cols, int base, bool band, pt_buf *out)
{
    int width = cols > 2 ? cols - 1 : 1, rows, mg = m->margin < 0 ? 0 : m->margin > 8 ? 8 : m->margin;
    pt_str margin = {spaces, (size_t)mg};
    size_t ind;
    char ch;
    int len;
    pt_buf t, p1, p2;
    if (m->code) {
        if (closing_fence(m, line)) {
            m->code = false;
            return plain_row(out, mg, line, base | TUI_DIM);
        }
        if (!band)
            return plain_row(out, mg, line, base); /* the terminal wraps it; copies stay whole */
        pt_buf_init(&t, out->al);
        if (base)
            tui_style(&t, base);
        pt_buf_puts(&t, line);
        rows = emit(out, margin, margin, mg, pt_buf_str(&t), width, true, false);
        pt_buf_free(&t);
        return rows;
    }
    if (fence(line, &ch, &len)) {
        m->code = true;
        m->fch = ch;
        m->flen = len;
        return plain_row(out, mg, line, base | TUI_DIM);
    }
    ind = indent_of(line);
    if (ind == line.n)
        return plain_row(out, 0, PT_STR_NULL, 0);
    pt_buf_init(&t, out->al);
    pt_buf_init(&p1, out->al);
    pt_buf_init(&p2, out->al);
    if (base)
        tui_style(&t, base);
    if (rule(line)) {
        int i;
        tui_style(&t, base | TUI_DIM);
        for (i = mg; i < width; i++)
            pt_buf_putz(&t, m->ascii ? "-" : "\xe2\x94\x80");
        rows = plain_row(out, mg, pt_buf_str(&t), 0);
    } else if (line.p[ind] == '|') { /* a table row: as it is (aligned tables can come later) */
        pt_buf_puts(&t, line);
        rows = band ? emit(out, margin, margin, mg, pt_buf_str(&t), width, true, false)
                    : plain_row(out, mg, pt_buf_str(&t), 0);
    } else if (ind <= 3 && line.p[ind] == '#') {
        size_t h = ind;
        while (h < line.n && line.p[h] == '#')
            h++;
        if (h - ind <= 6 && (h == line.n || is_space(line.p[h]))) {
            pt_str text = pt_str_trim((pt_str){line.p + h, line.n - h});
            while (text.n && text.p[text.n - 1] == '#') /* closing hashes */
                text.n--;
            tui_style(&t, base | TUI_BOLD);
            inline_md(&t, pt_str_trim(text), base | TUI_BOLD);
        } else {
            inline_md(&t, line, base);
        }
        rows = emit(out, margin, margin, mg, pt_buf_str(&t), width, band, true);
    } else {
        /* quotes, then a list item, then a paragraph */
        size_t i = ind, k;
        int pw = mg;
        bool quoted = false;
        pt_buf_puts(&p1, margin);
        while (i < line.n && line.p[i] == '>') {
            tui_style(&p1, base | TUI_DIM);
            pt_buf_putz(&p1, m->ascii ? "| " : "\xe2\x94\x82 ");
            pw += 2;
            quoted = true;
            i++;
            if (i < line.n && line.p[i] == ' ')
                i++;
            while (i < line.n && line.p[i] == ' ')
                i++;
        }
        if (quoted)
            tui_style(&p1, base); /* back from the dim bar */
        pt_buf_puts(&p2, pt_buf_str(&p1));
        k = i;
        if (k < line.n && (line.p[k] == '-' || line.p[k] == '*' || line.p[k] == '+') && k + 1 < line.n &&
            line.p[k + 1] == ' ') {
            size_t j;
            for (j = 0; j < ind && !quoted; j++) /* nesting, outside quotes */
                pt_buf_putc(&p1, ' '), pt_buf_putc(&p2, ' ');
            pw += quoted ? 0 : (int)ind;
            tui_style(&p1, base | TUI_DIM);
            pt_buf_putz(&p1, m->ascii ? "- " : "\xe2\x80\xa2 ");
            tui_style(&p1, base);
            pt_buf_putz(&p2, "  ");
            pw += 2;
            i = k + 2;
        } else {
            size_t d = k;
            while (d < line.n && d - k < 9 && line.p[d] >= '0' && line.p[d] <= '9')
                d++;
            if (d > k && d + 1 < line.n && (line.p[d] == '.' || line.p[d] == ')') && line.p[d + 1] == ' ') {
                size_t j;
                for (j = 0; j < ind && !quoted; j++)
                    pt_buf_putc(&p1, ' '), pt_buf_putc(&p2, ' ');
                pw += quoted ? 0 : (int)ind;
                tui_style(&p1, base | TUI_DIM);
                pt_buf_put(&p1, line.p + k, d - k + 2);
                tui_style(&p1, base);
                for (j = k; j < d + 2; j++)
                    pt_buf_putc(&p2, ' ');
                pw += (int)(d - k + 2);
                i = d + 2;
            } else if (!quoted) {
                i = 0; /* a paragraph keeps its indentation */
            }
        }
        inline_md(&t, (pt_str){line.p + i, line.n - i}, base);
        rows = emit(out, pt_buf_str(&p1), pt_buf_str(&p2), pw, pt_buf_str(&t), width, band, true);
    }
    pt_buf_free(&t);
    pt_buf_free(&p1);
    pt_buf_free(&p2);
    return rows;
}
