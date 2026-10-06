#pragma once

#include "pinyin_full_table.h"

#include <string>
#include <string_view>

namespace snowdesktop::name_pinyin
{
inline bool IsAsciiLetterOrDigit(wchar_t ch)
{
    return (ch >= L'A' && ch <= L'Z') ||
        (ch >= L'a' && ch <= L'z') ||
        (ch >= L'0' && ch <= L'9');
}

inline char ToAsciiSearchChar(wchar_t ch)
{
    if (ch >= L'a' && ch <= L'z')
        return static_cast<char>(
            ch - L'a' + L'A');
    return static_cast<char>(ch);
}

inline bool IsLatinUmlaut(wchar_t ch)
{
    switch (ch)
    {
    case L'\u00FC':
    case L'\u01D6':
    case L'\u01D8':
    case L'\u01DA':
    case L'\u01DC':
    case L'\u00DC':
    case L'\u01D5':
    case L'\u01D7':
    case L'\u01D9':
    case L'\u01DB':
        return true;
    default:
        return false;
    }
}
}

inline std::string BuildNamePinyinFullKey(
    const std::wstring& name)
{
    std::string result;
    result.reserve(name.size() * 4);
    for (wchar_t ch : name)
    {
        const std::string_view syllable =
            PinyinFullSyllableForCodepoint(ch);
        if (!syllable.empty())
            result.append(syllable);
        else if (
            snowdesktop::name_pinyin::
                IsAsciiLetterOrDigit(ch))
            result.push_back(
                snowdesktop::name_pinyin::
                    ToAsciiSearchChar(ch));
        else if (
            snowdesktop::name_pinyin::
                IsLatinUmlaut(ch))
            result.push_back('V');
    }
    return result;
}

inline std::string BuildNamePinyinInitialKey(
    const std::wstring& name)
{
    std::string result;
    result.reserve(name.size());
    for (wchar_t ch : name)
    {
        const std::string_view syllable =
            PinyinFullSyllableForCodepoint(ch);
        if (!syllable.empty())
            result.push_back(syllable.front());
        else if (
            snowdesktop::name_pinyin::
                IsAsciiLetterOrDigit(ch))
            result.push_back(
                snowdesktop::name_pinyin::
                    ToAsciiSearchChar(ch));
        else if (
            snowdesktop::name_pinyin::
                IsLatinUmlaut(ch))
            result.push_back('V');
    }
    return result;
}
