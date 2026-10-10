/* The commit line: one atomic batch of record writes.
 *
 *   {"seq":42,"ts":1760000000123,"w":[{record},...],"crc":"8a3f12c0"}\n
 *
 * The CRC-32 covers the bytes from "{" up to, not including, ",\"crc\"". A
 * checksum inside the JSON keeps the log readable with jq and tail -f. */
#include "core/core.h"

#define CRC_KEY ",\"crc\":\""
#define SUFFIX_LEN (sizeof(CRC_KEY) - 1 + 8 + 2) /* ,"crc":" + 8 hex + "} */

void pt_log_begin(pt_buf *b, pt_jw *w, uint64_t seq, int64_t ts)
{
    pt_buf_reset(b);
    pt_jw_init(w, b);
    pt_jw_obj(w);
    pt_jw_key(w, "seq");
    pt_jw_uint(w, seq);
    pt_jw_key(w, "ts");
    pt_jw_int(w, ts);
    pt_jw_key(w, "w");
    pt_jw_arr(w);
}

static void hex8(char *out, uint32_t v)
{
    static const char digits[] = "0123456789abcdef";
    int i;
    for (i = 7; i >= 0; i--, v >>= 4)
        out[i] = digits[v & 15];
}

void pt_log_end(pt_buf *b, pt_jw *w)
{
    char hex[8];
    pt_jw_arr_end(w);
    hex8(hex, pt_crc32(0, b->p, b->n));
    pt_buf_putz(b, CRC_KEY);
    pt_buf_put(b, hex, 8);
    pt_buf_putz(b, "\"}\n");
}

static bool parse_hex8(const char *p, uint32_t *out)
{
    uint32_t v = 0;
    int i;
    for (i = 0; i < 8; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9')
            v |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f')
            v |= (uint32_t)(c - 'a' + 10);
        else
            return false;
    }
    *out = v;
    return true;
}

const pt_json *pt_log_parse(pt_str line, uint64_t *seq, pt_arena *a)
{
    size_t body;
    uint32_t want;
    const pt_json *root, *w;
    int64_t s;
    if (line.n < SUFFIX_LEN + 2 || line.p[0] != '{')
        return NULL;
    body = line.n - SUFFIX_LEN;
    if (!pt_str_eq((pt_str){line.p + body, sizeof(CRC_KEY) - 1}, PT_S(CRC_KEY)) ||
        !pt_str_eq((pt_str){line.p + line.n - 2, 2}, PT_S("\"}")) ||
        !parse_hex8(line.p + body + sizeof(CRC_KEY) - 1, &want) ||
        pt_crc32(0, line.p, body) != want)
        return NULL;
    root = pt_json_parse(line, a, NULL);
    w = pt_json_get(root, "w");
    if (!pt_json_is(root, PT_J_OBJ) || !pt_json_is(w, PT_J_ARR) ||
        !pt_json_int(pt_json_get(root, "seq"), &s) || s < 1 ||
        !pt_json_is(pt_json_get(root, "ts"), PT_J_NUM))
        return NULL;
    *seq = (uint64_t)s;
    return root;
}
