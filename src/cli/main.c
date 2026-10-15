/* pytte: the durable agent CLI (section 16.1).
 *
 *   pytte ["PROMPT"]            the TUI in a new session
 *   pytte -c / -r [ID]          the TUI on this directory's newest session / on ID
 *   pytte -p "PROMPT"           one-shot: run until idle and print the answer
 *   ... | pytte -p "PROMPT"     the same, with stdin as data for the prompt
 *   pytte log [ID] [--json]     print a session's transcript
 *   pytte approve ID TASK yes|no   answer a tool call waiting for approval
 *   pytte rewind ID [--at ENTRY]   put files and conversation back to before a message
 *   pytte ls                    list sessions with their status
 *   pytte update-catalog        fetch the model catalog (models.dev, OpenRouter)
 *   pytte mcp [list|trust|untrust|login NAME|logout NAME]   MCP servers
 *
 * Kill the process at any point and `pytte -c` continues from its last
 * commit.
 *
 * Flags and dispatch are here; print mode is in print.c, the other
 * subcommands in commands.c (main.h). */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif

#include "cli/main.h"

static const char usage[] =
    "usage: pytte [options] [PROMPT]      the TUI in a new session\n"
    "       pytte -c | -r [ID] [PROMPT]   continue this directory's newest session, or ID\n"
    "       pytte -p [options] [PROMPT]   print mode: run until done, print the answer\n"
    "                                     on stdout; tool calls, thinking and the\n"
    "                                     text between them go to stderr. Piped\n"
    "                                     stdin is the PROMPT, or data for it\n"
    "       pytte log [ID] [--json] [--all-epochs]\n"
    "                                     print a session's transcript (by default\n"
    "                                     since the last compaction)\n"
    "       pytte approve ID TASK yes|no  answer a tool call waiting for approval,\n"
    "                                     then go on as -p would\n"
    "       pytte rewind ID [--at ENTRY] [--files | --conversation] [--yes]\n"
    "                                     put files and conversation back to before a\n"
    "                                     user message (default: the newest; ENTRY from\n"
    "                                     log --json), in a fork whose ID is printed.\n"
    "                                     --files: only files; --conversation: only the\n"
    "                                     fork; --yes: overwrite changed files\n"
    "       pytte ls                      list sessions, newest first: status, directory\n"
    "                                     and name (its first prompt)\n"
    "       pytte update-catalog          fetch the newest models from models.dev and\n"
    "                                     OpenRouter into ~/.cache/pytte/models.bin\n"
    "       pytte mcp [list]              MCP servers (mcpServers in config.json; a\n"
    "                                     project's in .mcp.json): their state, tools\n"
    "       pytte mcp trust | untrust     run this directory's project servers, or not\n"
    "       pytte mcp login | logout NAME log in to a remote server (in the browser)\n"
    "\n"
    "  -c, --continue          the newest session started in this directory\n"
    "  -r, --resume [ID]       that session, or pick from this directory's\n"
    "  -p, --print             no TUI (also when stdin or stdout is not a terminal)\n"
    "  -w, --worktree [NAME]   start in the git worktree <repo>/.pytte/worktrees/NAME\n"
    "                          on branch worktree-NAME, made if missing (default: a\n"
    "                          random name; --worktree=NAME, or -- before a PROMPT\n"
    "                          that could be a name)\n"
    "  -m, --model REF         provider/model-id (default: $PYTTE_MODEL, the config\n"
    "                          file; else the session's last, or " DEFAULT_MODEL ")\n"
    "  --effort LEVEL          off, minimal, low, medium, high, xhigh, max ($PYTTE_EFFORT)\n"
    "                          starts at medium unless configured otherwise\n"
    "  --compact-model REF     the model that writes compaction summaries (default:\n"
    "                          $PYTTE_COMPACT_MODEL, the config file, else the model)\n"
    "  --max-tokens N          default: the model's maximum that fits the context\n"
    "  --context-window N      compact as if the model's window were N tokens\n"
    "  --system-prompt TEXT    system prompt of a new session\n"
    "  --append-system-prompt TEXT\n"
    "  --output-format FMT     with -p: text (default), json (one object at the end),\n"
    "                          stream-json (every commit as a JSON line)\n"
    "  --permission-mode MODE  ask (default), auto or read-only: what tools that may\n"
    "                          change things do ($PYTTE_PERMISSION_MODE, permissions.mode)\n"
    "  --script FILE           faux provider script (default: $PYTTE_FAUX_SCRIPT)\n"
    "  --no-stdin              with -p: leave stdin unread (as ssh -n does), for\n"
    "                          loops and callers that keep it open\n"
    "  --no-sync               do not fsync commits (survives crashes, not power loss)\n"
    "  --no-mcp                start no MCP servers\n"
    "  --force                 take over a session another host left open\n"
    "\n"
    "Exit codes: 0 done, 1 usage or setup, 3 waiting for approval (-p), 10 the run\n"
    "failed or stdout could not be written, 20 storage error, 130 interrupted, 141\n"
    "stdout closed (the session stays resumable after either).\n";

