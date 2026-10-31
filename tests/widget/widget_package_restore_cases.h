#pragma once

namespace widget_package_restore_cases
{
// Uses the real package manager, loader, failure cache and first D2D render.
// Only deployment data is redirected by the existing native test profile.
inline void Run(bool legacyMissingFailure = false)
{
    native_drag_commit_cases::Profile profile;
    const auto require = [](bool ok, const std::string& message) {
        if (!ok) throw std::runtime_error(message);
    };
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
    Microsoft::WRL::ComPtr<ID2D1Factory1> factory;
    Microsoft::WRL::ComPtr<ID2D1Device> d2dDevice;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> context;
    Microsoft::WRL::ComPtr<IDWriteFactory> text;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> target;
    require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &device, nullptr, nullptr)) && SUCCEEDED(device.As(&dxgi)) &&
        SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateDevice(dxgi.Get(), &d2dDevice)) &&
        SUCCEEDED(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context)) &&
        SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(text.GetAddressOf()))),
        "initialize offscreen rendering for package restoration");
    require(SUCCEEDED(context->CreateBitmap(D2D1::SizeU(192, 128), nullptr, 0,
        D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &target)),
        "create an actual first-frame render target");
    context->SetTarget(target.Get());
    WidgetEngine engine;
    require(engine.InitPreview(context.Get(), text.Get()), "initialize isolated actual widget engine");
    const std::wstring packageId = L"4c360510-dd3d-4d3d-9496-686477fcfa37";
    const std::string narrowId(packageId.begin(), packageId.end());
    const std::wstring instance = L"retained-unsubscribed-instance";
    const auto source = profile.root / L"restore-source";
    std::filesystem::create_directories(source);
    const auto write = [&](const wchar_t* file, const std::string& contents) {
        std::ofstream stream(source / file, std::ios::binary | std::ios::trunc);
        stream << contents;
        stream.close();
        require(static_cast<bool>(stream), "write isolated restoration package");
    };
    const auto manifest = [&](bool permission) {
        write(L"widget.json", std::string(R"({"schemaVersion":2,"apiVersion":2,"dataVersion":1,
            "id":"4c360510-dd3d-4d3d-9496-686477fcfa37","slug":"restore-test","version":"1.0.0",
            "entry":"main.lua","name":"Restoration fixture","requiredFeatures":["view.tree.core"],
            "permissions":)") + (permission ? "[\"clipboard.write\"]}" : "[]}"));
    };
    manifest(false);
    write(L"main.lua", R"(return widget.define({apiVersion=2,view=function()
        return view.text({key="restored",text="Restored",width="fill",height="fill"}) end}))");
    using snowdesktop::widget_runtime::WidgetHostStateKind;
    require(!engine.EnsureWidgetLoaded(instance, packageId) &&
        engine.GetWidgetHostState(instance, packageId).kind == WidgetHostStateKind::PackageMissing,
        "a retained instance starts as a missing-package placeholder");
    if (legacyMissingFailure)
        engine.RuntimeRecordError(instance,
            "Widget package is missing, disabled, or has not been migrated");
    const auto render = [&] {
        require(engine.EnsureWidgetLoaded(instance, packageId),
            "the first load after reinstall must recover without calling RetryWidget");
        context->BeginDraw();
        context->Clear(D2D1::ColorF(0, 0));
        engine.RenderWidget(instance, L"", context.Get(), {0, 0, 192, 128}, 2, 2);
        require(SUCCEEDED(context->EndDraw()) &&
            engine.GetWidgetHostState(instance, packageId).kind == WidgetHostStateKind::Ready &&
            !engine.GetWidgetRuntimeFailure(instance) && !engine.GetWidgets().empty() &&
            engine.GetWidgets().front().viewTree.has_value(),
            "the first restored frame uses a valid Lua view and has no stale runtime error");
    };
    std::wstring error;
    require(engine.InstallAndVerifyWidgetPackage(source.wstring(), error),
        "install and verify the package while its desktop placeholder persists");
    render();
    engine.UnloadWidget(instance);
    std::string uninstallError;
    require(WidgetEngine::UninstallWidgetPackage(narrowId, uninstallError),
        "uninstall the loaded package without deleting its persistent instance identity");
    require(!engine.EnsureWidgetLoaded(instance, packageId), "retain the placeholder after unsubscribe");
    require(engine.InstallAndVerifyWidgetPackage(source.wstring(), error),
        "reinstall and verify the same subscribed package");
    render();

    engine.RuntimeRecordError(L"actual-script-failure", "isolated syntax error");
    require(!engine.EnsureWidgetLoaded(L"actual-script-failure", packageId),
        "package availability does not silently retry an unrelated real script failure");
    engine.UnloadWidget(instance);
    require(WidgetEngine::UninstallWidgetPackage(narrowId, uninstallError) &&
        !engine.EnsureWidgetLoaded(instance, packageId), "return the retained instance to a missing package");
    manifest(true);
    require(WidgetEngine::InstallWidgetPackage(source.wstring(), error),
        "reinstall a package whose changed permission scope requires consent");
    require(!engine.EnsureWidgetLoaded(instance, packageId) &&
        engine.GetWidgetHostState(instance, packageId).kind == WidgetHostStateKind::PermissionPending,
        "automatic missing-package recovery still honors permission consent");
    engine.Shutdown();
    require(!IsWindowVisible(NativeDragIntegrationAccess::completionWindow),
        "the restoration regression never presents a desktop window");
    std::cout << "Retained package instances recover and render on the first load after reinstall\n";
}
}
