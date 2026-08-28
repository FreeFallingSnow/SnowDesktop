#include "widget_notification_presenter.h"

#include <wincodec.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <utility>
#include <vector>

namespace
{
using Microsoft::WRL::ComPtr;

constexpr wchar_t WindowClassName[] =
    L"SnowDesktop.WidgetNotificationPresenter";
constexpr UINT_PTR DismissTimerId = 1;
constexpr UINT DismissDelayMs = 30000;

int Scale(HWND hwnd, int value)
{
    const UINT dpi = hwnd ? GetDpiForWindow(hwnd) : 96;
    return MulDiv(value, static_cast<int>(dpi ? dpi : 96), 96);
}

HBITMAP LoadImage(const std::wstring& path, int targetPixels)
{
    if (path.empty() || targetPixels <= 0) return nullptr;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr,
            GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)))
        return nullptr;

    UINT sourceWidth = 0;
    UINT sourceHeight = 0;
    if (FAILED(frame->GetSize(&sourceWidth, &sourceHeight)) ||
        sourceWidth == 0 || sourceHeight == 0)
        return nullptr;
    IWICBitmapSource* source = frame.Get();
    if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(frame.Get(), targetPixels, targetPixels,
            WICBitmapInterpolationModeFant)))
        return nullptr;
    source = scaler.Get();
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(source, GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0,
            WICBitmapPaletteTypeCustom)))
        return nullptr;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = targetPixels;
    info.bmiHeader.biHeight = -targetPixels;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(
        nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    const UINT stride = static_cast<UINT>(targetPixels * 4);
    if (!bitmap || !pixels || FAILED(converter->CopyPixels(nullptr, stride,
            stride * static_cast<UINT>(targetPixels),
            static_cast<BYTE*>(pixels))))
    {
        if (bitmap) DeleteObject(bitmap);
        return nullptr;
    }
    return bitmap;
}

HFONT CreateUiFont(HWND hwnd, int points, int weight)
{
    LOGFONTW font{};
    font.lfHeight = -MulDiv(points,
        static_cast<int>(GetDpiForWindow(hwnd)), 72);
    font.lfWeight = weight;
    wcscpy_s(font.lfFaceName, L"Segoe UI");
    return CreateFontIndirectW(&font);
}
}

WidgetNotificationPresenter::Toast::~Toast()
{
    if (image) DeleteObject(image);
}

WidgetNotificationPresenter::~WidgetNotificationPresenter()
{
    Shutdown();
}

bool WidgetNotificationPresenter::EnsureWindowClass()
{
    static std::once_flag once;
    static bool registered = false;
    std::call_once(once, [] {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = WindowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = nullptr;
        windowClass.lpszClassName = WindowClassName;
        registered = RegisterClassExW(&windowClass) != 0 ||
            GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    });
    return registered;
}

bool WidgetNotificationPresenter::Contains(
    std::string_view notificationId) const
{
    return toasts_.find(std::string(notificationId)) != toasts_.end();
}

bool WidgetNotificationPresenter::Show(HWND owner,
    const snowdesktop::widget_runtime::WidgetNotificationHostRequest& request)
{
    if (!owner || request.id.empty() || request.title.empty() ||
        request.message.empty() || !EnsureWindowClass())
        return false;
    if (Contains(request.id)) return Update(owner, request);

    auto toast = std::make_unique<Toast>();
    toast->presenter = this;
    toast->owner = owner;
    toast->request = request;
    toast->hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        WindowClassName, L"", WS_POPUP,
        0, 0, 1, 1, owner, nullptr, GetModuleHandleW(nullptr), toast.get());
    if (!toast->hwnd) return false;
    if (!request.imagePath.empty())
        toast->image = LoadImage(request.imagePath, Scale(toast->hwnd, 48));
    const HWND hwnd = toast->hwnd;
    toasts_.emplace(request.id, std::move(toast));
    Reflow(owner);
    SetTimer(hwnd, DismissTimerId, DismissDelayMs, nullptr);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
    return true;
}

bool WidgetNotificationPresenter::Update(HWND owner,
    const snowdesktop::widget_runtime::WidgetNotificationHostRequest& request)
{
    const auto found = toasts_.find(request.id);
    if (found == toasts_.end()) return false;
    Toast& toast = *found->second;
    toast.owner = owner ? owner : toast.owner;
    toast.request = request;
    if (toast.image)
    {
        DeleteObject(toast.image);
        toast.image = nullptr;
    }
    if (!request.imagePath.empty())
        toast.image = LoadImage(request.imagePath, Scale(toast.hwnd, 48));
    Reflow(toast.owner);
    SetTimer(toast.hwnd, DismissTimerId, DismissDelayMs, nullptr);
    InvalidateRect(toast.hwnd, nullptr, FALSE);
    return true;
}

