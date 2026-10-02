local weather=module.require("modules/weather.lua")
local json=module.require("modules/json.lua")
local cities=module.require("modules/cities.lua")
local skies={sky=resource.image("sky"),cloudy=resource.image("cloudy"),rain=resource.image("rain"),
    snow=resource.image("snow"),fog=resource.image("fog"),night=resource.image("night")}
local weekdayKeys={"lua_widget.sky_weather.sun_day","lua_widget.sky_weather.mon_day",
    "lua_widget.sky_weather.tue_day","lua_widget.sky_weather.wed_day","lua_widget.sky_weather.thu_day",
    "lua_widget.sky_weather.fri_day","lua_widget.sky_weather.sat_day"}
local langs={["zh-CN"]="zh",["zh-TW"]="zh",["en-US"]="en",["ja-JP"]="ja",["ko-KR"]="ko",
    ["de-DE"]="de",["fr-FR"]="fr",["es-ES"]="es",["es-419"]="es",["pt-BR"]="pt"}
local sourceKeys={satellite="lua_widget.sky_weather.location_satellite",wifi="lua_widget.sky_weather.location_wifi",cellular="lua_widget.sky_weather.location_cellular"}
local function config()
    local selection=storage.get("citySelection")
    local valid=cities.valid(selection)
    return {location=valid and tostring(selection.lat)..","..tostring(selection.lon) or "",
        cityName=valid and (selection.device and l10n.tr("lua_widget.sky_weather.current_location") or selection.name) or nil,
        interval=math.max(600,math.min(21600,tonumber(storage.get("interval")) or 1800)),
        unit=storage.get("unit")=="f" and "f" or "c"}
end
local function palette()
    local dark=widget.theme().contentTheme==1
    return {primary=dark and 0x142D50 or 0xFFFFFF,secondary=dark and 0x375779 or 0xDCEAFF,
        card=dark and 0xFFFFFF or 0x7DA8E4,cardAlpha=dark and 0.32 or 0.16,outline=dark and 0x4F76A1 or 0xE3EFFF,dark=dark}
end
local function text(x,y,value,size,color,width,bold,height)
    draw.text(x,y,tostring(value),size,color,width,bold or false,true,height or size*1.5)
end
local function center(x,y,width,value,size,color,bold)
    local measured=draw.measureText(tostring(value),size,width,bold or false)
    text(x+math.max(0,(width-measured.width)*0.5),y,value,size,color,width,bold)
end
local function sun(x,y,s)
    draw.circle(x,y,s*0.45,0xFC9012,1)
    for i=1,12 do
        local t=i/12
        local color=0xFF0000+math.floor(146+53*t)*256+math.floor(15+23*t)
        draw.circle(x-s*0.075*t,y-s*0.095*t,s*(0.45-0.035*i),color,1)
    end
end
local moonPath={{op="move",x=0.55,y=-0.78},{op="cubic",x1=-0.08,y1=-1.05,x2=-0.84,y2=-0.50,x=-0.82,y=0.18},
    {op="cubic",x1=-0.80,y1=0.91,x2=0.28,y2=1.12,x=0.72,y=0.48},
    {op="cubic",x1=-0.02,y1=0.56,x2=-0.13,y2=-0.24,x=0.55,y=-0.78},{op="close"}}
