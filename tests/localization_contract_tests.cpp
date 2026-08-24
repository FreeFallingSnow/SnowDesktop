#include "json_value.h"
#include "l10n.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
namespace fs = std::filesystem;

using Catalog =
    std::unordered_map<std::string, std::string>;
using References =
    std::map<std::string, std::vector<std::string>>;

int failures = 0;
std::vector<std::string> warnings;

void Check(bool condition, const std::string& message)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
}

void Warn(const std::string& message)
{
    warnings.push_back(message);
}

std::string ReadFile(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    Check(file.good(),
        path.string() + ": unable to read file");
    if (!file) return {};
    std::ostringstream stream;
    stream << file.rdbuf();
    return stream.str();
}

std::string LowerAscii(std::string value)
{
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](unsigned char character) {
            return static_cast<char>(
                std::tolower(character));
        });
    return value;
}

std::wstring Utf8ToWide(const std::string& value)
{
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()),
        nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(
        static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()),
        result.data(), length);
    return result;
}

bool IsValidUtf8(const std::string& value)
{
    return value.empty() ||
        MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS,
            value.data(), static_cast<int>(value.size()),
            nullptr, 0) > 0;
}

bool ContainsCjk(const std::string& value)
{
    const std::wstring wide = Utf8ToWide(value);
    return std::ranges::any_of(
        wide,
        [](wchar_t character) {
            return (character >= 0x3400 &&
                    character <= 0x4DBF) ||
                (character >= 0x4E00 &&
                    character <= 0x9FFF) ||
                (character >= 0xF900 &&
                    character <= 0xFAFF);
        });
}

std::string PathLabel(
    const fs::path& path,
    const fs::path& root)
{
    std::error_code error;
    const fs::path relative =
        fs::relative(path, root, error);
    return (error ? path : relative).generic_string();
}

size_t LineNumberAt(
    std::string_view text,
    size_t offset)
{
    return 1 + static_cast<size_t>(
        std::count(
            text.begin(),
            text.begin() +
                static_cast<std::ptrdiff_t>(
                    std::min(offset, text.size())),
            '\n'));
}

std::vector<fs::path> SortedFiles(
    const fs::path& root,
    const std::function<bool(const fs::path&)>& accept)
{
    std::vector<fs::path> result;
    if (!fs::is_directory(root)) return result;
    for (const auto& entry :
         fs::recursive_directory_iterator(root))
    {
        if (entry.is_regular_file() &&
            accept(entry.path()))
            result.push_back(
                entry.path().lexically_normal());
    }
    std::ranges::sort(result);
    return result;
}

bool IsCppSource(const fs::path& path)
{
    static const std::set<std::string> extensions{
        ".c", ".cc", ".cpp", ".h", ".hpp", ".rc"
    };
    return extensions.contains(
        LowerAscii(path.extension().string()));
}

void ReplaceAll(
    std::string& value,
    const std::string& needle,
    const std::string& replacement)
{
    size_t position = 0;
    while ((position = value.find(
                needle, position)) !=
            std::string::npos)
    {
        value.replace(
            position, needle.size(), replacement);
        position += replacement.size();
    }
}

struct FormatSignature
{
    std::multiset<std::string> printfTokens;
    std::multiset<std::string> indexedTokens;

    bool operator==(
        const FormatSignature&) const = default;
};

FormatSignature ExtractFormatSignature(
    const std::string& value)
{
    FormatSignature signature;
    constexpr std::string_view conversions =
        "diuoxXfFeEgGaAcspn";
    for (size_t index = 0;
        index < value.size(); ++index)
    {
        if (value[index] == '{')
        {
            size_t end = index + 1;
            while (end < value.size() &&
                std::isdigit(
                    static_cast<unsigned char>(
                        value[end])))
                ++end;
            if (end > index + 1 &&
                end < value.size() &&
                value[end] == '}')
            {
                signature.indexedTokens.insert(
                    value.substr(
                        index, end - index + 1));
                index = end;
            }
            continue;
        }
        if (value[index] != '%' ||
            index + 1 >= value.size())
            continue;
        if (value[index + 1] == '%')
        {
            ++index;
            continue;
        }

        size_t end = index + 1;
        while (end < value.size() &&
            conversions.find(value[end]) ==
                std::string_view::npos)
        {
            const unsigned char character =
                static_cast<unsigned char>(
                    value[end]);
            if (!(std::isdigit(character) ||
                    value[end] == '$' ||
                    value[end] == '-' ||
                    value[end] == '+' ||
                    value[end] == ' ' ||
                    value[end] == '#' ||
                    value[end] == '0' ||
                    value[end] == '\'' ||
                    value[end] == '.' ||
                    value[end] == '*' ||
                    value[end] == 'h' ||
                    value[end] == 'l' ||
                    value[end] == 'j' ||
                    value[end] == 'z' ||
                    value[end] == 't' ||
                    value[end] == 'L'))
                break;
            ++end;
        }
        if (end < value.size() &&
            conversions.find(value[end]) !=
                std::string_view::npos)
        {
            signature.printfTokens.insert(
                value.substr(
                    index, end - index + 1));
            index = end;
        }
    }
    return signature;
}

