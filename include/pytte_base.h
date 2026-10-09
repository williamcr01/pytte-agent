/* pytte_base.h: memory, strings, buffers, JSON and SSE shared by every layer.
 *
 * Portable C99. No stdio, no malloc (the host passes an allocator), no
 * floating point, no time. */
#ifndef PYTTE_BASE_H
#define PYTTE_BASE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- allocator ---------------------------------------------------------
 * One function, like Lua's: n == 0 frees p, p == NULL allocates, otherwise
 * resizes from old to n bytes. Returns NULL on failure. */
typedef void *(*pt_alloc_fn)(void *ud, void *p, size_t old, size_t n);
typedef struct pt_allocator {
    pt_alloc_fn fn;
    void *ud;
} pt_allocator;

/* ---- strings ------------------------------------------------------------ */
typedef struct pt_str {
    const char *p;
    size_t n;
} pt_str;

#define PT_S(lit) ((pt_str){(lit), sizeof(lit) - 1})
#define PT_STR_NULL ((pt_str){NULL, 0})

pt_str pt_strz(const char *z);
bool pt_str_eq(pt_str a, pt_str b);
bool pt_str_eqz(pt_str a, const char *z);
bool pt_str_ieq(pt_str a, pt_str b); /* ASCII case-insensitive */
bool pt_str_starts(pt_str s, pt_str prefix);
bool pt_str_contains(pt_str hay, pt_str needle);
bool pt_str_icontains(pt_str hay, pt_str needle);
pt_str pt_str_trim(pt_str s);
/* Index of the first c in s, or -1. */
long pt_str_find(pt_str s, char c);
/* Decimal digits only; false on empty, junk or overflow. */
bool pt_str_to_u64(pt_str s, uint64_t *out);

/* ---- arena ---------------------------------------------------------------
 * Grows in chunks from the allocator, frees everything at once. After a
 * failed allocation `oom` stays set, so callers may check once at the end. */
typedef struct pt_arena_chunk pt_arena_chunk;
typedef struct pt_arena {
    pt_allocator al;
    pt_arena_chunk *head;
    size_t chunk;
    bool oom;
} pt_arena;

void pt_arena_init(pt_arena *a, pt_allocator al, size_t chunk);
void *pt_arena_alloc(pt_arena *a, size_t n);  /* 16-byte aligned */
void *pt_arena_zalloc(pt_arena *a, size_t n); /* zeroed */
/* NUL-terminated copy; the returned slice excludes the NUL. */
pt_str pt_arena_dup(pt_arena *a, pt_str s);
const char *pt_arena_cstr(pt_arena *a, pt_str s);
/* Free all chunks but the first and make it empty again. */
void pt_arena_reset(pt_arena *a);
void pt_arena_free(pt_arena *a);

/* ---- growable buffer ------------------------------------------------------ */
typedef struct pt_buf {
    char *p;
    size_t n, cap;
    pt_allocator al;
    bool oom;
} pt_buf;

void pt_buf_init(pt_buf *b, pt_allocator al);
bool pt_buf_reserve(pt_buf *b, size_t extra);
void pt_buf_put(pt_buf *b, const void *p, size_t n);
void pt_buf_puts(pt_buf *b, pt_str s);
void pt_buf_putz(pt_buf *b, const char *z);
void pt_buf_putc(pt_buf *b, char c);
void pt_buf_u64(pt_buf *b, uint64_t v);
void pt_buf_i64(pt_buf *b, int64_t v);
pt_str pt_buf_str(const pt_buf *b);
void pt_buf_reset(pt_buf *b);
void pt_buf_free(pt_buf *b);

/* ---- JSON -----------------------------------------------------------------
 * Strict parser (RFC 8259, UTF-8 validated, no comments or trailing commas)
 * building a DOM in an arena. Every value keeps `raw`, its exact source
 * bytes, so stored JSON can be passed through unchanged. Numbers stay raw
 * text. String values are decoded on demand. */
#define PT_JSON_MAX_DEPTH 64

typedef enum {
    PT_J_NULL,
    PT_J_FALSE,
    PT_J_TRUE,
    PT_J_NUM,
    PT_J_STR,
    PT_J_ARR,
    PT_J_OBJ
} pt_jtype;

typedef struct pt_json {
    pt_jtype type;
    bool esc;     /* string contains escapes */
    uint32_t len; /* number of children (arrays, objects) */
    pt_str raw;   /* exact source bytes of the value */
    pt_str key;   /* decoded member name, when inside an object */
    struct pt_json *next, *child;
} pt_json;

typedef struct pt_json_err {
    size_t pos;
    const char *msg;
} pt_json_err;

pt_json *pt_json_parse(pt_str src, pt_arena *a, pt_json_err *err);
const pt_json *pt_json_get(const pt_json *obj, const char *key);
const pt_json *pt_json_at(const pt_json *arr, uint32_t i);
bool pt_json_is(const pt_json *v, pt_jtype t);
/* Decoded string value; a slice of the source when it has no escapes.
 * Empty for anything that is not a string. */
pt_str pt_json_str(const pt_json *v, pt_arena *a);
pt_str pt_json_gets(const pt_json *obj, const char *key, pt_arena *a);
bool pt_json_int(const pt_json *v, int64_t *out);
int64_t pt_json_geti(const pt_json *obj, const char *key, int64_t dflt);
bool pt_json_truthy(const pt_json *v);
/* Decode the inside of a JSON string literal (no quotes) into b. */
bool pt_json_unescape(pt_str s, pt_buf *b);

/* Streaming writer. Keys come out in the order the code writes them. */
typedef struct pt_jw {
    pt_buf *b;
    uint32_t depth;
    bool after_key;
    uint8_t comma[PT_JSON_MAX_DEPTH + 1];
} pt_jw;

void pt_jw_init(pt_jw *w, pt_buf *b);
void pt_jw_obj(pt_jw *w);
void pt_jw_obj_end(pt_jw *w);
void pt_jw_arr(pt_jw *w);
void pt_jw_arr_end(pt_jw *w);
void pt_jw_key(pt_jw *w, const char *k);
void pt_jw_str(pt_jw *w, pt_str s);
void pt_jw_strz(pt_jw *w, const char *z);
void pt_jw_int(pt_jw *w, int64_t v);
void pt_jw_uint(pt_jw *w, uint64_t v);
void pt_jw_bool(pt_jw *w, bool v);
void pt_jw_null(pt_jw *w);
void pt_jw_raw(pt_jw *w, pt_str json); /* already valid JSON, written as is */
/* Quoted, escaped string. Invalid UTF-8 becomes U+FFFD. */
void pt_json_quote(pt_buf *b, pt_str s);

/* ---- SSE ------------------------------------------------------------------
 * Incremental server-sent events parser. Feed any byte chunks; complete
 * events go to the callback. A nonzero callback result stops parsing and
 * is returned. */
typedef struct pt_sse_event {
    pt_str event, data, id;
} pt_sse_event;

typedef int (*pt_sse_cb)(void *ud, const pt_sse_event *ev);

typedef struct pt_sse {
    pt_buf line, data, event, id;
    bool cr, has_data;
} pt_sse;

void pt_sse_init(pt_sse *s, pt_allocator al);
int pt_sse_feed(pt_sse *s, const char *p, size_t n, pt_sse_cb cb, void *ud);
/* End of input: handles a last line or event without a final blank line. */
int pt_sse_finish(pt_sse *s, pt_sse_cb cb, void *ud);
void pt_sse_free(pt_sse *s);

#endif
