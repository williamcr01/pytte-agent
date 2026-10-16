/* Settings (saved through a worker), the model shown and its catalog
 * facts, and the slash commands. */
#define _POSIX_C_SOURCE 200809L
#include "tui/app.h"

#include <unistd.h>

static const syms uni = {"›", "∴", "·", "✓", "✗", "?", "›", "│", "…", "•", "↑", "↓", "▪", "●", "○"};
static const syms asc = {">", "~", "-", "+", "x", "?", ">", "|", "...", "*", "in", "out", "*", "[x]", "[ ]"};

const app_cmd_def app_cmds[] = {
    {"model", " [id]", "switch the model; /settings model also saves it"},
    {"effort", " [level]", "switch the reasoning effort; /settings effort also saves it"},
    {"new", "", "start a new session"},
    {"resume", " [id]", "open another session from this directory"},
    {"fork", "", "continue in a copy of this session"},
    {"worktree", " [name]", "a new session in a git worktree (also /wt)"},
    {"status", "", "session, model, context and terminal"},
    {"compact", "", "summarize the conversation to free the context"},
    {"ps", "", "background commands"},
    {"stop", "", "stop the background commands"},
    {"usage", "", "tokens used in this session, per model"},
    {"thinking", " [mode]", "how thinking is shown: hidden, full (saved)"},
    {"login", " [api|oauth|search] [name] [key]", "save an API key, or sign in with a subscription"},
    {"mcp", " [login|logout|restart name] [trust|untrust]", "MCP servers: their state, logins, trust"},
    {"tools", " [name] [on|off]", "turn the model's tools on or off (saved)"},
    {"settings", " [name] [value]", "change and save settings; they apply now"},
    {"help", "", "commands and keys"},
    {"quit", "", "exit; it prints how to continue (also /exit)"},
};
const size_t app_ncmds = sizeof app_cmds / sizeof app_cmds[0];

const char *const app_levels[] = {"off", "minimal", "low", "medium", "high", "xhigh", "max"};
const size_t app_nlevels = sizeof app_levels / sizeof app_levels[0];
const char *const app_th_names[] = {"hidden", "full"};
/* one name for each TH_*: a missing or extra one does not build */
typedef char app_th_names_cover_th[sizeof app_th_names / sizeof app_th_names[0] == NTH ? 1 : -1];
static const char *const sym_names[] = {"auto", "unicode", "ascii"};
static const char *const perm_names[] = {"ask", "auto", "read-only"};
static const char *const on_off[] = {"on", "off"};
#define VALUES(v) v, (int)(sizeof v / sizeof v[0])
const app_setting app_settings[] = {{CFG_THINKING, "thinking", VALUES(app_th_names)},
                                    {CFG_SYMBOLS, "symbols", VALUES(sym_names)},
                                    {CFG_MODEL, "model", NULL, 0},
                                    {CFG_REASONING, "effort", VALUES(app_levels)},
                                    {CFG_PAGER, "pager", NULL, 0},
                                    {CFG_PERMS, "permissions", VALUES(perm_names)},
                                    {CFG_COMPACT_MODEL, "compact-model", NULL, 0},
                                    {CFG_SKILLS, "skills", VALUES(on_off)},
                                    {SET_TOOLS, "tools", NULL, 0}};
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
void app_effort_label(const app *a, char *out, size_t cap)
{
    pt_reasoning used = effort_used(a, a->o.reasoning);
    if (used == a->o.reasoning)
        snprintf(out, cap, "%s", pt_reasoning_name(used));
    else
        snprintf(out, cap, "%s%s%s", pt_reasoning_name(a->o.reasoning), a->ascii ? "->" : "→",
                 pt_reasoning_name(used));
}

void app_apply_symbols(app *a)
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

void app_apply_thinking(app *a)
{
    int i;
    a->thinking = TH_HIDDEN;
    for (i = 0; i < NTH; i++)
        if (strcmp(a->cfg->val[CFG_THINKING], app_th_names[i]) == 0)
            a->thinking = i;
}

static void write_setting(void *ud)
{
    config_job *job = (config_job *)ud;
    if (job->key == SET_TOOLS)
        job->ok = cli_config_write_tool(job->path, job->tool.name, job->tool.on, job->err, sizeof job->err);
    else
        job->ok = cli_config_write(job->path, job->key, job->value, job->err, sizeof job->err);
}

static void start_setting(app *a)
{
    config_job *job = a->saving;
    int k;
    if (!job || job->active || (!job->pending && !job->ntq))
        return;
    for (k = 0; k < CFG_N; k++)
        if (job->pending & (1u << k))
            break;
    job->key = k;
    if (k == CFG_N) { /* a tool: the first waiting */
        job->key = SET_TOOLS;
        job->tool = job->tq[0];
        memmove(&job->tq[0], &job->tq[1], --job->ntq * sizeof job->tq[0]);
    } else {
        memcpy(job->value, job->next[k], sizeof job->value);
        job->pending &= ~(1u << k);
    }
    job->active = pt_worker_start(job->worker, write_setting, job);
    if (!job->active)
        app_out_z(a, TUI_RED, "not saved: could not schedule the settings write");
}

