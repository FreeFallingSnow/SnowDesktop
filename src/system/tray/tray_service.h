#pragma once
#include "tray_focus.h"
#include "tray_menu_placement.h"
#include "tray_change_notifications.h"
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace snowdesktop::tray
{
struct Icon
{
    Identity identity;
    std::string key, persistentKey;
    std::wstring tip, application;
    bool tipKnown = false;
    DWORD callback = 0, version = 0, state = 0;
    DWORD width = 0, height = 0;
    std::vector<std::uint32_t> pixels;
};
struct Snapshot
{
    bool connected = false, degraded = false;
    DWORD error = 0;
    std::uint64_t revision = 0;
    std::vector<Icon> icons;
};
// Pure model operations are shared by the live collector and protocol tests.
std::string Key(const Identity& identity);
bool Apply(std::vector<Icon>& icons, const Event& event);
enum class Activation { LeftDown, LeftUp, DoubleClick, RightDown, RightUp, Keyboard, ContextKeyboard, Hover, Leave };
std::vector<Callback> Callbacks(const Icon& icon, Activation activation, POINT anchor);
struct FocusOrigin { HWND target = nullptr, source = nullptr; };
// Host-private synchronous updates. Keys not present in this batch keep the
// geometry supplied by other tray presenters.
struct GeometryUpdate { std::string key; RECT rect{}; };

class Service
{
public:
    Service();
    ~Service();
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;
    Snapshot Current() const;
    void RefreshNativeTooltips();
    void SubscribeChanges(HWND window);
    void UnsubscribeChanges(HWND window);
    bool AcknowledgeChange(HWND window, std::uint64_t token);
    void SetGeometry(const std::string& key, RECT rect);
    void SetGeometries(std::span<const GeometryUpdate> updates);
    void ClearGeometries();
    bool Activate(const std::string& key, Activation action, POINT anchor, FocusOrigin focus = {},
        std::optional<RECT> invocationBounds = {}, std::optional<Identity> expectedIdentity = {});
    bool ActivatePrimary(const std::string& key, bool doubleClick, POINT anchor,
        FocusOrigin focus = {}, std::optional<RECT> invocationBounds = {}, std::optional<Identity> expectedIdentity = {});
    void CancelFocusReturn(HWND origin = nullptr);
    void ObserveForeground(HWND window, DWORD eventTime);
    std::optional<FocusDelivery> TakeFocusReturn(HWND origin, std::uint64_t serial);
    MenuPopupBindings MenuPopups(HWND target) const;
    void OpenNativeTray();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
