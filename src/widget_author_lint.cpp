#include "widget_author_lint.h"

#include "widget_api_registry.h"
#include "widget_view_contract.h"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace snowdesktop::widget_authoring
{
namespace
{
enum class TokenKind
{
    Identifier,
    String,
    Symbol,
};

struct Token
{
    TokenKind kind = TokenKind::Symbol;
    std::string text;
    std::size_t line = 1;
};

bool IsIdentifierStart(unsigned char value) noexcept
{
    return std::isalpha(value) != 0 || value == '_';
}

bool IsIdentifierContinue(unsigned char value) noexcept
{
    return std::isalnum(value) != 0 || value == '_';
}

std::optional<std::pair<std::size_t, std::size_t>> LongBracketAt(
    std::string_view source, std::size_t offset)
{
    if (offset >= source.size() || source[offset] != '[')
        return std::nullopt;
    std::size_t cursor = offset + 1;
    while (cursor < source.size() && source[cursor] == '=') ++cursor;
    if (cursor >= source.size() || source[cursor] != '[')
        return std::nullopt;
    return std::pair{ cursor - offset - 1, cursor + 1 };
}

std::vector<Token> Tokenize(std::string_view source)
{
    std::vector<Token> tokens;
    std::size_t offset = 0;
    std::size_t line = 1;
    while (offset < source.size())
    {
        const unsigned char current =
            static_cast<unsigned char>(source[offset]);
        if (current == '\n')
        {
            ++line;
            ++offset;
            continue;
        }
        if (std::isspace(current) != 0)
        {
            ++offset;
            continue;
        }
        if (offset + 1 < source.size() && source[offset] == '-' &&
                source[offset + 1] == '-')
        {
            const std::size_t commentStart = offset;
            offset += 2;
            if (const auto bracket = LongBracketAt(source, offset))
            {
                const std::string close = "]" +
                    std::string(bracket->first, '=') + "]";
                offset = bracket->second;
                while (offset < source.size())
                {
                    if (source.substr(offset, close.size()) == close)
                    {
                        offset += close.size();
                        break;
                    }
                    if (source[offset] == '\n') ++line;
                    ++offset;
                }
            }
            else
            {
                offset = source.find('\n', commentStart + 2);
                if (offset == std::string_view::npos)
                    offset = source.size();
            }
            continue;
        }
        if (source[offset] == '\'' || source[offset] == '"')
        {
            const char quote = source[offset++];
            const std::size_t tokenLine = line;
            std::string value;
            while (offset < source.size())
            {
                const char character = source[offset++];
                if (character == quote) break;
                if (character == '\n') ++line;
                if (character == '\\' && offset < source.size())
                {
                    const char escaped = source[offset++];
                    switch (escaped)
                    {
                    case 'n': value.push_back('\n'); break;
                    case 'r': value.push_back('\r'); break;
                    case 't': value.push_back('\t'); break;
                    default: value.push_back(escaped); break;
                    }
                }
                else value.push_back(character);
            }
            tokens.push_back({ TokenKind::String, std::move(value),
                tokenLine });
            continue;
        }
        if (const auto bracket = LongBracketAt(source, offset))
        {
            const std::size_t tokenLine = line;
            const std::string close = "]" +
                std::string(bracket->first, '=') + "]";
            offset = bracket->second;
            std::string value;
            while (offset < source.size())
            {
                if (source.substr(offset, close.size()) == close)
                {
                    offset += close.size();
                    break;
                }
                if (source[offset] == '\n') ++line;
                value.push_back(source[offset++]);
            }
            tokens.push_back({ TokenKind::String, std::move(value),
                tokenLine });
            continue;
        }
        if (IsIdentifierStart(current))
        {
            const std::size_t start = offset++;
            while (offset < source.size() && IsIdentifierContinue(
                    static_cast<unsigned char>(source[offset])))
                ++offset;
            tokens.push_back({ TokenKind::Identifier,
                std::string(source.substr(start, offset - start)), line });
            continue;
        }
        tokens.push_back({ TokenKind::Symbol,
            std::string(1, source[offset]), line });
        ++offset;
    }
    return tokens;
}

bool IsSymbol(const Token& token, std::string_view symbol) noexcept
{
    return token.kind == TokenKind::Symbol && token.text == symbol;
}

struct TokenRange
{
    std::size_t begin = 0;
    std::size_t end = 0;
};

std::vector<TokenRange> CallArguments(
    const std::vector<Token>& tokens, std::size_t openParenthesis)
{
    std::vector<TokenRange> arguments;
    if (openParenthesis >= tokens.size() ||
            !IsSymbol(tokens[openParenthesis], "("))
        return arguments;
    std::size_t begin = openParenthesis + 1;
    std::size_t parentheses = 1;
    std::size_t braces = 0;
    std::size_t brackets = 0;
    for (std::size_t cursor = begin; cursor < tokens.size(); ++cursor)
    {
        if (IsSymbol(tokens[cursor], "(")) ++parentheses;
        else if (IsSymbol(tokens[cursor], ")"))
        {
            if (--parentheses == 0)
            {
                if (cursor > begin) arguments.push_back({ begin, cursor });
                return arguments;
            }
        }
        else if (IsSymbol(tokens[cursor], "{")) ++braces;
        else if (IsSymbol(tokens[cursor], "}") && braces > 0) --braces;
        else if (IsSymbol(tokens[cursor], "[")) ++brackets;
        else if (IsSymbol(tokens[cursor], "]") && brackets > 0) --brackets;
        else if (IsSymbol(tokens[cursor], ",") && parentheses == 1 &&
                braces == 0 && brackets == 0)
        {
            arguments.push_back({ begin, cursor });
            begin = cursor + 1;
        }
    }
    return {};
}

bool IsSingleToken(const std::vector<Token>& tokens, TokenRange range,
    TokenKind kind, std::string_view text) noexcept
{
    return range.end == range.begin + 1 &&
        tokens[range.begin].kind == kind && tokens[range.begin].text == text;
}

bool IsSingleTokenKind(const std::vector<Token>& tokens, TokenRange range,
    TokenKind kind) noexcept
{
    return range.end == range.begin + 1 &&
        tokens[range.begin].kind == kind;
}

bool IsLayoutDimension(const std::vector<Token>& tokens, TokenRange range,
    std::string_view first, std::string_view second) noexcept
{
    if (range.end != range.begin + 5) return false;
    const auto matches = [&](std::string_view name) {
        return tokens[range.begin].kind == TokenKind::Identifier &&
            tokens[range.begin].text == "layout" &&
            IsSymbol(tokens[range.begin + 1], ".") &&
            tokens[range.begin + 2].kind == TokenKind::Identifier &&
            tokens[range.begin + 2].text == name &&
            IsSymbol(tokens[range.begin + 3], "(") &&
            IsSymbol(tokens[range.begin + 4], ")");
    };
    return matches(first) || matches(second);
}

void AddIssue(LintReport& report, LintSeverity severity,
    std::string code, const std::filesystem::path& path,
    std::size_t line, std::string message)
{
    report.issues.push_back({ severity, std::move(code), path, line,
        std::move(message) });
}

std::string PathUtf8(const std::filesystem::path& path);

std::size_t LuaErrorLine(std::string_view message) noexcept
{
    for (std::size_t offset = 0; offset < message.size(); ++offset)
    {
        if (message[offset] != ':') continue;
        std::size_t cursor = offset + 1;
        std::size_t value = 0;
        bool hasDigit = false;
        while (cursor < message.size() &&
                std::isdigit(static_cast<unsigned char>(message[cursor])))
        {
            hasDigit = true;
            value = value * 10 + static_cast<std::size_t>(
                message[cursor] - '0');
            ++cursor;
        }
        if (hasDigit && cursor < message.size() &&
                message[cursor] == ':')
            return std::max<std::size_t>(1, value);
    }
    return 1;
}

bool LintLuaSyntax(LintReport& report,
    const std::filesystem::path& path, std::string_view source)
{
    lua_State* state = luaL_newstate();
    if (!state)
    {
        AddIssue(report, LintSeverity::Error, "lua.memory", path, 1,
            "cannot allocate a Lua syntax-check state");
        return false;
    }
    const std::string chunkName = "@" + PathUtf8(path);
    const int status = luaL_loadbuffer(state, source.data(), source.size(),
        chunkName.c_str());
    if (status == LUA_OK)
    {
        lua_close(state);
        return true;
    }
    const char* raw = lua_tostring(state, -1);
    std::string message = raw ? raw : "invalid Lua syntax";
    if (message.size() > 4096) message.resize(4096);
    const std::size_t line = LuaErrorLine(message);
    AddIssue(report, LintSeverity::Error, "lua.syntax", path, line,
        std::move(message));
    lua_close(state);
    return false;
}

std::string JsonEscape(std::string_view value)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string result = "\"";
    for (const unsigned char character : value)
    {
        switch (character)
        {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (character < 0x20)
            {
                result += "\\u00";
                result.push_back(kHex[character >> 4]);
                result.push_back(kHex[character & 0x0f]);
            }
            else result.push_back(static_cast<char>(character));
            break;
        }
    }
    result.push_back('"');
    return result;
}

std::string PathUtf8(const std::filesystem::path& path)
{
    const auto value = path.generic_u8string();
    return std::string(value.begin(), value.end());
}

const snowdesktop::widget_api::PublicApiFunctionContract*
FindPublicFunction(std::string_view library, std::string_view name)
{
    const auto contracts =
        snowdesktop::widget_api::PublicApiFunctionContracts();
    const auto found = std::find_if(contracts.begin(), contracts.end(),
        [library, name](const auto& contract) {
            return library == contract.library && name == contract.name;
        });
    return found == contracts.end() ? nullptr : &*found;
}

const char* CapabilityPermission(
    std::string_view function, std::string_view capability)
{
    if (function == "data.subscribe")
    {
        for (const auto& contract :
                snowdesktop::widget_api::SystemDataTopicContracts())
            if (capability == contract.name)
                return contract.requiredPermission;
    }
    else if (function == "task.start")
    {
        for (const auto& contract :
                snowdesktop::widget_api::SystemTaskContracts())
            if (capability == contract.name)
                return contract.requiredPermission;
    }
    return nullptr;
}

bool IsKnownCapability(std::string_view function,
    std::string_view capability)
{
    if (function == "data.subscribe")
        return std::any_of(
            snowdesktop::widget_api::SystemDataTopicContracts().begin(),
            snowdesktop::widget_api::SystemDataTopicContracts().end(),
            [capability](const auto& contract) {
                return capability == contract.name;
            });
    if (function == "task.start")
        return std::any_of(
            snowdesktop::widget_api::SystemTaskContracts().begin(),
            snowdesktop::widget_api::SystemTaskContracts().end(),
            [capability](const auto& contract) {
                return capability == contract.name;
            });
    return false;
}

void CheckPermission(LintReport& report,
    const std::unordered_set<std::string>& declared,
    const std::filesystem::path& path, std::size_t line,
    std::string_view qualifiedName, const char* permission)
{
    if (!permission || permission[0] == '\0' ||
            declared.contains(permission))
        return;
    AddIssue(report, LintSeverity::Error, "permission.undeclared", path,
        line, std::string(qualifiedName) + " requires manifest permission " +
            permission);
}

void LintApiCalls(LintReport& report,
    const snowdesktop::widget::PackageManifest& manifest,
    const std::filesystem::path& path, const std::vector<Token>& tokens)
{
    std::unordered_set<std::string> hostLibraries;
    for (const auto& contract :
            snowdesktop::widget_api::PublicApiFunctionContracts())
        hostLibraries.insert(contract.library);
    std::unordered_set<std::string> declared(
        manifest.permissions.begin(), manifest.permissions.end());
    declared.insert(manifest.optionalPermissions.begin(),
        manifest.optionalPermissions.end());
    static constexpr std::array<std::string_view, 5> kForbiddenLibraries = {
        "os", "io", "debug", "package", "coroutine",
    };

    for (std::size_t index = 0; index < tokens.size(); ++index)
    {
        const bool memberAccess = index > 0 &&
            (IsSymbol(tokens[index - 1], ".") ||
                IsSymbol(tokens[index - 1], ":"));
        if (!memberAccess &&
                tokens[index].kind == TokenKind::Identifier &&
                tokens[index].text == "require" &&
                index + 1 < tokens.size() && IsSymbol(tokens[index + 1], "("))
        {
            AddIssue(report, LintSeverity::Error,
                "api.forbidden-global", path, tokens[index].line,
                "require() is unavailable; use module.require() for package modules");
        }
        if (index + 3 >= tokens.size() ||
                tokens[index].kind != TokenKind::Identifier ||
                !IsSymbol(tokens[index + 1], ".") ||
                tokens[index + 2].kind != TokenKind::Identifier ||
                !IsSymbol(tokens[index + 3], "("))
            continue;
        const std::string& library = tokens[index].text;
        const std::string& function = tokens[index + 2].text;
        const std::string qualified = library + "." + function;
        if (std::find(kForbiddenLibraries.begin(),
                kForbiddenLibraries.end(), library) !=
                kForbiddenLibraries.end())
        {
            AddIssue(report, LintSeverity::Error,
                "api.forbidden-library", path, tokens[index].line,
                library + " is not available in the API v2 sandbox");
            continue;
        }
        if (!hostLibraries.contains(library)) continue;
        const auto* contract = FindPublicFunction(library, function);
        if (!contract)
        {
            AddIssue(report, LintSeverity::Error, "api.unknown", path,
                tokens[index].line, qualified +
                    " is not present in the host API catalog");
            continue;
        }
        if (manifest.apiVersion < static_cast<int>(contract->sinceApi) ||
                (contract->untilApi != 0 && manifest.apiVersion >
                    static_cast<int>(contract->untilApi)))
        {
            AddIssue(report, LintSeverity::Error, "api.version", path,
                tokens[index].line, qualified +
                    " is unavailable for the manifest API version");
        }
        CheckPermission(report, declared, path, tokens[index].line,
            qualified, contract->requiredPermission);

        if (qualified == "l10n.tr" && index + 4 < tokens.size() &&
                tokens[index + 4].kind == TokenKind::String)
        {
            const std::string& key = tokens[index + 4].text;
            for (const auto& [locale, metadata] : manifest.locales)
            {
                const auto localized = metadata.strings.find(key);
                if (localized != metadata.strings.end() &&
                        !localized->second.empty())
                    continue;
                AddIssue(report, LintSeverity::Error,
                    "l10n.missing-key", path, tokens[index + 4].line,
                    "localization key " + key + " is missing or empty in " +
                        locale);
            }
        }

        if ((qualified == "data.subscribe" ||
                qualified == "task.start") && index + 4 < tokens.size() &&
                tokens[index + 4].kind == TokenKind::String)
        {
            const std::string& capability = tokens[index + 4].text;
            if (!IsKnownCapability(qualified, capability))
            {
                AddIssue(report, LintSeverity::Error,
                    "api.unknown-capability", path,
                    tokens[index + 4].line, capability +
                        " is not present in the " + qualified + " catalog");
            }
            else
            {
                CheckPermission(report, declared, path,
                    tokens[index + 4].line, capability,
                    CapabilityPermission(qualified, capability));
            }
        }
    }
}

void LintViewConstructors(LintReport& report,
    const std::filesystem::path& path, const std::vector<Token>& tokens)
{
    std::unordered_map<std::string, std::size_t> literalKeys;
    static const std::unordered_set<std::string> kVisibleProperties = {
        "text", "label", "title", "placeholder", "alt",
        "validationMessage", "accessKeyHint",
    };
    for (std::size_t index = 0; index + 4 < tokens.size(); ++index)
    {
        if (tokens[index].kind != TokenKind::Identifier ||
                tokens[index].text != "view" ||
                !IsSymbol(tokens[index + 1], ".") ||
                tokens[index + 2].kind != TokenKind::Identifier ||
                !IsSymbol(tokens[index + 3], "(") ||
                !IsSymbol(tokens[index + 4], "{") ||
                !snowdesktop::widget_runtime::FindViewNodeContract(
                    tokens[index + 2].text))
            continue;

        bool hasKey = false;
        std::optional<std::string> literalKey;
        std::size_t braces = 1;
        std::size_t parentheses = 0;
        for (std::size_t cursor = index + 5;
                cursor < tokens.size() && braces > 0; ++cursor)
        {
            if (IsSymbol(tokens[cursor], "{")) ++braces;
            else if (IsSymbol(tokens[cursor], "}"))
            {
                --braces;
                if (braces == 0) break;
            }
            else if (IsSymbol(tokens[cursor], "(")) ++parentheses;
            else if (IsSymbol(tokens[cursor], ")") && parentheses > 0)
                --parentheses;
            if (braces != 1 || parentheses != 0 ||
                    tokens[cursor].kind != TokenKind::Identifier ||
                    cursor + 2 >= tokens.size() ||
                    !IsSymbol(tokens[cursor + 1], "="))
                continue;
            const std::string& property = tokens[cursor].text;
            if (property == "key")
            {
                hasKey = true;
                if (tokens[cursor + 2].kind == TokenKind::String)
                    literalKey = tokens[cursor + 2].text;
            }
            if (kVisibleProperties.contains(property) &&
                    tokens[cursor + 2].kind == TokenKind::String &&
                    !tokens[cursor + 2].text.empty())
            {
                AddIssue(report, LintSeverity::Warning,
                    "l10n.hardcoded", path, tokens[cursor + 2].line,
                    property +
                        " contains a literal UI string; prefer l10n.tr()");
            }
        }
        if (!hasKey)
        {
            AddIssue(report, LintSeverity::Error, "view.key.missing", path,
                tokens[index].line, "view." + tokens[index + 2].text +
                    " uses a literal property table without key");
        }
        else if (literalKey)
        {
            if (literalKey->empty())
            {
                AddIssue(report, LintSeverity::Error, "view.key.empty", path,
                    tokens[index].line,
                    "declarative view keys must not be empty");
            }
            else if (const auto [found, inserted] =
                    literalKeys.emplace(*literalKey, tokens[index].line);
                    !inserted)
            {
                AddIssue(report, LintSeverity::Warning,
                    "view.key.duplicate-literal", path,
                    tokens[index].line, "literal view key " + *literalKey +
                        " was already used on line " +
                        std::to_string(found->second));
            }
        }
    }
}

void LintImmediateDrawing(LintReport& report,
    const std::filesystem::path& path, const std::vector<Token>& tokens,
    bool allowFullSurfaceContent)
{
    for (std::size_t index = 0; index + 3 < tokens.size(); ++index)
    {
        if (tokens[index].kind != TokenKind::Identifier ||
                tokens[index].text != "draw" ||
                !IsSymbol(tokens[index + 1], ".") ||
                tokens[index + 2].kind != TokenKind::Identifier ||
                !IsSymbol(tokens[index + 3], "("))
            continue;
        const std::string& function = tokens[index + 2].text;
        const auto arguments = CallArguments(tokens, index + 3);
        if (function == "text" && arguments.size() >= 3 &&
                IsSingleTokenKind(tokens, arguments[2], TokenKind::String) &&
                !tokens[arguments[2].begin].text.empty())
        {
            AddIssue(report, LintSeverity::Warning, "l10n.hardcoded", path,
                tokens[arguments[2].begin].line,
                "draw.text contains a literal UI string; prefer l10n.tr()");
        }
        if (!allowFullSurfaceContent &&
                (function == "rect" || function == "gradientRect") &&
                arguments.size() >= 4 &&
                IsSingleToken(tokens, arguments[0], TokenKind::Symbol, "0") &&
                IsSingleToken(tokens, arguments[1], TokenKind::Symbol, "0") &&
                IsLayoutDimension(tokens, arguments[2],
                    "contentWidth", "width") &&
                IsLayoutDimension(tokens, arguments[3],
                    "contentHeight", "height"))
        {
            AddIssue(report, LintSeverity::Warning,
                "surface.full-background", path, tokens[index].line,
                "full-bounds drawing can duplicate the host widget material; "
                "draw content or internal surfaces instead, or add "
                "-- snowwidget: allow-full-surface-content when the full "
                "canvas is intentional content");
        }
    }
}

void LintLocaleCatalogs(LintReport& report,
    const snowdesktop::widget::PackageManifest& manifest)
{
    std::unordered_set<std::string> keys;
    for (const auto& localeEntry : manifest.locales)
        for (const auto& stringEntry : localeEntry.second.strings)
            keys.insert(stringEntry.first);
    for (const auto& key : keys)
    {
        for (const auto& [locale, metadata] : manifest.locales)
        {
            const auto value = metadata.strings.find(key);
            if (value != metadata.strings.end() && !value->second.empty())
                continue;
            AddIssue(report, LintSeverity::Error,
                "l10n.locale-key-set", "widget.json", 1,
                "localization key " + key + " is missing or empty in " +
                    locale);
        }
    }
}

void Merge(LintReport& destination, LintReport source)
{
    destination.fileCount += source.fileCount;
    destination.issues.insert(destination.issues.end(),
        std::make_move_iterator(source.issues.begin()),
        std::make_move_iterator(source.issues.end()));
}
}

