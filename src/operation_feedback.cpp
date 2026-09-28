#include "operation_feedback.h"
#include "operation_recovery_dialog.h"
#include <commctrl.h>
#include "diagnostic_log.h"
#include "l10n.h"
#include "launcher_messages.h"
#include "language_fallback.h"
#include <deque>
#include <iterator>
#include <vector>

namespace snowdesktop::operation_feedback
{
namespace
{
constexpr UINT kDeliver = WM_APP + 1;
std::wstring Describe(const Failure& failure)
{
    std::wstring message = Locale::Instance().TrW(failure.messageKey.c_str());
    if (failure.messageKey == "app.operation.startFailed" && message == L"app.operation.startFailed")
    {
        wchar_t locale[LOCALE_NAME_MAX_LENGTH]{};
        GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH);
        char name[LOCALE_NAME_MAX_LENGTH * 4]{};
        WideCharToMultiByte(CP_UTF8, 0, locale, -1, name, sizeof(name), nullptr, nullptr);
        std::vector<std::string> languages;
        for (const auto& entry : kLauncherMessages) languages.emplace_back(entry.language);
        auto selected = localization::ResolveBestLanguage(languages, name);
        if (selected.empty()) selected = "en-US";
        for (const auto& entry : kLauncherMessages)
            if (selected == entry.language) message = entry.startup;
    }
    if (!failure.detail.empty()) message += L"\n\n" + failure.detail;
    if (failure.error)
    {
        wchar_t text[1024]{};
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, failure.error, 0, text, static_cast<DWORD>(std::size(text)), nullptr);
        message += L"\n\n" + _LFW("app.operation.errorCode", std::to_wstring(failure.error));
        if (*text) message += L"\n" + std::wstring(text);
    }
    return message;
}
}

void Show(const Failure& failure)
{
    if (failure.error == ERROR_CANCELLED || failure.error == static_cast<DWORD>(E_ABORT) ||
        failure.error == static_cast<DWORD>(HRESULT_FROM_WIN32(ERROR_CANCELLED))) return;
    const auto message = Describe(failure);
    WriteDiagnosticLogEntry(message.c_str(), failure.warning
        ? DiagnosticLogLevel::Warning : DiagnosticLogLevel::Error);
    MessageBoxW(nullptr, message.c_str(), L"SnowDesktop", MB_OK |
        (failure.warning ? MB_ICONWARNING : MB_ICONERROR) | MB_SETFOREGROUND);
}

RecoveryChoice ChooseRecovery(HWND owner, const Failure& failure, const wchar_t* continueLabel)
{
    // A modal loop can dispatch another save request. Keep a single decision
    // dialog; the outer save will serialize the latest model after the choice.
    static thread_local bool choosing = false;
    if (choosing) return RecoveryChoice::Cancel;
    choosing = true;
    struct Reset { bool& value; ~Reset() { value = false; } } reset{choosing};
    const auto message = Describe(failure);
    WriteDiagnosticLogEntry(message.c_str(), DiagnosticLogLevel::Warning);
    const TASKDIALOG_BUTTON buttons[] = {
        { IDRETRY, _LW("app.operation.retry") },
        { IDCONTINUE, continueLabel },
        { IDCANCEL, _LW("settings.dialog.cancel") }
    };
    TASKDIALOGCONFIG dialog{sizeof(dialog)};
    dialog.hwndParent = owner && IsWindow(owner) ? owner : nullptr;
    dialog.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    dialog.pszWindowTitle = L"SnowDesktop";
    dialog.pszMainIcon = TD_WARNING_ICON;
    dialog.pszContent = message.c_str();
    dialog.cButtons = static_cast<UINT>(std::size(buttons));
    dialog.pButtons = buttons;
    dialog.nDefaultButton = IDRETRY;
    int selected = IDCANCEL;
    if (FAILED(TaskDialogIndirect(&dialog, &selected, nullptr, nullptr)))
        selected = MessageBoxW(dialog.hwndParent, message.c_str(), L"SnowDesktop",
            MB_CANCELTRYCONTINUE | MB_ICONWARNING | MB_SETFOREGROUND);
    if (selected == IDRETRY || selected == IDTRYAGAIN) return RecoveryChoice::Retry;
    return selected == IDCONTINUE ? RecoveryChoice::Continue : RecoveryChoice::Cancel;
}

struct Session::State
{
    std::mutex mutex;
    std::deque<Failure> pending;
    HWND window = nullptr;
    DWORD threadId = GetCurrentThreadId();
    bool closed = false;
    bool presenting = false;
    std::wstring last;
    ULONGLONG lastAt = 0;

    void Enqueue(const Failure& failure)
    {
        // Localization has a UI-thread cache. Workers log identifiers and
        // values; translation occurs only when the UI drains this queue.
        const auto description = std::wstring(failure.messageKey.begin(), failure.messageKey.end()) +
            L": " + failure.detail + L" (error " + std::to_wstring(failure.error) + L")";
        WriteDiagnosticLogEntry(description.c_str(), failure.warning
            ? DiagnosticLogLevel::Warning : DiagnosticLogLevel::Error);
        std::lock_guard lock(mutex);
        if (closed) return;
        const auto now = GetTickCount64();
        if (last == description && now - lastAt < 10000) return;
        last = description;
        lastAt = now;
        // Always log every failure; bound pending UI work during an error storm.
        if (pending.size() < 16) pending.push_back(failure);
        if (window) PostMessageW(window, kDeliver, 0, 0);
        else PostThreadMessageW(threadId, kDeliver, 0, 0);
    }

    void Drain()
    {
        // MessageBox pumps messages; a nested delivery must not stack dialogs.
        if (presenting) return;
        presenting = true;
        for (;;)
        {
            Failure failure;
            {
                std::lock_guard lock(mutex);
                if (pending.empty()) break;
                failure = std::move(pending.front());
                pending.pop_front();
            }
            Show(failure);
        }
        presenting = false;
    }

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
    {
        if (message == WM_NCCREATE)
            SetWindowLongPtrW(window, GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == kDeliver && state) { state->Drain(); return 0; }
        return DefWindowProcW(window, message, wp, lp);
    }
};

Session::Session() : state_(std::make_shared<State>())
{
    WNDCLASSW type{};
    type.lpfnWndProc = State::WindowProc;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"SnowDesktopOperationFeedback";
    RegisterClassW(&type);
    state_->window = CreateWindowExW(0, type.lpszClassName, L"", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, type.hInstance, state_.get());
    const std::weak_ptr<State> weak = state_;
    SetReporter([weak](const Failure& failure) {
        if (const auto state = weak.lock()) state->Enqueue(failure);
    });
}

void Session::Drain() { state_->Drain(); }
Session::~Session()
{
    SetReporter({});
    { std::lock_guard lock(state_->mutex); state_->closed = true; }
    if (state_->window) DestroyWindow(state_->window);
}
}
