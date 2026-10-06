#pragma once
#include "native_tooltip_state.h"
#include "personalization.h"
#include <functional>
#include <memory>

struct IDCompositionDesktopDevice;
struct IDWriteFactory;
struct ID2D1DeviceContext;

namespace snowdesktop
{
// UI-thread-only, nonactivating popup; owns no graphics device or worker.
// The owner must Hide on capture/menu/fullscreen/target removal and Close
// before destroying its HWND. Configure again after graphics device recovery.
class NativeTooltip final
{
public:
    using DrawBackground = std::function<void(ID2D1DeviceContext*, RECT,
        const PersonalizationSettings&, float)>;
    NativeTooltip();
    ~NativeTooltip();
    NativeTooltip(const NativeTooltip&) = delete;
    NativeTooltip& operator=(const NativeTooltip&) = delete;
    void Configure(HWND owner, IDCompositionDesktopDevice* composition,
        IDWriteFactory* text, const PersonalizationSettings& appearance,
        DrawBackground drawBackground = {});
    // A repeated key only updates content/geometry: no Hide, ShowWindow, or
    // hover timer restart. Immediate promotes an existing delayed tooltip.
    void SetTarget(std::string key, std::wstring body, RECT screenAnchor,
        NativeTooltipPlacement placement = NativeTooltipPlacement::Below,
        bool immediate = false, std::wstring title = {});
    void Hide();
    void Close();
    bool Visible() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