void die(const char *msg, const char *arg)
{
    fprintf(stderr, "pytte: %s%s%s\n", msg, arg ? ": " : "", arg ? arg : "");
    exit(1);
}

/* YYYYMMDD-HHMMSS-xxxx */
static bool looks_like_id(const char *s)
{
    return strlen(s) == 20 && s[8] == '-' && s[15] == '-' && s[0] >= '0' && s[0] <= '9';
}

static void parse_args(int argc, char **argv, opts *o)
{
    int i;
    memset(o, 0, sizeof o[0]);
    o->c.reasoning = DEFAULT_EFFORT;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
#define TAKES(name) (strcmp(a, name) == 0 && (v || (die("missing value for", name), 0)) && ++i)
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            fputs(usage, stdout);
            exit(0);
        } else if (strcmp(a, "--version") == 0) {
            puts("pytte " PT_VERSION);
            exit(0);
        } else if (strcmp(a, "-p") == 0 || strcmp(a, "--print") == 0) {
            o->print = true;
        } else if (strcmp(a, "-c") == 0 || strcmp(a, "--continue") == 0) {
            o->cont = true;
        } else if (strcmp(a, "-r") == 0 || strcmp(a, "--resume") == 0) {
            o->resume = true;
            if (v && looks_like_id(v))
                o->id = argv[++i];
        } else if (strcmp(a, "-w") == 0 || strcmp(a, "--worktree") == 0) {
            o->c.worktree = ""; /* a random name */
            if (v && cli_worktree_name_ok(v))
                o->c.worktree = argv[++i];
        } else if (strncmp(a, "--worktree=", 11) == 0) {
            o->c.worktree = a + 11;
            if (!cli_worktree_name_ok(o->c.worktree))
                die("not a worktree name (letters, digits, - _ .)", o->c.worktree);
        } else if (strcmp(a, "--json") == 0) { /* log */
            o->json = true;
        } else if (strcmp(a, "--all-epochs") == 0) { /* log */
            o->all_epochs = true;
        } else if (strcmp(a, "--files") == 0) { /* rewind */
            o->files_only = true;
        } else if (strcmp(a, "--conversation") == 0) {
            o->conversation_only = true;
        } else if (strcmp(a, "--yes") == 0) {
            o->yes = true;
        } else if (TAKES("--at")) {
            o->at = v;
        } else if (strcmp(a, "--no-stdin") == 0) {
            o->no_stdin = true;
        } else if (strcmp(a, "--no-mcp") == 0) {
            o->no_mcp = true;
        } else if (strcmp(a, "--no-sync") == 0) {
            o->c.no_sync = true;
        } else if (strcmp(a, "--force") == 0) {
            o->c.force = true;
        } else if (TAKES("-m") || TAKES("--model")) {
            o->c.model = v;
            o->model_flag = true;
        } else if (TAKES("--compact-model")) {
            o->c.compact_model = v;
            o->compact_flag = true;
        } else if (TAKES("--system-prompt")) {
            o->c.system = v;
        } else if (TAKES("--append-system-prompt")) {
            o->c.append_system = v;
        } else if (TAKES("--script")) {
            o->c.script = v;
        } else if (TAKES("--effort")) {
            if (!cli_effort_parse(pt_strz(v), &o->c.reasoning))
                die("unknown effort level", v);
            o->effort_flag = true;
        } else if (TAKES("--permission-mode")) {
            if (!pt_perm_parse(pt_strz(v), &o->c.perm))
                die("unknown permission mode (ask, auto, read-only)", v);
            o->perm_flag = true;
        } else if (TAKES("--output-format")) {
            if (strcmp(v, "text") == 0)
                o->out = OUT_TEXT;
            else if (strcmp(v, "json") == 0)
                o->out = OUT_JSON;
            else if (strcmp(v, "stream-json") == 0)
                o->out = OUT_STREAM;
            else
                die("unknown output format", v);
        } else if (TAKES("--max-tokens") || TAKES("--retry-base-ms") || TAKES("--context-window")) {
            uint64_t n;
            if (!pt_str_to_u64(pt_strz(v), &n) || n == 0 || n > UINT32_MAX)
                die("bad number", v);
            if (strcmp(a, "--max-tokens") == 0)
                o->c.max_tokens = (uint32_t)n;
            else if (strcmp(a, "--context-window") == 0)
                o->c.context_window = (uint32_t)n;
            else
                o->c.retry_base_ms = (uint32_t)n; /* for tests */
        } else if (strcmp(a, "--") == 0 && v) {
            o->prompt = argv[++i];
        } else if (a[0] == '-' && a[1]) {
            die("unknown option", a);
        } else if (i == 1 && strcmp(a, "log") == 0) {
            o->cmd_log = a;
        } else if (i == 1 && strcmp(a, "ls") == 0) {
            o->cmd_ls = a;
        } else if (i == 1 && strcmp(a, "update-catalog") == 0) {
            o->cmd_update = a;
        } else if (i == 1 && strcmp(a, "mcp") == 0) {
            o->cmd_mcp = a;
        } else if (o->cmd_mcp) {
            if (o->mcp_args[1])
                die("too many arguments", a);
            o->mcp_args[o->mcp_args[0] ? 1 : 0] = a;
        } else if (i == 1 && strcmp(a, "approve") == 0) {
            o->cmd_approve = a;
        } else if (i == 1 && strcmp(a, "rewind") == 0) {
            o->cmd_rewind = a;
        } else if ((o->cmd_log || o->cmd_approve || o->cmd_rewind) && !o->id) {
            o->id = a;
        } else if (o->cmd_approve && !o->task) {
            o->task = a;
        } else if (o->cmd_approve && !o->answer) {
            if (strcmp(a, "yes") != 0 && strcmp(a, "no") != 0)
                die("answer yes or no", a);
            o->answer = a;
        } else if (o->cmd_ls || o->cmd_update) {
            die(o->cmd_ls ? "ls takes no arguments" : "update-catalog takes no arguments", a);
        } else if (!o->prompt) {
            o->prompt = a;
        } else {
            die("too many arguments", a);
        }
