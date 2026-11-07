local board = module.require("modules/board.lua")

local skin = {
    modes = {
        ["local"] = "lua_widget.gomoku.hotseat",
        casual = "lua_widget.gomoku.ai_casual",
        balanced = "lua_widget.gomoku.ai_balanced",
        advanced = "lua_widget.gomoku.ai_advanced",
    },
    rules = {free = "lua_widget.gomoku.rule_free", exact = "lua_widget.gomoku.rule_exact"},
    reactions = {
        {label = "lua_widget.gomoku.say_hello"},
        {label = "lua_widget.gomoku.say_nice"},
        {label = "lua_widget.gomoku.say_rematch"},
        {label = "lua_widget.gomoku.stamp_smile", stamp = "smile"},
        {label = "lua_widget.gomoku.stamp_spark", stamp = "spark"},
        {label = "lua_widget.gomoku.stamp_pair", stamp = "pair"},
    },
}

local dark = {
    top = 0x183C34, base = 0x0E2422, panel = 0x153630, raised = 0x214940,
    ink = 0xFFF2D6, muted = 0xA9CBBB, trim = 0x568675, accent = 0x98E4BE,
    gold = 0xF1CA86, onAccent = 0x15352B,
}
local light = {
    top = 0xF0EDD6, base = 0xC9D9BD, panel = 0xE7ECDA, raised = 0xD5E4CF,
    ink = 0x172E25, muted = 0x34584A, trim = 0x698E75, accent = 0x336D56,
    gold = 0x81561F, onAccent = 0xFFF4D8,
}
local function palette()
    return widget.theme().contentTheme == 1 and light or dark
end

local function diamond(x, y, width, height, color, opacity)
    draw.line(x - width, y, x, y - height, 1, color, opacity)
    draw.line(x, y - height, x + width, y, 1, color, opacity)
    draw.line(x + width, y, x, y + height, 1, color, opacity)
    draw.line(x, y + height, x - width, y, 1, color, opacity)
end

function skin.background()
    local w, h, c = layout.contentWidth(), layout.contentHeight(), palette()
    draw.gradientRect(0, 0, w, h, c.top, c.base, "vertical", 0, 1)
    local unit = math.max(9, math.min(w, h) * 0.044)
    for row = 0, 4 do
        for col = 0, 9 do
            local x, y = w - (col + row * 0.5) * unit * 1.55, row * unit * 0.83
            diamond(x, y, unit * 0.76, unit * 0.42, c.trim, 0.22)
        end
    end
    for side = 0, 1 do
        local x = side == 0 and unit * 0.45 or w - unit * 0.45
        draw.line(x, h * 0.25, x, h * 0.87, 1, c.trim, 0.29)
    end
end

local function text(value, x, y, width, size, color, bold, centered)
    local measured = draw.measureText(value, size, 4096, bold).width
    if measured > width then
        size = math.max(size * 0.64, size * width / measured)
        measured = draw.measureText(value, size, 4096, bold).width
    end
    local offset = centered and math.max(0, (width - measured) / 2) or 0
    draw.text(x + offset, y, value, size, color, width - offset, bold, true)
end

local function region(id, label, x, y, w, h, radius)
    interaction.region({
        key = id,
        shape = {type = "roundedRect", x = x, y = y, width = w, height = h, radius = radius},
        cursor = "hand", focusable = true, events = {click = {id = id}},
        accessibility = {role = "button", label = label},
    })
end

local function stamp(icon, x, y, r)
    if icon == "smile" then
        draw.circle(x, y, r, 0xEDBA6D)
        draw.circle(x - r * 0.32, y - r * 0.16, r * 0.10, 0x4D352C)
        draw.circle(x + r * 0.32, y - r * 0.16, r * 0.10, 0x4D352C)
        draw.arc(x, y + r * 0.05, r * 0.47, 18, 144, math.max(1, r * 0.12), 0x4D352C)
    elseif icon == "spark" then
        draw.circle(x, y, r, 0x6DBE9D)
        draw.line(x - r * 0.54, y, x + r * 0.54, y, r * 0.16, 0xF5FFDF)
        draw.line(x, y - r * 0.54, x, y + r * 0.54, r * 0.16, 0xF5FFDF)
        diamond(x, y, r * 0.40, r * 0.40, 0xF5FFDF, 1)
    else
        draw.circle(x - r * 0.34, y + r * 0.1, r * 0.67, 0x252F2B)
        draw.circle(x + r * 0.36, y - r * 0.14, r * 0.65, 0xF6E3BD)
        draw.circle(x + r * 0.22, y - r * 0.28, r * 0.13, 0xFFFCED)
    end
