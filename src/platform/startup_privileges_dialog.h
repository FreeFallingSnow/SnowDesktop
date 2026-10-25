#pragma once

#include "common/language_fallback.h"
#include "launcher_messages.h"

#include <windows.h>
#include <commctrl.h>
#include <cwchar>
#include <iterator>
#include <vector>

namespace snowdesktop::auto_start_launch
{
struct StartupChoiceLabels
{
    const wchar_t* start;
    const wchar_t* cancel;
};

inline std::vector<WORD> StartupChoiceTemplate(const wchar_t* content, const StartupChoiceLabels& labels)
{
    std::vector<WORD> data;
    const auto word = [&](WORD value) { data.push_back(value); };
    const auto dword = [&](DWORD value) { word(LOWORD(value)); word(HIWORD(value)); };
    const auto text = [&](const wchar_t* value) {
        do { word(static_cast<WORD>(*value)); } while (*value++);
    };
    dword(WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT | DS_CENTER);
    dword(0);
    word(3); word(0); word(0); word(380); word(160);
    word(0); word(0); text(L"SnowDesktop");
    word(9); text(L"Segoe UI");
    const auto control = [&](DWORD style, WORD x, WORD y, WORD width, WORD height,
        WORD id, WORD type, const wchar_t* label) {
        if (data.size() % 2) word(0);
        dword(WS_CHILD | WS_VISIBLE | style); dword(0);
        word(x); word(y); word(width); word(height); word(id);
        word(0xffff); word(type); text(label); word(0);
    };
    // A scrolling read-only control keeps full diagnostics available even when
    // a system path or translated explanation is longer than the visible area.
    control(ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
        12, 10, 356, 112, 100, 0x0081, content);
    control(WS_TABSTOP | BS_PUSHBUTTON, 154, 134, 104, 18, IDCONTINUE, 0x0080, labels.start);
    control(WS_TABSTOP | BS_DEFPUSHBUTTON, 266, 134, 102, 18, IDCANCEL, 0x0080, labels.cancel);
    return data;
}

inline INT_PTR CALLBACK StartupChoiceWindowProc(HWND window, UINT message, WPARAM wp, LPARAM)
{
    if (message == WM_INITDIALOG)
    {
        SetFocus(GetDlgItem(window, IDCANCEL));
        return FALSE;
    }
    if (message == WM_CLOSE || (message == WM_COMMAND &&
            (LOWORD(wp) == IDCONTINUE || LOWORD(wp) == IDCANCEL)))
    {
        EndDialog(window, message == WM_CLOSE ? IDCANCEL : LOWORD(wp));
        return TRUE;
    }
    return FALSE;
}

inline int ShowStartupChoiceFallback(const wchar_t* content, const StartupChoiceLabels& labels) noexcept
{
    try
    {
        const auto dialog = StartupChoiceTemplate(content, labels);
        const INT_PTR choice = DialogBoxIndirectParamW(GetModuleHandleW(nullptr),
            reinterpret_cast<const DLGTEMPLATE*>(dialog.data()), nullptr, StartupChoiceWindowProc, 0);
        return choice == IDCONTINUE ? IDCONTINUE : IDCANCEL;
    }
    catch (...) { return IDCANCEL; } // An unavailable UI is never consent.
}

struct StartupDialogApi
{
    decltype(&TaskDialogIndirect) taskDialog = &TaskDialogIndirect;
    decltype(&ShowStartupChoiceFallback) fallback = &ShowStartupChoiceFallback;
};

inline bool ConfirmStartupContinuation(const wchar_t* content, const StartupChoiceLabels& labels,
    const StartupDialogApi& api = {}) noexcept
{
    const TASKDIALOG_BUTTON buttons[] = {{IDCONTINUE, labels.start}, {IDCANCEL, labels.cancel}};
    TASKDIALOGCONFIG dialog{sizeof(dialog)};
    dialog.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    dialog.pszWindowTitle = L"SnowDesktop";
    dialog.pszMainIcon = TD_WARNING_ICON;
    dialog.pszContent = content;
    dialog.cButtons = static_cast<UINT>(std::size(buttons));
    dialog.pButtons = buttons;
    dialog.nDefaultButton = IDCANCEL;
    int choice = IDCANCEL;
    if (FAILED(api.taskDialog(&dialog, &choice, nullptr, nullptr)))
        choice = api.fallback(content, labels);
    return choice == IDCONTINUE;
}

inline const LauncherMessage& StartupMessages() noexcept
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

    return *selected;
}

inline bool ConfirmCurrentStartupPrivileges(const wchar_t* operation, DWORD error) noexcept
{
    const auto& selected = StartupMessages();
    wchar_t message[2048]{};
    const int written = swprintf_s(message, L"%ls\n\n%ls (%lu)",
        selected.startupPrivileges, operation, error);
    return ConfirmStartupContinuation(written > 0 ? message : selected.startupPrivileges,
        {selected.startAnyway, selected.cancel});
}
}
