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
/* East Asian ambiguous characters (é, ±, °, ...) are one column, as nearly
 * every terminal draws them, unless the terminal is set to draw them wide
 * (PYTTE_AMBIGUOUS=wide). */
void tui_set_ambiguous_wide(bool wide);

/* Grapheme clusters, simplified: a base plus combining marks, variation
 * selectors, emoji modifiers and ZWJ-joined parts; regional indicators in
 * pairs. i must be a code point boundary. */
size_t tui_next_cluster(pt_str s, size_t i);
size_t tui_prev_cluster(pt_str s, size_t i);
int tui_cluster_w(pt_str s, size_t i, size_t j, bool max);
/* The least any terminal is known to use (older tables draw emoji in one
 * column): with w_max, it brackets what the terminal really does. */
int tui_cluster_wmin(pt_str s, size_t i, size_t j);

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

/* How a terminal that rewraps lines lays styled text out in `cols`
 * columns: the rows it takes, and the column after the last character.
 * max: whole clusters at w_max, which no terminal exceeds; else single
 * code points at their least (ZWJ-joined parts and emoji modifiers take
 * nothing), which no terminal undercuts, whether it splits clusters at
 * the edge or not. A wide character that does not fit wraps whole. */
int tui_rows(pt_str s, int cols, bool max, int *endcol);
/* The width the next character at s[i] takes, in the same two ways. */
int tui_next_w(pt_str s, size_t i, bool max);

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

/* ---- band.c: the frame model (16.2.3, 16.2.6) ---- */

/* What the band should show: styled rows, each followed by '\n' and at
 * most cols - 1 columns wide by w_max (the band clips them anyway), and
 * where the cursor is: a row, and the byte offset in it. */
typedef struct tui_frame {
    pt_buf rows;
    int n;
    int cur_row;
    size_t cur_at;
} tui_frame;

void tui_frame_init(tui_frame *f, pt_allocator al);
void tui_frame_free(tui_frame *f);
void tui_frame_reset(tui_frame *f);
/* Add a row (clipped to cols - 1 by w_max); returns its index. */
int tui_frame_row(tui_frame *f, pt_str styled, int cols);

typedef struct tui_band {
    int rows, cols;
    bool reflow; /* the terminal rewraps lines on resize (16.2.6) */
    bool color;
    pt_buf out;  /* bytes for the terminal, sent in one write */
    /* the last frame */
    pt_buf drawn; /* its rows, each followed by '\n' */
    int h, cur;   /* its height and the cursor's row */
    size_t cur_at; /* the cursor's byte offset in its row */
    int up;       /* rows from the cursor up to the top of the band */
    bool clear;   /* erase from the top before the next paint (resize, Ctrl-L) */
    bool gap;     /* and leave one empty row there first (a resize that had to guess) */
} tui_band;

void tui_band_init(tui_band *b, int rows, int cols, pt_allocator al);
void tui_band_free(tui_band *b);
/* Redraw the band to show f (only the rows that changed). */
void tui_band_paint(tui_band *b, const tui_frame *f);
/* Add lines to the transcript above the band (styled text, each line
 * followed by '\n'; any width, the terminal wraps them), then draw f. */
void tui_band_commit(tui_band *b, pt_str lines, const tui_frame *f);
/* The terminal now has this size: the next paint starts from the band's
 * new top, found from the reflow model. */
void tui_band_resize(tui_band *b, int rows, int cols);
/* Repaint everything from the top (Ctrl-L). */
void tui_band_redraw(tui_band *b);
/* Erase the band, leaving the cursor where its top was (exit, suspend). */
void tui_band_erase(tui_band *b);

/* ---- md.c: line-based Markdown (16.2.5) ---- */

typedef struct tui_md {
    bool code;  /* inside a fenced block */
    char fch;   /* its fence character and length */
    int flen;
    bool ascii; /* ASCII symbols */
} tui_md;

/* Render one complete source line (no newline) as styled rows, each
 * followed by '\n', at most cols - 1 wide. base: a style under everything
 * (dim for thinking). band: for the band, where code lines are cut into
 * rows and widths are w_max; otherwise code and table lines are left for
 * the terminal to wrap. Returns the rows added. Advances the fence state;
 * render a copy to show an unfinished line. */
int tui_md_line(tui_md *m, pt_str line, int cols, int base, bool band, pt_buf *out);

/* ---- input.c: the input box (16.2.8) ---- */

