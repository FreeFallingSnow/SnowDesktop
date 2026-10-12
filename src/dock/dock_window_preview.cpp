#include "ui/render/app_font.h"
#include "dock_window_preview.h"
#include "dock_window_preview_layout.h"
#include "ui/menu/menu_fluent_glyphs.h"

#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace
{

constexpr wchar_t kDockWindowPreviewClassName[] =
    L"SnowDesktopDockWindowPreview";
// dismiss_20_regular from the same pinned Fluent Regular font as kPin.
constexpr wchar_t kCloseGlyph[] = L"\uF369";

int ScaleForDpi(int value, UINT dpi)
{
    return MulDiv(value, static_cast<int>(dpi), 96);
}

int PreviewActionGap(const RECT& cardRect, UINT dpi)
{
    const int width = std::max(0L, cardRect.right - cardRect.left);
    return std::min(std::max(1, ScaleForDpi(4, dpi)), std::max(1, width / 4));
}

void DrawCenteredActionGlyph(HDC dc, HFONT font, const wchar_t* glyph, const RECT& bounds)
{
    const HGDIOBJ previousFont = SelectObject(dc, font);
    MAT2 transform{};
    transform.eM11.value = 1;
    transform.eM22.value = 1;
    GLYPHMETRICS metrics{};
    // Center the visible ink, not the font's advance width and line box.
    if (GetGlyphOutlineW(dc, glyph[0], GGO_METRICS, &metrics, 0, nullptr, &transform) != GDI_ERROR)
    {
        const int x = bounds.left + (bounds.right - bounds.left -
            static_cast<int>(metrics.gmBlackBoxX)) / 2 - metrics.gmptGlyphOrigin.x;
        const int baseline = bounds.top + (bounds.bottom - bounds.top -
            static_cast<int>(metrics.gmBlackBoxY)) / 2 + metrics.gmptGlyphOrigin.y;
        const UINT previousAlign = SetTextAlign(dc, TA_LEFT | TA_BASELINE);
        ExtTextOutW(dc, x, baseline, ETO_CLIPPED, &bounds, glyph, 1, nullptr);
        SetTextAlign(dc, previousAlign);
    }
    SelectObject(dc, previousFont);
}

bool RectContainsScreenPoint(const RECT& rect, POINT point)
{
    return PtInRect(&rect, point) != FALSE;
}

bool IsPointInTriangle(
    POINT point, POINT first, POINT second, POINT third)
{
    const auto cross = [](POINT lineStart, POINT lineEnd, POINT target) {
        return static_cast<int64_t>(lineEnd.x - lineStart.x) *
                static_cast<int64_t>(target.y - lineStart.y) -
            static_cast<int64_t>(lineEnd.y - lineStart.y) *
                static_cast<int64_t>(target.x - lineStart.x);
    };
    const int64_t firstSide = cross(first, second, point);
    const int64_t secondSide = cross(second, third, point);
    const int64_t thirdSide = cross(third, first, point);
    const bool hasNegative =
        firstSide < 0 || secondSide < 0 || thirdSide < 0;
    const bool hasPositive =
        firstSide > 0 || secondSide > 0 || thirdSide > 0;
    return !(hasNegative && hasPositive);
}

RECT FitThumbnailRect(RECT bounds, SIZE sourceSize)
{
    const int availableWidth = std::max(1L, bounds.right - bounds.left);
    const int availableHeight = std::max(1L, bounds.bottom - bounds.top);
    if (sourceSize.cx <= 0 || sourceSize.cy <= 0)
        return bounds;

    const double scale = std::min(
        static_cast<double>(availableWidth) / sourceSize.cx,
        static_cast<double>(availableHeight) / sourceSize.cy);
    const int width = std::max(1, static_cast<int>(
        std::round(sourceSize.cx * scale)));
    const int height = std::max(1, static_cast<int>(
        std::round(sourceSize.cy * scale)));
    const int left = bounds.left + (availableWidth - width) / 2;
    const int top = bounds.top + (availableHeight - height) / 2;
    return { left, top, left + width, top + height };
}

struct DockWindowPreviewMonitorContext
{
    UINT dpi = 96;
    RECT workArea{};
};

DockWindowPreviewMonitorContext ResolveDockWindowPreviewMonitorContext(
    POINT anchorCenter)
{
    DockWindowPreviewMonitorContext context;
    const HMONITOR monitor = MonitorFromPoint(
        anchorCenter, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{ sizeof(monitorInfo) };
    if (!GetMonitorInfoW(monitor, &monitorInfo))
        monitorInfo.rcWork = {
            0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)
        };

    UINT dpiX = 96;
    UINT dpiY = 96;
    if (FAILED(GetDpiForMonitor(
            monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)))
        dpiX = 96;
    context.dpi = std::max<UINT>(96, dpiX);
    context.workArea = monitorInfo.rcWork;
    return context;
}

RECT ResolveDockWindowPreviewPanelPlacement(
    RECT anchorScreen, DockPosition dockPosition, SIZE panelSize,
    const RECT& workArea, UINT dpi)
{
    if (IsRectEmpty(&anchorScreen) ||
        panelSize.cx <= 0 || panelSize.cy <= 0)
        return {};

    const int panelWidth = std::max(1L, panelSize.cx);
    const int panelHeight = std::max(1L, panelSize.cy);
    const POINT anchorCenter{
        (anchorScreen.left + anchorScreen.right) / 2,
        (anchorScreen.top + anchorScreen.bottom) / 2
    };
    const int gap = ScaleForDpi(10, dpi);
    int left = anchorCenter.x - panelWidth / 2;
    int top = anchorScreen.top - gap - panelHeight;
    switch (dockPosition)
    {
    case DockPosition::Top:
        top = anchorScreen.bottom + gap;
        break;
    case DockPosition::Left:
        left = anchorScreen.right + gap;
        top = anchorCenter.y - panelHeight / 2;
        break;
    case DockPosition::Right:
        left = anchorScreen.left - gap - panelWidth;
        top = anchorCenter.y - panelHeight / 2;
        break;
    case DockPosition::Bottom:
    default:
        break;
    }
    top += ScaleForDpi(4, dpi);
    left = std::clamp(left, static_cast<int>(workArea.left),
        static_cast<int>(std::max<LONG>(
            workArea.left, workArea.right - panelWidth)));
    top = std::clamp(top, static_cast<int>(workArea.top),
        static_cast<int>(std::max<LONG>(
            workArea.top, workArea.bottom - panelHeight)));
    return { left, top, left + panelWidth, top + panelHeight };
}

} // namespace