void app_finish_setting(app *a)
{
    config_job *job = a->saving;
    if (job && job->active && pt_worker_finish(job->worker)) {
        char msg[700];
        unsigned i;
        job->active = false;
        if (!job->ok) {
            app_group_close(a);
            app_out_note(a, &a->lines, TUI_RED, pt_strz(job->err));
        } else if (job->key == SET_TOOLS) {
            snprintf(msg, sizeof msg, "saved %s: %s%s", job->tool.name, job->tool.on ? "on" : "off",
                     job->tool.wins ? " (.pytte/config.json sets another at the next start)"
                     : job->tool.now ? "" /* the session's first request gets it */
                     : job->tool.on ? " (the model gets it at the next session or /compact)"
                                    : " (refused now; the model forgets it at the next session or /compact)");
            app_out_z(a, TUI_DIM, msg);
        } else {
            const char *wins = job->wins[job->key];
            int n;
            for (i = 0; i < NSETTINGS && app_settings[i].k != job->key; i++)
                ;
            n = snprintf(msg, sizeof msg, "saved %s: %s", i < NSETTINGS ? app_settings[i].name : "setting",
                         job->value[0] ? job->value : "none");
            if (wins && n > 0 && (size_t)n < sizeof msg)
                snprintf(msg + n, sizeof msg - (size_t)n, " (%s sets another at the next start)",
                         strcmp(wins, "env") == 0 ? "the environment" : ".pytte/config.json");
            else if (job->key == CFG_SKILLS && n > 0 && (size_t)n < sizeof msg)
                snprintf(msg + n, sizeof msg - (size_t)n, " (from the next session or /compact)");
            app_out_z(a, TUI_DIM, msg);
        }
        a->pace.urgent = true;
    }
    start_setting(a);
}

/* Use ref (copied) from now on; pinned: sessions opened next use it too. */
static void set_model(app *a, const char *ref, bool pinned)
{
    snprintf(a->model.ref, sizeof a->model.ref, "%.255s", ref);
    a->o.model = a->model.ref;
    a->model.pinned = pinned;
    app_set_config(a);
}

/* The model for the session about to open (id, else a new one or a fork):
 * one chosen goes on; otherwise a resumed session keeps its own, a fork
 * its original's, and a new one gets the configured default, as at start. */
void app_model_for(app *a, const char *id, bool fork)
{
    if (a->model.pinned || fork)
        return;
    if (id) {
        a->o.model = NULL;
    } else {
        snprintf(a->model.ref, sizeof a->model.ref, "%.255s",
                 a->cfg->val[CFG_MODEL][0] ? a->cfg->val[CFG_MODEL] : DEFAULT_MODEL);
        a->o.model = a->model.ref;
    }
}

/* The settings worker, made at the first save; NULL (said) when it cannot be. */
static config_job *saver(app *a)
{
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
    if (!a->saving)
        app_out_z(a, TUI_RED, "not saved: could not allocate the settings worker");
    return a->saving;
}

/* Apply now and queue a durable save; report success after it lands.
 * Every setting takes effect in this session at once, even one the
 * environment or the project file set: they win again at the next start. */
bool app_save_setting(app *a, int k, const char *v)
{
    char err[700];
    bool ok = true;
    pt_ai_target t;
    const char *why = NULL, *src = a->cfg->src[k];
    pt_arena_reset(&a->ar);
    if ((k == CFG_MODEL || (k == CFG_COMPACT_MODEL && *v)) &&
        !pt_ai_resolve(cli_catalog(), pt_strz(v), &t, &a->ar, &why)) {
        snprintf(err, sizeof err, "%s: %s", v, why ? why : "unknown");
        app_out_z(a, TUI_RED, err);
        return false;
    }
    cli_config_set(a->cfg, k, v, "user");
    if (saver(a) && (strcmp(src, "env") == 0 || strcmp(src, "project") == 0))
        a->saving->wins[k] = src;
    if (!a->saving) {
        ok = false;
    } else {
        snprintf(a->saving->next[k], sizeof a->saving->next[k], "%s", v);
        a->saving->pending |= 1u << k;
        start_setting(a);
    }
    if (k == CFG_THINKING) {
        app_apply_thinking(a);
        app_set_config(a);
    }
    if (k == CFG_SYMBOLS)
        app_apply_symbols(a);
    if (k == CFG_PERMS && pt_perm_parse(pt_strz(a->cfg->val[k]), &a->o.perm) && a->open)
        pt_rt_set_perm(a->s.rt, a->o.perm);
    if (k == CFG_SKILLS) { /* from the next epoch on: compaction or a new session */
        a->o.manual_skills = strcmp(a->cfg->val[k], "off") == 0;
        if (a->open)
            pt_rt_skills_auto(a->s.rt, !a->o.manual_skills);
    }
    if (k == CFG_MODEL)
        set_model(a, a->cfg->val[k], true);
    if (k == CFG_COMPACT_MODEL) {
        a->o.compact_model = a->cfg->val[k][0] ? a->cfg->val[k] : NULL;
        app_set_config(a);
    }
    if (k == CFG_REASONING) {
        pt_reasoning r;
        if (cli_effort_parse(pt_strz(a->cfg->val[k]), &r)) {
            a->o.reasoning = r;
            app_set_config(a);
        }
    }
    return ok;
}

/* The model shown: the configured one, else the session's last. */
void app_update_model(app *a)
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
    if (!a->model.name[0] && a->open) { /* a session resumed before its first request */
        pt_str m = pt_session_model(a->s.a);
        snprintf(a->model.name, sizeof a->model.name, "%.*s", (int)(m.n < 255 ? m.n : 255), m.p ? m.p : "");
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

void app_set_config(app *a)
{
    pt_config c;
    memset(&c, 0, sizeof c);
    c.model = a->o.model ? pt_strz(a->o.model) : PT_STR_NULL;
    c.compact_model = a->o.compact_model ? pt_strz(a->o.compact_model) : PT_STR_NULL;
    c.reasoning = a->o.reasoning;
    c.hide_thinking = a->thinking != TH_FULL; /* hidden shows only how long */
    c.max_tokens = a->o.max_tokens;
    c.retry_base_ms = a->o.retry_base_ms;
    c.context_window = a->o.context_window;
    c.no_sync = a->o.no_sync;
    if (a->open)
        pt_set_config(a->s.a, &c);
    app_update_model(a);
}

/* Background commands still running. */
uint32_t app_bg_running(app *a)
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
        app_out_z(a, TUI_DIM, "no background commands");
        return;
    }
    for (i = 0; i < n && i < 32; i++) {
        app_secs_short(took, sizeof took, pt_sys_now_ms() - v[i].started);
        if (v[i].ended)
            snprintf(line, sizeof line, "session %u · ended (exit %d), not read yet · %.200s", v[i].session,
                     v[i].status, v[i].command);
        else
            snprintf(line, sizeof line, "session %u · running %s · %.200s", v[i].session, took, v[i].command);
        app_out_note(a, &a->lines, TUI_DIM, pt_strz(line));
    }
}