bool ParseJsonFile(
    const fs::path& path,
    JsonValue& root)
{
    if (!fs::is_regular_file(path))
    {
        Check(false,
            path.string() + ": file does not exist");
        return false;
    }
    const std::string text = ReadFile(path);
    Check(IsValidUtf8(text),
        path.string() + ": file must be valid UTF-8");
    if (!IsValidUtf8(text)) return false;
    std::string error;
    const bool parsed = ParseJson(
        text, root, &error);
    Check(parsed,
        path.string() + ": invalid JSON: " + error);
    return parsed;
}

std::map<std::string, Catalog> LoadCatalogs(
    const fs::path& languageDirectory)
{
    std::map<std::string, Catalog> catalogs;
    std::set<std::string> normalizedCodes;
    for (const auto& entry :
         fs::directory_iterator(languageDirectory))
    {
        if (!entry.is_regular_file() ||
            LowerAscii(
                entry.path().extension().string()) !=
                ".json")
            continue;

        const std::string code =
            entry.path().stem().string();
        Check(normalizedCodes.insert(
                LowerAscii(code)).second,
            code +
                ": language codes must be unique ignoring case");

        JsonValue root;
        if (!ParseJsonFile(entry.path(), root))
            continue;
        Check(root.IsObject(),
            code +
                ": language catalog must be a JSON object");
        if (!root.IsObject()) continue;

        Catalog catalog;
        for (const auto& [key, value] : root.object)
        {
            Check(!key.empty(),
                code +
                    ": translation keys must not be empty");
            Check(value.IsString(),
                code + ": " + key +
                    ": translation value must be a string");
            if (!value.IsString()) continue;
            Check(!value.string.empty(),
                code + ": " + key +
                    ": translation value must not be empty");
            Check(!Utf8ToWide(value.string).empty(),
                code + ": " + key +
                    ": translation value must be valid UTF-8");
            if (LowerAscii(code).starts_with("en"))
                Check(!ContainsCjk(value.string),
                    code + ": " + key +
                        ": English translation contains Chinese text");
            catalog.emplace(key, value.string);
        }
        Check(!catalog.empty(),
            code +
                ": language catalog must not be empty");
        catalogs.emplace(code, std::move(catalog));
    }
    return catalogs;
}

void TestCatalogMatrix(
    const std::map<std::string, Catalog>& catalogs)
{
    const auto english = catalogs.find("en-US");
    Check(english != catalogs.end(),
        "en-US must exist as the runtime fallback catalog");
    if (english == catalogs.end()) return;

    for (const auto& [language, catalog] : catalogs)
    {
        Check(catalog.size() ==
                english->second.size(),
            language +
                ": every catalog must contain the complete key set");
        for (const auto& [key, fallback] :
             english->second)
        {
            const auto translation = catalog.find(key);
            Check(translation != catalog.end(),
                language +
                    ": missing translation key " + key);
            if (translation == catalog.end()) continue;
            Check(
                ExtractFormatSignature(
                    translation->second) ==
                    ExtractFormatSignature(fallback),
                language + ": " + key +
                    ": format placeholders must match en-US");
        }
        for (const auto& [key, value] : catalog)
        {
            (void)value;
            Check(english->second.contains(key),
                language +
                    ": unexpected translation key " + key);
        }
        for (const auto& [key, fallback] : english->second)
        {
            if (fallback.find("SnowDesktop") == std::string::npos)
                continue;
            const auto translation = catalog.find(key);
            Check(translation != catalog.end() &&
                    translation->second.find("SnowDesktop") !=
                        std::string::npos,
                language + ": " + key +
                    ": the SnowDesktop product name must remain unchanged");
        }
    }
}

std::string DecodeEscapedKey(
    const std::string& encoded)
{
    JsonValue decoded;
    std::string error;
    if (ParseJson(
            "\"" + encoded + "\"",
            decoded, &error) &&
        decoded.IsString())
        return decoded.string;
    return encoded;
}

