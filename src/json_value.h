#pragma once

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

struct JsonValue
{
    enum class Type
    {
        Null,
        Boolean,
        Number,
        String,
        Array,
        Object,
    };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<JsonValue> array;
    std::unordered_map<std::string, JsonValue> object;

    bool IsNull() const { return type == Type::Null; }
    bool IsBoolean() const { return type == Type::Boolean; }
    bool IsNumber() const { return type == Type::Number; }
    bool IsString() const { return type == Type::String; }
    bool IsArray() const { return type == Type::Array; }
    bool IsObject() const { return type == Type::Object; }

    const JsonValue* Find(std::string_view key) const
    {
        if (!IsObject())
            return nullptr;
        auto found = object.find(std::string(key));
        return found == object.end() ? nullptr : &found->second;
    }
};

namespace json_detail
{
    inline void AppendUtf8(std::string& output, unsigned codePoint)
    {
        if (codePoint <= 0x7F)
        {
            output.push_back(static_cast<char>(codePoint));
        }
        else if (codePoint <= 0x7FF)
        {
            output.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
        else if (codePoint <= 0xFFFF)
        {
            output.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
            output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
        else
        {
            output.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
            output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
    }

    class Parser
    {
    public:
        Parser(std::string_view text, std::string* error)
            : text_(text), error_(error)
        {
            if (text_.size() >= 3 &&
                static_cast<unsigned char>(text_[0]) == 0xEF &&
                static_cast<unsigned char>(text_[1]) == 0xBB &&
                static_cast<unsigned char>(text_[2]) == 0xBF)
            {
                position_ = 3;
            }
        }

        bool Parse(JsonValue& output)
        {
            SkipWhitespace();
            if (!ParseValue(output))
                return false;
            SkipWhitespace();
            if (position_ != text_.size())
                return Fail("unexpected trailing characters");
            return true;
        }

    private:
        bool ParseValue(JsonValue& output)
        {
            SkipWhitespace();
            if (position_ >= text_.size())
                return Fail("unexpected end of input");

            switch (text_[position_])
            {
            case '{': return ParseObject(output);
            case '[': return ParseArray(output);
            case '"':
                output = {};
                output.type = JsonValue::Type::String;
                return ParseString(output.string);
            case 't': return ParseLiteral("true", JsonValue::Type::Boolean, output, true);
            case 'f': return ParseLiteral("false", JsonValue::Type::Boolean, output, false);
            case 'n': return ParseLiteral("null", JsonValue::Type::Null, output, false);
            default:
                if (text_[position_] == '-' ||
                    std::isdigit(static_cast<unsigned char>(text_[position_])))
                {
                    return ParseNumber(output);
                }
                return Fail("unexpected token");
            }
        }

        bool ParseObject(JsonValue& output)
        {
            ++position_;
            output = {};
            output.type = JsonValue::Type::Object;
            SkipWhitespace();
            if (Consume('}'))
                return true;

            while (position_ < text_.size())
            {
                std::string key;
                if (!ParseString(key))
                    return false;
                SkipWhitespace();
                if (!Consume(':'))
                    return Fail("expected ':' after object key");

                JsonValue value;
                if (!ParseValue(value))
                    return false;
                if (!output.object.emplace(std::move(key), std::move(value)).second)
                    return Fail("duplicate object key");

                SkipWhitespace();
                if (Consume('}'))
                    return true;
                if (!Consume(','))
                    return Fail("expected ',' or '}' in object");
                SkipWhitespace();
            }
            return Fail("unterminated object");
        }

        bool ParseArray(JsonValue& output)
        {
            ++position_;
            output = {};
            output.type = JsonValue::Type::Array;
            SkipWhitespace();
            if (Consume(']'))
                return true;

            while (position_ < text_.size())
            {
                JsonValue value;
                if (!ParseValue(value))
                    return false;
                output.array.push_back(std::move(value));

                SkipWhitespace();
                if (Consume(']'))
                    return true;
                if (!Consume(','))
                    return Fail("expected ',' or ']' in array");
                SkipWhitespace();
            }
            return Fail("unterminated array");
        }

        bool ParseString(std::string& output)
        {
            if (!Consume('"'))
                return Fail("expected string");
            output.clear();

            while (position_ < text_.size())
            {
                unsigned char ch = static_cast<unsigned char>(text_[position_++]);
                if (ch == '"')
                    return true;
                if (ch < 0x20)
                    return Fail("control character in string");
                if (ch != '\\')
                {
                    output.push_back(static_cast<char>(ch));
                    continue;
                }

                if (position_ >= text_.size())
                    return Fail("unterminated escape sequence");
                char escaped = text_[position_++];
                switch (escaped)
                {
                case '"': output.push_back('"'); break;
                case '\\': output.push_back('\\'); break;
                case '/': output.push_back('/'); break;
                case 'b': output.push_back('\b'); break;
                case 'f': output.push_back('\f'); break;
                case 'n': output.push_back('\n'); break;
                case 'r': output.push_back('\r'); break;
                case 't': output.push_back('\t'); break;
                case 'u':
                {
                    unsigned codePoint = 0;
                    if (!ParseHex4(codePoint))
                        return false;
                    if (codePoint >= 0xD800 && codePoint <= 0xDBFF)
                    {
                        if (position_ + 2 > text_.size() ||
                            text_[position_] != '\\' || text_[position_ + 1] != 'u')
                        {
                            return Fail("missing low surrogate");
                        }
                        position_ += 2;
                        unsigned low = 0;
                        if (!ParseHex4(low))
                            return false;
                        if (low < 0xDC00 || low > 0xDFFF)
                            return Fail("invalid low surrogate");
                        codePoint = 0x10000 +
                            ((codePoint - 0xD800) << 10) + (low - 0xDC00);
                    }
                    else if (codePoint >= 0xDC00 && codePoint <= 0xDFFF)
                    {
                        return Fail("unexpected low surrogate");
                    }
                    AppendUtf8(output, codePoint);
                    break;
                }
                default:
                    return Fail("invalid escape sequence");
                }
            }
            return Fail("unterminated string");
        }

        bool ParseHex4(unsigned& output)
        {
            if (position_ + 4 > text_.size())
                return Fail("incomplete unicode escape");
            output = 0;
            for (int i = 0; i < 4; ++i)
            {
                char ch = text_[position_++];
                output <<= 4;
                if (ch >= '0' && ch <= '9') output |= static_cast<unsigned>(ch - '0');
                else if (ch >= 'a' && ch <= 'f') output |= static_cast<unsigned>(ch - 'a' + 10);
                else if (ch >= 'A' && ch <= 'F') output |= static_cast<unsigned>(ch - 'A' + 10);
                else return Fail("invalid unicode escape");
            }
            return true;
        }

        bool ParseNumber(JsonValue& output)
        {
            const size_t start = position_;
            if (text_[position_] == '-')
                ++position_;
            if (position_ >= text_.size())
                return Fail("incomplete number");

            if (text_[position_] == '0')
            {
                ++position_;
            }
            else
            {
                if (!std::isdigit(static_cast<unsigned char>(text_[position_])))
                    return Fail("invalid number");
                while (position_ < text_.size() &&
                    std::isdigit(static_cast<unsigned char>(text_[position_])))
                {
                    ++position_;
                }
            }

            if (position_ < text_.size() && text_[position_] == '.')
            {
                ++position_;
                if (position_ >= text_.size() ||
                    !std::isdigit(static_cast<unsigned char>(text_[position_])))
                {
                    return Fail("invalid fractional number");
                }
                while (position_ < text_.size() &&
                    std::isdigit(static_cast<unsigned char>(text_[position_])))
                {
                    ++position_;
                }
            }

            if (position_ < text_.size() &&
                (text_[position_] == 'e' || text_[position_] == 'E'))
            {
                ++position_;
                if (position_ < text_.size() &&
                    (text_[position_] == '+' || text_[position_] == '-'))
                {
                    ++position_;
                }
                if (position_ >= text_.size() ||
                    !std::isdigit(static_cast<unsigned char>(text_[position_])))
                {
                    return Fail("invalid exponent");
                }
                while (position_ < text_.size() &&
                    std::isdigit(static_cast<unsigned char>(text_[position_])))
                {
                    ++position_;
                }
            }

            std::string token(text_.substr(start, position_ - start));
            char* end = nullptr;
            errno = 0;
            const double number = std::strtod(token.c_str(), &end);
            if (errno == ERANGE || !end || *end != '\0')
                return Fail("number out of range");
            output = {};
            output.type = JsonValue::Type::Number;
            output.number = number;
            return true;
        }

        bool ParseLiteral(std::string_view literal, JsonValue::Type type,
            JsonValue& output, bool boolean)
        {
            if (text_.substr(position_, literal.size()) != literal)
                return Fail("invalid literal");
            position_ += literal.size();
            output = {};
            output.type = type;
            output.boolean = boolean;
            return true;
        }

        void SkipWhitespace()
        {
            while (position_ < text_.size() &&
                std::isspace(static_cast<unsigned char>(text_[position_])))
            {
                ++position_;
            }
        }

        bool Consume(char expected)
        {
            if (position_ < text_.size() && text_[position_] == expected)
            {
                ++position_;
                return true;
            }
            return false;
        }

        bool Fail(const char* message)
        {
            if (error_)
            {
                *error_ = message;
                *error_ += " at byte ";
                *error_ += std::to_string(position_);
            }
            return false;
        }

        std::string_view text_;
        size_t position_ = 0;
        std::string* error_ = nullptr;
    };
}

inline bool ParseJson(std::string_view text, JsonValue& output,
    std::string* error = nullptr)
{
    if (error)
        error->clear();
    json_detail::Parser parser(text, error);
    return parser.Parse(output);
}
