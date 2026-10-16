/* Permission rules from config files (section 15.5).
 *
 *   "permissions": {
 *     "allow": ["bash(git diff:*)", "web_fetch(docs.python.org)", "mcp__github__*"],
 *     "deny":  ["bash(rm -rf:*)"]
 *   }
 *
 * The user's config.json gives rules that apply at once. A project's
 * (.pytte/config.json in the working directory) deny rules apply at once
 * too, as they only take permission away; its allow rules apply once
 * trusted. trust.json, next to config.json, keeps a hash of each trusted
 * project's allow list, so any change asks again (as for MCP servers). */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cli/cli.h"

static void user_path(char *out, size_t cap)
{
    char dir[4000];
    cli_config_dir(dir, sizeof dir);
    snprintf(out, cap, "%s/config.json", dir);
}

/* The key trust.json keeps a project's allow list under. */
static void trust_key(const char *cwd, char *out, size_t cap)
{
    snprintf(out, cap, "%s#permissions", cwd);
}

/* The rules of one list, "allow" or "deny": strings that are rules. */
static void add_list(pt_rt *rt, const pt_json *perms, const char *key, bool deny, pt_rule_src src, pt_arena *ar)
{
    const pt_json *list = pt_json_get(perms, key), *e;
    for (e = pt_json_is(list, PT_J_ARR) ? list->child : NULL; e; e = e->next)
        if (pt_json_is(e, PT_J_STR))
            pt_rt_rule_add(rt, deny, pt_json_str(e, ar), src); /* a bad one was reported when the config loaded */
}

/* The "permissions" object of a config file, parsed into ar; text is the
 * file's contents (the caller frees it), NULL when there is none. */
static const pt_json *perms_of(const char *path, pt_arena *ar, char **text)
{
    size_t n;
    const pt_json *j;
    *text = pt_sys_read_file(path, &n);
    if (!*text)
        return NULL;
    j = pt_json_parse((pt_str){*text, n}, ar, NULL);
    j = pt_json_is(j, PT_J_OBJ) ? pt_json_get(j, "permissions") : NULL;
    return pt_json_is(j, PT_J_OBJ) ? j : NULL;
}

/* The hash of a project's allow list ("" when it has none). */
static void allow_hash(const char *cwd, const pt_json *perms, char out[65])
{
    const pt_json *allow = pt_json_get(perms, "allow");
    pt_sha256 sh;
    unsigned char d[32];
    out[0] = 0;
    if (!pt_json_is(allow, PT_J_ARR) || !allow->len)
        return;
    pt_sha256_init(&sh);
    pt_sha256_update(&sh, cwd, strlen(cwd) + 1);
    pt_sha256_update(&sh, allow->raw.p, allow->raw.n);
    pt_sha256_final(&sh, d);
    pt_hex(out, d, 32);
    out[64] = 0;
}

static bool trusted_hash(const char *cwd, const char *hash)
{
    char path[4096], key[4200];
    pt_buf have;
    bool ok;
    if (!hash[0])
        return false;
    cli_trust_path(path, sizeof path);
    trust_key(cwd, key, sizeof key);
    pt_buf_init(&have, pt_sys_allocator());
    ok = cli_file_get(path, key, &have) && pt_str_eqz(pt_buf_str(&have), hash);
    pt_buf_free(&have);
    return ok;
}

bool cli_perms_apply(pt_rt *rt, const char *cwd)
{
    char user[4096], proj[4200], *t1 = NULL, *t2 = NULL, hash[65];
    pt_arena ar;
    const pt_json *u, *p;
    bool ok;
    pt_rt_rules_drop_config(rt);
    pt_arena_init(&ar, pt_sys_allocator(), 0);
    user_path(user, sizeof user);
    snprintf(proj, sizeof proj, "%s/.pytte/config.json", cwd);
    u = perms_of(user, &ar, &t1);
    p = perms_of(proj, &ar, &t2);
    add_list(rt, u, "deny", true, PT_RULE_USER, &ar);
    add_list(rt, u, "allow", false, PT_RULE_USER, &ar);
    add_list(rt, p, "deny", true, PT_RULE_PROJECT, &ar);
    add_list(rt, p, "allow", false, PT_RULE_PROJECT, &ar);
    allow_hash(cwd, p, hash);
    pt_rt_rules_trust(rt, trusted_hash(cwd, hash));
    ok = !ar.oom;
    pt_arena_free(&ar);
    free(t1);
    free(t2);
    return ok;
}

