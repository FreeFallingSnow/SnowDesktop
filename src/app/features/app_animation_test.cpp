#include "app/features/app_animation_test.h"

#include "app/app.h"
#include "app/navigation/quick_navigation_state.h"
#include "common/l10n.h"
#include "common/utils.h"
#include "core/dock.h"
#include "core/slot.h"
#include "data/data_paths.h"
#include "diagnostics/performance_capture.h"
#include "dock/dock_window_transition.h"
#include "platform/control/control_protocol.h"
#include "settings/animation_settings.h"
#include "ui/input/private_pointer.h"
#include "widget/preview/widget_preview_scene.h"
#include "widget/runtime/widget_engine.h"

#include <cwchar>
#include <fstream>
#include <shellapi.h>

namespace
{
namespace capture = snowdesktop::performance;
namespace json = snowdesktop::control;
namespace motion = snowdesktop::animation;

enum class Route { DockHover, Collection, Fan, FanAll, Resize, LuaPanel, NavDock, NavPointer,
    Fade, Roll, Window };

struct Scenario
{
    std::string_view id;
    std::string_view surface;
    Route route;
    int effect = motion::Scale;
};

constexpr Scenario kScenarios[]{
    {"dock-hover", "floating-dock", Route::DockHover, 2},
    {"dock-hover-single", "floating-dock", Route::DockHover, 1},
    {"popup-scale", "popup", Route::Collection},
    {"popup-fade", "popup", Route::Collection, motion::Fade},
    {"popup-resize", "popup", Route::Resize},
    {"popup-fan", "popup", Route::Fan},
    {"popup-fan-show-all", "popup", Route::FanAll},
    {"popup-lua", "popup", Route::LuaPanel},
    {"navigation-dock", "quick-navigation", Route::NavDock, 1},
    {"navigation-dock-fade", "quick-navigation", Route::NavDock, 2},
    {"navigation-dock-genie", "quick-navigation", Route::NavDock, 3},
    {"navigation-dock-attraction", "quick-navigation", Route::NavDock, 4},
    {"navigation-pointer", "quick-navigation", Route::NavPointer},
    {"navigation-pointer-fade", "quick-navigation", Route::NavPointer, motion::Fade},
    {"widget-fade", "desktop", Route::Fade},
    {"widget-roll", "desktop", Route::Roll},
    {"window-system", "window-transition", Route::Window, 0},
    {"window-scale", "window-transition", Route::Window, 1},
    {"window-fade", "window-transition", Route::Window, 2},
    {"window-genie", "window-transition", Route::Window, 3},
    {"window-attraction", "window-transition", Route::Window, 4},
};
constexpr wchar_t kFixtureClass[] = L"SnowDesktopAnimationFixture";
constexpr wchar_t kCanvasClass[] = L"SnowDesktopAnimationCanvas";
constexpr POINT kOutside{1, 1};

POINT Center(const RECT& rect)
{
    return {(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2};
}

void Require(bool ok, const char* detail)
{
    if (!ok) throw std::runtime_error(detail);
}

bool WriteJson(const std::filesystem::path& path, const json::Value& value)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << json::Serialize(value) << '\n';
    output.close();
    return static_cast<bool>(output);
}

double ClockMs()
{
    return snowdesktop::UiAnimationScheduler::MonotonicMilliseconds();
}
}

struct NativeAnimationTest
{
    HINSTANCE instance;
    std::filesystem::path root;
    DesktopApp app;
    HWND frame = nullptr;
    HWND source = nullptr;
    DockContainer* dock = nullptr;
    snowdesktop::WidgetPreviewScene scene;
    std::unique_ptr<DockWindowTransition> transition;
    snowdesktop::private_pointer::Device input;
    RECT client{};

