#pragma once
#include <windows.h>
#include <imm.h>
#include <memory>
#include <optional>
#include <string>

namespace snowdesktop::status_bar_input_method
{
struct Snapshot
{
    HWND foreground = nullptr;
    HWND focus = nullptr;
    DWORD thread = 0;
    HKL layout = nullptr;
    std::wstring label, description;
    friend bool operator==(const Snapshot&, const Snapshot&) = default;
};

namespace detail
{
// A language is not a conversion mode. If IMM cannot answer, show the language
// abbreviation, never infer Chinese/native input merely from the locale.
inline std::wstring Label(LANGID language, std::wstring abbreviation,
    std::optional<bool> open, std::optional<DWORD> conversion)
{
    const auto primary = PRIMARYLANGID(language);
    if (open && conversion)
    {
        const bool native = *open && (*conversion & IME_CMODE_NATIVE);
        if (primary == LANG_CHINESE) return native ? L"中" : L"英";
        if (primary == LANG_JAPANESE)
            return native ? (*conversion & IME_CMODE_KATAKANA ? L"カ" : L"あ") : L"A";
        if (primary == LANG_KOREAN) return native ? L"가" : L"A";
    }
    return abbreviation.empty() ? L"—" : abbreviation;
}
inline bool Matches(const Snapshot& value, HWND foreground, DWORD thread, HKL layout, HWND focus = nullptr)
{
    return foreground && thread && layout && value.foreground == foreground &&
        value.focus == focus && value.thread == thread && value.layout == layout;
}
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
    HRESULT Show(RECT anchor);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
