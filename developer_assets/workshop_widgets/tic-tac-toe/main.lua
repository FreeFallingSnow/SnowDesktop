local empty = "........."
local lines = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}, {1, 4, 7},
    {2, 5, 8}, {3, 6, 9}, {1, 5, 9}, {3, 5, 7}}

local function outcome(board)
    for _, line in ipairs(lines) do
        local mark = board:sub(line[1], line[1])
        if mark ~= "." and mark == board:sub(line[2], line[2]) and mark == board:sub(line[3], line[3]) then
            return mark, line
        end
    end
    if not board:find(".", 1, true) then return "draw" end
end

local function turn(board)
    local _, blanks = board:gsub("%.", "")
    return blanks % 2 == 1 and "X" or "O"
end

local function setup()
    local preview = storage.get("preview_board")
    return {
        board = type(preview) == "string" and #preview == 9 and preview:match("^[XO.]+$") or empty,
        online = "unavailable", revision = 0,
    }
end

local function send(model, text)
    if model.sendTask then
        model.nextMessage = text
        return
    end
    model.sendTask = task.start("multiplayer.send", {text = text})
    if not model.sendTask then model.error, model.awaiting = true, false end
end

local function shareBoard(model)
    send(model, "S:" .. model.revision .. ":" .. model.board)
end

local function play(model, cell)
    if not cell or cell < 1 or cell > 9 or outcome(model.board) or
        model.board:sub(cell, cell) ~= "." then return end
    local mark = turn(model.board)
    model.board = model.board:sub(1, cell - 1) .. mark .. model.board:sub(cell + 1)
    model.revision = model.revision + 1
    if model.online == "connected" then shareBoard(model) end
end

local function receive(model, event)
    if model.online ~= "connected" then return end
    if model.host then
        if event.fromHost then return end
        if event.text == "R" then shareBoard(model); return end
        local revision, cell = event.text:match("^M:(%d+):([1-9])$")
        if revision and tonumber(revision) == model.revision and turn(model.board) == "O" then
            play(model, tonumber(cell))
        end
    elseif event.fromHost then
        local revision, board = event.text:match("^S:(%d+):([XO.]+)$")
        if not revision or #revision > 9 or #board ~= 9 then return end
        local number = tonumber(revision)
        local _, crosses = board:gsub("X", "")
        local _, circles = board:gsub("O", "")
        if (crosses ~= circles and crosses ~= circles + 1) or number < model.revision or
            (number == model.revision and board ~= model.board) then return end
        model.board, model.revision, model.awaiting = board, number, false
    end
end

local function action(model, id)
    model.error = false
    local cell = id and id:match("^cell%.([1-9])$")
    if cell then
        if model.online == "connected" then
            if model.host and turn(model.board) == "X" then play(model, tonumber(cell))
            elseif not model.host and turn(model.board) == "O" and not model.awaiting then
                model.awaiting = true
                send(model, "M:" .. model.revision .. ":" .. cell)
            end
        elseif model.online ~= "connecting" and model.online ~= "waiting" then
            play(model, tonumber(cell))
        end
        return
    end
    if id == "new" then
        if model.online == "connected" and not model.host then return end
        model.board = empty
        model.ended = false
        model.revision = model.revision + 1
        if model.online == "connected" then shareBoard(model) end
        return
    end
    if id == "sync" then send(model, "R"); return end
    if model.controlTask and id ~= "leave" then return end
    if id == "create" then model.controlTask = task.start("multiplayer.create")
    elseif id == "listen" then model.controlTask = task.start("multiplayer.listen")
    elseif id == "invite" then model.controlTask = task.start("multiplayer.invite")
    elseif id == "join" then model.controlTask = task.start("multiplayer.join")
    elseif id == "leave" then model.controlTask = task.start("multiplayer.leave")
    else return end
    model.error = not model.controlTask
end

local function event(_, model, e)
    if e.kind == "action" then
        action(model, e.id)
    elseif e.kind == "multiplayer.state" then
        if e.state == "connected" and model.online ~= "connected" then
            model.board, model.revision, model.error, model.ended = empty, 0, false, false
        elseif model.online == "connected" and e.state ~= "connected" then
            model.awaiting, model.nextMessage, model.ended = false, nil, true
        end
        model.online, model.host = e.state, e.host
    elseif e.kind == "multiplayer.message" then
        receive(model, e)
    elseif e.kind == "task.complete" and e.taskId == model.controlTask then
        model.controlTask = nil
        model.error = not e.ok and e.error ~= "canceled" and e.error ~= "advancedFeatureLocked"
    elseif e.kind == "task.complete" and e.taskId == model.sendTask then
        model.sendTask = nil
        if not e.ok then model.error, model.awaiting = true, false end
        local nextMessage = model.nextMessage
        model.nextMessage = nil
        if model.online == "connected" and nextMessage then send(model, nextMessage) end
    else return end
    widget.invalidate()
end

