/* The menu under the input: it follows the draft (commands after "/",
 * then their arguments; $skills and @mentions anywhere), and Tab
 * completes the selection. */
#define _POSIX_C_SOURCE 200809L
#include "tui/app.h"

int app_find_setting(pt_str name)
{
    size_t i;
    for (i = 0; i < NSETTINGS; i++)
        if (pt_str_eqz(name, app_settings[i].name) ||
            (app_settings[i].k != SET_TOOLS && pt_str_eqz(name, cli_config_key(app_settings[i].k))))
            return (int)i;
    return -1;
}

bool app_menu_takes_arg(pt_str cmd)
{
    return pt_str_eqz(cmd, "model") || pt_str_eqz(cmd, "effort") || pt_str_eqz(cmd, "thinking") ||
           pt_str_eqz(cmd, "resume") || pt_str_eqz(cmd, "settings") || pt_str_eqz(cmd, "login") ||
           pt_str_eqz(cmd, "rewind") || pt_str_eqz(cmd, "tools");
}

static void add_item(app *a, const char *label, size_t ln, const char *desc)
{
    app_menu_add(a, label, ln, desc ? pt_strz(desc) : PT_STR_NULL, desc ? strlen(desc) : 0);
}

/* An item whose description is the first dn bytes of desc (not NUL-ended). */
void app_menu_add(app *a, const char *label, size_t ln, pt_str desc, size_t dn)
{
    item *it;
    if (a->menu.n >= MAX_ITEMS)
        return;
    it = &a->menu.items[a->menu.n++];
    it->at = a->menu.buf.n;
    it->n = ln;
    pt_buf_put(&a->menu.buf, label, ln);
    it->dat = a->menu.buf.n;
    it->dn = dn;
    if (dn)
        pt_buf_put(&a->menu.buf, desc.p, dn);
}

pt_str app_item_label(const app *a, int i)
{
    return (pt_str){a->menu.buf.p + a->menu.items[i].at, a->menu.items[i].n};
}

static void add_models(app *a, pt_str q)
{
    const pt_ai_catalog *cat = cli_catalog();
    char ref[256];
    uint32_t i;
    for (i = 0; i < cat->nmodels; i++) {
        int n = snprintf(ref, sizeof ref, "%s/%s", cat->providers[cat->models[i].provider].id,
                         cat->models[i].id);
        if (n > 0 && (size_t)n < sizeof ref && pt_str_icontains(pt_strz(ref), q))
            add_item(a, ref, (size_t)n, cat->models[i].name);
    }
}

/* Skills whose invocation (lead and name: "$name", "skill:name") starts
 * with q. */
static void add_skills(app *a, const char *lead, pt_str q, bool rescan)
{
    uint32_t i, n = 0;
    const pt_rt_skill *sk = a->open ? pt_rt_skills(a->s.rt, rescan, &n) : NULL;
    char label[80];
    for (i = 0; i < n; i++) {
        int ln = snprintf(label, sizeof label, "%s%.*s", lead, (int)sk[i].name.n, sk[i].name.p);
        if (ln > 0 && (size_t)ln < sizeof label && pt_str_starts(pt_strz(label), q)) {
            pt_buf_reset(&a->tmp);
            pt_buf_puts(&a->tmp, sk[i].description);
            pt_buf_putc(&a->tmp, 0);
            add_item(a, label, (size_t)ln, a->tmp.p);
        }
    }
    a->menu.of_skills = true;
}

/* MCP prompts whose command ("server:name") starts with q. */
static void add_prompts(app *a, pt_str q)
{
    uint32_t i, k, n = 0;
    const pt_rt_mcp_prompt *p = a->open ? pt_rt_mcp_prompts(a->s.rt, &n) : NULL;
    char label[160];
    for (i = 0; i < n; i++) {
        int ln = snprintf(label, sizeof label, "%.*s:%.*s", (int)p[i].server.n, p[i].server.p, (int)p[i].name.n,
                          p[i].name.p);
        if (ln <= 0 || (size_t)ln >= sizeof label || !pt_str_starts(pt_strz(label), q))
            continue;
        pt_buf_reset(&a->tmp);
        pt_buf_puts(&a->tmp, p[i].description.n ? p[i].description : PT_S("MCP prompt"));
        for (k = 0; k < p[i].nargs; k++) {
            pt_buf_putz(&a->tmp, k ? ", " : " · ");
            pt_buf_puts(&a->tmp, p[i].args[k].name);
            if (!p[i].args[k].required)
                pt_buf_putc(&a->tmp, '?');
        }
        pt_buf_putc(&a->tmp, 0);
        add_item(a, label, (size_t)ln, a->tmp.p);
    }
}

