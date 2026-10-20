-- rss-reader/main.lua - API v2 bounded network task and virtual feed list
local descriptor

local function componentMetrics()
    local row = ui.metrics().layoutRowHeight
    local scale = row / 28
    return {
        layoutRowHeight = row,
        spacingXs = 4 * scale,
        spacingSm = 8 * scale,
        spacingMd = 12 * scale,
        spacingLg = 16 * scale,
        captionFontSize = 10 * scale,
        bodyFontSize = 12 * scale,
        titleFontSize = 14 * scale,
        controlFontSize = 12 * scale,
        iconSize = 16 * scale,
        controlRadius = 8 * scale,
        strokeWidth = scale,
    }
end

local fluent = {
    refresh = utf8.char(0xF13D),
    clear = utf8.char(0xF201),
    open = utf8.char(0xF582),
}

local DEFAULT_URL = "https://www.ithome.com/rss/"
local function responseLimit()
    return widget.hasFeature("task.network.largeResponse") and
        4 * 1024 * 1024 or 1024 * 1024
end

local settings = {
    fields = {
        { key = "url", label = l10n.tr("lua_widget.rss_reader.url"),
            type = "text", default = DEFAULT_URL },
        { key = "interval",
            label = l10n.tr("lua_widget.rss_reader.refresh_interval"),
            type = "duration", default = 1800, min = 60, max = 3600 },
        { key = "maxItems",
            label = l10n.tr("lua_widget.rss_reader.max_items"),
            type = "int", default = 30, min = 10, max = 100 },
        { key = "rowScale",
            label = l10n.tr("lua_widget.rss_reader.article_row_scale"),
            type = "int", default = 100, min = 70, max = 160 },
    },
}

local function config()
    local rowScale = tonumber(storage.get("rowScale"))
    if rowScale then
        rowScale = math.max(70, math.min(160, rowScale)) / 100
    else
        local previousScale = tonumber(storage.get("fontScale"))
        if previousScale then
            rowScale = math.max(70, math.min(160, previousScale)) / 100
        else
            local legacy = math.max(10, math.min(24,
                tonumber(storage.get("fontSize")) or 15))
            rowScale = legacy / 15
        end
    end
    return {
        url = storage.get("url") or DEFAULT_URL,
        interval = math.max(60, math.min(3600,
            tonumber(storage.get("interval")) or 1800)),
        maxItems = math.max(10, math.min(100,
            tonumber(storage.get("maxItems")) or 30)),
        rowScale = rowScale,
    }
end

local function palette(context)
    if context.theme and context.theme.mode == "light" then
        return {
            header = 0x0F172A, count = 0x334155, divider = 0x334155,
            number = 0x475569, title = 0x1E293B, date = 0x64748B,
            status = 0x334155, error = 0xB91C1C, card = 0x000000,
        }
    end
    return {
        header = 0xF8FAFC, count = 0xF1F5F9, divider = 0xFFFFFF,
        number = 0xFFFFFF, title = 0xF1F5F9, date = 0x94A3B8,
        status = 0xF1F5F9, error = 0xFF8B8B, card = 0xFFFFFF,
    }
end

local entities = { lt = "<", gt = ">", amp = "&", quot = "\"", apos = "'" }
local function decodeText(value)
    return (value or ""):gsub("<!%[CDATA%[(.-)%]%]>", "%1")
        :gsub("<[^>]+>", "")
        :gsub("&([#%w]+);", function(entity)
            if entities[entity] then return entities[entity] end
            local decimal = entity:match("^#(%d+)$")
            local hex = entity:match("^#[xX]([%da-fA-F]+)$")
            local code = decimal and tonumber(decimal) or
                (hex and tonumber(hex, 16))
            if code and (code == 9 or code == 10 or code == 13 or
                code >= 32) and code <= 0x10FFFF and
                not (code >= 0xD800 and code <= 0xDFFF) then
                return utf8.char(code)
            end
            return "&" .. entity .. ";"
        end)
end

local function elementText(xml, name)
    return xml:match("<" .. name .. "%s*>(.-)</" .. name .. "%s*>") or
        xml:match("<" .. name .. "%s+[^>]*>(.-)</" .. name .. "%s*>")
end

