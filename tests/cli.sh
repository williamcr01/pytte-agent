#!/bin/sh
# End-to-end checks of pytte-ai and pytte on the faux provider. No network.
set -u
exec </dev/null # pytte -p reads stdin that is not a terminal
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
# Run outside any repository and home with prompt files, so this repo's
# CLAUDE.md and the user's skills stay out of the system prompts.
HOME=$XDG_STATE_HOME
export HOME
pytte=$(cd "$(dirname "$pytte")" && pwd)/$(basename "$pytte")
mkdir "$XDG_STATE_HOME/work"
ln -s "$(pwd)/tests" "$XDG_STATE_HOME/work/tests"
cd "$XDG_STATE_HOME/work" || exit 1

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

out=$("$pytte" -p -m faux/faux --script tests/faux/tool_call.json "echo hi" 2>"$XDG_STATE_HOME/err")
check "tool calls get an error result, then the run goes on" [ "$out" = "The echo tool said: hi" ]
check "...and the text before a tool call goes to stderr" grep -qx "I'll echo that." "$XDG_STATE_HOME/err"

out=$(PYTTE_PERMISSION_MODE=auto "$pytte" -p -m faux/faux --script tests/faux/big_output.json "count" 2>/dev/null)
check "a cut output runs on" [ "$out" = "Counted past the cap." ]
if command -v jq >/dev/null; then
    id=$(ls "$XDG_STATE_HOME/pytte/sessions" | tail -1)
    results=$("$pytte" log "$id" --json | jq -c 'select(.kind == "tool_result")')
    sum=$(seq 1 10000 | sha256sum | cut -c1-64)
    check "...its whole output is a blob" sh -c 'printf "%s" "$1" | jq -se ".[0].data.details | .blob == \"sha256:$2\" and .size == 48894 and .exit == 0" >/dev/null' _ "$results" "$sum"
    blob="$XDG_STATE_HOME/pytte/sessions/$id/blobs/$(echo "$sum" | cut -c1-2)/$(echo "$sum" | cut -c3-)"
    check "...stored under its hash" [ "$(sha256sum <"$blob" | cut -c1-64)" = "$sum" ]
    check "...the result names its file" sh -c 'printf "%s" "$1" | jq -se ".[0].data.content | contains(\"bytes omitted; the whole output is in $2]\")" >/dev/null' _ "$results" "$blob"
    PYTTE_PERMISSION_MODE=auto "$pytte" -p -m faux/faux --script tests/faux/background_print.json go >/dev/null 2>&1
    results=$("$pytte" log --json | jq -c 'select(.kind == "tool_result") | .data')
    bg() { sh -c 'printf "%s" "$1" | jq -se "$2" >/dev/null' _ "$results" "$1"; }
    check "a command past its yield goes to the background" bg '.[0] | (.content == "first\n\n[still running as session 8; you will be told when it ends]") and .details.session == 8 and (.is_error | not)'
    check "...a poll returns at once" bg '.[1] | .content == "\n[still running as session 8; you will be told when it ends]" and .details.ms < 1000'
    check "...its end is told in the next result" bg '.[2] | .content == "(no output)\n[pytte: background session 8 ended with exit code 3: echo first; sleep 2; echo second; exit 3; bash_poll reads its output]"'
    check "...a poll reads the rest and its exit code" bg '.[3] | .content == "second\n\n[exit code 3]" and .details.exit == 3 and .is_error'
    check "...an ended session read once is gone" bg '.[4] | (.content | startswith("No session 8:")) and .is_error'
    check "...a stopped session is no error" bg '.[6] | .content == "\n[stopped]" and (.is_error | not)'
    check "...and nothing outlives pytte" sh -c '! pgrep -f "^sh -c echo up; sleep 100" >/dev/null && ! pgrep -xf "sleep 100" >/dev/null'
    out=$(PYTTE_PERMISSION_MODE=auto "$pytte" -p -m faux/faux --script tests/faux/notify_later.json go 2>/dev/null)
    log=$("$pytte" log --json)
    check "a background command that ends during a turn is told in the next result" sh -c 'printf "%s" "$1" | jq -se "map(select(.kind == \"tool_result\")) | .[1].data.content == \"busy\n\n[pytte: background session 8 ended with exit code 0: sleep 1.5; echo late; bash_poll reads its output]\"" >/dev/null' _ "$log"
    check "...not again in a note" sh -c 'printf "%s" "$1" | jq -se "map(select(.kind == \"user\" and .data.from == \"pytte\")) | length == 0" >/dev/null' _ "$log"
    check "...and bash_poll reads it" sh -c 'printf "%s" "$1" | jq -se "map(select(.kind == \"tool_result\")) | .[-1].data.content == \"late\n\n[ended]\"" >/dev/null' _ "$log"
    out=$(PYTTE_PERMISSION_MODE=auto "$pytte" -p -m faux/faux --script tests/faux/notify_idle.json go 2>/dev/null)
    check "-p waits for a background command, which starts a turn when it ends" [ "$(printf '%s\n' "$out" | tail -1)" = "It printed late." ]
    log=$("$pytte" log --json)
    check "...from a note by pytte, once, with what it printed" sh -c 'printf "%s" "$1" | jq -se "map(select(.kind == \"user\" and .data.from == \"pytte\")) | length == 1 and (.[0].data.content | contains(\"- session 8 ended with exit code 0: sleep 1.5; echo late\n\") and contains(\"Session 8 printed:\n<output>\nlate\n</output>\n\"))" >/dev/null' _ "$log"
    out=$(PYTTE_PERMISSION_MODE=auto "$pytte" -p -m faux/faux --script tests/faux/notify_long.json go 2>/dev/null)
    log=$("$pytte" log --json)
    check "a note shows the end of a long output, from a whole line" sh -c 'printf "%s" "$1" | jq -se "map(select(.kind == \"user\" and .data.from == \"pytte\")) | .[0].data.content | test(\"Session 8 printed \\\\(its last [0-9]+ bytes; bash_poll reads what came before\\\\):\n<output>\n[0-9]+\n(.|\n)*\n3000\n</output>\")" >/dev/null' _ "$log"
    check "...and the session stays for bash_poll" sh -c 'printf "%s" "$1" | jq -se "map(select(.kind == \"tool_result\")) | .[-1].data.content | startswith(\"1\n2\n3\n\") and endswith(\"3000\n\n[ended]\")" >/dev/null' _ "$log"
    d=$XDG_STATE_HOME/files
    mkdir -p "$d" && seq 1 5000 >"$d/big.txt"
    printf '{"turns": [{"content": [{"type": "tool_call", "name": "write", "args": {"path": "%s/notes.txt", "content": "hi"}}]}, {"content": [{"type": "tool_call", "name": "read", "args": {"path": "%s/big.txt"}}]}, {"content": [{"type": "text", "text": "Read."}]}]}' \
        "$d" "$d" >"$d/script.json"
    PYTTE_PERMISSION_MODE=auto "$pytte" -p -m faux/faux --context-window 4000 --script "$d/script.json" "write, then read" >/dev/null 2>&1
    check "a compaction lists the files read and modified" sh -c '"$1" log --json | jq -se "map(select(.kind == \"epoch\")) | .[0].data.files == {\"read\": [\"$2/big.txt\"], \"modified\": [\"$2/notes.txt\"]}" >/dev/null' _ "$pytte" "$d"