/* The prompt a command names ("/server:name ..." or "server:name"). */
const pt_rt_mcp_prompt *app_find_prompt(app *a, pt_str cmd)
{
    uint32_t i, n = 0;
    const pt_rt_mcp_prompt *p = a->open ? pt_rt_mcp_prompts(a->s.rt, &n) : NULL;
    long colon;
    if (cmd.n && cmd.p[0] == '/')
        cmd = (pt_str){cmd.p + 1, cmd.n - 1};
    for (i = 0; i < cmd.n && cmd.p[i] != ' ' && cmd.p[i] != '\n'; i++)
        ;
    cmd.n = i;
    if ((colon = pt_str_find(cmd, ':')) <= 0)
        return NULL;
    for (i = 0; i < n; i++)
        if (pt_str_eq(p[i].server, (pt_str){cmd.p, (size_t)colon}) &&
            pt_str_eq(p[i].name, (pt_str){cmd.p + colon + 1, cmd.n - (size_t)colon - 1}))
            return &p[i];
    return NULL;
}

/* Resources whose mention ("@server:uri") starts with q, or whose name
 * has q's text; add_paths then adds files and directories. */
static void add_mentions(app *a, pt_str q)
{
    uint32_t i, n = 0;
    const pt_rt_mcp_resource *r = a->open ? pt_rt_mcp_resources(a->s.rt, &n) : NULL;
    pt_str rest = {q.p + 1, q.n - 1};
    for (i = 0; i < n && a->menu.n < MAX_ITEMS; i++) {
        pt_buf_reset(&a->tmp2);
        pt_buf_putc(&a->tmp2, '@');
        pt_buf_puts(&a->tmp2, r[i].server);
        pt_buf_putc(&a->tmp2, ':');
        pt_buf_puts(&a->tmp2, r[i].uri);
        if (a->tmp2.oom || a->tmp2.n > 300 || memchr(a->tmp2.p, ' ', a->tmp2.n))
            continue;
        if (!pt_str_starts(pt_buf_str(&a->tmp2), q) && !(rest.n && pt_str_icontains(r[i].name, rest)))
            continue;
        pt_buf_reset(&a->tmp);
        pt_buf_puts(&a->tmp, r[i].name);
        if (r[i].description.n) {
            pt_buf_putz(&a->tmp, r[i].name.n ? " · " : "");
            pt_buf_puts(&a->tmp, r[i].description);
        }
        pt_buf_putc(&a->tmp, 0);
        add_item(a, a->tmp2.p, a->tmp2.n, a->tmp.p);
    }
}

#define PATHS_MAX 20000 /* paths a menu looks through: a walk that is quick, and a list that is small */

/* Paths under cwd for "@q": when q is empty or ends in "/", the entries of
 * that directory; otherwise those it starts, then those it is part of.
 * fresh: the menu just opened, so the walk is made again. */
