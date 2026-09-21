#include "w2f/Text.h"

#include <fstream>
#include <sstream>

#include "w2f/Json.h"

namespace w2f {

namespace {
std::string At(const json::Value& v) { return "line " + std::to_string(v.line()) + ", column " + std::to_string(v.column()) + ": "; }
}  // namespace

std::unique_ptr<TextTable> TextTable::FromJson(std::string_view text, std::string* error) {
    const auto fail = [error](const std::string& message) -> std::unique_ptr<TextTable> {
        if (error) *error = message;
        return nullptr;
    };
    json::Value root;
    std::string parseError;
    if (!json::Parse(text, root, &parseError)) return fail(parseError);
    if (!root.IsObject()) return fail(At(root) + "the text file must be a JSON object");
    std::unique_ptr<TextTable> table(new TextTable());
    const json::Value* entries = nullptr;
    bool haveVersion = false;
    for (std::size_t i = 0; i < root.MemberCount(); ++i) {
        const std::string& key = root.MemberKey(i);
        const json::Value& value = root.MemberValue(i);
        if (key == "version") {
            long long version = 0;
            if (!value.ToInt(version) || version != 1) return fail(At(value) + "\"version\" must be 1");
            haveVersion = true;
        } else if (key == "language") {
            if (!value.IsString() || value.AsString().empty()) return fail(At(value) + "\"language\" must be a non-empty string");
            table->language_ = value.AsString();
        } else if (key == "entries") {
            entries = &value;
        } else {
            return fail(At(value) + "unknown key \"" + key + "\" (allowed: version, language, entries)");
        }
    }
    if (!haveVersion) return fail("missing \"version\"");
    if (table->language_.empty()) return fail("missing \"language\"");
    if (entries == nullptr || !entries->IsObject()) return fail("missing \"entries\" (an object of key: text)");
    table->entries_.reserve(entries->MemberCount());
    for (std::size_t i = 0; i < entries->MemberCount(); ++i) {
        const std::string& key = entries->MemberKey(i);
        const json::Value& value = entries->MemberValue(i);
        if (key.empty()) return fail(At(value) + "empty key");
        if (!value.IsString() || value.AsString().empty()) return fail(At(value) + "entry \"" + key + "\" must be a non-empty string");
        table->entries_.emplace_back(key, value.AsString());
    }
    return table;
}

std::unique_ptr<TextTable> TextTable::FromFile(const std::string& path, std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (error) *error = "cannot open text file '" + path + "'";
        return nullptr;
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    std::string inner;
    auto table = FromJson(contents.str(), &inner);
    if (!table && error) *error = path + ": " + inner;
    return table;
}

const std::string* TextTable::Find(std::string_view key) const {
    for (const auto& entry : entries_) {
        if (entry.first == key) return &entry.second;
    }
    return nullptr;
}

}  // namespace w2f
