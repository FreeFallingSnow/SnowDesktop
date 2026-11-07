local board = module.require("modules/board.lua")

local function setup()
    return {
        game = board.parse(storage.get("preview_board")) or board.parse(board.empty),
        revision = 0,
        connected = false,
    }
end

local function sendFailed(model, text)
    if text:sub(1, 2) == "M:" then model.pending, model.pendingIndex = nil, nil end
    if text == "R" then model.resyncRequested = false end
    model.sendFailed = true
end

local function send(model, text)
    if model.sendTask then
        model.queued = text
        return
    end
    model.sendTask = task.start("multiplayer.send", {text = text})
    if model.sendTask then model.sent = text else sendFailed(model, text) end
end

local function publish(model)
    send(model, "S:" .. model.revision .. ":" .. model.game.cells)
end

local function applyMove(model, nextGame)
    model.game = nextGame
    model.revision = model.revision + 1
    if model.connected then publish(model) end
end

local function move(model, index)
    if model.pending then return end
    local nextGame = board.play(model.game, index)
    if not nextGame then return end
    if not model.connected then
        applyMove(model, nextGame)
    elseif model.host and model.game.turn == "B" then
        applyMove(model, nextGame)
    elseif not model.host and model.game.turn == "W" then
        model.pending, model.pendingIndex = nextGame, index
        send(model, "M:" .. model.revision .. ":" .. index)
    end
end

local function receive(model, e)
    if not model.connected then return end
    if model.host then
        if e.fromHost then return end
        if e.text == "R" then publish(model); return end
        local revision, index = e.text:match("^M:(%d+):(%d+)$")
        if revision and #revision <= 9 and #index <= 3 and tonumber(revision) == model.revision and model.game.turn == "W" then
            local nextGame = board.play(model.game, tonumber(index))
            if nextGame then applyMove(model, nextGame) else send(model, "N:" .. revision .. ":" .. index) end
        elseif revision then
            publish(model)
        end
    elseif e.fromHost then
        if model.pending and e.text == "N:" .. model.revision .. ":" .. model.pendingIndex then
            model.pending, model.pendingIndex, model.resyncRequested = nil, nil, false
            model.sendFailed = true
            return
        end
        local revision, cells = e.text:match("^S:(%d+):([BW.]+)$")
        if not revision or #revision > 9 then return end
        local number = tonumber(revision)
        if number < model.revision then return end
        if number == model.revision then
            if cells ~= model.game.cells or not model.resyncRequested then return end
            model.pending, model.pendingIndex, model.resyncRequested = nil, nil, false
            model.sendFailed = false
            return
        end
        local confirmed = board.parse(cells)
        if confirmed then
            model.game, model.revision = confirmed, number
            model.pending, model.pendingIndex, model.resyncRequested, model.sendFailed = nil, nil, false, false
        end
    end
end

local function event(_, model, e)
    if e.kind == "action" then
        model.sendFailed = false
        local index = e.id and e.id:match("^cell%.(%d+)$")
        if index then
            move(model, tonumber(index))
        elseif e.id == "new" and (not model.connected or model.host) then
            applyMove(model, board.parse(board.empty))
            model.roomEnded = false
        elseif e.id == "sync" and model.connected and not model.host then
            model.resyncRequested = true
            send(model, "R")
        end
    elseif e.kind == "multiplayer.state" then
        local connected = e.state == "connected"
        if connected and not model.connected then
            model.game, model.revision = board.parse(board.empty), 0
            model.roomEnded, model.sendFailed = false, false
        elseif model.connected and not connected then
            model.pending, model.pendingIndex, model.sendTask, model.sent, model.queued = nil, nil, nil, nil, nil
            model.resyncRequested, model.sendFailed, model.roomEnded = false, false, true
        end
        local arrived = connected and not model.connected
        model.connected, model.host = connected, e.host
        if arrived and not model.host then send(model, "R") end
    elseif e.kind == "multiplayer.message" then
        receive(model, e)
    elseif e.kind == "task.complete" and e.taskId == model.sendTask then
        if not e.ok then sendFailed(model, model.sent) end
        local nextMessage = model.queued
        model.sendTask, model.sent, model.queued = nil, nil, nil
        if model.connected and nextMessage then send(model, nextMessage) end
    else
        return
    end
    widget.invalidate()
end

local function status(model, shown)
    if shown.winner == "B" then return l10n.tr("lua_widget.gomoku.black_wins") end
    if shown.winner == "W" then return l10n.tr("lua_widget.gomoku.white_wins") end
    if shown.winner == "draw" then return l10n.tr("lua_widget.gomoku.draw") end
    if model.sendFailed then return l10n.tr("lua_widget.gomoku.send_failed") end
    if model.pending then return l10n.tr("lua_widget.gomoku.confirming") end
    if model.roomEnded then return l10n.tr("lua_widget.gomoku.room_ended") end
    if model.connected then
        if shown.turn == (model.host and "B" or "W") then return l10n.tr("lua_widget.gomoku.your_turn") end
        return l10n.tr("lua_widget.gomoku.their_turn")
    end
    if shown.turn == "B" then return l10n.tr("lua_widget.gomoku.black_turn") end
    return l10n.tr("lua_widget.gomoku.white_turn")
