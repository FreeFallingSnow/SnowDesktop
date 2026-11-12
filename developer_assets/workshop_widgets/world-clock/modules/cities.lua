local cities = {
    -- East Asia
    { id = "beijing", zone = "China Standard Time",
        label = l10n.tr("lua_widget.world_clock.beijing") },
    { id = "shanghai", zone = "China Standard Time", legacy = true,
        label = l10n.tr("lua_widget.world_clock.shanghai") },
    { id = "guangzhou", zone = "China Standard Time",
        label = l10n.tr("lua_widget.world_clock.guangzhou") },
    { id = "shenzhen", zone = "China Standard Time",
        label = l10n.tr("lua_widget.world_clock.shenzhen") },
    { id = "chengdu", zone = "China Standard Time",
        label = l10n.tr("lua_widget.world_clock.chengdu") },
    { id = "hong_kong", zone = "China Standard Time",
        label = l10n.tr("lua_widget.world_clock.hong_kong") },
    { id = "taipei", zone = "Taipei Standard Time",
        label = l10n.tr("lua_widget.world_clock.taipei") },
    { id = "tokyo", zone = "Tokyo Standard Time", legacy = true,
        label = l10n.tr("lua_widget.world_clock.tokyo") },
    { id = "seoul", zone = "Korea Standard Time", legacy = true,
        label = l10n.tr("lua_widget.world_clock.seoul") },
    -- Southeast Asia
    { id = "singapore", zone = "Singapore Standard Time", legacy = true,
        label = l10n.tr("lua_widget.world_clock.singapore") },
    { id = "kuala_lumpur", zone = "Singapore Standard Time",
        label = l10n.tr("lua_widget.world_clock.kuala_lumpur") },
    { id = "bangkok", zone = "SE Asia Standard Time",
        label = l10n.tr("lua_widget.world_clock.bangkok") },
    { id = "ho_chi_minh_city", zone = "SE Asia Standard Time",
        label = l10n.tr("lua_widget.world_clock.ho_chi_minh_city") },
    { id = "jakarta", zone = "SE Asia Standard Time",
        label = l10n.tr("lua_widget.world_clock.jakarta") },
    { id = "manila", zone = "Singapore Standard Time",
        label = l10n.tr("lua_widget.world_clock.manila") },
    -- South and West Asia
    { id = "new_delhi", zone = "India Standard Time", legacy = true,
        label = l10n.tr("lua_widget.world_clock.new_delhi") },
    { id = "mumbai", zone = "India Standard Time",
        label = l10n.tr("lua_widget.world_clock.mumbai") },
    { id = "kathmandu", zone = "Nepal Standard Time",
        label = l10n.tr("lua_widget.world_clock.kathmandu") },
    { id = "dhaka", zone = "Bangladesh Standard Time",
        label = l10n.tr("lua_widget.world_clock.dhaka") },
    { id = "karachi", zone = "Pakistan Standard Time",
        label = l10n.tr("lua_widget.world_clock.karachi") },
    { id = "dubai", zone = "Arabian Standard Time", legacy = true,
        label = l10n.tr("lua_widget.world_clock.dubai") },
    { id = "riyadh", zone = "Arab Standard Time",
        label = l10n.tr("lua_widget.world_clock.riyadh") },
    { id = "istanbul", zone = "Turkey Standard Time",
        label = l10n.tr("lua_widget.world_clock.istanbul") },
    -- Europe
    { id = "london", zone = "GMT Standard Time", legacy = true,
        label = l10n.tr("lua_widget.world_clock.london") },
    { id = "lisbon", zone = "GMT Standard Time",
        label = l10n.tr("lua_widget.world_clock.lisbon") },
    { id = "paris", zone = "Romance Standard Time", legacy = true,
        label = l10n.tr("lua_widget.world_clock.paris") },
    { id = "amsterdam", zone = "W. Europe Standard Time",
        label = l10n.tr("lua_widget.world_clock.amsterdam") },
    { id = "berlin", zone = "W. Europe Standard Time",
        label = l10n.tr("lua_widget.world_clock.berlin") },
    { id = "madrid", zone = "Romance Standard Time",
        label = l10n.tr("lua_widget.world_clock.madrid") },
    { id = "rome", zone = "W. Europe Standard Time",
        label = l10n.tr("lua_widget.world_clock.rome") },
    { id = "stockholm", zone = "W. Europe Standard Time",
        label = l10n.tr("lua_widget.world_clock.stockholm") },
    { id = "helsinki", zone = "FLE Standard Time",
        label = l10n.tr("lua_widget.world_clock.helsinki") },
    { id = "athens", zone = "GTB Standard Time",
        label = l10n.tr("lua_widget.world_clock.athens") },
    { id = "moscow", zone = "Russian Standard Time",
        label = l10n.tr("lua_widget.world_clock.moscow") },
    -- Africa
    { id = "cairo", zone = "Egypt Standard Time",
        label = l10n.tr("lua_widget.world_clock.cairo") },
    { id = "nairobi", zone = "E. Africa Standard Time",
        label = l10n.tr("lua_widget.world_clock.nairobi") },
    { id = "johannesburg", zone = "South Africa Standard Time",
        label = l10n.tr("lua_widget.world_clock.johannesburg") },
    { id = "lagos", zone = "W. Central Africa Standard Time",
        label = l10n.tr("lua_widget.world_clock.lagos") },
    { id = "casablanca", zone = "Morocco Standard Time",
        label = l10n.tr("lua_widget.world_clock.casablanca") },
    -- North America
    { id = "vancouver", zone = "Pacific Standard Time",
        label = l10n.tr("lua_widget.world_clock.vancouver") },
    { id = "san_francisco", zone = "Pacific Standard Time",
        label = l10n.tr("lua_widget.world_clock.san_francisco") },
    { id = "los_angeles", zone = "Pacific Standard Time", legacy = true,
        label = l10n.tr("lua_widget.world_clock.los_angeles") },
    { id = "denver", zone = "Mountain Standard Time",
        label = l10n.tr("lua_widget.world_clock.denver") },
    { id = "phoenix", zone = "US Mountain Standard Time",
        label = l10n.tr("lua_widget.world_clock.phoenix") },
    { id = "chicago", zone = "Central Standard Time",
        label = l10n.tr("lua_widget.world_clock.chicago") },
    { id = "toronto", zone = "Eastern Standard Time",
        label = l10n.tr("lua_widget.world_clock.toronto") },
    { id = "new_york", zone = "Eastern Standard Time", legacy = true,
        label = l10n.tr("lua_widget.world_clock.new_york") },
    { id = "st_johns", zone = "Newfoundland Standard Time",
        label = l10n.tr("lua_widget.world_clock.st_johns") },
    { id = "mexico_city", zone = "Central Standard Time (Mexico)",
        label = l10n.tr("lua_widget.world_clock.mexico_city") },
    { id = "honolulu", zone = "Hawaiian Standard Time",
        label = l10n.tr("lua_widget.world_clock.honolulu") },
    -- South America
    { id = "bogota", zone = "SA Pacific Standard Time",
        label = l10n.tr("lua_widget.world_clock.bogota") },
    { id = "sao_paulo", zone = "E. South America Standard Time",
        label = l10n.tr("lua_widget.world_clock.sao_paulo") },
    { id = "buenos_aires", zone = "Argentina Standard Time",
        label = l10n.tr("lua_widget.world_clock.buenos_aires") },
    { id = "santiago", zone = "Pacific SA Standard Time",
        label = l10n.tr("lua_widget.world_clock.santiago") },
    -- Oceania
    { id = "perth", zone = "W. Australia Standard Time",
        label = l10n.tr("lua_widget.world_clock.perth") },
    { id = "adelaide", zone = "Cen. Australia Standard Time",
        label = l10n.tr("lua_widget.world_clock.adelaide") },
    { id = "melbourne", zone = "AUS Eastern Standard Time",
        label = l10n.tr("lua_widget.world_clock.melbourne") },
    { id = "sydney", zone = "AUS Eastern Standard Time", legacy = true,
        label = l10n.tr("lua_widget.world_clock.sydney") },
    { id = "brisbane", zone = "E. Australia Standard Time",
        label = l10n.tr("lua_widget.world_clock.brisbane") },
    { id = "auckland", zone = "New Zealand Standard Time", legacy = true,
        label = l10n.tr("lua_widget.world_clock.auckland") },
}

