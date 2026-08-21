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

    Check(rules::ShouldPresentWidgetSurface(true, false),
        "a visible widget must present its child surface");
    Check(!rules::ShouldPresentWidgetSurface(false, false),
        "a hidden widget must hide its child surface");
    Check(!rules::ShouldPresentWidgetSurface(true, true),
        "a move or resize preview source must hide its child surface");

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
        const std::string pointer = ReadFile(
            root / "src" / "app" / "app_pointer_move.cpp");
        const std::string scrolling = ReadFile(
            root / "src" / "app" / "app_scroll_interaction.cpp");
        const std::string foreground = ReadFile(
            root / "src" / "app" /
                "app_desktop_foreground_composition.cpp");

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
    }

    std::cout << "widget composition layer rules tests passed\n";
    return 0;
}
