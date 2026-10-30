// Included inside native_preview_order_cases. Only DWM thumbnail allocation is
// supplied: inactive desktops do not promise compositor surfaces. Native source
// windows, popup layout, mouse dispatch, host close/notification and discovery
// remain real. No user desktop host or physical pointer is operated.
namespace close_refresh
{
inline unsigned enumerations = 0, registrations = 0;
inline std::uintptr_t nextThumbnail = 0;
inline std::unordered_map<HTHUMBNAIL, HWND> thumbnails;
inline HWND staleSource = nullptr;
inline decltype(&EnumWindows) originalEnum = nullptr;
inline BOOL WINAPI Enumerate(WNDENUMPROC callback, LPARAM parameter)
{
    ++enumerations;
    return originalEnum(callback, parameter);
}
inline HRESULT WINAPI Register(HWND, HWND source, PHTHUMBNAIL result)
{
    ++registrations;
    *result = reinterpret_cast<HTHUMBNAIL>(++nextThumbnail);
    thumbnails.emplace(*result, source);
    return S_OK;
}
inline HRESULT WINAPI Unregister(HTHUMBNAIL thumbnail)
{
    return thumbnails.erase(thumbnail) == 1 ? S_OK : E_INVALIDARG;
}
inline HRESULT WINAPI QuerySize(HTHUMBNAIL thumbnail, PSIZE size)
{
    const auto found = thumbnails.find(thumbnail);
    if (found == thumbnails.end()) return E_INVALIDARG;
    if (found->second == staleSource)
    {
        staleSource = nullptr;
        return E_FAIL;
    }
    *size = {300, 180};
    return S_OK;
}
inline HRESULT WINAPI Update(HTHUMBNAIL thumbnail, const DWM_THUMBNAIL_PROPERTIES* properties)
{
    return thumbnails.contains(thumbnail) && properties->fVisible &&
        properties->rcDestination.right > properties->rcDestination.left &&
        properties->rcDestination.bottom > properties->rcDestination.top ? S_OK : E_INVALIDARG;
}

inline void Check(bool legacyRefresh, const wchar_t* desktopName)
{
    struct ModulePolicy
    {
        // Keep this private discovery fixture independent of the user's
        // taskbar auto-hide setting and external minimize-hook installation.
        decltype(snowdesktop::runtime_modules::policyMask.load()) previous =
            snowdesktop::runtime_modules::policyMask.exchange(0);
        ~ModulePolicy() { snowdesktop::runtime_modules::policyMask.store(previous); }
    } modulePolicy;
    native_drag_commit_cases::Profile profile;
    auto& app = profile.Model(false, profile.root / L"close-refresh");
    Access::PreparePreviewOrder(app);
    Access::EnableCloseRefresh(app);
    Fixture fixture;
    fixture.Start(desktopName);
    const HWND first = fixture.Create(40, false);
    const HWND middle = fixture.Create(41, false);
    const HWND last = fixture.Create(42, false);
    Access::InputSafetyImport enumerate("EnumWindows", reinterpret_cast<ULONG_PTR>(&Enumerate));
    originalEnum = reinterpret_cast<decltype(&EnumWindows)>(enumerate.original);
    Access::InputSafetyImport registration("DwmRegisterThumbnail", reinterpret_cast<ULONG_PTR>(&Register));
    Access::InputSafetyImport retirement("DwmUnregisterThumbnail", reinterpret_cast<ULONG_PTR>(&Unregister));
    Access::InputSafetyImport measurement("DwmQueryThumbnailSourceSize", reinterpret_cast<ULONG_PTR>(&QuerySize));
    Access::InputSafetyImport properties("DwmUpdateThumbnailProperties", reinterpret_cast<ULONG_PTR>(&Update));
    auto& preview = Access::PrepareClosePreview(app,
        {{first, L"first"}, {middle, L"middle"}, {last, L"last"}}, 0);
    // Keep the real live-preview identity, so hide/destroy would also enumerate
    // all application windows through the old collector path.
    Access::TrackLivePreview(app, fixture.executable);
    const auto cards = preview.GetWindows();
    Require(cards.size() == 3 && thumbnails.size() == 3, "prepare three owned thumbnail relationships");
    enumerations = registrations = 0;
    const auto clickClose = [](HWND card) {
        RECT client{};
        GetClientRect(card, &client);
        const RECT close = CalculateDockWindowPreviewCloseButtonRect(client, GetDpiForWindow(card));
        SendMessageW(card, WM_LBUTTONUP, 0, MAKELPARAM(
            (close.left + close.right) / 2, (close.top + close.bottom) / 2));
    };
    try
    {
        clickClose(cards[1]);
        // Restore the removed production call as an isolated negative control.
        if (legacyRefresh) Access::ReconcileCloseRefresh(app);
        Require(enumerations == 0,
            "closing a preview must leave global discovery off the next card's input path");
        Require(registrations == 0 && thumbnails.size() == 2 &&
                preview.GetWindows() == std::vector<HWND>{cards[0], cards[2]},
            "closing reuses surviving native cards and DWM relationships");
        Require(!Access::CloseRefreshReconciled(app), "accepted close leaves membership dirty for maintenance");
        const ULONGLONG deadline = GetTickCount64() + 3000;
        while (IsWindow(middle) && GetTickCount64() < deadline) Sleep(1);
        Require(!IsWindow(middle), "asynchronous close destroys the owned source window");
        Access::NotifyPreviewWindow(app, middle, EVENT_OBJECT_HIDE);
        Access::NotifyPreviewWindow(app, middle, EVENT_OBJECT_DESTROY);
        Require(enumerations == 0 && registrations == 0 && preview.GetWindows().size() == 2,
            "accepted close notification burst must not rescan or rebuild remaining cards");
        staleSource = last;
        SendMessageW(cards[0], WM_MBUTTONUP, 0, MAKELPARAM(10, 10));
        Require(enumerations == 0 && registrations == 1 && thumbnails.size() == 1 &&
                preview.GetWindows() == std::vector<HWND>{cards[2]},
            "next card remains actionable and only an invalid DWM relationship is retried");
        clickClose(cards[2]);
        Require(preview.IsCleared() && thumbnails.empty() && enumerations == 0,
            "last close releases thumbnail ownership without a synchronous discovery pass");
        Access::ReconcileCloseRefresh(app);
        Require(enumerations == 1 && Access::CloseRefreshReconciled(app),
            "one normal maintenance discovery consumes the accumulated close revisions");
    }
    catch (...)
    {
        preview.Hide();
        throw;
    }
}
}

inline void RunCloseRefresh()
{
    const auto name = L"SnowDesktopCloseRefresh-" + std::to_wstring(GetCurrentProcessId());
    const HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    Require(desktop != nullptr, "create inactive preview-close desktop");
    std::exception_ptr failure;
    std::thread worker([&] {
        try
        {
            Require(SetThreadDesktop(desktop) != FALSE, "select inactive preview-close desktop");
            bool rejected = false;
            try { close_refresh::Check(true, name.c_str()); }
            catch (const std::runtime_error& error)
            {
                if (std::string_view(error.what()) !=
                    "closing a preview must leave global discovery off the next card's input path") throw;
                rejected = true;
            }
            Require(rejected, "old synchronous discovery must fail the next-input-path assertion");
            close_refresh::Check(false, name.c_str());
            std::cout << "close refresh: legacy synchronous scan rejected; candidate close/notifications 0 scans, "
                "survivors 0 registrations, invalid relationship 1 retry, maintenance 1 scan\n";
        }
        catch (...) { failure = std::current_exception(); }
    });
    worker.join();
    CloseDesktop(desktop);
    if (failure) std::rethrow_exception(failure);
}
