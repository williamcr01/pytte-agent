/* HTML to Markdown-like text, incrementally (for web_fetch): a state
 * machine fed any chunks, never rescanning what came before. Scripts,
 * styles and page chrome (head, nav, footer, ...) are dropped; headings,
 * paragraphs, lists, links, code and tables keep their shape. Not a
 * conforming HTML parser: it reads what pages actually serve, and on bad
 * input it keeps going rather than failing. */
#include "pytte_base.h"

#include <string.h>

#define TAG_MAX 4096 /* bytes of a tag kept; the rest of a longer one is ignored */
#define ENT_MAX 32
#define TITLE_MAX 512

enum { S_TEXT, S_LT, S_TAG, S_COMMENT, S_DECL, S_RAW };

/* Elements whose content is dropped. void: has no end tag. */
static const char *const skipped[] = {"script", "style", "head", "nav", "footer", "noscript", "svg",
                                      "template", "iframe", "select", "button", "form", "aside",
                                      "canvas", "object", "math", NULL};
/* ...whose content is text up to their end tag, with no tags inside. */
static const char *const rawtext[] = {"script", "style", "textarea", "title", "xmp", "noscript", NULL};
static const char *const blocks[] = {"p", "div", "section", "article", "main", "header", "address",
                                     "blockquote", "figure", "figcaption", "dl", "dt", "dd", "table",
                                     "details", "summary", "fieldset", "center", "body", "html", NULL};
static const char *const voids[] = {"area", "base", "br", "col", "embed", "hr", "img", "input",
                                    "link", "meta", "param", "source", "track", "wbr", NULL};

static bool in_list(pt_str name, const char *const *l)
{
    for (; *l; l++)
        if (pt_str_eqz(name, *l))
            return true;
    return false;
}

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

static bool alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

void pt_html_init(pt_html *h, pt_allocator al, pt_buf *out, pt_str base)
{
    memset(h, 0, sizeof *h);
    h->out = out;
    h->al = al;
    pt_buf_init(&h->tag, al);
    pt_buf_init(&h->ent, al);
    pt_buf_init(&h->href, al);
    pt_buf_init(&h->base, al);
    pt_buf_init(&h->title, al);
    pt_buf_puts(&h->base, base);
    h->nl = 2; /* at the start: no leading blank lines */
}

void pt_html_free(pt_html *h)
{
    pt_buf_free(&h->tag);
    pt_buf_free(&h->ent);
    pt_buf_free(&h->href);
    pt_buf_free(&h->base);
    pt_buf_free(&h->title);
}

/* ---- output ---- */

/* Text goes out through these, which collapse whitespace outside pre and
 * keep at most one blank line between blocks. */
static void put_raw(pt_html *h, const char *p, size_t n)
{
    size_t i;
    if (!n)
        return;
    pt_buf_put(h->out, p, n);
    for (i = n; i > 0 && p[i - 1] == '\n'; i--)
        ;
    h->nl = i == 0 ? h->nl + (uint32_t)n : (uint32_t)(n - i);
    h->space = false;
}

/* Up to n newlines in a row at the end of the output. */
static void breaks(pt_html *h, uint32_t n)
{
    while (h->nl < n) {
        pt_buf_putc(h->out, '\n');
        h->nl++;
    }
    h->space = false;
}

static void indent(pt_html *h)
{
    uint32_t i;
    if (h->nl == 0)
        return;
    for (i = 1; i < h->depth && i < 8; i++)
        pt_buf_putz(h->out, "  ");
    if (h->quote)
        pt_buf_putz(h->out, "> ");
}

