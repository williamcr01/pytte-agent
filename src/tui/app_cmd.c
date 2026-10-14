/* Settings (saved through a worker), the model shown and its catalog
 * facts, and the slash commands. */
#define _POSIX_C_SOURCE 200809L
#include "tui/app.h"

#include <unistd.h>

static const syms uni = {"›", "∴", "·", "✓", "✗", "?", "›", "│", "…", "•"};
static const syms asc = {">", "~", "-", "+", "x", "?", ">", "|", "...", "*"};

const app_cmd_def app_cmds[] = {
    {"model", " [id]", "set the model of this session"},
    {"effort", " [level]", "set the reasoning effort of this session"},
    {"new", "", "start a new session"},
    {"resume", " [id]", "open another session from this directory"},
    {"fork", "", "continue in a copy of this session"},
    {"worktree", " [name]", "a new session in a git worktree (also /wt)"},
    {"status", "", "session, model, context and terminal"},
    {"compact", "", "summarize the conversation to free the context"},
    {"ps", "", "background commands"},
    {"stop", "", "stop the background commands"},
    {"usage", "", "tokens used in this session, per model"},
    {"thinking", " [mode]", "how thinking is shown: collapsed, full, hidden"},
    {"login", " [provider] [key]", "save an API key to auth.json"},
    {"mcp", " [login|logout|restart name] [trust|untrust]", "MCP servers: their state, logins, trust"},
    {"settings", "", "user settings (saved)"},
    {"help", "", "commands and keys"},
    {"quit", "", "exit; it prints how to continue"},
};
const size_t app_ncmds = sizeof app_cmds / sizeof app_cmds[0];

const char *const app_levels[] = {"off", "minimal", "low", "medium", "high", "xhigh", "max"};
const size_t app_nlevels = sizeof app_levels / sizeof app_levels[0];
const char *const app_th_names[] = {"collapsed", "full", "hidden"};
/* one name for each TH_*: a missing or extra one does not build */
typedef char app_th_names_cover_th[sizeof app_th_names / sizeof app_th_names[0] == NTH ? 1 : -1];
static const char *const sym_names[] = {"auto", "unicode", "ascii"};
static const char *const perm_names[] = {"ask", "auto", "read-only"};
static const char *const on_off[] = {"on", "off"};
#define VALUES(v) v, (int)(sizeof v / sizeof v[0])
const app_setting app_settings[] = {{CFG_THINKING, "thinking", VALUES(app_th_names)},
                                    {CFG_SYMBOLS, "symbols", VALUES(sym_names)},
                                    {CFG_MODEL, "model", NULL, 0},
                                    {CFG_REASONING, "reasoning", VALUES(app_levels)},
                                    {CFG_PAGER, "pager", NULL, 0},
                                    {CFG_PERMS, "permissions", VALUES(perm_names)},
                                    {CFG_COMPACT_MODEL, "compact-model", NULL, 0},
                                    {CFG_SKILLS, "skills", VALUES(on_off)}};
const size_t app_nsettings = sizeof app_settings / sizeof app_settings[0];

/* The level requests use: the chosen one, moved to one the model takes. */
static pt_reasoning effort_used(const app *a, pt_reasoning want)
{
    pt_ai_target t;
    memset(&t, 0, sizeof t);
    t.model.reasoning = a->model.reasons;
    t.efforts = a->model.efforts;
    return pt_ai_effort(&t, want);
}

/* "max", or "max→high" when the model does not take max. */
void effort_label(const app *a, char *out, size_t cap)
{
    pt_reasoning used = effort_used(a, a->o.reasoning);
    if (used == a->o.reasoning)
        snprintf(out, cap, "%s", pt_reasoning_name(used));
    else
        snprintf(out, cap, "%s%s%s", pt_reasoning_name(a->o.reasoning), a->ascii ? "->" : "→",
                 pt_reasoning_name(used));
}

void apply_symbols(app *a)
{
    const char *v = a->cfg->val[CFG_SYMBOLS], *term = getenv("TERM");
    const char *loc = getenv("LC_ALL");
    bool utf8;
    if (!loc || !*loc)
        loc = getenv("LC_CTYPE");
    if (!loc || !*loc)
        loc = getenv("LANG");
    utf8 = loc && (strstr(loc, "UTF-8") || strstr(loc, "utf-8") || strstr(loc, "UTF8") || strstr(loc, "utf8"));
    if (strcmp(v, "ascii") == 0)
        a->ascii = true;
    else if (strcmp(v, "unicode") == 0)
        a->ascii = false;
    else
        a->ascii = !term || !utf8 || strncmp(term, "linux", 5) == 0 || strncmp(term, "vt", 2) == 0;
    a->sy = a->ascii ? &asc : &uni;
    a->view.md.ascii = a->ascii;
}

