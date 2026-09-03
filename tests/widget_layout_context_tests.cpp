#include "widget_layout_context.h"
#include "widget_ui_metrics.h"
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
    float semanticCuScale = 1.0f;
    snowdesktop::widget_runtime::SemanticUiMetricTokens semanticUiMetrics;
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
    using snowdesktop::font_cu_rules::CellScale;
    using snowdesktop::font_cu_rules::Scale;
    Expect(CellScale(92, 116) == 1.0f,
        "the baseline grid cell defines one cu scale");
    Expect(CellScale(85, 124) == 85.0f / 92.0f,
        "standard cu excludes inter-cell grid spacing");
    Expect(Scale(15.0f, 1.0f) == 15.0f &&
            Scale(15.0f, 1.5f) == 22.5f,
        "font cu follows only the local grid or component scale");
    Expect(Scale(10.0f, 0.5f) == 9.0f,
        "font cu preserves the shared minimum readable pixel size");
}

void TestReferencePixelsUseTheManifestDefaultShortEdge()
{
    using snowdesktop::widget_runtime::ReferenceSpanHeight;
    using snowdesktop::widget_runtime::ReferenceSpanShortEdge;
    using snowdesktop::widget_runtime::ReferenceSpanWidth;
    using snowdesktop::widget_runtime::ScaleReferenceAxis;
    using snowdesktop::widget_runtime::ScaleReferencePixel;
    Expect(ReferenceSpanWidth(3) == 292.0f &&
            ReferenceSpanHeight(2) == 240.0f,
        "reference axes use the canonical cell dimensions and gap");
    Expect(ReferenceSpanShortEdge(3, 2) == 240.0f,
        "a 3 by 2 default span uses its 240 pixel reference short edge");
    Expect(ReferenceSpanShortEdge(2, 2) == 192.0f,
        "a 2 by 2 default span uses its 192 pixel reference short edge");
    Expect(ScaleReferencePixel(16.0f, 584.0f, 480.0f, 3, 2) == 32.0f,
        "reference pixels scale linearly from the current short edge");
    Expect(ScaleReferencePixel(16.0f, 292.0f, 240.0f, 3, 2) == 16.0f,
        "reference pixels are identity-sized at the canonical default span");
    Expect(ScaleReferenceAxis(16.0f, 584.0f, 292.0f) == 32.0f &&
            ScaleReferenceAxis(16.0f, 360.0f, 240.0f) == 24.0f,
        "reference axes scale independently from width and height");
}

void TestSemanticUiMetricsUseTitleAreaAndPageCu()
{
    using snowdesktop::widget_runtime::SemanticUiMetricTokens;
    using snowdesktop::widget_runtime::ResolveSemanticUiMetrics;
    const auto standard = ResolveSemanticUiMetrics(1.0f, 1.0f);
    Expect(standard.titleAreaHeight == 40.0f &&
            standard.bodyFontSize == 12.0f &&
            standard.controlHeight == 32.0f &&
            standard.compactControlHeight == 28.0f &&
            standard.rowHeight == 40.0f,
        "semantic UI metrics expose shared page-CU defaults");
    const auto denserPage = ResolveSemanticUiMetrics(0.75f, 1.0f);
    Expect(denserPage.titleAreaHeight == 30.0f &&
            denserPage.bodyFontSize == 9.0f &&
            denserPage.controlHeight == 24.0f &&
            denserPage.spacingSm == 6.0f,
        "semantic UI metrics follow the stable page CU scale");
    SemanticUiMetricTokens custom;
    custom.titleAreaHeight = 48.0f;
    const auto customized = ResolveSemanticUiMetrics(custom, 1.0f, 1.0f);
    Expect(customized.titleAreaHeight == 48.0f &&
            customized.controlHeight == 40.0f &&
            customized.spacingSm == 8.0f,
        "title area height drives linked header controls without changing content spacing");
    const auto largeText = ResolveSemanticUiMetrics(1.0f, 2.0f);
    Expect(largeText.bodyFontSize == 24.0f &&
            largeText.controlHeight == 40.0f &&
            largeText.rowHeight == 44.0f,
        "semantic controls expand only when accessibility text needs room");
}

void TestLegacyPointSizesMigrateToCuOnce()
{
    using snowdesktop::font_cu_rules::LegacyPointsToCu;
    using snowdesktop::font_cu_rules::ResolveStoredSize;
    Expect(kDefaultItemFontSizeCu == 16.0f,
        "new layouts default both configurable font sizes to 16 cu");
    Expect(LegacyPointsToCu(15.0f) == 20.0f,
        "legacy point sizes convert through the 96/72 ratio");
    Expect(LegacyPointsToCu(24.0f) == 24.0f,
        "converted legacy point sizes respect the supported cu maximum");

    const auto nativeCu = ResolveStoredSize(18.0f, 15.0f);
    Expect(nativeCu && *nativeCu == 18.0f,
        "native cu fields take precedence and are not converted again");
    const auto migrated = ResolveStoredSize(std::nullopt, 15.0f);
    Expect(migrated && *migrated == 20.0f,
        "legacy point fields migrate when native cu fields are absent");
    Expect(!ResolveStoredSize(std::nullopt, 9.0f).has_value(),
        "invalid legacy point fields fall back to the caller default");
}
}

int main()
{
    TestNestedWidgetMetricsRestoreInOrder();
    TestMetricsAreNormalizedPerWidget();
    TestFontCuUsesOnlyTheLocalCellScale();
    TestReferencePixelsUseTheManifestDefaultShortEdge();
    TestSemanticUiMetricsUseTitleAreaAndPageCu();
    TestLegacyPointSizesMigrateToCuOnce();
    if (failures == 0)
        std::cout << "Widget layout context tests passed\n";
    return failures == 0 ? 0 : 1;
}