/* Text content: whitespace runs become one space, none at a line start. */
static void put_text(pt_html *h, const char *p, size_t n)
{
    size_t i, from = 0;
    if (h->in_title) {
        size_t room = TITLE_MAX - h->title.n, k = n < room ? n : room;
        if (h->title.n < TITLE_MAX)
            pt_buf_put(&h->title, p, k);
        return;
    }
    if (h->skip)
        return;
    if (h->pre) {
        for (i = 0; i < n; i++)
            if (p[i] == '\r') {
                put_raw(h, p + from, i - from);
                from = i + 1;
            }
        put_raw(h, p + from, n - from);
        return;
    }
    for (i = 0; i < n; i++) {
        if (space(p[i])) {
            if (i > from) {
                if (h->space && h->nl == 0)
                    pt_buf_putc(h->out, ' ');
                indent(h);
                put_raw(h, p + from, i - from);
            }
            from = i + 1;
            h->space = true;
        }
    }
    if (n > from) {
        if (h->space && h->nl == 0)
            pt_buf_putc(h->out, ' ');
        indent(h);
        put_raw(h, p + from, n - from);
    }
}

static void put_utf8(pt_html *h, uint32_t c)
{
    char u[4];
    size_t n;
    if (c == 0 || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF))
        c = 0xFFFD;
    if (c < 0x80) {
        u[0] = (char)c;
        n = 1;
    } else if (c < 0x800) {
        u[0] = (char)(0xC0 | (c >> 6));
        u[1] = (char)(0x80 | (c & 0x3F));
        n = 2;
    } else if (c < 0x10000) {
        u[0] = (char)(0xE0 | (c >> 12));
        u[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        u[2] = (char)(0x80 | (c & 0x3F));
        n = 3;
    } else {
        u[0] = (char)(0xF0 | (c >> 18));
        u[1] = (char)(0x80 | ((c >> 12) & 0x3F));
        u[2] = (char)(0x80 | ((c >> 6) & 0x3F));
        u[3] = (char)(0x80 | (c & 0x3F));
        n = 4;
    }
    if (c == 0xA0)
        put_text(h, " ", 1); /* nbsp: a space, as text-mode browsers show it */
    else
        put_text(h, u, n);
}

/* ---- entities ---- */

static const struct {
    const char *name;
    uint32_t c;
} entities[] = {
    {"amp", '&'},       {"lt", '<'},        {"gt", '>'},         {"quot", '"'},      {"apos", '\''},
    {"nbsp", 0xA0},     {"copy", 0xA9},     {"reg", 0xAE},       {"trade", 0x2122},  {"mdash", 0x2014},
    {"ndash", 0x2013},  {"hellip", 0x2026}, {"laquo", 0xAB},     {"raquo", 0xBB},    {"lsquo", 0x2018},
    {"rsquo", 0x2019},  {"ldquo", 0x201C},  {"rdquo", 0x201D},   {"bull", 0x2022},   {"middot", 0xB7},
    {"times", 0xD7},    {"divide", 0xF7},   {"euro", 0x20AC},    {"pound", 0xA3},    {"yen", 0xA5},
    {"cent", 0xA2},     {"sect", 0xA7},     {"para", 0xB6},      {"deg", 0xB0},      {"plusmn", 0xB1},
    {"larr", 0x2190},   {"rarr", 0x2192},   {"uarr", 0x2191},    {"darr", 0x2193},   {"harr", 0x2194},
    {"le", 0x2264},     {"ge", 0x2265},     {"ne", 0x2260},      {"minus", 0x2212},  {"shy", 0xAD},
    {"zwj", 0x200D},    {"zwnj", 0x200C},   {"thinsp", 0x2009},  {"ensp", 0x2002},   {"emsp", 0x2003},
    {"iexcl", 0xA1},    {"iquest", 0xBF},   {"auml", 0xE4},      {"ouml", 0xF6},     {"uuml", 0xFC},
    {"Auml", 0xC4},     {"Ouml", 0xD6},     {"Uuml", 0xDC},      {"szlig", 0xDF},    {"eacute", 0xE9},
    {"egrave", 0xE8},   {"aacute", 0xE1},   {"agrave", 0xE0},    {"aring", 0xE5},    {"Aring", 0xC5},
    {"oslash", 0xF8},   {"aelig", 0xE6},    {"ccedil", 0xE7},    {"ntilde", 0xF1},   {"hearts", 0x2665},
    {"check", 0x2713},  {"dagger", 0x2020}, {"prime", 0x2032},   {"infin", 0x221E},  {"micro", 0xB5},
};

