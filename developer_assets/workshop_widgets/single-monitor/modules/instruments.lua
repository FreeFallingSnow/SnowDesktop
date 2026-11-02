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

local function measure(text, size, bold, width)
    return draw.measureText(text, size, width, bold)
end

local function bounds(metric)
    return metric.ink or { left = 0, top = 0, width = metric.width, height = metric.height }
end

-- Numeric fonts depend on a stable reference, never on the current reading.
local function font(reference, size, width, height, bold, wrap)
    for _ = 1, 3 do
        local metric = measure(reference, size, bold, wrap and width or nil)
        local ink = bounds(metric)
        local factor = math.min(1, width * 0.98 / math.max(metric.width, ink.width),
            height * 0.98 / math.max(ink.height, size * 0.2))
        if factor >= 0.999 then break end
        size = size * factor
    end
    return size
end

local function text(x, y, width, height, value, size, color, bold, align, wrap)
    local metric = measure(value, size, bold, wrap and width or nil)
    local ink = bounds(metric)
    local left = align == "center" and (width - ink.width) / 2 or
        (align == "end" and width - ink.width or 0)
    local ox, oy = x + left - ink.left, y + (height - ink.height) / 2 - ink.top
    -- Measurement and drawing share one line box; leave rounding room without
    -- invoking DirectWrite's end ellipsis on values that already fit.
    draw.text(ox, oy, value, size, color, wrap and width or math.max(width, metric.width + size * 0.12),
        bold, not wrap, math.max(metric.height + size * 0.15, height + size * 0.15))
end

