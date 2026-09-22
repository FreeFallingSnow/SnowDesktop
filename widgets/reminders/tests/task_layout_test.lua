local taskLayout = module.require("modules/task_layout.lua")

local function fixture()
    local heights = { Short = 14, Wrapped = 56, Last = 28 }
    return taskLayout.build({
        { id = "1", text = "Short" },
        { id = "2", text = "Wrapped" },
        { id = "3", text = "Last" },
    }, function(text) return { width = 80, height = heights[text] } end,
        100, 12, 28, 4, 4, "Untitled")
end

return {
    ["wrapped content fits its card and pushes following tasks down"] = function()
        local rows, height = fixture()
        assert(rows[1].height == 28)
        assert(rows[2].height == 64 and rows[3].top == 100)
        assert(height == 136, "scroll extent must include the full last task")
    end,

    ["scrolling inside a tall task keeps that task visible"] = function()
        local rows = fixture()
        local first, last = taskLayout.visibleRange(rows, 60, 24)
        assert(first == 2 and last == 2,
            "fixed-height indexing would skip the visible wrapped task")
    end,

    ["scroll end reaches the final task without a trailing blank row"] = function()
        local rows, height = fixture()
        local first, last = taskLayout.visibleRange(rows, height - 32, 32)
        assert(first == 3 and last == 3)
        assert(rows[last].top + rows[last].height == height)
    end,

    ["a viewport wholly in a row gap has no visible item"] = function()
        local rows = fixture()
        local first, last = taskLayout.visibleRange(rows, 28, 4)
        assert(first > last, "hidden rows must not register clickable controls")
    end,

    ["width and font changes remeasure the actual text"] = function()
        local tasks = { { id = "1", text = "Long task" } }
        local function measure(text, fontSize, width)
            assert(text == "Long task")
            local height = width == 80 and 56 or 28
            return { width = width, height = height * fontSize / 12 }
        end
        local narrow = taskLayout.build(tasks, measure, 80, 12, 28, 4, 4, "")
        local wide = taskLayout.build(tasks, measure, 160, 12, 28, 4, 4, "")
        local larger = taskLayout.build(tasks, measure, 80, 18, 28, 4, 4, "")
        assert(narrow[1].height == 64 and wide[1].height == 36)
        assert(larger[1].height == 92)
    end,

    ["Enter reserves the final empty line without changing stored text"] = function()
        local tasks = { { id = "1", text = "First\nSecond\n" } }
        local rows = taskLayout.build(tasks, function(text)
            assert(text == "First\nSecond\n ")
            return { width = 60, height = 42 }
        end, 100, 12, 28, 4, 4, "Untitled")
        assert(rows[1].height == 50)
        assert(rows[1].text == "First\nSecond\n" and tasks[1].text == rows[1].text)
    end,
}
