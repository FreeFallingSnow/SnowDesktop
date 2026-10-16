// Included at the engine binding boundary and by the native callback regression.
// Every owning value survives outside the inner pcall, including error paths.
struct LuaDataSubscribeContext
{
    std::string topicValue, rangeStart, rangeEnd, eventId, scopeHandle;
    std::string calendarQueryError, feature;
    std::unordered_set<std::string> seen;
    std::optional<std::string> maximumEnd;
    std::optional<snowdesktop::widget_runtime::CalendarEventQueryOptions> calendarQuery;
    snowdesktop::widget_runtime::DataSubscriptionResult result;
    bool nativeFailure = false;
};

// Lua errors use longjmp. Keep nontrivial C++ temporaries out of the parsing
// frame: MSVC /EHsc can otherwise run their cleanup before construction when
// unwinding an extern-C Lua error. These helpers must not inline into it.
static __declspec(noinline) void LuaDataSubscribeMaximumEnd(LuaDataSubscribeContext& context)
{
    context.maximumEnd = snowdesktop::calendar::CalendarService::
        AddDays(context.rangeStart, 365);
}

template <typename Host>
static __declspec(noinline) void LuaDataSubscribeRegister(lua_State* state,
    Host& host, LuaDataSubscribeContext& context, lua_Integer maxAgeMs,
    snowdesktop::widget_runtime::DataHiddenPolicy hiddenPolicy,
    snowdesktop::widget_runtime::WidgetAudioAnalysisConfiguration audioAnalysis)
{
    // Resolve Lua state before constructing the native call's by-value
    // arguments. No Lua API operation follows the owning widget ID here.
    const auto widgetId = BoundWidgetId(state);
    context.result = host.RuntimeSubscribeData(
        widgetId, context.topicValue, std::chrono::milliseconds(maxAgeMs), hiddenPolicy,
        std::move(context.rangeStart), std::move(context.rangeEnd),
        std::move(context.scopeHandle), audioAnalysis, std::move(context.eventId),
        std::move(context.calendarQuery));
}

