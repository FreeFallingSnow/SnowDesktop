#include "widget_composition_layer_rules.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace rules = snowdesktop::widget_composition_layer_rules;

namespace
{
void Check(bool condition, const char* message)
{
    if (condition) return;
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
}

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}
}

int main(int argc, char** argv)
{
    using rules::DesktopLayer;
    Check(rules::IsAbove(
            DesktopLayer::Widget, DesktopLayer::Background),
        "widget surfaces must stay above the desktop background");
    Check(rules::IsAbove(
            DesktopLayer::Foreground, DesktopLayer::Widget),
        "desktop foreground content must cover every widget layer");
    Check(rules::IsAbove(
            DesktopLayer::AnimationOverlay, DesktopLayer::Foreground),
        "animation snapshots must stay above the desktop foreground");
    Check(rules::NullReferenceInsertsAboveAll(false) &&
            !rules::NullReferenceInsertsAboveAll(true),
        "a null DirectComposition reference reverses the intuitive insertAbove order");

    using rules::CompositionHost;
    Check(rules::BelongsToCompositionRoot(
            CompositionHost::Desktop,
            CompositionHost::Desktop) &&
            rules::BelongsToCompositionRoot(
                CompositionHost::FloatingPopup,
                CompositionHost::FloatingPopup) &&
            !rules::BelongsToCompositionRoot(
                CompositionHost::FloatingPopup,
                CompositionHost::Desktop) &&
            !rules::BelongsToCompositionRoot(
                CompositionHost::Desktop,
                CompositionHost::FloatingPopup),
        "composition roots must reject visuals owned by another host");

    Check(rules::ShouldPresentWidgetSurface(true, false),
        "a visible widget must present its child surface");
    Check(!rules::ShouldPresentWidgetSurface(false, false),
        "a hidden widget must hide its child surface");
    Check(!rules::ShouldPresentWidgetSurface(true, true),
        "a move or resize preview source must hide its child surface");
    Check(rules::kWidgetSurfaceBorderOverdraw == 3 &&
            rules::WidgetSurfaceOrigin(120) == 117 &&
            rules::WidgetSurfaceOrigin(-120) == -123 &&
            rules::WidgetSurfaceExtent(80) == 86,
        "compact widget surfaces must reserve the maximum dimensional border overdraw");

    using rules::PointerVisualLayer;
    Check(rules::NeedsWidgetSurfaceRefresh(
            PointerVisualLayer::Widget),
        "widget hover feedback must refresh its owning child surface");
    Check(!rules::NeedsDesktopPaint(
            PointerVisualLayer::Widget),
        "widget-only hover feedback must not repaint the desktop surfaces");
    Check(rules::NeedsDesktopPaint(
            PointerVisualLayer::Background) &&
            rules::NeedsDesktopPaint(
                PointerVisualLayer::Foreground),
        "desktop icons and foreground overlays still require desktop paint");
    Check(rules::NeedsBackgroundPaint(
            PointerVisualLayer::Background) &&
            !rules::NeedsBackgroundPaint(
                PointerVisualLayer::Foreground),
        "only desktop icon feedback belongs to the root surface");
    Check(rules::NeedsForegroundPaint(
            PointerVisualLayer::Foreground) &&
            !rules::NeedsForegroundPaint(
                PointerVisualLayer::Widget),
        "only shared overlay feedback belongs to the foreground surface");

    Check(!rules::ShouldDeferWidgetSurfaceDraw(false, false, false) &&
            rules::ShouldDeferWidgetSurfaceDraw(true, false, false) &&
            rules::ShouldDeferWidgetSurfaceDraw(false, true, false) &&
            rules::ShouldDeferWidgetSurfaceDraw(false, false, true),
        "a widget child surface must wait for every host surface to leave BeginDraw");
    Check(rules::SurfaceIncludesDesktop("") &&
            rules::SurfaceIncludesDesktop("desktop") &&
            !rules::SurfaceIncludesDesktop("panel") &&
            !rules::SurfaceIncludesDesktop("popover"),
        "auxiliary-surface invalidation must not redraw the desktop widget surface");
    Check(rules::SurfaceIncludesAuxiliary("") &&
            rules::SurfaceIncludesAuxiliary("panel") &&
            rules::SurfaceIncludesAuxiliary("dialog") &&
            rules::SurfaceIncludesAuxiliary("popover") &&
            !rules::SurfaceIncludesAuxiliary("desktop"),
        "desktop-only invalidation must not redraw an auxiliary surface");

    rules::WidgetDragFeedbackState presentedFeedback{};
    rules::WidgetDragFeedbackState currentFeedback{};
    currentFeedback.active = true;
    currentFeedback.pageId = L"page-a";
    Check(rules::NeedsWidgetDragFeedbackPresent(
            presentedFeedback, currentFeedback),
        "the first active widget drag target must present its feedback");
    presentedFeedback = currentFeedback;
    Check(!rules::NeedsWidgetDragFeedbackPresent(
            presentedFeedback, currentFeedback),
        "pointer-only movement inside one widget drag target must not redraw feedback");
    currentFeedback.column = 1;
    Check(rules::NeedsWidgetDragFeedbackPresent(
            presentedFeedback, currentFeedback),
        "moving to another widget grid cell must redraw feedback");
    presentedFeedback = currentFeedback;
    currentFeedback.dockTarget = true;
    currentFeedback.dockOwner = 1;
    currentFeedback.dockInsertIndex = 2;
    Check(rules::NeedsWidgetDragFeedbackPresent(
            presentedFeedback, currentFeedback),
        "changing the widget Dock insertion target must redraw feedback");
    presentedFeedback = currentFeedback;
    currentFeedback.navigationSide = 1;
    Check(rules::NeedsWidgetDragFeedbackPresent(
            presentedFeedback, currentFeedback),
        "changing widget page navigation feedback must redraw it");
    currentFeedback.active = false;
    Check(!rules::NeedsWidgetDragFeedbackPresent(
            presentedFeedback, currentFeedback),
        "an inactive widget gesture must not request another feedback frame");

    Check(argc == 2, "source root argument is provided");
    if (argc == 2)
    {
        const std::filesystem::path root(argv[1]);
        const std::string composition = ReadFile(
            root / "src" / "app" / "app_widget_composition.cpp");
        const std::string scene = ReadFile(
            root / "src" / "app" / "app_scene_render.cpp");
        const std::string invalidation = ReadFile(
            root / "src" / "app" / "app_run.cpp");
        const std::string marquee = ReadFile(
            root / "src" / "app" /
                "app_widget_marquee_composition.cpp");
        const std::string engine = ReadFile(
            root / "src" / "widget_engine.cpp");
        const std::string luaWidget = ReadFile(
            root / "src" / "widgets" / "lua_script.cpp");
        const std::string pointer = ReadFile(
            root / "src" / "app" / "app_pointer_move.cpp");
        const std::string scrolling = ReadFile(
            root / "src" / "app" / "app_scroll_interaction.cpp");
        const std::string foreground = ReadFile(
            root / "src" / "app" /
                "app_desktop_foreground_composition.cpp");
        const std::string animationOverlay = ReadFile(
            root / "src" / "app" /
                "app_composition_animation_overlay.cpp");
        const std::string floatingDock = ReadFile(
            root / "src" / "app" /
                "app_floating_dock_window.cpp");
        const std::string floatingPopup = ReadFile(
            root / "src" / "app" /
                "app_floating_popup_window.cpp");

        const std::size_t queueBegin = composition.find(
            "bool DesktopApp::QueueDesktopWidgetComposition(");
        const std::size_t flushBegin = composition.find(
            "bool DesktopApp::FlushPendingDesktopWidgetComposition(",
            queueBegin);
        const std::string queue = queueBegin == std::string::npos ||
                flushBegin == std::string::npos
            ? std::string{}
            : composition.substr(queueBegin, flushBegin - queueBegin);
        Check(!queue.empty(),
            "generic desktop widget composition queue is readable");
        Check(queue.find("widget.type") == std::string::npos &&
                queue.find("audio.output.analysis") == std::string::npos,
            "child-surface ownership must not depend on widget type or data subscription");
        Check(scene.find("QueueDesktopWidgetComposition(widgetData.id)") !=
                std::string::npos,
            "the desktop scene must route visible widgets to child surfaces");
        const std::size_t rootBegin = scene.find(
            "void DesktopApp::DrawStaticBackground(");
        const std::size_t foregroundBegin = scene.find(
            "void DesktopApp::DrawDesktopForeground(", rootBegin);
        const std::string rootDraw = rootBegin == std::string::npos ||
                foregroundBegin == std::string::npos
            ? std::string{}
            : scene.substr(rootBegin, foregroundBegin - rootBegin);
        Check(!rootDraw.empty() &&
                rootDraw.find("wc->DrawChrome(") == std::string::npos &&
                rootDraw.find("widget->Draw(") == std::string::npos &&
                rootDraw.find("Fallback") == std::string::npos &&
                rootDraw.find("fallback") == std::string::npos,
            "the root surface must never render a widget fallback");
        Check(invalidation.find(
                "QueueDesktopWidgetComposition(widgetId)") !=
                std::string::npos,
            "Lua invalidations must update the owning child surface directly");
        Check(invalidation.find(
                "SurfaceIncludesDesktop(surface)") !=
                std::string::npos &&
                invalidation.find(
                    "SurfaceIncludesAuxiliary(surface)") !=
                    std::string::npos &&
                invalidation.find(
                    "InvalidateFloatingPopupWindow(false)") !=
                    std::string::npos,
            "Lua invalidation must preserve its desktop or auxiliary surface scope");
        Check(marquee.find(
                "parent->second.visual->AddVisual(") !=
                std::string::npos &&
                marquee.find("widgetMarqueeCompositionLayer_") ==
                std::string::npos,
            "native marquees must remain children of their widget visual");
        Check(engine.find("RealtimeCompositionCallback") ==
                std::string::npos &&
                engine.find("realtimeCompositionCallback_") ==
                std::string::npos,
            "the widget runtime must not contain subscription-based composition promotion");
        const std::size_t materialPass = luaWidget.find(
            "materialEffects, !preview && registerBackdrop");
        const std::size_t componentBackground = luaWidget.find(
            "SafeRenderBackgroundLayer(", materialPass);
        const std::size_t materialOverlay = luaWidget.find(
            "&backgroundEffects, false", componentBackground);
        const std::size_t widgetForeground = luaWidget.find(
            "SafeRenderWidget(", materialOverlay);
        Check(materialPass != std::string::npos &&
                componentBackground != std::string::npos &&
                materialOverlay != std::string::npos &&
                widgetForeground != std::string::npos &&
                materialPass < componentBackground &&
                componentBackground < materialOverlay &&
                materialOverlay < widgetForeground,
            "Lua background layers must render after the material tint and before acrylic, border, and widget foreground content");
        Check(pointer.find(
                "NeedsWidgetSurfaceRefresh(visual.layer)") !=
                std::string::npos &&
                pointer.find(
                    "QueueDesktopWidgetComposition(visual.widget->id)") !=
                std::string::npos,
            "pointer hover must route widget feedback directly to its child surface");
        Check(scrolling.find(
                "QueueDesktopWidgetComposition(widgets_[luaWidget].id)") !=
                std::string::npos &&
                scrolling.find(
                    "QueueDesktopWidgetComposition(data->id)") !=
                std::string::npos,
            "widget wheel input must refresh only the owning child surface");
        Check(foreground.find(
                "bool DesktopApp::PresentDesktopForegroundComposition(") !=
                std::string::npos &&
                pointer.find(
                    "PresentDesktopForegroundComposition(") !=
                std::string::npos &&
                scrolling.find(
                    "PresentDesktopForegroundComposition(") !=
                std::string::npos,
            "high-frequency Dock and popup feedback must update the foreground surface directly");
        Check(queue.find("ShouldDeferWidgetSurfaceDraw(") !=
                std::string::npos &&
                floatingPopup.find(
                    "FlushPendingDesktopWidgetComposition()") !=
                    std::string::npos,
            "widget child draws must defer during popup BeginDraw and flush after popup EndDraw");
        Check(composition.find(
                "failure.retry ? L\"recreating\"") !=
                    std::string::npos &&
                composition.find(
                    "pendingDesktopWidgetCompositions_.insert(failure.widgetId)") !=
                    std::string::npos,
            "one widget draw failure must recreate only its child surface");
        Check(composition.find(
                "WidgetSurfaceExtent(rawWidth)") != std::string::npos &&
                composition.find(
                    "WidgetSurfaceOrigin(bounds.left)") !=
                    std::string::npos &&
                marquee.find(
                    "WidgetSurfaceOrigin(") != std::string::npos,
            "widget content and child marquees must share the padded surface origin");

        const std::size_t rootZOrderBegin = foreground.find(
            "HRESULT DesktopApp::SyncDesktopCompositionRootZOrder()");
        const std::size_t foregroundCreateBegin = foreground.find(
            "HRESULT DesktopApp::CreateOrResizeDesktopForegroundCompositionSurface()",
            rootZOrderBegin);
        const std::string rootZOrder =
            rootZOrderBegin == std::string::npos ||
                foregroundCreateBegin == std::string::npos
            ? std::string{}
            : foreground.substr(
                rootZOrderBegin,
                foregroundCreateBegin - rootZOrderBegin);
        const auto inRootOrder = [&](const char* token) {
            return rootZOrder.find(token);
        };
        const std::size_t widgetLayer = inRootOrder(
            "desktopWidgetCompositionLayer_.Get()");
        const std::size_t foregroundLayer = inRootOrder(
            "desktopForegroundCompositionVisual_.Get()");
        const std::size_t pageOverlay = inRootOrder(
            "desktopOverlayVisual(pageNotifyAnimationOverlay_)");
        const std::size_t popupOverlay = inRootOrder(
            "desktopOverlayVisual(popupAnimationOverlay_)");
        const std::size_t luaPanelOverlay = inRootOrder(
            "desktopOverlayVisual(luaWidgetPanelAnimationOverlay_)");
        Check(!rootZOrder.empty() &&
                widgetLayer < foregroundLayer &&
                foregroundLayer < pageOverlay &&
                pageOverlay < popupOverlay &&
                popupOverlay < luaPanelOverlay &&
                rootZOrder.find(
                    "visual, TRUE, predecessor") !=
                    std::string::npos,
            "the desktop root tree must explicitly stack widgets below foreground and animation overlays");
        Check(rootZOrder.find(
                "BelongsToCompositionRoot(") !=
                    std::string::npos &&
                rootZOrder.find(
                    "UiCompositionAnimationHost::Desktop") !=
                    std::string::npos,
            "the desktop root tree must exclude floating-popup animation visuals");
        Check(rootZOrder.find("removedFromRoot") !=
                    std::string::npos &&
                rootZOrder.find("restoreRemovedPrefix(index)") !=
                    std::string::npos &&
                rootZOrder.find("attachedToRoot") !=
                    std::string::npos &&
                rootZOrder.find("FAILED(addFailure)") !=
                    std::string::npos,
            "a failed root reorder must reattach visuals instead of leaving a partial tree");
        const std::size_t floatingRootZOrderBegin = floatingPopup.find(
            "HRESULT DesktopApp::SyncFloatingPopupCompositionRootZOrder()");
        const std::size_t floatingRootResetBegin = floatingPopup.find(
            "void DesktopApp::ResetFloatingPopupCompositionResources()",
            floatingRootZOrderBegin);
        const std::string floatingRootZOrder =
            floatingRootZOrderBegin == std::string::npos ||
                floatingRootResetBegin == std::string::npos
            ? std::string{}
            : floatingPopup.substr(
                floatingRootZOrderBegin,
                floatingRootResetBegin - floatingRootZOrderBegin);
        Check(floatingRootZOrder.find(
                "BelongsToCompositionRoot(") !=
                    std::string::npos &&
                floatingRootZOrder.find(
                    "UiCompositionAnimationHost::FloatingPopup") !=
                    std::string::npos,
            "the shared popup root must exclude desktop animation visuals");
        Check(floatingRootZOrder.find("removedFromRoot") !=
                    std::string::npos &&
                floatingRootZOrder.find(
                    "restoreRemovedPrefix(index)") !=
                    std::string::npos &&
                floatingRootZOrder.find("attachedToRoot") !=
                    std::string::npos &&
                floatingRootZOrder.find("FAILED(addFailure)") !=
                    std::string::npos,
            "a failed shared-popup root reorder must reattach visuals instead of leaving a partial tree");
        Check(composition.find(
                "hr = SyncDesktopCompositionRootZOrder();") !=
                std::string::npos &&
                foreground.find(
                    "hr = SyncDesktopCompositionRootZOrder();") !=
                    std::string::npos &&
                animationOverlay.find(
                    "hr = SyncDesktopCompositionRootZOrder();") !=
                    std::string::npos &&
                floatingDock.find(
                    "SyncDesktopCompositionRootZOrder(") ==
                    std::string::npos &&
                floatingDock.find("ShowPopupWindowPair(") !=
                    std::string::npos,
            "desktop-owned visuals must resynchronize the desktop root while the persistent Dock stays on its independent popup root");
    }

    std::cout << "widget composition layer rules tests passed\n";
    return 0;
}