static void cmd_status(app *a)
{
    char line[5000], name[CLI_NAME_MAX + 1], eff[40];
    pt_str cwd = a->open ? pt_session_cwd(a->s.a) : PT_STR_NULL;
    snprintf(line, sizeof line, "session %s, owned by pid %ld\nstore %s", a->s.id, (long)getpid(), a->s.dir);
    app_out_z(a, TUI_DIM, line);
    if (cli_session_name(a->s.dir, name, sizeof name)) {
        snprintf(line, sizeof line, "name %s", name);
        app_out_z(a, TUI_DIM, line);
    }
    snprintf(line, sizeof line, "directory %.*s", (int)(cwd.n < 4096 ? cwd.n : 4096), cwd.p ? cwd.p : "");
    app_out_z(a, TUI_DIM, line);
    app_effort_label(a, eff, sizeof eff);
    snprintf(line, sizeof line, "model %s · effort %s", a->model.name[0] ? a->model.name : "(none)", eff);
    app_out_z(a, TUI_DIM, line);
    if (a->model.ctx_window)
        snprintf(line, sizeof line, "context %llu of %u tokens (%u%%)", (unsigned long long)a->model.ctx_used, a->model.ctx_window,
                 (unsigned)(a->model.ctx_used * 100 / a->model.ctx_window));
    else
        snprintf(line, sizeof line, "context %llu tokens", (unsigned long long)a->model.ctx_used);
    app_out_z(a, TUI_DIM, line);
    snprintf(line, sizeof line, "terminal %dx%d · %s · %s · kitty keys %s · %s · %s symbols", a->cols, a->rows,
             getenv("TERM") ? getenv("TERM") : "(no TERM)", a->band.reflow ? "reflows" : "does not reflow",
             a->caps.kitty ? "on" : "off", a->caps.answered ? "answers queries" : "no query replies",
             a->ascii ? "ascii" : "unicode");
    app_out_z(a, TUI_DIM, line);
}

/* The usage doc: tokens per model, and what they cost where prices are known. */
static void cmd_usage(app *a)
{
    const pt_json *u, *c;
    char line[600], in[16], out[16], cache[16];
    pt_arena_reset(&a->ar);
    u = a->open ? pt_json_parse(pt_doc(a->s.a, PT_S("usage")), &a->ar, NULL) : NULL;
    if (!pt_json_is(u, PT_J_OBJ) || !u->len) {
        app_out_z(a, TUI_DIM, "no tokens used yet");
        return;
    }
    for (c = u->child; c; c = c->next) {
        uint64_t ti = (uint64_t)pt_json_geti(c, "input", 0), to = (uint64_t)pt_json_geti(c, "output", 0);
        uint64_t cr = (uint64_t)pt_json_geti(c, "cache_read", 0), cw = (uint64_t)pt_json_geti(c, "cache_write", 0);
        pt_ai_target t;
        const char *why;
        int n;
        app_tokens_short(in, sizeof in, ti + cr + cw);
        app_tokens_short(out, sizeof out, to);
        app_tokens_short(cache, sizeof cache, cr);
        n = snprintf(line, sizeof line, "%.*s: %s in (%s from cache) · %s out · %lld requests", (int)c->key.n,
                     c->key.p, in, cache, out, (long long)pt_json_geti(c, "requests", 0));
        if (pt_ai_resolve(cli_catalog(), c->key, &t, &a->ar, &why) && t.known && t.model.cost.input) {
            const pt_cost *k = &t.model.cost; /* micro-dollars per million tokens */
            double usd = ((double)ti * (double)k->input + (double)to * (double)k->output +
                          (double)cr * (double)k->cache_read + (double)cw * (double)k->cache_write) / 1e12;
            snprintf(line + n, sizeof line - (size_t)n, " · $%.2f", usd);
        }
        app_out_z(a, TUI_DIM, line);
    }
}

static void cmd_help(app *a)
{
    size_t i;
    char line[200];
    for (i = 0; i < NCMDS; i++) {
        snprintf(line, sizeof line, "/%s%s  %s", app_cmds[i].name, app_cmds[i].args, app_cmds[i].desc);
        app_out_z(a, TUI_DIM, line);
    }
    app_out_z(a, TUI_DIM, "$name anywhere in a message, or /skill:name first, gives the model a skill's instructions");
    if (a->mcp && a->mcp->nall)
        app_out_z(a, TUI_DIM, "/server:prompt [args] sends an MCP server's prompt; @server:uri adds a resource to a message");
    app_out_z(a, TUI_DIM,
          "enter send · shift+enter, alt+enter or ctrl-j newline · esc esc interrupt · ctrl-c clear, "
          "interrupt or quit · ctrl-d quit · ctrl-t thinking · ctrl-o details · ctrl-z suspend · ctrl-l redraw");
}

/* Where each provider's key comes from (oauth false), or whether each
 * login is there (oauth true). */