/* The entity in h->ent (without & and ;): true with its character. */
static bool entity(pt_html *h, uint32_t *c)
{
    pt_str e = pt_buf_str(&h->ent);
    size_t i;
    uint32_t v = 0;
    if (e.n >= 2 && e.p[0] == '#') {
        bool hex = e.p[1] == 'x' || e.p[1] == 'X';
        size_t k = hex ? 2 : 1;
        if (k == e.n)
            return false;
        for (; k < e.n; k++) {
            char d = e.p[k];
            uint32_t dv;
            if (d >= '0' && d <= '9')
                dv = (uint32_t)(d - '0');
            else if (hex && lower(d) >= 'a' && lower(d) <= 'f')
                dv = (uint32_t)(lower(d) - 'a' + 10);
            else
                return false;
            if (v > 0x10FFFF)
                continue; /* too big: U+FFFD */
            v = v * (hex ? 16u : 10u) + dv;
        }
        *c = v;
        return true;
    }
    for (i = 0; i < sizeof entities / sizeof entities[0]; i++)
        if (pt_str_eqz(e, entities[i].name)) {
            *c = entities[i].c;
            return true;
        }
    return false;
}

/* An entity ended (at ch, which is ';' or what stopped it): write it, or
 * the bytes as they were when it is not one. */
static void end_entity(pt_html *h, bool semi)
{
    uint32_t c;
    if (entity(h, &c)) {
        put_utf8(h, c);
    } else {
        put_text(h, "&", 1);
        put_text(h, h->ent.p, h->ent.n);
        if (semi)
            put_text(h, ";", 1);
    }
    pt_buf_reset(&h->ent);
    h->in_ent = false;
}

/* ---- URLs ---- */

static size_t scheme_len(pt_str s)
{
    size_t i;
    if (!s.n || !alpha(s.p[0]))
        return 0;
    for (i = 1; i < s.n; i++) {
        char c = s.p[i];
        if (c == ':')
            return i;
        if (!alpha(c) && !(c >= '0' && c <= '9') && c != '+' && c != '-' && c != '.')
            return 0;
    }
    return 0;
}

/* Remove dot segments from a path appended at out[from..]. */
static void dot_segments(pt_buf *out, size_t from)
{
    char *p = out->p + from;
    size_t n = out->n - from, r = 0, w = 0;
    while (r < n) {
        size_t seg = r + 1, len;
        while (seg < n && p[seg] != '/')
            seg++;
        len = seg - r; /* "/name" */
        if (len == 2 && p[r + 1] == '.') {
            if (seg == n)
                p[w++] = '/';
        } else if (len == 3 && p[r + 1] == '.' && p[r + 2] == '.') {
            while (w > 0 && p[w - 1] != '/')
                w--;
            if (w > 0)
                w--;
            if (seg == n)
                p[w++] = '/';
        } else {
            memmove(p + w, p + r, len);
            w += len;
        }
        r = seg;
    }
    out->n = from + w;
}

