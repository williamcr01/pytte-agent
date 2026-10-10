/* JSON Schema subset: never crashes, whatever the schema and the value.
 * The input is a schema and a value separated by a NUL byte. */
#include <stdlib.h>
#include <string.h>

#include "pytte_base.h"

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

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t n);

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t n)
{
    pt_allocator al = {al_fn, NULL};
    pt_arena a;
    const char *sep = (const char *)memchr(data, 0, n);
    size_t k = sep ? (size_t)(sep - (const char *)data) : n;
    pt_json *schema, *value;
    char err[200];
    pt_arena_init(&a, al, 0);
    schema = pt_json_parse((pt_str){(const char *)data, k}, &a, NULL);
    value = sep ? pt_json_parse((pt_str){sep + 1, n - k - 1}, &a, NULL) : NULL;
    if (schema && value)
        pt_schema_check(schema, value, &a, err, sizeof err);
    pt_arena_free(&a);
    return 0;
}