end

local function button(id, label, x, y, w, h, c, primary, icon)
    local r = h * 0.22
    draw.rect(x, y, w, h, primary and c.accent or c.raised, r, interaction.isHovered(id) and 1 or 0.90)
    if not primary then draw.strokeRect(x, y, w, h, c.trim, r, 1, 0.54) end
    if icon then stamp(icon, x + w / 2, y + h * 0.49, h * 0.32)
    else text(label, x + h * 0.12, y + h * 0.20, w - h * 0.24, h * 0.45, primary and c.onAccent or c.ink, true, true) end
    if interaction.isFocused(id) then draw.strokeRect(x, y, w, h, c.gold, r, 2) end
    region(id, label, x, y, w, h, r)
end

local function chip(label, x, y, w, h, c, actionable)
    local r = h * 0.42
    draw.rect(x, y, w, h, c.raised, r, 0.94)
    draw.strokeRect(x, y, w, h, actionable and c.accent or c.trim, r, 1, 0.65)
    text(label, x + 5, y + h * 0.18, w - 10, h * 0.43, c.ink, true, true)
    if actionable then region("rules", label, x, y, w, h, r) end
end

local function victory(x, y, r, c)
    diamond(x, y, r, r * 0.78, c.gold, 0.70)
    draw.line(x - r * 0.46, y + r * 0.25, x + r * 0.46, y + r * 0.25, r * 0.12, c.gold)
    draw.line(x - r * 0.46, y - r * 0.11, x - r * 0.24, y + r * 0.15, r * 0.10, c.gold)
    draw.line(x - r * 0.24, y + r * 0.15, x, y - r * 0.33, r * 0.10, c.gold)
    draw.line(x, y - r * 0.33, x + r * 0.24, y + r * 0.15, r * 0.10, c.gold)
    draw.line(x + r * 0.24, y + r * 0.15, x + r * 0.46, y - r * 0.11, r * 0.10, c.gold)
end

local function boardArt(game, pending, canMove, x, y, side)
    local step = side / (board.size + 1.65)
    local ox, oy = x + step * 1.325, y + step * 1.325
    local endX, endY = ox + step * (board.size - 1), oy + step * (board.size - 1)
    local radius = step * 0.52
    draw.rect(x, y + step * 0.17, side, side, 0x0A1613, radius, 0.36)
    draw.gradientRect(x, y, side, side, 0xEAD6A6, 0xD9B579, "vertical", radius, 1)
    draw.strokeRect(x + 1, y + 1, side - 2, side - 2, 0xA2763F, radius, math.max(1, step * 0.12), 0.84)
    draw.strokeRect(x + step * 0.48, y + step * 0.48, side - step * 0.96, side - step * 0.96,
        0x8B663E, step * 0.21, 1, 0.28)

    for i = 1, 8 do
        local grain = y + step * (0.75 + i * 1.69)
        draw.line(x + step * (0.59 + i % 3 * 0.16), grain, x + side - step * 0.63,
            grain + step * 0.19, math.max(1, step * 0.052), 0x9F6D42, 0.11)
    end

    for i = 0, board.size - 1 do
        local dx, dy = ox + i * step, oy + i * step
        local thickness = math.max(0.85, step * ((i == 0 or i == board.size - 1) and 0.052 or 0.038))
        draw.line(dx, oy, dx, endY, thickness, 0x654D39, 0.86)
        draw.line(ox, dy, endX, dy, thickness, 0x654D39, 0.86)
        text(string.char(65 + i), dx - step / 2, y + step * 0.53, step, step * 0.40, 0x654D39, true, true)
        text(tostring(i + 1), x + step * 0.43, dy - step * 0.25, step * 0.71, step * 0.40, 0x654D39, true, true)
    end
    for _, sy in ipairs({4, 8, 12}) do
        for _, sx in ipairs({4, 8, 12}) do
            draw.circle(ox + (sx - 1) * step, oy + (sy - 1) * step, step * 0.106, 0x57432E)
        end
    end

    local first, last
    if game.line then
        first, last = game.line[1], game.line[1]
        for _, index in ipairs(game.line) do
            first, last = math.min(first, index), math.max(last, index)
        end
        local ax, ay = board.coordinates(first)
        local bx, by = board.coordinates(last)
        draw.line(ox + (ax - 1) * step, oy + (ay - 1) * step, ox + (bx - 1) * step, oy + (by - 1) * step,
            step * 0.38, 0x34765C, 0.83)
    end

    local cellLabel = l10n.tr("lua_widget.gomoku.intersection")
    for index = 1, board.count do
        local col, row = board.coordinates(index)
        local px, py = ox + (col - 1) * step, oy + (row - 1) * step
        local stone = game.cells:sub(index, index)
        local id = "cell." .. index
        if stone ~= "." then
            if game.last == index then
                draw.circle(px, py, step * 0.49, game.winner and 0x3C8768 or 0xAD793D, 0.95)
            end
            draw.circle(px, py + step * 0.055, step * 0.43, 0x725339, 0.46)
            draw.circle(px, py, step * 0.42, stone == "B" and 0x142320 or 0xA8916B)
            draw.circle(px, py - step * 0.019, step * 0.375, stone == "B" and 0x26312E or 0xFFF3D5,
                pending == index and 0.73 or 1)
            draw.circle(px - step * 0.105, py - step * 0.145, step * 0.14, stone == "B" and 0x7D998C or 0xFFFDF2, 0.33)
            if game.last == index then draw.circle(px, py, step * 0.075, stone == "B" and 0xE7C784 or 0xA06B3B) end
        elseif canMove then
            if interaction.isHovered(id) or interaction.isFocused(id) then
                draw.circle(px, py, step * 0.39, game.turn == "B" and 0x26312E or 0xFFF3D5, 0.43)
            end
            interaction.region({
                key = id, shape = {type = "rect", x = px - step / 2, y = py - step / 2, width = step, height = step},
                cursor = "crosshair", focusable = true, events = {click = {id = id}},
                accessibility = {role = "button", label = cellLabel .. " " .. string.char(64 + col) .. row},
            })
        end
    end
    if game.line then
        for _, index in ipairs(game.line) do
            local col, row = board.coordinates(index)
            diamond(ox + (col - 1) * step, oy + (row - 1) * step, step * 0.17, step * 0.17, 0x83E6BD, 1)
        end
    end
