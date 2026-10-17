#pragma once

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::localization_audit
{
enum class TokenKind { Identifier, String, Character, Punctuation };
struct Token
{
    TokenKind kind;
    std::string value;
    std::size_t line;
};

// Source references include keys passed through tables/IPC, not just the first
// argument of _LW. Comments and standalone strings are never consumers.
inline std::vector<Token> Lex(std::string_view text, bool lua = false, bool cmake = false)
{
    std::vector<Token> tokens;
    std::size_t line = 1;
    std::size_t index = 0;
    const auto advance = [&] { if (text[index++] == '\n') ++line; };
    const auto longDelimiter = [&](std::size_t at) {
        if (at >= text.size() || text[at] != '[') return std::string{};
        std::size_t end = at + 1;
        while (end < text.size() && text[end] == '=') ++end;
        return end < text.size() && text[end] == '['
            ? "]" + std::string(end - at - 1, '=') + "]" : std::string{};
    };
    while (index < text.size())
    {
        const char c = text[index];
        if (std::isspace(static_cast<unsigned char>(c))) { advance(); continue; }
        if (cmake && c == '#')
        {
            while (index < text.size() && text[index] != '\n') advance();
            continue;
        }
        const auto rest = text.substr(index);
        if ((!lua && rest.starts_with("//")) || (lua && rest.starts_with("--")))
        {
            const auto delimiter = lua ? longDelimiter(index + 2) : std::string{};
            if (!delimiter.empty())
            {
                const auto end = text.find(delimiter, index + 2 + delimiter.size());
                const auto stop = end == std::string_view::npos ? text.size() : end + delimiter.size();
                while (index < stop) advance();
            }
            else while (index < text.size() && text[index] != '\n') advance();
            continue;
        }
        if (!lua && rest.starts_with("/*"))
        {
            const auto end = text.find("*/", index + 2);
            const auto stop = end == std::string_view::npos ? text.size() : end + 2;
            while (index < stop) advance();
            continue;
        }
        if (lua && c == '[')
        {
            const auto delimiter = longDelimiter(index);
            const auto end = delimiter.empty() ? std::string_view::npos : text.find(delimiter, index + delimiter.size());
            if (end != std::string_view::npos)
            {
                tokens.push_back({TokenKind::String, std::string(text.substr(index + delimiter.size(), end - index - delimiter.size())), line});
                while (index < end + delimiter.size()) advance();
                continue;
            }
        }
        const auto rawPrefix = !lua ? (rest.starts_with("u8R\"") ? 2u :
            (rest.starts_with("LR\"") || rest.starts_with("uR\"") || rest.starts_with("UR\"")) ? 1u : 0u) : 0u;
        if (!lua && rest.substr(rawPrefix).starts_with("R\""))
        {
            const auto opening = text.find('(', index + rawPrefix + 2);
            if (opening != std::string_view::npos && opening - index - rawPrefix <= 18)
            {
                const std::string delimiter(text.substr(index + rawPrefix + 2, opening - index - rawPrefix - 2));
                const auto ending = ")" + delimiter + "\"";
                const auto end = text.find(ending, opening + 1);
                if (end != std::string_view::npos)
                {
                    const auto tokenLine = line;
                    tokens.push_back({TokenKind::String, std::string(text.substr(opening + 1, end - opening - 1)), tokenLine});
                    while (index < end + ending.size()) advance();
                    continue;
                }
            }
        }
        if (!lua && c == '\'' && index > 0 && index + 1 < text.size() &&
            std::isdigit(static_cast<unsigned char>(text[index - 1])) &&
            std::isalnum(static_cast<unsigned char>(text[index + 1])))
        { advance(); continue; } // C++ digit separator, not a character literal.
        if (c == '"' || c == '\'')
        {
            const char quote = c;
            const auto tokenLine = line;
            std::string value;
            advance();
            while (index < text.size() && text[index] != quote)
            {
                if (text[index] == '\\' && index + 1 < text.size())
                {
                    advance();
                    const char escaped = text[index];
                    value += escaped == 'n' ? '\n' : escaped == 'r' ? '\r' : escaped == 't' ? '\t' : escaped;
                    advance();
                }
                else { value += text[index]; advance(); }
            }
            if (index < text.size()) advance();
            tokens.push_back({lua || quote == '"' ? TokenKind::String : TokenKind::Character, std::move(value), tokenLine});
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_')
        {
            const auto begin = index;
            while (index < text.size() && (std::isalnum(static_cast<unsigned char>(text[index])) || text[index] == '_')) advance();
            tokens.push_back({TokenKind::Identifier, std::string(text.substr(begin, index - begin)), line});
            continue;
        }
        tokens.push_back({TokenKind::Punctuation, std::string(1, c), line});
        advance();
    }
    return tokens;
}

struct Source
{
    std::string path;
    std::vector<Token> tokens;
    bool lua = false;
};
using References = std::map<std::string, std::vector<std::string>>;

inline bool IsDirectKey(const std::vector<Token>& tokens, size_t call, size_t literal, bool lua)
{
    size_t first = call + 2;
    while (first < literal && tokens[first].value == "(") ++first;
    const auto& previous = tokens[literal - 1].value;
    const bool branch = previous == "?" || previous == ":" ||
        (lua && (previous == "and" || previous == "or"));
    if (literal != first && !branch) return false;
    return previous != "+" && previous != "." &&
        (literal + 1 == tokens.size() ||
            (tokens[literal + 1].value != "+" && tokens[literal + 1].value != "."));
}

inline std::vector<const Token*> LiveStrings(const Source& source,
    const std::map<std::string, std::size_t>& identifierUses)
{
    const auto& tokens = source.tokens;
    std::set<std::size_t> discarded;
    const size_t outside = tokens.size();
    std::vector<size_t> parent(tokens.size(), outside), scopeEnd(tokens.size(), outside), stack;
    for (size_t at = 0; at < tokens.size(); ++at)
    {
        const auto& token = tokens[at];
        const bool keyword = source.lua && token.kind == TokenKind::Identifier;
        if (keyword && (token.value == "else" || token.value == "elseif"))
        {
            if (!stack.empty()) { scopeEnd[stack.back()] = at; stack.pop_back(); }
        }
        parent[at] = stack.empty() ? outside : stack.back();
        const bool opening = source.lua
            ? keyword && (token.value == "function" || token.value == "then" || token.value == "do" || token.value == "repeat" || token.value == "else")
            : token.kind == TokenKind::Punctuation && token.value == "{";
        const bool closing = source.lua
            ? keyword && (token.value == "end" || token.value == "until")
            : token.kind == TokenKind::Punctuation && token.value == "}";
        if (opening) stack.push_back(at);
        else if (closing && !stack.empty()) { scopeEnd[stack.back()] = at; stack.pop_back(); }
    }
    // A constant with no source consumer must not keep a dead translation
    // alive. Count across translation units so shared header tables work.
    for (std::size_t index = 0; index < tokens.size(); ++index)
    {
        if (tokens[index].kind != TokenKind::Punctuation ||
            (tokens[index].value != "=" && tokens[index].value != "{")) continue;
        if (index == 0) continue;
        std::size_t name = index - 1;
        if (tokens[name].value == "]")
        {
            while (name > 0 && tokens[name].value != "[") --name;
            if (name == 0) continue;
            --name;
        }
        if (tokens[name].kind != TokenKind::Identifier) continue;
        // Lua's table fields (mode = "key") are not local variable
        // declarations. Their value is consumed through the enclosing table.
        if (source.lua && (name == 0 || tokens[name - 1].value != "local")) continue;
        if (!source.lua && name > 0 && tokens[name - 1].value == ".") continue;
        if (tokens[index].value == "{")
        {
            bool declaration = false;
            for (std::size_t before = index - 1; before > 0; --before)
            {
                const auto& value = tokens[before - 1].value;
                if (value == ";" || value == "{" || value == "}") break;
                if (value == "namespace" || value == "class" || value == "struct") { declaration = false; break; }
                if (value == "constexpr" || value == "const" || value == "auto") declaration = true;
            }
            if (!declaration) continue;
        }
        size_t uses = 0;
        const auto enclosing = parent[name];
        if (enclosing != outside || source.lua)
        {
            const auto begin = enclosing == outside ? size_t{0} : enclosing + 1;
            const auto end = enclosing == outside ? tokens.size() : scopeEnd[enclosing];
            for (size_t at = begin; at < end; ++at)
                if (tokens[at].kind == TokenKind::Identifier && tokens[at].value == tokens[name].value) ++uses;
            // Header members may be consumed by out-of-line definitions.
            if (!source.lua && (source.path.ends_with(".h") || source.path.ends_with(".hpp")))
                if (const auto found = identifierUses.find(tokens[name].value); found != identifierUses.end()) uses = found->second;
        }
        else if (const auto found = identifierUses.find(tokens[name].value); found != identifierUses.end()) uses = found->second;
        if (uses != 1) continue;
        if (index + 1 >= tokens.size() ||
            (tokens[index + 1].kind != TokenKind::String && tokens[index + 1].value != "{" && tokens[index].value != "{")) continue;
        std::size_t end = index + 1;
        int braces = tokens[index].value == "{" ? 1 : 0;
        for (; end < tokens.size(); ++end)
        {
            if (source.lua && braces <= 0 && end > index + 1 &&
                tokens[end].line > tokens[end - 1].line) break;
            if (tokens[end].kind == TokenKind::Punctuation)
            {
                if (tokens[end].value == "{") ++braces;
                if (tokens[end].value == "}") --braces;
                if (tokens[end].value == ";" && braces <= 0) break;
            }
            if (tokens[end].kind == TokenKind::String) discarded.insert(end);
        }
    }
    std::vector<const Token*> result;
    for (std::size_t index = 0; index < tokens.size(); ++index)
    {
        if (tokens[index].kind != TokenKind::String || discarded.contains(index)) continue;
        const bool standalone = index + 1 < tokens.size() && tokens[index + 1].value == ";" &&
            (index == 0 || tokens[index - 1].value == ";" || tokens[index - 1].value == "{");
        if (!standalone) result.push_back(&tokens[index]);
    }
    return result;
}

inline References CollectLiteralBindings(const std::vector<Source>& sources,
    const std::set<std::string>& definedKeys)
{
    std::map<std::string, std::size_t> identifierUses;
    for (const auto& source : sources)
        for (const auto& token : source.tokens)
            if (token.kind == TokenKind::Identifier) ++identifierUses[token.value];
    References references;
    for (const auto& source : sources)
        for (const auto* token : LiveStrings(source, identifierUses))
            if (definedKeys.contains(token->value))
                references[token->value].push_back(source.path + ":" + std::to_string(token->line));
    return references;
}

inline std::vector<std::string> Unused(const std::set<std::string>& definitions,
    const References& references)
{
    std::vector<std::string> result;
    for (const auto& key : definitions) if (!references.contains(key)) result.push_back(key);
    return result;
}

inline std::vector<std::string> CallStrings(const Source& source, std::string_view call, std::size_t wantedArgument)
{
    std::vector<std::string> values;
    const auto& tokens = source.tokens;
    for (std::size_t index = 0; index + 1 < tokens.size(); ++index)
    {
        if (tokens[index].kind != TokenKind::Identifier || tokens[index].value != call || tokens[index + 1].value != "(") continue;
        int depth = 0;
        std::size_t argument = 0;
        for (std::size_t at = index + 2; at < tokens.size(); ++at)
        {
            const auto& token = tokens[at];
            if (token.kind == TokenKind::Punctuation)
            {
                if (token.value == ")" && depth == 0) break;
                if (token.value == "," && depth == 0) { ++argument; continue; }
                if (token.value == "(") ++depth;
                if (token.value == ")") --depth;
            }
            if (argument == wantedArgument && token.kind == TokenKind::String) values.push_back(token.value);
        }
    }
    return values;
}

inline std::vector<std::string> InitializerStrings(const Source& source, std::string_view name)
{
    const auto& tokens = source.tokens;
    for (std::size_t index = 0; index + 1 < tokens.size(); ++index)
    {
        if (tokens[index].value != name || tokens[index + 1].value != "{") continue;
        std::vector<std::string> result;
        for (std::size_t at = index + 2; at < tokens.size() && tokens[at].value != "}"; ++at)
            if (tokens[at].kind == TokenKind::String) result.push_back(tokens[at].value);
        return result;
    }
    return {};
}
}
