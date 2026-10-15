# Sessions and crash recovery

A session is one conversation and everything it did: every prompt,
answer, tool call and result, written to disk before it takes effect. If
pytte is killed at any point (Ctrl-C, a closed terminal, `kill -9`, a
crash, a power cut), `pytte -c` or `pytte -r ID` carries on from the last
step that was written, without losing work or running an unsafe tool
call twice.

## Configuration

| Setting | Where | Default | Meaning |
|---|---|---|---|
| `$XDG_STATE_HOME` | environment | `~/.local/state` | Sessions live in `$XDG_STATE_HOME/pytte/sessions/<id>/` |
| `--no-sync` | flag | off | Skip fsync: a session still survives the process being killed, but a power cut may lose its newest steps |
| `--force` | flag | off | Open a session that another host opened and never closed |

## Behavior

### What is on disk

Each session is a directory:

| Path | What it holds |
|---|---|
| `log.jsonl` | The whole truth: one commit per line, each a JSON object with a sequence number and a CRC-32 checksum, readable with `jq` |
| `blobs/ab/cdef…` | Large content (prompts of 16 KB or more, long command output, images), named by its SHA-256 |
| `live/` | The newest output of a request or tool call in progress, for recovery and watchers |
| `procs/` | The process groups of `bash` calls that may still be running |
| `LOCK` | Held by the process that has the session open, and says which one |
| `name`, `draft` | The session's name and the unsent input |

### What is durable

- **A step is written before it happens.** pytte appends the decision
  (send this request, run this tool call) to `log.jsonl` and fsyncs it,
  then starts it. Its result is written the same way. Something that was
  never written never happened.
- **Blobs come first.** A blob is complete and synced before any log
  line names it.
- **Files the model writes.** `write` and `edit` replace a file through a
  temporary file and a rename, and sync both the file and its directory
  before the result is written, so the log does not claim a change that a
  power cut undid. Once the rename is done the file has changed, so a
  failed directory sync after it is not reported as a failed write (the
  same goes for a directory `write` creates, and for `config.json`).
- **macOS** syncs with `F_FULLFSYNC`, which also empties the drive's
  write cache; plain `fsync` there does not survive a power cut. pytte
  falls back to `fsync` only on a file system without `F_FULLFSYNC`; any
  other error is reported.
- **One owner at a time.** Opening a session takes its lock; a second
  `pytte` on the same session is refused and told who has it.

### Recovery

When a session is opened again after its owner died:

- A half-written last line is cut off: that step never happened.
- A damaged line before the last means the log is corrupt, and the
  session is not opened (exit code 20). A blob the log names that is
  missing is treated the same way.
- **An answer that was streaming** is kept in the transcript as an
  interrupted entry (never sent to the model), and the request is sent
  again.
- **A tool call that was running** follows its tool's replay policy.
  `read`, `grep`, `find`, `ls`, `write`, `bash_poll` and `web_fetch` run
  again. A `bash` or `edit` call, and MCP tools not marked read-only or
  idempotent, are not repeated: the model gets an error result saying the
  call was interrupted and may or may not have taken effect, with the
  last output it printed. It reads "interrupted by a crash" after a crash,
  and "cut off when pytte exited" when pytte quit cleanly mid-call.
- **Commands left running.** A `bash` call's command, and anything it
  started in its process group, is stopped (SIGKILL) when the session is
  next opened, before any tool runs but after the session is known to be
  this host's to open. Until then it goes on. Each group's record names
  its host, the boot, its shell's start time, and a random
  `PYTTE_GROUP` value that the command and its children inherit in their
  environment. A group is stopped only when its shell still runs with
  that start time, or, on Linux, when its shell is gone and a process
  still in the group carries that `PYTTE_GROUP`. A process that later got
  the same ID descends from someone else and lacks the value, so it is
  never stopped by mistake. When pytte cannot tell (another system with
  the shell gone, or a process that cleared its environment) it leaves
  the group running and forgets it. A record from another host (a
  session opened with `--force`) is kept for that host; one from before
  a reboot is dropped.
- **Approvals.** A tool call waiting for approval keeps waiting; an
  answer given before the crash was written, and counts.

### Exits

| How pytte stops | The session |
|---|---|
| `/quit`, Ctrl-D, Ctrl-C twice | closed |
| SIGTERM, SIGINT, SIGHUP (a closed terminal or ssh connection), in the TUI and in print mode | closed |
| SIGHUP to print mode started ignoring it (`nohup pytte -p …`, `trap '' HUP`) | runs on |
| `kill -9`, a crash, a power cut | left open: `pytte ls` says *crashed* |

A closed session with a run unfinished continues where it stopped when
resumed. After `kill -9` the TUI cannot restore the terminal's modes; run
`reset` if keys misbehave.

`pytte ls` shows each session as running (with the owner's pid), idle,
waiting (a tool call needs approval), closed or crashed. A session that
died before its first step was written has nothing to resume, and
`pytte ls` removes it.

## Examples

Continue the newest session of this directory after a crash:

```sh
pytte -c
```

Look at what a session did, including steps before its compactions:

```sh
pytte log --all-epochs 20261014-193443-kvdn
jq -c '.w[] | select(.t == "task")' ~/.local/state/pytte/sessions/20261014-193443-kvdn/log.jsonl
```

A throwaway batch run on a machine with slow disks, where losing the last
steps to a power cut is acceptable:

```sh
pytte -p --no-sync "summarize the changes in src/"
```