static void login_status(app *a, bool oauth)
{
    const pt_ai_catalog *cat = cli_catalog();
    char msg[400];
    uint32_t i;
    for (i = 0; i < cat->nproviders; i++) {
        const pt_provider *q = &cat->providers[i];
        if (q->auth == PT_AUTH_NONE || (q->auth == PT_AUTH_OAUTH) != oauth)
            continue;
        if (oauth) {
            cli_login l;
            if (!cli_auth_login(q->id, false, &l))
                snprintf(msg, sizeof msg, "%s: not logged in", q->id);
            else if (l.expires <= pt_sys_now_ms())
                snprintf(msg, sizeof msg, "%s: logged in (refreshed when next used)", q->id);
            else
                snprintf(msg, sizeof msg, "%s: logged in", q->id);
        } else {
            const char *env = q->key_env[0] ? getenv(q->key_env[0]) : NULL;
            snprintf(msg, sizeof msg, "%s: %s%s", q->id,
                     env && *env ? "from " : cli_auth_key(q->id) ? "saved in auth.json" : "no key",
                     env && *env ? q->key_env[0] : "");
        }
        app_out_z(a, TUI_DIM, msg);
    }
}

/* "/login oauth": whether each login is there. "/login oauth <provider>":
 * sign in in the browser. "/login oauth <provider> <address>": the
 * address the browser ended on, for a login waiting for it. */
static void login_oauth(app *a, const pt_provider *p, pt_str answer)
{
    char msg[4400], err[600], url[4096];
    if (answer.n) {
        if (!a->login)
            snprintf(msg, sizeof msg, "no login is waiting: /login oauth %s starts one", p->id);
        else if (!cli_oauth_paste(a->login, answer))
            snprintf(msg, sizeof msg, "that is not an address the login can take");
        else
            return; /* it says how it went */
        app_out_z(a, TUI_RED, msg);
        return;
    }
    cli_oauth_free(a->login); /* a second /login starts over */
    if (!(a->login = cli_oauth_begin(p->id, url, sizeof url, err, sizeof err))) {
        app_out_z(a, TUI_RED, err);
        return;
    }
    app_out_z(a, TUI_DIM, "sign in with your ChatGPT account in the browser; if it did not open, go to:");
    app_out_z(a, 0, url);
    if (!cli_oauth_listening(a->login))
        snprintf(msg, sizeof msg,
                 "port 1455 is in use, so the browser cannot come back here: when it ends on a page that "
                 "does not load, paste its address after /login oauth %s",
                 p->id);
    else
        snprintf(msg, sizeof msg,
                 "waiting for the browser (5 minutes); if it ends on a page that does not load (pytte on "
                 "another machine), paste its address after /login oauth %s",
                 p->id);
    app_out_z(a, TUI_DIM, msg);
}

void app_finish_login(app *a)
{
    char msg[700];
    bool ok;
    if (!a->login || !cli_oauth_done(a->login, &ok, msg, sizeof msg))
        return;
    cli_oauth_free(a->login);
    a->login = NULL;
    app_out_z(a, ok ? TUI_DIM : TUI_RED, msg);
}

/* Where each search key comes from. */
static void search_status(app *a)
{
    char msg[400], k[4200];
    int i;
    for (i = 0; i < 2; i++) {
        const char *env = cli_search_env(cli_search_backends[i]);
        snprintf(msg, sizeof msg, "%s: %s%s", cli_search_backends[i],
                 getenv(env) && *getenv(env) ? "from " : cli_search_saved(cli_search_backends[i]) ? "saved in auth.json" : "no key",
                 getenv(env) && *getenv(env) ? env : "");
        app_out_z(a, TUI_DIM, msg);
    }
    if (!cli_search_key(NULL, "exa", k, sizeof k) && !cli_search_key(NULL, "parallel", k, sizeof k))
        app_out_z(a, TUI_DIM, "/login search <exa|parallel> saves a key for web_search (one in the environment comes first)");
}

/* Whether either search backend has a key. */
static bool search_has_key(void)
{
    char k[4200];
    return cli_search_key(NULL, "exa", k, sizeof k) || cli_search_key(NULL, "parallel", k, sizeof k);
}

/* "/login search": where each key comes from. "/login search <backend>":
 * ready for the key. "/login search <backend> <key>": save it, then ask
 * about turning the web tools on. */
static void login_search(app *a, pt_str name, pt_str key)
{
    char msg[4400], err[600], path[4200], lbl[16];
    const char *backend = NULL;
    int i;
    if (!name.n) {
        search_status(a);
        return;
    }
    for (i = 0; i < 2; i++)
        if (pt_str_eqz(name, cli_search_backends[i]))
            backend = cli_search_backends[i];
    if (!backend) {
        snprintf(msg, sizeof msg, "no search backend %.*s: exa or parallel", (int)(name.n > 100 ? 100 : name.n), name.p);
        app_out_z(a, TUI_RED, msg);
        return;
    }
    snprintf(lbl, sizeof lbl, "%s", backend);
    lbl[0] = (char)(lbl[0] - 'a' + 'A');
    if (!key.n) {
        snprintf(msg, sizeof msg, "/login search %s ", backend);
        tui_input_set(&a->in, pt_strz(msg));
        snprintf(msg, sizeof msg, "paste the %s API key and press enter", lbl);
        app_out_z(a, TUI_DIM, msg);
        return;
    }
    if (!cli_auth_valid(key)) {
        app_out_z(a, TUI_RED, "not an API key: it has spaces or control characters");
        return;
    }
    if (!cli_search_save(backend, key, err, sizeof err)) {
        app_out_z(a, TUI_RED, err);
        return;
    }
    cli_auth_path(path, sizeof path);
    snprintf(msg, sizeof msg, "saved the %s key to %s", lbl, path);
    app_out_z(a, TUI_DIM, msg);
    if (err[0])
        app_out_z(a, TUI_RED, err);
    if (getenv(cli_search_env(backend)) && *getenv(cli_search_env(backend))) {
        snprintf(msg, sizeof msg, "%s is set in the environment and is used instead", cli_search_env(backend));
        app_out_z(a, TUI_DIM, msg);
    }
    if (!cli_tool_on(a->cfg, PT_S("web_search")) || !cli_tool_on(a->cfg, PT_S("web_fetch"))) {
        a->web_prompt = true;
        app_out_z(a, 0, "Turn on web_search and web_fetch? (y/n)");
    }
}

