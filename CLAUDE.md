# CLAUDE.md — W2F project memory

W2F is a **headless, deterministic, integer-only C++17 authoritative server ("the brain")** for a TFT-style auto battler. It will later be wrapped by
Unreal Engine 5 as a "dumb viewer": the server decides everything, the client only replays what it is told. Repo: `github.com/mak3sumwork/w2f`, branch `main`.
All code lives in `server/`. The single canonical design source is `W2F_GameDesignDocument.md` (root). Where it is silent, the data marks the guess `ASSUMED`
and `server/docs/content-notes.md` lists it.

Current focus: **rules engine + combat simulation, network server, content.** Platform order: macOS first (verified locally), then Windows/Linux (CI),
then Android/iOS. Everything must stay UE5-friendly.

## Build & Run Commands
Run from `server/`.

| What | Command |
|---|---|
| Everything (engine tests + network tests, sanitizers on) | `make test` |
| Engine tests only (includes the isolation check) | `make test-engine` |
| Network tests only | `make test-net` |
| Engine must contain no networking | `make check-isolation` |
| Build the server | `make server` → `./build/w2f_server [--port N] [--bind ADDR] [--players 2..8] [--bots N] [--data DIR] [--seed N] [--autosave FILE] [--fast]` |
| Play 1 human vs 7 AI locally | `./build/w2f_server --bots 7` (add `--fast` for 4 s planning rounds), then open `client/index.html` in a browser and press Connect (see `client/README.md`) |
| Headless 8-player demo match | `make demo SEED=7` (`W2F_ROSTER_ONLY=1` sells only the real 30 champions; `W2F_NO_DRILL=1` skips the crash-recovery drill) |
| Clean | `make clean` |
| CMake (what CI and Windows use) | `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build && ctest --test-dir build --output-on-failure` |
| CMake options | `-DW2F_SANITIZE=ON/OFF` `-DW2F_WERROR=ON` `-DW2F_UE_COMPAT=ON` (no exceptions, no RTTI) |
| Determinism fingerprint of seeded matches | `scripts/fingerprint.sh path/to/w2f_demo` (must be identical across builds and OSes) |

* The Makefile builds with `clang++ -std=c++17 -O1 -g -Wall -Wextra -Wpedantic -Wshadow -Wconversion -fsanitize=address,undefined`. **A build with any warning is a failed build.**
* The suite currently runs ~3,900 engine checks + ~1,460 network checks; a change is not done until `make test` is green with zero warnings.
* CI: `.github/workflows/build.yml` (ubuntu clang+gcc, macOS, Windows MSVC, UE5-flags job, cross-platform determinism job). Docs: `server/docs/build.md`.
* Layout: `src/` + `include/w2f/` = engine (no networking); `net/` = sockets, WebSocket, lobby, JSON protocol; `tools/` = server + demo; `tests/` = custom CHECK-macro runner + `tests/support`; `data/` = production JSON; `client/index.html` = the single-file browser test client (no build step, no dependencies).
* Do **not** commit or push unless the user asks. End commit messages with `Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>`.

