#pragma once
#include <windows.h>
#include <imm.h>
#include <msctf.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace snowdesktop::status_bar_input_method
{
struct Snapshot
{
    HWND foreground = nullptr;
    HWND focus = nullptr;
    DWORD thread = 0;
    HKL layout = nullptr;
    bool menuActive = false;
    std::wstring label, description;
    friend bool operator==(const Snapshot&, const Snapshot&) = default;
};

// Host-private picker data. TSF profiles distinguish input programs sharing
// the same language/HKL; keyboard layout handles alone do not identify them.
struct Choice
{
    TF_INPUTPROCESSORPROFILE profile{};
    std::wstring name, language;
    bool selected = false;
};
struct Selection
{
    Snapshot target;
    std::vector<Choice> choices;
};
Selection CaptureSelection();
bool RestoreTarget(const Snapshot& target);
HRESULT Select(const Selection&, const Choice&);

inline bool SameProfile(const TF_INPUTPROCESSORPROFILE& a, const TF_INPUTPROCESSORPROFILE& b)
{
    return a.dwProfileType == b.dwProfileType && a.langid == b.langid &&
        a.clsid == b.clsid && a.guidProfile == b.guidProfile && a.hkl == b.hkl;
}

namespace detail
{
// A language is not a conversion mode. A failed IME query is not a successful
// mode sample; retain the last successful presentation instead of showing ZH.
inline std::wstring Label(LANGID language, std::wstring abbreviation,
    std::optional<bool> open, std::optional<DWORD> conversion)
{
    const auto primary = PRIMARYLANGID(language);
    if (open && conversion)
    {
        const bool native = *open && (*conversion & IME_CMODE_NATIVE);
        if (primary == LANG_CHINESE) return native ? L"中" : L"英"; // l10n-allow: intrinsic Chinese IME mode symbols, independent of UI language
        if (primary == LANG_JAPANESE)
            return native ? (*conversion & IME_CMODE_KATAKANA ? L"カ" : L"あ") : L"A";
        if (primary == LANG_KOREAN) return native ? L"가" : L"A";
    }
    if (primary == LANG_CHINESE || primary == LANG_JAPANESE || primary == LANG_KOREAN) return {};
    return abbreviation;
}
inline bool Matches(const Snapshot& value, HWND foreground, DWORD thread, HKL layout, HWND focus = nullptr)
{
    return foreground && thread && layout && value.foreground == foreground &&
        value.focus == focus && value.thread == thread && value.layout == layout;
}

// Last successful presentation, with no expiry. It never chooses an action
// target; commands still use current Windows focus. Menus are not typing targets.
class DisplayCache
{
public:
    Snapshot Get(const Snapshot& sample, const Snapshot& target, bool menu)
    {
        if (!menu && !sample.label.empty() &&
            Matches(sample, target.foreground, target.thread, target.layout, target.focus)) shown_ = sample;
        return shown_;
    }
private:
    Snapshot shown_;
};
}

// Internal to the status bar. One bounded background sampler for every monitor;
// no reading composition text, global input hooks or cross-thread input attachment.
// Construct, Current, Show and destruction are called on the owning UI thread.
class Service final
{
public:
    Service();
    ~Service();
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;
    Snapshot Current() const;
    HRESULT Show(RECT anchor, bool context);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