DockWindowPreviewZOrderPolicy
ResolveDockWindowPreviewZOrderPolicy(
    bool useDockLayer, bool wasVisible)
{
    DockWindowPreviewZOrderPolicy policy;
    policy.insertAfter = HWND_TOPMOST;
    policy.flags = SWP_NOACTIVATE |
        (wasVisible ? 0 : SWP_NOREDRAW);
    if (useDockLayer)
        policy.flags |= SWP_NOOWNERZORDER;
    return policy;
}

DockWindowPreviewGrid CalculateDockWindowPreviewGrid(
    size_t itemCount, int maximumWidth, int maximumHeight, UINT dpi)
{
    DockWindowPreviewGrid result;
    if (itemCount == 0 || maximumWidth <= 0 || maximumHeight <= 0)
        return result;

    const int padding = 0;
    const int gap = std::max(1, ScaleForDpi(8, dpi));
    const int desiredCardWidth = std::max(1, ScaleForDpi(210, dpi));
    const int desiredCardHeight = std::max(1, ScaleForDpi(156, dpi));
    const int count = static_cast<int>(std::min<size_t>(
        itemCount, static_cast<size_t>(std::numeric_limits<int>::max())));

    double bestScale = -1.0;
    int bestRows = std::numeric_limits<int>::max();
    int bestColumns = 1;
    for (int columns = 1; columns <= count; ++columns)
    {
        const int rows = (count + columns - 1) / columns;
        const int horizontalChrome =
            padding * 2 + gap * std::max(0, columns - 1);
        const int verticalChrome =
            padding * 2 + gap * std::max(0, rows - 1);
        if (horizontalChrome >= maximumWidth ||
            verticalChrome >= maximumHeight)
            continue;

        const double widthScale =
            static_cast<double>(maximumWidth - horizontalChrome) /
            (desiredCardWidth * columns);
        const double heightScale =
            static_cast<double>(maximumHeight - verticalChrome) /
            (desiredCardHeight * rows);
        const double scale = std::min({ 1.0, widthScale, heightScale });
        if (scale <= 0.0)
            continue;

        if (scale > bestScale + 0.001 ||
            (std::abs(scale - bestScale) <= 0.001 && rows < bestRows))
        {
            bestScale = scale;
            bestRows = rows;
            bestColumns = columns;
        }
    }

    if (bestScale <= 0.0)
        bestScale = 0.1;
    result.columns = bestColumns;
    result.rows = (count + bestColumns - 1) / bestColumns;
    result.cardWidth = std::max(1, static_cast<int>(
        std::floor(desiredCardWidth * bestScale)));
    result.cardHeight = std::max(1, static_cast<int>(
        std::floor(desiredCardHeight * bestScale)));
    result.panelWidth = padding * 2 +
        result.cardWidth * result.columns +
        gap * std::max(0, result.columns - 1);
    result.panelHeight = padding * 2 +
        result.cardHeight * result.rows +
        gap * std::max(0, result.rows - 1);
    return result;
}

DockWindowPreviewLayout CalculateDockWindowPreviewLayout(
    const std::vector<SIZE>& sourceSizes, int maximumWidth, int maximumHeight, UINT dpi)
{
    if (sourceSizes.empty() || maximumWidth <= 0 || maximumHeight <= 0) return {};
    const int gap = std::max(1, ScaleForDpi(8, dpi));
    const int padding = std::max(1, ScaleForDpi(5, dpi));
    const int title = std::max(1, ScaleForDpi(34, dpi));
    const int desiredHeight = std::max(1, ScaleForDpi(113, dpi));
    const auto atHeight = [&](int height) {
        DockWindowPreviewLayout layout;
        layout.cardHeight = title + padding + height;
        int rowWidth = 0;
        int row = 0;
        int columns = 0;
        for (const SIZE size : sourceSizes)
        {
            const double ratio = size.cx > 0 && size.cy > 0
                ? static_cast<double>(size.cx) / size.cy : 16.0 / 9.0;
            const int width = padding * 2 + static_cast<int>(std::min<double>(
                maximumWidth + 1.0, std::max(1.0, std::round(height * ratio))));
            if (width > maximumWidth) return DockWindowPreviewLayout{};
            if (rowWidth && rowWidth + gap + width > maximumWidth)
            {
                layout.columns = std::max(layout.columns, columns);
                columns = 0;
                rowWidth = 0;
                ++row;
            }
            const int left = rowWidth ? rowWidth + gap : 0;
            const int top = row * (layout.cardHeight + gap);
            layout.cardWidths.push_back(width);
            layout.cardRects.push_back({left, top, left + width, top + layout.cardHeight});
            layout.cardWidth = std::max(layout.cardWidth, width);
            rowWidth = left + width;
            layout.panelWidth = std::max(layout.panelWidth, rowWidth);
            ++columns;
        }
        layout.columns = std::max(layout.columns, columns);
        layout.rows = row + 1;
        layout.panelHeight = layout.rows * layout.cardHeight + row * gap;
        for (size_t first = 0; first < layout.cardRects.size();)
        {
            size_t end = first + 1;
            while (end < layout.cardRects.size() &&
                    layout.cardRects[end].top == layout.cardRects[first].top) ++end;
            const int offset = (layout.panelWidth - layout.cardRects[end - 1].right) / 2;
            for (size_t index = first; index < end; ++index)
                OffsetRect(&layout.cardRects[index], offset, 0);
            first = end;
        }
        return layout;
    };
    DockWindowPreviewLayout best;
    int low = 1;
    int high = std::min(desiredHeight, maximumHeight - title - padding);
    while (low <= high)
    {
        const int middle = low + (high - low) / 2;
        auto candidate = atHeight(middle);
        if (!candidate.cardWidths.empty() && candidate.panelHeight <= maximumHeight)
        {
            best = std::move(candidate);
            low = middle + 1;
        }
        else high = middle - 1;
    }
    if (best.cardWidths.empty())
    {
        static_cast<DockWindowPreviewGrid&>(best) = CalculateDockWindowPreviewGrid(
            sourceSizes.size(), maximumWidth, maximumHeight, dpi);
        best.cardWidths.assign(sourceSizes.size(), best.cardWidth);
        best.cardRects = CalculateDockWindowPreviewCardRects(sourceSizes.size(), best, dpi);
    }
    return best;
}

