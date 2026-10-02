#pragma once
#include "navigation_settings.h"
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::quick_navigation_query
{
inline std::wstring Trim(std::wstring_view text)
{
    while (!text.empty() && iswspace(text.front())) text.remove_prefix(1);
    while (!text.empty() && iswspace(text.back())) text.remove_suffix(1);
    return std::wstring(text);
}
inline std::string Prefix(std::wstring_view text)
{
    std::string result;
    for (wchar_t c : Trim(text))
    {
        if (c > 127) return {};
        result += static_cast<char>(towlower(c));
    }
    return result;
}
struct Scope { QuickNavigationSearchType type; std::string engine; };
struct PrefixCandidate { Scope scope; std::string prefix; };
inline std::vector<PrefixCandidate> PrefixCandidates(const NavigationSettings& settings, std::wstring_view input)
{
    const auto prefix = Prefix(input);
    if (prefix.empty()) return {};
    std::vector<PrefixCandidate> candidates;
    for (const auto type : kQuickNavigationSearchTypes)
        if (type != QuickNavigationSearchType::All)
        {
            const auto& value = settings.prefixes[static_cast<size_t>(type) - 1];
            if (value.starts_with(prefix)) candidates.push_back({{type, {}}, value});
        }
    for (const auto& engine : settings.engines)
        if (!engine.prefix.empty() && engine.prefix.starts_with(prefix))
            candidates.push_back({{QuickNavigationSearchType::Web, engine.id}, engine.prefix});
    // Exact matches stay first even when another custom prefix extends them.
    std::stable_partition(candidates.begin(), candidates.end(), [&](const auto& value) { return value.prefix == prefix; });
    return candidates;
}
inline std::optional<Scope> ResolvePrefix(const NavigationSettings& settings, std::wstring_view input)
{
    const auto prefix = Prefix(input);
    if (prefix.empty()) return {};
    for (const auto type : kQuickNavigationSearchTypes)
        if (type != QuickNavigationSearchType::All && settings.prefixes[static_cast<size_t>(type) - 1] == prefix)
            return Scope{type, {}};
    for (const auto& engine : settings.engines)
        if (engine.prefix == prefix) return Scope{QuickNavigationSearchType::Web, engine.id};
    return {};
}
enum class SubmitIntent { Composition, ConfirmPrefix, ActivateResult };
inline SubmitIntent GetSubmitIntent(const NavigationSettings& settings, QuickNavigationSearchType type, std::wstring_view text, bool composing, bool menuOpen)
{
    if (composing) return SubmitIntent::Composition;
    if (!menuOpen && type == QuickNavigationSearchType::All && ResolvePrefix(settings,text)) return SubmitIntent::ConfirmPrefix;
    return SubmitIntent::ActivateResult;
}
inline std::string EncodeQuery(std::wstring_view query)
{
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, query.data(),
        static_cast<int>(query.size()), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string utf8(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, query.data(), static_cast<int>(query.size()),
        utf8.data(), bytes, nullptr, nullptr);
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (unsigned char c : utf8)
    {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') result += static_cast<char>(c);
        else { result += '%'; result += hex[c >> 4]; result += hex[c & 15]; }
    }
    return result;
}
inline std::string SearchUrl(const QuickNavigationSearchEngine& engine, std::wstring_view query)
{
    std::string result = engine.url;
    const auto encoded = EncodeQuery(query);
    size_t position = 0;
    while ((position = result.find("{query}", position)) != std::string::npos)
    { result.replace(position, 7, encoded); position += encoded.size(); }
    return result;
}
struct Command { std::wstring target, parameters; };
inline std::optional<Command> ParseCommand(std::wstring_view text)
{
    auto command = Trim(text);
    const DWORD expandedLength = ExpandEnvironmentStringsW(command.c_str(), nullptr, 0);
    if (expandedLength && expandedLength <= 32768)
    {
        std::wstring expanded(expandedLength, L'\0');
        if (ExpandEnvironmentStringsW(command.c_str(), expanded.data(), expandedLength))
        { expanded.resize(expandedLength - 1); command = std::move(expanded); }
    }
    if (command.empty() || command.find(L'\0') != std::wstring::npos) return {};
    Command result;
    if (command.front() == L'"')
    {
        const auto end = command.find(L'"', 1);
        if (end == std::wstring::npos || end == 1 || (end + 1 < command.size() && !iswspace(command[end + 1]))) return {};
        result.target = command.substr(1, end - 1);
        result.parameters = Trim(std::wstring_view(command).substr(end + 1));
    }
    else
    {
        // A complete existing path may contain spaces without being quoted.
        const auto colon = command.find(L':');
        bool uri = colon != std::wstring::npos && colon > 1 && iswalpha(command.front());
        for (size_t i = 1; uri && i < colon; ++i)
            uri = iswalnum(command[i]) || command[i] == L'+' || command[i] == L'-' || command[i] == L'.';
        if (GetFileAttributesW(command.c_str()) != INVALID_FILE_ATTRIBUTES || uri)
            result.target = command;
        else
        {
            const auto end = command.find_first_of(L" \t");
            result.target = command.substr(0, end);
            if (end != std::wstring::npos) result.parameters = Trim(std::wstring_view(command).substr(end));
        }
    }
    DWORD length = ExpandEnvironmentStringsW(result.target.c_str(), nullptr, 0);
    if (length && length <= 32768)
    {
        std::wstring expanded(length, L'\0');
        ExpandEnvironmentStringsW(result.target.c_str(), expanded.data(), length);
        expanded.resize(length - 1); result.target = std::move(expanded);
    }
    return result.target.empty() ? std::nullopt : std::optional<Command>(std::move(result));
}

enum class CalculationError { None, Invalid, DivisionByZero, NonFinite };
struct Calculation { double value = 0; CalculationError error = CalculationError::None; };
class Calculator
{
public:
    Calculation Evaluate(std::wstring_view text)
    {
        input_ = text; position_ = 0; depth_ = 0; error_ = CalculationError::None;
        if (text.size() > 4096 || Trim(text).empty()) return {0, CalculationError::Invalid};
        const double result = Sum(); Skip();
        if (position_ != input_.size()) Fail(CalculationError::Invalid);
        if (!std::isfinite(result)) Fail(CalculationError::NonFinite);
        return {result, error_};
    }
private:
    std::wstring_view input_; size_t position_ = 0; unsigned depth_ = 0;
    CalculationError error_ = CalculationError::None;
    void Fail(CalculationError error) { if (error_ == CalculationError::None) error_ = error; }
    void Skip() { while (position_ < input_.size() && iswspace(input_[position_])) ++position_; }
    bool Take(wchar_t c) { Skip(); if (position_ < input_.size() && input_[position_] == c) { ++position_; return true; } return false; }
    double Sum()
    {
        double value = Product();
        while (error_ == CalculationError::None)
        { if (Take(L'+')) value += Product(); else if (Take(L'-')) value -= Product(); else break; }
        return value;
    }
    double Product()
    {
        double value = Unary();
        while (error_ == CalculationError::None)
        {
            if (Take(L'*') || Take(L'×')) value *= Unary();
            else if (Take(L'/') || Take(L'÷')) { const auto divisor = Unary(); if (divisor == 0) Fail(CalculationError::DivisionByZero); else value /= divisor; }
            else break;
        }
        return value;
    }
    double Unary()
    {
        if (++depth_ > 128) { Fail(CalculationError::Invalid); --depth_; return 0; }
        double value;
        if (Take(L'+')) value = Unary(); else if (Take(L'-')) value = -Unary(); else value = Power();
        --depth_; return value;
    }
    double Power()
    {
        double value = Atom();
        while (Take(L'%')) value /= 100;
        if (Take(L'^')) value = std::pow(value, Unary());
        return value;
    }
    double Atom()
    {
        if (Take(L'(')) { const auto value = Sum(); if (!Take(L')')) Fail(CalculationError::Invalid); return value; }
        Skip(); const auto start = position_; bool digit = false, dot = false;
        while (position_ < input_.size())
        {
            const auto c = input_[position_];
            if (c >= L'0' && c <= L'9') { digit = true; ++position_; }
            else if (c == L'.' && !dot) { dot = true; ++position_; }
            else break;
        }
        if (!digit) { Fail(CalculationError::Invalid); return 0; }
        const std::wstring number(input_.substr(start, position_ - start));
        std::wistringstream stream(number); stream.imbue(std::locale::classic());
        double value = 0; if (!(stream >> value)) Fail(CalculationError::NonFinite); return value;
    }
};
inline std::wstring FormatCalculation(double value)
{
    std::wostringstream stream; stream.imbue(std::locale::classic());
    stream << std::setprecision(15) << (value == 0 ? 0 : value); return stream.str();
}
}