fi

# ---- prompt files and skills ----
if command -v jq >/dev/null; then
    pa=$pytte
    hello=$(pwd)/tests/faux/hello.json
    old=$(pwd)/tests/faux/old_output.json
    p=$XDG_STATE_HOME/project
    u=$XDG_CONFIG_HOME/pytte
    skill() { # dir name description [extra frontmatter]
        mkdir -p "$1/skills/$2"
        printf -- '---\nname: %s\ndescription: %s\n%b---\n%s body\n' "$2" "$3" "${4:-}" "$2" >"$1/skills/$2/SKILL.md"
    }
    run() { (cd "$p" && "$pa" -p "$@" >/dev/null 2>&1); }
    mkdir -p "$p/.pytte" "$u"
    echo "user system" >"$u/SYSTEM.md"
    echo "project system" >"$p/.pytte/SYSTEM.md"
    echo "user append" >"$u/APPEND_SYSTEM.md"
    skill "$u" shared "from the user"
    skill "$u" mine "only the user's"
    skill "$p/.agents" shared "from the project"
    skill "$p/.pytte" deploy "ships it" "disable-model-invocation: true\n"
    epoch() { (cd "$p" && "$pa" log --json | jq -se "map(select(.kind == \"epoch\")) | .[$1].data | $2" >/dev/null); }
    user() { (cd "$p" && "$pa" log --json | jq -se "map(select(.kind == \"user\")) | .[-1].data.content | $1" >/dev/null); }
    run -m faux/faux --script "$hello" --append-system-prompt "flag append" hi
    check "the project's SYSTEM.md replaces the user's, then the appends" \
        epoch 0 '.system | startswith("project system\n\nuser append\n\nflag append\n\n# Skills")'
    check "...the project's skill wins, and manual ones are not listed" \
        epoch 0 '.skills | map(.name + ": " + .description) == ["mine: only the user'"'"'s", "shared: from the project"]'
    run -m faux/faux --script "$hello" 'use $deploy, then $shared. $5 is $nope'
    check '$name puts the skills before the message' \
        user 'startswith("<skill name=\"deploy\" path=\"'"$p"'/.pytte/skills/deploy/SKILL.md\">\ndeploy body\n</skill>\n\n<skill name=\"shared\"") and endswith("</skill>\n\nuse $deploy, then $shared. $5 is $nope")'
    run -m faux/faux --script "$hello" '/skill:mine  do it'
    check "/skill:name puts the skill before the rest" user 'test("^<skill name=\"mine\"[^>]*>\nmine body\n</skill>\n\ndo it$")'
    n=$(find "$XDG_STATE_HOME/pytte/sessions" -mindepth 1 -maxdepth 1 | wc -l)
    run -m faux/faux --script "$hello" '/skill:nope x'
    check "an unknown /skill: exits with 1" [ $? -eq 1 ]
    check "...and leaves no session" [ "$(find "$XDG_STATE_HOME/pytte/sessions" -mindepth 1 -maxdepth 1 | wc -l)" -eq "$n" ]
    printf '{"skills": {"auto": false}}\n' >"$u/config.json"
    run -m faux/faux --script "$hello" hi
    check "skills.auto off lists no skills" epoch 0 '(.skills == []) and (.system | contains("# Skills") | not)'
    rm "$u/config.json"
    PYTTE_PERMISSION_MODE=auto run -m faux/faux --append-system-prompt "flag append" --script "$old" count
    echo "changed append" >"$p/.pytte/APPEND_SYSTEM.md"
    skill "$p/.pytte" fresh "added later"
    PYTTE_PERMISSION_MODE=auto run -c --context-window 8000 --script "$old" "page it"
    check "a compaction reads the prompt files and skills again, keeping the flags" \
        epoch -1 '(.n == 2) and (.system | startswith("project system\n\nchanged append\n\nflag append")) and (.skills | map(.name) | index("fresh"))'
    r=$XDG_STATE_HOME/repo
    mkdir -p "$r/.git" "$r/src/tui"
    echo "above the repo" >"$XDG_STATE_HOME/AGENTS.md"
    echo "user agents" >"$u/AGENTS.md"
    echo "root agents" >"$r/AGENTS.md"
    echo "claude in src" >"$r/src/CLAUDE.md"
    echo "tui agents" >"$r/src/tui/AGENTS.md"
    echo "not this claude" >"$r/src/tui/CLAUDE.md"
    rm -rf "$p"
    p=$r/src/tui
    run -m faux/faux --script "$hello" hi
    check "AGENTS.md: the user's, then from the git root down, CLAUDE.md where there is none" \
        epoch 0 '.system | test("user agents\n\n## '"$r"'/AGENTS.md\n\nroot agents\n\n## '"$r"'/src/CLAUDE.md\n\nclaude in src\n\n## '"$p"'/AGENTS.md\n\ntui agents")'
    check "...and nothing from above the repository" epoch 0 '.system | (contains("above the repo") or contains("not this claude")) | not'
    head -c 40000 /dev/zero | tr '\0' x >"$r/AGENTS.md"
    run -m faux/faux --script "$hello" hi
    check "...over 32 KiB the outermost are cut first" \
        epoch 0 '.system | contains("[cut to ") and (contains("user agents") | not)'
    check "...and the nearest are kept whole" epoch 0 '.system | contains("tui agents")'
    rm -rf "$r" "$XDG_STATE_HOME/AGENTS.md" "$u/AGENTS.md" "$u/SYSTEM.md" "$u/APPEND_SYSTEM.md" "$u/skills"