local function label(x, y, width, height, value, size, color, align, bold, wrap)
    local fitted = font(value, size, width, height, bold, wrap)
    text(x, y, width, height, value, fitted, color, bold, align, wrap)
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
    local w,h=layout.contentWidth(),layout.contentHeight()
    local s=math.min(w,h*0.8)
    local t=math.min(w*1.35,h*0.89)
    local pad,p=s*0.065,M.palette()
    local x,bw=pad,w-pad*2
    local wide=w/h>=1.6
    local ringMode=mode=="gpu" or mode=="disk"
    local ringWide=wide and ringMode and reading.status=="ready"
    local titleH,iconSize=t*0.105,t*0.065
    local titleFont=font(c[mode],t*0.064,bw*0.62-t*0.155,titleH,true)
    local titleInk=bounds(measure(c[mode],titleFont,true))
    local selectorW=titleInk.width+t*0.155
    local selectorX=x+bw-selectorW
    local titleX=selectorX+iconSize+t*0.028
    M.icon(mode,selectorX,pad+(titleH-iconSize)/2,iconSize,p.text)
    text(titleX,pad,titleInk.width+t*0.004,titleH,c[mode],titleFont,p.text,true)
    local arrowX=titleX+titleInk.width+t*0.025
    local arrowY=pad+titleH*0.5
    path({{arrowX,arrowY-t*0.008},{arrowX+t*0.014,arrowY+t*0.007},
        {arrowX+t*0.028,arrowY-t*0.008}},p.sub,t*0.005)
    local source=reading.source or c[mode.."_source"] or ""
    if reading.stale then source=source=="" and c.stale or source.." · "..c.stale end
    if source~="" then
        local sourceSize=t*0.049
        local metric=measure(source,sourceSize,false)
        local ink=bounds(metric)
        local sourceY=pad+(titleH-ink.height)/2-ink.top-math.max(0,(titleH-metric.height)/2)
        draw.marqueeText({key="monitor.source",x=x,y=sourceY,width=selectorX-x-t*0.045,height=titleH,
            text=source,size=sourceSize,color=p.sub,speed=t*0.12,gap=t*0.10})
    end
    local headerBottom=pad+titleH+t*0.065
    if interaction.isHovered("monitor.selector") then
        draw.line(titleX,pad+titleH,arrowX+t*0.028,pad+titleH,s*0.004,p.sub,0.6)
    end
    local y=headerBottom
    local bh=h-pad-y
    local function percentage(px,py,pw,ph,size,align)
        local fixed=font("100%",size,pw,ph,true)
        text(px,py,pw,ph,format.percent(reading.percent),fixed,p.text,true,align)
    end
    local function capacityLabel(px,py,pw,ph,align)
        local fixed=font(format.capacityReference(reading.total),t*0.065,pw,ph,false)
        text(px,py,pw,ph,format.capacity(reading.used,reading.total),fixed,p.sub,false,align)
    end
    if reading.status~="ready" then
        local groupH=math.min(bh,t*0.60)
        local gy=y+(bh-groupH)/2
        label(x,gy,bw,groupH*0.48,"—",t*0.25,p.sub,"center",true)
        label(x,gy+groupH*0.56,bw,groupH*0.44,c[reading.status] or c.unavailable,
            t*0.059,p.sub,"center",false,true)
    elseif ringMode then
        local stroke=s*0.061
        local radius,cx,cy,fx,fy,fw,fh
        if ringWide then
            radius=math.min((bh-stroke)*0.48,bw*0.22)
            cx,cy=x+bw*0.76,y+bh/2
            fx,fy,fw,fh=x,y+bh*0.55,bw*0.48,bh*0.28
        else
            local captionH,gap=t*0.10,s*0.06
            radius=math.min((bw-stroke)*0.47,(bh-captionH-gap-stroke)/2)
            cx,cy=x+bw/2,y+(bh-captionH-gap)/2
            fx,fy,fw,fh=x,y+bh-captionH,bw,captionH
        end
        draw.arc(cx,cy,radius,-90,360,stroke,p.track,p.trackAlpha)
        if reading.percent>0 then draw.arc(cx,cy,radius,-90,reading.percent*3.6,stroke,p[mode]) end
        local inner=radius-stroke/2-s*0.01
        -- This rectangle lies inside the inner circle. Center visible glyph
        -- bounds rather than the font's asymmetric line box.
        percentage(cx-inner*0.90,cy-inner*0.40,inner*1.80,inner*0.80,inner*0.95,"center")
        if mode=="disk" then capacityLabel(fx,fy,fw,fh,"center")
        else label(fx,fy,fw,fh,c.gpu_load,t*0.06,p.sub,"center") end
    elseif mode=="network" then
        local stacked=bw/bh<0.85
        local gap=stacked and s*0.12 or s*0.075
        local cellW=stacked and bw or (bw-gap)/2
        local cellH=stacked and (bh-gap)/2 or bh
        local top=y
        local maximum=1
        for _,entry in ipairs(values) do maximum=math.max(maximum,entry.download or 0,entry.upload or 0) end
        local reference=format.rateReference()
        local valueFont=font(reference,t*0.18,cellW,cellH*0.25,true)
        local nameFont=math.min(font("↓ "..c.download,t*0.052,cellW,cellH*0.13,false),
            font("↑ "..c.upload,t*0.052,cellW,cellH*0.13,false))
        local unitFont=font("MiB/s",t*0.048,cellW,cellH*0.13,false)
        for i,field in ipairs({"download","upload"}) do
            local nx=x+(stacked and 0 or (i-1)*(cellW+gap))
            local ny=top+(stacked and (i-1)*(cellH+gap) or 0)
            local color=i==1 and p.network or p.upload
            text(nx,ny,cellW,cellH*0.13,(i==1 and "↓ " or "↑ ")..c[field],nameFont,color)
            local amount,unit=format.rateParts(reading[field])
            text(nx,ny+cellH*0.17,cellW,cellH*0.25,amount,valueFont,p.text,true)
            text(nx,ny+cellH*0.43,cellW,cellH*0.13,unit,unitFont,p.sub)
            history(values,field,now,nx,ny+cellH*0.64,cellW-s*0.012,cellH*0.35,
                maximum,color,s,p)
        end
    else
        local color=p[mode]
        if mode=="battery" and reading.percent<=20 and not reading.charging then color=p.danger end
        local px,py,pw,ph,gx,gy,gw,gh,captionY,captionH,detailY,detailH
        if wide then
            pw,ph=bw*0.43,bh*0.61
            px,py=x,y+bh*0.04
            detailY,detailH=py+ph+bh*0.05,bh*0.19
            gx,gw=x+bw*0.48,bw*0.52
            captionH=bh*0.19
            captionY=y+bh-captionH
            if mode=="cpu" then gy,gh=y+bh*0.06,bh*0.65
            else
                gh=mode=="battery" and math.min(s*0.20,bh*0.40) or
                    math.min(mode=="vram" and s*0.14 or s*0.09,bh*0.35)
                gy=y+(bh-captionH-gh-s*0.04)/2
            end
        else
            px,py,pw,ph=x,y,bw,bh*(mode=="cpu" and 0.35 or 0.42)
            detailY,detailH=y+bh*0.44,math.min(t*0.11,bh*0.11)
            gx,gw=x,bw
            captionH=math.min(t*0.10,bh*0.10)
            captionY=y+bh-captionH
            if mode=="cpu" then gy,gh=y+bh*0.40,bh*0.46
            else
                gh=math.min(s*(mode=="battery" and 0.20 or (mode=="vram" and 0.14 or 0.09)),bh*0.20)
                gy=captionY-s*0.06-gh
            end
        end
        percentage(px,py,pw,ph,t*0.28)
        if mode=="memory" or mode=="vram" then capacityLabel(px,detailY,pw,detailH) end
        if mode=="cpu" then
            local legendW=s*0.12
            history(values,"value",now,gx,gy,gw-legendW,gh,100,color,s,p)
            label(gx+gw-legendW+s*0.015,gy-s*0.025,legendW-s*0.015,s*0.05,
                "100%",t*0.038,p.sub)
            label(gx+gw-legendW+s*0.015,gy+gh-s*0.025,legendW-s*0.015,s*0.05,
                "0%",t*0.038,p.sub)
            label(gx,captionY,gw,captionH,c.history,t*0.05,p.sub)
        elseif mode=="battery" then
            local batteryW=gw*0.92
            local stroke=s*0.007
            draw.strokeRect(gx,gy,batteryW,gh,p.sub,gh*0.16,stroke)
            draw.rect(gx+batteryW+s*0.014,gy+gh*0.29,gw*0.035,gh*0.42,p.sub,s*0.006)
            local inset=math.min(s*0.017,gh*0.16)
            capacityTrack(gx+inset,gy+inset,batteryW-inset*2,gh-inset*2,reading.percent,color,p,gh*0.09)
            if reading.charging then
                local cx,cy=gx+batteryW*0.52,gy+gh/2
                path({{cx+gh*0.09,cy-gh*0.29},{cx-gh*0.10,cy},
                    {cx+gh*0.10,cy},{cx-gh*0.09,cy+gh*0.29}},p.text,s*0.009)
            end
            local status=reading.charging and c.charging or
                (reading.acPower and (reading.percent>=99 and c.charged or c.plugged_in) or
                (reading.saver and c.saver or c.on_battery))
            -- All power states share a fixed font as charge and AC state change.
            local fixed=t*0.052
            for _,candidate in ipairs({c.charging,c.charged,c.plugged_in,c.saver,c.on_battery}) do
                fixed=math.min(fixed,font(candidate,t*0.052,gw,captionH,false))
            end
            text(gx,captionY,gw,captionH,status,fixed,p.sub)
        else
            capacityTrack(gx,gy,gw,gh,reading.percent,color,p,mode=="vram" and s*0.017 or gh/2)
            if mode=="vram" then
                for i=1,3 do draw.line(gx+gw*i/4,gy+gh+s*0.012,gx+gw*i/4,
                    gy+gh+s*0.025,s*0.004,p.sub,0.6) end
                label(gx,captionY,gw,captionH,c.dedicated,t*0.05,p.sub)
            else
                local fixed=math.min(font(c.used,t*0.05,gw*0.46,captionH,false),
                    font(c.available,t*0.05,gw*0.46,captionH,false))
                text(gx,captionY,gw*0.46,captionH,c.used,fixed,p.sub)
                text(gx+gw*0.54,captionY,gw*0.46,captionH,c.available,fixed,p.sub,false,"end")
            end
        end
    end
    if reading.stale then
        draw.circle(w-pad*0.62,pad*0.55,s*0.012,p.warning)
    end
    interaction.region({key="monitor.body",shape={type="rect",x=0,y=0,width=w,height=h},
        events={contextMenu={id="monitor.menu",scope="component"}},
        accessibility={role="group",label=c[mode]..": "..
            (reading.status=="ready" and (mode=="network" and
                (c.download.." "..format.rate(reading.download)..", "..c.upload.." "..format.rate(reading.upload)) or
                format.percent(reading.percent)) or (c[reading.status] or c.unavailable))}})
    interaction.region({key="monitor.selector",shape={type="rect",x=selectorX,y=pad*0.5,
        width=selectorW,height=titleH+pad},cursor="hand",
        events={click={id="selector.open"},contextMenu={id="monitor.menu",scope="component"}},
        accessibility={role="button",label=c.choose}})
end

return M
