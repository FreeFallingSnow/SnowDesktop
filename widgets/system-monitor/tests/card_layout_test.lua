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
}