void apply_thinking(app *a)
{
    int i;
    a->thinking = TH_COLLAPSED;
    for (i = 0; i < NTH; i++)
        if (strcmp(a->cfg->val[CFG_THINKING], app_th_names[i]) == 0)
            a->thinking = i;
}

static void write_setting(void *ud)
{
    config_job *job = (config_job *)ud;
    job->ok = cli_config_write(job->path, job->key, job->value, job->err, sizeof job->err);
}

static void start_setting(app *a)
{
    config_job *job = a->saving;
    int k;
    if (!job || job->active || !job->pending)
        return;
    for (k = 0; k < CFG_N; k++)
        if (job->pending & (1u << k))
            break;
    job->key = k;
    memcpy(job->value, job->next[k], sizeof job->value);
    job->pending &= ~(1u << k);
    job->active = pt_worker_start(job->worker, write_setting, job);
    if (!job->active)
        out_z(a, TUI_RED, "not saved: could not schedule the settings write");
}

void finish_setting(app *a)
{
    config_job *job = a->saving;
    if (job && job->active && pt_worker_finish(job->worker)) {
        char msg[700];
        unsigned i;
        job->active = false;
        if (!job->ok) {
            group_close(a);
            out_note(a, &a->lines, TUI_RED, pt_strz(job->err));
        } else {
            for (i = 0; i < NSETTINGS && app_settings[i].k != job->key; i++)
                ;
            snprintf(msg, sizeof msg, "saved %s: %s", i < NSETTINGS ? app_settings[i].name : "setting",
                     job->value[0] ? job->value : "(none)");
            out_z(a, TUI_DIM, msg);
        }
        a->pace.urgent = true;
    }
    start_setting(a);
}

/* Apply now and queue a durable save; report success after it lands. */
bool save_setting(app *a, int k, const char *v)
{
    char err[700];
    bool ok = true;
    pt_ai_target t;
    const char *why = NULL;
    pt_arena_reset(&a->ar);
    if ((k == CFG_MODEL || (k == CFG_COMPACT_MODEL && *v)) &&
        !pt_ai_resolve(cli_catalog(), pt_strz(v), &t, &a->ar, &why)) {
        snprintf(err, sizeof err, "%s: %s", v, why ? why : "unknown");
        out_z(a, TUI_RED, err);
        return false;
    }
    if (strcmp(a->cfg->src[k], "default") == 0 || strcmp(a->cfg->src[k], "user") == 0)
        cli_config_set(a->cfg, k, v, "user");
    if (!a->saving) {
        config_job *job = (config_job *)calloc(1, sizeof *job);
        if (job) {
            job->worker = pt_worker_new();
            snprintf(job->path, sizeof job->path, "%s", a->cfg->user_path);
            if (!job->worker) {
                free(job);
                job = NULL;
            }
        }
        a->saving = job;
    }
    if (!a->saving) {
        out_z(a, TUI_RED, "not saved: could not allocate the settings worker");
        ok = false;
    } else {
        snprintf(a->saving->next[k], sizeof a->saving->next[k], "%s", v);
        a->saving->pending |= 1u << k;
        start_setting(a);
    }
    if (k == CFG_THINKING) {
        apply_thinking(a);
        set_config(a);
    }
    if (k == CFG_SYMBOLS)
        apply_symbols(a);
    if (k == CFG_PERMS && pt_perm_parse(pt_strz(a->cfg->val[k]), &a->o.perm) && a->open)
        pt_rt_set_perm(a->s.rt, a->o.perm);
    if (k == CFG_SKILLS) { /* from the next epoch on: compaction or a new session */
        a->o.manual_skills = strcmp(a->cfg->val[k], "off") == 0;
        if (a->open)
            pt_rt_skills_auto(a->s.rt, !a->o.manual_skills);
    }
    if (k == CFG_COMPACT_MODEL) {
        a->o.compact_model = a->cfg->val[k][0] ? a->cfg->val[k] : NULL;
        set_config(a);
    }
    if (k == CFG_REASONING) {
        pt_reasoning r;
        if (cli_effort_parse(pt_strz(a->cfg->val[k]), &r)) {
            a->o.reasoning = r;
            set_config(a);
        }
    }
    return ok;
}

