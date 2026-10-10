/* The input decoder (section 16.2.7): terminal bytes to keys, text,
 * pastes and replies. Keeps partial sequences between reads. */
#include "tui/tui.h"

#include <stdlib.h>
#include <string.h>

static void *al_fn(void *ud, void *p, size_t old, size_t n)
{
    (void)ud;
    (void)old;
    if (!n) {
        free(p);
        return NULL;
    }
    return realloc(p, n);
}

void tui_keys_init(tui_keys *k)
{
    pt_allocator al;
    al.fn = al_fn;
    al.ud = NULL;
    memset(k, 0, sizeof *k);
    pt_buf_init(&k->in, al);
    pt_buf_init(&k->out, al);
    pt_buf_init(&k->pasted, al);
}

void tui_keys_free(tui_keys *k)
{
    pt_buf_free(&k->in);
    pt_buf_free(&k->out);
    pt_buf_free(&k->pasted);
}

void tui_keys_feed(tui_keys *k, const char *p, size_t n)
{
    if (k->at && k->at == k->in.n) { /* all decoded: start over */
        pt_buf_reset(&k->in);
        k->at = 0;
    } else if (k->at > 4096) {
        memmove(k->in.p, k->in.p + k->at, k->in.n - k->at);
        k->in.n -= k->at;
        k->at = 0;
    }
    pt_buf_put(&k->in, p, n);
}

bool tui_keys_pending(const tui_keys *k)
{
    return k->at < k->in.n;
}

static void key(tui_ev *ev, uint32_t code, int mods)
{
    ev->kind = TE_KEY;
    ev->key = code;
    ev->mods = mods;
}

/* xterm and kitty send modifiers as 1 + a bitmask: shift 1, alt 2, ctrl 4
 * (and super, hyper, meta, caps and num lock above, which are dropped). */
static int mods_of(long m)
{
    return m > 1 ? (int)((m - 1) & 7) : 0;
}

/* A control byte, or a byte after ESC (alt). */
static void control(tui_ev *ev, unsigned char c, int mods)
{
    switch (c) {
    case 0x0d:
        key(ev, TK_ENTER, mods);
        break;
    case 0x0a:
        key(ev, 'j', mods | TM_CTRL);
        break;
    case 0x09:
        key(ev, TK_TAB, mods);
        break;
    case 0x08:
    case 0x7f:
        key(ev, TK_BACKSPACE, mods);
        break;
    case 0x00:
        key(ev, ' ', mods | TM_CTRL);
        break;
    default:
        if (c < 0x1b)
            key(ev, 'a' + c - 1, mods | TM_CTRL);
        else /* 0x1c-0x1f: Ctrl-\ ] ^ _ */
            key(ev, '\\' + c - 0x1c, mods | TM_CTRL);
    }
}

/* kitty's code for a key: the functional ones it shares with legacy. */
static uint32_t kitty_key(long code)
{
    switch (code) {
    case 13:
        return TK_ENTER;
    case 9:
        return TK_TAB;
    case 127:
    case 8:
        return TK_BACKSPACE;
    case 27:
        return TK_ESC;
    case 57414: /* keypad enter */
        return TK_ENTER;
    }
    if (code >= 'A' && code <= 'Z')
        return (uint32_t)(code - 'A' + 'a');
    if (code > 0 && code < 0x110000 && !(code >= 57344 && code <= 63743))
        return (uint32_t)code;
    return 0; /* private-use function keys (keypad, media, ...) */
}

typedef struct csi {
    char priv;     /* '?', '>', '<', '=' or 0 */
    long p[4];     /* parameters (sub-parameters after ':' dropped) */
    int np;
    char final;
    size_t len;    /* bytes, from the ESC */
} csi;

/* 1: parsed, 0: incomplete, -1: not a CSI we can parse (len set) */
static int parse_csi(const char *s, size_t n, csi *c)
{
    size_t i = 2;
    bool sub = false;
    memset(c, 0, sizeof *c);
    if (i < n && strchr("?><=", s[i]) && s[i])
        c->priv = s[i++];
    c->np = 0;
    for (; i < n; i++) {
        unsigned char b = (unsigned char)s[i];
        if (b >= '0' && b <= '9') {
            if (!sub) {
                if (c->np == 0)
                    c->np = 1;
                if (c->np <= 4 && c->p[c->np - 1] < 100000000)
                    c->p[c->np - 1] = c->p[c->np - 1] * 10 + (b - '0');
            }
        } else if (b == ';') {
            if (c->np == 0)
                c->np = 1;
            c->np++;
            sub = false;
        } else if (b == ':') {
            sub = true;
        } else if (b >= 0x20 && b <= 0x2F) {
            /* intermediates: kept out of the parameters */
        } else if (b >= 0x40 && b <= 0x7E) {
            c->final = (char)b;
            c->len = i + 1;
            if (c->np > 4)
                c->np = 4;
            return 1;
        } else {
            c->len = i; /* broken: drop up to here */
            return -1;
        }
        if (i > 64) {
            c->len = i;
            return -1;
        }
    }
    return 0;
}

