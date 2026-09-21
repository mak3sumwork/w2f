#include "w2f/Snapshot.h"

#include <algorithm>

#include "w2f/Hash.h"
#include "w2f/MatchManager.h"

namespace w2f {

namespace {

// ---- Byte plumbing ------------------------------------------------------------------------
// Fixed-width little-endian, written byte by byte: no dependence on host endianness, struct padding or the size of int.

class ByteWriter {
public:
    void U8(std::uint32_t v) { bytes_.push_back(static_cast<std::uint8_t>(v & 0xFFu)); }
    void U32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) U8(v >> (8 * i));
    }
    void U64(std::uint64_t v) {
        for (int i = 0; i < 8; ++i) U8(static_cast<std::uint32_t>(v >> (8 * i)));
    }
    void I32(int v) { U32(static_cast<std::uint32_t>(v)); }
    void Bool(bool v) { U8(v ? 1u : 0u); }
    void Rng(const RngState& s) {
        for (std::uint64_t word : s.words) U64(word);
    }
    std::vector<std::uint8_t>& Bytes() { return bytes_; }

private:
    std::vector<std::uint8_t> bytes_;
};

// Bounds-checked: after the first violation every read returns 0 and ok() stays false, so a decoder can read a whole
// structure and check once.
class ByteReader {
public:
    ByteReader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    bool ok() const { return ok_; }
    std::size_t remaining() const { return size_ - pos_; }
    bool atEnd() const { return pos_ == size_; }

