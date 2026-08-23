local spectrum = module.require("modules/spectrum.lua")
local audioAnalysis

local DEFAULT_COLOR = 0xFFFFFF
local CAPTURE_BINS = 128

local settings = {
    presets = {
        {
            id = "transparent",
            label = l10n.tr(
                "workshop.audio_spectrum.preset_transparent"),
            default = true,
            values = {
                bg = 0x000000,
                border = 0x000000,
                alpha = 0,
                borderAlpha = 0,
                gradientEndA = 0,
                glassEnabled = false,
                acrylicEnabled = false,
            },
        },
    },
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
            key = "vertical_alignment",
            label = l10n.tr(
                "workshop.audio_spectrum.vertical_alignment"),
            description = l10n.tr(
                "workshop.audio_spectrum.vertical_alignment_help"),
            type = "select",
            options = { "bottom", "center", "top" },
            optionLabels = {
                l10n.tr("workshop.audio_spectrum.align_bottom"),
                l10n.tr("workshop.audio_spectrum.align_center"),
                l10n.tr("workshop.audio_spectrum.align_top"),
            },
            default = "center",
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
    permission = "workshop.audio_spectrum.permission",
    notPresent = "workshop.audio_spectrum.not_present",
    unavailable = "workshop.audio_spectrum.unavailable",
}

local function readConfig()
    return {
        color = spectrum.color(storage.get("bar_color"), DEFAULT_COLOR),
        barCount = spectrum.binCount(storage.get("bar_count")),
        alignment = spectrum.alignment(
            storage.get("vertical_alignment")),
        sensitivity = spectrum.sensitivity(storage.get("sensitivity")),
    }
end

local function applyConfig(model)
    local config = readConfig()
    local countChanged = config.barCount ~= model.barCount
    local sensitivityChanged = config.sensitivity ~= model.sensitivity
    model.color = config.color
    model.barCount = config.barCount
    model.alignment = config.alignment
    model.sensitivity = config.sensitivity
    if countChanged then
        model.values = spectrum.zeroes(config.barCount)
    end
    return countChanged or sensitivityChanged, countChanged
end

local function capture(model, force, reset)
    local snapshot = audioAnalysis:value()
    local status = spectrum.status(snapshot)
    local timestamp = tonumber(snapshot.timestamp) or 0
    local changed = force or timestamp ~= model.lastTimestamp or
        status ~= model.status
    model.status = status
    model.lastTimestamp = timestamp
    if not changed then return end
    if model.preview and status == "ready" then
        model.values = spectrum.preview(
            model.barCount, model.sensitivity)
    elseif status == "silent" then
        model.values = spectrum.zeroes(model.barCount)
    elseif snapshot.available and type(snapshot.value) == "table" and
        type(snapshot.value.spectrum) == "table" then
        model.values = spectrum.smooth(model.values,
            snapshot.value.spectrum, model.sensitivity, model.barCount,
            reset or snapshot.value.deviceChanged == true)
    else
        model.values = spectrum.zeroes(model.barCount)
    end
end

local function setup(context)
    local config = readConfig()
    audioAnalysis = data.subscribe("audio.output.analysis", {
        features = { "spectrum" },
        updateHz = 60,
        spectrumBins = CAPTURE_BINS,
        whenHidden = "pause",
    })
    local model = {
        color = config.color,
        barCount = config.barCount,
        alignment = config.alignment,
        sensitivity = config.sensitivity,
        values = spectrum.zeroes(config.barCount),
        status = "warming",
        lastTimestamp = nil,
        preview = context.preview == true,
    }
    capture(model, true, true)
    return model
end

local function dataSeriesStyle(model)
    return {
        foreground = model.color,
    }
end

local function dataSeriesAccessibility(hidden)
    return {
        label = l10n.tr("workshop.audio_spectrum.spectrum_label"),
        hidden = hidden == true,
    }
end

local function spectrumNode(model)
    local padding = math.max(layout.cu(4), math.min(
        layout.cu(10), layout.vmin(5)))
    local values = spectrum.display(model.values, model.barCount)
    local plan = spectrum.seriesPlan(model.alignment)
    local function properties(key, seriesValues, hidden, trackOpacity)
        return {
            key = key,
            values = seriesValues,
            width = "fill",
            height = "fill",
            padding = padding,
            fillOpacity = 0.94,
            trackOpacity = trackOpacity,
            min = plan.minimum,
            max = plan.maximum,
            style = dataSeriesStyle(model),
            accessibility = dataSeriesAccessibility(hidden),
        }
    end

    if plan.renderer == "spectrum" then
        return view.spectrum(properties(
            "audio-spectrum.bars", values, false, nil))
    end

    if plan.negate then values = spectrum.negate(values) end
    local primary = view.barChart(properties(
        "audio-spectrum.bars", values, false, 1))
    if not plan.mirror then return primary end

    return view.stack({
        key = "audio-spectrum.centered",
        width = "fill",
        height = "fill",
        children = {
            primary,
            view.barChart(properties("audio-spectrum.reflection",
                spectrum.negate(values), true, 0)),
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
    local force, reset = applyConfig(model)
    capture(model, force, reset)
    if model.status == "ready" or model.status == "silent" or
        model.status == "warming" or model.status == "stale" then
        return spectrumNode(model)
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
    -- Match the default host-managed transparent preset before instance
    -- appearance storage is materialized.
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