static std::vector<RECT> CalculatePreviewCardRects(
    size_t itemCount, const DockWindowPreviewGrid& grid, UINT dpi,
    const std::vector<int>& heights)
{
    std::vector<RECT> cards;
    if (itemCount == 0 || grid.columns <= 0 || grid.rows <= 0 ||
        grid.cardWidth <= 0 || grid.cardHeight <= 0 ||
        grid.panelWidth <= 0 || grid.panelHeight <= 0)
        return cards;

    const int gap = std::max(1, ScaleForDpi(8, dpi));
    const int padding = 0;
    cards.reserve(itemCount);
    size_t rowStartIndex = 0;
    int top = padding;
    for (int row = 0; row < grid.rows &&
        rowStartIndex < itemCount; ++row)
    {
        const int itemsInRow = static_cast<int>(std::min<size_t>(
            static_cast<size_t>(grid.columns),
            itemCount - rowStartIndex));
        const int rowWidth = itemsInRow * grid.cardWidth +
            std::max(0, itemsInRow - 1) * gap;
        const int rowLeft = std::max(
            padding, (grid.panelWidth - rowWidth) / 2);
        int rowHeight = 0;
        for (int column = 0; column < itemsInRow; ++column)
        {
            const int left =
                rowLeft + column * (grid.cardWidth + gap);
            const size_t index = rowStartIndex + static_cast<size_t>(column);
            const int height = index < heights.size() ? heights[index] : grid.cardHeight;
            rowHeight = std::max(rowHeight, height);
            cards.push_back({
                left, top,
                left + grid.cardWidth,
                top + height
            });
        }
        rowStartIndex += static_cast<size_t>(itemsInRow);
        top += rowHeight + gap;
    }
    return cards;
}

std::vector<RECT> CalculateDockWindowPreviewCardRects(
    size_t itemCount, const DockWindowPreviewGrid& grid, UINT dpi)
{
    return CalculatePreviewCardRects(itemCount, grid, dpi, {});
}

std::vector<RECT> CalculateDockWindowPreviewLayoutCardRects(
    const DockWindowPreviewLayout& layout, UINT)
{
    return layout.cardRects;
}

RECT CalculateDockWindowPreviewCloseButtonRect(
    const RECT& cardRect, UINT dpi)
{
    const int width = std::max(
        0L, cardRect.right - cardRect.left);
    const int height = std::max(
        0L, cardRect.bottom - cardRect.top);
    if (width <= 0 || height <= 0)
        return {};

    const int titleHeight = std::min(
        ScaleForDpi(34, dpi),
        height);
    const int inset = std::min(
        std::max(1, ScaleForDpi(6, dpi)),
        std::max(1, width / 4));
    const int verticalPadding =
        std::max(1, ScaleForDpi(6, dpi));
    // Budget both square actions before choosing their size. Portrait cards
    // keep their proportional width rather than gaining extra side padding.
    const int actionWidth = std::max(
        1, (width - inset * 2 - PreviewActionGap(cardRect, dpi)) / 2);
    const int buttonSize = std::max(
        1, std::min({
            ScaleForDpi(20, dpi),
            std::max(1, titleHeight -
                verticalPadding * 2),
            actionWidth
        }));
    const int right =
        static_cast<int>(cardRect.right) - inset;
    const int left = std::max(
        static_cast<int>(cardRect.left) + inset,
        right - buttonSize);
    const int top =
        static_cast<int>(cardRect.top) +
        std::max(0, (titleHeight - buttonSize) / 2);
    return {
        left, top,
        std::min(
            static_cast<int>(cardRect.right),
            left + buttonSize),
        std::min(
            static_cast<int>(cardRect.bottom),
            top + buttonSize)
    };
}

RECT CalculateDockWindowPreviewPinButtonRect(
    const RECT& cardRect, UINT dpi)
{
    RECT pin = CalculateDockWindowPreviewCloseButtonRect(cardRect, dpi);
    const int width = pin.right - pin.left;
    if (width <= 0) return {};
    const int inset = cardRect.right - pin.right;
    OffsetRect(&pin, -width - PreviewActionGap(cardRect, dpi), 0);
    if (pin.left < cardRect.left + inset) return {};
    return pin;
}

bool IsPointInDockWindowPreviewCloseButton(
    POINT point, const RECT& cardRect, UINT dpi)
{
    const RECT closeButton =
        CalculateDockWindowPreviewCloseButtonRect(
            cardRect, dpi);
    return !IsRectEmpty(&closeButton) &&
        PtInRect(&closeButton, point) != FALSE;
}

bool IsPointInDockPreviewTransitionRegion(
    POINT screenPoint, POINT transitionOriginScreen,
    const RECT& anchorScreen,
    const RECT& previewScreen, DockPosition dockPosition,
    int tolerance)
{
    if (IsRectEmpty(&anchorScreen) || IsRectEmpty(&previewScreen))
        return false;

    tolerance = std::max(0, tolerance);
    const POINT apex = transitionOriginScreen;
    POINT baseStart{};
    POINT baseEnd{};
    POINT anchorEdgeStart{};
    POINT anchorEdgeEnd{};
    switch (dockPosition)
    {
    case DockPosition::Top:
        anchorEdgeStart = {
            anchorScreen.left - tolerance,
            anchorScreen.bottom + tolerance
        };
        anchorEdgeEnd = {
            anchorScreen.right + tolerance,
            anchorScreen.bottom + tolerance
        };
        baseStart = {
            previewScreen.left - tolerance,
            previewScreen.top - tolerance
        };
        baseEnd = {
            previewScreen.right + tolerance,
            previewScreen.top - tolerance
        };
        break;
    case DockPosition::Left:
        anchorEdgeStart = {
            anchorScreen.right + tolerance,
            anchorScreen.top - tolerance
        };
        anchorEdgeEnd = {
            anchorScreen.right + tolerance,
            anchorScreen.bottom + tolerance
        };
        baseStart = {
            previewScreen.left - tolerance,
            previewScreen.top - tolerance
        };
        baseEnd = {
            previewScreen.left - tolerance,
            previewScreen.bottom + tolerance
        };
        break;
    case DockPosition::Right:
        anchorEdgeStart = {
            anchorScreen.left - tolerance,
            anchorScreen.top - tolerance
        };
        anchorEdgeEnd = {
            anchorScreen.left - tolerance,
            anchorScreen.bottom + tolerance
        };
        baseStart = {
            previewScreen.right + tolerance,
            previewScreen.top - tolerance
        };
        baseEnd = {
            previewScreen.right + tolerance,
            previewScreen.bottom + tolerance
        };
        break;
    case DockPosition::Bottom:
    default:
        anchorEdgeStart = {
            anchorScreen.left - tolerance,
            anchorScreen.top - tolerance
        };
        anchorEdgeEnd = {
            anchorScreen.right + tolerance,
            anchorScreen.top - tolerance
        };
        baseStart = {
            previewScreen.left - tolerance,
            previewScreen.bottom + tolerance
        };
        baseEnd = {
            previewScreen.right + tolerance,
            previewScreen.bottom + tolerance
        };
        break;
    }

    // Keep the cursor-to-preview aim triangle, then cover the complete
    // icon-facing edge with two additional triangles. Windows can coalesce
    // WM_MOUSEMOVE messages, so a single sampled apex is not a reliable
    // representation of the point where the pointer actually left the icon.
    return IsPointInTriangle(
            screenPoint, apex, baseStart, baseEnd) ||
        IsPointInTriangle(
            screenPoint, anchorEdgeStart, baseStart, baseEnd) ||
        IsPointInTriangle(
            screenPoint, anchorEdgeStart, baseEnd, anchorEdgeEnd);
}