/* The model shown: the configured one, else the session's last. */
void update_model(app *a)
{
    uint32_t n = a->open ? pt_entry_count(a->s.a) : 0;
    pt_entry_view v;
    pt_ai_target t;
    const char *why;
    a->model.name[0] = 0;
    if (a->o.model)
        snprintf(a->model.name, sizeof a->model.name, "%s", a->o.model);
    while (n-- > 0 && pt_entry_at(a->s.a, n, &v))
        if (strcmp(v.kind, "assistant") == 0 && v.msg->model.n) {
            if (!a->model.name[0])
                snprintf(a->model.name, sizeof a->model.name, "%.*s/%.*s", (int)v.msg->provider.n, v.msg->provider.p,
                         (int)v.msg->model.n, v.msg->model.p);
            break;
        }
    a->model.ctx_used = a->open ? pt_context_tokens(a->s.a) : 0;
    a->model.ctx_window = 0;
    a->model.efforts = 0;
    a->model.reasons = false;
    pt_arena_reset(&a->ar);
    if (a->model.name[0] && pt_ai_resolve(cli_catalog(), pt_strz(a->model.name), &t, &a->ar, &why)) {
        a->model.ctx_window = t.model.context_window;
        a->model.efforts = t.efforts;
        a->model.reasons = t.model.reasoning;
    }
}

void set_config(app *a)
{
    pt_config c;
    memset(&c, 0, sizeof c);
    c.model = a->o.model ? pt_strz(a->o.model) : PT_STR_NULL;
    c.compact_model = a->o.compact_model ? pt_strz(a->o.compact_model) : PT_STR_NULL;
    c.reasoning = a->o.reasoning;
    c.hide_thinking = a->thinking != TH_FULL; /* collapsed shows only how long */
    c.max_tokens = a->o.max_tokens;
    c.retry_base_ms = a->o.retry_base_ms;
    c.context_window = a->o.context_window;
    c.no_sync = a->o.no_sync;
    if (a->open)
        pt_set_config(a->s.a, &c);
    update_model(a);
}

/* Background commands still running. */
uint32_t bg_running(app *a)
{
    pt_rt_background v[32];
    uint32_t n = a->open ? pt_rt_backgrounds(a->s.rt, v, 32) : 0, i, k = 0;
    for (i = 0; i < n && i < 32; i++)
        k += !v[i].ended;
    return k;
}

/* "session 8 · running 1m 4s · make dev", newest first. */
static void cmd_ps(app *a)
{
    pt_rt_background v[32];
    uint32_t n = a->open ? pt_rt_backgrounds(a->s.rt, v, 32) : 0, i;
    char line[300], took[32];
    if (!n) {
        out_z(a, TUI_DIM, "no background commands");
        return;
    }
    for (i = 0; i < n && i < 32; i++) {
        secs_short(took, sizeof took, pt_sys_now_ms() - v[i].started);
        if (v[i].ended)
            snprintf(line, sizeof line, "session %u · ended (exit %d), not read yet · %.200s", v[i].session,
                     v[i].status, v[i].command);
        else
            snprintf(line, sizeof line, "session %u · running %s · %.200s", v[i].session, took, v[i].command);
        out_note(a, &a->lines, TUI_DIM, pt_strz(line));
    }
}

