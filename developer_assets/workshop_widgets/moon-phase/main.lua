local moon = module.require("modules/moon.lua")

local frames = {}
local lazy = widget.hasFeature("resource.image.lazy")
for index = 0, moon.frameCount - 1 do
    local name = string.format("moon%02d", index)
    frames[index] = lazy and resource.image(name, { lazy = true }) or resource.image(name)
end

local cardAppearance = {
    bg = 0x203149,
    alpha = 0.68,
    border = 0x9CAFC6,
    borderAlpha = 0.42,
    gradientEndA = 0,
    glassEnabled = true,
}
local moonOnlyAppearance = {
    bg = 0,
    alpha = 0,
    border = 0,
    borderAlpha = 0,
    gradientEndA = 0,
    glassEnabled = false,
    acrylicEnabled = false,
    edgeHighlightEnabled = false,
}
local settings = {
    presets = {
        {
            id = "card",
            label = l10n.tr("lua_widget.moon_phase.card"),
            default = true,
            values = cardAppearance,
        },
        {
            id = "moon-only",
            label = l10n.tr("lua_widget.moon_phase.moon_only"),
            values = moonOnlyAppearance,
        },
    },
}

local function quarterName(quarter)
    if quarter == 0 then return l10n.tr("lua_widget.moon_phase.new") end
    if quarter == 1 then return l10n.tr("lua_widget.moon_phase.first") end
    if quarter == 2 then return l10n.tr("lua_widget.moon_phase.full") end
    return l10n.tr("lua_widget.moon_phase.last")
end

local function refresh(model)
    local now = tonumber(storage.get("previewEpochMs")) or time.now()
    local frame, quarter, quarterTime = moon.at(now)
    local date
    if quarterTime then
        local parts = time.parts(quarterTime, "local")
        date = l10n.tr("lua_widget.moon_phase.date", tostring(parts.month), tostring(parts.day))
    end

    if frame ~= model.frame or quarter ~= model.quarter or date ~= model.date then
        model.frame, model.quarter, model.date = frame, quarter, date
        widget.invalidate()
    end
end

local function setup()
    local model = {}
    refresh(model)
    schedule.every("moon-phase", 60000, { whenHidden = "pause" })
    return model
end

local function event(_context, model, change)
    if (change.kind == "schedule" and change.id == "moon-phase")
        or change.kind == "environment" or (change.kind == "visibility" and change.visible) then
        refresh(model)
    end
end

local function render(context, model)
    local width, height = context.layoutSize.width, context.layoutSize.height
    local side = math.min(width, height)
    local diameter = side * 0.63
    if model.frame then
        draw.image(frames[model.frame], (width - diameter) / 2, (height - diameter) / 2, diameter, diameter)
    end

    -- Applying an appearance preset updates this host key and its material together.
    if storage.get("__preset") == "moon-only" then return end

    local color = widget.theme().contentTheme == 1 and 0x18263A or 0xEDF3FA
    local inset = side * 0.074
    local font = math.min(24, side * 0.067)
    local footerFont = math.min(font, side * 0.060)
    local footerY = height - inset - footerFont * 1.12
    draw.text(inset, inset * 0.77, l10n.tr("lua_widget.moon_phase.name"), font, color, width - inset * 2, false, true)
    draw.line(inset, footerY - side * 0.023, width - inset, footerY - side * 0.023, 1, color, 0.30)

    local footerWidth = width - inset * 2
    if model.date then
        local dateWidth = draw.measureText(model.date, footerFont).width + 1
        draw.text(width - inset - dateWidth, footerY, model.date, footerFont, color, dateWidth, false, true)
        draw.text(inset, footerY, quarterName(model.quarter), footerFont, color,
            math.max(1, footerWidth - dateWidth - inset / 2), false, true)
    else
        draw.text(inset, footerY, l10n.tr("lua_widget.moon_phase.out_of_range"), footerFont, color,
            footerWidth, false, true)
    end
end

return widget.define({
    name = l10n.tr("lua_widget.moon_phase.name"),
    useCustomStyle = true,
    followPersonalizationDefault = false,
    showTitle = false,
    bg = cardAppearance.bg,
    alpha = cardAppearance.alpha,
    border = cardAppearance.border,
    borderAlpha = cardAppearance.borderAlpha,
    gradientEndA = cardAppearance.gradientEndA,
    glassEnabled = cardAppearance.glassEnabled,
    setup = setup,
    event = event,
    render = render,
    settings = settings,
})
