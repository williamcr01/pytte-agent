/* The runtime: the reference host loop. Turns the core's effects into
 * driver calls (store, HTTP, timers) and feeds the results back. Used by
 * the CLI and by the simulation tests (with the memory store, the faux
 * HTTP driver and a fake clock). */
#ifndef PT_RT_H
#define PT_RT_H

#include "drv/drv.h"
#include "pytte.h"

typedef struct pt_rt pt_rt;

typedef struct pt_rt_cfg {
    pt_store *store;
    pt_http_driver *http;
    /* The clock; NULL: the system's. Must match the agent's now_ms. */
    int64_t (*now)(void *ud);
    void (*sleep)(void *ud, uint32_t ms);
    void *clock_ud;
    /* Polled between effects; true stops pt_rt_run. */
    bool (*stop)(void *ud);
    void *stop_ud;
    /* Whose API key variables tools never see; NULL: the built-in one.
     * Must outlive the runtime. */
    const pt_ai_catalog *catalog;
    /* The session's ID, for {session} in MCP arguments (copied); NULL: none. */
    const char *session;
    /* web_search: the API key for a backend ("exa", "parallel") into out;
     * false when there is none. Called for each search. NULL: no keys. */
    bool (*search_key)(void *ud, const char *backend, char *out, size_t cap);
    void *search_ud;
    /* The backend to use ("exa", "parallel"); NULL or empty: the first with
     * a key, in that order. Must outlive the runtime. */
    const char *search_backend;
} pt_rt_cfg;

pt_rt *pt_rt_new(const pt_rt_cfg *c, pt_agent *a);
/* Cancels outstanding requests and closes the store. The HTTP driver
 * stays the caller's to free (after this). If an async tool does not finish
 * synchronously on cancel, destruction is deferred until its done callback. */
void pt_rt_free(pt_rt *rt);
pt_http_driver *pt_rt_http(const pt_rt *rt);
/* Borrow an idle worker to perform durable writes/live snapshots. I/O
 * completion and every agent callback remain on the calling thread.
 * Configure between steps, before scheduling effects. The worker must
 * outlive rt; free/close finish any borrowed in-flight effect first. */
void pt_rt_use_worker(pt_rt *rt, pt_worker *worker);

/* Recovery (section 6.4): read the log into the agent, cut a torn tail,
 * hand over live files. False with err when the log is corrupt. */
bool pt_rt_recover(pt_rt *rt, char *err, size_t cap);
/* Once the session is ours (pt_open), before any tool runs: stop the
 * process groups a dead owner left running, and forget them. A group
 * another host started is left to it. */
void pt_rt_stop_orphans(pt_rt *rt);

/* ---- tools (section 13) ----
 * The runtime keeps a registry of tools and answers the core's GATE and
 * TOOL effects. A tool's run starts a call; it reports output and its end
 * with pt_rt_tool_output and pt_rt_tool_done, at once or later from a
 * timer or an fd watch. */
typedef struct pt_rt_tool {
    pt_tool_def def;
    void (*run)(void *ud, pt_rt *rt, const pt_fx *call);
    /* Stop a running call (abort, exit). It must still end with
     * pt_rt_tool_done, now or later. During pt_rt_free, outstanding calls
     * keep the runtime state alive until their completion arrives. NULL:
     * calls finish inside run. */
    void (*cancel)(void *ud, pt_rt *rt, uint32_t fx);
    void *ud;
    /* Runs without asking, though it may change things (bash_poll stops
     * only what an approved bash call started). Read-only mode still
     * denies it: only def.read_only tools run there. */
    bool no_ask;
} pt_rt_tool;

/* Copied (strings must outlive rt, or its removal). False when out of
 * memory. */
bool pt_rt_tool_add(pt_rt *rt, const pt_rt_tool *t);
/* Runs calls of tools that are not registered (yet), when it claims
 * them: MCP tools whose server is still starting. Its def.name is a
 * prefix of the names it claims. A gate asks about such calls. */
