# pytte documentation

Reference for how pytte behaves: what each feature does, how to configure
it, and the rules it follows. The top-level README is the tour; these pages
are where the details live.

| Page | Covers |
|---|---|
| [login.md](login.md) | `/login`: API keys, search API keys, Sign in with ChatGPT (`openai-codex`), auth.json, token refresh |
| [bash.md](bash.md) | Shell commands: background sessions, how the model is told they ended, `-p` waiting for them |
| [input.md](input.md) | Sending while the agent works: Enter steers at the next turn boundary, Alt+Enter queues a follow-up |
| [permissions.md](permissions.md) | Which tool calls ask, run or are refused: modes, `permissions.allow` and `permissions.deny` rules, trusting a project's rules, `/permissions` |
| [sessions.md](sessions.md) | Sessions on disk, what survives a crash, recovery, exits, `--no-sync` |
| [settings.md](settings.md) | `/settings`, `/model`, `/effort`, `/thinking`: every setting, where it is read from, what applies when |
| [tools.md](tools.md) | `/tools`: turning the model's tools on and off, `tools.<name>`, web_fetch off by default |
| [web.md](web.md) | `web_fetch` and `web_search`: what they send, the search backends (Exa, Parallel), `search.backend` |
| [mcp.md](mcp.md) | MCP servers: config keys, trust, OAuth, prompts and resources, filled-in tool arguments |
| [rewind.md](rewind.md) | `/rewind`, `pytte rewind`: file snapshots, restoring files, forking before a message |
| [mentions.md](mentions.md) | `@path` file mentions and image attachments in the TUI: the menu, what is sent, limits, `Ctrl-V` |

## Writing a page

One page per feature area, named after it (`mcp.md`, `permissions.md`,
`sessions.md`, ...). Add it to the table above. Each page has, in order:

1. **What it is**: a paragraph on what the feature does and when to use it.
2. **Configuration**: every key, with its type, default and an example.
   Say where it can be set (user config, project config, environment, flag)
   and which wins.
3. **Behavior**: the rules a user can rely on: what happens when, what is
   kept or sent where, edge cases, and what is refused and why.
4. **Examples**: complete, copyable config for real cases.

Write what pytte does, not how the code does it: internals belong in code
comments. Update the page in the same change as the behavior.