local function moon(x,y,s)
    local commands={}
    for _,p in ipairs(moonPath) do
        local q={op=p.op}
        for k,v in pairs(p) do if k~="op" then q[k]=(k:sub(1,1)=="x" and x or y)+v*s*0.46 end end
        commands[#commands+1]=q
    end
    draw.path(commands,{fillColor=0xFFE6A2,alpha=1})
    draw.circle(x+s*0.31,y-s*0.25,s*0.038,0xE4F5FF,1)
end
local function cloud(x,y,s)
    draw.circle(x-s*0.28,y+s*0.04,s*0.21,0xD7E8F9,1)
    draw.circle(x,y-s*0.10,s*0.30,0xE9F3FF,1)
    draw.circle(x+s*0.28,y+s*0.02,s*0.21,0xF6FAFF,1)
    draw.rect(x-s*0.46,y+s*0.03,s*0.94,s*0.27,0xDEECFC,s*0.13,1)
end
local function glyph(kind,x,y,s)
    if kind=="sun" then sun(x,y,s)
    elseif kind=="moon" then moon(x,y,s)
    elseif kind=="cloudSun" or kind=="cloudMoon" then
        if kind=="cloudSun" then sun(x-s*0.15,y-s*0.13,s*0.77) else moon(x-s*0.15,y-s*0.13,s*0.77) end
        cloud(x+s*0.12,y+s*0.19,s*0.74)
    elseif kind=="unknown" then
        draw.circle(x,y,s*0.35,0xD8E8F5,0.7)
        center(x-s*0.5,y-s*0.33,s,l10n.tr("lua_widget.sky_weather.unknown_glyph"),s*0.47,0x294B74,true)
    else
        cloud(x,y-s*0.12,s*0.92)
        if kind=="rain" or kind=="rainHeavy" then
            for i=1,(kind=="rainHeavy" and 4 or 3) do
                local dx=x+s*(-0.45+i*0.19)
                draw.line(dx,y+s*0.25,dx-s*0.06,y+s*0.47,s*0.065,0x79C9FF,1)
            end
        elseif kind=="snow" then
            for i=-1,1 do draw.circle(x+i*s*0.22,y+s*0.39,s*0.065,0xE5F6FF,1) end
        elseif kind=="thunder" then
            draw.path({{op="move",x=x+s*0.09,y=y+s*0.12},{op="line",x=x-s*0.12,y=y+s*0.40},
                {op="line",x=x+s*0.01,y=y+s*0.40},{op="line",x=x-s*0.10,y=y+s*0.62},
                {op="line",x=x+s*0.24,y=y+s*0.28},{op="line",x=x+s*0.10,y=y+s*0.28},{op="close"}},
                {fillColor=0xFFD367,alpha=1})
        elseif kind=="fog" then
            draw.line(x-s*0.4,y+s*0.28,x+s*0.3,y+s*0.28,s*0.055,0xD7E9FC,1)
            draw.line(x-s*0.3,y+s*0.43,x+s*0.4,y+s*0.43,s*0.055,0xD7E9FC,0.7)
        end
    end
end
local function background(_context,m)
    if storage.get("skyBackground")==false then return end
    local w,h=layout.contentWidth(),layout.contentHeight()
    local colors=palette()
    local data=m.weather.weather
    local scene=weather.background(data and data.code,nil)
    if data then scene=weather.background(data.code,data.night) end
    local bitmap=skies[scene.asset]
    if bitmap then draw.imageFit(bitmap,0,0,w,h,"cover","center",1) end
    if colors.dark then draw.gradientRect(0,0,w,h,0xD8EAFD,0xBCD7F5,"vertical",0,0.91)
    elseif scene.night then
        draw.gradientRect(0,0,w,h,0x101C4D,0x223D71,"vertical",0,0.85)
    elseif scene.asset=="rain" then draw.gradientRect(0,0,w,h,0x243B59,0x182D50,"vertical",0,scene.heavy and 0.64 or 0.47)
    elseif scene.asset=="fog" or scene.asset=="snow" then draw.gradientRect(0,0,w,h,0x536F90,0x345572,"vertical",0,0.61)
    elseif scene.asset=="cloudy" then draw.gradientRect(0,0,w,h,scene.overcast and 0x3B4E68 or 0x3E628D,0x294766,"vertical",0,scene.overcast and 0.55 or 0.40)
    else draw.gradientRect(0,0,w,h,0x28528E,0x183E79,"vertical",0,0.64) end
    if scene.mostlyClear and not scene.night then draw.gradientRect(0,0,w,h,0x3978BD,0x285A99,"vertical",0,0.3) end
    if scene.thunder then
        local x,y,s=w*0.88,h*0.11,math.min(w,h)*0.15
        local bolt={{op="move",x=x+s*0.12,y=y},{op="line",x=x-s*0.17,y=y+s*0.52},
            {op="line",x=x,y=y+s*0.52},{op="line",x=x-s*0.08,y=y+s},
            {op="line",x=x+s*0.29,y=y+s*0.35},{op="line",x=x+s*0.10,y=y+s*0.35},{op="close"}}
        draw.path(bolt,{fillColor=colors.dark and 0x5F7794 or 0xDAE9FF,alpha=0.4})
    end
end
local function region(id,x,y,w,h,label)
    interaction.region({key=id,shape={type="roundedRect",x=x,y=y,width=w,height=h,radius=h*0.22},
        cursor="hand",focusable=true,tooltip=label,accessibility={role="button",label=label},
        events={click={id=id},keyDown={id="keyboard",value=id},contextMenu={id="weather.menu"}}})
    if interaction.isHovered(id) or interaction.isFocused(id) then draw.rect(x,y,w,h,palette().primary,h*0.22,0.10) end
    if interaction.isFocused(id) then draw.strokeRect(x,y,w,h,palette().primary,h*0.22,h*0.045,0.8) end
end
local function status(m)
    local a=m.weather
    if a.loading then return l10n.tr("lua_widget.sky_weather.loading") end
    if a.error=="permission" then return l10n.tr("lua_widget.sky_weather.permission") end
    if a.error=="choose_city" then return l10n.tr("lua_widget.sky_weather.choose_hint") end
    if a.error=="city_missing" then return l10n.tr("lua_widget.sky_weather.no_results") end
    if a.error then return l10n.tr("lua_widget.sky_weather.network_error") end
    if a:stale() then return l10n.tr("lua_widget.sky_weather.stale") end
    return ""
end
local function render(_context,m)
    local w,h=layout.contentWidth(),layout.contentHeight();local short=math.min(w,h)
    local p=short*0.065;local colors=palette();local a=m.weather;local data=a.weather;local cfg=config()
    local header=short*0.07;local title=a.city~="" and a.city or l10n.tr("lua_widget.sky_weather.choose_city")
    local headerCenter=p+header*0.18
    local titleWidth=w-p*2-header*3.6
    local titleSize=header*0.68
    local measuredTitle=draw.measureText(title,titleSize,0,true)
    local titleInk=measuredTitle.ink
    region("cities",p,p*0.65,w-p*2-header*2.6,header*1.35,l10n.tr("lua_widget.sky_weather.choose_city"))
    draw.path({{op="move",x=p+header*0.05,y=headerCenter-header*0.09},{op="line",x=p+header*0.62,y=headerCenter-header*0.32},
        {op="line",x=p+header*0.4,y=headerCenter+header*0.32},{op="line",x=p+header*0.3,y=headerCenter+header*0.03},{op="close"}},
        {fillColor=colors.primary,alpha=1})
    text(p+header*0.9,headerCenter-titleInk.top-titleInk.height*0.5,title,titleSize,colors.primary,titleWidth-header*0.8,true,measuredTitle.height)
    local cx=p+header*1.2+math.min(measuredTitle.width,titleWidth-header*0.8)
    draw.line(cx,headerCenter-header*0.075,cx+header*0.15,headerCenter+header*0.075,header*0.04,colors.primary,1)
    draw.line(cx+header*0.15,headerCenter+header*0.075,cx+header*0.30,headerCenter-header*0.075,header*0.04,colors.primary,1)
    local rx=w-p-header*2.25
    region("refresh",rx,p*0.65,header*1.1,header*1.35,l10n.tr("lua_widget.sky_weather.refresh"))
    draw.arc(rx+header*0.55,headerCenter,header*0.23,35,290,header*0.065,colors.primary,0.9)
    draw.line(rx+header*0.78,headerCenter-header*0.08,rx+header*0.80,headerCenter-header*0.25,header*0.055,colors.primary,0.9)
    region("settings",w-p-header,p*0.65,header,header*1.35,l10n.tr("lua_widget.sky_weather.settings"))
    for i=0,2 do draw.circle(w-p-header*0.74+i*header*0.23,headerCenter,header*0.035,colors.primary,1) end
    local wide=w/h>1.8;local tall=w/h<0.7
    local heroX,heroY,heroW,heroH=p,h*0.20,w-p*2,h*0.235
    local fx,fy,fw,fh=p,h*0.49,w-p*2,h*0.37
    if wide then heroY=h*0.26;heroW=w*0.40;heroH=h*0.34;fx=w*0.46;fy=h*0.26;fw=w-fx-p;fh=h*0.57 end
    if tall then heroY=h*0.16;heroH=h*0.19;fy=h*0.40;fh=h*0.46 end
    if not data then
        glyph("cloudSun",w*0.5,h*0.36,short*0.27)
        draw.text(p,h*0.53,status(m),short*0.041,colors.primary,w-p*2,true,false,h*0.14)
        region("cities.empty",w*0.2,h*0.68,w*0.6,short*0.12,l10n.tr("lua_widget.sky_weather.choose_city"))
        draw.rect(w*0.2,h*0.68,w*0.6,short*0.12,colors.card,short*0.027,0.34)
        center(w*0.2,h*0.70,w*0.6,l10n.tr("lua_widget.sky_weather.choose_city"),short*0.047,colors.primary,true)
    else
        local condition,kind=weather.condition(data.code,data.night)
        local icon=math.min(heroH*0.78,heroW*0.21)
        local size=math.min(heroH*0.91,heroW*0.20)
        local value=weather.temperature(data.temp,cfg.unit):gsub("°$","")
        local tx=heroX+icon*1.15;local ix=heroX+heroW*0.67
        local budget=ix-tx-size*0.38
        local natural=draw.measureText(value,size,heroW,false)
        if natural.width>budget then size=size*budget/natural.width end
        local measure=draw.measureText(value,size,heroW,false)
        local numberY=heroY-heroH*0.02
        local numberInk=measure.ink
        local numberTop=numberY+numberInk.top
        glyph(kind,heroX+icon*0.5,numberTop+numberInk.height*0.5,icon)
        text(tx,numberY,value,size,colors.primary,budget,false,heroH)
        local unit=cfg.unit=="f" and l10n.tr("lua_widget.sky_weather.fahrenheit") or l10n.tr("lua_widget.sky_weather.celsius")
        local unitSize=size*0.27
        local unitMeasure=draw.measureText(unit,unitSize,0,false)
        local unitInk=unitMeasure.ink
        text(tx+measure.width+size*0.02,numberTop-unitInk.top,unit,unitSize,colors.primary,size,false,unitMeasure.height)
        local infoSize=short*0.037
        text(ix,heroY+heroH*0.14,l10n.tr(condition),infoSize,colors.primary,heroW*0.36,true)
        text(ix,heroY+heroH*0.39,l10n.tr("lua_widget.sky_weather.feels",weather.temperature(data.feels,cfg.unit)),infoSize*0.91,colors.secondary,heroW*0.36)
        local today=data.days[1]
        text(ix,heroY+heroH*0.63,weather.temperature(today.high,cfg.unit).." / "..weather.temperature(today.low,cfg.unit),infoSize,colors.secondary,heroW*0.36)
        if not wide and not tall then
            local hint=l10n.tr("lua_widget.sky_weather.five_days")
            if data.days[2] and data.days[2].high<today.high-2 then
                local delta=today.high-data.days[2].high
                hint=l10n.tr("lua_widget.sky_weather.cooler",weather.temperature(cfg.unit=="f" and delta*9/5 or delta,"c"))
            end
            text(p,h*0.424,hint,short*0.034,colors.secondary,w-p*2)
        end
        local count=#data.days;local gap=short*0.018
        for i,d in ipairs(data.days) do
            local x,y,cw,ch
            if tall then x=fx;y=fy+(i-1)*(fh/count+gap*0.18);cw=fw;ch=fh/count-gap*0.75
            else cw=(fw-gap*(count-1))/count;ch=fh;x=fx+(i-1)*(cw+gap);y=fy end
            draw.rect(x,y,cw,ch,colors.card,short*0.016,colors.cardAlpha+(i==1 and 0.06 or 0))
            local dayLabel=d.date==data.localTime:sub(1,10) and l10n.tr("lua_widget.sky_weather.today") or l10n.tr(weekdayKeys[(weather.weekday(d.date) or 0)+1])
            local _,dayKind=weather.condition(d.code,false)
            local labelSize=math.min(short*0.039,cw*0.26)
            if tall then
                text(x+cw*0.05,y+ch*0.24,dayLabel,short*0.043,colors.primary,cw*0.29,true)
                glyph(dayKind,x+cw*0.46,y+ch*0.51,ch*0.60)
                text(x+cw*0.65,y+ch*0.24,weather.temperature(d.high,cfg.unit),short*0.047,colors.primary,cw*0.18,true)
                text(x+cw*0.83,y+ch*0.24,weather.temperature(d.low,cfg.unit),short*0.045,colors.secondary,cw*0.16)
            else
                center(x,y+ch*0.08,cw,dayLabel,labelSize,colors.primary,true)
                glyph(dayKind,x+cw*0.5,y+ch*0.40,math.min(cw*0.64,ch*0.24))
                center(x,y+ch*0.61,cw,weather.temperature(d.high,cfg.unit),labelSize*1.18,colors.primary,true)
                center(x,y+ch*0.81,cw,weather.temperature(d.low,cfg.unit),labelSize*1.06,colors.secondary)
            end
        end
    end
    local foot=short*0.0255
    local note=status(m)
    if note=="" and data then note=l10n.tr("lua_widget.sky_weather.updated",data.localTime:match("T(%d%d:%d%d)") or "") end
    text(p,h-short*0.077,l10n.tr("lua_widget.sky_weather.provider"),foot,colors.secondary,w*0.44)
    local noteWidth=w*0.48-p
    local measure=draw.measureText(note,foot,noteWidth,false)
    text(w-p-math.min(measure.width,noteWidth),h-short*0.077,note,foot,colors.secondary,noteWidth)
end
local function panel(_context,m)
    local row=ui.metrics().layoutRowHeight
    local colors={field="surface",border="border",selected="surfaceVariant",accent="info"}
    local function label(key,value,size,height,secondary,bold)
        return view.text({key=key,text=value,width="fill",height=height or row*0.75,fontSize=size,
            bold=bold==true,verticalAlign="center",style={foreground=secondary and "textSecondary" or "textPrimary"}})
    end
    local selected=storage.get("citySelection")
    local function cityButton(key,p)
        local active=cities.valid(selected) and math.abs(selected.lat-p.lat)<0.001 and math.abs(selected.lon-p.lon)<0.001
        return view.button({key=key,label=p.label or p.name,width="fill",height=row*2.1,
            flexShrink=0,fontSize=row*0.54,textAlign="start",padding={left=row*0.32,right=row*0.32,top=row*0.22,bottom=row*0.22},textWrap="wrap",maxLines=2,
            accessibility={label=p.label or p.name},events={click={id="choose",value=p}},
            style={foreground="textPrimary",background=active and colors.selected or colors.field,
                borderColor=active and colors.accent or colors.border,borderWidth=active and 1.3 or 1,cornerRadius=row*0.20},
            hoverStyle={background=colors.selected,borderColor=colors.accent},pressedStyle={background=colors.selected}})
    end
    local items={}
    if m.query=="" then
        local recent=cities.recent(storage.get("recentCities"))
        if #recent>0 then
            items[#items+1]=label("recent.title",l10n.tr("lua_widget.sky_weather.recent"),row*0.50,row*0.82,true,true)
            for i,p in ipairs(recent) do items[#items+1]=cityButton("recent."..i,p) end
        end
        if #recent==0 then
            items[#items+1]=view.column({key="city.empty",width="fill",height=row*4,gap=row*0.20,justifyContent="center",children={
                view.text({key="city.empty.title",text=l10n.tr("lua_widget.sky_weather.search_empty_title"),width="fill",height=row*0.78,
                    fontSize=row*0.54,bold=true,textAlign="center",style={foreground="textPrimary"}}),
                view.text({key="city.empty.hint",text=l10n.tr("lua_widget.sky_weather.search_empty_hint"),width="fill",height=row*2,
                    fontSize=row*0.46,textAlign="center",textWrap="wrap",maxLines=3,verticalAlign="start",style={foreground="textSecondary"}})}})
        end
    else
        local notice=m.searchLoading and l10n.tr("lua_widget.sky_weather.searching") or
            m.searchError and l10n.tr("lua_widget.sky_weather.network_error") or
            #m.results==0 and l10n.tr("lua_widget.sky_weather.no_results") or nil
        if notice then
            local node=label("search.status",notice,row*0.50,row*1.6,true);node.textWrap="wrap";node.maxLines=2
            items[#items+1]=node
        end
        for i,p in ipairs(m.results) do items[#items+1]=cityButton("result."..i,p) end
    end
    local savedInfo=cities.valid(selected) and selected.device and selected.label or nil
    local enabled=not m.locating and widget.hasPermission("location.read")
    local info=m.locationInfo or savedInfo or (not widget.hasPermission("location.read") and
        l10n.tr("lua_widget.sky_weather.location_permission_short") or l10n.tr("lua_widget.sky_weather.location_idle"))
    local hint=label("location.info",info,row*0.46,row*1.20,true);hint.textWrap="wrap";hint.maxLines=2
    local children={
        view.text({key="picker.subtitle",text=l10n.tr("lua_widget.sky_weather.picker_subtitle"),width="fill",height=row*1.2,flexShrink=0,fontSize=row*0.48,
            textWrap="wrap",maxLines=2,verticalAlign="start",style={foreground="textSecondary"}}),
        view.searchBox({key="city.search",value=m.query,placeholder=l10n.tr("lua_widget.sky_weather.search_hint"),
            width="fill",height=row*1.3,flexShrink=0,fontSize=row*0.54,maxBytes=256,padding=row*0.28,
            accessibility={label=l10n.tr("lua_widget.sky_weather.search_hint")},
            style={foreground="textPrimary",background=colors.field,borderColor=colors.border,borderWidth=1,cornerRadius=row*0.22},
            events={change={id="query"},submit={id="search"}}}),
        view.row({key="location.card",width="fill",height=row*2.35,flexShrink=0,padding={left=0,right=0,top=row*0.20,bottom=row*0.20},gap=row*0.36,alignItems="center",
            children={
                view.column({key="location.copy",width="fill",height="auto",gap=row*0.10,children={
                    label("location.title",l10n.tr("lua_widget.sky_weather.use_location"),row*0.54,row*0.72,false,true),hint}}),
                view.button({key="location",label=m.locating and l10n.tr("lua_widget.sky_weather.locating_short") or l10n.tr("lua_widget.sky_weather.locate_short"),
                    width=row*2.6,height=row*1.3,flexShrink=0,fontSize=row*0.50,enabled=enabled,textAlign="center",verticalAlign="center",
                    padding={left=row*0.18,right=row*0.18,top=0,bottom=0},
                    accessibility={label=l10n.tr("lua_widget.sky_weather.use_location")},events={click={id="locate"}},
                    style={foreground=enabled and colors.accent or "textDisabled",background=colors.field,
                        borderColor=colors.border,borderWidth=1,cornerRadius=row*0.18},hoverStyle={background=colors.selected}})
            }}),
        view.scroll({key="city.scroll",width="fill",height="fill",children={view.column({key="city.items",width="fill",height="auto",gap=row*0.22,children=items})}}),
        view.row({key="picker.footer",width="fill",height=row*1.2,flexShrink=0,gap=row*0.2,alignItems="center",children={
            label("picker.source","Open-Meteo · GeoNames",row*0.43,row*1.2,true),
            view.button({key="panel.settings",label=l10n.tr("lua_widget.sky_weather.settings"),width="auto",height=row*1.2,flexShrink=0,
                fontSize=row*0.46,padding=row*0.20,textAlign="center",verticalAlign="center",accessibility={label=l10n.tr("lua_widget.sky_weather.settings")},events={click={id="settings"}},
                style={foreground=colors.accent,borderWidth=0,cornerRadius=row*0.16},hoverStyle={background=colors.selected}})
            }})
    }
    return view.column({key="cities.panel",width="fill",height="fill",padding=row*0.50,gap=row*0.36,children=children})
end

local function setup(context)
    local m={preview=context.preview,query="",results={},searchLoading=false,locationTask=nil,locating=false}
    m.citySearch=cities.search({start=function(args)return task.start("network.request",args)end,cancel=task.cancel,
        decode=json.decode,encode=weather.encode,language=function()return langs[l10n.language()] or "en"end})
    m.weather=weather.new({config=config,permission=function() return widget.hasPermission("network.internet") end,
        language=l10n.language,now=time.now,monotonic=time.monotonic,decode=json.decode,
        start=function(args) return task.start("network.request",args) end,cancel=task.cancel,
        save=function(snapshot) storage.set("snapshot",snapshot) end})
    if context.preview then
        m.weather.city=l10n.tr("lua_widget.sky_weather.dalian")
        m.weather.weather={temp=21,feels=20,code=tonumber(storage.get("previewCode")) or 0,night=storage.get("previewNight")==true or storage.get("previewNight")=="true",localTime="2026-10-02T14:30",
            days={{date="2026-10-02",high=23,low=11,code=2},{date="2026-10-03",high=22,low=17,code=2},
                {date="2026-10-04",high=20,low=12,code=61},{date="2026-10-05",high=19,low=15,code=0},{date="2026-10-06",high=23,low=11,code=0}}}
        m.weather.updated=time.now()
        local previewTemperature=tonumber(storage.get("previewTemperature"))
        if weather.finite(previewTemperature) then m.weather.weather.temp=previewTemperature end
        local s=storage.get("previewState")
        if s=="error" then m.weather.weather=nil;m.weather.error="error"
        elseif s=="loading" then m.weather.weather=nil;m.weather.loading=true
        elseif s=="empty" then m.weather.weather=nil;m.weather.error="choose_city"
        elseif s=="permission" then m.weather.weather=nil;m.weather.error="permission"
        elseif s=="stale" then m.weather.error="error" end
    else
        m.weather:restore(storage.get("snapshot"));m.weather:refresh(false)
        schedule.every("weather.tick",60000,{whenHidden="pause"})
    end
    return m
end
local function search(m)
    m.citySearch:cancel()
    m.results={};m.searchError=nil
    if weather.trim(m.query)=="" or m.preview then m.searchLoading=false;return end
    m.citySearch:query(m.query)
    m.searchLoading=m.citySearch.loading;m.searchError=m.citySearch.error
end
local function choose(m,p)
    if not cities.valid(p) then return end
    if m.locationTask then task.cancel(m.locationTask);m.locationTask=nil;m.locating=false end
    storage.set("citySelection",p)
    if not p.device then storage.set("recentCities",cities.recent(storage.get("recentCities"),p)) end
    m.weather:refresh(true)
    if not p.device then widget.closePanel() end
end
local function event(_context,m,e)
    if e.kind=="task.complete" then
        if m.citySearch:complete(e) then
            m.searchLoading=m.citySearch.loading;m.searchError=m.citySearch.error;m.results=m.citySearch.results
        elseif e.taskId==m.locationTask then
            m.locationTask=nil;m.locating=false
            local p=e.value
            if e.ok and type(p)=="table" then
                if not sourceKeys[p.source] then
                    m.locationInfo=l10n.tr("lua_widget.sky_weather.location_coarse")
                else
                    local detail=l10n.tr("lua_widget.sky_weather.location_accuracy",tostring(weather.round(p.accuracyMeters) or 0),l10n.tr(sourceKeys[p.source]))
                    m.locationInfo=detail
                    choose(m,{name=l10n.tr("lua_widget.sky_weather.current_location"),label=detail,lat=p.latitude,lon=p.longitude,device=true,source=p.source,accuracy=p.accuracyMeters})
                end
            else m.locationInfo=l10n.tr("lua_widget.sky_weather.location_failed") end
        else m.weather:complete(e) end
    elseif e.kind=="schedule" and e.id=="city.search" then search(m)
    elseif e.kind=="schedule" and e.id=="weather.tick" then m.weather:tick()
    elseif e.kind=="visibility" and e.visible and not m.preview then m.weather:tick()
    elseif e.kind=="settings.changed" and not e.preview and not m.preview then m.weather:tick()
    elseif e.kind=="panel" and e.action=="closed" then
        m.citySearch:cancel()
        if m.locationTask then task.cancel(m.locationTask);m.locationTask=nil end
        m.locating=false;m.searchLoading=false;schedule.cancel("city.search")
    elseif e.kind=="action" then
        local id=e.id
        if id=="keyboard" then if e.key~="Enter" and e.key~="Space" then return end;id=e.value end
        if id=="cities" or id=="cities.empty" then widget.openPanel({title=l10n.tr("lua_widget.sky_weather.choose_city"),width=440,height=520})
        elseif id=="refresh" and not m.preview then m.weather:refresh(true)
        elseif id=="settings" then widget.openSettings()
        elseif id=="choose" and not m.preview then choose(m,e.value)
        elseif id=="query" then
            m.query=tostring(e.text or ""):sub(1,256)
            m.citySearch:cancel()
            m.results={};m.searchLoading=weather.trim(m.query)~="";m.searchError=nil
            schedule.after("city.search",350,{whenHidden="pause"})
        elseif id=="search" then schedule.cancel("city.search");search(m)
        elseif id=="locate" and not m.preview and not m.locating then
            if widget.hasPermission("location.read") then
                local taskId=task.start("location.current",{timeoutMs=15000,maximumAgeMs=300000})
                m.locationTask=taskId;m.locating=taskId~=nil
                if taskId then m.locationInfo=nil end
                if not taskId then m.locationInfo=l10n.tr("lua_widget.sky_weather.location_failed") end
            end
        end
    end
    widget.invalidate()
end
local function dispose(_context,m)
    m.weather:dispose()
    m.citySearch:cancel()
    if m.locationTask then task.cancel(m.locationTask) end
    schedule.cancel("weather.tick");schedule.cancel("city.search")
end
return widget.define({name=l10n.tr("lua_widget.sky_weather.name"),useCustomStyle=true,followPersonalizationDefault=true,
    showTitle=false,bg=0x214C86,border=0xD8E8FF,alpha=0.85,borderAlpha=0.15,gradientEndA=0.75,
    backgroundLayer={render=background,blurRadius=0},render=render,panel=panel,setup=setup,event=event,dispose=dispose,
    menu=function() return ui.menu({{id="cities",label=l10n.tr("lua_widget.sky_weather.choose_city")},
        {id="refresh",label=l10n.tr("lua_widget.sky_weather.refresh")},{id="settings",label=l10n.tr("lua_widget.sky_weather.settings")}}) end,
    settings={fields={{key="unit",type="select",label=l10n.tr("lua_widget.sky_weather.unit"),default="c",options={"c","f"},
        optionLabels={l10n.tr("lua_widget.sky_weather.celsius"),l10n.tr("lua_widget.sky_weather.fahrenheit")}},
        {key="interval",type="int",label=l10n.tr("lua_widget.sky_weather.interval"),default=1800,min=600,max=21600},
        {key="skyBackground",type="bool",label=l10n.tr("lua_widget.sky_weather.sky_background"),default=true}}}})
