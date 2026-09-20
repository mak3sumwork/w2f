#include "w2f/Json.h"

#include <cstddef>

namespace w2f::json {

namespace {
constexpr int kMaxDepth = 64;
constexpr long long kMaxMantissa = 999'999'999'999'999'999LL;  // 18 digits

long long Pow10(int n) {
    long long v = 1;
    for (int i = 0; i < n; ++i) v *= 10;
    return v;
}
}  // namespace

const char* Value::TypeName(Type type) {
    switch (type) {
        case Type::Null: return "null";
        case Type::Bool: return "a boolean";
        case Type::Number: return "a number";
        case Type::String: return "a string";
        case Type::Array: return "an array";
        case Type::Object: return "an object";
    }
    return "?";
}

const Value* Value::Find(std::string_view key) const {
    if (type_ != Type::Object) return nullptr;
    for (std::size_t i = 0; i < keys_.size(); ++i) {
        if (keys_[i] == key) return &values_[i];
    }
    return nullptr;
}

bool Value::ToInt(long long& out) const { return ToScaled(0, out); }

bool Value::ToScaled(int decimals, long long& out) const {
    if (type_ != Type::Number || decimals < 0 || decimals > 9) return false;
    if (scale_ <= decimals) {
        const long long factor = Pow10(decimals - scale_);
        if (mantissa_ != 0 && (mantissa_ > kMaxMantissa / factor || mantissa_ < -kMaxMantissa / factor)) return false;
        out = mantissa_ * factor;
        return true;
    }
    const long long divisor = Pow10(scale_ - decimals);
    if (mantissa_ % divisor != 0) return false;  // would lose precision
    out = mantissa_ / divisor;
    return true;
}

bool Value::ToTicks(int ticksPerSecond, long long& out) const {
    if (type_ != Type::Number || mantissa_ < 0 || ticksPerSecond <= 0) return false;
    if (mantissa_ > 1'000'000'000'000LL) return false;  // keep the multiplications below in range
    const long long denominator = Pow10(scale_);
    const long long numerator = mantissa_ * ticksPerSecond;
    out = (numerator * 2 + denominator) / (denominator * 2);  // round half up
    return true;
}

// ---- Parser ------------------------------------------------------------------------------

class Parser {
public:
    Parser(std::string_view text, std::string* error) : text_(text), error_(error) {}

    bool ParseDocument(Value& out) {
        if (!SkipTrivia()) return false;
        if (!ParseValue(out, 0)) return false;
        if (!SkipTrivia()) return false;
        if (pos_ != text_.size()) return Fail("unexpected content after the end of the document");
        return true;
    }

private:
    bool Fail(const std::string& message) {
        if (error_) *error_ = "line " + std::to_string(line_) + ", column " + std::to_string(column_) + ": " + message;
        return false;
    }

    bool AtEnd() const { return pos_ >= text_.size(); }
    char Peek() const { return text_[pos_]; }

    char Take() {
        const char c = text_[pos_++];
        if (c == '\n') {
            ++line_;
            column_ = 1;
        } else {
            ++column_;
        }
        return c;
    }

    // Whitespace and comments.
    bool SkipTrivia() {
        while (!AtEnd()) {
            const char c = Peek();
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                Take();
            } else if (c == '/' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '/') {
                while (!AtEnd() && Peek() != '\n') Take();
            } else if (c == '/' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '*') {
                const int startLine = line_, startColumn = column_;
                Take();
                Take();
                bool closed = false;
                while (!AtEnd()) {
                    if (Peek() == '*' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '/') {
                        Take();
                        Take();
                        closed = true;
                        break;
                    }
                    Take();
                }
                if (!closed) {
                    line_ = startLine;
                    column_ = startColumn;
                    return Fail("unterminated /* comment");
                }
            } else {
                break;
            }
        }
        return true;
    }

