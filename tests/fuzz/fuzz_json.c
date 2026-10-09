/* JSON parser: never crashes; whatever parses re-serializes to JSON that
 * parses again. */
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

static void rewrite(pt_jw *w, const pt_json *v, pt_arena *a)
{
    const pt_json *c;
    switch (v->type) {
    case PT_J_OBJ:
        pt_jw_obj(w);
        for (c = v->child; c; c = c->next) {
            pt_jw_key(w, pt_arena_cstr(a, c->key));
            rewrite(w, c, a);
        }
        pt_jw_obj_end(w);
        break;
    case PT_J_ARR:
        pt_jw_arr(w);
        for (c = v->child; c; c = c->next)
            rewrite(w, c, a);
        pt_jw_arr_end(w);
        break;
    case PT_J_STR:
        pt_jw_str(w, pt_json_str(v, a));
        break;
    default:
        pt_jw_raw(w, v->raw);
    }
}

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size);
int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
    pt_allocator al = {al_fn, NULL};
    pt_arena a;
    pt_json *v;
    pt_arena_init(&a, al, 0);
    v = pt_json_parse((pt_str){(const char *)data, size}, &a, NULL);
    if (v) {
        pt_buf b;
        pt_jw w;
        pt_buf_init(&b, al);
        pt_jw_init(&w, &b);
        rewrite(&w, v, &a);
        /* keys with embedded NULs are cut by cstr; values must still parse */
        if (!b.oom && !pt_json_parse(pt_buf_str(&b), &a, NULL))
            abort();
        pt_buf_free(&b);
    }
    pt_arena_free(&a);
    return 0;
}
