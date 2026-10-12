#!/bin/sh
# End-to-end checks of the built-in tools on the faux provider: read, edit,
# grep, find and ls, and what read-only mode lets run. No network.
set -u
pytte=${1:-build/pytte}
fail=0

check() {
    name=$1
    shift
    if "$@"; then
        echo "  ok   $name"
    else
        echo "  FAIL $name"
        fail=1
    fi
}

if ! command -v jq >/dev/null; then
    echo "  skip (no jq)"
    exit 0
fi

pytte=$(cd "$(dirname "$pytte")" && pwd)/$(basename "$pytte")
repo=$(pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
XDG_STATE_HOME=$T/state
XDG_CONFIG_HOME=$T/config
HOME=$T/home
export XDG_STATE_HOME XDG_CONFIG_HOME HOME
unset PYTTE_MODEL PYTTE_EFFORT
mkdir -p "$HOME" "$T/w"
cd "$T/w" || exit 1

# A tree: a repository with ignored files, a binary and a dotfile.
mkdir -p .git src/sub build node_modules/x
printf 'build/\nnode_modules\n*.log\n!keep.log\n' >.gitignore
printf 'int main(void)\n{\n    return foo(1);\n}\n' >src/a.c
printf 'int foo(int x)\n{\n    return x;\n}\n/* FOO */\n' >src/sub/b.h
echo 'foo in build' >build/out.c
echo foo >node_modules/x/i.js
echo foo >x.log
echo foo >keep.log
printf 'foo\0bin' >bin.dat
echo hidden >.env
echo 'foo in git' >.git/config

# run MODE CALL... : one turn per call ({"name":…,"args":…}), then "Done.";
# results holds the tool results' data, in order.
run() {
    mode=$1
    shift
    {
        printf '{"turns": ['
        for c in "$@"; do printf '{"content": [{"type": "tool_call", %s}]},' "$c"; done
        printf '{"content": [{"type": "text", "text": "Done."}]}]}'
    } >"$T/script.json"
    PYTTE_PERMISSION_MODE=$mode "$pytte" -p -m faux/faux --script "$T/script.json" go >/dev/null 2>"$T/err"
    results=$("$pytte" log --json | jq -c 'select(.kind == "tool_result") | .data')
}
# res N: the Nth result's content
res() { printf '%s' "$results" | jq -sr ".[$1].content"; }
# is N TEXT: the Nth result is TEXT, and no error
is() {
    got=$(res "$1")
    if [ "$got" = "$2" ] && [ "$(printf '%s' "$results" | jq -s ".[$1].is_error")" = false ]; then
        return 0
    fi
    printf '    got: %s\n' "$got" | head -20
    return 1
}
# q N FILTER: jq FILTER holds for the Nth result's data
q() { printf '%s' "$results" | jq -se ".[$1] | $2" >/dev/null; }

# ---- grep, find, ls ----
run auto \
    '"name": "grep", "args": {"pattern": "foo"}' \
    '"name": "grep", "args": {"pattern": "FOO", "ignore_case": true, "glob": "*.h", "context": 1}' \
    '"name": "grep", "args": {"pattern": "foo(", "literal": true, "path": "src"}' \
    '"name": "grep", "args": {"pattern": "foo", "files_only": true}' \
    '"name": "grep", "args": {"pattern": "return", "path": "src/a.c"}' \
    '"name": "grep", "args": {"pattern": "foo", "limit": 1}' \
    '"name": "grep", "args": {"pattern": "(", "path": "src"}' \
    '"name": "grep", "args": {"pattern": "nowhere"}' \
    '"name": "find", "args": {"pattern": "*.c"}' \
    '"name": "find", "args": {"pattern": "src/**/*.h"}' \
    '"name": "find", "args": {"pattern": "*", "path": "src/"}' \
    '"name": "ls", "args": {}' \
    '"name": "ls", "args": {"path": "src", "limit": 1}' \
    '"name": "grep", "args": {"pattern": "^int", "path": "src"}' \
    '"name": "grep", "args": {"pattern": "x;$|FOO", "path": "src"}' \
    '"name": "grep", "args": {"pattern": "RETURN fo+", "ignore_case": true, "path": "src"}' \
    '"name": "grep", "args": {"pattern": "[0-9]", "path": "src", "context": 1}'
check "grep skips .git, ignored and binary files" is 0 'keep.log:1:foo
src/a.c:3:    return foo(1);
src/sub/b.h:1:int foo(int x)'
check "grep: ignore_case, glob and context" is 1 'src/sub/b.h:1:int foo(int x)
src/sub/b.h-2-{
--
src/sub/b.h-4-}
src/sub/b.h:5:/* FOO */'
check "grep: a literal pattern, under a path" is 2 'src/a.c:3:    return foo(1);
src/sub/b.h:1:int foo(int x)'
check "grep: files_only" is 3 'keep.log
src/a.c
src/sub/b.h'
check "grep: one file" is 4 'src/a.c:3:    return foo(1);'
check "grep: the limit, said" is 5 'keep.log:1:foo
[stopped at 1 matches: the limit. Narrow the search, or raise limit]'
check "grep: a bad pattern is an error" q 6 '.is_error and (.content | startswith("pattern is not a POSIX extended regular expression"))'
check "grep: no matches" is 7 'No matches.'
check "find: a name at any depth, ignored files skipped" is 8 'src/a.c'
check "find: ** with a slash" is 9 'src/sub/b.h'
check "find: directories end with /" is 10 'src/a.c
src/sub/
src/sub/b.h'
check "ls: name order, dotfiles, directories with /" is 11 '.env
.git/
.gitignore
bin.dat
build/
keep.log
node_modules/
src/
x.log'
check "ls: the limit, said" is 12 'a.c
[1 more entries; raise limit, or use find]'
check "grep: ^ holds at line starts only" is 13 'src/a.c:1:int main(void)
src/sub/b.h:1:int foo(int x)'
check "grep: an alternation and \$, over a whole file" is 14 'src/sub/b.h:3:    return x;
src/sub/b.h:5:/* FOO */'
check "grep: required text, any case" is 15 'src/a.c:3:    return foo(1);'
check "grep: a class, with context" is 16 'src/a.c-2-{
src/a.c:3:    return foo(1);
src/a.c-4-}'