References CollectReferences(
    const std::vector<fs::path>& files,
    const fs::path& root,
    bool lua)
{
    const std::regex pattern(lua
        ? R"KEY(\bl10n\.tr\s*\(\s*"([^"\\]*(?:\\.[^"\\]*)*)")KEY"
        : R"KEY(\b(?:_L(?:FW|F|W)?|L10N_KEY|L)\s*\(\s*"([^"\\]*(?:\\.[^"\\]*)*)")KEY");
    References references;
    for (const fs::path& path : files)
    {
        const std::string text = ReadFile(path);
        Check(IsValidUtf8(text),
            PathLabel(path, root) +
                ": source file must be valid UTF-8");
        if (!IsValidUtf8(text)) continue;
        for (std::sregex_iterator found(
                 text.begin(), text.end(), pattern),
             end;
             found != end; ++found)
        {
            const std::string key =
                DecodeEscapedKey((*found)[1].str());
            references[key].push_back(
                PathLabel(path, root) + ":" +
                std::to_string(LineNumberAt(
                    text,
                    static_cast<size_t>(
                        found->position()))));
        }
    }
    return references;
}

void MergeReferences(
    References& destination,
    const References& source)
{
    for (const auto& [key, locations] : source)
        destination[key].insert(
            destination[key].end(),
            locations.begin(), locations.end());
}

struct StringLiteral
{
    size_t line = 0;
    std::string content;
};

size_t SkipQuoted(
    std::string_view text,
    size_t start,
    char quote)
{
    size_t index = start + 1;
    while (index < text.size())
    {
        if (text[index] == '\\')
        {
            index += 2;
            continue;
        }
        if (text[index] == quote)
            return index + 1;
        ++index;
    }
    return text.size();
}

std::pair<size_t, std::string> RawStringStart(
    std::string_view text,
    size_t quoteIndex)
{
    size_t prefixStart = quoteIndex;
    while (prefixStart > 0 &&
        (std::isalnum(
             static_cast<unsigned char>(
                 text[prefixStart - 1])) ||
         text[prefixStart - 1] == '_'))
        --prefixStart;
    const std::string prefix(
        text.substr(
            prefixStart,
            quoteIndex - prefixStart));
    static const std::set<std::string> prefixes{
        "R", "u8R", "uR", "UR", "LR"
    };
    if (!prefixes.contains(prefix))
        return {std::string::npos, {}};

    const size_t delimiterEnd =
        text.find('(', quoteIndex + 1);
    if (delimiterEnd == std::string::npos)
        return {std::string::npos, {}};
    const std::string delimiter(
        text.substr(
            quoteIndex + 1,
            delimiterEnd - quoteIndex - 1));
    if (delimiter.size() > 16 ||
        std::ranges::any_of(
            delimiter,
            [](unsigned char character) {
                return std::isspace(character) ||
                    character == '(' ||
                    character == ')' ||
                    character == '\\';
            }))
        return {std::string::npos, {}};
    return {
        delimiterEnd + 1,
        ")" + delimiter + "\""
    };
}

std::vector<StringLiteral> CppStringLiterals(
    const std::string& text)
{
    std::vector<StringLiteral> result;
    size_t index = 0;
    while (index < text.size())
    {
        if (text.compare(index, 2, "//") == 0)
        {
            const size_t newline =
                text.find('\n', index + 2);
            index = newline == std::string::npos
                ? text.size() : newline + 1;
            continue;
        }
        if (text.compare(index, 2, "/*") == 0)
        {
            const size_t end =
                text.find("*/", index + 2);
            index = end == std::string::npos
                ? text.size() : end + 2;
            continue;
        }
        if (text[index] == '\'')
        {
            index = SkipQuoted(text, index, '\'');
            continue;
        }
        if (text[index] != '"')
        {
            ++index;
            continue;
        }

        const size_t line = LineNumberAt(text, index);
        const auto [rawContentStart, rawEndMarker] =
            RawStringStart(text, index);
        if (rawContentStart != std::string::npos)
        {
            const size_t rawEnd =
                text.find(rawEndMarker, rawContentStart);
            if (rawEnd == std::string::npos)
            {
                result.push_back({
                    line,
                    text.substr(rawContentStart)
                });
                break;
            }
            result.push_back({
                line,
                text.substr(
                    rawContentStart,
                    rawEnd - rawContentStart)
            });
            index = rawEnd + rawEndMarker.size();
            continue;
        }

        const size_t contentStart = index + 1;
        index = contentStart;
        while (index < text.size())
        {
            if (text[index] == '\\')
            {
                index += 2;
                continue;
            }
            if (text[index] == '"') break;
            ++index;
        }
        result.push_back({
            line,
            text.substr(
                contentStart,
                index - contentStart)
        });
        index = std::min(index + 1, text.size());
    }
    return result;
}

