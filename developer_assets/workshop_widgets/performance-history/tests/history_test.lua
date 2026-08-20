local history = module.require("modules/history.lua")

return {
    ["append keeps the newest bounded samples"] = function()
        local result = history.append({ 1, 2, 3 }, 4, 3)
        assert(#result == 3)
        assert(result[1] == 2 and result[2] == 3 and result[3] == 4)
    end,

    ["append does not mutate its input"] = function()
        local source = { 1, 2 }
        local result = history.append(source, 3, 2)
        assert(#source == 2 and source[2] == 2)
        assert(#result == 2 and result[1] == 2 and result[2] == 3)
    end,

    ["seed is deterministic and bounded"] = function()
        local first = history.seed(24, 50, 70, 3)
        local second = history.seed(24, 50, 70, 3)
        assert(#first == 24 and #second == 24)
        for index = 1, #first do
            assert(first[index] == second[index])
            assert(first[index] >= 0 and first[index] <= 100)
        end
    end,

    ["latest uses a fallback for empty input"] = function()
        assert(history.latest({}, 17) == 17)
        assert(history.latest({ 3, 8 }, 17) == 8)
    end,
}
