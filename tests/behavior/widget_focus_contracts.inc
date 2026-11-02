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
    auto provider = std::make_shared<snowdesktop::widget_runtime::WidgetSystemDataProvider>();
    WidgetEngine engine;
    engine.SetSystemDataProvider(provider);
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

    // Exercise the shipped community source before the opened callback, as the
    // real host does when caching its opening animation. Also cover reopening.
    auto sourceRoot = std::filesystem::absolute(std::filesystem::path(__FILE__)).parent_path();
    while (!std::filesystem::exists(sourceRoot / L"developer_assets/workshop_widgets/single-monitor/widget.json"))
    {
        const auto parent = sourceRoot.parent_path();
        Require(parent != sourceRoot && !parent.empty(), "find community source in candidate");
        sourceRoot = parent;
    }
    Require(WidgetEngine::InstallWidgetPackage((sourceRoot /
        L"developer_assets/workshop_widgets/single-monitor/widget.json").wstring(), error),
        "install actual single-monitor package");
    std::string permissionError;
    Require(WidgetEngine::SetWidgetPermissionDecision(L"b6a2874c-97b4-4f5d-a02b-d08a077c7efe",
        snowdesktop::widget::PermissionDecisionState::Granted,
        {"system.performance.read", "system.power.read", "system.network.read", "system.storage.read"},
        {}, permissionError), "approve installed monitor in the private profile");
    for (const std::string mode : {"cpu", "gpu", "memory", "vram", "battery", "disk", "network"})
    {
        const std::wstring instance = L"monitor-" + std::wstring(mode.begin(), mode.end());
        engine.RuntimeSetStorageValue(instance, "metric", mode);
        Require(engine.EnsureWidgetLoaded(instance, L"b6a2874c-97b4-4f5d-a02b-d08a077c7efe"),
            "load actual monitor " + mode + ": " + diagnostics());
        for (int opening = 0; opening < 2; ++opening)
        {
            engine.PrepareWidgetPanelFocus(instance);
            const auto render = [&] {
                context->BeginDraw();
                const bool rendered = engine.RenderWidgetPanel(instance, context.Get(),
                    RECT{0, 0, 320, 360}, "popover");
                const HRESULT drawn = context->EndDraw();
                Require(rendered && SUCCEEDED(drawn), "render actual monitor " + mode + ": " + diagnostics());
                Require(engine.GetWidgets().front().panelViewKeyboardFocusKey == "mode." + mode,
                    mode + " must be focused in the first animation frame");
            };
            render();
            engine.InvokeMouseEvent(instance, "onPopoverOpened", 0, 0, 0, 0);
            render();
            Require(engine.GetWidgetErrors().empty(), "monitor must not move focus after opening: " + diagnostics());
            engine.CloseWidgetPanelSurface(instance, "popover");
        }
        engine.UnloadWidget(instance);
    }

    // Another host consumer is already sampling before the Lua subscription is
    // created. A newly loaded widget must read that same past immediately.
    for (const std::string topic : {"system.cpu", "system.network.traffic"})
        Require(provider->StartTopic("history-contract", topic, std::chrono::milliseconds(500)),
            "start pre-existing private monitor " + topic);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (provider->ResourceHistory("system.cpu").size() < 3 ||
        provider->ResourceHistory("system.network.traffic").size() < 3)
    {
        Require(std::chrono::steady_clock::now() < deadline, "pre-existing monitors must have three time buckets within 8s");
        Sleep(20);
    }
    Write(package / L"main.lua", R"(
return widget.define({setup=function()
    assert(widget.hasFeature("data.subscription.history"),"history capability missing")
    for _,topic in ipairs({"system.cpu","system.network.traffic"}) do
        local h=data.subscribe(topic,{maxAgeMs=1000})
        local points,reason=h:history()
        assert(not reason and #points>=3 and #points<=61,"existing history unavailable: "..tostring(reason))
        local previous=0
        for _,p in ipairs(points) do
            assert(p.timestamp>previous and p.ageMs>=0 and p.ageMs<=60000,"invalid history time")
            assert(p.value==nil or (p.value>=0 and p.value<math.huge),"invalid history value")
            previous=p.timestamp
        end
        h:unsubscribe()
        local empty,closed=h:history()
        assert(#empty==0 and closed=="unsubscribed","released handle leaked history")
    end
    return {}
end,render=function() end})
)");
    Write(package / L"widget.json", R"({"schemaVersion":2,"apiVersion":2,
        "dataVersion":1,"id":"57aeb6ac-104b-43fb-af95-10d7ce082f56",
        "slug":"focus-contract","version":"1.0.1","entry":"main.lua",
        "name":"History Contract","permissions":[],"optionalPermissions":["system.performance.read","system.network.read"],
        "requiredFeatures":["data.subscribe","data.system.cpu","data.system.network.traffic","data.subscription.history"]})");
    Require(WidgetEngine::InstallWidgetPackage((package / L"widget.json").wstring(), error),
        "install actual history subscription fixture");
    Require(WidgetEngine::SetWidgetPermissionDecision(L"57aeb6ac-104b-43fb-af95-10d7ce082f56",
        snowdesktop::widget::PermissionDecisionState::Granted,
        {"system.performance.read", "system.network.read"}, {}, permissionError), "grant private fixture permissions");
    Require(engine.EnsureWidgetLoaded(L"history-reader", L"57aeb6ac-104b-43fb-af95-10d7ce082f56"),
        "read already collected history during setup: " + diagnostics());
    Require(engine.GetWidgetErrors().empty(), "Lua history contract: " + diagnostics());
    engine.UnloadWidget(L"history-reader");
    Write(package / L"main.lua", R"(
return widget.define({setup=function()
    local h=data.subscribe("system.cpu")
    local points,reason=h:history()
    assert(#points==0 and reason=="permissionDenied","denied subscription leaked shared history")
    return {}
end,render=function() end})
)");
    auto deniedManifest = Read(package / L"widget.json");
    const auto version = deniedManifest.find("1.0.1");
    Require(version != std::string::npos, "find private fixture version");
    deniedManifest.replace(version, 5, "1.0.2");
    Write(package / L"widget.json", deniedManifest);
    Require(WidgetEngine::InstallWidgetPackage((package / L"widget.json").wstring(), error), "update denied fixture");
    Require(WidgetEngine::SetWidgetPermissionDecision(L"57aeb6ac-104b-43fb-af95-10d7ce082f56",
        snowdesktop::widget::PermissionDecisionState::Granted, {}, {}, permissionError), "approve package without granting the optional topic");
    Require(engine.EnsureWidgetLoaded(L"history-denied", L"57aeb6ac-104b-43fb-af95-10d7ce082f56"),
        "load denied history subscription: " + diagnostics());
    Require(engine.GetWidgetErrors().empty(), "history must respect permission: " + diagnostics());
    engine.UnloadWidget(L"history-denied");
    provider->RemoveConsumer("history-contract");
}
}
