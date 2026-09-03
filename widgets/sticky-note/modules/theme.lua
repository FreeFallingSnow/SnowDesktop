local noteTheme = {}

function noteTheme.resolveTextColor(contentTheme)
    return contentTheme == 1 and 0x000000 or 0xFFFFFF
end

return noteTheme
