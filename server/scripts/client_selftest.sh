#!/bin/bash
# The UE client's screens, tested by driving the real widgets (demo 1.5.1; updates/update-1.5.1.md).
#
#   scripts/client_selftest.sh [path/to/work2fightgame]
#
# Starts a queue server with a copy of tests' accounts (Tester with one match in the history, Rival), client A running -w2fclienttest=CTnnnn, and a
# headless client B logged in as Rival that sends A a friend request when A is ready for one (twice). Prints A's results (Saved/W2F/clienttest.txt);
# A's screenshots land in <project>/Saved/Screenshots/MacEditor. Needs: make server, the UE editor built (Build.sh work2fightgameEditor Mac Development),
# The accounts come from tests/data/client_selftest_accounts.json (Tester with one finished match, Rival; both password secret1), copied fresh each run.
set -u
HERE="$(cd "$(dirname "$0")/.." && pwd)"
P="${1:-$HOME/Desktop/work2fightgame/work2fightgame}"
UE="${UE:-/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor}"
T="$(mktemp -d)"
ACCOUNTS="$T/accounts.json"
cp "$HERE/tests/data/client_selftest_accounts.json" "$ACCOUNTS"
NAME=CT$((RANDOM % 9000 + 1000))
PORT=7777

"$HERE/build/w2f_server" --queue --fast --fill-seconds 5 --port $PORT --accounts "$ACCOUNTS" > "$T/server.log" 2>&1 &
SERVER=$!
sleep 1
rm -rf "$P/Saved/Screenshots/MacEditor" "$P/Saved/W2F/clienttest.txt"
[ -f "$P/Saved/W2F/account.json" ] && mv "$P/Saved/W2F/account.json" "$T/account.json.bak"   # the developer's own login, put back at the end

"$UE" "$P/work2fightgame.uproject" /Game/W2F/Maps/L_Viewer -game -windowed -ResX=1600 -ResY=900 -w2fquality=1 -w2flive -w2fclienttest=$NAME -abslog="$T/a.log" > /dev/null 2>&1 &
A=$!
waitfor() { for _ in $(seq 900); do grep -q "$1" "$T/a.log" 2>/dev/null && return 0; kill -0 $A 2>/dev/null || return 1; sleep 1; done; return 1; }
rival() { "$UE" "$P/work2fightgame.uproject" /Game/W2F/Maps/L_Viewer -game -nullrhi -nosound -w2flive -w2flogin=Rival:secret1 -w2ffriend=$NAME -abslog="$T/b$1.log" > /dev/null 2>&1 & echo $!; }

B=""
waitfor "PASS: CANCEL takes the request back" && B=$(rival 1)
waitfor "MY PROFILE goes back"; [ -n "$B" ] && { kill $B; sleep 3; kill -9 $B; } 2>/dev/null
B=""
waitfor "PASS: the x removes the friend" && B=$(rival 2)
waitfor "declines it"; [ -n "$B" ] && { kill $B; sleep 3; kill -9 $B; } 2>/dev/null
for _ in $(seq 600); do kill -0 $A 2>/dev/null || break; sleep 1; done
kill -9 $A 2>/dev/null
kill $SERVER 2>/dev/null
rm -f "$P/Saved/W2F/account.json"
[ -f "$T/account.json.bak" ] && mv "$T/account.json.bak" "$P/Saved/W2F/account.json"
cat "$P/Saved/W2F/clienttest.txt" 2>/dev/null
echo "logs: $T"
grep -q "CLIENTTEST FAIL" "$P/Saved/W2F/clienttest.txt" 2>/dev/null && exit 1
exit 0
