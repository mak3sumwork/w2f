#pragma once

// The crash-recovery file `w2f_server --autosave FILE` writes and `--resume FILE` reads: ONE file holding both halves of a restart point -- the engine's
// snapshot and the seats sidecar (tokens, bots, bot state; see GameServer::SetSnapshotSink) -- so a crash in the middle of saving can never leave a
// snapshot from one round next to the seats of another (the file is written to a temporary and renamed into place).
//
//   bytes 0..3   'W' '2' 'F' 'R'
//   bytes 4..7   little-endian u32: the length N of the seats JSON
//   next N       the seats JSON (UTF-8)
//   the rest     the engine snapshot (docs/snapshots.md)

#include <cstdint>
#include <string>
#include <vector>

namespace w2f::net {

inline std::vector<std::uint8_t> PackResumeFile(const std::vector<std::uint8_t>& snapshot, const std::string& seatsJson) {
    std::vector<std::uint8_t> out = {'W', '2', 'F', 'R'};
    const std::uint32_t n = static_cast<std::uint32_t>(seatsJson.size());
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(n >> (8 * i)));
    out.insert(out.end(), seatsJson.begin(), seatsJson.end());
    out.insert(out.end(), snapshot.begin(), snapshot.end());
    return out;
}

inline bool UnpackResumeFile(const std::vector<std::uint8_t>& file, std::vector<std::uint8_t>& snapshot, std::string& seatsJson, std::string* error = nullptr) {
    const auto fail = [error](const char* why) {
        if (error) *error = why;
        return false;
    };
    if (file.size() < 8 || file[0] != 'W' || file[1] != '2' || file[2] != 'F' || file[3] != 'R') return fail("not a W2F resume file");
    std::uint32_t n = 0;
    for (int i = 0; i < 4; ++i) n |= static_cast<std::uint32_t>(file[static_cast<std::size_t>(4 + i)]) << (8 * i);
    if (static_cast<std::uint64_t>(n) + 8 > file.size()) return fail("the resume file is truncated");
    seatsJson.assign(file.begin() + 8, file.begin() + 8 + static_cast<std::ptrdiff_t>(n));
    snapshot.assign(file.begin() + 8 + static_cast<std::ptrdiff_t>(n), file.end());
    return true;
}

}  // namespace w2f::net
