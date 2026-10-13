# pytte

A tiny, durable, embeddable coding agent in C. The goal is an agent that can
run on almost anything (a laptop, a router, a microcontroller, a browser) and
survive being killed at any point.

Status: **M5 under way: prompt files and skills.** The AI layer (`libpytte-ai`) talks to Anthropic,
OpenAI and OpenRouter. The core (`libpytte`) commits every step to a session
log before it happens, so a session survives `kill -9` at any point and
`pytte -c` carries on. `pytte` is an inline terminal UI that redraws only a
few rows at the bottom and leaves the rest to the terminal's scrollback.
The model works with eight tools (bash, bash_poll, read, write, edit, grep,
find, ls);
each call is a durable task, so a crash never runs an unsafe call twice.
When the conversation nears the model's context window, pytte compacts it
into a summary and carries on, so sessions outlive any one window.

## Build

Needs a C99 compiler, make, and libcurl 7.84+ with headers.

```sh
make            # build/pytte, build/pytte-ai and the libraries
make release    # optimized, stripped binaries in build/release/
make test       # unit, fixture, crash-simulation, screen, pty and CLI tests (ASan, UBSan)
make screen     # the screen property test with many more random sessions
make pty-asan   # the pty test against pytte built with ASan and UBSan
make crash      # kill -9 pytte at random moments, 100 rounds
make portable   # the core builds as strict C99 with gcc and clang, no stdio
make fuzz       # libFuzzer targets in build/fuzz/ (clang)
make bench      # offline latency/allocation workloads, optimized without sanitizers
make models     # refresh the model catalog (network, needs python3)
make width      # regenerate the display-width table from Unicode (network)
```

The default build loads the system libcurl when the first real HTTP request
starts. Offline sessions and terminal startup do not load its TLS dependency
tree. Use `make CURL_LINK=direct BUILD=build/direct` for a custom or static
libcurl. See [PERFORMANCE.md](PERFORMANCE.md) for reproducible measurements.

For distribution, `make release` builds the native 64-bit executable
`build/release/pytte` with system curl/TLS. It bundles neither library.

With OpenSSL, pytte prefers a hashed system certificate directory so only
needed roots are loaded. It uses curl's configured directory, or
`/etc/ssl/certs` for the conventional `/etc/ssl/certs/ca-certificates.crt` and
`/etc/ssl/cert.pem` system bundles. Explicit `SSL_CERT_FILE` overrides and
proxy configurations retain the bundle; `SSL_CERT_DIR` chooses a directory.
The paths are captured when the HTTP driver is created. Certificate and
hostname verification remain enabled, with a bundle fallback before any
request bytes are sent. Other TLS backends keep their normal trust handling.

## pytte

```sh
export ANTHROPIC_API_KEY=...   # or OPENAI_API_KEY, OPENROUTER_API_KEY; or /login in the TUI

build/pytte                                  # the TUI, new session (model: anthropic/claude-opus-5-5)
build/pytte -m openai/gpt-5 --effort high    # another model
build/pytte -c                               # continue this directory's newest session
build/pytte -r [ID]                          # that session, or pick one
build/pytte -p "what is 25*18?"              # print mode: run, print the answer, exit
echo "hello" | build/pytte -p --output-format json
build/pytte log [ID]                         # a transcript (--json: records,
                                             #   --all-epochs: before compactions too)
build/pytte ls                               # every session: status, directory, name
build/pytte --permission-mode auto           # run every tool call without asking
build/pytte approve ID TASK yes              # answer a call -p left waiting (exit 3)
build/pytte -m faux/faux --script tests/faux/slow.json   # offline
```

In the TUI: Enter sends (queued while a run is active), Shift+Enter or
Ctrl-J adds a newline, Esc Esc interrupts, Ctrl-C clears the draft (twice
on an empty one quits), Ctrl-D quits, Ctrl-T cycles how thinking is shown,
Ctrl-O opens the full transcript in a pager, Ctrl-Z suspends. `/` opens the
command menu: `/model`, `/effort`, `/new`, `/resume`, `/fork`, `/worktree`,
`/status`, `/thinking`, `/compact`, `/ps`, `/stop`, `/usage`, `/settings`, `/login`, `/help`, `/quit`. Settings are saved to
`~/.config/pytte/config.json`.

A session is named after the first line of its first prompt; `/resume`,
`pytte -r` and `pytte ls` show the name, and `/resume <words>` finds
sessions by it. `/fork` continues in a copy of the session (the original
stays as it was). `/worktree [name]` (or `/wt`) starts a new session in a
git worktree at `<repo>/.pytte/worktrees/<name>`, on a new branch
`worktree-<name>`; the same name later opens that worktree again, and
`/resume` lists the worktrees' sessions with the repository's. `pytte -w
[name]` (or `--worktree`) starts in one; `--worktree=<name>` or `--` keeps
a one-word prompt from being taken for the name.