static void add_paths(app *a, pt_str q, bool fresh)
{
    pt_str rest = {q.p + 1, q.n - 1}, all;
    bool dir = !rest.n || rest.p[rest.n - 1] == '/';
    int pass;
    if (!a->open)
        return;
    if (fresh) {
        bool cut;
        pt_buf_reset(&a->menu.paths);
        pt_rt_walk_paths(a->s.rt, PATHS_MAX, &a->menu.paths, &cut);
    }
    all = pt_buf_str(&a->menu.paths);
    for (pass = 0; pass < (dir ? 1 : 2); pass++) {
        size_t at = 0, e;
        for (; at < all.n && a->menu.n < MAX_ITEMS; at = e + 1) {
            pt_str path;
            bool starts;
            e = at;
            while (e < all.n && all.p[e] != '\n')
                e++;
            path = (pt_str){all.p + at, e - at};
            if (!path.n || path.n > 300 || memchr(path.p, ' ', path.n))
                continue; /* a mention ends at a space */
            starts = pt_str_starts(path, rest);
            if (dir) { /* direct children: no slash after the prefix, but a directory's own */
                pt_str tail = {path.p + rest.n, path.n - rest.n};
                if (!starts || !tail.n || pt_str_find((pt_str){tail.p, tail.n - 1}, '/') >= 0)
                    continue;
            } else if (pass == 0 ? !starts : starts || !pt_str_icontains(path, rest)) {
                continue;
            }
            pt_buf_reset(&a->tmp2);
            pt_buf_putc(&a->tmp2, '@');
            pt_buf_puts(&a->tmp2, path);
            add_item(a, a->tmp2.p, a->tmp2.n, NULL);
        }
    }
}

static const char *const mcp_subs[] = {"login", "logout", "restart", "trust", "untrust"};
static const char *const perm_subs[] = {"trust", "untrust"};

/* "/login api|oauth <provider> <key>": where the key (or the address a
 * login's browser ended on) starts, or 0. It is shown masked and never
 * reaches the input history or a saved draft. */
size_t app_secret_at(pt_str d)
{
    size_t at = 7;
    int words;
    if (!pt_str_starts(d, PT_S("/login ")))
        return 0;
    for (words = 0; words < 2; words++) {
        long sp = pt_str_find((pt_str){d.p + at, d.n - at}, ' ');
        if (sp < 0)
            return 0;
        at += (size_t)sp + 1;
    }
    return at;
}

static const char *const login_subs[] = {"api", "oauth", "search"};
static const char *const login_descs[] = {"save an API key, billed by the provider",
                                          "sign in with a subscription (ChatGPT)",
                                          "Search API keys, for web_search"};

/* The word being typed: after the last space or newline. */
static pt_str last_word(pt_str d)
{
    size_t at = d.n;
    while (at && d.p[at - 1] != ' ' && d.p[at - 1] != '\n' && d.p[at - 1] != '\t')
        at--;
    return (pt_str){d.p + at, d.n - at};
}

static void add_list(app *a, const char *const *v, size_t n, pt_str q)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (pt_str_starts(pt_strz(v[i]), q))
            add_item(a, v[i], strlen(v[i]), NULL);
}

/* The menu follows the draft: commands after "/", then their arguments. */
static void list_sessions(void *ud)
{
    session_list *list = (session_list *)ud;
    char root[4096], dir[4200], cwd[4096], name[CLI_NAME_MAX + 1];
    uint32_t i;
    list->n = cli_sessions_list(list->cwd, true, list->ids, 50);
    cli_sessions_dir(root, sizeof root);
    for (i = 0; i < list->n; i++) {
        const char *wt = "";
        snprintf(dir, sizeof dir, "%s/%s", root, list->ids[i]);
        cli_session_name(dir, name, sizeof name);
        if (cli_session_cwd(list->ids[i], cwd, sizeof cwd) && strcmp(cwd, list->cwd) != 0)
            wt = strrchr(cwd, '/') + 1; /* under list->cwd: there is a slash */
        snprintf(list->names[i], sizeof list->names[i], "%.64s%s%s", wt, wt[0] && name[0] ? " · " : "", name);
    }
}

static session_list *get_sessions(app *a, bool refresh)
{
    session_list *list = a->menu.sessions;
    if (!list) {
        list = (session_list *)calloc(1, sizeof *list);
        if (!list)
            return NULL;
        list->worker = pt_worker_new();
        if (!list->worker) {
            free(list);
            return NULL;
        }
        snprintf(list->cwd, sizeof list->cwd, "%s", a->sess.start_dir);
        a->menu.sessions = list;
    }
    if (refresh && !list->pending)
        list->ready = false;
    if (!list->ready && !list->pending)
        list->pending = pt_worker_start(list->worker, list_sessions, list);
    return list->ready ? list : NULL;
}