    std::uint32_t U8() {
        if (!Need(1)) return 0;
        return data_[pos_++];
    }
    std::uint32_t U32() {
        if (!Need(4)) return 0;
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(data_[pos_++]) << (8 * i);
        return v;
    }
    std::uint64_t U64() {
        if (!Need(8)) return 0;
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(data_[pos_++]) << (8 * i);
        return v;
    }
    int I32() { return static_cast<int>(U32()); }
    bool Bool() {
        const std::uint32_t v = U8();
        if (v > 1) ok_ = false;
        return v == 1;
    }
    RngState Rng() {
        RngState s;
        for (std::uint64_t& word : s.words) word = U64();
        return s;
    }
    // A collection size. Refused when it exceeds `limit`, or when the bytes left could not possibly hold that many
    // elements of at least `minElementBytes` each (so a corrupt count can never trigger a huge allocation).
    std::size_t Count(std::size_t limit, std::size_t minElementBytes) {
        const std::size_t n = U32();
        if (!ok_) return 0;
        if (n > limit || (minElementBytes > 0 && n > remaining() / minElementBytes)) {
            ok_ = false;
            return 0;
        }
        return n;
    }

private:
    bool Need(std::size_t n) {
        if (!ok_ || size_ - pos_ < n) {
            ok_ = false;
            return false;
        }
        return true;
    }

    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

constexpr std::size_t kHeaderBytes = 4 + 4 + 8 * 7 + 1 + 4 + 4 + 1;  // magic, version, seed + 5 hashes + state hash, phase, round, tick, player count
constexpr std::size_t kGiftBytes = 4 + 1 + 4 + 4 + 4;   // gift id, type, amount, item, champion
constexpr std::size_t kTrailerBytes = 8;

// Serialized size of one CombatEvent (kept next to the writer so the two cannot drift apart unnoticed: the round-trip
// test would fail).
constexpr std::size_t kEventBytes = 4 + 1 + 1 + 4 + 4 + 16 + 4 + 4 + 4 + 1 + 4 + 1 + 1 + 4 + 4 + 4 + 4 + 4 + 4 + 4 + 4 + 1 + 1 + 1;
constexpr std::size_t kUnitBytes = 4 + 4 + 1 + 1 + 4 + 4 + 4 * kMaxItemsPerUnit;
constexpr std::size_t kMaxUnitsPerPlayer = static_cast<std::size_t>(kBenchSlots + kBoardRows * kBoardColumns);
constexpr std::size_t kMaxItemBag = 65535;

std::uint64_t Checksum(const std::uint8_t* data, std::size_t size) {
    Fnv1a h;
    for (std::size_t i = 0; i < size; ++i) h.AddByte(data[i]);
    return h.value;
}

void WriteEvent(ByteWriter& w, const CombatEvent& e) {
    w.I32(e.tick);
    w.U8(static_cast<std::uint32_t>(e.type));
    w.U8(e.team);
    w.U32(e.unit);
    w.U32(e.other);
    w.I32(e.from.x);
    w.I32(e.from.y);
    w.I32(e.to.x);
    w.I32(e.to.y);
    w.I32(e.amount);
    w.I32(e.hpAfter);
    w.U32(e.champion);
    w.U8(e.star);
    w.I32(e.absorbed);
    w.U8(e.subtype);
    w.U8(e.flags);
    w.U32(e.ability);
    w.I32(e.duration);
    w.I32(e.manaMax);
    w.I32(e.manaRegen);
    w.I32(e.reduced);
    w.U32(e.traitId);
    w.I32(e.windup);
    w.I32(e.flight);
    w.U8(e.kind);
    w.U8(e.shape);
    w.U8(e.size);
}

bool ReadEvent(ByteReader& r, CombatEvent& e) {
    e.tick = r.I32();
    const std::uint32_t type = r.U8();
    e.team = static_cast<std::uint8_t>(r.U8());
    e.unit = r.U32();
    e.other = r.U32();
    e.from.x = r.I32();
    e.from.y = r.I32();
    e.to.x = r.I32();
    e.to.y = r.I32();
    e.amount = r.I32();
    e.hpAfter = r.I32();
    e.champion = r.U32();
    e.star = static_cast<std::uint8_t>(r.U8());
    e.absorbed = r.I32();
    e.subtype = static_cast<std::uint8_t>(r.U8());
    e.flags = static_cast<std::uint8_t>(r.U8());
    e.ability = r.U32();
    e.duration = r.I32();
    e.manaMax = r.I32();
    e.manaRegen = r.I32();
    e.reduced = r.I32();
    e.traitId = r.U32();
    e.windup = r.I32();
    e.flight = r.I32();
    e.kind = static_cast<std::uint8_t>(r.U8());
    e.shape = static_cast<std::uint8_t>(r.U8());
    e.size = static_cast<std::uint8_t>(r.U8());
    if (type > static_cast<std::uint32_t>(CombatEventType::Overtime)) return false;
    e.type = static_cast<CombatEventType>(type);
    return r.ok();
}

constexpr std::size_t kMatchupBytes = 1 + 1 + 1 + 1 + 4;

void WriteMatchup(ByteWriter& w, const Matchup& m) {
    w.U8(m.home);
    w.U8(m.away);
    w.Bool(m.awayIsGhost);
    w.Bool(m.awayIsMonsters);
    w.U32(m.encounter);
}

Matchup ReadMatchup(ByteReader& r) {
    Matchup m;
    m.home = static_cast<PlayerId>(r.U8());
    m.away = static_cast<PlayerId>(r.U8());
    m.awayIsGhost = r.Bool();
    m.awayIsMonsters = r.Bool();
    m.encounter = r.U32();
    return m;
}

bool Fail(std::string* error, const std::string& message) {
    if (error) *error = "snapshot: " + message;
    return false;
}

// Verifies magic / version / trailer and parses the header. On success `body` is positioned right after the header.
bool ParseHeader(const std::vector<std::uint8_t>& bytes, SnapshotInfo& info, std::string* error) {
    if (bytes.size() < kHeaderBytes + kTrailerBytes) return Fail(error, "too short to be a snapshot");
    ByteReader tail(bytes.data() + bytes.size() - kTrailerBytes, kTrailerBytes);
    if (tail.U64() != Checksum(bytes.data(), bytes.size() - kTrailerBytes)) {
        return Fail(error, "checksum mismatch (the data is corrupt or truncated)");
    }
    ByteReader r(bytes.data(), bytes.size() - kTrailerBytes);
    if (r.U32() != kSnapshotMagic) return Fail(error, "bad magic (not a match snapshot)");
    info.version = r.U32();
    if (info.version != kSnapshotVersion) {
        return Fail(error, "unsupported format version " + std::to_string(info.version) + " (this build reads " +
                               std::to_string(kSnapshotVersion) + ")");
    }
    info.seed = r.U64();
    info.configHash = r.U64();
    info.championHash = r.U64();
    info.itemHash = r.U64();
    info.encounterHash = r.U64();
    info.motherNatureHash = r.U64();
    info.stateHash = r.U64();
    info.phase = static_cast<std::uint8_t>(r.U8());
    info.round = r.I32();
    info.ticksInPhase = r.I32();
    info.playerCount = static_cast<int>(r.U8());
    return r.ok();
}

}  // namespace

