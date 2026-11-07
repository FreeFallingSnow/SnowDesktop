local M={}
local function label(value)
    value=type(value)=="string" and value:lower():gsub("[%s%c]","") or ""
    if not utf8.len(value) then return value end
    local text={}
    for _,code in utf8.codes(value) do
        if code~=0xA0 and code~=0xAD and code~=0xFEFF and code~=0x3000 and code~=0x202F and code~=0x205F and
            not (code>=0x2000 and code<=0x200F) and not (code>=0x202A and code<=0x202E) and
            not (code>=0x2060 and code<=0x206F) then text[#text+1]=utf8.char(code) end
    end
    return table.concat(text)
end

-- Complete application labels and executable names. Short names that may also
-- be artists require the matching browser source (third column).
local browsers={
    {"edge",{"MSEdge","msedge.exe","Microsoft Edge","Microsoft Edge Beta","Microsoft Edge Dev","Microsoft Edge Canary"}},
    {"chrome",{"Chrome","chrome.exe","Google Chrome","Google Chrome Beta","Google Chrome Dev","Google Chrome Canary"}},
    {"chromium",{"Chromium","chromium.exe"}},
    {"firefox",{"Firefox","firefox.exe","Mozilla Firefox","Firefox ESR","Firefox Developer Edition","Firefox Nightly"}},
    {"brave",{"brave.exe","Brave Browser","Brave Browser Beta","Brave Browser Nightly"},{"Brave"}},
    {"opera",{"opera.exe","operagx.exe","Opera Browser","Opera GX","Opera GX Browser"},{"Opera","OperaGX"}},
    {"vivaldi",{"vivaldi.exe","Vivaldi Browser"},{"Vivaldi"}},
    {"arc",{"arc.exe","Arc Browser"},{"Arc"}},
    {"zen",{"zen.exe","Zen Browser"},{"Zen"}},
    {"waterfox",{"waterfox.exe","Waterfox Browser"},{"Waterfox"}},
    {"yandex",{"Yandex Browser","Yandex.Browser","YandexBrowser","browser.exe"},{"Yandex"}},
    {"whale",{"whale.exe","Naver Whale","Whale Browser"},{"Whale"}},
    {"qq",{"QQBrowser","QQBrowser.exe","QQ浏览器"}}, -- l10n-allow: Exact browser labels from media metadata, never displayed as component UI text.
    {"360",{"360se","360se.exe","360chrome","360chrome.exe","360ChromeX","360ChromeX.exe",
        "360 Browser","360安全浏览器","360极速浏览器","360极速浏览器X"}}, -- l10n-allow: Exact browser labels from media metadata, never displayed as component UI text.
    {"sogou",{"SogouExplorer","SogouExplorer.exe","Sogou Browser","搜狗浏览器","搜狗高速浏览器"}}, -- l10n-allow: Exact browser labels from media metadata, never displayed as component UI text.
    {"quark",{"Quark Browser","夸克浏览器","quark.exe"},{"Quark"}}, -- l10n-allow: Exact browser labels from media metadata, never displayed as component UI text.
    {"uc",{"UC Browser","UCBrowser.exe","UC浏览器"}}, -- l10n-allow: Exact browser labels from media metadata, never displayed as component UI text.
    {"maxthon",{"Maxthon Browser","Maxthon.exe","傲游浏览器","遨游浏览器"},{"Maxthon"}}, -- l10n-allow: Exact browser labels from media metadata, never displayed as component UI text.
    {"duckduckgo",{"DuckDuckGo Browser","DuckDuckGo.exe"},{"DuckDuckGo"}},
    {"electron",{"Electron","Electron.exe"}},
}
local names,shorts={},{}
for _,browser in ipairs(browsers) do
    for _,value in ipairs(browser[2]) do names[label(value)]=browser[1] end
    for _,value in ipairs(browser[3] or {}) do shorts[label(value)]=browser[1] end
end
local function appIdFamily(name)
    if name:match("^microsoft%.microsoftedge[._]") then return "edge" end
    if name:match("^thebrowsercompany%.arc[_%.]") then return "arc" end
    if name:match("^duckduckgo%.desktopbrowser[_%.]") then return "duckduckgo" end
end
local function family(value)
    local name=label(value)
    local base=name:match("[^/\\]+$") or name
    return names[base] or shorts[base] or appIdFamily(name)
end
function M.excluded(session)
    if type(session)~="table" then return false end
    local artist,source=label(session.artist),label(session.sourceName)
    if source:match("^electron%.app%.") or artist:match("^electron%.app%.") then return true end
    local artistBase=artist
    if artist:match("^%a:[/\\]") or artist:match("^\\\\") then artistBase=artist:match("[^/\\]+$") or artist end
    if names[artistBase] or appIdFamily(artist) then return true end
    local browser=family(source)
    if browser and (shorts[artist]==browser or artist==source) then return true end
    if browser and (artist:match("^https?://") or artist:match("^www%.")) then return true end
    return false
end
function M.pendingMetadata(session)
    return type(session)=="table" and family(session.sourceName)~=nil and label(session.artist)==""
end
return M
