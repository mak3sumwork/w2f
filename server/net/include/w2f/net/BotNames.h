#pragma once

// Usernames for the AI players (demo 1.5, FEEDBACK V5 "make the 7 bots have usernames"): they look like players' names, never like "Bot 3".
// Built from two word lists (+ sometimes a number), so a lobby never shows the same name twice; `taken` (lower case) keeps them off real players' names.

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace w2f::net {

std::vector<std::string> PickBotNames(int count, const std::function<std::uint64_t()>& entropy, const std::set<std::string>& taken = {});

}  // namespace w2f::net
