#include "pytte_base.h"

#include <string.h>

pt_str pt_strz(const char *z)
{
    pt_str s;
    s.p = z;
    s.n = z ? strlen(z) : 0;
    return s;
}

bool pt_str_eq(pt_str a, pt_str b)
{
    return a.n == b.n && (a.n == 0 || memcmp(a.p, b.p, a.n) == 0);
}

bool pt_str_eqz(pt_str a, const char *z)
{
    return pt_str_eq(a, pt_strz(z));
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

bool pt_str_ieq(pt_str a, pt_str b)
{
    size_t i;
    if (a.n != b.n)
        return false;
    for (i = 0; i < a.n; i++)
        if (lower(a.p[i]) != lower(b.p[i]))
            return false;
    return true;
}

bool pt_str_starts(pt_str s, pt_str prefix)
{
    return s.n >= prefix.n && (prefix.n == 0 || memcmp(s.p, prefix.p, prefix.n) == 0);
}

static bool contains(pt_str hay, pt_str needle, bool icase)
{
    size_t i, j;
    if (needle.n == 0)
        return true;
    if (hay.n < needle.n)
        return false;
    for (i = 0; i + needle.n <= hay.n; i++) {
        for (j = 0; j < needle.n; j++) {
            char a = hay.p[i + j], b = needle.p[j];
            if (icase ? lower(a) != lower(b) : a != b)
                break;
        }
        if (j == needle.n)
            return true;
    }
    return false;
}

bool pt_str_contains(pt_str hay, pt_str needle)
{
    return contains(hay, needle, false);
}

bool pt_str_icontains(pt_str hay, pt_str needle)
{
    return contains(hay, needle, true);
}

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

pt_str pt_str_trim(pt_str s)
{
    while (s.n && is_space(s.p[0])) {
        s.p++;
        s.n--;
    }
    while (s.n && is_space(s.p[s.n - 1]))
        s.n--;
    return s;
}

long pt_str_find(pt_str s, char c)
{
    size_t i;
    for (i = 0; i < s.n; i++)
        if (s.p[i] == c)
            return (long)i;
    return -1;
}

bool pt_str_to_u64(pt_str s, uint64_t *out)
{
    uint64_t v = 0;
    size_t i;
    if (s.n == 0)
        return false;
    for (i = 0; i < s.n; i++) {
        unsigned d;
        if (s.p[i] < '0' || s.p[i] > '9')
            return false;
        d = (unsigned)(s.p[i] - '0');
        if (v > (UINT64_MAX - d) / 10)
            return false;
        v = v * 10 + d;
    }
    *out = v;
    return true;
}