bool pt_url_resolve(pt_str base, pt_str ref, pt_buf *out)
{
    size_t bs = scheme_len(base), auth_end, path_end, i, from;
    ref = pt_str_trim(ref);
    if (scheme_len(ref)) {
        pt_buf_puts(out, ref);
        return !out->oom;
    }
    if (!bs || base.n < bs + 3 || base.p[bs + 1] != '/' || base.p[bs + 2] != '/')
        return false; /* only hierarchical bases (http, https) */
    for (auth_end = bs + 3; auth_end < base.n && base.p[auth_end] != '/' && base.p[auth_end] != '?' &&
                            base.p[auth_end] != '#';
         auth_end++)
        ;
    for (path_end = auth_end; path_end < base.n && base.p[path_end] != '?' && base.p[path_end] != '#'; path_end++)
        ;
    if (ref.n >= 2 && ref.p[0] == '/' && ref.p[1] == '/') { /* //host/path */
        pt_buf_put(out, base.p, bs + 1);
        pt_buf_puts(out, ref);
        return !out->oom;
    }
    pt_buf_put(out, base.p, auth_end);
    from = out->n;
    if (!ref.n || ref.p[0] == '#' || ref.p[0] == '?') {
        /* the base's path; for a fragment, its query too */
        size_t end = path_end;
        if (!ref.n || ref.p[0] == '#')
            while (end < base.n && base.p[end] != '#')
                end++;
        pt_buf_put(out, base.p + auth_end, end - auth_end);
        if (out->n == from)
            pt_buf_putc(out, '/');
        pt_buf_puts(out, ref);
        return !out->oom;
    }
    if (ref.p[0] == '/') {
        pt_buf_puts(out, ref);
    } else {
        /* the base's directory, then ref */
        for (i = path_end; i > auth_end && base.p[i - 1] != '/'; i--)
            ;
        if (i == auth_end)
            pt_buf_putc(out, '/');
        else
            pt_buf_put(out, base.p + auth_end, i - auth_end);
        pt_buf_puts(out, ref);
    }
    if (out->oom)
        return false;
    /* dot segments, in the path only */
    {
        size_t q;
        pt_buf tail;
        for (q = from; q < out->n && out->p[q] != '?' && out->p[q] != '#'; q++)
            ;
        pt_buf_init(&tail, out->al);
        pt_buf_put(&tail, out->p + q, out->n - q);
        out->n = q;
        dot_segments(out, from);
        pt_buf_puts(out, pt_buf_str(&tail));
        pt_buf_free(&tail);
    }
    return !out->oom;
}

/* ---- tags ---- */

/* The value of attribute name in tag text t (after the tag name), or
 * empty; entities are left as they are, except &amp;. */
static pt_str attr(pt_str t, const char *name)
{
    size_t i = 0, nl = strlen(name);
    while (i < t.n) {
        size_t ks, ke;
        pt_str v = PT_STR_NULL;
        while (i < t.n && (space(t.p[i]) || t.p[i] == '/'))
            i++;
        ks = i;
        while (i < t.n && !space(t.p[i]) && t.p[i] != '=' && t.p[i] != '/')
            i++;
        ke = i;
        while (i < t.n && space(t.p[i]))
            i++;
        if (i < t.n && t.p[i] == '=') {
            i++;
            while (i < t.n && space(t.p[i]))
                i++;
            if (i < t.n && (t.p[i] == '"' || t.p[i] == '\'')) {
                char q = t.p[i++];
                size_t vs = i;
                while (i < t.n && t.p[i] != q)
                    i++;
                v = (pt_str){t.p + vs, i - vs};
                if (i < t.n)
                    i++;
            } else {
                size_t vs = i;
                while (i < t.n && !space(t.p[i]))
                    i++;
                v = (pt_str){t.p + vs, i - vs};
            }
        }
        if (ke - ks == nl) {
            size_t k;
            for (k = 0; k < nl && lower(t.p[ks + k]) == name[k]; k++)
                ;
            if (k == nl)
                return v.p ? v : PT_S("");
        }
        if (ke == ks && i == ks)
            i++; /* no progress: skip a stray byte */
    }
    return PT_STR_NULL;
}

/* href with "&amp;" decoded, resolved against the base: into h->href.
 * False for links that lead nowhere (fragments, javascript:). */