DockPreviewHoverTransition DockPreviewHoverController::UpdateTarget(
    const std::wstring& targetToken,
    bool previewVisible,
    bool previewMatchesTarget)
{
    DockPreviewHoverTransition transition;
    if (targetToken != currentTarget_)
    {
        if (timerArmed_)
            transition.cancelTimer = true;
        timerArmed_ = false;
        pendingTarget_.clear();
        if (!suppressedTarget_.empty() &&
            targetToken != suppressedTarget_)
            suppressedTarget_.clear();
        currentTarget_ = targetToken;
    }

    if (targetToken.empty())
    {
        transition.schedulePreviewHide = previewVisible;
        return transition;
    }
    if (previewVisible && previewMatchesTarget)
    {
        transition.keepPreviewVisible = true;
        return transition;
    }
    if (previewVisible)
        transition.schedulePreviewHide = true;
    if (targetToken == suppressedTarget_)
        return transition;
    if (!timerArmed_)
    {
        pendingTarget_ = targetToken;
        timerArmed_ = true;
        transition.armTimer = true;
    }
    return transition;
}

bool DockPreviewHoverController::ConsumeTimer(
    const std::wstring& observedTargetToken)
{
    const bool accepted = timerArmed_ &&
        !pendingTarget_.empty() &&
        pendingTarget_ == currentTarget_ &&
        pendingTarget_ == observedTargetToken &&
        pendingTarget_ != suppressedTarget_;
    timerArmed_ = false;
    pendingTarget_.clear();
    return accepted;
}

bool DockPreviewHoverController::SuppressForActivation()
{
    const bool cancelTimer = timerArmed_;
    timerArmed_ = false;
    pendingTarget_.clear();
    suppressedTarget_ = !shownTarget_.empty()
        ? shownTarget_ : currentTarget_;
    shownTarget_.clear();
    return cancelTimer;
}

void DockPreviewHoverController::MarkPreviewShown(
    const std::wstring& targetToken)
{
    shownTarget_ = targetToken;
}

void DockPreviewHoverController::Reset()
{
    currentTarget_.clear();
    pendingTarget_.clear();
    shownTarget_.clear();
    suppressedTarget_.clear();
    timerArmed_ = false;
}

DockWindowPreview::~DockWindowPreview()
{
    UnregisterThumbnails();
    for (const HWND window : windows_)
        if (window) DestroyWindow(window);
    windows_.clear();
    hwnd_ = nullptr;
    if (instance_)
        UnregisterClassW(kDockWindowPreviewClassName, instance_);
}

bool DockWindowPreview::Initialize(
    HINSTANCE instance,
    ActivateCallback activateCallback,
    CloseCallback closeCallback, std::function<void()> visibilityChanged)
{
    instance_ = instance;
    activateCallback_ = std::move(activateCallback);
    closeCallback_ = std::move(closeCallback);
    visibilityChanged_ = std::move(visibilityChanged);

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_DBLCLKS;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance_;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_HAND);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kDockWindowPreviewClassName;
    return RegisterClassExW(&windowClass) != 0 ||
        GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool DockWindowPreview::EnsureWindow()
{
    return EnsureWindows(std::max<size_t>(1, windows_.size()));
}

bool DockWindowPreview::EnsureWindows(size_t count)
{
    if (!instance_ || !count)
        return false;
    if (count == windows_.size() && std::all_of(windows_.begin(), windows_.end(),
            [](HWND window) { return window && IsWindow(window); }))
    {
        hwnd_ = windows_.front();
        return true;
    }
    UnregisterThumbnails();
    while (windows_.size() > count)
    {
        if (windows_.back()) DestroyWindow(windows_.back());
        windows_.pop_back();
    }
    windows_.resize(count, nullptr);
    for (HWND& window : windows_)
    {
        if (window && IsWindow(window)) continue;
        window = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kDockWindowPreviewClassName, L"Dock Window Preview",
            WS_POPUP | WS_CLIPCHILDREN,
            0, 0, 1, 1, nullptr, nullptr, instance_, this);
        if (!window) return false;
    }
    hwnd_ = windows_.empty() ? nullptr : windows_.front();
    return hwnd_ != nullptr;
}

