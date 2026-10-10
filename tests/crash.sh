#!/usr/bin/env bash
# Real-process crash test (section 22.3): kill -9 `pytte -p` and every
# `pytte -p -c` at random moments until one finishes on its own, then check
# that the session ended exactly as an uninterrupted one would.
#
#   tests/crash.sh [BIN] [ROUNDS]
set -u
bin=${1:-build/pytte}
rounds=${2:-20}
script=tests/faux/crash.json
want="No tools are available yet, so I cannot list the files. This answer streams slowly enough to be killed halfway."
fail=0 kills=0

nap() { # a random pause of up to $1 ms
    local ms=$((RANDOM % $1))
    sleep "$((ms / 1000)).$(printf %03d $((ms % 1000)))"
}

# Run a command, kill -9 it after a random pause; returns its exit status
# (137 when killed).
run_and_kill() {
    "$@" >/dev/null 2>&1 &
    local p=$!
    nap 1200
    kill -9 $p 2>/dev/null && kills=$((kills + 1))
    { wait $p; } 2>/dev/null
}

for round in $(seq "$rounds"); do
    export XDG_STATE_HOME
    XDG_STATE_HOME=$(mktemp -d)
    run_and_kill "$bin" -p -m faux/faux --script "$script" "list the files"
    status=$?
    for attempt in $(seq 40); do
        [ "$status" -eq 0 ] && break
        if ! "$bin" log >/dev/null 2>&1 || [ -z "$("$bin" log --json)" ]; then
            # killed before the first commit: the session never existed
            rm -rf "$XDG_STATE_HOME/pytte"
            run_and_kill "$bin" -p -m faux/faux --script "$script" "list the files"
        else
            run_and_kill "$bin" -p -c --script "$script"
        fi
        status=$?
    done
    log=$(echo "$XDG_STATE_HOME"/pytte/sessions/*/log.jsonl)
    last=$("$bin" log --json | grep '"kind":"assistant"' | tail -1)
    problems=""
    [ "$status" -eq 0 ] || problems="$problems never finished (status $status);"
    case $last in *"\"text\":\"$want\""*) ;; *) problems="$problems wrong final answer;" ;; esac
    [ "$("$bin" log --json | grep -c '"kind":"user"')" -eq 1 ] || problems="$problems not one user entry;"
    [ -z "$(ls "$(dirname "$log")/live")" ] || problems="$problems live files left;"
    if command -v jq >/dev/null; then
        jq -se 'map(.seq) == [range(1; length + 1)]' "$log" >/dev/null || problems="$problems bad seqs;"
        jq -se '[.[].w[] | select(.t == "claim")] | last | .event == "close"' "$log" >/dev/null ||
            problems="$problems not closed;"
    fi
    if [ -n "$problems" ]; then
        echo "  FAIL round $round:$problems (kept $XDG_STATE_HOME)"
        fail=1
    else
        rm -rf "$XDG_STATE_HOME"
    fi
done
echo "  crash: $rounds rounds, $kills kills, $([ $fail -eq 0 ] && echo ok || echo FAILED)"
exit $fail
