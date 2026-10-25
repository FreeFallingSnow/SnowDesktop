#pragma once

#include "common/language_fallback.h"
#include "launcher_messages.h"

#include <windows.h>
#include <commctrl.h>
#include <cwchar>
#include <iterator>

namespace snowdesktop::auto_start_launch
{
inline bool ConfirmCurrentStartupPrivileges(const wchar_t* operation, DWORD error) noexcept
{
    const LauncherMessage* selected = &kLauncherMessages[0];
    for (const auto& entry : kLauncherMessages)
        if (std::string_view(entry.language) == "en-US") selected = &entry;
    try
    {
        wchar_t locale[LOCALE_NAME_MAX_LENGTH]{};
        GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH);
        char name[LOCALE_NAME_MAX_LENGTH * 4]{};
        WideCharToMultiByte(CP_UTF8, 0, locale, -1, name, sizeof(name), nullptr, nullptr);
        std::vector<std::string> languages;
        for (const auto& entry : kLauncherMessages) languages.emplace_back(entry.language);
        const auto language = localization::ResolveBestLanguage(languages, name);
        for (const auto& entry : kLauncherMessages)
            if (language == entry.language) selected = &entry;
    }
    catch (...) {} // Embedded English remains available even before locale/data initialization.

    wchar_t message[2048]{};
    const int written = swprintf_s(message, L"%ls\n\n%ls (%lu)",
        selected->startupPrivileges, operation, error);
    const wchar_t* content = written > 0 ? message : selected->startupPrivileges;
    const TASKDIALOG_BUTTON buttons[] = {
        {IDCONTINUE, selected->startAnyway}, {IDCANCEL, selected->cancel}
    };
    TASKDIALOGCONFIG dialog{sizeof(dialog)};
    dialog.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    dialog.pszWindowTitle = L"SnowDesktop";
    dialog.pszMainIcon = TD_WARNING_ICON;
    dialog.pszContent = content;
    dialog.cButtons = static_cast<UINT>(std::size(buttons));
    dialog.pButtons = buttons;
    dialog.nDefaultButton = IDCANCEL;
    int choice = IDCANCEL;
    if (SUCCEEDED(TaskDialogIndirect(&dialog, &choice, nullptr, nullptr)))
        return choice == IDCONTINUE;
    return MessageBoxW(nullptr, content, L"SnowDesktop",
        MB_OKCANCEL | MB_ICONWARNING | MB_SETFOREGROUND | MB_DEFBUTTON2) == IDOK;
}
}
