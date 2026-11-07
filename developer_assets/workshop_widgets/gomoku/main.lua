local board = module.require("modules/board.lua")
local ai = module.require("modules/ai.lua")
local skin = module.require("modules/skin.lua")

local function setup()
    local preview = board.parse(storage.get("preview_board"), storage.get("preview_rule"))
    if preview then
        local last = storage.get("preview_last")
        if type(last) == "number" and last >= 1 and last <= board.count and last % 1 == 0 then
            preview.last = last
        end
    end
    return {
        localGame = preview or board.parse(board.empty),
        revision = 0,
        connected = false,
        showSteam = not preview and widget.hasFeature("multiplayer.room.settings"),
        supportsParticipation = widget.hasFeature("multiplayer.room.participation"),
        muted = storage.get("mute_reactions") == true,
    }
end

local function shownGame(model)
    if model.connected then return model.pending or model.game end
    if model.aiLevel then return model.aiGame end
    return model.localGame
end

local function sendFailed(model, text)
    if text:sub(1, 2) == "C:" then return end
    if text:sub(1, 2) == "M:" then model.pending, model.pendingIndex = nil, nil end
    if text == "R" then model.resyncRequested = false end
    model.sendFailed = true
end

local function send(model, text)
    if model.sendTask then
        if text ~= "R" or not model.queued then model.queued = text end
        return
    end
    model.sendTask = task.start("multiplayer.send", {text = text})
    if model.sendTask then model.sent = text else sendFailed(model, text) end
end

local function publish(model)
    local game = model.game
    send(model, "S:" .. model.revision .. ":" .. (game.last or 0) .. ":" .. game.rule .. ":" .. game.cells)
end

local function applyOnlineMove(model, game)
    model.game = game
    model.revision = model.revision + 1
    publish(model)
end

local function applyComputerMove(model)
    local game = model.aiGame
    if model.connected or not model.aiLevel or game.winner or game.turn ~= "W" then return end
    model.aiGame = board.play(game, ai.choose(game, model.aiLevel)) or game
end

local function scheduleComputer(model)
    if model.connected or not model.aiLevel or model.aiGame.winner or model.aiGame.turn ~= "W" then return end
    if not schedule.after("computer", 240, {whenHidden = "pause"}) then applyComputerMove(model) end
end

local function move(model, index)
    if not model.connected then
        if model.aiLevel then
            if model.aiGame.turn ~= "B" then return end
            local game = board.play(model.aiGame, index)
            if game then
                model.aiGame = game
                scheduleComputer(model)
            end
        else
            model.localGame = board.play(model.localGame, index) or model.localGame
        end
        return
    end
    if model.pending then return end
    local nextGame = board.play(model.game, index)
    if not nextGame then return end
    if model.host and model.game.turn == "B" then
        applyOnlineMove(model, nextGame)
    elseif not model.host and model.game.turn == "W" then
        model.pending, model.pendingIndex = nextGame, index
        send(model, "M:" .. model.revision .. ":" .. index)
    end
end

local function showReaction(model, id, peer)
    model.reaction = {id = id, peer = peer}
    schedule.after("reaction", 3100, {whenHidden = "throttle"})
end

local function reactionId(text)
    local id = tonumber(text:match("^C:([1-6])$"))
    if id and skin.reactions[id] then return id end
end

local function sendReaction(model, id)
    local now = time.monotonic()
    if not model.connected or not skin.reactions[id] or model.sendTask or
        now - (model.lastSentReaction or -2000) < 1200 then return end
    model.lastSentReaction = now
    model.menu = nil
    send(model, "C:" .. id)
end

