#include "app/app.h"
#include "layout/widget_displacement.h"

// Collision-aware widget placement and displacement planning.
void DesktopApp::PlaceWidgetWithDisplacement(size_t widgetIndex, GridCell targetCell, GridSpan targetSpan, bool isMove)
{
    const auto order = BuildMonitorRenderOrder();
    snowdesktop::widget_displacement::Services services;
    services.grouped = [this](const auto& widget) { return IsGroupedWidget(widget); };
    services.owned = [this](const auto& item) { return IsItemInAnyWidget(item); };
    services.generatePageId = [this] { return GeneratePageId(); };
    services.committed = [this] {
        // Finish page mapping before runtime pointers and persistence are rebuilt.
        ApplyPageMapping();
        LayoutItems();
        SaveLayoutSlots();
    };
    (void)snowdesktop::widget_displacement::Place(
        {widgets_, items_, gridPages_, order, savedPageIds_, savedPageColumns_, savedPageRows_},
        widgetIndex, std::move(targetCell), targetSpan, isMove, services);
}