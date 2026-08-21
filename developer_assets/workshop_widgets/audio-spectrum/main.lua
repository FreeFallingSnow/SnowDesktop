local spectrum = module.require("modules/spectrum.lua")
local audioAnalysis

local DEFAULT_COLOR = 0x72C7FF

local settings = {
    groups = {
        {
            id = "appearance",
            label = l10n.tr("workshop.audio_spectrum.settings_group"),
            description = l10n.tr(
                "workshop.audio_spectrum.settings_group_help"),
            collapsible = false,
        },
    },
    fields = {
        {
            key = "bar_color",
            label = l10n.tr("workshop.audio_spectrum.bar_color"),
            description = l10n.tr(
                "workshop.audio_spectrum.bar_color_help"),
            type = "color",
            default = DEFAULT_COLOR,
            group = "appearance",
        },
        {
            key = "bar_count",
            label = l10n.tr("workshop.audio_spectrum.bar_count"),
            description = l10n.tr(
                "workshop.audio_spectrum.bar_count_help"),
            type = "range",
            min = 16,
            max = 128,
            step = 16,
            default = 64,
            group = "appearance",
        },
        {
            key = "sensitivity",
            label = l10n.tr("workshop.audio_spectrum.sensitivity"),
            description = l10n.tr(
                "workshop.audio_spectrum.sensitivity_help"),
            type = "range",
            min = 0.5,
            max = 3.0,
            step = 0.1,
            default = 1.5,
            group = "appearance",
        },
    },
}

local statusKeys = {
    warming = "workshop.audio_spectrum.warming",
    stale = "workshop.audio_spectrum.stale",
    permission = "workshop.audio_spectrum.permission",
    notPresent = "workshop.audio_spectrum.not_present",
    unavailable = "workshop.audio_spectrum.unavailable",
}

local function readConfig()
    return {
        color = tonumber(storage.get("bar_color")) or DEFAULT_COLOR,
        barCount = spectrum.binCount(storage.get("bar_count")),
        sensitivity = spectrum.sensitivity(storage.get("sensitivity")),
    }
end

local function capture(model, reset)
    local snapshot = audioAnalysis:value()
    local status = spectrum.status(snapshot)
    local timestamp = tonumber(snapshot.timestamp) or 0
    local changed = reset or timestamp ~= model.lastTimestamp or
        status ~= model.status
    model.status = status
    model.lastTimestamp = timestamp
    if not changed then return end
    if snapshot.available and type(snapshot.value) == "table" and
        type(snapshot.value.spectrum) == "table" then
        model.values = spectrum.smooth(model.values,
            snapshot.value.spectrum, model.sensitivity, model.barCount,
            reset or snapshot.value.deviceChanged == true)
    else
        model.values = spectrum.zeroes(model.barCount)
    end
end

local function setup()
    local config = readConfig()
    audioAnalysis = data.subscribe("audio.output.analysis", {
        features = { "spectrum" },
        updateHz = 30,
        spectrumBins = config.barCount,
        whenHidden = "pause",
    })
    local model = {
        color = config.color,
        barCount = config.barCount,
        sensitivity = config.sensitivity,
        values = spectrum.zeroes(config.barCount),
        status = "warming",
        lastTimestamp = nil,
    }
    capture(model, true)
    return model
end

local function spectrumNode(model, opacity)
    local padding = math.max(layout.cu(4), math.min(
        layout.cu(10), layout.vmin(5)))
    return view.spectrum({
        key = "audio-spectrum.bars",
        values = spectrum.display(model.values, model.barCount),
        min = 0,
        max = 1,
        width = "fill",
        height = "fill",
        padding = padding,
        fillOpacity = 0.94,
        style = {
            foreground = model.color,
            opacity = opacity or 1,
        },
        accessibility = {
            label = l10n.tr("workshop.audio_spectrum.spectrum_label"),
        },
    })
end

local function statusNode(status)
    local key = statusKeys[status] or statusKeys.unavailable
    return view.text({
        key = "audio-spectrum.status",
        text = l10n.tr(key),
        width = "fill",
        height = "fill",
        padding = layout.cu(8),
        fontSize = layout.fontCu(12),
        textAlign = "center",
        verticalAlign = "center",
        textWrap = "wrap",
        maxLines = 2,
        style = { foreground = "textSecondary" },
    })
end

local function viewTree(_context, model)
    capture(model, false)
    if model.status == "ready" or model.status == "silent" or
        model.status == "warming" then
        return spectrumNode(model)
    end
    if model.status == "stale" then
        return view.stack({
            key = "audio-spectrum.stale",
            width = "fill",
            height = "fill",
            children = {
                spectrumNode(model, 0.32),
                statusNode(model.status),
            },
        })
    end
    return statusNode(model.status)
end

local function event(_context, model, value)
    if value.kind == "environment" then
        widget.setTitle(l10n.tr("workshop.audio_spectrum.name"))
    end
end

local function dispose()
    if audioAnalysis then audioAnalysis:unsubscribe() end
    audioAnalysis = nil
end

return widget.define({
    name = l10n.tr("workshop.audio_spectrum.name"),
    useCustomStyle = true,
    showTitle = false,
    bg = 0x000000,
    border = 0x000000,
    alpha = 0,
    borderAlpha = 0,
    gradientEndA = 0,
    glassEnabled = false,
    settings = settings,
    setup = setup,
    view = viewTree,
    event = event,
    dispose = dispose,
})