bool ReadSnapshotInfo(const std::vector<std::uint8_t>& bytes, SnapshotInfo& out, std::string* error) {
    SnapshotInfo info;
    if (!ParseHeader(bytes, info, error)) return false;
    out = info;
    return true;
}

// ---- Save ---------------------------------------------------------------------------------

std::vector<std::uint8_t> MatchManager::Snapshot() const {
    ByteWriter w;
    w.U32(kSnapshotMagic);
    w.U32(kSnapshotVersion);
    w.U64(seed_);
    w.U64(config_.ContentHash());
    w.U64(database_.ContentHash());
    w.U64(items_ != nullptr ? items_->ContentHash() : 0);
    w.U64(encounters_ != nullptr ? encounters_->ContentHash() : 0);
    w.U64(motherNature_ != nullptr ? motherNature_->ContentHash() : 0);
    w.U64(StateHash());
    w.U8(static_cast<std::uint32_t>(phase_));
    w.I32(round_);
    w.I32(ticksInPhase_);
    w.U8(static_cast<std::uint32_t>(players_.PlayerCount()));

    // ---- body ----
    w.Rng(rng_.GetState());
    w.U8(winner_);

    const auto& champions = database_.All();
    w.U32(static_cast<std::uint32_t>(champions.size()));
    for (const ChampionDefinition& champion : champions) w.I32(pool_.Remaining(champion.id));

    for (int i = 0; i < players_.PlayerCount(); ++i) {
        const PlayerState& p = *players_.Get(static_cast<PlayerId>(i));
        w.I32(p.Health());
        w.I32(p.Level());
        w.I32(p.Xp());
        w.I32(p.Gold());
        w.I32(p.Streak());
        w.Bool(!p.IsAlive());
        w.I32(p.Placement());

        w.U32(p.Roster().NextSerial());
        w.U32(static_cast<std::uint32_t>(p.Roster().Units().size()));
        for (const UnitInstance& unit : p.Roster().Units()) {
            w.U32(unit.id);
            w.U32(unit.champion->id);
            w.U8(static_cast<std::uint32_t>(unit.starLevel));
            w.U8(static_cast<std::uint32_t>(unit.location));
            w.I32(unit.x);
            w.I32(unit.y);
            for (ItemId item : unit.items) w.U32(item);
        }

        w.U32(static_cast<std::uint32_t>(p.ItemBag().size()));
        for (ItemId item : p.ItemBag()) w.U32(item);

        w.U32(static_cast<std::uint32_t>(p.Shop().Slots().size()));
        for (const ChampionDefinition* slot : p.Shop().Slots()) w.U32(slot != nullptr ? slot->id : kInvalidChampionId);
        w.Rng(p.Shop().GetRngState());

        const PlayerGifts& gifts = gifts_[static_cast<std::size_t>(i)];
        w.Bool(gifts.settled);
        w.U32(static_cast<std::uint32_t>(gifts.offers.size()));
        for (const GiftOffer& offer : gifts.offers) {
            w.U32(offer.gift);
            w.U8(static_cast<std::uint32_t>(offer.type));
            w.I32(offer.amount);
            w.U32(offer.item);
            w.U32(offer.champion != nullptr ? offer.champion->id : kInvalidChampionId);
        }
    }

    w.U32(static_cast<std::uint32_t>(matchups_.size()));
    for (const Matchup& m : matchups_) WriteMatchup(w, m);

    w.U32(static_cast<std::uint32_t>(outcomes_.size()));
    for (const CombatOutcome& o : outcomes_) {
        WriteMatchup(w, o.matchup);
        w.U8(static_cast<std::uint32_t>(o.winner));
        w.I32(o.winnerSurvivors);
        w.I32(o.damageToLoser);
        w.U8(static_cast<std::uint32_t>(o.drop.type));
        w.I32(o.drop.gold);
        w.U32(o.drop.champion);
        w.U32(o.drop.item);
        w.I32(o.log.endTick);
        w.I32(o.log.survivors[0]);
        w.I32(o.log.survivors[1]);
        w.I32(o.log.pathSearches);
        w.U64(o.log.checksum);
        w.U32(static_cast<std::uint32_t>(o.log.events.size()));
        for (const CombatEvent& e : o.log.events) WriteEvent(w, e);
    }

    std::vector<std::uint8_t> bytes = std::move(w.Bytes());
    const std::uint64_t checksum = Checksum(bytes.data(), bytes.size());
    for (int i = 0; i < 8; ++i) bytes.push_back(static_cast<std::uint8_t>(checksum >> (8 * i)));
    return bytes;
}

