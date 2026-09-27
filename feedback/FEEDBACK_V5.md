# FEEDBACK V5

The designer's request after W2F DEMO 1.4, 2026-09-26. Kept word for word below, with the answers to the three questions asked before building.
The changes made in answer are listed in `updates/update-1.5.md`.

## As written

> lets make profile friends and the other client demo and finish them. and package it. make it playable make it an app. just for macOS now. disable the cursor update for now. add match history to client. make 7 bots has usernames. make a rank system.

## Questions asked, and the answers
* Where does the game server run for the app? **Online server only** (the app connects to one server; no server inside the app).
* How do players log in? **Username + password** (a login screen; passwords stored hashed on the server).
* Which matches change the rank? **A new Ranked queue** (TFT-style tiers, LP by placement; Solo vs AI and Normal stay unranked; AI fills empty seats).

## Read as
1. The game client: accounts (register / log in with a password), **Profile** (rank, stats, **match history**), **Friends** (add by username, requests, online / in-match status), **Collection** (champions, traits, items) -- finished, no "coming soon" pages.
2. A **rank system**: a Ranked queue, tiers Iron .. Challenger with divisions and LP by placement.
3. The AI players have **usernames**.
4. The custom cursor is switched off for now.
5. A **macOS app**: packaged, double-click to play (it connects to the server address set on the login screen).
