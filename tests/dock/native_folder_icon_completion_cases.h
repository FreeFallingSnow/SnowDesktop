#pragma once
#include "app/app.h"
#include "icons/icon_render_rules.h"
#include <cstring>
#include <d3d11.h>
#include <iostream>
#include <stdexcept>
#include <cwchar>

struct DockFolderIconCompletionTestAccess
{
    // The provider boundary supplies deterministic HBITMAPs; completion, cache
    // publication, derived-image retirement, HWND damage and drawing stay real.
    static DesktopApp& FolderIconScene(ID2D1DeviceContext* context, HWND window)
    {
        auto& app = *new DesktopApp;
        app.hwnd_ = window; app.d2dContext_ = context;
        app.dockIconWork_.Stop(); // Never query the machine's Shell in this fixture.
        app.shellVisualWork_.Stop(); app.dockIdentityWork_.Stop();
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(app.dwriteFactory_.GetAddressOf()))) ||
            FAILED(app.dwriteFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-US", &app.itemTextFormat_)))
            throw std::runtime_error("R09 initializes real title drawing and measurement");
        app.itemTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        app.iconBeautifySettings_ = snowdesktop::icon_beautify::MakePreset(
            snowdesktop::IconBeautifyPreset::DefaultBeautify);
        app.iconBeautifySettings_.glassEnabled = false;
        app.generalSettings_.dockEnabled = true;
        app.dockSettings_.showWindowsButton = false;
        app.dockSettings_.showQuickSearchButton = false;
        app.dockSettings_.hoverEffect = 0;
        app.lastMousePoint_ = {LONG_MIN, LONG_MIN};
        for (int index = 0; index != 2; ++index)
        {
            DesktopWidget widget;
            widget.type = DesktopWidgetType::FolderMapping;
            widget.id = index ? L"r09-other" : L"r09-target";
            widget.title = index ? L"Other folder" : L"Selected folder with an expanded title";
            widget.selected = index == 0;
            widget.sourceFolderPath = index ? L"Z:\\r09-other" : L"Z:\\r09-target";
            widget.gridSpan = {1, 1}; widget.singleCellIconMode = true;
            widget.bounds = index ? RECT{520, 120, 616, 216} : RECT{120, 120, 216, 216};
            app.widgets_.push_back(std::move(widget));
            app.dockEntries_.push_back({DockEntryType::FolderMapping, app.widgets_.back().id, true});
        }
        app.containers_.push_back(std::make_unique<DockContainer>(&app, &app.dockEntries_, RECT{20, 440, 380, 560}));
        app.containers_.push_back(std::make_unique<DockContainer>(&app, &app.dockEntries_, RECT{420, 440, 780, 560}));
        return app;
    }
    static std::wstring FolderIconKey(const DesktopApp& app, size_t index = 0)
    { return ToUpperInvariant(app.widgets_[index].id + L"\n" + app.widgets_[index].sourceFolderPath); }
    static auto FolderIconRead(DesktopApp& app, bool expire = false, size_t index = 0)
    {
        using Clock = snowdesktop::dock_refresh_cache::Cache<ComPtr<ID2D1Bitmap1>>::Clock;
        return app.dockFolderBitmapCache_.Read(FolderIconKey(app, index),
            std::to_wstring(app.GetMaximumShellIconBitmapSize()),
            Clock::now() + (expire ? std::chrono::seconds(6) : std::chrono::seconds(0)));
    }
    static void DeliverFolderIcon(DesktopApp& app, const std::wstring& key,
        std::uint64_t ticket, HBITMAP source, bool refined, bool reference)
    {
        if (!reference) { app.OnDockFolderIconLoaded(key, ticket, source, refined); return; }
        // Frozen R09 pre-change callback, before/manifest.json D2844D2A... .
        // Execute its broad invalidation as the isolated negative control.
        auto bitmap = source ? app.CreateD2DBitmapFromHBitmap(source, false) : ComPtr<ID2D1Bitmap1>{};
        if (bitmap)
        {
            if (!app.dockFolderBitmapCache_.Publish(key, ticket, std::move(bitmap), refined)) return;
        }
        else if (!refined || !app.dockFolderBitmapCache_.PublishFailure(key, ticket, std::chrono::seconds(5))) return;
        app.ClearDockAppearanceImages();
        app.InvalidateDragStaticScene();
        app.InvalidateDockRects();
        if (app.hwnd_) InvalidateRect(app.hwnd_, nullptr, FALSE);
    }
    static void SetFolderCollection(DesktopApp& app, bool pending = false)
    { app.desktopBackdropFullCollectionPending_ = pending; }
    static bool FolderCollectionPending(const DesktopApp& app)
    { return app.desktopBackdropFullCollectionPending_; }
    static void SetFolderGlass(DesktopApp& app, bool enabled)
    { app.iconBeautifySettings_.enabled = true; app.iconBeautifySettings_.glassEnabled = enabled; }
    static void AddUnrelatedRootIcon(DesktopApp& app)
    {
        for (auto& widget : app.widgets_) widget.bounds = {};
        DesktopItem item;
        item.name = L"Unrelated root icon";
        item.layoutKey = item.parsingName = L"Z:\\r09-unrelated-root";
        item.bounds = {24, 24, 120, 120};
        snowdesktop::icon_image::Image image;
        image.width = image.height = 32; image.pixels.assign(32 * 32, 0xff4090d0);
        item.iconBitmap = snowdesktop::icon_image::CreateBitmap(image);
        if (!item.iconBitmap) throw std::runtime_error("R09 creates owned root sentinel pixels");
        app.items_.push_back(std::move(item));
        app.items_oo_.push_back(std::make_unique<DesktopIcon>(&app.items_.back(), nullptr, &app));
    }
    static void ClearRootProbeResources(DesktopApp& app)
    { app.ClearStyledIconCache(); app.d2dIconCache_.clear(); }
    static size_t RootProbeResources(const DesktopApp& app)
    {
        const auto source = reinterpret_cast<std::uintptr_t>(app.items_.front().iconBitmap);
        return static_cast<size_t>(std::count_if(app.styledIconCache_.begin(), app.styledIconCache_.end(),
            [source](const auto& entry) { return std::get<0>(entry.first) == source; }));
    }
    static std::uint64_t FolderIconSceneRevision(const DesktopApp& app)
    { return app.dragSession_.StaticSceneRevision(); }
    static void InvalidateFolderIconProbe(DesktopApp& app)
    { app.dockFolderBitmapCache_.Invalidate(); }
    static void ChangeFolderIconPath(DesktopApp& app)
    { app.widgets_.front().sourceFolderPath = L"Z:\\r09-changed"; }
    static void SetFolderIconMode(DesktopApp& app, bool icon)
    { app.widgets_.front().singleCellIconMode = icon; }
    static void UnpinFolderIconProbe(DesktopApp& app)
    { app.dockEntries_.clear(); for (auto& container : app.containers_) container->InvalidateSlots(); }
    static void HostFolderIconDocks(DesktopApp& app, HWND first, HWND second)
    {
        for (auto& widget : app.widgets_) widget.bounds = {};
        for (size_t index = 0; index != 2; ++index)
        {
            auto host = std::make_unique<DesktopApp::PersistentDockHost>();
            host->owner = &app; host->container = dynamic_cast<DockContainer*>(app.containers_[index].get());
            host->hwnd = index ? second : first; host->active = true;
            app.persistentDockHosts_.push_back(std::move(host));
        }
        app.floatingDockHostActive_ = true;
    }
    static bool HasFolderAppearance(const DesktopApp& app, size_t index)
    {
        const auto found = app.dockAppearanceImages_.find(L"widget:" + app.widgets_[index].id + L":" +
            std::to_wstring(snowdesktop::icon_render_rules::kMaximumSourcePixels));
        return found != app.dockAppearanceImages_.end() &&
            app.iconGlassBackdrop_.contains(found->second.bitmap.Get()) &&
            app.iconBitmapStyles_.contains(found->second.bitmap.Get());
    }
    static auto FolderIconGeometry(DesktopApp& app)
    {
        std::vector<LONG> result;
        for (const auto& widget : app.widgets_)
        {
            result.push_back(widget.selected ? 1 : 0);
            result.push_back(widget.singleCellIconMode ? 1 : 0);
        }
        for (const auto& container : app.containers_)
        {
            const auto* dock = dynamic_cast<DockContainer*>(container.get());
            if (!dock) continue;
            const RECT bounds = dock->GetInteractiveBounds();
            result.insert(result.end(), {bounds.left, bounds.top, bounds.right, bounds.bottom});
            for (const auto& slot : container->GetSlots())
            {
                const RECT rect = slot->GetBounds();
                result.insert(result.end(), {rect.left, rect.top, rect.right, rect.bottom});
                const POINT center{(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2};
                result.push_back(dock->EntryAtPoint(center) != nullptr ? 1 : 0);
            }
        }
        return result;
    }
    static std::vector<std::uint32_t> DrawFolderIconScene(DesktopApp& app, ID2D1DeviceContext* context,
        HRGN damage = nullptr, const std::vector<std::uint32_t>* previous = nullptr, bool rootScene = false)
    {
        auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        ComPtr<ID2D1Bitmap1> target, readable;
        const auto require = [](HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("R09 WARP scene/readback"); };
        require(context->CreateBitmap(D2D1::SizeU(800, 600), previous ? previous->data() : nullptr,
            previous ? 3200u : 0u, &properties, &target));
        context->SetTarget(target.Get()); context->BeginDraw();
        context->SetTransform(D2D1::Matrix3x2F::Identity());
        const auto draw = [&] {
            context->Clear(D2D1::ColorF(0.12f, 0.21f, 0.27f, 1.0f));
            if (rootScene)
            {
                RECT dirty{};
                if (damage) GetRgnBox(damage, &dirty);
                // Match app_paint.cpp's complete-collection routing, then
                // execute the real root renderer and DesktopIcon::Draw.
                app.RenderFrame(context, app.desktopBackdropFullCollectionPending_ || !damage
                    ? nullptr : &dirty, false);
                return;
            }
            for (const auto& container : app.containers_)
                if (auto* dock = dynamic_cast<DockContainer*>(container.get())) dock->DrawContents(context);
            for (auto& widget : app.widgets_)
            {
                if (IsRectEmpty(&widget.bounds)) continue;
                // Real widgets redraw a complete compact child surface; the
                // root update rectangle never clips their primitive raster.
                // Compose that independent surface into the damage receiver.
                const RECT frame = app.GetStandaloneWidgetFrameRect(widget);
                ComPtr<ID2D1Device> device; context->GetDevice(&device);
                ComPtr<ID2D1DeviceContext> child;
                require(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &child));
                ComPtr<ID2D1Bitmap1> surface;
                require(child->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(frame.right - frame.left),
                    static_cast<UINT32>(frame.bottom - frame.top)), nullptr, 0, &properties, &surface));
                child->SetTarget(surface.Get()); child->BeginDraw();
                child->SetDpi(96.0f, 96.0f); child->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
                child->SetTransform(D2D1::Matrix3x2F::Translation(
                    -static_cast<float>(frame.left), -static_cast<float>(frame.top)));
                child->Clear(D2D1::ColorF(0, 0, 0, 0));
                app.brushCache_.clear(); app.brushCacheContext_ = child.Get();
                app.desktopWidgetBackdropRequestedDuringDraw_ = false;
                const bool wasChild = std::exchange(app.desktopWidgetCompositionDrawInProgress_, true);
                Widget entrance(&widget, &app);
                entrance.Draw(child.Get(), widget.bounds, widget.selected ? 2 : 0);
                app.desktopWidgetCompositionDrawInProgress_ = wasChild;
                app.desktopWidgetBackdropRequestedDuringDraw_ = false;
                require(child->EndDraw()); child->SetTarget(nullptr);
                app.brushCache_.clear(); app.brushCacheContext_ = nullptr;
                context->DrawBitmap(surface.Get(), D2D1::RectF(static_cast<float>(frame.left),
                    static_cast<float>(frame.top), static_cast<float>(frame.right),
                    static_cast<float>(frame.bottom)), 1.0f, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
            }
        };
        if (!damage) draw();
        else
        {
            const DWORD bytes = GetRegionData(damage, 0, nullptr);
            if (!bytes) throw std::runtime_error("R09 reads damage rectangles for actual partial D2D redraw");
            std::vector<std::uint32_t> regionStorage((bytes + 3) / 4);
            auto* region = reinterpret_cast<RGNDATA*>(regionStorage.data());
            if (!GetRegionData(damage, bytes, region)) throw std::runtime_error("R09 damage rectangle data");
            const auto* rects = reinterpret_cast<const RECT*>(region->Buffer);
            for (DWORD index = 0; index < region->rdh.nCount; ++index)
            {
                context->PushAxisAlignedClip(D2D1::RectF(static_cast<float>(rects[index].left),
                    static_cast<float>(rects[index].top), static_cast<float>(rects[index].right),
                    static_cast<float>(rects[index].bottom)), D2D1_ANTIALIAS_MODE_ALIASED);
                draw(); context->PopAxisAlignedClip();
            }
        }
        require(context->EndDraw()); context->SetTarget(nullptr);
        properties.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
        require(context->CreateBitmap(D2D1::SizeU(800, 600), nullptr, 0, &properties, &readable));
        require(readable->CopyFromBitmap(nullptr, target.Get(), nullptr));
        D2D1_MAPPED_RECT mapped{}; require(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped));
        std::vector<std::uint32_t> pixels(800 * 600);
        for (size_t row = 0; row != 600; ++row)
            std::memcpy(pixels.data() + row * 800, mapped.bits + row * mapped.pitch, 800 * sizeof(std::uint32_t));
        require(readable->Unmap());
        return pixels;
    }
    static void StopFolderIconScene(DesktopApp& app)
    {
        app.dockIconWork_.Stop(); app.ClearDockAppearanceImages(); app.dockFolderBitmapCache_.Retain([](const auto&) { return false; });
        app.iconGlassBackdrop_.clear(); app.iconBitmapStyles_.clear(); app.d2dContext_.Reset();
        app.itemTextLayoutCache_.clear(); app.itemTextFormat_.Reset(); app.dwriteFactory_.Reset();
        app.persistentDockHosts_.clear(); app.containers_.clear(); app.items_oo_.clear();
        app.ClearStyledIconCache(); app.d2dIconCache_.clear(); app.items_.clear(); app.hwnd_ = nullptr;
    }

};


