# Permissions

Before a tool call runs, pytte decides whether to run it, ask you first,
or refuse it. The decision comes from the permission mode and from rules
you keep in your config: `allow` rules for calls you do not want to be
asked about, and `deny` rules for calls that must never run.

## Configuration

Keys of `~/.config/pytte/config.json` (user) and `.pytte/config.json` in
the working directory (project):

```json
{
  "permissions": {
    "mode": "ask",
    "allow": ["bash(git status)", "bash(git diff:*)", "bash(make:*)", "web_fetch(docs.python.org)", "mcp__github__*"],
    "deny":  ["bash(rm -rf:*)", "web_fetch(*.internal)"]
  }
}
```

| Key | Type | Default | Meaning |
|---|---|---|---|
| `permissions.mode` | `ask`, `auto` or `read-only` | `ask` | What happens to a call no rule decides. Also `--permission-mode` and `$PYTTE_PERMISSION_MODE`, which win over the files |
| `permissions.allow` | array of rules | `[]` | Calls that run without asking |
| `permissions.deny` | array of rules | `[]` | Calls that are refused |

A rule is a string, `tool` or `tool(pattern)`. `*` matches any text, in the
tool name and in a pattern. What the pattern is matched against depends on
the tool:

| Rule | Matches |
|---|---|
| `bash` | every `bash` call |
| `bash(git status)` | a command that is exactly `git status` |
| `bash(git diff:*)` | a command that starts with the words `git diff`: `git diff` and `git diff HEAD`, not `git diffstat` |
| `bash(npm run *)` | a command that matches the pattern as a whole |
| `web_fetch(docs.python.org)` | a URL on that host (any scheme, port or path; case does not matter) |
| `web_fetch(*.internal)` | a URL on any host ending in `.internal` |
| `mcp__github__*` | every tool of the `github` MCP server |
| `write(src/*)` | for other tools, the first string argument: the `path` of `read`, `write`, `edit` and `ls`, written as the model wrote it (not resolved) |

The user file and the project file are both read; their rules are added
together. Entries that are not rules are skipped, and the first problem is
shown at startup. The files are read when a session opens (and by
`/permissions trust`), so a change applies to the next session.

## Behavior

**Order.** For each call, pytte checks in this order and stops at the first
that decides:

1. The tool is turned off (see [tools.md](tools.md)): the call is refused,
   whatever the mode.
2. A `deny` rule matches: the call is refused, whatever the mode, and the
   model is told which rule refused it.
3. The mode is `read-only`: only the tools that change nothing run
   (`read`, `grep`, `find`, `ls`). Allow rules do not change this.
4. The tool changes nothing, or the mode is `auto`, or an `allow` rule
   matches: the call runs.
5. Otherwise it asks (in print mode, the process ends with exit code 3).

So a `deny` rule also stops a read-only tool (`read(*.env)`) and a call in
`auto` mode.

**Shell lines.** A `bash` line can hold several commands, split at `;`, `&`,
`|` and newlines.

- An `allow` rule lets a line run only when rules allow *every* command in
  it. With `bash(git diff:*)` alone, `git diff; rm -rf x` still asks. With
  `bash(git status)` and `bash(make:*)`, `git status && make test` runs.
- A line with `$(...)`, backticks, `<`, `>` or parentheses is not taken
  apart: only a bare `bash` rule allows it.
- A `deny` rule refuses a line when it matches *any* command in it, and it
  looks inside `$(...)` and backticks too: `bash(rm -rf:*)` refuses
  `ls; rm -rf build` and `echo $(rm -rf build)`.

Rules match what the command says. They are not a sandbox: `sudo rm -rf`,
`sh -c 'rm -rf x'` or a script that deletes files get past
`bash(rm -rf:*)`.

**Project rules and trust.** A repository you clone can carry a
`.pytte/config.json`, so its rules are not all taken on trust:

- Its `deny` rules apply at once. They only take permission away.
- Its `allow` rules do not apply until you run `/permissions trust`. Until
  then pytte says so at startup and `/permissions` lists them as not
  trusted.
- Trust is kept in `~/.config/pytte/trust.json` with a hash of the project's
  `permissions.allow` list, by directory. Any change to the list asks
  again. `/permissions untrust` takes it back.

User rules apply at once.

**Always allow.** When a call asks in the TUI, answer 4 saves an allow rule
to `permissions.allow` of the user file and applies it now: `bash(<first
word>:*)` for `bash` (allowing `git` allows every `git` command), the host
for `web_fetch`, the tool name for the others. Nothing else in the file
changes, and a file that does not parse is left as it is (the rule then
applies to this session only, and pytte says why). It is not offered for a
`bash` command that has no first word to name.

**What is recorded.** A call that a rule allowed is recorded with the rule
(`allow bash(git diff:*) (user config)`) in the session log. A call a rule
refused gets an error result that names the rule. A call you approved is
recorded as your answer. A resumed session does not ask again about either.

**`/permissions`** shows the mode and every rule in force with where it
came from (`this session`, `user config` or `project config`), and which
project allow rules are not trusted. `/permissions trust` and
`/permissions untrust` change the trust of the project in the current
directory. The rules the TUI's answer 2 allows for the session are listed
as `this session`.

## Examples

Never ask about read-only git and the build, never allow a force push or a
delete:

```json
{
  "permissions": {
    "allow": ["bash(git status)", "bash(git log:*)", "bash(git diff:*)", "bash(make:*)"],
    "deny":  ["bash(git push --force:*)", "bash(rm -rf:*)"]
  }
}
```

Stop the file tools from touching `.env` files, in a project's
`.pytte/config.json` (the deny applies without trust). The model can still
reach them through `bash`, so deny that too if it matters:

```json
{"permissions": {"deny": ["read(*.env)", "write(*.env)", "edit(*.env)", "bash(cat .env:*)"]}}
```

Let a team's tools run in a repository after you have looked at them:

```json
{"permissions": {"allow": ["bash(make test)", "web_fetch(docs.python.org)"]}}
```

then `/permissions trust` in that directory.