void pt_rt_tool_fallback(pt_rt *rt, const pt_rt_tool *t);
/* Unregister a tool (no call of it may be running). Epochs that froze it
 * keep their copy; calls of it then find it unavailable. */
void pt_rt_tool_remove(pt_rt *rt, pt_str name);
/* Whether a tool of that name is registered. */
bool pt_rt_tool_has(pt_rt *rt, pt_str name);
/* read, write, edit (tools_builtin.c); bash, bash_poll (tools_bash.c);
 * grep, find, ls (tools_search.c); web_fetch (tools_web.c); web_search
 * (tools_websearch.c). */
bool pt_rt_add_builtins(pt_rt *rt);

/* A bash command that went to the background. command lives until the
 * next call into rt. */
typedef struct pt_rt_background {
    uint32_t session;
    const char *command;
    int64_t started; /* ms */
    bool ended;      /* ...and not read since */
    int status;      /* when ended: the exit code */
} pt_rt_background;

/* The background sessions, newest first; returns how many there are
 * (up to max are written). */
uint32_t pt_rt_backgrounds(pt_rt *rt, pt_rt_background *out, uint32_t max);
/* Stop every background session still running (SIGTERM, then SIGKILL
 * two seconds later); a later bash_poll reads what they printed.
 * Returns how many. */
uint32_t pt_rt_backgrounds_stop(pt_rt *rt);
/* The registered tools that are on, for a new epoch: what the model sees.
 * Valid until the next call that adds, removes or turns off a tool. */
const pt_tool_def *pt_rt_tool_defs(pt_rt *rt, uint32_t *n);
/* Every registered tool, on or off, in the same order and validity. */
const pt_tool_def *pt_rt_tool_all(pt_rt *rt, uint32_t *n);
/* Turn a tool off, or back on, by name: registered or not yet (an MCP
 * tool may come later). An off tool is left out of the tools the next
 * epoch freezes, and calls of it are denied from now on. False when out
 * of memory (nothing changed). */
bool pt_rt_tool_set_off(pt_rt *rt, pt_str name, bool off);
bool pt_rt_tool_off(const pt_rt *rt, pt_str name);

/* Output as it comes: shown live, kept for the result. */
void pt_rt_tool_output(pt_rt *rt, uint32_t fx, const void *p, size_t n);

typedef struct pt_rt_out {
    /* The text for the model. NULL p: the output reported so far, with
     * note appended. Long content is cut to its head and tail. */
    pt_str content, note;
    bool is_error;
    /* content is bounded by the tool itself (and says how to see more):
     * kept whole, never cut to its head and tail. */
    bool whole;
    pt_str details; /* a JSON object, or empty */
    pt_str doc_key, doc_value;
    /* An image after the text: a blob ref (pt_rt_put_blob) and its MIME
     * type, or empty. */
    pt_str image, image_mime;
} pt_rt_out;

void pt_rt_tool_done(pt_rt *rt, uint32_t fx, const pt_rt_out *r);
/* Store n bytes as a blob: true with ref set ("sha256:<hex>", NUL
 * terminated). False when the store keeps no blobs or writing failed. */
bool pt_rt_put_blob(pt_rt *rt, const void *p, size_t n, char ref[72]);
/* An image to attach to a user input, stored as a blob with read's limits
 * (bytes, pixels). pt_rt_attach_file: 1 attached (ref and mime set; mime is
 * static); 0 the file is not an image providers take; -1 it is one that
 * cannot be attached (err says why). pt_rt_attach_bytes is the same for
 * bytes already in memory (the clipboard): true, or false with err. */
int pt_rt_attach_file(pt_rt *rt, const char *path, char ref[72], const char **mime, char *err, size_t cap);
bool pt_rt_attach_bytes(pt_rt *rt, const void *p, size_t n, char ref[72], const char **mime, char *err, size_t cap);

