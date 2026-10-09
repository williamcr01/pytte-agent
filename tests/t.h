/* Tiny test helpers shared by the test programs. */
#ifndef PT_TEST_H
#define PT_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pytte_base.h"

static int t_failed, t_checks;

#define CHECK(c)                                                                        \
    do {                                                                                \
        t_checks++;                                                                     \
        if (!(c)) {                                                                     \
            t_failed++;                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c);      \
        }                                                                               \
    } while (0)

#define CHECK_STR(a, b)                                                                 \
    do {                                                                                \
        pt_str a_ = (a);                                                                \
        const char *b_ = (b);                                                           \
        t_checks++;                                                                     \
        if (!pt_str_eqz(a_, b_)) {                                                      \
            t_failed++;                                                                 \
            fprintf(stderr, "%s:%d: \"%.*s\" != \"%s\"\n", __FILE__, __LINE__, (int)a_.n, \
                    a_.p ? a_.p : "", b_);                                              \
        }                                                                               \
    } while (0)

#define RUN(fn)                                \
    do {                                       \
        fprintf(stderr, "  %s\n", #fn);        \
        fn();                                  \
    } while (0)

static void *t_alloc(void *ud, void *p, size_t old, size_t n)
{
    (void)ud;
    (void)old;
    if (!n) {
        free(p);
        return NULL;
    }
    return realloc(p, n);
}

static const pt_allocator T_AL = {t_alloc, NULL};

static int t_done(const char *name)
{
    fprintf(stderr, "%s: %d checks, %d failed\n", name, t_checks, t_failed);
    return t_failed ? 1 : 0;
}

#endif
