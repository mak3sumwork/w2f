#!/usr/bin/env bash
# Runs N independent w2f_server processes (one match each) on consecutive ports, each with its own autosave, and restarts any that crashes -- resuming its
# match from the autosave. For a machine without Docker; put a TLS proxy in front (server/docs/deploy.md).
#   scripts/run_matches.sh 4 7777 [extra w2f_server arguments, e.g. --bots 0 --join-code secret]
set -u
count="${1:?usage: run_matches.sh COUNT BASE_PORT [server args...]}"
base="${2:?usage: run_matches.sh COUNT BASE_PORT [server args...]}"
shift 2
extra=("$@")
server="${W2F_SERVER:-$(dirname "$0")/../server/build/w2f_server}"
dir="${W2F_SAVE_DIR:-/tmp/w2f}"
mkdir -p "$dir"
run() {
  local port="$1" save="$dir/match-$1.w2fsave"
  while true; do
    if [ -f "$save" ]; then "$server" --port "$port" --autosave "$save" --resume "$save" ${extra[@]+"${extra[@]}"} || true
    else "$server" --port "$port" --autosave "$save" ${extra[@]+"${extra[@]}"} || true; fi
    sleep 1
  done
}
for i in $(seq 0 $((count - 1))); do run $((base + i)) & done
trap 'kill $(jobs -p) 2>/dev/null' EXIT
wait