/* The paths under the session's directory that find would list (it skips
 * .git and what .gitignore ignores), at most max, appended to out one per
 * line, relative, directories ending in "/". Returns how many; *cut says the
 * walk stopped early (max, or its own limit). */
uint32_t pt_rt_walk_paths(pt_rt *rt, uint32_t max, pt_buf *out, bool *cut);

/* What "@path" adds to a message, appended to out: a text file's contents
 * (at most what read shows, with a note when cut) or a directory's listing,
 * in a <file> or <directory> block. path is relative to the session's
 * directory, or absolute; "~/" is the home directory. 1 added; 0 there is no
 * such file (the @ was plain text); -1 it exists but cannot be added (err
 * says why). */
int pt_rt_mention_path(pt_rt *rt, pt_str path, pt_buf *out, char *err, size_t cap);

/* A process group a tool started (its pt_proc_mark), which must not
 * outlive this process: noted in the store while it may run, so that if
 * this process dies first the next owner stops it (pt_rt_stop_orphans).
 * Ended: once nothing of it runs. */
void pt_rt_proc_started(pt_rt *rt, int64_t group, pt_str mark);
void pt_rt_proc_ended(pt_rt *rt, int64_t group);
/* The agent the runtime drives (for a tool's lookups: pt_doc, the cwd). */
pt_agent *pt_rt_agent(pt_rt *rt);
/* pt_opts.blob for a store (blob_ud: the pt_store). */
bool pt_rt_read_blob(void *store, pt_str ref, pt_arena *ar, pt_str *out);

/* HTTP for tools and MCP, pumped by the runtime's loop: like the driver's
 * start and cancel. The sink's callbacks run inside a step; they must not
 * start or cancel requests (defer that with a timer at 0). */
void *pt_rt_http_start(pt_rt *rt, const pt_http_req *req, const pt_http_sink *sink);
void pt_rt_http_cancel(pt_rt *rt, void *h);

/* While held (counted), the answer to the core's EPOCH_MATERIALS waits: a
 * new session's first request waits for MCP servers to list their tools.
 * The last release answers it with the tools registered then. */
void pt_rt_materials_hold(pt_rt *rt, bool on);
/* Whether the core waits on a held answer (a prompt is held back). */
bool pt_rt_materials_waiting(const pt_rt *rt);

/* Event-loop hooks for calls that end later. A timer fires once at `at`
 * (wall-clock ms). A watch fires whenever fd is readable, until removed.
 * Callbacks may add and remove timers and watches. */
typedef void (*pt_rt_fn)(void *ud, pt_rt *rt);
uint32_t pt_rt_timer(pt_rt *rt, int64_t at, pt_rt_fn fn, void *ud);
void pt_rt_timer_cancel(pt_rt *rt, uint32_t id);
bool pt_rt_watch(pt_rt *rt, int fd, pt_rt_fn fn, void *ud);
void pt_rt_unwatch(pt_rt *rt, int fd);
int64_t pt_rt_now(pt_rt *rt);

/* ---- MCP: Model Context Protocol servers (mcp.c) ----
 * Servers start once the runtime's loop runs (pt_rt_step or pt_rt_run, on
 * the host thread): stdio servers as child processes, remote ones over
 * Streamable HTTP, with OAuth when they ask for it. Their tools register
 * as "mcp__<server>__<tool>" while starting; until each server has listed
 * its tools, failed, or its startup time has passed, the materials of a
 * provisional epoch wait (pt_rt_materials_hold), so a new session's first
 * request offers them. Their prompts and resources are listed for the
 * frontend: prompts become slash commands, resources @-mentions. */