fi

printf '%s' '{"turns": [[{"status": 529}, {"status": 400, "body": {"message": "bad request"}}]]}' \
    >"$XDG_STATE_HOME/fail.json"
"$pytte" -p -m faux/faux --compact-model nope/none x >/dev/null 2>&1
check "an unknown compaction model is refused" [ $? -eq 1 ]

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

"$pytte" -p -m faux/faux --script tests/faux/slow.json "list" >/dev/null 2>&1 &
owner=$!
for i in $(seq 50); do # until the owner has committed its session (slow under load)
    "$pytte" ls | grep -q "running (pid $owner)" && break
    sleep 0.1
done
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
newest=$(ls "$XDG_STATE_HOME/pytte/sessions" | tail -1)
check "the first prompt names the session" [ "$(cat "$XDG_STATE_HOME/pytte/sessions/$newest/name")" = "may I" ]
if command -v git >/dev/null 2>&1; then
    (
        r=$XDG_STATE_HOME/repo
        script=$(pwd)/tests/faux/hello.json
        mkdir "$r" && cd "$r" || exit 1
        git init -q . && git -c user.name=t -c user.email=t@t commit -q --allow-empty -m init || exit 1
        err=$("$pytte" -p -w feat -m faux/faux --script "$script" hi 2>&1 >/dev/null)
        check "-w starts in a new worktree" [ "$(echo "$err" | head -1)" = "pytte: made worktree $r/.pytte/worktrees/feat on branch worktree-feat" ]
        id=$(echo "$err" | sed -n 's/^session //p')
        grep -q "\"cwd\":\"$r/.pytte/worktrees/feat\"" "$XDG_STATE_HOME/pytte/sessions/$id/log.jsonl"
        check "...and its session works there" [ $? -eq 0 ]
        err=$("$pytte" -p --worktree=feat -m faux/faux --script "$script" hi 2>&1 >/dev/null | head -1)
        check "...and --worktree=feat goes in again" [ "$err" = "pytte: in worktree $r/.pytte/worktrees/feat" ]
        check "...and the checkout stays clean" [ -z "$(git status --porcelain)" ]
        "$pytte" -p -w -c hi >/dev/null 2>&1
        check "...but not with -c" [ $? -eq 1 ]
    )