local function receive(model, e)
    if not model.connected then return end
    if e.text:sub(1, 2) == "C:" then
        local id, now = reactionId(e.text), time.monotonic()
        if id and not model.muted and now - (model.lastReceivedReaction or -2000) >= 1200 then
            model.lastReceivedReaction = now
            showReaction(model, id, true)
        end
        return
    end
    if model.host then
        if e.fromHost then return end
        if e.text == "R" then publish(model); return end
        local revision, index = e.text:match("^M:(%d+):(%d+)$")
        if revision and #revision <= 9 and #index <= 3 and tonumber(revision) == model.revision and model.game.turn == "W" then
            local nextGame = board.play(model.game, tonumber(index))
            if nextGame then applyOnlineMove(model, nextGame) else send(model, "N:" .. revision .. ":" .. index) end
        elseif revision then
            publish(model)
        end
    elseif e.fromHost then
        if model.pending and e.text == "N:" .. model.revision .. ":" .. model.pendingIndex then
            model.pending, model.pendingIndex, model.resyncRequested = nil, nil, false
            model.sendFailed = true
            return
        end
        local revision, last, rule, cells = e.text:match("^S:(%d+):(%d+):(%a+):([BW.]+)$")
        if not revision or #revision > 9 or #last > 3 then return end
        local number, index = tonumber(revision), tonumber(last)
        if number < model.revision or index > board.count then return end
        if number == model.revision then
            if cells ~= model.game.cells or rule ~= model.game.rule or not model.resyncRequested then return end
            model.pending, model.pendingIndex, model.resyncRequested = nil, nil, false
            model.sendFailed = false
            return
        end
        local confirmed = board.parse(cells, rule)
        if confirmed then
            local recentStone = confirmed.turn == "B" and "W" or "B"
            if confirmed.played == 0 and index ~= 0 then return end
            if confirmed.played > 0 and (index == 0 or cells:sub(index, index) ~= recentStone) then return end
            confirmed.last = index > 0 and index or nil
            model.game, model.revision = confirmed, number
            model.pending, model.pendingIndex, model.resyncRequested, model.sendFailed = nil, nil, false, false
        end
    end
end

local function changeMode(model)
    if model.modeTask then return end
    model.menu = nil
    if model.connected or model.state == "waitingPeer" then
        model.modeTask = task.start("multiplayer.deactivate")
    elseif model.state == "roomReady" then
        model.modeTask = task.start("multiplayer.activate")
    else
        task.start("multiplayer.openSettings")
        return
    end
    model.modeFailed = not model.modeTask
end

local function setOpponent(model, mode)
    schedule.cancel("computer")
    if mode == "local" then
        model.aiLevel = nil
    elseif model.aiLevel ~= mode then
        model.aiGame = board.parse(board.empty, shownGame(model).rule)
        model.aiLevel = mode
    end
    model.menu, model.roomEnded = nil, false
    scheduleComputer(model)
end

local function startRound(model)
    local empty = board.parse(board.empty, shownGame(model).rule)
    if model.connected then
        if model.host then applyOnlineMove(model, empty) end
    elseif model.aiLevel then
        schedule.cancel("computer")
        model.aiGame = empty
    else
        model.localGame = empty
    end
    model.menu, model.roomEnded = nil, false
end

local function canChooseRules(model)
    return (not model.connected or model.host) and shownGame(model).played == 0
end

local function chooseRules(model, rule)
    if not canChooseRules(model) then return end
    local game = board.parse(board.empty, rule)
    if model.connected then
        if model.game.rule ~= rule then applyOnlineMove(model, game) end
    elseif model.aiLevel then
        model.aiGame = game
    else
        model.localGame = game
    end
    model.menu = nil
end

local function action(model, id)
    if id == "steam" and model.showSteam then
        changeMode(model)
    elseif id == "mode" and not model.connected then
        model.menu = model.menu ~= "mode" and "mode" or nil
    elseif id == "rules" and canChooseRules(model) then
        model.menu = model.menu ~= "rules" and "rules" or nil
    elseif id == "chat" and model.connected then
        model.menu = model.menu ~= "chat" and "chat" or nil
    elseif id == "mute" and model.connected then
        model.muted = not model.muted
        storage.set("mute_reactions", model.muted)
        if model.muted and model.reaction and model.reaction.peer then model.reaction = nil end
    elseif id == "new" then
        startRound(model)
    elseif id == "sync" and model.connected and not model.host then
        model.resyncRequested = true
        send(model, "R")
    elseif id then
        local index = id:match("^cell%.(%d+)$")
        local mode = id:match("^opponent%.(%a+)$")
        local rule = id:match("^rule%.(%a+)$")
        local reaction = tonumber(id:match("^reaction%.([1-6])$"))
        if index and not model.menu then
            model.sendFailed, model.roomEnded = false, false
            move(model, tonumber(index))
        elseif mode and model.menu == "mode" and not model.connected and skin.modes[mode] then
            setOpponent(model, mode)
        elseif rule and model.menu == "rules" and skin.rules[rule] then
            chooseRules(model, rule)
        elseif reaction and model.menu == "chat" then
            sendReaction(model, reaction)
        end
    end
end

