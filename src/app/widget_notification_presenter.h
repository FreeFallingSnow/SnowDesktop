#pragma once

#include "../widget_notification_runtime.h"

#include <windows.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

class WidgetNotificationPresenter
{
public:
    using ActionCallback = std::function<void(
        const std::string& notificationId, const std::string& actionId)>;

    WidgetNotificationPresenter() = default;
    ~WidgetNotificationPresenter();

    WidgetNotificationPresenter(const WidgetNotificationPresenter&) = delete;
    WidgetNotificationPresenter& operator=(
        const WidgetNotificationPresenter&) = delete;

    void SetActionCallback(ActionCallback callback)
    {
        actionCallback_ = std::move(callback);
    }

    bool Show(HWND owner,
        const snowdesktop::widget_runtime::WidgetNotificationHostRequest&
            request);
    bool Update(HWND owner,
        const snowdesktop::widget_runtime::WidgetNotificationHostRequest&
            request);
    bool Dismiss(std::string_view notificationId);
    bool Contains(std::string_view notificationId) const;
    void Shutdown();

private:
    struct Toast
    {
        WidgetNotificationPresenter* presenter = nullptr;
        HWND hwnd = nullptr;
        HWND owner = nullptr;
        snowdesktop::widget_runtime::WidgetNotificationHostRequest request;
        HBITMAP image = nullptr;
        RECT closeRect{};
        std::vector<RECT> actionRects;
        int height = 0;

        ~Toast();
    };

    static LRESULT CALLBACK WindowProc(
        HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static bool EnsureWindowClass();

    void Reflow(HWND owner);
    void Paint(Toast& toast);
    void HandleClick(Toast& toast, POINT point);

    ActionCallback actionCallback_;
    std::unordered_map<std::string, std::unique_ptr<Toast>> toasts_;
};
