// Included inside native_preview_order_cases, after its owned process fixture.
// A private inactive desktop isolates actual EnumWindows input from user apps.
inline void RunRunningRefresh()
{
    const auto name = L"SnowDesktopRunningRefresh-" + std::to_wstring(GetCurrentProcessId());
    const HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    Require(desktop != nullptr, "create inactive running-window discovery desktop");
    std::exception_ptr failure;
    std::thread worker([&] {
        try
        {
            Require(SetThreadDesktop(desktop) != FALSE, "select private discovery desktop before window creation");
            native_drag_commit_cases::Profile profile;
            Fixture fixture;
            fixture.Start(name.c_str());
            const HWND window = fixture.Create(7, false);
            auto& app = profile.Model(false, profile.root / L"running-refresh");
            ComPtr<ID3D11Device> d3d;
            Require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                &d3d, nullptr, nullptr)), "create WARP pixel reference without a desktop presentation host");
            ComPtr<IDXGIDevice> dxgi;
            Require(SUCCEEDED(d3d.As(&dxgi)), "query WARP reference device");
            ComPtr<ID2D1Factory1> factory;
            Require(SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                IID_PPV_ARGS(&factory))), "create reference Direct2D factory");
            ComPtr<ID2D1Device> device;
            Require(SUCCEEDED(factory->CreateDevice(dxgi.Get(), &device)), "create reference Direct2D device");
            ComPtr<ID2D1DeviceContext> context;
            Require(SUCCEEDED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context)),
                "create reference draw context");
            Access::CheckRunningRefresh(app, window, context.Get());
            wchar_t legacyFlag[2]{};
            if (!GetEnvironmentVariableW(L"SNOWDESKTOP_TEST_R07_LEGACY_DAMAGE", legacyFlag, 2))
            {
                auto& legacy = profile.Model(false, profile.root / L"running-refresh-legacy");
                bool rejected = false;
                try { Access::CheckRunningRefresh(legacy, window, context.Get(), true); }
                catch (const std::runtime_error& error)
                {
                    if (std::string_view(error.what()) !=
                        "running-window changes must not invalidate unrelated desktop pixels (old full route fails here)" ||
                        Access::runningRefreshLegacyCalls == 0) throw;
                    rejected = true;
                    std::cout << "R07 legacy production invalidation reached and rejected by desktop-damage assertion\n";
                }
                Require(rejected, "the actual old full-invalidation route must fail its business assertion");
            }
        }
        catch (...) { failure = std::current_exception(); }
    });
    worker.join();
    CloseDesktop(desktop);
    if (failure) std::rethrow_exception(failure);
}
