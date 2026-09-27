#include "w2f/net/BotNames.h"

#include <algorithm>

namespace w2f::net {

namespace {

const char* const kFirst[] = {"Shadow", "Iron", "Lunar", "Solar", "Frost", "Ember", "Storm", "Void", "Silent", "Crimson", "Golden", "Mossy", "Rune", "Star",
                              "Thorn", "Wild", "Arcane", "Swift", "Grim", "Lucky", "Sly", "Brave", "Quiet", "Stone", "Tidal", "Hex", "Ashen", "Neon"};
const char* const kSecond[] = {"Fox", "Blade", "Wolf", "Sage", "Knight", "Rogue", "Owl", "Titan", "Viper", "Pip", "Bard", "Golem", "Hawk", "Drake",
                               "Monk", "Lynx", "Wisp", "Tactician", "Gambit", "Rook", "Oracle", "Sprout", "Comet", "Warden", "Moth", "Otter", "Raven", "Pilot"};

std::string Lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

}  // namespace

std::vector<std::string> PickBotNames(int count, const std::function<std::uint64_t()>& entropy, const std::set<std::string>& taken) {
    std::vector<std::string> names;
    std::set<std::string> used = taken;
    constexpr std::uint64_t kFirstCount = sizeof(kFirst) / sizeof(kFirst[0]);
    constexpr std::uint64_t kSecondCount = sizeof(kSecond) / sizeof(kSecond[0]);
    for (int tries = 0; static_cast<int>(names.size()) < count && tries < 1000; ++tries) {
        const std::uint64_t r = entropy();
        std::string name = std::string(kFirst[r % kFirstCount]) + kSecond[(r / kFirstCount) % kSecondCount];
        const std::uint64_t style = (r >> 24) % 4;   // a quarter of them carry a number, like real accounts
        if (style == 0) name += std::to_string((r >> 32) % 99 + 1);
        if (used.count(Lower(name)) != 0 || name.size() > 16) continue;
        used.insert(Lower(name));
        names.push_back(name);
    }
    while (static_cast<int>(names.size()) < count) names.push_back("Tactician" + std::to_string(names.size() + 1));   // (never: 784 x 100 combinations)
    return names;
}

}  // namespace w2f::net
