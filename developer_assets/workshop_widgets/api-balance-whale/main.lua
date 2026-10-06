local providers=module.require("modules/providers.lua")
local client=module.require("modules/client.lua")
local json=module.require("modules/json.lua")
local whale=resource.image("whale")
local function copy()
    return {
        name=l10n.tr("lua_widget.api_balance_whale.name"),description=l10n.tr("lua_widget.api_balance_whale.description"),
        total=l10n.tr("lua_widget.api_balance_whale.total"),remaining=l10n.tr("lua_widget.api_balance_whale.remaining"),
        topped=l10n.tr("lua_widget.api_balance_whale.topped"),granted=l10n.tr("lua_widget.api_balance_whale.granted"),
        purchased=l10n.tr("lua_widget.api_balance_whale.purchased"),used=l10n.tr("lua_widget.api_balance_whale.used"),
        cash=l10n.tr("lua_widget.api_balance_whale.cash"),voucher=l10n.tr("lua_widget.api_balance_whale.voucher"),
        quota=l10n.tr("lua_widget.api_balance_whale.quota"),
        key_limit=l10n.tr("lua_widget.api_balance_whale.key_limit"),available=l10n.tr("lua_widget.api_balance_whale.available"),
        depleted=l10n.tr("lua_widget.api_balance_whale.depleted"),low=l10n.tr("lua_widget.api_balance_whale.low"),
        unlimited=l10n.tr("lua_widget.api_balance_whale.unlimited"),refresh=l10n.tr("lua_widget.api_balance_whale.refresh"),
        settings=l10n.tr("lua_widget.api_balance_whale.settings"),console=l10n.tr("lua_widget.api_balance_whale.console"),
        whale_mode=l10n.tr("lua_widget.api_balance_whale.whale_mode"),data_mode=l10n.tr("lua_widget.api_balance_whale.data_mode"),
        mode=l10n.tr("lua_widget.api_balance_whale.mode"),provider=l10n.tr("lua_widget.api_balance_whale.provider"),
        key=l10n.tr("lua_widget.api_balance_whale.key"),key_hint=l10n.tr("lua_widget.api_balance_whale.key_hint"),
        interval=l10n.tr("lua_widget.api_balance_whale.interval"),threshold=l10n.tr("lua_widget.api_balance_whale.threshold"),
        quota_threshold=l10n.tr("lua_widget.api_balance_whale.quota_threshold"),currency=l10n.tr("lua_widget.api_balance_whale.currency"),
        endpoint=l10n.tr("lua_widget.api_balance_whale.endpoint"),endpoint_hint=l10n.tr("lua_widget.api_balance_whale.endpoint_hint"),
        path=l10n.tr("lua_widget.api_balance_whale.path"),path_hint=l10n.tr("lua_widget.api_balance_whale.path_hint"),
        total_path=l10n.tr("lua_widget.api_balance_whale.total_path"),scale=l10n.tr("lua_widget.api_balance_whale.scale"),
        kind=l10n.tr("lua_widget.api_balance_whale.kind"),balance_kind=l10n.tr("lua_widget.api_balance_whale.balance_kind"),
        quota_kind=l10n.tr("lua_widget.api_balance_whale.quota_kind"),auth=l10n.tr("lua_widget.api_balance_whale.auth"),
        label=l10n.tr("lua_widget.api_balance_whale.label"),loading=l10n.tr("lua_widget.api_balance_whale.loading"),
        key_missing=l10n.tr("lua_widget.api_balance_whale.key_missing"),permission=l10n.tr("lua_widget.api_balance_whale.permission"),
        auth_error=l10n.tr("lua_widget.api_balance_whale.auth_error"),forbidden=l10n.tr("lua_widget.api_balance_whale.forbidden"),
        rate=l10n.tr("lua_widget.api_balance_whale.rate"),server=l10n.tr("lua_widget.api_balance_whale.server"),
        invalid_data=l10n.tr("lua_widget.api_balance_whale.invalid_data"),network=l10n.tr("lua_widget.api_balance_whale.network"),
        configure=l10n.tr("lua_widget.api_balance_whale.configure"),stale=l10n.tr("lua_widget.api_balance_whale.stale"),
        sample=l10n.tr("lua_widget.api_balance_whale.sample"),unknown=l10n.tr("lua_widget.api_balance_whale.unknown"),
        custom=l10n.tr("lua_widget.api_balance_whale.custom"),
        credit=l10n.tr("lua_widget.api_balance_whale.credit"),management_hint=l10n.tr("lua_widget.api_balance_whale.management_hint"),
        show_details=l10n.tr("lua_widget.api_balance_whale.show_details"),back_to_whale=l10n.tr("lua_widget.api_balance_whale.back_to_whale"),
    }
