local M = {}

function M.palette()
    local dark = widget.theme().contentTheme == 1
    return {
        text = dark and 0x152334 or 0xF5F8FF,
        sub = dark and 0x48596C or 0xC1D0E2,
        track = dark and 0x64748B or 0xB6C7DD,
        trackAlpha = dark and 0.18 or 0.14,
        cpu = dark and 0x007E99 or 0x53D6F2,
        gpu = dark and 0x7050BF or 0xB8A0FF,
        memory = dark and 0x167B54 or 0x88E5AF,
        vram = dark and 0xAD4F68 or 0xF2A3AD,
        battery = dark and 0x187C49 or 0x7DE3A7,
        disk = dark and 0x2568BD or 0x78B9FF,
        network = dark and 0x096BAA or 0x65BFFF,
        upload = dark and 0xAF573D or 0xF9AE99,
        warning = dark and 0xAD5C00 or 0xFFD185,
        danger = dark and 0xBC3045 or 0xFF8B98,
    }
end

local function measure(text, size, bold)
    return draw.measureText(text, size, nil, bold)
end

-- Fit the complete numeric value. Labels use the host's single-line ellipsis.
function M.text(x, y, value, size, color, width, bold, align, fit)
    local metric = measure(value, size, bold)
    if fit and metric.width > width then
        size = size * width / metric.width
        metric = measure(value, size, bold)
    end
    local offset = align == "center" and math.max(0, (width - metric.width) / 2) or
        (align == "end" and math.max(0, width - metric.width) or 0)
    draw.text(x + offset, y, value, size, color, width - offset, bold, true, size * 1.5)
end

local function path(points, color, thickness, alpha)
    if #points < 2 then return end
    local commands = { { op = "move", x = points[1][1], y = points[1][2] } }
    for i = 2, #points do commands[#commands + 1] = { op = "line", x = points[i][1], y = points[i][2] } end
    draw.path(commands, { strokeColor = color, thickness = thickness, alpha = alpha or 1 })
end

function M.icon(mode, x, y, size, color)
    local t = size * 0.075
    if mode == "cpu" or mode == "gpu" then
        draw.strokeRect(x + size * 0.20, y + size * 0.20, size * 0.60, size * 0.60, color, size * 0.08, t)
        for i = 1, 3 do
            local p = size * (0.2 + i * 0.15)
            draw.line(x + p, y, x + p, y + size * 0.2, t, color)
            draw.line(x + p, y + size * 0.8, x + p, y + size, t, color)
            draw.line(x, y + p, x + size * 0.2, y + p, t, color)
            draw.line(x + size * 0.8, y + p, x + size, y + p, t, color)
        end
        if mode == "gpu" then draw.circle(x + size * 0.5, y + size * 0.5, size * 0.12, color) end
    elseif mode == "memory" or mode == "vram" then
        draw.strokeRect(x, y + size * 0.20, size, size * 0.56, color, size * 0.05, t)
        for i = 0, 2 do draw.rect(x + size * (0.14 + i * 0.26), y + size * 0.34,
            size * 0.15, size * 0.23, color, size * 0.02) end
        for i = 1, 5 do draw.line(x + size * i / 6, y + size * 0.76,
            x + size * i / 6, y + size * 0.91, t, color) end
    elseif mode == "battery" then
        draw.strokeRect(x, y + size * 0.25, size * 0.85, size * 0.5, color, size * 0.06, t)
        draw.rect(x + size * 0.89, y + size * 0.39, size * 0.11, size * 0.22, color, size * 0.02)
        draw.rect(x + size * 0.12, y + size * 0.37, size * 0.45, size * 0.26, color, size * 0.02)
    elseif mode == "disk" then
        draw.strokeRect(x + size * 0.12, y, size * 0.76, size, color, size * 0.08, t)
        draw.arc(x + size * 0.5, y + size * 0.36, size * 0.16, 0, 360, t, color)
        draw.line(x + size * 0.26, y + size * 0.77, x + size * 0.74, y + size * 0.77, t, color)
    else
        draw.arc(x + size * 0.5, y + size * 0.5, size * 0.45, 0, 360, t, color)
        draw.line(x + size * 0.05, y + size * 0.5, x + size * 0.95, y + size * 0.5, t, color)
        draw.arc(x + size * 0.5, y + size * 0.5, size * 0.23, 0, 360, t, color)
        draw.line(x + size * 0.5, y + size * 0.05, x + size * 0.5, y + size * 0.95, t, color)
    end
end

