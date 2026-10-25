#include "system/status_bar/status_bar_view.h"
#include "system/status_bar/status_bar_glyphs.h"
#include "system/status_bar/status_bar_interaction.h"
#include "system/status_bar/status_bar_fullscreen.h"
#include "system/status_bar/status_bar_reveal.h"
#include "common/l10n.h"
#include "ui/render/app_font.h"
#include "tray_geometry_cases.h"
#include <d3d11.h>
#include <dwrite.h>
#include <dcomp.h>
#include <wrl/client.h>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <cstring>
#include <array>
#include <initializer_list>
#include <string_view>
#include <functional>

// Replace only the embedded resource loader with its original bundled TTF.
// Production glyph lookup, outlines, drawing, layout and hit testing stay real.
Microsoft::WRL::ComPtr<IDWriteFontCollection1> fluentTestCollection;
unsigned fluentFormatCreations = 0;
Microsoft::WRL::ComPtr<IDWriteFontCollection1> fluentFallbackCollection;
bool fluentFallbackForControl = false;
IDWriteTextFormat* CreateFluentTextFormat(IDWriteFactory* factory, float size)
{
    ++fluentFormatCreations;
    IDWriteTextFormat* format = nullptr;
    if (FAILED(factory->CreateTextFormat(L"FluentSystemIcons-Regular",
        fluentFallbackForControl ? fluentFallbackCollection.Get() : fluentTestCollection.Get(), DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &format))) return nullptr;
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    return format;
}
namespace
{
using namespace snowdesktop;
using Microsoft::WRL::ComPtr;
void Check(bool passed, const char* message)
{
    if (!passed) { std::cerr << message << '\n'; std::exit(1); }
}
void CheckIndependentReveal()
{
    const RECT monitor{-1920, -1080, 0, 0}, bar{-1920, -1080, 0, -1048};
    const POINT edge{-1000, -1080}, inside{-1000, -1060}, outside{-1000, -900};
    StatusBarRevealState state, otherMonitor;
    Check(!state.Update(edge, monitor, bar, DockPosition::Top, 100, 96, 1, false) && !state.Revealed(),
        "hover does not reveal an independent bar on initial edge contact");
    Check(!state.Update(edge, monitor, bar, DockPosition::Top, 349, 96, 1, false),
        "hover waits for Dock's default dwell rather than revealing early");
    Check(state.Update(edge, monitor, bar, DockPosition::Top, 350, 96, 1, false) && state.Revealed() && !otherMonitor.Revealed(),
        "default hover reveals only the requested monitor, including negative screen coordinates");
    state.Update(inside, monitor, bar, DockPosition::Top, 500, 96, 1, false);
    state.Update(outside, monitor, bar, DockPosition::Top, 600, 96, 1, false);
    Check(!state.Update(outside, monitor, bar, DockPosition::Top, 959, 96, 1, false) && state.Revealed(),
        "pointer leave keeps a short grace period for crossing into an owned surface");
    state.Update(outside, monitor, bar, DockPosition::Top, 960, 96, 1, true);
    state.Update(outside, monitor, bar, DockPosition::Top, 2000, 96, 1, true);
    Check(state.Revealed(), "an owned menu or system panel retains the revealed bar beyond the leave deadline");
    state.Update(outside, monitor, bar, DockPosition::Top, 2100, 96, 1, false);
    Check(state.Update(outside, monitor, bar, DockPosition::Top, 2460, 96, 1, false) && !state.Revealed(),
        "ending the interaction and leaving the bar dismisses after a new grace period");
    state.Update(outside, monitor, bar, DockPosition::Top, 2500, 96, 1, false);
    state.Update(edge, monitor, bar, DockPosition::Top, 2600, 96, 1, false);
    state.Update(edge, monitor, bar, DockPosition::Top, 2850, 96, 1, false);
    state.Dismiss();
    Check(!state.Update(edge, monitor, bar, DockPosition::Top, 4000, 96, 1, false) && !state.Revealed(),
        "explicit dismissal suppresses repeated hover until the pointer leaves the edge");
    state.Reset();
    const RECT bottom{0, 0, 2560, 1440}, bottomBar{0, 1376, 2560, 1440};
    state.Update({500, 1439}, bottom, bottomBar, DockPosition::Bottom, 5000, 192, 0, false);
    Check(!state.Update({643, 1439}, bottom, bottomBar, DockPosition::Bottom, 5100, 192, 0, false),
        "swipe travel is DPI scaled, and a short stroke does not trigger");
    Check(state.Update({644, 1439}, bottom, bottomBar, DockPosition::Bottom, 5150, 192, 0, false) && state.Revealed(),
        "a bottom-edge swipe at the scaled threshold reveals the bar");
    state.Reset();
    state.Update(edge, monitor, bar, DockPosition::Top, 6000, 96, 0, false);
    Check(!state.Update(edge, monitor, bar, DockPosition::Top, 6500, 96, 0, false) && !state.Revealed(),
        "stationary hover cannot trigger when swipe is selected");
    Check(HideIndependentStatusBar(false, true, false, false) &&
        !HideIndependentStatusBar(false, true, true, false) &&
        !HideIndependentStatusBar(false, true, false, true) &&
        !HideIndependentStatusBar(false, false, false, false) &&
        HideIndependentStatusBar(true, false, true, false),
        "summon-only and interaction gates preserve fullscreen hiding and ordinary persistent bars");
    for (const bool summonOnly : {false, true})
    {
        Check(ReserveIndependentStatusBarSpace(false, true, summonOnly) == !summonOnly,
            "independent summon-only bars release the desktop work area");
        for (const bool dockReservation : {false, true})
            Check(ReserveIndependentStatusBarSpace(true, dockReservation, summonOnly) == dockReservation,
                "merged reservations follow Dock regardless of independent status-bar preferences");
    }
}
void CheckFullscreenRounds()
{
    // Replace only native sampling/posting. These are the production Z-order
    // fold and request lifecycle, without touching the user's windows/AppBars.
    const auto hwnd = [](UINT_PTR id) { return reinterpret_cast<HWND>(id); };
    const RECT left{-1920, 0, 0, 1080}, right{0, 0, 2560, 1440};
    const RECT both{-1920, 0, 2560, 1440};
    const std::array<std::optional<RECT>, 3> monitors{left, right, std::nullopt};
    const auto window = [&](UINT_PTR id, RECT client, LONG_PTR style = WS_POPUP,
        LONG_PTR extended = 0, bool maximized = false, bool cloaked = false, bool shell = false) {
        return StatusBarFullscreenWindow{hwnd(id), client, style, extended, maximized, cloaked, shell};
    };
    unsigned enumerations = 0, samples = 0;
    const auto scan = [&](const auto& targets, std::initializer_list<StatusBarFullscreenWindow> windows,
        bool success = true) {
        return ObserveStatusBarFullscreen(targets, [&](const auto& visit) {
            ++enumerations;
            for (const auto& sample : windows)
            {
                ++samples;
                if (!visit(sample)) return false; // EnumWindows callback stop.
            }
            return success;
        });
    };
    auto observed = scan(monitors, {
        window(1, {-1100, 450, -800, 650}, WS_POPUP, WS_EX_TOOLWINDOW),
        window(2, right), window(3, left), window(4, both)});
    Check(enumerations == 1 && samples == 3 && observed[0] == hwnd(3) && observed[1] == hwnd(2) && !observed[2],
        "one round samples each Z-order window once and stops only after every valid screen settles");

    observed = scan(monitors, {window(5, {-1500, 100, -200, 1000}, WS_OVERLAPPEDWINDOW), window(6, both)});
    Check(observed[0] == HWND{} && observed[1] == hwnd(6),
        "Alt+Tab center occlusion settles only its monitor, preserving another screen's fullscreen source");
    observed = scan(monitors, {window(7, both, WS_POPUP, 0, false, true),
        window(8, both, WS_POPUP, 0, false, false, true),
        window(9, {-1100, 450, -800, 650}, WS_POPUP, WS_EX_NOACTIVATE),
        window(10, {1000, 600, 1400, 850}, WS_POPUP, WS_EX_TRANSPARENT), window(11, both)});
    Check(observed[0] == hwnd(11) && observed[1] == hwnd(11),
        "cloaked/Shell windows and passive overlays do not hide a spanning fullscreen source");
    observed = scan(monitors, {window(12, left, WS_OVERLAPPEDWINDOW | WS_MAXIMIZE, 0, true), window(13, both)});
    Check(observed[0] == HWND{} && observed[1] == hwnd(13),
        "ordinary maximization remains a center blocker rather than a fullscreen source");
    observed = scan(monitors, {window(14, left, WS_POPUP | WS_MAXIMIZE, 0, true), window(15, right)});
    Check(observed[0] == hwnd(14) && observed[1] == hwnd(15),
        "browser fullscreen retaining maximized style remains independently detected");
    observed = scan(monitors, {window(16, right)}, false);
    Check(!observed[0] && observed[1] == hwnd(16) && !observed[2],
        "partial enumeration failure preserves a settled monitor and leaves unresolved/removed monitors unknown");
    observed = scan(monitors, {});
    Check(observed[0] == HWND{} && observed[1] == HWND{} && !observed[2],
        "successful empty observation clears fullscreen; failed monitor lookup remains unknown");
    const std::array<std::optional<RECT>, 1> changedMonitor{RECT{0, 0, 3840, 2160}};
    observed = scan(changedMonitor, {window(17, {0, 32, 3840, 2160})});
    Check(observed[0] == HWND{}, "work-area coverage is not full monitor coverage after a DPI/topology change");
    observed = scan(changedMonitor, {window(18, *changedMonitor[0])});
    Check(observed[0] == hwnd(18), "a new round uses current monitor geometry without a cross-round result cache");
    const std::array<std::optional<RECT>, 1> invalidMonitor{std::nullopt};
    const auto before = enumerations;
    observed = scan(invalidMonitor, {window(19, both)});
    Check(enumerations == before && !observed[0], "removing the last valid monitor avoids unnecessary enumeration");

    StatusBarFullscreenRequest request;
    std::vector<HWND> posted;
    const auto post = [&](HWND receiver) { posted.push_back(receiver); return true; };
    Check(!request.Queue(hwnd(20), [](HWND) { return false; }) && request.Queue(hwnd(20), post),
        "failed event posting remains retryable");
    Check(!request.Queue(hwnd(21), post) && posted.size() == 1 && !request.Consume(hwnd(21)),
        "all monitors coalesce into one request; another HWND cannot consume it");
    Check(request.Consume(hwnd(20)) && request.Queue(hwnd(21), post) && posted.size() == 2,
        "destroying the recipient permits transfer to a surviving bar");
    Check(!request.Consume(hwnd(20)) && request.Consume(hwnd(21)) && request.Queue(hwnd(21), post),
        "an obsolete recipient cannot clear its replacement request, and later events trigger a fresh round");
    Check(request.Consume(hwnd(21)) && !request.Queue(nullptr, post),
        "closing the last receiver leaves no pointer or queued request to a destroyed bar");
    std::cout << "Shared fullscreen Z-order rounds and request lifecycle passed\n";
}
const StatusBarItem* Find(const std::vector<StatusBarItem>& items, std::string_view key)
{
    const auto found = std::find_if(items.begin(), items.end(), [&](const auto& item) { return item.key == key; });
    return found == items.end() ? nullptr : &*found;
}

// Replace only DComp's atlas allocation/copy boundary. Production damage,
// update-offset handling, clipping, retained layout and D2D drawing stay real.
class AtlasSurface final : public IDCompositionSurface
{
public:
    AtlasSurface(ID2D1DeviceContext* context, ID2D1Bitmap1* retained, bool aligned = false)
        : aligned_(aligned), context_(context), retained_(retained)
    {
        const auto size = retained->GetPixelSize();
        Check(SUCCEEDED(context_->CreateBitmap(D2D1::SizeU(size.width + 32, size.height + 24), nullptr, 0,
            D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &atlas_)),
            "hover atlas initializes");
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override
    {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(IDCompositionSurface)) return E_NOINTERFACE;
        *value = static_cast<IDCompositionSurface*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto count = --references_; if (!count) delete this; return count; }
    HRESULT STDMETHODCALLTYPE BeginDraw(const RECT* region, REFIID iid, void** value, POINT* offset) override
    {
        if (!value || !offset || drawing_) return E_INVALIDARG;
        const auto size = retained_->GetPixelSize();
        const RECT full{0, 0, static_cast<LONG>(size.width), static_cast<LONG>(size.height)};
        const bool complete = !region;
        if (!region) region = &full;
        if (region->left < 0 || region->top < 0 || region->right > static_cast<LONG>(size.width) ||
            region->bottom > static_cast<LONG>(size.height) || IsRectEmpty(region)) return E_INVALIDARG;
        region_ = *region;
        // The complete-reference pass uses one physical raster origin. The
        // arbitrary-offset pass retains the original fixed atlas placement.
        origin_ = aligned_ ? POINT{13 + region->left, 9 + region->top} : POINT{13, 9};
        *offset = origin_;
        const auto result = context_->QueryInterface(iid, value);
        if (FAILED(result)) return result;
        updates.push_back(*region); if (complete) ++completeUpdates; drawing_ = true;
        context_->SetTarget(atlas_.Get()); context_->SetTransform(D2D1::Matrix3x2F::Identity());
        context_->BeginDraw(); context_->Clear(D2D1::ColorF(0xff00ff));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EndDraw() override
    {
        if (!drawing_) return E_UNEXPECTED;
        drawing_ = false;
        const auto result = context_->EndDraw(); context_->SetTarget(nullptr);
        if (FAILED(result)) return result;
        const D2D1_POINT_2U destination{static_cast<UINT32>(region_.left), static_cast<UINT32>(region_.top)};
        const D2D1_RECT_U source{static_cast<UINT32>(origin_.x), static_cast<UINT32>(origin_.y),
            static_cast<UINT32>(origin_.x + region_.right - region_.left),
            static_cast<UINT32>(origin_.y + region_.bottom - region_.top)};
        return retained_->CopyFromBitmap(&destination, atlas_.Get(), &source);
    }
    HRESULT STDMETHODCALLTYPE SuspendDraw() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ResumeDraw() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Scroll(const RECT*, const RECT*, int, int) override { return E_NOTIMPL; }
    std::vector<RECT> updates;
    std::size_t completeUpdates = 0;
private:
    ULONG references_ = 1;
    bool drawing_ = false;
    RECT region_{};
    bool aligned_ = false;
    POINT origin_{13, 9}; // Both placements exercise nonzero atlas offsets.
    ComPtr<ID2D1DeviceContext> context_;
    ComPtr<ID2D1Bitmap1> retained_, atlas_;
};

void CheckHoverUpdates(ID2D1Device* device, ID2D1DeviceContext* context, IDWriteFactory* text,
    const StatusBarSnapshot& data, const std::vector<std::string>& pinned,
    bool alignedPhysicalOrigin, bool requireCompleteReference)
{
    std::uint64_t updatedPixels = 0, fullPixels = 0;
    for (const bool merged : {false, true}) for (const bool highContrast : {false, true})
    for (const bool lightTheme : {false, true})
    for (const float scale : {1.f, 1.5f, 2.f}) for (const UINT dipWidth : {640u, 1920u})
    {
        const UINT width = static_cast<UINT>(static_cast<float>(dipWidth) * scale),
            height = static_cast<UINT>((merged ? 64.f : 32.f) * scale);
        const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        ComPtr<ID2D1Bitmap1> retained, reference, legacy, readable, fullAtlas;
        Check(SUCCEEDED(context->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0, properties, &retained)) &&
            SUCCEEDED(context->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0, properties, &reference)) &&
            SUCCEEDED(context->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0, properties, &legacy)) &&
            SUCCEEDED(context->CreateBitmap(D2D1::SizeU(width + 32, height + 24), nullptr, 0, properties, &fullAtlas)) &&
            SUCCEEDED(context->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0,
                D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                    properties.pixelFormat), &readable)), "hover comparison bitmaps initialize");
        StatusBarSettings settings; settings.pinnedTrayItems = pinned; settings.batteryPercentage = true;
        auto items = BuildStatusBarItems(settings, data);
        PersonalizationSettings appearance; appearance.contentTheme = lightTheme ? 1 : 0;
        const StatusBarPalette palette{highContrast, D2D1::ColorF(0xffffff), D2D1::ColorF(0x000000),
            D2D1::ColorF(0x003399), D2D1::ColorF(0xffffff)};
        StatusBarTrayScroll scrolling;
        StatusBarDrawResources resources;
        // Outline antialiasing depends on physical device coordinates. Match
        // those coordinates for the complete-reference comparison, retaining
        // an independent fresh full layout/draw of every item. A separate pass
        // checks exact cached/original behavior at arbitrary atlas offsets.
        const POINT fullOrigin = alignedPhysicalOrigin ? POINT{13, 9} : POINT{0, 0};
        const auto fullPaint = [&](ID2D1Bitmap1* target, std::vector<StatusBarItem>& content,
            std::optional<std::size_t> hover, StatusBarTrayScroll& scroll, StatusBarDrawResources* cache = nullptr) {
            context->SetTarget(fullAtlas.Get()); context->SetDpi(96, 96);
            context->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(fullOrigin.x), static_cast<float>(fullOrigin.y)));
            context->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
            context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            context->BeginDraw(); context->Clear(D2D1::ColorF(0, 0.f));
            const auto result = DrawStatusBarContent(context, text, content, width, height, scale,
                appearance, palette, hover, true, 0, merged, true, &scroll, true, nullptr, cache);
            const auto ended = context->EndDraw(); context->SetTarget(nullptr);
            Check(SUCCEEDED(result) && SUCCEEDED(ended), "full reference frame renders");
            const D2D1_RECT_U client{static_cast<UINT32>(fullOrigin.x), static_cast<UINT32>(fullOrigin.y),
                static_cast<UINT32>(fullOrigin.x) + width, static_cast<UINT32>(fullOrigin.y) + height};
            Check(SUCCEEDED(target->CopyFromBitmap(nullptr, fullAtlas.Get(), &client)), "complete atlas frame copies to client pixels");
        };
        const auto pixels = [&](ID2D1Bitmap1* target) {
            Check(SUCCEEDED(readable->CopyFromBitmap(nullptr, target, nullptr)), "hover pixels copy to CPU");
            D2D1_MAPPED_RECT mapped{};
            Check(SUCCEEDED(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped)), "hover pixels map");
            std::vector<std::uint32_t> result(static_cast<std::size_t>(width) * height);
            for (UINT row = 0; row < height; ++row)
                std::memcpy(result.data() + static_cast<std::size_t>(row) * width,
                    mapped.bits + static_cast<std::size_t>(row) * mapped.pitch, static_cast<std::size_t>(width) * 4);
            readable->Unmap(); return result;
        };
        fullPaint(retained.Get(), items, {}, scrolling, &resources);
        auto legacyItems = BuildStatusBarItems(settings, data);
        StatusBarTrayScroll legacyScroll;
        fullPaint(legacy.Get(), legacyItems, {}, legacyScroll);
        // Retain a partially scrolled icon at the viewport edge as well as
        // complete controls; fading/clipping must match a full redraw exactly.
        scrolling.Move(scrolling.Maximum() / 2 + (scrolling.Maximum() ? 7 : 0));
        legacyScroll.Move(legacyScroll.Maximum() / 2 + (legacyScroll.Maximum() ? 7 : 0));
        fullPaint(retained.Get(), items, {}, scrolling, &resources);
        fullPaint(legacy.Get(), legacyItems, {}, legacyScroll);
        ComPtr<AtlasSurface> surface; surface.Attach(new AtlasSurface(context, retained.Get(), alignedPhysicalOrigin));
        ComPtr<AtlasSurface> legacySurface; legacySurface.Attach(new AtlasSurface(context, legacy.Get(), alignedPhysicalOrigin));
        std::vector<std::optional<std::size_t>> sequence;
        const auto add = [&](std::string_view key) {
            const auto* item = Find(items, key);
            if (item && !IsRectEmpty(&item->bounds)) sequence.push_back(static_cast<std::size_t>(item - items.data()));
        };
        add("menu"); add("clock"); add("controlCenter");
        for (std::size_t index = 0; index < items.size(); ++index)
            if (items[index].icon && !IsRectEmpty(&items[index].bounds)) { sequence.push_back(index); break; }
        for (std::size_t index = items.size(); index-- > 0;)
            if (items[index].icon && !IsRectEmpty(&items[index].bounds)) { sequence.push_back(index); break; }
        add("showDesktop"); sequence.push_back({}); add("menu"); add("menu"); sequence.push_back({});
        std::optional<std::size_t> previous;
        for (const auto hover : sequence)
        {
            const auto boundsBefore = items;
            const auto scrollBefore = scrolling;
            const auto updateStart = surface->updates.size();
            const auto damage = StatusBarHoverDamage(items, previous, hover, width, height);
            Check(SUCCEEDED(DrawStatusBarHoverUpdate(surface.Get(), text, items, damage, width, height, scale,
                appearance, palette, hover, true, 0, merged, scrolling, &resources)), "production hover update renders both damage regions");
            const auto legacyDamage = StatusBarHoverDamage(legacyItems, previous, hover, width, height);
            Check(SUCCEEDED(DrawStatusBarHoverUpdate(legacySurface.Get(), text, legacyItems, legacyDamage, width, height, scale,
                appearance, palette, hover, true, 0, merged, legacyScroll)), "original uncached hover route renders the same atlas offsets");
            for (std::size_t index = 0; index < items.size(); ++index)
                Check(EqualRect(&items[index].bounds, &boundsBefore[index].bounds) &&
                    EqualRect(&items[index].contentBounds, &boundsBefore[index].contentBounds), "hover retains arranged hit geometry");
            Check(EqualRect(&scrolling.viewport, &scrollBefore.viewport) && scrolling.offset == scrollBefore.offset,
                "hover retains tray scroll position and viewport");
            auto expected = BuildStatusBarItems(settings, data); auto expectedScroll = scrollBefore;
            fullPaint(reference.Get(), expected, hover, expectedScroll);
            const auto observed = pixels(retained.Get()), wanted = pixels(reference.Get());
            const auto uncached = pixels(legacy.Get());
            Check(observed == uncached, "cached hover pixels differ from the original uncached hover route at identical atlas offsets");
            bool equivalent = true;
            for (std::size_t pixel = 0; pixel < observed.size(); ++pixel)
            {
                if (observed[pixel] == wanted[pixel]) continue;
                const POINT location{static_cast<LONG>(pixel % width), static_cast<LONG>(pixel / width)};
                // Static repaint controls with shifted atlas offsets show
                // 1-2/255 variation only in cropped opacity-mask pixels.
                // Permit that bounded variation; all other pixels stay exact.
                if (highContrast || !scrollBefore.Maximum() || !PtInRect(&scrollBefore.viewport, location))
                    equivalent = false;
                for (unsigned shift = 0; shift < 32; shift += 8)
                    if (std::abs(static_cast<int>((observed[pixel] >> shift) & 255) -
                        static_cast<int>((wanted[pixel] >> shift) & 255)) > 2) equivalent = false;
            }
            if (requireCompleteReference && !equivalent)
            {
                std::size_t mismatchCount = 0;
                unsigned maximumDelta = 0;
                RECT mismatchBounds{static_cast<LONG>(width), static_cast<LONG>(height), 0, 0};
                for (std::size_t pixel = 0; pixel < observed.size(); ++pixel)
                {
                    if (observed[pixel] == wanted[pixel]) continue;
                    ++mismatchCount;
                    const auto x = static_cast<LONG>(pixel % width), y = static_cast<LONG>(pixel / width);
                    mismatchBounds.left = std::min(mismatchBounds.left, x); mismatchBounds.top = std::min(mismatchBounds.top, y);
                    mismatchBounds.right = std::max(mismatchBounds.right, x + 1); mismatchBounds.bottom = std::max(mismatchBounds.bottom, y + 1);
                    for (unsigned shift = 0; shift < 32; shift += 8)
                        maximumDelta = std::max(maximumDelta, static_cast<unsigned>(std::abs(
                            static_cast<int>((observed[pixel] >> shift) & 255) - static_cast<int>((wanted[pixel] >> shift) & 255))));
                }
                std::cerr << "hover mismatch merged=" << merged << " hc=" << highContrast << " light=" << lightTheme << " scale=" << scale
                    << " width=" << width << " previous=" << previous.value_or(items.size())
                    << " hover=" << hover.value_or(items.size()) << " uncached-same-offset-equal=" << (observed == uncached)
                    << " full-different-pixels=" << mismatchCount << " max-channel-delta=" << maximumDelta
                    << " bounds=" << mismatchBounds.left << ',' << mismatchBounds.top << ',' << mismatchBounds.right << ',' << mismatchBounds.bottom << '\n';
                Check(false, "incremental enter/switch/leave pixels differ from a complete reference frame");
            }
            for (std::size_t index = updateStart; index < surface->updates.size(); ++index)
            {
                const auto& region = surface->updates[index];
                updatedPixels += static_cast<std::uint64_t>(region.right - region.left) * (region.bottom - region.top);
                Check(region.right - region.left < static_cast<LONG>(width), "hover never updates the complete strip");
            }
            if (previous == hover) Check(surface->updates.size() == updateStart, "stationary hover submits no surface update");
            fullPixels += static_cast<std::uint64_t>(width) * height;
            previous = hover;
        }
    }
    // Exercise the same production update boundary on a real initialized DComp
    // surface too. No target HWND or input-desktop interaction is required.
    ComPtr<IDCompositionDesktopDevice> composition;
    ComPtr<IDCompositionSurface> native;
    Check(SUCCEEDED(DCompositionCreateDevice2(device, IID_PPV_ARGS(&composition))) &&
        SUCCEEDED(composition->CreateSurface(640, 64, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, &native)),
        "native incremental surface initializes");
    auto nativeItems = BuildStatusBarItems(StatusBarSettings{}, data); StatusBarTrayScroll nativeScroll;
    POINT origin{}; ComPtr<ID2D1DeviceContext> nativeContext;
    Check(SUCCEEDED(native->BeginDraw(nullptr, IID_PPV_ARGS(&nativeContext), &origin)), "native first frame covers the full surface");
    nativeContext->SetDpi(96, 96); nativeContext->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(origin.x), static_cast<float>(origin.y)));
    nativeContext->Clear(D2D1::ColorF(0, 0.f));
    const auto initial = DrawStatusBarContent(nativeContext.Get(), text, nativeItems, 640, 64, 1.f,
        PersonalizationSettings{}, StatusBarPalette{}, {}, false, 0, true, false, &nativeScroll);
    nativeContext.Reset();
    Check(SUCCEEDED(initial) && SUCCEEDED(native->EndDraw()), "native complete frame is initialized before incremental draws");
    const auto* menu = Find(nativeItems, "menu"); Check(menu != nullptr, "native menu target exists");
    const auto menuIndex = static_cast<std::size_t>(menu - nativeItems.data());
    for (const auto hover : {std::optional<std::size_t>(menuIndex), std::optional<std::size_t>{}})
    {
        const auto damage = StatusBarHoverDamage(nativeItems, hover ? std::optional<std::size_t>{} : menuIndex,
            hover, 640, 64);
        Check(SUCCEEDED(DrawStatusBarHoverUpdate(native.Get(), text, nativeItems, damage, 640, 64, 1.f,
            PersonalizationSettings{}, StatusBarPalette{}, hover, false, 0, true, nativeScroll)) &&
            SUCCEEDED(composition->Commit()), "real DComp accepts the production hover enter/leave updates");
    }
    Check(updatedPixels < fullPixels / 4, "hover repaint area remains bounded to changed controls");
    std::cout << (requireCompleteReference ? "Complete-reference hover" : "Cached/original arbitrary-offset hover")
        << " and native DComp passed; updated pixels=" << updatedPixels
        << " equivalent full-frame pixels=" << fullPixels << '\n';
}
#include "status_bar_resources_cases.h"
#include "status_bar_scroll_cases.h"
#include "status_bar_content_update_cases.h"
#include "status_bar_focus_update_cases.h"
}
int main(int argc, char** argv)
{
    const std::string_view control = argc == 3 ? argv[2] : "";
    Check(argc == 2 || (argc == 3 && (control == "--uncached-resource-control" || control == "--unaligned-hover-control" || control == "--full-scroll-control" || control == "--full-content-control" || control == "--legacy-focus-return" || control == "--legacy-geometry-noop-control")),
        "expected repository root and optional offscreen regression control");
    CheckFullscreenRounds();
    CheckIndependentReveal();
    Locale::Instance().Init((std::filesystem::path(argv[1]) / L"lang").c_str());
    Locale::Instance().SetLanguage("en-US");
    ComPtr<ID3D11Device> gpu; ComPtr<IDXGIDevice> dxgi;
    ComPtr<ID2D1Factory1> factory; ComPtr<ID2D1Device> device; ComPtr<ID2D1DeviceContext> context;
    ComPtr<IDWriteFactory> text;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &gpu, nullptr, nullptr)) &&
        SUCCEEDED(gpu.As(&dxgi)) && SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf())) &&
        SUCCEEDED(factory->CreateDevice(dxgi.Get(), &device)) &&
        SUCCEEDED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context)) &&
        SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(text.GetAddressOf()))), "offscreen production renderer initializes");
    ComPtr<IDWriteFactory5> fontFactory;
    ComPtr<IDWriteFontFile> fontFile; ComPtr<IDWriteFontSetBuilder1> fontBuilder;
    ComPtr<IDWriteFontSet> fontSet;
    const auto fluentPath = std::filesystem::path(argv[1]) / L"assets/fonts/FluentSystemIcons-Regular.ttf";
    Check(SUCCEEDED(text.As(&fontFactory)) &&
        SUCCEEDED(fontFactory->CreateFontFileReference(fluentPath.c_str(), nullptr, &fontFile)) &&
        SUCCEEDED(fontFactory->CreateFontSetBuilder(&fontBuilder)) && SUCCEEDED(fontBuilder->AddFontFile(fontFile.Get())) &&
        SUCCEEDED(fontBuilder->CreateFontSet(&fontSet)) &&
        SUCCEEDED(fontFactory->CreateFontCollectionFromFontSet(fontSet.Get(), &fluentTestCollection)),
        "load the original bundled Fluent glyphs for resource/pixel regression");
    // Model a first font-resource collection without Fluent, then restore the
    // real collection. Its absence is deterministic even on a system that
    // also has a copy of Fluent installed globally.
    fontFile.Reset(); fontBuilder.Reset(); fontSet.Reset();
    const auto fallbackPath = std::filesystem::path(argv[1]) / L"assets/fonts/HarmonyOS-Sans/HarmonyOS_Sans_SC_Regular.ttf";
    Check(SUCCEEDED(fontFactory->CreateFontFileReference(fallbackPath.c_str(), nullptr, &fontFile)) &&
        SUCCEEDED(fontFactory->CreateFontSetBuilder(&fontBuilder)) && SUCCEEDED(fontBuilder->AddFontFile(fontFile.Get())) &&
        SUCCEEDED(fontBuilder->CreateFontSet(&fontSet)) &&
        SUCCEEDED(fontFactory->CreateFontCollectionFromFontSet(fontSet.Get(), &fluentFallbackCollection)),
        "load a deterministic non-Fluent collection for resource recovery");
    CheckResourceReuse(factory.Get(), dxgi.Get(), context.Get(), text.Get(), control == "--uncached-resource-control");
    StatusBarSnapshot data; data.clock = L"2026/10/23   16:25";
    data.power.emplace(); data.power->available = true; data.power->batteryPercent = 58;
    std::vector<std::string> pinned;
    for (unsigned index = 0; index < 24; ++index)
    {
        tray::Icon icon;
        icon.key = icon.persistentKey = "visibility-test-" + std::to_string(index);
        icon.width = icon.height = 1; icon.pixels = {0xff33aa55};
        pinned.push_back(icon.persistentKey);
        data.tray.push_back(std::move(icon));
    }
    const auto defaults = BuildStatusBarItems(StatusBarSettings{}, data);
    Check(Find(defaults, "clock") && Find(defaults, "notifications") && Find(defaults, "controlCenter"),
        "clock, notifications and grouped control center start visible");
    if (control == "--unaligned-hover-control")
    {
        // Preserve the observed cross-origin failure as a calibration control.
        CheckHoverUpdates(device.Get(), context.Get(), text.Get(), data, pinned, false, true);
        return 0;
    }
    if (control == "--full-scroll-control")
    {
        CheckTrayScrollUpdates(device.Get(), context.Get(), text.Get(), data, pinned, true);
        return 0;
    }
    if (control == "--full-content-control")
    {
        CheckContentUpdates(device.Get(), context.Get(), text.Get(), data, true);
        return 0;
    }
    if (control == "--legacy-focus-return")
    {
        CheckFocusReturnRoutes(context.Get(), text.Get(), data, pinned, true);
        return 0;
    }
    tray_geometry_tests::Run(Check, context.Get(), text.Get(), data, pinned, control == "--legacy-geometry-noop-control");
    if (control == "--legacy-geometry-noop-control") return 2; // The retained old writer must fail the no-op assertion.
    CheckFocusFeedbackUpdates(context.Get(), text.Get(), data, pinned);
    CheckFocusNativeUpdates(device.Get(), text.Get(), data, pinned);
    CheckFocusReturnRoutes(context.Get(), text.Get(), data, pinned, false);
    CheckHoverUpdates(device.Get(), context.Get(), text.Get(), data, pinned, true, true);
    CheckHoverUpdates(device.Get(), context.Get(), text.Get(), data, pinned, false, false);
    CheckTrayScrollUpdates(device.Get(), context.Get(), text.Get(), data, pinned);
    CheckContentUpdates(device.Get(), context.Get(), text.Get(), data);
    CheckContentTextGenerations(context.Get(), text.Get(), data);
    for (const int mode : {0, 1, 2}) for (const bool clock : {false, true})
    for (const bool notifications : {false, true}) for (const bool merged : {false, true})
    for (const bool rightAligned : {false, true}) for (const float scale : {1.f, 2.f})
    for (const UINT dipWidth : {640u, 1920u})
    {
        StatusBarSettings settings;
        settings.controlCenter = mode != 2; settings.controlCenterSingleIcon = mode == 1;
        settings.clock = clock; settings.notifications = notifications; settings.batteryPercentage = true;
        settings.pinnedTrayItems = pinned;
        auto items = BuildStatusBarItems(settings, data);
        Check((Find(items, "clock") != nullptr) == clock && (Find(items, "notifications") != nullptr) == notifications &&
            (Find(items, "controlCenter") != nullptr) == (mode != 2), "hidden items have no rendered or keyboard target");
        const UINT width = static_cast<UINT>(static_cast<float>(dipWidth) * scale),
            height = static_cast<UINT>((merged ? 64.f : 32.f) * scale);
        ComPtr<ID2D1Bitmap1> target;
        Check(SUCCEEDED(context->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0,
            D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &target)), "render target initializes");
        context->SetTarget(target.Get()); context->SetDpi(96, 96);
        context->BeginDraw(); context->Clear(D2D1::ColorF(0, 0.f));
        const auto result = DrawStatusBarContent(context.Get(), text.Get(), items, width, height, scale,
            PersonalizationSettings{}, StatusBarPalette{}, {}, false, 0, merged, rightAligned);
        const auto drawn = context->EndDraw(); context->SetTarget(nullptr);
        Check(SUCCEEDED(result) && SUCCEEDED(drawn), "visibility combinations render through the production layout");
        Check(!IsRectEmpty(&Find(items, "menu")->bounds) && !IsRectEmpty(&Find(items, "taskView")->bounds),
            "hiding the date and notifications preserves left-side controls");
        for (std::size_t i = 0; i < items.size(); ++i)
        {
            const auto& item = items[i];
            if (IsRectEmpty(&item.bounds)) continue;
            const POINT point{(item.bounds.left + item.bounds.right) / 2, (item.bounds.top + item.bounds.bottom) / 2};
            Check(HitTestStatusBarItems(items, point) == i, "visible targets remain distinct");
            if (item.action == StatusBarAction::ControlCenter)
            {
                const auto invocation = ResolveStatusBarInvocation(items, i, false);
                Check(invocation && invocation->action == StatusBarAction::ControlCenter,
                    "both control-center presentations open the same panel");
                if (mode == 1)
                    Check(item.glyph == status_bar_glyphs::kControls && item.controlGlyphs[0].empty() &&
                        item.controlGlyphs[1].empty() && item.controlGlyphs[2].empty() && item.batteryPercentageText.empty() &&
                        item.bounds.right - item.bounds.left == static_cast<LONG>(28 * scale),
                        "single-icon mode has one icon and no device or percentage placeholders");
            }
            for (std::size_t j = i + 1; j < items.size(); ++j)
            {
                RECT intersection{};
                Check(!IntersectRect(&intersection, &item.bounds, &items[j].bounds), "visible targets do not overlap");
            }
        }
    }
    std::cout << "192 production visibility/layout combinations passed\n";
}