    NativeAnimationTest(HINSTANCE instance, std::filesystem::path root) : instance(instance), root(std::move(root)) {}
    ~NativeAnimationTest()
    {
        capture::Shutdown();
        transition.reset();
        app.CloseQuickNavigation(false);
        app.DestroyFloatingPopupWindow();
        input.End();
        app.uiAnimationScheduler_.Shutdown();
        if (source) DestroyWindow(source);
        if (app.hwnd_) DestroyWindow(std::exchange(app.hwnd_, nullptr));
        if (frame) DestroyWindow(frame);
    }

    static LRESULT CALLBACK CanvasProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
    {
        if (message == WM_NCCREATE)
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(
                reinterpret_cast<const CREATESTRUCTW*>(lp)->lpCreateParams));
        if (message == WM_ERASEBKGND) return 1;
        if (message == WM_PAINT)
        {
            auto* owner = reinterpret_cast<NativeAnimationTest*>(GetWindowLongPtrW(window, GWLP_USERDATA));
            RECT dirty{};
            if (GetUpdateRect(window, &dirty, FALSE)) ValidateRect(window, &dirty);
            // Paint after dispatch returns, just as a normal host does
            // when a pending render could reenter a composition transaction.
            if (owner && !IsRectEmpty(&dirty))
                UnionRect(&owner->damage, &owner->damage, &dirty);
            return 0;
        }
        return DefWindowProcW(window, message, wp, lp);
    }

    RECT damage{};

    void Register(const wchar_t* name, WNDPROC procedure, UINT style = 0)
    {
        WNDCLASSEXW window{};
        window.cbSize = sizeof(window);
        window.hInstance = instance;
        window.lpszClassName = name;
        window.lpfnWndProc = procedure;
        window.style = style;
        window.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        Require(RegisterClassExW(&window) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS,
            "register animation fixture window");
    }

    void Prepare()
    {
        const auto language = std::filesystem::path(GetExecutableDirectoryPath()) / L"lang";
        Locale::Instance().Init(language.c_str());
        Locale::Instance().SetLanguage("en-US");
        app.instance_ = instance;
        app.generalSettings_.dockEnabled = true;
        app.generalSettings_.widgetHoverVisibilityEffect = motion::Fade;
        app.generalSettings_.widgetHoverExpandEffect = motion::Roll;
        app.personalizationSettings_ = PersonalizationSettings::DarkPreset();
        app.personalizationSettings_.glassEnabled = true;
        app.personalizationSettings_.scrollableTitleBarOnTop = true;
        app.collectionPopupAppearance_ = app.personalizationSettings_;
        app.collectionPopupGlassTheme_ = true;
        app.collectionPopupBlurRadius_ = app.personalizationSettings_.glassBlurRadius;
        app.dockSettings_.position = DockPosition::Bottom;
        app.dockSettings_.showRunningApps = false;
        app.dockSettings_.showWindowPreviews = false;
        app.dockSettings_.reserveScreenSpace = false;
        app.dockSettings_.showOnlyWhenSummoned = false;
        app.dockSettings_.showQuickSearchButton = true;
        app.dockSettings_.fullscreenPolicy = DockFullscreenPolicy::Allow;
        app.navigationSettings_.enabled = true;
        app.quickNavigation_->appsIndexed = true;
        app.customDesktopVisible_ = true;
        app.desktopStartupPresentationPending_ = false;
        app.lastMousePoint_ = kOutside;

        Register(kFixtureClass, DefWindowProcW);
        Register(kCanvasClass, CanvasProc);
        Register(kFloatingDockWindowClassName, DesktopApp::FloatingDockWndProc, CS_DBLCLKS);
        Register(kFloatingPopupWindowClassName, DesktopApp::FloatingPopupWndProc, CS_DBLCLKS);
        Register(kQuickNavigationWindowClassName, DesktopApp::QuickNavigationWndProc, CS_DBLCLKS);
        MONITORINFO info{sizeof(info)};
        Require(GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &info) != FALSE,
            "read fixture monitor");
        const auto& work = info.rcWork;
        app.virtualWidth_ = std::min(1120L, work.right - work.left);
        app.virtualHeight_ = std::min(760L, work.bottom - work.top);
        Require(app.virtualWidth_ >= 720 && app.virtualHeight_ >= 540, "fixture display is too small");
        app.virtualLeft_ = work.left + (work.right - work.left - app.virtualWidth_) / 2;
        app.virtualTop_ = work.top + (work.bottom - work.top - app.virtualHeight_) / 2;
        client = {0, 0, app.virtualWidth_, app.virtualHeight_};
        frame = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, kFixtureClass,
            L"SnowDesktop Animation Fixture", WS_POPUP, app.virtualLeft_, app.virtualTop_,
            app.virtualWidth_, app.virtualHeight_, nullptr, nullptr, instance, nullptr);
        Require(frame != nullptr, "create fixture window");
        app.hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, kCanvasClass, L"", WS_CHILD | WS_VISIBLE,
            0, 0, app.virtualWidth_, app.virtualHeight_, frame, nullptr, instance, this);
        Require(app.hwnd_ != nullptr, "create fixture composition host");
        Require(app.uiAnimationScheduler_.Initialize() && app.InitGraphics(), "initialize fixture graphics");
        Require(SUCCEEDED(app.dcompDevice_->CreateTargetForHwnd(app.hwnd_, FALSE, &app.dcompTarget_)) &&
            SUCCEEDED(app.dcompDevice_->CreateVisual(&app.dcompVisual_)) &&
            SUCCEEDED(app.dcompTarget_->SetRoot(app.dcompVisual_.Get())) &&
            SUCCEEDED(app.CreateOrResizeCompositionSurface()), "initialize fixture composition tree");
        Require(app.desktopBackdropCompositor_.Initialize(app.hwnd_), "initialize fixture backdrop");
        PrepareModel();
        const RECT screen{app.virtualLeft_, app.virtualTop_, app.virtualLeft_ + app.virtualWidth_,
            app.virtualTop_ + app.virtualHeight_};
        std::string inputError;
        Require(input.Begin(screen, {screen.left + 1, screen.top + 1},
            [this](POINT point) { return app.PrivatePointerWindowAt(point); },
            [this](HWND window) { return app.OwnsPrivatePointerWindow(window); }, inputError),
            "initialize fixture logical input");
        app.startupInitializationComplete_ = true;
        ShowWindow(frame, SW_SHOWNOACTIVATE);
        Require(app.SyncPersistentDockHosts(), "initialize fixture Dock");
        Require(app.OnPaint(nullptr), "paint the fixture");
    }

    void PrepareModel()
    {
        app.widgets_.reserve(5);
        app.layoutWorkArea_ = client;
        GridPage page;
        page.id = L"animation-fixture";
        page.bounds = page.workArea = page.visualWorkArea = client;
        page.dpiX = page.dpiY = 96;
        page.columns = 10;
        page.rows = 8;
        page.cellWidth = page.itemPitchWidth = kCellWidth;
        page.cellHeight = page.itemPitchHeight = kMinCellHeight;
        app.gridPages_.push_back(page);
        for (int i = 0; i < 18; ++i)
        {
            snowdesktop::WidgetPreviewItem item;
            item.key = L"fixture-item-" + std::to_wstring(i + 1);
            item.title = L"Fixture item " + std::to_wstring(i + 1);
            item.glyph = L"\uE8A5";
            item.backgroundRgb = 0x5476A5;
            scene.AddItem(std::move(item));
        }
        scene.PreparePlaceholderModels(64, false);
        for (const auto& sample : scene.Items())
        {
            const auto* model = scene.FindDesktopItem(sample.key);
            Require(model && model->iconBitmap, "create fixture icons");
            DesktopItem item;
            item.layoutKey = item.parsingName = sample.key;
            item.name = sample.title;
            item.iconBitmap = CopyBitmapToAlphaDib(model->iconBitmap, item.iconBitmapSize);
            Require(item.iconBitmap != nullptr, "copy fixture icon");
            item.iconState = IconState::FullQuality;
            app.items_.push_back(std::move(item));
        }
        app.RefreshDesktopItemIndexCache();

        const auto add = [&](const wchar_t* id, RECT bounds) -> DesktopWidget& {
            DesktopWidget widget;
            widget.id = id;
            widget.title = id;
            widget.type = DesktopWidgetType::Collection;
            widget.bounds = bounds;
            widget.gridCell.pageId = page.id;
            widget.gridSpan = {3, 3};
            widget.cellScale = 1;
            widget.showTitle = true;
            for (const auto& item : app.items_) widget.itemKeys.push_back(item.layoutKey);
            return app.widgets_.emplace_back(std::move(widget));
        };
        add(L"fixture-collection", {38, 48, 270, 268});
        auto& fan = add(L"fixture-fan", {});
        fan.fanPopup = true;
        fan.gridCell.pageId = kDockPageId;
        add(L"fixture-fade", {298, 48, 520, 268}).showOnHoverOnly = true;
        auto& roll = add(L"fixture-roll", {38, 310, 270, 513});
        roll.scrollContainerMode = true;
        roll.titleBarCollapsed = roll.titleBarExpandOnHover = true;

        for (auto& widget : app.widgets_)
        {
            if (widget.gridCell.pageId == kDockPageId) continue;
            auto item = CreateWidget(&widget, &app);
            auto* container = dynamic_cast<WidgetContainer*>(item.get());
            Require(container != nullptr, "create fixture collection");
            item.release();
            app.containers_.emplace_back(container);
        }
        DesktopWidget lua;
        lua.id = L"fixture-lua";
        lua.type = DesktopWidgetType::LuaScript;
        lua.bounds = {330, 320, 500, 470};
        lua.gridCell.pageId = page.id;
        lua.gridSpan = {2, 2};
        app.widgets_.push_back(std::move(lua));
        app.dockEntries_.push_back({DockEntryType::Collection, L"fixture-fan"});
        for (std::size_t i = 0; i < 5; ++i)
            app.dockEntries_.push_back({DockEntryType::DesktopItem, app.items_[i].layoutKey});
        auto model = std::make_unique<DockContainer>(&app, &app.dockEntries_, client);
        dock = model.get();
        app.containers_.push_back(std::move(model));
        app.EnsureNavTabOrder();
    }

    void PrepareLua()
    {
        if (app.widgetEngine_) return;
        const auto package = root / L"lua-fixture";
        std::filesystem::create_directory(package);
        std::ofstream manifest(package / L"widget.json", std::ios::binary);
        manifest << R"json({
    "schemaVersion": 2, "apiVersion": 2, "dataVersion": 1,
    "id": "750f25e7-1ce1-4b5e-93e5-fe439dee6517", "slug": "animation-fixture",
    "entry": "main.lua", "name": "Animation fixture", "version": "1.0.0",
    "author": "SnowDesktop", "license": "GPL-3.0-only",
    "minHostVersion": "1.0.14.0", "description": "Fixed panel for animation pacing",
    "permissions": []
})json";
        manifest.close();
        Require(static_cast<bool>(manifest), "write fixture Lua manifest");
        std::ofstream output(package / L"main.lua", std::ios::binary);
        output << R"lua(return {
    panel = function()
        return view.column({ padding = 16, children = {
            view.text({ text = "Fixed animation fixture", height = 32 }),
            view.text({ text = "Lua panel content", height = 32 })
        } })
    end
})lua";
        output.close();
        Require(static_cast<bool>(output), "write fixture Lua file");
        app.widgetEngine_ = std::make_unique<WidgetEngine>();
        Require(app.widgetEngine_->Init(app.d2dContext_.Get(), app.dwriteFactory_.Get()) &&
            app.widgetEngine_->EnsureWidgetDirectoryPreviewLoaded(L"fixture-lua", package),
            "load fixture Lua panel");
    }

    void Pump(double milliseconds)
    {
        const double until = ClockMs() + milliseconds;
        do
        {
            const HANDLE timer = app.uiAnimationScheduler_.WaitHandle();
            const auto remaining = static_cast<DWORD>(std::max(0.0, std::ceil(until - ClockMs())));
            const DWORD wait = MsgWaitForMultipleObjectsEx(timer ? 1 : 0, timer ? &timer : nullptr,
                remaining, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            Require(wait != WAIT_FAILED, "wait for fixture animation");
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                Require(message.message != WM_QUIT, "fixture closed while running");
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            app.uiAnimationScheduler_.DispatchDue();
            app.FlushPendingCompositionCommit();
            app.FlushPendingQuickNavigationCompositionCommit();
            if (!IsRectEmpty(&damage))
            {
                const RECT dirty = std::exchange(damage, RECT{});
                app.OnPaint(&dirty);
            }
        } while (ClockMs() < until);
    }

    POINT DockPoint(bool search)
    {
        RECT bounds = dock->GetSearchRect();
        if (!search)
        {
            bounds = {};
            for (const auto& slot : dock->GetSlots())
                if (const auto* entry = dynamic_cast<DockEntryItem*>(slot->GetItem());
                    entry && entry->GetReference() == L"fixture-fan")
                {
                    bounds = slot->GetBounds();
                    break;
                }
        }
        Require(!IsRectEmpty(&bounds), "resolve fixture Dock target");
        return Center(bounds);
    }

    void MoveDock(POINT point)
    {
        app.lastMousePoint_ = point;
        dock->GetElementVisualRect(dock->GetBounds(), point);
        app.RequestDockAnimationPresentation(*dock);
    }

    bool Exercise(const Scenario& scenario, int iteration, json::Value& actions)
    {
        using Source = QuickNavigationInvocationSource;
        const auto step = [&](const char* edge, const auto& start, const auto& confirm) {
            const double begin = ClockMs();
            const bool triggered = start();
            const double acknowledged = ClockMs();
            Pump(900);
            const bool confirmed = triggered && confirm();
            actions.array.push_back(json::Object({
                {"scenario", json::String(std::string(scenario.id))}, {"edge", json::String(edge)},
                {"iteration", json::Number(iteration)}, {"surface", json::String(std::string(scenario.surface))},
                {"startNs", json::Number(std::round(begin * 1e6))},
                {"ackNs", json::Number(std::round(acknowledged * 1e6))},
                {"endNs", json::Number(std::round(ClockMs() * 1e6))}, {"confirmed", json::Boolean(confirmed)}
            }));
            return confirmed;
        };

        const bool windowEffect = scenario.route == Route::Window || scenario.route == Route::NavDock;
        motion::SetRuntimePreferences(motion::AlwaysOn,
            windowEffect ? motion::Scale : scenario.effect,
            1, 0, false, false, windowEffect ? scenario.effect : 1);
        if (scenario.route == Route::DockHover)
        {
            app.dockSettings_.hoverEffect = scenario.effect;
            const bool enter = step("enter", [&] {
                MoveDock(DockPoint(false));
                return dock->IsMagnificationAnimating();
            }, [&] { return !dock->IsMagnificationAnimating(); });
            return step("leave", [&] {
                MoveDock(kOutside);
                return dock->IsMagnificationAnimating();
            }, [&] { return !dock->IsMagnificationAnimating(); }) && enter;
        }
        if (scenario.route == Route::NavDock || scenario.route == Route::NavPointer)
        {
            const bool fromDock = scenario.route == Route::NavDock;
            POINT point = fromDock ? DockPoint(true) : Center(client);
            point.x += app.virtualLeft_;
            point.y += app.virtualTop_;
            const bool open = step("open", [&] {
                app.OpenQuickNavigation(fromDock ? Source::DockSearch : Source::Pointer, point);
                return app.quickNavigation_->open && app.quickNavigation_->animation.IsAnimating() &&
                    (!fromDock || app.quickNavigationDockHost_);
            }, [&] { return app.quickNavigation_->open && !app.quickNavigation_->animation.IsAnimating(); });
            return step("close", [&] {
                app.CloseQuickNavigation(false);
                return app.quickNavigation_->animation.IsAnimating();
            }, [&] { return !app.quickNavigation_->open && !app.quickNavigation_->animation.IsAnimating(); }) && open;
        }
        if (scenario.route == Route::Fade || scenario.route == Route::Roll)
        {
            const bool roll = scenario.route == Route::Roll;
            const auto index = app.FindWidgetIndexById(roll ? L"fixture-roll" : L"fixture-fade");
            const auto& widget = app.widgets_.at(index);
            const auto move = [&](POINT point, bool visible) {
                app.lastMousePoint_ = point;
                if (roll) app.UpdateWidgetHoverExpansion(point);
                else app.SyncDesktopWidgetPresentationVisibility(widget, widget.bounds, visible, true);
                app.OnPaint(&widget.bounds);
                return roll ? app.IsWidgetHoverExpansionAnimating(widget) :
                    app.desktopWidgetHoverFadeFrameToken_ != 0;
            };
            const auto isVisible = [&] {
                if (roll) return app.GetWidgetExpansionProgress(widget) > 0.0f;
                const auto found = app.desktopWidgetCompositionItems_.find(widget.id);
                return found != app.desktopWidgetCompositionItems_.end() && found->second.visible;
            };
            const bool enter = step("enter", [&] { return move(Center(widget.bounds), true); }, isVisible);
            return step("leave", [&] { return move(kOutside, false); }, [&] { return !isVisible(); }) && enter;
        }
        if (scenario.route == Route::LuaPanel)
        {
            PrepareLua();
            LuaWidgetPanelRequest request;
            request.widgetId = L"fixture-lua";
            request.title = L"Lua fixture panel";
            request.width = 420;
            request.height = 270;
            const bool open = step("open", [&] {
                app.OpenLuaWidgetPanel(request);
                return app.luaWidgetPanelAnimation_.IsAnimating();
            }, [&] { return app.luaWidgetPanelAnimation_.IsInteractive() && !app.luaWidgetPanelAnimation_.IsAnimating(); });
            return step("close", [&] {
                app.CloseLuaWidgetPanel();
                return app.luaWidgetPanelAnimation_.IsAnimating();
            }, [&] { return app.luaWidgetPanelRequest_.widgetId.empty(); }) && open;
        }
        if (scenario.route == Route::Window)
        {
            if (!source)
            {
                source = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kFixtureClass, L"Animation fixture source",
                    WS_OVERLAPPEDWINDOW, app.virtualLeft_ + 95, app.virtualTop_ + 80, 420, 270,
                    frame, nullptr, instance, nullptr);
                Require(source != nullptr, "create owned minimize source");
                transition = std::make_unique<DockWindowTransition>();
                Require(transition->Initialize(instance, &app.uiAnimationScheduler_, app.d2dDevice_.Get(),
                    app.dcompDevice_.Get()), "initialize window transition");
            }
            ShowWindow(source, SW_SHOWNOACTIVATE);
            Pump(120);
            RECT anchor = dock->GetSearchRect();
            OffsetRect(&anchor, app.virtualLeft_, app.virtualTop_);
            const bool native = scenario.effect == 0;
            const bool minimize = step("minimize", [&] {
                if (native) { ShowWindow(source, SW_SHOWMINNOACTIVE); return true; }
                return transition->StartMinimize(source, anchor) && transition->IsActiveFor(source);
            }, [&] { return IsIconic(source) && !transition->IsActive(); });
            const bool restore = step("restore", [&] {
                if (native) { ShowWindow(source, SW_SHOWNOACTIVATE); return true; }
                return transition->StartRestore(source, anchor, [](HWND window, DockWindowRestoreTransitionPhase phase) {
                    if (phase != DockWindowRestoreTransitionPhase::ActivateRestored)
                        ShowWindow(window, SW_SHOWNOACTIVATE);
                }) && transition->IsActiveFor(source);
            }, [&] { return !IsIconic(source) && !transition->IsActive(); });
            ShowWindow(source, SW_HIDE);
            return minimize && restore;
        }

        const bool fan = scenario.route == Route::Fan || scenario.route == Route::FanAll;
        const auto index = app.FindWidgetIndexById(fan ? L"fixture-fan" : L"fixture-collection");
        auto& widget = app.widgets_.at(index);
        bool ok = step("open", [&] {
            app.OpenCollectionPopupAt(index, fan ? DockPoint(false) : Center(widget.bounds));
            return app.GetOpenPopupWidget() && app.popupAnimation_.IsAnimating() &&
                app.UsesCollectionPopupFan(*app.GetOpenPopupWidget()) == fan;
        }, [&] { return app.IsCollectionPopupInteractive() && !app.popupAnimation_.IsAnimating(); });
        if (ok && scenario.route == Route::FanAll)
            ok = step("show-all", [&] {
                app.ShowAllCollectionPopupItems();
                return app.popupFanShowAll_ && app.popupAnimation_.IsAnimating();
            }, [&] { return app.IsCollectionPopupInteractive() && !app.popupAnimation_.IsAnimating(); });
        if (ok && scenario.route == Route::Resize)
        {
            for (const bool list : {true, false})
            {
                ok = step(list ? "resize-list" : "resize-grid", [&] {
                    const auto before = app.GetPresentedCollectionPopupGeometry();
                    widget.listMode = list;
                    widget.listColumns = 1;
                    app.StartFolderPreviewPopupResize(before);
                    return app.GetCollectionPopupResizeTransition() != nullptr;
                }, [&] { return app.GetCollectionPopupResizeTransition() == nullptr; }) && ok;
            }
        }
        return step("close", [&] {
            app.CloseCollectionPopup();
            return app.popupAnimation_.IsAnimating();
        }, [&] { return app.GetOpenPopupWidget() == nullptr && !app.popupAnimation_.IsAnimating(); }) && ok;
    }
};

