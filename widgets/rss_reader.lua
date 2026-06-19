name = "RSS 阅读器"
useCustomStyle = true
bottomBarHover = true

bg = 0x111820
border = 0x355166
alpha = 0.96
gradientEndA = 0.45

local feedUrl = "https://hnrss.org/frontpage"
local titles = {}
local loading = false
local lastError = ""

local function fetch()
    if loading then return end
    loading = true
    local id = http.request({
        url = feedUrl,
        method = "GET",
        timeoutMs = 10000,
        cacheSeconds = 300,
        headers = { ["User-Agent"] = "SnowDesktop RSS Widget" }
    })
    if not id then
        loading = false
        lastError = "请求未启动，请检查权限和域名白名单"
    end
end

function onVisible()
    widget.setTimer("rss-refresh", 300000, true)
    fetch()
end

function onHidden()
    widget.cancelTimer("rss-refresh")
end

function onTimer(name)
    if name == "rss-refresh" then fetch() end
end

function onHttpResponse(id, response)
    loading = false
    if not response.ok then
        lastError = response.error ~= "" and response.error or ("HTTP " .. tostring(response.status))
        return
    end
    local parsed = {}
    local firstTitle = true
    for title in string.gmatch(response.body, "<title><!%[CDATA%[(.-)%]%]></title>") do
        if firstTitle then firstTitle = false else parsed[#parsed + 1] = title end
        if #parsed >= 20 then break end
    end
    if #parsed == 0 then
        firstTitle = true
        for title in string.gmatch(response.body, "<title>(.-)</title>") do
            if firstTitle then firstTitle = false else parsed[#parsed + 1] = title end
            if #parsed >= 20 then break end
        end
    end
    titles = parsed
    lastError = #titles == 0 and "未解析到文章" or ""
end

function render()
    local w = layout.width()
    local h = layout.height()
    local pad = 14
    draw.text(pad, 12, "RSS · Hacker News", 15, 0xFFFFFF, w - pad * 2, true, true)

    if loading and #titles == 0 then
        draw.text(pad, 48, "正在获取…", 13, 0x92A9B7)
        return
    end
    if lastError ~= "" and #titles == 0 then
        draw.text(pad, 48, lastError, 12, 0xFF8B8B, w - pad * 2)
        return
    end

    local top = 42
    local bottom = h - 28
    local itemHeight = 30
    local visible = ui.virtualList("articles", pad, top, w - pad * 2,
        bottom - top, itemHeight, #titles)
    for i = visible.first, visible.last do
        local y = top + (i - visible.first) * itemHeight
        draw.text(pad, y + 5, tostring(i) .. ". " .. titles[i], 12,
            0xD7E4EB, w - pad * 2, false, true)
        draw.line(pad, y + itemHeight - 1, w - pad, y + itemHeight - 1,
            1, 0x2C3C47, 0.8)
    end
end