void app_menu_update(app *a)
{
    bool was_sessions = a->menu.of_sessions, was_skills = a->menu.of_skills, was_rewind = a->menu.of_rewind;
    pt_str d = pt_buf_str(&a->in.text), cmd, arg, word = last_word(d);
    long sp;
    int old = a->menu.n;
    a->menu.n = 0;
    pt_buf_reset(&a->menu.buf);
    a->menu.of_sessions = a->menu.of_skills = a->menu.of_mentions = a->menu.of_rewind = false;
    if (a->menu.kind == MENU_SETTINGS)
        return;
    if (a->menu.kind == MENU_TOOLS) {
        app_tools_list(a);
        return;
    }
    a->menu.kind = MENU_NONE;
    if (a->menu.paths.cap && (a->menu.closed || !word.n || word.p[0] != '@')) {
        pt_buf_free(&a->menu.paths); /* the list lives as long as the @ menu */
        pt_buf_init(&a->menu.paths, pt_sys_allocator());
    }
    if (!a->menu.closed && word.n && word.p[0] == '@') { /* @server:uri and @path anywhere */
        add_mentions(a, word);
        add_paths(a, word, !a->menu.paths.n);
        a->menu.kind = a->menu.n ? MENU_ARG : MENU_NONE;
        a->menu.of_mentions = a->menu.n > 0;
        if (a->menu.n != old || a->menu.sel >= a->menu.n)
            a->menu.sel = a->menu.top = 0;
        return;
    }
    if (!a->menu.closed && word.n && word.p[0] == '$') { /* $skill anywhere */
        add_skills(a, "$", word, !was_skills);
        a->menu.kind = a->menu.n ? MENU_ARG : MENU_NONE;
        if (a->menu.n != old || a->menu.sel >= a->menu.n)
            a->menu.sel = a->menu.top = 0;
        return;
    }
    if (a->menu.closed || !d.n || d.p[0] != '/' || pt_str_find(d, '\n') >= 0)
        return;
    d.p++;
    d.n--;
    sp = pt_str_find(d, ' ');
    if (sp < 0) {
        size_t i;
        a->menu.kind = MENU_CMD;
        for (i = 0; i < NCMDS; i++)
            if (pt_str_starts(pt_strz(app_cmds[i].name), d))
                add_item(a, app_cmds[i].name, strlen(app_cmds[i].name), app_cmds[i].desc);
        add_skills(a, "skill:", d, !was_skills);
        add_prompts(a, d);
    } else {
        cmd = (pt_str){d.p, (size_t)sp};
        arg = (pt_str){d.p + sp + 1, d.n - (size_t)sp - 1};
        a->menu.kind = MENU_ARG;
        if (pt_str_eqz(cmd, "settings")) { /* "/settings model <id>" etc. */
            long sp2 = pt_str_find(arg, ' ');
            if (sp2 >= 0) {
                int st = app_find_setting((pt_str){arg.p, (size_t)sp2});
                arg = (pt_str){arg.p + sp2 + 1, arg.n - (size_t)sp2 - 1};
                cmd = PT_STR_NULL;
                if (st >= 0 && app_settings[st].vals)
                    add_list(a, app_settings[st].vals, (size_t)app_settings[st].nv, arg);
                else if (st >= 0 && app_settings[st].k == SET_TOOLS)
                    cmd = PT_S("tools");
                else if (st >= 0 && app_settings[st].k != CFG_PAGER)
                    cmd = PT_S("model");
                if (st >= 0 && app_settings[st].k == CFG_COMPACT_MODEL && pt_str_starts(PT_S("none"), arg))
                    add_item(a, "none", 4, "the session's model compacts");
            } else {
                size_t i;
                for (i = 0; i < NSETTINGS; i++)
                    if (pt_str_starts(pt_strz(app_settings[i].name), arg))
                        add_item(a, app_settings[i].name, strlen(app_settings[i].name),
                                 app_settings[i].k == SET_TOOLS ? "tools.<name>" : cli_config_key(app_settings[i].k));
                cmd = PT_STR_NULL;
            }
        }
        if (pt_str_eqz(cmd, "model")) {
            add_models(a, arg);
        } else if (pt_str_eqz(cmd, "effort")) {
            size_t i;
            for (i = 0; i < NLEVELS; i++) /* those the model takes */
                if ((!a->model.reasons || !a->model.efforts || a->model.efforts & 1u << i) && pt_str_starts(pt_strz(app_levels[i]), arg))
                    add_item(a, app_levels[i], strlen(app_levels[i]), NULL);
        } else if (pt_str_eqz(cmd, "thinking")) {
            add_list(a, app_th_names, NTH, arg);
        } else if (pt_str_eqz(cmd, "tools")) {
            long sp2 = pt_str_find(arg, ' ');
            if (sp2 < 0) { /* the tools, each with its state */
                uint32_t i, n = 0;
                const pt_tool_def *defs = a->open ? pt_rt_tool_all(a->s.rt, &n) : NULL;
                for (i = 0; i < n; i++)
                    if (pt_str_starts(defs[i].name, arg) && !cli_tool_leader(defs[i].name))
                        add_item(a, defs[i].name.p, defs[i].name.n, cli_tool_on(a->cfg, defs[i].name) ? "on" : "off");
            } else if (pt_str_find((pt_str){arg.p + sp2 + 1, arg.n - (size_t)sp2 - 1}, ' ') < 0) {
                static const char *const on_off[] = {"on", "off"};
                add_list(a, on_off, 2, (pt_str){arg.p + sp2 + 1, arg.n - (size_t)sp2 - 1});
            }
        } else if (pt_str_eqz(cmd, "login")) {
            long sp2 = pt_str_find(arg, ' ');
            if (sp2 < 0) {
                size_t i;
                for (i = 0; i < 3; i++)
                    if (pt_str_starts(pt_strz(login_subs[i]), arg))
                        add_item(a, login_subs[i], strlen(login_subs[i]), login_descs[i]);
            } else {
                pt_str how = {arg.p, (size_t)sp2}, name = {arg.p + sp2 + 1, arg.n - (size_t)sp2 - 1};
                bool oauth = pt_str_eqz(how, "oauth");
                const pt_ai_catalog *cat = cli_catalog();
                uint32_t i;
                for (i = 0; pt_str_eqz(how, "search") && pt_str_find(name, ' ') < 0 && i < 2; i++)
                    if (pt_str_starts(pt_strz(cli_search_backends[i]), name))
                        add_item(a, cli_search_backends[i], strlen(cli_search_backends[i]), i ? "Parallel" : "Exa");
                for (i = 0; (oauth || pt_str_eqz(how, "api")) && pt_str_find(name, ' ') < 0 && i < cat->nproviders;
                     i++) {
                    const pt_provider *q = &cat->providers[i];
                    if (q->auth != PT_AUTH_NONE && (q->auth == PT_AUTH_OAUTH) == oauth &&
                        pt_str_starts(pt_strz(q->id), name))
                        add_item(a, q->id, strlen(q->id), q->name);
                }
            }
        } else if (pt_str_eqz(cmd, "mcp")) {
            long sp2 = pt_str_find(arg, ' ');
            if (sp2 < 0) {
                add_list(a, mcp_subs, sizeof mcp_subs / sizeof mcp_subs[0], arg);
            } else if (a->open && pt_str_find((pt_str){arg.p + sp2 + 1, arg.n - (size_t)sp2 - 1}, ' ') < 0 &&
                       !pt_str_starts(arg, PT_S("trust")) && !pt_str_starts(arg, PT_S("untrust"))) {
                pt_rt_mcp_info in[CLI_MCP_MAX];
                pt_str name = {arg.p + sp2 + 1, arg.n - (size_t)sp2 - 1};
                uint32_t i, n = pt_rt_mcp_servers(a->s.rt, in, CLI_MCP_MAX);
                for (i = 0; i < n && i < CLI_MCP_MAX; i++)
                    if (pt_str_starts(pt_strz(in[i].name), name))
                        add_item(a, in[i].name, strlen(in[i].name), in[i].target);
            }
        } else if (pt_str_eqz(cmd, "rewind")) {
            long sp2 = pt_str_find(arg, ' ');
            if (sp2 < 0) {
                app_rewind_messages(a, arg, !was_rewind);
            } else if (pt_str_find((pt_str){arg.p + sp2 + 1, arg.n - (size_t)sp2 - 1}, ' ') < 0) {
                static const char *const modes[] = {"both", "files", "conversation"};
                static const char *const descs[] = {"files and conversation: continue in a fork before it",
                                                    "only the files; this session goes on",
                                                    "only the conversation, in a fork; the files stay"};
                pt_str w = last_word(arg);
                size_t i;
                for (i = 0; i < 3; i++)
                    if (pt_str_starts(pt_strz(modes[i]), w))
                        add_item(a, modes[i], strlen(modes[i]), descs[i]);
            }
        } else if (pt_str_eqz(cmd, "permissions")) {
            if (pt_str_find(arg, ' ') < 0)
                add_list(a, perm_subs, sizeof perm_subs / sizeof perm_subs[0], arg);
        } else if (pt_str_eqz(cmd, "resume")) {
            session_list *list = get_sessions(a, !was_sessions);
            uint32_t i;
            a->menu.of_sessions = true;
            for (i = 0; list && i < list->n; i++)
                if ((pt_str_starts(pt_strz(list->ids[i]), arg) || pt_str_icontains(pt_strz(list->names[i]), arg)) &&
                    (!a->open || strcmp(list->ids[i], a->s.id) != 0))
                    add_item(a, list->ids[i], strlen(list->ids[i]), list->names[i]);
        }
        if (!a->menu.n)
            a->menu.kind = MENU_NONE;
    }
    if (a->menu.n != old || a->menu.sel >= a->menu.n)
        a->menu.sel = a->menu.top = 0;
}