namespace snowdesktop::animation_test
{
std::optional<int> TryRun(HINSTANCE instance)
{
    int argc = 0;
    auto* args = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!args) return std::nullopt;
    const std::unique_ptr<wchar_t*, decltype(&LocalFree)> arguments(args, &LocalFree);
    if (argc < 2 || std::wstring_view(args[1]) != L"--animation-test") return std::nullopt;
    if (argc < 5 || (std::wstring_view(args[2]) != L"plan" && std::wstring_view(args[2]) != L"run"))
        return ERROR_INVALID_PARAMETER;

    const bool planOnly = std::wstring_view(args[2]) == L"plan";
    const std::filesystem::path output = args[3];
    if (!output.is_absolute()) return ERROR_INVALID_PARAMETER;
    wchar_t* end = nullptr;
    const long repeats = std::wcstol(args[4], &end, 10);
    if (!end || *end || repeats < 1 || repeats > 8) return ERROR_INVALID_PARAMETER;
    std::vector<const Scenario*> selected;
    for (int i = 5; i < argc; ++i)
    {
        const std::string id = WideToUtf8(args[i]);
        const auto match = std::ranges::find(kScenarios, id, &Scenario::id);
        if (match == std::end(kScenarios) || std::ranges::find(selected, &*match) != selected.end())
            return ERROR_INVALID_PARAMETER;
        selected.push_back(&*match);
    }
    if (selected.empty())
        for (const auto& scenario : kScenarios) selected.push_back(&scenario);