std::pair<size_t, std::string> LuaLongBracket(
    std::string_view text,
    size_t start)
{
    if (start >= text.size() || text[start] != '[')
        return {std::string::npos, {}};
    size_t index = start + 1;
    while (index < text.size() && text[index] == '=')
        ++index;
    if (index >= text.size() || text[index] != '[')
        return {std::string::npos, {}};
    return {
        index + 1,
        "]" + std::string(
            text.substr(start + 1, index - start - 1)) +
            "]"
    };
}

std::vector<StringLiteral> LuaStringLiterals(
    const std::string& text)
{
    std::vector<StringLiteral> result;
    size_t index = 0;
    while (index < text.size())
    {
        if (text.compare(index, 2, "--") == 0)
        {
            const auto [contentStart, endMarker] =
                LuaLongBracket(text, index + 2);
            if (contentStart != std::string::npos)
            {
                const size_t end =
                    text.find(endMarker, contentStart);
                index = end == std::string::npos
                    ? text.size() : end + endMarker.size();
            }
            else
            {
                const size_t newline =
                    text.find('\n', index + 2);
                index = newline == std::string::npos
                    ? text.size() : newline + 1;
            }
            continue;
        }

        if (text[index] == '\'' || text[index] == '"')
        {
            const char quote = text[index];
            const size_t line = LineNumberAt(text, index);
            const size_t contentStart = index + 1;
            index = contentStart;
            while (index < text.size())
            {
                if (text[index] == '\\')
                {
                    index += 2;
                    continue;
                }
                if (text[index] == quote) break;
                ++index;
            }
            result.push_back({
                line,
                text.substr(
                    contentStart,
                    index - contentStart)
            });
            index = std::min(index + 1, text.size());
            continue;
        }

        const auto [contentStart, endMarker] =
            LuaLongBracket(text, index);
        if (contentStart != std::string::npos)
        {
            const size_t line = LineNumberAt(text, index);
            const size_t end =
                text.find(endMarker, contentStart);
            if (end == std::string::npos)
            {
                result.push_back({
                    line,
                    text.substr(contentStart)
                });
                break;
            }
            result.push_back({
                line,
                text.substr(
                    contentStart,
                    end - contentStart)
            });
            index = end + endMarker.size();
            continue;
        }
        ++index;
    }
    return result;
}

std::vector<std::string> SourceLines(
    const std::string& text)
{
    std::vector<std::string> lines;
    std::istringstream stream(text);
    for (std::string line; std::getline(stream, line);)
        lines.push_back(std::move(line));
    return lines;
}

void TestNoHardcodedChinese(
    const std::vector<fs::path>& files,
    const fs::path& root,
    bool lua)
{
    constexpr std::string_view allowMarker =
        "l10n-allow";
    for (const fs::path& path : files)
    {
        const std::string text = ReadFile(path);
        const std::vector<std::string> lines =
            SourceLines(text);
        const std::vector<StringLiteral> literals = lua
            ? LuaStringLiterals(text)
            : CppStringLiterals(text);
        for (const StringLiteral& literal : literals)
        {
            if (!ContainsCjk(literal.content)) continue;
            const std::string sourceLine =
                literal.line > 0 &&
                literal.line <= lines.size()
                ? LowerAscii(lines[literal.line - 1])
                : std::string{};
            if (sourceLine.find(allowMarker) !=
                std::string::npos)
                continue;
            std::string compact = literal.content;
            ReplaceAll(compact, "\r", "\\r");
            ReplaceAll(compact, "\n", "\\n");
            if (compact.size() > 100)
                compact = compact.substr(0, 97) + "...";
            Check(false,
                "hard-coded Chinese string: " +
                PathLabel(path, root) + ":" +
                std::to_string(literal.line) + ": " +
                compact +
                " (translate it or add a line l10n-allow marker with a reason)");
        }
    }
}

