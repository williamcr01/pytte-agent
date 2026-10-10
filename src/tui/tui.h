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

/* ---- keys.c: the input decoder (16.2.7) ---- */

enum { /* special keys, above the code points */
    TK_ENTER = 0x110001,
    TK_TAB,
    TK_BACKSPACE,
    TK_ESC,
    TK_UP,
    TK_DOWN,
    TK_LEFT,
    TK_RIGHT,
    TK_HOME,
    TK_END,
    TK_INSERT,
    TK_DELETE,
    TK_PGUP,
    TK_PGDN
};
enum { TM_SHIFT = 1, TM_ALT = 2, TM_CTRL = 4 };
typedef enum { TE_KEY = 1, TE_TEXT, TE_PASTE, TE_REPLY } tui_ev_kind;
typedef enum { TR_DA1 = 1, TR_KITTY, TR_CPR } tui_reply;

typedef struct tui_ev {
    tui_ev_kind kind;
    uint32_t key; /* KEY: a TK_ key, or a code point (lower case for Ctrl/Alt letters) */
    int mods;
    pt_str text; /* TEXT, PASTE: clean UTF-8, valid until the next call */
    tui_reply reply;
    int a, b; /* KITTY: flags in a; CPR: row a, column b */
} tui_ev;

typedef struct tui_keys {
    pt_buf in;   /* bytes not decoded yet */
    size_t at;   /* how far they were */
    pt_buf out;  /* the text of the last event */
    bool paste;  /* inside a bracketed paste */
    pt_buf pasted;
    bool want_cpr; /* a cursor position query is outstanding */
} tui_keys;

void tui_keys_init(tui_keys *k);
void tui_keys_free(tui_keys *k);
void tui_keys_feed(tui_keys *k, const char *p, size_t n);
/* The next event, or false when the bytes left are an incomplete sequence
 * (or none). flush: the Esc timeout has passed, so a lone ESC is the Esc
 * key and an unfinished sequence is dropped. */
bool tui_keys_next(tui_keys *k, tui_ev *ev, bool flush);
/* Bytes are waiting for more (the caller should time out and flush). */
bool tui_keys_pending(const tui_keys *k);

#endif
