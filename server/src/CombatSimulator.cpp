#include "w2f/CombatSimulator.h"
#include "w2f/Pve.h"

#include <algorithm>
#include <cassert>
#include <limits>
#include <variant>

#include "w2f/Item.h"
#include "w2f/PlayerManager.h"
#include "w2f/Trait.h"
#include "w2f/Rng.h"

namespace w2f {

namespace {

constexpr int kDealtHistoryTicks = 600;  // longest window a formula may look back over (validated)
constexpr int kMaxDeathCastRounds = 16;  // guard against endless cast-on-death chains
constexpr long long kMaxValue = 1'000'000'000;
constexpr int kMaxSummonsPerFight = 48;   // a hard cap: a runaway summon loop cannot grow the fight without bound
constexpr int kMaxHookRounds = 8;         // hook reactions to hook damage settle within this many rounds per tick
constexpr int kMaxAurasPerUnit = 31;
constexpr int kTakedownWindowTicks = 90;  // damage within 3 s of a death is an assist ("takedown")
constexpr int kMaxEchoes = 8;             // Echo Engine: how many times the Invention repeats its modules at most

struct ShieldInst {
    int amount;
    int expiresTick;
    int reductionPercent;
    AbilityId source = kNoAbility;   // the ability that made it (an OnShieldBreak hook may listen to one ability's shields only)
    int absorbed = 0;                // damage it has absorbed so far: the "stored damage" of a detonation
};

struct StatusInst {
    StatusType type;
    int percent;
    int expiresTick;
    int source = -1;  // index of the unit that applied it (a Tether redirects damage to it)
    int aura = 0;     // non-zero: granted by an aura (key = holder * 32 + aura index + 1); ends when the unit leaves the aura
};

// One hook / rider in a unit's trigger list (from an item, the champion's `triggers` or a synergy) with its counters.
struct TriggerSlot {
    const AbilityDefinition* ability = nullptr;
    int counter = 0;      // occurrences since it last fired (for "every Nth")
    int fired = 0;        // firings so far this fight (for maxTriggers)
    bool armed = true;    // OnHpDropBelowPercent: false after it fired, until HP climbs back above the threshold
    bool teamOnce = false; // a synergy trigger with scope Team: each event fires it once for the whole team (the first living holder)
};

struct DotInst {
    int source;             // index of the unit that applied it (credit for damage dealt)
    AbilityId key;          // stacks are counted per ability
    DamageType type;
    int start;              // tick it was applied
    int duration;           // ticks; hit k of hitsTotal lands at start + duration * k / hitsTotal (the last exactly at the end)
    int hitsTotal;
    int nextTick;
    int hitsLeft;
    int remainingRaw;       // raw damage still to deliver; each hit takes remainingRaw / hitsLeft
    std::uint8_t flags;
    bool visible;           // shown to the client as a status (burn); spread basic attacks are not
    int healPercent = 0;    // a drain: the source heals this % of each hit's damage
    StatusType visual = StatusType::Burn;   // which typed DoT it is (Burn / Poison / Bleed / Drain): tags its Damage events
};

struct DealtEntry {
    int tick;
    int amount;
    int target;  // index of the unit it was dealt to
};

struct Hit {
    int source;
    int target;
    DamageType type;
    int raw;
    std::uint8_t flags;
    const DamageEffect* effect = nullptr;  // the ability effect that made this hit (for its on-kill statuses); null for basic attacks / DoTs
    bool fromHook = false;                 // made by a hook trigger's effect: it fires no hooks of its own (no chain reactions)
    int healPercent = 0;                   // a drain (DoT): the source heals this % of the damage the hit deals
    std::uint8_t kind = 0;                 // a typed DoT tick: its visual (StatusType), for the Damage event's `kind`; 0 otherwise
};

struct CastRecord {
    int caster = -1;
    int target = -1;  // main target, or -1
    HexCoord center;  // where the target stood at cast time (area effects centre here)
    const AbilityDefinition* ability = nullptr;
    int rawSnapshot = 0;  // RawDamageDealtToTarget, captured at cast time
    int serial = 0;       // channelled casts: identifies the channel (its delayed effects check they still belong to it)
    bool channeled = false;
    // Hook casts: the event that fired the hook.
    int triggerAttacker = -1;
    int triggerVictim = -1;
    int triggerDamage = 0;
    bool fromHook = false;
    int originTick = -1;   // the tick the cast that made this record began (set when an effect is scheduled; -1 = "now")
};

struct Scheduled {
    int due;
    CastRecord cast;
    const AbilityEffect* effect;
};

struct FightUnit {
    UnitId id = kInvalidUnitId;
    int team = 0;
    const ChampionDefinition* champion = nullptr;
    int star = 1;
    HexCoord pos;
    int startRow = 0;  // arena row it spawned in (for "allies in the start line" targeting)

    int hp = 0;
    int maxHp = 0;      // effective max HP (base plus MaxHp statuses)
    int baseMaxHp = 0;
    int armor = 0;
    int magicResist = 0;
    int damage = 0;
    int critChance = 0;
    int range = 1;
    int attackInterval = 30;
    const AbilityDefinition* ability = nullptr;
    const AbilityDefinition* passive = nullptr;
    const AbilityDefinition* onAttack = nullptr;
    std::vector<const ItemDefinition*> items;
    std::vector<std::string> extraTraits;  // trait tags granted by items (on top of the champion's own)
    std::vector<TriggerSlot> triggers;                      // hooks and riders (items, champion `triggers`, synergies)
    std::vector<const AbilityDefinition*> extraPassives;    // StartOfCombat abilities from items / champion `triggers`
    std::vector<const AuraDefinition*> auras;               // standing effects around this unit
    std::vector<std::vector<int>> auraMembers;              // parallel to `auras`: the units currently inside each one (ascending)
    bool isSummon = false;
    int summoner = -1;
    int manaFraction = 0;   // mana regen remainder (thousandths x ticks), so a changing regen stays exact

    // Mana is kept in thousandths so fractional regen (0.8/s) is exact.
    int maxMana = 0;
    int mana = 0;
    int regenMilli = 0;
    int livedTicks = 0;
    int manaFromDamage = 0;
    bool manaDirty = false;  // mana changed for a discrete reason this tick: tell the client
    int attacksSinceCast = 0;
    int lockedUntil = 0;  // cast animation: cannot act while tick < lockedUntil

    int nextAttackTick = 0;
    int nextMoveTick = 0;
    int nextPathTick = 0;  // backoff after a failed search

    int target = -1;            // index into Fight::units_, or -1
    HexCoord pathGoal{-1, -1};  // where the target stood when `path` was computed
    std::vector<HexCoord> path;
    std::size_t pathIndex = 0;

    bool alive = true;

    std::vector<ShieldInst> shields;
    std::vector<StatusInst> statuses;
    std::vector<DotInst> dots;
    std::vector<DealtEntry> dealt;  // damage this unit dealt, for "damage in the last N ticks"
    int rawCounter = 0;             // raw basic-attack damage dealt to rawCounterTarget since the last cast
    int rawCounterTarget = -1;
    int lastDamagedTick = -1;       // the last tick this unit took any damage (even fully absorbed): the "took no damage" condition

    int activeChannelSerial = 0;  // non-zero while channelling; 0 once completed or interrupted
    int channelEnd = 0;
    AbilityId channelAbility = kNoAbility;
    int killer = -1;                          // whoever's hit brought this unit to 0 HP, if that hit had on-kill statuses
    const DamageEffect* killEffect = nullptr;
    // ---- trait system v2 ----
    int castCount = 0;          // casts so far (TargetCastCount)
    long long totalDealt = 0;   // damage dealt this fight (TopDamageEnemies)
    int totalAttacks = 0;       // basic attacks this fight (afterAttacks)
    int baseMaxMana = 0;        // the bar before ManaCost statuses (thousandths; BonusMaxMana moves it)
    int pilot = -1;             // Hexa: the unit piloting it
    int mech = -1;              // a pilot: the unit it sits in
    int takedowns = 0;
};

int Clamp32(long long v) { return static_cast<int>(std::clamp<long long>(v, -kMaxValue, kMaxValue)); }

class Fight {
public:
    Fight(const CombatConfig& config, const TraitDatabase* traits, const std::vector<FightUnitSpec>& specs, int maxTicks,
          std::uint64_t seed, const ChampionDatabase* summons, const ChampionDatabase* summons2, const FightSetup& setup = FightSetup{})
        : config_(config),
          traits_(traits),
          setup_(setup),
          summons_(summons),
          summons2_(summons2),
          maxTicks_(maxTicks),
          rng_(seed),
          grid_(kArenaColumns, kArenaRows),
          pathfinder_(kArenaColumns, kArenaRows) {
        // Summons are appended mid-fight and every stored unit index / reference must stay valid: never reallocate.
        units_.reserve(specs.size() + static_cast<std::size_t>(kMaxSummonsPerFight));
        for (const FightUnitSpec& spec : specs) {
            if (spec.champion == nullptr || !spec.champion->stats.IsCombatCapable()) continue;
            const int starIndex = std::clamp(spec.starLevel, 1, kMaxStarLevel) - 1;
            units_.push_back(MakeUnit(spec.champion, starIndex + 1, spec.team == 0 ? 0 : 1, spec.position, spec.id, spec.items));
        }
        // Canonical order: everything below iterates by ascending UnitId.
        std::sort(units_.begin(), units_.end(), [](const FightUnit& a, const FightUnit& b) { return a.id < b.id; });
        for (const FightUnit& u : units_) {
            assert(grid_.InBounds(u.pos) && !grid_.IsBlocked(u.pos) && "bad or duplicate spawn hex");
            grid_.SetBlocked(u.pos, true);
            ++alive_[u.team];
        }
        // The busiest arena row of each team at the start: the most units; a tie goes to the row nearest the middle.
        for (int team = 0; team < 2; ++team) {
            int counts[kArenaRows] = {};
            for (const FightUnit& u : units_) counts[u.team == team ? u.pos.y : 0] += u.team == team ? 1 : 0;
            auto frontDistance = [team](int y) { return team == 0 ? (kBoardRows - 1) - y : y - kBoardRows; };
            int best = -1;
            for (int y = 0; y < kArenaRows; ++y) {
                if (counts[y] == 0) continue;
                if (best < 0 || counts[y] > counts[best] || (counts[y] == counts[best] && frontDistance(y) < frontDistance(best))) best = y;
            }
            busiestRow_[team] = best;
        }
    }

    // Builds a fighter from a definition: stats for its star level, its own abilities, the items it carries (which add hooks,
    // passives, auras and trait tags).
    FightUnit MakeUnit(const ChampionDefinition* champion, int star, int team, HexCoord position, UnitId id,
                       const std::vector<const ItemDefinition*>& items) {
        const auto si = static_cast<std::size_t>(std::clamp(star, 1, kMaxStarLevel) - 1);
        const CombatStats& stats = champion->stats;
        FightUnit u;
        u.id = id;
        u.team = team;
        u.champion = champion;
        u.star = static_cast<int>(si) + 1;
        u.pos = position;
        u.startRow = position.y;
        u.maxHp = u.hp = u.baseMaxHp = stats.maxHp[si];
        u.armor = stats.armor[si];
        u.magicResist = stats.magicResist[si];
        u.damage = stats.attackDamage[si];
        u.critChance = stats.critChance[si];
        u.range = stats.attackRange;
        u.attackInterval = stats.AttackIntervalTicks();
        u.maxMana = stats.maxMana * 1000;
        u.baseMaxMana = u.maxMana;
        u.regenMilli = stats.manaRegenMilli;
        if (champion->ability.HasAbility()) u.ability = &champion->ability;
        if (champion->passive.HasAbility()) u.passive = &champion->passive;
        if (champion->onAttack.HasAbility()) u.onAttack = &champion->onAttack;
        u.mana = std::min(stats.startMana, stats.maxMana) * 1000;
        const auto addAbility = [&u](const AbilityDefinition& a) {
            if (a.trigger == EventTrigger::StartOfCombat) u.extraPassives.push_back(&a);
            else u.triggers.push_back(TriggerSlot{&a, 0, 0, true});
        };
        const auto addAura = [&u](const AuraDefinition& a) {
            if (static_cast<int>(u.auras.size()) < kMaxAurasPerUnit) {
                u.auras.push_back(&a);
                u.auraMembers.emplace_back();
            }
        };
        for (const AbilityDefinition& hook : champion->triggers) addAbility(hook);
        for (const AuraDefinition& aura : champion->auras) addAura(aura);
        for (const ItemDefinition* item : items) {
            if (item == nullptr) continue;
            u.items.push_back(item);
            for (const std::string& trait : item->grantsTraits) {
                if (!HasTrait(u, trait)) u.extraTraits.push_back(trait);
            }
            for (const AbilityDefinition& a : item->abilities) addAbility(a);
            for (const AuraDefinition& a : item->auras) addAura(a);
        }
        return u;
    }

    FightResult Run() {
        FightResult result;
        for (const FightUnit& u : units_) {
            CombatEvent e = MakeEvent(0, CombatEventType::Spawn, u.id);
            e.team = static_cast<std::uint8_t>(u.team);
            e.to = u.pos;
            e.amount = u.maxHp;
            e.hpAfter = u.hp;
            e.champion = u.champion->id;
            e.star = static_cast<std::uint8_t>(u.star);
            e.manaMax = u.maxMana;
            e.manaRegen = u.regenMilli;
            log_.events.push_back(e);
        }
        ApplyItems();     // equipment first (it can grant trait tags and start mana), then synergies, then passives
        EmitInitialMana();
        ApplyTraits();    // (so a passive can build on the synergy's stats)
        ApplyPilots();    // Hexa takes its pilot in (after the synergies: the pilot's own bonuses are in its max HP)
        ApplyPassives();
        for (const FightUnit& u : units_) {   // the teams' total max HP, for OnTeamHpLoss (summons and plants excluded)
            if (!u.isSummon && !u.champion->plant) teamMaxHp_[u.team] += u.maxHp;
        }

        int tick = 0;
        bool timedOut = true;
        // The fight's safety limit (see CombatConfig::hardLimitTicks) -- never more than the phase gives it. Normal fights end long before.
        const int limit = std::min(maxTicks_, config_.hardLimitTicks);
        if (alive_[0] == 0 || alive_[1] == 0) {
            timedOut = false;  // nothing to fight
        } else {
            for (; tick < limit; ++tick) {
                if (tick == config_.regulationTicks && config_.overtimeSpeed > 1) EnterOvertime(tick);
                ExpireEffects(tick);
                UpdateAuras(tick);
                for (std::size_t i = 0; i < units_.size(); ++i) {
                    if (units_[i].alive) Act(static_cast<int>(i), tick);
                }
                Resolve(tick);
                if (alive_[0] == 0 || alive_[1] == 0) {
                    timedOut = false;
                    break;
                }
            }
        }

        log_.endTick = timedOut ? limit : tick;
        DespawnSummons(log_.endTick);   // "vanish completely when combat ends"
        log_.survivors[0] = alive_[0];
        log_.survivors[1] = alive_[1];
        log_.pathSearches = pathSearches_;
        for (const FightUnit& u : units_) {   // (units_ is in ascending UnitId order up to the summons, which never score)
            if (!u.isSummon && u.takedowns > 0) log_.takedowns.emplace_back(u.id, u.takedowns);
        }
        result.winner = timedOut ? DecideAtLimit() : Decide();   // (a fight only reaches its safety limit if it can never end)
        log_.checksum = log_.ComputeChecksum();
        result.log = std::move(log_);
        return result;
    }

private:
    // ---- small helpers ---------------------------------------------------------------------

    static CombatEvent MakeEvent(int tick, CombatEventType type, UnitId unit) {
        CombatEvent e;
        e.tick = tick;
        e.type = type;
        e.unit = unit;
        return e;
    }

    static int Distance(const FightUnit& a, const FightUnit& b) { return hex::Distance(a.pos, b.pos); }
    static bool InRange(const FightUnit& a, const FightUnit& b) { return Distance(a, b) <= a.range; }

    static int StatusPercent(const FightUnit& u, StatusType type) {
        int sum = 0;
        for (const StatusInst& s : u.statuses) sum += s.type == type ? s.percent : 0;
        return sum;
    }
    static int ApplyPercent(int base, int percentChange) {
        return Clamp32(std::max<long long>(0, static_cast<long long>(base) * (100 + percentChange) / 100));
    }
    // Effective max HP follows the MaxHp statuses. Current HP moves by the same amount when the maximum
    // grows (like gaining health), and is only clipped when the maximum shrinks.
    static bool AffectsMaxHp(StatusType t) { return t == StatusType::MaxHp || t == StatusType::BonusMaxHp; }
    static void RecomputeMaxHp(FightUnit& u) {
        const long long total = static_cast<long long>(ApplyPercent(u.baseMaxHp, StatusPercent(u, StatusType::MaxHp))) +
                                StatusPercent(u, StatusType::BonusMaxHp);
        const int newMax = std::max(1, Clamp32(total));
        if (newMax > u.maxHp) {
            u.hp += newMax - u.maxHp;
        } else if (newMax < u.maxHp) {
            u.hp = std::min(u.hp, newMax);
        }
        u.maxHp = newMax;
    }

