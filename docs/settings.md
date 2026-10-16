# Settings

pytte's settings are the model, effort, how thinking is shown, symbols,
the pager, the permission mode, the compaction model and automatic skills.
In the TUI, `/settings` opens a menu of them; a change applies to the
running session at once and is saved for later sessions. `/model` and
`/effort` switch the model or effort without saving.

## Configuration

| Setting (`/settings`) | Config key | Values | Default | Also set by |
|---|---|---|---|---|
| `thinking` | `tui.thinking` | `hidden`, `full` | `hidden` | Ctrl-T, `/thinking` |
| `symbols` | `tui.symbols` | `auto`, `unicode`, `ascii` | `auto` | `PYTTE_ASCII=1` |
| `model` | `model` | a model ref, `provider/id` | `anthropic/claude-opus-5-5` | `-m`, `PYTTE_MODEL` |
| `effort` | `reasoning` | `off`, `minimal`, `low`, `medium`, `high`, `xhigh`, `max` | `medium` | `--effort`, `PYTTE_EFFORT` |
| `pager` | `tui.pager` | a command | `$PAGER`, else `less -R` | |
| `permissions` | `permissions.mode` | `ask`, `auto`, `read-only` | `ask` | `--permission-mode`, `PYTTE_PERMISSION_MODE` |
| `compact-model` | `compaction.model` | a model ref, or `none` | `none`: the session's model | `--compact-model`, `PYTTE_COMPACT_MODEL` |
| `skills` | `skills.auto` | `on`, `off` (or `true`, `false`) | `on` | |

They are read, each later one winning: the built-in default, the user file
`~/.config/pytte/config.json`, the project file `.pytte/config.json`, the
environment, then flags. The TUI writes changes to the user file, one key
at a time, keeping every other key in it.

## Behavior

- **Every change applies now.** Picking a value in `/settings` (or
  `/settings <name> <value>`) changes the running session and saves it. A
  setting that skills read when an epoch starts (`skills`) takes effect
  from the next session or `/compact`, and the saved line says so.
- **One line per change.** `saved effort: high` appears once the value is
  on disk; an invalid value is refused with the values that are allowed
  (`not a value for thinking: some (hidden, full)`) and nothing is saved.
- **Values set elsewhere.** The menu marks a value that the project file,
  the environment or a flag set (`(set by env)`). Changing it in the TUI
  still applies it now and saves it to the user file; the saved line notes
  that the environment or the project file sets another at the next start.
- **`/model` and `/effort`** switch this session without saving.
  `/settings model` and `/settings effort` switch it too, and also save it
  as the default.
- **Which model a session gets.** At start, a new session gets the
  configured model, a resumed one keeps its own. In the TUI the same holds
  for `/new`, `/worktree` and `/resume`, unless a model was chosen: one
  given with `-m` or `PYTTE_MODEL`, or picked with `/model` or
  `/settings model`, goes on to the sessions opened next. `/fork` keeps the
  original's model.
- **`/thinking`** and Ctrl-T change how thinking is shown, and save it.
- **Clearing the compaction model.** `/settings compact-model none` goes back
  to compacting with the session's model.
- **Seeing a value.** `/settings <name>` prints it and where it came from
  (`effort: high (user)`). The setting's config key works as its name too
  (`/settings reasoning`).

## Examples

`~/.config/pytte/config.json` for a cheaper default with a separate
compaction model and full thinking:

```json
{
  "model": "anthropic/claude-sonnet-5-5",
  "reasoning": "high",
  "compaction": {"model": "anthropic/claude-haiku-5-5"},
  "tui": {"thinking": "full"}
}
```

A project that never edits without asking, whatever the user file says,
in `.pytte/config.json`:

```json
{"permissions": {"mode": "ask"}}
```
