# pytte

A tiny, durable, embeddable coding agent in C. The goal is an agent that can
run on almost anything (a laptop, a router, a microcontroller, a browser) and
survive being killed at any point.

Status: **M0, providers.** The AI layer (`libpytte-ai`) talks to Anthropic,
OpenAI and OpenRouter, and `pytte-ai` is a small CLI on top of it. The
durable agent comes next.

## Build

Needs a C99 compiler, make, and libcurl with headers.

```sh
make            # build/pytte-ai and build/libpytte-ai.a
make test       # unit, fixture and CLI tests, with ASan and UBSan
make portable   # the core builds as strict C99 with gcc and clang, no stdio
make fuzz       # libFuzzer targets in build/fuzz/ (clang)
```

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
include/pytte_base.h   arena, strings, buffers, JSON, SSE
include/pytte_ai.h     the AI layer: messages, providers, models, streams
src/base/              portable C99: no stdio, no malloc, no floating point
src/ai/                APIs (anthropic-messages, openai-responses for OpenAI,
                       openai-completions for OpenRouter and other compatible
                       servers, faux), providers and models as data,
                       cross-provider transforms
src/drv/               host drivers: libc, curl HTTP, faux HTTP
src/cli/pytte_ai.c     the pytte-ai CLI
tests/fixtures/        response streams per provider, with golden results
tests/faux/            faux provider scripts
```

## Fixtures

Every `tests/fixtures/<provider>/*.sse` file is a response (one JSON header
line, then the raw body). The tests replay each one at several chunk sizes and
compare the result with its `.expect.json` golden file.

The current fixtures are synthetic, written to match each wire format. To add
real recordings (costs a little money):

```sh
tests/record.sh anthropic openai openrouter
make fixtures-update   # writes the golden files; review the diff
```

`--record FILE.sse` on any `pytte-ai` call saves that response the same way.
API keys never reach a fixture.