#undef TAKES
    }
    if (o->cmd_approve && !o->answer)
        die("usage: pytte approve ID TASK yes|no", NULL);
    if (o->cmd_rewind && !o->id)
        die("usage: pytte rewind ID [--at ENTRY] [--files | --conversation] [--yes]", NULL);
    if (o->files_only && o->conversation_only)
        die("--files and --conversation exclude each other", NULL);
    if ((o->at || o->files_only || o->conversation_only || o->yes) && !o->cmd_rewind)
        die("--at, --files, --conversation and --yes belong to pytte rewind", NULL);
    if (!o->c.script)
        o->c.script = getenv("PYTTE_FAUX_SCRIPT");
}

/* An unknown model stops pytte before a session exists to leave behind. */
static void check_model(const char *ref, const char *src)
{
    pt_arena a;
    pt_ai_target t;
    const char *why;
    bool ok;
    if (!ref || !*ref)
        return;
    pt_arena_init(&a, pt_sys_allocator(), 0);
    ok = pt_ai_resolve(cli_catalog(), pt_strz(ref), &t, &a, &why);
    pt_arena_free(&a);
    if (!ok) {
        fprintf(stderr, "pytte: model %s (from %s): %s, e.g. %s\n", ref, src, why, DEFAULT_MODEL);
        exit(1);
    }
}