## Code Style & Architecture Guidelines
* **C++17**, modern standard library first (`std::variant`, `std::optional`, `std::string_view`, `std::array`, `std::vector`, structured bindings, `constexpr`). No third-party dependencies.
* **Ownership**: `std::unique_ptr` for owners (`std::make_unique`), plain references / `const T*` for non-owning views. **No `new`/`delete`, no `malloc`, no owning raw pointers** — the sanitized tests must show no leaks. A `const ChampionDefinition*` handed out by a database is a *view* that lives as long as the database.
* **No exceptions, no RTTI** (Unreal compiles that way; `-DW2F_UE_COMPAT=ON` builds and tests the whole code base with `-fno-exceptions -fno-rtti`). Errors are return values: `bool` + `std::string* error`, `ActionResult` enums, `nullptr` from loaders. No `throw`, `try`, `dynamic_cast`, `typeid`.
* **Integers only** in the engine: no `float`/`double`, no platform-dependent behaviour, no unordered-container iteration, no `std::random`. Randomness = our own xoshiro256** `Rng` with named streams. Sorts must have a total order (tie-break on ids).
* **Headless and decoupled**: nothing in `src/` or `include/w2f/` may include a socket header, reference `w2f::net`, or use Unreal types (`make check-isolation` enforces the first two). The network layer uses only the engine's public headers.
* **Data-driven**: champions, abilities, traits, items, PvE are JSON (`data/*.json`, format in `server/docs/champions-json.md`). A new champion or synergy is **data plus generic primitives, never a per-champion C++ subclass or `if (champion == X)`**. If data cannot express something, add a general primitive (effect / status / target / hook), document it, and test it.
* **Separate simulation logic from state updates**: the combat tick runs in fixed phases — expire → act (only *declare*: attacks, casts, moves) → resolve (mana, scheduled effects, casts, DoTs, hits, status sweeps, hooks, deaths). Deciders read the world; only the resolve phase mutates HP/statuses/positions, in canonical order (ascending `UnitId`). Managers (`PlayerState`, `ShopManager`, `UnitRoster`, `MatchManager`) own state and validate every action; the simulator never touches economy state.
* Every rejected action changes nothing (check before you spend/mutate). Every accepted action emits an event to `IMatchListener`.
* Validate at the boundary: loaders reject bad data with the exact JSON path; the network layer rejects malformed commands before they reach the engine.
* **MSVC-proof lifetimes** (CI runs MSVC + ASan): never range-`for` over a member or a returned reference of a temporary (`for (x : f().words)`, `for (x : Last(..).Find(..)->Items())`): copy to a named local first. GCC/clang extend the temporary, MSVC does not.
* Write code that matches its surroundings (naming, comment density). Zero warnings. Add a test for every rule you add; prefer property/fuzz tests where inputs are large.

## Core Game Loop & State Rules
* **Fixed 30 ticks/second, fully deterministic.** Same seed + same inputs ⇒ identical state hash on every compiler, OS and optimisation level (CI proves it). All randomness derives from the match seed.
* **Opening + board size** (designer's TFT rules): round 1 has no shop; `Start()` deals every player one random 1-cost unit (`MatchConfig::shopClosedOpeningRounds` / `openingUnitCosts`); the shop opens in round 2. The board holds `level` units (`limitBoardToLevel`, on by default; level 10 max).
* **Match phases** (`MatchManager`): [MotherNature: every 3rd round, a free gift instead of a shop] → Planning (Bench/Shop phase: buy, sell, reroll, move, level, equip; no shop in Mother Nature rounds; automatic snapshot at the start of every Planning) → **Combat** (players cannot act on state that combat reads) → Resolution (damage, streaks, gold, drops, eliminations) → next round. Stages: 1-1..1-3 are PvE and X-7 are PvE; the rest PvP. Eliminated players' units and shop go back to the shared pool.
* **State isolation**: a fight is a pure function `(units on both boards, traits, items, seed) → CombatLog`. It is simulated **instantly** and never mutates player/economy state; the result (winner, survivors) is applied afterwards by the match. Summons exist only inside a fight and are not units of the team (never survivors, never on a board).
* **Immutable event logs**: combat produces a timestamped, append-only `CombatEvent` stream (Spawn, Move, Attack, SpellCast, Damage, Heal, Shield*, Status*, Teleport, TraitActivated, Death …) with a checksum. Viewers/clients **replay** it; they never decide anything. The independent replay validator in the tests must accept every log. Enum values that appear in the stream are only ever **appended**.
* **The shared champion pool is finite**: every copy is in exactly one place (pool, a shop slot, a roster). `VerifyPoolIntegrity()` must hold every tick. If tiers run dry the shop falls back to the nearest tier with stock; only an empty pool leaves empty slots.
* **Privacy**: gold, shop and bench are private to their owner; boards are public; combat logs go to the fight's participants (`get_fight` for others).
* **Snapshots**: binary format v2 records config/champion/item/PvE data hashes; a restore must reproduce the identical `StateHash()`. Changing data shape changes the data hash on purpose.
* Ability model in one line: an ability = trigger + effect primitives (`Damage`, `Shield`, `Status`, `DoT`, `Heal`, `Teleport`, `Displace`, `Mana`, `Summon`) with targets and formulas; hooks are silent triggers; hook damage never fires hooks (no chain reactions). Synergies pick the *highest* breakpoint reached (count of *different* champions).

## Auto-Memory Clause
Whenever we lock down a major engine mechanic, rule or convention (a new primitive, a phase rule, a data-format decision, a determinism finding), **update the "Locked Engine Mechanics" section below in the same change**, keeping each entry to one or two lines. Also correct any entry that a change makes wrong. Do not wait to be asked.

## Status & Honest Assessment (as of Phase 15; re-check before trusting)
**Verdict:** the engine is a strong foundation for UE5. Since Phase 14 (and with TFT rules since Phase 15) it is playable locally (1 human vs 7 AI in the browser test client), but balance is unproven and the client is a test tool, not a game.
* **UE5 fit:** good. Server-authoritative + replayable event log = UE only renders. The engine is C++17 with no exceptions/RTTI/floats/platform calls (CI builds it that way), so it can be embedded as a UE module or used through the WebSocket server. Engine speed is not a problem (12 full 8-bot matches ≈ 1 s).
  The hard part is everything visual, ~80% of the remaining work: art/animation for 30 champions, 60+ effect/status VFX, UI (shop, bench, board, gifts, item combining, traits), sound. The log has NO animation data (hits are instant: no windup, no projectile flight; the 0.5 s cast lock is a guess) and every DoT shows as the same `Burn` status.
* **Playable vs 7 AI (Phase 14/15):** `w2f_server --bots 7` + `client/index.html`, with the designer's TFT rules: round 1 = free random 1-cost unit and no shop, board = level (max 10). Bots bank interest, level to a per-stage target, reroll, chase synergies (breakpoint-aware buying and board choice), field the best `level` units, equip items, sell to make room, and take heals when hurt. Still open: (1) PvE is no threat (bots won 100% of 628 PvE fights; TFT's early PvE is easy too, but 2-7+ bosses could bite); (2) the bots are heuristics, not strong players (no positioning beyond tank-front, no item planning, no scouting); (3) the opening cost tier (1-cost) is assumed.
* **Balance facts** (12 bot-only matches under the real rules, Phase 15 bots: a smoke test, low confidence): avg 38 rounds; **0% of 1,221 PvP fights timed out** (avg 23.9 s; the earlier 16.7% came from the old unlimited-board, item-less bots, so it was not a stalling bug); ~394 buys, ~74 sales, ~67 item equips, ~7 item combines and ~89 star-ups per match. The old per-champion win rates (Mortis 75%, Coregons-3 74%, Baira 32%) predate the new bots and the board cap: re-measure before acting on them.
  Higher breakpoints (Coregons 6/8, Helios 6, Hexagon 4, Assassin 4) are still unmeasured. Mother Nature: bots now take heals when hurt (8 of ~50 gifts in a demo match) and about half the unit gifts are still paid as gold (full rosters).