    bool ParseValue(Value& out, int depth) {
        if (depth > kMaxDepth) return Fail("nesting is too deep");
        if (AtEnd()) return Fail("unexpected end of document");
        out.line_ = line_;
        out.column_ = column_;
        const char c = Peek();
        if (c == '{') return ParseObject(out, depth);
        if (c == '[') return ParseArray(out, depth);
        if (c == '"') {
            out.type_ = Value::Type::String;
            return ParseString(out.string_);
        }
        if (c == '-' || (c >= '0' && c <= '9')) return ParseNumber(out);
        if (Literal("true")) { out.type_ = Value::Type::Bool; out.bool_ = true; return true; }
        if (Literal("false")) { out.type_ = Value::Type::Bool; out.bool_ = false; return true; }
        if (Literal("null")) { out.type_ = Value::Type::Null; return true; }
        return Fail(std::string("unexpected character '") + c + "'");
    }

    bool Literal(std::string_view word) {
        if (text_.substr(pos_, word.size()) != word) return false;
        // Must not run on into an identifier ("trueish").
        const std::size_t after = pos_ + word.size();
        if (after < text_.size()) {
            const char n = text_[after];
            if ((n >= 'a' && n <= 'z') || (n >= 'A' && n <= 'Z') || (n >= '0' && n <= '9') || n == '_') return false;
        }
        for (std::size_t i = 0; i < word.size(); ++i) Take();
        return true;
    }

    bool ParseObject(Value& out, int depth) {
        out.type_ = Value::Type::Object;
        Take();  // {
        if (!SkipTrivia()) return false;
        if (!AtEnd() && Peek() == '}') { Take(); return true; }
        for (;;) {
            if (!SkipTrivia()) return false;
            if (AtEnd() || Peek() != '"') return Fail("expected a string key");
            const int keyLine = line_, keyColumn = column_;
            std::string key;
            if (!ParseString(key)) return false;
            for (const std::string& existing : out.keys_) {
                if (existing == key) {
                    line_ = keyLine;
                    column_ = keyColumn;
                    return Fail("duplicate key \"" + key + "\"");
                }
            }
            if (!SkipTrivia()) return false;
            if (AtEnd() || Peek() != ':') return Fail("expected ':' after the key");
            Take();
            if (!SkipTrivia()) return false;
            Value member;
            if (!ParseValue(member, depth + 1)) return false;
            out.keys_.push_back(std::move(key));
            out.values_.push_back(std::move(member));
            if (!SkipTrivia()) return false;
            if (AtEnd()) return Fail("unterminated object");
            if (Peek() == ',') { Take(); continue; }
            if (Peek() == '}') { Take(); return true; }
            return Fail("expected ',' or '}'");
        }
    }

    bool ParseArray(Value& out, int depth) {
        out.type_ = Value::Type::Array;
        Take();  // [
        if (!SkipTrivia()) return false;
        if (!AtEnd() && Peek() == ']') { Take(); return true; }
        for (;;) {
            if (!SkipTrivia()) return false;
            Value item;
            if (!ParseValue(item, depth + 1)) return false;
            out.values_.push_back(std::move(item));
            if (!SkipTrivia()) return false;
            if (AtEnd()) return Fail("unterminated array");
            if (Peek() == ',') { Take(); continue; }
            if (Peek() == ']') { Take(); return true; }
            return Fail("expected ',' or ']'");
        }
    }

