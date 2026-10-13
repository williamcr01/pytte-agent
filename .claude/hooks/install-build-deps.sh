#!/bin/sh
# SessionStart hook: install the system packages pytte needs that the
# Claude Code cloud image lacks. No-op locally and when already installed.
#   libcurl4-openssl-dev  curl/curl.h for `make`, `make test`, bench targets
#   libclang-rt-18-dev    libFuzzer/ASan runtimes for `make fuzz`
[ "$CLAUDE_CODE_REMOTE" = "true" ] || exit 0

pkgs="libcurl4-openssl-dev libclang-rt-18-dev"
missing=""
for p in $pkgs; do
	dpkg-query -W -f='${Status}' "$p" 2>/dev/null | grep -q "ok installed" ||
		missing="$missing $p"
done
[ -n "$missing" ] || exit 0

echo "installing build deps:$missing" >&2
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null &&
	apt-get install -y -qq $missing >/dev/null
