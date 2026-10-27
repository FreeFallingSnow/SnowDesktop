#include "platform/window_support/window_capabilities.h"
#include "platform/window_support/window_pointer_events.h"

#include <cstdio>
#include <initializer_list>

using namespace snowdesktop::window_support;

int main()
{
    unsigned failed = 0;
    const auto check = [&](bool result, const char* message) {
        if (!result) { std::fprintf(stderr, "FAIL: %s\n", message); ++failed; }
    };
    constexpr Capability operations[] = {Capability::PointerObservation,
        Capability::WindowPin, Capability::WindowControl, Capability::MinimizeAnimation};
    for (const auto operation : operations)
    {
        for (CapabilityMask local = 0; local < 16; ++local)
            for (CapabilityMask remote = 0; remote < 16; ++remote)
                for (const auto state : {ProviderState::Absent, ProviderState::Starting,
                         ProviderState::Ready, ProviderState::Failed})
                {
                    const CapabilitySnapshot snapshot{local, remote, state, 19};
                    const auto route = ResolveCapability(snapshot, operation);
                    const bool hasLocal = Contains(local, operation);
                    const bool hasRemote = state == ProviderState::Ready && Contains(remote, operation);
                    check(route.Available() == (hasLocal || hasRemote), "only established capability enables an operation");
                    check(route.provider == (hasLocal ? Provider::InProcess : hasRemote ? Provider::Helper : Provider::Unavailable),
                        "provider precedence never depends on a permission label");
                    check(route.generation == 19, "route carries its observation generation");
                    check(ResolveCapability(snapshot, operation, {false, true, true}).NeedsFallback(),
                        "stale target cannot be authorized by a ready provider");
                    check(ResolveCapability(snapshot, operation, {true, false, true}).Available() == hasRemote,
                        "target-specific local unavailability permits only an established remote operation");
                    check(ResolveCapability(snapshot, operation, {true, true, false}).Available() == hasLocal,
                        "a ready helper cannot override its target access restriction");
                }
    }
    const CapabilitySnapshot partial{Mask(Capability::WindowControl), Mask(Capability::WindowPin), ProviderState::Ready, 20};
    check(ResolveCapability(partial, Capability::WindowPin).provider == Provider::Helper,
        "pin can use helper while native control remains local");
    check(ResolveCapability(partial, Capability::PointerObservation).NeedsFallback(),
        "pin support does not suppress an unavailable pointer fallback");
    check(ResolveCapability(partial, Capability::None).NeedsFallback(), "empty capability is never usable");
    RecentPointerEvents recent;
    protocol::PointerEvent down{1, {42, -10}, WM_LBUTTONDOWN, 1000, 0, 0, 1};
    auto up = down;
    up.sequence = 2; up.message = WM_LBUTTONUP; up.time = 1001; up.buttons = 0;
    check(recent.Accept(down, 1010) && recent.Accept(up, 1010), "button transitions remain separate events");
    down.sequence = 89;
    check(!recent.Accept(down, 1011) && !recent.Accept(up, 1011), "interleaved native/helper duplicates are delivered once");
    down.time = 700;
    check(!recent.Accept(down, 1011), "delayed events cannot dismiss newly opened content");
    down.time = 0xfffffff0;
    check(recent.Accept(down, 16), "event age handles DWORD tick wrap");
    down.time = 1100; down.point.x = 43;
    check(recent.Accept(down, 1101), "a later distinct press remains actionable");
    HWND fixture = CreateWindowExW(0, L"STATIC", L"Window support identity fixture", WS_POPUP,
        0, 0, 20, 20, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(fixture != nullptr, "native target fixture is created");
    if (fixture)
    {
        auto identity = protocol::Capture(fixture);
        check(protocol::Current(identity), "live HWND is bound to its process and thread");
        ++identity.process;
        check(!protocol::Current(identity), "a copied HWND with another process is rejected");
        identity = protocol::Capture(fixture);
        DestroyWindow(fixture);
        check(!protocol::Current(identity), "destroyed targets invalidate deferred actions");
    }
    std::printf("window support capability matrix: %s\n", failed ? "FAILED" : "passed");
    return failed ? 1 : 0;
}
