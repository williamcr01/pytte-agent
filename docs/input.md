# Sending while the agent works: steer and queue

You can type while a run is active. A message sent then either **steers**
the run or waits as a **follow-up**. Use a steer to correct course
without interrupting ("only the C files", "stop, use the other
directory"); use a follow-up for the next task.

## Configuration

There are no config keys. In the TUI:

| Key | While a run is active | When idle |
|---|---|---|
| Enter | steer | send |
| Alt+Enter | queue as a follow-up | send |
| Shift+Enter, Ctrl-J | newline | newline |

Alt+Enter needs a terminal that sends Alt as Esc (Terminal.app and iTerm2
need "Option as Meta") or speaks the kitty protocol. Slash commands are
never steered or queued this way; they run at once, as always.

The library takes the same choice as `pt_input.steer`. In the log it is
`"when_busy":"steer"` on the input record.

## Behavior

- **A steer is saved at once**, like any input, and shown under the run as
  `↳ will be sent after the current tool calls · <text>`.
- **It reaches the model at the next turn boundary**: when every tool call
  of the round in flight has ended, before the next request. It is placed
  in the transcript as a message of yours after the tool results, and the
  `↳` row moves into the transcript. Several steers are placed in the
  order you sent them.
- **A run that ends with an answer and no tool calls** has no next
  boundary. A steer still waiting then starts the next turn of the same
  run, with no new "worked for" line in between.
- **A follow-up** (`queued · <text>`) waits for the run to end, then starts
  a run of its own. A steer is placed before an older follow-up.
- **When the run cannot take it**: if the run fails or you interrupt it,
  a waiting steer becomes an ordinary follow-up and starts the next run.
  A steer also waits while the context is being compacted, and joins the
  next boundary after.
- **A crash neither loses nor repeats a steer.** The placement is committed
  together with the step that moves the run on. Resuming after a crash
  before it places the steer at the same boundary; after it, finds it
  placed. A steer's input is marked `done` once placed.
- **Nothing running:** both keys just send.

## Examples

```
› list the files          Enter, the model starts a tool call
  ↳ will be sent after the current tool calls · only the c files
```

After the tool round the row disappears and `› only the c files` appears in
the transcript; the next request carries it.
