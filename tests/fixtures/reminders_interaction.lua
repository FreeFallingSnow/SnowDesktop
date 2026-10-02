-- Drive the actual entry's registered actions. Only host storage, drawing,
-- pointer routing and timers are substituted; no sorting or event logic is mocked.
local descriptor
local storage, l10n, widget, schedule, control, ui, layout, draw, interaction
-- The C++ preview test inserts the repository's actual main.lua in this loader.
local function loadReminders()
-- REMINDERS_ENTRY
end
local function harness(initial)
    local h = { values = initial or {}, regions = {}, controls = {}, timers = {},
        transactions = 0, offset = 0, pressed = nil, focused = nil, focusCalls = 0 }
    storage = {
        get = function(key) return h.values[key] end,
        transaction = function(callback)
            h.transactions = h.transactions + 1
            callback({
                set = function(_, key, value) h.values[key] = value end,
                remove = function(_, key) h.values[key] = nil end,
            })
        end,
    }
    l10n = { tr = function(key) return key end }
    widget = { define = function(value) return value end, setTitle = function() end,
        theme = function() return { contentTheme = 1 } end }
    schedule = {
        every = function(id) h.timers[id] = true end,
        after = function(id) h.timers[id] = true end,
        cancel = function(id) h.timers[id] = nil end,
    }
    control = {
        textArea = function(spec) h.controls[spec.key] = spec end,
        focus = function(key)
            h.focused = key
            h.focusedText = h.values.draft
            h.focusCalls = h.focusCalls + 1
            return true
        end,
        blur = function() return true end,
    }
    ui = { metrics = function() return { layoutRowHeight = 28 } end,
        menu = function(items) return items end }
    layout = { contentWidth = function() return 250 end,
        contentHeight = function() return 200 end }
    draw = {
        measureText = function(text)
            return { width = #text * 5, height = text == "wrapped" and 72 or 16 }
        end,
    }
    for _, name in ipairs({ "circle", "path", "text", "rect", "strokeRect",
        "line", "fluent", "pushClip", "popClip" }) do draw[name] = function() end end
    interaction = {
        isHovered = function() return false end,
        isPressed = function(key) return h.pressed == key end,
        region = function(spec) h.regions[spec.key] = spec end,
        scroll = function()
            return { offset = h.offset, maximum = 300 }
        end,
        setScrollOffset = function(_, offset)
            h.offset = math.max(0, math.min(300, offset))
            return h.offset
        end,
    }
    if not descriptor then descriptor = loadReminders() end
    h.model = descriptor.setup()
    function h:render()
        self.regions, self.controls = {}, {}
        descriptor.render({ selected = true }, self.model)
    end
    function h:action(key, name, x, y)
        local region = self.regions[key] or self.controls[key]
        local binding = assert(region.events[name], "action must be registered by the entry")
        if name == "pointerDown" then self.pressed = key end
        if name == "pointerUp" then self.pressed = nil end
        descriptor.event({}, self.model, { kind = "action", id = binding.id,
            value = binding.value, action = name, targetKey = key,
            x = x, y = y, button = 1, text = self.values.draft })
    end
    function h:start(id)
        self:render()
        local key = "task.row." .. id
        local shape = self.regions[key].shape
        local x, y = shape.x + shape.width / 2, shape.y + shape.height / 2
        self:action(key, "pointerDown", x, y)
        return key, x, y
    end
    function h:menu(taskId)
        return descriptor.menu({}, self.model, { id = "task.menu", value = taskId })
    end
    function h:command(id, value)
        descriptor.event({}, self.model, { kind = "action", id = id, value = value })
        self:render()
    end
    return h
end

local function initial()
    return { order = "1,2,3", task_1_text = "urgent", task_2_text = "normal",
        task_3_text = "low", priorities = "1:3,3:0" }
end

-- Module loading belongs to entry evaluation, including cached modules.
harness({})

return {
    ["submit creates once and refocuses the cleared input for consecutive tasks"] = function()
        local h = harness({ draft = "  first\nsecond  " })
        h:render()
        h:action("new-task", "submit")
        assert(h.values.task_1_text == "first\nsecond" and h.values.order == "1")
        assert(h.values.draft == nil and h.values.nextId == "2" and h.transactions == 1)
        assert(h.focused == "new-task" and h.focusedText == nil and h.focusCalls == 1)
        h:action("new-task", "submit")
        assert(h.transactions == 1 and h.values.task_2_text == nil)
        assert(h.focusCalls == 2, "an empty submit must keep keyboard entry available")
        h.values.draft = "third"
        h:action("new-task", "submit")
        assert(h.values.task_2_text == "third" and h.values.order == "1,2")
        assert(h.focusedText == nil and h.focusCalls == 3)
    end,
    ["drag commits once, survives reload, and leaves completion and urgency intact"] = function()
        local values = initial()
        values.doneIds = "2"
        local h = harness(values)
        local key, x = h:start("3")
        assert(h.regions[key].capturePointer)
        h:action(key, "pointerMove", x, h.model.viewport.y + 1)
        assert(h.transactions == 0, "pointer motion must not persist intermediate order")
        h:render()
        h:action(key, "pointerUp", x, h.model.viewport.y + 1)
        assert(h.transactions == 1 and h.values.order == "3,1,2")
        assert(h.values.doneIds == "2" and h.values.priorities == "1:3,3:0")
        assert(h.values.task_3_text == "low")
        local reloaded = harness(h.values)
        reloaded:render()
        assert(reloaded.model.rows[1].task.id == "3", "priority sorting must not undo the drop")
    end,
    ["small movement preserves click selection without saving order"] = function()
        local h = harness(initial())
        local key, x, y = h:start("1")
        h:action(key, "pointerMove", x + 1, y + 1)
        h:action(key, "pointerUp", x + 1, y + 1)
        h:action(key, "click", x, y)
        assert(h.model.selectedId == "1" and h.transactions == 0)
    end,
    ["outside release, resize, hidden surface and lost capture cancel without writes"] = function()
        for _, reason in ipairs({ "outside", "resize", "hidden", "capture", "timeout" }) do
            local h = harness(initial())
            local key, x = h:start("3")
            h:action(key, "pointerMove", x, h.model.viewport.y + 1)
            h:render()
            if reason == "outside" then h:action(key, "pointerUp", -1, -1)
            elseif reason == "capture" then h.pressed = nil; h:action(key, "pointerMove", x, 50)
            else descriptor.event({}, h.model,
                reason == "timeout" and { kind = "schedule", id = "tasks.dragTimeout" } or
                reason == "hidden" and { kind = "visibility", visible = false } or
                { kind = "resize" }) end
            assert(h.model.drag == nil and h.transactions == 0, reason .. " must not save a drop")
            assert(next(h.timers) == nil)
        end
    end,
    ["hidden tasks remain stored and auto-scroll retains the captured stable key"] = function()
        local values = initial()
        values.showCompleted, values.doneIds = "0", "2"
        local h = harness(values)
        local key, x = h:start("3")
        h:action(key, "pointerMove", x, h.model.viewport.y + h.model.viewport.height - 1)
        descriptor.event({}, h.model, { kind = "schedule", id = "tasks.dragScroll" })
        h:render()
        assert(h.offset > 0 and h.regions[key].capturePointer)
        assert(h.regions[key].shape == h.model.viewport)
        h:action(key, "pointerUp", x, h.model.viewport.y + 1)
        assert(h.values.task_2_text == "normal" and h.values.doneIds == "2")
    end,
    ["completion stays at the end after manual moves and reopening"] = function()
        local h = harness(initial())
        h:command("task.toggle", "1")
        assert(h.model.rows[3].task.id == "1" and h.values.doneIds == "1")
        local key, x = h:start("1")
        h:action(key, "pointerMove", x, h.model.viewport.y + 1)
        h:action(key, "pointerUp", x, h.model.viewport.y + 1)
        h:render()
        assert(h.model.rows[3].task.id == "1", "completed rows cannot cross into pending rows")
        h:command("task.toggle", "1")
        assert(h.model.rows[1].task.id == "1" and h.values.doneIds == nil)
    end,
    ["one-off urgency sorting saves the order without enabling automatic sorting"] = function()
        local values = initial()
        values.order, values.doneIds = "3,2,1", "1"
        local h = harness(values)
        h:render()
        h:command("task.sortPriority")
        assert(h.values.order == "2,3,1" and h.values.autoSort ~= "1")
        local reloaded = harness(h.values)
        reloaded:render()
        assert(reloaded.model.rows[1].task.id == "2" and reloaded.model.rows[3].task.id == "1")
        reloaded:command("task.priority.3", "3")
        assert(reloaded.model.rows[1].task.id == "2", "manual order survives urgency edits")
    end,
    ["automatic urgency sorting blocks drag and disabling retains the current order"] = function()
        local values = initial()
        values.order = "3,2,1"
        local h = harness(values)
        h:render()
        h:command("task.autoSort")
        assert(h.values.autoSort == "1" and h.model.rows[1].task.id == "1")
        local row = h.regions["task.row.1"]
        assert(not row.capturePointer and row.events.pointerDown == nil and row.events.click)
        local before = h.transactions
        h:command("task.dragStart", "1")
        h:command("task.sortPriority")
        assert(h.model.drag == nil and h.transactions == before and h.values.order == "3,2,1")
        h:command("task.priority.3", "3")
        assert(h.model.rows[1].task.id == "3" and h.model.rows[2].task.id == "1", "urgency ties are stable")
        local reloaded = harness(h.values)
        reloaded:render()
        assert(reloaded.regions["task.row.3"].events.pointerDown == nil)
        reloaded:command("task.autoSort")
        assert(reloaded.values.autoSort == "0" and reloaded.values.order == "3,1,2")
        assert(reloaded.regions["task.row.3"].capturePointer)
        reloaded:command("task.priority.0", "3")
        assert(reloaded.model.rows[1].task.id == "3", "disabling automatic sorting must preserve its last order")
    end,
    ["task and background menus expose sort modes and urgency glyphs"] = function()
        local h = harness(initial())
        h:render()
        local function find(items, id)
            for _, item in ipairs(items) do if item.id == id then return item end end
        end
        local items = h:menu("1")
        assert(items[2].icon and items[2].iconFont == "fluent")
        for _, item in ipairs(items[2].children) do assert(item.icon and item.iconFont == "fluent") end
        for _, taskId in ipairs({ false, "1" }) do
            items = h:menu(taskId or nil)
            assert(find(items, "task.sortPriority").enabled)
            assert(not find(items, "task.autoSort").checked)
        end
        h:command("task.autoSort")
        assert(not find(h:menu("1"), "task.sortPriority").enabled)
        assert(find(h:menu(), "task.autoSort").checked)
    end,
}