    std::error_code fsError;
    std::filesystem::create_directories(output.parent_path(), fsError);
    if (fsError || !std::filesystem::create_directory(output, fsError)) return ERROR_CANNOT_MAKE;
    auto planned = json::Array();
    for (const auto* scenario : selected)
        planned.array.push_back(json::Object({
            {"id", json::String(std::string(scenario->id))},
            {"surface", json::String(std::string(scenario->surface))},
            {"effect", json::Number(scenario->effect)}
        }));
    const auto plan = json::Object({
        {"schemaVersion", json::Number(1)}, {"fixtureVersion", json::Number(1)},
        {"repeats", json::Number(static_cast<double>(repeats))}, {"scenarios", std::move(planned)}
    });
    if (!WriteJson(output / L"plan.json", plan)) return ERROR_WRITE_FAULT;
    if (planOnly) return 0;

    const auto fixture = output / L"fixture-data";
    if (!std::filesystem::create_directory(fixture, fsError)) return ERROR_CANNOT_MAKE;
    snowdesktop::data_paths::SetIsolatedAuthoringDataRoot(fixture.wstring());
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT com = OleInitialize(nullptr);
    if (FAILED(com)) return ERROR_GEN_FAILURE;
    struct OleScope { ~OleScope() { OleUninitialize(); } } ole;

