#include "widget/runtime/widget_engine.h"
#include "widget/runtime/widget_lua_binding_context.h"
#include "widget/view/widget_render_host_context.h"
#include "widget/api/widget_api_registry.h"
#include "widget/packages/widget_package.h"
#include "widget/data/widget_data_subscription_lua.h"
#include "widget/data/widget_resource_lua.h"
#include "widget/data/widget_system_lua.h"
#include "widget/data/widget_gpu_lua.h"
#include "widget/runtime/widget_calendar_lua.h"
#include "widget/runtime/widget_desktop_host.h"
#include "widget/runtime/widget_logical_slot_lua.h"
#include "widget/runtime/widget_l10n_lua.h"
#include "widget/runtime/widget_module_lua.h"
#include "widget/runtime/widget_schedule_lua.h"
#include "widget/runtime/widget_time_lua.h"
#include "widget/storage/widget_storage_lua.h"
#include "widget/tasks/widget_task_start_lua.h"
#include "widget/view/widget_control_lua.h"
#include "widget/view/widget_draw_lua.h"
#include "widget/view/widget_layout_lua.h"
#include "widget/view/widget_resource_host.h"
#include "widget/view/widget_surface_host.h"
#include "widget/view/widget_surface_interaction.h"
#include "widget/view/widget_view_lua.h"
#include "common/utils.h"

using snowdesktop::widget_runtime::BoundWidgetId;
using snowdesktop::widget_l10n::PushWidgetL10nAPI;

static int lua_GetTime(lua_State* L)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    lua_createtable(L, 0, 7);
    lua_pushinteger(L, st.wYear);   lua_setfield(L, -2, "year");
    lua_pushinteger(L, st.wMonth);  lua_setfield(L, -2, "month");
    lua_pushinteger(L, st.wDay);    lua_setfield(L, -2, "day");
    lua_pushinteger(L, st.wDayOfWeek + 1); lua_setfield(L, -2, "wday");
    lua_pushinteger(L, st.wHour);   lua_setfield(L, -2, "hour");
    lua_pushinteger(L, st.wMinute); lua_setfield(L, -2, "min");
    lua_pushinteger(L, st.wSecond); lua_setfield(L, -2, "sec");
    return 1;
}

static int lua_Notify(lua_State* L)
{
    const char* title = luaL_checkstring(L, 1);
    const char* message = luaL_checkstring(L, 2);
    auto* s = GetD2D(L);
    if (s && s->engine)
    {
        const std::wstring widgetId = BoundWidgetId(L);
        if (!s->engine->RuntimeHasPermission(widgetId, "ui.notify"))
            return luaL_error(L, "widget permission denied: ui.notify");
        s->engine->RuntimeNotify(widgetId, Utf8ToWide(title),
            Utf8ToWide(message));
    }
    return 0;
}

static int LuaReadOnlyApiWrite(lua_State* state)
{
    return luaL_error(state, "SnowDesktop host API tables are read-only");
}

static void PushReadOnlyProxyForTopTable(lua_State* state)
{
    // Stack before: [..., source]. Stack after: [..., proxy].
    lua_newtable(state);
    lua_newtable(state);
    lua_pushvalue(state, -3);
    lua_setfield(state, -2, "__index");
    lua_pushcfunction(state, LuaReadOnlyApiWrite);
    lua_setfield(state, -2, "__newindex");
    lua_pushboolean(state, 0);
    lua_setfield(state, -2, "__metatable");
    lua_setmetatable(state, -2);
    lua_remove(state, -2);
}

static void PushReadOnlyGlobal(lua_State* state, const char* name)
{
    lua_getglobal(state, name);
    PushReadOnlyProxyForTopTable(state);
}

void WidgetEngine::PushSafeEnvironment(lua_State* L, const LuaWidget& widget)
{
    static const char* funcs[] = {
        "assert", "error", "ipairs", "next", "pairs", "pcall", "select",
        "tonumber", "tostring", "type", "xpcall"
    };

    lua_newtable(L);
    for (const char* name : funcs)
    {
        lua_getglobal(L, name);
        lua_setfield(L, -2, name);
    }
    for (const std::string_view name :
        snowdesktop::widget_api::SandboxLibraries())
    {
        if (name == "l10n") continue;
        const std::string libraryName(name);
        PushReadOnlyGlobal(L, libraryName.c_str());
        lua_setfield(L, -2, libraryName.c_str());
    }
    PushWidgetL10nAPI(L, widget.manifest.locales);
    PushReadOnlyProxyForTopTable(L);
    lua_setfield(L, -2, "l10n");

    lua_pushstring(L, WideToUtf8(widget.widgetId).c_str());
    lua_setfield(L, -2, "widgetId");
}

