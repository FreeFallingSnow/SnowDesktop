local order = module.require("modules/task_order.lua")

local function tasks(...)
    local values = {}
    for _, id in ipairs({ ... }) do values[#values + 1] = { id = id } end
    return values
end

return {
    ["move up, down and to either end without losing tasks"] = function()
        local source = tasks("1", "2", "3", "4")
        assert(table.concat(order.move(source, "4", "1"), ",") == "4,1,2,3")
        assert(table.concat(order.move(source, "1", "4"), ",") == "2,3,1,4")
        assert(table.concat(order.move(source, "2", nil), ",") == "1,3,4,2")
        assert(source[1].id == "1", "dragging must not mutate the displayed snapshot")
    end,
    ["hidden completed tasks and priorities survive a cross-priority move"] = function()
        local source = {
            { id = "urgent", priority = 3 }, { id = "normal", priority = 1 },
            { id = "hidden", done = true }, { id = "low", priority = 0 },
        }
        assert(table.concat(order.move(source, "low", "urgent"), ",") ==
            "low,urgent,normal,hidden")
        assert(source[1].priority == 3 and source[3].done and source[4].priority == 0)
    end,
    ["same slot, self drop, deleted source and stale target make no change"] = function()
        local source = tasks("1", "2", "3")
        assert(order.move(source, "1", "2") == nil)
        assert(order.move(source, "3", nil) == nil)
        assert(order.move(source, "1", "1") == nil)
        assert(order.move(source, "gone", "1") == nil)
        assert(order.move(source, "1", "gone") == nil)
    end,
    ["wrapped row midpoints and gaps choose insertion positions"] = function()
        local rows = {
            { task = { id = "1" }, top = 0, height = 28 },
            { task = { id = "2" }, top = 32, height = 84 },
            { task = { id = "3" }, top = 120, height = 28 },
        }
        local id, marker = order.target(rows, "1", 73)
        assert(id == "2" and marker == 32)
        id, marker = order.target(rows, "1", 74)
        assert(id == "3" and marker == 120)
        id, marker = order.target(rows, "1", 200)
        assert(id == nil and marker == 148)
        id, marker = order.target({ rows[1] }, "1", 0)
        assert(id == nil and marker == nil)
    end,
    ["outside and missing pointer coordinates cannot commit a drop"] = function()
        local shape = { x = 8, y = 40, width = 200, height = 100 }
        assert(order.contains(shape, 8, 40) and order.contains(shape, 208, 140))
        assert(not order.contains(shape, 7, 40) and not order.contains(shape, 20, 141))
        assert(not order.contains(shape, nil, 40) and not order.contains(nil, 8, 40))
    end,
}