/* The answer to that question, when text is one: true if it was taken.
 * Anything else drops the question and is handled as usual. */
bool app_web_answer(app *a, pt_str text)
{
    bool yes = pt_str_ieq(text, PT_S("y")) || pt_str_ieq(text, PT_S("yes"));
    if (!a->web_prompt)
        return false;
    a->web_prompt = false;
    if (!yes && !pt_str_ieq(text, PT_S("n")) && !pt_str_ieq(text, PT_S("no")))
        return false;
    if (yes) {
        app_tool_set(a, PT_S("web_search"), true);
        app_tool_set(a, PT_S("web_fetch"), true);
    }
    return true;
}

/* "/login": api, oauth or search. "/login api": where each provider's key comes
 * from. "/login api <provider>": ready for the key. "/login api <provider>
 * <key>": save it. "/login oauth ...": login_oauth. */
static void cmd_login(app *a, pt_str arg)
{
    const pt_ai_catalog *cat = cli_catalog();
    const pt_provider *p = NULL;
    pt_str how = pt_str_trim(arg), name = PT_STR_NULL, key = PT_STR_NULL;
    long sp = pt_str_find(how, ' ');
    char msg[4400], err[600], path[4200];
    bool oauth;
    uint32_t i;
    if (sp >= 0) {
        name = pt_str_trim((pt_str){how.p + sp + 1, how.n - (size_t)sp - 1});
        how.n = (size_t)sp;
        if ((sp = pt_str_find(name, ' ')) >= 0) {
            key = pt_str_trim((pt_str){name.p + sp + 1, name.n - (size_t)sp - 1});
            name.n = (size_t)sp;
        }
    }
    if (!how.n) {
        login_status(a, false);
        login_status(a, true);
        search_status(a);
        app_out_z(a, TUI_DIM,
                  "/login api <provider> saves an API key (one in the environment comes first); "
                  "/login oauth <provider> signs in with a subscription; "
                  "/login search <exa|parallel> saves a key for web_search");
        return;
    }
    if (pt_str_eqz(how, "search")) {
        login_search(a, name, key);
        return;
    }
    if (!pt_str_eqz(how, "api") && !pt_str_eqz(how, "oauth")) {
        app_out_z(a, TUI_RED, "/login api <provider> [key], /login oauth <provider>, or /login search <exa|parallel> [key]");
        return;
    }
    oauth = pt_str_eqz(how, "oauth");
    if (!name.n) {
        login_status(a, oauth);
        return;
    }
    for (i = 0; i < cat->nproviders; i++)
        if (cat->providers[i].auth != PT_AUTH_NONE && (cat->providers[i].auth == PT_AUTH_OAUTH) == oauth &&
            pt_str_eqz(name, cat->providers[i].id))
            p = &cat->providers[i];
    if (!p) {
        snprintf(msg, sizeof msg, "%s: %.*s", oauth ? "no subscription login for" : "no API key for",
                 (int)(name.n > 100 ? 100 : name.n), name.p);
        app_out_z(a, TUI_RED, msg);
        return;
    }
    if (oauth) {
        login_oauth(a, p, key);
        return;
    }
    if (!key.n) {
        snprintf(msg, sizeof msg, "/login api %s ", p->id);
        tui_input_set(&a->in, pt_strz(msg));
        snprintf(msg, sizeof msg, "paste the %s API key and press enter", p->name);
        app_out_z(a, TUI_DIM, msg);
        return;
    }
    if (!cli_auth_valid(key)) {
        app_out_z(a, TUI_RED, "not an API key: it has spaces or control characters");
        return;
    }
    if (!cli_auth_save(p->id, key, err, sizeof err)) {
        app_out_z(a, TUI_RED, err);
        return;
    }
    cli_auth_path(path, sizeof path);
    snprintf(msg, sizeof msg, "saved the %s key to %s", p->name, path);
    app_out_z(a, TUI_DIM, msg);
    if (err[0])
        app_out_z(a, TUI_RED, err);
    if (p->key_env[0] && getenv(p->key_env[0]) && *getenv(p->key_env[0])) {
        snprintf(msg, sizeof msg, "%s is set in the environment and is used instead", p->key_env[0]);
        app_out_z(a, TUI_DIM, msg);
    }
}

/* From a server, inside a step: shown at the loop's next pass. */
void app_mcp_note(void *ud, const char *server, const char *msg)
{
    app *a = (app *)ud;
    pt_buf_putz(&a->mcp_notes, server ? server : "");
    pt_buf_putc(&a->mcp_notes, '\t');
    pt_buf_putz(&a->mcp_notes, msg);
    pt_buf_putc(&a->mcp_notes, '\n');
    set_dirty(a);
}

