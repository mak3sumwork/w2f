#include "w2f/Ability.h"

#include <type_traits>
#include <variant>

#include "w2f/Hash.h"
#include "w2f/Hex.h"

namespace w2f {

namespace {

bool Fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}

bool ValidateStatus(const StatusEffect& status, const std::string& name, std::string* error);
bool ValidateAmount(const Amount& amount, const std::string& what, std::string* error) {
    for (const ScalingTerm& term : amount.terms) {
        if (term.source == StatSource::None) return Fail(error, what + ": scaling term has no source");
        const bool windowed = term.source == StatSource::DamageDealtInWindow || term.source == StatSource::DamageDealtToTargetInWindow;
        if (windowed && (term.windowTicks < 1 || term.windowTicks > 600)) {
            return Fail(error, what + ": a windowed source needs a window of 1..600 ticks");
        }
    }
    return true;
}

bool ValidateStatus(const StatusEffect& status, const std::string& name, std::string* error) {
    if (status.status == StatusType::Burn) return Fail(error, name + ": Burn can only come from a DotEffect");
    if (status.multiplierPercent < 0) return Fail(error, name + ": negative status multiplier");
    if (!ValidateAmount(status.duration, name, error)) return false;
    if (!ValidateAmount(status.value, name, error)) return false;
    const bool ccStatus = status.status == StatusType::Stun || status.status == StatusType::Knockup;
    if (status.permanent && ccStatus) return Fail(error, name + ": a permanent Stun / Knockup would freeze the unit for the whole fight");
    if (IsFlatBonusStatus(status.status)) {
        bool hasValue = false;
        for (int f : status.value.flat) hasValue = hasValue || f != 0;
        if (!hasValue && status.value.terms.empty()) return Fail(error, name + ": a flat bonus status needs a \"value\"");
    } else if (!status.value.terms.empty()) {
        return Fail(error, name + ": \"value\" only applies to the flat bonus statuses (BonusAttackDamage, BonusArmor, ...)");
    }
    if (status.status == StatusType::Tether) {
        for (int pct : status.percent) {
            if (pct < 1 || pct > 100) return Fail(error, name + ": a Tether redirects 1..100% of the damage");
        }
    }
    return true;
}

}  // namespace