/* Tab: complete the selected menu item. Returns true when it changed
 * the draft. */
bool app_complete(app *a)
{
    pt_str d = pt_buf_str(&a->in.text), label;
    pt_buf nd;
    bool changed;
    if (a->menu.kind == MENU_NONE || a->menu.kind == MENU_SETTINGS || a->menu.kind == MENU_TOOLS || !a->menu.n)
        return false;
    label = app_item_label(a, a->menu.sel);
    pt_buf_init(&nd, pt_sys_allocator());
    if (a->menu.kind == MENU_CMD) {
        size_t i;
        pt_buf_putc(&nd, '/');
        pt_buf_puts(&nd, label);
        for (i = 0; i < NCMDS; i++)
            if (pt_str_eqz(label, app_cmds[i].name) && app_cmds[i].args[0])
                pt_buf_putc(&nd, ' ');
        if (pt_str_starts(label, PT_S("skill:"))) /* then what it is for */
            pt_buf_putc(&nd, ' ');
        else if (app_find_prompt(a, label) && app_find_prompt(a, label)->nargs) /* then its arguments */
            pt_buf_putc(&nd, ' ');
    } else if (a->menu.of_sessions) { /* the whole argument: names have spaces */
        pt_buf_puts(&nd, PT_S("/resume "));
        pt_buf_puts(&nd, label);
    } else if (a->menu.of_rewind) { /* the message, then how */
        pt_buf_puts(&nd, PT_S("/rewind "));
        pt_buf_puts(&nd, label);
        pt_buf_putc(&nd, ' ');
    } else { /* replace the last word */
        pt_buf_put(&nd, d.p, (size_t)(last_word(d).p - d.p));
        pt_buf_puts(&nd, label);
        if ((label.n && (label.p[0] == '$' || (label.p[0] == '@' && label.p[label.n - 1] != '/'))) || /* a directory goes on */
            (pt_str_starts(d, PT_S("/login ")) && !pt_str_starts(d, PT_S("/login oauth "))) ||
            (pt_str_starts(d, PT_S("/tools ")) && pt_str_find((pt_str){d.p + 7, d.n - 7}, ' ') < 0) ||
            (pt_str_starts(d, PT_S("/mcp ")) && pt_str_find((pt_str){d.p + 5, d.n - 5}, ' ') < 0 &&
             !pt_str_eqz(label, "trust") && !pt_str_eqz(label, "untrust"))) /* then the key, the name */
            pt_buf_putc(&nd, ' ');
    }
    changed = !pt_str_eq(pt_buf_str(&nd), d);
    if (changed)
        tui_input_set(&a->in, pt_buf_str(&nd));
    pt_buf_free(&nd);
    app_menu_update(a);
    return changed;
}