bool LintReport::Ok() const noexcept
{
    return ErrorCount() == 0;
}

std::size_t LintReport::ErrorCount() const noexcept
{
    return static_cast<std::size_t>(std::count_if(
        issues.begin(), issues.end(), [](const auto& issue) {
            return issue.severity == LintSeverity::Error;
        }));
}

std::size_t LintReport::WarningCount() const noexcept
{
    return issues.size() - ErrorCount();
}

std::string LintReport::ToJson() const
{
    std::ostringstream output;
    output << "{\"ok\":" << (Ok() ? "true" : "false")
           << ",\"fileCount\":" << fileCount
           << ",\"errorCount\":" << ErrorCount()
           << ",\"warningCount\":" << WarningCount()
           << ",\"issues\":[";
    for (std::size_t index = 0; index < issues.size(); ++index)
    {
        if (index) output << ',';
        const auto& issue = issues[index];
        output << "{\"severity\":\""
               << (issue.severity == LintSeverity::Error
                    ? "error" : "warning")
               << "\",\"code\":" << JsonEscape(issue.code)
               << ",\"path\":" << JsonEscape(PathUtf8(issue.path))
               << ",\"line\":" << issue.line
               << ",\"message\":" << JsonEscape(issue.message) << '}';
    }
    output << "]}";
    return output.str();
}

