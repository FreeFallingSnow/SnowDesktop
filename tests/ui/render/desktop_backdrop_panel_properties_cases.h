#pragma once

#include "app/render/desktop_backdrop_compositor.h"
#include "diagnostics/native_trace_observation.h"
#include <d2d1_1helper.h>
#include <array>
#include <numeric>

namespace snowdesktop::test::backdrop_properties
{
// Observe successful native setters in the real compositor, rather than a
// second implementation of the cache. HWND fences remain real as well; no
// external desktop window, user data or rasterization result is substituted.
inline std::array<unsigned, 6> submissions{};
inline float lastOpacity = -1;
inline std::uintptr_t lastOwner = 0;
struct Observation
{
    NativeTraceObservation trace{[](const NativeTraceEvent& event) {
        if (event.kind == NativeTraceEvent::Instant && event.name == "numeric.operation")
        { lastOwner = static_cast<std::uintptr_t>(event.operation); return; }
        if (event.kind != NativeTraceEvent::Counter) return;
        constexpr std::array names{"backdrop.panel.transform", "backdrop.panel.offset",
            "backdrop.panel.size", "backdrop.panel.radius", "backdrop.panel.clip", "backdrop.panel.opacity"};
        for (std::size_t index = 0; index < names.size(); ++index)
            if (event.name == names[index])
            {
                ++submissions[index];
                if (index == 5) lastOpacity = static_cast<float>(event.value);
            }
    }};
};

inline unsigned Total() { return std::accumulate(submissions.begin(), submissions.end(), 0u); }
inline void Clear() { submissions = {}; lastOpacity = -1; lastOwner = 0; }

template<class Check, class FindHelper>
void Run(HWND content, Check check, FindHelper findHelper)
{
    Observation observation;
    DesktopBackdropCompositor glass;
    if (!check(glass.InitializePopup(content, false, false),
            "property-cache fixture creates a hidden native compositor")) return;
    constexpr std::uintptr_t owner = 91001;
    RECT frame{10, 10, 90, 90};
    glass.BeginFrame(true);
    check(glass.AddPanel(frame, 18, 24, owner), "create the initial owned panel");
    glass.EndFrame(false);
    Clear();
    for (int iteration = 0; iteration < 4; ++iteration)
    {
        glass.BeginFrame(true);
        check(glass.AddPanel(frame, 18, 24, owner), "repeat the stationary owned panel");
        glass.EndFrame(false);
    }
    check(Total() == 0 && glass.PanelCount() == 1,
        "stationary frames submit no unchanged native panel properties");

    OffsetRect(&frame, 20, 0);
    Clear();
    check(glass.AddPanel(frame, 18, 24, owner) && submissions[1] == 1 && Total() == 1,
        "moving a panel submits only its offset and retains its visual identity");
    frame.right += 10;
    Clear();
    check(glass.AddPanel(frame, 18, 24, owner) && submissions[2] == 1 && Total() == 1,
        "resizing submits the native visual and geometry sizes without unrelated properties");
    Clear();
    check(glass.AddPanel(frame, 12, 24, owner) && submissions[3] == 1 && Total() == 1,
        "radius changes still update the rounded geometry and smooth path");

    Clear();
    check(glass.SetPanelOpacity(frame, 0) && glass.SetPanelOpacity(frame, 0) &&
            submissions[5] == 1 && lastOpacity == 0,
        "repeated hidden hand-off submits opacity once");
    check(glass.AddPanel(frame, 12, 24, owner) && submissions[5] == 2 && lastOpacity == 1,
        "ordinary repaint restores a previously hidden panel even at unchanged geometry");

    constexpr RECT projected{140, 40, 230, 120};
    const auto matrix = D2D1::Matrix4x4F::Translation(110, 30, 0);
    Clear();
    check(glass.SetPanelTransform(owner, matrix, projected) &&
            glass.SetPanelTransform(owner, matrix, projected) && submissions[0] == 1,
        "equal animation matrices reuse the submitted matrix");
    glass.EndFrame(false);
    HRGN region = CreateRectRgn(0, 0, 0, 0);
    const HWND helper = findHelper(glass);
    check(region && helper && GetWindowRgn(helper, region) != ERROR && PtInRegion(region, 180, 80),
        "projected animation bounds still reach the real helper window fence");
    check(glass.AddPanel(frame, 12, 24, owner) && submissions[0] == 2,
        "normal painting resets an animated matrix exactly once");
    glass.EndFrame(false);
    check(region && GetWindowRgn(helper, region) != ERROR &&
            PtInRegion(region, 60, 50) && !PtInRegion(region, 180, 80),
        "normal painting restores the original fence after the projected pose");
    if (region) DeleteObject(region);

    check(glass.SetPanelOpacity(frame, .4f) &&
            glass.SetGenieTransform(frame, {130, 180, 180, 220}, 0, .3f, .8f),
        "Genie takes over a partially transparent real panel");
    Clear();
    check(glass.SetPanelOpacity(frame, .6f), "update opacity while Genie owns the panel");
    glass.ClearGenieTransform();
    check(submissions[5] == 1 && lastOpacity == .6f,
        "Genie exit restores the latest logical opacity into the ordinary panel cache");
    check(glass.AddPanel(frame, 12, 24, owner) && submissions[5] == 2 && lastOpacity == 1,
        "repaint after Genie exit restores ordinary visibility");

    check(glass.AddIconPanel(frame, IconBeautifyShape::Circle, 24, owner), "switch the owner to a custom icon mask");
    Clear();
    check(glass.AddIconPanel(frame, IconBeautifyShape::Circle, 24, owner) && Total() == 0,
        "a stationary custom icon mask does not round-trip through an ordinary clip");
    Clear();
    check(glass.AddPanel(frame, 0, 24, owner, false) && submissions[4] == 1,
        "an ordinary panel restores its rounded clip after a custom mask");
    Clear();
    check(glass.AddIconPanel(frame, IconBeautifyShape::Circle, 24, owner) && submissions[4] == 1,
        "returning to a cached icon mask restores that mask once");
    check(glass.AddLargeIconPanel(frame, 2, 0, 24, owner), "install a real large-icon silhouette");
    Clear();
    check(glass.AddLargeIconPanel(frame, 2, 0, 24, owner) && Total() == 0,
        "stationary large-icon silhouettes retain their final clip");
    Clear();
    check(!glass.AddLargeIconPanel(frame, 999, 0, 24, owner) && glass.IsAvailable() && submissions[4] == 1,
        "failed custom outlines retain the original ordinary-clip fallback");
    Clear();
    check(glass.AddLargeIconPanel(frame, 2, 0, 24, owner) && submissions[4] == 1,
        "a cached custom outline restores correctly after a failed outline request");

    // Reallocation and ordered erase must not leave an owner pointing at its
    // old neighbour. The correlation below comes from that actual neighbour.
    glass.BeginFrame(true);
    for (std::uintptr_t key = 92001; key <= 92016; ++key)
        check(glass.AddPanel({10, 10, 70, 70}, 8, 24, key), "grow the owner-indexed native visual collection");
    glass.EndFrame(false);
    check(glass.PanelCount() == 16 && glass.RemovePanel({}, 92001) &&
            glass.RemovePanel({}, 92008) && glass.RemovePanel({}, 92016),
        "retire the first, middle and last owner without reordering survivors");
    Clear();
    check(glass.SetPanelTransform(92009, matrix, projected) && lastOwner == performance::OperationId(92009) &&
            !glass.SetPanelTransform(92008, matrix, projected),
        "surviving owner lookup follows ordered erasure and removed owners are absent");
    check(glass.AddPanel(frame, 12, 24, 92008) && glass.PanelCount() == 14 &&
            glass.AddPanel(frame, 12, 24) && glass.PanelCount() == 15,
        "a reused owner creates one fresh visual while anonymous identity stays separate");

    check(glass.BeginStagedFrame(), "start owner-indexed candidate collection");
    check(glass.AddPanel({10, 10, 70, 70}, 8, 24, 93001) &&
            glass.AddPanel({80, 10, 140, 70}, 8, 24, 93002) &&
            glass.AddPanel({10, 80, 70, 140}, 8, 24, 93003) &&
            glass.RemovePanel({}, 93001) && glass.AddPanel(frame, 12, 24, 93003),
        "stage a moved owner after erasing a preceding candidate");
    check(glass.ApplyStagedFrame() && glass.PanelCount() == 2 && glass.HasPanelContaining(frame),
        "staged owner replacement and erase publish exactly the surviving candidates");
    check(glass.BeginStagedFrame() && glass.AddPanel(frame, 12, 24, owner), "stage a discardable owner");
    glass.DiscardStagedFrame();
    check(glass.BeginStagedFrame() && glass.AddPanel(frame, 12, 24, owner) &&
            glass.ApplyStagedFrame() && glass.PanelCount() == 1,
        "discarding a candidate clears its owner index before the next collection");

    glass.Reset();
    check(glass.InitializePopup(content, false, false), "recreate the native target after reset");
    Clear();
    check(glass.AddPanel(frame, 12, 24, owner) && glass.PanelCount() == 1 &&
            submissions[0] == 1 && submissions[1] == 1 && submissions[2] == 1 &&
            submissions[3] == 1 && submissions[4] == 1 && submissions[5] == 1,
        "target recreation discards both owner indexes and every submitted-property cache");
}
}