static void cmd_status(app *a)
{
    char line[5000], name[CLI_NAME_MAX + 1], eff[40];
    pt_str cwd = a->open ? pt_session_cwd(a->s.a) : PT_STR_NULL;
    snprintf(line, sizeof line, "session %s, owned by pid %ld\nstore %s", a->s.id, (long)getpid(), a->s.dir);
    out_z(a, TUI_DIM, line);
    if (cli_session_name(a->s.dir, name, sizeof name)) {
        snprintf(line, sizeof line, "name %s", name);
        out_z(a, TUI_DIM, line);
    }
    snprintf(line, sizeof line, "directory %.*s", (int)(cwd.n < 4096 ? cwd.n : 4096), cwd.p ? cwd.p : "");
    out_z(a, TUI_DIM, line);
    effort_label(a, eff, sizeof eff);
    snprintf(line, sizeof line, "model %s · effort %s", a->model.name[0] ? a->model.name : "(none)", eff);
    out_z(a, TUI_DIM, line);
    if (a->model.ctx_window)
        snprintf(line, sizeof line, "context %llu of %u tokens (%u%%)", (unsigned long long)a->model.ctx_used, a->model.ctx_window,
                 (unsigned)(a->model.ctx_used * 100 / a->model.ctx_window));
    else
        snprintf(line, sizeof line, "context %llu tokens", (unsigned long long)a->model.ctx_used);
    out_z(a, TUI_DIM, line);
    snprintf(line, sizeof line, "terminal %dx%d · %s · %s · kitty keys %s · %s · %s symbols", a->cols, a->rows,
             getenv("TERM") ? getenv("TERM") : "(no TERM)", a->band.reflow ? "reflows" : "does not reflow",
             a->caps.kitty ? "on" : "off", a->caps.answered ? "answers queries" : "no query replies",
             a->ascii ? "ascii" : "unicode");
    out_z(a, TUI_DIM, line);
}

/* The usage doc: tokens per model, and what they cost where prices are known. */
static void cmd_usage(app *a)
{
    const pt_json *u, *c;
    char line[600], in[16], out[16], cache[16];
    pt_arena_reset(&a->ar);
    u = a->open ? pt_json_parse(pt_doc(a->s.a, PT_S("usage")), &a->ar, NULL) : NULL;
    if (!pt_json_is(u, PT_J_OBJ) || !u->len) {
        out_z(a, TUI_DIM, "no tokens used yet");
        return;
    }
    for (c = u->child; c; c = c->next) {
        uint64_t ti = (uint64_t)pt_json_geti(c, "input", 0), to = (uint64_t)pt_json_geti(c, "output", 0);
        uint64_t cr = (uint64_t)pt_json_geti(c, "cache_read", 0), cw = (uint64_t)pt_json_geti(c, "cache_write", 0);
        pt_ai_target t;
        const char *why;
        int n;
        tokens_short(in, sizeof in, ti + cr + cw);
        tokens_short(out, sizeof out, to);
        tokens_short(cache, sizeof cache, cr);
        n = snprintf(line, sizeof line, "%.*s: %s in (%s from cache) · %s out · %lld requests", (int)c->key.n,
                     c->key.p, in, cache, out, (long long)pt_json_geti(c, "requests", 0));
        if (pt_ai_resolve(cli_catalog(), c->key, &t, &a->ar, &why) && t.known && t.model.cost.input) {
            const pt_cost *k = &t.model.cost; /* micro-dollars per million tokens */
            double usd = ((double)ti * (double)k->input + (double)to * (double)k->output +
                          (double)cr * (double)k->cache_read + (double)cw * (double)k->cache_write) / 1e12;
            snprintf(line + n, sizeof line - (size_t)n, " · $%.2f", usd);
        }
        out_z(a, TUI_DIM, line);
    }
}

static void cmd_help(app *a)
{
    size_t i;
    char line[200];
    for (i = 0; i < NCMDS; i++) {
        snprintf(line, sizeof line, "/%s%s  %s", app_cmds[i].name, app_cmds[i].args, app_cmds[i].desc);
        out_z(a, TUI_DIM, line);
    }
    out_z(a, TUI_DIM, "$name anywhere in a message, or /skill:name first, gives the model a skill's instructions");
    if (a->mcp && a->mcp->nall)
        out_z(a, TUI_DIM, "/server:prompt [args] sends an MCP server's prompt; @server:uri adds a resource to a message");
    out_z(a, TUI_DIM,
          "enter send · shift+enter, alt+enter or ctrl-j newline · esc esc interrupt · ctrl-c clear, "
          "interrupt or quit · ctrl-d quit · ctrl-t thinking · ctrl-o details · ctrl-z suspend · ctrl-l redraw");
}

/* "/login": where each provider's key comes from. "/login <provider>":
 * ready for the key. "/login <provider> <key>": save it. */