/* Say on stderr when the model does not take the chosen effort level and
 * requests use another. */
void effort_note(const char *ref, pt_reasoning want)
{
    pt_arena a;
    pt_ai_target t;
    const char *why;
    if (!ref || !*ref)
        return;
    pt_arena_init(&a, pt_sys_allocator(), 0);
    if (pt_ai_resolve(cli_catalog(), pt_strz(ref), &t, &a, &why) && pt_ai_effort(&t, want) != want)
        fprintf(stderr, "pytte: %s takes no effort %s; using %s\n", ref, pt_reasoning_name(want),
                pt_reasoning_name(pt_ai_effort(&t, want)));
    pt_arena_free(&a);
}

/* Model: a flag, else the environment; config files set new sessions'
 * defaults (an existing one keeps its last model). Effort: a flag, else
 * the layered settings, starting at an explicit medium. */
void apply_config(opts *o, cli_config *cfg, bool create)
{
    bool env_or_new;
    env_or_new = create || strcmp(cfg->src[CFG_MODEL], "env") == 0;
    if (o->model_flag)
        cli_config_set(cfg, CFG_MODEL, o->c.model, "flag");
    else if (cfg->val[CFG_MODEL][0] && env_or_new)
        o->c.model = cfg->val[CFG_MODEL];
    else if (create)
        o->c.model = DEFAULT_MODEL;
    if (o->effort_flag) {
        cli_config_set(cfg, CFG_REASONING, pt_reasoning_name(o->c.reasoning), "flag");
    } else if (!cli_effort_parse(pt_strz(cfg->val[CFG_REASONING]), &o->c.reasoning)) {
        fprintf(stderr, "pytte: unknown effort level %s (from %s)\n", cfg->val[CFG_REASONING],
                cfg->src[CFG_REASONING]);
        exit(1);
    }
    if (o->perm_flag) {
        cli_config_set(cfg, CFG_PERMS, pt_perm_name(o->c.perm), "flag");
    } else if (!pt_perm_parse(pt_strz(cfg->val[CFG_PERMS]), &o->c.perm)) {
        fprintf(stderr, "pytte: unknown permission mode %s (from %s)\n", cfg->val[CFG_PERMS],
                cfg->src[CFG_PERMS]);
        exit(1);
    }
    if (strcmp(cfg->val[CFG_SKILLS], "on") != 0 && strcmp(cfg->val[CFG_SKILLS], "off") != 0) {
        fprintf(stderr, "pytte: skills.auto is on or off, not %s (from %s)\n", cfg->val[CFG_SKILLS],
                cfg->src[CFG_SKILLS]);
        exit(1);
    }
    o->c.manual_skills = strcmp(cfg->val[CFG_SKILLS], "off") == 0;
    if (o->compact_flag)
        cli_config_set(cfg, CFG_COMPACT_MODEL, o->c.compact_model, "flag");
    else if (cfg->val[CFG_COMPACT_MODEL][0])
        o->c.compact_model = cfg->val[CFG_COMPACT_MODEL];
    check_model(o->c.model, o->c.model == cfg->val[CFG_MODEL] ? cfg->src[CFG_MODEL] : "flag");
    check_model(o->c.compact_model, cfg->src[CFG_COMPACT_MODEL]);
}

