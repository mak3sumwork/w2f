#pragma once

// Display text for the data (text_en.json): names, titles, ability / trait / item descriptions, Mother Nature gift wording.
// A flat map key -> string. The engine never reads it: it only travels to clients in the `catalog` message, so a client can build its
// localisation table (Unreal: a String Table) without hard-coding wording. Keys are documented at the top of data/text_en.json.
//
// Strict like the other loaders: unknown top-level keys, non-string entries, empty keys / texts and duplicate keys are errors that name
// the place in the file. Entries keep the order of the file, so the catalog message is stable.

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace w2f {

class TextTable {
public:
    static std::unique_ptr<TextTable> FromJson(std::string_view text, std::string* error = nullptr);
    static std::unique_ptr<TextTable> FromFile(const std::string& path, std::string* error = nullptr);

    const std::string& Language() const { return language_; }   // "en"
    const std::vector<std::pair<std::string, std::string>>& Entries() const { return entries_; }
    // nullptr if there is no such key. The pointer lives as long as the table.
    const std::string* Find(std::string_view key) const;

private:
    TextTable() = default;
    std::string language_;
    std::vector<std::pair<std::string, std::string>> entries_;
};

}  // namespace w2f