bool WidgetNotificationPresenter::Dismiss(std::string_view notificationId)
{
    const auto found = toasts_.find(std::string(notificationId));
    if (found == toasts_.end()) return false;
    Toast* toast = found->second.get();
    if (toast->hwnd)
    {
        KillTimer(toast->hwnd, DismissTimerId);
        DestroyWindow(toast->hwnd);
        toast->hwnd = nullptr;
    }
    const HWND owner = toast->owner;
    toasts_.erase(found);
    Reflow(owner);
    return true;
}

void WidgetNotificationPresenter::Shutdown()
{
    while (!toasts_.empty())
        (void)Dismiss(toasts_.begin()->first);
    actionCallback_ = {};
}

void WidgetNotificationPresenter::Reflow(HWND owner)
{
    HMONITOR monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{ sizeof(info) };
    if (!GetMonitorInfoW(monitor, &info)) return;
    int y = info.rcWork.top + Scale(owner, 12);
    const int gap = Scale(owner, 10);
    const int width = Scale(owner, 360);
    for (auto& [id, entry] : toasts_)
    {
        Toast& toast = *entry;
        if (toast.owner != owner) continue;
        const int contentHeight = Scale(toast.hwnd,
            16 + 68 + (toast.request.progress ? 18 : 0) +
            (toast.request.actions.empty() ? 0 : 46) + 14);
        toast.height = contentHeight;
        const int x = info.rcWork.right - width - Scale(owner, 12);
        SetWindowPos(toast.hwnd, HWND_TOPMOST, x, y, width, toast.height,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
        HRGN region = CreateRoundRectRgn(
            0, 0, width + 1, toast.height + 1,
            Scale(toast.hwnd, 12), Scale(toast.hwnd, 12));
        if (region && !SetWindowRgn(toast.hwnd, region, TRUE))
            DeleteObject(region);
        y += toast.height + gap;
    }
}

void WidgetNotificationPresenter::Paint(Toast& toast)
{
    PAINTSTRUCT paint{};
    HDC target = BeginPaint(toast.hwnd, &paint);
    RECT client{};
    GetClientRect(toast.hwnd, &client);
    const int width = client.right;
    const int height = client.bottom;
    HDC buffer = CreateCompatibleDC(target);
    HBITMAP surface = CreateCompatibleBitmap(target, width, height);
    HGDIOBJ oldSurface = SelectObject(buffer, surface);

    HBRUSH background = CreateSolidBrush(RGB(37, 40, 48));
    HPEN border = CreatePen(PS_SOLID, 1, RGB(78, 84, 99));
    HGDIOBJ oldBrush = SelectObject(buffer, background);
    HGDIOBJ oldPen = SelectObject(buffer, border);
    RoundRect(buffer, 0, 0, width, height,
        Scale(toast.hwnd, 12), Scale(toast.hwnd, 12));

    SetBkMode(buffer, TRANSPARENT);
    const int padding = Scale(toast.hwnd, 16);
    const int closeSize = Scale(toast.hwnd, 24);
    toast.closeRect = { width - padding - closeSize, padding,
        width - padding, padding + closeSize };
    SetTextColor(buffer, RGB(180, 185, 197));
    HFONT closeFont = CreateUiFont(toast.hwnd, 12, FW_NORMAL);
    HGDIOBJ oldFont = SelectObject(buffer, closeFont);
    DrawTextW(buffer, L"\u00D7", 1, &toast.closeRect,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    int textLeft = padding;
    if (!toast.request.imagePath.empty())
    {
        const int imageSize = Scale(toast.hwnd, 48);
        if (toast.image)
        {
            HDC imageDc = CreateCompatibleDC(buffer);
            HGDIOBJ oldImage = SelectObject(imageDc, toast.image);
            BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            AlphaBlend(buffer, padding, padding + Scale(toast.hwnd, 6),
                imageSize, imageSize, imageDc, 0, 0,
                imageSize, imageSize, blend);
            SelectObject(imageDc, oldImage);
            DeleteDC(imageDc);
        }
        textLeft += imageSize + Scale(toast.hwnd, 14);
    }
    RECT titleRect{ textLeft, padding,
        toast.closeRect.left - Scale(toast.hwnd, 8),
        padding + Scale(toast.hwnd, 24) };
    HFONT titleFont = CreateUiFont(toast.hwnd, 11, FW_SEMIBOLD);
    SelectObject(buffer, titleFont);
    SetTextColor(buffer, RGB(247, 248, 250));
    DrawTextW(buffer, toast.request.title.c_str(), -1, &titleRect,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT messageRect{ textLeft, titleRect.bottom + Scale(toast.hwnd, 4),
        width - padding, padding + Scale(toast.hwnd, 68) };
    HFONT bodyFont = CreateUiFont(toast.hwnd, 10, FW_NORMAL);
    SelectObject(buffer, bodyFont);
    SetTextColor(buffer, RGB(202, 206, 216));
    DrawTextW(buffer, toast.request.message.c_str(), -1, &messageRect,
        DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);

    int cursorY = padding + Scale(toast.hwnd, 72);
    if (toast.request.progress)
    {
        RECT track{ padding, cursorY,
            width - padding, cursorY + Scale(toast.hwnd, 6) };
        HBRUSH trackBrush = CreateSolidBrush(RGB(73, 78, 91));
        FillRect(buffer, &track, trackBrush);
        DeleteObject(trackBrush);
        RECT fill = track;
        fill.right = fill.left + static_cast<LONG>(std::lround(
            (fill.right - fill.left) * *toast.request.progress));
        HBRUSH fillBrush = CreateSolidBrush(RGB(104, 163, 255));
        FillRect(buffer, &fill, fillBrush);
        DeleteObject(fillBrush);
        cursorY += Scale(toast.hwnd, 18);
    }

    toast.actionRects.clear();
    if (!toast.request.actions.empty())
    {
        const int actionGap = Scale(toast.hwnd, 8);
        const int actionWidth = (width - 2 * padding -
            actionGap * (static_cast<int>(toast.request.actions.size()) - 1)) /
            static_cast<int>(toast.request.actions.size());
        const int actionHeight = Scale(toast.hwnd, 32);
        for (std::size_t index = 0;
            index < toast.request.actions.size(); ++index)
        {
            RECT actionRect{
                padding + static_cast<int>(index) * (actionWidth + actionGap),
                cursorY,
                padding + static_cast<int>(index) * (actionWidth + actionGap) +
                    actionWidth,
                cursorY + actionHeight };
            toast.actionRects.push_back(actionRect);
            HBRUSH actionBrush = CreateSolidBrush(
                index == 0 ? RGB(63, 115, 194) : RGB(60, 64, 75));
            FillRect(buffer, &actionRect, actionBrush);
            DeleteObject(actionBrush);
            SetTextColor(buffer, RGB(247, 248, 250));
            DrawTextW(buffer, toast.request.actions[index].label.c_str(),
                -1, &actionRect,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }

    BitBlt(target, 0, 0, width, height, buffer, 0, 0, SRCCOPY);
    SelectObject(buffer, oldFont);
    DeleteObject(bodyFont);
    DeleteObject(titleFont);
    DeleteObject(closeFont);
    SelectObject(buffer, oldPen);
    SelectObject(buffer, oldBrush);
    DeleteObject(border);
    DeleteObject(background);
    SelectObject(buffer, oldSurface);
    DeleteObject(surface);
    DeleteDC(buffer);
    EndPaint(toast.hwnd, &paint);
}

void WidgetNotificationPresenter::HandleClick(Toast& toast, POINT point)
{
    const std::string notificationId = toast.request.id;
    if (PtInRect(&toast.closeRect, point))
    {
        (void)Dismiss(notificationId);
        return;
    }
    for (std::size_t index = 0; index < toast.actionRects.size(); ++index)
    {
        if (!PtInRect(&toast.actionRects[index], point)) continue;
        const std::string actionId = toast.request.actions[index].id;
        if (actionCallback_) actionCallback_(notificationId, actionId);
        (void)Dismiss(notificationId);
        return;
    }
}

LRESULT CALLBACK WidgetNotificationPresenter::WindowProc(
    HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    Toast* toast = reinterpret_cast<Toast*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        toast = static_cast<Toast*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(toast));
        toast->hwnd = hwnd;
    }
    if (!toast) return DefWindowProcW(hwnd, message, wParam, lParam);
    switch (message)
    {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        toast->presenter->Paint(*toast);
        return 0;
    case WM_LBUTTONUP:
        toast->presenter->HandleClick(*toast,
            POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
        return 0;
    case WM_TIMER:
        if (wParam == DismissTimerId)
        {
            const std::string id = toast->request.id;
            toast->presenter->Dismiss(id);
            return 0;
        }
        break;
    case WM_NCHITTEST:
        return HTCLIENT;
    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}