    static int HexDigit(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    bool ParseHex4(unsigned& out) {
        out = 0;
        for (int i = 0; i < 4; ++i) {
            if (AtEnd()) return Fail("unterminated \\u escape");
            const int d = HexDigit(Peek());
            if (d < 0) return Fail("invalid \\u escape");
            Take();
            out = out * 16 + static_cast<unsigned>(d);
        }
        return true;
    }

    static void AppendUtf8(std::string& s, unsigned cp) {
        if (cp < 0x80) {
            s += static_cast<char>(cp);
        } else if (cp < 0x800) {
            s += static_cast<char>(0xC0 | (cp >> 6));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            s += static_cast<char>(0xE0 | (cp >> 12));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            s += static_cast<char>(0xF0 | (cp >> 18));
            s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool ParseString(std::string& out) {
        Take();  // opening quote
        out.clear();
        for (;;) {
            if (AtEnd()) return Fail("unterminated string");
            const char c = Take();
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return Fail("control character inside a string");
            if (c != '\\') { out += c; continue; }
            if (AtEnd()) return Fail("unterminated string");
            const char e = Take();
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp = 0;
                    if (!ParseHex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {  // high surrogate: needs a following \uDC00..DFFF
                        if (pos_ + 1 >= text_.size() || Peek() != '\\' || text_[pos_ + 1] != 'u') return Fail("lone surrogate in \\u escape");
                        Take();
                        Take();
                        unsigned low = 0;
                        if (!ParseHex4(low)) return false;
                        if (low < 0xDC00 || low > 0xDFFF) return Fail("invalid surrogate pair");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return Fail("lone surrogate in \\u escape");
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default: return Fail(std::string("invalid escape '\\") + e + "'");
            }
        }
    }

    bool ParseNumber(Value& out) {
        out.type_ = Value::Type::Number;
        bool negative = false;
        if (Peek() == '-') { negative = true; Take(); }
        if (AtEnd() || Peek() < '0' || Peek() > '9') return Fail("a digit is required after '-'");

        long long mantissa = 0;
        int digits = 0;   // significant digits stored
        int scale = 0;

        auto addDigit = [&](char d) -> bool {
            if (mantissa == 0 && d == '0') return true;  // leading zeros don't count
            if (++digits > 18) return false;
            mantissa = mantissa * 10 + (d - '0');
            return true;
        };

        if (Peek() == '0') {
            Take();
            if (!AtEnd() && Peek() >= '0' && Peek() <= '9') return Fail("numbers may not have leading zeros");
        } else {
            while (!AtEnd() && Peek() >= '0' && Peek() <= '9') {
                if (!addDigit(Take())) return Fail("number has too many digits");
            }
        }
        if (!AtEnd() && Peek() == '.') {
            Take();
            if (AtEnd() || Peek() < '0' || Peek() > '9') return Fail("a digit is required after the decimal point");
            while (!AtEnd() && Peek() >= '0' && Peek() <= '9') {
                if (!addDigit(Take())) return Fail("number has too many digits");
                ++scale;
            }
        }
        if (!AtEnd() && (Peek() == 'e' || Peek() == 'E')) {
            Take();
            bool expNegative = false;
            if (!AtEnd() && (Peek() == '+' || Peek() == '-')) expNegative = Take() == '-';
            if (AtEnd() || Peek() < '0' || Peek() > '9') return Fail("a digit is required in the exponent");
            int exponent = 0;
            while (!AtEnd() && Peek() >= '0' && Peek() <= '9') {
                exponent = exponent * 10 + (Take() - '0');
                if (exponent > 30) return Fail("exponent is too large");
            }
            scale += expNegative ? exponent : -exponent;
        }
        // A negative scale means "times a power of ten": fold it into the mantissa.
        while (scale < 0) {
            if (mantissa > kMaxMantissa / 10) return Fail("number is too large");
            mantissa *= 10;
            ++scale;
        }
        while (scale > 0 && mantissa % 10 == 0 && mantissa != 0) {  // normalise 1.50 -> 1.5, 2.0 -> 2
            mantissa /= 10;
            --scale;
        }
        if (mantissa == 0) scale = 0;
        out.mantissa_ = negative ? -mantissa : mantissa;
        out.scale_ = scale;
        return true;
    }

    std::string_view text_;
    std::string* error_;
    std::size_t pos_ = 0;
    int line_ = 1;
    int column_ = 1;
};

bool Parse(std::string_view text, Value& out, std::string* error) {
    Value value;
    Parser parser(text, error);
    if (!parser.ParseDocument(value)) return false;
    out = std::move(value);
    return true;
}

}  // namespace w2f::json
