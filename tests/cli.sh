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

out=$("$bin" faux/faux --script tests/faux/hello.json hi 2>/dev/null)
check "text goes to stdout" [ "$out" = "Hello from the faux provider! Nothing here touched the network." ]

out=$("$bin" faux/faux --script tests/faux/tool_call.json --tools tests/tools/echo.json --turns 3 "echo hi" 2>/dev/null)
check "tool calls get canned results" [ "$out" = "I'll echo that.
The echo tool said: hi" ]

"$bin" faux/faux --script tests/faux/errors.json x >/dev/null 2>&1
check "provider errors exit with 2" [ $? -eq 2 ]

err=$("$bin" faux/faux --script tests/faux/errors.json x 2>&1 >/dev/null)
case $err in *"retryable, HTTP 529"*) ok=0 ;; *) ok=1 ;; esac
check "error class and status are reported" [ $ok -eq 0 ]

events=$("$bin" faux/faux --script tests/faux/tool_call.json --events x 2>/dev/null)
check "events end in exactly one done" [ "$(printf '%s\n' "$events" | grep -c '"type":"done"')" -eq 1 ]
if command -v jq >/dev/null; then
    check "every event line is JSON" sh -c 'printf "%s\n" "$1" | jq -e . >/dev/null' _ "$events"
fi

env -u ANTHROPIC_API_KEY "$bin" anthropic/claude-opus-5-5 hi >/dev/null 2>&1
check "a missing key exits with 1" [ $? -eq 1 ]

"$bin" --list | grep -q 'anthropic/claude-opus-5-5'
check "--list shows the catalog" [ $? -eq 0 ]

# ---- pytte ----
XDG_STATE_HOME=$(mktemp -d)
XDG_CONFIG_HOME="$XDG_STATE_HOME/config"
export XDG_STATE_HOME XDG_CONFIG_HOME
unset PYTTE_MODEL PYTTE_EFFORT

out=$("$pytte" -p -m faux/faux --script tests/faux/hello.json hi 2>/dev/null)
check "pytte -p prints the answer" [ "$out" = "Hello from the faux provider! Nothing here touched the network." ]
id=$(ls "$XDG_STATE_HOME/pytte/sessions" | tail -1)
check "effort starts at medium" grep -q '"reasoning":"medium"' "$XDG_STATE_HOME/pytte/sessions/$id/log.jsonl"
"$pytte" -p -m faux/faux --effort default --script tests/faux/hello.json hi >/dev/null 2>&1
check "default is not an effort choice" [ $? -eq 1 ]
mkdir -p "$XDG_CONFIG_HOME/pytte"
for invalid_effort in default ""; do
    printf '{"reasoning":"%s"}\n' "$invalid_effort" >"$XDG_CONFIG_HOME/pytte/config.json"
    "$pytte" -p -m faux/faux --script tests/faux/hello.json hi >/dev/null 2>&1
    check "rejects invalid saved effort [$invalid_effort]" [ $? -eq 1 ]
done
rm "$XDG_CONFIG_HOME/pytte/config.json"

out=$("$pytte" log 2>/dev/null)
case $out in *"> hi"*"Hello from the faux provider"*) ok=0 ;; *) ok=1 ;; esac
check "pytte log shows the transcript" [ $ok -eq 0 ]

"$pytte" -p -c </dev/null >/dev/null 2>&1
check "resuming an idle session exits 0" [ $? -eq 0 ]

out=$("$pytte" -p -m faux/faux --script tests/faux/tool_call.json "echo hi" 2>/dev/null)
check "tool calls get an error result, then the run goes on" [ "$out" = "I'll echo that.
The echo tool said: hi" ]

out=$(PYTTE_PERMISSION_MODE=auto "$pytte" -p -m faux/faux --script tests/faux/big_output.json "count" 2>/dev/null)
check "a cut output can be paged" [ "$out" = "Paged through the output." ]
if command -v jq >/dev/null; then
    id=$(ls "$XDG_STATE_HOME/pytte/sessions" | tail -1)
    results=$("$pytte" log "$id" --json | jq -c 'select(.kind == "tool_result")')
    sum=$(seq 1 10000 | sha256sum | cut -c1-64)
    check "...its whole output is a blob" sh -c 'printf "%s" "$1" | jq -se ".[0].data.details | .blob == \"sha256:$2\" and .size == 48894 and .exit == 0" >/dev/null' _ "$results" "$sum"
    blob="$XDG_STATE_HOME/pytte/sessions/$id/blobs/$(echo "$sum" | cut -c1-2)/$(echo "$sum" | cut -c3-)"
    check "...stored under its hash" [ "$(sha256sum <"$blob" | cut -c1-64)" = "$sum" ]
    check "...the result says how to read it" sh -c 'printf "%s" "$1" | jq -se ".[0].data.content | contains(\"read_output pages through them\")" >/dev/null' _ "$results"
    check "...read_output returns the asked bytes" sh -c 'printf "%s" "$1" | jq -se ".[1].data.content == \"8001\n8002\n8003\n8004\n8005\n8006\n…[bytes 38893-38923 of 48894; read on with offset 38923]\"" >/dev/null' _ "$results"
    check "...and refuses an unknown call" sh -c 'printf "%s" "$1" | jq -se ".[2].data.is_error == true" >/dev/null' _ "$results"
    PYTTE_PERMISSION_MODE=auto "$pytte" -p -m faux/faux --context-window 8000 --script tests/faux/old_output.json count >/dev/null 2>&1
    PYTTE_PERMISSION_MODE=auto "$pytte" -p -c --context-window 8000 --script tests/faux/old_output.json "page it" >/dev/null 2>&1
    check "read_output finds a call from before a compaction" sh -c '"$1" log --json | jq -se "(map(select(.kind == \"epoch\")) | .[0].data.n == 2) and (map(select(.kind == \"tool_result\")) | .[0].data.content == \"10000\n\")" >/dev/null' _ "$pytte"
fi

printf '%s' '{"turns": [[{"status": 529}, {"status": 400, "body": {"message": "bad request"}}]]}' \
    >"$XDG_STATE_HOME/fail.json"
"$pytte" -p -m faux/faux --script "$XDG_STATE_HOME/fail.json" --retry-base-ms 1 x >/dev/null 2>&1
check "a run that fails after a retry exits with 10" [ $? -eq 10 ]

n=$(ls "$XDG_STATE_HOME/pytte/sessions" | wc -l)
err=$("$pytte" -p -m anthropic hi 2>&1 >/dev/null)
case $err in *"no model given"*) ok=0 ;; *) ok=1 ;; esac
check "a bare provider is refused" [ $ok -eq 0 ]
check "...before a session is made" [ "$(ls "$XDG_STATE_HOME/pytte/sessions" | wc -l)" -eq "$n" ]

