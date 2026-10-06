local preferences = module.require("modules/card_preferences.lua")
local cardLayout = module.require("modules/card_layout.lua")

local function ids(cards)
    local result = {}
    for _, card in ipairs(cards) do result[#result + 1] = card.id end
    return table.concat(result, ",")
end

return {
    ["moving visible cards preserves hidden cards and survives serialization"] = function()
        local cards = { { id = "cpu" }, { id = "network" }, { id = "disk_io" } }
        local function visible(id) return id == "cpu" or id == "network" or id == "disk_io" end
        local saved = preferences.move(nil, "disk_io", "first", visible)
        assert(ids(preferences.arrange(cards, saved)) == "disk_io,cpu,network")
        local all = preferences.order(saved)
        assert(all[2] == "memory" and all[3] == "gpu" and all[4] == "vram")
        saved = preferences.move(saved, "disk_io", "next", visible)
        assert(ids(preferences.arrange(cards, saved)) == "cpu,disk_io,network")
        saved = preferences.move(saved, "network", "previous", visible)
        assert(ids(preferences.arrange(cards, saved)) == "cpu,network,disk_io")
        saved = preferences.move(saved, "cpu", "last", visible)
        assert(ids(preferences.arrange(cards, saved)) == "network,disk_io,cpu")
        assert(ids(preferences.arrange(cards, nil)) == "cpu,network,disk_io")
    end,
    ["partial duplicated and unknown saved identities cannot lose a card"] = function()
        local order = preferences.order("network,network,unknown,cpu")
        assert(table.concat(order, ",") == "network,cpu,memory,gpu,vram,battery,storage,disk_io,uptime")
        assert(not preferences.customOrder({ "network" }))
        assert(not preferences.customOrder(nil))
        assert(preferences.customOrder("network"))
    end,
    ["boundary hidden and unknown move actions never propose a storage write"] = function()
        local function visible(id) return id == "cpu" or id == "memory" end
        assert(preferences.move(nil, "cpu", "previous", visible) == nil)
        assert(preferences.move(nil, "cpu", "first", visible) == nil)
        assert(preferences.move(nil, "memory", "next", visible) == nil)
        assert(preferences.move(nil, "memory", "last", visible) == nil)
        assert(preferences.move(nil, "gpu", "first", visible) == nil)
        assert(preferences.move(nil, "unknown", "first", visible) == nil)
        assert(preferences.move(nil, "memory", "invalid", visible) == nil)
    end,
    ["custom visual order cannot be undone by filling an earlier grid hole"] = function()
        local cards = { { id = "cpu" }, { id = "memory" }, { id = "gpu" }, { id = "network" } }
        local saved = "network,memory,gpu,cpu"
        cards = preferences.arrange(cards, saved)
        local sizes = { memory = "2x2", gpu = "2x1" }
        local cells = cardLayout.pack(cards, 3, function(id) return sizes[id] end, preferences.customOrder(saved))
        assert(ids(cards) == saved)
        assert(cells[1].row == 0 and cells[1].column == 0)
        assert(cells[2].row == 0 and cells[2].column == 1)
        assert(cells[3].row == 2 and cells[3].column == 0)
        assert(cells[4].row == 2 and cells[4].column == 2)
    end,
    ["font preference accepts both storage forms and bounds invalid persisted values"] = function()
        assert(preferences.fontScale(nil) == 1)
        assert(preferences.fontScale("125") == 1.25)
        assert(preferences.fontScale(70) == 0.7)
        assert(preferences.fontScale(900) == 1.5)
        assert(preferences.fontScale(-10) == 0.7)
        assert(preferences.fontScale("bad") == 1)
        assert(preferences.fontScale({}) == 1)
        assert(preferences.fontScale(math.huge) == 1)
        assert(preferences.fontScale(0 / 0) == 1)
    end,
}
