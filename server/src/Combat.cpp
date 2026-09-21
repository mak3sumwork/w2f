#include "w2f/Combat.h"

#include "w2f/Hash.h"

namespace w2f {

std::uint64_t CombatLog::ComputeChecksum() const {
    Fnv1a h;
    h.AddInt(static_cast<std::int64_t>(events.size()));
    for (const CombatEvent& e : events) {
        h.AddInt(e.tick);
        h.Add(static_cast<std::uint64_t>(e.type));
        h.Add(e.team);
        h.Add(e.unit);
        h.Add(e.other);
        h.AddInt(e.from.x);
        h.AddInt(e.from.y);
        h.AddInt(e.to.x);
        h.AddInt(e.to.y);
        h.AddInt(e.amount);
        h.AddInt(e.hpAfter);
        h.Add(e.champion);
        h.Add(e.star);
        h.AddInt(e.absorbed);
        h.Add(e.subtype);
        h.Add(e.flags);
        h.Add(e.ability);
        h.AddInt(e.duration);
        h.AddInt(e.manaMax);
        h.AddInt(e.manaRegen);
        h.AddInt(e.reduced);
        h.Add(e.traitId);
        h.AddInt(e.windup);
        h.AddInt(e.flight);
        h.Add(e.kind);
        h.Add(e.shape);
        h.Add(e.size);
    }
    h.AddInt(endTick);
    h.AddInt(survivors[0]);
    h.AddInt(survivors[1]);
    return h.value;
}

}  // namespace w2f
