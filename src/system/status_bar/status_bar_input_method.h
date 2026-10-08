#pragma once
#include <windows.h>
#include <imm.h>
#include <msctf.h>
#include "ui/render/ui_animation_scheduler.h"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <utility>

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
Selection CaptureSelection(const Snapshot& target);
HRESULT Select(const Selection&, const Choice&);

// A live target is separate from the last successful display sample. No cached
// application becomes a command target when Windows temporarily has no foreground.
inline bool SameTarget(const Snapshot& expected, const Snapshot& current)
{
    return expected.foreground && expected.thread && current.thread && current.layout && !current.menuActive &&
        expected.foreground == current.foreground &&
        (!expected.focus || (expected.thread == current.thread && expected.focus == current.focus));
}
struct TargetCallbacks
{
    std::function<Snapshot()> sample, desktop;
    std::function<bool(const Snapshot&)> valid;
    std::function<HRESULT(const Snapshot&)> activate;
    std::function<bool(UiScheduleToken)> current;
    std::function<void(UiScheduleToken, HRESULT, Snapshot)> finished;
    std::function<void(const wchar_t*, const Snapshot&, const Snapshot&, HRESULT, unsigned, double)> trace;
    std::function<double()> nowMilliseconds = UiAnimationScheduler::MonotonicMilliseconds;
};
TargetCallbacks WindowsTargetCallbacks();

// nullopt acquires a target before showing the panel; a supplied snapshot
// restores that target after closing it. Keep Windows message processing alive
// while cross-queue activation restores focus. Native calls are the test seam;
// cancellation, scheduling, deadlines and completion run through this same path.
inline UiScheduleToken ScheduleTarget(UiAnimationScheduler& scheduler,
    std::optional<Snapshot> target, TargetCallbacks callbacks)
{
    struct Progress { bool requested = false; HWND initial = nullptr; unsigned polls = 0; };
    const auto progress = std::make_shared<Progress>();
    const double started = callbacks.nowMilliseconds();
    const double deadline = started + (target ? 750 : 250);
    return scheduler.ScheduleInterval(16,
        [&scheduler, target = std::move(target), callbacks = std::move(callbacks), progress, started, deadline](auto token) {
            Snapshot observed;
            const auto trace = [&](const wchar_t* stage, HRESULT result) {
                if (callbacks.trace) callbacks.trace(stage, target.value_or(Snapshot{}), observed,
                    result, progress->polls, callbacks.nowMilliseconds() - started);
            };
            const auto finish = [&](const wchar_t* stage, HRESULT result, Snapshot resolved = {}) {
                scheduler.Cancel(token);
                trace(stage, result);
                callbacks.finished(token, result, std::move(resolved));
            };
            if (!callbacks.current(token))
            { finish(L"target-cancelled", HRESULT_FROM_WIN32(ERROR_CANCELLED)); return; }
            observed = callbacks.sample();
            if (++progress->polls == 1) trace(target ? L"restore-start" : L"capture-start", S_OK);
            if (!target)
            {
                if (callbacks.valid(observed) && !observed.menuActive)
                { finish(L"capture-ready", S_OK, observed); return; }
                if (callbacks.nowMilliseconds() < deadline) return;
                // Persistent absence of a foreground window is a valid desktop
                // scenario. Never substitute the desktop for a known app/menu.
                if (!observed.foreground)
                {
                    const auto desktop = callbacks.desktop();
                    if (callbacks.valid(desktop))
                    { observed = desktop; finish(L"capture-desktop", S_OK, desktop); return; }
                }
                finish(L"capture-unavailable", HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
                return;
            }
            if (!callbacks.valid(*target))
            { finish(L"restore-invalid", HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE)); return; }
            if (SameTarget(*target, observed))
            { finish(L"restore-ready", S_OK, observed); return; }
            if (callbacks.nowMilliseconds() >= deadline)
            { finish(L"restore-timeout", HRESULT_FROM_WIN32(ERROR_TIMEOUT)); return; }
            if (!progress->requested)
            {
                progress->requested = true;
                progress->initial = observed.foreground;
                // A different child focus in the same app may still be settling.
                // Request activation only once, and do not repeatedly steal focus.
                if (observed.foreground != target->foreground)
                {
                    const HRESULT result = callbacks.activate(*target);
                    trace(L"restore-activation", result);
                    if (FAILED(result)) { finish(L"restore-denied", result); return; }
                    if (!callbacks.current(token))
                    { finish(L"target-cancelled", HRESULT_FROM_WIN32(ERROR_CANCELLED)); return; }
                }
            }
            else if (observed.foreground && observed.foreground != target->foreground &&
                observed.foreground != progress->initial)
                finish(L"restore-focus-moved", HRESULT_FROM_WIN32(ERROR_CANCELLED));
        });
}

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
