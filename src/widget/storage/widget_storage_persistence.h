#pragma once

#include "widget/preview/widget_preview_context.h"
#include "common/json_value.h"
#include <windows.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <vector>

namespace snowdesktop::widget_runtime::storage_persistence
{
inline std::string EscapeStorageJson(std::string_view value)
{
    std::string result;
    result.reserve(value.size() + 8);
    static constexpr char hex[] = "0123456789abcdef";
    for (unsigned char ch : value)
    {
        switch (ch)
        {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (ch < 0x20)
            {
                result += "\\u00";
                result.push_back(hex[(ch >> 4) & 0x0f]);
                result.push_back(hex[ch & 0x0f]);
            }
            else result.push_back(static_cast<char>(ch));
            break;
        }
    }
    return result;
}

template<class Exclude>
inline bool ParseLegacyStorageText(const std::string& text,
    StorageMap& output, Exclude exclude)
{
    // Releases before package format v1 wrote string values without JSON
    // escaping. Recover the same key/value pairs that the former reader
    // accepted, then immediately rewrite them through the safe serializer.
    size_t pos = text.find('{');
    if (pos == std::string::npos) return false;
    while (true)
    {
        pos = text.find('"', pos);
        if (pos == std::string::npos) break;
        const size_t keyEnd = text.find('"', pos + 1);
        if (keyEnd == std::string::npos) break;
        const std::string key = text.substr(pos + 1, keyEnd - pos - 1);
        pos = text.find(':', keyEnd + 1);
        if (pos == std::string::npos) break;
        pos = text.find('"', pos + 1);
        if (pos == std::string::npos) break;
        const size_t valueEnd = text.find('"', pos + 1);
        if (valueEnd == std::string::npos) break;
        if (!key.empty() && !exclude(key))
            output[key] = text.substr(pos + 1, valueEnd - pos - 1);
        pos = valueEnd + 1;
    }
    return !output.empty();
}

template<class Exclude>
inline bool ParseStorageText(const std::string& text,
    StorageMap& output, Exclude exclude)
{
    JsonValue root;
    std::string parseError;
    if (ParseJson(text, root, &parseError) && root.IsObject())
    {
        for (const auto& [key, value] : root.object)
            if (value.IsString() && !exclude(key))
                output.emplace(key, value.string);
        return true;
    }
    return ParseLegacyStorageText(text, output, exclude);
}

template<class Exclude>
inline bool ReadStorageMap(const std::filesystem::path& path,
    StorageMap& output, Exclude exclude)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::ostringstream stream;
    stream << file.rdbuf();
    return ParseStorageText(stream.str(), output, exclude);
}

inline bool SaveStorageMap(const std::filesystem::path& path, const StorageMap& storage)
{
    std::vector<std::pair<std::string, std::string>> values(
        storage.begin(), storage.end());
    std::sort(values.begin(), values.end(),
        [](const auto& left, const auto& right) {
            return left.first < right.first;
        });
    std::ostringstream output;
    output << "{";
    for (std::size_t index = 0; index < values.size(); ++index)
    {
        if (index) output << ',';
        output << "\n  \"" << EscapeStorageJson(values[index].first)
            << "\": \"" << EscapeStorageJson(values[index].second) << '"';
    }
    if (!values.empty()) output << '\n';
    output << "}\n";

    const std::wstring temporary = path.wstring() + L".tmp";
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    const std::string text = output.str();
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    file.flush();
    if (!file) return false;
    file.close();
    return MoveFileExW(temporary.c_str(), path.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}
}
