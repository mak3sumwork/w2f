#include "w2f/net/Progression.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "w2f/Json.h"

namespace w2f::net {

namespace {

int IntOr(const json::Value* v, int fallback) {
    long long n = 0;
    return v != nullptr && v->IsNumber() && v->ToInt(n) ? static_cast<int>(n) : fallback;
}
std::string StrOr(const json::Value* v, std::string fallback = {}) { return v != nullptr && v->IsString() ? v->AsString() : fallback; }

bool Fail(std::string* error, const std::string& what) {
    if (error != nullptr) *error = what;
    return false;
}

bool ReadInts(const json::Value* v, std::vector<int>& out, std::size_t size, const char* path, std::string* error) {
    if (v == nullptr) return true;
    if (!v->IsArray() || v->Items().size() != size) return Fail(error, std::string(path) + ": an array of " + std::to_string(size) + " numbers");
    out.clear();
    for (const json::Value& n : v->Items()) {
        long long x = 0;
        if (!n.IsNumber() || !n.ToInt(x) || x < 0) return Fail(error, std::string(path) + ": numbers >= 0");
        out.push_back(static_cast<int>(x));
    }
    return true;
}

// The same pseudo-random draw on every server and every compiler (splitmix64): which missions a day / week has.
std::uint64_t Mix(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

std::vector<const MissionDef*> Draw(const std::vector<MissionDef>& all, const char* period, long long when, int count) {
    std::vector<const MissionDef*> pool;
    for (const MissionDef& m : all) {
        if (m.period == period) pool.push_back(&m);
    }
    std::vector<const MissionDef*> out;
    std::uint64_t seed = Mix(static_cast<std::uint64_t>(when) * 2 + (std::string(period) == "weekly" ? 1 : 0));
    while (!pool.empty() && static_cast<int>(out.size()) < count) {
        seed = Mix(seed);
        const std::size_t at = static_cast<std::size_t>(seed % pool.size());
        out.push_back(pool[at]);
        pool.erase(pool.begin() + static_cast<std::ptrdiff_t>(at));
    }
    return out;
}

}  // namespace

const StoreItem* Progression::FindItem(std::string_view id) const {
    for (const StoreItem& s : store) {
        if (s.id == id) return &s;
    }
    return nullptr;
}

const StoreItem* Progression::DefaultOf(std::string_view kind) const {
    for (const StoreItem& s : store) {
        if (s.kind == kind && s.isDefault) return &s;
    }
    return nullptr;
}

int Progression::CoinsFor(std::string_view mode, int placement) const {
    if (placement < 1 || placement > static_cast<int>(coinsByPlacement.size())) return 0;
    auto it = modePercent.find(std::string(mode));
    const int percent = it == modePercent.end() ? 100 : it->second;
    return coinsByPlacement[static_cast<std::size_t>(placement - 1)] * percent / 100;
}

int Progression::XpFor(std::string_view mode, int placement) const {
    if (placement < 1 || placement > static_cast<int>(xpByPlacement.size())) return 0;
    auto it = modePercent.find(std::string(mode));
    const int percent = it == modePercent.end() ? 100 : it->second;
    return xpByPlacement[static_cast<std::size_t>(placement - 1)] * percent / 100;
}

int Progression::XpToNext(int level) const { return std::max(1, std::min(levelMax, levelBase + levelStep * std::max(0, level - 1))); }

void Progression::LevelOf(int totalXp, int& level, int& into, int& need) const {
    level = 1;
    into = std::max(0, totalXp);
    need = XpToNext(level);
    while (into >= need && level < 9999) {
        into -= need;
        ++level;
        need = XpToNext(level);
    }
}

std::vector<const MissionDef*> Progression::Daily(long long day) const { return Draw(missions, "daily", day, dailyCount); }
std::vector<const MissionDef*> Progression::Weekly(long long week) const { return Draw(missions, "weekly", week, weeklyCount); }

std::uint64_t NextDayMs(std::uint64_t ms) { return static_cast<std::uint64_t>(DayOf(ms) + 1) * static_cast<std::uint64_t>(kMsPerDay); }
std::uint64_t NextWeekMs(std::uint64_t ms) { return static_cast<std::uint64_t>((WeekOf(ms) + 1) * 7 - 3) * static_cast<std::uint64_t>(kMsPerDay); }

bool MissionCounts(const MissionDef& m, const MissionMatch& match) {
    if (!m.mode.empty() && m.mode != match.mode) return false;
    if (match.placement < 1) return false;
    if (m.kind == "play") return true;
    if (m.kind == "top4") return match.placement <= 4;
    if (m.kind == "win") return match.placement == 1;
    if (m.kind == "level") return match.level >= m.value;
    if (m.kind == "three_star") return match.threeStar;
    if (m.kind == "items") return match.items >= m.value;
    if (m.kind == "cost") return match.maxCost >= m.value;
    if (m.kind == "champions") return match.units >= m.value;
    return false;
}

std::unique_ptr<Progression> Progression::FromText(std::string_view text, std::string* error) {
    json::Value root;
    std::string parseError;
    if (!json::Parse(text, root, &parseError)) { Fail(error, "progression: " + parseError); return nullptr; }
    if (!root.IsObject()) { Fail(error, "progression: not an object"); return nullptr; }
    auto p = std::make_unique<Progression>();
    if (const json::Value* r = root.Find("rewards")) {
        if (!ReadInts(r->Find("coins_by_placement"), p->coinsByPlacement, 8, "rewards.coins_by_placement", error)) return nullptr;
        if (!ReadInts(r->Find("xp_by_placement"), p->xpByPlacement, 8, "rewards.xp_by_placement", error)) return nullptr;
        if (const json::Value* mp = r->Find("mode_percent"); mp != nullptr && mp->IsObject()) {
            for (std::size_t i = 0; i < mp->MemberCount(); ++i) p->modePercent[mp->MemberKey(i)] = std::max(0, IntOr(&mp->MemberValue(i), 100));
        }
        p->levelUpCoins = std::max(0, IntOr(r->Find("level_up_coins"), p->levelUpCoins));
        p->startingCoins = std::max(0, IntOr(r->Find("starting_coins"), p->startingCoins));
    }
    if (const json::Value* l = root.Find("levels")) {
        p->levelBase = std::max(1, IntOr(l->Find("base"), p->levelBase));
        p->levelStep = std::max(0, IntOr(l->Find("step"), p->levelStep));
        p->levelMax = std::max(p->levelBase, IntOr(l->Find("max"), p->levelMax));
    }
    if (const json::Value* s = root.Find("store")) {
        if (!s->IsArray()) { Fail(error, "store: an array"); return nullptr; }
        for (std::size_t i = 0; i < s->Items().size(); ++i) {
            const json::Value& e = s->Items()[i];
            const std::string at = "store[" + std::to_string(i) + "]";
            StoreItem item;
            item.id = StrOr(e.Find("id"));
            item.kind = StrOr(e.Find("kind"));
            item.name = StrOr(e.Find("name"));
            item.desc = StrOr(e.Find("desc"));
            item.price = IntOr(e.Find("price"), -1);
            item.isDefault = e.Find("default") != nullptr && e.Find("default")->IsBool() && e.Find("default")->AsBool();
            if (item.id.empty() || item.id.size() > 40 || item.name.empty()) { Fail(error, at + ": id (1-40 characters) and name are required"); return nullptr; }
            if (item.kind != "border" && item.kind != "title" && item.kind != "arena") { Fail(error, at + ".kind: border, title or arena"); return nullptr; }
            if (item.price < 0) { Fail(error, at + ".price: a number >= 0"); return nullptr; }
            if (item.isDefault && item.price != 0) { Fail(error, at + ": a default costs 0"); return nullptr; }
            if (p->FindItem(item.id) != nullptr) { Fail(error, at + ".id: \"" + item.id + "\" twice"); return nullptr; }
            if (item.isDefault && p->DefaultOf(item.kind) != nullptr) { Fail(error, at + ": a second default " + item.kind); return nullptr; }
            p->store.push_back(item);
        }
        for (const char* kind : {"border", "title", "arena"}) {
            if (p->DefaultOf(kind) == nullptr) { Fail(error, std::string("store: no default ") + kind); return nullptr; }
        }
    }
    if (const json::Value* m = root.Find("missions")) {
        p->dailyCount = std::max(0, IntOr(m->Find("daily_count"), p->dailyCount));
        p->weeklyCount = std::max(0, IntOr(m->Find("weekly_count"), p->weeklyCount));
        const json::Value* pool = m->Find("pool");
        if (pool == nullptr || !pool->IsArray()) { Fail(error, "missions.pool: an array"); return nullptr; }
        for (std::size_t i = 0; i < pool->Items().size(); ++i) {
            const json::Value& e = pool->Items()[i];
            const std::string at = "missions.pool[" + std::to_string(i) + "]";
            MissionDef d;
            d.id = StrOr(e.Find("id"));
            d.period = StrOr(e.Find("period"));
            d.kind = StrOr(e.Find("kind"));
            d.mode = StrOr(e.Find("mode"));
            d.text = StrOr(e.Find("text"));
            d.count = IntOr(e.Find("count"), 1);
            d.value = IntOr(e.Find("value"), 0);
            d.coins = IntOr(e.Find("coins"), 0);
            d.xp = IntOr(e.Find("xp"), 0);
            static const char* const kKinds[] = {"play", "top4", "win", "level", "three_star", "items", "cost", "champions"};
            bool kindOk = false;
            for (const char* k : kKinds) kindOk = kindOk || d.kind == k;
            if (d.id.empty() || d.text.empty()) { Fail(error, at + ": id and text are required"); return nullptr; }
            if (d.period != "daily" && d.period != "weekly") { Fail(error, at + ".period: daily or weekly"); return nullptr; }
            if (!kindOk) { Fail(error, at + ".kind: unknown \"" + d.kind + "\""); return nullptr; }
            if (!d.mode.empty() && d.mode != "bots" && d.mode != "normal" && d.mode != "ranked") { Fail(error, at + ".mode: bots, normal, ranked or empty"); return nullptr; }
            if (d.count < 1 || d.coins < 0 || d.xp < 0) { Fail(error, at + ": count >= 1, coins and xp >= 0"); return nullptr; }
            for (const MissionDef& other : p->missions) {
                if (other.id == d.id) { Fail(error, at + ".id: \"" + d.id + "\" twice"); return nullptr; }
            }
            p->missions.push_back(d);
        }
    }
    return p;
}

std::unique_ptr<Progression> Progression::FromFile(const std::string& path, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { Fail(error, "cannot read " + path); return nullptr; }
    std::stringstream buffer;
    buffer << in.rdbuf();
    return FromText(buffer.str(), error);
}

}  // namespace w2f::net