local function history(historyValues, field, now, x, y, width, height, maximum, color, scale, palette)
    for i = 0, 2 do
        draw.line(x, y + height * i / 2, x + width, y + height * i / 2,
            scale * 0.003, palette.track, 0.2)
    end
    local points, lastTime, endpoint = {}, nil, nil
    local function flush()
        if #points >= 2 then
            local area = { { op = "move", x = points[1][1], y = y + height } }
            for _, point in ipairs(points) do area[#area + 1] = { op = "line", x = point[1], y = point[2] } end
            area[#area + 1] = { op = "line", x = points[#points][1], y = y + height }
            area[#area + 1] = { op = "close" }
            draw.path(area, { fillColor = color, alpha = 0.10 })
            path(points, color, scale * 0.009)
        end
        points = {}
    end
    for _, entry in ipairs(historyValues) do
        local value = entry[field]
        if value and entry.time >= now - 60000 then
            if lastTime and entry.time - lastTime > 3000 then flush() end
            local point = { x + width * math.max(0, math.min(1, (entry.time - now + 60000) / 60000)),
                y + height * (1 - math.min(1, value / maximum)) }
            points[#points + 1], endpoint, lastTime = point, point, entry.time
        end
    end
    flush()
    if endpoint then draw.circle(endpoint[1], endpoint[2], scale * 0.014, color) end
end

local function capacityTrack(x, y, width, height, percent, color, palette, radius)
    draw.rect(x, y, width, height, palette.track, radius, palette.trackAlpha)
    if percent > 0 then
        draw.pushClip(x, y, width * percent / 100, height)
        draw.rect(x, y, width, height, color, radius)
        draw.popClip()
    end
    draw.strokeRect(x, y, width, height, palette.track, radius, height * 0.025, 0.35)
end

function M.render(mode, reading, values, now, c, format)
    local w, h = layout.contentWidth(), layout.contentHeight()
    local s = math.min(w, h)
    local pad, p = s * 0.065, M.palette()
    local titleHeight = s * 0.14
    M.icon(mode, pad, pad + s * 0.018, s * 0.070, p.text)
    local titleX, titleWidth = pad + s * 0.105, w * 0.50 - pad - s * 0.105
    M.text(titleX, pad, c[mode], s * 0.077, p.text, titleWidth, true)
    local measured = measure(c[mode], s * 0.077, true).width
    local arrowX = titleX + math.min(titleWidth, measured) + s * 0.025
    path({ { arrowX, pad + s * 0.045 }, { arrowX + s * 0.016, pad + s * 0.061 },
        { arrowX + s * 0.032, pad + s * 0.045 } }, p.sub, s * 0.005)
    local source = reading.source or c[mode .. "_source"] or ""
    M.text(w * 0.56, pad + s * 0.009, source, s * 0.047, p.sub, w * 0.44 - pad, false, "end")
    if interaction.isHovered("monitor.selector") then
        draw.line(titleX, pad + s * 0.105, math.min(w * 0.52, arrowX + s * 0.04),
            pad + s * 0.105, s * 0.004, p.sub, 0.6)
    end
    local x, y, bw, bh = pad, pad + titleHeight, w - pad * 2, h - pad * 2 - titleHeight
    if reading.status ~= "ready" then
        M.text(x, y + bh * 0.15, "—", s * 0.27, p.sub, bw, true, "center")
        M.text(x, y + bh * 0.64, c[reading.status] or c.unavailable, s * 0.055, p.sub, bw, false, "center", true)
    else
        local color = p[mode]
        if mode == "battery" and reading.percent <= 20 and not reading.charging then color = p.danger end
        local wide = w / h > 1.65
        local mainSize = s * 0.27
        if mode == "gpu" or mode == "disk" then
            local radius = math.min(bw * 0.35, bh * 0.33)
            local cx, cy = x + bw / 2, y + bh * 0.43
            draw.arc(cx, cy, radius, -90, 360, s * 0.073, p.track, p.trackAlpha)
            if reading.percent > 0 then draw.arc(cx, cy, radius, -90,
                reading.percent * 3.6, s * 0.073, color) end
            M.text(cx - radius * 0.90, cy - s * 0.125, format.percent(reading.percent),
                s * 0.22, p.text, radius * 1.8, true, "center", true)
            local footer = mode == "disk" and format.capacity(reading.used, reading.total) or c.gpu_load
            M.text(x, y + bh * 0.84, footer, s * 0.060, p.sub, bw, false, "center", true)
        elseif mode == "network" then
            local gap, column = s * 0.065, (bw - s * 0.065) / 2
            local graphY, graphH = y + bh * 0.43, bh * 0.39
            local maximum = 1
            for _, entry in ipairs(values) do maximum = math.max(maximum, entry.download or 0, entry.upload or 0) end
            for i, field in ipairs({ "download", "upload" }) do
                local nx = x + (i - 1) * (column + gap)
                local netColor = i == 1 and color or p.upload
                M.text(nx, y + bh * 0.04, (i == 1 and "↓ " or "↑ ") .. format.rate(reading[field]),
                    s * 0.117, p.text, column, true, nil, true)
                history(values, field, now, nx, graphY, column - s * 0.012, graphH,
                    maximum, netColor, s, p)
                M.text(nx, y + bh * 0.87, c[field], s * 0.050, p.sub, column, false, "center")
            end
        else
            local mainWidth = wide and bw * 0.43 or bw
            local mainY = wide and y + bh * 0.12 or y
            M.text(x, mainY, format.percent(reading.percent), mainSize, p.text, mainWidth, true, nil, true)
            local gx = wide and x + bw * 0.48 or x
            local gy = wide and y + bh * 0.18 or y + bh * 0.50
            local gw = wide and bw * 0.52 or bw
            if mode == "cpu" then
                local gh = wide and bh * 0.59 or bh * 0.33
                history(values, "value", now, gx, gy, gw - s * 0.11, gh, 100, color, s, p)
                M.text(gx + gw - s * 0.10, gy - s * 0.019, "100%", s * 0.038, p.sub, s * 0.11)
                M.text(gx + gw - s * 0.10, gy + gh - s * 0.025, "0%", s * 0.038, p.sub, s * 0.11)
                M.text(gx, y + bh * 0.88, c.history, s * 0.049, p.sub, gw)
            elseif mode == "battery" then
                local batteryY = gy + bh * 0.01
                local batteryH, batteryW = math.min(s * 0.20, bh * 0.24), gw * 0.92
                draw.strokeRect(gx, batteryY, batteryW, batteryH, p.sub, batteryH * 0.16, s * 0.008)
                draw.rect(gx + batteryW + s * 0.014, batteryY + batteryH * 0.29,
                    gw * 0.035, batteryH * 0.42, p.sub, s * 0.006)
                local inset = s * 0.018
                capacityTrack(gx + inset, batteryY + inset, batteryW - inset * 2,
                    batteryH - inset * 2, reading.percent, color, p, batteryH * 0.09)
                if reading.charging then
                    local cx = gx + batteryW * 0.52
                    local cy = batteryY + batteryH / 2
                    path({ { cx + s * 0.015, cy - s * 0.055 }, { cx - s * 0.018, cy },
                        { cx + s * 0.018, cy }, { cx - s * 0.015, cy + s * 0.055 } }, p.text, s * 0.012)
                end
                local status = reading.charging and c.charging or
                    (reading.acPower and (reading.percent >= 99 and c.charged or c.plugged_in) or
                    (reading.saver and c.saver or c.on_battery))
                M.text(gx, y + bh * 0.88, status, s * 0.052, p.sub, gw, false, nil, true)
            else
                local detailY = wide and mainY + mainSize * 1.34 or y + bh * 0.35
                M.text(x, detailY, format.capacity(reading.used, reading.total), s * 0.068,
                    p.sub, mainWidth, false, nil, true)
                local trackY = wide and gy + bh * 0.17 or y + bh * 0.62
                local trackH = mode == "vram" and s * 0.14 or s * 0.085
                capacityTrack(gx, trackY, gw, trackH, reading.percent, color, p,
                    mode == "vram" and s * 0.018 or trackH / 2)
                if mode == "vram" then
                    for i = 1, 3 do draw.line(gx + gw * i / 4, trackY + trackH + s * 0.012,
                        gx + gw * i / 4, trackY + trackH + s * 0.025, s * 0.004, p.sub, 0.6) end
                    M.text(gx, y + bh * 0.88, c.dedicated, s * 0.050, p.sub, gw)
                else
                    M.text(gx, y + bh * 0.88, c.used, s * 0.050, p.sub, gw / 2)
                    M.text(gx + gw / 2, y + bh * 0.88, c.available, s * 0.050, p.sub, gw / 2, false, "end")
                end
            end
        end
        if reading.stale then
            draw.circle(w - pad * 0.62, pad * 0.55, s * 0.012, p.warning)
            M.text(x, h - s * 0.055, c.stale, s * 0.038, p.warning, bw, false, "end")
        end
    end
    interaction.region({ key = "monitor.body", shape = { type = "rect", x = 0, y = 0, width = w, height = h },
        events = { contextMenu = { id = "monitor.menu", scope = "component" } },
        accessibility = { role = "group", label = c[mode] .. ": " ..
            (reading.status == "ready" and (mode == "network" and
                (c.download .. " " .. format.rate(reading.download) .. ", " .. c.upload .. " " .. format.rate(reading.upload)) or
                format.percent(reading.percent)) or (c[reading.status] or c.unavailable)) } })
    interaction.region({ key = "monitor.selector", shape = { type = "rect", x = pad, y = pad * 0.6,
        width = w * 0.52 - pad, height = titleHeight }, cursor = "hand",
        events = { click = { id = "selector.open" }, contextMenu = { id = "monitor.menu", scope = "component" } },
        accessibility = { role = "button", label = c.choose } })
end

return M