* **CI:** all jobs green as of Phase 14 (the Windows/MSVC failure, an ASan stack-use-after-scope in `StateHash`, was fixed in `6a1f782`; the designer confirmed the run passed).
* Unproven/assumed: all Mother Nature weights, Tier 3 start (stage 4), no Tier 2, cast lock 0.5 s, many `ASSUMED` numbers in the data (`server/docs/content-notes.md`).

## Roadmap (next steps, in order)
1. ~~Confirm Windows CI is green~~ (done).
2. ~~Server-side AI seats (`--bots 7`) + bots that equip items, sell, reroll, chase synergies, field the best units~~ (done, Phase 14/15).
3. ~~Minimal local browser test client~~ (done: `client/index.html`). Play it first: Mother Nature, board size vs level and fight length are the things to judge by hand.
4. Real balance harness (win rate per champion and synergy tier, fight timeout rate, PvE difficulty, game length); a throwaway prototype was used for the numbers above. First tuning targets: fight timeouts and PvE difficulty.
5. UE5 vertical slice: replay one recorded fight before any UI; decide WebSocket client vs embedding the engine as a module.
6. Later: persistence/restore of autosaves, auth/matchmaking, Docker deploy, split `tests.cpp` and `CombatSimulator.cpp`, Android/iOS.