bool cli_perms_trust(const char *cwd, bool on, char *err, size_t cap)
{
    char proj[4200], path[4096], key[4200], hash[65], *text;
    pt_arena ar;
    pt_buf v;
    bool ok;
    snprintf(proj, sizeof proj, "%s/.pytte/config.json", cwd);
    pt_arena_init(&ar, pt_sys_allocator(), 0);
    allow_hash(cwd, perms_of(proj, &ar, &text), hash);
    pt_arena_free(&ar);
    free(text);
    if (!hash[0]) {
        snprintf(err, cap, "this project has no allow rules to trust (permissions.allow in .pytte/config.json)");
        return false;
    }
    cli_trust_path(path, sizeof path);
    trust_key(cwd, key, sizeof key);
    pt_buf_init(&v, pt_sys_allocator());
    if (on)
        pt_json_quote(&v, pt_strz(hash));
    ok = !v.oom && cli_file_put(path, key, pt_buf_str(&v), err, cap);
    pt_buf_free(&v);
    return ok;
}

bool cli_perms_allow_save(pt_str spec, char *err, size_t cap)
{
    char path[4096];
    size_t n = 0;
    char *text = NULL;
    pt_arena ar;
    const pt_json *root, *perms, *allow, *e;
    pt_buf list, out;
    bool ok = false, have = false;
    user_path(path, sizeof path);
    text = pt_sys_read_file(path, &n);
    pt_arena_init(&ar, pt_sys_allocator(), 0);
    pt_buf_init(&list, pt_sys_allocator());
    pt_buf_init(&out, pt_sys_allocator());
    root = text ? pt_json_parse((pt_str){text, n}, &ar, NULL) : NULL;
    if (text && !pt_json_is(root, PT_J_OBJ)) {
        snprintf(err, cap, "not saved, %s does not parse", path);
        goto done;
    }
    perms = root ? pt_json_get(root, "permissions") : NULL;
    allow = pt_json_is(perms, PT_J_OBJ) ? pt_json_get(perms, "allow") : NULL;
    if (allow && !pt_json_is(allow, PT_J_ARR)) {
        snprintf(err, cap, "not saved, permissions.allow in %s is not a list", path);
        goto done;
    }
    pt_buf_putc(&list, '[');
    for (e = allow ? allow->child : NULL; e; e = e->next) {
        if (pt_json_is(e, PT_J_STR) && pt_str_eq(pt_json_str(e, &ar), spec))
            have = true;
        pt_buf_putz(&list, e == allow->child ? "" : ", ");
        pt_buf_puts(&list, e->raw);
    }
    if (!have) {
        pt_buf_putz(&list, allow && allow->child ? ", " : "");
        pt_json_quote(&list, spec);
    }
    pt_buf_putc(&list, ']');
    if (list.oom || ar.oom) {
        snprintf(err, cap, "out of memory saving the rule");
        goto done;
    }
    ok = cli_json_set(text ? (pt_str){text, n} : PT_S("{}\n"), "permissions.allow", pt_buf_str(&list), &out, err, cap);
    if (ok)
        ok = cli_write_atomic(path, pt_buf_str(&out), err, cap);
done:
    pt_buf_free(&list);
    pt_buf_free(&out);
    pt_arena_free(&ar);
    free(text);
    return ok;
}

bool cli_perms_check(const pt_json *root, const char *path, char *warn, size_t cap)
{
    const pt_json *perms = pt_json_get(root, "permissions"), *list, *e;
    pt_arena ar;
    bool ok = true;
    int pass;
    if (!pt_json_is(perms, PT_J_OBJ))
        return true;
    pt_arena_init(&ar, pt_sys_allocator(), 0);
    for (pass = 0; pass < 2 && ok; pass++) {
        const char *key = pass ? "deny" : "allow";
        list = pt_json_get(perms, key);
        if (list && !pt_json_is(list, PT_J_ARR)) {
            snprintf(warn, cap, "%s: permissions.%s is a list of strings", path, key);
            ok = false;
        }
        for (e = ok && list ? list->child : NULL; e && ok; e = e->next) {
            const char *why = "a rule is a string";
            ok = pt_json_is(e, PT_J_STR) && pt_rule_check(pt_json_str(e, &ar), &why);
            if (!ok)
                snprintf(warn, cap, "%s: permissions.%s: %s; skipped", path, key, why);
        }
    }
    pt_arena_free(&ar);
    return ok;
}
