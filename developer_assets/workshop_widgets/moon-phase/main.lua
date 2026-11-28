local moon = module.require("modules/moon.lua")

local frames = {}
local lazy = widget.hasFeature("resource.image.lazy")
for index = 0, moon.frameCount - 1 do
    local name = string.format("moon%02d", index)
    frames[index] = lazy and resource.image(name, { lazy = true }) or resource.image(name)
end

local function isMoonOnly()
    local value = storage.get("pureMoon")
    return value == true or value == "1" or value == "true"
end

-- With the instance set to custom appearance, these stored settings can
-- override the descriptor's transparent defaults.
local surfaceOverrides = {
    "__preset", "__panelGradient", "__edgeLight", "alpha", "borderAlpha",
    "gradientEndA", "glassEnabled", "acrylicEnabled", "edgeHighlightEnabled",
    "borderStyle",
}

local savedAppearanceKey = "moonOnlyPreviousAppearance"

local function restoreValue(tx, key, value)
    if value == nil then
        tx:remove(key)
    else
        tx:set(key, value)
    end
end

local function applyMode(pureMoon, saveToggle)
    storage.transaction(function(tx)
        if saveToggle then
            tx:set("pureMoon", pureMoon and "1" or "0")
        end
        local previous = tx:get(savedAppearanceKey)
        if pureMoon then
            -- Keep the original values across reloads and theme edits in pure mode.
            if type(previous) ~= "table" then
                previous = { followPersonalization = tx:get("followPersonalization") }
                for _, key in ipairs(surfaceOverrides) do previous[key] = tx:get(key) end
                tx:set(savedAppearanceKey, previous)
            end
            tx:set("followPersonalization", "0")
            for _, key in ipairs(surfaceOverrides) do tx:remove(key) end
        elseif type(previous) == "table" then
            restoreValue(tx, "followPersonalization", previous.followPersonalization)
            for _, key in ipairs(surfaceOverrides) do restoreValue(tx, key, previous[key]) end
            tx:remove(savedAppearanceKey)
        end
    end)
end

local function migrateStorage(oldVersion)
    local preset = storage.get("__preset")
    local legacyPurePreset = oldVersion == 1 and preset == "moon-only"
    if legacyPurePreset or isMoonOnly() then
        -- Older packages already overwrote these values while pure mode was on.
        -- An empty snapshot returns these instances to the default follow mode.
        storage.transaction(function(tx)
            if legacyPurePreset then tx:set("pureMoon", "1") end
            tx:set(savedAppearanceKey, {})
        end)
    elseif oldVersion == 1 and (preset == "card" or storage.get("followPersonalization") == "0") then
        storage.remove("followPersonalization")
    end
end

local function menu(_context, _model, request)
    if request.id ~= "moon.menu" then return nil end
    local status = isMoonOnly() and l10n.tr("lua_widget.moon_phase.on") or l10n.tr("lua_widget.moon_phase.off")
    return ui.menu({
        {
            id = "moon.toggle",
            label = l10n.tr("lua_widget.moon_phase.moon_only") .. "\t" .. status,
            icon = utf8.char(0xF87D), -- Fluent System Icons Regular: weather_moon_20.
            iconFont = "fluent",
            keepOpen = widget.hasFeature("interaction.contextMenu.keepOpen"),
        },
    })
end

local function phaseName(phase)
    if phase == 0 then return l10n.tr("lua_widget.moon_phase.new") end
    if phase == 1 then return l10n.tr("lua_widget.moon_phase.waxing_crescent") end
    if phase == 2 then return l10n.tr("lua_widget.moon_phase.first") end
    if phase == 3 then return l10n.tr("lua_widget.moon_phase.waxing_gibbous") end
    if phase == 4 then return l10n.tr("lua_widget.moon_phase.full") end
    if phase == 5 then return l10n.tr("lua_widget.moon_phase.waning_gibbous") end
    if phase == 6 then return l10n.tr("lua_widget.moon_phase.last") end
    if phase == 7 then return l10n.tr("lua_widget.moon_phase.waning_crescent") end
    return l10n.tr("lua_widget.moon_phase.out_of_range")