end

local modeOrder = {"local", "casual", "balanced", "advanced"}

local function overlay(model, x, y, side, u, c)
    local title = l10n.tr("lua_widget.gomoku.mode")
    local rows = 2
    if model.menu == "rules" then title = l10n.tr("lua_widget.gomoku.rules")
    elseif model.menu == "chat" then title, rows = l10n.tr("lua_widget.gomoku.chat"), 4 end
    local width = side * 0.88
    local item = math.min(42 * u, side * 0.12)
    local gap, inset = 7 * u, 13 * u
    local titleHeight = 29 * u
    local height = titleHeight + item * rows + gap * (rows - 1) + inset * 2
    local left, top = x + (side - width) / 2, y + (side - height) / 2
    draw.rect(x, y, side, side, 0x142821, 8 * u, 0.53)
    draw.rect(left, top + 4 * u, width, height, 0x081614, 13 * u, 0.60)
    draw.rect(left, top, width, height, c.panel, 13 * u)
    draw.strokeRect(left, top, width, height, c.trim, 13 * u, 1.2)
    text(title, left + inset, top + inset * 0.72, width - 2 * inset, 17 * u, c.ink, true, true)
    local buttonTop = top + titleHeight + inset
    local buttonWidth = (width - inset * 2 - gap) / 2
    local function tile(id, label, i, primary, icon)
        button(id, label, left + inset + ((i - 1) % 2) * (buttonWidth + gap),
            buttonTop + ((i - 1) // 2) * (item + gap), buttonWidth, item, c, primary, icon)
    end
    if model.menu == "mode" then
        for i, mode in ipairs(modeOrder) do
            tile("opponent." .. mode, l10n.tr(skin.modes[mode]), i, (model.aiLevel or "local") == mode)
        end
    elseif model.menu == "rules" then
        local game = model.connected and model.game or (model.aiLevel and model.aiGame or model.localGame)
        button("rule.free", l10n.tr("lua_widget.gomoku.rule_free_detail"), left + inset, buttonTop,
            width - inset * 2, item, c, game.rule == "free")
        button("rule.exact", l10n.tr("lua_widget.gomoku.rule_exact_detail"), left + inset, buttonTop + item + gap,
            width - inset * 2, item, c, game.rule == "exact")
    else
        for i, reaction in ipairs(skin.reactions) do tile("reaction." .. i, l10n.tr(reaction.label), i, false, reaction.stamp) end
        local mute = model.muted and l10n.tr("lua_widget.gomoku.unmute") or l10n.tr("lua_widget.gomoku.mute")
        button("mute", mute, left + inset, buttonTop + 3 * (item + gap), width - inset * 2, item, c, model.muted)
    end
end

function skin.render(model, game, status, canMove)
    local w, h, c = layout.contentWidth(), layout.contentHeight(), palette()
    local u = math.max(0.45, math.min(w, h) / 440)
    local margin, headerH, buttonH = 18 * u, 87 * u, 32 * u
    local footer = h - margin - buttonH
    local statusH = 27 * u
    local side = math.min(w - 2 * margin, footer - statusH - headerH - 15 * u)
    if side <= 0 then return end
    local x, y = (w - side) / 2, headerH + (footer - statusH - headerH - 15 * u - side) / 2
    local title = l10n.tr("lua_widget.gomoku.name")
    local modeKey = model.aiLevel and skin.modes[model.aiLevel] or "lua_widget.gomoku.hotseat"
    local mode = model.connected and l10n.tr("lua_widget.gomoku.online_game") or l10n.tr(modeKey)
    text(title, margin, 12 * u, w - margin * 2, 27 * u, c.ink, true)
    if model.reaction then
        local reaction = skin.reactions[model.reaction.id]
        local speaker = model.reaction.peer and l10n.tr("lua_widget.gomoku.opponent") or l10n.tr("lua_widget.gomoku.me")
        local inset = reaction.stamp and 20 * u or 0
        if reaction.stamp then stamp(reaction.stamp, margin + 8 * u, 56 * u, 8 * u) end
        text(speaker .. " · " .. l10n.tr(reaction.label), margin + inset, 47 * u, w * 0.50 - inset, 13 * u, c.accent, true)
    else
        text(mode, margin + u, 47 * u, w * 0.51, 13 * u, c.muted, false)
    end

    local canChoose = (not model.connected or model.host) and game.played == 0
    local badgeWidth = math.min(130 * u, w * 0.42)
    chip(l10n.tr(skin.rules[game.rule]), w - margin - badgeWidth, 27 * u, badgeWidth, 30 * u, c, canChoose)
    boardArt(game, model.pendingIndex, canMove, x, y, side)
    if model.menu then overlay(model, x, y, side, u, c) end

    local statusY = y + side + 7 * u
    local lineWidth = side - 38 * u
    draw.rect(x, statusY, side, statusH, c.panel, 7 * u, 0.9)
    draw.strokeRect(x, statusY, side, statusH, game.winner and c.gold or c.trim, 7 * u, 1, 0.60)
    if game.winner then
        victory(x + 16 * u, statusY + statusH / 2, 11 * u, c)
    else
        draw.circle(x + 16 * u, statusY + statusH / 2, 6 * u, game.turn == "B" and 0x121D1A or 0xFFF0CC)
        draw.arc(x + 16 * u, statusY + statusH / 2, 6.7 * u, 0, 360, math.max(1, u), c.trim)
    end
    text(status, x + 30 * u, statusY + 4 * u, lineWidth, game.winner and 17 * u or 14 * u,
        game.winner and c.gold or c.ink, true)

    local sync = model.connected and not model.host
    local actions = {
        {id = sync and "sync" or "new",
            label = sync and l10n.tr("lua_widget.gomoku.sync") or
                (game.winner and l10n.tr("lua_widget.gomoku.play_again") or l10n.tr("lua_widget.gomoku.new"))},
        {id = model.connected and "chat" or "mode",
            label = model.connected and l10n.tr("lua_widget.gomoku.chat") or l10n.tr("lua_widget.gomoku.mode")},
    }
    if model.showSteam and model.state ~= "busy" then
        local label = l10n.tr("lua_widget.gomoku.steam_room")
        if model.connected or model.state == "waitingPeer" then label = l10n.tr("lua_widget.gomoku.local_game")
        elseif model.state == "roomReady" then label = l10n.tr("lua_widget.gomoku.online_game") end
        actions[#actions + 1] = {id = "steam", label = label}
    end
    local gap = 7 * u
    local buttonWidth = (w - 2 * margin - gap * (#actions - 1)) / #actions
    for i, item in ipairs(actions) do
        button(item.id, item.label, margin + (i - 1) * (buttonWidth + gap), footer, buttonWidth, buttonH, c,
            i == 1 and game.winner and not sync)
    end
end

return skin
