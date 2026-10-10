#!/bin/sh
# End-to-end checks of pytte-ai and pytte on the faux provider. No network.
set -u
bin=${1:-build/pytte-ai}
pytte=${2:-build/pytte}
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

out=$("$bin" faux --script tests/faux/hello.json hi 2>/dev/null)
check "text goes to stdout" [ "$out" = "Hello from the faux provider! Nothing here touched the network." ]

out=$("$bin" faux --script tests/faux/tool_call.json --tools tests/tools/echo.json --turns 3 "echo hi" 2>/dev/null)
check "tool calls get canned results" [ "$out" = "I'll echo that.
The echo tool said: hi" ]

"$bin" faux --script tests/faux/errors.json x >/dev/null 2>&1
check "provider errors exit with 2" [ $? -eq 2 ]

err=$("$bin" faux --script tests/faux/errors.json x 2>&1 >/dev/null)
case $err in *"retryable, HTTP 529"*) ok=0 ;; *) ok=1 ;; esac
check "error class and status are reported" [ $ok -eq 0 ]

events=$("$bin" faux --script tests/faux/tool_call.json --events x 2>/dev/null)
check "events end in exactly one done" [ "$(printf '%s\n' "$events" | grep -c '"type":"done"')" -eq 1 ]
if command -v jq >/dev/null; then
    check "every event line is JSON" sh -c 'printf "%s\n" "$1" | jq -e . >/dev/null' _ "$events"
fi

env -u ANTHROPIC_API_KEY "$bin" anthropic hi >/dev/null 2>&1
check "a missing key exits with 1" [ $? -eq 1 ]

"$bin" --list | grep -q 'anthropic/claude-opus-5-5'
check "--list shows the catalog" [ $? -eq 0 ]

# ---- pytte ----
XDG_STATE_HOME=$(mktemp -d)
export XDG_STATE_HOME

out=$("$pytte" -p -m faux --script tests/faux/hello.json hi 2>/dev/null)
check "pytte -p prints the answer" [ "$out" = "Hello from the faux provider! Nothing here touched the network." ]

out=$("$pytte" log 2>/dev/null)
case $out in *"> hi"*"Hello from the faux provider"*) ok=0 ;; *) ok=1 ;; esac
check "pytte log shows the transcript" [ $ok -eq 0 ]

"$pytte" -p -c </dev/null >/dev/null 2>&1
check "resuming an idle session exits 0" [ $? -eq 0 ]

out=$("$pytte" -p -m faux --script tests/faux/tool_call.json "echo hi" 2>/dev/null)
check "tool calls get an error result, then the run goes on" [ "$out" = "I'll echo that.
The echo tool said: hi" ]

printf '%s' '{"turns": [[{"status": 529}, {"status": 400, "body": {"message": "bad request"}}]]}' \
    >"$XDG_STATE_HOME/fail.json"
"$pytte" -p -m faux --script "$XDG_STATE_HOME/fail.json" --retry-base-ms 1 x >/dev/null 2>&1
check "a run that fails after a retry exits with 10" [ $? -eq 10 ]

env -u ANTHROPIC_API_KEY "$pytte" -p -m anthropic hi >/dev/null 2>&1
check "a missing key exits with 1" [ $? -eq 1 ]
env -u ANTHROPIC_API_KEY "$pytte" -p -c -m faux --script tests/faux/hello.json </dev/null >/dev/null 2>&1
check "...and the session resumes with another model" [ $? -eq 0 ]

"$pytte" -p -m faux --script tests/faux/slow.json "list" >/dev/null 2>&1 &
owner=$!
sleep 0.5
err=$("$pytte" -p -c --script tests/faux/slow.json </dev/null 2>&1 >/dev/null)
case $err in *"in use by"*"pid $owner"*) ok=0 ;; *) ok=1 ;; esac
check "a second process is refused, naming the owner" [ $ok -eq 0 ]
kill -INT $owner
wait $owner
check "Ctrl-C detaches with 130" [ $? -eq 130 ]
out=$("$pytte" -p -c --script tests/faux/slow.json </dev/null 2>/dev/null)
case $out in *"plenty of time to kill the process"*) ok=0 ;; *) ok=1 ;; esac
check "...and the detached session resumes to the end" [ $ok -eq 0 ]

if command -v jq >/dev/null; then
    "$pytte" -p -m faux --script tests/faux/hello.json --output-format stream-json hi >"$XDG_STATE_HOME/out" 2>/dev/null
    check "stream-json prints commits" sh -c 'jq -se "length == 6 and .[0].seq == 1" "$1" >/dev/null' _ \
        "$XDG_STATE_HOME/out"
fi

out=$(echo "hi from a pipe" | "$pytte" -m faux --script tests/faux/hello.json 2>/dev/null)
check "a piped prompt runs in print mode" [ "$out" = "Hello from the faux provider! Nothing here touched the network." ]
id=$(ls "$XDG_STATE_HOME/pytte/sessions" | tail -1)
"$pytte" log "$id" | grep -q "hi from a pipe"
check "...and is the newest session" [ $? -eq 0 ]
out=$("$pytte" -p -r "$id" -m faux --script tests/faux/hello.json "again" 2>/dev/null)
check "-r continues a session by ID" [ "$("$pytte" log "$id" | grep -c '^> ')" -eq 2 ]
if command -v jq >/dev/null; then
    out=$("$pytte" -p -m faux --script tests/faux/hello.json --output-format json hi 2>/dev/null)
    check "json prints one result object" sh -c 'printf "%s" "$1" | jq -e ".status == \"completed\" and (.result | startswith(\"Hello\"))" >/dev/null' _ "$out"
fi
abs=$(cd "$(dirname "$pytte")" && pwd)/$(basename "$pytte")
(cd "$XDG_STATE_HOME" && "$abs" -p -c </dev/null >/dev/null 2>&1)
check "-c only continues sessions of this directory" [ $? -eq 1 ]
rm -rf "$XDG_STATE_HOME"

exit $fail