`/login <provider>` asks for an API key (shown masked, kept out of the input
history) and saves it to `~/.config/pytte/auth.json` (mode 0600), so no
exported variable is needed. A key in the environment still comes first;
`/login` alone shows where each provider's key comes from.

The footer shows the selected model, effort and context use of the model's
window (`12k/200k`). It counts what the next request sends: the provider's
count for the newest answer plus an estimate of what came since, so a fresh
session already shows its system prompt, `AGENTS.md` files, skills index and
tools. Compaction goes by the same number. Effort starts at `medium`;
`/effort <level>` changes it for this session, and `/settings reasoning <level>`
changes it now and saves it for future sessions. The levels are `off`,
`minimal`, `low`, `medium`, `high`, `xhigh`, and `max`. `off` requests disabled
reasoning where the model supports it. `/quit` prints a dimmed `pytte -r <id>`
command and keeps the session available to resume, even if it has no messages.

Sessions live in `$XDG_STATE_HOME/pytte/sessions/<id>/` (by default
`~/.local/state/pytte`). `log.jsonl` is the whole truth: one commit per line,
readable with `jq`. Kill the process at any point and `pytte -c` continues
from the last commit: a half-streamed answer is kept as an interrupted entry
and the request is sent again. A watchdog process restores the terminal
modes even after `kill -9` (run `reset` if keys still misbehave). Exit codes:
0 done, 1 setup (such as a missing key), 3 a tool call waits for approval
(print mode), 10 the run failed, 20 storage error, 130 detached (Ctrl-C in
print mode).

Some terminal settings: `PYTTE_AMBIGUOUS=wide` for terminals set to draw
East Asian ambiguous characters wide, `PYTTE_REFLOW=0/1` if pytte guesses
wrong whether the terminal rewraps lines on resize, `PYTTE_ASCII=1` for
ASCII symbols, `NO_COLOR`. `PYTTE_TUI_TRACE=<file>` records every byte read
and written, for replaying a display bug.

Prompt files and skills: `SYSTEM.md` replaces the built-in system prompt and
`APPEND_SYSTEM.md` adds to it; each is read from `<project>/.pytte/`, else
`~/.config/pytte/`. Skills are `skills/<name>/SKILL.md` (frontmatter `name`
and `description`) in `~/.agents/`, `~/.config/pytte/`, `<project>/.agents/`
or `<project>/.pytte/`; a project skill replaces a user skill of the same
name. The model sees each skill's name, description and path, and reads the
file when a task matches (`skills.auto` off, or `/settings skills off`, stops
that). `$name` anywhere in a message, or `/skill:name ...`, sends the skill's
instructions with it; typing `$` lists the skills. `AGENTS.md` files are
added too: `~/.config/pytte/AGENTS.md`, then one from each directory from the
git root down to the working directory (`CLAUDE.md` where a directory has
none), up to 32 KiB, the outermost cut first. The files are read when a
session starts and again at each compaction.

Tools: `bash` runs a command line in its own process group (stdin empty,
stdout and stderr together, an optional timeout). A command still running
after `yield` seconds (default 10) goes on in the background as a session,
and the model works on; `bash_poll` reads what it printed since, waits for
it to end (at least 5 seconds per poll), or stops it. The footer counts
the ones still running, `/ps` lists them and `/stop` stops them; they
end when pytte does. When one ends and nothing has read it by the time
the model is idle, pytte starts a turn with a note saying so (shown as
`· session 8 ended with exit code 0: make test`), and the model reads it
and carries on; a command the user stopped with `/stop` gets no note. `read` shows a file with
line numbers, up to 2000 lines or 50 KiB, cut at a whole line, and says
where to read on; an image (PNG, JPEG, GIF, WebP, up to 3.75 MiB and 8000
pixels a side) goes to the model as an image, or as a note to a model that
takes none, its bytes kept as a blob. `write` replaces a whole file
atomically. `edit` makes one or more replacements in one call, each
matched against the file as it was: each must match exactly once (or set
`all`), they must not overlap, and it refuses when the file changed since
the model last read it. `grep` (POSIX extended regular expressions, with
`glob`, `context`, `ignore_case`, `literal` and `files_only`), `find`
(globs; `**` crosses directories) and `ls` run in pytte itself: no shell,
ripgrep or fd. `grep` and `find` skip `.git` and what `.gitignore` files
ignore, from the repository's root down. Each result is bounded (matches,
entries, 50 KiB) and says when it was cut. For `bash`, the model sees up
to 16 KB of output: the first 4 KB and the last 12 KB. Longer output is kept whole in the session's
`blobs/` (content-addressed, SHA-256), and the cut result names that file
so the model can `read` or `grep` it. A prompt of 16 KB or more (a big paste) is
stored the same way, once, and the log refers to it. Arguments are checked against each tool's JSON
schema before anything runs.

