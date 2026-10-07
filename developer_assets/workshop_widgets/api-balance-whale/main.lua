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
    return {primary=darkText and 0x202E45 or 0xEDF4FF,secondary=darkText and 0x5B6C83 or 0xB1C1D8,
        card=darkText and 0xF6F9FF or 0x142338,border=darkText and 0x91A8C6 or 0x93AFD5,
        blue=darkText and 0x2865C9 or 0x92BEFF,green=darkText and 0x197352 or 0x8DDBB5,
        orange=darkText and 0xA55319 or 0xFFC18B}
end
local function text(x,y,value,size,color,width,bold,height)
    local measured=draw.measureText(tostring(value),size,0,bold or false)
    local ink=measured.ink
    local boxHeight=height or size*1.6
    local originY=y+(boxHeight-ink.height)*0.5-ink.top
    -- Keep the visible ink center and leave room for fractional line metrics.
    draw.text(x,originY,tostring(value),size,color,width,bold or false,true,measured.height+size*0.25)
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
local function region(id,x,y,w,h,label,quiet)
    interaction.region({key=id,shape={type="roundedRect",x=x,y=y,width=w,height=h,radius=h*0.18},
        cursor="hand",focusable=true,tooltip=label,accessibility={role="button",label=label},
        events={click={id=id},keyDown={id="keyboard",value=id},contextMenu={id="menu",scope="component"}}})
    if not quiet and (interaction.isHovered(id) or interaction.isFocused(id)) then draw.rect(x,y,w,h,palette().primary,h*0.18,0.08) end
    if interaction.isFocused(id) then draw.strokeRect(x,y,w,h,palette().blue,h*0.18,h*0.035,0.9) end
end
local function card(x,y,w,h,colors,radius)
    draw.rect(x,y,w,h,colors.card,radius,0.48)
    draw.strokeRect(x,y,w,h,colors.border,radius,radius*0.05,0.14)