void WidgetEngine::RegisterDrawAPI(lua_State* L)
{
    using snowdesktop::widget_api::CatalogFunctionDescriptor;
    using snowdesktop::widget_api::RegisterFunctionCatalog;
    using snowdesktop::widget_runtime::lua_DrawText;
    using snowdesktop::widget_runtime::lua_MeasureText;
    using snowdesktop::widget_runtime::lua_DrawRect;
    using snowdesktop::widget_runtime::lua_DrawPushClip;
    using snowdesktop::widget_runtime::lua_DrawPopClip;
    using snowdesktop::widget_runtime::lua_DrawStrokeRect;
    using snowdesktop::widget_runtime::lua_DrawMarqueeText;
    using snowdesktop::widget_runtime::lua_DrawArc;
    using snowdesktop::widget_runtime::lua_DrawPath;
    using snowdesktop::widget_runtime::lua_DrawGradientRect;
    using snowdesktop::widget_runtime::lua_DrawShadow;
    using snowdesktop::widget_runtime::lua_DrawSparkline;
    using snowdesktop::widget_runtime::lua_DrawImage;
    using snowdesktop::widget_runtime::lua_DrawImageFit;
    using snowdesktop::widget_runtime::lua_DrawLine;
    using snowdesktop::widget_runtime::lua_DrawCircle;
    using snowdesktop::widget_runtime::lua_DrawFa;
    using snowdesktop::widget_runtime::lua_DrawFluent;
    using snowdesktop::widget_runtime::lua_DrawGlyph;
    using snowdesktop::widget_runtime::lua_ResourceExists;
    using snowdesktop::widget_runtime::lua_ResourceAudio;

    using snowdesktop::widget_runtime::lua_LayoutWidth;
    using snowdesktop::widget_runtime::lua_LayoutHeight;
    using snowdesktop::widget_runtime::lua_LayoutColumns;
    using snowdesktop::widget_runtime::lua_LayoutRows;
    using snowdesktop::widget_runtime::lua_LayoutSizeClass;
    using snowdesktop::widget_runtime::lua_LayoutCellWidth;
    using snowdesktop::widget_runtime::lua_LayoutCellHeight;
    using snowdesktop::widget_runtime::lua_LayoutCellScale;
    using snowdesktop::widget_runtime::lua_LayoutCu;
    using snowdesktop::widget_runtime::lua_LayoutFontCu;
    using snowdesktop::widget_runtime::lua_LayoutCellGap;
    using snowdesktop::widget_runtime::lua_LayoutBarHeight;
    using snowdesktop::widget_runtime::lua_LayoutContentWidth;
    using snowdesktop::widget_runtime::lua_LayoutContentHeight;
    using snowdesktop::widget_runtime::lua_LayoutVw;
    using snowdesktop::widget_runtime::lua_LayoutVh;
    using snowdesktop::widget_runtime::lua_LayoutVmin;
    using snowdesktop::widget_runtime::lua_LayoutVmax;
    using snowdesktop::widget_runtime::lua_LayoutRpx;
    using snowdesktop::widget_runtime::lua_LayoutRpxX;
    using snowdesktop::widget_runtime::lua_LayoutRpxY;
    using snowdesktop::widget_runtime::lua_StorageGet;
    using snowdesktop::widget_runtime::lua_StorageSet;
    using snowdesktop::widget_runtime::lua_StorageRemove;
    using snowdesktop::widget_runtime::lua_StorageKeys;
    using snowdesktop::widget_runtime::lua_StorageTransaction;
    using snowdesktop::widget_runtime::RegisterDataSubscriptionHandle;
    using snowdesktop::widget_runtime::lua_DataSubscribe;
    using snowdesktop::widget_runtime::lua_SystemCpu;
    using snowdesktop::widget_runtime::lua_SystemMemory;
    using snowdesktop::widget_runtime::lua_SystemBattery;
    using snowdesktop::widget_runtime::lua_SystemNetwork;
    using snowdesktop::widget_runtime::lua_SystemGpu;
    using snowdesktop::widget_runtime::lua_MediaCurrent;
    using snowdesktop::widget_runtime::lua_MediaPlayPause;
    using snowdesktop::widget_runtime::lua_MediaNext;
    using snowdesktop::widget_runtime::lua_MediaPrevious;
    using snowdesktop::widget_runtime::lua_SystemInfoV2;
    using snowdesktop::widget_runtime::lua_SystemUptime;
    using snowdesktop::widget_runtime::lua_TimeNow;
    using snowdesktop::widget_runtime::lua_TimeMonotonic;
    using snowdesktop::widget_runtime::lua_TimeParts;
    using snowdesktop::widget_runtime::lua_TimeFormat;
    using snowdesktop::widget_runtime::lua_TimeAdd;
    using snowdesktop::widget_runtime::lua_TimeCompare;
    using snowdesktop::widget_runtime::lua_ScheduleAt;
    using snowdesktop::widget_runtime::lua_ScheduleTimeline;
    using snowdesktop::widget_runtime::lua_ScheduleEvery;
    using snowdesktop::widget_runtime::lua_ScheduleAfter;
    using snowdesktop::widget_runtime::lua_ScheduleCancel;
    using snowdesktop::widget_runtime::lua_AnimationRequestFrame;
    using snowdesktop::widget_runtime::lua_AnimationCancelFrame;
    using snowdesktop::widget_runtime::lua_CalendarSelectedDate;
    using snowdesktop::widget_runtime::lua_CalendarSetSelectedDate;
    using snowdesktop::widget_runtime::lua_CalendarSelectDate;
    using snowdesktop::widget_runtime::lua_CalendarPreferences;
    using snowdesktop::widget_runtime::lua_CalendarDisplayOptions;
    using snowdesktop::widget_runtime::lua_CalendarAnnotations;
    using snowdesktop::widget_runtime::lua_CalendarDateInfo;
    using snowdesktop::widget_runtime::lua_CalendarAddDays;
    using snowdesktop::widget_runtime::lua_CalendarEvents;
    using snowdesktop::widget_runtime::lua_CalendarSeriesById;
    using snowdesktop::widget_runtime::lua_CalendarCreate;
    using snowdesktop::widget_runtime::lua_CalendarUpdate;
    using snowdesktop::widget_runtime::lua_CalendarRemove;
    using snowdesktop::widget_runtime::lua_SlotsBinding;
    using snowdesktop::widget_runtime::lua_SlotsCollection;
    using snowdesktop::widget_runtime::lua_SlotsCanUndo;
    using snowdesktop::widget_runtime::lua_SlotsCanRedo;
    using snowdesktop::widget_runtime::lua_SlotsUndo;
    using snowdesktop::widget_runtime::lua_SlotsRedo;
    using snowdesktop::widget_runtime::lua_ModuleRequire;
    using snowdesktop::widget_runtime::lua_UiTextInput;
    using snowdesktop::widget_runtime::lua_UiTextArea;
    using snowdesktop::widget_runtime::lua_UiFocusInput;
    using snowdesktop::widget_runtime::lua_ControlTextInput;
    using snowdesktop::widget_runtime::lua_ControlTextArea;
    using snowdesktop::widget_runtime::lua_ControlFocus;
    using snowdesktop::widget_runtime::lua_ControlBlur;
    using snowdesktop::widget_runtime::lua_UiButton;
    using snowdesktop::widget_runtime::lua_UiToggle;
    using snowdesktop::widget_runtime::lua_UiProgress;
    using snowdesktop::widget_runtime::lua_UiScrollArea;
    using snowdesktop::widget_runtime::lua_UiVirtualList;
    using snowdesktop::widget_runtime::lua_UiSetScrollOffset;
    using snowdesktop::widget_runtime::lua_TaskStart;
    using snowdesktop::widget_runtime::lua_TaskCancel;
    using snowdesktop::widget_runtime::lua_HttpRequest;
    using snowdesktop::widget_runtime::lua_HttpCancel;
    static constexpr CatalogFunctionDescriptor functions[] = {
#define SNOW_WIDGET_PUBLIC_FUNCTION(                                      \
    library, name, callback, sinceApi, permission, untilApi)              \
    { library, { name, callback, sinceApi, permission, untilApi } },
#define SNOW_WIDGET_MANIFEST_FUNCTION(                                    \
    library, name, callback, sinceApi, permission, untilApi)