void app_show_notes(app *a)
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
        app_out_z(a,
              app_starts(msg, "failed") || app_starts(msg, "stopped") || app_starts(msg, "login failed") ||
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
        app_out_z(a, TUI_DIM, "MCP servers are off (--no-mcp)");
        return;
    }
    n = a->open ? pt_rt_mcp_servers(a->s.rt, in, CLI_MCP_MAX) : 0;
    if (n > CLI_MCP_MAX)
        n = CLI_MCP_MAX;
    if (!sub.n) {
        if (!a->mcp->nall) {
            app_out_z(a, TUI_DIM, "no MCP servers: add them to mcpServers in ~/.config/pytte/config.json, or to "
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
            app_out_z(a, in[i].state == PT_MCP_FAILED || in[i].state == PT_MCP_AUTH ? TUI_RED : TUI_DIM, msg);
        }
        for (i = 0; i < a->mcp->nall; i++)
            if (!a->mcp->info[i].runs) {
                snprintf(msg, sizeof msg, "%s · not trusted (%s): /mcp trust runs this project's servers",
                         a->mcp->all[i].name, a->mcp->info[i].source);
                app_out_z(a, TUI_DIM, msg);
            }
        return;
    }
    if (pt_str_eqz(sub, "trust") || pt_str_eqz(sub, "untrust")) {
        bool on = sub.p[0] == 't';
        pt_rt_mcp_server add[CLI_MCP_MAX];
        uint32_t k = 0;
        if (!cli_mcp_trust(a->mcp, on, err, sizeof err)) {
            app_out_z(a, TUI_RED, err);
            return;
        }
        if (!on) {
            app_out_z(a, TUI_DIM, "no longer trusting this project's MCP servers: new sessions start without them");
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
            app_out_z(a, TUI_RED, "out of memory starting them");
            return;
        }
        snprintf(msg, sizeof msg, "trusted this project's MCP servers%s", k ? ": starting them; their tools come with the next session (/new) or after /compact" : "");
        app_out_z(a, TUI_DIM, msg);
        return;
    }
    if (!pt_str_eqz(sub, "login") && !pt_str_eqz(sub, "logout") && !pt_str_eqz(sub, "restart")) {
        app_out_z(a, TUI_RED, "/mcp [login|logout|restart NAME] [trust|untrust]");
        return;
    }
    if (!name.n) {
        app_out_z(a, TUI_RED, "which server?");
        return;
    }
    for (i = 0; i < n && !pt_str_eqz(name, in[i].name); i++)
        ;
    if (i == n) {
        snprintf(msg, sizeof msg, "no MCP server %.*s%s", (int)(name.n > 64 ? 64 : name.n), name.p,
                 app_mcp_known(a, name) ? " runs: /mcp trust runs this project's servers" : "");
        app_out_z(a, TUI_RED, msg);
        return;
    }
    if (pt_str_eqz(sub, "restart")) {
        pt_rt_mcp_restart(a->s.rt, name);
        snprintf(msg, sizeof msg, "restarting %s", in[i].name);
    } else if (strcmp(in[i].transport, "http") != 0) {
        snprintf(msg, sizeof msg, "%s is not a remote server: only those log in", in[i].name);
        app_out_z(a, TUI_RED, msg);
        return;
    } else if (pt_str_eqz(sub, "login")) {
        pt_rt_mcp_login(a->s.rt, name); /* it says what goes on */
        return;
    } else {
        pt_rt_mcp_logout(a->s.rt, name);
        snprintf(msg, sizeof msg, "logged out of %s", in[i].name);
    }
    app_out_z(a, TUI_DIM, msg);
}

/* ---- tools ---- */

/* "Read a text file, with line numbers: ..." to its first sentence. */
static pt_str first_sentence(pt_str d)
{
    size_t i;
    for (i = 0; i < d.n && d.p[i] != '\n'; i++)
        if (d.p[i] == '.' && (i + 1 == d.n || d.p[i + 1] == ' ' || d.p[i + 1] == '\n'))
            break;
    return (pt_str){d.p, i};
}

void app_tools_list(app *a)
{
    uint32_t i, n = 0;
    const pt_tool_def *d = a->open ? pt_rt_tool_all(a->s.rt, &n) : NULL;
    for (i = 0; i < n && a->menu.n < MAX_ITEMS; i++) {
        item *it;
        pt_str desc = first_sentence(d[i].description);
        if (cli_tool_leader(d[i].name)) /* listed as its leader */
            continue;
        it = &a->menu.items[a->menu.n++];
        it->at = a->menu.buf.n;
        it->n = d[i].name.n;
        pt_buf_puts(&a->menu.buf, d[i].name);
        it->dat = a->menu.buf.n;
        it->dn = desc.n;
        pt_buf_puts(&a->menu.buf, desc);
    }
    if (a->menu.sel >= a->menu.n)
        a->menu.sel = a->menu.top = 0;
}

/* Tools on, of all registered. */
uint32_t app_tools_on(app *a, uint32_t *all)
{
    uint32_t i, n = 0, on = 0;
    const pt_tool_def *d = a->open ? pt_rt_tool_all(a->s.rt, &n) : NULL;
    *all = 0;
    for (i = 0; i < n; i++)
        if (!cli_tool_leader(d[i].name)) {
            on += cli_tool_on(a->cfg, d[i].name);
            (*all)++;
        }
    return on;
}

void app_tools_open(app *a)
{
    if (!a->open) { /* its runtime lists the tools: open with it */
        a->menu.tools_pending = a->sess.opening != NULL;
        a->menu.kind = MENU_NONE;
        if (!a->sess.opening)
            app_out_z(a, TUI_RED, "no session: no tools to list");
        return;
    }
    a->menu.kind = MENU_TOOLS;
    a->menu.sel = a->menu.top = 0;
    a->menu.closed = false;
    app_menu_update(a);
}

/* Turn a tool on or off: calls are refused (or run) at once, the model's
 * list changes with the next epoch; saved to the user file. */