end
local function bubble(x,y,w,h,colors,r,row,pointRight)
    local right,bottom=x+w,y+h;local k=r*0.55228475
    local path={{op="move",x=x+r,y=y},{op="line",x=right-r,y=y},
        {op="cubic",x1=right-r+k,y1=y,x2=right,y2=y+r-k,x=right,y=y+r}}
    local function add(command) path[#path+1]=command end
    if pointRight then
        local cy=y+h*0.46
        add({op="line",x=right,y=cy-row*0.24})
        add({op="cubic",x1=right,y1=cy-row*0.10,x2=right+row*0.16,y2=cy-row*0.04,x=right+row*0.36,y=cy+row*0.03})
        add({op="quadratic",x1=right+row*0.42,y1=cy+row*0.07,x=right+row*0.34,y=cy+row*0.11})
        add({op="cubic",x1=right+row*0.14,y1=cy+row*0.17,x2=right,y2=cy+row*0.13,x=right,y=cy+row*0.27})
    end
    add({op="line",x=right,y=bottom-r})
    add({op="cubic",x1=right,y1=bottom-r+k,x2=right-r+k,y2=bottom,x=right-r,y=bottom})
    if not pointRight then
        local cx=x+w*0.52
        add({op="line",x=cx+row*0.27,y=bottom})
        add({op="cubic",x1=cx+row*0.17,y1=bottom,x2=cx+row*0.12,y2=bottom+row*0.18,x=cx+row*0.08,y=bottom+row*0.32})
        add({op="quadratic",x1=cx+row*0.05,y1=bottom+row*0.38,x=cx+row*0.01,y=bottom+row*0.30})
        add({op="cubic",x1=cx-row*0.09,y1=bottom+row*0.10,x2=cx-row*0.16,y2=bottom,x=cx-row*0.27,y=bottom})
    end
    add({op="line",x=x+r,y=bottom})
    add({op="cubic",x1=x+r-k,y1=bottom,x2=x,y2=bottom-r+k,x=x,y=bottom-r})
    add({op="line",x=x,y=y+r})
    add({op="cubic",x1=x,y1=y+r-k,x2=x+r-k,y2=y,x=x+r,y=y})
    add({op="close"})
    draw.path(path,{fillColor=colors.card,alpha=0.48})
    draw.path(path,{strokeColor=colors.border,thickness=r*0.05,alpha=0.14})
end
local function hero(x,y,w,h,m,c,colors,row)
    local data=m.client.data;local pad=row*0.70
    local inner=w-pad*2
    local rhythm=math.min(row,h/3.65)
    local groupTop=y+(h-rhythm*3.65)*0.5
    local unit=data and not data.unlimited and data.currency or nil
    local unitSize=row*0.36
    local unitWidth=unit and draw.measureText(unit,unitSize,0,false).width or 0
    local headingWidth=inner-(unit and unitWidth+row*0.45 or 0)
    fitted(x+pad,groupTop+rhythm*0.40,data and data.kind=="quota" and c.remaining or c.total,row*0.49,colors.secondary,headingWidth,false,rhythm*0.58)
    if unit then text(x+w-pad-unitWidth,groupTop+rhythm*0.40,unit,unitSize,colors.secondary,unitWidth,false,rhythm*0.58) end
    local label=badge(data,c)
    local amount=c.unknown
    if data then
        if data.unlimited then amount=c.unlimited
        elseif data.kind=="balance" then amount=money(data.remaining,data.currency)
        elseif data.windows[1] then amount=data.currency and money(data.windows[1].remaining,data.currency) or number(data.windows[1].percent).."%" end
    end
    fitted(x+pad,groupTop+rhythm*1.12,amount,row*(data and 1.44 or 0.94),data and colors.blue or colors.secondary,inner,true,rhythm*1.45)
    if data then
        local statusColor=label==c.available and colors.green or colors.orange
        local statusY=groupTop+rhythm*2.91
        draw.circle(x+pad+row*0.06,statusY+rhythm*0.27,row*0.06,statusColor,0.95)
        fitted(x+pad+row*0.28,statusY,label,row*0.42,statusColor,inner-row*0.28,false,rhythm*0.54)
    end
end
local function details(x,y,w,h,m,c,colors,row,stack,embedded)
    local data=m.client.data;if not data or h<=0 then return end
    local gap=row*0.35
    if data.kind=="balance" or data.unlimited then
        local metrics=data.metrics or {};local count=math.min(2,#metrics)
        stack=stack or w/row<4.5
        local inset=embedded and 0 or row*0.70
        local metricWidth=stack and w or count>0 and (w-gap*(count-1))/count or w
        local valueWidth=metricWidth-inset*2
        local metricHeight=stack and (h-gap*(count-1))/math.max(1,count) or h
        local valueSize=math.min(row*0.80,metricHeight*0.42)
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
            if not embedded then card(cx,cy,cw,ch,colors,row*0.34) end
            local rhythm=math.min(row,ch/2.30)
            local groupTop=cy+(ch-rhythm*2.30)*0.5
            fitted(cx+inset,groupTop+rhythm*0.25,c[metrics[i].key] or c.used,row*0.38,colors.secondary,valueWidth,false,rhythm*0.56)
            text(cx+inset,groupTop+rhythm*0.99,money(metrics[i].value,data.currency),valueSize,colors.primary,valueWidth,true,rhythm*0.94)
            if embedded and not stack and i<count then
                local dividerX=cx+cw+gap*0.5
                draw.line(dividerX,cy+row*0.30,dividerX,cy+ch-row*0.30,row*0.025,colors.border,0.16)
            end
        end
    else
        local windows=data.windows or {};local count=#windows;if count==0 then return end
        local rh=math.min(row*2.25,(h-gap*(count-1))/count)
        for i,item in ipairs(windows) do
            local cy=y+(i-1)*(rh+gap);local label=c[item.key] or c.quota
            fitted(x,cy+rh*0.04,label,row*0.40,colors.secondary,w,false,rh*0.28)
            local value=data.currency and money(item.remaining,data.currency).." / "..money(item.total,data.currency) or number(item.remaining).." / "..number(item.total)
            fitted(x,cy+rh*0.33,value,row*0.65,colors.primary,w,true,rh*0.42)
            local py=cy+rh*0.86;local ph=row*0.12
            draw.rect(x,py,w,ph,colors.border,ph*0.5,0.3)
            if item.percent>0 then draw.rect(x,py,w*item.percent/100,ph,colors.blue,ph*0.5,0.94) end
        end
    end
end
local function render(_context,m)
    local c=copy();local colors=palette();local outerWidth,outerHeight=layout.contentWidth(),layout.contentHeight()
    local whaleMode=storage.get("mode")~="data"
    local w,h=outerWidth,outerHeight
    local row=ui.metrics().layoutRowHeight
    -- The illustration scales as one composition. Data mode keeps the host's
    -- semantic row unit and centers only the information actually present.
    w=math.min(w,h*1.70);h=math.min(h,w/0.45)
    if whaleMode then row=math.min(w*0.112,h*0.068) end
    local ox,oy=(outerWidth-w)*0.5,(outerHeight-h)*0.5
    local pad=row*0.68;local cfg=config()
    -- Uncommitted provider/URL previews must not label another account's data.
    if not m.preview and cfg.identity~=m.client.identity then m={client={error="configure"},preview=false} end
    local name=providers.presets[cfg.provider].name
    if cfg.provider=="openrouter_key" then name="OpenRouter" end -- The body already identifies the key quota.
    if cfg.provider=="custom" then name=cfg.label and cfg.label~="" and tostring(cfg.label) or c.custom end
    local headerY=oy+pad*0.58
    local titleWidth=w-pad*2-row*2.30
    fitted(ox+pad,headerY,name,row*0.63,colors.primary,titleWidth,true,row)
    local function headerAction(id,x,glyph,label)
        region(id,x,headerY,row,row,label)
        draw.fa(glyph,x+row*0.24,headerY+row*0.24,row*0.52,colors.secondary)
    end
    headerAction("refresh",ox+w-pad-row*2.12,"",c.refresh)
    headerAction("settings",ox+w-pad-row,"",c.settings)
    local top=headerY+row*1.48
    local footerY=oy+h-pad-row*0.94
    local hasNotice=m.client.error~=nil or (m.client.loading and m.client.data==nil)
    local noticeHeight=hasNotice and row*1.02 or 0
    local bottom=footerY-row*0.35-noticeHeight
    if whaleMode then
        local wide=w/h>1.32
        local bx,by,bw,bh=ox+pad,top,w-pad*2,row*3.65
        if wide then
            bw=(w-pad*2-row*0.80)*0.49
            bh=math.min(bottom-top,row*4.45)
            by=top+(bottom-top-bh)*0.43
        end
        bubble(bx,by,bw,bh,colors,row*0.48,row,wide);hero(bx,by,bw,bh,m,c,colors,row)
        local imageTop=wide and top or by+bh+row*0.18
        local imageLeft=wide and bx+bw+row*0.75 or ox+pad*0.45
        local imageWidth=wide and ox+w-pad*0.45-imageLeft or w-pad*0.90
        local imageHeight=bottom-imageTop
        local imageSide=math.max(0,math.min(imageWidth,imageHeight))
        local imageX=imageLeft+(imageWidth-imageSide)*0.5
        local imageY=imageTop+(imageHeight-imageSide)*0.55
        region("whale",imageX,imageY,imageSide,imageSide,c.refresh,true)
        local shrink=interaction.isPressed("whale") and 0.95 or 1
        draw.imageFit(whale,imageX+imageSide*(1-shrink)*0.5,imageY+imageSide*(1-shrink)*0.5,imageSide*shrink,imageSide*shrink,"contain","center",1)
    else
        local available=bottom-top
        local bodyWidth=w-pad*2;local innerPad=row*0.70
        local data=m.client.data
        local hasDetails=data and (#(data.metrics or {})>0 or #(data.windows or {})>0)
        if w/h>1.32 and hasDetails then
            local groupHeight=math.min(available,row*5.40)
            local groupY=top+(available-groupHeight)*0.5
            card(ox+pad,groupY,bodyWidth,groupHeight,colors,row*0.48)
            local split=ox+pad+bodyWidth*0.50
            hero(ox+pad,groupY,bodyWidth*0.50,groupHeight,m,c,colors,row)
            draw.line(split,groupY+innerPad,split,groupY+groupHeight-innerPad,row*0.025,colors.border,0.16)
            details(split+innerPad,groupY+row*0.25,bodyWidth*0.50-innerPad*2,groupHeight-row*0.50,m,c,colors,row,true,true)
        else
            local stacked=data and data.kind=="balance" and (bodyWidth-innerPad*2)/row<4.5
            local detailRows=stacked and 4.60 or 2.75
            local groupHeight=math.min(available,row*(hasDetails and 3.65+detailRows or 3.65))
            local groupY=top+(available-groupHeight)*0.5
            local hh=hasDetails and groupHeight*3.65/(3.65+detailRows) or groupHeight
            card(ox+pad,groupY,bodyWidth,groupHeight,colors,row*0.48)
            hero(ox+pad,groupY,bodyWidth,hh,m,c,colors,row)
            if hasDetails then
                local split=groupY+hh
                draw.line(ox+pad+innerPad,split,ox+w-pad-innerPad,split,row*0.025,colors.border,0.16)
                details(ox+pad+innerPad,split+row*0.22,bodyWidth-innerPad*2,groupHeight-hh-row*0.44,m,c,colors,row,stacked,true)
            end
        end
    end
    local action=whaleMode and c.show_details or c.back_to_whale
    local actionId=whaleMode and "mode.data" or "mode.whale"
    local buttonWidth=math.min(draw.measureText(action,row*0.41,0,false).width+row*1.05,w-pad*2-row*1.4)
    draw.rect(ox+pad,footerY,buttonWidth,row*0.94,colors.blue,row*0.28,0.09)
    region(actionId,ox+pad,footerY,buttonWidth,row*0.94,action)
    fitted(ox+pad+row*0.32,footerY,action,row*0.41,colors.blue,buttonWidth-row*1.05,false,row*0.94)
    draw.fa(whaleMode and "" or "",ox+pad+buttonWidth-row*0.49,footerY+row*0.32,row*0.28,colors.blue)
    local consoleX=ox+w-pad-row
    region("console",consoleX,footerY,row,row*0.94,c.console)
    draw.fa("",consoleX+row*0.29,footerY+row*0.28,row*0.38,colors.secondary)
    if hasNotice then
        local noticeY=footerY-noticeHeight-row*0.10
        local notice=status(m,c)
        text(ox+pad,noticeY,notice,row*0.40,m.client.error and colors.orange or colors.secondary,w-pad*2,false,row*0.82)
        interaction.region({key="notice",shape={type="rect",x=ox+pad,y=noticeY,width=w-pad*2,height=row*0.82},
            tooltip=notice,accessibility={role="text",label=notice}})
    elseif m.client.updated or m.client.loading then
        local update=m.client.loading and c.loading or time.format(m.client.updated,{dateStyle="none",timeStyle="short"})
        local updateWidth=consoleX-row*0.30-(ox+pad+buttonWidth+row*0.45)
        if updateWidth>row*0.9 then
            local measured=draw.measureText(update,row*0.37,0,false)
            local visibleWidth=math.min(measured.width,updateWidth)
            local updateX=consoleX-row*0.30-visibleWidth
            fitted(updateX,footerY,update,row*0.37,colors.secondary,visibleWidth,false,row*0.94)
            interaction.region({key="updated",shape={type="rect",x=updateX,y=footerY,width=visibleWidth,height=row*0.94},
                tooltip=status(m,c),accessibility={role="text",label=status(m,c)}})
        end
    end
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
        m.client:settingsChanged(e.keys)
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