bool ValidateAbility(const AbilityDefinition& ability, int maxMana, std::string* error) {
    if (!ability.HasAbility()) return true;  // "no ability" is valid as long as it stays empty
    const std::string name = "ability '" + ability.name + "'";

    switch (ability.trigger) {
        case CastTrigger::None: return Fail(error, name + " has an id but no trigger");
        case CastTrigger::Mana:
            if (maxMana <= 0) return Fail(error, name + " is mana-triggered but the champion has no max mana");
            break;
        case CastTrigger::EveryNthAttack:
            if (ability.attackCount < 1) return Fail(error, name + " needs attackCount >= 1");
            break;
        case CastTrigger::StartOfCombat:
            if (ability.castLockTicks != 0 || ability.channelTicks != 0 || ability.castOnDeath || ability.attackCount != 0 ||
                ability.resetCountOnTargetChange) {
                return Fail(error, name + " is a passive: it cannot have a cast lock, a channel, cast on death, or an attack count");
            }
            break;
        case CastTrigger::OnBasicAttack:
            if (ability.castLockTicks != 0 || ability.channelTicks != 0 || ability.castOnDeath || ability.attackCount != 0 ||
                ability.resetCountOnTargetChange) {
                return Fail(error, name + " is an attack rider: it cannot have a cast lock, a channel, cast on death, or an attack count");
            }
            break;
        case CastTrigger::OnCast:
        case CastTrigger::OnTakeBasicAttackDamage:
        case CastTrigger::OnTakeAbilityDamage:
        case CastTrigger::OnDealDamage:
        case CastTrigger::OnCritTaken:
        case CastTrigger::OnHpDropBelowPercent:
        case CastTrigger::OnAllyDealDamage:
            if (ability.castLockTicks != 0 || ability.channelTicks != 0 || ability.castOnDeath || ability.resetCountOnTargetChange) {
                return Fail(error, name + " is a hook: it cannot have a cast lock, a channel, cast on death, or reset-on-target-change");
            }
            if (ability.attackCount < 0 || ability.attackCount > 1000) return Fail(error, name + ": the count (attackCount) must be 0..1000");
            break;
    }
    if (ability.maxTriggers < 0 || ability.maxTriggers > 1000) return Fail(error, name + ": maxTriggers must be 0..1000");
    if (ability.trigger == CastTrigger::OnHpDropBelowPercent) {
        if (ability.thresholdPercent < 1 || ability.thresholdPercent > 99) return Fail(error, name + ": thresholdPercent must be 1..99");
    } else if (ability.thresholdPercent != 0) {
        return Fail(error, name + ": thresholdPercent only applies to OnHpDropBelowPercent");
    }
    if (ability.damageFilter != DamageFilter::Any && ability.trigger != CastTrigger::OnDealDamage && ability.trigger != CastTrigger::OnAllyDealDamage) {
        return Fail(error, name + ": damageFilter only applies to OnDealDamage and OnAllyDealDamage");
    }
    if (ability.maxTriggers != 0 && !IsHook(ability.trigger)) return Fail(error, name + ": maxTriggers only applies to hook triggers");
    if (ability.channelTicks < 0) return Fail(error, name + " has a negative channel time");
    if (ability.channelTicks > 0 && ability.castLockTicks != 0) return Fail(error, name + ": a channel is its own lock; leave castLock at 0");
    if (ability.resetCountOnTargetChange && ability.trigger != CastTrigger::EveryNthAttack) {
        return Fail(error, name + ": resetOnTargetChange only makes sense for an every-Nth-attack trigger");
    }
    if (ability.castLockTicks < 0) return Fail(error, name + " has a negative cast lock");
    if (ability.effects.empty()) return Fail(error, name + " has no effects");

    for (const AbilityEffect& effect : ability.effects) {
        if (effect.delayTicks < 0) return Fail(error, name + " has a negative effect delay");
        if (effect.repeatCount < 1 || effect.repeatCount > 64) return Fail(error, name + ": repeat count must be 1..64");
        if (effect.repeatCount > 1 && effect.repeatIntervalTicks < 1) return Fail(error, name + ": a repeating effect needs an interval of at least 1 tick");
        if (std::holds_alternative<TeleportEffect>(effect.payload) && effect.target.mode != TargetMode::Self) {
            return Fail(error, name + ": a Teleport effect must target Self");
        }
        if (effect.target.radius < 0 || effect.target.radius > kArenaColumns + kArenaRows) {
            return Fail(error, name + " has an invalid target radius");
        }
        const bool needsCastTarget = effect.target.mode == TargetMode::CurrentTarget || effect.target.mode == TargetMode::AreaAroundTarget ||
                                     effect.target.mode == TargetMode::LineBehindTarget || effect.target.mode == TargetMode::ConeTowardTarget ||
                                     effect.target.mode == TargetMode::HighestHpEnemyNearTarget;
        if (ability.trigger == CastTrigger::StartOfCombat && needsCastTarget) {
            return Fail(error, name + " is a passive: it has no target to aim at (use Self, AreaAroundSelf, ClosestEnemies or AlliesInStartLine)");
        }
        if (effect.target.mode == TargetMode::ClosestEnemies && (effect.target.count < 1 || effect.target.count > 16)) {
            return Fail(error, name + ": ClosestEnemies needs a count of 1..16");
        }
        if ((effect.target.mode == TargetMode::LineBehindTarget || effect.target.mode == TargetMode::ConeTowardTarget) && effect.target.radius < 1) {
            return Fail(error, name + ": a line / cone target needs a length of at least 1");
        }
        if (effect.target.mode == TargetMode::HighestDamageAlly && (effect.target.windowTicks < 0 || effect.target.windowTicks > 600)) {
            return Fail(error, name + ": HighestDamageAlly window must be 0..600 ticks");
        }
        if ((effect.target.mode == TargetMode::TriggerAttacker || effect.target.mode == TargetMode::TriggerVictim) &&
            !IsDamageHook(ability.trigger) && ability.trigger != CastTrigger::OnBasicAttack) {
            return Fail(error, name + ": TriggerAttacker / TriggerVictim only exist for damage hooks (OnTake*, OnDealDamage, OnCritTaken, OnHpDropBelowPercent, OnAllyDealDamage) and OnBasicAttack");
        }
        {
            bool badSource = false;
            const auto check = [&](const Amount& a) {
                for (const ScalingTerm& t : a.terms) badSource = badSource || (t.source == StatSource::TriggerDamage && !IsDamageHook(ability.trigger));
            };
            std::visit([&](const auto& p) {
                using T = std::decay_t<decltype(p)>;
                if constexpr (std::is_same_v<T, DamageEffect> || std::is_same_v<T, HealEffect> || std::is_same_v<T, ManaEffect>) check(p.amount);
                else if constexpr (std::is_same_v<T, ShieldEffect>) { check(p.amount); check(p.duration); check(p.cap); }
                else if constexpr (std::is_same_v<T, StatusEffect>) { check(p.duration); check(p.value); }
                else if constexpr (std::is_same_v<T, DotEffect>) { check(p.amount); check(p.duration); }
                else if constexpr (std::is_same_v<T, SummonEffect>) { check(p.count); check(p.maxHp); check(p.attackDamage); }
            }, effect.payload);
            if (badSource) return Fail(error, name + ": the TriggerDamage source only exists for damage hooks");
        }
        if (const auto* summon = std::get_if<SummonEffect>(&effect.payload)) {
            if (effect.target.mode != TargetMode::Self) return Fail(error, name + ": a Summon effect must target Self (the summons appear next to the caster)");
            if (summon->champion == kInvalidChampionId) return Fail(error, name + ": a Summon effect needs a champion");
            if (summon->starLevel < 0 || summon->starLevel > kMaxStarLevel) return Fail(error, name + ": summon star level must be 0 (the summoner's) or 1..3");
            if (!ValidateAmount(summon->count, name, error) || !ValidateAmount(summon->maxHp, name, error) || !ValidateAmount(summon->attackDamage, name, error)) return false;
            for (int f : summon->count.flat) {
                if (f < 0 || f > 8) return Fail(error, name + ": a summon count must be 0..8 per star");
            }
            bool any = !summon->count.terms.empty();
            for (int f : summon->count.flat) any = any || f > 0;
            if (!any) return Fail(error, name + ": a summon count of 0 summons nothing");
        } else if (const auto* mana = std::get_if<ManaEffect>(&effect.payload)) {
            if (!ValidateAmount(mana->amount, name, error)) return false;
        } else if (const auto* damage = std::get_if<DamageEffect>(&effect.payload)) {
            if (damage->multiplierPercent < 0) return Fail(error, name + ": negative damage multiplier");
            if (!ValidateAmount(damage->amount, name, error)) return false;
            for (const StatusEffect& k : damage->onKill) {
                if (!ValidateStatus(k, name + " (on kill)", error)) return false;
            }
        } else if (const auto* shield = std::get_if<ShieldEffect>(&effect.payload)) {
            if (!ValidateAmount(shield->amount, name, error) || !ValidateAmount(shield->duration, name, error)) return false;
            if (!ValidateAmount(shield->cap, name, error)) return false;
            if (shield->capped && shield->cap.terms.empty()) {
                bool any = false;
                for (int f : shield->cap.flat) any = any || f > 0;
                if (!any) return Fail(error, name + ": a shield cap must be positive");
            }
            for (int pct : shield->damageReductionPercent) {
                if (pct < 0 || pct > 100) return Fail(error, name + ": shield damage reduction must be 0..100%");
            }
        } else if (const auto* status = std::get_if<StatusEffect>(&effect.payload)) {
            if (!ValidateStatus(*status, name, error)) return false;
        } else if (const auto* heal = std::get_if<HealEffect>(&effect.payload)) {
            if (!ValidateAmount(heal->amount, name, error)) return false;
        } else if (const auto* dot = std::get_if<DotEffect>(&effect.payload)) {
            if (dot->intervalTicks < 1) return Fail(error, name + ": DoT interval must be >= 1 tick");
            if (!ValidateAmount(dot->amount, name, error) || !ValidateAmount(dot->duration, name, error)) return false;
        }
    }
    return true;
}

