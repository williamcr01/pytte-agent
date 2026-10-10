/* The TUI (ARCHITECTURE.md section 16.2): an inline frontend that redraws
 * only a small band at the bottom of the terminal. Not part of libpytte.
 *
 *   text.c   UTF-8, widths, clusters, sanitizing, styled text, wrapping
 *   keys.c   the input decoder
 *   md.c     line-based Markdown to styled text
 *   band.c   the frame model: repaint, commit, resize
 *   input.c  the input box
 *   term.c   raw mode, queries, signals, the watchdog
 *   app.c    the event loop, the display model, slash commands */
#ifndef PT_TUI_H
#define PT_TUI_H

#include "pytte_base.h"

/* ---- text.c ---- */

#define TUI_REPLACEMENT 0xFFFDu

/* One code point from p[0..n) (n > 0). Returns the bytes used, at least 1;
 * an invalid sequence gives U+FFFD for its maximal bad prefix. */
size_t tui_utf8_dec(const char *p, size_t n, uint32_t *cp);
size_t tui_utf8_enc(uint32_t cp, char out[4]);

/* Display width of one code point: the Unicode answer, and the most any
 * terminal is known to use (16.2.4). */
int tui_cp_best(uint32_t cp);
int tui_cp_max(uint32_t cp);

/* Grapheme clusters, simplified: a base plus combining marks, variation
 * selectors, emoji modifiers and ZWJ-joined parts; regional indicators in
 * pairs. i must be a code point boundary. */
size_t tui_next_cluster(pt_str s, size_t i);
size_t tui_prev_cluster(pt_str s, size_t i);
int tui_cluster_w(pt_str s, size_t i, size_t j, bool max);

/* Clean text from outside for display (R6): invalid UTF-8 becomes U+FFFD;
 * C0 controls except \n and \t, DEL, C1 and bidi controls are dropped;
 * \r\n becomes \n, a lone \r is dropped; tabs expand to stops of 4.
 * *col is the column the text starts at (tracked across calls). */
void tui_clean(pt_buf *out, pt_str in, int *col);

/* ---- styled text ----
 * Clean UTF-8 plus style switches: TUI_STY then one byte, 0x20 + bits.
 * Clean text never holds TUI_STY, so the two cannot be confused. */
#define TUI_STY '\x01'
enum {
    TUI_BOLD = 1,
    TUI_DIM = 2,
    TUI_RED = 1 << 2, /* a colour index in bits 2-4 */
    TUI_GREEN = 2 << 2,
    TUI_CYAN = 3 << 2,
    TUI_MAGENTA = 4 << 2,
    TUI_COLOR_MASK = 7 << 2
};

void tui_style(pt_buf *b, int style);
/* Width of styled text without newlines. */
int tui_width(pt_str s, bool max);
/* The style in effect at the end of s, starting from `style`. */
int tui_style_at(pt_str s, int style);
/* The longest prefix of s (in bytes) that fits in `cols` columns. */
size_t tui_fit(pt_str s, int cols, bool max);

/* Break styled text (no newlines) into rows of at most `cols` columns.
 * Each row is appended to out followed by '\n', starting with the style in
 * effect. Rows after the first start with `indent` spaces. words: break
 * at spaces (dropping them at a break), else anywhere between clusters.
 * Returns the number of rows (at least 1). */
int tui_wrap(pt_buf *out, pt_str s, int cols, int indent, bool words, bool max);

/* Styled text to terminal bytes: style switches become SGR. color: false
 * drops the colours (NO_COLOR) but keeps bold and dim. */
void tui_sgr(pt_buf *out, pt_str s, bool color);

#endif