typedef struct pt_rt_mcp_server {
    const char *name; /* 1 to 32 of [A-Za-z0-9_-] */
    /* stdio: the command, found on PATH, and its arguments */
    const char *command;
    const char *const *args;
    uint32_t nargs;
    const char *const *env; /* "NAME=value" added to its environment */
    uint32_t nenv;
    const char *cwd; /* NULL: the session's directory */
    /* http: the endpoint, and headers sent with every request */
    const char *url;
    const pt_header *headers;
    uint32_t nheaders;
    /* OAuth: a registered client (else one registers dynamically) and the
     * scope to ask for; NULL: none */
    const char *client_id, *client_secret, *scope;
    uint32_t timeout_s; /* a tool call; 0: 600 */
    uint32_t startup_s; /* listing its tools at start; 0: 15 */
    bool trust_hints;   /* the server's readOnlyHint makes a tool read-only */
    /* Arguments pytte fills in, as JSON text: {"<tool>" or "*": {"<arg>":
     * value}}; NULL: none. The model is not shown them: they leave the
     * tool's schema, and a call gets them in place of the model's. "*"
     * applies to the tools whose schema has the argument. In strings,
     * {model} becomes the session's model ("provider/model-id"),
     * {model_id} the part after the provider, {session} its ID. */
    const char *arguments;
} pt_rt_mcp_server;

/* The host's side: what to tell the user, and where OAuth tokens live.
 * Callbacks run on the host thread, inside a step. */
typedef struct pt_rt_mcp_host {
    void *ud;
    /* A line for the user ("ready: 12 tools", "needs login: ...").
     * server is NULL for messages about MCP as a whole. */
    void (*notify)(void *ud, const char *server, const char *msg);
    /* Open a login page in a browser; NULL: the URL is only notified. */
    void (*open_url)(void *ud, const char *url);
    /* The OAuth state saved for a server at url (a JSON object), into out;
     * false when there is none. Save it (empty: forget it). */
    bool (*auth_load)(void *ud, const char *server, const char *url, pt_buf *out);
    void (*auth_save)(void *ud, const char *server, const char *url, pt_str json);
} pt_rt_mcp_host;

/* Copied, strings and all; servers start at the first step. Called again
 * it adds servers (host is then ignored), started at once when the loop
 * runs already; their tools are offered from the next epoch. False when
 * out of memory. */
bool pt_rt_mcp_setup(pt_rt *rt, const pt_rt_mcp_host *host, const pt_rt_mcp_server *servers, uint32_t n);

typedef enum {
    PT_MCP_STARTING, /* connecting, or listing its tools */
    PT_MCP_READY,
    PT_MCP_FAILED, /* error says why; the next call of its tools starts it again */
    PT_MCP_AUTH,   /* it needs a login (pt_rt_mcp_login) */
    PT_MCP_STOPPED
} pt_mcp_state;

typedef struct pt_rt_mcp_info {
    const char *name, *transport; /* "stdio" or "http" */
    const char *target;           /* the command line, or the URL */
    pt_mcp_state state;
    const char *error; /* the last one, or "" */
    uint32_t ntools, nprompts, nresources;
} pt_rt_mcp_info;

/* The servers, in setup order; returns how many (up to max written).
 * Strings live until the next step. */
uint32_t pt_rt_mcp_servers(pt_rt *rt, pt_rt_mcp_info *out, uint32_t max);

typedef struct pt_rt_mcp_arg {
    pt_str name, description;
    bool required;
} pt_rt_mcp_arg;

typedef struct pt_rt_mcp_prompt {
    pt_str server, name, description;
    const pt_rt_mcp_arg *args;
    uint32_t nargs;
} pt_rt_mcp_prompt;

typedef struct pt_rt_mcp_resource {
    pt_str server, uri, name, description, mime;
} pt_rt_mcp_resource;

/* The ready servers' prompts and resources, server by server; valid until
 * the next step. */
const pt_rt_mcp_prompt *pt_rt_mcp_prompts(pt_rt *rt, uint32_t *n);
const pt_rt_mcp_resource *pt_rt_mcp_resources(pt_rt *rt, uint32_t *n);

/* An asynchronous answer: error NULL and text, or error. Not called when
 * the runtime is freed first. */