    // The mana bar follows the ManaCost statuses (Rally: -10% each), never below 30% of the bar they shorten. Mana above the new bar stays: the
    // unit casts as soon as it can act.
    static void RecomputeMaxMana(FightUnit& u) {
        if (u.baseMaxMana <= 0) return;
        const long long scaled = static_cast<long long>(u.baseMaxMana) * std::max(30, 100 + StatusPercent(u, StatusType::ManaCost)) / 100;
        u.maxMana = static_cast<int>(std::max<long long>(1000, scaled));
    }

    // Returns false if the status was refused: crowd control from another unit bounces off CcImmunity.
    // (A unit's own self-applied Root, e.g. Les in wall mode, is not blocked by its own immunity.)
    static bool AddStatus(FightUnit& holder, bool selfApplied, StatusType type, int percent, int expiresTick, bool refresh = false,
                          int source = -1) {
        if (IsCcStatus(type) && !selfApplied && HasStatus(holder, StatusType::CcImmunity)) return false;
        if (type == StatusType::Wound || refresh) {
            // Wounds never stack (and any status can be marked "refreshes"): the strongest applies, for as long as
            // the latest one lasts.
            for (StatusInst& s : holder.statuses) {
                if (s.type != type) continue;
                s.percent = std::max(s.percent, percent);
                s.expiresTick = std::max(s.expiresTick, expiresTick);
                if (AffectsMaxHp(type)) RecomputeMaxHp(holder);
                return true;
            }
        }
        holder.statuses.push_back(StatusInst{type, percent, expiresTick, source});
        if (AffectsMaxHp(type)) RecomputeMaxHp(holder);
        return true;
    }

    static bool HasStatus(const FightUnit& u, StatusType type) {
        for (const StatusInst& s : u.statuses) {
            if (s.type == type) return true;  // statuses are removed the tick they expire
        }
        return false;
    }
    static bool IsCcStatus(StatusType t) { return t == StatusType::Stun || t == StatusType::Root || t == StatusType::Knockup; }
    // Cannot move, attack or cast.
    static bool IsDisabled(const FightUnit& u) {
        return HasStatus(u, StatusType::Stun) || HasStatus(u, StatusType::Knockup) || HasStatus(u, StatusType::Piloting);
    }
    // A plant that has not come to life: it never walks and never attacks.
    static bool IsStationary(const FightUnit& u) { return u.champion->stationary && !HasStatus(u, StatusType::Awakened); }
    static bool IsRooted(const FightUnit& u) { return HasStatus(u, StatusType::Root); }
    static int WoundPercent(const FightUnit& u) {  // the strongest wound applies
        int best = 0;
        for (const StatusInst& s : u.statuses) {
            if (s.type == StatusType::Wound) best = std::max(best, s.percent);
        }
        return best;
    }
    static int EffectiveAttackDamage(const FightUnit& u) {
        // Percent changes scale the base; BonusAttackDamage adds flat points on top.
        const long long bonus = StatusPercent(u, StatusType::BonusAttackDamage);
        return Clamp32(std::max<long long>(0, ApplyPercent(u.damage, StatusPercent(u, StatusType::AttackDamage)) + bonus));
    }
    static int EffectiveArmor(const FightUnit& u) {
        return Clamp32(std::max<long long>(0, static_cast<long long>(ApplyPercent(u.armor, StatusPercent(u, StatusType::Armor))) +
                                                  StatusPercent(u, StatusType::BonusArmor)));
    }
    static int EffectiveMagicResist(const FightUnit& u) {
        return Clamp32(std::max<long long>(0, static_cast<long long>(ApplyPercent(u.magicResist, StatusPercent(u, StatusType::MagicResist))) +
                                                  StatusPercent(u, StatusType::BonusMagicResist)));
    }
    // Enemies' automatic targeting (attacks, "closest enemy") skips units that dropped aggro or are untargetable...
    static bool CanAutoTarget(const FightUnit& u) {
        return !HasStatus(u, StatusType::Untargetable) && !HasStatus(u, StatusType::AggroDrop) && !HasStatus(u, StatusType::Piloting);
    }
    // ...and NO enemy effect of any kind can reach an untargetable unit.
    static bool CanBeAffectedByEnemies(const FightUnit& u) { return !HasStatus(u, StatusType::Untargetable) && !HasStatus(u, StatusType::Piloting); }
    static bool HasTrait(const FightUnit& u, const std::string& trait) {
        return std::find(u.champion->traits.begin(), u.champion->traits.end(), trait) != u.champion->traits.end() ||
               std::find(u.extraTraits.begin(), u.extraTraits.end(), trait) != u.extraTraits.end();
    }
    // (Overtime multiplies attack speed: overtimeFactor_ is 1 until the fight goes into overtime.)
    int EffectiveAttackInterval(const FightUnit& u) const {
        const int speed = std::max(10, 100 + StatusPercent(u, StatusType::AttackSpeed));
        return std::max(1, u.attackInterval * 100 / (speed * overtimeFactor_));
    }
    int MoveTicks() const { return std::max(1, config_.ticksPerHexMove / overtimeFactor_); }

    // The safety net for a fight that cannot end: more surviving units, then more total HP, then a coin from the fight's own seed. Never a draw.
    CombatWinner DecideAtLimit() {
        if (alive_[0] != alive_[1]) return alive_[0] > alive_[1] ? CombatWinner::Home : CombatWinner::Away;
        long long hp[2] = {0, 0};
        for (const FightUnit& u : units_) {
            if (u.alive && !u.isSummon) hp[u.team] += u.hp;
        }
        if (hp[0] != hp[1]) return hp[0] > hp[1] ? CombatWinner::Home : CombatWinner::Away;
        return (rng_.Next64() & 1) == 0 ? CombatWinner::Home : CombatWinner::Away;
    }

    // Whoever still has units when the other side has none. (Both sides gone on the same tick is the only draw.)
    CombatWinner Decide() const {
        if (alive_[0] == 0 && alive_[1] == 0) return CombatWinner::Draw;
        if (alive_[1] == 0) return CombatWinner::Home;
        if (alive_[0] == 0) return CombatWinner::Away;
        return CombatWinner::Draw;
    }

    // Regulation is over and nobody has won: everything that acts on a timer runs overtimeSpeed times faster from now on. Timers that were
    // already running are shortened to match, so the change is felt at once rather than up to a whole attack interval later.
    void EnterOvertime(int tick) {
        overtimeFactor_ = config_.overtimeSpeed;
        for (FightUnit& u : units_) {
            u.nextAttackTick = tick + std::max(0, u.nextAttackTick - tick) / overtimeFactor_;
            u.nextMoveTick = tick + std::max(0, u.nextMoveTick - tick) / overtimeFactor_;
        }
        CombatEvent e = MakeEvent(tick, CombatEventType::Overtime, kInvalidUnitId);
        e.amount = overtimeFactor_;   // (duration stays 0: overtime lasts until a team is wiped out)
        log_.events.push_back(e);
    }

    // Closest living enemy; ties go to the lowest UnitId (units_ is sorted, and only a strictly
    // smaller distance replaces the current best).
    int ClosestEnemy(const FightUnit& u) const {
        int best = -1;
        int bestDistance = 0;
        for (std::size_t i = 0; i < units_.size(); ++i) {
            const FightUnit& other = units_[i];
            if (!other.alive || other.team == u.team || !CanAutoTarget(other)) continue;
            const int d = Distance(u, other);
            if (best < 0 || d < bestDistance) {
                best = static_cast<int>(i);
                bestDistance = d;
            }
        }
        return best;
    }

    int ClosestEnemyInRange(const FightUnit& u) const {
        int best = -1;
        int bestDistance = 0;
        for (std::size_t i = 0; i < units_.size(); ++i) {
            const FightUnit& other = units_[i];
            if (!other.alive || other.team == u.team || !CanAutoTarget(other)) continue;
            const int d = Distance(u, other);
            if (d <= u.range && (best < 0 || d < bestDistance)) {
                best = static_cast<int>(i);
                bestDistance = d;
            }
        }
        return best;
    }

    static void SetTarget(FightUnit& u, int target) {
        u.target = target;
        if (u.ability && u.ability->resetCountOnTargetChange) u.attacksSinceCast = 0;  // "resets on target change / death"
        u.path.clear();  // any cached route belonged to the old target
        u.pathIndex = 0;
    }

    // ---- 1. EXPIRE -------------------------------------------------------------------------

    void ExpireEffects(int tick) {
        for (FightUnit& u : units_) {
            if (!u.alive) continue;
            for (std::size_t i = 0; i < u.shields.size();) {
                if (u.shields[i].expiresTick <= tick) {
                    CombatEvent e = MakeEvent(tick, CombatEventType::ShieldEnded, u.id);
                    e.amount = u.shields[i].amount;  // unused capacity
                    log_.events.push_back(e);
                    u.shields.erase(u.shields.begin() + static_cast<std::ptrdiff_t>(i));
                } else {
                    ++i;
                }
            }
            // Statuses that end this tick. Every event carries the unit's HP at that point in the stream, so the
            // max-HP statuses go LAST: removing one can clip HP, and events emitted before it must not show that.
            for (std::size_t i = 0; i < u.statuses.size();) {
                if (u.statuses[i].expiresTick <= tick && !AffectsMaxHp(u.statuses[i].type)) {
                    CombatEvent e = MakeEvent(tick, CombatEventType::StatusEnded, u.id);
                    e.subtype = static_cast<std::uint8_t>(u.statuses[i].type);
                    e.hpAfter = u.hp;
                    log_.events.push_back(e);
                    u.statuses.erase(u.statuses.begin() + static_cast<std::ptrdiff_t>(i));
                } else {
                    ++i;
                }
            }
            for (std::size_t i = 0; i < u.statuses.size();) {
                if (u.statuses[i].expiresTick <= tick) {  // only max-HP statuses are left that can be expired
                    const StatusType type = u.statuses[i].type;
                    u.statuses.erase(u.statuses.begin() + static_cast<std::ptrdiff_t>(i));
                    RecomputeMaxHp(u);
                    CombatEvent e = MakeEvent(tick, CombatEventType::StatusEnded, u.id);
                    e.subtype = static_cast<std::uint8_t>(type);
                    e.hpAfter = u.hp;
                    log_.events.push_back(e);
                } else {
                    ++i;
                }
            }
        }
    }

    // ---- 2. ACT (declare only) -------------------------------------------------------------

    void Act(int index, int tick) {
        FightUnit& u = units_[static_cast<std::size_t>(index)];
        if (IsDisabled(u) || tick < u.lockedUntil) return;

        // --- Target: only re-evaluated when it can have changed. ---
        const bool targetUsable = u.target >= 0 && units_[static_cast<std::size_t>(u.target)].alive &&
                                  CanAutoTarget(units_[static_cast<std::size_t>(u.target)]);
        if (!targetUsable) {
            SetTarget(u, ClosestEnemy(u));
            if (u.target < 0) return;
        } else if (!InRange(u, units_[static_cast<std::size_t>(u.target)])) {
            const int nearby = ClosestEnemyInRange(u);
            if (nearby >= 0 && nearby != u.target) SetTarget(u, nearby);
        }
        const FightUnit& target = units_[static_cast<std::size_t>(u.target)];

        const bool stationary = IsStationary(u);
        if (InRange(u, target) || (stationary && u.ability != nullptr)) {
            // A full mana bar pre-empts the next attack. (A stationary unit casts at whatever it would target, from where it stands.)
            if (u.ability && u.ability->trigger == CastTrigger::Mana && u.maxMana > 0 && u.mana >= u.maxMana) {
                DeclareCast(index, u.target, tick, false);
                if (u.ability->castLockTicks > 0) return;  // locked in the animation; otherwise keep attacking
            }
            if (!stationary && tick >= u.nextAttackTick && !HasStatus(u, StatusType::Blind)) DeclareAttack(index, tick);
            return;
        }
        if (stationary) return;   // a plant that has not come to life never walks

        // --- Otherwise walk toward it, one hex per ticksPerHexMove. ---
        if (IsRooted(u)) return;  // rooted units can still attack and cast, but not walk
        if (tick < u.nextMoveTick) return;
        if (!EnsurePath(u, target, tick)) return;

        const HexCoord next = u.path[u.pathIndex];
        grid_.SetBlocked(u.pos, false);
        grid_.SetBlocked(next, true);
        CombatEvent e = MakeEvent(tick, CombatEventType::Move, u.id);
        e.from = u.pos;
        e.to = next;
        e.amount = MoveTicks();
        log_.events.push_back(e);
        u.pos = next;
        ++u.pathIndex;
        u.nextMoveTick = tick + MoveTicks();
    }

    // True if `u.path[u.pathIndex]` is a valid, free next step. Only searches when the cached
    // route is unusable: the target moved, the next hex got occupied, or the route ran out.
    bool EnsurePath(FightUnit& u, const FightUnit& target, int tick) {
        const bool cachedOk = u.pathIndex < u.path.size() && u.pathGoal == target.pos &&
                              !grid_.IsBlocked(u.path[u.pathIndex]);
        if (cachedOk) return true;
        if (tick < u.nextPathTick) return false;

        ++pathSearches_;
        const bool found = pathfinder_.FindPathToRange(grid_, u.pos, target.pos, u.range, u.path);
        u.pathIndex = 0;
        u.pathGoal = target.pos;
        if (!found || u.path.empty()) {
            u.path.clear();
            u.nextPathTick = tick + config_.repathBackoffTicks;  // boxed in; don't hammer the search
            return false;
        }
        return true;
    }

    void DeclareAttack(int index, int tick) {
        FightUnit& u = units_[static_cast<std::size_t>(index)];
        FightUnit& victim = units_[static_cast<std::size_t>(u.target)];
        u.nextAttackTick = tick + EffectiveAttackInterval(u);

        int raw = EffectiveAttackDamage(u);
        std::uint8_t flags = kFlagBasic;
        if (raw > 0 && RollCrit(u)) {
            raw = CritRaw(raw, u, victim);
            flags |= kFlagCrit;
        }

        CombatEvent e = MakeEvent(tick, CombatEventType::Attack, u.id);
        e.other = victim.id;
        // Presentation: this tick is the moment of impact; the swing starts `windup` (+ `flight`) ticks earlier (see Combat.h). Faster in overtime.
        e.from = u.pos;
        e.to = victim.pos;
        const CombatStats& stats = u.champion->stats;
        const int speed = stats.projectileSpeedMilli >= 0 ? stats.projectileSpeedMilli : (stats.attackRange >= 2 ? config_.defaultRangedProjectileSpeedMilli : 0);
        e.windup = (stats.attackWindupTicks >= 0 ? stats.attackWindupTicks : config_.defaultAttackWindupTicks) / overtimeFactor_;
        if (speed > 0) {
            const long long distance = std::max(1, hex::Distance(u.pos, victim.pos));
            e.kind = 1;
            e.flight = std::max<int>(1, static_cast<int>((distance * kTicksPerSecond * 1000 + speed - 1) / speed) / overtimeFactor_);   // (a projectile is always in the air for a tick)
        }
        log_.events.push_back(e);

        if (u.maxMana > 0) {
            const int before = u.mana;
            u.mana = std::min(u.maxMana, u.mana + config_.manaPerAttackMilli);
            u.manaDirty = u.manaDirty || u.mana != before;
        }

        if (raw > 0) {
            const int spread = u.champion->stats.attackSpreadTicks;
            if (spread > 0) {
                // Spread attack: the damage arrives in equal parts over `spread` ticks.
                DotInst dot;
                dot.source = index;
                dot.key = kNoAbility;
                dot.type = u.champion->stats.attackType;
                dot.start = tick;
                dot.duration = spread;
                dot.hitsTotal = dot.hitsLeft = std::max(1, spread / config_.dotSpreadIntervalTicks);
                dot.nextTick = tick + spread / dot.hitsTotal;
                dot.remainingRaw = raw;
                dot.flags = static_cast<std::uint8_t>(flags | kFlagDot);
                dot.visible = false;
                victim.dots.push_back(dot);
            } else {
                hits_.push_back(Hit{index, u.target, u.champion->stats.attackType, raw, flags});
            }
        }

        ++u.attacksSinceCast;
        ++u.totalAttacks;
        for (TriggerSlot& slot : u.triggers) {   // item / synergy riders and counted attack hooks
            if (slot.ability->trigger != EventTrigger::OnBasicAttack && slot.ability->trigger != EventTrigger::EveryNthAttack) continue;
            if (u.totalAttacks <= slot.ability->afterAttacks[static_cast<std::size_t>(u.star - 1)]) continue;   // "after attacking N times"
            FireHook(index, slot, index, u.target, 0, true, tick);
        }
        if (u.onAttack != nullptr) {  // a rider on every basic attack: no SpellCast, no mana, no lock
            CastRecord rider;
            rider.caster = index;
            rider.target = u.target;
            rider.center = victim.pos;
            rider.ability = u.onAttack;
            casts_.push_back(rider);
        }
        if (u.ability && u.ability->trigger == CastTrigger::EveryNthAttack &&
            u.attacksSinceCast >= u.ability->attackCount) {
            DeclareCast(index, u.target, tick, false);
        }
    }

