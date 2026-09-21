#pragma once

// Typed view of a combat log, for the "dumb viewer" (UE5). The server produces one finished
// CombatLog per fight; ReplayCombatLog() walks it in order and calls the matching method on a
// sink, with the event's combat-local tick. A UE5 wrapper implements this interface and turns
// each call into a queued animation / particle effect / UI update scheduled for that tick.
// Nothing here simulates anything: the sink only ever hears about things that already happened.

#include "w2f/Combat.h"

namespace w2f {

class ICombatEventSink {
public:
    virtual ~ICombatEventSink() = default;

    virtual void OnSpawn(const CombatEvent& /*e*/) {}   // unit, team, champion, star, to, amount = max HP
    virtual void OnMove(const CombatEvent& /*e*/) {}    // unit, from, to, amount = step duration
    virtual void OnAttack(int /*tick*/, UnitId /*attacker*/, UnitId /*target*/) {}
    virtual void OnDamage(const CombatEvent& /*e*/) {}  // amount, absorbed, hpAfter, subtype, flags (crit / dot / ...)
    virtual void OnDeath(int /*tick*/, UnitId /*unit*/) {}

    // A spell was cast. `lockTicks` = how long the cast animation locks the caster.
    virtual void OnSpellCast(int /*tick*/, UnitId /*caster*/, AbilityId /*spell*/, UnitId /*mainTarget*/,
                             int /*lockTicks*/, bool /*castOnDeath*/) {}
    // A shield appeared on `unit` (absorb capacity `amount`, lasting `durationTicks`).
    virtual void OnShieldApplied(int /*tick*/, UnitId /*unit*/, int /*amount*/, int /*durationTicks*/) {}
    // The shield is gone: expired or broken. `unusedAmount` is what it had left (0 = broken).
    virtual void OnShieldEnded(int /*tick*/, UnitId /*unit*/, int /*unusedAmount*/) {}
    // A status (stun, burn, buff/debuff) landed on `unit`. `magnitudePercent` is 0 for Stun / Burn.
    virtual void OnStatusApplied(int /*tick*/, UnitId /*unit*/, StatusType /*status*/, int /*durationTicks*/,
                                 int /*magnitudePercent*/, UnitId /*source*/) {}
    virtual void OnStatusEnded(int /*tick*/, UnitId /*unit*/, StatusType /*status*/) {}
    // `unit` regained `amount` HP (after Wound; `reducedByWound` is what Wound took away). `source` is the healer
    // (the unit itself for self-regeneration). Only sent when HP was actually restored.
    virtual void OnHeal(int /*tick*/, UnitId /*unit*/, UnitId /*source*/, int /*amount*/, int /*reducedByWound*/, int /*hpAfter*/) {}
    // A synergy is active for `team` at the start of the fight: `count` different champions have the trait,
    // and `breakpoint` (1 = lowest) is the tier reached. Its buffs follow as OnStatusApplied events.
    virtual void OnTraitActivated(int /*tick*/, int /*team*/, std::uint32_t /*traitId*/, int /*count*/, int /*breakpoint*/) {}
    // An assassin jump: `unit` is at `to` from this tick on (animate it however you like between the two).
    virtual void OnTeleport(int /*tick*/, UnitId /*unit*/, HexCoord /*from*/, HexCoord /*to*/) {}
    // A channelled spell was broken: stop its effect. Nothing more from that cast will happen.
    virtual void OnSpellInterrupted(int /*tick*/, UnitId /*unit*/, AbilityId /*spell*/) {}
    // Mana bar sync. `manaMilli` is thousandths of a mana; max and regen rate came with OnSpawn.
    // Not sent for passive regen: interpolate between events (see CombatEventType::ManaChanged).
    virtual void OnManaChanged(int /*tick*/, UnitId /*unit*/, int /*manaMilli*/) {}
    // The fight went into overtime: units act `speed` times faster from `tick`, until one team is wiped out (`durationTicks` is 0).
    virtual void OnOvertime(int /*tick*/, int /*speed*/, int /*durationTicks*/) {}
};

// Dispatches every event of `log`, in stream order, to `sink`.
void ReplayCombatLog(const CombatLog& log, ICombatEventSink& sink);

}  // namespace w2f