void TestGlobalSourceContract(
    const std::map<std::string, Catalog>& catalogs,
    const References& references,
    bool strictUnused)
{
    for (const auto& [key, locations] : references)
    {
        for (const auto& [language, catalog] : catalogs)
        {
            Check(catalog.contains(key),
                language +
                    ": missing referenced key " + key +
                    " (used at " +
                    (locations.empty()
                        ? "unknown"
                        : locations.front()) +
                    ")");
        }
    }

    const auto baseline = catalogs.contains("zh-CN")
        ? catalogs.find("zh-CN")
        : catalogs.begin();
    if (baseline == catalogs.end()) return;
    size_t unused = 0;
    for (const auto& [key, value] : baseline->second)
    {
        (void)value;
        if (!references.contains(key)) ++unused;
    }
    if (unused != 0)
    {
        const std::string message =
            std::to_string(unused) +
            " global translation keys are not referenced by literal C/C++ localization calls";
        if (strictUnused)
            Check(false, message);
        else
            Warn(message +
                " (run SnowDesktopLocalizationContractTests with --strict-unused to make this an error)");
    }
}

const JsonValue* FindField(
    const JsonValue& object,
    std::string_view key)
{
    return object.Find(key);
}

std::string RequiredManifestKey(
    const JsonValue& manifest,
    std::string_view field,
    const std::string& label)
{
    const JsonValue* value = FindField(manifest, field);
    Check(value != nullptr,
        label + ": missing localization field " +
            std::string(field));
    if (value == nullptr) return {};
    Check(value->IsString() && !value->string.empty(),
        label + ": " + std::string(field) +
            " must be a non-empty string");
    return value->IsString() ? value->string : std::string{};
}