typedef void (*pt_rt_mcp_done)(void *ud, const char *error, pt_str text);
/* A prompt's messages as one text, for the user's turn. args: a JSON
 * object of strings, or empty. False (fn not called) when the server or
 * prompt is unknown. */
bool pt_rt_mcp_get_prompt(pt_rt *rt, pt_str server, pt_str name, pt_str args, pt_rt_mcp_done fn, void *ud);
/* A resource's contents as text. */
bool pt_rt_mcp_read(pt_rt *rt, pt_str server, pt_str uri, pt_rt_mcp_done fn, void *ud);
/* Log in to a remote server (OAuth in the browser); progress is notified.
 * False when the server is unknown or not remote. */
bool pt_rt_mcp_login(pt_rt *rt, pt_str server);
/* Forget a remote server's tokens (it then needs a login). */
bool pt_rt_mcp_logout(pt_rt *rt, pt_str server);
/* Stop and start a server again. */
bool pt_rt_mcp_restart(pt_rt *rt, pt_str server);

/* ---- prompt files and skills (sections 14.1 and 14.2) ----
 * Looked up in four roots, later ones winning: ~/.agents and the user's
 * config directory (~/.config/pytte), then <cwd>/.agents and <cwd>/.pytte.
 * Prompt files live in the pytte roots only: SYSTEM.md replaces the
 * built-in system prompt and APPEND_SYSTEM.md is appended to it; for each,
 * the project's file wins over the user's. Skills are skills/<name>/SKILL.md
 * in every root; a later skill of the same name replaces an earlier one. */
typedef struct pt_rt_prompt {
    const char *base;       /* the built-in system prompt */
    const char *home;       /* holds .agents; NULL: none */
    const char *config_dir; /* the user's pytte directory; NULL: none */
    bool skills_auto;       /* list skills in the system prompt, for the model to use */
} pt_rt_prompt;

/* Copied. Until set, a new epoch keeps the current system prompt. */
bool pt_rt_set_prompt(pt_rt *rt, const pt_rt_prompt *p);
/* Change skills_auto (from the next epoch on). */
void pt_rt_skills_auto(pt_rt *rt, bool on);

/* The system prompt and skills index (a JSON array) for a new epoch in
 * cwd. prompt: the session's own settings (pt_session_prompt), a JSON
 * object with optional strings "system" (replaces the system prompt and
 * its files) and "append" (appended after the files). Rescans the skills.
 * False when out of memory. */
bool pt_rt_build_prompt(pt_rt *rt, pt_str cwd, pt_str prompt, pt_buf *system, pt_buf *skills);

typedef struct pt_rt_skill {
    pt_str name, description;
    pt_str path; /* of SKILL.md */
    bool manual; /* disable-model-invocation: only the user invokes it */
} pt_rt_skill;

/* The session's skills, sorted by name; with rescan, read again from
 * disk. Valid until the next rescan (pt_rt_expand_skills rescans). */
const pt_rt_skill *pt_rt_skills(pt_rt *rt, bool rescan, uint32_t *n);

/* Expand the skills a message invokes: "/skill:<name> rest" and "$<name>"
 * anywhere. Each skill's instructions come first, in a <skill> block, then
 * the text ("rest" for /skill:). Returns 1 with out set, 0 when the text
 * invokes no skill (out untouched), -1 with err (an unknown /skill:, a file
 * that cannot be read, out of memory). Rescans the skills first. */
int pt_rt_expand_skills(pt_rt *rt, pt_str text, pt_buf *out, char *err, size_t cap);

/* For display: when text starts with a skill block, name is its skill and
 * text is advanced past it. */
bool pt_rt_skill_block(pt_str *text, pt_str *name);

/* ---- permissions (section 15.5) ---- */
typedef enum {
    PT_PERM_ASK,      /* tools that change things ask first */
    PT_PERM_AUTO,     /* everything runs */
    PT_PERM_READ_ONLY /* only read-only tools run; the rest are denied */
} pt_perm_mode;

