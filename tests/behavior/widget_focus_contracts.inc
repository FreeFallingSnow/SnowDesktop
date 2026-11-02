#include "widget/runtime/widget_engine.h"
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

namespace behavior
{
void WidgetAuxiliaryFocus()
{
    // Own the profile and offscreen device. Never create a desktop HWND or
    // initialize DesktopApp, Explorer, hooks or the user's widget storage.
    Profile profile;
    Apartment apartment;
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Device> d3d;
    Require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP,
        nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
        D3D11_SDK_VERSION, &d3d, nullptr, nullptr)), "create offscreen WARP device");
    ComPtr<IDXGIDevice> dxgi;
    Require(SUCCEEDED(d3d.As(&dxgi)), "query private DXGI device");
    ComPtr<ID2D1Factory1> factory;
    const D2D1_FACTORY_OPTIONS options{};
    Require(SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
        __uuidof(ID2D1Factory1), &options,
        reinterpret_cast<void**>(factory.GetAddressOf()))), "create offscreen D2D factory");
    ComPtr<ID2D1Device> device;
    Require(SUCCEEDED(factory->CreateDevice(dxgi.Get(), &device)), "create private D2D device");
    ComPtr<ID2D1DeviceContext> context;
    Require(SUCCEEDED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
        &context)), "create offscreen context");
    ComPtr<ID2D1Bitmap1> target;
    const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    Require(SUCCEEDED(context->CreateBitmap(D2D1::SizeU(320, 360), nullptr, 0,
        &properties, &target)), "create private render target");
    context->SetTarget(target.Get());
    ComPtr<IDWriteFactory> dwrite;
    Require(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwrite.GetAddressOf()))),
        "create DirectWrite factory");

    const auto package = profile.root / L"focus-package";
    Write(package / L"widget.json", R"({"schemaVersion":2,"apiVersion":2,
        "dataVersion":1,"id":"57aeb6ac-104b-43fb-af95-10d7ce082f56",
        "slug":"focus-contract","version":"1.0.0","entry":"main.lua",
        "name":"Focus Contract","permissions":[],"requiredFeatures":[
        "view.tree.core","view.surface.panel","view.surface.dialog",
        "view.surface.popover","control.focus","control.blur","view.focus.request"]})");
    Write(package / L"main.lua", R"(
local function tree()
    return view.column({key="metrics",width="fill",height="fill",children={
        view.button({key="mode.cpu",label="CPU",width="fill",height=28}),
        view.button({key="mode.gpu",label="GPU",width="fill",height=28}),
        view.button({key="mode.battery",label="Battery",width="fill",height=28})}})
end
return widget.define({view=tree,panel=tree,dialog=tree,popover=tree,
    event=function(context,model,event)
        if event.action=="opened" then
            assert(context.surface==event.kind,"opened callback surface mismatch")
            local accepted,reason=control.focus("mode.battery")
            assert(accepted,"actual opened focus rejected: "..tostring(reason))
            local blurred,blurReason=control.blur("mode.cpu")
            assert(not blurred and blurReason=="trustedGestureRequired",
                "focus-only opening permission must not authorize other input mutations")
        end
    end})
)");
    WidgetEngine engine;
    Require(engine.Init(context.Get(), dwrite.Get()), "initialize isolated real widget engine");
    std::wstring error;
    Require(WidgetEngine::InstallWidgetPackage((package / L"widget.json").wstring(), error),
        "install isolated focus package");
    const auto diagnostics = [&] {
        std::string text;
        for (const auto& item : engine.GetWidgetErrors()) text += item.message + "; ";
        return text;
    };
    for (const std::string surface : {"panel", "dialog", "popover"})
    {
        for (const bool renderBeforeOpened : {false, true})
        {
            const std::wstring instance = L"focus-" + std::wstring(surface.begin(), surface.end()) +
                (renderBeforeOpened ? L"-cached" : L"-first");
            Require(engine.EnsureWidgetLoaded(instance,
                L"57aeb6ac-104b-43fb-af95-10d7ce082f56"), "load real non-preview instance");
            engine.PrepareWidgetPanelFocus(instance);
            const auto render = [&] {
                context->BeginDraw();
                const bool rendered = engine.RenderWidgetPanel(instance, context.Get(),
                    RECT{0, 0, 320, 360}, surface);
                const HRESULT drawn = context->EndDraw();
                Require(rendered && SUCCEEDED(drawn), "render " + surface + ": " + diagnostics());
            };
            if (renderBeforeOpened)
            {
                render();
                Require(engine.GetWidgets().front().panelViewKeyboardFocusKey == "mode.cpu",
                    "pre-open animation cache reproduces first-button auto focus");
            }
            const char* opened = surface == "panel" ? "onPanelOpened" :
                (surface == "dialog" ? "onDialogOpened" : "onPopoverOpened");
            engine.InvokeMouseEvent(instance, opened, 0, 0, 0, 0);
            render();
            Require(engine.GetWidgetErrors().empty(), "opened callback accepted: " + diagnostics());
            Require(engine.GetWidgets().front().panelViewKeyboardFocusKey == "mode.battery",
                surface + " initial focus must follow the requested metric instead of CPU");
            std::string rejection;
            Require(!engine.RuntimeFocusHostInputFromTrustedGesture(instance, "mode.cpu", rejection) &&
                rejection == "trustedGestureRequired", "opening focus permission cannot escape its callback");
            engine.CloseWidgetPanelSurface(instance, surface);
            engine.UnloadWidget(instance);
        }
    }
}
}