end

local function centeredText(text, x, y, width, size, color, bold)
    local measured = draw.measureText(text, size, width, bold)
    local padding = math.max(0, (width - measured.width) / 2)
    draw.text(x + padding, y, text, size, color, width - padding, bold, true)
end

local function button(id, label, x, y, width, height, color, foreground)
    local radius = height * 0.2
    draw.rect(x, y, width, height, color, radius, interaction.isHovered(id) and 0.94 or 0.72)
    centeredText(label, x, y + height * 0.18, width, height * 0.49, foreground, true)
    interaction.region({
        key = id,
        shape = {type = "roundedRect", x = x, y = y, width = width, height = height, radius = radius},
        cursor = "hand", focusable = true, events = {click = {id = id}},
        accessibility = {role = "button", label = label},
    })
end

local starPoints = {4, 8, 12}

local function render(_, model)
    local width, height = layout.contentWidth(), layout.contentHeight()
    local side = math.min(width * 0.90, height * 0.70)
    if side <= 0 then return end
    local left, top = (width - side) / 2, (height - side * 1.31) / 2 + side * 0.13
    local step = side / (board.size + 0.4)
    local startX, startY = left + step * 0.7, top + step * 0.7
    local darkInk = widget.theme().contentTheme == 1
    local ink = darkInk and 0x192634 or 0xF6F8FC
    local secondary = darkInk and 0x48586B or 0xD9E5F2
    local buttonFill = darkInk and 0xDCE6F4 or 0x385472
    centeredText(l10n.tr("lua_widget.gomoku.name"), left, top - side * 0.112, side, side * 0.067, ink, true)
    draw.rect(left, top, side, side, 0xD1A05F, step * 0.58)
    draw.rect(left + step * 0.10, top + step * 0.10, side - step * 0.20, side - step * 0.20, 0xE8BA7A, step * 0.52)
    for line = 0, board.size - 1 do
        local d = line * step
        draw.line(startX + d, startY, startX + d, startY + step * (board.size - 1), step * 0.043, 0x5D3D24)
        draw.line(startX, startY + d, startX + step * (board.size - 1), startY + d, step * 0.043, 0x5D3D24)
    end
    for _, x in ipairs(starPoints) do
        for _, y in ipairs(starPoints) do
            draw.circle(startX + (x - 1) * step, startY + (y - 1) * step, step * 0.12, 0x5D3D24)
        end
    end

    local shown = model.pending or model.game
    local wins = {}
    for _, index in ipairs(shown.line or {}) do wins[index] = true end
    local canMove = not shown.winner and not model.pending and
        (not model.connected or model.game.turn == (model.host and "B" or "W"))
    local cellLabel = l10n.tr("lua_widget.gomoku.intersection")
    for index = 1, board.count do
        local column, row = board.coordinates(index)
        local x, y = startX + (column - 1) * step, startY + (row - 1) * step
        local stone = shown.cells:sub(index, index)
        if stone ~= "." then
            local predicted = model.pending and model.game.cells:sub(index, index) == "."
            draw.circle(x, y + step * 0.06, step * 0.455, 0x795637, 0.58)
            draw.circle(x, y, step * 0.44, wins[index] and 0x36668A or 0x736047)
            draw.circle(x, y, step * 0.39, stone == "B" and 0x25272C or 0xFFF5E4, predicted and 0.76 or 1.0)
            if wins[index] then draw.circle(x, y, step * 0.105, 0x6EB5EF) end
        elseif canMove then
            local id = "cell." .. index
            interaction.region({
                key = id, shape = {type = "rect", x = x - step / 2, y = y - step / 2, width = step, height = step},
                cursor = "crosshair", focusable = true, events = {click = {id = id}},
                accessibility = {role = "button", label = cellLabel .. " " .. string.char(64 + column) .. row},
            })
        end
    end

    centeredText(status(model, shown), left, top + side * 1.026, side, side * 0.047, secondary, false)
    if model.connected and not model.host then
        button("sync", l10n.tr("lua_widget.gomoku.sync"), left + side * 0.27, top + side * 1.105,
            side * 0.46, side * 0.095, buttonFill, ink)
    else
        button("new", l10n.tr("lua_widget.gomoku.new"), left + side * 0.27, top + side * 1.105,
            side * 0.46, side * 0.095, buttonFill, ink)
    end
end

return widget.define({
    name = l10n.tr("lua_widget.gomoku.name"),
    useCustomStyle = true, followPersonalizationDefault = true,
    bg = 0x172131, border = 0xFFFFFF, alpha = 0.46, borderAlpha = 0.17, gradientEndA = 0.30,
    setup = setup, event = event, render = render,
})