static void cmd_login(app *a, pt_str arg)
{
    const pt_ai_catalog *cat = cli_catalog();
    const pt_provider *p = NULL;
    pt_str name = arg, key = PT_STR_NULL;
    long sp = pt_str_find(arg, ' ');
    char msg[4400], err[600], path[4200];
    uint32_t i;
    if (sp >= 0) {
        name.n = (size_t)sp;
        key = pt_str_trim((pt_str){arg.p + sp + 1, arg.n - (size_t)sp - 1});
    }
    for (i = 0; i < cat->nproviders; i++)
        if (cat->providers[i].auth != PT_AUTH_NONE && pt_str_eqz(name, cat->providers[i].id))
            p = &cat->providers[i];
    if (!arg.n) {
        for (i = 0; i < cat->nproviders; i++) {
            const pt_provider *q = &cat->providers[i];
            const char *env = q->key_env[0] ? getenv(q->key_env[0]) : NULL;
            if (q->auth == PT_AUTH_NONE)
                continue;
            snprintf(msg, sizeof msg, "%s: %s%s", q->id,
                     env && *env ? "from " : cli_auth_key(q->id) ? "saved in auth.json" : "no key",
                     env && *env ? q->key_env[0] : "");
            out_z(a, TUI_DIM, msg);
        }
        out_z(a, TUI_DIM, "/login <provider> saves a key; a key in the environment comes first");
        return;
    }
    if (!p) {
        snprintf(msg, sizeof msg, "not a provider: %.*s", (int)(name.n > 100 ? 100 : name.n), name.p);
        out_z(a, TUI_RED, msg);
        return;
    }
    if (!key.n) {
        snprintf(msg, sizeof msg, "/login %s ", p->id);
        tui_input_set(&a->in, pt_strz(msg));
        snprintf(msg, sizeof msg, "paste the %s API key and press enter", p->name);
        out_z(a, TUI_DIM, msg);
        return;
    }
    if (!cli_auth_valid(key)) {
        out_z(a, TUI_RED, "not an API key: it has spaces or control characters");
        return;
    }
    if (!cli_auth_save(p->id, key, err, sizeof err)) {
        out_z(a, TUI_RED, err);
        return;
    }
    cli_auth_path(path, sizeof path);
    snprintf(msg, sizeof msg, "saved the %s key to %s", p->name, path);
    out_z(a, TUI_DIM, msg);
    if (err[0])
        out_z(a, TUI_RED, err);
    if (p->key_env[0] && getenv(p->key_env[0]) && *getenv(p->key_env[0])) {
        snprintf(msg, sizeof msg, "%s is set in the environment and is used instead", p->key_env[0]);
        out_z(a, TUI_DIM, msg);
    }
}

/* From a server, inside a step: shown at the loop's next pass. */
void mcp_note(void *ud, const char *server, const char *msg)
{
    app *a = (app *)ud;
    pt_buf_putz(&a->mcp_notes, server ? server : "");
    pt_buf_putc(&a->mcp_notes, '\t');
    pt_buf_putz(&a->mcp_notes, msg);
    pt_buf_putc(&a->mcp_notes, '\n');
    set_dirty(a);
}

void show_notes(app *a)
{
    pt_str all = pt_buf_str(&a->mcp_notes);
    size_t at = 0;
    if (!all.n)
        return;
    while (at < all.n) {
        size_t e = at, tab;
        char line[1200];
        pt_str msg;
        while (e < all.n && all.p[e] != '\n')
            e++;
        for (tab = at; tab < e && all.p[tab] != '\t'; tab++)
            ;
        msg = (pt_str){all.p + tab + 1, e > tab ? e - tab - 1 : 0};
        snprintf(line, sizeof line, "mcp %.*s: %.*s", (int)(tab - at), all.p + at, (int)msg.n, msg.p);
        out_z(a,
              starts(msg, "failed") || starts(msg, "stopped") || starts(msg, "login failed") ||
                      pt_str_contains(msg, PT_S("log in with"))
                  ? TUI_RED
                  : TUI_DIM,
              line);
        at = e + 1;
    }
    pt_buf_reset(&a->mcp_notes);
    a->pace.urgent = true;
}

static const char *mcp_state_name(pt_mcp_state st)
{
    switch (st) {
    case PT_MCP_STARTING:
        return "starting";
    case PT_MCP_READY:
        return "ready";
    case PT_MCP_FAILED:
        return "failed";
    case PT_MCP_AUTH:
        return "needs login";
    default:
        return "stopped";
    }
}