size_t TestWidgetManifests(
    const std::vector<fs::path>& manifestFiles,
    const std::vector<fs::path>& luaFiles,
    const std::map<fs::path, References>& luaReferences,
    const std::map<std::string, Catalog>& globalCatalogs,
    const fs::path& root,
    bool strictUnused)
{
    size_t referenceCount = 0;
    std::set<fs::path> pairedLuaFiles;
    for (const fs::path& path : manifestFiles)
    {
        const std::string label = PathLabel(path, root);
        JsonValue manifest;
        if (!ParseJsonFile(path, manifest)) continue;
        Check(manifest.IsObject(),
            label +
                ": top-level JSON value must be an object");
        if (!manifest.IsObject()) continue;

        std::vector<fs::path> packageLuaFiles;
        if (LowerAscii(path.filename().string()) ==
            "widget.json")
        {
            std::string entryName = "main.lua";
            if (const JsonValue* entry =
                    FindField(manifest, "entry"))
            {
                Check(entry->IsString() &&
                        !entry->string.empty(),
                    label +
                        ": entry must be a non-empty string");
                if (!entry->IsString() ||
                    entry->string.empty())
                    entryName.clear();
                else
                    entryName = entry->string;
            }
            if (!entryName.empty())
            {
                Check(fs::is_regular_file(
                        path.parent_path() /
                        fs::path(entryName)),
                    label +
                        ": package entry does not exist: " +
                        entryName);
                packageLuaFiles = SortedFiles(
                    path.parent_path(),
                    [](const fs::path& candidate) {
                        return LowerAscii(
                            candidate.extension().string()) ==
                            ".lua";
                    });
            }
        }
        else
        {
            constexpr std::string_view suffix =
                ".widget.json";
            const std::string filename =
                path.filename().string();
            const std::string stem = filename.substr(
                0, filename.size() - suffix.size());
            packageLuaFiles.push_back(
                (path.parent_path() /
                    (stem + ".lua")).lexically_normal());
        }

        References references;
        for (const fs::path& luaPath : packageLuaFiles)
        {
            const fs::path normalized =
                luaPath.lexically_normal();
            pairedLuaFiles.insert(normalized);
            if (const auto found =
                    luaReferences.find(normalized);
                found != luaReferences.end())
                MergeReferences(references, found->second);
        }

        for (std::string_view field :
             {"nameKey", "descriptionKey"})
        {
            const std::string key =
                RequiredManifestKey(
                    manifest, field, label);
            if (!key.empty())
                references[key].push_back(
                    label + ":" +
                    std::string(field));
        }

        if (const JsonValue* titleKeys =
                FindField(manifest, "titleKeys"))
        {
            Check(titleKeys->IsArray(),
                label +
                    ": titleKeys must be an array of non-empty strings");
            if (titleKeys->IsArray())
            {
                for (const JsonValue& value :
                     titleKeys->array)
                {
                    Check(value.IsString() &&
                            !value.string.empty(),
                        label +
                            ": titleKeys must be an array of non-empty strings");
                    if (value.IsString() &&
                        !value.string.empty())
                        references[value.string].push_back(
                            label + ":titleKeys");
                }
            }
        }

        auto collectOptionalKey = [&](const JsonValue& object,
            std::string_view field, const std::string& location) {
            const JsonValue* value = FindField(object, field);
            if (value == nullptr) return;
            Check(value->IsString() && !value->string.empty(),
                location + " must be a non-empty string");
            if (value->IsString() && !value->string.empty())
                references[value->string].push_back(location);
        };
        auto collectStorageKeys = [&](const JsonValue& object,
            const std::string& location) {
            const JsonValue* storageKeys =
                FindField(object, "storageKeys");
            if (storageKeys == nullptr) return;
            Check(storageKeys->IsObject(),
                location + " must be an object");
            if (!storageKeys->IsObject()) return;
            const JsonValue* storage = FindField(object, "storage");
            Check(storage != nullptr && storage->IsObject(),
                location + " requires an adjacent storage object with English fallbacks");
            for (const auto& [storageName, localizationKey] :
                storageKeys->object)
            {
                Check(!storageName.empty() && localizationKey.IsString() &&
                        !localizationKey.string.empty(),
                    location + " must map storage names to non-empty localization keys");
                if (storage != nullptr && storage->IsObject())
                    Check(storage->Find(storageName) != nullptr,
                        location + " has no English fallback for " + storageName);
                if (localizationKey.IsString() &&
                    !localizationKey.string.empty())
                    references[localizationKey.string].push_back(
                        location + "." + storageName);
            }
        };
        if (const JsonValue* previewData =
                FindField(manifest, "previewData"))
        {
            Check(previewData->IsObject(),
                label + ": previewData must be an object");
            if (previewData->IsObject())
            {
                collectOptionalKey(*previewData, "introductionKey",
                    label + ":previewData.introductionKey");
                collectStorageKeys(*previewData,
                    label + ":previewData.storageKeys");
                if (const JsonValue* variants =
                        FindField(*previewData, "variants"))
                {
                    Check(variants->IsArray(),
                        label + ": previewData.variants must be an array");
                    if (variants->IsArray())
                    {
                        for (size_t index = 0;
                            index < variants->array.size(); ++index)
                        {
                            const JsonValue& variant = variants->array[index];
                            const std::string variantLocation = label +
                                ":previewData.variants[" +
                                std::to_string(index) + "]";
                            Check(variant.IsObject(),
                                variantLocation + " must be an object");
                            if (!variant.IsObject()) continue;
                            collectOptionalKey(variant, "titleKey",
                                variantLocation + ".titleKey");
                            collectOptionalKey(variant, "descriptionKey",
                                variantLocation + ".descriptionKey");
                            collectStorageKeys(variant,
                                variantLocation + ".storageKeys");
                        }
                    }
                }
            }
        }

        for (std::string_view field :
             {"name", "description"})
        {
            if (const JsonValue* fallback =
                    FindField(manifest, field);
                fallback && fallback->IsString())
                Check(!ContainsCjk(fallback->string),
                    label +
                        ": hard-coded Chinese in manifest " +
                        std::string(field));
        }

        const JsonValue* locales =
            FindField(manifest, "locales");
        Check(locales != nullptr &&
                locales->IsObject() &&
                !locales->object.empty(),
            label +
                ": locales must be a non-empty object");
        if (locales == nullptr ||
            !locales->IsObject() ||
            locales->object.empty())
            continue;

        std::map<std::string, Catalog> catalogs;
        for (const auto& [language, value] :
             locales->object)
        {
            Check(value.IsObject(),
                label + ": locale " + language +
                    " must be an object");
            if (!value.IsObject()) continue;
            Catalog catalog;
            for (const auto& [key, translation] :
                 value.object)
            {
                Check(translation.IsString(),
                    label + ": locale " + language +
                        " contains a non-string value for " +
                        key);
                if (!translation.IsString()) continue;
                Check(!translation.string.empty(),
                    label + ": empty " + language +
                        " translation for " + key);
                if (LowerAscii(language).starts_with("en"))
                    Check(!ContainsCjk(translation.string),
                        label + ": Chinese text remains in " +
                            language + " translation " + key);
                catalog.emplace(key, translation.string);
            }
            catalogs.emplace(language, std::move(catalog));
        }

        for (const auto& [language, catalog] :
             globalCatalogs)
        {
            (void)catalog;
            Check(catalogs.contains(language),
                label +
                    ": missing widget locale " + language);
        }
        if (catalogs.empty()) continue;

        const auto baseline = catalogs.contains("zh-CN")
            ? catalogs.find("zh-CN")
            : catalogs.begin();
        for (const auto& [language, catalog] : catalogs)
        {
            for (const auto& [key, value] : baseline->second)
            {
                (void)value;
                Check(catalog.contains(key),
                    label + ": locale " + language +
                        " missing key present in " +
                        baseline->first + ": " + key);
            }
            for (const auto& [key, value] : catalog)
            {
                (void)value;
                Check(baseline->second.contains(key),
                    label + ": locale " + language +
                        " has extra key absent from " +
                        baseline->first + ": " + key);
            }
        }

        for (const auto& [key, locations] : references)
        {
            for (const auto& [language, catalog] : catalogs)
                Check(catalog.contains(key),
                    label + ": locale " + language +
                        " missing referenced key " + key +
                        " (used at " +
                        (locations.empty()
                            ? "unknown"
                            : locations.front()) +
                        ")");
        }

        for (const auto& [key, baselineValue] :
             baseline->second)
        {
            for (const auto& [language, catalog] : catalogs)
            {
                const auto translation = catalog.find(key);
                if (translation == catalog.end()) continue;
                Check(
                    ExtractFormatSignature(
                        translation->second) ==
                        ExtractFormatSignature(
                            baselineValue),
                    label + ": placeholder mismatch for " +
                        key + " in " + language +
                        " vs " + baseline->first);
            }
        }

        size_t unused = 0;
        for (const auto& [key, value] : baseline->second)
        {
            (void)value;
            if (!references.contains(key)) ++unused;
        }
        if (unused != 0)
        {
            const std::string message = label + ": " +
                std::to_string(unused) +
                " widget locale keys are unused";
            if (strictUnused)
                Check(false, message);
            else
                Warn(message);
        }
        referenceCount += references.size();
    }

    for (const fs::path& luaPath : luaFiles)
        Check(pairedLuaFiles.contains(luaPath),
            PathLabel(luaPath, root) +
                ": missing containing widget.json or matching .widget.json manifest");
    return referenceCount;
}