typedef struct tui_irow {
    size_t start, end; /* the bytes shown */
    size_t next;       /* where the next row starts */
} tui_irow;

typedef struct tui_input {
    pt_buf text; /* the draft: clean UTF-8, newlines allowed */
    size_t cur;  /* the cursor: a byte offset on a cluster boundary */
    pt_buf kill;
    int goal;    /* the column ↑/↓ aim for; -1: the cursor's */
    /* history, oldest first */
    pt_buf *hist;
    uint32_t nhist, hcap;
    uint32_t hpos; /* nhist: the draft itself */
    pt_buf saved;  /* the draft while browsing history */
    /* layout (tui_input_layout) */
    tui_irow *rows;
    int nrows, rcap, cur_row, cur_col;
    int scroll; /* the first row shown */
} tui_input;

typedef enum {
    TI_IGNORED, /* not an editing key */
    TI_EDITED,
    TI_MOVED,
    TI_TOP,     /* ↑ on the first row: for history or a menu */
    TI_BOTTOM   /* ↓ on the last row */
} tui_input_result;

void tui_input_init(tui_input *in, pt_allocator al);
void tui_input_free(tui_input *in);
/* Replace the draft; the cursor goes to the end. */
void tui_input_set(tui_input *in, pt_str text);
void tui_input_insert(tui_input *in, pt_str clean);
/* An editing key; width is the columns for text, as in the last layout. */
tui_input_result tui_input_key(tui_input *in, const tui_ev *ev, int width);
/* Wrap the draft into rows of at most width columns (w_max): words where
 * possible, hard newlines kept. Sets rows, cur_row and cur_col, and
 * scrolls so the cursor is within `show` rows. */
void tui_input_layout(tui_input *in, int width, int show);

/* History: an entry (not a repeat of the newest), and browsing. The file
 * holds one JSON string per line, newest last. */
void tui_hist_add(tui_input *in, pt_str entry);
bool tui_hist_move(tui_input *in, int dir); /* -1 older, +1 newer */
void tui_hist_load(tui_input *in, pt_str file, uint32_t max);
/* The line to append to the file for an entry. */
void tui_hist_line(pt_buf *out, pt_str entry);

/* ---- term.c: the terminal (16.2.10) ---- */

typedef struct tui_term tui_term;

enum { /* signals, as tui_term_signals reports them */
    TS_WINCH = 1,
    TS_TSTP = 2, /* from outside (kill -TSTP); Ctrl-Z arrives as a key */
    TS_CONT = 4,
    TS_QUIT = 8, /* SIGINT, SIGTERM, SIGQUIT from outside */
    TS_HUP = 16
};

/* Start the watchdog: before the store is opened, before curl, before
 * anything else holds a file (it must not inherit the session's lock).
 * Then install the signal handlers. NULL when stdin or stdout is not a
 * terminal. */
tui_term *tui_term_new(void);
/* Raw mode, bracketed paste, and the startup queries: cursor position,
 * kitty keyboard (when kitty is true) and DA1. */
bool tui_term_start(tui_term *t, bool kitty);
/* After the replies (or the timeout): push the kitty flags. */
void tui_term_kitty(tui_term *t, bool push);
/* The terminal fds to wait on: stdin and the signal pipe. */
int tui_term_in(const tui_term *t);
int tui_term_sigfd(const tui_term *t);
/* Signals since the last call (TS_ bits); the most recent one in *last. */
int tui_term_signals(tui_term *t, int *last);
/* Read what stdin has (after a wait said it is ready). Returns the bytes,
 * 0 at end of file (the terminal is gone), -1 for nothing. */
long tui_term_read(tui_term *t, char *buf, size_t cap);
/* Write everything; returns the milliseconds it took (for pacing). Does
 * nothing once the terminal is gone. */
int tui_term_write(tui_term *t, pt_str bytes);
void tui_term_size(tui_term *t, int *rows, int *cols);
/* The terminal hung up: skip every write from now on. */
void tui_term_gone(tui_term *t);
/* Back to the saved state (pop kitty, paste off, cursor on, termios);
 * resume undoes it. For Ctrl-Z and the pager. */
void tui_term_suspend(tui_term *t);
void tui_term_resume(tui_term *t);
/* Stop this process (SIGTSTP with the default action), between suspend
 * and resume. */
void tui_term_stop_self(void);
/* Restore, tell the watchdog the exit is clean, and free. */
void tui_term_free(tui_term *t);

#endif