end
local function config()
    local id=storage.get("provider") or "deepseek"
    if not providers.presets[id] then id="deepseek" end
    local p=providers.presets[id]
    local cfg={provider=id,url=p.url,secretRef=storage.get("key_"..id),rawAuth=p.rawAuth,
        interval=math.max(60,math.min(86400,tonumber(storage.get("interval")) or 300)),
        currency=storage.get("currency") or "CNY",kind=p.kind or "balance",console=p.console,label=storage.get("label")}
    if id=="custom" then
        cfg.url=storage.get("endpoint") or "";cfg.kind=storage.get("customKind") or "balance"
        cfg.remainingPath=storage.get("remainingPath") or "data.balance";cfg.totalPath=storage.get("totalPath") or "data.total"
        cfg.scale=tonumber(storage.get("scale")) or 1;cfg.rawAuth=storage.get("authStyle")=="raw"
        cfg.header=storage.get("authHeader") or "Authorization"
        cfg.valid=cfg.header=="Authorization" or cfg.header=="x-api-key" or cfg.header=="api-key"
    end
    -- Provider-specific host-managed passwords; the Lua VM never reads a plaintext key.
    cfg.identity=table.concat({id,tostring(cfg.secretRef or ""),cfg.url or "",cfg.kind,cfg.currency,
        cfg.remainingPath or "",cfg.totalPath or "",tostring(cfg.scale or 1),cfg.header or "",tostring(cfg.rawAuth)},"\n")
    return cfg
end
local function palette()
    local darkText=widget.theme().contentTheme==1
    return {primary=darkText and 0x18243C or 0xF5F8FF,secondary=darkText and 0x516078 or 0xBFCAE0,
        card=darkText and 0xF3F6FF or 0x151E2D,border=darkText and 0xA8B8D1 or 0x586B8D,
        blue=darkText and 0x2864C7 or 0x86B8FF,green=darkText and 0x167A52 or 0x84E3B1,
        orange=darkText and 0xA44C12 or 0xFFBD87,purple=darkText and 0x7250BB or 0xC1A2FF}
end
local function text(x,y,value,size,color,width,bold,height)
    local measured=draw.measureText(tostring(value),size,0,bold or false)
    local ink=measured.ink
    local boxHeight=height or size*1.6
    local originY=y+(boxHeight-ink.height)*0.5-ink.top
    draw.text(x,originY,tostring(value),size,color,width,bold or false,true,measured.height)
end
local function fitted(x,y,value,size,color,width,bold,height)
    local measured=draw.measureText(tostring(value),size,0,bold or false)
    if measured.width>width*0.94 then size=size*width*0.94/measured.width end
    text(x,y,value,size,color,width,bold,height)
end
local function money(n,currency)
    if n==nil then return copy().unknown end
    local scaled=n*1000000
    local precise=math.abs(scaled-math.floor(scaled+0.5))>0.000001
    local amount=precise and tostring(n) or l10n.formatNumber(n,{minimumFractionDigits=2,maximumFractionDigits=6})
    return (currency=="USD" and "$" or currency=="CNY" and "¥" or "")..amount
