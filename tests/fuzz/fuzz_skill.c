/* Skills: SKILL.md frontmatter, $name and /skill: expansion, and the
 * display's skill blocks never crash, whatever the text. The input is the
 * message and, after a NUL byte, the SKILL.md every skill reads. */
#include "rt/prompt.c"

#include <stdlib.h>
#include <string.h>

static pt_str file;

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

pt_allocator pt_sys_allocator(void)
{
    pt_allocator al = {al_fn, NULL};
    return al;
}

char *pt_sys_read_file(const char *path, size_t *n)
{
    char *p = (char *)malloc(file.n + 1);
    (void)path;
    if (p) {
        memcpy(p, file.p, file.n);
        p[file.n] = 0;
        *n = file.n;
    }
    return p;
}

bool pt_rt_grow(void **arr, uint32_t *cap, uint32_t need, size_t elem)
{
    (void)arr;
    (void)cap;
    (void)need;
    (void)elem;
    return false;
}

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t n);

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t n)
{
    pt_rt rt;
    pt_rt_skill skills[2];
    frontmatter f;
    pt_buf out;
    char err[200];
    const char *sep = (const char *)memchr(data, 0, n);
    size_t k = sep ? (size_t)(sep - (const char *)data) : n;
    pt_str text = {(const char *)data, k}, name;
    file = sep ? (pt_str){sep + 1, n - k - 1} : PT_STR_NULL;
    memset(&rt, 0, sizeof rt);
    memset(skills, 0, sizeof skills);
    skills[0].name = PT_S("a");
    skills[0].path = PT_S("/a/SKILL.md");
    skills[1].name = PT_S("b-c.d");
    skills[1].path = PT_S("/b/SKILL.md");
    rt.skills = skills;
    rt.nskills = 2;
    pt_buf_init(&f.name, pt_sys_allocator());
    pt_buf_init(&f.description, pt_sys_allocator());
    parse_skill(file, &f);
    pt_buf_free(&f.name);
    pt_buf_free(&f.description);
    pt_buf_init(&out, pt_sys_allocator());
    if (pt_rt_expand_skills(&rt, text, &out, err, sizeof err) == 1) {
        pt_str shown = pt_buf_str(&out);
        while (pt_rt_skill_block(&shown, &name))
            ;
    }
    while (pt_rt_skill_block(&text, &name))
        ;
    pt_buf_free(&out);
    return 0;
}