static long param(const csi *c, int i, long def)
{
    return i < c->np && c->p[i] ? c->p[i] : def;
}

/* A complete CSI sequence: 1 = an event, 0 = ignored. */
static int do_csi(tui_keys *k, const csi *c, tui_ev *ev)
{
    int mods = mods_of(param(c, 1, 1));
    if (c->priv == '?') {
        if (c->final == 'c') {
            ev->kind = TE_REPLY;
            ev->reply = TR_DA1;
            return 1;
        }
        if (c->final == 'u') {
            ev->kind = TE_REPLY;
            ev->reply = TR_KITTY;
            ev->a = (int)param(c, 0, 0);
            return 1;
        }
        return 0;
    }
    if (c->priv)
        return 0;
    switch (c->final) {
    case 'A':
        key(ev, TK_UP, mods);
        return 1;
    case 'B':
        key(ev, TK_DOWN, mods);
        return 1;
    case 'C':
        key(ev, TK_RIGHT, mods);
        return 1;
    case 'D':
        key(ev, TK_LEFT, mods);
        return 1;
    case 'H':
        key(ev, TK_HOME, mods);
        return 1;
    case 'F':
        key(ev, TK_END, mods);
        return 1;
    case 'Z':
        key(ev, TK_TAB, mods | TM_SHIFT);
        return 1;
    case 'R':
        if (k->want_cpr) { /* else it is a legacy Shift+F3, and dropped */
            k->want_cpr = false;
            ev->kind = TE_REPLY;
            ev->reply = TR_CPR;
            ev->a = (int)param(c, 0, 1);
            ev->b = (int)param(c, 1, 1);
            return 1;
        }
        return 0;
    case 'u': {
        uint32_t code = kitty_key(param(c, 0, 0));
        if (!code)
            return 0;
        key(ev, code, mods);
        return 1;
    }
    case '~':
        switch (param(c, 0, 0)) {
        case 1:
        case 7:
            key(ev, TK_HOME, mods);
            return 1;
        case 4:
        case 8:
            key(ev, TK_END, mods);
            return 1;
        case 2:
            key(ev, TK_INSERT, mods);
            return 1;
        case 3:
            key(ev, TK_DELETE, mods);
            return 1;
        case 5:
            key(ev, TK_PGUP, mods);
            return 1;
        case 6:
            key(ev, TK_PGDN, mods);
            return 1;
        case 27: { /* xterm modifyOtherKeys: CSI 27;mods;code ~ */
            uint32_t code = kitty_key(param(c, 2, 0));
            if (!code)
                return 0;
            key(ev, code, mods);
            return 1;
        }
        case 200:
            k->paste = true;
            pt_buf_reset(&k->pasted);
            return 0;
        }
        return 0;
    }
    return 0;
}

/* Inside a paste: collect up to the end marker, which may be split. */
static bool do_paste(tui_keys *k, tui_ev *ev)
{
    static const char end[] = "\x1b[201~";
    const char *s = k->in.p + k->at;
    size_t n = k->in.n - k->at, i, keep, j;
    for (i = 0; i + 6 <= n; i++)
        if (memcmp(s + i, end, 6) == 0)
            break;
    if (i + 6 > n) { /* no end yet: keep what could be the start of one */
        keep = n < 5 ? n : 5;
        while (keep && memcmp(s + n - keep, end, keep) != 0)
            keep--;
        pt_buf_put(&k->pasted, s, n - keep);
        k->at += n - keep;
        return false;
    }
    pt_buf_put(&k->pasted, s, i);
    k->at += i + 6;
    k->paste = false;
    /* \r\n and a lone \r are newlines, then clean as any outside text */
    for (i = j = 0; i < k->pasted.n; i++) {
        char c = k->pasted.p[i];
        if (c == '\r') {
            if (i + 1 < k->pasted.n && k->pasted.p[i + 1] == '\n')
                continue;
            c = '\n';
        }
        k->pasted.p[j++] = c;
    }
    k->pasted.n = j;
    {
        int col = 0;
        pt_buf_reset(&k->out);
        tui_clean(&k->out, pt_buf_str(&k->pasted), &col);
    }
    ev->kind = TE_PASTE;
    ev->text = pt_buf_str(&k->out);
    return true;
}