end
local function number(n) return l10n.formatNumber(n,{minimumFractionDigits=0,maximumFractionDigits=2}) end
local function status(m,c)
    local a=m.client
    if a.loading then return c.loading end
    if a.error then
        local message=a.error=="auth" and c.auth_error or c[a.error] or c.network
        return a.data and c.stale.." · "..message or message
    end
    if m.preview then return c.sample end
    if a.updated then return time.format(a.updated,{dateStyle="short",timeStyle="short"}) end
    return c.key_missing
end
local function badge(data,c)
    if not data then return c.unknown end
    if data.available==false then return c.depleted end
    local lowBalance=math.max(0,tonumber(storage.get("lowBalance")) or 10)
    local lowQuota=math.max(0,math.min(100,tonumber(storage.get("lowQuota")) or 10))
    if data.kind=="balance" and data.remaining<=lowBalance then return c.low end
    if data.kind=="quota" then for _,w in ipairs(data.windows or {}) do if w.percent<=lowQuota then return c.low end end end
    return c.available
end
local function region(id,x,y,w,h,label)
    interaction.region({key=id,shape={type="roundedRect",x=x,y=y,width=w,height=h,radius=h*0.18},
        cursor="hand",focusable=true,tooltip=label,accessibility={role="button",label=label},
        events={click={id=id},keyDown={id="keyboard",value=id},contextMenu={id="menu",scope="component"}}})
    if interaction.isHovered(id) or interaction.isFocused(id) then draw.rect(x,y,w,h,palette().primary,h*0.18,0.1) end
    if interaction.isFocused(id) then draw.strokeRect(x,y,w,h,palette().blue,h*0.18,h*0.035,0.9) end
end
local function card(x,y,w,h,colors,radius)
    draw.rect(x,y,w,h,colors.card,radius,0.54)
    draw.strokeRect(x,y,w,h,colors.border,radius,radius*0.065,0.20)