void HashAbility(Fnv1a& h, const AbilityDefinition& a) {
    h.Add(a.id);
    h.AddString(a.name);
    h.Add(static_cast<std::uint64_t>(a.trigger));
    h.AddInt(a.attackCount);
    h.AddInt(a.thresholdPercent);
    h.Add(static_cast<std::uint64_t>(a.damageFilter));
    h.AddInt(a.maxTriggers);
    h.Add(a.resetCountOnTargetChange ? 1 : 0);
    h.AddInt(a.castLockTicks);
    h.AddInt(a.channelTicks);
    h.Add(a.castOnDeath ? 1 : 0);
    h.AddInt(static_cast<std::int64_t>(a.effects.size()));
    for (const AbilityEffect& e : a.effects) {
        h.Add(static_cast<std::uint64_t>(e.payload.index()));
        h.AddInt(e.delayTicks);
        h.AddInt(e.repeatCount);
        h.AddInt(e.repeatIntervalTicks);
        h.Add(static_cast<std::uint64_t>(e.target.mode));
        h.AddInt(e.target.radius);
        h.Add(static_cast<std::uint64_t>(e.target.side));
        h.AddInt(e.target.count);
        if (const auto* summon = std::get_if<SummonEffect>(&e.payload)) {
            h.Add(summon->champion);
            h.AddInt(summon->starLevel);
        }
        if (const auto* status = std::get_if<StatusEffect>(&e.payload)) h.Add(static_cast<std::uint64_t>(status->status));
    }
}

}  // namespace w2f
