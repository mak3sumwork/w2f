# FEEDBACK V6

The designer's requests after W2F DEMO 1.5.1, 2026-09-28 (a brainstorm: traits and champions compared with TFT, packaging, a client overhaul). Kept word for word below,
with the answers given along the way. The changes made in answer are listed in `updates/update-1.6.md`.

## As written

> lets brainstorm. How our traits and champs are right now ? we have like 50 but in tft theres like 60 right. compare the traits and champs and lets see if we have any problems with that. and then i think we should package the client for mac and the game starts like that yk. if we have problems with traits or champions like is it enoguh we work on it if not we going to balance

> breakpoints makes sense because thet are prismatic and shouldnt be easy to go them normally. when you give an emblem the board becomes powerful thats why only emblems work. coregon needing 2 emblems are really balanced cause at 8 its really powerfull trait. idk about omnilium but protector made only for them. they are duos yk like i thought it would be good for najimi + 2 units like those type of boards its a good option . let me know ur opinions. we need tanks yeah kinda.check tft's traits and check what we need really

> i approve . while packaging the mac app are we packaging the game or client ? how does that work. and we need client overhaul to lets talk about that too

> for now it is in my mac we building the game. then we will look at the online server. Ok to that haul. i want it looking good. im ok wit champs. for client it bothers me how cheap it look. we need better background. better profile section. and we need better splash arts for champs its so important. rn they look slop bs.

## Decisions
* Top breakpoints stay emblem-only (prismatic chase); Coregons 8 stays a 2-emblem goal; Protector stays the Les + Lum duo.
* The roster pass in the design doc section 2D is approved: Pulsar, Rampart, Skarn, Maren, Vector; Faire and Lich lose Sorcerer (Lich -> Mystic); Omnilium tag removed; Protector 1/2.
* The app is client + game in one. For now the server runs on the designer's Mac (the app starts its own); an online server comes later.
* Splash arts: **both** -- cinematic in-engine renders now for every champion, plus one image prompt per champion so painted versions can replace them later.

## Read as
1. Implement the 5 new champions + re-tags end to end (data, engine, tests, balance, Mixamo models, FX, portraits).
2. Client overhaul: it looks cheap. A much better background, a much better Profile, and good splash arts (the most important part).
3. Package the Mac app so it starts the game by itself (local server inside the app).