end
local function bubble(x,y,w,h,colors,r,wide,row,tailFraction)
    local right,bottom=x+w,y+h;local k=r*0.55228475
    local path={{op="move",x=x+r,y=y},{op="line",x=right-r,y=y},
        {op="cubic",x1=right-r+k,y1=y,x2=right,y2=y+r-k,x=right,y=y+r}}
    local function add(command) path[#path+1]=command end
    if wide then
        local cy=y+h*0.42
        add({op="line",x=right,y=cy-row*0.30})
        add({op="cubic",x1=right,y1=cy-row*0.12,x2=right+row*0.28,y2=cy-row*0.03,x=right+row*0.48,y=cy+row*0.03})
        add({op="quadratic",x1=right+row*0.56,y1=cy+row*0.08,x=right+row*0.46,y=cy+row*0.13})
        add({op="cubic",x1=right+row*0.20,y1=cy+row*0.23,x2=right,y2=cy+row*0.17,x=right,y=cy+row*0.33})
    end
    add({op="line",x=right,y=bottom-r})
    add({op="cubic",x1=right,y1=bottom-r+k,x2=right-r+k,y2=bottom,x=right-r,y=bottom})
    if not wide then
        local cx=x+w*tailFraction
        add({op="line",x=cx+row*0.34,y=bottom})
        add({op="cubic",x1=cx+row*0.24,y1=bottom,x2=cx+row*0.21,y2=bottom+row*0.30,x=cx+row*0.20,y=bottom+row*0.42})
        add({op="quadratic",x1=cx+row*0.18,y1=bottom+row*0.50,x=cx+row*0.12,y=bottom+row*0.44})
        add({op="cubic",x1=cx-row*0.10,y1=bottom+row*0.18,x2=cx-row*0.20,y2=bottom,x=cx-row*0.34,y=bottom})
    end
    add({op="line",x=x+r,y=bottom})
    add({op="cubic",x1=x+r-k,y1=bottom,x2=x,y2=bottom-r+k,x=x,y=bottom-r})
    add({op="line",x=x,y=y+r})
    add({op="cubic",x1=x,y1=y+r-k,x2=x+r-k,y2=y,x=x+r,y=y})
    add({op="close"})
    draw.path(path,{fillColor=colors.card,alpha=0.54})
    draw.path(path,{strokeColor=colors.border,thickness=r*0.065,alpha=0.20})
end
local function hero(x,y,w,h,m,c,colors,size)
    local data=m.client.data;local pad=size*0.65
    text(x+pad,y+pad*0.55,data and data.kind=="quota" and c.remaining or c.total,size*0.56,colors.secondary,w-pad*2,false,size)
    local amount=c.unknown
    if data then
        if data.unlimited then amount=c.unlimited
        elseif data.kind=="balance" then amount=money(data.remaining,data.currency)
        elseif data.windows[1] then amount=data.currency and money(data.windows[1].remaining,data.currency) or number(data.windows[1].percent).."%" end
    end
    fitted(x+pad,y+h*0.35,amount,size*1.3,colors.blue,w-pad*2,true,size*1.8)
    local label=badge(data,c)
    local labelWidth=math.min(w-pad*2,draw.measureText(label,size*0.42,0,true).width+size*0.65)
    local by=y+h-size*0.94
    draw.rect(x+pad,by,labelWidth,size*0.66,label==c.available and colors.green or colors.orange,size*0.25,0.12)
    text(x+pad+size*0.3,by+size*0.03,label,size*0.42,label==c.available and colors.green or colors.orange,labelWidth-size*0.45,true,size*0.64)
    if data and data.currency then
        local unitWidth=draw.measureText(data.currency,size*0.4,0,false).width+size*0.12
        text(x+w-pad-unitWidth,by,data.currency,size*0.4,colors.secondary,unitWidth,false,size*0.66)
    end
end
local function details(x,y,w,h,m,c,colors,row,stack)
    local data=m.client.data;if not data or h<=0 then return end
    local gap=row*0.35
    if data.kind=="balance" or data.unlimited then
        local metrics=data.metrics or {};local count=math.min(2,#metrics)
        stack=stack or w/row<6
        local metricWidth=stack and w or count>0 and (w-gap*(count-1))/count or w
        local valueWidth=stack and metricWidth*0.48-row*0.65 or metricWidth-row*1.3
        local valueSize=row*(stack and 0.72 or 0.83)
        -- Paired amounts share one fitted font size and one visible ink center.
        for i=1,count do
            local measured=draw.measureText(money(metrics[i].value,data.currency),valueSize,0,true)
            if measured.width>valueWidth*0.94 then valueSize=valueSize*valueWidth*0.94/measured.width end
        end
        for i=1,count do
            local cw=stack and w or (w-gap*(count-1))/count
            local ch=stack and (h-gap*(count-1))/count or h
            local cx=stack and x or x+(i-1)*(cw+gap)
            local cy=stack and y+(i-1)*(ch+gap) or y
            card(cx,cy,cw,ch,colors,row*0.34)
            if stack then
                local ty=cy+(ch-row*0.9)*0.5
                fitted(cx+row*0.65,ty,c[metrics[i].key] or c.used,row*0.40,colors.secondary,cw*0.49-row*0.65,false,row*0.9)
                text(cx+cw*0.52,ty,money(metrics[i].value,data.currency),valueSize,i==1 and colors.orange or colors.purple,valueWidth,true,row*0.9)
            else
                text(cx+row*0.65,cy+row*0.30,c[metrics[i].key] or c.used,row*0.43,colors.secondary,cw-row*1.3,false,row*0.85)
                text(cx+row*0.65,cy+row*1.03,money(metrics[i].value,data.currency),valueSize,i==1 and colors.orange or colors.purple,valueWidth,true,row*1.2)
            end
        end
    else
        local windows=data.windows or {};local count=#windows;if count==0 then return end
        local rh=math.min(row*1.75,(h-gap*(count-1))/count)
        for i,item in ipairs(windows) do
            local cy=y+(i-1)*(rh+gap);local label=c[item.key] or c.quota
            text(x,cy,label,row*0.44,colors.secondary,w*0.42,false,rh*0.52)
            local value=data.currency and money(item.remaining,data.currency).." / "..money(item.total,data.currency) or number(item.remaining).." / "..number(item.total)
            fitted(x+w*0.45,cy,value,row*0.46,colors.primary,w*0.55,true,rh*0.52)
            local py=cy+rh*0.64;local ph=row*0.16
            draw.rect(x,py,w,ph,colors.border,ph*0.5,0.3)
            if item.percent>0 then draw.rect(x,py,w*item.percent/100,ph,colors.blue,ph*0.5,0.94) end
        end
    end
end
local function render(_context,m)
    local c=copy();local colors=palette();local w,h=layout.contentWidth(),layout.contentHeight()
    local whaleMode=storage.get("mode")~="data";local short=math.min(w,h)
    local row=whaleMode and short*0.095 or ui.metrics().layoutRowHeight
    local pad=row*0.60;local cfg=config()
    -- Uncommitted provider/URL previews must not label another account's data.
    if not m.preview and cfg.identity~=m.client.identity then m={client={error="configure"},preview=false} end
    local name=providers.presets[cfg.provider].name
    if cfg.provider=="custom" then name=cfg.label and cfg.label~="" and tostring(cfg.label) or c.custom end
    local titleWidth=w-pad*2-row*2.10
    if cfg.provider=="deepseek" then fitted(pad,pad*0.55,name,row*0.56,colors.blue,titleWidth,true,row*0.85)
    else text(pad,pad*0.55,name,row*0.56,colors.blue,titleWidth,true,row*0.85) end
    region("refresh",w-pad-row*2.05,pad*0.55,row*0.85,row*0.85,c.refresh)
    draw.fa("",w-pad-row*1.88,pad*0.55+row*0.17,row*0.52,colors.primary)
    region("settings",w-pad-row*1.00,pad*0.55,row*0.85,row*0.85,c.settings)
    draw.fa("",w-pad-row*0.83,pad*0.55+row*0.17,row*0.52,colors.primary)
    local top=pad*0.55+row*1.42;local bottom=h-pad-row*(whaleMode and 1 or 1.9)
    if whaleMode then
        local wide=w/h>1.45
        local narrow=w/h<0.68
        local bx,by,bw,bh=pad,top,w-pad*2,h*0.34
        local ix,iy,iw,ih=w*0.34,top+bh+short*0.025,w*0.63,bottom-top-bh-short*0.025
        if wide then bw=w*0.50;bh=bottom-top;ix=w*0.56;iy=top;iw=w*0.40;ih=bottom-top end
        if narrow then ix=pad;iy=top+bh+row*0.50;iw=w-pad*2;ih=bottom-iy-row*1.25 end
        bubble(bx,by,bw,bh,colors,row*0.45,wide,row,narrow and 0.50 or 0.70);hero(bx,by,bw,bh,m,c,colors,row)
        local imageSide=math.min(iw,ih)
        if not wide then
            local buttonY=narrow and iy+imageSide+row*0.25 or iy+ih*0.58
            local buttonWidth=narrow and w-pad*2 or w*0.29
            local bw=math.min(buttonWidth,draw.measureText(c.show_details,row*0.42,0,false).width+row*0.55)
            draw.rect(pad,buttonY,bw,row,colors.blue,row*0.35,0.12)
            region("mode.data",pad,buttonY,bw,row,c.show_details)
            fitted(pad+row*0.25,buttonY,c.show_details,row*0.42,colors.blue,bw-row*0.50,false,row)
        end
        local imageX=ix+(iw-imageSide)*(narrow and 0.5 or 1)
        local imageY=wide and iy+(ih-imageSide)*0.5 or iy
        region("whale",imageX,imageY,imageSide,imageSide,c.refresh)
        local shrink=interaction.isPressed("whale") and 0.95 or 1
        draw.imageFit(whale,imageX+imageSide*(1-shrink)*0.5,imageY+imageSide*(1-shrink)*0.5,imageSide*shrink,imageSide*shrink,"contain","center",1)
    else
        local available=bottom-top;local wide=w/h>2.0
        local hh=math.min(row*4.1,available*0.58);local hx,hy,hw=pad,top,w-pad*2
        if wide then hw=w*0.46-pad;hh=available end
        card(hx,hy,hw,hh,colors,row*0.4);hero(hx,hy,hw,hh,m,c,colors,row)
        if wide then details(w*0.5,top,w*0.5-pad,available,m,c,colors,row,true)
        else details(pad,top+hh+row*0.4,w-pad*2,math.max(0,available-hh-row*0.4),m,c,colors,row) end
    end
    local footerY=h-pad-row*0.5
    local footerX=pad
    if not whaleMode then
        local bw=draw.measureText(c.back_to_whale,row*0.40,0,false).width+row*0.45
        bw=math.min(bw,w-pad*2-row*1.4)
        draw.rect(pad,footerY,bw,row*0.80,colors.blue,row*0.28,0.12)
        region("mode.whale",pad,footerY,bw,row*0.80,c.back_to_whale)
        fitted(pad+row*0.20,footerY,c.back_to_whale,row*0.40,colors.blue,bw-row*0.40,false,row*0.80)
        text(pad,footerY-row*1.15,status(m,c),row*0.40,m.client.error and colors.orange or colors.secondary,w-pad*2,false,row*0.80)
    elseif w/h>1.45 then
        local bw=draw.measureText(c.show_details,row*0.40,0,false).width+row*0.5
        draw.rect(pad,footerY,bw,row*0.80,colors.blue,row*0.28,0.12)
        region("mode.data",pad,footerY,bw,row*0.80,c.show_details)
        fitted(pad+row*0.25,footerY,c.show_details,row*0.40,colors.blue,bw-row*0.50,false,row*0.80)
        footerX=pad+bw+row*0.30
    end
    if whaleMode then text(footerX,footerY,status(m,c),row*0.40,m.client.error and colors.orange or colors.secondary,w-pad-row*1.4-footerX,false,row*0.80) end
    region("console",w-pad-row,footerY,row,row*0.8,c.console)
    draw.fa("",w-pad-row*0.78,footerY+row*0.13,row*0.42,colors.secondary)
end
local function setup(context)
    local m={preview=context.preview==true}
    m.client=client.new({config=config,permission=function()return widget.hasPermission("network.internet")end,
        now=time.now,monotonic=time.monotonic,decode=json.decode,parse=providers.parse,safeUrl=providers.safeUrl,
        start=function(args)return task.start("network.request",args)end,cancel=task.cancel})
    if m.preview then
        local cfg=config();local sample=storage.get("previewState") or "ready"
        local currency=providers.presets[cfg.provider].currency or cfg.currency
        if cfg.provider=="stepfun" then currency=nil end
        m.client.data={kind="balance",currency=currency,remaining=43.16,available=true,
            metrics={{key="topped",value=40},{key="granted",value=3.16}}}
        if providers.presets[cfg.provider].kind=="quota" or (cfg.provider=="custom" and cfg.kind=="quota") then
            m.client.data={kind="quota",currency=cfg.provider=="openrouter_key" and "USD" or nil,available=true,
                windows={{key=cfg.provider=="openrouter_key" and "key_limit" or "quota",remaining=73,total=100,percent=73}}}
        end
        m.client.updated=time.now()
        if sample=="loading" then m.client.data=nil;m.client.loading=true
        elseif sample=="empty" then m.client.data=nil;m.client.error="key_missing"
        elseif sample=="permission" then m.client.data=nil;m.client.error="permission"
        elseif sample=="error" then m.client.data=nil;m.client.error="auth"
        elseif sample=="stale" then m.client.error="network"
        elseif sample=="low" then m.client.data.remaining=2.35
        elseif sample=="depleted" then m.client.data.remaining=0;m.client.data.available=false end
    else m.client:refresh(false);schedule.every("balance.tick",15000,{whenHidden="pause"}) end
    return m
end
local function menu()
    local c=copy()
    return ui.menu({{id="refresh",label=c.refresh},{id="mode.whale",label=c.whale_mode},{id="mode.data",label=c.data_mode},
        {id="console",label=c.console},{id="settings",label=c.settings}})
end
local function event(_context,m,e)
    if e.kind=="task.complete" then
        if e.taskId==m.openTask then m.openTask=nil end
        if not m.preview then m.client:complete(e) end
    elseif e.kind=="settings.changed" and not e.preview and not m.preview then
        local cfg=config()
        for _,key in ipairs(e.keys or {}) do
            if key=="key_"..cfg.provider or key=="interval" then m.client:reset();break end -- Replacement preserves the opaque reference.
        end
        m.client:refresh(false)
    elseif (e.kind=="schedule" and e.id=="balance.tick") or (e.kind=="visibility" and e.visible) then
        if not m.preview then m.client:refresh(false) end
    elseif e.kind=="action" then
        local id=e.id
        if id=="keyboard" then if e.key~="Enter" and e.key~="Space" then return end;id=e.value end
        if (id=="refresh" or id=="whale") and not m.preview then m.client:refresh(true)
        elseif id=="settings" then widget.openSettings()
        elseif id=="mode.whale" then storage.set("mode","whale")
        elseif id=="mode.data" then storage.set("mode","data")
        elseif id=="console" and not m.preview then
            if widget.hasFeature("task.shell.openUri") and widget.hasPermission("shell.launch") and config().console then
                if m.openTask then task.cancel(m.openTask) end
                m.openTask=task.start("shell.openUri",{url=config().console})
            else widget.openSettings() end
        end
    end
    widget.invalidate()
end
local function settings()
    local c=copy();local labels={}
    for _,id in ipairs(providers.ids) do labels[#labels+1]=id=="custom" and c.custom or providers.presets[id].name end
    local fields={
        {key="provider",type="select",label=c.provider,default="deepseek",options=providers.ids,optionLabels=labels},
        {key="mode",type="select",label=c.mode,default="whale",options={"whale","data"},optionLabels={c.whale_mode,c.data_mode}},
    }
    for _,id in ipairs(providers.ids) do fields[#fields+1]={key="key_"..id,type="password",label=c.key,description=id=="openrouter" and c.management_hint or c.key_hint,
        showWhen={key="provider",operator="equals",value=id}} end
    local function add(field,provider)
        if provider then field.showWhen={key="provider",operator="equals",value=provider} end
        fields[#fields+1]=field
    end
    add({key="interval",type="int",label=c.interval,default=300,min=60,max=86400})
    add({key="lowBalance",type="float",label=c.threshold,default=10,min=0,max=1000000})
    add({key="lowQuota",type="int",label=c.quota_threshold,default=10,min=0,max=100})
    add({key="currency",type="select",label=c.currency,default="CNY",options={"CNY","USD"}})
    add({key="label",type="text",label=c.label,default=""},"custom")
    add({key="endpoint",type="url",label=c.endpoint,description=c.endpoint_hint,default=""},"custom")
    add({key="customKind",type="select",label=c.kind,default="balance",options={"balance","quota"},optionLabels={c.balance_kind,c.quota_kind}},"custom")
    add({key="remainingPath",type="text",label=c.path,description=c.path_hint,default="data.balance"},"custom")
    add({key="totalPath",type="text",label=c.total_path,default="data.total"},"custom")
    add({key="scale",type="float",label=c.scale,default=1,min=0.00000001,max=1000000},"custom")
    add({key="authHeader",type="select",label=c.auth,default="Authorization",options={"Authorization","x-api-key","api-key"}},"custom")
    add({key="authStyle",type="select",label=c.auth,default="bearer",options={"bearer","raw"},optionLabels={"Bearer","API Key"}},"custom")
    return {fields=fields}
end
return widget.define({name=copy().name,useCustomStyle=true,followPersonalizationDefault=true,showTitle=false,
    bg=0x172439,border=0xFFFFFF,alpha=0.40,borderAlpha=0.18,gradientEndA=0.30,
    setup=setup,render=render,event=event,menu=menu,settings=settings(),
    dispose=function(_context,m)m.client:dispose();schedule.cancel("balance.tick");if m.openTask then task.cancel(m.openTask) end end})
