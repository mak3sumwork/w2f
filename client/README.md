# W2F browser test client

One file, `index.html`: no build step, no dependencies. It is a **dumb viewer** like the future UE5 client: it draws what the server sends and sends commands; every rule is decided by the server.

## Play 1 human vs 7 AI
```
cd server
make server
./build/w2f_server --bots 7            # real timers: planning 30 s, combat 40 s, Mother Nature 20 s (a match is ~30 minutes)
./build/w2f_server --bots 7 --fast     # short phases (planning 4 s, combat 6 s): for trying things quickly
```
Then open `client/index.html` in Chrome / Safari / Firefox (double-click it) and press **Connect** (default `ws://localhost:7777/`; the address box is editable).
The match starts the moment you connect. After it ends the server reopens its lobby after ~20 s: press "Connect to a new match".
Useful server options: `--seed N` (the same seed = the same shops, gifts and bot rolls), `--port N`.

## Using it
* **Rules you will notice**: the board holds as many units as your level (level 1 = 1 unit ... level 10 = 10). Round 1 has no shop: everyone is dealt one free random 1-cost unit (it is on your bench: put it on the board). The shop opens in round 2.
* **Shop**: click a card to buy. **Reroll** `D`, **Buy XP** `F`. The shop is closed in round 1 and in Mother Nature rounds, and **open while units fight** (a champion bought then goes to the bench; the board is locked: only bench units can be sold, moved or equipped).
* **Units**: click a unit (bench or board), then a bench slot or a board cell to move it (swaps with what is there). The row at the top of the board is the front row. **Sell** `E` (or the button in "Selected").
* **Drag and drop**: drag a champion onto a cell or bench slot to move it, onto the *Shop* panel to sell it, and an item from the bag onto a champion to equip it (or click both). The red dashed **Item Remover** takes all items off the champion you drop it on.
* **Items**: click an item in the bag, then a unit. Two components that combine become one finished item (hover an item to see its recipes). Click an item chip in "Selected" to take it off.
* **Mother Nature** (every 3rd round): a window offers 2 gifts; click one. If time runs out the first is taken for you.
* **Players** (left): everyone's health / level / streak; click a player to look at their (public) board and synergies.
* **Battle**: your fight starts playing when the Combat phase begins (1x / 2x / 4x / pause / restart / skip). It shows the presentation data: projectiles in flight, swing and cast anticipation, spell areas, typed damage-over-time colours, and the OVERTIME banner (4x speed until a team is wiped out). "This round's fights" lists every fight of the round: "Load" fetches another one to watch.
* The connection re-establishes itself with your reconnect token if it drops during a match.

Synergy counts and sell values shown in the client are informational (computed from the catalog); the server decides the real ones.
`window.W2F` exposes the client state in the browser console (`W2F.G`).