    void DeclareCast(int index, int targetIndex, int tick, bool onDeath) {
        FightUnit& u = units_[static_cast<std::size_t>(index)];
        const AbilityDefinition& ability = *u.ability;

        CastRecord cast;
        cast.caster = index;
        cast.target = targetIndex;
        cast.center = targetIndex >= 0 ? units_[static_cast<std::size_t>(targetIndex)].pos : u.pos;
        cast.ability = &ability;
        cast.rawSnapshot = (targetIndex >= 0 && u.rawCounterTarget == targetIndex) ? u.rawCounter : 0;

        u.rawCounter = 0;
        u.rawCounterTarget = -1;
        if (u.maxMana > 0 && u.mana != 0) u.manaDirty = true;
        u.mana = 0;  // drained by every cast, whatever triggered it
        u.attacksSinceCast = 0;
        ++u.castCount;

        int lock = 0;
        if (!onDeath && ability.channelTicks > 0) {
            // A channel: locked for its whole length (plus the tick its finale resolves on); interruptible.
            lock = ability.channelTicks;
            u.lockedUntil = tick + lock + 1;
            u.nextAttackTick = std::max(u.nextAttackTick, tick) + lock + 1;
            cast.serial = ++castSerial_;
            cast.channeled = true;
            u.activeChannelSerial = cast.serial;
            u.channelEnd = tick + lock;
            u.channelAbility = ability.id;
        } else if (!onDeath && ability.castLockTicks > 0) {
            lock = ability.castLockTicks;
            u.lockedUntil = tick + lock;
            u.nextAttackTick = std::max(u.nextAttackTick, tick) + lock;  // the attack timer pauses for the cast
        }
        casts_.push_back(cast);
        if (!onDeath) {
            for (TriggerSlot& slot : u.triggers) {
                if (slot.ability->trigger == EventTrigger::OnCast) FireHook(index, slot, index, targetIndex, 0, true, tick);
            }
        }

        CombatEvent e = MakeEvent(tick, CombatEventType::SpellCast, u.id);
        e.other = targetIndex >= 0 ? units_[static_cast<std::size_t>(targetIndex)].id : kInvalidUnitId;
        e.ability = ability.id;
        e.duration = lock;
        e.flags = onDeath ? kFlagOnDeath : 0;
        // Presentation: the animation's lead time, and the area the spell covers (see Combat.h).
        e.windup = (ability.windupTicks >= 0 ? ability.windupTicks : config_.defaultCastWindupTicks) / overtimeFactor_;
        const AreaDescription area = DescribeArea(ability);
        e.shape = static_cast<std::uint8_t>(area.shape);
        e.size = static_cast<std::uint8_t>(std::clamp(area.size, 0, 255));
        e.from = u.pos;
        const bool aroundTarget = area.shape == AreaShape::Circle || area.shape == AreaShape::Line || area.shape == AreaShape::Cone || area.shape == AreaShape::Single;
        e.to = aroundTarget && targetIndex >= 0 ? units_[static_cast<std::size_t>(targetIndex)].pos : u.pos;
        for (const AbilityEffect& effect : ability.effects) {   // a "largest cluster" spell: `to` is that cluster's centre as it stands now (the effects re-resolve when they land)
            HexCoord cluster;
            if (effect.target.mode == TargetMode::AreaAroundDensestEnemy && DensestEnemyCenter(index, effect.target.radius, cluster)) { e.to = cluster; break; }
        }
        log_.events.push_back(e);
    }

    // A critical hit: base + the crit bonus (config + the attacker's CritDamage), of which the victim does not take its
    // CritDamageTakenReduction share.
    int CritRaw(int raw, const FightUnit& attacker, const FightUnit& victim) const {
        const long long bonus = std::max<long long>(0, static_cast<long long>(config_.critBonusPercent) + StatusPercent(attacker, StatusType::CritDamage));
        const long long reduction = std::clamp(StatusPercent(victim, StatusType::CritDamageTakenReduction), 0, 100);
        const long long effective = bonus * (100 - reduction) / 100;
        return Clamp32(static_cast<long long>(raw) * (100 + effective) / 100);
    }

    bool RollCrit(const FightUnit& u) {
        const int chance = std::clamp(u.critChance + StatusPercent(u, StatusType::BonusCritChance), 0, 100);
        return chance > 0 && rng_.NextBelow(100) < static_cast<std::uint32_t>(chance);
    }

    // ---- 3. RESOLVE ------------------------------------------------------------------------

    void Resolve(int tick) {
        RegenerateMana(tick);
        FireIntervalHooks(tick);
        ExecuteScheduled(tick);
        ExecuteCasts(tick);
        TickDots(tick);
        ApplyHits(tick);
        ApplyStatusSweeps(tick);
        ProcessHooks(tick);
        ApplyManaFromDamage(tick);
        CheckTeamHp(tick);
        ReapDeathsAndDeathCasts(tick);
        EmitManaEvents(tick);
    }

    // EveryInterval hooks: at tick N, 2N ... each living, enabled holder with a living enemy target in attack range fires once (Twin Snipers).
    void FireIntervalHooks(int tick) {
        if (tick <= 0) return;
        for (std::size_t i = 0; i < units_.size(); ++i) {
            FightUnit& u = units_[i];
            if (!u.alive || u.triggers.empty() || IsDisabled(u) || u.target < 0) continue;
            const FightUnit& target = units_[static_cast<std::size_t>(u.target)];
            if (!target.alive || target.team == u.team || !CanBeAffectedByEnemies(target) || !InRange(u, target)) continue;
            for (TriggerSlot& slot : u.triggers) {
                if (slot.ability->trigger != EventTrigger::EveryInterval || tick % slot.ability->intervalTicks != 0) continue;
                FireHook(static_cast<int>(i), slot, static_cast<int>(i), u.target, 0, true, tick);
            }
        }
    }

    // OnTeamHpLoss hooks: the team's total HP (its real units: summons and plants excluded; the dead count 0) against its total max HP at the
    // start. Every `thresholdPercent` lost fires the hook once (a slot remembers how many steps it has seen). Scope-Team slots count per team:
    // the first living holder fires for everybody.
    void CheckTeamHp(int tick) {
        for (int team = 0; team < 2; ++team) {
            if (teamMaxHp_[team] <= 0) continue;
            long long current = 0;
            for (const FightUnit& u : units_) {
                if (u.team == team && u.alive && !u.isSummon && !u.champion->plant) current += u.hp;
            }
            const long long lost = std::max<long long>(0, teamMaxHp_[team] - current);
            for (std::size_t i = 0; i < units_.size(); ++i) {
                FightUnit& u = units_[i];
                if (u.team != team || !u.alive || u.hp <= 0) continue;
                for (TriggerSlot& slot : u.triggers) {
                    if (slot.ability->trigger != EventTrigger::OnTeamHpLoss) continue;
                    const int steps = static_cast<int>(lost * 100 / (teamMaxHp_[team] * slot.ability->thresholdPercent));
                    int* seen = &slot.counter;
                    if (slot.teamOnce) {
                        seen = nullptr;
                        for (auto& entry : teamOnce_[team]) {
                            if (entry.first == slot.ability) seen = &entry.second;
                        }
                        if (seen == nullptr) {
                            teamOnce_[team].emplace_back(slot.ability, 0);
                            seen = &teamOnce_[team].back().second;
                        }
                    }
                    while (*seen < steps && (slot.ability->maxTriggers == 0 || slot.fired < slot.ability->maxTriggers)) {
                        ++*seen;
                        FireHook(static_cast<int>(i), slot, -1, -1, 0, true, tick);
                    }
                }
            }
        }
    }

    // Statuses that act on their own: HpPerSecond (a heal, or true damage, once a second) and ExecuteBelow (dies below the threshold).
    void ApplyStatusSweeps(int tick) {
        const bool secondBoundary = tick > 0 && tick % kTicksPerSecond == 0;
        for (std::size_t i = 0; i < units_.size(); ++i) {
            FightUnit& u = units_[i];
            if (!u.alive || u.hp <= 0 || u.statuses.empty() || HasStatus(u, StatusType::Piloting)) continue;
            const int self = static_cast<int>(i);
            if (secondBoundary) {
                const std::vector<StatusInst> snapshot = u.statuses;   // damage / heals below never change the list, but stay safe
                for (const StatusInst& st : snapshot) {
                    if (st.type != StatusType::HpPerSecond || st.percent == 0 || u.hp <= 0) continue;
                    const int amount = Clamp32(static_cast<long long>(u.maxHp) * (st.percent < 0 ? -st.percent : st.percent) / 100);
                    const int source = st.source >= 0 ? st.source : self;
                    if (st.percent > 0) HealUnit(source, self, amount, tick);
                    else DealResolved(self, source, amount, amount, DamageType::True, 0, tick, nullptr, true, false);
                }
            }
            int threshold = 0;
            int source = self;
            for (const StatusInst& st : u.statuses) {
                if (st.type == StatusType::ExecuteBelow && st.percent > threshold) {
                    threshold = st.percent;
                    source = st.source >= 0 ? st.source : self;
                }
            }
            if (u.hp > 0 && threshold > 0 && static_cast<long long>(u.hp) * 100 < static_cast<long long>(threshold) * u.maxHp) {
                CombatEvent e = MakeEvent(tick, CombatEventType::Damage, u.id);   // executed: the rest of its HP, past shields
                e.other = units_[static_cast<std::size_t>(source)].id;
                e.amount = u.hp;
                e.hpAfter = 0;
                e.subtype = static_cast<std::uint8_t>(DamageType::True);
                e.flags = kFlagTriggered;
                log_.events.push_back(e);
                u.hp = 0;
            }
        }
    }

    void EmitManaEvents(int tick) {
        for (FightUnit& u : units_) {
            if (u.alive && u.manaDirty && u.maxMana > 0) {
                CombatEvent e = MakeEvent(tick, CombatEventType::ManaChanged, u.id);
                e.amount = u.mana;
                log_.events.push_back(e);
            }
            u.manaDirty = false;
        }
    }

    // Passives: every unit's StartOfCombat effects, in UnitId order, before tick 0. They produce
    // status / shield events (and could queue damage) but no SpellCast.
    void ApplyPassives() {
        const std::size_t fielded = units_.size();   // a passive that summons appends units: those run their own passive when they appear
        for (std::size_t i = 0; i < fielded; ++i) RunPassives(static_cast<int>(i));
    }
    // The champion's passive, then the StartOfCombat abilities that items / triggers carry.
    void RunPassives(int index) {
        FightUnit& u = units_[static_cast<std::size_t>(index)];
        if (HasStatus(u, StatusType::Piloting)) return;   // inside Hexa: its passive runs when it ejects
        const auto run = [&](const AbilityDefinition* ability) {
            CastRecord cast;
            cast.caster = index;
            cast.target = -1;
            cast.center = u.pos;
            cast.ability = ability;
            for (const AbilityEffect& effect : ability->effects) RunOrSchedule(cast, effect, 0);
        };
        if (u.passive != nullptr) run(u.passive);
        for (const AbilityDefinition* extra : u.extraPassives) run(extra);
    }

    void RegenerateMana(int tick) {
        for (FightUnit& u : units_) {
            if (!u.alive) continue;
            // Exact and Bresenham-like: with a constant regen the total after L+1 ticks is floor(regen * (L+1) / 30); the remainder is
            // carried, so regen bonuses that appear mid-fight (items, hooks) stay exact too.
            const int regen = std::max(0, u.regenMilli + StatusPercent(u, StatusType::BonusManaRegen));
            u.manaFraction += regen * overtimeFactor_;
            const int gain = u.manaFraction / kTicksPerSecond;
            u.manaFraction -= gain * kTicksPerSecond;
            ++u.livedTicks;
            if (u.maxMana > 0 && tick >= u.lockedUntil) u.mana = std::min<int>(u.maxMana, u.mana + gain);
        }
    }

    void ExecuteCasts(int tick) {
        // Effects may not add casts, but keep iteration safe anyway.
        std::vector<CastRecord> casts;
        casts.swap(casts_);
        // Two passes: effects that grant CC immunity land first. Otherwise a crowd-control effect and an immunity
        // cast on the same tick would resolve by UnitId order, and "who acted first" would decide who won.
        for (int pass = 0; pass < 2; ++pass) {
            for (const CastRecord& cast : casts) {
                for (const AbilityEffect& effect : cast.ability->effects) {
                    if (GrantsImmunity(effect) != (pass == 0)) continue;
                    RunOrSchedule(cast, effect, tick);
                }
            }
        }
    }

    // Runs an effect now, or schedules it: `repeatCount` runs, the first `delayTicks` from `tick`, then every interval.
    void RunOrSchedule(const CastRecord& cast, const AbilityEffect& effect, int tick, int extraDelay = 0) {
        for (int i = 0; i < effect.repeatCount; ++i) {
            const int due = extraDelay + effect.delayTicks + i * effect.repeatIntervalTicks;
            if (due <= 0) {
                ExecuteEffect(cast, effect, tick);
            } else {
                Scheduled later{tick + due, cast, &effect};
                later.cast.originTick = tick;
                scheduled_.push_back(later);
            }
        }
    }

    static bool GrantsImmunity(const AbilityEffect& effect) {
        const auto* status = std::get_if<StatusEffect>(&effect.payload);
        return status != nullptr && status->status == StatusType::CcImmunity;
    }

