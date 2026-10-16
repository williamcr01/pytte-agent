# Logging in

pytte talks to a provider with either an **API key**, billed per token by
the provider, or a **subscription login**: signing in with an account you
already pay for. `/login` in the TUI does both. A third kind saves the API keys of the search
services behind `web_search`. Today the one subscription
is ChatGPT (Plus, Pro and the other plans with Codex), as the provider
`openai-codex`; `openai` stays the API with an API key.

## Configuration

Both kinds are saved to `~/.config/pytte/auth.json` (`$XDG_CONFIG_HOME/pytte`
when set), created with mode 0600, by provider:

```json
{
  "anthropic": {"key": "sk-ant-..."},
  "search:exa": {"key": "..."},
  "openai-codex": {"access": "eyJ...", "refresh": "...", "expires": 1760000000000, "account_id": "..."}
}
```

| Member | Meaning |
|---|---|
| `key` | an API key saved by `/login api`, or a search key saved by `/login search` (under `search:exa` or `search:parallel`, so a provider named `exa` does not share the key) |
| `access` | a login's access token (sent as a bearer token) |
| `refresh` | the token that gets a new access token |
| `expires` | when the access token expires, in milliseconds since the epoch |
| `account_id` | `openai-codex`: the ChatGPT account the token is for |

An API key in the environment (`ANTHROPIC_API_KEY`, `OPENAI_API_KEY`,
`OPENROUTER_API_KEY`, or a configured provider's `key_env`) comes before a
saved one. A subscription login has no environment variable. A search key
likewise: `EXA_API_KEY` or `PARALLEL_API_KEY` comes before a saved one.

## Behavior

`/login` opens a menu of three kinds:

- **`/login api <provider> [key]`**: picking a provider puts
  `/login api <provider> ` in the input, ready for the key. The key is shown
  masked and never reaches the input history or a saved draft. `/login api`
  alone shows where each provider's key comes from.
- **`/login oauth <provider>`**: signs in in the browser. `/login oauth`
  alone shows whether each login is there.
- **`/login search <exa|parallel> [key]`** ("Search API keys" in the menu):
  the same as `api`, for the key of a search service. The key is masked and
  kept out of the input history, saved to `auth.json` under the service's
  name, and one in the environment comes first. `/login search` alone shows
  where each key comes from. After a key is saved, pytte asks `Turn on
  web_search and web_fetch? (y/n)` (unless both are on already). `y` turns
  both on like `/tools` does ([tools.md](tools.md)) and saves
  `tools.web_search` and `tools.web_fetch` to the user config; `n`, or
  anything else typed, leaves them as they are. See [web.md](web.md) for
  the tool.

`/login` with nothing else (the menu closed with Esc) shows all three.

### Sign in with ChatGPT (`openai-codex`)

`/login oauth openai-codex` opens the browser on OpenAI's sign-in page (the
address is printed too, to open by hand) and waits up to 5 minutes. After you
sign in, the browser is sent back to `http://localhost:1455/auth/callback`,
where pytte listens; the page says you can close it, and pytte saves the
login. It is the same flow, port and client as the Codex CLI.

When the browser cannot reach pytte (pytte runs on another machine over ssh,
or port 1455 is taken, for example by the Codex CLI logging in), the
browser ends on a page that does not load. Copy that page's address and
paste it after `/login oauth openai-codex `; it is masked like a key. A
second `/login oauth openai-codex` starts over; quitting pytte cancels a
login that is still waiting.

The access token is refreshed when a request starts within 5 minutes of it
expiring. That request waits for the refresh (up to 30 seconds). Before
refreshing, pytte reads `auth.json` again, so a refresh made by another
pytte is used rather than repeated. When the refresh fails and the token
has expired, the run stops with "not logged in to openai-codex, or the login
expired: run /login oauth openai-codex".

What is sent: the access token and account id, to `chatgpt.com` with each
request; the refresh token, to `auth.openai.com` when refreshing. Nothing
else leaves the machine.

### The `openai-codex` models

Requests go to ChatGPT's Codex backend
(`https://chatgpt.com/backend-api/codex/responses`), which counts them
against the subscription's usage limits, not an API bill. When a limit is
hit, the run stops with "you have hit your ChatGPT usage limit" instead of
retrying.

No source lists the models a subscription serves, so the catalog has an
explicit list (like pi's): `gpt-5.3-codex-spark`, `gpt-5.5`,
`gpt-5.6-luna`, `gpt-5.6-sol`, `gpt-5.6-terra`, `gpt-6-astra`,
`gpt-6-luna`, `gpt-6-sol` and `gpt-6.1-sol`. Their details (reasoning
levels, images, prices) come from models.dev's `openai` model of the same
id, refreshed by `pytte update-catalog`; their context window is the
backend's (272,000 tokens; 128,000 for `gpt-5.3-codex-spark`), with up to
128,000 tokens of output. The prices shown by `/usage` are what the API
would charge, for comparison.

`pytte-ai` cannot use `openai-codex`: it has no logins.

## Examples

Use a ChatGPT subscription:

```text
/login oauth openai-codex
/model openai-codex/gpt-5.5
```

or start with it: `pytte -m openai-codex/gpt-6-sol`.

Save an Exa key and turn on the web tools:

```text
/login search exa ...
y
```

Save an OpenAI API key instead:

```text
/login api openai sk-...
```