static bool link_target(pt_html *h, pt_str href)
{
    pt_buf dec;
    size_t i;
    bool ok;
    pt_buf_reset(&h->href);
    href = pt_str_trim(href);
    if (!href.n || href.p[0] == '#' || pt_str_starts(href, PT_S("javascript:")) ||
        pt_str_starts(href, PT_S("mailto:")) || pt_str_starts(href, PT_S("data:")))
        return false;
    pt_buf_init(&dec, h->al);
    for (i = 0; i < href.n; i++) {
        if (href.p[i] == '&' && i + 5 <= href.n && memcmp(href.p + i, "&amp;", 5) == 0) {
            pt_buf_putc(&dec, '&');
            i += 4;
        } else if (href.p[i] != '\n' && href.p[i] != '\r' && href.p[i] != '\t') {
            pt_buf_putc(&dec, href.p[i]);
        }
    }
    ok = !dec.oom && (h->base.n ? pt_url_resolve(pt_buf_str(&h->base), pt_buf_str(&dec), &h->href)
                                : (pt_buf_puts(&h->href, pt_buf_str(&dec)), true));
    pt_buf_free(&dec);
    /* a ")" or space in the URL would end the markdown link */
    for (i = 0; ok && i < h->href.n; i++)
        if (h->href.p[i] == ' ' || h->href.p[i] == ')' || h->href.p[i] == '(')
            return false;
    return ok && h->href.n;
}

static void open_tag(pt_html *h, pt_str name, pt_str rest)
{
    if (pt_str_eqz(name, "title")) {
        h->in_title = true;
        return;
    }
    if (pt_str_eqz(name, "body"))
        h->skip = 0; /* </head> is optional: the head ends here */
    if (in_list(name, skipped)) {
        if (!in_list(name, voids))
            h->skip++;
        return;
    }
    if (h->skip)
        return;
    if (name.n == 2 && name.p[0] == 'h' && name.p[1] >= '1' && name.p[1] <= '6') {
        int k;
        breaks(h, 2);
        for (k = 0; k < name.p[1] - '0'; k++)
            pt_buf_putc(h->out, '#');
        put_raw(h, " ", 1);
        h->nl = 0;
        return;
    }
    if (pt_str_eqz(name, "pre")) {
        breaks(h, 2);
        put_raw(h, "```\n", 4);
        h->pre++;
        return;
    }
    if (pt_str_eqz(name, "code") && !h->pre) {
        put_text(h, "`", 1);
        return;
    }
    if (pt_str_eqz(name, "br")) {
        put_raw(h, "\n", 1);
        return;
    }
    if (pt_str_eqz(name, "hr")) {
        breaks(h, 2);
        put_raw(h, "---\n\n", 5);
        return;
    }
    if (pt_str_eqz(name, "ul") || pt_str_eqz(name, "ol")) {
        if (h->depth < 16) {
            h->ordered[h->depth] = name.p[0] == 'o';
            h->counter[h->depth] = 0;
        }
        h->depth++;
        breaks(h, h->depth == 1 ? 2 : 1);
        return;
    }
    if (pt_str_eqz(name, "li")) {
        uint32_t d = h->depth ? h->depth - 1 : 0;
        breaks(h, 1);
        indent(h);
        if (d < 16 && h->ordered[d]) {
            pt_buf_u64(h->out, ++h->counter[d]);
            put_raw(h, ". ", 2);
        } else {
            put_raw(h, "- ", 2);
        }
        h->nl = 0;
        return;
    }
    if (pt_str_eqz(name, "blockquote")) {
        breaks(h, 2);
        h->quote++;
        return;
    }
    if (pt_str_eqz(name, "tr")) {
        breaks(h, 1);
        h->cell = 0;
        return;
    }
    if (pt_str_eqz(name, "td") || pt_str_eqz(name, "th")) {
        if (h->cell++)
            put_raw(h, " | ", 3);
        h->space = false;
        return;
    }
    if (pt_str_eqz(name, "a")) {
        if (h->in_link)
            return;
        if (link_target(h, attr(rest, "href"))) {
            h->in_link = true;
            h->link_at = h->out->n;
            put_text(h, "[", 1);
            h->link_text = h->out->n;
        }
        return;
    }
    if (pt_str_eqz(name, "img")) {
        pt_str alt = pt_str_trim(attr(rest, "alt"));
        if (alt.n) {
            put_text(h, "[image: ", 8);
            put_text(h, alt.p, alt.n);
            put_text(h, "]", 1);
        }
        return;
    }
    if (in_list(name, blocks))
        breaks(h, 2);
}

