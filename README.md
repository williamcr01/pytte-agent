# pytte

A tiny, durable, embeddable coding agent in C. The goal is an agent that can
run on almost anything (a laptop, a router, a microcontroller, a browser) and
survive being killed at any point.

Status: **M2, the TUI.** The AI layer (`libpytte-ai`) talks to Anthropic,
OpenAI and OpenRouter. The core (`libpytte`) commits every step to a session
log before it happens, so a session survives `kill -9` at any point and
`pytte -c` carries on. `pytte` is an inline terminal UI that redraws only a
few rows at the bottom and leaves the rest to the terminal's scrollback.
Tools come next (M3).

## Build

Needs a C99 compiler, make, and libcurl with headers.

```sh
make            # build/pytte, build/pytte-ai and the libraries
make test       # unit, fixture, crash-simulation, screen, pty and CLI tests (ASan, UBSan)
make screen     # the screen property test with many more random sessions
make crash      # kill -9 pytte at random moments, 100 rounds
make portable   # the core builds as strict C99 with gcc and clang, no stdio
make fuzz       # libFuzzer targets in build/fuzz/ (clang)
make models     # refresh the model catalog (network, needs python3)
make width      # regenerate the display-width table from Unicode (network)
```

## pytte

```sh
export ANTHROPIC_API_KEY=...   # or OPENAI_API_KEY, OPENROUTER_API_KEY

build/pytte                                  # the TUI, new session (model: anthropic)
build/pytte -m openai/gpt-5 --effort high    # another model
build/pytte -c                               # continue this directory's newest session
build/pytte -r [ID]                          # that session, or pick one
build/pytte -p "what is 25*18?"              # print mode: run, print the answer, exit
echo "hello" | build/pytte -p --output-format json
build/pytte log [ID]                         # a transcript (--json: records)
build/pytte -m faux --script tests/faux/slow.json   # offline
```

In the TUI: Enter sends (queued while a run is active), Shift+Enter or
Ctrl-J adds a newline, Esc Esc interrupts, Ctrl-C clears the draft (twice
on an empty one quits), Ctrl-D quits, Ctrl-T cycles how thinking is shown,
Ctrl-O opens the full transcript in a pager, Ctrl-Z suspends. `/` opens the
command menu: `/model`, `/effort`, `/new`, `/resume`, `/status`,
`/thinking`, `/settings`, `/help`, `/quit`. Settings are saved to
`~/.config/pytte/config.json`.

Sessions live in `$XDG_STATE_HOME/pytte/sessions/<id>/` (by default
`~/.local/state/pytte`). `log.jsonl` is the whole truth: one commit per line,
readable with `jq`. Kill the process at any point and `pytte -c` continues
from the last commit: a half-streamed answer is kept as an interrupted entry
and the request is sent again. A watchdog process restores the terminal
modes even after `kill -9` (run `reset` if keys still misbehave). Exit codes:
0 done, 1 setup (such as a missing key), 10 the run failed, 20 storage
error, 130 detached (Ctrl-C in print mode).

Some terminal settings: `PYTTE_AMBIGUOUS=wide` for terminals set to draw
East Asian ambiguous characters wide, `PYTTE_REFLOW=0/1` if pytte guesses
wrong whether the terminal rewraps lines on resize, `PYTTE_ASCII=1` for
ASCII symbols, `NO_COLOR`. `PYTTE_TUI_TRACE=<file>` records every byte read
and written, for replaying a display bug.

There are no tools yet: when a model calls one, the call gets an error
result and the run goes on.

## pytte-ai

```sh
export ANTHROPIC_API_KEY=...   # or OPENAI_API_KEY, OPENROUTER_API_KEY

build/pytte-ai anthropic "what is 25*18?"
build/pytte-ai openai/gpt-5 --system "be brief" "hello"
build/pytte-ai openrouter/<vendor>/<model> --reasoning high "hello"
build/pytte-ai anthropic --tools tests/tools/echo.json --turns 2 "echo hi"
build/pytte-ai faux --script tests/faux/tool_call.json --turns 2 "anything"
build/pytte-ai --list
```

Text goes to stdout; thinking, tool calls and a usage summary go to stderr.
`--events` prints the unified event stream as JSON lines instead.

## Layout

```
include/pytte_base.h   arena, strings, buffers, JSON, SSE, CRC-32
include/pytte_ai.h     the AI layer: messages, providers, models, streams
include/pytte.h        the core: a sans-I/O durable task interpreter
src/base/              portable C99: no stdio, no malloc, no floating point
src/ai/                APIs (anthropic-messages, openai-responses for OpenAI,
                       openai-completions for OpenRouter and other compatible
                       servers, faux), providers and models as data,
                       cross-provider transforms
src/core/              the core: commit lines, state, scheduler, the run task
src/rt/                the runtime: the host loop over stores, HTTP and timers
src/drv/               host drivers: libc, curl HTTP, faux HTTP, posix and
                       memory session stores
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