local accents = {}
for ascii, characters in pairs({
    a = "ÀÁÂÃÄÅàáâãäå", c = "Çç", e = "ÈÉÊËèéêë", i = "ÌÍÎÏìíîï",
    n = "Ññ", o = "ÒÓÔÕÖØòóôõöø", u = "ÙÚÛÜùúûü", y = "ÝŸýÿ",
}) do
    for _, codepoint in utf8.codes(characters) do
        accents[utf8.char(codepoint)] = ascii
    end
end

local function normalize(text)
    return text:gsub(utf8.charpattern, accents):lower():gsub("’", ""):gsub("[.']", "")
        :gsub("[%s_-]+", " "):match("^%s*(.-)%s*$")
end

local byId = {}
for _, city in ipairs(cities) do
    -- Version 1.0 saved Windows zone keys for its original twelve cities.
    city.storageId = city.legacy and city.zone or city.id
    city.searchLabel = normalize(city.label)
    city.searchEnglish = normalize(city.id)
    byId[city.storageId] = city
end

local function search(text)
    local query = normalize(text)
    if query == "" then return cities end
    local results = {}
    for _, city in ipairs(cities) do
        if city.searchLabel:find(query, 1, true) or city.searchEnglish:find(query, 1, true) then
            results[#results + 1] = city
        end
    end
    return results
end

return { byId = byId, search = search }