void app_tool_set(app *a, pt_str name, bool on)
{
    const cli_tool_cfg *e = cli_tool_entry(a->cfg, name);
    const char *wins = e && strcmp(e->src, "project") == 0 ? "project" : NULL;
    config_job *job;
    uint32_t i;
    char msg[300];
    if (!cli_tool_set(a->cfg, name, on, "user")) {
        snprintf(msg, sizeof msg, "cannot turn %.100s %s: %s", name.p ? name.p : "", on ? "on" : "off",
                 a->cfg->ntools == CLI_TOOLS_MAX ? "the config names too many tools" : "its name cannot be a config key");
        app_out_z(a, TUI_RED, msg);
        return;
    }
    cli_tools_off(a->cfg, a->o.tools_off);
    if (a->open && (!pt_rt_tool_set_off(a->s.rt, name, !on) ||
                    (cli_tool_follower(name) && !pt_rt_tool_set_off(a->s.rt, pt_strz(cli_tool_follower(name)), !on))))
        app_out_z(a, TUI_RED, "out of memory turning the tool off: its calls still run");
    if (!(job = saver(a)))
        return;
    for (i = 0; i < job->ntq && !pt_str_eqz(name, job->tq[i].name); i++)
        ;
    if (i == job->ntq)
        job->ntq++; /* room: one per tool, and cli_tool_set took a slot */
    snprintf(job->tq[i].name, sizeof job->tq[i].name, "%.*s", (int)name.n, name.p);
    job->tq[i].on = on;
    job->tq[i].now = a->open && pt_epoch_provisional(a->s.a); /* before the first request */
    job->tq[i].wins = wins;
    start_setting(a);
    if (on && pt_str_eqz(name, "web_search") && !search_has_key())
        app_out_z(a, TUI_DIM, "web_search has no search API key yet: run /login search exa (or parallel)");
}

/* "/tools": the menu. "/tools <name>": its state. "/tools <name> on|off". */
static void cmd_tools(app *a, pt_str arg)
{
    pt_str name = arg, v = PT_STR_NULL;
    long sp = pt_str_find(arg, ' ');
    const cli_tool_cfg *e;
    char msg[400];
    if (!arg.n) {
        app_tools_open(a);
        return;
    }
    if (sp >= 0) {
        name.n = (size_t)sp;
        v = pt_str_trim((pt_str){arg.p + sp + 1, arg.n - (size_t)sp - 1});
    }
    if (cli_tool_leader(name)) {
        snprintf(msg, sizeof msg, "%.*s goes with %s: /tools %s turns both on or off", (int)name.n, name.p,
                 cli_tool_leader(name), cli_tool_leader(name));
        app_out_z(a, TUI_RED, msg);
        return;
    }
    if (!a->open || !pt_rt_tool_has(a->s.rt, name)) {
        snprintf(msg, sizeof msg, "no tool named %.*s; /tools lists them", (int)(name.n > 100 ? 100 : name.n),
                 name.p);
        app_out_z(a, TUI_RED, msg);
        return;
    }
    if (v.n && !pt_str_eqz(v, "on") && !pt_str_eqz(v, "off")) {
        snprintf(msg, sizeof msg, "not a value for %.*s: %.*s (on, off)", (int)name.n, name.p,
                 (int)(v.n > 100 ? 100 : v.n), v.p);
        app_out_z(a, TUI_RED, msg);
        return;
    }
    if (v.n) {
        app_tool_set(a, name, pt_str_eqz(v, "on")); /* it says when it is saved */
        return;
    }
    e = cli_tool_entry(a->cfg, name);
    snprintf(msg, sizeof msg, "%.*s: %s (%s)", (int)name.n, name.p, cli_tool_on(a->cfg, name) ? "on" : "off",
             e ? e->src : "default");
    app_out_z(a, TUI_DIM, msg);
}

static bool valid_setting(int s, const char *v)
{
    int i;
    for (i = 0; i < app_settings[s].nv; i++)
        if (strcmp(v, app_settings[s].vals[i]) == 0)
            return true;
    return !app_settings[s].vals;
}

/* "not a value for effort: x (off, minimal, ...)" */
static void bad_value(app *a, const char *what, const char *const *vals, int nv, const char *v)
{
    char msg[700];
    int i, n = snprintf(msg, sizeof msg, "not a value for %s: %.100s", what, v);
    for (i = 0; i < nv && n > 0 && (size_t)n < sizeof msg; i++)
        n += snprintf(msg + n, sizeof msg - (size_t)n, "%s%s", i ? ", " : " (", vals[i]);
    if (nv && n > 0 && (size_t)n < sizeof msg)
        snprintf(msg + n, sizeof msg - (size_t)n, ")");
    app_out_z(a, TUI_RED, msg);
}

