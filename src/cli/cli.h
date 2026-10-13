/* Shared by the pytte CLI (main.c) and the TUI (src/tui/app.c): settings
 * from config files, the environment and flags, and opening sessions. */
#ifndef PT_CLI_H
#define PT_CLI_H

#include "rt/rt.h"

/* The model for a new session when nothing else names one. */
#define DEFAULT_MODEL "anthropic/claude-opus-5-5"
/* The initial effort when no user, project, environment or flag sets one. */
#define DEFAULT_EFFORT PT_REASON_MEDIUM

/* ---- config.c: settings (section 19) ---- */

enum { CFG_MODEL, CFG_REASONING, CFG_THINKING, CFG_SYMBOLS, CFG_PAGER, CFG_PERMS, CFG_COMPACT_MODEL, CFG_SKILLS, CFG_N };

typedef struct cli_config {
    char val[CFG_N][512];
    const char *src[CFG_N]; /* where the value came from: "default", "user", "project", "env", "flag" */
    char user_path[4096];   /* ~/.config/pytte/config.json */
} cli_config;

/* The user's pytte directory: $XDG_CONFIG_HOME/pytte, else ~/.config/pytte.
 * It holds config.json, SYSTEM.md, APPEND_SYSTEM.md and skills/. */
void cli_config_dir(char *out, size_t cap);
/* The dotted key of a setting ("tui.thinking"). */
const char *cli_config_key(int k);
/* Only concrete user-facing levels (off through max). On failure, out
 * is unchanged; the AI library's default sentinel is not a CLI choice. */
bool cli_effort_parse(pt_str s, pt_reasoning *out);
/* Defaults, then the user file, the project file (.pytte/config.json in
 * the working directory) and PYTTE_* variables. Problems with a file are
 * reported in warn (empty when none); the file is then skipped. */
void cli_config_load(cli_config *c, char *warn, size_t cap);
void cli_config_set(cli_config *c, int k, const char *v, const char *src);
/* Save a setting to the user file and apply it (section 19.4). On failure
 * (a file that does not parse, I/O) it applies to this process only, and
 * err says why. */
bool cli_config_save(cli_config *c, int k, const char *v, char *err, size_t cap);
/* Persist one setting without mutating the caller's configuration. */
bool cli_config_write(const char *path, int k, const char *v, char *err, size_t cap);
/* Set key (dotted: "tui.thinking") to a JSON value in the file's text,
 * keeping every other member; out is indented by two spaces. False with err when the text is not a JSON
 * object, or the path is not made of objects. */
bool cli_json_set(pt_str src, const char *key, pt_str value, pt_buf *out, char *err, size_t cap);
/* Replace a file atomically (temporary file, fsync, rename), keeping the
 * old file's mode, or 0600 for a new one. */
bool cli_write_atomic(const char *path, pt_str bytes, char *err, size_t cap);

/* ---- config.c: API keys ----
 * Keys saved by /login live in auth.json next to config.json, by provider:
 * {"anthropic": {"key": "..."}}. A key in the environment comes first.
 * These are safe to call from any thread. */

/* $XDG_CONFIG_HOME/pytte/auth.json, else ~/.config/pytte/auth.json. */
void cli_auth_path(char *out, size_t cap);
/* Printable ASCII without spaces, 1 to 4096 bytes. */
bool cli_auth_valid(pt_str key);
/* The provider's saved key, or NULL. The file is read on first use; the
 * string stays valid until the process exits, even after a new save. */
const char *cli_auth_key(const char *provider);
/* Save the provider's key to auth.json (0600 when created), changing
 * nothing else in the file, and use it from now on. False with err when
 * it could not be saved; true with a non-empty err when it was saved but
 * this process could not take it. */
bool cli_auth_save(const char *provider, pt_str key, char *err, size_t cap);

/* ---- providers.c: the model catalog ----
 * The built-in catalog, merged with what `pytte update-catalog` fetched
 * (cli_catalog_path, a binary cache) and with the "providers" of the user's config.json:
 *
 *   "providers": {"groq": {"base_url": "https://api.groq.com/openai/v1",
 *                          "models": ["llama-3.3-70b-versatile"]}}
 *
 * A project's config.json cannot add providers: one could send a key from
 * the environment to any server. */

/* The catalog; the built-in one until cli_catalog_load. It lives for the
 * process (a second load frees the first). */
const pt_ai_catalog *cli_catalog(void);
/* $XDG_CACHE_HOME/pytte/models.bin, else ~/.cache/pytte/models.bin. */
void cli_catalog_path(char *out, size_t cap);
/* Build the catalog from the user file's "providers" (NULL: none; path is
 * for messages) and the fetched models. The first problem goes to warn,
 * when it is empty; a provider or model with a problem is skipped. */
void cli_catalog_load(const pt_json *providers, const char *path, char *warn, size_t cap);
/* models.dev's api.json and OpenRouter's /models in, models.bin out: the
 * built-in providers' models, and those of config.json's providers that
 * models.dev lists by the same id. msg says how many each got, or why it
 * failed. */
