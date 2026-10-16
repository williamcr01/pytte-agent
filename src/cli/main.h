/* The pytte binary's own parts: its options, and what main.c (flags and
 * dispatch), print.c (print mode) and commands.c (log, ls,
 * update-catalog, mcp) share. The TUI is tui_run (src/tui). */
#ifndef PT_CLI_MAIN_H
#define PT_CLI_MAIN_H

#include "cli/cli.h"

enum { OUT_TEXT, OUT_JSON, OUT_STREAM };

typedef struct opts {
    cli_opts c;
    const char *cmd_log, *cmd_approve, *cmd_rewind, *cmd_ls, *cmd_update, *cmd_mcp, *id, *prompt, *task, *answer;
    const char *at; /* rewind --at */
    const char *mcp_args[2];
    bool print, cont, resume, json, all_epochs, model_flag, compact_flag, effort_flag, perm_flag;
    bool no_stdin, no_mcp;
    bool files_only, conversation_only, yes; /* rewind */
    int out;
} opts;

/* main.c */
void die(const char *msg, const char *arg)
#if defined(__GNUC__)
    __attribute__((noreturn))
#endif
    ;
/* The layered config into o: flags win; an unknown model or setting exits. */
void apply_config(opts *o, cli_config *cfg, bool create);
/* Say on stderr when the model takes another effort level than asked. */
void effort_note(const char *ref, pt_reasoning want);

/* print.c: run until idle (also pytte approve); the exit code */
int pytte_print(opts *o, const char *id, cli_config *cfg);

/* commands.c */
int pytte_log(const opts *o);
int pytte_ls(void);
/* pytte rewind: the files and the conversation back to before a message */
int pytte_rewind(opts *o, cli_config *cfg);
int pytte_update_catalog(void);
int pytte_mcp(opts *o);
/* This directory's servers into o, warning about those waiting for trust;
 * NULL with --no-mcp. The set lives as long as the process. */
cli_mcp *load_mcp(opts *o, bool quiet);

#endif
