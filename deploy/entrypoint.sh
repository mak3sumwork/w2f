#!/bin/sh
# Starts w2f_server for the container. If an autosave from a match that was still running exists, it RESUMES it (the players reconnect with their
# tokens); a finished match deletes its autosave, so a normal restart begins with a fresh lobby. W2F_RESUME=0 forces a fresh start.
# Extra arguments (docker run ... --bots 7 --players 8) are passed on. W2F_JOIN_CODE turns on a private server (?code=...).
SAVE="${W2F_SAVE:-/var/lib/w2f/match.w2fsave}"
set -- --data /app/data --bind 0.0.0.0 --port "${W2F_PORT:-7777}" --autosave "$SAVE" "$@"
if [ -n "$W2F_JOIN_CODE" ]; then set -- "$@" --join-code "$W2F_JOIN_CODE"; fi
if [ -f "$SAVE" ] && [ "$W2F_RESUME" != "0" ]; then
  echo "found $SAVE: resuming the match"
  set -- "$@" --resume "$SAVE"
fi
exec /app/w2f_server "$@"
