# Tools

The model's tools are `bash`, `read`, `write`, `edit`, `grep`, `find`,
`ls` and `web_fetch`, plus any tools from MCP servers. You can turn each
one on or off. The model is not told about a tool that is off: it is left
out of the tool list sent with requests. Calls to it are refused. Every
tool starts on except `web_fetch`, which you have to turn on.

## Configuration

`tools` in the config is an object of booleans keyed by tool name. A tool
that is not listed keeps its default.

| Key | Type | Default | Example |
|---|---|---|---|
| `tools.<name>` | boolean | `true`; `false` for `web_fetch` | `"tools": {"web_fetch": true, "bash": false}` |

The key can go in the user file (`~/.config/pytte/config.json`) or the
project file (`.pytte/config.json`). The project file wins. The TUI writes
to the user file. MCP tools are named as the model sees them
(`mcp__<server>__<tool>`).

`bash_poll` only reads and stops background `bash` sessions, so it is on
and off with `bash`. It is not listed by itself, and `tools.bash_poll` has
no effect.

## Behavior

- **`/tools`** opens a list of the tools. Each row shows a filled dot (`●`,
  or `[x]` in ASCII mode) when the tool is on and a hollow one (`○`, `[ ]`)
  when it is off, with the first sentence of the tool's description. Use
  ↑↓ to select and space or Enter to turn the selected tool on or off. ←
  turns it off, → turns it on, and Esc closes the list. The footer counts
  the tools that are on (`7 of 8 on`).
- **`/settings`** has a `tools` row showing the same count. Enter on it
  opens the list.
- **Commands.** `/tools <name> on|off` sets a tool and `/tools <name>`
  shows its state and where that came from (`web_fetch: off (default)`).
  Completion lists each tool with its state.
- **When a change applies.** A change is saved at once (`saved web_fetch:
  on`).
  - **Before your first message,** the session's first request uses the
    new state. That includes a resumed session that has no messages yet,
    a `/tools` command typed while the session is still opening, and the
    list opened from `/settings` at that point (it opens once the session
    is ready).
  - **Later in a session,** a tool you turn off is refused from the next
    call. The model keeps seeing it in its list until the next session or
    `/compact`. A tool you turn on reaches the model at the next session or
    `/compact`. The saved line says which case applies.
- **A tool that is off** is refused even in `auto` permission mode. The
  model is told "the user turned this tool off".
- **Print mode** (`pytte -p`) reads the same config.

## Examples

Let the model fetch web pages, in `~/.config/pytte/config.json`:

```json
{"tools": {"web_fetch": true}}
```

A project where the model only reads and edits, without a shell, in
`.pytte/config.json`:

```json
{"tools": {"bash": false, "web_fetch": false}}
```
