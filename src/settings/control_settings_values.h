#pragma once

#include "control_settings.h"
#include "settings_ipc_values.h"
#include <charconv>
#include <limits>

namespace snowdesktop::control_settings
{
// These templates are instantiated only by fixed, typed settings adapters.
// The explicit field bindings are shared with the internal value transport;
// no caller can select a C++ type, private RPC name, tuple or binary payload.
namespace values_detail
{
using namespace control;
inline std::string Utf8(const std::wstring& text)
{
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) throw Error("invalid_text", "Settings contain invalid UTF-16");
    std::string result(static_cast<std::size_t>(count), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), count, nullptr, nullptr))
        throw Error("invalid_text", "Settings text conversion failed");
    return result;
}
inline std::wstring Wide(const std::string& text)
{
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) throw Error("invalid_params", "Settings text must be valid UTF-8", -32602);
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), count))
        throw Error("invalid_params", "Settings text conversion failed", -32602);
    return result;
}
template<class T> struct IsArray : std::false_type {};
template<class T, std::size_t N> struct IsArray<std::array<T, N>> : std::true_type {};
template<class T> inline constexpr bool Sequence = settings_ipc::IsVector<T>::value || IsArray<T>::value;
template<class T> inline constexpr bool TextArray = std::is_array_v<T> &&
    (std::is_same_v<std::remove_extent_t<T>, char> || std::is_same_v<std::remove_extent_t<T>, wchar_t>);
