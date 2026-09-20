#include "w2f/CombatEventSink.h"

namespace w2f {

void ReplayCombatLog(const CombatLog& log, ICombatEventSink& sink) {
    for (const CombatEvent& e : log.events) {
        switch (e.type) {
            case CombatEventType::Spawn: sink.OnSpawn(e); break;
            case CombatEventType::Move: sink.OnMove(e); break;
            case CombatEventType::Attack: sink.OnAttack(e.tick, e.unit, e.other); break;
            case CombatEventType::Damage: sink.OnDamage(e); break;
            case CombatEventType::Death: sink.OnDeath(e.tick, e.unit); break;
            case CombatEventType::SpellCast:
                sink.OnSpellCast(e.tick, e.unit, e.ability, e.other, e.duration, (e.flags & kFlagOnDeath) != 0);
                break;
            case CombatEventType::ShieldApplied: sink.OnShieldApplied(e.tick, e.unit, e.amount, e.duration); break;
            case CombatEventType::ShieldEnded: sink.OnShieldEnded(e.tick, e.unit, e.amount); break;
            case CombatEventType::StatusApplied:
                sink.OnStatusApplied(e.tick, e.unit, static_cast<StatusType>(e.subtype), e.duration, e.amount, e.other);
                break;
            case CombatEventType::StatusEnded: sink.OnStatusEnded(e.tick, e.unit, static_cast<StatusType>(e.subtype)); break;
            case CombatEventType::Heal: sink.OnHeal(e.tick, e.unit, e.other, e.amount, e.reduced, e.hpAfter); break;
            case CombatEventType::TraitActivated: sink.OnTraitActivated(e.tick, e.team, e.traitId, e.amount, e.subtype); break;
            case CombatEventType::Teleport: sink.OnTeleport(e.tick, e.unit, e.from, e.to); break;
            case CombatEventType::SpellInterrupted: sink.OnSpellInterrupted(e.tick, e.unit, e.ability); break;
            case CombatEventType::ManaChanged: sink.OnManaChanged(e.tick, e.unit, e.amount); break;
        }
    }
}

}  // namespace w2f
