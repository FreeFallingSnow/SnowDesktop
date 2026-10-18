#pragma once

namespace native_popup_request_cases
{
using namespace native_drag_commit_cases;
namespace pair = native_widget_pair_cases;
struct RequestObserverScope
{
    DesktopApp& app;
    RequestObserverScope(DesktopApp& host, const std::function<void(std::size_t, POINT)>& presenter) : app(host)
    { Access::ObserveNativePopupRequests(app, presenter); }
    ~RequestObserverScope() { Access::ClearNativePopupRequestObserver(app); }
};
inline void Run()
{
    Profile profile;
    std::size_t scenes = 0;
    for (const bool multiple : {false, true})
    for (const int sequence : {0, 1, 2, 3, 4})
    {
        context = "S03 native popup request multiple=" + std::to_string(multiple) + " sequence=" + std::to_string(sequence);
        pair::Samples samples(profile, scenes++);
        auto first = pair::Component(L"popup-A", pair::collection, 0, 0, false);
        auto second = pair::Component(L"popup-B", pair::collection, 3, 0, false);
        first.gridSpan = second.gridSpan = {1, 1};
        samples.Add(first, L"keep"); samples.Add(second, L"keep");
        auto incoming = pair::Component(L"incoming", pair::files, 10, 6, false);
        samples.Add(incoming, L"one");
        std::vector<fs::path> selected{samples.all.back().path};
        if (multiple) { samples.Add(incoming, L"two"); selected.push_back(samples.all.back().path); }
        auto& app = profile.Model(false, samples.root / L"layout");
        Access::SeedWidgetTransactions(app, {first, second}, samples.ordinary);
        Access::PrepareReload(app); Access::BindAsync(app);
        native_drag_async_reload_cases::Refresh(app);
        const auto original = Access::Widgets(app);
        Require(Access::Save(app), "save native popup request source before the gesture");
        const auto saved = Read(Access::LayoutPath(app));
        const auto captured = Access::BeginGroupSurfaceIncoming(app, selected);
        Require(captured.entries.size() == selected.size(), "popup dwell receives the actual complete desktop selection");
        POINT expectedPoint = Access::TrackNativePopupRequest(app, L"popup-A");
        Require(Access::NativePopupRequestArmed(app) && Access::NoIngressPopup(app),
            "S03: entering the real opener tracks a candidate without constructing its popup");
        std::wstring expectedId = L"popup-A", replacement = L"popup-A";
        if (sequence == 1) Access::LeaveNativePopupRequest(app);
        else if (sequence == 2) { expectedPoint = Access::TrackNativePopupRequest(app, L"popup-B"); expectedId = L"popup-B"; }
        else if (sequence == 3) Access::EndActualGesture(app);
        else if (sequence == 4) { Access::ReplaceNativePopupTarget(app); replacement = L"replacement-popup-A"; }
        std::size_t requests = 0;
        const auto presenter = [&](std::size_t index, POINT point) {
            ++requests;
            Require(index < Access::Widgets(app).size() && Access::Widgets(app)[index].id == expectedId &&
                point.x == expectedPoint.x && point.y == expectedPoint.y,
                "S03: the final native UI service request must use the same live opener identity and final pointer point");
            Require(Access::NoIngressPopup(app), "the request observer cannot stand in for a constructed or visible popup");
        };
        const bool opened = Access::RequestNativePopup(app, GetTickCount() + 10000, presenter);
        const bool expected = sequence == 0 || sequence == 2;
        Require(opened == expected && requests == (expected ? 1u : 0u) && !Access::NativePopupRequestArmed(app),
            "S03: actual popup readiness requests once; leave, session end or replaced target identity cancels the old request");
        Require(!Access::RequestNativePopup(app, GetTickCount() + 20000, presenter) && requests == (expected ? 1u : 0u),
            "S03: a consumed or cancelled dwell cannot repeat its native popup request");
        Access::EndActualGesture(app);
        const auto check = [&] {
            Require(Access::Widgets(app).size() == original.size(), "popup request decisions cannot create/remove target components");
            for (const auto& widget : original)
            {
                auto config = widget;
                if (config.id == L"popup-A") config.id = replacement;
                Require(pair::Configuration(pair::Widget(app, config.id)) == pair::Configuration(config),
                    "a popup request changes no component membership, geometry, settings or physical file ownership");
            }
            samples.Check(); Require(Access::NoIngressPopup(app), "native request-boundary tests cannot construct desktop presentation windows");
        };
        check();
        if (sequence != 4) Require(Read(Access::LayoutPath(app)) == saved, "requesting or cancelling presentation cannot persist a file-drop/layout change");
        Require(Access::Save(app), "save actual source model after popup request cancellation");
        Access::Load(app); native_drag_async_reload_cases::Refresh(app, true); check();
        Access::StopModel(app); samples.Cleanup();
    }
    std::cout << scenes << " native popup readiness, live request identity, cancellation and storage scenes passed; final UI service observed only\n";
}
inline void RunExternalReplacement()
{
    // Two actual Enter sequences cover the shared source-turnover boundary.
    // Only the final native UI service is observed. Waiting uses the current
    // policy delay as an input; no exact duration becomes a product assertion.
    Profile profile;
    std::size_t scenes = 0;
    for (const bool tab : {false, true})
    {
        context = "S03 external source replacement tab=" + std::to_string(tab);
        pair::Samples samples(profile, scenes++);
        std::vector<DesktopWidget> widgets;
        if (tab)
        {
            widgets = native_group_surface_cases::SurfaceWidgets(samples, false, false, false);
            widgets[4].activeCategoryId = L"A";
        }
        else
        {
            auto collection = pair::Component(L"popup-A", pair::collection, 0, 0, false);
            collection.gridSpan = {1, 1}; samples.Add(collection, L"keep"); widgets.push_back(std::move(collection));
        }
        const auto a = samples.root / L"external-A.txt", b = samples.root / L"external-B.txt";
        { std::ofstream stream(a, std::ios::binary); stream << "external-A"; }
        { std::ofstream stream(b, std::ios::binary); stream << "external-B"; }
        auto first = ole_file_source_fixture::Make({a}, true), second = ole_file_source_fixture::Make({b}, true);
        auto& app = profile.Model(false, samples.root / L"layout");
        Access::SeedWidgetTransactions(app, std::move(widgets), samples.ordinary);
        Access::PrepareReload(app); Access::BindAsync(app);
        native_drag_async_reload_cases::Refresh(app);
        Require(Access::Save(app), "save native model before the actual external source-turnover sequence");
        const auto saved = Read(Access::LayoutPath(app));
        std::size_t requests = 0;
        RequestObserverScope observe(app, [&](std::size_t index, POINT) {
            ++requests;
            Require(index < Access::Widgets(app).size() && Access::Widgets(app)[index].id == L"popup-A" && Access::NoIngressPopup(app),
                "external popup observation records the actual request without constructing a native window");
        });
        auto adapter = Access::ExternalAdapter(app);
        const POINT outside{1050, 650};
        DWORD effect = 7;
        Require(SUCCEEDED(adapter->DragEnter(first.Get(), MK_LBUTTON, Access::ExternalPoint(outside), &effect)),
            "first source must enter through the actual native OLE adapter");
        POINT point{};
        if (tab)
        {
            Access::TrackGroupSurfaceDwell(app, L"B");
            const RECT frame = Access::GroupSurfaceTabRect(app, L"surface-group", L"B");
            point = {(frame.left + frame.right) / 2, (frame.top + frame.bottom) / 2};
            Require(Access::GroupSurfaceDwellArmed(app), "external tab source must actually arm its native timer");
        }
        else
        {
            point = Access::TrackNativePopupRequest(app, L"popup-A");
            Require(Access::NativePopupRequestArmed(app), "external popup source must actually arm its native timer");
        }
        const auto waited = GetTickCount64();
        const auto delay = static_cast<ULONGLONG>(tab ? kCollectionGroupTabDwellDelayMs : kCollectionPopupDwellDelayMs) + 50;
        native_ole_file_cases::Wait([&] { return GetTickCount64() - waited >= delay; },
            "wait for the original source's actual dwell to mature without a presentation window");
        effect = 7;
        Require(SUCCEEDED(adapter->DragEnter(second.Get(), MK_LBUTTON, Access::ExternalPoint(point), &effect)),
            "replacement source must enter through the same actual native OLE adapter");
        Require(requests == 0 && (tab ? Access::GroupSurfaceDwellArmed(app) : Access::NativePopupRequestArmed(app)),
            "D12: a replacement external source cannot consume the prior source's mature popup dwell");
        if (tab)
        {
            Require(!Access::ActivateGroupSurfaceDwell(app, GetTickCount()) && pair::Widget(app, L"surface-group").activeCategoryId == L"A",
                "D12: a replacement external source must restart the group-tab wait before activating anything");
            Require(Access::ActivateGroupSurfaceDwell(app, GetTickCount() + 10000) && pair::Widget(app, L"surface-group").activeCategoryId == L"B",
                "the same live replacement source can activate its own new tab dwell");
        }
        else
            Require(Access::RequestNativePopup(app, GetTickCount() + 10000, {}) && requests == 1,
                "the same live replacement source emits exactly one new popup UI request after its own wait");
        Require(first->dataReads == 0 && second->dataReads == 0 && first->starts == 0 && second->starts == 0 &&
            first->ends == 0 && second->ends == 0 && !first->wrongApartment && !second->wrongApartment,
            "external hover/source replacement must not extract delayed content or start a file-operation lease");
        Require(Read(a) == "external-A" && Read(b) == "external-B" && Access::NoIngressPopup(app),
            "both real external source files remain unchanged without any desktop presentation window");
        if (!tab) Require(Read(Access::LayoutPath(app)) == saved, "popup request source turnover cannot save a file-drop change");
        Access::EndExternalModel(app); samples.Check();
        Access::ClearNativePopupRequestObserver(app);
        Access::StopModel(app); samples.Cleanup();
    }
    std::cout << scenes << " actual native external source replacement popup/tab dwell scenes passed; UI requests observed only\n";
}
}