# ---- read ----
seq 1 3000 >lines.txt
awk 'BEGIN { for (i = 1; i <= 100; i++) { s = sprintf("%04d", i); while (length(s) < 1000) s = s "x"; print s } }' >wide.txt
awk 'BEGIN { s = ""; while (length(s) < 60000) s = s "y"; print s; print "next" }' >long.txt
# a PNG header: 2x3 pixels
printf '\211PNG\r\n\032\n\000\000\000\rIHDR\000\000\000\002\000\000\000\003\010\002\000\000\000' >img.png
run auto \
    '"name": "read", "args": {"path": "lines.txt"}' \
    '"name": "read", "args": {"path": "wide.txt"}' \
    '"name": "read", "args": {"path": "long.txt"}' \
    '"name": "read", "args": {"path": "wide.txt", "offset": 99}' \
    '"name": "read", "args": {"path": "img.png"}'
check "read: 2000 lines, then where to read on" q 0 '(.content | endswith("  2000\t2000\n… 1000 more lines; read on with offset 2001")) and (.is_error | not)'
check "read: at most 50 KiB, cut at a line" q 1 '(.content | endswith("x\n… output cut at 50 KiB before line 51 of 100; read on with offset 51")) and (.content | length) < 51300'
check "read: a line longer than 50 KiB, cut" q 2 '.content | endswith("y\n… line 1 is longer than 50 KiB; only its start is shown. Read on with offset 2")'
check "read: the end of a file" q 3 '.content | startswith("    99\t0099x") and endswith("x\n")'
check "read: an image" q 4 '.content == "Image img.png (image/png, 2x3, 29 bytes)." and .image.mime == "image/png" and (.image.blob | startswith("sha256:"))'
sum=$(sha256sum <img.png | cut -c1-64)
check "...its bytes in a blob" q 4 ".image.blob == \"sha256:$sum\""
check "...and the session loads again with it" sh -c '"$1" -p -c -m faux/faux --script "$2" again >/dev/null 2>&1' _ \
    "$pytte" "$repo/tests/faux/hello.json"

# ---- edit ----
printf 'one\ntwo\nthree\ntwo\n' >e.txt
run auto \
    '"name": "read", "args": {"path": "e.txt"}' \
    '"name": "edit", "args": {"path": "e.txt", "edits": [{"old": "one", "new": "1"}, {"old": "three", "new": "3"}]}' \
    '"name": "edit", "args": {"path": "e.txt", "edits": [{"old": "1", "new": "x"}, {"old": "two", "new": "2"}]}' \
    '"name": "edit", "args": {"path": "e.txt", "edits": [{"old": "1\ntwo", "new": "x"}, {"old": "two\n3", "new": "y"}]}' \
    '"name": "edit", "args": {"path": "e.txt", "edits": [{"old": "two", "new": "2", "all": true}, {"old": "3", "new": "three"}]}'
check "edit: several edits in one call" is 1 'Edited e.txt: 2 edits replaced 2 occurrences.'
check "edit: which edit matches more than once" q 2 '.is_error and .content == "edit 2: old matches 2 times. Give more context so it matches once, or set all: true."'
check "edit: overlapping edits are refused" q 3 '.is_error and .content == "edits 1 and 2 overlap. Merge them into one edit."'
check "edit: all, with another edit" is 4 'Edited e.txt: 2 edits replaced 3 occurrences.'
check "...and the file has them all" [ "$(cat e.txt)" = '1
2
three
2' ]

# ---- read-only mode: read, grep, find and ls only ----
run read-only \
    '"name": "bash", "args": {"command": "touch made-by-bash"}' \
    '"name": "write", "args": {"path": "made-by-write", "content": "x"}' \
    '"name": "edit", "args": {"path": "e.txt", "edits": [{"old": "three", "new": "3"}]}' \
    '"name": "bash_poll", "args": {"session": 1}' \
    '"name": "read", "args": {"path": "e.txt"}' \
    '"name": "grep", "args": {"pattern": "three", "path": "e.txt"}' \
    '"name": "find", "args": {"pattern": "e.*"}' \
    '"name": "ls", "args": {"path": "src"}'
for i in 0 1 2 3; do
    check "read-only: $(printf '%s' "$results" | jq -sr ".[$i].name") is denied" \
        q $i '.is_error and (.content | contains("read-only mode"))'
done
check "...and nothing was made or changed" sh -c '[ ! -e made-by-bash ] && [ ! -e made-by-write ] && grep -q three e.txt'
check "read-only: read runs" q 4 '.is_error | not'
check "read-only: grep runs" is 5 'e.txt:3:three'
check "read-only: find runs" is 6 'e.txt'
check "read-only: ls runs" is 7 'a.c
sub/'

# ---- ask mode: nothing here asks ----
run ask \
    '"name": "bash_poll", "args": {"session": 1}' \
    '"name": "grep", "args": {"pattern": "three", "path": "e.txt"}' \
    '"name": "find", "args": {"pattern": "e.*"}' \
    '"name": "ls", "args": {"path": "src"}'
check "ask: bash_poll, grep, find and ls run without asking" q 3 '.content == "a.c\nsub/"'
check "...bash_poll too" q 0 '.content | startswith("No session 1:")'

exit $fail