bool cli_catalog_convert(pt_str dev_json, pt_str openrouter_json, const char *updated, pt_buf *out, char *msg,
                         size_t cap);

/* ---- session.c: opening sessions ---- */

typedef struct cli_opts {
    const char *model, *compact_model, *system, *append_system, *script;
    bool manual_skills; /* skills are not listed for the model (skills.auto off) */
    pt_reasoning reasoning;
    pt_perm_mode perm;
    uint32_t max_tokens, retry_base_ms, context_window;
    bool no_sync, force, continue_current;
    /* For cli_session_begin replacing an open session: fork copies it to a
     * new session and opens the copy; worktree (a name) makes a git
     * worktree at <repo>/.pytte/worktrees/<name> on branch worktree-<name>,
     * or reuses it, and starts a new session there (the process changes
     * into it). */
    bool fork;
    const char *worktree;
    bool (*stop)(void *ud); /* polled by pt_rt_run (print mode) */
} cli_opts;

typedef struct cli_open_job cli_open_job;

typedef struct cli_session {
    char id[64], dir[4200];
    char host[256], boot[64]; /* the identity in claims */
    pt_agent *a;
    pt_rt *rt;
    pt_http_driver *http;
    cli_open_job *background; /* owns the interactive I/O worker, when used */
} cli_session;

/* $XDG_STATE_HOME/pytte/sessions (by default ~/.local/state/...). */
void cli_sessions_dir(char *out, size_t cap);
/* Session IDs, newest first; with cwd, only those started there (with
 * worktrees, also in its worktrees: see cli_session_cwd_under). Returns
 * how many were written (at most max). */
uint32_t cli_sessions_list(const char *cwd, bool worktrees, char (*ids)[64], uint32_t max);
/* cwd is base, or a worktree /worktree made there (base/.pytte/worktrees/...). */
bool cli_session_cwd_under(const char *cwd, const char *base);
/* A log's first line, without its newline: the session record and the
 * first epoch, as long as its system prompt made it. False when the line
 * is unfinished, past 16 MiB, or cannot be read. */
bool cli_log_first_line(int fd, pt_buf *line);
/* The session's original working directory, from its header. */
bool cli_session_cwd(const char *id, char *out, size_t cap);

/* A session's name, in <dir>/name: the first line of its first prompt, at
 * most CLI_NAME_MAX bytes, without control characters. */
#define CLI_NAME_MAX 80
/* False (out empty) when it has none. */
bool cli_session_name(const char *dir, char *out, size_t cap);
/* Name the session after prompt, unless it has a name. */
void cli_session_name_set(const char *dir, pt_str prompt);

/* 1 to 64 of letters, digits, - _ and ., not starting with - or ., no "..". */
bool cli_worktree_name_ok(const char *name);
/* Make the git worktree <repo>/.pytte/worktrees/<name> on branch
 * worktree-<name> (name NULL or "": a random one), or reuse it, and chdir
 * into it. note gets what was done; false with err on failure. */
bool cli_worktree_enter(const char *name, char *note, size_t ncap, char *err, size_t ecap);

/* Open a session: a new one (id NULL; then submit the first input), or an
 * existing one, recovered. Takes ownership (an open claim). Returns 0, or
 * an exit code (1 setup, 20 storage or corruption) with err set. */
int cli_session_open(cli_session *s, const cli_opts *o, const char *id, pt_event_fn on_event,
                     void *ud, char *err, size_t cap);
/* Interactive setup/recovery on a reusable worker. s is exclusively owned
 * by the job until finish returns true. Options/ID strings are copied;
 * on_event/ud remain borrowed for the session lifetime. finish never waits:
 * fd wakes the UI, code is the ordinary open result, and success transfers
 * ownership of the job to s. When replacing an open s, its close and draft
 * save run on the new worker before opening the replacement; its callback
 * is suppressed after handoff. discard removes an empty prior session.
 * cancel joins setup and releases it. */
cli_open_job *cli_session_begin(cli_session *s, const cli_opts *o, const char *id,
                                bool history, pt_str draft, bool discard,
                                pt_event_fn on_event, void *ud,
                                char *err, size_t cap);
int cli_session_job_fd(const cli_open_job *job);
bool cli_session_finish(cli_open_job *job, int *code, char *err, size_t cap);
void cli_session_cancel(cli_open_job *job);
/* Take malloc'd setup data (NULL when absent); caller frees it. */
char *cli_session_take_history(cli_session *s, size_t *n);
char *cli_session_take_draft(cli_session *s, size_t *n);
/* What a fork or worktree open did, for the user ("" when nothing). */
const char *cli_session_note(const cli_session *s);
/* Clean exit: the close claim, then free everything. */
void cli_session_close(cli_session *s);
/* Free without closing (the agent is fatal). */
void cli_session_free(cli_session *s);
/* Remove a closed session that holds nothing worth keeping. */
void cli_session_discard(const char *dir);

extern const char cli_default_system[];

/* ---- src/tui/app.c ---- */

/* The TUI on session id (NULL: a new one), sending prompt first when
 * given. Returns the exit code. */
int tui_run(const cli_opts *o, cli_config *cfg, const char *id, const char *prompt);

#endif