end

local function refresh(model)
    local now = tonumber(storage.get("previewEpochMs")) or time.now()
    local frame, phase = moon.at(now)
    local parts = time.parts(now, "local")
    local date = l10n.tr("lua_widget.moon_phase.date", tostring(parts.month), tostring(parts.day))

    if frame ~= model.frame or phase ~= model.phase or date ~= model.date then
        model.frame, model.phase, model.date = frame, phase, date
        widget.invalidate()
    end
end

local function setup()
    local model = { pureMoon = isMoonOnly() }
    if model.pureMoon or storage.get(savedAppearanceKey) then
        applyMode(model.pureMoon, false)
    end
    refresh(model)
    schedule.every("moon-phase", 60000, { whenHidden = "pause" })
    return model
end

local function event(_context, model, change)
    if (change.kind == "schedule" and change.id == "moon-phase")
        or change.kind == "environment" or (change.kind == "visibility" and change.visible) then
        refresh(model)
    elseif change.kind == "action" and change.source == "contextMenu" and change.id == "moon.toggle" then
        local pureMoon = not isMoonOnly()
        applyMode(pureMoon, true)
        model.pureMoon = pureMoon
        widget.invalidate()
    elseif change.kind == "settings.changed" and not change.preview then
        local pureMoon = isMoonOnly()
        if pureMoon or model.pureMoon ~= pureMoon then
            applyMode(pureMoon, false)
        end
        model.pureMoon = pureMoon
        widget.invalidate()
    end
end

local function render(context, model)
    local width, height = context.layoutSize.width, context.layoutSize.height
    interaction.region({
        key = "moon.surface",
        shape = { type = "rect", x = 0, y = 0, width = width, height = height },
        events = { contextMenu = { id = "moon.menu", scope = "component" } },
    })
    local side = math.min(width, height)
    local pureMoon = isMoonOnly()
    local diameter = side * (pureMoon and 0.84 or 0.63)
    if model.frame then
        draw.image(frames[model.frame], (width - diameter) / 2, (height - diameter) / 2, diameter, diameter)
    end

    if pureMoon then return end

    local color = widget.theme().contentTheme == 1 and 0x18263A or 0xEDF3FA
    local inset = side * 0.074
    local font = math.min(24, side * 0.067)
    local footerFont = math.min(font, side * 0.060)
    local footerY = height - inset - footerFont * 1.12
    draw.text(inset, inset * 0.77, l10n.tr("lua_widget.moon_phase.name"), font, color, width - inset * 2, false, true)
    draw.line(inset, footerY - side * 0.023, width - inset, footerY - side * 0.023, 1, color, 0.30)

    local footerWidth = width - inset * 2
    local dateWidth = draw.measureText(model.date, footerFont).width + 1
    draw.text(width - inset - dateWidth, footerY, model.date, footerFont, color, dateWidth, false, true)
    draw.text(inset, footerY, phaseName(model.phase), footerFont, color,
        math.max(1, footerWidth - dateWidth - inset / 2), false, true)
end

return widget.define({
    name = l10n.tr("lua_widget.moon_phase.name"),
    showTitle = false,
    useCustomStyle = true,
    followPersonalizationDefault = true,
    alpha = 0,
    borderAlpha = 0,
    gradientEndA = 0,
    glassEnabled = false,
    acrylicEnabled = false,
    edgeHighlightEnabled = false,
    migrateStorage = migrateStorage,
    setup = setup,
    event = event,
    menu = menu,
    render = render,
    settings = {
        fields = {
            {
                key = "pureMoon",
                label = l10n.tr("lua_widget.moon_phase.moon_only"),
                type = "bool",
                default = false,
                agentDescription = "Shows a larger lunar disc on a transparent surface. Turning it off restores the previous Follow Global setting and any appearance values overridden by Moon Only, along with the title, current phase name, and today's local date. The component menu changes this same per-instance setting.",
            },
        },
    },
})
