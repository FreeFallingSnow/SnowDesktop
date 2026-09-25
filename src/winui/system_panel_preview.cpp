#include "pch.h"
#include "system_panel_preview.h"
#include "system_panel_surface.h"
#include "system_calendar_view.h"
#include "winui_runtime.h"
#include "../l10n.h"
#include "../data_paths.h"
#include "../preview_png_writer.h"
#include "../widget_preview_stage.h"
#include <robuffer.h>
#include <cmath>
#include <stdexcept>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Globalization.h>

namespace snowdesktop::winui
{
namespace x = winrt::Microsoft::UI::Xaml;
namespace
{
template<class Ready> void PumpUntil(Ready ready)
{
    const auto deadline = GetTickCount64() + 15000;
    while (!ready())
    {
        if (GetTickCount64() >= deadline) throw std::runtime_error("XAML preview operation timed out");
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT) throw std::runtime_error("XAML preview closed");
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        if (!ready()) MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
}
template<class Operation> auto Await(const Operation& operation)
{
    try { PumpUntil([&] { return operation.Status() != winrt::Windows::Foundation::AsyncStatus::Started; }); }
    catch (...) { operation.Cancel(); throw; }
    return operation.GetResults();
}
struct PreviewHost
{
    WinUiRuntime runtime;
    HWND window = nullptr;
    ~PreviewHost()
    {
        runtime.Detach();
        if (window) DestroyWindow(window);
        runtime.Shutdown();
    }
};
struct ComScope
{
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
};
void CheckMonthFits(const x::DependencyObject& element, const x::Controls::CalendarView& month = nullptr,
    bool* foundLastDay = nullptr)
{
    if (const auto calendar = element.try_as<x::Controls::CalendarView>())
    {
        bool found = false;
        for (int i = 0; i < x::Media::VisualTreeHelper::GetChildrenCount(calendar); ++i)
            CheckMonthFits(x::Media::VisualTreeHelper::GetChild(calendar, i), calendar, &found);
        if (!found) throw std::runtime_error("calendar preview clipped the last day of the fixture month");
        return;
    }
    if (month)
        if (const auto day = element.try_as<x::Controls::CalendarViewDayItem>())
        {
            winrt::Windows::Globalization::Calendar date;
            date.ChangeCalendarSystem(L"GregorianCalendar"); date.SetDateTime(day.Date());
            if (date.Year() == 2026 && date.Month() == 9 && date.Day() == 30)
            {
                const auto bounds = day.TransformToVisual(month).TransformBounds(
                    {0, 0, static_cast<float>(day.ActualWidth()), static_cast<float>(day.ActualHeight())});
                *foundLastDay = bounds.Width > 0 && bounds.Height > 0 && bounds.Y >= 0 &&
                    bounds.Y + bounds.Height <= month.ActualHeight() + 1;
            }
        }
    for (int i = 0; i < x::Media::VisualTreeHelper::GetChildrenCount(element); ++i)
        CheckMonthFits(x::Media::VisualTreeHelper::GetChild(element, i), month, foundLastDay);
}
}
native_component_preview::Result ExportCalendarPanelPreview(
    const native_component_preview::Request& request, PersonalizationSettings appearance)
{
    native_component_preview::Result result; result.request = request;
    try
    {
        result.stage = "panel.appearance";
        // RenderTargetBitmap captures XAML, not the compositor's desktop blur.
        // Keep that unsupported boundary explicit instead of exporting fake glass.
        if (appearance.glassEnabled || appearance.acrylicEnabled)
            throw std::runtime_error("calendar-panel currently supports light/dark XAML surfaces; desktop blur requires separate compositor verification");
        if (request.contentOnly) { appearance.widgetAlpha = 0; appearance.widgetBorderAlpha = 0; }
        result.stage = "panel.locale";
        const auto languageDirectory = std::filesystem::path(GetExecutableDirectoryPath()) / L"lang";
        Locale::Instance().Init(languageDirectory.c_str());
        if (request.locale != "system" && !Locale::Instance().HasLanguage(request.locale))
            throw std::runtime_error("requested preview locale is not installed");
        Locale::Instance().SetLanguage(request.locale.c_str());
        ComScope com; winrt::check_hresult(com.result);
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        PreviewHost host;
        result.stage = "panel.runtime";
        if (!host.runtime.Initialize()) throw winrt::hresult_error(E_FAIL, host.runtime.LastError());
        // An isolated render window, never a DesktopApp, AppBar or Shell hook.
        // RTB supports offscreen content provided it is attached and not collapsed.
        host.window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
            L"STATIC", L"SnowDesktop offline calendar render", WS_POPUP,
            -32000, -32000, 900, 900, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!host.window) winrt::throw_last_error();
        ShowWindow(host.window, SW_SHOWNOACTIVATE);
        constexpr float widthDip = 520;
        for (const bool populated : {false, true})
        {
            result.stage = "panel.tree";
            SystemCalendarActions actions;
            actions.today = [] { return std::string("2026-09-26"); };
            actions.manage = [] {}; // Preview never opens settings or changes events.
            actions.events = [populated](const std::string& date) {
                std::vector<calendar::CalendarEvent> events;
                if (!populated) return events;
                calendar::CalendarEvent first; first.id = "preview-brunch"; first.revision = 1;
                first.date = date; first.title = _L("app.widget_preview.api.calendar_review"); first.startMinutes = 630; first.endMinutes = 690;
                calendar::CalendarEvent second; second.id = "preview-weekend"; second.revision = 1;
                second.date = date; second.title = _L("app.widget_preview.api.calendar_publish"); second.allDay = true;
                events.push_back(std::move(first)); events.push_back(std::move(second)); return events;
            };
            SystemCalendarView calendar(std::move(actions));
            auto frame = CreateSystemPanelFrame(appearance); frame.Child(calendar.Root());
            bool loaded = false;
            auto loadedEvent = frame.Loaded(winrt::auto_revoke, [&](const auto&, const auto&) { loaded = true; });
            if (!host.runtime.Attach(host.window, frame)) throw winrt::hresult_error(E_FAIL, host.runtime.LastError());
            PumpUntil([&] { return loaded; }); loadedEvent.revoke();
            frame.Measure({widthDip, 1000});
            const float heightDip = std::ceil(frame.DesiredSize().Height);
            const double windowScale = GetDpiForWindow(host.window) / 96.;
            SetWindowPos(host.window, nullptr, 0, 0, static_cast<int>(std::ceil(widthDip * windowScale)),
                static_cast<int>(std::ceil(heightDip * windowScale)), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            host.runtime.ResizeToClient();
            // SiteBridge sizing is dispatched asynchronously. UpdateLayout by
            // itself can still arrange against the initial 900-pixel island.
            const double layoutHeight = std::ceil(heightDip * windowScale) / windowScale;
            PumpUntil([&] {
                frame.UpdateLayout();
                return std::abs(frame.ActualWidth() - widthDip) < .51 / windowScale &&
                    std::abs(frame.ActualHeight() - layoutHeight) < .51 / windowScale;
            });
            int width = static_cast<int>(std::ceil(frame.ActualWidth() * request.dpi / 96.));
            int height = static_cast<int>(std::ceil(frame.ActualHeight() * request.dpi / 96.));
            if (width + request.padding * 2 > request.canvasWidth || height + request.padding * 2 > request.canvasHeight)
                throw std::runtime_error("preview canvas is too small for the calendar panel");
            result.stage = "panel.bitmap";
            x::Media::Imaging::RenderTargetBitmap bitmap;
            // WinUI's island rasterizer applies the XamlRoot scale to these
            // dimensions. Convert from requested output pixels exactly once.
            const double rasterScale = frame.XamlRoot().RasterizationScale();
            const auto renderWidth = static_cast<int>(std::lround(width / rasterScale));
            const auto renderHeight = static_cast<int>(std::lround(height / rasterScale));
            Await(bitmap.RenderAsync(frame, renderWidth, renderHeight));
            const auto buffer = Await(bitmap.GetPixelsAsync());
            // Check the real visual tree as well as outer pixels. A valid PNG
            // and rounded frame must not mask missing dates inside the month.
            CheckMonthFits(frame);
            // Allow one-pixel rounding at a fractional rasterization scale.
            // Reject a stale layout or missing pixels; retain the real size in
            // both the image and its metadata rather than stretching it.
            const int renderedWidth = bitmap.PixelWidth(), renderedHeight = bitmap.PixelHeight();
            if (renderedWidth <= 0 || renderedHeight <= 0 ||
                std::abs(renderedWidth - width) > 1 || std::abs(renderedHeight - height) > 1 ||
                buffer.Length() != static_cast<unsigned>(renderedWidth * renderedHeight * 4))
                throw std::runtime_error("calendar bitmap mismatch: requested=" + std::to_string(width) + "x" +
                    std::to_string(height) + ", returned=" + std::to_string(renderedWidth) + "x" +
                    std::to_string(renderedHeight) + ", bytes=" + std::to_string(buffer.Length()));
            width = renderedWidth; height = renderedHeight;
            BYTE* bytes = nullptr;
            winrt::check_hresult(buffer.as<::Windows::Storage::Streams::IBufferByteAccess>()->Buffer(&bytes));
            widget_preview::Wallpaper canvas;
            if (request.transparent || request.contentOnly)
                canvas = {request.canvasWidth, request.canvasHeight, std::vector<std::uint32_t>(static_cast<std::size_t>(request.canvasWidth) * request.canvasHeight)};
            else if (!request.backgroundImage.empty())
                canvas = widget_preview::GenerateWallpaper(widget_preview::LoadWallpaperImage(request.backgroundImage), request.canvasWidth, request.canvasHeight);
            else canvas = widget_preview::GenerateWallpaper(request.canvasWidth, request.canvasHeight, appearance.contentTheme == 1);
            const int left = (canvas.width - width) / 2, top = (canvas.height - height) / 2;
            const auto* pixels = reinterpret_cast<const std::uint32_t*>(bytes);
            for (int y = 0; y < height; ++y) for (int col = 0; col < width; ++col)
            {
                const auto source = pixels[y * width + col];
                auto& destination = canvas.pixels[static_cast<std::size_t>(top + y) * canvas.width + left + col];
                const unsigned inverse = 255 - (source >> 24);
                std::uint32_t composed = 0;
                for (unsigned shift : {0u, 8u, 16u, 24u})
                    composed |= (std::min)(255u, ((source >> shift) & 255) + ((((destination >> shift) & 255) * inverse + 127) / 255)) << shift;
                destination = composed;
            }
            const std::string preset = populated ? "agenda" : "empty";
            const auto path = request.outputDirectory / ("calendar-panel-" + preset + ".png");
            result.stage = "panel.png";
            if (!preview_png::Save(path, canvas.width, canvas.height, canvas.pixels, result.error)) return result;
            result.outputs.push_back({"calendar-panel", preset, path, false, false, false, false, false, false,
                static_cast<int>(std::lround(appearance.cornerRadius * request.dpi / 96.)), width, height, left, top});
            host.runtime.Detach();
        }
        result.ok = true; result.stage = "complete";
    }
    catch (const winrt::hresult_error& error) { result.error = winrt::to_string(error.message()); }
    catch (const std::exception& error) { result.error = error.what(); }
    return result;
}
}
