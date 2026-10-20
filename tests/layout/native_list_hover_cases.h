#pragma once

namespace native_list_hover_cases
{
using namespace native_drag_commit_cases;

struct Rendering
{
    ComPtr<ID3D11Device> d3d;
    ComPtr<ID2D1Factory1> factory;
    ComPtr<ID2D1Device> device;
    ComPtr<ID2D1DeviceContext> context;
    ComPtr<ID2D1Bitmap1> target;
    Rendering()
    {
        Require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr)),
            "create isolated WARP list renderer");
        ComPtr<IDXGIDevice> dxgi;
        Require(SUCCEEDED(d3d.As(&dxgi)), "query list rendering DXGI device");
        Require(SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->CreateDevice(dxgi.Get(), &device)) &&
            SUCCEEDED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context)),
            "create isolated actual Direct2D list renderer");
        context->SetDpi(96, 96);
        const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        Require(SUCCEEDED(context->CreateBitmap(D2D1::SizeU(1200, 900), nullptr, 0, &properties, &target)),
            "allocate offscreen list target without a presentation window");
        context->SetTarget(target.Get());
    }
};

inline POINT Center(RECT rect) { return {(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2}; }

// Executes FileGroup -> hosted FolderMapping -> DrawListItem -> actual DirectWrite.
// Differing saved/popup scales reproduce the reported later-items-only change.
inline void RunFonts()
{
    Profile profile; Rendering rendering;
    auto& app = profile.Model(false, profile.root / L"fonts");
    Access::InitListTitleRendering(app, rendering.device.Get(), rendering.context.Get());
    for (const bool grouped : {false, true})
        for (const int columns : {1, 2, 3, 4})
        {
            context = "font isolation grouped=" + std::to_string(grouped) + " columns=" + std::to_string(columns);
            auto& renderer = Access::SeedListTitleScene(app, columns, grouped, true);
            Access::PaintListTitles(app, renderer, rendering.context.Get(), {LONG_MIN, LONG_MIN}, true);
            const auto baseline = Access::ListTitleFonts(app);
            const auto point = Center(renderer.GetMemberLayoutRect(1));
            Access::PaintListTitles(app, renderer, rendering.context.Get(), point, true);
            const auto hovered = Access::ListTitleFonts(app);
            Require(Access::ListTitleToken(app) != 0, "font scenario must execute the hovered scrolling title path");
            Require(!baseline[0].empty() && !baseline[1].empty() && !baseline[2].empty(),
                "observe real layouts before, at and after the hovered item");
            for (size_t i = 0; i < baseline.size(); ++i)
            {
                Require(!baseline[i].empty() && !hovered[i].empty(), "all list entries remain rendered during hover");
                for (const float size : hovered[i])
                    Require(std::abs(size - baseline[i][0]) < .001f,
                        "hover must preserve the glyph size of item " + std::to_string(i));
            }
            Require(Access::Widgets(app)[0].cellScale == 2,
                "offscreen popup rendering preserves the child's saved desktop scale");
        }
    Access::ClearListTitleRendering(app);
    Require(Access::NoPresentationWindows(app), "font isolation never starts a desktop host");
    std::cout << "Actual native popup font sizes remain unchanged before and after hovered entries in 1-4 columns\n";
}

inline void RunHover()
{
    Profile profile; Rendering rendering;
    auto& app = profile.Model(false, profile.root / L"hover");
    Access::InitListTitleRendering(app, rendering.device.Get(), rendering.context.Get());
    for (const bool popup : {false, true})
        for (const bool grouped : {false, true})
        {
            context = "hover popup=" + std::to_string(popup) + " grouped=" + std::to_string(grouped);
            auto& renderer = Access::SeedListTitleScene(app, 3, grouped, popup);
            const RECT row = renderer.GetMemberLayoutRect(1);
            const RECT text = renderer.GetListItemTextRect(row);
            const POINT title = Center(text);
            Access::PaintListTitles(app, renderer, rendering.context.Get(), title, true);
            Require(Access::ListTitleToken(app) != 0, "hovering a truncated native title starts its animation");
            Access::ListTitleElapsed(app, 100);
            Require(Access::ListTitleOffset(app) > 2 && Access::ListTitleOffset(app) < 4,
                "the actual hovered title advances within the first 100ms without an initial pause");
            const double started = Access::ListTitleStart(app);
            const auto key = Access::ListTitleKey(app);
            const auto token = Access::ListTitleToken(app);
            Access::ListTitlePressed(app, true); Access::SelectListTitle(app, 1);
            Access::PaintListTitles(app, renderer, rendering.context.Get(), title);
            Require(Access::ListTitleStart(app) == started && Access::ListTitleToken(app) == token &&
                Access::ListTitleKey(app) == key,
                "pressing and selecting the hovered entry must preserve animation identity and phase");
            Access::ListTitlePressed(app, false);
            const POINT icon{row.left + 2, title.y};
            Require(icon.x < text.left, "icon-side pointer is outside the title rectangle");
            Access::PaintListTitles(app, renderer, rendering.context.Get(), icon);
            Require(Access::ListTitleStart(app) == started && Access::ListTitleKey(app) == key,
                "moving within the item's hover area must retain scrolling outside the title");
            // The real scheduler advances after an intentionally slow redraw;
            // a bounded wait supplies only the OS clock, not the callback result.
            const auto deadline = GetTickCount64() + 1000;
            const auto begin = GetTickCount64();
            while (GetTickCount64() - begin < 300 && GetTickCount64() < deadline) Sleep(1);
            Access::DispatchListTitleFrame(app);
            Require(Access::ListTitleToken(app) == token && Access::ListTitleStart(app) == started,
                "a redraw taking over 250ms must not cancel or replay a hovered title");
            Access::MoveListTitlePointer(app, {row.right + 1, row.bottom + 1});
            Require(Access::ListTitleToken(app) == 0, "leaving the hover region cancels before the next animation tick");
            Access::PaintListTitles(app, renderer, rendering.context.Get(), icon);
            Require(Access::ListTitleToken(app) != 0 && Access::ListTitleStart(app) > started,
                "reentering the icon area immediately starts a fresh hover");
            const auto other = Center(renderer.GetMemberLayoutRect(2));
            Access::PaintListTitles(app, renderer, rendering.context.Get(), other);
            Require(Access::ListTitleKey(app) != key,
                "moving to another entry transfers scrolling exclusively to that identity");
            Access::ShortenListTitle(app, 2);
            Access::PaintListTitles(app, renderer, rendering.context.Get(), other);
            Require(Access::ListTitleToken(app) == 0,
                "a title that fits after a content change returns to static rendering");
            Access::PaintListTitles(app, renderer, rendering.context.Get(), icon);
            if (popup)
            {
                Access::CloseListTitlePopup(app);
                Require(Access::ListTitleToken(app) == 0, "closing a popup cancels its title animation");
            }
        }
    Access::ClearListTitleRendering(app);
    Require(Access::NoPresentationWindows(app), "hover regression never starts a desktop presentation host");
    std::cout << "Actual list draw and scheduler preserve click/selection phase, whole-item hover and slow frames\n";
}
}
