#pragma once

#include "../platform/native_import_patch.h"
#include "diagnostics/performance_trace.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <iterator>

namespace native_popup_input_cases
{
using namespace native_drag_commit_cases;
using VisibleFunction = BOOL (WINAPI*)(HWND);
using UpdateFunction = BOOL (WINAPI*)(HWND);
using InvalidateFunction = BOOL (WINAPI*)(HWND, const RECT*, BOOL);
using PositionFunction = decltype(&SetWindowPos);
inline DesktopApp* owner = nullptr;
inline HWND popup = nullptr, desktop = nullptr;
inline VisibleFunction originalVisible = nullptr;
inline UpdateFunction originalUpdate = nullptr;
inline InvalidateFunction originalInvalidate = nullptr;
inline PositionFunction originalPosition = nullptr;
inline unsigned rootInvalidations = 0, popupInvalidations = 0, commits = 0, backdropUpdates = 0;
inline bool floatingRoute = false;
inline std::atomic<DWORD> testThread{0};
inline std::vector<std::wstring> drawnOwners;

inline BOOL WINAPI Visible(HWND window)
{
    if (window == popup) return TRUE;
    return originalVisible(window);
}
inline BOOL WINAPI Update(HWND window)
{
    if (window != popup) return originalUpdate(window);
    if (GetUpdateRect(window, nullptr, FALSE)) Access::PaintPopupInput(*owner);
    return TRUE;
}
inline BOOL WINAPI Invalidate(HWND window, const RECT* rect, BOOL erase)
{
    if (window == desktop) ++rootInvalidations;
    if (window == popup) ++popupInvalidations;
    return originalInvalidate(window, rect, erase);
}
inline BOOL WINAPI Position(HWND window, HWND after, int x, int y, int width, int height, UINT flags)
{
    if (window == popup) flags &= ~SWP_SHOWWINDOW;
    return originalPosition(window, after, x, y, width, height, flags);
}
inline void Begin(snowdesktop::performance::ScopeToken&, std::string_view module,
    std::string_view phase, std::wstring_view id, std::uint64_t) noexcept
{
    if (GetCurrentThreadId() != testThread.load()) return;
    if (module == "composition.shared" && phase == "commit") ++commits;
    if (module == "popup" && phase == "backdrop.update") ++backdropUpdates;
    if (module == "widget.composition" && phase == "draw") drawnOwners.emplace_back(id);
}
inline void End(snowdesktop::performance::ScopeToken&) noexcept {}
inline void Value(std::string_view, std::string_view, std::wstring_view, double, std::uint64_t) noexcept {}
inline void Link(std::string_view, std::wstring_view, bool) noexcept {}
inline const snowdesktop::performance::Hooks hooks{Begin, End, Value, Link};

struct Observation
{
    const snowdesktop::performance::Hooks* previous;
    snowdesktop::test::NativeImportPatch visiblePatch, updatePatch, invalidatePatch, positionPatch;
    explicit Observation(DesktopApp& app)
        : previous(snowdesktop::performance::captureHooks.load()),
          visiblePatch("IsWindowVisible", reinterpret_cast<ULONG_PTR>(&Visible)),
          updatePatch("UpdateWindow", reinterpret_cast<ULONG_PTR>(&Update)),
          invalidatePatch("InvalidateRect", reinterpret_cast<ULONG_PTR>(&Invalidate)),
          positionPatch("SetWindowPos", reinterpret_cast<ULONG_PTR>(&Position))
    {
        owner = &app; popup = Access::PopupInputWindow(app); desktop = Access::PopupInputDesktop(app);
        originalVisible = reinterpret_cast<VisibleFunction>(visiblePatch.original);
        originalUpdate = reinterpret_cast<UpdateFunction>(updatePatch.original);
        originalInvalidate = reinterpret_cast<InvalidateFunction>(invalidatePatch.original);
        originalPosition = reinterpret_cast<PositionFunction>(positionPatch.original);
        testThread = GetCurrentThreadId();
        snowdesktop::performance::captureHooks.store(&hooks);
    }
    ~Observation()
    {
        Access::StopPopupInputMonitor(*owner);
        snowdesktop::performance::captureHooks.store(previous);
        owner = nullptr; popup = desktop = nullptr;
    }
    void Reset()
    {
        rootInvalidations = popupInvalidations = commits = backdropUpdates = 0; drawnOwners.clear();
        ValidateRect(desktop, nullptr); ValidateRect(popup, nullptr);
    }
};

inline POINT Tab(ScrollingItemWidget& view, std::wstring_view id)
{
    const RECT frame = view.GetLayoutFrameRect();
    for (LONG y = frame.top; y < std::min(frame.bottom, frame.top + 240); y += 3)
        for (LONG x = frame.left; x < frame.right; x += 3)
            if (view.CategoryIdAtPoint({x, y}) == id) return {x, y};
    throw std::runtime_error("R02 fixture must expose the actual requested category hit");
}
inline void Move(DesktopApp& app, POINT point)
{
    if (std::getenv("SNOWDESKTOP_R02_ORIGINAL_ROUTING")) Access::OriginalPopupInput(app, point, floatingRoute);
    else Access::MovePopupInput(app, point, floatingRoute);
}
inline void CheckPixels(DesktopApp& app, const Access::PopupInputSurface& surface)
{
    if (FAILED(surface.capture))
        std::cerr << "R02 surface capture HRESULT=0x" << std::hex << static_cast<unsigned long>(surface.capture) << std::dec << '\n';
    Require(SUCCEEDED(surface.capture) && !surface.pixels.empty(), "R02 reads pixels from the actual production surface draw");
    const auto reference = Access::FullPopupInputPixels(app);
    const bool equal = surface.pixels == reference;
    if (!equal)
    {
        const auto mismatch = std::mismatch(surface.pixels.begin(), surface.pixels.end(), reference.begin(), reference.end());
        const size_t index = static_cast<size_t>(mismatch.first - surface.pixels.begin());
        std::cerr << "R02 pixel mismatch at x=" << index / 4 % surface.Width() << " y=" << index / 4 / surface.Width()
            << " channel=" << index % 4 << " sizes=" << surface.pixels.size() << '/' << reference.size() << '\n';
        if (mismatch.first != surface.pixels.end() && mismatch.second != reference.end())
            std::cerr << "R02 actual/reference byte=" << static_cast<unsigned>(*mismatch.first)
                << '/' << static_cast<unsigned>(*mismatch.second) << '\n';
    }
    Require(equal,
        "R02 retained popup pixels equal an independently redrawn complete reference");
    Require(!Access::PopupInputFailed(app), "R02 normal routing cannot silently enter the full desktop failure fallback");
}

inline void CheckOriginalRoute(Profile& profile)
{
    // The child runs while this selected CTest still owns its binary lease.
    // A real exit 1 at the same business assertion proves the old route fails.
    constexpr auto variable = L"SNOWDESKTOP_R02_ORIGINAL_ROUTING";
    wchar_t executable[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    Require(length > 0 && length < std::size(executable), "R02 resolves the original-route control executable");
    const auto log = profile.root / L"r02-original-route.log";
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    struct CloseOutput
    {
        HANDLE value;
        ~CloseOutput() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    } output{CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
    Require(output.value != INVALID_HANDLE_VALUE, "R02 creates an isolated original-route log");
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = startup.hStdError = output.value; startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION child{};
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --popup-request";
    wchar_t previous[32768]{};
    const DWORD previousLength = GetEnvironmentVariableW(variable, previous, static_cast<DWORD>(std::size(previous)));
    Require(previousLength < std::size(previous), "R02 saves the process-local control flag");
    struct RestoreEnvironment
    {
        const wchar_t* name; const wchar_t* value;
        ~RestoreEnvironment() { SetEnvironmentVariableW(name, value); }
    } restore{variable, previousLength ? previous : nullptr};
    Require(SetEnvironmentVariableW(variable, L"1") != FALSE, "R02 enables the control only in this test process");
    const BOOL created = CreateProcessW(executable, command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child);
    CloseHandle(output.value); output.value = INVALID_HANDLE_VALUE;
    Require(created != FALSE, "R02 starts the isolated original-route control");
    const DWORD wait = WaitForSingleObject(child.hProcess, 30000);
    if (wait != WAIT_OBJECT_0)
    {
        TerminateProcess(child.hProcess, 99); // Only the fixture child created above.
        WaitForSingleObject(child.hProcess, 5000);
    }
    DWORD code = 0;
    const BOOL readExit = GetExitCodeProcess(child.hProcess, &code);
    CloseHandle(child.hThread); CloseHandle(child.hProcess);
    const auto evidence = Read(log);
    // Keep setup failures/timeouts observable too; Profile removes its owned
    // directory on failure, so this output is the persistent batch evidence.
    std::cout << "R02 original-route control command: --popup-request; expected exit=1; actual exit="
        << code << '\n' << evidence << std::flush;
    Require(wait == WAIT_OBJECT_0 && readExit && code == 1, "R02 original route must finish at its expected failing assertion");
    Require(evidence.find("R02 negative: executing the frozen original popup move/presentation route") != std::string::npos &&
        evidence.find("R02 unchanged held tab must add zero desktop damage, surface draws or commits") != std::string::npos,
        "R02 negative control must execute the original fault and fail at the target business assertion");
}

inline void Run(Profile& profile)
{
    if (!std::getenv("SNOWDESKTOP_R02_ORIGINAL_ROUTING")) CheckOriginalRoute(profile);
    struct Scene { const char* name; bool grouped, singleCell, dockCopy, fileCategories; };
    constexpr Scene scenes[] = {
        {"single-cell mapping", false, true, false, false},
        {"visible mapping source", false, false, false, false},
        {"visible file-group owner", true, false, false, false},
        {"single-cell file-group", true, true, false, false},
        {"single-cell file categories", false, true, false, true},
        {"visible file-categories group", true, false, false, true},
        {"Dock folder copy", false, true, true, false},
    };
    if (std::getenv("SNOWDESKTOP_R02_ORIGINAL_ROUTING"))
        std::cout << "R02 negative: executing the frozen original popup move/presentation route\n";
    for (const bool floating : {false, true})
    for (const auto& scene : scenes)
    {
        floatingRoute = floating;
        const std::string origin = floating ? "floating-" : "desktop-";
        context = std::string("R02 ") + origin + scene.name;
        std::cerr << context << " begin\n";
        auto& app = profile.Model(false, profile.root / (std::string("r02-") + origin + scene.name));
        Access::InitializePopupInputRendering(app);
        auto& view = Access::SeedPopupInput(app, scene.grouped, scene.singleCell, scene.dockCopy, scene.fileCategories);
        auto surface = Access::ObservePopupInputSurface(app);
        const auto cleanup = [&] { Access::ResetPopupInput(app); Access::StopModel(app); };
        try
        {
            Observation observation(app);
            Access::PaintPopupInput(app); CheckPixels(app, *surface.Get());
            Access::SyncPopupInputFrame(app, view);
            const auto pointA = Tab(view, L"a"), pointB = Tab(view, L"b");
            Require(view.BeginCategoryTabDrag(pointA), "R02 begins through the real category hit path");
            const auto originalOrder = view.GetCategoryTabOrder();
            Require(originalOrder == std::vector<std::wstring>{L"all", L"a", L"b", L"others"},
                "R02 fixture has a literal independently specified starting order");
            observation.Reset();
            auto draws = surface->begins;
            for (int move = 0; move < 12; ++move) Move(app, pointA);
            Require(view.GetCategoryTabOrder() == originalOrder && rootInvalidations == 0 &&
                popupInvalidations == 0 && surface->begins == draws && commits == 0 &&
                backdropUpdates == 0 && drawnOwners.empty(),
                "R02 unchanged held tab must add zero desktop damage, surface draws or commits");
            if (floating)
            {
                Access::ForgetPopupInputHostBounds(app);
                Move(app, pointA);
                Require(view.GetCategoryTabOrder() == originalOrder && surface->begins == draws + 1 &&
                    backdropUpdates == 1 && rootInvalidations == 0 && drawnOwners.empty(),
                    "R02 stable input still rebuilds actual floating geometry and content when host bounds need recovery");
                CheckPixels(app, *surface.Get());
                observation.Reset(); draws = surface->begins;
            }
            const POINT crossed{pointA.x + GetSystemMetrics(SM_CXDRAG) + 1, pointA.y};
            Require(view.CategoryIdAtPoint(crossed) == L"a", "R02 threshold crossing stays inside the same actual tab");
            Move(app, crossed);
            Require(view.GetCategoryTabOrder() == originalOrder && surface->begins == draws && commits == 0 && backdropUpdates == 0,
                "R02 crossing the drag threshold without changing order adds no visible work");
            Move(app, pointB);
            Require(view.GetCategoryTabOrder() == std::vector<std::wstring>{L"all", L"b", L"a", L"others"},
                "R02 moving Alpha onto Beta swaps the two literal category positions");
            Require(rootInvalidations == 0 && surface->begins == draws + 1 && surface->ends == surface->begins,
                "R02 changed order redraws the actual popup exactly once without desktop damage");
            const auto& expectedOwner = scene.grouped ? std::wstring(L"r02-group") : std::wstring(L"r02-source");
            Require(scene.singleCell ? drawnOwners.empty() : drawnOwners == std::vector<std::wstring>{expectedOwner},
                "R02 only the actual visible source owner redraws; a single-cell icon or Dock copy does not");
            CheckPixels(app, *surface.Get());
            const auto movedA = Tab(view, L"a");
            observation.Reset(); draws = surface->begins;
            Move(app, movedA); Move(app, {0, 0});
            Require(view.GetCategoryTabOrder() == std::vector<std::wstring>{L"all", L"b", L"a", L"others"} &&
                surface->begins == draws && commits == 0 && rootInvalidations == 0 && backdropUpdates == 0,
                "R02 staying on the moved tab or leaving all tabs preserves the last order without drawing");
            Access::CancelPopupInput(app);
            Require(!view.HasCategoryTabPress() && view.GetCategoryTabOrder() == originalOrder &&
                Access::PopupInputSource(app).categoryTabOrder.empty(),
                "R02 production cancellation restores the original empty/default storage, not a serialized reorder");
            Access::PaintPopupInput(app); CheckPixels(app, *surface.Get());

            // The root HWND is an observation boundary in this fixture. Start
            // the next independent gesture with no unrelated recovery pending.
            Access::SetPopupInputFullCollection(app, false);
            view.SetSearchText(L"Wm\u6c49e\u0301\U0001f642Z");
            surface = Access::ObservePopupInputSurface(app, surface.Get());
            Access::PaintPopupInput(app); Access::SyncPopupInputFrame(app, view);
            const RECT search = view.GetSearchBoxRect();
            Require(!IsRectEmpty(&search), "R02 actual categorized search box remains available with a nonempty query");
            const POINT start{search.left + view.Cu(10.0f), (search.top + search.bottom) / 2};
            view.BeginSearchPointerSelection(start, false);
            view.SetSearchComposition(L"\u5019\u9009", 1);
            Access::PaintPopupInput(app); CheckPixels(app, *surface.Get());
            const auto cursor = view.GetSearchCursorPosition(), anchor = view.GetSearchSelectionAnchor();
            observation.Reset(); draws = surface->begins;
            for (int move = 0; move < 12; ++move) Move(app, start);
            Move(app, {start.x + 1, start.y});
            Require(view.GetSearchCursorPosition() == cursor && view.GetSearchSelectionAnchor() == anchor &&
                surface->begins == draws && commits == 0 && popupInvalidations == 0 && rootInvalidations == 0 && backdropUpdates == 0,
                "R02 repeated same-glyph search selection must add zero damage, surface draws or commits");
            Move(app, {search.right + 100, start.y});
            Require(view.GetSearchCursorPosition() == view.GetSearchText().size() &&
                view.GetSearchSelectionAnchor() == anchor && view.GetSearchCompositionText() == L"\u5019\u9009" &&
                view.GetSearchCompositionCursor() == 1 && view.IsSearchPointerSelecting(),
                "R02 right-edge selection clamps to the text end while retaining anchor, composition and capture state");
            Require(rootInvalidations == 0 && surface->begins == draws + 1,
                "R02 changed text selection redraws the production popup once with no full desktop invalidation");
            Require(scene.singleCell ? drawnOwners.empty() : drawnOwners == std::vector<std::wstring>{expectedOwner},
                "R02 search selection updates exactly the visible source sharing that editing state");
            CheckPixels(app, *surface.Get());
            observation.Reset(); draws = surface->begins;
            Move(app, {search.right + 200, start.y});
            Require(surface->begins == draws && commits == 0 && popupInvalidations == 0 && backdropUpdates == 0,
                "R02 a second pointer position past the same text end does not redraw");
            Move(app, {search.left - 100, start.y});
            Require(view.GetSearchCursorPosition() == 0 && view.GetSearchSelectionAnchor() == anchor,
                "R02 reversing beyond the search left edge restores the literal start with the same anchor");
            CheckPixels(app, *surface.Get());
            RECT caret{};
            Require(view.GetSearchCaretRect(caret) && !IsRectEmpty(&caret),
                "R02 actual DirectWrite caret geometry remains available for IME positioning");
            Access::CancelPopupInput(app);
            Require(!view.IsSearchPointerSelecting() && view.GetSearchSelectionAnchor() == anchor,
                "R02 production cancellation retires pointer selection without changing its anchor");
            Access::SetPopupInputFullCollection(app, false);
            view.ClearSearchComposition();
            view.SetSearchEditingState(2, 2, L"", 0);
            view.BeginSearchPointerSelection(start, true);
            Require(view.GetSearchSelectionAnchor() == 2, "R02 Shift selection retains its independent middle anchor");
            Access::PaintPopupInput(app); observation.Reset();
            Move(app, {search.right + 100, start.y});
            Require(view.GetSearchSelectionAnchor() == 2 && view.GetSearchCursorPosition() == view.GetSearchText().size(),
                "R02 Shift drag extends from the retained middle anchor through the actual input route");
            CheckPixels(app, *surface.Get());
            Access::CancelPopupInput(app);
            view.ClearSearchText();
            surface = Access::ObservePopupInputSurface(app, surface.Get());
            Access::PaintPopupInput(app);
            Access::SyncPopupInputFrame(app, view);
            Require(view.BeginCategoryTabDrag(Tab(view, L"a")), "R02 begins a final tab gesture before closing");
            Access::SetPopupInputFullCollection(app, !scene.singleCell);
            observation.Reset(); draws = surface->begins;
            Move(app, Tab(view, L"b"));
            if (!scene.singleCell)
                Require(rootInvalidations > 0 && drawnOwners.empty() && surface->begins == draws + 1,
                    "R02 pending complete collection retains the root recovery route without prematurely drawing a child");
            observation.Reset();
            draws = surface->begins;
            Move(app, Tab(view, L"a"));
            Require(rootInvalidations == 0 && drawnOwners.empty() && surface->begins == draws && commits == 0 && backdropUpdates == 0,
                "R02 an unchanged tab adds no work even while a complete recovery remains pending");
            Require(Access::FinalizePopupInput(app) && rootInvalidations > 0 &&
                Access::PopupInputSource(app).categoryTabOrder.empty(),
                "R02 popup finalization restores original source order and retains full desktop recovery");
            if (!scene.dockCopy)
                Require(!view.IsPopupHosted() && !view.HasCategoryTabPress() &&
                    !view.IsSearchPointerSelecting() && !view.IsSearchFocused(),
                    "R02 finalization restores the surviving desktop view and releases categorized input state");
            // Dock copy finalization destroys its temporary view; inspect only
            // the surviving source/model and the production recovery request.
        }
        catch (...) { cleanup(); throw; }
        cleanup();
    }
    std::cout << "R02 production popup input, actual surface/commit counts, complete reference pixels, hits and cancellation passed\n";
}
}
