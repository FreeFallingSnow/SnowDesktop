#include "dock_window_preview.h"

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

int ScaleForDpi(int value, UINT dpi)
{
    return MulDiv(value, static_cast<int>(dpi), 96);
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

} // namespace

DockWindowPreviewGrid CalculateDockWindowPreviewGrid(
    size_t itemCount, int maximumWidth, int maximumHeight, UINT dpi)
{
    DockWindowPreviewGrid result;
    if (itemCount == 0 || maximumWidth <= 0 || maximumHeight <= 0)
        return result;

    const int padding = std::max(1, ScaleForDpi(10, dpi));
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

std::vector<RECT> CalculateDockWindowPreviewCardRects(
    size_t itemCount, const DockWindowPreviewGrid& grid, UINT dpi)
{
    std::vector<RECT> cards;
    if (itemCount == 0 || grid.columns <= 0 || grid.rows <= 0 ||
        grid.cardWidth <= 0 || grid.cardHeight <= 0 ||
        grid.panelWidth <= 0 || grid.panelHeight <= 0)
        return cards;

    const int gap = std::max(1, ScaleForDpi(8, dpi));
    const int padding = std::max(1, ScaleForDpi(10, dpi));
    cards.reserve(itemCount);
    size_t rowStartIndex = 0;
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
        const int top = padding + row * (grid.cardHeight + gap);
        for (int column = 0; column < itemsInRow; ++column)
        {
            const int left =
                rowLeft + column * (grid.cardWidth + gap);
            cards.push_back({
                left, top,
                left + grid.cardWidth,
                top + grid.cardHeight
            });
        }
        rowStartIndex += static_cast<size_t>(itemsInRow);
    }
    return cards;
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
    if (hwnd_)
        DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    if (instance_)
        UnregisterClassW(kDockWindowPreviewClassName, instance_);
}

bool DockWindowPreview::Initialize(
    HINSTANCE instance, ActivateCallback activateCallback)
{
    instance_ = instance;
    activateCallback_ = std::move(activateCallback);

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
    if (hwnd_ && IsWindow(hwnd_))
        return true;
    if (!instance_)
        return false;

    hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kDockWindowPreviewClassName, L"Dock Window Preview",
        WS_POPUP | WS_CLIPCHILDREN,
        0, 0, 1, 1, nullptr, nullptr, instance_, this);
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
    dpi_ = std::max<UINT>(96, dpiX);
    Layout(monitorInfo.rcWork, dpi_);

    const int panelWidth = std::max(1L, panelSize_.cx);
    const int panelHeight = std::max(1L, panelSize_.cy);

    const int gap = ScaleForDpi(10, dpi_);
    int left = anchorCenter.x - panelWidth / 2;
    int top = anchorScreen.top - gap - panelHeight;
    switch (dockPosition_)
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
    left = std::clamp(left, static_cast<int>(monitorInfo.rcWork.left),
        static_cast<int>(std::max<LONG>(
            monitorInfo.rcWork.left,
            monitorInfo.rcWork.right - panelWidth)));
    top = std::clamp(top, static_cast<int>(monitorInfo.rcWork.top),
        static_cast<int>(std::max<LONG>(
            monitorInfo.rcWork.top,
            monitorInfo.rcWork.bottom - panelHeight)));

    const BOOL darkMode = lightTheme_ ? FALSE : TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE,
        &darkMode, sizeof(darkMode));
    const DWM_WINDOW_CORNER_PREFERENCE corner =
        DWMWCP_ROUNDSMALL;
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE,
        &corner, sizeof(corner));

    const bool wasVisible = IsWindowVisible(hwnd_) != FALSE;
    const bool useDockLayer =
        dockLayerOwner &&
        IsWindow(dockLayerOwner);
    SetWindowLongPtrW(
        hwnd_, GWLP_HWNDPARENT,
        reinterpret_cast<LONG_PTR>(
            useDockLayer
                ? dockLayerOwner : nullptr));

    // Prepare the complete preview while it is still hidden. Showing the
    // HWND before applying its region and registering the DWM thumbnails
    // exposes one empty rectangular frame; the floating Dock made that frame
    // especially noticeable because it also performed a second visible
    // Z-order transition.
    SetWindowPos(
        hwnd_,
        useDockLayer ? HWND_NOTOPMOST : HWND_TOPMOST,
        left, top, panelWidth, panelHeight,
        SWP_NOACTIVATE |
            (wasVisible ? 0 : SWP_NOREDRAW));
    HRGN region = CreateRoundRectRgn(
        0, 0, panelWidth + 1, panelHeight + 1,
        ScaleForDpi(14, dpi_), ScaleForDpi(14, dpi_));
    if (region)
        SetWindowRgn(hwnd_, region, wasVisible ? TRUE : FALSE);

    RegisterThumbnails();
    InvalidateRect(hwnd_, nullptr, TRUE);
    if (!wasVisible)
    {
        // Ownership already keeps the preview above the floating Dock. Reveal
        // it without another Z-order mutation so the Dock and preview enter
        // the compositor as one stable layer pair.
        SetWindowPos(
            hwnd_, nullptr,
            0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    UpdateWindow(hwnd_);
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
    const DockWindowPreviewGrid grid = CalculateDockWindowPreviewGrid(
        items_.size(), maximumWidth, maximumHeight, dpi);
    panelSize_ = { grid.panelWidth, grid.panelHeight };

    const int titleHeight = std::min(
        ScaleForDpi(34, dpi),
        std::max(1, grid.cardHeight / 3));
    const int contentPadding = std::max(1, ScaleForDpi(5, dpi));
    cardRects_ = CalculateDockWindowPreviewCardRects(
        items_.size(), grid, dpi);
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
                hwnd_, items_[index].window, &thumbnail)) ||
            !thumbnail)
            continue;

        SIZE sourceSize{};
        DwmQueryThumbnailSourceSize(thumbnail, &sourceSize);
        const RECT destination = FitThumbnailRect(
            thumbnailRects_[index], sourceSize);
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