namespace native_folder_icon_completion_cases
{
using Access = DockFolderIconCompletionTestAccess;

inline void Require(bool value, const char* message)
{ if (!value) throw std::runtime_error(message); }

struct Window
{
    unsigned paints = 0;
    HWND value = nullptr;
    static LRESULT CALLBACK Procedure(HWND hwnd, UINT message, WPARAM wp, LPARAM lp)
    {
        if (message == WM_NCCREATE)
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(
                reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        if (message == WM_PAINT)
        {
            auto* owner = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (owner) ++owner->paints;
            PAINTSTRUCT paint{}; BeginPaint(hwnd, &paint); EndPaint(hwnd, &paint); return 0;
        }
        return DefWindowProcW(hwnd, message, wp, lp);
    }
    Window()
    {
        static const ATOM type = [] {
            WNDCLASSW klass{}; klass.lpfnWndProc = Procedure; klass.hInstance = GetModuleHandleW(nullptr);
            klass.lpszClassName = L"SnowDesktopR09DamageFixture";
            return RegisterClassW(&klass);
        }();
        Require(type != 0, "R09 registers isolated fixture class");
        // Visible receivers retain Win32 damage on this never-activated desktop.
        value = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            L"SnowDesktopR09DamageFixture", L"R09 isolated damage owner", WS_POPUP | WS_VISIBLE,
            0, 0, 800, 600, nullptr, nullptr, GetModuleHandleW(nullptr), this);
        Require(value != nullptr, "R09 creates a damage receiver on an inactive private desktop");
        ValidateRect(value, nullptr);
    }
    ~Window() { DestroyWindow(value); }
};

void Run(ID2D1DeviceContext* context)
{
    std::cerr << "R09 completion cases: private receivers and models\n";
    Window currentWindow, referenceWindow;
    auto& current = Access::FolderIconScene(context, currentWindow.value);
    auto& reference = Access::FolderIconScene(context, referenceWindow.value);
    snowdesktop::icon_image::Image image;
    image.width = image.height = 64; image.pixels.assign(64 * 64, 0xff2080e0);
    const auto first = snowdesktop::icon_image::CreateBitmap(image);
    image.pixels.assign(64 * 64, 0xffe04020);
    const auto refined = snowdesktop::icon_image::CreateBitmap(image);
    Require(first && refined, "R09 deterministic source HBITMAPs exist");

    const auto otherCurrent = Access::FolderIconRead(current, false, 1);
    const auto otherReference = Access::FolderIconRead(reference, false, 1);
    Access::DeliverFolderIcon(current, Access::FolderIconKey(current, 1), otherCurrent.ticket, first, true, false);
    Access::DeliverFolderIcon(reference, Access::FolderIconKey(reference, 1), otherReference.ticket, first, true, true);
    auto currentPixels = Access::DrawFolderIconScene(current, context);
    auto referencePixels = Access::DrawFolderIconScene(reference, context);
    Require(currentPixels == referencePixels, "R09 initial two-mirror and desktop pixels match reference");
    Require(Access::HasFolderAppearance(current, 0) && Access::HasFolderAppearance(current, 1),
        "R09 primes both real derived-image consumers before completion");
    auto currentRead = Access::FolderIconRead(current);
    auto referenceRead = Access::FolderIconRead(reference);
    size_t accepted = 0, rejected = 0;
    unsigned transition = 0;
    const auto deliver = [&](HBITMAP source, bool isRefined, bool expectedAccepted, bool expectedPixelChange,
        std::uint64_t currentTicket, std::uint64_t referenceTicket,
        const std::wstring& currentKey, const std::wstring& referenceKey) {
        std::cerr << "R09 transition=" << ++transition << ", source=" << (source ? "pixels" : "empty")
            << ", refined=" << isRefined << ", accepted=" << expectedAccepted << '\n';
        ValidateRect(currentWindow.value, nullptr); ValidateRect(referenceWindow.value, nullptr);
        const auto geometry = Access::FolderIconGeometry(current);
        const auto oldRevision = Access::FolderIconSceneRevision(current);
        Access::SetFolderCollection(current); Access::SetFolderCollection(reference);
        Access::DeliverFolderIcon(current, currentKey, currentTicket, source, isRefined, false);
        Access::DeliverFolderIcon(reference, referenceKey, referenceTicket, source, isRefined, true);
        std::cerr << "R09 transition=" << transition << ", both completion routes returned\n";
        HRGN damage = CreateRectRgn(0, 0, 0, 0);
        Require(damage != nullptr && GetUpdateRgn(currentWindow.value, damage, FALSE) != ERROR,
            "R09 reads actual candidate HWND update region");
        const bool hasDamage = GetUpdateRect(currentWindow.value, nullptr, FALSE) != FALSE;
        Require(hasDamage == expectedAccepted, "R09 accepted result presents; stale or rejected result submits no damage");
        Require(!PtInRegion(damage, 10, 10), "R09 excludes unrelated desktop background from real update region");
        Require(Access::FolderIconSceneRevision(current) == oldRevision + (expectedAccepted ? 1 : 0),
            "R09 preserves exactly one drag-static invalidation per accepted result");
        Require(Access::FolderIconGeometry(current) == geometry, "R09 leaves real Dock slot geometry and hit ownership unchanged");
        Require(!Access::FolderCollectionPending(current) &&
            Access::FolderCollectionPending(reference) == expectedAccepted,
            "R09 bitmap-only completion leaves full collection pending only on the original path");
        if (expectedAccepted)
        {
            ++accepted;
            Require(GetUpdateRect(referenceWindow.value, nullptr, FALSE) != FALSE,
                "R09 executes old callback's broad invalidation as negative control");
            HRGN broad = CreateRectRgn(0, 0, 0, 0);
            Require(broad != nullptr && GetUpdateRgn(referenceWindow.value, broad, FALSE) != ERROR && PtInRegion(broad, 10, 10),
                "R09 original callback demonstrably includes the unrelated corner");
            DeleteObject(broad);
            Require(!Access::HasFolderAppearance(current, 0) && Access::HasFolderAppearance(current, 1) &&
                !Access::HasFolderAppearance(reference, 1),
                "R09 retires target appearance while old callback also discards the unrelated appearance");
        }
        else ++rejected;
        // Exercise the cold derived-image path inside the partial draw first;
        // a preceding full candidate frame must not warm away a missed update.
        const auto partial = Access::DrawFolderIconScene(current, context, damage, &currentPixels);
        const auto complete = Access::DrawFolderIconScene(reference, context);
        const auto next = Access::DrawFolderIconScene(current, context);
        Require(next == complete, "R09 complete production rendering matches frozen original callback");
        Require((next != currentPixels) == expectedPixelChange,
            "R09 local/refined/fallback transitions obey independently declared visible changes");
        if (partial != complete)
        {
            size_t differences = 0, outside = 0, first = partial.size();
            RECT bounds{800, 600, 0, 0}, dirty{};
            GetRgnBox(damage, &dirty);
            for (size_t index = 0; index != partial.size(); ++index)
            {
                if (partial[index] == complete[index]) continue;
                const auto x = static_cast<LONG>(index % 800), y = static_cast<LONG>(index / 800);
                bounds.left = std::min(bounds.left, x); bounds.top = std::min(bounds.top, y);
                bounds.right = std::max(bounds.right, x + 1); bounds.bottom = std::max(bounds.bottom, y + 1);
                if (!PtInRegion(damage, x, y)) ++outside;
                if (first == partial.size()) first = index;
                ++differences;
            }
            std::cerr << "R09 partial mismatch: pixels=" << differences << ", outsideDamage=" << outside
                << ", bounds=" << bounds.left << ',' << bounds.top << ',' << bounds.right << ',' << bounds.bottom
                << ", damageBox=" << dirty.left << ',' << dirty.top << ',' << dirty.right << ',' << dirty.bottom
                << ", first=" << first % 800 << ',' << first / 800 << ", partial/reference="
                << std::hex << partial[first] << '/' << complete[first] << std::dec << '\n';
        }
        Require(partial == complete, "R09 updating only actual damage yields every reference pixel with no stale image");
        size_t changedPixels = 0;
        for (size_t index = 0; index != next.size(); ++index)
            if (next[index] != currentPixels[index]) ++changedPixels;
        std::cerr << "R09 transition=" << transition << ", changedPixels=" << changedPixels
            << ", comparedPixels=" << complete.size() << ", exactPartialReference=1\n";
        DeleteObject(damage); currentPixels = next; referencePixels = complete;
        const auto actualState = Access::FolderIconRead(current);
        const auto expectedState = Access::FolderIconRead(reference);
        Require(actualState.fresh == expectedState.fresh && actualState.sameSourceVersion == expectedState.sameSourceVersion &&
            actualState.value.has_value() == expectedState.value.has_value(), "R09 publication/failure cache state equals reference");
    };
    const auto apply = [&](HBITMAP source, bool isRefined, bool acceptedResult, bool changed) {
        deliver(source, isRefined, acceptedResult, changed, currentRead.ticket, referenceRead.ticket,
            Access::FolderIconKey(current), Access::FolderIconKey(reference));
    };
    apply(nullptr, false, false, false); // Local miss waits for Shell.
    apply(first, false, true, true);
    apply(refined, true, true, true);
    apply(first, false, false, false); // Late local cannot downgrade refined pixels.
    Access::InvalidateFolderIconProbe(current); Access::InvalidateFolderIconProbe(reference);
    apply(first, true, false, false); // Retired generation, before another Read.
    currentRead = Access::FolderIconRead(current); referenceRead = Access::FolderIconRead(reference);
    apply(first, false, false, false); // Refresh retains final pixels.
    apply(nullptr, true, true, false); // Refinement failure keeps valid pixels and sets retry.
    const auto failedTicket = currentRead.ticket;
    currentRead = Access::FolderIconRead(current, true); referenceRead = Access::FolderIconRead(reference, true);
    Require(!currentRead.fresh && currentRead.ticket != failedTicket && currentRead.value,
        "R09 five-second retry advances ticket while retaining visible pixels");
    apply(first, false, false, false);
    apply(first, true, true, true);
    const auto retiredCurrentKey = Access::FolderIconKey(current);
    const auto retiredReferenceKey = Access::FolderIconKey(reference);
    const auto retiredCurrentTicket = currentRead.ticket;
    const auto retiredReferenceTicket = referenceRead.ticket;

    // New path starts independently. A failure with no pixels changes pending
    // placeholder to the production privacy-folder fallback, just as before.
    Access::ChangeFolderIconPath(current); Access::ChangeFolderIconPath(reference);
    currentRead = Access::FolderIconRead(current); referenceRead = Access::FolderIconRead(reference);
    currentPixels = Access::DrawFolderIconScene(current, context);
    referencePixels = Access::DrawFolderIconScene(reference, context);
    Require(currentPixels == referencePixels, "R09 path replacement starts with identical pending source");
    apply(nullptr, true, true, true);
    const auto emptyFailureTicket = currentRead.ticket;
    apply(refined, false, true, true);
    currentRead = Access::FolderIconRead(current, true); referenceRead = Access::FolderIconRead(reference, true);
    Require(!currentRead.fresh && currentRead.ticket != emptyFailureTicket && currentRead.value,
        "R09 local pixels delivered after failure preserve the original refinement retry deadline");
    apply(refined, false, false, false);
    apply(first, true, true, true);

    ValidateRect(currentWindow.value, nullptr); ValidateRect(referenceWindow.value, nullptr);
    const auto beforeRetiredSource = Access::FolderIconSceneRevision(current);
    Access::DeliverFolderIcon(current, retiredCurrentKey, retiredCurrentTicket, refined, true, false);
    Access::DeliverFolderIcon(reference, retiredReferenceKey, retiredReferenceTicket, refined, true, true);
    Require(!GetUpdateRect(currentWindow.value, nullptr, FALSE) &&
        Access::FolderIconSceneRevision(current) == beforeRetiredSource + 1,
        "R09 accepted retired-path cache result retains drag revision but has no current visual consumer");
    Require(Access::DrawFolderIconScene(current, context) == Access::DrawFolderIconScene(reference, context),
        "R09 retired-path delivery cannot alter the new source's visible pixels");
    ++accepted;

    Access::UnpinFolderIconProbe(current); Access::UnpinFolderIconProbe(reference);
    currentPixels = Access::DrawFolderIconScene(current, context);
    referencePixels = Access::DrawFolderIconScene(reference, context);
    Require(currentPixels == referencePixels, "R09 unpinned desktop-only consumer remains identical");
    apply(refined, true, true, true);
    RECT desktopDamage{};
    Require(GetUpdateRect(currentWindow.value, &desktopDamage, FALSE) && desktopDamage.bottom < 440,
        "R09 desktop-only consumer does not request absent Dock mirrors");
    Access::SetFolderIconMode(current, false); Access::SetFolderIconMode(reference, false);
    ValidateRect(currentWindow.value, nullptr); ValidateRect(referenceWindow.value, nullptr);
    const auto expandedRevision = Access::FolderIconSceneRevision(current);
    Access::DeliverFolderIcon(current, Access::FolderIconKey(current), currentRead.ticket, refined, true, false);
    Access::DeliverFolderIcon(reference, Access::FolderIconKey(reference), referenceRead.ticket, refined, true, true);
    Require(!GetUpdateRect(currentWindow.value, nullptr, FALSE) &&
        Access::FolderIconSceneRevision(current) == expandedRevision + 1,
        "R09 expanded unpinned mappings do not consume folder-entry icons from the Dock cache");
    Require(Access::DrawFolderIconScene(current, context) == Access::DrawFolderIconScene(reference, context),
        "R09 expanded mapping pixels remain unchanged after an unconsumed result");
    ++accepted;

    Window hostA, hostB, oldHostA, oldHostB;
    auto& floating = Access::FolderIconScene(context, currentWindow.value);
    auto& oldFloating = Access::FolderIconScene(context, referenceWindow.value);
    Access::HostFolderIconDocks(floating, hostA.value, hostB.value);
    Access::HostFolderIconDocks(oldFloating, oldHostA.value, oldHostB.value);
    const auto hostTicket = Access::FolderIconRead(floating).ticket;
    const auto oldHostTicket = Access::FolderIconRead(oldFloating).ticket;
    ValidateRect(currentWindow.value, nullptr); ValidateRect(referenceWindow.value, nullptr);
    Access::DeliverFolderIcon(floating, Access::FolderIconKey(floating), hostTicket, first, false, false);
    Access::DeliverFolderIcon(oldFloating, Access::FolderIconKey(oldFloating), oldHostTicket, first, false, true);
    Require(hostA.paints == 1 && hostB.paints == 1 && oldHostA.paints == 1 && oldHostB.paints == 1,
        "R09 preserves one synchronous UpdateWindow delivery to each persistent mirror");
    Require(!GetUpdateRect(currentWindow.value, nullptr, FALSE) && GetUpdateRect(referenceWindow.value, nullptr, FALSE),
        "R09 floating-only completion removes the old unrelated root update");
    Require(Access::DrawFolderIconScene(floating, context) == Access::DrawFolderIconScene(oldFloating, context),
        "R09 persistent-host entry content matches the full reference pixels");
    Access::StopFolderIconScene(floating); Access::StopFolderIconScene(oldFloating);

    Access::StopFolderIconScene(current); Access::StopFolderIconScene(reference);
    DeleteObject(first); DeleteObject(refined);
    std::cerr << "R09 production completion: accepted=" << accepted << ", rejected=" << rejected
        << "; two Dock mirrors + desktop pixels/hits/state/dirty-region reference comparison passed\n";
}

inline void RunRootCulling(ID2D1DeviceContext* context)
{
    std::cerr << "R09 root culling: private receivers and real renderer\n";
    Window currentWindow, referenceWindow;
    auto& current = Access::FolderIconScene(context, currentWindow.value);
    auto& reference = Access::FolderIconScene(context, referenceWindow.value);
    Access::AddUnrelatedRootIcon(current); Access::AddUnrelatedRootIcon(reference);
    const auto before = Access::DrawFolderIconScene(current, context, nullptr, nullptr, true);
    Require(before == Access::DrawFolderIconScene(reference, context, nullptr, nullptr, true),
        "R09 real root renderer starts from identical sentinel pixels");
    Access::ClearRootProbeResources(current); Access::ClearRootProbeResources(reference);
    Access::SetFolderCollection(current); Access::SetFolderCollection(reference);
    ValidateRect(currentWindow.value, nullptr); ValidateRect(referenceWindow.value, nullptr);
    snowdesktop::icon_image::Image image;
    image.width = image.height = 32; image.pixels.assign(32 * 32, 0xff80b040);
    const auto source = snowdesktop::icon_image::CreateBitmap(image);
    Require(source != nullptr, "R09 creates owned completion source for root routing");
    const auto ticket = Access::FolderIconRead(current).ticket;
    const auto oldTicket = Access::FolderIconRead(reference).ticket;
    Access::DeliverFolderIcon(current, Access::FolderIconKey(current), ticket, source, true, false);
    Access::DeliverFolderIcon(reference, Access::FolderIconKey(reference), oldTicket, source, true, true);
    HRGN damage = CreateRectRgn(0, 0, 0, 0);
    Require(damage && GetUpdateRgn(currentWindow.value, damage, FALSE) != ERROR &&
        GetUpdateRect(currentWindow.value, nullptr, FALSE), "R09 root probe uses actual completion damage");
    Require(!PtInRegion(damage, 50, 50) && !Access::FolderCollectionPending(current) &&
        Access::FolderCollectionPending(reference), "R09 keeps unrelated root icon outside bitmap-only collection");
    const auto partial = Access::DrawFolderIconScene(current, context, damage, &before, true);
    const auto complete = Access::DrawFolderIconScene(reference, context, nullptr, nullptr, true);
    Require(partial == complete, "R09 real root partial update preserves every complete-reference pixel");
    const auto currentUploads = Access::RootProbeResources(current);
    const auto referenceUploads = Access::RootProbeResources(reference);
    const auto requireCulled = [](size_t resources) {
        Require(resources == 0, "R09 unrelated real DesktopIcon must be culled before bitmap upload");
    };
    requireCulled(currentUploads);
    bool oldRejected = false;
    try { requireCulled(referenceUploads); }
    catch (const std::runtime_error& error)
    {
        if (std::string_view(error.what()) != "R09 unrelated real DesktopIcon must be culled before bitmap upload") throw;
        oldRejected = true;
    }
    Require(oldRejected && referenceUploads > 0, "R09 original callback fails the same real draw-resource assertion");
    std::cerr << "R09 actual root routing: unrelated styled uploads candidate=" << currentUploads
        << ", original=" << referenceUploads << ", comparedPixels=" << complete.size() << "; negative control rejected\n";
    // Existing first-frame/layout/device work remains pending. Native glass
    // still uses complete owner collection for a bitmap-only callback.
    Access::SetFolderCollection(current, true);
    Access::DeliverFolderIcon(current, Access::FolderIconKey(current), ticket, source, true, false);
    Require(Access::FolderCollectionPending(current), "R09 never clears pre-existing complete-collection work");
    Access::SetFolderCollection(current);
    Access::SetFolderGlass(current, true);
    Access::DeliverFolderIcon(current, Access::FolderIconKey(current), ticket, source, true, false);
    Require(Access::FolderCollectionPending(current), "R09 preserves complete recollection for native glass");
    DeleteObject(damage); DeleteObject(source);
    Access::StopFolderIconScene(current); Access::StopFolderIconScene(reference);
}

inline void RunOffscreen()
{
    std::cerr << "R09 offscreen: initialize COM and WARP\n";
    Require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "R09 initializes isolated COM scope");
    struct ComScope { ~ComScope() { CoUninitialize(); } } comScope;
    ComPtr<ID3D11Device> d3d;
    Require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr)),
        "R09 creates offscreen WARP device without a desktop host");
    ComPtr<IDXGIDevice> dxgi;
    Require(SUCCEEDED(d3d.As(&dxgi)), "R09 obtains WARP DXGI device");
    ComPtr<ID2D1Factory1> factory;
    Require(SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&factory))),
        "R09 creates isolated D2D factory");
    ComPtr<ID2D1Device> device;
    Require(SUCCEEDED(factory->CreateDevice(dxgi.Get(), &device)), "R09 creates isolated D2D device");
    ComPtr<ID2D1DeviceContext> context;
    Require(SUCCEEDED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context)),
        "R09 creates isolated D2D context");
    Run(context.Get());
    RunRootCulling(context.Get());
}

