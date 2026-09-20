#pragma once

// A small, dependency-free JSON reader for game data files.
//
// Differences from a general-purpose library, chosen for this project:
//  * Numbers are kept as EXACT decimals (integer mantissa + decimal scale), never as floats.
//    "0.78" is 78 / 10^2 and converts to 780 milli-units by integer arithmetic, so loading data
//    is as deterministic as the simulation that consumes it.
//  * Comments (// line, and /* block */) are accepted, because designers annotate data files.
//    Trailing commas are not.
//  * Duplicate object keys are an error (a silent "last one wins" hides designer mistakes).
//  * Every value remembers its line/column so later validation errors can point at the file.
//  * No exceptions: parse failures come back as a message.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace w2f::json {

class Value {
public:
    enum class Type : std::uint8_t { Null, Bool, Number, String, Array, Object };

    Type type() const { return type_; }
    bool IsNull() const { return type_ == Type::Null; }
    bool IsBool() const { return type_ == Type::Bool; }
    bool IsNumber() const { return type_ == Type::Number; }
    bool IsString() const { return type_ == Type::String; }
    bool IsArray() const { return type_ == Type::Array; }
    bool IsObject() const { return type_ == Type::Object; }
    static const char* TypeName(Type type);

    bool AsBool() const { return bool_; }
    const std::string& AsString() const { return string_; }

    // Arrays.
    const std::vector<Value>& Items() const { return values_; }
    // Objects: members in file order.
    std::size_t MemberCount() const { return values_.size(); }
    const std::string& MemberKey(std::size_t i) const { return keys_[i]; }
    const Value& MemberValue(std::size_t i) const { return values_[i]; }
    const Value* Find(std::string_view key) const;  // nullptr if absent or not an object

    // ---- Numbers (exact) ----
    // Integer-valued numbers only ("3", "3.0" and "3e0" all qualify; "3.5" does not).
    bool ToInt(long long& out) const;
    // value * 10^decimals, exactly; false if that would need more precision than that
    // (e.g. 0.7812 with decimals = 3) or overflows. ToScaled(3) turns 0.78 into 780.
    bool ToScaled(int decimals, long long& out) const;
    // Seconds -> ticks: value * ticksPerSecond, rounded half up (1.75 s at 30/s = 52.5 -> 53).
    // Negative values are rejected.
    bool ToTicks(int ticksPerSecond, long long& out) const;

    // Source position (1-based) of the first character of this value.
    int line() const { return line_; }
    int column() const { return column_; }

private:
    friend class Parser;

    Type type_ = Type::Null;
    bool bool_ = false;
    long long mantissa_ = 0;  // number = mantissa_ / 10^scale_, normalised (no trailing zeros while scale_ > 0)
    int scale_ = 0;
    std::string string_;
    std::vector<std::string> keys_;  // objects only, parallel to values_
    std::vector<Value> values_;      // array items, or object member values
    int line_ = 0;
    int column_ = 0;
};

// Parses a whole document (one value, then only whitespace / comments).
// On failure returns false and sets *error to "line L, column C: message".
bool Parse(std::string_view text, Value& out, std::string* error);

}  // namespace w2f::json
