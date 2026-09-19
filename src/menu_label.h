#pragma once
#include <string>
#include <string_view>
#include <cwctype>

namespace snowdesktop
{
struct MenuLabel
{
    std::wstring text;
    wchar_t accessKey = 0;
};
// Decode native HMENU text once, before discarding its access-key marker.
// Parentheses in literal names are not inferred as keys; && remains literal.
inline MenuLabel DecodeMenuLabel(std::wstring_view source)
{
    MenuLabel result;
    for (size_t i = 0; i < source.size(); ++i)
    {
        if (source[i] == L'\t')
        {
            result.text.append(source.substr(i));
            break; // The shortcut column is not an access-key declaration.
        }
        if (source[i] == L'&' && i + 1 < source.size() && source[i + 1] != L'\t')
        {
            if (source[i + 1] == L'&') ++i;
            else
            {
                if (!result.accessKey) result.accessKey = static_cast<wchar_t>(std::towlower(source[i + 1]));
                continue;
            }
        }
        result.text += source[i];
    }
    return result;
}
} // namespace snowdesktop
