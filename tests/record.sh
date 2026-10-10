#!/bin/sh
# Record live response fixtures from the real providers. Costs a little
# money and needs the API keys. Afterwards run `make fixtures-update`,
# review the new .expect.json files, and commit.
#
# usage: tests/record.sh [anthropic] [openai] [openrouter]
#   MODEL=provider/model overrides the model for every provider given.
set -u
bin=build/pytte-ai
[ -x "$bin" ] || { echo "build first: make" >&2; exit 1; }

rec() {
    dir=$1 name=$2
    shift 2
    echo "== $dir/live_$name"
    "$bin" "$@" --record "tests/fixtures/$dir/live_$name.sse" </dev/null
    echo
}

for p in ${@:-anthropic openai openrouter}; do
    case $p in
    anthropic) m=anthropic/claude-haiku-5-5 key=ANTHROPIC_API_KEY ;;
    openai) m=openai/gpt-6-luna key=OPENAI_API_KEY ;;
    openrouter) m=openrouter/anthropic/claude-haiku-5.5 key=OPENROUTER_API_KEY ;;
    *) echo "unknown provider: $p" >&2; exit 1 ;;
    esac
    m=${MODEL:-$m}
    rec "$p" text "$m" "What is 25*18? Answer with just the number."
    rec "$p" thinking "$m" --reasoning high "Is 1013 prime? Think it through, then answer yes or no."
    rec "$p" tool_call "$m" --tools tests/tools/echo.json --turns 2 "Use the echo tool to echo 'hi', then tell me what it said."
    rec "$p" parallel_tools "$m" --tools tests/tools/files.json "Read a.txt and b.txt. Call read for both files at once, in parallel."
    rec "$p" max_tokens "$m" --max-tokens 40 "Write a long story about a lighthouse keeper."
    env "$key=invalid-key-for-recording" "$bin" "$m" hi --record "tests/fixtures/$p/live_http_401.sse"
done
echo "now: make fixtures-update && git diff --stat"