void TestRuntimeCatalogMatrix(
    const fs::path& languageDirectory,
    const std::map<std::string, Catalog>& catalogs)
{
    const std::vector<std::string> chineseLanguages{
        "en-US", "zh-CN", "zh-TW"
    };
    Check(snowdesktop::localization::ResolveBestLanguage(
            chineseLanguages, "zh-HK") == "zh-TW",
        "traditional Chinese regions must prefer zh-TW");
    Check(snowdesktop::localization::ResolveBestLanguage(
            chineseLanguages, "zh-MO") == "zh-TW",
        "Macau Chinese must prefer zh-TW");
    Check(snowdesktop::localization::ResolveBestLanguage(
            chineseLanguages, "zh-Hans-SG") == "zh-CN",
        "simplified Chinese regions must prefer zh-CN");

    Locale& locale = Locale::Instance();
    locale.Init(languageDirectory.c_str());
    Check(locale.GetAvailableLanguages().size() ==
            catalogs.size(),
        "Locale must load every valid language catalog");
    Check(locale.GetAvailableLanguages().size() < 2 ||
            locale.GetAvailableLanguages()[0].code == "en-US",
        "English must be the first selectable language");
    Check(locale.GetAvailableLanguages().size() < 2 ||
            locale.GetAvailableLanguages()[1].code == "zh-CN",
        "Simplified Chinese must be the second selectable language");

    locale.SetLanguage("zh-CN");
    for (const LanguageInfo& language :
        locale.GetAvailableLanguages())
    {
        const std::string localizedName =
            locale.GetLocalizedLanguageName(language.code);
        Check(!localizedName.empty() && localizedName != "???",
            language.code + ": language name must be available in the current language");
    }
    const std::string koreanLanguageName =
        locale.GetLocalizedLanguageName("ko-KR");
    Check(koreanLanguageName == "韩语",
        "language names must be localized in the active language: got " +
            koreanLanguageName);
    Check(locale.GetAvailableLanguages().empty() ||
            locale.GetAvailableLanguages().at(4).displayName == "한국어",
        "Korean must use a stable native language name");

    constexpr const char* arguments[]{
        "ARG0", "ARG1", "ARG2",
        "ARG3", "ARG4", "ARG5"
    };
    for (const auto& [language, catalog] : catalogs)
    {
        Check(locale.HasLanguage(language) &&
                locale.HasLanguage(
                    LowerAscii(language)),
            language +
                ": language lookup must be case insensitive");
        locale.SetLanguage(language.c_str());
        Check(locale.GetEffectiveLanguage() == language,
            language +
                ": runtime must resolve the selected catalog");

        for (const auto& [key, translation] : catalog)
        {
            Check(locale.Tr(key.c_str()) == translation,
                language + ": " + key +
                    ": narrow runtime lookup must use the selected catalog");
            Check(locale.TrW(key.c_str()) ==
                    Utf8ToWide(translation),
                language + ": " + key +
                    ": wide runtime lookup must preserve UTF-8 text");

            std::string formatted = translation;
            for (size_t index = 0;
                index < std::size(arguments); ++index)
            {
                ReplaceAll(
                    formatted,
                    "{" + std::to_string(index) + "}",
                    arguments[index]);
            }
            Check(locale.TrFormat(
                    key.c_str(),
                    {arguments[0], arguments[1],
                     arguments[2], arguments[3],
                     arguments[4], arguments[5]}) ==
                    formatted,
                language + ": " + key +
                    ": indexed formatting must replace every argument");
        }
    }

    const std::string previous = locale.GetLanguage();
    locale.SetLanguage("not-a-registered-language");
    Check(locale.GetLanguage() == previous,
        "an unknown language must not replace the active catalog");
    Check(std::string(locale.Tr(
            "missing.translation.key")) ==
            "missing.translation.key" &&
        std::wstring(locale.TrW(
            "missing.translation.key")) ==
            L"missing.translation.key",
        "missing keys must retain the visible diagnostic fallback");
}

}