static void close_tag(pt_html *h, pt_str name)
{
    if (in_list(name, skipped)) {
        if (h->skip)
            h->skip--;
        return;
    }
    if (pt_str_eqz(name, "title")) {
        h->in_title = false;
        return;
    }
    if (h->skip)
        return;
    if (name.n == 2 && name.p[0] == 'h' && name.p[1] >= '1' && name.p[1] <= '6') {
        breaks(h, 2);
        return;
    }
    if (pt_str_eqz(name, "pre")) {
        if (h->pre) {
            h->pre--;
            breaks(h, 1);
            put_raw(h, "```\n\n", 5);
        }
        return;
    }
    if (pt_str_eqz(name, "code") && !h->pre) {
        put_text(h, "`", 1);
        return;
    }
    if (pt_str_eqz(name, "ul") || pt_str_eqz(name, "ol")) {
        if (h->depth)
            h->depth--;
        breaks(h, h->depth ? 1 : 2);
        return;
    }
    if (pt_str_eqz(name, "blockquote")) {
        if (h->quote)
            h->quote--;
        breaks(h, 2);
        return;
    }
    if (pt_str_eqz(name, "a")) {
        if (!h->in_link)
            return;
        h->in_link = false;
        if (h->out->n == h->link_text) { /* no text: no link */
            h->out->n = h->link_at;
            return;
        }
        put_raw(h, "](", 2);
        put_raw(h, h->href.p, h->href.n);
        put_raw(h, ")", 1);
        return;
    }
    if (pt_str_eqz(name, "table") || pt_str_eqz(name, "tr") || in_list(name, blocks))
        breaks(h, pt_str_eqz(name, "tr") ? 1 : 2);
}

/* A whole tag is in h->tag (between < and >). */
static void tag(pt_html *h)
{
    pt_str t = pt_buf_str(&h->tag), name;
    bool end = false;
    size_t i = 0, k;
    char lname[16];
    if (t.n && t.p[0] == '/') {
        end = true;
        i = 1;
    }
    for (k = i; k < t.n && !space(t.p[k]) && t.p[k] != '/' && t.p[k] != '>'; k++)
        ;
    if (k - i == 0 || k - i >= sizeof lname)
        return; /* no name, or one no element has */
    for (name.n = 0; i + name.n < k; name.n++)
        lname[name.n] = lower(t.p[i + name.n]);
    name.p = lname;
    if (end) {
        close_tag(h, name);
        return;
    }
    open_tag(h, name, (pt_str){t.p + k, t.n - k});
    if (in_list(name, rawtext) && !(t.n && t.p[t.n - 1] == '/')) {
        /* text up to </name>, matched as it streams */
        memcpy(h->raw_name, lname, name.n);
        h->raw_len = name.n;
        h->raw_match = 0;
        h->state = S_RAW;
    }
}

/* ---- the state machine ---- */

/* In raw text (script, style, title): look for "</name". raw_match counts
 * the bytes of it matched so far; a match that fails was text. Returns
 * how many bytes it took; the tag state reads on from there. */
static size_t raw(pt_html *h, const char *p, size_t n)
{
    size_t i = 0;
    while (i < n) {
        size_t m = h->raw_match, want = 2 + h->raw_len;
        char c = lower(p[i]);
        if (!m) { /* text up to the next '<' */
            const char *lt = (const char *)memchr(p + i, '<', n - i);
            size_t end = lt ? (size_t)(lt - p) : n;
            put_text(h, p + i, end - i);
            i = end;
            if (lt) {
                h->raw_match = 1;
                i++;
            }
            continue;
        }
        if (m == want) {
            if (space(c) || c == '>' || c == '/') {
                h->raw_match = 0;
                h->state = S_TAG;
                h->tag_quote = 0;
                pt_buf_reset(&h->tag);
                pt_buf_putc(&h->tag, '/');
                pt_buf_put(&h->tag, h->raw_name, h->raw_len);
                return i;
            }
        } else if (m == 1 ? c == '/' : c == h->raw_name[m - 2]) {
            h->raw_match++;
            i++;
            continue;
        }
        /* not the end tag: what matched was text; look at c again */
        put_text(h, "</", m < 2 ? m : 2);
        if (m > 2)
            put_text(h, h->raw_name, m - 2);
        h->raw_match = 0;
        if (c == '<') {
            h->raw_match = 1;
            i++;
        }
    }
    return n;
}

