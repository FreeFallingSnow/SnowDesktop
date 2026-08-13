#include "widget_layout_context.h"
#include "font_cu_rules.h"

#include <iostream>

namespace
{
int failures = 0;

void Expect(bool condition, const char* message)
{
    if (condition)
        return;
    std::cerr << "FAILED: " << message << '\n';
    ++failures;
}

struct TestRenderState
{
    int gridCellW = 0;
    int gridCellH = 0;
    int gridGapY = 0;
    int barHeight = 0;
    DWRITE_FONT_WEIGHT itemFontWeight = DWRITE_FONT_WEIGHT_NORMAL;
};

void TestNestedWidgetMetricsRestoreInOrder()
{
    using namespace snowdesktop::widget_runtime;
    TestRenderState state;
    const LayoutMetrics widgetA = NormalizeLayoutMetrics(
        92, 116, 8, 24, DWRITE_FONT_WEIGHT_NORMAL);
    const LayoutMetrics widgetB = NormalizeLayoutMetrics(
        144, 168, 12, 32, DWRITE_FONT_WEIGHT_BOLD);
    const LayoutMetrics widgetC = NormalizeLayoutMetrics(
        60, 72, 4, 18, DWRITE_FONT_WEIGHT_LIGHT);

    ApplyLayoutMetrics(state, widgetA);
    const LayoutMetrics outerSnapshot = CaptureLayoutMetrics(state);
    ApplyLayoutMetrics(state, widgetB);
    const LayoutMetrics nestedSnapshot = CaptureLayoutMetrics(state);
    ApplyLayoutMetrics(state, widgetC);

    RestoreLayoutMetrics(state, nestedSnapshot, [&]() {
        // Activating the caller VM also applies its latest stored metrics.
        // The captured execution snapshot must win after that activation.
        ApplyLayoutMetrics(state, widgetC);
    });
    Expect(CaptureLayoutMetrics(state) == widgetB,
        "leaving a nested widget restores the immediate caller metrics");
    RestoreLayoutMetrics(state, outerSnapshot, [&]() {
        ApplyLayoutMetrics(state, widgetB);
    });
    Expect(CaptureLayoutMetrics(state) == widgetA,
        "leaving the outer widget restores its caller metrics");
}

void TestMetricsAreNormalizedPerWidget()
{
    using namespace snowdesktop::widget_runtime;
    const LayoutMetrics metrics = NormalizeLayoutMetrics(
        1, -10, -2, 17, DWRITE_FONT_WEIGHT_MEDIUM);
    Expect(metrics.gridCellWidth == 4 &&
        metrics.gridCellHeight == 4 && metrics.gridGapY == 0,
        "invalid grid metrics are clamped before storage");
}

void TestFontCuUsesOnlyTheLocalCellScale()
{
    using snowdesktop::font_cu_rules::Scale;
    Expect(Scale(15.0f, 1.0f) == 15.0f &&
            Scale(15.0f, 1.5f) == 22.5f,
        "font cu follows only the local grid or component scale");
    Expect(Scale(10.0f, 0.5f) == 9.0f,
        "font cu preserves the shared minimum readable pixel size");
}
}

int main()
{
    TestNestedWidgetMetricsRestoreInOrder();
    TestMetricsAreNormalizedPerWidget();
    TestFontCuUsesOnlyTheLocalCellScale();
    if (failures == 0)
        std::cout << "Widget layout context tests passed\n";
    return failures == 0 ? 0 : 1;
}
