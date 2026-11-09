local board = module.require("modules/board.lua")

local skin = {
    reactionEnter = 480,
    reactionFadeAt = 2820,
    reactionDuration = 3100,
    modes = {
        ["local"] = "lua_widget.gomoku.hotseat",
        casual = "lua_widget.gomoku.ai_casual",
        balanced = "lua_widget.gomoku.ai_balanced",
        advanced = "lua_widget.gomoku.ai_advanced",
    },
    rules = {free = "lua_widget.gomoku.rule_free", standard = "lua_widget.gomoku.rule_standard"},
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

local function text(value, x, y, width, size, color, bold, centered, lineHeight, alpha)
    local measured = draw.measureText(value, size, 4096, bold)
    if measured.width > width then
        size = math.max(size * 0.64, size * width / measured.width)
        measured = draw.measureText(value, size, 4096, bold)
    end
    local offset = centered and math.max(0, (width - measured.width) / 2) or 0
    if lineHeight then
        local ink = measured.ink
        local height, top = measured.height, 0
        if ink and ink.height > 0 then height, top = ink.height, ink.top end
        y = y + (lineHeight - height) / 2 - top
    end
    draw.text(x + offset, y, value, size, color, width - offset, bold, true, nil, alpha)
end

local function region(id, label, x, y, w, h, radius)
    interaction.region({
        key = id,
        shape = {type = "roundedRect", x = x, y = y, width = w, height = h, radius = radius},
        cursor = "hand", focusable = true, events = {click = {id = id}},
        accessibility = {role = "button", label = label},
    })
end

local function star(x, y, r, color, alpha)
    local points = {}
    for i = 0, 7 do
        local angle = (i - 2) * math.pi / 4
        local length = i % 2 == 0 and r or r * 0.28
        points[i + 1] = {op = i == 0 and "move" or "line",
            x = x + math.cos(angle) * length, y = y + math.sin(angle) * length}
    end
    points[9] = {op = "close"}
    draw.path(points, {fillColor = color, alpha = alpha})
end

local function stamp(icon, x, y, r, alpha)
    local function dot(dx, dy, radius, color)
        draw.circle(x + r * dx, y + r * dy, r * radius, color, alpha)
    end
    local function arc(dx, dy, radius, start, sweep, color)
        draw.arc(x + r * dx, y + r * dy, r * radius, start, sweep, r * 0.10, color, alpha)
    end
    if icon == "smile" then
        dot(0, 0.07, 0.89, 0xA85E2E)
        dot(0, -0.04, 0.85, 0xFFCD68)
        dot(-0.34, -0.57, 0.15, 0xFFF1B7)
        arc(-0.30, -0.14, 0.14, 195, 150, 0x54352A)
        arc(0.30, -0.14, 0.14, 195, 150, 0x54352A)
        dot(-0.51, 0.15, 0.13, 0xF18762)
        dot(0.51, 0.15, 0.13, 0xF18762)
        arc(0, 0.02, 0.34, 18, 144, 0x54352A)
    elseif icon == "spark" then
        dot(-0.10, 0.08, 0.72, 0x287858)
        dot(-0.10, 0.01, 0.68, 0x55BC8D)
        star(x - r * 0.12, y + r * 0.02, r * 0.56, 0xFFF6C9, alpha)
        star(x + r * 0.61, y - r * 0.52, r * 0.39, 0xFFD77C, alpha)
        dot(0.72, 0.40, 0.10, 0x8CF3CA)
    elseif icon == "pair" then
        dot(-0.41, 0.28, 0.59, 0x83B19D)
        dot(-0.41, 0.26, 0.53, 0x263C38)
        dot(0.40, -0.17, 0.59, 0xA77548)
        dot(0.40, -0.22, 0.55, 0xFFF0CB)
        dot(-0.57, 0.14, 0.085, 0xCEE8D8)
        dot(-0.28, 0.14, 0.085, 0xCEE8D8)
        dot(0.27, -0.33, 0.085, 0x654736)
        dot(0.54, -0.33, 0.085, 0x654736)
        arc(-0.42, 0.24, 0.19, 20, 140, 0xCEE8D8)
        arc(0.40, -0.22, 0.19, 20, 140, 0x654736)
        star(x - r * 0.48, y - r * 0.63, r * 0.27, 0xFFD77C, alpha)
    end
end

local function button(id, label, x, y, w, h, c, primary, icon)
    local r = h * 0.22
    draw.rect(x, y, w, h, primary and c.accent or c.raised, r, interaction.isHovered(id) and 1 or 0.90)
    if not primary then draw.strokeRect(x, y, w, h, c.trim, r, 1, 0.54) end
    if icon then stamp(icon, x + w / 2, y + h / 2, h * 0.39, 1)
    else text(label, x + h * 0.12, y + h * 0.20, w - h * 0.24, h * 0.45, primary and c.onAccent or c.ink, true, true) end
    if interaction.isFocused(id) then draw.strokeRect(x, y, w, h, c.gold, r, 2) end
    region(id, label, x, y, w, h, r)
end

local function chip(label, x, y, w, h, c, actionable)
    local r = h * 0.42
    draw.rect(x, y, w, h, c.raised, r, 0.94)
    draw.strokeRect(x, y, w, h, actionable and c.accent or c.trim, r, 1, 0.65)
    text(label, x + h * 0.16, y, w - h * 0.32, h * 0.43, c.ink, true, true, h)
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

local grainCurves = {
    {0.04, 0.14, 0.26, 0.45, 0.69, 0.11, -0.008},
    {0.32, 0.31, 0.55, 0.77, 0.96, 0.33, 0.013},
    {0.05, 0.40, 0.17, 0.34, 0.57, 0.42, 0.009},
    {0.60, 0.46, 0.73, 0.88, 0.95, 0.48, -0.009},
    {0.05, 0.65, 0.25, 0.57, 0.89, 0.66, -0.016},
    {0.15, 0.79, 0.41, 0.60, 0.82, 0.77, 0.012},
    {0.48, 0.89, 0.66, 0.83, 0.96, 0.88, -0.009},
}

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

    for _, grain in ipairs(grainCurves) do
        local left, top, control1, control2, right, bottom, bend = table.unpack(grain)
        draw.path({
            {op = "move", x = x + side * left, y = y + side * top},
            {op = "cubic", x1 = x + side * control1, y1 = y + side * (top + bend),
                x2 = x + side * control2, y2 = y + side * (bottom - bend),
                x = x + side * right, y = y + side * bottom},
        }, {strokeColor = 0x99663D, thickness = step * 0.055, alpha = 0.09})
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
        button("rule.standard", l10n.tr("lua_widget.gomoku.rule_standard_detail"), left + inset, buttonTop + item + gap,
            width - inset * 2, item, c, game.rule == "standard")
    else
        for i, reaction in ipairs(skin.reactions) do tile("reaction." .. i, l10n.tr(reaction.label), i, false, reaction.stamp) end
        local mute = model.muted and l10n.tr("lua_widget.gomoku.unmute") or l10n.tr("lua_widget.gomoku.mute")
        button("mute", mute, left + inset, buttonTop + 3 * (item + gap), width - inset * 2, item, c, model.muted)
    end
end

local function reactionCard(reaction, x, y, w, u, c)
    local age = time.monotonic() - reaction.startedAt
    if age >= skin.reactionDuration then return false end
    local progress = reaction.animated and math.min(1, math.max(0, age / skin.reactionEnter)) or 1
    local remaining = 1 - progress
    local slide = remaining ^ 3
    local pop = math.sin(math.pi * progress) * remaining
    local fade = 1
    if reaction.animated and reaction.fade and age >= skin.reactionFadeAt then
        fade = (skin.reactionDuration - age) / (skin.reactionDuration - skin.reactionFadeAt)
    end
    local alpha = math.min(1, 0.25 + progress * 2) * fade
    local accent = reaction.peer and c.gold or c.accent
    x = x + (reaction.peer and 24 or -12) * u * slide
    y = y + (4 * slide - 4 * pop - 4 * (1 - fade)) * u
    local h, radius = 37 * u, 9 * u
    draw.rect(x, y + 2 * u, w, h, 0x081614, radius, 0.42 * alpha)
    draw.gradientRect(x, y, w, h, c.raised, c.panel, "horizontal", radius, alpha)
    draw.strokeRect(x, y, w, h, accent, radius, 1.15 * u, 0.88 * alpha)
    draw.rect(x + 4 * u, y + 8 * u, 2.2 * u, h - 16 * u, accent, u, alpha)

    local item = skin.reactions[reaction.id]
    local iconX, iconY = x + 26 * u, y + h / 2 - 4 * u * pop
    if item.stamp then
        stamp(item.stamp, iconX, iconY, (12.5 + 7 * pop) * u, alpha)
    else
        draw.rect(iconX - 10 * u, iconY - 8 * u, 20 * u, 15 * u, accent, 5 * u, alpha)
        draw.line(iconX - 4 * u, iconY + 4 * u, iconX - 7 * u, iconY + 9 * u, 2.5 * u, accent, alpha)
        for i = -1, 1 do
            draw.circle(iconX + i * 4.4 * u, iconY - 0.5 * u, 1.25 * u, c.onAccent, alpha)
        end
    end
    if reaction.peer and progress < 1 then
        star(iconX - (10 + progress * 11) * u, iconY - (9 + progress * 10) * u, 3.3 * u, accent, remaining * alpha)
        star(iconX + (10 + progress * 9) * u, iconY - (7 + progress * 11) * u, 2.6 * u, accent, remaining * alpha)
    end

    local speaker = reaction.peer and l10n.tr("lua_widget.gomoku.opponent") or l10n.tr("lua_widget.gomoku.me")
    local labelX, labelWidth = x + 47 * u, w - 56 * u
    text(speaker, labelX, y + 4 * u, labelWidth, 10 * u, accent, true, false, 12 * u, alpha)
    text(l10n.tr(item.label), labelX, y + 16 * u, labelWidth, 15 * u, c.ink, true, false, 17 * u, alpha)
    return true
end

function skin.render(model, game, status, canMove)
    local w, h, c = layout.contentWidth(), layout.contentHeight(), palette()
    local u = math.max(0.45, math.min(w, h) / 440)
    local margin, headerH, buttonH = 18 * u, 89 * u, 32 * u
    local controlsWidth = w - 2 * margin
    local footer = h - margin - buttonH
    local statusH = 27 * u
    local side = math.min(controlsWidth, footer - statusH - headerH - 15 * u)
    if side <= 0 then return end
    local x, y = (w - side) / 2, headerH + (footer - statusH - headerH - 15 * u - side) / 2
    local title = l10n.tr("lua_widget.gomoku.name")
    local modeKey = model.aiLevel and skin.modes[model.aiLevel] or "lua_widget.gomoku.hotseat"
    local mode = model.connected and l10n.tr("lua_widget.gomoku.online_game") or l10n.tr(modeKey)
    local canChoose = (not model.connected or model.host) and game.played == 0
    local badgeWidth = math.min(130 * u, w * 0.42)
    local badgeX = w - margin - badgeWidth
    local titleY, modeY, badgeHeight = 12 * u, 47 * u, 30 * u
    text(title, margin, titleY, badgeX - margin - 8 * u, 27 * u, c.ink, true)
    local cardWidth = math.min(214 * u, badgeX - margin - 12 * u)
    if not model.reaction or not reactionCard(model.reaction, margin, 46 * u, cardWidth, u, c) then
        text(mode, margin + u, modeY, badgeX - margin - 9 * u, 13 * u, c.muted, false)
    end
    local headerCenter = (titleY + modeY + 18 * u) / 2
    chip(l10n.tr(skin.rules[game.rule]), badgeX, headerCenter - badgeHeight / 2, badgeWidth, badgeHeight, c, canChoose)
    boardArt(game, model.pendingIndex, canMove, x, y, side)
    if model.menu then overlay(model, x, y, side, u, c) end

    local statusY = y + side + 7 * u
    local statusIconX = margin + 16 * u
    draw.rect(margin, statusY, controlsWidth, statusH, c.panel, 7 * u, 0.9)
    draw.strokeRect(margin, statusY, controlsWidth, statusH, game.winner and c.gold or c.trim, 7 * u, 1, 0.60)
    if game.winner then
        victory(statusIconX, statusY + statusH / 2, 11 * u, c)
    else
        draw.circle(statusIconX, statusY + statusH / 2, 6 * u, game.turn == "B" and 0x121D1A or 0xFFF0CC)
        draw.arc(statusIconX, statusY + statusH / 2, 6.7 * u, 0, 360, math.max(1, u), c.trim)
    end
    text(status, margin + 30 * u, statusY, controlsWidth - 38 * u, game.winner and 17 * u or 14 * u,
        game.winner and c.gold or c.ink, true, false, statusH)

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
    local buttonWidth = (controlsWidth - gap * (#actions - 1)) / #actions
    for i, item in ipairs(actions) do
        button(item.id, item.label, margin + (i - 1) * (buttonWidth + gap), footer, buttonWidth, buttonH, c,
            i == 1 and game.winner and not sync)
    end
end

return skin
