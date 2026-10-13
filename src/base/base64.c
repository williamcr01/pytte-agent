/* Base64 (RFC 4648): decoding for images in MCP results, and the URL-safe
 * alphabet without padding for OAuth's PKCE values. */
#include "pytte_base.h"

void pt_base64url(pt_buf *out, pt_str bytes)
{
    static const char abc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    const unsigned char *in = (const unsigned char *)bytes.p;
    size_t i;
    char *o;
    if (bytes.n > (SIZE_MAX - 4) / 4 * 3 || !pt_buf_reserve(out, bytes.n / 3 * 4 + 4))
        return;
    o = out->p + out->n;
    for (i = 0; i + 2 < bytes.n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8 | in[i + 2];
        *o++ = abc[v >> 18];
        *o++ = abc[(v >> 12) & 63];
        *o++ = abc[(v >> 6) & 63];
        *o++ = abc[v & 63];
    }
    if (i < bytes.n) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < bytes.n ? (uint32_t)in[i + 1] << 8 : 0);
        *o++ = abc[v >> 18];
        *o++ = abc[(v >> 12) & 63];
        if (i + 1 < bytes.n)
            *o++ = abc[(v >> 6) & 63];
    }
    out->n = (size_t)(o - out->p);
}

static int digit(unsigned char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+' || c == '-')
        return 62;
    if (c == '/' || c == '_')
        return 63;
    return -1;
}

bool pt_base64_decode(pt_str in, pt_buf *out)
{
    uint32_t v = 0;
    unsigned bits = 0;
    size_t i, pad = 0;
    char *o;
    if (!pt_buf_reserve(out, in.n / 4 * 3 + 3))
        return false;
    o = out->p + out->n;
    for (i = 0; i < in.n; i++) {
        unsigned char c = (unsigned char)in.p[i];
        int d;
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t')
            continue;
        if (c == '=') {
            pad++;
            continue;
        }
        if (pad || (d = digit(c)) < 0)
            return false; /* data after padding, or not base64 */
        v = v << 6 | (uint32_t)d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            *o++ = (char)(v >> bits);
            v &= (1u << bits) - 1;
        }
    }
    if (bits >= 6 || pad > 2)
        return false;
    out->n = (size_t)(o - out->p);
    return true;
}