void DockWindowPreview::Show(
    const std::vector<DockWindowPreviewItem>& items,
    RECT anchorScreen, DockPosition dockPosition, bool lightTheme,
    HWND dockLayerOwner)
{
    if (!EnsureWindow())
        return;

    std::unordered_set<HWND> seen;
    items_.clear();
    items_.reserve(items.size());
    for (const DockWindowPreviewItem& item : items)
    {
        if (!item.window || !IsWindow(item.window) ||
            !seen.insert(item.window).second)
            continue;
        items_.push_back(item);
    }
    if (items_.empty())
    {
        Hide();
        return;
    }

    anchorScreen_ = anchorScreen;
    dockPosition_ = dockPosition;
    lightTheme_ = lightTheme;
    hoveredIndex_ = -1;
    hoveredCloseIndex_ = -1;
    hoveredPinIndex_ = -1;
    const POINT anchorCenter{
        (anchorScreen.left + anchorScreen.right) / 2,
        (anchorScreen.top + anchorScreen.bottom) / 2
    };
    transitionOriginScreen_ = anchorCenter;
    hasTransitionOrigin_ = true;
    POINT pointer{};
    if (GetCursorPos(&pointer) &&
        RectContainsScreenPoint(anchorScreen_, pointer))
    {
        transitionOriginScreen_ = pointer;
    }
    KeepVisible();

    const DockWindowPreviewMonitorContext context =
        ResolveDockWindowPreviewMonitorContext(anchorCenter);
    dpi_ = context.dpi;
    Layout(context.workArea, dpi_);
    if (!EnsureWindows(items_.size()))
    {
        Hide();
        return;
    }
    const RECT panelRect = ResolveDockWindowPreviewPanelPlacement(
        anchorScreen_, dockPosition_, panelSize_,
        context.workArea, dpi_);

    const BOOL darkMode = lightTheme_ ? FALSE : TRUE;
    const DWM_WINDOW_CORNER_PREFERENCE corner =
        DWMWCP_ROUNDSMALL;
    const bool wasVisible = IsVisible();
    const bool useDockLayer =
        dockLayerOwner &&
        IsWindow(dockLayerOwner);
    const HWND requestedOwner =
        useDockLayer ? dockLayerOwner : nullptr;
    // Each card has its own native popup. Prepare owners, geometry and DWM
    // surfaces before revealing any new card, without moving the Dock owner.
    for (size_t index = 0; index < windows_.size(); ++index)
    {
        const HWND window = windows_[index];
        const RECT& card = cardRects_[index];
        DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE,
            &darkMode, sizeof(darkMode));
        DwmSetWindowAttribute(window, DWMWA_WINDOW_CORNER_PREFERENCE,
            &corner, sizeof(corner));
        const HWND currentOwner = reinterpret_cast<HWND>(
            GetWindowLongPtrW(window, GWLP_HWNDPARENT));
        if (currentOwner != requestedOwner)
        {
            ShowWindow(window, SW_HIDE);
            SetWindowLongPtrW(window, GWLP_HWNDPARENT,
                reinterpret_cast<LONG_PTR>(requestedOwner));
        }
        const bool visibleForUpdate = IsWindowVisible(window) != FALSE;
        const auto zOrder = ResolveDockWindowPreviewZOrderPolicy(useDockLayer, visibleForUpdate);
        const int width = card.right - card.left;
        const int height = card.bottom - card.top;
        SetWindowPos(window, zOrder.insertAfter,
            panelRect.left + card.left, panelRect.top + card.top,
            width, height, zOrder.flags);
        HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1,
            ScaleForDpi(14, dpi_), ScaleForDpi(14, dpi_));
        if (region && !SetWindowRgn(window, region, visibleForUpdate ? TRUE : FALSE))
            DeleteObject(region);
    }
    RegisterThumbnails();
    for (const HWND window : windows_)
    {
        InvalidateRect(window, nullptr, TRUE);
        if (!IsWindowVisible(window)) SetWindowPos(window, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                SWP_NOOWNERZORDER | SWP_NOACTIVATE |
                SWP_SHOWWINDOW);
        UpdateWindow(window);
    }
    if (!wasVisible && IsVisible() && visibilityChanged_) visibilityChanged_();
}

void DockWindowPreview::Layout(RECT monitorWorkArea, UINT dpi)
{
    const int workWidth = std::max(1L,
        monitorWorkArea.right - monitorWorkArea.left);
    const int workHeight = std::max(1L,
        monitorWorkArea.bottom - monitorWorkArea.top);
    const int maximumWidth = std::max(1,
        static_cast<int>(std::floor(workWidth * 0.90)));
    const int maximumHeight = std::max(1,
        static_cast<int>(std::floor(workHeight * 0.78)));
    std::vector<SIZE> sourceSizes;
    sourceSizes.reserve(items_.size());
    for (const auto& item : items_)
    {
        SIZE size{};
        HTHUMBNAIL thumbnail = nullptr;
        if (SUCCEEDED(DwmRegisterThumbnail(hwnd_, item.window, &thumbnail)))
        {
            DwmQueryThumbnailSourceSize(thumbnail, &size);
            DwmUnregisterThumbnail(thumbnail);
        }
        if (size.cx <= 0 || size.cy <= 0)
        {
            RECT frame{};
            if (GetWindowRect(item.window, &frame))
                size = {frame.right - frame.left, frame.bottom - frame.top};
        }
        sourceSizes.push_back(size);
    }
    const DockWindowPreviewLayout grid = CalculateDockWindowPreviewLayout(
        sourceSizes, maximumWidth, maximumHeight, dpi);
    panelSize_ = { grid.panelWidth, grid.panelHeight };

    const int titleHeight = ScaleForDpi(34, dpi);
    const int contentPadding = std::max(1, ScaleForDpi(5, dpi));
    cardRects_ = CalculateDockWindowPreviewLayoutCardRects(grid, dpi);
    thumbnailRects_.clear();
    thumbnailRects_.reserve(items_.size());
    for (const RECT& card : cardRects_)
    {
        thumbnailRects_.push_back({
            card.left + contentPadding,
            card.top + titleHeight,
            card.right - contentPadding,
            card.bottom - contentPadding
        });
    }
}

void DockWindowPreview::RegisterThumbnails()
{
    UnregisterThumbnails();
    thumbnails_.resize(items_.size(), nullptr);
    for (size_t index = 0; index < items_.size(); ++index)
    {
        HTHUMBNAIL thumbnail = nullptr;
        if (FAILED(DwmRegisterThumbnail(
                windows_[index], items_[index].window, &thumbnail)) ||
            !thumbnail)
            continue;

        SIZE sourceSize{};
        DwmQueryThumbnailSourceSize(thumbnail, &sourceSize);
        RECT destination = FitThumbnailRect(
            thumbnailRects_[index], sourceSize);
        OffsetRect(&destination, -cardRects_[index].left, -cardRects_[index].top);
        DWM_THUMBNAIL_PROPERTIES properties{};
        properties.dwFlags =
            DWM_TNP_RECTDESTINATION |
            DWM_TNP_VISIBLE |
            DWM_TNP_OPACITY |
            DWM_TNP_SOURCECLIENTAREAONLY;
        properties.rcDestination = destination;
        properties.opacity = 255;
        properties.fVisible = TRUE;
        properties.fSourceClientAreaOnly = FALSE;
        if (FAILED(DwmUpdateThumbnailProperties(
                thumbnail, &properties)))
        {
            DwmUnregisterThumbnail(thumbnail);
            continue;
        }
        thumbnails_[index] = thumbnail;
    }
}

