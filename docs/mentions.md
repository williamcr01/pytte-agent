# Mentions and attachments

In the TUI, `@path` adds a file or a directory listing to your message, and
a pasted image path (or the clipboard's image) attaches an image. The model
gets the contents with the message, so it need not spend a tool call to
read them. `@server:uri` adds an MCP resource the same way (see
[mcp.md](mcp.md)).

## Configuration

There are no config keys. Image attachments from the clipboard (`Ctrl-V`)
need one of these programs on `PATH`:

| Program | Where |
|---|---|
| `wl-paste` | Wayland |
| `xclip` | X11 |
| `pngpaste` | macOS (`brew install pngpaste`) |

pytte tries them in that order and uses the first that is installed and has
an image.

## Behavior

### `@path`

- **The menu.** Typing `@` at the start of a word opens a menu of the paths
  under the session's directory, filtered as you type. Resources of MCP
  servers are listed first. With nothing after the `@`, or after a path that
  ends in `/`, it lists that directory's entries; otherwise it lists the
  paths that start with what you typed, then those that contain it. Paths
  with a space in them are not listed (a mention ends at a space).
- **What it lists.** The same walk as the `find` tool: it skips `.git` and
  whatever `.gitignore` files ignore, does not follow links, and looks at
  20,000 paths at most.
- **Keys.** ↑/↓ choose, Tab or Enter completes, Esc closes. A file is
  completed with a space after it, so you go on typing. A directory is
  completed as `@dir/` and the menu shows what is in it, so Tab walks down a
  tree.
- **On send.** Each `@path` in the message, at the start of a word, that
  names a file or directory that exists adds a block after your text:

  ```
  see @src/main.c

  <file path="src/main.c">
  ...the file...
  </file>
  ```

  A file is added as text with the limits of `read`: at most 2,000 lines and
  50 KiB, cut at a whole line, and the block says when it was cut. A binary
  file, or an image, is named but not included (paste an image's path to
  attach it). A directory is added as its entries, one per line, directories
  ending in `/`, at most 500. A path may be relative to the session's
  directory, absolute, or start with `~/`.
- **Plain text stays plain.** An `@word` that is not the name of an existing
  path or a ready MCP server (`@alice`, an email address's `@`) is sent as it
  is. A file mentioned twice is added once. At most 8 mentions, resources and
  paths together, go with one message.
- **What is stored.** The input in the log holds the text as sent, blocks
  included, as with `$skill` blocks, so the log shows what the model saw. The
  TUI shows the blocks as one row each (`file src/main.c`, `directory src`).

### Images

- **Pasting a path.** Terminals paste a dropped file as its path. When every
  line of a paste is the path of a PNG, JPEG, GIF or WebP file (plain, in
  quotes, with backslash escapes, or as `file://`), it becomes an
  attachment, shown in the draft as `[image 1: name.png]`. Any other paste is
  text. This needs a terminal with bracketed paste; a terminal that types a
  dropped path key by key sends it as text.
- **`Ctrl-V`.** Reads an image from the clipboard (programs above) and
  attaches it as `[image N: clipboard.png]`. With no image, or no program, it
  says so. A terminal that turns `Ctrl-V` into a paste of its own sends an
  empty paste when the clipboard holds only an image, and that attaches it
  too. The program has 1.5 seconds to answer.
- **Limits.** The same as `read`: at most 3,932,160 bytes and 8,000 pixels on
  a side, nothing is scaled. An image over a limit is refused with the
  reason and not attached. At most 8 images go with one message.
- **Editing.** The label is the attachment: delete it from the draft and the
  image is dropped; an edited label is just text. On send the labels are
  numbered from 1 in the order they stand. Clearing the draft (`Ctrl-C`)
  drops the images. A draft restored after a restart keeps its label text,
  not its images.
- **What is stored.** The image bytes go to a blob in the session's store
  (the log holds only their reference, type and name), and the user entry
  carries the references. The text keeps its `[image N: name]` labels. Each
  image reaches the model as an image after the message's text, labeled with
  the same text. A model that takes no images gets the label and a note that
  an image was omitted, as `read` does for one.
- **Sessions that resume.** Images are read back from their blobs when the
  session is loaded, so a resumed session sends them again.

## Examples

Attach a file and ask about it:

```
› why does @src/tui/app_input.c queue inputs?
```

Drop a screenshot into the terminal, or copy one and press `Ctrl-V`:

```
› what is wrong with this layout? [image 1: screenshot.png]
```
