# Rewind

pytte can take back what a session did to your files and to the
conversation. Before `write` and `edit` change a file, pytte saves the
file's previous contents in the session. `/rewind` (or `pytte rewind`)
then goes back to before one of your messages: it puts those files back
and continues in a fork of the session that ends just before the message,
with the message in the input box to send again, or to change first.

Use it when a turn went the wrong way: the model rewrote half a project
from a vague request, and you want the tree and the conversation as they
were before you asked.

## Configuration

There are no config keys. Every `write` and `edit` is saved first.

The TUI command:

```
/rewind [MESSAGE] [both|files|conversation] [overwrite]
```

| Part | Meaning |
|---|---|
| `MESSAGE` | The entry ID of one of your messages. `/rewind` alone lists them, newest first, with the text of each |
| `both` (default) | Put the files back, then continue in a fork before the message |
| `files` | Put the files back. The session stays as it is |
| `conversation` | Fork before the message. The files stay as they are |
| `overwrite` | Replace files that were changed outside pytte (see below) |

Esc Esc with nothing typed, while no run is going, opens the same list.

The command line:

```
pytte rewind ID [--at ENTRY] [--files | --conversation] [--yes]
```

| Flag | Meaning |
|---|---|
| `ID` | The session |
| `--at ENTRY` | The user message's entry ID (`pytte log ID --json` shows them). Default: the newest |
| `--files` | Only the files |
| `--conversation` | Only the fork |
| `--yes` | Replace files changed outside pytte without asking |

It prints what it restores on stderr and, with a fork, the new session's
ID on stdout. `pytte -r NEW` opens it with the message in the input box.

## Behavior

- **What is saved.** Before `write` or `edit` runs, the file's bytes (or
  the fact that it did not exist) are stored as a blob in the session's
  `blobs/` directory, named by their SHA-256, so an unchanged file costs
  nothing again. The tool call's record in the log names the blob, and is
  committed before the file changes. A crash cannot leave a changed file
  without its saved copy. A file that cannot be saved (too large, over
  256 MiB, or not a regular file) is not changed: the model is told the
  call did not run.
- **What is put back.** Every file that `write` or `edit` changed after
  your message goes back to what it was when the first of those calls
  came: contents restored, or the file removed if the call created it.
  Files that already are as they were are left alone, so a second rewind
  to the same message does nothing. Directories a call created stay.
  Each file is restored whole, by writing a new file next to it and
  renaming it over the old one.
- **The fork.** The fork is a copy of the session whose log ends just
  before the commit that placed your message, so the model sees the
  conversation as it was. The message is in the input box, not sent. The
  original session is not changed, apart from the rewind itself being
  recorded in it. A message you sent while a run was going and that waited
  its turn cannot be rewound to: the log cannot end before it without
  cutting that run. Pick the message before it.
- **What cannot be undone.** Changes made by `bash` and by MCP tools are
  not saved. When the range holds such calls, `/rewind` says how many.
  Check what they changed.
- **Files changed outside pytte.** pytte compares each file it would
  overwrite with the stamp of its last `read`, `write` or `edit` (the
  `reads:` doc). A file that no longer matches was changed by something
  else, say you, or a `bash` command. These are listed, and nothing is
  changed until you say `overwrite` (`--yes` on the command line, which
  asks on a terminal otherwise).
- **After restoring.** The read stamps of the restored files are cleared.
  `edit` asks the model to `read` a file again before editing it, as the
  model has not seen the files as they are now. With `files`, the
  conversation goes on in the same session and the model is not told that
  the files changed. Say so in your next message.
- **Crashes.** Restoring is a task in the session's log, committed with
  the list of files before the first one is touched. Killed at any point,
  `pytte -r` resumes it and restores the files again; restoring a file
  twice does the same as once. The fork is made after the files are
  restored. If pytte dies between the two, `/rewind MESSAGE conversation`
  makes the fork.
- **Disk.** Saved files stay in the session's `blobs/`, and forks share
  them (hard links where the file system allows). The log records
  reference every blob, so a future `pytte gc` keeps them.

## Examples

Undo the last turn from the command line, without asking about changed
files:

```
$ pytte rewind 20261015-091500-k3xq --yes
pytte: back to before entry 19: rename the parser functions
  restore /home/w/proj/parse.c
  remove  /home/w/proj/parse_new.c
pytte: 1 call of bash or MCP tools since then cannot be undone
pytte: restored 2 files
pytte: continue with: pytte -r 20261015-091500-w7mc
20261015-091500-w7mc
```

In the TUI, pick the message from the list, then how:

```
/rewind
/rewind 19 files
/rewind 19 both overwrite
```
