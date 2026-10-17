#include "app/app.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

// This fixture supplies models and the initial gesture only. Target selection
// executes DesktopApp -> controller -> actual container virtual hit methods.
// Run() is never called. Process-lifetime owners deliberately avoid the host
// destructor, whose global Shell shutdown is outside this windowless test.
struct NativeDragIntegrationAccess
{
    static DesktopApp& Scene(bool secondary)
    {
        auto& app = *new DesktopApp;
        GridPage page;
        page.id = L"native-page";
        page.monitorId = L"native-monitor";
        page.bounds = secondary ? RECT{-800, 0, -400, 300} : RECT{0, 0, 400, 300};
        page.workArea = page.visualWorkArea = page.bounds;
        page.columns = 4; page.rows = 3;
        page.cellWidth = page.cellHeight = 80;
        page.itemPitchWidth = page.itemPitchHeight = 100;
        page.gapX = page.gapY = 20;
        page.marginX = page.marginY = 10;
        app.gridPages_.push_back(page);
        for (int i = 0; i < 2; ++i)
        {
            DesktopItem item;
            item.name = i ? L"B" : L"A";
            item.layoutKey = item.parsingName = item.name;
            item.gridCell = {page.id, i * 2, 0};
            item.gridSpan = {1, 1};
            item.selected = true;
            item.bounds = {page.bounds.left + 10 + i * 200, 10,
                page.bounds.left + 90 + i * 200, 90};
            app.items_.push_back(std::move(item));
        }
        app.containers_.push_back(std::make_unique<DesktopGrid>(&app.gridPages_, &app.items_, &app));
        for (auto& item : app.items_)
            app.items_oo_.push_back(std::make_unique<DesktopIcon>(&item, app.containers_.front().get(), &app));
        return app;
    }

    static void Begin(DesktopApp& app, bool external, std::size_t count)
    {
        std::vector<Item*> items;
        DragSourceList sources;
        Container* origin = nullptr;
        if (external)
            app.dragDropController_.BeginExternalDrag({static_cast<int>(count), false, false});
        else
        {
            origin = app.containers_.front().get();
            for (std::size_t i = 0; i < count; ++i) items.push_back(app.items_oo_[i].get());
            sources = app.BuildDragSourceList(items, origin);
        }
        const POINT point{app.gridPages_.front().bounds.left + 50, 50};
        app.dragSession_.Begin(origin, std::move(items), std::move(sources), point, point);
    }

    static Container* AddStorage(DesktopApp& app, DesktopWidgetType type, bool icon)
    {
        DesktopWidget widget;
        widget.id = L"native-storage";
        widget.type = type;
        widget.title = L"Native target";
        widget.gridCell = {L"native-page", 1, 1};
        widget.gridSpan = icon ? GridSpan{1, 1} : GridSpan{3, 2};
        widget.singleCellIconMode = icon;
        const int left = app.gridPages_.front().bounds.left;
        widget.bounds = icon ? RECT{left + 110, 110, left + 190, 190}
            : RECT{left + 110, 110, left + 390, 290};
        app.widgets_.push_back(std::move(widget));
        auto object = CreateWidget(&app.widgets_.back(), &app);
        auto* container = dynamic_cast<WidgetContainer*>(object.get());
        if (!container) throw std::runtime_error("native storage factory must construct a real container");
        object.release();
        app.containers_.emplace_back(container);
        return container;
    }

    static void Resolve(DesktopApp& app, POINT point)
    {
        if (app.hwnd_ || app.inputHwnd_ || app.controlHwnd_)
            throw std::runtime_error("windowless native test must never initialize a host window");
        app.ResolveCurrentDragTargetAt(point);
    }

    static Container* Target(const DesktopApp& app) { return app.dragSession_.TargetContainer(); }
    static Slot* TargetSlot(const DesktopApp& app) { return app.dragSession_.TargetSlot(); }
    static HitRegion Region(const DesktopApp& app) { return app.dragSession_.TargetRegion(); }
    static Container* Desktop(const DesktopApp& app) { return app.containers_.front().get(); }
    static void Hide(DesktopApp& app) { app.desktopIconsHidden_ = true; }
};

namespace
{
void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

void RunTargets()
{
    using Access = NativeDragIntegrationAccess;
    std::size_t scenes = 0;
    for (const bool secondary : {false, true})
        for (const bool external : {false, true})
            for (const std::size_t count : {1u, 2u})
            {
                auto& app = Access::Scene(secondary);
                const LONG x = secondary ? -650 : 150;
                Access::Begin(app, external, count);
                Access::Resolve(app, {x, 150});
                Require(Access::Target(app) == Access::Desktop(app) &&
                    Access::Region(app) == HitRegion::Empty && Access::TargetSlot(app) &&
                    Access::TargetSlot(app)->GetIndex() == 5,
                    "actual host desktop hit must select the physical free row and column");
                Access::Resolve(app, {-4000, -4000});
                Require(!Access::Target(app) && Access::Region(app) == HitRegion::None,
                    "actual host target resolution must clear a target outside all native frames");
                ++scenes;
                for (const auto type : {DesktopWidgetType::Collection, DesktopWidgetType::FileCategories,
                        DesktopWidgetType::FolderMapping, DesktopWidgetType::CollectionGroup, DesktopWidgetType::FileGroup})
                    for (const bool icon : {false, true})
                    {
                        auto& storageScene = Access::Scene(secondary);
                        Container* expected = Access::AddStorage(storageScene, type, icon);
                        Access::Begin(storageScene, external, count);
                        Access::Resolve(storageScene, {x, 150});
                        Require(Access::Target(storageScene) == expected &&
                            Access::Region(storageScene) != HitRegion::None &&
                            Access::Region(storageScene) != HitRegion::Blocked,
                            "actual host must select the foreground native storage body or icon rather than the desktop behind it");
                        Access::Hide(storageScene);
                        Access::Resolve(storageScene, {x, 150});
                        Require(!Access::Target(storageScene),
                            "actual host visibility filtering must prevent hidden unretained storage and desktop targets");
                        ++scenes;
                    }
            }
    std::cout << scenes << " actual host and native container target scenes passed\n";
}
}

int main()
{
    try
    {
        RunTargets();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