void DockWindowPreview::UnregisterThumbnails()
{
    for (HTHUMBNAIL thumbnail : thumbnails_)
        if (thumbnail)
            DwmUnregisterThumbnail(thumbnail);
    thumbnails_.clear();
}

void DockWindowPreview::UpdateAnchor(
    RECT anchorScreen, DockPosition dockPosition)
{
    if (!hwnd_ || !IsWindowVisible(hwnd_) ||
        IsRectEmpty(&anchorScreen) ||
        panelSize_.cx <= 0 || panelSize_.cy <= 0)
        return;

    anchorScreen_ = anchorScreen;
    dockPosition_ = dockPosition;
    POINT pointer{};
    if (GetCursorPos(&pointer) &&
        RectContainsScreenPoint(anchorScreen_, pointer))
    {
        transitionOriginScreen_ = pointer;
        hasTransitionOrigin_ = true;
    }

    const POINT anchorCenter{
        (anchorScreen_.left + anchorScreen_.right) / 2,
        (anchorScreen_.top + anchorScreen_.bottom) / 2
    };
    const DockWindowPreviewMonitorContext context =
        ResolveDockWindowPreviewMonitorContext(anchorCenter);
    if (context.dpi != dpi_)
    {
        // 跨监视器 DPI 变化：卡片与缩略图目标必须按新 DPI 重新布局。
        dpi_ = context.dpi;
        Layout(context.workArea, dpi_);
        RegisterThumbnails();
    }
    const RECT panelRect = ResolveDockWindowPreviewPanelPlacement(
        anchorScreen_, dockPosition_, panelSize_,
        context.workArea, dpi_);
    for (size_t index = 0; index < windows_.size(); ++index)
    {
        const RECT& card = cardRects_[index];
        SetWindowPos(windows_[index], nullptr,
            panelRect.left + card.left, panelRect.top + card.top,
            card.right - card.left, card.bottom - card.top,
            SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER |
                SWP_NOSENDCHANGING | SWP_NOREDRAW);
        HRGN region = CreateRoundRectRgn(0, 0,
            card.right - card.left + 1, card.bottom - card.top + 1,
            ScaleForDpi(14, dpi_), ScaleForDpi(14, dpi_));
        if (region && !SetWindowRgn(windows_[index], region, FALSE)) DeleteObject(region);
    }
    InvalidatePopups();
}

void DockWindowPreview::Hide()
{
    if (IsCleared())
        return;
    const bool wasVisible = IsVisible();
    if (hwnd_)
        KillTimer(hwnd_, kHideTimerId);
    for (const HWND window : windows_)
        if (window) ShowWindow(window, SW_HIDE);
    hideTimerArmed_ = false;
    UnregisterThumbnails();
    items_.clear();
    cardRects_.clear();
    thumbnailRects_.clear();
    panelSize_ = {};
    hoveredIndex_ = -1;
    hoveredCloseIndex_ = -1;
    hoveredPinIndex_ = -1;
    trackingMouse_ = false;
    trackingWindow_ = nullptr;
    hasTransitionOrigin_ = false;
    if (wasVisible && visibilityChanged_) visibilityChanged_();
}

void DockWindowPreview::ScheduleHide()
{
    if (!IsVisible())
        return;
    POINT pointer{};
    GetCursorPos(&pointer);
    const RECT preview = GetBounds();
    if (RectContainsScreenPoint(anchorScreen_, pointer) ||
        RectContainsScreenPoint(preview, pointer))
    {
        KeepVisible();
        return;
    }
    if (IsPointerInTransitionRegion(pointer))
    {
        if (SetTimer(hwnd_, kHideTimerId,
                kTransitionHideDelayMs, nullptr) != 0)
            hideTimerArmed_ = true;
        return;
    }
    Hide();
}

void DockWindowPreview::KeepVisible()
{
    if (hwnd_)
        KillTimer(hwnd_, kHideTimerId);
    hideTimerArmed_ = false;
    POINT pointer{};
    if (GetCursorPos(&pointer) &&
        RectContainsScreenPoint(anchorScreen_, pointer))
    {
        transitionOriginScreen_ = pointer;
        hasTransitionOrigin_ = true;
    }
}

bool DockWindowPreview::IsVisible() const
{
    return std::any_of(windows_.begin(), windows_.end(),
        [](HWND window) { return window && IsWindowVisible(window); });
}

RECT DockWindowPreview::GetBounds() const
{
    RECT bounds{};
    for (const HWND window : windows_)
    {
        RECT rect{};
        if (window && IsWindowVisible(window) && GetWindowRect(window, &rect))
            UnionRect(&bounds, &bounds, &rect);
    }
    return bounds;
}

bool DockWindowPreview::IsCleared() const
{
    return !IsVisible() &&
        items_.empty() &&
        cardRects_.empty() &&
        thumbnailRects_.empty() &&
        thumbnails_.empty() &&
        panelSize_.cx == 0 &&
        panelSize_.cy == 0 &&
        hoveredIndex_ == -1 &&
        hoveredCloseIndex_ == -1 &&
        hoveredPinIndex_ == -1 &&
        !trackingMouse_ &&
        !hasTransitionOrigin_ &&
        !hideTimerArmed_;
}

bool DockWindowPreview::IsShowingWindow(HWND window) const
{
    return std::any_of(items_.begin(), items_.end(),
        [window](const DockWindowPreviewItem& item) {
            return item.window == window;
        });
}

bool DockWindowPreview::ContainsInteractionPoint(
    POINT screenPoint) const
{
    if (!IsVisible())
        return false;
    const RECT preview = GetBounds();
    return RectContainsScreenPoint(anchorScreen_, screenPoint) ||
        RectContainsScreenPoint(preview, screenPoint) ||
        IsPointerInTransitionRegion(screenPoint);
}

