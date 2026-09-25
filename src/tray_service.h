#pragma once
#include "tray_focus.h"
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace snowdesktop::tray
{
struct Icon
{
    Identity identity;
    std::string key, persistentKey;
    std::wstring tip, application;
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

class Service
{
public:
    Service();
    ~Service();
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;
    Snapshot Current() const;
    void SetGeometry(const std::string& key, RECT rect);
    void ClearGeometries();
    bool Activate(const std::string& key, Activation action, POINT anchor, FocusOrigin focus = {});
    void CancelFocusReturn(HWND origin = nullptr);
    void ObserveForeground(HWND window, DWORD eventTime);
    std::optional<FocusDelivery> TakeFocusReturn(HWND origin, std::uint64_t serial);
    void OpenNativeTray();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