    auto actions = json::Array();
    std::string error;
    std::string tracePhase;
    bool result = true;
    {
        NativeAnimationTest test(instance, fixture);
        capture::CaptureOptions options;
        options.session = options.requestId = "animation-test";
        options.output = output / L"trace.pftrace";
        options.preset = capture::CapturePreset::Interaction;
        options.seconds = static_cast<unsigned>(std::min<std::size_t>(
            600, 15 + selected.size() * static_cast<std::size_t>(repeats) * 5));
        options.bufferKiB = 65536;
        options.scopeCpu = true;
        options.modules = {"dock", "frame", "popup", "render"};
        try
        {
            test.Prepare();
            Require(capture::Start(options, error), "start animation capture");
            for (int retry = 0; retry < 30 && capture::Snapshot(options.session).phase == capture::CapturePhase::Starting; ++retry)
                test.Pump(50);
            Require(capture::Snapshot(options.session).phase == capture::CapturePhase::Recording,
                "animation capture did not start");
            for (const auto* scenario : selected)
                for (int iteration = 1; iteration <= repeats; ++iteration)
                    result = test.Exercise(*scenario, iteration, actions) && result;
        }
        catch (const std::exception& failure)
        {
            result = false;
            if (error.empty()) error = failure.what();
        }
        capture::RequestStop(options.session);
        for (int retry = 0; retry < 100; ++retry)
        {
            const auto status = capture::Snapshot(options.session);
            if (status.phase == capture::CapturePhase::Completed || status.phase == capture::CapturePhase::Failed || !status.found)
                break;
            Sleep(50);
        }
        const auto status = capture::Snapshot(options.session);
        tracePhase = status.phase == capture::CapturePhase::Completed ? "completed" : "incomplete";
        if (status.phase != capture::CapturePhase::Completed)
        {
            result = false;
            if (error.empty()) error = status.error.empty() ? "animation trace did not complete" : status.error;
        }
    }
    const auto report = json::Object({
        {"schemaVersion", json::Number(1)}, {"session", json::String("animation-test")},
        {"processId", json::Number(GetCurrentProcessId())}, {"ok", json::Boolean(result)},
        {"trace", json::String(std::move(tracePhase))}, {"error", json::String(std::move(error))},
        {"actions", std::move(actions)}
    });
    if (!WriteJson(output / L"actions.json", report)) return ERROR_WRITE_FAULT;
    return result ? 0 : ERROR_GEN_FAILURE;
}
}