/* How many bytes from s are a run of text: up to a control byte, and only
 * whole UTF-8 sequences unless flush. */
static size_t text_run(const char *s, size_t n, bool flush)
{
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        size_t need;
        if (c < 0x20 || c == 0x7F)
            break;
        need = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        if (i + need > n && !flush) {
            size_t j; /* incomplete only if what is there continues it */
            for (j = i + 1; j < n && ((unsigned char)s[j] & 0xC0) == 0x80; j++)
                ;
            if (j == n)
                break;
        }
        i++;
        while (i < n && ((unsigned char)s[i] & 0xC0) == 0x80 && --need)
            i++;
    }
    return i;
}

bool tui_keys_next(tui_keys *k, tui_ev *ev, bool flush)
{
    memset(ev, 0, sizeof *ev);
    for (;;) {
        const char *s = k->in.p + k->at;
        size_t n = k->in.n - k->at;
        unsigned char c;
        if (k->paste) {
            if (do_paste(k, ev))
                return true;
            return false;
        }
        if (!n)
            return false;
        c = (unsigned char)s[0];
        if (c != 0x1b) {
            size_t t;
            if (c < 0x20 || c == 0x7F) {
                control(ev, c, 0);
                k->at++;
                return true;
            }
            t = text_run(s, n, flush);
            if (!t)
                return false; /* an incomplete UTF-8 sequence */
            {
                int col = 0;
                pt_buf_reset(&k->out);
                tui_clean(&k->out, (pt_str){s, t}, &col);
            }
            k->at += t;
            ev->kind = TE_TEXT;
            ev->text = pt_buf_str(&k->out);
            return true;
        }
        if (n == 1) {
            if (!flush)
                return false;
            key(ev, TK_ESC, 0);
            k->at++;
            return true;
        }
        c = (unsigned char)s[1];
        if (c == '[') {
            csi cs;
            int r = parse_csi(s, n, &cs);
            if (r == 0) {
                if (!flush)
                    return false;
                k->at = k->in.n; /* an unfinished sequence: dropped */
                return false;
            }
            k->at += cs.len;
            if (r > 0 && do_csi(k, &cs, ev))
                return true;
            continue;
        }
        if (c == 'O') { /* SS3 */
            if (n < 3) {
                if (!flush)
                    return false;
                k->at = k->in.n;
                return false;
            }
            k->at += 3;
            switch (s[2]) {
            case 'A':
                key(ev, TK_UP, 0);
                return true;
            case 'B':
                key(ev, TK_DOWN, 0);
                return true;
            case 'C':
                key(ev, TK_RIGHT, 0);
                return true;
            case 'D':
                key(ev, TK_LEFT, 0);
                return true;
            case 'H':
                key(ev, TK_HOME, 0);
                return true;
            case 'F':
                key(ev, TK_END, 0);
                return true;
            case 'M':
                key(ev, TK_ENTER, 0);
                return true;
            }
            continue; /* F1-F4 and the rest: ignored */
        }
        if (c == ']' || c == 'P' || c == '_' || c == '^' || c == 'X') {
            /* OSC, DCS, APC, PM, SOS: skipped whole, up to BEL or ST */
            size_t i;
            for (i = 2; i < n; i++)
                if (s[i] == '\a' || (s[i] == 0x1b && i + 1 < n && s[i + 1] == '\\'))
                    break;
            if (i >= n) {
                if (!flush)
                    return false;
                k->at = k->in.n;
                return false;
            }
            k->at += i + (s[i] == '\a' ? 1 : 2);
            continue;
        }
        if (c == 0x1b) { /* Esc, then something else */
            key(ev, TK_ESC, 0);
            k->at++;
            return true;
        }
        /* Alt + a key */
        if (c < 0x20 || c == 0x7F) {
            control(ev, c, TM_ALT);
            k->at += 2;
            return true;
        }
        {
            uint32_t cp;
            size_t used;
            if (c >= 0x80 && !flush && text_run(s + 1, n - 1, false) == 0)
                return false;
            used = tui_utf8_dec(s + 1, n - 1, &cp);
            if (cp >= 'A' && cp <= 'Z')
                key(ev, cp - 'A' + 'a', TM_ALT | TM_SHIFT);
            else
                key(ev, cp, TM_ALT);
            k->at += 1 + used;
            return true;
        }
    }
}