local function status(model, result)
    if model.error then return l10n.tr("lua_widget.tic_tac_toe.error") end
    if model.online == "invited" then return l10n.tr("lua_widget.tic_tac_toe.invited") end
    if model.online == "waiting" then return l10n.tr("lua_widget.tic_tac_toe.waiting") end
    if model.online == "listening" then return l10n.tr("lua_widget.tic_tac_toe.listening") end
    if model.online == "connecting" then return l10n.tr("lua_widget.tic_tac_toe.connecting") end
    if result == "X" then return l10n.tr("lua_widget.tic_tac_toe.win_x") end
    if result == "O" then return l10n.tr("lua_widget.tic_tac_toe.win_o") end
    if result == "draw" then return l10n.tr("lua_widget.tic_tac_toe.draw") end
    if model.online == "connected" then
        if model.awaiting then return l10n.tr("lua_widget.tic_tac_toe.syncing") end
        if turn(model.board) == (model.host and "X" or "O") then return l10n.tr("lua_widget.tic_tac_toe.your_turn") end
        return l10n.tr("lua_widget.tic_tac_toe.their_turn")
    end
    if model.ended then return l10n.tr("lua_widget.tic_tac_toe.ended") end
    if turn(model.board) == "X" then return l10n.tr("lua_widget.tic_tac_toe.turn_x") end
    return l10n.tr("lua_widget.tic_tac_toe.turn_o")
end

local function desktop(_, model)
    local width, height = layout.contentWidth(), layout.contentHeight()
    local side = math.min(width, height)
    local gap, cell = side * 0.022, side * 0.155
    local darkInk = widget.theme().contentTheme == 1
    local tile = darkInk and 0xE4EDF7 or 0x253B55
    local highlight = darkInk and 0xC6E0F6 or 0x345476
    local result, winning = outcome(model.board)
    local online = model.online
    local canMove = not result and online ~= "connecting" and online ~= "waiting" and
        (online ~= "connected" or (not model.awaiting and turn(model.board) == (model.host and "X" or "O")))
    local rows = {}
    for y = 0, 2 do
        local buttons = {}
        for x = 1, 3 do
            local index = y * 3 + x
            local mark = model.board:sub(index, index)
            local id = "cell." .. index
            local isWin = winning and (index == winning[1] or index == winning[2] or index == winning[3])
            local playable = canMove and mark == "."
            buttons[x] = view.box({key = id, width = cell, height = cell, flexShrink = 0,
                events = playable and {click = {id = id}} or nil,
                accessibility = {role = playable and "button" or "text",
                    label = l10n.tr("lua_widget.tic_tac_toe.cell") .. " " .. index .. " " .. mark},
                style = {background = isWin and highlight or tile,
                    borderColor = isWin and "systemAccent" or "border", borderWidth = side * 0.005, cornerRadius = cell * 0.21},
                hoverStyle = playable and {background = highlight} or nil,
                children = {view.text({key = id .. ".mark", text = mark == "." and "·" or mark,
                    width = "fill", height = "fill", fontSize = cell * 0.55,
                    bold = true, textAlign = "center", verticalAlign = "center",
                    style = {foreground = mark == "." and "textSecondary" or "textPrimary"}})}})
        end
        rows[#rows + 1] = view.row({key = "row." .. y, height = cell, gap = gap,
            flexShrink = 0, justifyContent = "center", children = buttons})
    end
    local function control(id, label)
        return view.button({key = id, label = label, width = "fill", height = side * 0.115,
            padding = {horizontal = side * 0.025, vertical = 0}, fontSize = side * 0.045,
            textAlign = "center", action = {id = id}, enabled = not model.controlTask or id == "leave"})
    end
    local controls = {}
    if online == "connected" or online == "waiting" or online == "connecting" then
        controls[1] = control("leave", l10n.tr("lua_widget.tic_tac_toe.leave"))
        if online == "waiting" then controls[2] = control("invite", l10n.tr("lua_widget.tic_tac_toe.invite"))
        elseif online == "connected" then
            if model.host then controls[2] = control("new", l10n.tr("lua_widget.tic_tac_toe.new"))
            elseif model.awaiting then controls[2] = control("sync", l10n.tr("lua_widget.tic_tac_toe.sync")) end
        end
    else
        controls[1] = control("new", l10n.tr("lua_widget.tic_tac_toe.new"))
        if online == "invited" then controls[2] = control("join", l10n.tr("lua_widget.tic_tac_toe.join"))
        elseif online == "idle" or online == "locked" then
            controls[2] = control("create", l10n.tr("lua_widget.tic_tac_toe.create"))
            controls[3] = control("listen", l10n.tr("lua_widget.tic_tac_toe.listen"))
        end
    end
    return view.column({key = "game", width = "fill", height = "fill", gap = gap,
        padding = {horizontal = side * 0.055, vertical = side * 0.043}, justifyContent = "center", children = {
            view.text({key = "title", text = l10n.tr("lua_widget.tic_tac_toe.name"), height = side * 0.10,
                fontSize = side * 0.072, bold = true, textAlign = "center", verticalAlign = "center"}),
            view.column({key = "board", height = cell * 3 + gap * 2, gap = gap, flexShrink = 0, children = rows}),
            view.text({key = "status", text = status(model, result), height = side * 0.082,
                fontSize = side * 0.050, textAlign = "center", verticalAlign = "center", style = {foreground = "textSecondary"}}),
            view.row({key = "controls", width = "fill", height = side * 0.115, gap = gap, flexShrink = 0, children = controls}),
        }})
end

return widget.define({name = l10n.tr("lua_widget.tic_tac_toe.name"),
    useCustomStyle = true, followPersonalizationDefault = true,
    bg = 0x172131, border = 0xFFFFFF, alpha = 0.46, borderAlpha = 0.17, gradientEndA = 0.30,
    setup = setup, event = event, view = desktop})