local function parseRssItem(xml)
    return {
        title = decodeText(elementText(xml, "title")),
        link = decodeText(elementText(xml, "link")),
        date = decodeText(elementText(xml, "pubDate") or
            elementText(xml, "dc:date")),
    }
end

local function parseAtomEntry(xml)
    local link = xml:match('<link[^>]+href="([^"]*)"') or
        xml:match("<link[^>]+href='([^']*)'") or ""
    return {
        title = decodeText(xml:match("<title[^>]*>(.-)</title>")),
        link = decodeText(link),
        date = decodeText(elementText(xml, "published") or
            elementText(xml, "updated")),
    }
end

local function parseFeed(body, maximum)
    local title = decodeText(body:match("<channel%s*>.-<title[^>]*>(.-)</title>") or
        body:match("<channel%s+[^>]*>.-<title[^>]*>(.-)</title>") or
        body:match("<feed[^>]*>.-<title[^>]*>(.-)</title>"))
    local items = {}
    local atom = false
    for xml in body:gmatch("<entry[^>]*>(.-)</entry>") do
        atom = true
        local item = parseAtomEntry(xml)
        if item.title ~= "" then items[#items + 1] = item end
        if #items >= maximum then break end
    end
    if not atom then
        for xml in body:gmatch("<item[^>]*>(.-)</item>") do
            local item = parseRssItem(xml)
            if item.title ~= "" then items[#items + 1] = item end
            if #items >= maximum then break end
        end
    end
    return title, items
end

local function loadPreview(model)
    model.feedTitle = storage.get("previewFeedTitle") or ""
    for index = 1, 12 do
        local title = storage.get("previewArticle" .. index .. "Title")
        if not title or title == "" then break end
        model.articles[#model.articles + 1] = {
            title = title,
            date = "",
            displayDate = storage.get(
                "previewArticle" .. index .. "Date") or "",
            link = storage.get("previewArticle" .. index .. "Link") or "",
            description = storage.get(
                "previewArticle" .. index .. "Description") or "",
        }
    end
end

local function fetch(model, bypassCache, useBrowserAgent)
    if model.requestId then return end
    local cfg = config()
    if cfg.url == "" then return end
    local arguments = {
        url = cfg.url,
        timeoutMs = 15000,
        cacheSeconds = bypassCache and 0 or 120,
        maxBytes = responseLimit(),
    }
    if widget.hasFeature("task.network.headers") then
        arguments.headers = {
            Accept = "application/rss+xml, application/atom+xml, " ..
                "application/xml, text/xml, */*",
        }
        if useBrowserAgent then
            arguments.headers["User-Agent"] =
                "Mozilla/5.0 (Windows NT 10.0; Win64; x64) " ..
                "AppleWebKit/537.36 (KHTML, like Gecko) SnowDesktopRSS/1.0"
        end
    end
    local requestId, requestError = task.start("network.request", arguments)
    model.responseLimitMiB = arguments.maxBytes / (1024 * 1024)
    model.errorStatus = nil
    model.requestUsedBrowserAgent = useBrowserAgent == true
    if requestId then
        model.requestId = requestId
        model.loading = true
        model.lastFetchMs = time.monotonic()
        model.error = nil
    else
        model.loading = false
        model.error = requestError or "requestRejected"
    end
end

local function clear(model)
    model.articles = {}
    model.feedTitle = ""
    model.error = nil
    model.errorStatus = nil
    interaction.setScrollOffset("rss.scroll", 0)
end

local function errorText(model)
    if model.error == "emptyResponse" then
        return l10n.tr("lua_widget.rss_reader.empty_response")
    elseif model.error == "parseFailed" then
        return l10n.tr("lua_widget.rss_reader.no_articles_parsed")
    elseif model.error == "responseTooLarge" then
        return l10n.tr("lua_widget.rss_reader.response_too_large",
            tostring(math.floor(model.responseLimitMiB or 1)))
    elseif model.error == "httpStatus" and model.errorStatus then
        return l10n.tr("lua_widget.rss_reader.http_error",
            tostring(model.errorStatus))
    elseif model.error == "permissionDenied" or
        model.error == "permissionRevoked" then
        return l10n.tr("lua_widget.rss_reader.network_permission_required")
    elseif model.error == "redirectRejected" then
        return l10n.tr("lua_widget.rss_reader.redirect_rejected")
    end
    return l10n.tr("lua_widget.rss_reader.request_failed")
end

local function registerRegion(key, shape, events, label, enabled, viewport)
    if viewport then
        local left, top = math.max(shape.x, viewport.x), math.max(shape.y, viewport.y)
        local right = math.min(shape.x + shape.width, viewport.x + viewport.width)
        local bottom = math.min(shape.y + shape.height, viewport.y + viewport.height)
        if right <= left or bottom <= top then return end
        if left ~= shape.x or top ~= shape.y or right ~= shape.x + shape.width or bottom ~= shape.y + shape.height then
            shape = { type = "rect", x = left, y = top, width = right - left, height = bottom - top }
        end
    end
    interaction.region({
        key = key, shape = shape,
        cursor = enabled == false and "default" or "hand",
        enabled = enabled ~= false,
        events = events,
        accessibility = { role = "button", label = label },
    })
end

local function setup(context)
    local model = {
        articles = {}, feedTitle = "", loading = false,
        error = nil, requestId = nil, lastFetchMs = 0,
    }
    widget.setTitle(l10n.tr("lua_widget.rss_reader.name"))
    schedule.every("rss-refresh", 60000, { whenHidden = "pause" })
    if context.preview then loadPreview(model) else fetch(model, false) end
    return model
end

local function drawCenteredStatus(text, x, y, width, height, font, color,
    unit, alpha)
    local measured = draw.measureText(text, font, 0, true)
    local measuredWidth = math.min(width, measured.width)
    draw.text(x + math.max(0, (width - measuredWidth) / 2),
        y + math.max(0, (height - measured.height) / 2),
        text, font, color, math.max(unit, measuredWidth + unit),
        true, true, 0, alpha or 1.0)
end

local function render(context, model)
    local cfg = config()
    local colors = palette(context)
    local width = layout.contentWidth()
    local height = layout.contentHeight()
    local metrics = componentMetrics()
    local unit = metrics.strokeWidth
    local pad = metrics.spacingMd
    local headerFont = metrics.titleFontSize
    local countFont = metrics.controlFontSize
    local bodyFont = metrics.bodyFontSize * cfg.rowScale
    local smallFont = metrics.captionFontSize * cfg.rowScale
    local title = model.feedTitle ~= "" and model.feedTitle or "RSS"
    widget.setTitle(model.feedTitle ~= "" and model.feedTitle or
        l10n.tr("lua_widget.rss_reader.name"))

    local countText = l10n.tr("lua_widget.rss_reader.article_count",
        tostring(#model.articles))
    local countMetrics = draw.measureText(countText, countFont, width, false)
    local headerTop = math.min(metrics.spacingSm,
        math.max(0, height - unit))
    local headerHeight = math.min(metrics.layoutRowHeight,
        math.max(unit, height - headerTop))
    local titleMetrics = draw.measureText(title, headerFont, width, true)
    draw.text(pad, headerTop + math.max(0,
            (headerHeight - titleMetrics.height) / 2),
        title, headerFont, colors.header,
        math.max(unit, width - pad * 2 - countMetrics.width -
            metrics.spacingSm),
        false, true)
    draw.text(width - pad - countMetrics.width,
        headerTop + math.max(0,
            (headerHeight - countMetrics.height) / 2),
        countText, countFont, colors.count, countMetrics.width + unit,
        false, true)
    local headerBottom = headerTop + headerHeight
    draw.line(pad, headerBottom, width - pad, headerBottom,
        metrics.strokeWidth, colors.divider, 0.10)

    local errorHeight = model.error and #model.articles > 0 and
        metrics.layoutRowHeight or 0
    if errorHeight > 0 then
        draw.text(pad, headerBottom + metrics.spacingXs,
            errorText(model), smallFont, colors.error,
            width - pad * 2, false, true)
    end
    local listTop = math.min(height,
        headerBottom + metrics.spacingXs + errorHeight)
    local listBottom = height - metrics.spacingXs
    local viewportHeight = math.max(unit, listBottom - listTop)
    local viewport = { type = "rect", x = pad, y = listTop,
        width = width - pad * 2, height = viewportHeight }
    local scrollViewport = { type = "rect", x = pad, y = listTop,
        width = width - pad, height = viewportHeight }
    interaction.region({
        key = "rss.surface", shape = viewport,
        events = { contextMenu = {
            id = "rss.menu", scope = "component" } },
        accessibility = { role = "list", label = descriptor.name },
    })

    if model.loading and #model.articles == 0 then
        draw.text(pad, listTop + viewportHeight * 0.34,
            l10n.tr("lua_widget.rss_reader.loading"),
            bodyFont, colors.status,
            width - pad * 2, true, true)
        return
    end
    if model.error and #model.articles == 0 then
        local message = errorText(model)
        local measured = draw.measureText(message, bodyFont, width - pad * 2)
        draw.text(pad, listTop + math.max(0,
                (viewportHeight - measured.height) / 2),
            message,
            bodyFont, colors.error,
            width - pad * 2, false, false, viewportHeight)
        return
    end
    if #model.articles == 0 then
        drawCenteredStatus(
            l10n.tr("lua_widget.rss_reader.no_articles"),
            pad, listTop, width - pad * 2, viewportHeight,
            headerFont, colors.status, unit, 0.78)
        return
    end

    local rowPadding = metrics.spacingXs * cfg.rowScale
    local rowTextGap = math.max(unit,
        metrics.spacingXs * 0.5 * cfg.rowScale)
    local bodyMetrics = draw.measureText("Ag", bodyFont, 0, false)
    local smallMetrics = draw.measureText("Ag", smallFont, 0, false)
    local itemHeight = math.max(
        metrics.layoutRowHeight * 1.35 * cfg.rowScale,
        rowPadding * 2 + bodyMetrics.height + rowTextGap +
            smallMetrics.height)
    local itemGap = math.max(unit,
        metrics.spacingXs * math.min(cfg.rowScale, 1.25))
    local rowHeight = itemHeight + itemGap
    local scroll = interaction.scroll({
        key = "rss.scroll", shape = scrollViewport,
        contentHeight = math.ceil(#model.articles * rowHeight - itemGap),
    })
    local first = math.max(1, math.floor(scroll.offset / rowHeight) + 1)
    local last = math.min(#model.articles,
        math.ceil((scroll.offset + viewportHeight) / rowHeight))
    local textX = pad
    local textWidth = math.max(unit, width - pad * 2)
    draw.pushClip(pad, listTop, width - pad * 2, viewportHeight)
    for index = first, last do
        local article = model.articles[index]
        local y = listTop + (index - 1) * rowHeight - scroll.offset
        local key = "rss.article." .. tostring(index)
        local hovered = interaction.isHovered(key)
        if hovered then
            draw.rect(pad, y, width - pad * 2,
                itemHeight,
                colors.card, metrics.controlRadius, 0.07)
        end
        registerRegion(key, { type = "roundedRect", x = pad, y = y,
            width = width - pad * 2,
            height = itemHeight,
            radius = metrics.controlRadius }, {
            doubleClick = { id = "rss.open", value = article.link },
            contextMenu = { id = "rss.menu", value = article.link },
        }, article.title, article.link ~= "", scrollViewport)
        local titleY = y + rowPadding
        if hovered then
            draw.marqueeText({
                key = key .. ".title",
                x = textX,
                y = titleY,
                width = textWidth,
                height = bodyMetrics.height,
                text = article.title,
                size = bodyFont,
                color = colors.title,
                speed = 24,
                gap = math.max(metrics.spacingLg,
                    metrics.spacingLg * cfg.rowScale),
            })
        else
            draw.text(textX, titleY, article.title,
                bodyFont, colors.title,
                textWidth, false, true)
        end
        local shortDate = article.displayDate or
            article.date:match("(%d%d? .%l%l%l? %d%d%d%d)") or
            article.date:sub(1, 16)
        if shortDate == "" then shortDate = article.link:sub(1, 42) end
        local number = tostring(index)
        local currentNumberMetrics = draw.measureText(number,
            smallFont, 0, true)
        local metadataGap = math.max(unit,
            metrics.spacingSm * cfg.rowScale)
        local dateWidth = math.max(unit,
            textWidth - currentNumberMetrics.width - metadataGap)
        local metadataY = y + itemHeight - rowPadding - smallMetrics.height
        draw.text(textX,
            metadataY,
            shortDate,
            smallFont, colors.date, dateWidth, false, true)
        draw.text(textX + textWidth - currentNumberMetrics.width,
            metadataY, number, smallFont, colors.number,
            currentNumberMetrics.width, true, true)
        draw.line(textX, y + itemHeight - unit, width - pad,
            y + itemHeight - unit, metrics.strokeWidth,
            colors.divider, 0.07)
    end
    draw.popClip()
end

local function openUri(model, url)
    if not url or url == "" or not widget.hasPermission("shell.launch") then
        return
    end
    local taskId, taskError = task.start("shell.openUri", { url = url })
    if taskId then
        model.openTaskId = taskId
    else
        widget.log("warn", "shell.openUri rejected: " .. tostring(taskError))
    end
end

local function event(_context, model, value)
    if value.kind == "environment" then
        widget.setTitle(model.feedTitle ~= "" and model.feedTitle or
            l10n.tr("lua_widget.rss_reader.name"))
        return
    elseif value.kind == "schedule" and value.id == "rss-refresh" then
        local cfg = config()
        if not model.requestId and
            time.monotonic() - model.lastFetchMs >= cfg.interval * 1000 then
            fetch(model, false)
        end
        return
    elseif value.kind == "task.complete" then
        if value.taskId == model.requestId then
            model.requestId = nil
            model.loading = false
            model.errorStatus = nil
            if value.ok and value.value then
                if not value.value.body or value.value.body == "" then
                    model.error = "emptyResponse"
                else
                    local feedTitle, articles = parseFeed(
                        value.value.body, config().maxItems)
                    model.error = #articles == 0 and "parseFailed" or nil
                    if #articles > 0 then
                        model.feedTitle = feedTitle
                        model.articles = articles
                        interaction.setScrollOffset("rss.scroll", 0)
                    end
                end
            else
                model.error = value.error or "networkError"
                model.errorStatus = tonumber(value.status)
                if model.error == "httpStatus" and
                    (model.errorStatus == 403 or model.errorStatus == 406) and
                    not model.requestUsedBrowserAgent and
                    widget.hasFeature("task.network.headers") then
                    fetch(model, true, true)
                end
            end
        elseif value.taskId == model.openTaskId then
            model.openTaskId = nil
            if not value.ok then
                widget.log("warn", "shell.openUri failed: " ..
                    tostring(value.error))
            end
        end
        return
    elseif value.kind ~= "action" then
        return
    end

    local url = value.value and tostring(value.value) or nil
    if value.id == "rss.refresh" then
        fetch(model, true)
    elseif value.id == "rss.clear" then
        clear(model)
        fetch(model, true)
    elseif value.id == "rss.open" then
        openUri(model, url)
    elseif value.id == "rss.openSource" then
        openUri(model, config().url)
    end
end

local function menu(_context, _model, request)
    if request.id ~= "rss.menu" then return nil end
    local articleUrl = request.value and tostring(request.value) or nil
    local canOpen = widget.hasPermission("shell.launch")
    if articleUrl and articleUrl ~= "" then
        return ui.menu({
            { id = "rss.open",
                label = l10n.tr("lua_widget.rss_reader.open_article"),
                icon = fluent.open, iconFont = "fluent",
                enabled = canOpen },
        })
    end
    local items = {
        { id = "rss.refresh",
            label = l10n.tr("lua_widget.rss_reader.refresh_now"),
            icon = fluent.refresh, iconFont = "fluent" },
        { id = "rss.clear",
            label = l10n.tr("lua_widget.rss_reader.clear_cache"),
            icon = fluent.clear, iconFont = "fluent" },
    }
    items[#items + 1] = { type = "separator" }
    items[#items + 1] = { id = "rss.openSource",
        label = l10n.tr("lua_widget.rss_reader.open_source"),
        icon = fluent.open, iconFont = "fluent", enabled = canOpen }
    return ui.menu(items)
end

local function dispose(_context, model)
    if model.requestId then task.cancel(model.requestId) end
    if model.openTaskId then task.cancel(model.openTaskId) end
    schedule.cancel("rss-refresh")
end

descriptor = {
    name = l10n.tr("lua_widget.rss_reader.name"),
    useCustomStyle = true,
    followPersonalizationDefault = true,
    bottomBarHover = false,
    bg = 0x0F172A,
    border = 0xFFFFFF,
    alpha = 0.38,
    borderAlpha = 0.16,
    gradientEndA = 0.28,
    settings = settings,
    setup = setup,
    render = render,
    event = event,
    menu = menu,
    dispose = dispose,
}

return widget.define(descriptor)