LintReport LintWidgetSource(
    const snowdesktop::widget::PackageManifest& manifest,
    const std::filesystem::path& relativePath,
    std::string_view source)
{
    LintReport report;
    report.fileCount = 1;
    if (!LintLuaSyntax(report, relativePath, source)) return report;
    const auto tokens = Tokenize(source);
    LintApiCalls(report, manifest, relativePath, tokens);
    LintViewConstructors(report, relativePath, tokens);
    LintImmediateDrawing(report, relativePath, tokens,
        source.find("-- snowwidget: allow-full-surface-content") !=
            std::string_view::npos);
    return report;
}

LintReport LintWidgetDirectory(
    const std::filesystem::path& root,
    const snowdesktop::widget::PackageManifest& manifest)
{
    LintReport report;
    if (manifest.preview.empty())
    {
        AddIssue(report, LintSeverity::Warning, "package.preview.missing",
            "widget.json", 1,
            "final packages should declare a generated preview image");
    }
    LintLocaleCatalogs(report, manifest);
    std::error_code error;
    const auto absoluteRoot = std::filesystem::absolute(root, error);
    if (error)
    {
        AddIssue(report, LintSeverity::Error, "source.root", {}, 1,
            "cannot resolve the package directory");
        return report;
    }
    std::filesystem::recursive_directory_iterator iterator(
        absoluteRoot, std::filesystem::directory_options::skip_permission_denied,
        error);
    if (error)
    {
        AddIssue(report, LintSeverity::Error, "source.enumerate", {}, 1,
            "cannot enumerate the package source files");
        return report;
    }
    const std::filesystem::recursive_directory_iterator end;
    for (; iterator != end; iterator.increment(error))
    {
        if (error)
        {
            AddIssue(report, LintSeverity::Error, "source.enumerate", {}, 1,
                "cannot enumerate every package source file");
            break;
        }
        if (!iterator->is_regular_file(error) || error ||
                iterator->path().extension() != L".lua")
            continue;
        std::ifstream input(iterator->path(), std::ios::binary);
        if (!input)
        {
            AddIssue(report, LintSeverity::Error, "source.read",
                iterator->path().lexically_relative(absoluteRoot), 1,
                "cannot read Lua source");
            continue;
        }
        const std::string source((std::istreambuf_iterator<char>(input)),
            std::istreambuf_iterator<char>());
        Merge(report, LintWidgetSource(manifest,
            iterator->path().lexically_relative(absoluteRoot), source));
    }
    std::sort(report.issues.begin(), report.issues.end(),
        [](const auto& left, const auto& right) {
            if (left.path != right.path) return left.path < right.path;
            if (left.line != right.line) return left.line < right.line;
            return left.code < right.code;
        });
    return report;
}
}
