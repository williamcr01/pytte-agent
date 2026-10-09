#!/bin/sh
# End-to-end checks of pytte-ai on the faux provider. No network.
set -u
bin=${1:-build/pytte-ai}
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

exit $fail