inline void Invalid(const std::string& path, const char* message)
{ throw Error("invalid_params", path + ": " + message, -32602); }
inline std::vector<std::string> Names(std::string_view source)
{
    std::vector<std::string> names;
    while (!source.empty())
    {
        const auto comma = source.find(',');
        auto name = source.substr(0, comma);
        const auto first = name.find_first_not_of(" \t\r\n");
        const auto last = name.find_last_not_of(" \t\r\n");
        if (first == std::string_view::npos || name.substr(first, 2) != "v.")
            throw Error("settings_schema_error", "Invalid explicit settings field binding");
        name = name.substr(first + 2, last - first - 1);
        names.emplace_back(name);
        if (comma == std::string_view::npos) break;
        source.remove_prefix(comma + 1);
    }
    return names;
}
template<class T, class Visit> void Members(T& value, Visit visit)
{
    using Type = std::remove_cv_t<T>;
    const auto names = Names(settings_ipc::Fields<Type>::Names);
    auto fields = settings_ipc::Fields<Type>::Tie(value);
    if (names.size() != std::tuple_size_v<decltype(fields)>)
        throw Error("settings_schema_error", "Settings field names and bindings differ");
    std::size_t index = 0;
    std::apply([&](auto&... field) { (visit(names[index++], field), ...); }, fields);
}
template<class T> std::vector<int> EnumChoices()
{
    if constexpr (std::is_same_v<T, DockPosition> || std::is_same_v<T, QuickNavigationDesktopViewMode>) return {0, 1, 2, 3};
    else if constexpr (std::is_same_v<T, DockMonitorScope> || std::is_same_v<T, DockFullscreenPolicy> ||
        std::is_same_v<T, shell_extensions::Placement> || std::is_same_v<T, shell_extensions::Visibility>) return {0, 1, 2};
    else if constexpr (std::is_same_v<T, SystemTaskbarThemeMode>) return {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    else if constexpr (std::is_same_v<T, shell_extensions::Context>) return {-1, 0, 1, 2, 3};
    else if constexpr (std::is_same_v<T, shell_extensions::Category>) return {0, 1};
    else if constexpr (std::is_same_v<T, IconBeautifyShape>) return {0, 1, 2, 3, 10};
    else if constexpr (std::is_same_v<T, IconBeautifyPreset>) return {0, 1, 5, 6, 7, 8};
    else return {}; // New writable enum types require an explicit contract.
}
template<class T> Value Encode(const T& value);
template<class T> void Patch(T& value, const Value& patch, const std::string& path, unsigned depth);
template<class T> Value Describe(const T& defaults);
template<class T> std::string MapKey(const T& key)
{
    if constexpr (std::is_same_v<T, std::string>) return key;
    else if constexpr (std::is_same_v<T, std::wstring>) return Utf8(key);
    else if constexpr (std::is_enum_v<T>) return std::to_string(static_cast<std::underlying_type_t<T>>(key));
    else return std::to_string(key);
}
template<class T> T ParseMapKey(const std::string& key, const std::string& path)
{
    if constexpr (std::is_same_v<T, std::string>) return key;
    else if constexpr (std::is_same_v<T, std::wstring>) return Wide(key);
    else
    {
        T result{};
        if constexpr (std::is_enum_v<T>)
        {
            std::underlying_type_t<T> number{};
            const auto parsed = std::from_chars(key.data(), key.data() + key.size(), number);
            if (parsed.ec != std::errc{} || parsed.ptr != key.data() + key.size()) Invalid(path, "invalid map key");
            result = static_cast<T>(number);
        }
        else
        {
            const auto parsed = std::from_chars(key.data(), key.data() + key.size(), result);
            if (parsed.ec != std::errc{} || parsed.ptr != key.data() + key.size()) Invalid(path, "invalid map key");
        }
        return result;
    }
}
template<class T> Value Encode(const T& value)
{
    if constexpr (std::is_same_v<T, bool>) return Boolean(value);
    else if constexpr (std::is_integral_v<T> && sizeof(T) >= 8) return String(std::to_string(value));
    else if constexpr (std::is_arithmetic_v<T>) return Number(static_cast<double>(value));
    else if constexpr (std::is_enum_v<T>) return Number(static_cast<double>(static_cast<std::underlying_type_t<T>>(value)));
    else if constexpr (std::is_same_v<T, std::string>) return String(value);
    else if constexpr (std::is_same_v<T, std::wstring>) return String(Utf8(value));
    else if constexpr (std::is_same_v<T, std::filesystem::path>) return String(Utf8(value.wstring()));
    else if constexpr (TextArray<T>)
    {
        const auto end = std::find(std::begin(value), std::end(value), std::remove_extent_t<T>{});
        if constexpr (std::is_same_v<std::remove_extent_t<T>, char>) return String(std::string(std::begin(value), end));
        else return String(Utf8(std::wstring(std::begin(value), end)));
    }
    else if constexpr (settings_ipc::IsOptional<T>::value || settings_ipc::IsShared<T>::value)
        return value ? Encode(*value) : Value{};
    else if constexpr (Sequence<T>)
    {
        auto result = Array();
        for (const auto& element : value) result.array.push_back(Encode(static_cast<typename T::value_type>(element)));
        return result;
    }
    else if constexpr (std::is_array_v<T>)
    { auto result = Array(); for (const auto& element : value) result.array.push_back(Encode(element)); return result; }
    else if constexpr (settings_ipc::IsMap<T>::value)
    {
        auto result = Object();
        for (const auto& [key, element] : value) result.object.emplace(MapKey(key), Encode(element));
        return result;
    }
    else
    {
        auto result = Object();
        Members(value, [&](const std::string& key, const auto& field) { result.object.emplace(key, Encode(field)); });
        return result;
    }
}
template<class T> void Patch(T& value, const Value& patch, const std::string& path, unsigned depth)
{
    if (depth > 32) Invalid(path, "settings nesting exceeds limit");
    if constexpr (std::is_same_v<T, bool>)
    { if (!patch.IsBoolean()) Invalid(path, "expected boolean"); value = patch.boolean; }
    else if constexpr (std::is_integral_v<T> && sizeof(T) >= 8)
    {
        if (!patch.IsString() || patch.string.empty()) Invalid(path, "expected decimal integer string");
        T number{};
        const auto parsed = std::from_chars(patch.string.data(), patch.string.data() + patch.string.size(), number);
        if (parsed.ec != std::errc{} || parsed.ptr != patch.string.data() + patch.string.size()) Invalid(path, "integer string is out of range");
        value = number;
    }
    else if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>)
    {
        if (!patch.IsNumber() || !std::isfinite(patch.number)) Invalid(path, "expected finite number");
        if constexpr (std::is_enum_v<T>)
        {
            const auto choices = EnumChoices<T>();
            if (choices.empty()) throw Error("settings_schema_error", "Settings enum has no explicit public value list");
            if (std::floor(patch.number) != patch.number ||
                std::find(choices.begin(), choices.end(), patch.number) == choices.end())
                Invalid(path, "unsupported enumeration value");
        }
        else
        {
            if (patch.number < static_cast<double>((std::numeric_limits<T>::lowest)()) ||
                patch.number > static_cast<double>((std::numeric_limits<T>::max)())) Invalid(path, "number is out of range");
            if constexpr (std::is_integral_v<T>) if (std::floor(patch.number) != patch.number) Invalid(path, "expected integer");
        }
        value = static_cast<T>(patch.number);
    }
    else if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, std::wstring> ||
        std::is_same_v<T, std::filesystem::path> || TextArray<T>)
    {
        if (!patch.IsString() || patch.string.size() > 32768 || patch.string.find('\0') != std::string::npos)
            Invalid(path, "expected bounded text without NUL");
        if constexpr (std::is_same_v<T, std::string>) value = patch.string;
        else if constexpr (std::is_same_v<T, std::wstring>) value = Wide(patch.string);
        else if constexpr (std::is_same_v<T, std::filesystem::path>) value = Wide(patch.string);
        else
        {
            const auto text = [&] {
                if constexpr (std::is_same_v<std::remove_extent_t<T>, char>) return patch.string;
                else return Wide(patch.string);
            }();
            if (text.size() >= std::extent_v<T>) Invalid(path, "text exceeds field capacity");
            std::fill(std::begin(value), std::end(value), std::remove_extent_t<T>{});
            std::copy(text.begin(), text.end(), std::begin(value));
        }
    }
    else if constexpr (settings_ipc::IsOptional<T>::value)
    {
        if (patch.IsNull()) value.reset();
        else { if (!value) value.emplace(); Patch(*value, patch, path, depth + 1); }
    }
    else if constexpr (Sequence<T> || std::is_array_v<T>)
    {
        if (!patch.IsArray() || patch.array.size() > 16384) Invalid(path, "expected bounded array");
        if constexpr (settings_ipc::IsVector<T>::value)
        {
            T result;
            for (std::size_t index = 0; index < patch.array.size(); ++index)
            {
                typename T::value_type element{};
                Patch(element, patch.array[index], path + "[" + std::to_string(index) + "]", depth + 1);
                result.push_back(std::move(element));
            }
            value = std::move(result);
        }
        else
        {
            if (patch.array.size() != std::size(value)) Invalid(path, "array length must match the field");
            for (std::size_t index = 0; index < patch.array.size(); ++index)
                Patch(value[index], patch.array[index], path + "[" + std::to_string(index) + "]", depth + 1);
        }
    }
    else if constexpr (settings_ipc::IsMap<T>::value)
    {
        if (!patch.IsObject() || patch.object.size() > 16384) Invalid(path, "expected bounded map");
        T result;
        for (const auto& [key, item] : patch.object)
        {
            if (key.size() > 4096 || key.find('\0') != std::string::npos) Invalid(path, "invalid map identity");
            typename T::mapped_type element{};
            Patch(element, item, path + "." + key, depth + 1);
            result.emplace(ParseMapKey<typename T::key_type>(key, path), std::move(element));
        }
        value = std::move(result);
    }
    else
    {
        if (!patch.IsObject()) Invalid(path, "expected named object");
        std::size_t matched = 0;
        Members(value, [&](const std::string& key, auto& field) {
            if (const auto* input = patch.Find(key)) { ++matched; Patch(field, *input, path.empty() ? key : path + "." + key, depth + 1); }
        });
        if (matched != patch.object.size()) Invalid(path, "unknown settings field");
    }
}
template<class T> Value Describe(const T& defaults)
{
    auto result = Object({{"default", Encode(defaults)}, {"readOnly", Boolean(false)}});
    const auto type = [&](const char* name) { result.object.emplace("type", String(name)); };
    if constexpr (std::is_same_v<T, bool>) type("boolean");
    else if constexpr (std::is_enum_v<T>)
    {
        type("integer"); auto choices = Array(); const auto allowed = EnumChoices<T>();
        for (int number : allowed) choices.array.push_back(Number(number));
        result.object.emplace("enum", std::move(choices));
        if (allowed.empty()) result.object["readOnly"] = Boolean(true);
    }
    else if constexpr (std::is_integral_v<T> && sizeof(T) >= 8)
    { type("string"); result.object.emplace("format", String(std::is_unsigned_v<T> ? "uint64" : "int64")); }
    else if constexpr (std::is_arithmetic_v<T>)
    {
        type(std::is_integral_v<T> ? "integer" : "number");
        result.object.emplace("minimum", Number(static_cast<double>((std::numeric_limits<T>::lowest)())));
        result.object.emplace("maximum", Number(static_cast<double>((std::numeric_limits<T>::max)())));
    }
    else if constexpr (settings_ipc::IsString<T>::value || std::is_same_v<T, std::filesystem::path> || TextArray<T>)
    { type("string"); result.object.emplace("maxLength", Number(static_cast<double>(TextArray<T> ? std::extent_v<T> - 1 : 32768))); }
    else if constexpr (settings_ipc::IsOptional<T>::value)
    {
        result = Describe(typename T::value_type{});
        result.object["default"] = Encode(defaults); result.object.emplace("nullable", Boolean(true));
    }
    else if constexpr (Sequence<T>)
    {
        type("array"); result.object.emplace("items", Describe(typename T::value_type{}));
        result.object.emplace("maxItems", Number(static_cast<double>(settings_ipc::IsVector<T>::value ? 16384 : std::size(defaults))));
        if constexpr (IsArray<T>::value) result.object.emplace("minItems", Number(static_cast<double>(std::size(defaults))));
    }
    else if constexpr (std::is_array_v<T>)
    {
        type("array"); result.object.emplace("items", Describe(std::remove_extent_t<T>{}));
        result.object.emplace("minItems", Number(static_cast<double>(std::extent_v<T>)));
        result.object.emplace("maxItems", Number(static_cast<double>(std::extent_v<T>)));
    }
    else if constexpr (settings_ipc::IsMap<T>::value)
    { type("object"); result.object.emplace("additionalProperties", Describe(typename T::mapped_type{})); }
    else
    {
        type("object"); auto properties = Object();
        Members(defaults, [&](const std::string& key, const auto& field) { properties.object.emplace(key, Describe(field)); });
        result.object.emplace("properties", std::move(properties)); result.object.emplace("additionalProperties", Boolean(false));
    }
    return result;
}
}
template<class T> Value ToValue(const T& value) { return values_detail::Encode(value); }
template<class T> void PatchValue(T& value, const Value& patch)
{
    // Work on a copy so a malformed later field cannot partially modify a
    // caller's model. Adapters validate business constraints before committing.
    T candidate = value; values_detail::Patch(candidate, patch, {}, 0); value = std::move(candidate);
}
template<class T> Value DescribeValue(const T& defaults) { return values_detail::Describe(defaults); }
}