static int LuaDataSubscribeBody(lua_State* state, LuaDataSubscribeContext& context)
{
    size_t topicLength = 0;
    const char* topic = luaL_checklstring(state, 1, &topicLength);
    if (topicLength == 0 || topicLength > 128)
        return luaL_error(state,
            "data.subscribe: topic must contain 1 to 128 bytes");
    if (lua_gettop(state) > 2)
        return luaL_error(state,
            "data.subscribe: expected topic and optional options");

    context.topicValue.assign(topic, topicLength);
    const auto& topicValue = context.topicValue;
    const bool audioAnalysisTopic =
        topicValue == "audio.output.analysis";
    lua_Integer maxAgeMs = audioAnalysisTopic ? 33 : 1000;
    bool maxAgeSpecified = false;
    bool includeGpuDetails = false;
    bool updateHzSpecified = false;
    bool waveformPointsSpecified = false;
    bool spectrumBinsSpecified = false;
    snowdesktop::widget_runtime::WidgetAudioAnalysisConfiguration
        audioAnalysis;
    auto& rangeStart = context.rangeStart;
    auto& rangeEnd = context.rangeEnd;
    auto& eventId = context.eventId;
    auto& scopeHandle = context.scopeHandle;
    auto& calendarQuery = context.calendarQuery;
    auto hiddenPolicy = topicValue == "filesystem.watch" ||
            audioAnalysisTopic
        ? snowdesktop::widget_runtime::DataHiddenPolicy::Pause
        : snowdesktop::widget_runtime::DataHiddenPolicy::Throttle;
    if (!lua_isnoneornil(state, 2))
    {
        luaL_checktype(state, 2, LUA_TTABLE);
        lua_pushnil(state);
        while (lua_next(state, 2) != 0)
        {
            if (lua_type(state, -2) != LUA_TSTRING)
                return luaL_error(state,
                    "data.subscribe: option keys must be strings");
            size_t keyLength = 0;
            const char* keyRaw = lua_tolstring(state, -2, &keyLength);
            const std::string_view key(keyRaw, keyLength);
            const bool common = key == "maxAgeMs" ||
                key == "whenHidden";
            const bool calendar = topicValue == "calendar.events" &&
                (key == "fromDate" || key == "toDate" || key == "eventId" ||
                    key == "query" || key == "offset" || key == "limit");
            const bool filesystem = topicValue == "filesystem.watch" &&
                key == "handle";
            const bool audio = audioAnalysisTopic &&
                (key == "features" || key == "updateHz" ||
                    key == "waveformPoints" || key == "spectrumBins");
            const bool gpu = topicValue == "system.gpu" && key == "includeDetails";
            if (!common && !calendar && !filesystem && !audio && !gpu)
            {
                return luaL_error(state,
                    "data.subscribe: option '%.*s' is not valid for topic '%s'",
                    static_cast<int>(keyLength), keyRaw,
                    topicValue.c_str());
            }
            lua_pop(state, 1);
        }
        lua_getfield(state, 2, "includeDetails");
        if (!lua_isnil(state, -1))
        {
            if (!lua_isboolean(state, -1))
                return luaL_error(state, "data.subscribe: includeDetails must be a boolean");
            includeGpuDetails = lua_toboolean(state, -1) != 0;
        }
        lua_pop(state, 1);
        lua_getfield(state, 2, "maxAgeMs");
        if (!lua_isnil(state, -1))
        {
            maxAgeSpecified = true;
            int isInteger = 0;
            maxAgeMs = lua_tointegerx(state, -1, &isInteger);
            if (!isInteger || maxAgeMs <= 0 || maxAgeMs > 86400000)
                return luaL_error(state,
                    "data.subscribe: maxAgeMs must be an integer from 1 to 86400000");
        }
        lua_pop(state, 1);

        lua_getfield(state, 2, "whenHidden");
        if (!lua_isnil(state, -1))
        {
            size_t policyLength = 0;
            const char* policy = luaL_checklstring(
                state, -1, &policyLength);
            const std::string_view value(policy, policyLength);
            if (value == "pause")
                hiddenPolicy = snowdesktop::widget_runtime::DataHiddenPolicy::Pause;
            else if (value == "throttle")
                hiddenPolicy = snowdesktop::widget_runtime::DataHiddenPolicy::Throttle;
            else if (value == "continue")
                hiddenPolicy = snowdesktop::widget_runtime::DataHiddenPolicy::Continue;
            else
                return luaL_error(state,
                    "data.subscribe: whenHidden must be 'pause', 'throttle', or 'continue'");
            if (audioAnalysisTopic && value != "pause")
                return luaL_error(state,
                    "data.subscribe: audio.output.analysis requires whenHidden='pause'");
        }
        lua_pop(state, 1);

        const auto readDateOption = [&](const char* name,
                                        std::string& output) {
            lua_getfield(state, 2, name);
            if (!lua_isnil(state, -1))
            {
                size_t length = 0;
                const char* value = luaL_checklstring(
                    state, -1, &length);
                output.assign(value, length);
            }
            lua_pop(state, 1);
        };
        readDateOption("fromDate", rangeStart);
        readDateOption("toDate", rangeEnd);
        lua_getfield(state, 2, "eventId");
        if (!lua_isnil(state, -1))
        {
            if (lua_type(state, -1) != LUA_TSTRING)
                return luaL_error(state, "data.subscribe: eventId must be a string");
            size_t length = 0;
            const char* raw = lua_tolstring(state, -1, &length);
            eventId.assign(raw, length);
            if (length == 0 || length > 128 || eventId.find('\0') != std::string::npos)
                return luaL_error(state, "data.subscribe: eventId must contain 1 to 128 bytes without NUL");
            lua_getfield(state, 2, "fromDate");
            const bool hasStart = !lua_isnil(state, -1);
            lua_pop(state, 1);
            lua_getfield(state, 2, "toDate");
            const bool hasEnd = !lua_isnil(state, -1);
            lua_pop(state, 1);
            if (hasStart || hasEnd)
                return luaL_error(state, "data.subscribe: eventId and date range are mutually exclusive");
        }
        lua_pop(state, 1);

        lua_getfield(state, 2, "handle");
        if (!lua_isnil(state, -1))
        {
            size_t handleLength = 0;
            const char* handle = luaL_checklstring(
                state, -1, &handleLength);
            if (handleLength == 0 || handleLength > 128)
                return luaL_error(state,
                    "data.subscribe: handle must contain 1 to 128 bytes");
            scopeHandle.assign(handle, handleLength);
        }
        lua_pop(state, 1);

        if (audioAnalysisTopic)
        {
            lua_getfield(state, 2, "features");
            if (!lua_isnil(state, -1))
            {
                luaL_checktype(state, -1, LUA_TTABLE);
                const int featuresTable = lua_absindex(state, -1);
                if (lua_getmetatable(state, featuresTable) != 0)
                {
                    lua_pop(state, 1);
                    return luaL_error(state,
                        "data.subscribe: audio features must be a plain array");
                }
                const size_t featureCount = lua_rawlen(
                    state, featuresTable);
                if (featureCount == 0 || featureCount > 4)
                    return luaL_error(state,
                        "data.subscribe: audio features must contain 1 to 4 entries");
                size_t actualCount = 0;
                lua_pushnil(state);
                while (lua_next(state, featuresTable) != 0)
                {
                    if (!lua_isinteger(state, -2))
                        return luaL_error(state,
                            "data.subscribe: audio features must be a dense array");
                    const lua_Integer key = lua_tointeger(state, -2);
                    if (key < 1 || static_cast<size_t>(key) > featureCount)
                        return luaL_error(state,
                            "data.subscribe: audio features must be a dense array");
                    ++actualCount;
                    lua_pop(state, 1);
                }
                if (actualCount != featureCount)
                    return luaL_error(state,
                        "data.subscribe: audio features must be a dense array");

                audioAnalysis.waveform = false;
                audioAnalysis.spectrum = false;
                audioAnalysis.rms = false;
                audioAnalysis.peak = false;
                auto& seen = context.seen;
                for (size_t index = 1; index <= featureCount; ++index)
                {
                    lua_rawgeti(state, featuresTable,
                        static_cast<lua_Integer>(index));
                    size_t featureLength = 0;
                    const char* featureRaw = luaL_checklstring(
                        state, -1, &featureLength);
                    context.feature.assign(featureRaw, featureLength);
                    const auto& feature = context.feature;
                    lua_pop(state, 1);
                    if (!seen.insert(feature).second)
                        return luaL_error(state,
                            "data.subscribe: audio features must be unique");
                    if (feature == "waveform")
                        audioAnalysis.waveform = true;
                    else if (feature == "spectrum")
                        audioAnalysis.spectrum = true;
                    else if (feature == "rms")
                        audioAnalysis.rms = true;
                    else if (feature == "peak")
                        audioAnalysis.peak = true;
                    else
                        return luaL_error(state,
                            "data.subscribe: unsupported audio feature '%s'",
                            feature.c_str());
                }
            }
            lua_pop(state, 1);

            const auto readAudioInteger = [&](const char* field,
                lua_Integer minimum, lua_Integer maximum,
                std::size_t& output, bool& specified) {
                lua_getfield(state, 2, field);
                if (!lua_isnil(state, -1))
                {
                    specified = true;
                    if (!lua_isinteger(state, -1))
                        luaL_error(state,
                            "data.subscribe: %s must be an integer",
                            field);
                    const lua_Integer value = lua_tointeger(state, -1);
                    if (value < minimum || value > maximum)
                        luaL_error(state,
                            "data.subscribe: %s must be between %lld and %lld",
                            field, static_cast<long long>(minimum),
                            static_cast<long long>(maximum));
                    output = static_cast<std::size_t>(value);
                }
                lua_pop(state, 1);
            };
            readAudioInteger("waveformPoints", 16, 256,
                audioAnalysis.waveformPoints, waveformPointsSpecified);
            readAudioInteger("spectrumBins", 16, 128,
                audioAnalysis.spectrumBins, spectrumBinsSpecified);

            lua_getfield(state, 2, "updateHz");
            if (!lua_isnil(state, -1))
            {
                updateHzSpecified = true;
                if (!lua_isinteger(state, -1))
                    return luaL_error(state,
                        "data.subscribe: updateHz must be an integer");
                const lua_Integer updateHz = lua_tointeger(state, -1);
                if (updateHz < 1 || updateHz > 60)
                    return luaL_error(state,
                        "data.subscribe: updateHz must be between 1 and 60");
                maxAgeMs = (1000 + updateHz - 1) / updateHz;
            }
            lua_pop(state, 1);

            if (maxAgeSpecified && updateHzSpecified)
                return luaL_error(state,
                    "data.subscribe: maxAgeMs and updateHz are mutually exclusive");
            if (waveformPointsSpecified && !audioAnalysis.waveform)
                return luaL_error(state,
                    "data.subscribe: waveformPoints requires the waveform feature");
            if (spectrumBinsSpecified && !audioAnalysis.spectrum)
                return luaL_error(state,
                    "data.subscribe: spectrumBins requires the spectrum feature");
        }
    }

    if (topicValue == "filesystem.watch")
    {
        if (scopeHandle.empty())
            return luaL_error(state,
                "data.subscribe: filesystem.watch requires a folder handle");
    }
    else if (!scopeHandle.empty())
    {
        return luaL_error(state,
            "data.subscribe: handle is only valid for filesystem.watch");
    }

    if (!rangeStart.empty() || !rangeEnd.empty())
    {
        if (topicValue != "calendar.events" || rangeStart.empty() ||
            rangeEnd.empty() || rangeEnd < rangeStart ||
            !snowdesktop::calendar::CalendarService::
                GetDateInfo(rangeStart) ||
            !snowdesktop::calendar::CalendarService::
                GetDateInfo(rangeEnd))
        {
            return luaL_error(state,
                "data.subscribe: fromDate/toDate require a valid calendar.events range");
        }
        LuaDataSubscribeMaximumEnd(context);
        if (context.maximumEnd && rangeEnd > *context.maximumEnd)
        {
            return luaL_error(state,
                "data.subscribe: calendar.events range cannot exceed 366 days");
        }
    }

    auto& calendarQueryError = context.calendarQueryError;
    if (!snowdesktop::widget_api::ParseCalendarEventQueryOptions(
            state, 2, calendarQuery, calendarQueryError))
        return luaL_error(state, "data.subscribe: %s", calendarQueryError.c_str());

    auto* d2d = GetD2D(state);
    if (!d2d || !d2d->engine)
        return luaL_error(state, "data.subscribe: host is unavailable");
    // Allocate the complete Lua handle before creating the native subscription.
    auto* handle = static_cast<LuaDataSubscriptionHandle*>(
        lua_newuserdata(state, sizeof(LuaDataSubscriptionHandle)));
    *handle = {};
    luaL_getmetatable(state, kDataSubscriptionHandleMetatable);
    lua_setmetatable(state, -2);
    auto& result = context.result;
    LuaDataSubscribeRegister(state, *d2d->engine, context,
        maxAgeMs, hiddenPolicy, audioAnalysis);
    if (!result)
        return luaL_error(state, "data.subscribe: %s", result.error.c_str());

    // No allocating Lua operation follows native registration.
    *handle = { d2d->engine, result.id, includeGpuDetails };
    return 1;
}

static int LuaDataSubscribeBridge(lua_State* state)
{
    auto& context = *static_cast<LuaDataSubscribeContext*>(lua_touserdata(state, 1));
    lua_remove(state, 1);
    try { return LuaDataSubscribeBody(state, context); }
    catch (...) { context.nativeFailure = true; return 0; }
}

static int lua_DataSubscribe(lua_State* state)
{
    const int arguments = lua_gettop(state);
    if (arguments < 1 || arguments > 2)
        return luaL_error(state, "data.subscribe: expected topic and optional options");
    if (!lua_checkstack(state, arguments + 2))
        return luaL_error(state, "data.subscribe: not enough Lua stack space");
    int status = LUA_OK;
    bool nativeFailure = false;
    {
        LuaDataSubscribeContext context;
        lua_pushcfunction(state, LuaDataSubscribeBridge);
        lua_pushlightuserdata(state, &context);
        for (int index = 1; index <= arguments; ++index) lua_pushvalue(state, index);
        status = lua_pcall(state, arguments + 1, 1, 0);
        nativeFailure = context.nativeFailure;
    }
    if (nativeFailure)
        return luaL_error(state, "data.subscribe: native host failure");
    if (status != LUA_OK) return lua_error(state);
    return 1;
}