static void cmd_mcp(app *a, pt_str arg)
{
    pt_rt_mcp_info in[CLI_MCP_MAX];
    pt_str sub = arg, name = PT_STR_NULL;
    long sp = pt_str_find(arg, ' ');
    char msg[1200], err[700];
    uint32_t i, n;
    if (sp >= 0) {
        sub.n = (size_t)sp;
        name = pt_str_trim((pt_str){arg.p + sp + 1, arg.n - (size_t)sp - 1});
    }
    if (!a->mcp) {
        out_z(a, TUI_DIM, "MCP servers are off (--no-mcp)");
        return;
    }
    n = a->open ? pt_rt_mcp_servers(a->s.rt, in, CLI_MCP_MAX) : 0;
    if (n > CLI_MCP_MAX)
        n = CLI_MCP_MAX;
    if (!sub.n) {
        if (!a->mcp->nall) {
            out_z(a, TUI_DIM, "no MCP servers: add them to mcpServers in ~/.config/pytte/config.json, or to "
                              ".mcp.json in a project");
            return;
        }
        for (i = 0; i < n; i++) {
            int k = snprintf(msg, sizeof msg, "%s · %s · %s", in[i].name, mcp_state_name(in[i].state), in[i].target);
            if (in[i].state == PT_MCP_READY)
                snprintf(msg + k, sizeof msg - (size_t)k, " · %u tools, %u prompts, %u resources", in[i].ntools,
                         in[i].nprompts, in[i].nresources);
            else if (in[i].error[0])
                snprintf(msg + k, sizeof msg - (size_t)k, " · %s", in[i].error);
            out_z(a, in[i].state == PT_MCP_FAILED || in[i].state == PT_MCP_AUTH ? TUI_RED : TUI_DIM, msg);
        }
        for (i = 0; i < a->mcp->nall; i++)
            if (!a->mcp->info[i].runs) {
                snprintf(msg, sizeof msg, "%s · not trusted (%s): /mcp trust runs this project's servers",
                         a->mcp->all[i].name, a->mcp->info[i].source);
                out_z(a, TUI_DIM, msg);
            }
        return;
    }
    if (pt_str_eqz(sub, "trust") || pt_str_eqz(sub, "untrust")) {
        bool on = sub.p[0] == 't';
        pt_rt_mcp_server add[CLI_MCP_MAX];
        uint32_t k = 0;
        if (!cli_mcp_trust(a->mcp, on, err, sizeof err)) {
            out_z(a, TUI_RED, err);
            return;
        }
        if (!on) {
            out_z(a, TUI_DIM, "no longer trusting this project's MCP servers: new sessions start without them");
            cli_mcp_free(a->mcp);
            cli_mcp_load(a->mcp, a->mcp->project);
            a->o.nmcp = a->mcp->n;
            return;
        }
        {
            char dir[4096];
            snprintf(dir, sizeof dir, "%s", a->mcp->project);
            cli_mcp_free(a->mcp);
            cli_mcp_load(a->mcp, dir);
        }
        a->o.mcp = a->mcp->run;
        a->o.nmcp = a->mcp->n;
        /* those not running yet start now */
        for (i = 0; i < a->mcp->n; i++) {
            uint32_t j;
            for (j = 0; j < n && strcmp(in[j].name, a->mcp->run[i].name) != 0; j++)
                ;
            if (j == n)
                add[k++] = a->mcp->run[i];
        }
        if (k && a->open && !pt_rt_mcp_setup(a->s.rt, &a->o.mcp_host, add, k)) {
            out_z(a, TUI_RED, "out of memory starting them");
            return;
        }
        snprintf(msg, sizeof msg, "trusted this project's MCP servers%s", k ? ": starting them; their tools come with the next session (/new) or after /compact" : "");
        out_z(a, TUI_DIM, msg);
        return;
    }
    if (!pt_str_eqz(sub, "login") && !pt_str_eqz(sub, "logout") && !pt_str_eqz(sub, "restart")) {
        out_z(a, TUI_RED, "/mcp [login|logout|restart NAME] [trust|untrust]");
        return;
    }
    if (!name.n) {
        out_z(a, TUI_RED, "which server?");
        return;
    }
    for (i = 0; i < n && !pt_str_eqz(name, in[i].name); i++)
        ;
    if (i == n) {
        snprintf(msg, sizeof msg, "no MCP server %.*s%s", (int)(name.n > 64 ? 64 : name.n), name.p,
                 mcp_known(a, name) ? " runs: /mcp trust runs this project's servers" : "");
        out_z(a, TUI_RED, msg);
        return;
    }
    if (pt_str_eqz(sub, "restart")) {
        pt_rt_mcp_restart(a->s.rt, name);
        snprintf(msg, sizeof msg, "restarting %s", in[i].name);
    } else if (strcmp(in[i].transport, "http") != 0) {
        snprintf(msg, sizeof msg, "%s is not a remote server: only those log in", in[i].name);
        out_z(a, TUI_RED, msg);
        return;
    } else if (pt_str_eqz(sub, "login")) {
        pt_rt_mcp_login(a->s.rt, name); /* it says what goes on */
        return;
    } else {
        pt_rt_mcp_logout(a->s.rt, name);
        snprintf(msg, sizeof msg, "logged out of %s", in[i].name);
    }
    out_z(a, TUI_DIM, msg);
}