local function event(_, model, e)
    if e.kind == "action" then
        action(model, e.id)
    elseif e.kind == "schedule" then
        if e.id == "computer" then applyComputerMove(model)
        elseif e.id == "reaction" then model.reaction = nil
        else return end
    elseif e.kind == "multiplayer.state" then
        local connected = model.supportsParticipation and e.state == "connected"
        local arrived = connected and not model.connected
        if arrived then
            schedule.cancel("computer")
            model.game, model.revision, model.menu = board.parse(board.empty), 0, nil
            model.roomEnded, model.sendFailed = false, false
        elseif model.connected and not connected then
            model.pending, model.pendingIndex, model.sendTask, model.sent, model.queued = nil, nil, nil, nil, nil
            model.resyncRequested, model.sendFailed = false, false
            model.roomEnded = e.state ~= "waitingPeer" and e.state ~= "roomReady" and e.state ~= "busy"
            model.menu, model.reaction, model.lastSentReaction, model.lastReceivedReaction = nil, nil, nil, nil
            schedule.cancel("reaction")
        end
        local left = model.connected and not connected
        model.state, model.connected, model.host = e.state, connected, e.host
        model.modeFailed = false
        if arrived and not model.host then send(model, "R") end
        if left then scheduleComputer(model) end
    elseif e.kind == "multiplayer.message" then
        receive(model, e)
    elseif e.kind == "task.complete" and e.taskId == model.modeTask then
        model.modeTask, model.modeFailed = nil, not e.ok
    elseif e.kind == "task.complete" and e.taskId == model.sendTask then
        if not e.ok then sendFailed(model, model.sent)
        else
            local id = reactionId(model.sent)
            if id and model.connected then showReaction(model, id, false) end
        end
        local nextMessage = model.queued
        model.sendTask, model.sent, model.queued = nil, nil, nil
        if model.connected and nextMessage then send(model, nextMessage) end
    else
        return
    end
    widget.invalidate()
end

local function status(model, game)
    if game.winner == "draw" then return l10n.tr("lua_widget.gomoku.draw") end
    if game.winner and model.aiLevel and not model.connected then
        if game.winner == "B" then return l10n.tr("lua_widget.gomoku.you_win") end
        return l10n.tr("lua_widget.gomoku.ai_win")
    end
    if game.winner == "B" then return l10n.tr("lua_widget.gomoku.black_wins") end
    if game.winner == "W" then return l10n.tr("lua_widget.gomoku.white_wins") end
    if model.modeFailed then return l10n.tr("lua_widget.gomoku.mode_failed") end
    if model.state == "waitingPeer" then return l10n.tr("lua_widget.gomoku.waiting_peer") end
    if model.state == "busy" then return l10n.tr("lua_widget.gomoku.local_board_busy") end
    if model.sendFailed then return l10n.tr("lua_widget.gomoku.send_failed") end
    if model.pending then return l10n.tr("lua_widget.gomoku.confirming") end
    if model.roomEnded then return l10n.tr("lua_widget.gomoku.room_ended") end
    if game.played == 0 then return l10n.tr("lua_widget.gomoku.opening") end
    if model.connected then
        if game.turn == (model.host and "B" or "W") then return l10n.tr("lua_widget.gomoku.your_turn") end
        return l10n.tr("lua_widget.gomoku.their_turn")
    end
    if model.aiLevel then
        if game.turn == "W" then return l10n.tr("lua_widget.gomoku.ai_thinking") end
        return l10n.tr("lua_widget.gomoku.your_turn")
    end
    if game.turn == "B" then return l10n.tr("lua_widget.gomoku.black_turn") end
    return l10n.tr("lua_widget.gomoku.white_turn")
end

local function render(_, model)
    local game = shownGame(model)
    local canMove = not game.winner and not model.menu and not model.pending
    if model.connected then
        canMove = canMove and game.turn == (model.host and "B" or "W")
    elseif model.aiLevel then
        canMove = canMove and game.turn == "B"
    end
    skin.render(model, game, status(model, game), canMove)
end

return widget.define({
    name = l10n.tr("lua_widget.gomoku.name"),
    useCustomStyle = true, followPersonalizationDefault = false, glassEnabled = false,
    bg = 0x102622, border = 0x79AE96, alpha = 1, borderAlpha = 0.68, gradientEndA = 1,
    backgroundLayer = {render = skin.background, opacity = 1, blurRadius = 0},
    setup = setup, event = event, render = render,
})
