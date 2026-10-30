local cardLayout = module.require("modules/card_layout.lua")

local function finalRowBottom(rows, visibleRows, cardHeight, gap, inset,
        viewport, offset)
    return cardLayout.rowTop(rows - 1, visibleRows, cardHeight, gap,
        inset, viewport) - offset + cardHeight
end

return {
    ["mixed sizes fill holes beside tall cards without overlapping"] = function()
        local cards = { { id = "cpu" }, { id = "memory" },
            { id = "gpu" }, { id = "network" }, { id = "battery" } }
        local sizes = { cpu = "1x2", memory = "2x1", gpu = "2x2" }
        local cells, rows = cardLayout.pack(cards, 3, function(id) return sizes[id] end)
        assert(rows == 4)
        assert(cells[1].row == 0 and cells[1].column == 0)
        assert(cells[2].row == 0 and cells[2].column == 1)
        assert(cells[3].row == 1 and cells[3].column == 1)
        assert(cells[4].row == 2 and cells[4].column == 0)
        assert(cells[5].row == 3 and cells[5].column == 0)
    end,

    ["default and corrupt sizes retain the legacy single-cell layout"] = function()
        local cards = { { id = "cpu" }, { id = "memory" }, { id = "gpu" } }
        for _, invalid in ipairs({ "4x4", "0x0", "2x9", "", false, 22, {} }) do
            local cells, rows = cardLayout.pack(cards, 2, function() return invalid end)
            assert(rows == 2)
            assert(cells[1].row == 0 and cells[1].column == 0)
            assert(cells[2].row == 0 and cells[2].column == 1)
            assert(cells[3].row == 1 and cells[3].column == 0)
            for _, cell in ipairs(cells) do assert(cell.columns == 1 and cell.rows == 1) end
        end
        assert(cardLayout.size(nil) == "1x1")
    end,

    ["narrowing the grid fits wide cards without discarding the saved size"] = function()
        local cards, sizes = { { id = "cpu" }, { id = "memory" } }, { cpu = "2x2" }
        local function size(id) return sizes[id] end
        local narrow, rows = cardLayout.pack(cards, 1, size)
        assert(rows == 3 and narrow[1].columns == 1 and narrow[1].rows == 2)
        assert(narrow[2].row == 2)
        local wide = cardLayout.pack(cards, 3, size)
        assert(wide[1].columns == 2 and wide[2].column == 2 and wide[2].row == 0)
        assert(sizes.cpu == "2x2")
    end,

    ["resizing or hiding a card changes the scroll reset key even at equal row counts"] = function()
        local cards = { { id = "cpu" }, { id = "memory" } }
        local _, oldRows, oldKey = cardLayout.pack(cards, 3, function() return "1x1" end)
        local _, newRows, newKey = cardLayout.pack(cards, 3, function(id)
            return id == "cpu" and "2x1" or "1x1"
        end)
        assert(oldRows == newRows and oldKey ~= newKey)
        local _, _, hiddenKey = cardLayout.pack({ cards[1] }, 3, function() return "1x1" end)
        assert(hiddenKey ~= oldKey)
    end,

    ["a tall card crossing the viewport keeps adjacent rows continuous and its bottom reachable"] = function()
        local cells, rows = cardLayout.pack({ { id = "cpu" }, { id = "memory" } }, 2,
            function(id) return id == "cpu" and "1x2" or "1x1" end)
        assert(cardLayout.crossesViewport(cells, 1))
        assert(not cardLayout.crossesViewport(cells, 2))
        local top = cardLayout.rowTop(0, 1, 104, 4, 4, 125, true)
        local nextTop = cardLayout.rowTop(1, 1, 104, 4, 4, 125, true)
        assert(nextTop - top == 108)
        local content = cardLayout.contentHeight(rows, 1, 104, 4, 4, 125, true)
        local bottom = top + 104 * 2 + 4 - cardLayout.maximumOffset(content, 125)
        assert(bottom == 121)
    end,

    ["every two-row span combination remains in bounds and disjoint"] = function()
        local cards = { { id = "a" }, { id = "b" }, { id = "c" } }
        for _, columns in ipairs({ 1, 2, 3, 8 }) do
            for combination = 0, 63 do
                local choices, value = {}, combination
                for _, card in ipairs(cards) do
                    choices[card.id] = cardLayout.sizes[value % 4 + 1]
                    value = math.floor(value / 4)
                end
                local cells, rows = cardLayout.pack(cards, columns, function(id) return choices[id] end)
                local occupied = {}
                for _, cell in ipairs(cells) do
                    assert(cell.column >= 0 and cell.column + cell.columns <= columns)
                    assert(cell.row >= 0 and cell.row + cell.rows <= rows)
                    for y = cell.row, cell.row + cell.rows - 1 do
                        for x = cell.column, cell.column + cell.columns - 1 do
                            local key = y .. ":" .. x
                            assert(not occupied[key], "cards overlap")
                            occupied[key] = true
                        end
                    end
                end
            end
        end
    end,

    ["seven cards keep the same height as six cards"] = function()
        local viewport = 240
        local six = cardLayout.cardHeight(2, 2, 104, viewport, 4, 4)
        local seven = cardLayout.cardHeight(3, 2, 104, viewport, 4, 4)
        assert(six == 114)
        assert(seven == six)
    end,

    ["a single grid row retains both insets without clipping or scrolling"] = function()
        for _, scale in ipairs({ 1, 2 }) do
            local viewport, inset = 116 * scale, 4 * scale
            local height = cardLayout.cardHeight(1, 1, viewport,
                viewport, inset, inset)
            assert(height == 108 * scale,
                "one-row cards must fit between the upper and lower insets")
            assert(finalRowBottom(1, 1, height, inset, inset,
                viewport, 0) == 112 * scale)
            assert(cardLayout.contentHeight(1, 1, height,
                inset, inset, viewport) == viewport)
        end
    end,

    ["two visible rows keep their bottom inset and stable overflow height"] = function()
        for _, scale in ipairs({ 1, 2 }) do
            local viewport, gap, inset = 240 * scale, 4 * scale, 4 * scale
            local height = cardLayout.cardHeight(2, 2, 116 * scale,
                viewport, gap, inset)
            assert(height == 114 * scale,
                "two-row cards must leave the bottom inset visible")
            assert(finalRowBottom(2, 2, height, gap, inset,
                viewport, 0) == 236 * scale)
            assert(cardLayout.contentHeight(2, 2, height,
                gap, inset, viewport) == viewport)
            assert(cardLayout.cardHeight(3, 2, 116 * scale,
                viewport, gap, inset) == height)
        end
    end,

    ["the first overflow row starts outside the viewport"] = function()
        local viewport = 240
        local height = cardLayout.cardHeight(3, 2, 104,
            viewport, 4, 4)
        assert(cardLayout.rowTop(2, 2, height, 4, 4, viewport) >=
            viewport)
    end,

    ["short overflow aligns the final row to the bottom inset"] = function()
        local viewport = 240
        local height = cardLayout.cardHeight(3, 2, 104,
            viewport, 4, 4)
        local content = cardLayout.contentHeight(3, 2, height, 4, 4,
            viewport)
        local offset = cardLayout.maximumOffset(content, viewport)
        assert(finalRowBottom(3, 2, height, 4, 4, viewport, offset) ==
            viewport - 4)
    end,

    ["content that fits does not create a scroll offset"] = function()
        local viewport = 360
        local content = cardLayout.contentHeight(2, 3, 104, 4, 4,
            viewport)
        assert(content == viewport)
        assert(cardLayout.maximumOffset(content, viewport) == 0)
    end,

    ["an empty grid uses the viewport height"] = function()
        assert(cardLayout.contentHeight(0, 2, 104, 4, 4, 240) == 240)
    end,

    ["a fitted fractional viewport does not keep a one-pixel scroll range"] = function()
        local viewport = 100.4
        local height = cardLayout.cardHeight(1, 1, 80, viewport, 4, 4)
        assert(cardLayout.hostExtent(viewport) == 100)
        assert(cardLayout.contentHeight(1, 1, height, 4, 4, viewport) == 100)
        assert(cardLayout.scrollExtent(101, viewport) == 100)
        assert(cardLayout.scrollExtent(140, viewport) == 140)
        local overflow = cardLayout.cardHeight(2, 1, 40, viewport, 4, 4)
        assert(cardLayout.contentHeight(2, 1, overflow, 4, 4, viewport) > 101)
    end,

    ["horizontal flow fills the fixed rows before opening another column"] = function()
        local cards = { { id = "a" }, { id = "b" }, { id = "c" },
            { id = "d" }, { id = "e" } }
        local cells, packed = cardLayout.pack(cards, 2, function() return "1x1" end, true)
        cells, packed = cardLayout.transpose(cells)
        assert(packed == 3)
        assert(cells[1].column == 0 and cells[1].row == 0)
        assert(cells[2].column == 0 and cells[2].row == 1)
        assert(cells[3].column == 1 and cells[3].row == 0)
        assert(cells[5].column == 2 and cells[5].row == 0)
        assert(cardLayout.swapSize("2x1") == "1x2")
        assert(cardLayout.swapSize("1x2") == "2x1")
        local sized = cardLayout.pack({ { id = "wide" }, { id = "tall" } }, 2,
            function(id) return cardLayout.swapSize(id == "wide" and "2x1" or "1x2") end,
            true)
        sized = cardLayout.transpose(sized)
        assert(sized[1].columns == 2 and sized[1].rows == 1)
        assert(sized[2].columns == 1 and sized[2].rows == 2)
    end,

    ["drop targets follow midpoints and exclude the dragged card"] = function()
        local frames = {
            { id = "a", x = 4, y = 4, w = 40, h = 80 },
            { id = "b", x = 48, y = 4, w = 40, h = 80 },
            { id = "c", x = 92, y = 4, w = 40, h = 80 },
        }
        assert(cardLayout.dropTarget(frames, "a", 100, 20, true) == 2)
        local endIndex, _, axis, at = cardLayout.dropTarget(frames, "a", 140, 20, true)
        assert(endIndex == 3 and axis == "x" and at == 132)
        assert(cardLayout.dropTarget(frames, "c", 10, 20, true) == 1)
        assert(cardLayout.dropTarget(frames, "b", 70, 20, true) == 2)
        assert(cardLayout.dropTarget(frames, "missing", 10, 20, true) == nil)
        local stacked = {
            { id = "a", x = 4, y = 4, w = 80, h = 40 },
            { id = "b", x = 4, y = 48, w = 80, h = 40 },
        }
        assert(cardLayout.dropTarget(stacked, "a", 20, 80, false) == 1)
        local below, marker, belowAxis, at = cardLayout.dropTarget(
            stacked, "a", 20, 100, false)
        assert(below == 2 and belowAxis == "y" and marker.id == "b" and at == 88)
        assert(cardLayout.dropTarget(stacked, "b", 20, 10, false) == 1)
        local grid = {
            { id = "a", x = 4, y = 4, w = 40, h = 40 },
            { id = "b", x = 4, y = 48, w = 40, h = 40 },
            { id = "c", x = 48, y = 4, w = 40, h = 40 },
        }
        assert(cardLayout.dropTarget(grid, "a", 20, 50, true) == 1)
        assert(cardLayout.dropTarget(grid, "a", 20, 80, true) == 2)
        assert(cardLayout.dropTarget(grid, "c", 20, 80, true) == 3)
        local afterColumn = cardLayout.dropTarget(grid, "a", 100, 20, true)
        assert(afterColumn == 3)
    end,
}