    void ExecuteScheduled(int tick) {
        // Entries are stored in creation order, so scanning in order also gives creation order.
        std::vector<Scheduled> due;
        for (std::size_t i = 0; i < scheduled_.size();) {
            if (scheduled_[i].due <= tick) {
                due.push_back(scheduled_[i]);
                scheduled_.erase(scheduled_.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
        for (const Scheduled& s : due) {
            if (s.cast.ability != nullptr && s.cast.ability->stopsOnDeath && !units_[static_cast<std::size_t>(s.cast.caster)].alive) continue;
            if (s.cast.channeled) {  // a channel's pulses / finale only happen while it is unbroken and its caster lives
                const FightUnit& caster = units_[static_cast<std::size_t>(s.cast.caster)];
                if (!caster.alive || caster.activeChannelSerial != s.cast.serial) continue;
            }
            ExecuteEffect(s.cast, *s.effect, tick);
        }
        for (FightUnit& u : units_) {   // channels whose time is up have completed
            if (u.activeChannelSerial != 0 && tick >= u.channelEnd) u.activeChannelSerial = 0;
        }
    }

    // A stun / knock-up breaks a channel: the rest of its pulses and its finale are cancelled.
    void InterruptChannel(int index, int tick) {
        FightUnit& u = units_[static_cast<std::size_t>(index)];
        if (u.activeChannelSerial == 0) return;
        u.activeChannelSerial = 0;
        u.lockedUntil = std::min(u.lockedUntil, tick);   // (it is disabled anyway; when that ends it may act again)
        CombatEvent e = MakeEvent(tick, CombatEventType::SpellInterrupted, u.id);
        e.ability = u.channelAbility;
        log_.events.push_back(e);
    }

    static bool SideMatches(TargetSide side, const FightUnit& caster, const FightUnit& other) {
        switch (side) {
            case TargetSide::Enemies: return other.team != caster.team;
            case TargetSide::Allies: return other.team == caster.team;
            case TargetSide::All: return true;
        }
        return false;
    }

    // The targeting rule, then the v2 extras: a per-star count, the trait / champion filters, and pilots (inside Hexa) are never reachable.
    void ResolveTargets(const CastRecord& cast, const TargetSpec& spec, int tick, std::vector<int>& out) {
        const FightUnit& caster = units_[static_cast<std::size_t>(cast.caster)];
        const int starCount = spec.countPerStar[static_cast<std::size_t>(caster.star - 1)];
        if (starCount > 0 && starCount != spec.count) {
            TargetSpec copy = spec;
            copy.count = starCount;
            ResolveTargetsRaw(cast, copy, tick, out);
        } else {
            ResolveTargetsRaw(cast, spec, tick, out);
        }
        if (spec.trait.empty() && spec.champion == 0 && !anyPilot_) return;
        out.erase(std::remove_if(out.begin(), out.end(), [&](int i) {
                      const FightUnit& u = units_[static_cast<std::size_t>(i)];
                      if (u.mech >= 0 && HasStatus(u, StatusType::Piloting)) return true;
                      if (!spec.trait.empty() && !HasTrait(u, spec.trait)) return true;
                      return spec.champion != 0 && u.champion->id != spec.champion;
                  }),
                  out.end());
    }

    void ResolveTargetsRaw(const CastRecord& cast, const TargetSpec& spec, int tick, std::vector<int>& out) {
        out.clear();
        const FightUnit& caster = units_[static_cast<std::size_t>(cast.caster)];
        // An untargetable enemy cannot be reached by ANY selector.
        auto reachable = [&](const FightUnit& other) { return other.alive && (other.team == caster.team || CanBeAffectedByEnemies(other)); };
        auto reachableEnemy = [&](const FightUnit& other) { return other.alive && other.team != caster.team && CanBeAffectedByEnemies(other); };
        switch (spec.mode) {
            case TargetMode::Self:
                if (caster.alive) out.push_back(cast.caster);
                return;
            case TargetMode::CurrentTarget:
                if (cast.target >= 0 && reachable(units_[static_cast<std::size_t>(cast.target)])) out.push_back(cast.target);
                return;
            case TargetMode::ClosestEnemies: {
                std::vector<std::pair<int, int>> ranked;  // (distance, index): index order == UnitId order breaks ties
                for (std::size_t i = 0; i < units_.size(); ++i) {
                    const FightUnit& other = units_[i];
                    if (reachableEnemy(other)) ranked.emplace_back(hex::Distance(other.pos, caster.pos), static_cast<int>(i));
                }
                std::sort(ranked.begin(), ranked.end());
                for (std::size_t i = 0; i < ranked.size() && static_cast<int>(i) < spec.count; ++i) out.push_back(ranked[i].second);
                return;
            }
            case TargetMode::LineBehindTarget: {
                if (cast.target < 0) return;
                const int direction = hex::DirectionToward(caster.pos, cast.center);
                HexCoord cursor = cast.center;
                for (int step = 0; step < spec.radius; ++step) {
                    cursor = hex::Neighbor(cursor, direction);
                    if (!hex::InBounds(cursor, kArenaColumns, kArenaRows)) break;
                    for (std::size_t i = 0; i < units_.size(); ++i) {
                        const FightUnit& other = units_[i];
                        if (reachable(other) && other.pos == cursor && SideMatches(spec.side, caster, other)) out.push_back(static_cast<int>(i));
                    }
                }
                return;
            }
            case TargetMode::ConeTowardTarget: {
                if (cast.target < 0) return;
                const int direction = hex::DirectionToward(caster.pos, cast.center);
                for (std::size_t i = 0; i < units_.size(); ++i) {
                    const FightUnit& other = units_[i];
                    if (reachableEnemy(other) && hex::InCone(caster.pos, direction, other.pos, spec.radius)) out.push_back(static_cast<int>(i));
                }
                return;
            }
            case TargetMode::AlliesInStartLine:
                for (std::size_t i = 0; i < units_.size(); ++i) {
                    const FightUnit& other = units_[i];
                    if (other.alive && other.team == caster.team && other.startRow == busiestRow_[caster.team]) out.push_back(static_cast<int>(i));
                }
                return;
            case TargetMode::HighestDamageAlly: {
                const int window = spec.windowTicks > 0 ? spec.windowTicks : 150;
                int best = -1;
                long long bestScore = 0;
                int bestAttack = 0;
                for (std::size_t i = 0; i < units_.size(); ++i) {
                    const FightUnit& other = units_[i];
                    if (!other.alive || other.team != caster.team || static_cast<int>(i) == cast.caster) continue;
                    const long long score = DamageInWindow(other, tick, window);
                    const int attack = EffectiveAttackDamage(other);
                    if (best < 0 || score > bestScore || (score == bestScore && attack > bestAttack)) {   // ties: stronger attacker, then lower UnitId
                        best = static_cast<int>(i);
                        bestScore = score;
                        bestAttack = attack;
                    }
                }
                if (best >= 0) out.push_back(best);
                return;
            }
            case TargetMode::LowestHpAlly: {
                std::vector<std::pair<int, int>> ranked;   // (hp, index): index order == UnitId order breaks ties
                for (std::size_t i = 0; i < units_.size(); ++i) {
                    const FightUnit& other = units_[i];
                    if (other.alive && other.team == caster.team) ranked.emplace_back(other.hp, static_cast<int>(i));
                }
                std::sort(ranked.begin(), ranked.end());
                const int wanted = std::max(1, spec.count);
                for (std::size_t i = 0; i < ranked.size() && static_cast<int>(i) < wanted; ++i) out.push_back(ranked[i].second);
                return;
            }
            case TargetMode::HighestHpEnemyNearTarget: {
                if (cast.target < 0) return;
                int best = -1;
                for (std::size_t i = 0; i < units_.size(); ++i) {
                    const FightUnit& other = units_[i];
                    if (!reachableEnemy(other) || hex::Distance(other.pos, cast.center) > spec.radius) continue;
                    if (best < 0 || other.hp > units_[static_cast<std::size_t>(best)].hp) best = static_cast<int>(i);
                }
                if (best >= 0) out.push_back(best);
                return;
            }
            case TargetMode::RandomEnemy: {
                std::vector<int> candidates;
                for (std::size_t i = 0; i < units_.size(); ++i) {
                    if (reachableEnemy(units_[i])) candidates.push_back(static_cast<int>(i));
                }
                if (!candidates.empty()) out.push_back(candidates[rng_.NextBelow(static_cast<std::uint32_t>(candidates.size()))]);
                return;
            }
            case TargetMode::LowestHpEnemy:
            case TargetMode::HighestHpEnemy: {
                int best = -1;
                for (std::size_t i = 0; i < units_.size(); ++i) {
                    const FightUnit& other = units_[i];
                    if (!reachableEnemy(other)) continue;
                    const bool better = best >= 0 && (spec.mode == TargetMode::LowestHpEnemy ? other.hp < units_[static_cast<std::size_t>(best)].hp
                                                                                                : other.hp > units_[static_cast<std::size_t>(best)].hp);
                    if (best < 0 || better) best = static_cast<int>(i);   // strict: ties keep the lowest UnitId
                }
                if (best >= 0) out.push_back(best);
                return;
            }
            case TargetMode::AllEnemies:
                for (std::size_t i = 0; i < units_.size(); ++i) {
                    if (reachableEnemy(units_[i])) out.push_back(static_cast<int>(i));
                }
                return;
            case TargetMode::TopDamageEnemies: {
                std::vector<std::pair<long long, int>> ranked;   // (-damage dealt, index): most damage first, ties lowest UnitId
                for (std::size_t i = 0; i < units_.size(); ++i) {
                    if (reachableEnemy(units_[i])) ranked.emplace_back(-units_[i].totalDealt, static_cast<int>(i));
                }
                std::sort(ranked.begin(), ranked.end());
                for (std::size_t i = 0; i < ranked.size() && static_cast<int>(i) < spec.count; ++i) out.push_back(ranked[i].second);
                return;
            }
            case TargetMode::AllAllies:
                for (std::size_t i = 0; i < units_.size(); ++i) {
                    if (units_[i].alive && units_[i].team == caster.team) out.push_back(static_cast<int>(i));
                }
                return;
            case TargetMode::TriggerAttacker:
            case TargetMode::TriggerVictim: {
                const int who = spec.mode == TargetMode::TriggerAttacker ? cast.triggerAttacker : cast.triggerVictim;
                if (who >= 0 && reachable(units_[static_cast<std::size_t>(who)])) out.push_back(who);
                return;
            }
            case TargetMode::AreaAroundTarget:
            case TargetMode::AreaAroundSelf:
            case TargetMode::AreaAroundDensestEnemy: {
                HexCoord center = spec.mode == TargetMode::AreaAroundSelf ? caster.pos : cast.center;
                if (spec.mode == TargetMode::AreaAroundDensestEnemy && !DensestEnemyCenter(cast.caster, spec.radius, center)) return;
                areaCenter_ = center;
                for (std::size_t i = 0; i < units_.size(); ++i) {  // ascending UnitId
                    const FightUnit& other = units_[i];
                    if (!reachable(other) || !SideMatches(spec.side, caster, other)) continue;
                    const int d = hex::Distance(other.pos, center);
                    if (d <= spec.radius && (spec.includeCenter || d >= 1)) out.push_back(static_cast<int>(i));
                }
                if (spec.count > 0 && static_cast<int>(out.size()) > spec.count) {   // "at most N": the ones nearest the centre (ties: lowest UnitId)
                    std::vector<std::pair<int, int>> ranked;
                    for (int index : out) ranked.emplace_back(hex::Distance(units_[static_cast<std::size_t>(index)].pos, center), index);
                    std::sort(ranked.begin(), ranked.end());
                    out.clear();
                    for (int k = 0; k < spec.count; ++k) out.push_back(ranked[static_cast<std::size_t>(k)].second);
                }
                return;
            }
        }
    }

    // "The largest cluster": the position of the living, targetable enemy (of `casterIndex`) that has the most such enemies within `radius` of it
    // (itself included; ties: lowest UnitId). False when there is no enemy.
    bool DensestEnemyCenter(int casterIndex, int radius, HexCoord& out) const {
        const FightUnit& caster = units_[static_cast<std::size_t>(casterIndex)];
        const auto enemy = [&](const FightUnit& o) { return o.alive && o.team != caster.team && CanBeAffectedByEnemies(o); };
        int best = -1, bestCount = -1;
        for (std::size_t i = 0; i < units_.size(); ++i) {
            if (!enemy(units_[i])) continue;
            int around = 0;
            for (std::size_t j = 0; j < units_.size(); ++j) {
                if (enemy(units_[j]) && hex::Distance(units_[j].pos, units_[i].pos) <= radius) ++around;
            }
            if (around > bestCount) { best = static_cast<int>(i); bestCount = around; }
        }
        if (best < 0) return false;
        out = units_[static_cast<std::size_t>(best)].pos;
        return true;
    }

    long long DamageToTargetInWindow(const FightUnit& u, int targetIndex, int tick, int window) const {
        long long sum = 0;
        for (const DealtEntry& d : u.dealt) {
            if (d.target == targetIndex && d.tick > tick - window) sum += d.amount;
        }
        return sum;
    }

    long long DamageInWindow(const FightUnit& u, int tick, int window) const {
        long long sum = 0;
        for (const DealtEntry& d : u.dealt) {
            if (d.tick > tick - window) sum += d.amount;
        }
        return sum;
    }

    // value = flat[star] + sum(source * percent[star] / 100), evaluated for one victim.
    long long Evaluate(const Amount& amount, const CastRecord& cast, int victim, int tick) const {
        const FightUnit& caster = units_[static_cast<std::size_t>(cast.caster)];
        const auto si = static_cast<std::size_t>(caster.star - 1);
        long long value = amount.flat[si];
        for (const ScalingTerm& term : amount.terms) {
            long long source = 0;
            switch (term.source) {
                case StatSource::None: break;
                case StatSource::SelfMaxHp: source = caster.maxHp; break;
                case StatSource::SelfCurrentHp: source = caster.hp; break;
                case StatSource::SelfAttackDamage: source = EffectiveAttackDamage(caster); break;
                case StatSource::SelfArmor: source = EffectiveArmor(caster); break;
                case StatSource::SelfMagicResist: source = EffectiveMagicResist(caster); break;
                case StatSource::DamageDealtToTargetInWindow:
                    source = cast.target >= 0 ? DamageToTargetInWindow(caster, cast.target, tick, term.windowTicks) : 0;
                    break;
                case StatSource::SelfAbilityDamage:
                    source = std::max<long long>(0, (static_cast<long long>(caster.champion->stats.abilityDamage[si]) + StatusPercent(caster, StatusType::BonusAbilityDamage)) *
                                                        std::max(0, caster.champion->stats.abilityPower + StatusPercent(caster, StatusType::AbilityPower)) / 100);
                    break;
                case StatSource::TriggerDamage: source = cast.triggerDamage; break;
                case StatSource::HighestAllyMaxHp:
                    for (const FightUnit& ally : units_) {
                        if (ally.alive && ally.team == caster.team && !ally.isSummon) source = std::max<long long>(source, ally.maxHp);
                    }
                    break;
                case StatSource::CastTargetAttackDamage:
                    source = cast.target >= 0 ? EffectiveAttackDamage(units_[static_cast<std::size_t>(cast.target)]) : 0;
                    break;
                case StatSource::CastTargetArmor:
                    source = cast.target >= 0 ? EffectiveArmor(units_[static_cast<std::size_t>(cast.target)]) : 0;
                    break;
                case StatSource::TargetMaxHp: source = victim >= 0 ? units_[static_cast<std::size_t>(victim)].maxHp : 0; break;
                case StatSource::TargetCurrentHp: source = victim >= 0 ? units_[static_cast<std::size_t>(victim)].hp : 0; break;
                case StatSource::DamageDealtInWindow: source = DamageInWindow(caster, tick, term.windowTicks); break;
                case StatSource::RawDamageDealtToTarget: source = cast.rawSnapshot; break;
                case StatSource::PlayerLevel: source = setup_.teams[caster.team].playerLevel; break;
                case StatSource::TraitGold: source = setup_.teams[caster.team].traitGold; break;
                case StatSource::TraitStarLevel:
                    for (const FightUnit& ally : units_) {
                        if (ally.team == caster.team && !ally.isSummon && !ally.champion->plant && HasTrait(ally, term.trait)) source += ally.star;
                    }
                    break;
                case StatSource::TargetCastCount: source = victim >= 0 ? units_[static_cast<std::size_t>(victim)].castCount : 0; break;
            }
            value += source * term.percent[si] / term.divisor;
        }
        return value;
    }

    void ExecuteEffect(const CastRecord& cast, const AbilityEffect& effect, int tick) {
        if (effect.condition == EffectCondition::NoDamageTakenSinceCast &&
            units_[static_cast<std::size_t>(cast.caster)].lastDamagedTick >= (cast.originTick >= 0 ? cast.originTick : tick)) {
            return;   // hurt since the cast began: the effect does not happen
        }
        ResolveTargets(cast, effect.target, tick, targets_);
        FightUnit& caster = units_[static_cast<std::size_t>(cast.caster)];
        const auto si = static_cast<std::size_t>(caster.star - 1);

        if (const auto* damage = std::get_if<DamageEffect>(&effect.payload)) {
            for (int t : targets_) {
                const long long scaled = Evaluate(damage->amount, cast, t, tick) * damage->multiplierPercent / 100;
                if (scaled <= 0) continue;
                int raw = Clamp32(scaled);
                std::uint8_t flags = kFlagAbility;
                if ((damage->canCrit || HasStatus(caster, StatusType::AbilityCrit)) && RollCrit(caster)) {
                    raw = CritRaw(raw, caster, units_[static_cast<std::size_t>(t)]);
                    flags |= kFlagCrit;
                }
                hits_.push_back(Hit{cast.caster, t, damage->type, raw, flags, damage, cast.fromHook});
                if ((flags & kFlagCrit) != 0 && damage->bounceOnCritPercent > 0) {   // a critical hit bounces on to the victim's nearest other enemy-of-the-caster
                    const HexCoord from = units_[static_cast<std::size_t>(t)].pos;
                    int best = -1, bestDistance = 0;
                    for (std::size_t i = 0; i < units_.size(); ++i) {
                        const FightUnit& other = units_[i];
                        if (static_cast<int>(i) == t || !other.alive || other.team == caster.team || !CanBeAffectedByEnemies(other)) continue;
                        const int d = hex::Distance(other.pos, from);
                        if (best < 0 || d < bestDistance) { best = static_cast<int>(i); bestDistance = d; }
                    }
                    const int bounced = static_cast<int>(static_cast<long long>(raw) * damage->bounceOnCritPercent / 100);
                    if (best >= 0 && bounced > 0) hits_.push_back(Hit{cast.caster, best, damage->type, bounced, kFlagAbility, nullptr, cast.fromHook});
                }
            }
        } else if (const auto* strike = std::get_if<AllyStrikeEffect>(&effect.payload)) {
            int ally = -1;   // the living ally of that champion: highest star, then lowest UnitId
            for (std::size_t i = 0; i < units_.size(); ++i) {
                const FightUnit& other = units_[i];
                if (!other.alive || other.team != caster.team || static_cast<int>(i) == cast.caster || other.champion == nullptr || other.champion->id != strike->ally) continue;
                if (ally < 0 || other.star > units_[static_cast<std::size_t>(ally)].star) ally = static_cast<int>(i);
            }
            if (ally >= 0) {
                FightUnit& helper = units_[static_cast<std::size_t>(ally)];
                const int pct = strike->percentOfAllyAttackDamage[static_cast<std::size_t>(helper.star - 1)];
                for (int t : targets_) {
                    const FightUnit& victim = units_[static_cast<std::size_t>(t)];
                    if (victim.team == helper.team) continue;
                    int raw = static_cast<int>(static_cast<long long>(EffectiveAttackDamage(helper)) * pct / 100);
                    if (raw <= 0) continue;
                    std::uint8_t flags = kFlagAbility;
                    if (strike->canCrit && RollCrit(helper)) {
                        raw = CritRaw(raw, helper, victim);
                        flags |= kFlagCrit;
                    }
                    CombatEvent e = MakeEvent(tick, CombatEventType::Teleport, helper.id);   // presentation: it blinks in, strikes, and is back on its own hex
                    e.from = helper.pos;
                    e.to = victim.pos;
                    e.subtype = 2;
                    e.other = victim.id;
                    log_.events.push_back(e);
                    hits_.push_back(Hit{ally, t, strike->type, raw, flags, nullptr, cast.fromHook});
                }
            }
        } else if (const auto* shield = std::get_if<ShieldEffect>(&effect.payload)) {
            for (int t : targets_) {
                int amount = Clamp32(Evaluate(shield->amount, cast, t, tick));
                const int duration = shield->permanent ? 0 : Clamp32(Evaluate(shield->duration, cast, t, tick));
                if (amount <= 0 || (!shield->permanent && duration <= 0)) continue;
                FightUnit& holder = units_[static_cast<std::size_t>(t)];
                if (shield->capped) {   // refill: only what fits under the cap is added
                    long long total = 0;
                    for (const ShieldInst& existing : holder.shields) total += existing.amount;
                    amount = static_cast<int>(std::min<long long>(amount, Evaluate(shield->cap, cast, t, tick) - total));
                    if (amount <= 0) continue;
                }
                const int amp = StatusPercent(holder, StatusType::HealingAmp);   // Tuned Oscillator: shields received are stronger too
                if (amp != 0) amount = ApplyPercent(amount, amp);
                if (amount <= 0) continue;
                holder.shields.push_back(ShieldInst{amount, shield->permanent ? std::numeric_limits<int>::max() : tick + duration,
                                                    shield->damageReductionPercent[si], cast.ability != nullptr ? cast.ability->id : kNoAbility, 0});
                CombatEvent e = MakeEvent(tick, CombatEventType::ShieldApplied, holder.id);
                e.other = caster.id;
                e.amount = amount;
                e.duration = duration;
                log_.events.push_back(e);
            }
        } else if (const auto* status = std::get_if<StatusEffect>(&effect.payload)) {
            for (int t : targets_) {
                const int duration =
                    status->permanent ? 0 : Clamp32(Evaluate(status->duration, cast, t, tick) * status->multiplierPercent / 100);
                if (!status->permanent && duration <= 0) continue;
                const bool flat = IsFlatBonusStatus(status->status);
                const bool durationOnly = status->status == StatusType::Blind || status->status == StatusType::Stun || status->status == StatusType::Root ||
                                          status->status == StatusType::Knockup || status->status == StatusType::CcImmunity ||
                                          status->status == StatusType::Untargetable || status->status == StatusType::AggroDrop ||
                                          status->status == StatusType::AbilityCrit || status->status == StatusType::SpellShield;
                const int percent = (flat || status->percentFromValue) ? Clamp32(Evaluate(status->value, cast, t, tick)) : durationOnly ? 0 : status->percent[si];
                if (flat && percent == 0) continue;   // e.g. stealing from a target with nothing to steal
                FightUnit& holder = units_[static_cast<std::size_t>(t)];
                if (!AddStatus(holder, t == cast.caster, status->status, percent,
                               status->permanent ? std::numeric_limits<int>::max() : tick + duration, status->refreshes, cast.caster)) {
                    continue;  // bounced off CC immunity: no status, no event
                }
                if (status->status == StatusType::BonusMaxMana && holder.maxMana > 0) {   // a longer bar (permanent: validated); no bar, no effect
                    holder.baseMaxMana = static_cast<int>(std::max<long long>(1000, static_cast<long long>(holder.baseMaxMana) + static_cast<long long>(percent) * 1000));
                    RecomputeMaxMana(holder);
                }
                if (status->status == StatusType::ManaCost && holder.maxMana > 0) RecomputeMaxMana(holder);   // a shorter bar (Rally)
                CombatEvent e = MakeEvent(tick, CombatEventType::StatusApplied, holder.id);
                e.other = caster.id;
                e.subtype = static_cast<std::uint8_t>(status->status);
                e.amount = percent;
                e.duration = duration;  // 0 = permanent
                e.hpAfter = holder.hp;
                log_.events.push_back(e);
                if (status->status == StatusType::Stun || status->status == StatusType::Knockup) InterruptChannel(t, tick);
            }
        } else if (const auto* mana = std::get_if<ManaEffect>(&effect.payload)) {
            for (int t : targets_) {
                FightUnit& holder = units_[static_cast<std::size_t>(t)];
                const long long gain = Evaluate(mana->amount, cast, t, tick) * 1000;
                if (holder.maxMana <= 0 || gain == 0) continue;
                const int before = holder.mana;
                holder.mana = static_cast<int>(std::clamp<long long>(static_cast<long long>(holder.mana) + gain, 0, holder.maxMana));
                holder.manaDirty = holder.manaDirty || holder.mana != before;
            }
        } else if (const auto* summon = std::get_if<SummonEffect>(&effect.payload)) {
            SummonUnits(cast, *summon, tick);
        } else if (const auto* clone = std::get_if<CloneEffect>(&effect.payload)) {
            CloneUnits(cast, *clone, tick);
        } else if (const auto* teleport = std::get_if<TeleportEffect>(&effect.payload)) {
            for (int t : targets_) TeleportUnit(t, teleport->destination, cast.target, tick);
        } else if (const auto* displace = std::get_if<DisplaceEffect>(&effect.payload)) {
            for (int t : targets_) DisplaceUnit(cast.caster, t, *displace, tick);
        } else if (const auto* heal = std::get_if<HealEffect>(&effect.payload)) {
            for (int t : targets_) HealUnit(cast.caster, t, Clamp32(Evaluate(heal->amount, cast, t, tick)), tick);
        } else if (const auto* dot = std::get_if<DotEffect>(&effect.payload)) {
            for (int t : targets_) {
                long long amount = Evaluate(dot->amount, cast, t, tick);
                const int duration = Clamp32(Evaluate(dot->duration, cast, t, tick));
                if (amount <= 0 || duration <= 0) continue;
                const int hits = std::max(1, duration / dot->intervalTicks);
                FightUnit& holder = units_[static_cast<std::size_t>(t)];

                // A refreshing burn replaces the one already running (from the same ability, whoever lit it) -- but keeps its RHYTHM: the new
                // burn's first tick lands when the old one's next tick was due. (Restarting the clock would let a fast attacker re-light the burn
                // before it ever ticked, and it would never deal damage.)
                int inheritedNext = -1;
                if (dot->refreshes) {
                    const AbilityId key = cast.ability != nullptr ? cast.ability->id : kNoAbility;
                    for (const DotInst& old : holder.dots) {
                        if (old.visible && old.key == key && old.hitsLeft > 0) inheritedNext = inheritedNext < 0 ? old.nextTick : std::min(inheritedNext, old.nextTick);
                    }
                    holder.dots.erase(std::remove_if(holder.dots.begin(), holder.dots.end(), [key](const DotInst& d) { return d.visible && d.key == key; }),
                                      holder.dots.end());
                }
                // A stack landing on a still-running stack of the same ability hits harder.
                int running = 0;
                for (const DotInst& d : holder.dots) running += (d.visible && d.key == cast.ability->id) ? 1 : 0;
                if (running > 0) amount = amount * (100 + dot->stackBonusPercent[si]) / 100;

                DotInst inst;
                inst.source = cast.caster;
                inst.key = cast.ability->id;
                inst.type = dot->type;
                inst.start = tick;
                inst.duration = duration;
                inst.hitsTotal = hits;
                inst.hitsLeft = hits;
                inst.nextTick = tick + duration / hits;
                if (inheritedNext >= 0) {   // (hit k lands at start + duration * k / hits, so start is set back one step from the inherited tick)
                    inst.nextTick = inheritedNext;
                    inst.start = inheritedNext - duration / hits;
                }
                inst.remainingRaw = Clamp32(dot->amountIsTotal ? amount : amount * hits);
                inst.flags = static_cast<std::uint8_t>(kFlagDot | kFlagAbility);
                inst.visible = true;
                inst.visual = dot->visual;
                inst.healPercent = dot->healPercent;
                holder.dots.push_back(inst);

                CombatEvent e = MakeEvent(tick, CombatEventType::StatusApplied, holder.id);
                e.other = caster.id;
                e.subtype = static_cast<std::uint8_t>(dot->visual);
                e.duration = duration;
                e.hpAfter = holder.hp;
                log_.events.push_back(e);

                // A caster carrying InflictsWound (a passive) wounds everything its DoT burns, for as long as it burns.
                const int wound = StatusPercent(caster, StatusType::InflictsWound);
                if (wound > 0) {
                    AddStatus(holder, t == cast.caster, StatusType::Wound, wound, tick + duration);
                    CombatEvent w = MakeEvent(tick, CombatEventType::StatusApplied, holder.id);
                    w.other = caster.id;
                    w.subtype = static_cast<std::uint8_t>(StatusType::Wound);
                    w.amount = wound;
                    w.duration = duration;
                    w.hpAfter = holder.hp;
                    log_.events.push_back(w);
                }
            }
        }
    }

    void TickDots(int tick) {
        for (std::size_t i = 0; i < units_.size(); ++i) {
            FightUnit& u = units_[i];
            if (!u.alive || u.dots.empty()) continue;
            std::uint64_t visualsBefore = 0;   // one bit per visible DoT visual (StatusType values are < 64)
            for (const DotInst& d : u.dots) visualsBefore |= d.visible ? 1ull << static_cast<unsigned>(d.visual) : 0;

            for (DotInst& d : u.dots) {
                if (tick < d.nextTick || d.hitsLeft <= 0) continue;
                const int raw = d.remainingRaw / d.hitsLeft;
                d.remainingRaw -= raw;
                --d.hitsLeft;
                if (d.hitsLeft > 0) {
                    d.nextTick = d.start + static_cast<int>(static_cast<long long>(d.duration) * (d.hitsTotal - d.hitsLeft + 1) / d.hitsTotal);
                }
                if (raw > 0) {
                    Hit hit{d.source, static_cast<int>(i), d.type, raw, d.flags};
                    hit.healPercent = d.healPercent;
                    hit.kind = d.visible ? static_cast<std::uint8_t>(d.visual) : 0;
                    hits_.push_back(hit);
                }
            }
            u.dots.erase(std::remove_if(u.dots.begin(), u.dots.end(), [](const DotInst& d) { return d.hitsLeft <= 0; }),
                         u.dots.end());

            std::uint64_t visualsAfter = 0;
            for (const DotInst& d : u.dots) visualsAfter |= d.visible ? 1ull << static_cast<unsigned>(d.visual) : 0;
            for (unsigned bit = 0; bit < 64; ++bit) {   // each typed DoT that has just run out ends on its own (Burn and Poison can overlap)
                const std::uint64_t mask = 1ull << bit;
                if ((visualsBefore & mask) == 0 || (visualsAfter & mask) != 0) continue;
                CombatEvent e = MakeEvent(tick, CombatEventType::StatusEnded, u.id);
                e.subtype = static_cast<std::uint8_t>(bit);
                e.hpAfter = u.hp;
                log_.events.push_back(e);
            }
        }
    }

    // Everything queued this tick lands now: mitigation -> tether split -> shield reduction -> shield absorption -> HP.
    void ApplyHits(int tick) {
        for (const Hit& h : hits_) {
            FightUnit& victim = units_[static_cast<std::size_t>(h.target)];
            if (!victim.alive || victim.hp <= 0) continue;  // already killed earlier in this batch
            FightUnit& attacker = units_[static_cast<std::size_t>(h.source)];
            // A spell shield swallows the next enemy ABILITY hit whole (not damage over time), and is used up.
            if ((h.flags & kFlagAbility) != 0 && (h.flags & kFlagDot) == 0 && attacker.team != victim.team && ConsumeSpellShield(victim, tick)) continue;
            // Damage amp multiplies what the attacker deals, before armor / resist.
            const int raw = ApplyPercent(h.raw, StatusPercent(attacker, StatusType::DamageAmp));

            int damage = raw;
            const int penetration = h.effect != nullptr ? std::clamp(h.effect->armorPenPercent, 0, 100) : 0;
            switch (h.type) {
                case DamageType::Physical: damage = MitigatedDamage(raw, EffectiveArmor(victim) * (100 - penetration) / 100, config_.minDamage); break;
                case DamageType::Magic: damage = MitigatedDamage(raw, EffectiveMagicResist(victim) * (100 - penetration) / 100, config_.minDamage); break;
                case DamageType::True: break;
            }
            const int takenPercent = StatusPercent(victim, StatusType::DamageTaken);   // "takes 10% more damage"
            if (takenPercent != 0 && damage > 0) damage = ApplyPercent(damage, takenPercent);

            // A tether sends its share of the (already mitigated) hit to the tether's source, as true damage.
            int redirected = 0;
            int tetherTo = -1;
            for (const StatusInst& s : victim.statuses) {
                if (s.type != StatusType::Tether || s.source < 0 || s.source == h.target) continue;
                const FightUnit& source = units_[static_cast<std::size_t>(s.source)];
                if (!source.alive || source.hp <= 0) continue;
                tetherTo = s.source;
                redirected = static_cast<int>(static_cast<long long>(damage) * std::clamp(s.percent, 0, 100) / 100);
                break;
            }
            damage -= redirected;

            DealResolved(h.target, h.source, damage, raw, h.type, h.flags, tick, h.effect, h.fromHook, true, h.kind);
            if (h.healPercent > 0 && attacker.alive) HealUnit(h.source, h.source, Clamp32(static_cast<long long>(damage) * h.healPercent / 100), tick);
            if (redirected > 0) {
                const auto flags = static_cast<std::uint8_t>((h.flags & ~kFlagBasic) | kFlagRedirected);
                DealResolved(tetherTo, h.source, redirected, 0, DamageType::True, flags, tick, nullptr, h.fromHook);
            }
        }
        hits_.clear();
    }

    // The tail of a hit, once its final damage is known: shield reduction and absorption, HP, the event, and the
    // bookkeeping that formulas / mana / kill effects read. `raw` is the pre-mitigation damage (0 for a redirected share).
    void DealResolved(int victimIndex, int attackerIndex, int damage, int raw, DamageType type, std::uint8_t flags, int tick,
                      const DamageEffect* effect, bool fromHook, bool credit = true, std::uint8_t kind = 0) {
        FightUnit& victim = units_[static_cast<std::size_t>(victimIndex)];
        FightUnit& attacker = units_[static_cast<std::size_t>(attackerIndex)];

        int reduction = 0;  // the best reduction among shields that still hold something
        for (const ShieldInst& s : victim.shields) {
            if (s.amount > 0) reduction = std::max(reduction, s.reductionPercent);
        }
        if (reduction > 0) damage = static_cast<int>(static_cast<long long>(damage) * (100 - reduction) / 100);

        int absorbed = 0;
        int remaining = damage;
        for (std::size_t i = 0; i < victim.shields.size() && remaining > 0; ++i) {  // oldest first
            const int taken = std::min(victim.shields[i].amount, remaining);
            victim.shields[i].amount -= taken;
            victim.shields[i].absorbed = Clamp32(static_cast<long long>(victim.shields[i].absorbed) + taken);
            remaining -= taken;
            absorbed += taken;
        }
        victim.hp = std::max(0, victim.hp - remaining);

        CombatEvent e = MakeEvent(tick, CombatEventType::Damage, victim.id);
        e.other = attacker.id;
        e.amount = damage;
        e.absorbed = absorbed;
        e.hpAfter = victim.hp;
        e.subtype = static_cast<std::uint8_t>(type);
        e.flags = static_cast<std::uint8_t>(flags | (fromHook ? kFlagTriggered : 0));
        e.kind = kind;
        log_.events.push_back(e);

        // Shields that were fully used up end now (and count as "broken").
        std::vector<std::pair<AbilityId, int>> broken;   // (the ability that made the shield, the damage it stored)
        for (std::size_t i = 0; i < victim.shields.size();) {
            if (victim.shields[i].amount <= 0) {
                CombatEvent ended = MakeEvent(tick, CombatEventType::ShieldEnded, victim.id);
                log_.events.push_back(ended);
                broken.emplace_back(victim.shields[i].source, victim.shields[i].absorbed);
                victim.shields.erase(victim.shields.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
        for (const auto& [source, stored] : broken) {
            for (TriggerSlot& slot : victim.triggers) {
                if (slot.ability->trigger != EventTrigger::OnShieldBreak) continue;
                if (slot.ability->shieldFromAbility != kNoAbility && slot.ability->shieldFromAbility != source) continue;
                FireHook(victimIndex, slot, attackerIndex, victimIndex, stored, false, tick);
            }
        }

        // Omnivamp: the attacker heals a share of what it dealt (not of a redirected share, not from a zone's per-second damage).
        if (credit && damage > 0 && attacker.alive && attackerIndex != victimIndex) {
            const int vamp = StatusPercent(attacker, StatusType::Omnivamp);
            if (vamp > 0) HealUnit(attackerIndex, attackerIndex, Clamp32(static_cast<long long>(damage) * vamp / 100), tick);
        }
        // Regeneration from damage taken (Les in wall mode): heals a share of every hit, after the damage event.
        const int regen = StatusPercent(victim, StatusType::DamageTakenRegen);
        if (regen > 0 && victim.hp > 0 && damage > 0) {
            HealUnit(victimIndex, victimIndex, Clamp32(static_cast<long long>(damage) * regen / 100), tick);
        }

        // The hit that brings a unit to 0 remembers its on-kill statuses; they are granted when the death is processed.
        if (victim.hp == 0 && victim.killEffect == nullptr && effect != nullptr && !effect->onKill.empty()) {
            victim.killer = attackerIndex;
            victim.killEffect = effect;
        }

        // Bookkeeping that later formulas / mana read.
        if (damage > 0) victim.lastDamagedTick = tick;
        if (credit) {   // (a zone's per-second damage is not "damage this unit dealt": formulas and ally ranking do not see it)
            attacker.totalDealt += damage;
            attacker.dealt.push_back(DealtEntry{tick, damage, victimIndex});
            while (!attacker.dealt.empty() && attacker.dealt.front().tick <= tick - kDealtHistoryTicks) {
                attacker.dealt.erase(attacker.dealt.begin());
            }
        }
        if (credit && (flags & kFlagBasic) != 0) {
            if (attacker.rawCounterTarget != victimIndex) {
                attacker.rawCounterTarget = victimIndex;
                attacker.rawCounter = 0;
            }
            attacker.rawCounter = Clamp32(static_cast<long long>(attacker.rawCounter) + raw);
        }
        if (victim.maxMana > 0) {
            victim.manaFromDamage = Clamp32(static_cast<long long>(victim.manaFromDamage) +
                                            static_cast<long long>(raw) * 1000 / config_.rawDamagePerMana);
        }

        DispatchDamageHooks(victimIndex, attackerIndex, damage, flags, tick, fromHook);
    }

    // ---- hooks ---------------------------------------------------------------------------------

    // Counts one occurrence of the hook's event and, if it is the Nth (and the cap allows), queues its effects. `immediate` hooks fire during
    // the Act phase and resolve with this tick's casts; the others (damage hooks) wait in a list until the tick's damage has landed.
    void FireHook(int holder, TriggerSlot& slot, int attacker, int victim, int damage, bool immediate, int tick) {
        const AbilityDefinition& ability = *slot.ability;
        if (ability.maxTriggers > 0 && slot.fired >= ability.maxTriggers) return;
        if (ability.requiresCharge && !SpendCharge(holder, tick)) return;
        if (ability.attackCount > 1) {
            if (++slot.counter < ability.attackCount) return;
            slot.counter = 0;
        }
        ++slot.fired;
        const FightUnit& h = units_[static_cast<std::size_t>(holder)];
        CastRecord cast;
        cast.caster = holder;
        // What "the current target" means: whoever hurt the holder, or whoever the holder / its ally hurt.
        const bool takeHook = ability.trigger == EventTrigger::OnTakeBasicAttackDamage || ability.trigger == EventTrigger::OnTakeAbilityDamage ||
                              ability.trigger == EventTrigger::OnCritTaken || ability.trigger == EventTrigger::OnHpDropBelowPercent ||
                              ability.trigger == EventTrigger::OnShieldBreak;
        cast.target = takeHook ? attacker : victim;
        cast.center = cast.target >= 0 ? units_[static_cast<std::size_t>(cast.target)].pos : h.pos;
        cast.ability = &ability;
        cast.triggerAttacker = attacker;
        cast.triggerVictim = victim;
        cast.triggerDamage = damage;
        cast.fromHook = true;
        if (immediate) casts_.push_back(cast);
        else pendingHooks_.push_back(cast);
    }

    // "Your next N basic attacks ...": spends one EmpoweredAttack charge of the holder; false if it has none.
    bool SpendCharge(int holder, int tick) {
        FightUnit& u = units_[static_cast<std::size_t>(holder)];
        for (std::size_t i = 0; i < u.statuses.size(); ++i) {
            if (u.statuses[i].type != StatusType::EmpoweredAttack) continue;
            if (--u.statuses[i].percent <= 0) {
                u.statuses.erase(u.statuses.begin() + static_cast<std::ptrdiff_t>(i));
                CombatEvent e = MakeEvent(tick, CombatEventType::StatusEnded, u.id);
                e.subtype = static_cast<std::uint8_t>(StatusType::EmpoweredAttack);
                e.hpAfter = u.hp;
                log_.events.push_back(e);
            }
            return true;
        }
        return false;
    }

    // Called after every hit lands. Hits caused by a hook's own effects fire no hooks (no chain reactions) -- except that they can still
    // push a unit below an HP threshold.
    void DispatchDamageHooks(int victimIndex, int attackerIndex, int damage, std::uint8_t flags, int tick, bool fromHook) {
        FightUnit& victim = units_[static_cast<std::size_t>(victimIndex)];
        if (victim.alive && victim.hp > 0) {
            for (TriggerSlot& slot : victim.triggers) {
                if (slot.ability->trigger != EventTrigger::OnHpDropBelowPercent) continue;
                const bool below = static_cast<long long>(victim.hp) * 100 < static_cast<long long>(slot.ability->thresholdPercent) * victim.maxHp;
                if (slot.armed && below) {
                    slot.armed = false;
                    FireHook(victimIndex, slot, attackerIndex, victimIndex, damage, false, tick);
                } else if (!slot.armed && !below && slot.ability->maxTriggers != 1) {
                    slot.armed = true;
                }
            }
        }
        if (fromHook || (flags & kFlagRedirected) != 0 || damage <= 0) return;

        const bool basic = (flags & kFlagBasic) != 0;
        const bool ability = (flags & kFlagAbility) != 0;
        if (victim.alive && victim.hp > 0 && (flags & kFlagDot) == 0) {   // damage-over-time ticks are not "being hit"
            for (TriggerSlot& slot : victim.triggers) {
                const EventTrigger t = slot.ability->trigger;
                if ((t == EventTrigger::OnTakeBasicAttackDamage && basic) || (t == EventTrigger::OnTakeAbilityDamage && ability) ||
                    (t == EventTrigger::OnCritTaken && (flags & kFlagCrit) != 0)) {
                    FireHook(victimIndex, slot, attackerIndex, victimIndex, damage, false, tick);
                }
            }
        }
        const auto matches = [&](const AbilityDefinition& a) {
            return a.damageFilter == DamageFilter::Any || (a.damageFilter == DamageFilter::Basic && basic) || (a.damageFilter == DamageFilter::Ability && ability);
        };
        FightUnit& attacker = units_[static_cast<std::size_t>(attackerIndex)];
        if (attacker.alive) {
            for (TriggerSlot& slot : attacker.triggers) {
                if (slot.ability->trigger == EventTrigger::OnDealDamage && matches(*slot.ability)) {
                    FireHook(attackerIndex, slot, attackerIndex, victimIndex, damage, false, tick);
                }
            }
        }
        for (std::size_t i = 0; i < units_.size(); ++i) {   // allies of the attacker, in unit order
            FightUnit& ally = units_[i];
            if (!ally.alive || ally.team != attacker.team || static_cast<int>(i) == attackerIndex) continue;
            for (TriggerSlot& slot : ally.triggers) {
                if (slot.ability->trigger == EventTrigger::OnAllyDealDamage && matches(*slot.ability)) {
                    FireHook(static_cast<int>(i), slot, attackerIndex, victimIndex, damage, false, tick);
                }
            }
        }
    }

    // Runs the hooks queued by the tick's damage, then lands whatever damage THEY made (which fires no further hooks), up to a bound.
    void ProcessHooks(int tick) {
        for (int round = 0; round < kMaxHookRounds && !pendingHooks_.empty(); ++round) {
            std::vector<CastRecord> batch;
            batch.swap(pendingHooks_);
            for (int pass = 0; pass < 2; ++pass) {
                for (const CastRecord& cast : batch) {
                    if (!units_[static_cast<std::size_t>(cast.caster)].alive) continue;
                    for (const AbilityEffect& effect : cast.ability->effects) {
                        if (GrantsImmunity(effect) != (pass == 0)) continue;
                        RunOrSchedule(cast, effect, tick);
                    }
                }
            }
            ApplyHits(tick);
        }
        pendingHooks_.clear();
    }

    // A SpellShield status swallows one ability hit.
    bool ConsumeSpellShield(FightUnit& victim, int tick) {
        for (std::size_t i = 0; i < victim.statuses.size(); ++i) {
            if (victim.statuses[i].type != StatusType::SpellShield) continue;
            victim.statuses.erase(victim.statuses.begin() + static_cast<std::ptrdiff_t>(i));
            CombatEvent e = MakeEvent(tick, CombatEventType::StatusEnded, victim.id);
            e.subtype = static_cast<std::uint8_t>(StatusType::SpellShield);
            e.hpAfter = victim.hp;
            log_.events.push_back(e);
            return true;
        }
        return false;
    }

    // ---- auras ---------------------------------------------------------------------------------

    // Recomputes who stands inside each aura. Units that came in gain the status (permanent, visible in the stream), units that left -- or
    // whose holder died -- lose it.
    void UpdateAuras(int tick) {
        for (std::size_t i = 0; i < units_.size(); ++i) {
            FightUnit& holder = units_[i];
            for (std::size_t a = 0; a < holder.auras.size(); ++a) {
                const AuraDefinition& aura = *holder.auras[a];
                const int key = static_cast<int>(i) * 32 + static_cast<int>(a) + 1;
                std::vector<int> now;
                if (holder.alive) {
                    for (std::size_t j = 0; j < units_.size(); ++j) {
                        const FightUnit& other = units_[j];
                        if (!other.alive) continue;
                        const bool sideOk = aura.side == TargetSide::All ||
                                            (aura.side == TargetSide::Enemies && other.team != holder.team && CanBeAffectedByEnemies(other)) ||
                                            (aura.side == TargetSide::Allies && other.team == holder.team && (j != i || aura.includeSelf));
                        if (sideOk && hex::Distance(holder.pos, other.pos) <= aura.radius) now.push_back(static_cast<int>(j));
                    }
                }
                std::vector<int>& before = holder.auraMembers[a];
                for (int j : before) {   // left the aura (or the holder died)
                    if (std::find(now.begin(), now.end(), j) != now.end()) continue;
                    FightUnit& other = units_[static_cast<std::size_t>(j)];
                    if (!other.alive) continue;   // a death already cleared its statuses
                    for (std::size_t s = 0; s < other.statuses.size(); ++s) {
                        if (other.statuses[s].aura != key) continue;
                        const StatusType type = other.statuses[s].type;
                        other.statuses.erase(other.statuses.begin() + static_cast<std::ptrdiff_t>(s));
                        if (AffectsMaxHp(type)) RecomputeMaxHp(other);
                        CombatEvent e = MakeEvent(tick, CombatEventType::StatusEnded, other.id);
                        e.subtype = static_cast<std::uint8_t>(type);
                        e.hpAfter = other.hp;
                        log_.events.push_back(e);
                        break;
                    }
                }
                for (int j : now) {   // came into it
                    if (std::find(before.begin(), before.end(), j) != before.end()) continue;
                    FightUnit& other = units_[static_cast<std::size_t>(j)];
                    StatusInst inst{aura.status, aura.amount, std::numeric_limits<int>::max(), static_cast<int>(i), key};
                    other.statuses.push_back(inst);   // pushed directly: an aura never merges into a same-type status
                    if (AffectsMaxHp(aura.status)) RecomputeMaxHp(other);
                    CombatEvent e = MakeEvent(tick, CombatEventType::StatusApplied, other.id);
                    e.other = holder.id;
                    e.subtype = static_cast<std::uint8_t>(aura.status);
                    e.amount = aura.amount;
                    e.hpAfter = other.hp;   // duration 0 = for as long as the aura lasts
                    log_.events.push_back(e);
                }
                before = std::move(now);
            }
        }
    }

    // ---- summons -------------------------------------------------------------------------------

    const ChampionDefinition* FindSummon(ChampionId id) const {
        if (summons_ != nullptr) {
            if (const ChampionDefinition* c = summons_->Find(id)) return c->summon ? c : nullptr;
        }
        if (summons2_ != nullptr) {
            if (const ChampionDefinition* c = summons2_->Find(id)) return c;
        }
        return nullptr;
    }

    // The free hexes nearest `center`: ring by ring, row by row (deterministic).
    bool FreeHexNear(HexCoord center, HexCoord& out) const {
        for (int ring = 1; ring <= 3; ++ring) {
            for (int y = 0; y < kArenaRows; ++y) {
                for (int x = 0; x < kArenaColumns; ++x) {
                    const HexCoord h{x, y};
                    if (hex::Distance(h, center) == ring && !grid_.IsBlocked(h)) {
                        out = h;
                        return true;
                    }
                }
            }
        }
        return false;
    }

    void SummonUnits(const CastRecord& cast, const SummonEffect& effect, int tick) {
        const ChampionDefinition* def = FindSummon(effect.champion);
        if (def == nullptr || !def->stats.IsCombatCapable()) return;
        const int summonerIndex = cast.caster;
        const long long count = std::clamp<long long>(Evaluate(effect.count, cast, summonerIndex, tick), 0, 8);
        const int star = effect.starLevel > 0 ? effect.starLevel : units_[static_cast<std::size_t>(summonerIndex)].star;
        const long long hp = Evaluate(effect.maxHp, cast, summonerIndex, tick);
        const long long attack = Evaluate(effect.attackDamage, cast, summonerIndex, tick);
        for (long long k = 0; k < count; ++k) {
            const FightUnit& summoner = units_[static_cast<std::size_t>(summonerIndex)];
            HexCoord where;
            if (!summoner.alive || !FreeHexNear(summoner.pos, where)) return;
            if (SpawnSummon(def, star, summoner.team, where, summonerIndex, hp, attack, tick) < 0) return;
        }
    }

    // A summon appears on `where` (a free hex) for `team`: event, grid, its own passive. `hp` / `attack` > 0 replace its stats. -1 = the cap is reached.
    int SpawnSummon(const ChampionDefinition* def, int star, int team, HexCoord where, int summonerIndex, long long hp, long long attack, int tick) {
        if (summonsSpawned_ >= kMaxSummonsPerFight || units_.size() >= units_.capacity()) return -1;
        const UnitId summonerId = units_[static_cast<std::size_t>(summonerIndex)].id;
        FightUnit u = MakeUnit(def, star, team, where, kSummonUnitBase + static_cast<UnitId>(++summonsSpawned_), {});
        u.isSummon = true;
        u.summoner = summonerIndex;
        u.startRow = where.y;
        if (hp > 0) u.maxHp = u.hp = u.baseMaxHp = Clamp32(hp);
        if (attack > 0) u.damage = Clamp32(attack);
        units_.push_back(std::move(u));
        const int index = static_cast<int>(units_.size()) - 1;
        FightUnit& made = units_.back();
        grid_.SetBlocked(made.pos, true);
        ++summonAlive_[team];

        CombatEvent e = MakeEvent(tick, CombatEventType::Spawn, made.id);
        e.team = static_cast<std::uint8_t>(team);
        e.to = made.pos;
        e.amount = made.maxHp;
        e.hpAfter = made.hp;
        e.champion = def->id;
        e.star = static_cast<std::uint8_t>(made.star);
        e.manaMax = made.maxMana;
        e.manaRegen = made.regenMilli;
        e.other = summonerId;
        e.flags = kFlagSummon;
        log_.events.push_back(e);
        if (made.maxMana > 0 && made.mana > 0) {
            CombatEvent m = MakeEvent(tick, CombatEventType::ManaChanged, made.id);
            m.amount = made.mana;
            log_.events.push_back(m);
        }
        RunPassives(index);   // a summon's own passive (e.g. untargetable) applies the moment it appears
        return index;
    }

    // Superior Lifeform: copies of the caster team's strongest units of a trait (cost, then star, then lowest UnitId), next to each original.
    void CloneUnits(const CastRecord& cast, const CloneEffect& effect, int tick) {
        const int team = units_[static_cast<std::size_t>(cast.caster)].team;
        std::vector<int> ranked;
        for (std::size_t i = 0; i < units_.size(); ++i) {
            const FightUnit& u = units_[i];
            if (u.alive && u.team == team && !u.isSummon && !u.champion->plant && HasTrait(u, effect.trait) && !HasStatus(u, StatusType::Piloting)) {
                ranked.push_back(static_cast<int>(i));
            }
        }
        std::sort(ranked.begin(), ranked.end(), [&](int a, int b) {
            const FightUnit& ua = units_[static_cast<std::size_t>(a)];
            const FightUnit& ub = units_[static_cast<std::size_t>(b)];
            if (ua.champion->cost != ub.champion->cost) return ua.champion->cost > ub.champion->cost;
            if (ua.star != ub.star) return ua.star > ub.star;
            return a < b;
        });
        for (std::size_t k = 0; k < ranked.size() && static_cast<int>(k) < effect.count; ++k) {
            const FightUnit& original = units_[static_cast<std::size_t>(ranked[k])];
            HexCoord where;
            if (!FreeHexNear(original.pos, where)) continue;
            const long long hp = static_cast<long long>(original.baseMaxHp) * effect.statPercent / 100;
            const long long attack = std::max<long long>(1, static_cast<long long>(original.damage) * effect.statPercent / 100);
            SpawnSummon(original.champion, original.star, team, where, cast.caster, hp, attack, tick);
        }
    }

    // The fight is over: every summon still standing vanishes (a Death event each, so the client removes it).
    void DespawnSummons(int tick) {
        for (FightUnit& u : units_) {
            if (!u.isSummon || !u.alive) continue;
            u.alive = false;
            u.hp = 0;
            --summonAlive_[u.team];
            grid_.SetBlocked(u.pos, false);
            CombatEvent e = MakeEvent(tick, CombatEventType::Death, u.id);
            e.flags = kFlagSummon;
            log_.events.push_back(e);
        }
    }

    // Restores HP, reduced by the target's Wound, capped at its max HP. Silent if nothing is restored.
    void HealUnit(int sourceIndex, int targetIndex, int amount, int tick) {
        FightUnit& target = units_[static_cast<std::size_t>(targetIndex)];
        if (!target.alive || amount <= 0) return;
        const int amp = StatusPercent(target, StatusType::HealingAmp);   // Tuned Oscillator
        if (amp != 0) amount = ApplyPercent(amount, amp);
        if (amount <= 0) return;
        const int wound = std::clamp(WoundPercent(target), 0, 100);
        const int effective = Clamp32(static_cast<long long>(amount) * (100 - wound) / 100);  // wound 30% -> x0.7
        const int restored = std::min(effective, target.maxHp - target.hp);
        if (restored <= 0) return;
        target.hp += restored;
        CombatEvent e = MakeEvent(tick, CombatEventType::Heal, target.id);
        e.other = units_[static_cast<std::size_t>(sourceIndex)].id;
        e.amount = restored;
        e.reduced = amount - effective;
        e.hpAfter = target.hp;
        log_.events.push_back(e);
    }

    // Items: each flat stat becomes a permanent status on tick 0, so it is visible in the event stream (and to the
    // replay validator) exactly like a passive. Attack speed simply adds to the other attack-speed bonuses.
    void ApplyItems() {
        for (std::size_t i = 0; i < units_.size(); ++i) {
            FightUnit& u = units_[i];
            for (const ItemDefinition* item : u.items) {
                const ItemStats& st = item->stats;
                auto grant = [&](StatusType type, int value) {
                    if (value == 0) return;
                    AddStatus(u, true, type, value, std::numeric_limits<int>::max(), false, static_cast<int>(i));
                    CombatEvent e = MakeEvent(0, CombatEventType::StatusApplied, u.id);
                    e.other = u.id;
                    e.subtype = static_cast<std::uint8_t>(type);
                    e.amount = value;
                    e.hpAfter = u.hp;
                    log_.events.push_back(e);
                };
                grant(StatusType::BonusMaxHp, st.maxHp);
                grant(StatusType::BonusArmor, st.armor);
                grant(StatusType::BonusMagicResist, st.magicResist);
                grant(StatusType::BonusAttackDamage, st.attackDamage);
                grant(StatusType::BonusAbilityDamage, st.abilityDamage);
                grant(StatusType::AttackSpeed, st.attackSpeedPercent);
                grant(StatusType::BonusCritChance, st.critChance);
                grant(StatusType::BonusManaRegen, st.manaRegenMilli);
                if (u.maxMana > 0) u.mana = std::min(u.maxMana, u.mana + st.startMana * 1000);
            }
        }
    }

    // Units that start with mana (a stat or an item) tell the client so its bar starts right.
    void EmitInitialMana() {
        for (const FightUnit& u : units_) {
            if (u.maxMana > 0 && u.mana > 0) {
                CombatEvent e = MakeEvent(0, CombatEventType::ManaChanged, u.id);
                e.amount = u.mana;
                log_.events.push_back(e);
            }
        }
    }

    // An assassin dive: land on the free hex next to the chosen enemy, on the side away from where the unit started ("behind"
    // it); try the ring of hexes at distance 1 first, then 2. Stays put if nothing is free. The original destinations (farthest /
    // closest enemy) leave the unit without a target; the new ones (lowest / highest HP, the cast target) make the enemy its target.
    void TeleportUnit(int index, TeleportDestination destination, int castTarget, int tick) {
        FightUnit& u = units_[static_cast<std::size_t>(index)];
        if (!u.alive) return;
        int enemy = -1;
        bool retarget = true;
        switch (destination) {
            case TeleportDestination::BehindCurrentTarget: {
                if (castTarget < 0) return;
                const FightUnit& t = units_[static_cast<std::size_t>(castTarget)];
                if (!t.alive || t.team == u.team || !CanBeAffectedByEnemies(t)) return;
                enemy = castTarget;
                break;
            }
            case TeleportDestination::NextToLowestHpEnemy:
            case TeleportDestination::NextToHighestHpEnemy:
                for (std::size_t i = 0; i < units_.size(); ++i) {
                    const FightUnit& other = units_[i];
                    if (!other.alive || other.team == u.team || !CanBeAffectedByEnemies(other)) continue;
                    const bool better = enemy >= 0 && (destination == TeleportDestination::NextToLowestHpEnemy ? other.hp < units_[static_cast<std::size_t>(enemy)].hp
                                                                                                              : other.hp > units_[static_cast<std::size_t>(enemy)].hp);
                    if (enemy < 0 || better) enemy = static_cast<int>(i);   // strict: ties keep the lowest UnitId
                }
                break;
            case TeleportDestination::BehindFarthestEnemy:
            case TeleportDestination::BehindClosestEnemy: {
                retarget = false;
                int enemyDistance = 0;
                for (std::size_t i = 0; i < units_.size(); ++i) {
                    const FightUnit& other = units_[i];
                    if (!other.alive || other.team == u.team) continue;
                    const int d = hex::Distance(u.pos, other.pos);
                    const bool better = destination == TeleportDestination::BehindFarthestEnemy ? d > enemyDistance : d < enemyDistance;
                    if (enemy < 0 || better) {   // strict: ties keep the lowest UnitId
                        enemy = static_cast<int>(i);
                        enemyDistance = d;
                    }
                }
                break;
            }
        }
        if (enemy < 0) return;
        const HexCoord enemyPos = units_[static_cast<std::size_t>(enemy)].pos;

        HexCoord best{-1, -1};
        int bestScore = -1;
        for (int ring = 1; ring <= 2 && bestScore < 0; ++ring) {
            for (int y = 0; y < kArenaRows; ++y) {
                for (int x = 0; x < kArenaColumns; ++x) {
                    const HexCoord h{x, y};
                    if (hex::Distance(h, enemyPos) != ring || grid_.IsBlocked(h)) continue;
                    const int score = hex::Distance(h, u.pos);   // farthest from the start = the far side of the enemy
                    if (score > bestScore) {
                        bestScore = score;
                        best = h;
                    }
                }
            }
        }
        if (bestScore < 0) return;

        CombatEvent e = MakeEvent(tick, CombatEventType::Teleport, u.id);
        e.from = u.pos;
        e.to = best;
        log_.events.push_back(e);
        grid_.SetBlocked(u.pos, false);
        grid_.SetBlocked(best, true);
        u.pos = best;
        SetTarget(u, retarget ? enemy : -1);   // -1: re-pick a target from the new position
    }

    // A pull or a knock-back: the target slides up to `hexes` hexes straight toward / away from the caster and stops at the first hex that is
    // blocked or off the board. Reported as a Teleport event with subtype 1 (a forced move, not a blink).
    void DisplaceUnit(int casterIndex, int targetIndex, const DisplaceEffect& effect, int tick) {
        FightUnit& u = units_[static_cast<std::size_t>(targetIndex)];
        const FightUnit& caster = units_[static_cast<std::size_t>(casterIndex)];
        if (!u.alive || targetIndex == casterIndex) return;
        if (effect.direction == DisplaceDirection::TowardAreaCenter && u.pos == areaCenter_) return;   // already at the centre
        const int direction = effect.direction == DisplaceDirection::TowardCaster       ? hex::DirectionToward(u.pos, caster.pos)
                              : effect.direction == DisplaceDirection::TowardAreaCenter ? hex::DirectionToward(u.pos, areaCenter_)
                                                                                        : hex::DirectionToward(caster.pos, u.pos);
        HexCoord cursor = u.pos;
        for (int step = 0; step < effect.hexes; ++step) {
            const HexCoord next = hex::Neighbor(cursor, direction);
            if (grid_.IsBlocked(next)) break;   // (also true off the board)
            cursor = next;
        }
        if (cursor == u.pos) return;
        CombatEvent e = MakeEvent(tick, CombatEventType::Teleport, u.id);
        e.from = u.pos;
        e.to = cursor;
        e.subtype = 1;
        log_.events.push_back(e);
        grid_.SetBlocked(u.pos, false);
        grid_.SetBlocked(cursor, true);
        u.pos = cursor;
        u.path.clear();   // whatever route it had started is stale
        u.pathIndex = 0;
    }

    int PathOf(TraitId id) const {
        for (const auto& p : setup_.traitPaths) {
            if (p.first == id) return p.second;
        }
        return 0;
    }

    // Synergies. Per team: count the DIFFERENT champions (two copies of one count once) per trait, activate the
    // highest breakpoint reached, and apply its effects to each qualifying unit as a self-cast on tick 0.
    // Trait system v2 on top: a trait with paths uses the match's path; Phaisa's mutations; scope-Team triggers; Hexagon's Invention.
    void ApplyTraits() {
        if (traits_ == nullptr) return;
        const std::size_t fielded = units_.size();   // (the Invention and souls are appended below; they are no holders)
        for (int team = 0; team < 2; ++team) {
            std::vector<ChampionId> unique;   // different champions on this team (two copies of one count once)
            for (std::size_t i = 0; i < fielded; ++i) {
                const FightUnit& u = units_[i];
                if (u.team != team || u.champion->plant) continue;   // plants take part in no count
                if (std::find(unique.begin(), unique.end(), u.champion->id) == unique.end()) unique.push_back(u.champion->id);
            }
            for (const TraitDefinition& trait : traits_->All()) {  // ascending trait id
                int count = 0;
                int emblemHolders = 0;   // champions that count only through an emblem (the prismatic breakpoint needs one)
                for (ChampionId champion : unique) {
                    // A champion counts if ANY of its copies has the trait -- its own, or granted by an item (an emblem).
                    bool has = false;
                    bool own = false;
                    for (std::size_t i = 0; i < fielded; ++i) {
                        const FightUnit& u = units_[i];
                        if (u.team != team || u.champion->id != champion) continue;
                        has = has || HasTrait(u, trait.name);
                        own = own || std::find(u.champion->traits.begin(), u.champion->traits.end(), trait.name) != u.champion->traits.end();
                    }
                    if (has) ++count;
                    if (has && !own) ++emblemHolders;
                }
                const std::vector<TraitBreakpoint>& breakpoints = trait.Breakpoints(PathOf(trait.id));
                const int tier = ActiveTier(breakpoints, count, emblemHolders);
                if (tier == 0) continue;
                const TraitBreakpoint* active = &breakpoints[static_cast<std::size_t>(tier - 1)];

                CombatEvent e = MakeEvent(0, CombatEventType::TraitActivated, kInvalidUnitId);
                e.team = static_cast<std::uint8_t>(team);
                e.traitId = trait.id;
                e.amount = count;
                e.subtype = static_cast<std::uint8_t>(tier);
                log_.events.push_back(e);

                for (const TraitEffect& te : active->effects) {
                    if (te.scope == TraitScope::Team) {   // once for the whole team, cast by its first (lowest UnitId) holder
                        for (std::size_t i = 0; i < fielded; ++i) {
                            if (units_[i].team != team || !HasTrait(units_[i], trait.name)) continue;
                            CastRecord cast;
                            cast.caster = static_cast<int>(i);
                            cast.target = -1;
                            cast.center = units_[i].pos;
                            RunOrSchedule(cast, te.effect, 0);
                            break;
                        }
                        continue;
                    }
                    for (std::size_t i = 0; i < fielded; ++i) {
                        FightUnit& u = units_[i];
                        if (u.team != team || u.isSummon || u.champion->plant) continue;   // synergies are for the team's real units
                        const bool holds = HasTrait(u, trait.name);
                        if (te.scope == TraitScope::TraitHolders && !holds) continue;
                        CastRecord cast;
                        cast.caster = static_cast<int>(i);
                        cast.target = -1;
                        cast.center = u.pos;
                        cast.ability = nullptr;  // trait effects are Status / Shield / Heal / Mana / Summon only (validated)
                        RunOrSchedule(cast, te.effect, 0);
                    }
                }
                for (const TraitTrigger& tt : active->triggers) {   // hooks the synergy hands to every unit in scope
                    for (std::size_t i = 0; i < fielded; ++i) {
                        FightUnit& u = units_[i];
                        if (u.team != team || u.isSummon || u.champion->plant) continue;
                        if (tt.scope != TraitScope::AllAllies && !HasTrait(u, trait.name)) continue;
                        TriggerSlot slot{&tt.ability, 0, 0, true};
                        slot.teamOnce = tt.scope == TraitScope::Team;
                        u.triggers.push_back(slot);
                    }
                }
                if (active->mutationSlots != 0) ApplyMutations(team, trait, *active, fielded);
                if (trait.invention != 0) {
                    int moduleTier = 0;
                    for (int k = 0; k < tier; ++k) moduleTier = std::max(moduleTier, breakpoints[static_cast<std::size_t>(k)].moduleTier);
                    if (moduleTier > 0) SpawnInvention(team, trait, moduleTier, fielded);
                }
            }
        }
    }

    // Phaisa: the `mutationSlots` strongest holders (cost, then star, then lowest UnitId; -1 = all of them) each get the first mutation whose classes
    // they carry (the class-less one otherwise), supercharged at the top breakpoint.
    void ApplyMutations(int team, const TraitDefinition& trait, const TraitBreakpoint& bp, std::size_t fielded) {
        std::vector<int> holders;
        for (std::size_t i = 0; i < fielded; ++i) {
            const FightUnit& u = units_[i];
            if (u.team == team && !u.isSummon && !u.champion->plant && HasTrait(u, trait.name)) holders.push_back(static_cast<int>(i));
        }
        std::sort(holders.begin(), holders.end(), [&](int a, int b) {
            const FightUnit& ua = units_[static_cast<std::size_t>(a)];
            const FightUnit& ub = units_[static_cast<std::size_t>(b)];
            if (ua.champion->cost != ub.champion->cost) return ua.champion->cost > ub.champion->cost;
            if (ua.star != ub.star) return ua.star > ub.star;
            return a < b;
        });
        const std::size_t slots = bp.mutationSlots < 0 ? holders.size() : std::min(holders.size(), static_cast<std::size_t>(bp.mutationSlots));
        for (std::size_t k = 0; k < slots; ++k) {
            const int index = holders[k];
            FightUnit& u = units_[static_cast<std::size_t>(index)];
            const TraitMutation* chosen = nullptr;
            const TraitMutation* fallback = nullptr;
            for (const TraitMutation& m : trait.mutations) {
                if (m.classes.empty()) {
                    if (fallback == nullptr) fallback = &m;
                    continue;
                }
                for (const std::string& cls : m.classes) {
                    if (HasTrait(u, cls)) { chosen = &m; break; }
                }
                if (chosen != nullptr) break;
            }
            if (chosen == nullptr) chosen = fallback;
            if (chosen == nullptr) continue;
            const bool super = bp.supercharge && (!chosen->superEffects.empty() || !chosen->superTriggers.empty());
            CastRecord cast;
            cast.caster = index;
            cast.target = -1;
            cast.center = u.pos;
            for (const AbilityEffect& effect : super ? chosen->superEffects : chosen->effects) RunOrSchedule(cast, effect, 0);
            for (const AbilityDefinition& hook : super ? chosen->superTriggers : chosen->triggers) u.triggers.push_back(TriggerSlot{&hook, 0, 0, true});
        }
    }

    // Hexagon's Invention: a summon on the team's right-most back hex that fires the chosen modules (those of an unlocked tier) -- each module's
    // effects carry their own delay (8 s). Echo Engine repeats them every `echoEveryTicks`.
    void SpawnInvention(int team, const TraitDefinition& trait, int moduleTier, std::size_t fielded) {
        const ChampionDefinition* def = FindSummon(trait.invention);
        if (def == nullptr || !def->stats.IsCombatCapable()) return;
        int holder = -1;
        for (std::size_t i = 0; i < fielded && holder < 0; ++i) {
            if (units_[i].team == team && HasTrait(units_[i], trait.name)) holder = static_cast<int>(i);
        }
        if (holder < 0) return;
        HexCoord where = BoardToArena(kBoardColumns - 1, 0, team == 0 ? ArenaSide::Home : ArenaSide::Away);
        if (grid_.IsBlocked(where) && !FreeHexNear(where, where)) return;
        const int index = SpawnSummon(def, 1, team, where, holder, 0, 0, 0);
        if (index < 0) return;
        std::vector<const AbilityDefinition*> modules;
        int echo = 0;
        for (std::uint32_t id : setup_.teams[team].modules) {
            const TraitModule* m = trait.FindModule(id);
            if (m == nullptr || m->tier > moduleTier) continue;
            if (m->echo) echo = m->echoEveryTicks;
            if (m->ability.HasAbility()) modules.push_back(&m->ability);
        }
        for (const AbilityDefinition* ability : modules) {
            CastRecord cast;
            cast.caster = index;
            cast.target = -1;
            cast.center = where;
            cast.ability = ability;
            for (const AbilityEffect& effect : ability->effects) {
                RunOrSchedule(cast, effect, 0);
                for (int k = 1; echo > 0 && k <= kMaxEchoes; ++k) RunOrSchedule(cast, effect, 0, k * echo);
            }
        }
    }

    // Hexa: the ally on the hex directly behind it (toward its own back row) climbs in. The pilot leaves the fight (Piloting) and the mech gains a
    // share of its max HP and the bonus of its class.
    void ApplyPilots() {
        const std::size_t fielded = units_.size();
        for (std::size_t i = 0; i < fielded; ++i) {
            if (!units_[i].champion->pilot.enabled || units_[i].isSummon) continue;
            const HexCoord behind{units_[i].pos.x, units_[i].pos.y + (units_[i].team == 0 ? -1 : 1)};
            int pilot = -1;
            for (std::size_t j = 0; j < fielded && pilot < 0; ++j) {
                const FightUnit& p = units_[j];
                if (j != i && p.team == units_[i].team && p.alive && !p.isSummon && !p.champion->plant && !p.champion->pilot.enabled && p.mech < 0 &&
                    p.pos == behind) {
                    pilot = static_cast<int>(j);
                }
            }
            if (pilot < 0) continue;
            FightUnit& mech = units_[i];
            FightUnit& p = units_[static_cast<std::size_t>(pilot)];
            mech.pilot = pilot;
            p.mech = static_cast<int>(i);
            anyPilot_ = true;
            AddStatus(p, true, StatusType::Piloting, 0, std::numeric_limits<int>::max(), false, static_cast<int>(i));
            CombatEvent in = MakeEvent(0, CombatEventType::StatusApplied, p.id);
            in.other = mech.id;
            in.subtype = static_cast<std::uint8_t>(StatusType::Piloting);
            in.hpAfter = p.hp;
            log_.events.push_back(in);

            CastRecord cast;
            cast.caster = static_cast<int>(i);
            cast.target = -1;
            cast.center = mech.pos;
            StatusEffect hp;
            hp.status = StatusType::BonusMaxHp;
            hp.permanent = true;
            hp.value = FlatAmount(Same(Clamp32(static_cast<long long>(p.maxHp) * mech.champion->pilot.hpPercent / 100)));
            AbilityEffect grant;
            grant.target = TargetSpec::Self();
            grant.payload = hp;
            ExecuteEffect(cast, grant, 0);
            for (const PilotBonus& bonus : mech.champion->pilot.bonuses) {
                bool match = false;
                for (const std::string& t : bonus.traits) match = match || HasTrait(p, t);
                if (!match) continue;
                for (const AbilityEffect& effect : bonus.effects) RunOrSchedule(cast, effect, 0);
                break;
            }
        }
    }

    // Hexa died: its pilot is back in the fight, on its own hex, as it went in (and now runs its passive).
    void EjectPilot(int mechIndex, int tick) {
        FightUnit& mech = units_[static_cast<std::size_t>(mechIndex)];
        if (mech.pilot < 0) return;
        const int pilot = mech.pilot;
        mech.pilot = -1;
        FightUnit& p = units_[static_cast<std::size_t>(pilot)];
        p.mech = -1;
        if (!p.alive) return;
        p.statuses.erase(std::remove_if(p.statuses.begin(), p.statuses.end(), [](const StatusInst& st) { return st.type == StatusType::Piloting; }),
                         p.statuses.end());
        CombatEvent out = MakeEvent(tick, CombatEventType::StatusEnded, p.id);
        out.subtype = static_cast<std::uint8_t>(StatusType::Piloting);
        out.hpAfter = p.hp;
        log_.events.push_back(out);
        SetTarget(p, -1);
        p.nextAttackTick = std::max(p.nextAttackTick, tick + 1);
        p.nextMoveTick = std::max(p.nextMoveTick, tick + 1);
        RunPassives(pilot);
    }

    void ApplyManaFromDamage(int tick) {
        for (FightUnit& u : units_) {
            if (!u.alive || u.manaFromDamage == 0) continue;
            const int gain = std::min(u.manaFromDamage, config_.manaFromDamagePerTickCapMilli);
            u.manaFromDamage = 0;
            if (u.maxMana > 0 && tick >= u.lockedUntil) {
                const int before = u.mana;
                u.mana = std::min(u.maxMana, u.mana + gain);
                u.manaDirty = u.manaDirty || u.mana != before;
            }
        }
    }

    // Units at 0 HP die. A dying unit whose ability has castOnDeath casts once; that cast may
    // kill more units (which may cast in turn), so repeat until the tick is stable.
    void ReapDeathsAndDeathCasts(int tick) {
        for (int round = 0; round < kMaxDeathCastRounds; ++round) {
            std::vector<int> dead;
            for (std::size_t i = 0; i < units_.size(); ++i) {
                FightUnit& u = units_[i];
                if (!u.alive || u.hp > 0) continue;
                u.alive = false;
                if (u.isSummon) --summonAlive_[u.team];
                else --alive_[u.team];
                grid_.SetBlocked(u.pos, false);
                u.activeChannelSerial = 0;   // a dead caster's channel is over (its remaining pulses are dropped)
                u.shields.clear();
                u.statuses.clear();
                u.dots.clear();
                CombatEvent death = MakeEvent(tick, CombatEventType::Death, u.id);
                if (u.isSummon) death.flags = kFlagSummon;
                log_.events.push_back(death);
                dead.push_back(static_cast<int>(i));
            }
            if (dead.empty()) return;

            for (int i : dead) {
                FightUnit& victim = units_[static_cast<std::size_t>(i)];
                if (!victim.isSummon) {   // takedowns: every enemy that hurt it in the last 3 s (the killing blow included)
                    for (FightUnit& other : units_) {
                        if (other.team == victim.team || other.isSummon) continue;
                        for (const DealtEntry& d : other.dealt) {
                            if (d.target == i && d.tick > tick - kTakedownWindowTicks) {
                                ++other.takedowns;
                                break;
                            }
                        }
                    }
                }
                for (TriggerSlot& slot : victim.triggers) {   // its own last act (the Stonebark Tree's stun)
                    if (slot.ability->trigger == EventTrigger::OnDeath) FireHook(i, slot, -1, i, 0, true, tick);
                }
                EjectPilot(i, tick);
            }

            // On-kill statuses: the killer gains them (e.g. Lunis drops aggro), evaluated with the victim as the cast target.
            for (int i : dead) {
                FightUnit& victim = units_[static_cast<std::size_t>(i)];
                const DamageEffect* killEffect = victim.killEffect;
                victim.killEffect = nullptr;
                if (killEffect == nullptr || victim.killer < 0 || !units_[static_cast<std::size_t>(victim.killer)].alive) continue;
                for (const StatusEffect& reward : killEffect->onKill) {
                    AbilityEffect wrapper;
                    wrapper.target = TargetSpec::Self();
                    wrapper.payload = reward;
                    CastRecord kill;
                    kill.caster = victim.killer;
                    kill.target = i;
                    kill.center = victim.pos;
                    kill.ability = units_[static_cast<std::size_t>(victim.killer)].ability;
                    ExecuteEffect(kill, wrapper, tick);
                }
            }

            bool cast = false;
            for (int i : dead) {
                FightUnit& u = units_[static_cast<std::size_t>(i)];
                if (u.ability == nullptr || !u.ability->castOnDeath) continue;
                const bool haveTarget = u.target >= 0 && units_[static_cast<std::size_t>(u.target)].alive;
                if (!haveTarget) continue;
                DeclareCast(i, u.target, tick, true);
                cast = true;
            }
            for (int i : dead) {   // "whenever any unit dies": one firing per death for every unit still standing that listens
                for (std::size_t j = 0; j < units_.size(); ++j) {
                    FightUnit& holder = units_[j];
                    if (!holder.alive) continue;
                    const FightUnit& fallen = units_[static_cast<std::size_t>(i)];
                    for (TriggerSlot& slot : holder.triggers) {
                        const EventTrigger t = slot.ability->trigger;
                        const bool ally = fallen.team == holder.team && static_cast<int>(j) != i &&
                                          (slot.ability->triggerTrait.empty() || HasTrait(fallen, slot.ability->triggerTrait));
                        if (t == EventTrigger::OnAnyUnitDeath || (t == EventTrigger::OnEnemyDeath && fallen.team != holder.team) ||
                            (t == EventTrigger::OnAllyDeath && ally)) {
                            FireHook(static_cast<int>(j), slot, -1, i, 0, true, tick);
                        }
                    }
                }
            }
            if (!cast && casts_.empty()) return;
            ExecuteCasts(tick);
            ApplyHits(tick);
            ProcessHooks(tick);
        }
    }

    CombatConfig config_;
    const TraitDatabase* traits_;
    FightSetup setup_;
    const ChampionDatabase* summons_;
    const ChampionDatabase* summons2_;
    int maxTicks_;
    int overtimeFactor_ = 1;   // 1 in regulation, CombatConfig::overtimeSpeed in overtime
    Rng rng_;
    HexGrid grid_;
    HexPathfinder pathfinder_;
    std::vector<FightUnit> units_;
    std::vector<Hit> hits_;
    std::vector<CastRecord> casts_;
    std::vector<Scheduled> scheduled_;
    std::vector<CastRecord> pendingHooks_;   // hooks fired by this tick's damage, waiting for it to land
    int summonsSpawned_ = 0;
    int summonAlive_[2] = {0, 0};
    std::vector<int> targets_;  // scratch
    HexCoord areaCenter_{};     // the centre the last area target resolved to (Displace TowardAreaCenter follows it)
    int alive_[2] = {0, 0};
    int busiestRow_[2] = {-1, -1};
    int pathSearches_ = 0;
    int castSerial_ = 0;
    long long teamMaxHp_[2] = {0, 0};   // OnTeamHpLoss: the teams' total max HP at the start
    std::vector<std::pair<const AbilityDefinition*, int>> teamOnce_[2];   // scope-Team OnTeamHpLoss hooks: steps already fired, per team
    bool anyPilot_ = false;
    CombatLog log_;
};

}  // namespace

FightResult CombatSimulator::RunFight(const std::vector<FightUnitSpec>& units, int maxTicks, std::uint64_t seed, const FightSetup& setup) const {
    return Fight(config_, traits_, units, maxTicks, seed, summons_, nullptr, setup).Run();
}

std::vector<CombatOutcome> CombatSimulator::Simulate(const CombatContext& context) {
    std::vector<CombatOutcome> outcomes;
    outcomes.reserve(context.matchups.size());

    std::uint64_t matchupIndex = 0;
    for (const Matchup& matchup : context.matchups) {
        std::vector<FightUnitSpec> specs;
        const ArenaSide sides[2] = {ArenaSide::Home, ArenaSide::Away};
        const PlayerId owners[2] = {matchup.home, matchup.away};
        const EncounterDefinition* encounter = nullptr;
        if (matchup.awayIsMonsters) {
            encounter = context.encounters != nullptr ? context.encounters->Find(matchup.encounter) : nullptr;
            if (encounter == nullptr) {   // a PvE round with no monsters to fight: nothing happens
                CombatOutcome none;
                none.matchup = matchup;
                outcomes.push_back(std::move(none));
                continue;
            }
            UnitId nextMonster = kMonsterUnitBase;
            for (const MonsterPlacement& placed : encounter->units) {
                const ChampionDefinition* monster = context.encounters->Monsters().Find(placed.monster);
                if (monster == nullptr) continue;   // validated at load time; defensive
                specs.push_back(FightUnitSpec{++nextMonster, monster, placed.starLevel, 1, BoardToArena(placed.x, placed.y, ArenaSide::Away), {}});
            }
        }
        for (int team = 0; team < 2; ++team) {
            if (team == 1 && matchup.awayIsMonsters) continue;   // the monsters are already in
            const PlayerState* player = context.players.Get(owners[team]);
            if (player == nullptr) continue;
            for (const UnitInstance& unit : player->Roster().Units()) {
                if (unit.location != LocationType::Board) continue;  // bench units don't fight
                FightUnitSpec spec{unit.id, unit.champion, unit.starLevel, team, BoardToArena(unit.x, unit.y, sides[team]), {}};
                if (items_ != nullptr) {
                    for (ItemId item : unit.items) {
                        if (const ItemDefinition* def = item != 0 ? items_->Find(item) : nullptr) spec.items.push_back(def);
                    }
                }
                specs.push_back(std::move(spec));
            }
        }

        // What the fight needs to know about the players (trait system v2).
        FightSetup setup;
        setup.traitPaths = context.traitPaths;
        for (int team = 0; team < 2; ++team) {
            if (team == 1 && matchup.awayIsMonsters) continue;
            const PlayerState* player = context.players.Get(owners[team]);
            if (player == nullptr) continue;
            setup.teams[team].playerLevel = player->Level();
            setup.teams[team].traitGold = player->Traits().traitGold;
            setup.teams[team].modules = player->Traits().modules;
        }

        // Each fight gets its own crit stream, derived from the round's seed.
        const std::uint64_t fightSeed = context.seed + 0x9E3779B97F4A7C15ull * (++matchupIndex);
        FightResult fight = Fight(config_, traits_, specs, context.maxTicks, fightSeed, context.champions != nullptr ? context.champions : summons_,
                                  context.encounters != nullptr ? &context.encounters->Monsters() : nullptr, setup).Run();
        CombatOutcome outcome;
        outcome.matchup = matchup;
        outcome.winner = fight.winner;
        if (fight.winner == CombatWinner::Home) outcome.winnerSurvivors = fight.log.survivors[0];
        if (fight.winner == CombatWinner::Away) outcome.winnerSurvivors = fight.log.survivors[1];
        outcome.log = std::move(fight.log);
        outcomes.push_back(std::move(outcome));
    }
    return outcomes;
}

}  // namespace w2f