int wmain(int argc, wchar_t* argv[])
{
    if (argc < 2 || argc > 3 ||
        (argc == 3 &&
            std::wstring_view(argv[2]) !=
                L"--strict-unused"))
    {
        std::cerr <<
            "usage: SnowDesktopLocalizationContractTests <project-root> [--strict-unused]\n";
        return 2;
    }
    const bool strictUnused = argc == 3;

    const fs::path projectRoot = argv[1];
    const fs::path languageDirectory =
        projectRoot / "lang";
    const fs::path sourceDirectory =
        projectRoot / "src";
    const fs::path widgetDirectory =
        projectRoot / "widgets";
    Check(fs::is_directory(languageDirectory),
        "language directory must exist");
    Check(fs::is_directory(sourceDirectory),
        "source directory must exist");
    Check(fs::is_directory(widgetDirectory),
        "widget directory must exist");
    if (failures != 0) return 1;

    const auto catalogs =
        LoadCatalogs(languageDirectory);
    Check(!catalogs.empty(),
        "at least one language catalog must be registered");
    TestCatalogMatrix(catalogs);

    const std::vector<fs::path> cppFiles =
        SortedFiles(sourceDirectory, IsCppSource);
    const std::vector<fs::path> luaFiles =
        SortedFiles(
            widgetDirectory,
            [](const fs::path& path) {
                if (LowerAscii(path.extension().string()) != ".lua")
                    return false;
                const std::string normalized = LowerAscii(
                    path.lexically_normal().generic_string());
                return normalized.find(
                    "/snowdesktop-lua-widget/library/") ==
                    std::string::npos;
            });
    const std::vector<fs::path> manifestFiles =
        SortedFiles(
            widgetDirectory,
            [](const fs::path& path) {
                const std::string filename =
                    LowerAscii(path.filename().string());
                return filename == "widget.json" ||
                    filename.ends_with(".widget.json");
            });
    Check(!cppFiles.empty(),
        "at least one C/C++ source file must exist");
    Check(!luaFiles.empty(),
        "at least one Lua widget source file must exist");

    const References globalReferences =
        CollectReferences(cppFiles, projectRoot, false);
    TestGlobalSourceContract(
        catalogs, globalReferences, strictUnused);
    TestNoHardcodedChinese(
        cppFiles, projectRoot, false);

    std::map<fs::path, References> luaReferences;
    for (const fs::path& path : luaFiles)
        luaReferences.emplace(
            path,
            CollectReferences(
                {path}, projectRoot, true));
    const size_t widgetReferenceCount =
        TestWidgetManifests(
            manifestFiles,
            luaFiles,
            luaReferences,
            catalogs,
            projectRoot,
            strictUnused);
    TestNoHardcodedChinese(
        luaFiles, projectRoot, true);
    TestRuntimeCatalogMatrix(
        languageDirectory, catalogs);

    for (const std::string& warning : warnings)
        std::cout << "WARNING: " << warning << '\n';
    std::cout << "Checked " <<
        cppFiles.size() + luaFiles.size() <<
        " source files, " <<
        globalReferences.size() + widgetReferenceCount <<
        " referenced keys, " <<
        catalogs.size() << " language files.\n";
    if (failures == 0)
        std::cout <<
            "All localization contract tests passed\n";
    return failures == 0 ? 0 : 1;
}