void DockWindowPreview::Hide()
{
    if (hwnd_)
    {
        KillTimer(hwnd_, kHideTimerId);
        ShowWindow(hwnd_, SW_HIDE);
    }
    UnregisterThumbnails();
    items_.clear();
    cardRects_.clear();
    thumbnailRects_.clear();
    panelSize_ = {};
    hoveredIndex_ = -1;
    trackingMouse_ = false;
    hasTransitionOrigin_ = false;
}

void DockWindowPreview::ScheduleHide()
{
    if (!IsVisible())
        return;
    POINT pointer{};
    GetCursorPos(&pointer);
    RECT preview{};
    GetWindowRect(hwnd_, &preview);
    if (RectContainsScreenPoint(anchorScreen_, pointer) ||
        RectContainsScreenPoint(preview, pointer))
    {
        KeepVisible();
        return;
    }
    if (IsPointerInTransitionRegion(pointer))
    {
        SetTimer(hwnd_, kHideTimerId,
            kTransitionHideDelayMs, nullptr);
        return;
    }
    Hide();
}

void DockWindowPreview::KeepVisible()
{
    if (hwnd_)
        KillTimer(hwnd_, kHideTimerId);
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
    return hwnd_ && IsWindowVisible(hwnd_);
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
    RECT preview{};
    if (!GetWindowRect(hwnd_, &preview))
        return false;
    return RectContainsScreenPoint(anchorScreen_, screenPoint) ||
        RectContainsScreenPoint(preview, screenPoint) ||
        IsPointerInTransitionRegion(screenPoint);
}

void DockWindowPreview::Paint()
{
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(hwnd_, &paint);
    if (!dc)
        return;

    RECT client{};
    GetClientRect(hwnd_, &client);
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

    HBRUSH backgroundBrush = CreateSolidBrush(background);
    FillRect(dc, &client, backgroundBrush);
    DeleteObject(backgroundBrush);

    const int corner = std::max(4, ScaleForDpi(8, dpi_));
    const int titleInset = ScaleForDpi(10, dpi_);
    const int titleHeight = ScaleForDpi(32, dpi_);
    HFONT font = CreateFontW(
        -ScaleForDpi(14, dpi_), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, text);

    for (size_t index = 0; index < cardRects_.size(); ++index)
    {
        const RECT& bounds = cardRects_[index];
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
        DrawTextW(dc, items_[index].title.c_str(), -1, &titleRect,
            DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS |
            DT_NOPREFIX);
    }

    SelectObject(dc, oldFont);
    DeleteObject(font);
    EndPaint(hwnd_, &paint);
}

int DockWindowPreview::CardIndexAtPoint(POINT point) const
{
    for (size_t index = 0; index < cardRects_.size(); ++index)
        if (PtInRect(&cardRects_[index], point))
            return static_cast<int>(index);
    return -1;
}

void DockWindowPreview::OnMouseMove(POINT point)
{
    KeepVisible();
    if (!trackingMouse_)
    {
        TRACKMOUSEEVENT tracking{
            sizeof(tracking), TME_LEAVE, hwnd_, 0
        };
        TrackMouseEvent(&tracking);
        trackingMouse_ = true;
    }
    const int hovered = CardIndexAtPoint(point);
    if (hovered != hoveredIndex_)
    {
        hoveredIndex_ = hovered;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void DockWindowPreview::OnMouseLeave()
{
    trackingMouse_ = false;
    hoveredIndex_ = -1;
    InvalidateRect(hwnd_, nullptr, FALSE);
    ScheduleHide();
}

void DockWindowPreview::OnLeftButtonUp(POINT point)
{
    const int index = CardIndexAtPoint(point);
    if (index < 0 || static_cast<size_t>(index) >= items_.size())
        return;
    const HWND target = items_[index].window;
    Hide();
    if (activateCallback_ && target && IsWindow(target))
        activateCallback_(target);
}

void DockWindowPreview::HideIfPointerOutside()
{
    POINT pointer{};
    GetCursorPos(&pointer);
    RECT preview{};
    if (hwnd_)
        GetWindowRect(hwnd_, &preview);
    if (RectContainsScreenPoint(anchorScreen_, pointer) ||
        RectContainsScreenPoint(preview, pointer))
    {
        KeepVisible();
        return;
    }
    if (IsPointerInTransitionRegion(pointer))
    {
        SetTimer(hwnd_, kHideTimerId,
            kTransitionHideDelayMs, nullptr);
        return;
    }
    Hide();
}

bool DockWindowPreview::IsPointerInTransitionRegion(
    POINT screenPoint) const
{
    if (!hwnd_ || IsRectEmpty(&anchorScreen_))
        return false;
    RECT preview{};
    if (!GetWindowRect(hwnd_, &preview))
        return false;
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
        preview->Paint();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_MOUSEMOVE:
        preview->OnMouseMove({
            GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)
        });
        return 0;
    case WM_MOUSELEAVE:
        preview->OnMouseLeave();
        return 0;
    case WM_LBUTTONUP:
        preview->OnLeftButtonUp({
            GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)
        });
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
        if (preview->hwnd_ == window)
            preview->hwnd_ = nullptr;
        break;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
