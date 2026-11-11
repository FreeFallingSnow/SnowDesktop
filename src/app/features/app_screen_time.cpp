#include "app/app.h"

#include "system/screen_time/screen_time_tracker.h"
#include "data/data_paths.h"

#include <cstring>
#include <wtsapi32.h>

namespace
{
// GUID_CONSOLE_DISPLAY_STATE. 0 = off, 1 = on, 2 = dimmed.
constexpr GUID kConsoleDisplayState = {
    0x6FE69556, 0x704A, 0x47A0,
    {0x8F, 0x24, 0xC2, 0x8D, 0x93, 0x6F, 0xDA, 0x47}};
}

void DesktopApp::EnsureScreenTimeTracker()
{
    if (screenTime_) return;
    screenTime_ = std::make_unique<snowdesktop::screen_time::Tracker>(
        GetDataFilePath(L"SnowDesktop.screen-time.json"));
    screenTime_->SetEnabled(generalSettings_.screenTimeEnabled);
}

void DesktopApp::ApplyScreenTimeEnabled()
{
    EnsureScreenTimeTracker();
    screenTime_->SetEnabled(generalSettings_.screenTimeEnabled);
}

void DesktopApp::ObserveScreenTime()
{
    if (screenTime_) screenTime_->Observe();
}

void DesktopApp::RegisterScreenTimeNotifications(HWND window)
{
    if (!window) return;
    if (!screenTimeDisplayNotify_)
    {
        screenTimeDisplayNotify_ = RegisterPowerSettingNotification(
            window, &kConsoleDisplayState, DEVICE_NOTIFY_WINDOW_HANDLE);
    }
    if (!screenTimeSessionRegistered_)
    {
        screenTimeSessionRegistered_ =
            WTSRegisterSessionNotification(window, NOTIFY_FOR_THIS_SESSION) != FALSE;
    }
}

void DesktopApp::UnregisterScreenTimeNotifications()
{
    if (screenTimeDisplayNotify_)
    {
        UnregisterPowerSettingNotification(screenTimeDisplayNotify_);
        screenTimeDisplayNotify_ = nullptr;
    }
    if (screenTimeSessionRegistered_ && hwnd_)
    {
        WTSUnRegisterSessionNotification(hwnd_);
        screenTimeSessionRegistered_ = false;
    }
}

void DesktopApp::HandleScreenTimePower(WPARAM wp, LPARAM lp)
{
    EnsureScreenTimeTracker();
    if (wp == PBT_APMSUSPEND)
    {
        screenTime_->SetSuspended(true);
        return;
    }
    if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND)
    {
        screenTime_->SetSuspended(false);
        return;
    }
    if (wp != PBT_POWERSETTINGCHANGE || !lp) return;
    const auto* setting = reinterpret_cast<const POWERBROADCAST_SETTING*>(lp);
    if (!IsEqualGUID(setting->PowerSetting, kConsoleDisplayState) ||
        setting->DataLength < sizeof(DWORD))
        return;
    DWORD state = 0;
    memcpy(&state, setting->Data, sizeof(state));
    screenTime_->SetDisplayOn(state != 0);
}

void DesktopApp::HandleScreenTimeSession(WPARAM wp)
{
    EnsureScreenTimeTracker();
    if (wp == WTS_SESSION_LOCK) screenTime_->SetSessionLocked(true);
    else if (wp == WTS_SESSION_UNLOCK) screenTime_->SetSessionLocked(false);
}

void DesktopApp::FlushScreenTime()
{
    if (screenTime_) screenTime_->Flush();
}
