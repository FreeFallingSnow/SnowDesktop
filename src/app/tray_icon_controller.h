#pragma once

#include <windows.h>

#include <string>

enum class TrayCallbackAction
{
    None,
    ShowContextMenu,
    ReloadItems,
};

/** Owns the Windows notification icon and its resource lifetime. */
class TrayIconController
{
public:
    TrayIconController() = default;
    ~TrayIconController();

    TrayIconController(const TrayIconController&) = delete;
    TrayIconController& operator=(
        const TrayIconController&) = delete;

    bool Add(HWND owner, bool force = false);
    void Remove(HWND fallbackOwner = nullptr);
    bool ShowBalloon(
        HWND owner,
        const std::wstring& title,
        const std::wstring& message);

    bool IsAdded() const { return added_; }
    static TrayCallbackAction ClassifyCallback(LPARAM value);

private:
    HICON icon_ = nullptr;
    HWND owner_ = nullptr;
    bool added_ = false;
};
