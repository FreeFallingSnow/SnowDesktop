-- digital_clock.lua - API v2 数字时钟
local showWeekday = true
local showDate = true
local showSeconds = true
local textColor = 0xFFFFFF
local textOpacity = 1.0
local clockScale = 1.0
local descriptor

local settings = {
    presets = {
        {
            id = "transparent",
            label = l10n.tr("lua_widget.digital_clock.preset_transparent"),
            default = true,
            values = {
                bg = 0x000000,
                border = 0x000000,
                alpha = 0.0,
                borderAlpha = 0.0,
                gradientEndA = 0.0,
                textColor = 0xFFFFFF,
                textOpacity = 1.0,
            }
        }
    },
    fields = {
        { key = "showWeekday", label = l10n.tr("lua_widget.digital_clock.show_weekday"), type = "bool", default = true },
        { key = "showDate", label = l10n.tr("lua_widget.digital_clock.show_date"), type = "bool", default = true },
        { key = "showSeconds", label = l10n.tr("lua_widget.digital_clock.show_seconds"), type = "bool", default = true },
        { key = "textColor", label = l10n.tr("lua_widget.common.text_color"), type = "color", default = 0xFFFFFF },
        { key = "textOpacity", label = l10n.tr("lua_widget.digital_clock.text_opacity"), type = "float", default = 1.0, min = 0.0, max = 1.0 },
        { key = "scale", label = l10n.tr("lua_widget.common.scale"), type = "float", default = 1.0, min = 0.5, max = 3.0 },
    }
}

local function setup()
    schedule.every("clock", 1000, { whenHidden = "pause" })
end

local function loadConfig()
    descriptor.bg = tonumber(storage.get("bg")) or descriptor.bg
    descriptor.border = descriptor.bg
    descriptor.alpha = tonumber(storage.get("alpha")) or descriptor.alpha
    descriptor.gradientEndA = tonumber(storage.get("gradientEndA")) or descriptor.gradientEndA
    showWeekday = storage.get("showWeekday") ~= "0"
    showDate = storage.get("showDate") ~= "0"
    showSeconds = storage.get("showSeconds") ~= "0"
    textColor = tonumber(storage.get("textColor")) or textColor
    textOpacity = math.max(0.0, math.min(1.0, tonumber(storage.get("textOpacity")) or textOpacity))
    clockScale = tonumber(storage.get("scale")) or clockScale
    local followPersonalization = storage.get("followPersonalization") == "1"
    if followPersonalization then
        local theme = widget.theme()
        if theme then
            textColor = (theme.contentTheme == 1) and 0x000000 or 0xFFFFFF
        end
    end
end

local function buildView(context)
    loadConfig()
    local t = time.parts(time.now())
    local timeStr
    if showSeconds then
        timeStr = string.format("%02d:%02d:%02d", t.hour, t.min, t.sec)
    else
        timeStr = string.format("%02d:%02d", t.hour, t.min)
    end
    local dateStr = l10n.tr("lua_widget.digital_clock.date_format",
        tostring(t.year), string.format("%02d", t.month), string.format("%02d", t.day))
    local weekDays = {
        l10n.tr("lua_widget.digital_clock.sunday"),
        l10n.tr("lua_widget.digital_clock.monday"),
        l10n.tr("lua_widget.digital_clock.tuesday"),
        l10n.tr("lua_widget.digital_clock.wednesday"),
        l10n.tr("lua_widget.digital_clock.thursday"),
        l10n.tr("lua_widget.digital_clock.friday"),
        l10n.tr("lua_widget.digital_clock.saturday"),
    }
    local weekdayStr = l10n.tr("lua_widget.digital_clock.weekday_format",
        weekDays[t.wday or 1])

    local width = math.max(1, context.logicalSize.width)
    local contentHeight = math.max(1,
        context.logicalSize.height - layout.barHeight())
    local padding = math.max(layout.cu(8), width * 0.06)
    local availableWidth = math.max(1, width - padding * 2)
    local timeSize = math.min(
        layout.fontCu(28) * clockScale,
        availableWidth / math.max(1, #timeStr * 0.55),
        contentHeight * 0.42)
    timeSize = math.max(layout.fontCu(12), timeSize)
    local secondarySize = math.max(layout.fontCu(7),
        math.min(layout.fontCu(10) * clockScale, timeSize * 0.42))

    local children = {
        view.text({
            key = "clock.time",
            text = timeStr,
            width = "fill",
            fontSize = timeSize,
            bold = true,
            textAlign = "center",
            accessibility = {
                role = "text",
                label = timeStr,
            },
            style = {
                foreground = textColor,
                opacity = textOpacity,
            },
        }),
    }

    local secondaryParts = {}
    if showDate then table.insert(secondaryParts, dateStr) end
    if showWeekday then table.insert(secondaryParts, weekdayStr) end
    if #secondaryParts > 0 then
        local secondaryText = table.concat(secondaryParts, "  ")
        children[#children + 1] = view.text({
            key = "clock.secondary",
            text = secondaryText,
            width = "fill",
            fontSize = secondarySize,
            textAlign = "center",
            accessibility = {
                role = "text",
                label = secondaryText,
            },
            style = {
                foreground = textColor,
                opacity = textOpacity,
            },
        })
    end

    return view.column({
        key = "clock.root",
        width = "fill",
        height = "fill",
        padding = padding,
        gap = math.max(layout.cu(2), contentHeight * 0.015),
        alignItems = "stretch",
        justifyContent = "center",
        accessibility = {
            role = "group",
            label = l10n.tr("lua_widget.digital_clock.name"),
        },
        children = children,
    })
end

descriptor = {
    useCustomStyle = true,
    bg = 0x000000,
    border = 0x000000,
    alpha = 0.0,
    gradientEndA = 0.0,
    settings = settings,
    setup = setup,
    view = buildView,
}

return widget.define(descriptor)