void app_run_command(app *a, pt_str line)
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
    app_group_close(a);
    app_out_blank(&a->lines); /* the command itself is not echoed: only what it did */
    if (pt_str_eqz(cmd, "model")) {
        if (arg.n) {
            pt_ai_target t;
            const char *why = NULL;
            pt_arena_reset(&a->ar);
            if (!pt_ai_resolve(cli_catalog(), arg, &t, &a->ar, &why)) {
                snprintf(msg, sizeof msg, "%s: %s", v, why ? why : "unknown");
                app_out_z(a, TUI_RED, msg);
                return;
            }
            set_model(a, v, true);
        }
        snprintf(msg, sizeof msg, "%smodel: %s", arg.n ? "switched " : "",
                 a->model.name[0] ? a->model.name : "(the session's last)");
        app_out_z(a, TUI_DIM, msg);
    } else if (pt_str_eqz(cmd, "effort")) {
        if (arg.n) {
            pt_reasoning r;
            if (!cli_effort_parse(arg, &r)) {
                bad_value(a, "effort", app_levels, (int)NLEVELS, v);
                return;
            }
            a->o.reasoning = r;
            app_set_config(a);
        }
        {
            char eff[40];
            app_effort_label(a, eff, sizeof eff);
            snprintf(msg, sizeof msg, "%seffort: %s%s", arg.n ? "switched " : "", eff,
                     effort_used(a, a->o.reasoning) != a->o.reasoning ? " (the nearest this model takes)" : "");
        }
        app_out_z(a, TUI_DIM, msg);
    } else if (pt_str_eqz(cmd, "new")) {
        app_switch_session(a, NULL);
    } else if (pt_str_eqz(cmd, "resume")) {
        if (!arg.n) {
            tui_input_set(&a->in, PT_S("/resume "));
            a->menu.closed = false;
            app_menu_update(a);
            return;
        }
        app_switch_session(a, v);
    } else if (pt_str_eqz(cmd, "fork")) {
        if (!a->open)
            app_out_z(a, TUI_RED, "no session to fork");
        else if (app_session_empty(a))
            app_out_z(a, TUI_DIM, "nothing to fork yet");
        else if (a->run.active)
            app_out_z(a, TUI_RED, "/fork waits for the run to end (esc esc stops it)");
        else {
            a->o.fork = true;
            app_switch_session(a, NULL);
            a->o.fork = false;
            a->sess.created = false; /* a copy: never discarded as empty */
        }
    } else if (pt_str_eqz(cmd, "worktree") || pt_str_eqz(cmd, "wt")) {
        if (arg.n && !cli_worktree_name_ok(v)) {
            snprintf(msg, sizeof msg, "not a worktree name: %.100s (letters, digits, - _ .)", v);
            app_out_z(a, TUI_RED, msg);
            return;
        }
        a->o.worktree = v; /* copied by the open */
        app_switch_session(a, NULL);
        a->o.worktree = NULL;
    } else if (pt_str_eqz(cmd, "status")) {
        cmd_status(a);
    } else if (pt_str_eqz(cmd, "compact")) {
        if (!a->open || !pt_compact(a->s.a))
            app_out_z(a, TUI_DIM, "nothing to compact yet");
    } else if (pt_str_eqz(cmd, "usage")) {
        cmd_usage(a);
    } else if (pt_str_eqz(cmd, "ps")) {
        cmd_ps(a);
    } else if (pt_str_eqz(cmd, "stop")) {
        uint32_t n = a->open ? pt_rt_backgrounds_stop(a->s.rt) : 0;
        char said[64];
        snprintf(said, sizeof said, n ? "stopping %u background command%s" : "no background commands running", n,
                 n == 1 ? "" : "s");
        app_out_z(a, TUI_DIM, said);
    } else if (pt_str_eqz(cmd, "thinking")) {
        if (!arg.n) {
            snprintf(msg, sizeof msg, "thinking: %s", app_th_names[a->thinking]);
            app_out_z(a, TUI_DIM, msg);
        } else if (!valid_setting(app_find_setting(PT_S("thinking")), v)) {
            bad_value(a, "thinking", app_th_names, NTH, v);
        } else {
            app_save_setting(a, CFG_THINKING, v); /* it says when it is saved */
        }
    } else if (pt_str_eqz(cmd, "settings")) {
        long sp2 = pt_str_find(arg, ' ');
        int s;
        if (!arg.n) {
            a->menu.kind = MENU_SETTINGS;
            a->menu.set_sel = a->menu.top = 0;
            return;
        }
        s = app_find_setting((pt_str){arg.p, sp2 >= 0 ? (size_t)sp2 : arg.n});
        if (s < 0) {
            size_t i;
            int n = snprintf(msg, sizeof msg, "settings:");
            for (i = 0; i < NSETTINGS && n > 0 && (size_t)n < sizeof msg; i++)
                n += snprintf(msg + n, sizeof msg - (size_t)n, "%s %s", i ? "," : "", app_settings[i].name);
            app_out_z(a, TUI_RED, msg);
            return;
        }
        if (app_settings[s].k == SET_TOOLS) { /* "/settings tools [name [on|off]]" is /tools */
            cmd_tools(a, sp2 >= 0 ? pt_str_trim((pt_str){arg.p + sp2 + 1, arg.n - (size_t)sp2 - 1}) : PT_S(""));
            return;
        }
        if (sp2 >= 0) {
            pt_str val = pt_str_trim((pt_str){arg.p + sp2 + 1, arg.n - (size_t)sp2 - 1});
            snprintf(v, sizeof v, "%.*s", (int)(val.n < sizeof v ? val.n : sizeof v - 1), val.p);
            if (app_settings[s].k == CFG_COMPACT_MODEL && strcmp(v, "none") == 0)
                v[0] = 0; /* the session's model compacts */
            if (!valid_setting(s, v))
                bad_value(a, app_settings[s].name, app_settings[s].vals, app_settings[s].nv, v);
            else
                app_save_setting(a, app_settings[s].k, v); /* it says when it is saved */
            return;
        }
        snprintf(msg, sizeof msg, "%s: %s (%s)", app_settings[s].name,
                 a->cfg->val[app_settings[s].k][0] ? a->cfg->val[app_settings[s].k] : "none",
                 a->cfg->src[app_settings[s].k]);
        app_out_z(a, TUI_DIM, msg);
    } else if (pt_str_eqz(cmd, "tools")) {
        cmd_tools(a, arg);
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
        app_out_z(a, TUI_RED, msg);
    }
}

/* The configured server of that name: running, or waiting for trust. */
bool app_mcp_known(app *a, pt_str name)
{
    uint32_t i;
    for (i = 0; a->mcp && i < a->mcp->nall; i++)
        if (pt_str_eqz(name, a->mcp->all[i].name))
            return true;
    return false;
}
