local clock=module.require("modules/media_clock.lua")
local plan=module.require("modules/layout.lua")
local Text=module.require("modules/text_layout.lua")
local strings=module.require("modules/strings.lua")
local M={}
local function number(key,default)
    local value=tonumber(storage.get(key));return value or default
end
function M.enabled(key,default)
    local v=storage.get(key);if v==nil then return default end
    return v==true or v==1 or v=="true" or v=="1"
end
local function rowText(x,y,row,font,color,alpha)
    draw.text(x,y,row.text,font,color,row.boxWidth,true,true,row.height,alpha)
end
local function geometry(model,index,font,width,translate,roman,estimate)
    local cache=model.textCache
    if not cache or cache.width~=width or cache.font~=font or cache.lines~=model.client.lines or
        cache.translate~=translate or cache.roman~=roman or cache.estimate~=estimate then
        cache={width=width,font=font,lines=model.client.lines,translate=translate,roman=roman,estimate=estimate,items={},order={}}
        model.textCache=cache
    end
    if cache.items[index] then return cache.items[index] end
    local line=model.client.lines[index]
    local words=estimate and Text.estimateWords(line) or (line.words or {})
    local result={main=Text.wrap(line.text,font,width,draw.measureText),words=words}
    Text.words(result.main,{text=line.text,words=words},draw.measureText);result.height=result.main.height
    local subtitle=translate and line.translation or ""
    subtitle=subtitle or ""
    if roman and line.romanization and line.romanization~="" then
        subtitle=subtitle~="" and (subtitle.." · "..line.romanization) or line.romanization
    end
    if subtitle~="" then
        result.subtitle=Text.wrap(subtitle,font*0.43,width,draw.measureText)
        result.subtitleY=result.height+font*0.15
        result.height=result.subtitleY+result.subtitle.height
    end
    cache.items[index]=result;cache.order[#cache.order+1]=index
    if #cache.order>128 then cache.items[table.remove(cache.order,1)]=nil end
    return result
end
function M.render(_context,m)
    local c=strings();local w,h=layout.contentWidth(),layout.contentHeight();local row=ui.metrics().layoutRowHeight
    local dark=widget.theme().contentTheme==1
    local primary=dark and 0x142235 or 0xFFFFFF;local accent=dark and 0x0759A6 or 0x9EDEFF
    interaction.region({key="lyrics",shape={type="rect",x=0,y=0,width=w,height=h},cursor="default",
        events={contextMenu={id="lyrics.menu",scope="component"}},accessibility={role="text",label=c.name}})
    local lines=m.client.lines
    if m.status~="ready" or not m.clock or #lines==0 then
        if M.enabled("hideIdlePlaceholder",false) and not m.playbackActive then return end
        local message=c[m.status] or c.waiting
        local block=Text.wrap(message,row*0.44,math.max(1,w-row),draw.measureText)
        draw.pushClip(0,0,w,h)
        for i,part in ipairs(block.rows) do
            rowText(row*0.5,(h-block.height)/2+(i-1)*block.lineHeight,part,block.font,primary,0.70)
        end
        draw.popClip();return
    end
    local translate,roman=M.enabled("translation",true),M.enabled("romanization",false)
    local estimate=M.enabled("estimateWordTiming",true)
    local secondary=(translate and m.client.hasTranslation) or (roman and m.client.hasRomanization)
    local p=plan.metrics(w,h,row,number("textScale",100),number("lineSpacing",175),secondary)
    local function get(index)return geometry(m,index,p.font,p.width,translate,roman,estimate)end
    local now=time.monotonic();local media=clock.position(m.clock,now)-number("offset",0)-(m.client.offset or 0)
    local index=clock.index(lines,media);local target=math.max(1,index)
    local elapsed=m.lastDraw and now-m.lastDraw or 0
    local reset=m.snap or m.reducedMotion or not m.scrollIndex or m.scrollIndex>#lines
    local shift=m.shift or 0
    if not reset and target~=m.scrollIndex then
        local delta=target-m.scrollIndex
        if math.abs(delta)>32 then reset=true
        else
            local direction=delta>0 and 1 or -1
            for i=m.scrollIndex,target-direction,direction do
                shift=shift+direction*((get(i).height+get(i+direction).height)/2+p.gap)
            end
        end
    end
    shift=plan.focus(shift,0,elapsed,reset)
    if math.abs(shift)<0.01 then shift=0 end
    m.scrollIndex=target;m.shift=shift;m.snap=nil;m.lastDraw=now
    local function paint(i,center)
        local line,g=lines[i],get(i);local top=center-g.height/2
        if top>=h or top+g.height<=0 then return end
        local fade=plan.opacity((center-h/2)/p.step,math.max(1,h/p.step))
        if fade<=0.003 then return end
        local active=i==index
        for n,part in ipairs(g.main.rows) do
            local y=top+(n-1)*g.main.lineHeight
            rowText(p.padding,y,part,g.main.font,primary,(active and 0.55 or 0.72)*fade)
            if active then
                if #g.words==0 then rowText(p.padding,y,part,g.main.font,accent,fade)
                else for _,segment in ipairs(part.segments or {}) do
                    local word=segment.word
                    local progress=media>=word.finish and 1 or math.max(0,(media-word.start)/math.max(1,word.finish-word.start))
                    local amount=math.max(0,math.min(1,(progress-segment.from)/math.max(0.0001,segment.to-segment.from)))
                    if amount>0 then
                        draw.pushClip(p.padding+segment.x1,y,(segment.x2-segment.x1)*amount,part.height)
                        rowText(p.padding,y,part,g.main.font,accent,fade);draw.popClip()
                    end
                end end
            end
        end
        if g.subtitle then for n,part in ipairs(g.subtitle.rows) do
            rowText(p.padding,top+g.subtitleY+(n-1)*g.subtitle.lineHeight,part,g.subtitle.font,primary,0.65*fade)
        end end
    end
    draw.pushClip(0,0,w,h)
    local center=h/2+shift;local current=get(target)
    paint(target,center)
    local edge=center-current.height/2;local i=target-1;local count=0
    while i>=1 and edge>0 and count<64 do
        local previous=get(i);center=edge-p.gap-previous.height/2;paint(i,center)
        edge=center-previous.height/2;i=i-1;count=count+1
    end
    edge=h/2+shift+current.height/2;i=target+1;count=0
    while i<=#lines and edge<h and count<64 do
        local following=get(i);center=edge+p.gap+following.height/2;paint(i,center)
        edge=center+following.height/2;i=i+1;count=count+1
    end
    draw.popClip()
end
return M