#include "widget/api/widget_public_api.inc"
#undef SNOW_WIDGET_MANIFEST_FUNCTION
#undef SNOW_WIDGET_PUBLIC_FUNCTION
    };

    static constexpr snowdesktop::widget_runtime::WidgetDataLuaImages dataImages{
        +[](lua_State* state, std::string_view token,
            const std::shared_ptr<const snowdesktop::widget_runtime::WidgetRuntimeImagePixels>& pixels,
            const char* source, bool preserveExisting) {
            auto* d2d = GetD2D(state);
            return d2d ? d2d->runtimeImageCache.Register(BoundWidgetId(state),
                token, pixels, source, preserveExisting) : std::string{};
        },
        +[](lua_State* state, const snowdesktop::widget_runtime::WidgetScreenTimeDataSnapshot& screenTime) {
            auto* d2d = GetD2D(state);
            const auto owner = BoundWidgetId(state);
            if (!d2d || owner.empty()) return;
            std::vector<std::string_view> tokens;
            tokens.reserve(screenTime.apps.size());
            for (const auto& app : screenTime.apps)
                tokens.push_back(app.iconToken);
            d2d->runtimeImageCache.Retain(owner, "screen-time", tokens);
        },
    };
    snowdesktop::widget_runtime::BindWidgetDataLuaImages(L, dataImages);
    RegisterDataSubscriptionHandle(L);
    RegisterFunctionCatalog(L, functions,
        static_cast<std::uint32_t>(
            snowdesktop::widget::kHostApiVersion));
}
