#pragma once

#include <windows.h>
#include <commctrl.h>
#include <sherrors.h>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace snowdesktop { class UiAnimationScheduler; }
struct IModalWindow;

namespace snowdesktop::operation_feedback
{
// Internal operation results. Workers report values; only the host UI displays
// them. A cancelled operation is not an error and must not produce a dialog.
struct Failure
{
    std::string messageKey;
    std::wstring detail;
    DWORD error = ERROR_SUCCESS;
    bool warning = false;
    // Recoverable background failures may become obsolete while queued or
    // displayed. Evaluate this only on the presentation thread.
    std::function<bool()> stillRelevant;
};
inline bool ShouldPresent(const Failure& failure) noexcept
{
    try { return !failure.stillRelevant || failure.stillRelevant(); }
    catch (...) { return false; }
}
template<typename Present>
bool PresentIfCurrent(const Failure& failure, Present&& present)
{
    if (!ShouldPresent(failure)) return false;
    present();
    return true;
}
using Reporter = std::function<void(const Failure&)>;
inline std::mutex reporterMutex;
inline Reporter reporter;

inline void SetReporter(Reporter replacement)
{
    std::lock_guard lock(reporterMutex);
    reporter = std::move(replacement);
}

inline void Report(Failure failure) noexcept
{
    if (failure.error == ERROR_CANCELLED || failure.error == static_cast<DWORD>(COPYENGINE_E_USER_CANCELLED) ||
        failure.error == static_cast<DWORD>(E_ABORT) ||
        failure.error == static_cast<DWORD>(HRESULT_FROM_WIN32(ERROR_CANCELLED))) return;
    try
    {
        Reporter callback;
        { std::lock_guard lock(reporterMutex); callback = reporter; }
        if (callback) callback(failure);
        else OutputDebugStringW((failure.detail + L" (error " +
            std::to_wstring(failure.error) + L")\n").c_str());
    }
    catch (...) { OutputDebugStringW(L"SnowDesktop: operation feedback unavailable.\n"); }
}

// These host functions are deliberately independent of the settings process.
void Show(const Failure& failure);
// Native modal loops dispatch HWND messages but do not wait on the host's
// animation timer. These entries keep the current UI session's deadlines and
// presentation moving; startup/worker callers without a session remain valid.
HRESULT ShowTaskDialog(const TASKDIALOGCONFIG* dialog, int* selected,
    int* radioButton, BOOL* verificationChecked);
int ShowMessageBox(HWND owner, const wchar_t* text, const wchar_t* caption,
    UINT type);
HRESULT ShowModalWindow(IModalWindow* dialog, HWND owner);
INT_PTR ShowDialog(HINSTANCE instance, const DLGTEMPLATE* dialog, HWND owner,
    DLGPROC procedure, LPARAM parameter);
class Session
{
public:
    // Bind dialog presentation to the host UI thread. A default session still
    // supports startup/standalone callers without an animation scheduler.
    explicit Session(UiAnimationScheduler* animations = nullptr,
        std::function<void()> flushPresentation = {});
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    void Drain();
private:
    struct State;
    std::shared_ptr<State> state_;
};
}
