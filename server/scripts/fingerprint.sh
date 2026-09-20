#!/usr/bin/env bash
# Prints a fingerprint of whole seeded matches: the final state hash, the final snapshot checksum, spells cast and placements, for two seeds
# with the generic shop and two with the real 30-champion roster only. Two builds (or two operating systems) that play the same game have
# identical fingerprints; CI compares them. Usage: scripts/fingerprint.sh path/to/w2f_demo
set -euo pipefail
demo="${1:?usage: fingerprint.sh path/to/w2f_demo}"
for seed in 2024 31337; do
  for mode in generic roster; do
    if [ "$mode" = roster ]; then export W2F_ROSTER_ONLY=1; else unset W2F_ROSTER_ONLY || true; fi
    echo "== seed=$seed shop=$mode"
    # (tr: Windows text-mode stdout writes CRLF)
    "$demo" "$seed" | tr -d '\r' | grep -E '^Finished after|^Pool integrity|^Final snapshot|^Spells cast|^Final placements|^Snapshot / restore|^Mother Nature gifts'
  done
done
