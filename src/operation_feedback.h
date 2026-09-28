#pragma once

#include <windows.h>
#include <sherrors.h>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

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
};
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
class Session
{
public:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    void Drain();
private:
    struct State;
    std::shared_ptr<State> state_;
};
}
