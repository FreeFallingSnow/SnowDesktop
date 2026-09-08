local M = {}

local function positive(value)
    value = tonumber(value)
    if not value or value ~= value or value == math.huge or
        value == -math.huge or value <= 0 then
        return 1
    end
    return value
end

function M.plan(width, height)
    width = positive(width)
    height = positive(height)
    local vertical = height > width * 1.05
    local padding
    local contentGap
    local coverSize
    local detailWidth
    local titleFont
    local primaryButton

    if vertical then
        padding = math.min(width * 0.034, height * 0.027)
        contentGap = math.min(width * 0.048, height * 0.038)
        coverSize = math.min(width * 0.60, height * 0.44)
        detailWidth = width - padding * 2
        titleFont = math.min(width * 0.065, height * 0.052)
        primaryButton = math.min(width * 0.145, height * 0.116)
    else
        padding = math.min(width * 0.045, height * 0.075)
        contentGap = math.min(width * 0.050, height * 0.085)
        local contentWidth = width - padding * 2
        coverSize = math.min(height * 0.62, contentWidth * 0.43)
        detailWidth = contentWidth - coverSize - contentGap
        titleFont = math.min(height * 0.079, detailWidth * 0.10)
        primaryButton = math.min(height * 0.175, detailWidth * 0.23)
    end

    local secondaryButton = primaryButton * 0.86
    local controlGap = math.min(primaryButton * 0.30,
        detailWidth * 0.065)
    local artistFont = titleFont * 0.63
    local albumFont = titleFont * 0.53
    return {
        vertical = vertical,
        padding = padding,
        contentGap = contentGap,
        coverSize = coverSize,
        detailWidth = detailWidth,
        primaryButton = primaryButton,
        secondaryButton = secondaryButton,
        controlGap = controlGap,
        controlHeight = primaryButton * 1.10,
        titleFont = titleFont,
        artistFont = artistFont,
        albumFont = albumFont,
        titleHeight = titleFont * 1.64,
        artistHeight = artistFont * 1.84,
        emptyArtistHeight = titleFont * 2.0,
        albumHeight = albumFont * 2.0,
        progressHeight = primaryButton * 0.95,
        sliderHeight = primaryButton * 0.57,
        timeFont = titleFont * 0.53,
        metadataInset = primaryButton * 0.19,
        detailGap = titleFont * 0.105,
        emptyDetailGap = titleFont * 0.316,
        spectrumSidePadding = width * 0.025,
        spectrumTop = vertical and height * 0.54 or height * 0.15,
    }
end

return M
