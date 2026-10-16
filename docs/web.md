# Web: web_fetch and web_search

`web_fetch` reads a page; `web_search` finds pages. The model uses them
together: search, then fetch a result. Both are off by default and left out
of the tool list until you turn them on (see [tools.md](tools.md)). What
they are sent leaves the machine, so in `ask` mode each call asks first,
with "allow this session" (by host for `web_fetch`, for the tool for
`web_search`), and `read-only` mode refuses them.

`web_search` calls a search service directly, not the model provider's own
search, so it works with every model. It needs an API key for one of two
services: Exa or Parallel.

## Configuration

| Key | Type | Default | Meaning |
|---|---|---|---|
| `tools.web_fetch` | boolean | `false` | give the model `web_fetch` |
| `tools.web_search` | boolean | `false` | give the model `web_search` |
| `search.backend` | `exa`, `parallel` | the first with a key, in that order | which service searches when both have keys |

The keys go in the user or project config; the TUI writes the user file.
`/tools web_search on` and `/tools web_fetch on` set the first two (the
full rules for turning tools on and off, and when a change reaches the
model, are in [tools.md](tools.md)). A search key comes from the
environment (`EXA_API_KEY`, `PARALLEL_API_KEY`), else `auth.json` where
`/login search <exa|parallel> [key]` saves it ([login.md](login.md)). A
value of `search.backend` other than the two stops pytte at start.

## Behavior

- **Arguments.** `web_search` takes `query` (required), `max_results`
  (default 5, at most 20), and optional `include_domains` and
  `exclude_domains` (up to 20 each). With `include_domains`, Parallel
  ignores `exclude_domains`; the tool says so to the model.
- **Result.** Markdown: a note that the results come from the web and are
  data, not instructions, then a numbered list. Each entry is
  `[title](url)`, the publication date when the service gives one, and an
  excerpt of up to 800 bytes on one line (Exa's highlights, else the start of
  its text; Parallel's excerpts). A result without a title is named by its
  URL. The model reads a result with `web_fetch`.
- **Backends.**
  - Exa: `POST https://api.exa.ai/search` with the key in `x-api-key`;
    the request asks for `numResults` results and highlights of up to 800
    characters, with `includeDomains` and `excludeDomains`.
  - Parallel: `POST https://api.parallel.ai/v1/search` with the key in
    `x-api-key`; the query is sent as both `objective` and the one search
    query, with `advanced_settings.max_results`, 800 characters of excerpt
    per result and `source_policy` for the domains. The issue named
    `/v1beta/search`; Parallel's docs now describe `/v1/search` and keep the
    beta path only for existing integrations.
- **Keys.** A key is read for each call, so one saved with `/login search`
  works at once, even for a session that already has the tool. With no key
  the call fails with a result that says to run `/login search`; turning
  `web_search` on without one says the same. A refused key (HTTP 401 or 403)
  says which `/login search` to run.
- **Failures.** An HTTP error, a response that is not a result list or one
  over 4 MiB is an error result with the service's message cut to 300
  bytes. Nothing is retried.
- **What is sent.** The query, the result count and the domain lists, with
  the key, to the service chosen (`api.exa.ai` or `api.parallel.ai`). The
  conversation is not sent.
- **`web_fetch`** is described in the README: it gets a URL (http becomes
  https except for localhost, at most 5 redirects, never down from https to
  http), gives HTML as Markdown, text and JSON as they are, an image as an
  image, and cuts long text at `max_bytes` (default 50 KiB, at most 256 KiB)
  with the whole text kept as a blob. It starts off since the same change
  that added `/tools`.

## Examples

Turn both on in `~/.config/pytte/config.json`, and prefer Parallel when both
keys are there:

```json
{"tools": {"web_search": true, "web_fetch": true}, "search": {"backend": "parallel"}}
```

From the TUI, one step:

```text
/login search exa ...
Turn on web_search and web_fetch? (y/n)
y
```