void DockWindowPreview::Paint(HWND window)
{
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);
    if (!dc)
        return;

    RECT client{};
    GetClientRect(window, &client);
    const COLORREF background = lightTheme_
        ? RGB(244, 246, 249) : RGB(30, 32, 37);
    const COLORREF card = lightTheme_
        ? RGB(255, 255, 255) : RGB(45, 48, 55);
    const COLORREF hovered = lightTheme_
        ? RGB(224, 235, 250) : RGB(58, 76, 99);
    const COLORREF border = lightTheme_
        ? RGB(190, 197, 208) : RGB(84, 89, 99);
    const COLORREF text = lightTheme_
        ? RGB(28, 31, 36) : RGB(246, 247, 249);
    const COLORREF closeIdle = lightTheme_
        ? RGB(92, 98, 108) : RGB(202, 207, 216);
    const COLORREF closeHovered = RGB(221, 62, 72);

    HBRUSH backgroundBrush = CreateSolidBrush(background);
    FillRect(dc, &client, backgroundBrush);
    DeleteObject(backgroundBrush);

    const int corner = std::max(4, ScaleForDpi(8, dpi_));
    const int titleInset = ScaleForDpi(10, dpi_);
    const int titleHeight = ScaleForDpi(34, dpi_);
    const RECT closeRect = CalculateDockWindowPreviewCloseButtonRect(client, dpi_);
    const int actionFontSize = std::min(ScaleForDpi(16, dpi_),
        std::max(1, static_cast<int>(closeRect.right - closeRect.left) - ScaleForDpi(4, dpi_)));
    HFONT font = CreateFontW(
        -ScaleForDpi(14, dpi_), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, snowdesktop::app_fonts::GdiFamily().c_str());
    HFONT actionFont = CreateFontW(
        -actionFontSize, 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"FluentSystemIcons-Regular");
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, text);

    const auto found = std::find(windows_.begin(), windows_.end(), window);
    const size_t index = static_cast<size_t>(found - windows_.begin());
    if (index < cardRects_.size() && index < items_.size())
    {
        const RECT& bounds = client;
        HBRUSH fill = CreateSolidBrush(
            static_cast<int>(index) == hoveredIndex_ ? hovered : card);
        HPEN outline = CreatePen(PS_SOLID, 1, border);
        HGDIOBJ oldBrush = SelectObject(dc, fill);
        HGDIOBJ oldPen = SelectObject(dc, outline);
        RoundRect(dc, bounds.left, bounds.top, bounds.right, bounds.bottom,
            corner, corner);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(outline);
        DeleteObject(fill);

        RECT titleRect{
            bounds.left + titleInset,
            bounds.top,
            bounds.right - titleInset,
            std::min(bounds.bottom, bounds.top + titleHeight)
        };
        RECT pinRect = CalculateDockWindowPreviewPinButtonRect(bounds, dpi_);
        titleRect.right = std::max(
            titleRect.left,
            (IsRectEmpty(&pinRect) ? closeRect.left : pinRect.left) -
                std::max(2, ScaleForDpi(4, dpi_)));
        SetTextColor(dc, text);
        DrawTextW(dc, items_[index].title.c_str(), -1, &titleRect,
            DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS |
            DT_NOPREFIX);

        const bool pinned = DockWindowPin::IsPinned(items_[index].window);
        const bool pinHovered = static_cast<int>(index) == hoveredPinIndex_;
        if (!IsRectEmpty(&pinRect))
        {
            if (pinned || pinHovered)
            {
                const COLORREF pinColor = pinned
                    ? (pinHovered ? RGB(0, 95, 180) : RGB(0, 120, 215))
                    : (lightTheme_ ? RGB(190, 215, 245) : RGB(70, 105, 145));
                HBRUSH pinFill = CreateSolidBrush(pinColor);
                HGDIOBJ previousBrush = SelectObject(dc, pinFill);
                HGDIOBJ previousPen = SelectObject(dc, GetStockObject(NULL_PEN));
                RoundRect(dc, pinRect.left, pinRect.top, pinRect.right, pinRect.bottom,
                    ScaleForDpi(5, dpi_), ScaleForDpi(5, dpi_));
                SelectObject(dc, previousPen);
                SelectObject(dc, previousBrush);
                DeleteObject(pinFill);
            }
            SetTextColor(dc, pinned ? RGB(255, 255, 255) : closeIdle);
            DrawCenteredActionGlyph(dc, actionFont, snowdesktop::menu_fluent_glyphs::kPin, pinRect);
        }

        const bool closeHoveredForItem =
            static_cast<int>(index) ==
                hoveredCloseIndex_;
        if (closeHoveredForItem)
        {
            HBRUSH closeFill =
                CreateSolidBrush(closeHovered);
            HGDIOBJ oldCloseBrush =
                SelectObject(dc, closeFill);
            HGDIOBJ oldClosePen =
                SelectObject(dc, GetStockObject(NULL_PEN));
            RoundRect(
                dc, closeRect.left, closeRect.top,
                closeRect.right, closeRect.bottom,
                std::max(2, ScaleForDpi(5, dpi_)),
                std::max(2, ScaleForDpi(5, dpi_)));
            SelectObject(dc, oldClosePen);
            SelectObject(dc, oldCloseBrush);
            DeleteObject(closeFill);
        }

        const COLORREF closeColor =
            closeHoveredForItem
            ? RGB(255, 255, 255)
            : closeIdle;
        SetTextColor(dc, closeColor);
        DrawCenteredActionGlyph(dc, actionFont, kCloseGlyph, closeRect);
    }

    SelectObject(dc, oldFont);
    DeleteObject(font);
    DeleteObject(actionFont);
    EndPaint(window, &paint);
}

void DockWindowPreview::InvalidatePopups()
{
    for (const HWND window : windows_)
        if (window) InvalidateRect(window, nullptr, FALSE);
}

POINT DockWindowPreview::ToLayoutPoint(HWND window, POINT point) const
{
    const auto found = std::find(windows_.begin(), windows_.end(), window);
    if (found == windows_.end()) return {-1, -1};
    const size_t index = static_cast<size_t>(found - windows_.begin());
    if (index >= cardRects_.size()) return {-1, -1};
    point.x += cardRects_[index].left;
    point.y += cardRects_[index].top;
    return point;
}

int DockWindowPreview::CardIndexAtPoint(POINT point) const
{
    for (size_t index = 0; index < cardRects_.size(); ++index)
        if (PtInRect(&cardRects_[index], point))
            return static_cast<int>(index);
    return -1;
}

