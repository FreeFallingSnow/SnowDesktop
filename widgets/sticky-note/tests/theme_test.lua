local noteTheme = module.require("modules/theme.lua")

return {
    ["dark foreground theme uses black text"] = function()
        assert(noteTheme.resolveTextColor(1) == 0x000000)
    end,

    ["light or missing foreground theme uses white text"] = function()
        assert(noteTheme.resolveTextColor(0) == 0xFFFFFF)
        assert(noteTheme.resolveTextColor(nil) == 0xFFFFFF)
    end,
}
