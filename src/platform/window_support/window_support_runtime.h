#pragma once

#include "window_capabilities.h"
#include "window_support_protocol.h"
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace snowdesktop::window_support
{
// One instance belongs to the real desktop host. Settings processes receive
// the status over existing settings IPC and never inspect their own token.
class Runtime final
{
public:
    using Completion = std::function<void(const protocol::Response&)>;
    using PointerCallback = std::function<void(const protocol::PointerEvent&)>;
    Runtime();
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    void Start();
    void Stop();
    // UI-thread publication; worker/Hook callbacks never call DesktopApp.
    void Poll();
    SupportStatus Status() const;
    CapabilityRoute Resolve(Capability capability, HWND target = nullptr) const;
    std::optional<std::uint32_t> PointerButtons() const;
    std::uint32_t PointerPresses() const;
    bool PointerPosition(POINT& point) const;
    void SetPinAppearance(protocol::PinAppearance appearance);
    void SetPinPeek(HWND target);
    void SetPointerCallback(PointerCallback callback);
    void SetNativeCapability(Capability capability, bool available);
    void SetAuthorizationRequestHandler(std::function<void()> handler);
    bool RequestPinAuthorization(HWND target);
    bool Submit(protocol::Request request, Completion completion = {});
    void Configure(bool enabled, HWND owner, std::function<void(DWORD)> completion);
    protocol::Response HelperSnapshot() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
Runtime& WindowSupport();
}