void pt_html_feed(pt_html *h, const char *p, size_t n)
{
    size_t i = 0, from;
    while (i < n) {
        char c = p[i];
        switch (h->state) {
        case S_TEXT:
            from = i;
            while (i < n && p[i] != '<' && p[i] != '&' && !h->in_ent)
                i++;
            if (i > from)
                put_text(h, p + from, i - from);
            if (i == n)
                break;
            c = p[i];
            if (h->in_ent) {
                if (c == ';') {
                    end_entity(h, true);
                    i++;
                } else if ((alpha(c) || (c >= '0' && c <= '9') || c == '#') && h->ent.n < ENT_MAX) {
                    pt_buf_putc(&h->ent, c);
                    i++;
                } else {
                    end_entity(h, false); /* look at c again as text */
                }
            } else if (c == '&') {
                h->in_ent = true;
                i++;
            } else {
                h->state = S_LT;
                i++;
            }
            break;
        case S_LT:
            if (alpha(c) || c == '/') {
                pt_buf_reset(&h->tag);
                h->tag_quote = 0;
                h->state = S_TAG;
            } else if (c == '!') {
                h->state = S_DECL;
                h->dashes = 0;
                h->decl_n = 0;
                i++;
            } else if (c == '?') {
                h->state = S_DECL;
                h->decl_n = 3; /* not a comment */
                i++;
            } else {
                put_text(h, "<", 1);
                h->state = S_TEXT;
            }
            break;
        case S_TAG:
            from = i;
            while (i < n) {
                c = p[i];
                if (h->tag_quote) {
                    if (c == h->tag_quote)
                        h->tag_quote = 0;
                } else if (c == '"' || c == '\'') {
                    h->tag_quote = c;
                } else if (c == '>') {
                    break;
                }
                i++;
            }
            if (h->tag.n < TAG_MAX) {
                size_t k = i - from, room = TAG_MAX - h->tag.n;
                pt_buf_put(&h->tag, p + from, k < room ? k : room);
            }
            if (i < n) { /* '>' */
                i++;
                h->state = S_TEXT;
                tag(h);
            }
            break;
        case S_DECL:
            /* <!-- ... --> or <!DOCTYPE ...> / <![CDATA[ ...]]> / <? ... > */
            if (h->decl_n < 2) {
                if (c == '-') {
                    h->decl_n++;
                    i++;
                    if (h->decl_n == 2)
                        h->state = S_COMMENT;
                    break;
                }
                h->decl_n = 3;
            }
            while (i < n && p[i] != '>')
                i++;
            if (i < n) {
                i++;
                h->state = S_TEXT;
            }
            break;
        case S_COMMENT:
            while (i < n) {
                c = p[i++];
                if (c == '-') {
                    if (h->dashes < 2)
                        h->dashes++;
                } else if (c == '>' && h->dashes >= 2) {
                    h->state = S_TEXT;
                    break;
                } else {
                    h->dashes = 0;
                }
            }
            break;
        case S_RAW:
            i += raw(h, p + i, n - i);
            break;
        }
    }
}

void pt_html_finish(pt_html *h)
{
    if (h->in_ent)
        end_entity(h, false);
    if (h->in_link) {
        h->in_link = false;
        if (h->out->n == h->link_text)
            h->out->n = h->link_at;
    }
    if (h->pre) {
        breaks(h, 1);
        put_raw(h, "```\n", 4);
        h->pre = 0;
    }
    /* one newline at the end */
    while (h->out->n && (h->out->p[h->out->n - 1] == '\n' || h->out->p[h->out->n - 1] == ' '))
        h->out->n--;
    if (h->out->n)
        pt_buf_putc(h->out, '\n');
}

pt_str pt_html_title(const pt_html *h)
{
    return pt_str_trim(pt_buf_str(&h->title));
}