bool pt_perm_parse(pt_str s, pt_perm_mode *out);
const char *pt_perm_name(pt_perm_mode m);
void pt_rt_set_perm(pt_rt *rt, pt_perm_mode mode);
/* Allow a tool from now on in this process: calls whose gist (the first
 * string argument) starts with prefix as a word, or every call when prefix
 * is empty. */
bool pt_rt_allow(pt_rt *rt, pt_str tool, pt_str prefix);

/* Rules from config (section 15.5): "tool" or "tool(pattern)", with * in
 * the tool name and in patterns matching any text:
 *   bash(git diff:*)   every command of the line starts with the words "git diff"
 *   bash(git status)   every command of the line is exactly that
 *   web_fetch(*.test)  the URL's host; other tools: the first string argument
 *   mcp__github__*     every call of the tools matching the name
 * A deny rule matches a call when any command of a bash line matches it; it
 * beats allow rules and the mode, except that read-only stays read-only.
 * A project's allow rules apply only once pt_rt_rules_trust says so. */
typedef enum { PT_RULE_SESSION, PT_RULE_USER, PT_RULE_PROJECT } pt_rule_src;
/* Whether spec is a rule; why says what is wrong when not (static text). */
bool pt_rule_check(pt_str spec, const char **why);
/* Add a rule (copied). False when spec is not a rule, or out of memory. */
bool pt_rt_rule_add(pt_rt *rt, bool deny, pt_str spec, pt_rule_src src);
/* Drop the rules from config (user and project), keeping the session's. */
void pt_rt_rules_drop_config(pt_rt *rt);
/* Whether the project's allow rules apply (deny rules always do). */
void pt_rt_rules_trust(pt_rt *rt, bool trusted);

typedef struct pt_rt_rule_info {
    pt_str spec;
    bool deny;
    pt_rule_src src;
    bool active; /* false: a project's allow rule that is not trusted */
} pt_rt_rule_info;
/* The rules in the order they were added; returns how many there are (up
 * to max are written). The specs live until a rule is added. */
uint32_t pt_rt_rules(pt_rt *rt, pt_rt_rule_info *out, uint32_t max);
/* The first string argument of a call ("ls -la", "src/a.c"), or empty;
 * allocated in a. */
pt_str pt_rt_gist(pt_str args, pt_arena *a);
/* The host of a URL ("docs.python.org", "[::1]"), or empty. web_fetch's
 * allow rules name hosts: "allow web_fetch docs.python.org". */
pt_str pt_rt_url_host(pt_str url);

typedef enum {
    PT_RT_IDLE,    /* nothing left to do */
    PT_RT_STOPPED, /* cfg.stop said so */
    PT_RT_FATAL,   /* see pt_fatal / pt_error */
    PT_RT_STUCK,   /* nothing outstanding, yet not idle: a bug */
    PT_RT_BUSY,    /* pt_rt_step: work is outstanding */
    PT_RT_ASK      /* nothing can go on until a tool call is approved (pt_asks) */
} pt_rt_result;

/* Until idle with no background command running, fatal or stopped. */
pt_rt_result pt_rt_run(pt_rt *rt);
/* One bounded step, for hosts that wait on more than the agent (a TUI):
 * start what the agent hands out, then wait up to timeout_ms (-1: no
 * limit) for HTTP progress, the next sleeper or one of fds (marked
 * ready), and start what that produced. When the agent handed out work
 * the step only looks (timeout 0), so the host shows that work at once.
 * Waits on fds even when idle or fatal; cfg.stop is not consulted. */
pt_rt_result pt_rt_step(pt_rt *rt, int timeout_ms, pt_waitfd *fds, unsigned nfds);
/* Clean exit: drop outstanding requests and commit the close claim.
 * Pending work stays pending for the next owner. */
bool pt_rt_close(pt_rt *rt);

#endif
