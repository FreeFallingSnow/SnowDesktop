#pragma once

#include <windows.h>

#include <cstdint>

namespace snowdesktop::application_hang_watchdog
{
enum class RecoveryStage : unsigned
{
    None,
    ResetInput,
    RebindDesktop,
    RestartApplication
};

struct Sample
{
    std::uint64_t now = 0;
    std::uint64_t heartbeat = 0;
    DWORD receivedButtonTime = 0;
    bool enabled = false;
    bool busy = false;
};

// A responsive pump alone does not prove that its attached input queue works.
// Require repeated fresh clicks, separated in time, before requesting repair.
class Monitor
{
public:
    void ObserveClick(std::uint64_t now, DWORD messageTime)
    {
        if (now < nextObservation_) return;
        if (pending_ && now > lastClick_ + 10000) ClearPending();
        if (!pending_)
        {
            pending_ = true;
            firstClick_ = lastClick_ = now;
            firstMessageTime_ = messageTime;
            clicks_ = 1;
        }
        else if (now >= lastClick_ + 1000)
        {
            lastClick_ = now;
            ++clicks_;
        }
    }

    RecoveryStage Poll(const Sample& sample)
    {
        // Suspend/resume and observer scheduling gaps are not input failures.
        const bool gap = lastPoll_ && sample.now > lastPoll_ + 3000;
        lastPoll_ = sample.now;
        if (stage_ != RecoveryStage::None && sample.receivedButtonTime &&
            static_cast<std::int32_t>(sample.receivedButtonTime - recoveryMessageTime_) >= 0)
        {
            confirmed_ = true;
            stage_ = RecoveryStage::None;
            ClearPending();
            nextObservation_ = sample.now + 60000;
        }
        if (gap || !sample.enabled || sample.busy || !sample.heartbeat ||
            sample.now < sample.heartbeat || sample.now - sample.heartbeat > 3000)
        {
            ClearPending();
            return RecoveryStage::None;
        }
        if (pending_ && sample.receivedButtonTime &&
            static_cast<std::int32_t>(sample.receivedButtonTime - firstMessageTime_) >= 0)
        {
            ClearPending();
            stage_ = RecoveryStage::None;
            // Acknowledgement of a late click does not justify immediate repair.
            nextObservation_ = sample.now + 10000;
        }
        if (pending_ && sample.now > lastClick_ + 10000) ClearPending();
        if (!pending_ || clicks_ < 2 || sample.now < firstClick_ + 5000 ||
            stage_ == RecoveryStage::RestartApplication)
            return RecoveryStage::None;
        stage_ = static_cast<RecoveryStage>(static_cast<unsigned>(stage_) + 1);
        recoveryMessageTime_ = static_cast<DWORD>(sample.now);
        ClearPending();
        // Each escalation requires NEW failed clicks after the previous repair.
        // Old queued clicks and an idle desktop must never cause escalation.
        nextObservation_ = sample.now + 10000;
        return stage_;
    }

    bool TakeRecoveryConfirmation()
    {
        const bool result = confirmed_;
        confirmed_ = false;
        return result;
    }

    void ClearPending()
    {
        pending_ = false;
        clicks_ = 0;
    }

private:
    bool pending_ = false;
    unsigned clicks_ = 0;
    DWORD firstMessageTime_ = 0;
    DWORD recoveryMessageTime_ = 0;
    bool confirmed_ = false;
    std::uint64_t firstClick_ = 0, lastClick_ = 0, lastPoll_ = 0;
    std::uint64_t nextObservation_ = 0;
    RecoveryStage stage_ = RecoveryStage::None;
};

// UI-thread scope. The existing restricted child watcher reads these small
// window properties; no debugger, global mouse hook or injected DLL is needed.
class UiSession
{
public:
    explicit UiSession(HWND control);
    ~UiSession();
    UiSession(const UiSession&) = delete;
    UiSession& operator=(const UiSession&) = delete;
    static void Publish(bool enabled, bool busy);
    static RecoveryStage TakeRequest();

private:
    static LRESULT CALLBACK ObserveMessage(int code, WPARAM wp, LPARAM lp);
    static void CALLBACK Heartbeat(HWND, UINT, UINT_PTR, DWORD);
    HWND control_ = nullptr;
    HHOOK hook_ = nullptr;
    UINT_PTR timer_ = 0;
};

// These use the same transport as production and permit isolated Win32 tests.
Sample ReadSample(HWND control, std::uint64_t now);
bool PostRecoveryRequest(HWND control, RecoveryStage stage);

// Reparent the existing child through the desktop without changing its model,
// HWND or final styles. Refuse DPI-context changes before touching the window.
DWORD RebindWindow(HWND child, HWND parent);
}
