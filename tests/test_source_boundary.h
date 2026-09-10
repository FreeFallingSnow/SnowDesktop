#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

namespace snowdesktop::test
{
struct SourceBoundary
{
    const char* file;
    const char* begin;
    const char* end;
    std::initializer_list<const char*> forbidden;
};

// Conservative lexical boundaries, including comments; never execution proof.
// Normalize all whitespace so LF/CRLF and wrapping do not change the contract.
inline std::string CompactSource(std::string value)
{
    std::erase_if(value, [](unsigned char ch) { return std::isspace(ch) != 0; });
    return value;
}

inline bool CheckSourceBoundaries(const std::filesystem::path& root,
    std::initializer_list<SourceBoundary> boundaries)
{
    bool passed = true;
    for (const auto& rule : boundaries)
    {
        const auto fail = [&](std::string_view reason) {
            passed = false;
            std::cerr << "FAIL: " << rule.file << " [" << rule.begin << "]: "
                      << reason << '\n';
        };
        std::ifstream input(root / rule.file, std::ios::binary);
        auto source = CompactSource(
            std::string{std::istreambuf_iterator<char>(input), {}});
        if (!input.is_open() || source.empty())
        {
            fail("source is missing or empty");
            continue;
        }
        const auto beginToken = CompactSource(rule.begin);
        const auto endToken = CompactSource(rule.end);
        const auto begin = beginToken.empty() ? 0 : source.find(beginToken);
        if (begin == std::string::npos)
        {
            fail("required section is missing");
            continue;
        }
        const auto end = endToken.empty() ? source.size() :
            source.find(endToken, begin + beginToken.size());
        if (end == std::string::npos || end <= begin)
        {
            fail("required section end is missing");
            continue;
        }
        const auto section = std::string_view(source).substr(begin, end - begin);
        for (const auto token : rule.forbidden)
            if (section.find(CompactSource(token)) != std::string_view::npos)
                fail(std::string("forbidden source token: ") + token);
    }
    return passed;
}
}
