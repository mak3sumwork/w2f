#include "w2f/net/Messages.h"

#include "w2f/net/Encoding.h"
#include "w2f/net/JsonWriter.h"
#include "w2f/Pve.h"

namespace w2f::net::msg {

namespace {

const char* kEventTypeNames[] = {"Spawn", "Move", "Attack", "Damage", "Death", "SpellCast", "ShieldApplied", "ShieldEnded", "StatusApplied",
                                 "StatusEnded", "ManaChanged", "Heal", "TraitActivated", "Teleport", "SpellInterrupted", "Overtime"};

const char* LocationName(LocationType l) {
    switch (l) {
        case LocationType::Bench: return "bench";
        case LocationType::Board: return "board";
        case LocationType::None: break;
    }
    return "none";
}

void WriteUnit(JsonWriter& w, const UnitInstance& u) {
    w.BeginObject();
    w.Field("id", u.id);
    w.Field("champion", u.champion != nullptr ? u.champion->id : 0u);
    w.Field("star", u.starLevel);
    w.Field("location", LocationName(u.location));
    w.Field("x", u.x);
    w.Field("y", u.y);
    w.Key("items").BeginArray();
    for (ItemId item : u.items) {
        if (item != 0) w.UInt(item);
    }
    w.EndArray();
    w.EndObject();
}

// One gift as a client draws it: which one, its name, and the concrete reward.
void WriteGift(JsonWriter& w, int index, const GiftOffer& offer, const MotherNatureDatabase* data) {
    w.BeginObject();
    w.Field("index", index);
    w.Field("gift", offer.gift);
    const GiftDefinition* def = data != nullptr ? data->FindGift(offer.gift) : nullptr;
    w.Field("name", def != nullptr ? std::string_view(def->name) : std::string_view());
    w.Field("kind", ToString(offer.type));
    if (offer.type == GiftType::Gold || offer.type == GiftType::Xp || offer.type == GiftType::Heal) w.Field("amount", offer.amount);
    if (offer.type == GiftType::Item) w.Field("item", offer.item);
    if (offer.type == GiftType::Unit && offer.champion != nullptr) {
        w.Field("champion", offer.champion->id);
        w.Field("cost", offer.champion->cost);
    }
    w.EndObject();
}

JsonWriter Start(std::string_view type) {
    JsonWriter w;
    w.BeginObject();
    w.Field("type", type);
    return w;
}
std::string Finish(JsonWriter& w) {
    w.EndObject();
    return w.Take();
}
void Seat(JsonWriter& w, const char* key, PlayerId p) {
    if (p == kInvalidPlayerId) w.Key(key).Null();
    else w.Key(key).Int(p);
}

}  // namespace

std::string Welcome(PlayerId seat, std::string_view token, bool reconnected, int connected, int seats, int bots, bool matchRunning) {
    JsonWriter w = Start("welcome");
    w.Field("protocol", kProtocolVersion);
    w.Field("protocol_revision", kProtocolRevision);
    w.Field("player_id", static_cast<int>(seat));
    w.Field("token", token);
    w.Field("reconnected", reconnected);
    w.Field("seats", seats);
    w.Field("bots", bots);
    w.Field("connected", connected);
    w.Field("match_running", matchRunning);
    return Finish(w);
}

std::string Lobby(const std::vector<PlayerId>& seatsTaken, int seats, int bots) {
    JsonWriter w = Start("lobby");
    w.Field("seats", seats);
    w.Field("bots", bots);
    w.Field("connected", static_cast<int>(seatsTaken.size()));
    w.Key("players").BeginArray();
    for (PlayerId p : seatsTaken) w.Int(p);
    w.EndArray();
    return Finish(w);
}

namespace {
void WriteStarValues(JsonWriter& w, const char* key, const StarValue& v) {
    w.Key(key).BeginArray();
    for (int x : v) w.Int(x);
    w.EndArray();
}
void WriteStats(JsonWriter& w, const ItemStats& s) {
    w.Key("stats").BeginObject();
    if (s.maxHp != 0) w.Field("hp", s.maxHp);
    if (s.armor != 0) w.Field("armor", s.armor);
    if (s.magicResist != 0) w.Field("magic_resist", s.magicResist);
    if (s.attackDamage != 0) w.Field("attack_damage", s.attackDamage);
    if (s.abilityDamage != 0) w.Field("ability_damage", s.abilityDamage);
    if (s.attackSpeedPercent != 0) w.Field("attack_speed_percent", s.attackSpeedPercent);
    if (s.critChance != 0) w.Field("crit_chance", s.critChance);
    if (s.startMana != 0) w.Field("start_mana", s.startMana);
    if (s.manaRegenMilli != 0) w.Field("mana_regen_milli", s.manaRegenMilli);
    w.EndObject();
}
}  // namespace

namespace {
const char* kStatusNames[] = {"Stun", "Burn", "AttackDamage", "AttackSpeed", "Armor", "MagicResist", "MaxHp", "Wound", "InflictsWound", "DamageAmp", "Root", "Knockup",
                              "CcImmunity", "DamageTakenRegen", "BonusAttackDamage", "Tether", "Untargetable", "AggroDrop", "BonusArmor", "BonusMagicResist", "BonusMaxHp",
                              "BonusAbilityDamage", "BonusCritChance", "AbilityCrit", "CritDamage", "CritDamageTakenReduction", "BonusManaRegen", "AbilityPower", "SpellShield",
                              "Blind", "DamageTaken", "BonusMaxMana", "ExecuteBelow", "HpPerSecond", "EmpoweredAttack", "Poison", "Bleed", "Drain"};
const char* kAreaNames[] = {"None", "Single", "Circle", "CircleSelf", "Line", "Cone", "Row", "All"};

void WriteNameList(JsonWriter& w, const char* key, const char* const* names, std::size_t count) {
    w.Key(key).BeginArray();
    for (std::size_t i = 0; i < count; ++i) w.String(names[i]);
    w.EndArray();
}
}  // namespace

std::string Catalog(const ChampionDatabase& champions, const ItemDatabase* items, const TraitDatabase* traits, const EncounterDatabase* encounters, const CombatConfig& combat,
                    const TextTable* text) {
    JsonWriter w = Start("catalog");
    // The vocabularies of the combat log: an index into each list is the number a log row carries.
    w.Key("enums").BeginObject();
    WriteNameList(w, "status_types", kStatusNames, sizeof(kStatusNames) / sizeof(kStatusNames[0]));   // Status* events' `subtype`; a DoT Damage's `kind`
    {
        const char* damageTypes[] = {"Physical", "Magic", "True"};
        WriteNameList(w, "damage_types", damageTypes, 3);                                               // Damage's `subtype`
        const char* attackStyles[] = {"melee", "projectile"};
        WriteNameList(w, "attack_styles", attackStyles, 2);                                             // Attack's `kind`
        const char* dotVisuals[] = {"Burn", "Poison", "Bleed", "Drain"};
        WriteNameList(w, "dot_visuals", dotVisuals, 4);                                                 // the StatusType names a typed DoT can have
    }
    WriteNameList(w, "area_shapes", kAreaNames, sizeof(kAreaNames) / sizeof(kAreaNames[0]));            // SpellCast's `shape`
    w.Key("damage_flags").BeginObject();                                                                // Damage's `flags` bits
    w.Field("crit", static_cast<int>(kFlagCrit)).Field("dot", static_cast<int>(kFlagDot)).Field("ability", static_cast<int>(kFlagAbility)).Field("basic", static_cast<int>(kFlagBasic));
    w.Field("on_death", static_cast<int>(kFlagOnDeath)).Field("redirected", static_cast<int>(kFlagRedirected)).Field("triggered", static_cast<int>(kFlagTriggered)).Field("summon", static_cast<int>(kFlagSummon));
    w.EndObject();
    w.EndObject();
    w.Key("champions").BeginArray();
    const auto writeChampions = [&w, &combat](const ChampionDatabase& db, bool monster) {
        for (const ChampionDefinition& c : db.All()) {
            w.BeginObject();
            w.Field("monster", monster);
            w.Field("id", c.id);
            w.Field("name", c.name);
            w.Field("cost", c.cost);
            w.Field("role", c.role == ChampionRole::Tank ? "tank" : "damage");
            w.Field("summon", c.summon);
            w.Key("traits").BeginArray();
            for (const std::string& t : c.traits) w.String(t);
            w.EndArray();
            WriteStarValues(w, "hp", c.stats.maxHp);
            WriteStarValues(w, "attack_damage", c.stats.attackDamage);
            w.Field("attack_speed_milli", c.stats.attackSpeedMilli);
            w.Field("range", c.stats.attackRange);
            w.Field("max_mana", c.stats.maxMana);
            w.Field("ability", c.ability.name);
            w.Field("passive", c.passive.name);
            // The presentation contract: EFFECTIVE timings (the champion's own numbers, else the CombatConfig defaults), in ticks. See docs/UE5-Integration.md.
            w.Key("presentation").BeginObject();
            const int speed = c.stats.projectileSpeedMilli >= 0 ? c.stats.projectileSpeedMilli : (c.stats.attackRange >= 2 ? combat.defaultRangedProjectileSpeedMilli : 0);
            w.Key("attack").BeginObject();
            w.Field("style", speed > 0 ? "projectile" : "melee");
            w.Field("windup_ticks", c.stats.attackWindupTicks >= 0 ? c.stats.attackWindupTicks : combat.defaultAttackWindupTicks);
            w.Field("projectile_speed_milli", speed);
            w.EndObject();
            w.Key("ability");
            if (c.ability.HasAbility()) {
                const AreaDescription area = DescribeArea(c.ability);
                w.BeginObject();
                w.Field("id", c.ability.id);
                w.Field("windup_ticks", c.ability.windupTicks >= 0 ? c.ability.windupTicks : combat.defaultCastWindupTicks);
                w.Field("cast_lock_ticks", c.ability.castLockTicks);
                w.Field("channel_ticks", c.ability.channelTicks);
                w.Key("area").BeginObject();
                w.Field("shape", kAreaNames[static_cast<std::size_t>(area.shape)]);
                w.Field("size", area.size);
                w.EndObject();
                w.Key("dot_visuals").BeginArray();   // the typed damage-over-time this spell applies (none for most)
                for (const AbilityEffect& effect : c.ability.effects) {
                    if (const auto* dot = std::get_if<DotEffect>(&effect.payload)) w.String(kStatusNames[static_cast<std::size_t>(dot->visual)]);
                }
                w.EndArray();
                w.EndObject();
            } else {
                w.Null();
            }
            w.EndObject();
            w.EndObject();
        }
    };
    writeChampions(champions, false);
    if (encounters != nullptr) writeChampions(encounters->Monsters(), true);
    w.EndArray();
    w.Key("items").BeginArray();
    if (items != nullptr) {
        for (const ItemDefinition& item : items->All()) {
            w.BeginObject();
            w.Field("id", item.id);
            w.Field("name", item.name);
            w.Key("components").BeginArray();
            if (item.IsCombined()) w.UInt(item.components[0]).UInt(item.components[1]);
            w.EndArray();
            WriteStats(w, item.stats);
            w.Key("traits").BeginArray();
            for (const std::string& t : item.grantsTraits) w.String(t);
            w.EndArray();
            w.Field("has_effect", !item.abilities.empty() || !item.auras.empty());
            w.Field("consumable", item.IsConsumable());   // used up when "equipped" (the Item Remover): it takes every item off the unit
            w.EndObject();
        }
    }
    w.EndArray();
    w.Key("traits").BeginArray();
    if (traits != nullptr) {
        for (const TraitDefinition& t : traits->All()) {
            w.BeginObject();
            w.Field("id", t.id);
            w.Field("name", t.name);
            w.Key("breakpoints").BeginArray();
            for (const TraitBreakpoint& b : t.breakpoints) w.Int(b.count);
            w.EndArray();
            w.EndObject();
        }
    }
    w.EndArray();
    // Display text (data/text_en.json): a flat key -> string map, keys documented in that file. Absent when the server has no text file.
    if (text != nullptr) {
        w.Key("text").BeginObject();
        w.Field("language", text->Language());
        w.Key("entries").BeginObject();
        for (const auto& entry : text->Entries()) w.Field(entry.first, entry.second);
        w.EndObject();
        w.EndObject();
    }
    return Finish(w);
}

std::string Error(std::string_view code, std::string_view detail, bool hasId, long long id) {
    JsonWriter w = Start("error");
    w.Field("code", code);
    w.Field("detail", detail);
    if (hasId) w.Field("id", id);
    return Finish(w);
}

std::string Result(const Command& command, ActionResult result) {
    JsonWriter w = Start("result");
    if (command.hasId) w.Field("id", command.id);
    w.Field("action", ToString(command.type));
    w.Field("result", ToString(result));
    w.Field("ok", result == ActionResult::Ok);
    return Finish(w);
}

std::string Pong(bool hasId, long long id) {
    JsonWriter w = Start("pong");
    if (hasId) w.Field("id", id);
    return Finish(w);
}

std::string MatchStarted(const GameConfig& config, int seats, PlayerId you, int motherNatureEvery, const std::vector<PlayerId>& botSeats) {
    JsonWriter w = Start("match_started");
    w.Field("player_id", static_cast<int>(you));
    w.Field("seats", seats);
    w.Key("bot_seats").BeginArray();
    for (PlayerId bot : botSeats) w.Int(bot);
    w.EndArray();
    w.Field("tick_rate", kTicksPerSecond);
    w.Key("phase_ticks").BeginObject();
    w.Field("mother_nature", config.match.motherNatureTicks);
    w.Field("planning", config.match.planningTicks);
    w.Field("combat", config.match.combatTicks);
    w.Field("resolution", config.match.resolutionTicks);
    w.EndObject();
    w.Key("board").BeginObject();
    w.Field("columns", kBoardColumns);
    w.Field("rows", kBoardRows);
    w.Field("bench_slots", kBenchSlots);
    w.Field("max_items_per_unit", kMaxItemsPerUnit);
    w.Field("shop_slots", config.shop.slotCount);
    w.EndObject();
    w.Field("mother_nature_every", motherNatureEvery);
    w.Key("combat_event_types").BeginArray();
    for (const char* name : kEventTypeNames) w.String(name);
    w.EndArray();
    return Finish(w);
}

std::string Phase(const GameConfig& config, MatchPhase phase, int round, int ticksElapsed, int durationTicks, std::uint64_t serverTick, bool motherNatureRound, bool shopClosed) {
    JsonWriter w = Start("phase");
    w.Field("phase", ToString(phase));
    w.Field("round", round);
    const StageRound sr = config.match.StageOf(round);
    w.Field("stage", sr.stage);
    w.Field("round_in_stage", sr.roundInStage);
    w.Field("pve", config.match.IsPveRound(round));
    w.Field("mother_nature", motherNatureRound);   // a gift phase this round, and no shop until it is over
    w.Field("shop_closed", shopClosed);            // no shop this round: Mother Nature's, or the opening round
    const int duration = durationTicks;   // (Combat lasts as long as the round's fights: the engine knows, the config only gives the maximum)
    w.Field("duration_ticks", duration);
    w.Field("ticks_remaining", duration > ticksElapsed ? duration - ticksElapsed : 0);
    w.Field("server_tick", serverTick);
    return Finish(w);
}

std::string PrivateState(const MatchManager& match, PlayerId player) {
    const PlayerState& p = *match.Players().Get(player);
    JsonWriter w = Start("state");
    w.Field("player_id", static_cast<int>(player));
    w.Field("alive", p.IsAlive());
    w.Field("health", p.Health());
    w.Field("gold", p.Gold());
    w.Field("level", p.Level());
    w.Field("xp", p.Xp());
    w.Field("xp_to_next", p.XpToNextLevel());
    w.Field("streak", p.Streak());
    w.Key("shop").BeginArray();
    for (const ChampionDefinition* slot : p.Shop().Slots()) w.UInt(slot != nullptr ? slot->id : 0);
    w.EndArray();
    w.Key("bench").BeginArray();
    for (int i = 0; i < kBenchSlots; ++i) {
        if (const UnitInstance* u = p.Roster().BenchAt(i)) WriteUnit(w, *u);
        else w.Null();
    }
    w.EndArray();
    w.Key("board").BeginArray();
    for (const UnitInstance& u : p.Roster().Units()) {
        if (u.location == LocationType::Board) WriteUnit(w, u);
    }
    w.EndArray();
    w.Key("item_bag").BeginArray();
    for (ItemId item : p.ItemBag()) w.UInt(item);
    w.EndArray();
    // Mother Nature: the offers still open (empty once picked, and outside her phase), and whether the choice is settled.
    w.Key("gifts").BeginArray();
    const auto& offers = match.GiftOffers(player);
    for (std::size_t i = 0; i < offers.size(); ++i) WriteGift(w, static_cast<int>(i), offers[i], match.MotherNature());
    w.EndArray();
    w.Field("gift_settled", match.Phase() == MatchPhase::MotherNature ? match.GiftSettled(player) : true);
    return Finish(w);
}

std::string PublicState(const MatchManager& match) {
    JsonWriter w = Start("public_state");
    w.Field("round", match.Round());
    w.Key("players").BeginArray();
    for (int i = 0; i < match.Players().PlayerCount(); ++i) {
        const PlayerState& p = *match.Players().Get(static_cast<PlayerId>(i));
        w.BeginObject();
        w.Field("player_id", i);
        w.Field("alive", p.IsAlive());
        w.Field("health", p.Health());
        w.Field("level", p.Level());
        w.Field("streak", p.Streak());
        w.Field("placement", p.Placement());
        w.Key("board").BeginArray();
        for (const UnitInstance& u : p.Roster().Units()) {
            if (u.location == LocationType::Board) WriteUnit(w, u);
        }
        w.EndArray();
        // The bench is public too (as in TFT, where you can scout it): it shows in the scouted player's arena. Gold, XP, shop, item bag stay private.
        w.Key("bench").BeginArray();
        for (const UnitInstance& u : p.Roster().Units()) {
            if (u.location == LocationType::Bench) WriteUnit(w, u);
        }
        w.EndArray();
        w.EndObject();
    }
    w.EndArray();
    return Finish(w);
}

FightSummary Summarize(int index, const CombatOutcome& o) {
    FightSummary s;
    s.index = index;
    s.home = o.matchup.home;
    s.away = o.matchup.away;
    s.ghost = o.matchup.awayIsGhost;
    s.monsters = o.matchup.awayIsMonsters;
    s.encounter = o.matchup.encounter;
    s.events = static_cast<int>(o.log.events.size());
    return s;
}

std::string CombatSummary(int round, const std::vector<FightSummary>& fights) {
    JsonWriter w = Start("combat_summary");
    w.Field("round", round);
    w.Key("fights").BeginArray();
    for (const FightSummary& f : fights) {
        w.BeginObject();
        w.Field("index", f.index);
        Seat(w, "home", f.home);
        Seat(w, "away", f.away);
        w.Field("away_is_ghost", f.ghost);
        w.Field("away_is_monsters", f.monsters);
        w.Field("encounter", f.encounter);
        w.Field("events", f.events);
        w.EndObject();
    }
    w.EndArray();
    return Finish(w);
}

std::string Combat(int round, int index, const CombatOutcome& o, const MatchManager* match) {
    JsonWriter w = Start("combat");
    w.Field("round", round);
    w.Field("fight_index", index);
    Seat(w, "home", o.matchup.home);
    Seat(w, "away", o.matchup.away);
    w.Field("away_is_ghost", o.matchup.awayIsGhost);
    w.Field("away_is_monsters", o.matchup.awayIsMonsters);
    w.Field("encounter", o.matchup.encounter);
    // The items the fighters carry (the boards are locked during a fight, so the rosters still say what stood there at its start). Monsters carry none.
    w.Key("unit_items").BeginObject();
    if (match != nullptr) {
        for (const PlayerId seat : {o.matchup.home, o.matchup.away}) {
            if (seat == kInvalidPlayerId || seat >= match->Players().PlayerCount()) continue;
            for (const UnitInstance& u : match->Players().Get(seat)->Roster().Units()) {
                if (u.location != LocationType::Board || u.ItemCount() == 0) continue;
                w.Key(std::to_string(u.id)).BeginArray();
                for (ItemId item : u.items) {
                    if (item != 0) w.UInt(item);
                }
                w.EndArray();
            }
        }
    }
    w.EndObject();
    w.Field("winner", o.winner == CombatWinner::Home ? "home" : o.winner == CombatWinner::Away ? "away" : "draw");
    w.Field("winner_survivors", o.winnerSurvivors);
    w.Field("end_tick", o.log.endTick);
    w.Key("survivors").BeginArray().Int(o.log.survivors[0]).Int(o.log.survivors[1]).EndArray();
    const std::uint64_t checksum = o.log.checksum;
    std::uint8_t bytes[8];
    for (int i = 0; i < 8; ++i) bytes[i] = static_cast<std::uint8_t>(checksum >> (56 - 8 * i));
    w.Field("checksum", HexEncode(bytes, 8));   // 64 bits do not fit a JSON number safely: sent as 16 hex digits
    w.Key("columns").BeginArray();
    for (const char* c : {"tick", "type", "team", "unit", "other", "from_x", "from_y", "to_x", "to_y", "amount", "hp_after", "champion", "star",
                          "absorbed", "subtype", "flags", "ability", "duration", "mana_max", "mana_regen", "reduced", "trait_id", "windup", "flight", "kind", "shape", "size"}) {
        w.String(c);
    }
    w.EndArray();
    w.Key("events").BeginArray();
    for (const CombatEvent& e : o.log.events) {
        w.BeginArray();
        w.Int(e.tick).Int(static_cast<int>(e.type)).Int(e.team).UInt(e.unit).UInt(e.other);
        w.Int(e.from.x).Int(e.from.y).Int(e.to.x).Int(e.to.y);
        w.Int(e.amount).Int(e.hpAfter).UInt(e.champion).Int(e.star).Int(e.absorbed).Int(e.subtype).Int(e.flags);
        w.UInt(e.ability).Int(e.duration).Int(e.manaMax).Int(e.manaRegen).Int(e.reduced).UInt(e.traitId);
        w.Int(e.windup).Int(e.flight).Int(e.kind).Int(e.shape).Int(e.size);
        w.EndArray();
    }
    w.EndArray();
    return Finish(w);
}

std::string PlayerDamaged(PlayerId player, int damage, int healthAfter) {
    JsonWriter w = Start("player_damaged");
    w.Field("player_id", static_cast<int>(player));
    w.Field("damage", damage);
    w.Field("health", healthAfter);
    return Finish(w);
}

std::string PlayerEliminated(PlayerId player, int placement) {
    JsonWriter w = Start("player_eliminated");
    w.Field("player_id", static_cast<int>(player));
    w.Field("placement", placement);
    return Finish(w);
}

std::string MatchOver(const MatchManager& match) {
    JsonWriter w = Start("match_over");
    Seat(w, "winner", match.Winner());
    w.Key("placements").BeginArray();
    for (int i = 0; i < match.Players().PlayerCount(); ++i) w.Int(match.Players().Get(static_cast<PlayerId>(i))->Placement());
    w.EndArray();
    return Finish(w);
}

std::string Income(PlayerId player, int round, const IncomeBreakdown& income) {
    JsonWriter w = Start("income");
    w.Field("player_id", static_cast<int>(player));
    w.Field("round", round);
    w.Field("base_gold", income.baseGold);
    w.Field("interest_gold", income.interestGold);
    w.Field("streak_gold", income.streakGold);
    w.Field("passive_xp", income.passiveXp);
    w.Field("total_gold", income.TotalGold());
    return Finish(w);
}

std::string GiftsOffered(const std::vector<GiftOffer>& offers, const MotherNatureDatabase* data) {
    JsonWriter w = Start("gift_event");
    w.Field("event", "offered");
    w.Key("gifts").BeginArray();
    for (std::size_t i = 0; i < offers.size(); ++i) WriteGift(w, static_cast<int>(i), offers[i], data);
    w.EndArray();
    return Finish(w);
}

std::string GiftPicked(int index, const GiftOffer& gift, const MotherNatureDatabase* data, bool automatic, int goldConverted) {
    JsonWriter w = Start("gift_event");
    w.Field("event", "picked");
    w.Key("gift");
    WriteGift(w, index, gift, data);
    w.Field("automatic", automatic);
    w.Field("gold_converted", goldConverted);   // > 0: a unit gift with no room for it was paid as gold instead
    return Finish(w);
}

std::string PveDropMsg(const PveDrop& drop) {
    JsonWriter w = Start("pve_drop");
    w.Field("drop", drop.type == PveDropType::Gold ? "gold" : drop.type == PveDropType::Champion ? "champion" : drop.type == PveDropType::Item ? "item" : "none");
    if (drop.type == PveDropType::Gold) w.Field("gold", drop.gold);
    if (drop.type == PveDropType::Champion) w.Field("champion", drop.champion);
    if (drop.type == PveDropType::Item) w.Field("item", drop.item);
    return Finish(w);
}

namespace {
JsonWriter UnitEvent(const char* name) {
    JsonWriter w = Start("unit_event");
    w.Field("event", name);
    return w;
}
}  // namespace

std::string UnitBought(const UnitInstance& unit, int goldSpent) {
    JsonWriter w = UnitEvent("bought");
    w.Key("unit");
    WriteUnit(w, unit);
    w.Field("gold_spent", goldSpent);
    return Finish(w);
}

std::string UnitSold(const UnitInstance& unit, int goldGained) {
    JsonWriter w = UnitEvent("sold");
    w.Key("unit");
    WriteUnit(w, unit);
    w.Field("gold_gained", goldGained);
    return Finish(w);
}

std::string UnitMoved(const UnitMove& move) {
    JsonWriter w = UnitEvent("moved");
    w.Key("unit");
    WriteUnit(w, move.unit);
    w.Key("from").BeginObject();
    w.Field("location", LocationName(move.fromLocation));
    w.Field("x", move.fromX);
    w.Field("y", move.fromY);
    w.EndObject();
    return Finish(w);
}

std::string UnitMerged(const UnitMerge& merge) {
    JsonWriter w = UnitEvent("merged");
    w.Key("unit");
    WriteUnit(w, merge.upgraded);
    w.Key("consumed").BeginArray().UInt(merge.consumed[0]).UInt(merge.consumed[1]).EndArray();
    w.Field("previous_star", merge.previousStarLevel);
    w.Key("overflow_items").BeginArray();
    for (ItemId item : merge.overflowItems) w.UInt(item);
    w.EndArray();
    return Finish(w);
}

std::string ItemEquipped(const UnitInstance& unit, ItemId item) {
    JsonWriter w = UnitEvent("item_equipped");
    w.Key("unit");
    WriteUnit(w, unit);
    w.Field("item", item);
    return Finish(w);
}

std::string BagItemsCombined(ItemId first, ItemId second, ItemId result) {
    JsonWriter w = Start("bag_event");
    w.Field("event", "combined");
    w.Field("first", first);
    w.Field("second", second);
    w.Field("result", result);
    return Finish(w);
}

std::string ItemUnequipped(const UnitInstance& unit, ItemId item) {
    JsonWriter w = UnitEvent("item_unequipped");
    w.Key("unit");
    WriteUnit(w, unit);
    w.Field("item", item);
    return Finish(w);
}

std::string ItemsCombined(const UnitInstance& unit, const ItemCombination& combination) {
    JsonWriter w = UnitEvent("items_combined");
    w.Key("unit");
    WriteUnit(w, unit);
    w.Field("first", combination.first);
    w.Field("second", combination.second);
    w.Field("result", combination.result);
    return Finish(w);
}

std::string ItemConsumed(const UnitInstance& unit, ItemId consumable, const std::vector<ItemId>& returned) {
    JsonWriter w = UnitEvent("consumable_used");
    w.Key("unit");
    WriteUnit(w, unit);
    w.Field("item", consumable);
    w.Key("returned").BeginArray();
    for (ItemId item : returned) w.UInt(item);
    w.EndArray();
    return Finish(w);
}

}  // namespace w2f::net::msg