Permissions (`--permission-mode`, `$PYTTE_PERMISSION_MODE` or
`permissions.mode` in the config): `ask` (the default) runs `read`, `grep`,
`find`, `ls` and `bash_poll` freely and asks before `bash`, `write` and
`edit`. The TUI asks inline: 1 yes,
2 yes and allow the same command word (or tool) for this session, 3 or Esc
no. Allowing `git` never allows `git status; rm -rf x`. `auto` runs
everything. `read-only` runs only the tools that change nothing (`read`,
`grep`, `find` and `ls`) and refuses the rest, `bash_poll` (which can
stop a command) and allowed tools included. In print mode
a question ends the process with exit code 3 and prints the
`pytte approve` command that answers it.

Long sessions: before a request that would not fit the context window
(less 24k tokens, or a quarter of a small window), or after the provider
says the prompt is too long, the model writes a summary of the
conversation so far and a new epoch starts from it, followed by the newest
turns as they were (about 20k tokens). The summary has fixed sections (goal,
progress, decisions, next steps, ...), is updated rather than rewritten at the
next compaction, and is followed by the files read and modified, which code
keeps. It is written with low effort by the session's model, or by
`--compact-model REF` (`$PYTTE_COMPACT_MODEL`, `compaction.model` in the config,
`/settings compact-model`). The log keeps everything: `pytte log --all-epochs` shows
the earlier epochs. `/compact` compacts now. `--context-window N` pretends
the window is N tokens, which is handy for trying it. Token usage per model
is recorded with each answer; `/usage` shows it with an estimated cost.

`pytte ls` shows each session as running (with the owner's pid), idle,
waiting (a tool call needs approval), closed (detached with a run
unfinished; resuming continues it) or crashed (its owner died without
closing it; `pytte -r ID` recovers it).

Recovery after a crash follows each tool's replay policy. `read`, `grep`,
`find`, `ls` and `write` run again. A `bash` or `edit` call that was cut off is not repeated;
the model gets an "interrupted" result and decides what to do.

## pytte-ai

```sh
export ANTHROPIC_API_KEY=...   # or OPENAI_API_KEY, OPENROUTER_API_KEY

build/pytte-ai anthropic/claude-opus-5-5 "what is 25*18?"
build/pytte-ai openai/gpt-5 --system "be brief" "hello"
build/pytte-ai openrouter/<vendor>/<model> --reasoning high "hello"
build/pytte-ai anthropic/claude-opus-5-5 --tools tests/tools/echo.json --turns 2 "echo hi"
build/pytte-ai faux/faux --script tests/faux/tool_call.json --turns 2 "anything"
build/pytte-ai --list
```

Text goes to stdout; thinking, tool calls and a usage summary go to stderr.
`--events` prints the unified event stream as JSON lines instead.

## Layout

```
include/pytte_base.h   arena, strings, buffers, JSON, SSE, CRC-32, SHA-256,
                       a JSON Schema subset
include/pytte_ai.h     the AI layer: messages, providers, models, streams
include/pytte.h        the core: a sans-I/O durable task interpreter
src/base/              portable C99: no stdio, no malloc, no floating point
src/ai/                APIs (anthropic-messages, openai-responses for OpenAI,
                       openai-completions for OpenRouter and other compatible
                       servers, faux), providers and models as data,
                       cross-provider transforms
src/core/              the core: commit lines, state, scheduler, the run,
                       tool tasks and compaction
src/rt/                the runtime: the host loop over stores, HTTP, timers
                       and fds; the tool registry, the permission gate and
                       the builtin tools
src/drv/               host drivers: libc, curl HTTP, faux HTTP, posix and
                       memory session stores, processes
src/tui/               the TUI: terminal, keys, text and widths, Markdown,
                       the band, the input box, the event loop (app.c)
src/ai/models.gen.inc  the model catalog, generated from models.dev and
                       OpenRouter by scripts/gen_models.py
src/cli/               the pytte CLI: flags, print mode, sessions, config files
src/cli/pytte_ai.c     the pytte-ai CLI
tests/fixtures/        response streams per provider, with golden results
tests/faux/            faux provider scripts
tests/test_sim.c       crash simulation: every scenario killed at every commit
tests/crash.sh         the same with real processes and kill -9
tests/vt.c             a strict terminal emulator for the TUI tests
tests/test_screen.c    random sessions checked against the emulator's screen
tests/test_pty.c       the real pytte in a pseudo-terminal
```

## Fixtures

Every `tests/fixtures/<provider>/*.sse` file is a response (one JSON header
line, then the raw body). The tests replay each one at several chunk sizes and
compare the result with its `.expect.json` golden file.

`live_*` fixtures are real recordings; the rest are synthetic, written to match
each wire format and cover the edge cases. To record again (costs a little money):

```sh
tests/record.sh anthropic openai openrouter
make fixtures-update   # writes the golden files; review the diff
```

`--record FILE.sse` on any `pytte-ai` call saves that response the same way.
API keys never reach a fixture.