inline void RunStandalone()
{
    constexpr wchar_t childVariable[] = L"SNOWDESKTOP_TEST_R09_PRIVATE_DESKTOP";
    wchar_t childDesktop[256]{};
    const DWORD markerLength = GetEnvironmentVariableW(childVariable, childDesktop,
        static_cast<DWORD>(std::size(childDesktop)));
    if (markerLength)
    {
        wchar_t actual[256]{};
        DWORD needed = 0;
        Require(markerLength < std::size(childDesktop) &&
            std::wstring_view(childDesktop).starts_with(L"SnowDesktopR09-") &&
            GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME,
                actual, sizeof(actual), &needed) && std::wstring_view(actual) == childDesktop,
            "R09 child starts on its owned inactive desktop");
        std::cerr << "R09 child: complete process-lifetime fixture on inactive desktop\n";
        RunOffscreen();
        std::cerr << "R09 child: all completion and root cases complete\n";
        return;
    }

    // The native model deliberately has process-lifetime owners (as do the
    // other native fixtures). Their WinUI/TSF/GPU workers can retain a desktop
    // after visible HWNDs exit. Give the whole owned process one initial
    // private desktop, then close our handle only after that process exits.
    struct Owned
    {
        HDESK desktop = nullptr;
        HANDLE job = nullptr, process = nullptr, thread = nullptr;
        bool assigned = false;
        ~Owned()
        {
            if (process && !assigned && WaitForSingleObject(process, 0) == WAIT_TIMEOUT)
                TerminateProcess(process, 1); // Own suspended child if assignment failed.
            if (job) CloseHandle(job); // Kill only our child on an error.
            if (process) { WaitForSingleObject(process, 5000); CloseHandle(process); }
            if (thread) CloseHandle(thread);
            if (desktop) CloseDesktop(desktop);
        }
    } owned;
    const auto name = L"SnowDesktopR09-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64());
    owned.desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    Require(owned.desktop != nullptr, "R09 creates a never-activated process fixture desktop");
    owned.job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    Require(owned.job && SetInformationJobObject(owned.job, JobObjectExtendedLimitInformation,
        &limits, sizeof(limits)), "R09 bounds its own fixture process lifetime");

    const std::wstring prefix = std::wstring(childVariable) + L"=";
    std::vector<std::wstring> entries;
    LPWCH original = GetEnvironmentStringsW();
    Require(original != nullptr, "R09 reads child environment without mutating the parent");
    for (const wchar_t* entry = original; *entry; entry += std::wcslen(entry) + 1)
    {
        const std::wstring value(entry);
        if (value.size() < prefix.size() || CompareStringOrdinal(value.c_str(),
                static_cast<int>(prefix.size()), prefix.c_str(), static_cast<int>(prefix.size()), TRUE) != CSTR_EQUAL)
            entries.push_back(value);
    }
    FreeEnvironmentStringsW(original);
    entries.push_back(prefix + name);
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    std::wstring environment;
    for (const auto& value : entries) { environment += value; environment.push_back(L'\0'); }
    environment.push_back(L'\0');
    wchar_t executable[32768]{};
    Require(GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable))) != 0,
        "R09 resolves the current compiled test program");
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --dock-folder-icon-completion";
    STARTUPINFOW startup{sizeof(startup)};
    startup.lpDesktop = const_cast<wchar_t*>(name.c_str());
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION child{};
    Require(CreateProcessW(executable, command.data(), nullptr, nullptr, TRUE,
        CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | CREATE_SUSPENDED,
        environment.data(), nullptr, &startup, &child), "R09 starts its owned private-desktop fixture process");
    owned.process = child.hProcess; owned.thread = child.hThread;
    Require(AssignProcessToJobObject(owned.job, owned.process) != FALSE,
        "R09 contains the suspended fixture before executing it");
    owned.assigned = true;
    Require(ResumeThread(owned.thread) != static_cast<DWORD>(-1), "R09 resumes its owned fixture");
    Require(WaitForSingleObject(owned.process, 75000) == WAIT_OBJECT_0,
        "R09 private fixture completes within the unchanged outer test budget");
    DWORD exitCode = 1;
    Require(GetExitCodeProcess(owned.process, &exitCode) && exitCode == 0,
        "R09 private fixture passes every completion, pixel and root-resource assertion");
    const bool closed = CloseDesktop(owned.desktop) != FALSE;
    if (!closed) std::cerr << "R09 parent desktop close error=" << GetLastError() << '\n';
    if (closed) owned.desktop = nullptr;
    Require(closed, "R09 closes the owned desktop after its fixture process exits");
    std::cerr << "R09 standalone: complete, child exited and owned desktop closed\n";
}
}
