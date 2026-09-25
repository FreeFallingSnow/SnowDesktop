local priority = module.require("modules/task_priority.lua")

local function task(id, level, done)
    return { id = id, priority = level, done = done == true }
end

local function ids(tasks)
    local result = {}
    for _, value in ipairs(tasks) do result[#result + 1] = value.id end
    return table.concat(result, ",")
end

return {
    ["pending urgency precedes completed tasks and ties retain saved order"] = function()
        local source = {
            task("normal", 1), task("done-urgent", 3, true),
            task("urgent-first", 3), task("low", 0), task("high", 2),
            task("urgent-second", 3), task("done-low", 0, true),
        }
        assert(ids(priority.sort(source)) ==
            "urgent-first,urgent-second,high,normal,low,done-urgent,done-low")
        assert(source[1].id == "normal", "sorting must not rewrite the saved order")
    end,

    ["old tasks without metadata remain normal and preserve order"] = function()
        assert(ids(priority.sort({ task("1"), task("2", 1), task("3") })) == "1,2,3")
        assert(next(priority.decode(nil)) == nil)
    end,

    ["low urgency survives storage round trips as zero"] = function()
        local raw = priority.encode({ "1", "2", "3" }, { ["1"] = 0, ["2"] = 2, ["3"] = 3 })
        local values = priority.decode(raw)
        assert(values["1"] == 0 and values["2"] == 2 and values["3"] == 3)
    end,

    ["deleted tasks and default urgency do not retain metadata"] = function()
        local raw = priority.encode({ "2", "3" }, { ["1"] = 3, ["2"] = 1, ["3"] = 2 })
        local values = priority.decode(raw)
        assert(values["1"] == nil and values["2"] == nil and values["3"] == 2)
        assert(priority.encode({ "1" }, { ["1"] = 1 }) == "")
    end,

    ["malformed persisted urgency is ignored"] = function()
        local values = priority.decode("bad,1:9,2:-1,3:1.5,4:3tail,5:0,6:2")
        assert(values["1"] == nil and values["2"] == nil and values["3"] == nil)
        assert(values["4"] == nil and values["5"] == 0 and values["6"] == 2)
    end,

    ["reopening a task restores its urgency position"] = function()
        local tasks = { task("normal", 1), task("urgent", 3, true) }
        assert(ids(priority.sort(tasks)) == "normal,urgent")
        tasks[2].done = false
        assert(ids(priority.sort(tasks)) == "urgent,normal")
    end,
}