int DockWindowPreview::CloseButtonIndexAtPoint(
    POINT point) const
{
    for (size_t index = 0;
         index < cardRects_.size(); ++index)
    {
        if (IsPointInDockWindowPreviewCloseButton(
                point, cardRects_[index], dpi_))
            return static_cast<int>(index);
    }
    return -1;
}

int DockWindowPreview::PinButtonIndexAtPoint(POINT point) const
{
    for (size_t index = 0; index < cardRects_.size(); ++index)
    {
        const RECT pin = CalculateDockWindowPreviewPinButtonRect(cardRects_[index], dpi_);
        if (PtInRect(&pin, point)) return static_cast<int>(index);
    }
    return -1;
}

void DockWindowPreview::OnMouseMove(HWND window, POINT point)
{
    KeepVisible();
    if (!trackingMouse_ || trackingWindow_ != window)
    {
        TRACKMOUSEEVENT tracking{
            sizeof(tracking), TME_LEAVE, window, 0
        };
        TrackMouseEvent(&tracking);
        trackingMouse_ = true;
        trackingWindow_ = window;
    }
    const int hovered = CardIndexAtPoint(point);
    const int hoveredClose =
        CloseButtonIndexAtPoint(point);
    const int hoveredPin = PinButtonIndexAtPoint(point);
    if (hovered != hoveredIndex_ ||
        hoveredClose != hoveredCloseIndex_ || hoveredPin != hoveredPinIndex_)
    {
        hoveredIndex_ = hovered;
        hoveredCloseIndex_ = hoveredClose;
        hoveredPinIndex_ = hoveredPin;
        InvalidatePopups();
    }
}

void DockWindowPreview::OnMouseLeave(HWND window)
{
    if (trackingWindow_ != window) return;
    trackingMouse_ = false;
    trackingWindow_ = nullptr;
    hoveredIndex_ = -1;
    hoveredCloseIndex_ = -1;
    hoveredPinIndex_ = -1;
    InvalidatePopups();
    ScheduleHide();
}

void DockWindowPreview::OnLeftButtonUp(POINT point)
{
    const int pinIndex = PinButtonIndexAtPoint(point);
    if (pinIndex >= 0 && static_cast<size_t>(pinIndex) < items_.size())
    {
        KeepVisible();
        if (windowPins_.Toggle(items_[pinIndex].window))
        {
            // Promoting the target inserts it above other topmost windows.
            // Keep its still-open controls available without activating either
            // the preview or its floating Dock owner.
            for (const HWND window : windows_)
                SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        }
        InvalidatePopups();
        return;
    }
    const int closeIndex =
        CloseButtonIndexAtPoint(point);
    if (closeIndex >= 0 &&
        static_cast<size_t>(closeIndex) <
            items_.size())
    {
        const HWND target =
            items_[closeIndex].window;
        Hide();
        if (closeCallback_ && target &&
            IsWindow(target))
            closeCallback_(target);
        return;
    }

    const int index = CardIndexAtPoint(point);
    if (index < 0 || static_cast<size_t>(index) >= items_.size())
        return;
    const HWND target = items_[index].window;
    Hide();
    if (activateCallback_ && target && IsWindow(target))
        activateCallback_(target);
}

void DockWindowPreview::OnMiddleButtonUp(POINT point)
{
    const int index = CardIndexAtPoint(point);
    if (index < 0 || static_cast<size_t>(index) >= items_.size()) return;
    const HWND target = items_[index].window;
    Hide();
    if (closeCallback_ && target && IsWindow(target)) closeCallback_(target);
}

void DockWindowPreview::HideIfPointerOutside()
{
    POINT pointer{};
    GetCursorPos(&pointer);
    const RECT preview = GetBounds();
    if (RectContainsScreenPoint(anchorScreen_, pointer) ||
        RectContainsScreenPoint(preview, pointer))
    {
        KeepVisible();
        return;
    }
    if (IsPointerInTransitionRegion(pointer))
    {
        if (SetTimer(hwnd_, kHideTimerId,
                kTransitionHideDelayMs, nullptr) != 0)
            hideTimerArmed_ = true;
        return;
    }
    Hide();
}

bool DockWindowPreview::IsPointerInTransitionRegion(
    POINT screenPoint) const
{
    if (!hwnd_ || IsRectEmpty(&anchorScreen_))
        return false;
    const RECT preview = GetBounds();
    if (IsRectEmpty(&preview)) return false;
    const POINT origin = hasTransitionOrigin_
        ? transitionOriginScreen_
        : POINT{
            (anchorScreen_.left + anchorScreen_.right) / 2,
            (anchorScreen_.top + anchorScreen_.bottom) / 2
        };
    const int tolerance = std::max(4, ScaleForDpi(12, dpi_));
    return IsPointInDockPreviewTransitionRegion(
        screenPoint, origin, anchorScreen_, preview,
        dockPosition_, tolerance);
}

LRESULT CALLBACK DockWindowPreview::WindowProc(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    DockWindowPreview* preview = nullptr;
    if (message == WM_NCCREATE)
    {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        preview = static_cast<DockWindowPreview*>(
            create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(preview));
    }
    else
    {
        preview = reinterpret_cast<DockWindowPreview*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
    }

    if (!preview)
        return DefWindowProcW(window, message, wParam, lParam);

    switch (message)
    {
    case WM_PAINT:
        preview->Paint(window);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_MOUSEMOVE:
        preview->OnMouseMove(window, preview->ToLayoutPoint(window, {
            GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)
        }));
        return 0;
    case WM_MOUSELEAVE:
        preview->OnMouseLeave(window);
        return 0;
    case WM_LBUTTONUP:
        preview->OnLeftButtonUp(preview->ToLayoutPoint(window, {
            GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)
        }));
        return 0;
    case WM_MBUTTONUP:
        preview->OnMiddleButtonUp(preview->ToLayoutPoint(window, {
            GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)
        }));
        return 0;
    case WM_TIMER:
        if (wParam == kHideTimerId)
        {
            preview->HideIfPointerOutside();
            return 0;
        }
        break;
    case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        for (HWND& popup : preview->windows_)
            if (popup == window) popup = nullptr;
        if (preview->hwnd_ == window)
        {
            preview->hwnd_ = nullptr;
            preview->hideTimerArmed_ = false;
        }
        break;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
