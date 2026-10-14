#pragma once

// Account progression (demo 2.0, protocol revision 10): what a player earns by playing and what they can spend it on, like the League / TFT client.
//
//   * COINS (an earned currency, never real money) and ACCOUNT XP for every finished match, by placement and mode;
//   * the ACCOUNT LEVEL, from the XP (each level asks a little more than the one before); a level-up pays coins;
//   * MISSIONS: a few daily and weekly tasks ("play 3 games", "finish top 4 twice", "field a 3-star"), the same for everyone on a given day / week,
//     drawn from a pool; finishing one pays coins and XP at once;
//   * the STORE: cosmetics bought with coins -- profile borders, titles, arena themes. Each kind has a free default everybody owns.
//
// Everything is data (data/progression.json, format below and in docs/network-protocol.md); the server only applies it. Like the accounts it knows
// nothing about sockets and takes the wall clock from the caller.

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace w2f::net {

struct StoreItem {
    std::string id;     // "border_gold"
    std::string kind;   // "border" | "title" | "arena"
    std::string name;   // "Gold Rim"
    std::string desc;
    int price = 0;      // coins; 0 = free
    bool isDefault = false;   // the kind's default: owned by everyone, equipped when nothing else is
};

// One mission. `kind` says what counts, once per finished match that qualifies (every primitive is generic: no mission is hard-coded):
//   play        any finished match                    top4       placement 1..4          win     placement 1
//   level       reached level >= value                 three_star a 3-star unit on the final board
//   items       >= value items on the final board      cost       a champion of cost >= value on the final board
//   champions   >= value units on the final board
// `mode` ("" = any) limits it to one queue ("bots" | "normal" | "ranked").
struct MissionDef {
    std::string id;
    std::string period;   // "daily" | "weekly"
    std::string kind;
    std::string mode;
    std::string text;     // "Finish in the top 4 twice"
    int count = 1;        // how many qualifying matches
    int value = 0;
    int coins = 0, xp = 0;   // the reward
};

struct Progression {
    std::vector<int> coinsByPlacement = {60, 50, 45, 40, 30, 25, 20, 20};   // [placement - 1]
    std::vector<int> xpByPlacement = {220, 200, 185, 170, 140, 125, 110, 100};
    std::map<std::string, int> modePercent = {{"bots", 50}, {"normal", 100}, {"ranked", 125}};
    int levelBase = 500, levelStep = 50, levelMax = 1500;   // XP for level L -> L+1: min(levelMax, levelBase + levelStep * (L - 1))
    int levelUpCoins = 150;
    int startingCoins = 500;   // a new account's coins
    int dailyCount = 3, weeklyCount = 3;
    std::vector<StoreItem> store;
    std::vector<MissionDef> missions;

    const StoreItem* FindItem(std::string_view id) const;
    const StoreItem* DefaultOf(std::string_view kind) const;
    // The coins / XP a placement in a mode earns.
    int CoinsFor(std::string_view mode, int placement) const;
    int XpFor(std::string_view mode, int placement) const;
    int XpToNext(int level) const;
    // A total XP as a level (1..) plus the XP into it and the XP that level needs.
    void LevelOf(int totalXp, int& level, int& into, int& need) const;
    // The missions of a day (unix day number) / week (number of the Monday-to-Sunday week): the same for every player, drawn from the pool.
    std::vector<const MissionDef*> Daily(long long day) const;
    std::vector<const MissionDef*> Weekly(long long week) const;

    // Loads data/progression.json; nullptr + *error on a bad file. A missing file is NOT an error for the caller to ignore: pass the defaults instead.
    static std::unique_ptr<Progression> FromFile(const std::string& path, std::string* error);
    static std::unique_ptr<Progression> FromText(std::string_view text, std::string* error);
};

constexpr long long kMsPerDay = 86'400'000;
inline long long DayOf(std::uint64_t ms) { return static_cast<long long>(ms / kMsPerDay); }
inline long long WeekOf(std::uint64_t ms) { return (DayOf(ms) + 3) / 7; }   // 1970-01-01 was a Thursday: weeks start on Monday
std::uint64_t NextDayMs(std::uint64_t ms);
std::uint64_t NextWeekMs(std::uint64_t ms);

// What a finished match looks like to the missions.
struct MissionMatch {
    std::string mode;
    int placement = 0, level = 0;
    int items = 0, units = 0, maxCost = 0;
    bool threeStar = false;
};
bool MissionCounts(const MissionDef& mission, const MissionMatch& match);

}  // namespace w2f::net
