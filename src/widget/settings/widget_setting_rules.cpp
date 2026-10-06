#include "widget_setting_rules.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <optional>
#include <unordered_set>

namespace snowdesktop::widget_runtime
{
namespace
{
bool IsAsciiDigit(char value) noexcept
{
    return value >= '0' && value <= '9';
}

int ReadTwoDigits(std::string_view value, std::size_t offset) noexcept
{
    if (offset + 1 >= value.size() ||
        !IsAsciiDigit(value[offset]) || !IsAsciiDigit(value[offset + 1]))
        return -1;
    return (value[offset] - '0') * 10 + (value[offset + 1] - '0');
}
}

bool IsValidUrlSettingValue(std::string_view value) noexcept
{
    if (value.empty()) return true;
    if (value.size() > 2048) return false;
    const bool http = value.starts_with("http://");
    const bool https = value.starts_with("https://");
    if (!http && !https) return false;
    const std::size_t authorityStart = http ? 7 : 8;
    if (authorityStart >= value.size()) return false;
    const std::size_t authorityEnd = value.find_first_of("/?#", authorityStart);
    if (authorityEnd == authorityStart) return false;
    for (const unsigned char byte : value)
    {
        if (byte <= 0x20 || byte == 0x7f || byte == '\\') return false;
    }
    return true;
}

bool IsValidDateSettingValue(std::string_view value) noexcept
{
    if (value.empty()) return true;
    if (value.size() != 10 || value[4] != '-' || value[7] != '-')
        return false;
    for (std::size_t index = 0; index < value.size(); ++index)
    {
        if (index == 4 || index == 7) continue;
        if (!IsAsciiDigit(value[index])) return false;
    }
    int year = 0;
    for (std::size_t index = 0; index < 4; ++index)
        year = year * 10 + (value[index] - '0');
    const int month = ReadTwoDigits(value, 5);
    const int day = ReadTwoDigits(value, 8);
    if (year < 1 || month < 1 || month > 12 || day < 1) return false;
    constexpr int daysPerMonth[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int maximumDay = daysPerMonth[month - 1];
    const bool leap = year % 4 == 0 &&
        (year % 100 != 0 || year % 400 == 0);
    if (month == 2 && leap) maximumDay = 29;
    return day <= maximumDay;
}

bool IsValidTimeSettingValue(std::string_view value) noexcept
{
    if (value.empty()) return true;
    if (value.size() != 5 || value[2] != ':') return false;
    const int hour = ReadTwoDigits(value, 0);
    const int minute = ReadTwoDigits(value, 3);
    return hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59;
}

bool ParseFiniteSettingNumber(std::string_view value,
    double& output) noexcept
{
    if (value.empty()) return false;
    const char* begin = value.data();
    const char* end = begin + value.size();
    const auto result = std::from_chars(begin, end, output);
    return result.ec == std::errc{} && result.ptr == end &&
        std::isfinite(output);
}

double SnapRangeSettingValue(double value, double minimum,
    double maximum, double step) noexcept
{
    if (!std::isfinite(minimum) || !std::isfinite(maximum) ||
        !std::isfinite(step) || step <= 0.0 || minimum > maximum)
        return minimum;
    if (!std::isfinite(value)) value = minimum;
    value = std::clamp(value, minimum, maximum);
    const double steps = std::round((value - minimum) / step);
    return std::clamp(minimum + steps * step, minimum, maximum);
}

bool IsValidMultiSelectSettingValue(
    const std::vector<std::string>& options,
    const std::vector<std::string>& values) noexcept
{
    if (options.empty() || options.size() > 64 || values.size() > 64)
        return false;
    std::unordered_set<std::string_view> allowed;
    allowed.reserve(options.size());
    for (const auto& option : options)
    {
        if (option.empty() || !allowed.emplace(option).second) return false;
    }
    std::unordered_set<std::string_view> selected;
    selected.reserve(values.size());
    for (const auto& value : values)
    {
        if (!allowed.contains(value) || !selected.emplace(value).second)
            return false;
    }
    return true;
}

bool IsValidFilesystemSettingAccess(std::string_view value) noexcept
{
    return value == "read" || value == "write" || value == "readWrite";
}

bool NormalizeFilesystemSettingExtensions(
    const std::vector<std::string>& input,
    std::vector<std::string>& output) noexcept
{
    output.clear();
    if (input.size() > 16) return false;
    for (std::string extension : input)
    {
        if (!extension.empty() && extension.front() == '.')
            extension.erase(extension.begin());
        const bool valid = !extension.empty() && extension.size() <= 32 &&
            extension.front() != '.' && extension.back() != '.' &&
            extension.find("..") == std::string::npos &&
            std::all_of(extension.begin(), extension.end(),
                [](const unsigned char character) {
                    return std::isalnum(character) || character == '.';
                });
        if (!valid)
        {
            output.clear();
            return false;
        }
        std::transform(extension.begin(), extension.end(),
            extension.begin(), [](const unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
        if (std::find(output.begin(), output.end(), extension) == output.end())
            output.push_back(std::move(extension));
    }
    return true;
}

bool IsValidSettingGroupId(std::string_view value) noexcept
{
    return !value.empty() && value.size() <= 64 &&
        std::all_of(value.begin(), value.end(),
            [](const unsigned char character) {
                return std::isalnum(character) || character == '_' ||
                    character == '-' || character == '.';
            });
}

bool IsValidSettingCondition(std::string_view operation,
    std::size_t valueCount) noexcept
{
    if (operation == "set" || operation == "unset" ||
        operation == "truthy" || operation == "falsy")
        return valueCount == 0;
    if (operation == "equals" || operation == "notEquals" ||
        operation == "contains" || operation == "notContains")
        return valueCount == 1;
    if (operation == "oneOf" || operation == "notOneOf")
        return valueCount >= 1 && valueCount <= 64;
    return false;
}

bool EvaluateSettingCondition(std::string_view operation,
    const std::vector<std::string>& current,
    const std::vector<std::string>& expected) noexcept
{
    const bool set = std::any_of(current.begin(), current.end(),
        [](const std::string& value) { return !value.empty(); });
    const auto truthyValue = [](std::string value) {
        std::transform(value.begin(), value.end(), value.begin(),
            [](const unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
        return !value.empty() && value != "0" && value != "false" &&
            value != "nil" && value != "null" && value != "off";
    };
    const bool truthy = current.size() > 1
        ? !current.empty()
        : (!current.empty() && truthyValue(current.front()));
    if (operation == "set") return set;
    if (operation == "unset") return !set;
    if (operation == "truthy") return truthy;
    if (operation == "falsy") return !truthy;
    if (expected.empty()) return false;
    if (operation == "equals" || operation == "notEquals")
    {
        const bool equals = current.size() == 1 &&
            current.front() == expected.front();
        return operation == "equals" ? equals : !equals;
    }
    if (operation == "contains" || operation == "notContains")
    {
        const bool contains = std::find(current.begin(), current.end(),
            expected.front()) != current.end();
        return operation == "contains" ? contains : !contains;
    }
    const bool oneOf = current.size() == 1 &&
        std::find(expected.begin(), expected.end(), current.front()) !=
            expected.end();
    if (operation == "oneOf") return oneOf;
    if (operation == "notOneOf") return !oneOf;
    return false;
}

bool ValidateSettingTextValue(std::string_view value, bool required,
    int minimumLength, int maximumLength) noexcept
{
    if (value.empty()) return !required;
    std::size_t length = 0;
    for (std::size_t index = 0; index < value.size();)
    {
        const unsigned char lead =
            static_cast<unsigned char>(value[index]);
        std::size_t width = 0;
        if (lead <= 0x7f) width = 1;
        else if (lead >= 0xc2 && lead <= 0xdf) width = 2;
        else if (lead >= 0xe0 && lead <= 0xef) width = 3;
        else if (lead >= 0xf0 && lead <= 0xf4) width = 4;
        else return false;
        if (index + width > value.size()) return false;
        for (std::size_t offset = 1; offset < width; ++offset)
        {
            const unsigned char continuation =
                static_cast<unsigned char>(value[index + offset]);
            if ((continuation & 0xc0) != 0x80) return false;
        }
        if (width == 3)
        {
            const unsigned char second =
                static_cast<unsigned char>(value[index + 1]);
            if ((lead == 0xe0 && second < 0xa0) ||
                (lead == 0xed && second >= 0xa0)) return false;
        }
        if (width == 4)
        {
            const unsigned char second =
                static_cast<unsigned char>(value[index + 1]);
            if ((lead == 0xf0 && second < 0x90) ||
                (lead == 0xf4 && second >= 0x90)) return false;
        }
        index += width;
        ++length;
    }
    if (minimumLength >= 0 &&
        length < static_cast<std::size_t>(minimumLength)) return false;
    if (maximumLength >= 0 &&
        length > static_cast<std::size_t>(maximumLength)) return false;
    return true;
}
}