// ---- Restore ------------------------------------------------------------------------------

std::unique_ptr<MatchManager> MatchManager::Restore(const std::vector<std::uint8_t>& snapshot, const GameConfig& config,
                                                    const ChampionDatabase& database,
                                                    std::unique_ptr<ICombatSimulator> simulator, std::string* error,
                                                    const ItemDatabase* items, const EncounterDatabase* encounters,
                                                    const MotherNatureDatabase* motherNature, const RestoreOptions& options) {
    const auto fail = [error](const std::string& message) -> std::unique_ptr<MatchManager> {
        Fail(error, message);
        return nullptr;
    };

    SnapshotInfo info;
    if (!ParseHeader(snapshot, info, error)) return nullptr;

    if (options.requireMatchingData) {
        if (info.configHash != config.ContentHash()) return fail("taken under a different game configuration");
        if (info.championHash != database.ContentHash()) return fail("taken under different champion data");
        if (info.itemHash != (items != nullptr ? items->ContentHash() : 0)) return fail("taken under different item data");
        if (info.encounterHash != (encounters != nullptr ? encounters->ContentHash() : 0)) return fail("taken under different PvE data");
        if (info.motherNatureHash != (motherNature != nullptr ? motherNature->ContentHash() : 0)) return fail("taken under different Mother Nature data");
    }
    if (info.phase > static_cast<std::uint8_t>(MatchPhase::MatchOver)) return fail("unknown phase");
    if (info.playerCount != config.match.playerCount) return fail("player count does not match the configuration");

    std::unique_ptr<MatchManager> match = Create(config, database, info.seed, std::move(simulator), error, items, encounters, motherNature);
    if (!match) return nullptr;
    match->phase_ = static_cast<MatchPhase>(info.phase);
    match->round_ = info.round;
    match->ticksInPhase_ = info.ticksInPhase;
    const bool active = match->phase_ != MatchPhase::NotStarted && match->phase_ != MatchPhase::MatchOver;
    if (active ? (info.round < 1 || info.ticksInPhase < 0 || info.ticksInPhase >= match->PhaseDuration(match->phase_))
               : (info.ticksInPhase != 0 || (match->phase_ == MatchPhase::NotStarted && info.round != 0) ||
                  (match->phase_ == MatchPhase::MatchOver && info.round < 1))) {
        return fail("phase, round and tick counter are inconsistent");
    }

    ByteReader r(snapshot.data() + kHeaderBytes, snapshot.size() - kHeaderBytes - kTrailerBytes);

    const RngState matchRng = r.Rng();
    if (!matchRng.IsValid()) return fail("invalid match RNG state");
    match->rng_.SetState(matchRng);
    match->winner_ = static_cast<PlayerId>(r.U8());
    if (match->winner_ != kInvalidPlayerId && match->winner_ >= info.playerCount) return fail("winner is not a seat in this match");

    // Pool
    const auto& champions = database.All();
    if (r.Count(champions.size(), 4) != champions.size()) return fail("pool does not list this database's champions");
    std::vector<int> remaining(champions.size());
    for (int& copies : remaining) copies = r.I32();
    if (!r.ok()) return fail("truncated pool section");
    if (!match->pool_.RestoreRemaining(remaining)) return fail("pool counts exceed the champions' supply");

    // Players
    for (int i = 0; i < info.playerCount; ++i) {
        PlayerRestoreData data;
        data.health = r.I32();
        data.level = r.I32();
        data.xp = r.I32();
        data.gold = r.I32();
        data.streak = r.I32();
        data.eliminated = r.Bool();
        data.placement = r.I32();
        data.nextUnitSerial = r.U32();

        const std::size_t unitCount = r.Count(kMaxUnitsPerPlayer, kUnitBytes);
        for (std::size_t u = 0; u < unitCount; ++u) {
            UnitInstance unit;
            unit.id = r.U32();
            const ChampionDefinition* champion = database.Find(r.U32());
            const int star = static_cast<int>(r.U8());
            const std::uint32_t location = r.U8();
            unit.x = r.I32();
            unit.y = r.I32();
            for (ItemId& item : unit.items) item = r.U32();
            if (!r.ok()) return fail("truncated unit list");
            if (champion == nullptr) return fail("a unit uses a champion missing from the loaded data");
            if (location > static_cast<std::uint32_t>(LocationType::Board)) return fail("unknown unit location");
            unit.champion = champion;
            unit.starLevel = star;
            unit.location = static_cast<LocationType>(location);
            data.units.push_back(unit);
        }

        const std::size_t bagCount = r.Count(kMaxItemBag, 4);
        for (std::size_t b = 0; b < bagCount; ++b) data.itemBag.push_back(r.U32());

        const std::size_t slotCount = r.Count(64, 4);
        for (std::size_t s = 0; s < slotCount; ++s) {
            const ChampionId id = r.U32();
            const ChampionDefinition* champion = id == kInvalidChampionId ? nullptr : database.Find(id);
            if (id != kInvalidChampionId && champion == nullptr) return fail("a shop slot offers a champion missing from the loaded data");
            data.shopSlots.push_back(champion);
        }
        data.shopRng = r.Rng();
        if (!r.ok()) return fail("truncated player section");

        // Mother Nature's offers: only ever present during the gift phase, and every one must resolve in the loaded data.
        PlayerGifts& gifts = match->gifts_[static_cast<std::size_t>(i)];
        gifts.settled = r.Bool();
        const std::size_t offerCount = r.Count(4, kGiftBytes);
        for (std::size_t g = 0; g < offerCount; ++g) {
            GiftOffer offer;
            offer.gift = r.U32();
            const std::uint32_t type = r.U8();
            offer.amount = r.I32();
            offer.item = r.U32();
            const ChampionId champion = r.U32();
            if (!r.ok()) return fail("truncated gift offers");
            if (type > static_cast<std::uint32_t>(GiftType::Unit)) return fail("unknown gift type");
            offer.type = static_cast<GiftType>(type);
            const GiftDefinition* def = motherNature != nullptr ? motherNature->FindGift(offer.gift) : nullptr;
            if (def == nullptr || def->type != offer.type) return fail("a gift offer does not match the loaded Mother Nature data");
            if (offer.type == GiftType::Unit) {
                offer.champion = champion == kInvalidChampionId ? nullptr : database.Find(champion);
                if (offer.champion == nullptr || offer.champion->summon) return fail("a gift offers a champion missing from the loaded data");
            } else if (champion != kInvalidChampionId) {
                return fail("a non-unit gift names a champion");
            }
            if (offer.type == GiftType::Item && (items == nullptr || items->Find(offer.item) == nullptr)) return fail("a gift offers an item missing from the loaded data");
            if (offer.type != GiftType::Item && offer.item != 0) return fail("a non-item gift names an item");
            gifts.offers.push_back(offer);
        }
        if ((gifts.settled || !gifts.offers.empty()) && info.phase != static_cast<std::uint8_t>(MatchPhase::MotherNature)) {
            return fail("gift offers outside Mother Nature's phase");
        }
        if (gifts.settled && !gifts.offers.empty()) return fail("a settled gift choice still lists offers");

        std::string playerError;
        if (!match->players_.Get(static_cast<PlayerId>(i))->RestoreState(data, &playerError)) return fail(playerError);
    }

    // Matchups and fights
    const std::size_t matchupCount = r.Count(kMaxPlayers, kMatchupBytes);
    for (std::size_t m = 0; m < matchupCount; ++m) match->matchups_.push_back(ReadMatchup(r));
    const std::size_t outcomeCount = r.Count(kMaxPlayers, kMatchupBytes + 1 + 4 + 4 + 1 + 4 + 4 + 4 + 4 + 4 + 4 + 4 + 8 + 4);
    for (std::size_t o = 0; o < outcomeCount; ++o) {
        CombatOutcome outcome;
        outcome.matchup = ReadMatchup(r);
        const std::uint32_t winner = r.U8();
        outcome.winnerSurvivors = r.I32();
        outcome.damageToLoser = r.I32();
        const std::uint32_t dropType = r.U8();
        outcome.drop.gold = r.I32();
        outcome.drop.champion = r.U32();
        outcome.drop.item = r.U32();
        outcome.log.endTick = r.I32();
        outcome.log.survivors[0] = r.I32();
        outcome.log.survivors[1] = r.I32();
        outcome.log.pathSearches = r.I32();
        outcome.log.checksum = r.U64();
        const std::size_t eventCount = r.Count(static_cast<std::size_t>(1) << 24, kEventBytes);
        outcome.log.events.resize(eventCount);
        for (CombatEvent& e : outcome.log.events) {
            if (!ReadEvent(r, e)) return fail("a combat event is malformed");
        }
        if (!r.ok()) return fail("truncated combat section");
        if (winner > static_cast<std::uint32_t>(CombatWinner::Draw)) return fail("unknown combat winner");
        if (dropType > static_cast<std::uint32_t>(PveDropType::Item)) return fail("unknown PvE drop type");
        outcome.winner = static_cast<CombatWinner>(winner);
        outcome.drop.type = static_cast<PveDropType>(dropType);
        if (outcome.log.ComputeChecksum() != outcome.log.checksum) return fail("a combat log does not match its checksum");
        match->outcomes_.push_back(std::move(outcome));
    }
    if (!r.ok() || !r.atEnd()) return fail(r.ok() ? "unexpected trailing data" : "truncated body");

    const auto validMatchup = [&](const Matchup& m) {
        if (m.home >= info.playerCount) return false;
        return m.awayIsMonsters ? (m.away == kInvalidPlayerId && !m.awayIsGhost) : m.away < info.playerCount;
    };
    for (const Matchup& m : match->matchups_) {
        if (!validMatchup(m)) return fail("a matchup names a seat that does not exist");
    }
    for (const CombatOutcome& o : match->outcomes_) {
        if (!validMatchup(o.matchup)) return fail("a fight names a seat that does not exist");
    }
    match->combatPhaseTicks_ = match->ComputeCombatTicks();   // derived from the fights, never stored

    // The proof: what we rebuilt must be a consistent match AND hash to exactly what the original hashed to.
    if (!match->VerifyRosterLayouts()) return fail("restored rosters violate the layout invariants");
    if (!match->VerifyPoolIntegrity()) return fail("restored pool does not balance against the shops and rosters");
    if (match->StateHash() != info.stateHash) return fail("restored state does not reproduce the recorded state hash");
    // A snapshot taken at the start of a Planning phase IS that phase's recovery point: keep it as such.
    if (match->phase_ == MatchPhase::Planning && match->ticksInPhase_ == 0) {
        match->autoSnapshot_ = snapshot;
        match->autoSnapshotRound_ = match->round_;
    }
    return match;
}

}  // namespace w2f