fi
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
printf 'line one\nline two\n' | "$pytte" -p -m faux/faux --script tests/faux/hello.json "sum up" >/dev/null 2>&1
id=$(ls "$XDG_STATE_HOME/pytte/sessions" | tail -1)
"$pytte" log "$id" | tr '\n' '|' | grep -q "> <stdin>|line one|line two|</stdin>||sum up|"
check "piped stdin goes before the prompt, as data" [ $? -eq 0 ]
check "...and the prompt names the session" [ "$(cat "$XDG_STATE_HOME/pytte/sessions/$id/name")" = "sum up" ]
out=$( (sleep 2; echo never) | "$pytte" -p --no-stdin -m faux/faux --script tests/faux/hello.json "sum up" 2>/dev/null)
check "--no-stdin leaves an open stdin unread" [ "$out" = "Hello from the faux provider! Nothing here touched the network." ]
id=$(ls "$XDG_STATE_HOME/pytte/sessions" | tail -1)
"$pytte" log "$id" | grep -q "^> sum up$"
check "...and sends the prompt alone" [ $? -eq 0 ]
rc=$({ ("$pytte" -p -m faux/faux --script tests/faux/slow.json --output-format stream-json x 2>/dev/null; echo $? >&3) | true; } 3>&1)
check "a closed stdout stops the run with 141" [ "$rc" -eq 141 ]
"$pytte" ls | head -1 | grep -q "closed .* x$"
check "...and leaves it resumable" [ $? -eq 0 ]
out=$("$pytte" -p -r "$id" -m faux/faux --script tests/faux/hello.json "again" 2>/dev/null)
check "-r continues a session by ID" [ "$("$pytte" log "$id" | grep -c '^> ')" -eq 2 ]
if command -v jq >/dev/null; then
    out=$("$pytte" -p -m faux/faux --script tests/faux/hello.json --output-format json hi 2>/dev/null)
    check "json prints one result object" sh -c 'printf "%s" "$1" | jq -e ".status == \"completed\" and (.result | startswith(\"Hello\"))" >/dev/null' _ "$out"
fi
(cd "$XDG_STATE_HOME" && "$pytte" -p -c </dev/null >/dev/null 2>&1)
check "-c only continues sessions of this directory" [ $? -eq 1 ]
rm -rf "$XDG_STATE_HOME"

exit $fail
