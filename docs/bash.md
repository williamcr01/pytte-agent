# Shell commands and background sessions

The `bash` tool runs a command line in your shell, in the session's
directory, and gives the model its output. A command that runs long keeps
going in the background as a *session*. pytte tells the model when a
session ends and shows it what the session printed, so the model can do
other work or end its turn instead of polling. `bash_poll` checks on a
session or stops it.

## Configuration

There are no config keys. The model sets these per call:

| Tool | Argument | Default | Meaning |
|---|---|---|---|
| `bash` | `command` | (required) | The command line, run with `$SHELL -c` (bash, zsh, sh, dash or ksh; otherwise `/bin/sh -c`) |
| `bash` | `timeout` | none | Seconds before the command is stopped (SIGTERM, then SIGKILL 2 s later) |
| `bash` | `yield` | 120 | Seconds to wait before the call returns and the command moves to the background. `0` waits for the command to end |
| `bash_poll` | `session` | (required) | The session to read |
| `bash_poll` | `stop` | false | Stop the session (SIGTERM, then SIGKILL 2 s later) |

## Behavior

- **Each call starts fresh.** Commands start in the working directory,
  which the system prompt names. A `cd` or `export` inside one call
  doesn't carry over to the next, and stdin is empty, so a command cannot
  stop to ask for input.
- **Going to the background.** If the command is still running after
  `yield` seconds, the call returns the output so far followed by
  `[still running as session N; you will be told when it ends]`. At most
  16 sessions run in the background at once. Past that, a call waits for
  its command to end.
- **Output kept.** pytte keeps a background session's newest 1 MiB of
  output and counts what it drops. `bash_poll` returns at once, without
  waiting: what was printed since the last read, how many bytes were
  dropped, and whether the session still runs (`[still running as session
  N; you will be told when it ends]`) or how it ended.
- **When a session ends while the model is working:** the next tool
  result of any tool gets a line like
  `[pytte: background session 8 ended with exit code 0: make test; bash_poll reads its output]`.
- **When a session ends while the agent is idle:** pytte sends one note
  that starts a turn. The note lists the sessions that ended, each with its
  exit code and command, and what each one printed inside an
  `<output>` block. If the session printed more than 8 KiB, the note
  carries the last 8 KiB, starting at a whole line, and `bash_poll` can
  read the rest. A session shown in full is closed, so a later
  `bash_poll` for it reports `No session N`.
- **No repeats.** Each session's end is reported once. Sessions you stop
  with `/stop`, or that the model stops with `bash_poll stop`, are not
  reported.
- **The note is not something you typed.** It is stored as an input from
  pytte (`"from":"pytte"` in the log). The TUI shows only its
  `· session N ended …` lines. It is committed like any input, so a crash
  neither loses it nor sends it twice.
- **Print mode (`-p`)** keeps running while a background session runs,
  so the note can wake the model as it would in the TUI. It exits once
  the agent is idle and no session runs. If the model leaves a server
  running, `-p` waits until the server ends. Ctrl-C detaches, and
  `pytte -r ID` continues the session.
- **Sessions live in this process.** They are stopped when pytte exits,
  and session numbers are never reused after a restart. If pytte is
  killed (`kill -9`, the OOM killer) its commands go on running until
  the session is opened again (`pytte -c`, `-r`), which stops whatever
  they left, before any new call runs. See [sessions.md](sessions.md).
- **TUI.** The footer counts running sessions. `/ps` lists them and
  `/stop` stops them all.

## Examples

A test run that takes a few minutes. The call returns after 120 seconds,
the model works on, and the result comes later:

```json
{"name": "bash", "args": {"command": "make test"}}
```

A dev server that should keep running. The call returns after 2 seconds
with the startup output:

```json
{"name": "bash", "args": {"command": "npm run dev", "yield": 2}}
```

Stopping it:

```json
{"name": "bash_poll", "args": {"session": 12, "stop": true}}
```
