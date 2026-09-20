#pragma once

// A tiny JSON builder for the messages the server sends. Handles commas and escaping so message code cannot produce invalid
// JSON by construction. Integers only (the protocol has no floating point, like the engine).

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace w2f::net {

class JsonWriter {
public:
    JsonWriter& BeginObject() { Value(); out_ += '{'; first_.push_back(true); return *this; }
    JsonWriter& EndObject() { out_ += '}'; first_.pop_back(); return *this; }
    JsonWriter& BeginArray() { Value(); out_ += '['; first_.push_back(true); return *this; }
    JsonWriter& EndArray() { out_ += ']'; first_.pop_back(); return *this; }

    // Inside an object: Key("gold").Int(3). Or use the Field helpers below.
    JsonWriter& Key(std::string_view key) {
        Comma();
        AppendString(key);
        out_ += ':';
        afterKey_ = true;
        return *this;
    }
    JsonWriter& String(std::string_view s) { Value(); AppendString(s); return *this; }
    JsonWriter& Int(long long v) { Value(); out_ += std::to_string(v); return *this; }
    JsonWriter& UInt(std::uint64_t v) { Value(); out_ += std::to_string(v); return *this; }
    JsonWriter& Bool(bool v) { Value(); out_ += v ? "true" : "false"; return *this; }
    JsonWriter& Null() { Value(); out_ += "null"; return *this; }
    // Splices already-serialized JSON (from another JsonWriter) in as one value.
    JsonWriter& Raw(std::string_view json) { Value(); out_.append(json); return *this; }

    JsonWriter& Field(std::string_view k, std::string_view v) { return Key(k).String(v); }
    JsonWriter& Field(std::string_view k, const char* v) { return Key(k).String(v); }
    JsonWriter& Field(std::string_view k, const std::string& v) { return Key(k).String(v); }
    JsonWriter& Field(std::string_view k, int v) { return Key(k).Int(v); }
    JsonWriter& Field(std::string_view k, long long v) { return Key(k).Int(v); }
    JsonWriter& Field(std::string_view k, std::uint32_t v) { return Key(k).UInt(v); }
    JsonWriter& Field(std::string_view k, std::uint64_t v) { return Key(k).UInt(v); }
    JsonWriter& Field(std::string_view k, bool v) { return Key(k).Bool(v); }

    const std::string& str() const { return out_; }
    std::string Take() { return std::move(out_); }

private:
    // Called before every value: a value directly after a key needs no comma; otherwise elements are comma-separated.
    void Value() {
        if (afterKey_) { afterKey_ = false; return; }
        Comma();
    }
    void Comma() {
        if (first_.empty()) return;
        if (first_.back()) first_.back() = false;
        else out_ += ',';
    }
    void AppendString(std::string_view s) {
        static const char kHex[] = "0123456789abcdef";
        out_ += '"';
        for (char c : s) {
            const auto u = static_cast<unsigned char>(c);
            switch (c) {
                case '"': out_ += "\\\""; break;
                case '\\': out_ += "\\\\"; break;
                case '\n': out_ += "\\n"; break;
                case '\r': out_ += "\\r"; break;
                case '\t': out_ += "\\t"; break;
                default:
                    if (u < 0x20 || u == 0x7F) { out_ += "\\u00"; out_ += kHex[u >> 4]; out_ += kHex[u & 15]; }
                    else out_ += c;   // UTF-8 passes through unchanged
            }
        }
        out_ += '"';
    }

    std::string out_;
    std::vector<bool> first_;
    bool afterKey_ = false;
};

}  // namespace w2f::net
