local spectrum = module.require("modules/spectrum.lua")
local audioAnalysis

local DEFAULT_COLOR = 0xBFE9FF
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
            default = 48,
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
        local deviceReset = reset or snapshot.value.deviceChanged == true
        model.rangeEnd = spectrum.adaptiveRange(snapshot.value.spectrum,
            deviceReset and nil or model.rangeEnd)
        model.values = spectrum.smooth(model.values,
            snapshot.value.spectrum, model.sensitivity, model.barCount,
            deviceReset, model.rangeEnd)
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
        rangeEnd = CAPTURE_BINS,
        preview = context.preview == true,
    }
    capture(model, true, true)
    return model
end

local function registerAccessibility(width, height, label)
    interaction.region({
        key = "audio-spectrum.visual",
        shape = {
            type = "rect",
            x = 0,
            y = 0,
            width = math.max(1, width),
            height = math.max(1, height),
        },
        accessibility = {
            role = "group",
            label = label,
        },
    })
end

local function drawStatus(status, width, height)
    local key = statusKeys[status] or statusKeys.unavailable
    local text = l10n.tr(key)
    local fontSize = layout.fontCu(12)
    local padding = layout.cu(8)
    local maxWidth = math.max(1, width - padding * 2)
    local metrics = draw.measureText(text, fontSize, maxWidth, false)
    local textX = math.max(padding, (width - metrics.width) / 2)
    local textY = math.max(padding, (height - metrics.height) / 2)
    local theme = widget.theme()
    local color = theme and theme.contentTheme == 1 and
        0x334155 or 0xCBD5E1
    draw.text(textX, textY, text, fontSize, color,
        maxWidth, false, false, math.max(1, height - textY - padding))
    registerAccessibility(width, height, text)
end

local function drawBar(x, baseline, barWidth, extent, level,
    direction, colors, alpha, reflection)
    if extent <= 0 or barWidth <= 0 then return end
    local minimum = math.min(extent,
        math.max(layout.cu(2), barWidth * 0.72))
    local barHeight = minimum + (extent - minimum) * level
    local y = direction < 0 and baseline - barHeight or baseline
    local radius = math.min(barWidth * 0.5, barHeight * 0.5)
    local glow = math.min(layout.cu(1.5), barWidth * 0.34)
    draw.gradientRect(x - glow, y - glow,
        barWidth + glow * 2, barHeight + glow * 2,
        colors.glow, colors.glow, "vertical", radius + glow,
        alpha * (reflection and 0.12 or 0.18))

    local topColor = direction < 0 and colors.highlight or colors.base
    local bottomColor = direction < 0 and colors.depth or colors.highlight
    draw.gradientRect(x, y, barWidth, barHeight,
        topColor, bottomColor, "vertical", radius, alpha)

    if not reflection then
        local capHeight = math.min(barHeight * 0.18,
            math.max(layout.cu(0.7), barWidth * 0.22))
        local capY = direction < 0 and y or y + barHeight - capHeight
        draw.rect(x + barWidth * 0.22, capY,
            barWidth * 0.56, capHeight, colors.highlight,
            capHeight * 0.5, alpha * 0.72)
    end
end

local function drawSpectrum(model, width, height)
    local padding = math.max(layout.cu(6), math.min(
        layout.cu(14), layout.vmin(3.8)))
    local plotWidth = math.max(1, width - padding * 2)
    local values = spectrum.visual(model.values, model.barCount)
    local stride = plotWidth / math.max(1, #values)
    local barWidth = math.max(0.75,
        math.min(layout.cu(12), stride * 0.62))
    local colors = {
        base = model.color,
        highlight = spectrum.mixColor(model.color, 0xFFFFFF, 0.52),
        depth = spectrum.mixColor(model.color, 0x071A3A, 0.32),
        glow = spectrum.mixColor(model.color, 0xFFFFFF, 0.18),
    }

    local alignment = model.alignment
    local baseline
    local primaryDirection
    local primaryExtent
    local reflectionExtent = 0
    if alignment == "top" then
        baseline = padding
        primaryDirection = 1
        primaryExtent = math.max(1, height - padding * 2)
    elseif alignment == "bottom" then
        baseline = height - padding
        primaryDirection = -1
        primaryExtent = math.max(1, height - padding * 2)
    else
        baseline = height * 0.56
        primaryDirection = -1
        primaryExtent = math.max(1, baseline - padding)
        reflectionExtent = math.max(1,
            (height - padding - baseline) * 0.78)
    end

    draw.rect(padding, baseline - layout.cu(0.45), plotWidth,
        layout.cu(0.9), colors.base, layout.cu(0.45), 0.16)

    for index, level in ipairs(values) do
        local x = padding + (index - 0.5) * stride - barWidth * 0.5
        drawBar(x, baseline, barWidth, primaryExtent,
            level, primaryDirection, colors, 0.94, false)
        if reflectionExtent > 0 then
            drawBar(x, baseline, barWidth, reflectionExtent,
                level * 0.72, 1, colors, 0.30, true)
        end
    end

    registerAccessibility(width, height,
        l10n.tr("workshop.audio_spectrum.spectrum_label"))
end

local function render(_context, model)
    local force, reset = applyConfig(model)
    capture(model, force, reset)
    local width = layout.contentWidth()
    local height = layout.contentHeight()
    if model.status == "ready" or model.status == "silent" or
        model.status == "warming" or model.status == "stale" then
        drawSpectrum(model, width, height)
        return
    end
    drawStatus(model.status, width, height)
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
    render = render,
    event = event,
    dispose = dispose,
})
