#pragma once
#include <cwctype>
#include <string>
#include <string_view>

namespace snowdesktop::status_bar_input_method::native_menu
{
// Invoke opens the system language picker; the IME's context menu is a
// separate provider operation. Never substitute one for the other on failure.
template<class Invoke, class Context>
auto InvokeInputButton(bool layoutPanel, Invoke invoke, Context context)
{
    return layoutPanel ? invoke() : context();
}
inline bool MatchesModeButton(std::wstring_view name, std::wstring_view prefix)
{
    return !prefix.empty() && name.starts_with(prefix) &&
        (name.size() == prefix.size() || iswspace(name[prefix.size()]));
}
inline std::wstring NormalizeButtonText(std::wstring_view value)
{
    std::wstring result;
    for (const auto character : value)
    {
        if (iswspace(character))
        {
            if (!result.empty() && result.back() != L' ') result += L' ';
        }
        else result += character;
    }
    if (!result.empty() && result.back() == L' ') result.pop_back();
    return result;
}
inline bool MatchesInputButton(std::wstring_view name, std::wstring_view prefix,
    std::wstring_view languageHint, bool layoutPanel)
{
    if (!MatchesModeButton(name, prefix)) return false;
    // Windows uses the same localized accessible prefix for both buttons.
    // The language button includes the OS Win+Space help string; invoking the
    // mode button instead would toggle conversion rather than open the picker.
    if (languageHint.empty()) return !layoutPanel;
    const bool languageButton = NormalizeButtonText(name).find(NormalizeButtonText(languageHint)) != std::wstring::npos;
    return layoutPanel == languageButton;
}
}
