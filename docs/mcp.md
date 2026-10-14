# MCP servers

MCP servers (the Model Context Protocol) give the model more tools, and
give you prompts and resources to use in messages. A server is either a
program pytte starts (stdio) or a remote URL (Streamable HTTP).

## Configuration

Servers go in `mcpServers` of `~/.config/pytte/config.json`, by name
(1 to 32 letters, digits, `-` and `_`):

```json
{
  "mcpServers": {
    "github": {"command": "npx", "args": ["-y", "@modelcontextprotocol/server-github"],
               "env": {"GITHUB_PERSONAL_ACCESS_TOKEN": "${GITHUB_TOKEN}"}},
    "linear": {"url": "https://mcp.linear.app/mcp"}
  }
}
```

| Key | Type | Default | Meaning |
|---|---|---|---|
| `command` | string | | stdio: the program, found on `PATH` |
| `args` | array of strings | `[]` | stdio: its arguments |
| `env` | object | `{}` | stdio: variables added to its environment |
| `cwd` | string | the session's directory | stdio: where it runs; relative to the project for a project's server |
| `url` | string | | remote: the server's Streamable HTTP endpoint (`http://` or `https://`) |
| `headers` | object | `{}` | remote: headers sent with every request |
| `type` | `"stdio"` or `"http"` | from `command`/`url` | the transport; the old `sse` one is not supported |
| `timeout` | seconds | 600 | one tool call |
| `startup_timeout` | seconds | 15 | starting and listing its tools |
| `disabled` | bool | false | skip this server |
| `trust_hints` | bool | false | let the server's `readOnlyHint` make a tool read-only (otherwise every MCP tool asks) |
| `client_id`, `client_secret`, `scope` | string | | OAuth: a registered client and the scope to ask for (also accepted inside `"oauth": {...}`) |
| `arguments` | object | | tool arguments pytte fills in: see below |

`${VAR}` and `${VAR:-default}` in strings take the environment's values
when the config is read. The provider API keys pytte knows are removed
from a stdio server's environment unless its `env` sets them.

### Project servers

A project's servers, in `.mcp.json` or `mcpServers` of
`.pytte/config.json`, start programs, so they run only once trusted. The
TUI says which there are, and `/mcp trust` (or `pytte mcp trust`) runs
them. Trust is kept in `~/.config/pytte/trust.json` with a hash of the
project's server config; any change asks again. A trusted project server
replaces a user server of the same name.

### Filled-in arguments

Some tools take arguments that pytte, not the model, should supply: which
model is calling, a stable session ID, a fixed option. `arguments` sets
them, by the server's tool name, or `*` for every tool of the server:

```json
"parallel": {
  "url": "https://search.parallel.ai/mcp",
  "arguments": {
    "*":         {"model_name": "{model_id}", "session_id": "pytte-{session}"},
    "web_fetch": {"full_content": true}
  }
}
```

- The model never sees them: they are removed from the tool's schema
  (`properties` and `required`) before it is offered.
- Each call gets them, in place of anything the model sent under the same
  name.
- A tool's own entry always applies. An `*` argument applies only to the
  tools whose schema has it, and a tool's own entry overrides it.
- Values are any JSON. In strings these are replaced at each call:

  | Placeholder | Becomes | Example |
  |---|---|---|
  | `{model}` | the model making the call, as pytte names it | `openrouter/deepseek/deepseek-v4.1-flash` |
  | `{model_id}` | the same without the provider | `deepseek/deepseek-v4.1-flash` |
  | `{session}` | the session's ID | `20261013-142233-ab12` |

  The model follows the session: after `/model`, calls send the new one.
  Anything else in braces is sent as written. `${VAR}` is replaced once,
  when the config is read.
- The session log records the call as the model made it; the filled-in
  values go only to the server.

## Behavior

Servers start with the session, in the background. A new session's first
request waits for them to list their tools (up to `startup_timeout`), so a
prompt sent at once shows as sent and goes a moment later. Their tools are
named `mcp__<server>__<tool>`. A server that answers the tool listing with
an error (a rate limit, say) runs with no tools, and the error is shown with
it and in `/mcp`. A server that dies starts again at the next
call of its tools; one whose tool list changes is listed again, and the
new tools are offered from the next epoch.

Every MCP tool asks for permission in `ask` mode (unless `trust_hints` and
the server marks it read-only), runs in `auto`, and is refused in
`read-only` unless read-only.

- **Prompts** are slash commands, `/<server>:<prompt> [args]` (arguments
  in order, the last taking the rest, or `name=value`), and send the
  prompt's text as the message.
- **Resources**: `@<server>:<uri>` anywhere in a message adds that
  resource's contents; typing `@` lists them.
- **OAuth**: remote servers that ask for a login get one (discovery,
  dynamic client registration or the configured client, PKCE, and a
  browser that answers to 127.0.0.1): `/mcp login <name>` or
  `pytte mcp login <name>`. Tokens are kept in
  `~/.config/pytte/mcp-auth.json` (0600) and refreshed when they expire.

Commands: `/mcp` (or `pytte mcp list`) shows each server's state and
tools; `/mcp restart`, `/mcp logout` and `/mcp untrust` do what they say;
`--no-mcp` starts none.

## Examples

A remote server with an API key from the environment:

```json
"search": {"url": "https://mcp.example.com/mcp",
           "headers": {"Authorization": "Bearer ${EXAMPLE_KEY}"}}
```

A local server whose tools are all read-only by its own account:

```json
"docs": {"command": "docs-mcp", "args": ["--root", "."], "trust_hints": true}
```