/* -r without an ID, in a terminal: pick from this directory's sessions. */
static bool pick_session(char *out, size_t cap)
{
    char ids[20][64], cwd[4096], line[64], root[4096];
    uint32_t n, i;
    cli_sessions_dir(root, sizeof root);
    unsigned long k;
    if (!getcwd(cwd, sizeof cwd) || !(n = cli_sessions_list(cwd, false, ids, 20)))
        return false;
    for (i = 0; i < n; i++) {
        char dir[4200], name[CLI_NAME_MAX + 1];
        snprintf(dir, sizeof dir, "%s/%s", root, ids[i]);
        cli_session_name(dir, name, sizeof name);
        fprintf(stderr, "%3u  %s  %s\n", i + 1, ids[i], name);
    }
    fprintf(stderr, "session [1]: ");
    if (!fgets(line, sizeof line, stdin))
        return false;
    k = line[0] == '\n' ? 1 : strtoul(line, NULL, 10);
    if (k < 1 || k > n)
        die("no such session", NULL);
    snprintf(out, cap, "%s", ids[k - 1]);
    return true;
}

int main(int argc, char **argv)
{
    opts o;
    cli_config cfg;
    cli_mcp *mcp;
    char warn[600], picked[64], cwd[4096];
    const char *term = getenv("TERM");
    bool tty;
#ifdef M_ARENA_MAX
    /* One heap: a worker thread's first malloc would otherwise reserve an
     * arena of its own, resident for the life of the process. */
    mallopt(M_ARENA_MAX, 1);
#endif
    parse_args(argc, argv, &o);
    if (o.cmd_log)
        return pytte_log(&o);
    if (o.cmd_ls)
        return pytte_ls();
    if (o.cmd_approve) {
        cli_config_load(&cfg, warn, sizeof warn);
        return pytte_print(&o, o.id, &cfg);
    }
    if (o.cmd_rewind) {
        cli_config_load(&cfg, warn, sizeof warn);
        return pytte_rewind(&o, &cfg);
    }
    cli_config_load(&cfg, warn, sizeof warn);
    if (warn[0])
        fprintf(stderr, "pytte: %s\n", warn);
    if (o.cmd_update)
        return pytte_update_catalog();
    if (o.cmd_mcp)
        return pytte_mcp(&o);
    tty = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    if (o.c.worktree && (o.cont || o.resume))
        die("-w starts a new session: not with -c or -r", NULL);
    if (o.c.worktree && (o.print || !tty)) { /* the TUI's open goes in, and says so */
        char note[700], err[700];
        if (!cli_worktree_enter(o.c.worktree, note, sizeof note, err, sizeof err))
            die(err, NULL);
        fprintf(stderr, "pytte: %s\n", note);
        o.c.worktree = NULL;
    }
    if (o.cont && (o.print || !tty)) {
        if (!getcwd(cwd, sizeof cwd) || !cli_sessions_list(cwd, false, &picked, 1))
            die("no session to continue in this directory", NULL);
        o.id = picked;
    } else if (o.resume && !o.id) {
        if (o.print || !tty)
            die("--resume needs a session ID here", NULL);
        if (!pick_session(picked, sizeof picked))
            die("no sessions in this directory", NULL);
        o.id = picked;
    }
    if (o.print || !tty)
        return pytte_print(&o, o.id, &cfg);
    if (term && strcmp(term, "dumb") == 0)
        die("TERM=dumb is not supported yet; use -p", NULL);
    o.c.continue_current = o.cont;
    apply_config(&o, &cfg, o.id == NULL && !o.cont);
    mcp = load_mcp(&o, true); /* the TUI says it, and asks */
    return tui_run(&o.c, &cfg, mcp, o.id, o.prompt);
}