static bool valid_setting(int s, const char *v)
{
    int i;
    for (i = 0; i < app_settings[s].nv; i++)
        if (strcmp(v, app_settings[s].vals[i]) == 0)
            return true;
    return !app_settings[s].vals;
}

void run_command(app *a, pt_str line)
{
    pt_str cmd = line, arg = PT_S("");
    long sp = pt_str_find(line, ' ');
    char v[600], msg[700];
    if (sp >= 0) {
        cmd.n = (size_t)sp;
        arg = pt_str_trim((pt_str){line.p + sp + 1, line.n - (size_t)sp - 1});
    }
    cmd.p++;
    cmd.n--;
    snprintf(v, sizeof v, "%.*s", (int)(arg.n < sizeof v ? arg.n : sizeof v - 1), arg.p);
    group_close(a);
    out_blank(&a->lines); /* the command itself is not echoed: only what it did */
    if (pt_str_eqz(cmd, "model")) {
        if (arg.n) {
            pt_ai_target t;
            const char *why = NULL;
            pt_arena_reset(&a->ar);
            if (!pt_ai_resolve(cli_catalog(), arg, &t, &a->ar, &why)) {
                snprintf(msg, sizeof msg, "%s: %s", v, why ? why : "unknown");
                out_z(a, TUI_RED, msg);
                return;
            }
            snprintf(a->model.ref, sizeof a->model.ref, "%.255s", v);
            a->o.model = a->model.ref;
            set_config(a);
        }
        snprintf(msg, sizeof msg, "%smodel: %s", arg.n ? "switched " : "",
                 a->model.name[0] ? a->model.name : "(the session's last)");
        out_z(a, TUI_DIM, msg);
    } else if (pt_str_eqz(cmd, "effort")) {
        if (arg.n) {
            pt_reasoning r;
            if (!cli_effort_parse(arg, &r)) {
                out_z(a, TUI_RED, "levels: off, minimal, low, medium, high, xhigh, max");
                return;
            }
            a->o.reasoning = r;
            set_config(a);
        }
        {
            char eff[40];
            effort_label(a, eff, sizeof eff);
            snprintf(msg, sizeof msg, "%seffort: %s%s", arg.n ? "switched " : "", eff,
                     effort_used(a, a->o.reasoning) != a->o.reasoning ? " (the nearest this model takes)" : "");
        }
        out_z(a, TUI_DIM, msg);
    } else if (pt_str_eqz(cmd, "new")) {
        switch_session(a, NULL);
    } else if (pt_str_eqz(cmd, "resume")) {
        if (!arg.n) {
            tui_input_set(&a->in, PT_S("/resume "));
            a->menu.closed = false;
            menu_update(a);
            return;
        }
        switch_session(a, v);
    } else if (pt_str_eqz(cmd, "fork")) {
        if (!a->open)
            out_z(a, TUI_RED, "no session to fork");
        else if (session_empty(a))
            out_z(a, TUI_DIM, "nothing to fork yet");
        else if (a->run.active)
            out_z(a, TUI_RED, "/fork waits for the run to end (esc esc stops it)");
        else {
            a->o.fork = true;
            switch_session(a, NULL);
            a->o.fork = false;
            a->sess.created = false; /* a copy: never discarded as empty */
        }
    } else if (pt_str_eqz(cmd, "worktree") || pt_str_eqz(cmd, "wt")) {
        if (arg.n && !cli_worktree_name_ok(v)) {
            snprintf(msg, sizeof msg, "not a worktree name: %.100s (letters, digits, - _ .)", v);
            out_z(a, TUI_RED, msg);
            return;
        }
        a->o.worktree = v; /* copied by the open */
        switch_session(a, NULL);
        a->o.worktree = NULL;
    } else if (pt_str_eqz(cmd, "status")) {
        cmd_status(a);
    } else if (pt_str_eqz(cmd, "compact")) {
        if (!a->open || !pt_compact(a->s.a))
            out_z(a, TUI_DIM, "nothing to compact yet");
    } else if (pt_str_eqz(cmd, "usage")) {
        cmd_usage(a);
    } else if (pt_str_eqz(cmd, "ps")) {
        cmd_ps(a);
    } else if (pt_str_eqz(cmd, "stop")) {
        uint32_t n = a->open ? pt_rt_backgrounds_stop(a->s.rt) : 0;
        char said[64];
        snprintf(said, sizeof said, n ? "stopping %u background command%s" : "no background commands running", n,
                 n == 1 ? "" : "s");
        out_z(a, TUI_DIM, said);
    } else if (pt_str_eqz(cmd, "thinking")) {
        if (arg.n) {
            if (!valid_setting(find_setting(PT_S("thinking")), v)) {
                out_z(a, TUI_RED, "modes: collapsed, full, hidden");
                return;
            }
            if (!save_setting(a, CFG_THINKING, v))
                return;
        }
        snprintf(msg, sizeof msg, "%sthinking: %s", arg.n ? "switched " : "", app_th_names[a->thinking]);
        out_z(a, TUI_DIM, msg);
    } else if (pt_str_eqz(cmd, "settings")) {
        long sp2 = pt_str_find(arg, ' ');
        int s;
        if (!arg.n) {
            a->menu.kind = MENU_SETTINGS;
            a->menu.set_sel = a->menu.top = 0;
            return;
        }
        s = find_setting((pt_str){arg.p, sp2 >= 0 ? (size_t)sp2 : arg.n});
        if (s < 0) {
            size_t i;
            int n = snprintf(msg, sizeof msg, "settings:");
            for (i = 0; i < NSETTINGS && n > 0 && (size_t)n < sizeof msg; i++)
                n += snprintf(msg + n, sizeof msg - (size_t)n, "%s %s", i ? "," : "", app_settings[i].name);
            out_z(a, TUI_RED, msg);
            return;
        }
        if (sp2 >= 0) {
            pt_str val = pt_str_trim((pt_str){arg.p + sp2 + 1, arg.n - (size_t)sp2 - 1});
            snprintf(v, sizeof v, "%.*s", (int)(val.n < sizeof v ? val.n : sizeof v - 1), val.p);
            if (!valid_setting(s, v)) {
                snprintf(msg, sizeof msg, "not a value for %s: %s", app_settings[s].name, v);
                out_z(a, TUI_RED, msg);
                return;
            }
            if (!save_setting(a, app_settings[s].k, v))
                return;
        }
        if (sp2 >= 0) /* saved: the value, and where it was saved */
            snprintf(msg, sizeof msg, "saving %s: %s", app_settings[s].name,
                     a->cfg->val[app_settings[s].k][0] ? a->cfg->val[app_settings[s].k] : "(none)");
        else
            snprintf(msg, sizeof msg, "%s: %s (%s)", app_settings[s].name,
                     a->cfg->val[app_settings[s].k][0] ? a->cfg->val[app_settings[s].k] : "(none)",
                     a->cfg->src[app_settings[s].k]);
        out_z(a, TUI_DIM, msg);
    } else if (pt_str_eqz(cmd, "login")) {
        cmd_login(a, arg);
    } else if (pt_str_eqz(cmd, "mcp")) {
        cmd_mcp(a, arg);
    } else if (pt_str_eqz(cmd, "help")) {
        cmd_help(a);
    } else if (pt_str_eqz(cmd, "quit") || pt_str_eqz(cmd, "exit")) {
        a->sess.keep_on_quit = true;
        a->quit = true;
    } else {
        snprintf(msg, sizeof msg, "unknown command /%.*s; /help lists them", (int)(cmd.n > 100 ? 100 : cmd.n), cmd.p);
        out_z(a, TUI_RED, msg);
    }
}

/* The configured server of that name: running, or waiting for trust. */
bool mcp_known(app *a, pt_str name)
{
    uint32_t i;
    for (i = 0; a->mcp && i < a->mcp->nall; i++)
        if (pt_str_eqz(name, a->mcp->all[i].name))
            return true;
    return false;
}