env -u ANTHROPIC_API_KEY "$pytte" -p -m anthropic/claude-opus-5-5 hi >/dev/null 2>&1
check "a missing key exits with 1" [ $? -eq 1 ]
env -u ANTHROPIC_API_KEY "$pytte" -p -c -m faux/faux --script tests/faux/hello.json </dev/null >/dev/null 2>&1
check "...and the session resumes with another model" [ $? -eq 0 ]

n=$(ls "$XDG_STATE_HOME/pytte/sessions" | wc -l)
"$pytte" -p -m faux/faux --script tests/faux/slow.json "list" >/dev/null 2>&1 &
owner=$!
for i in $(seq 50); do # until the owner has made its session (slow under load)
    [ "$(ls "$XDG_STATE_HOME/pytte/sessions" | wc -l)" -gt "$n" ] && break
    sleep 0.1
done
sleep 0.2
err=$("$pytte" -p -c --script tests/faux/slow.json </dev/null 2>&1 >/dev/null)
case $err in *"in use by"*"pid $owner"*) ok=0 ;; *) ok=1 ;; esac
check "a second process is refused, naming the owner" [ $ok -eq 0 ]
"$pytte" ls | grep -q "running (pid $owner) .* list$"
check "ls shows the owner running" [ $? -eq 0 ]
kill -INT $owner
wait $owner
check "Ctrl-C detaches with 130" [ $? -eq 130 ]
"$pytte" ls | head -1 | grep -q "closed .* list$"
check "...and ls shows it closed, with its run unfinished" [ $? -eq 0 ]
out=$("$pytte" -p -c --script tests/faux/slow.json </dev/null 2>/dev/null)
case $out in *"plenty of time to kill the process"*) ok=0 ;; *) ok=1 ;; esac
check "...and the detached session resumes to the end" [ $ok -eq 0 ]
"$pytte" ls | head -1 | grep -q "idle .* list$"
check "...and ls shows it idle" [ $? -eq 0 ]
PYTTE_PERMISSION_MODE=ask "$pytte" -p -m faux/faux --script tests/faux/tools.json "may I" >/dev/null 2>&1
check "a tool call waiting for approval exits with 3" [ $? -eq 3 ]
"$pytte" ls | head -1 | grep -q "waiting .* may I$"
check "...and ls shows it waiting" [ $? -eq 0 ]
(
    XDG_STATE_HOME=$(mktemp -d)
    "$pytte" -p -m faux/faux --script tests/faux/slow.json "doomed" >/dev/null 2>&1 &
    victim=$!
    for i in $(seq 50); do
        "$pytte" ls 2>/dev/null | grep -q running && break
        sleep 0.1
    done
    kill -9 $victim
    wait $victim 2>/dev/null
    "$pytte" ls | grep -q "crashed .* doomed$"
    status=$?
    rm -rf "$XDG_STATE_HOME"
    exit $status
)
check "ls shows a killed owner as crashed" [ $? -eq 0 ]

if command -v jq >/dev/null; then
    "$pytte" -p -m faux/faux --script tests/faux/hello.json --output-format stream-json hi >"$XDG_STATE_HOME/out" 2>/dev/null
    check "stream-json prints commits" sh -c 'jq -se "length == 6 and .[0].seq == 1" "$1" >/dev/null' _ \
        "$XDG_STATE_HOME/out"
fi

out=$(echo "hi from a pipe" | "$pytte" -m faux/faux --script tests/faux/hello.json 2>/dev/null)
check "a piped prompt runs in print mode" [ "$out" = "Hello from the faux provider! Nothing here touched the network." ]
id=$(ls "$XDG_STATE_HOME/pytte/sessions" | tail -1)
"$pytte" log "$id" | grep -q "hi from a pipe"
check "...and is the newest session" [ $? -eq 0 ]
out=$("$pytte" -p -r "$id" -m faux/faux --script tests/faux/hello.json "again" 2>/dev/null)
check "-r continues a session by ID" [ "$("$pytte" log "$id" | grep -c '^> ')" -eq 2 ]
if command -v jq >/dev/null; then
    out=$("$pytte" -p -m faux/faux --script tests/faux/hello.json --output-format json hi 2>/dev/null)
    check "json prints one result object" sh -c 'printf "%s" "$1" | jq -e ".status == \"completed\" and (.result | startswith(\"Hello\"))" >/dev/null' _ "$out"
fi
abs=$(cd "$(dirname "$pytte")" && pwd)/$(basename "$pytte")
(cd "$XDG_STATE_HOME" && "$abs" -p -c </dev/null >/dev/null 2>&1)
check "-c only continues sessions of this directory" [ $? -eq 1 ]
rm -rf "$XDG_STATE_HOME"

exit $fail