## Locked Engine Mechanics (living memory)
* Roster = 30 champions (tiers 8/7/6/5/4) + 2 summons (Skeleton 9101, Lost Soul 9102); ids 9001–9031 real, 9101+ summons, 10001+ PvE monsters. Null is range 3 (designer override of the doc's 1).
* Statuses `Blind`, `DamageTaken`, `BonusMaxMana` (permanent only), `ExecuteBelow`, `HpPerSecond` (negative = true damage credited to the source, not counted as "damage dealt"), `EmpoweredAttack` (charges spent by `requiresCharge` hooks). `Displace` = pull/knock-back, logged as a `Teleport` event with `subtype` 1.
* Targets `LowestHpEnemy`, `HighestHpEnemy`, `AllEnemies`, `AllAllies`; area targets accept `count` (nearest N). Teleport destinations `NextToLowestHpEnemy`, `NextToHighestHpEnemy`, `BehindCurrentTarget` retarget the caster.
* Hooks `OnAnyUnitDeath` (once per death, holder alive) and `OnShieldBreak` (shield used up by damage, not expiry). Champion stat `attackType` may be `Magic`.
* Trait scope `Team` = cast once by the lowest-id holder, aimed by its own target (`Self`/`AllEnemies`/`AllAllies`). Coregons 3/6/8 = 3 untargetable Lost Souls (25/40/60% of tankiest ally HP; echo 5/8/12% by summon star) + lifesteal 10/15/25%; from 6 the zone (+15 enemy mana cost, +2 ally mana/s, ∓2%/4% max HP per second, execute <5%/10%).
* Shop: exhausted tiers fall back to the nearest tier with stock (cheaper first on a tie); never crashes or waits.
* CI proves determinism by comparing `scripts/fingerprint.sh` output across sanitized/Release builds and across OSes; the whole code base builds with no exceptions/RTTI.
* Items (Phase 12): `data/items.json` = 8 components + Seed + 28 legendaries + 8 emblems (emblems = +1 trait only); ids 1–9 base, 10–37 legendaries, 40–47 emblems. There is no separate test-fixture item file any more.
* Shield tracking: every shield remembers its source ability and absorbed damage; `OnShieldBreak` gets it as `TriggerDamage` (+ `onlyShieldsFrom`). Effect `condition: NoDamageTakenSinceCast` (needs a delay). Hook `EveryInterval` (alive, not stunned, living enemy target in range). Gylachster's immunity is permanent.
* Assassin (2/4) = Vex, Lunis, Raa (+ Assassin Emblem): abilities can crit, +20/+50 crit damage.
* Mother Nature (Phase 13, replaces carousel + augments): every 3rd round (`motherNatureEveryRounds`) opens with a `MotherNature` phase (the old Draft phase is gone) — each player is offered 2 distinct concrete gifts from `data/mother_nature.json` (tier by stage; Gold/Xp/Heal/Item/Unit), picks 1 free (`TryPickGift`, `pick_gift`); shop closed the whole round (`ShopClosed`); units on offer are checked out of the pool; timeout auto-picks the first offer; phase ends early when all settled; snapshot format v3; bots pick unit > item > gold > xp > heal.
* Helios burn `refreshes` (one burn per target, keeps its rhythm). Assassin also gives +15/+30 crit chance.
* Bot economy (Phase 15, `BotProfile`): savings = min(50, (stage-1) x 10) gold, 0 when health <= 50; XP with everything above 50 and up to `targetLevelByStage` {3,5,6,7,8,9,10}; buys any unit while it owns < board size + 2 units, otherwise only star-ups (copies), synergy gains or strictly pricier units; rerolls (max 8) with gold above the savings from stage 2; board = greedy pick of `level` units by cost x star (1/3/9) + 1.5 x synergy gain (12 points per breakpoint, linear in between, counting DIFFERENT champions, item emblems included). `AIBotController` takes an optional `TraitDatabase*` (the server and demo pass it).
* Bot seats + catalog (Phase 14): `GameServerConfig::bots` / `--bots N` = the LAST N seats are `AIBotController`s ticked by `GameServer` after the engine's tick (no connection, no token, never in the lobby count; a lobby needs at least one human). A bot's Planning turn is: XP, buy (owned champions first; sells its weakest lone 1-star, item-free, bench-first unit when the roster is full and the purchase is worth more), place (tanks front), equip (`ItemFit`: defence for tanks, offence for damage dealers, recipes first, only fielded units, no duplicate trait emblem, a bare Seed only where it combines). `MatchManager::Items()` exposes the item database read-only. Bots add no RNG use beyond the shop-order shuffle, so `AIBotController::State` is unchanged.
* `get_catalog` -> `catalog` (id -> name/cost/traits/stats for champions, monsters, items, traits) is a command, not a push, so existing message sequences are unchanged; `welcome`/`lobby` carry `bots`, `match_started` carries `bot_seats`.
* Open design questions live in `server/docs/content-notes.md` (Lum's active, Mother Nature weights / Tier 3 start stage / a Tier 2).
