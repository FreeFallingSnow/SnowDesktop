local Text=module.require("modules/text_layout.lua")
local M={}
local function clamp(value)return math.max(0,math.min(1,value))end
local function ease(value)value=clamp(value);return value*value*(3-2*value)end
local function spring(seconds)
    seconds=math.max(0,seconds)
    return 1-math.exp(-10*seconds)*(math.cos(10*seconds)+math.sin(10*seconds))
end
local function random(seed,offset)
    local value=math.sin(seed+offset)*10000;return value-math.floor(value)
end
local function drift(age,phase,rate)
    return 0.65*math.sin(age*rate+phase)+0.35*math.sin(age*rate*1.618+phase*0.73)
end
local function color(a,b,t)
    t=clamp(t);local value=0
    for _,unit in ipairs({65536,256,1}) do
        local x,y=math.floor(a/unit)%256,math.floor(b/unit)%256
        value=value+math.floor(x+(y-x)*t+0.5)*unit
    end
    return value
end
local function extent(w,h,angle)
    angle=math.min(math.rad(angle),math.atan(h/w))
    return w*math.cos(angle)+h*math.sin(angle)
end
local function prepare(line,g,p,width,height)
    if g.replica and g.replica.surfaceHeight==height then return g.replica end
    local available=math.max(1,p.width-p.font*0.2)
    local block=Text.wrap(line.text,p.font,available/1.95,draw.measureText)
    Text.words(block,{text=line.text,words=g.words},draw.measureText)
    local result={pieces={},pages={},surfaceHeight=height,left=p.padding+p.font*0.1};local measurements={}
    if g.subtitle and #g.subtitle.rows==0 then g.subtitle=nil end
    local room=math.max(1,height-p.padding*2-p.font*0.44)
    result.subtitleRows=g.subtitle and math.min(#g.subtitle.rows,math.max(1,math.floor(room*0.22/g.subtitle.lineHeight))) or 0
    local subtitleHeight=g.subtitle and result.subtitleRows*g.subtitle.lineHeight+p.font*0.35 or 0
    local pageRoom=math.max(1,room-subtitleHeight)
    local bend,turn=0,0
    local function piece(first,last,start,finish,split)
        local value=line.text:sub(first,last)
        if value:match("^%s*$") then return end
        local chars=not split and utf8.len(value)>1 and Text.clusters(value) or nil;local isCjk=false
        if chars then for _,char in ipairs(chars) do
            if (char.code>=0x2e80 and char.code<=0xa4cf) or char.code>=0x20000 then isCjk=true;break end
        end end
        if not split and isCjk and #chars>1 then
            local span=(finish or line.finish)-(start or line.start)
            for i,char in ipairs(chars) do piece(first+char.first-1,first+char.last-1,
                start and start+span*(i-1)/#chars or nil,start and start+span*i/#chars or nil,true) end
            return
        end
        local index=#result.pieces+1;local measured=measurements[value]
        if not measured then
            local native=draw.measureText(value,p.font,0,true)
            local w,h=native.width+p.font*0.06,math.max(p.font*1.1,native.height)
            measured={width=w,height=h,spanX=extent(w,h,8),spanY=extent(h,w,8)};measurements[value]=measured
        end
        local item={key="lyrics.word."..tostring(line.start).."."..index,text=value,
            width=measured.width,height=measured.height,
            start=start or line.start,finish=finish or line.finish,scale=start and 1.2 or 1,first=first,last=last}
        item.focusStart=start or line.start+(line.finish-line.start)*(first-1)/math.max(1,#line.text)
        local peak=item.scale*1.25*1.044
        item.slotWidth=peak*measured.spanX
        item.slotHeight=peak*measured.spanY+p.font*0.24
        if chars and #chars>1 and (item.slotWidth>available or item.slotHeight>pageRoom) then
            local span=(finish or line.finish)-(start or line.start)
            for i,char in ipairs(chars) do piece(first+char.first-1,first+char.last-1,
                start and start+span*(i-1)/#chars or nil,start and start+span*i/#chars or nil,true) end
            return
        end
        item.fit=1
        if item.slotWidth>available or item.slotHeight>pageRoom then item.fit=math.min(available/item.slotWidth,pageRoom/item.slotHeight) end
        item.slotWidth=item.slotWidth*item.fit;item.slotHeight=item.slotHeight*item.fit
        local seed=line.start/1000+index*1.618
        bend=bend*0.35+(random(seed,2)*2-1)*0.65
        turn=turn*0.55+(random(seed,3)*2-1)*0.45
        item.anchorY=bend*p.font*0.18;item.tilt=turn*2.6
        item.motionSeed=seed
        result.pieces[#result.pieces+1]=item
    end
    for _,row in ipairs(block.rows) do
        local cursor=row.first
        for _,segment in ipairs(row.segments) do
            if segment.first>cursor then piece(cursor,segment.first-1) end
            local span=segment.word.finish-segment.word.start
            piece(segment.first,segment.last,segment.word.start+span*segment.from,segment.word.start+span*segment.to)
            cursor=segment.last+1
        end
        if cursor<=row.last then piece(cursor,row.last) end
    end
    local gap=p.font*0.03;local rows={};local row={pieces={},width=0,height=0}
    local bendRoomSquared=(p.font*0.36)^2
    local function advance(a,b)
        local clear=(a.slotWidth+b.slotWidth)/2+b.gapBefore
        local dy=b.anchorY-a.anchorY
        -- Preserve near-equal travel along an irregular path and horizontal
        -- clearance for both maximum transformed bounds.
        return math.sqrt(clear*clear+bendRoomSquared-dy*dy)
    end
    local function rowSize(r)
        r.width=0;r.height=0
        for i,item in ipairs(r.pieces) do
            if i==1 then r.width=item.slotWidth
            else local previous=r.pieces[i-1];r.width=r.width-previous.slotWidth/2+item.step+item.slotWidth/2 end
            r.height=math.max(r.height,item.slotHeight)
        end
    end
    local previous
    for _,item in ipairs(result.pieces) do
        item.gapBefore=previous and line.text:sub(previous.last+1,item.first-1):find("%s") and p.font*0.28 or gap
        item.step=previous and advance(previous,item) or 0
        local nextWidth=#row.pieces>0 and row.width-row.pieces[#row.pieces].slotWidth/2+
            item.step+item.slotWidth/2 or item.slotWidth
        if #row.pieces>0 and (nextWidth>available or #row.pieces>=128) then
            rows[#rows+1]=row;row={pieces={},width=0,height=0};nextWidth=item.slotWidth
        end
        row.pieces[#row.pieces+1]=item;row.width=nextWidth
        row.height=math.max(row.height,item.slotHeight);previous=item
    end
    if #row.pieces>0 then rows[#rows+1]=row end
    -- Balance a short final fragment without changing the cached row breaks during playback.
    if #rows>1 then
        local a,b=rows[#rows-1],rows[#rows]
        while #a.pieces>1 and b.width<a.width*0.65 do
            local item=a.pieces[#a.pieces]
            local added=item.slotWidth/2+b.pieces[1].step-b.pieces[1].slotWidth/2
            if b.width+added>available then break end
            table.remove(a.pieces);table.insert(b.pieces,1,item);rowSize(a);rowSize(b)
        end
    end
    local page={rows={},width=0,height=0,start=line.start,pieceCount=0}
    for i,r in ipairs(rows) do
        r.angle=(random(line.start/1000,i)-0.5)*3.6
        if #page.rows>0 and (page.height+p.font*0.10+r.height>pageRoom or page.pieceCount+#r.pieces>180) then
            result.pages[#result.pages+1]=page;page={rows={},width=0,height=0,start=r.pieces[1].focusStart,pieceCount=0}
        end
        local indent=#page.rows>0 and math.min(math.max(0,available-r.width)*(0.35+random(line.start/1000,i+30)*0.25),p.font*2.3) or 0
        page.width=math.max(page.width,indent+r.width)
        local x=result.left+indent+r.pieces[1].slotWidth/2
        for n,item in ipairs(r.pieces) do
            if n>1 then x=x+item.step end
            item.x=x-item.width/2
        end
        r.y=page.height+(#page.rows>0 and p.font*0.10 or 0)
        page.rows[#page.rows+1]=r;page.height=r.y+r.height;page.pieceCount=page.pieceCount+#r.pieces
    end
    if #page.rows>0 then result.pages[#result.pages+1]=page end
    for i,v in ipairs(result.pages) do
        v.finish=result.pages[i+1] and result.pages[i+1].start or line.finish
        -- Center the complete reserved fragment once, including hidden words.
        -- Singing and transforms never change these anchors.
        local shift=(width-v.width)/2-result.left
        for _,r in ipairs(v.rows) do for _,item in ipairs(r.pieces) do item.x=item.x+shift end end
    end
    result.subtitleHeight=subtitleHeight;g.replica=result;return result
end
local function glyphs(piece,font)
    if piece.glyphs then return piece.glyphs end
    local result={};local chars=Text.clusters(piece.text)
    for i,char in ipairs(chars) do
        local prefix=piece.text:sub(1,char.first-1)
        local x=prefix=="" and 0 or draw.measureText(prefix,font,0,true).width
        local value=piece.text:sub(char.first,char.last)
        result[#result+1]={text=value,x=x,width=draw.measureText(value,font,0,true).width+font*0.15,
            start=piece.start+(piece.finish-piece.start)*(i-1)/#chars,
            finish=piece.start+(piece.finish-piece.start)*i/#chars}
    end
    piece.glyphs=result;return result
end
local function label(key,value,x,y,width,height,font,foreground,alpha)
    return {type="text",key=key,text=value,width=math.max(1,width),height=height,offset={x=x,y=y},
        fontSize=font,fontWeight=700,textWrap="noWrap",maxLines=1,overflowText="clip",verticalAlign="center",
        style={foreground=foreground,opacity=alpha}}
end
local directions={{1,0},{-1,0},{0,1},{0,-1},{0.707,0.707},{-0.707,0.707},{0.707,-0.707},{-0.707,-0.707}}
local function pose(piece,media,pageStart,reduced,angle,previousAngle)
    local elapsed=(media-math.max(piece.start-150,pageStart))/1000
    local enter=spring(elapsed);local after=ease((media-piece.finish)/500)
    local target=piece.scale*1.25
    local scale=(reduced and 1 or 0.5+(target-0.5)*enter+(piece.scale-target)*after)*piece.fit
    local age=math.max(0,(media-piece.finish)/1000);local floating,spinning=0,0
    if not reduced and age>0 then
        if not piece.phase then
            piece.phase=random(piece.motionSeed,8)*math.pi*2;piece.rate=0.9+random(piece.motionSeed,9)*0.8
            piece.zeroY=drift(0,piece.phase,piece.rate);piece.zeroAngle=drift(0,piece.phase+1.7,piece.rate*0.67)
        end
        local settled=ease(age/0.35)*0.5
        floating=(drift(age,piece.phase,piece.rate)-piece.zeroY)*settled
        spinning=(drift(age,piece.phase+1.7,piece.rate*0.67)-piece.zeroAngle)*settled
    end
    local rotate=reduced and 0 or math.max(-8,math.min(8,angle+piece.tilt+1.8*(1-enter)+spinning*2.8))
    if previousAngle then rotate=math.max(previousAngle-8,math.min(previousAngle+8,rotate)) end
    local radians=math.rad(rotate)
    local ey=scale*(math.abs(math.cos(radians))*piece.height+math.abs(math.sin(radians))*piece.width)/2
    return {elapsed=elapsed,after=after,scale=scale,rotate=rotate,ey=ey,
        y=reduced and 0 or (1-enter)*0.06+floating*0.12,
        alpha=reduced and 0.9 or ease(elapsed/0.1)*(1-after*0.18)}
end
function M.interlude(m,start,finish,media,width,height,font,primary,accent,description)
    local children={};local reduced=m.reducedMotion==true
    local unit=math.min(font,width/9,height/3)
    local diameter=unit*0.25;local pitch=unit*1.15
    local span=math.max(1,(finish-start)/6)
    for i=1,6 do
        local at=start+(i-1)*span
        local progress=clamp((media-at)/span)
        local entered=media>=at
        local after=ease((media-at-span)/500)
        local scale=reduced and 1 or (entered and 0.5+spring((media-at)/1000)*(1-after*0.25) or 0.5)
        local glow=not reduced and entered and math.sin(progress*math.pi) or 0
        local x=width/2+(i-3.5)*pitch-diameter/2
        local y=height/2-diameter/2+(reduced and 0 or unit*0.08*math.sin((media-start)/1400+i*0.7))
        local dots={}
        for layer=1,3 do
            local size=diameter*(1+layer*0.65)
            dots[#dots+1]={type="stack",key="lyrics.interlude.glow."..i.."."..layer,width=size,height=size,
                offset={x=(diameter-size)/2,y=(diameter-size)/2},
                style={background=accent,cornerRadius=size/2,opacity=glow*0.10/layer}}
        end
        dots[#dots+1]={type="stack",key="lyrics.interlude.dot."..i,width=diameter,height=diameter,
            style={background=color(primary,accent,glow),cornerRadius=diameter/2}}
        children[#children+1]={type="stack",key="lyrics.interlude."..i,width=diameter,height=diameter,
            offset={x=x,y=y},children=dots,style={opacity=reduced and 0.8 or (entered and 0.85 or 0.18)},
            transform={scale=scale,rotate=0,originX=0.5,originY=0.5},accessibility={hidden=true}}
    end
    return {type="stack",key="lyrics.interlude",width=width,height=height,overflow="clip",children=children,
        accessibility={role="text",label=description}}
end
function M.scene(m,g,line,media,width,height,p,primary,accent)
    local prepared=prepare(line,g,p,width,height);local children={};local pageIndex=1
    for i,page in ipairs(prepared.pages) do if media>=page.start then pageIndex=i end end
    local page=prepared.pages[pageIndex]
    if not page then return {type="stack",key="lyrics.dynamic",width=width,height=height,children={}} end
    local reduced=m.reducedMotion==true
    local breath=reduced and 0 or p.font*0.20*math.sin((media-line.start)/7000*math.pi*2)
    local top=(height-page.height-prepared.subtitleHeight)/2+breath
    local lineAlpha=reduced and 1 or 0.2+0.8*ease((media-page.start)/260)
    local glowBudget=72
    for _,row in ipairs(page.rows) do
        local previousAngle
        for _,piece in ipairs(row.pieces) do
            local value=pose(piece,media,page.start,reduced,row.angle,previousAngle);previousAngle=value.rotate
            if reduced or value.elapsed>=0 then
                local y=piece.anchorY+value.y*p.font;local roomY=math.max(0,row.height/2-value.ey)
                y=math.max(-roomY,math.min(roomY,y))
                local py=top+row.y+(row.height-piece.height)/2+y
                local progress=clamp((media-piece.start)/math.max(1,piece.finish-piece.start))
                local brightness=progress*(1-ease((media-piece.finish)/800));local items={}
                if not reduced and glowBudget>=8 and media>=piece.start and media<=piece.finish+1200 then
                    local letters=glyphs(piece,p.font)
                    for i,glyph in ipairs(letters) do
                        local duration=math.max(70,glyph.finish-glyph.start);local age=media-glyph.start;local strength
                        if #letters==1 then strength=ease(age/math.max(80,duration*0.25))*math.exp(-math.max(0,media-piece.finish)/300)
                        else
                            local t=age/(duration*6);strength=t<0.3 and ease(t/0.3) or 1-ease((t-0.3)/0.7)
                            strength=strength*math.exp(-math.max(0,media-piece.finish)/300)
                        end
                        if strength>0.025 and glowBudget>=8 then
                            for j,direction in ipairs(directions) do
                                local radius=p.font*0.13
                                local glow=label(piece.key..".glow."..i.."."..j,glyph.text,glyph.x+direction[1]*radius,
                                    direction[2]*radius,glyph.width,piece.height,p.font,accent,0.075*strength)
                                glow.accessibility={hidden=true};items[#items+1]=glow
                            end
                            glowBudget=glowBudget-8
                        end
                    end
                end
                items[#items+1]=label(piece.key..".body",piece.text,0,0,piece.width,piece.height,p.font,color(primary,accent,brightness),1)
                children[#children+1]={type="stack",key=piece.key,width=piece.width,height=piece.height,
                    offset={x=piece.x,y=py},children=items,style={opacity=value.alpha*lineAlpha},
                    transform={scale=value.scale,rotate=value.rotate,originX=0.5,originY=0.5}}
            end
        end
    end
    if g.subtitle and prepared.subtitleRows>0 then
        local count=#g.subtitle.rows;local first=math.floor((pageIndex-1)*count/#prepared.pages)+1
        local last=math.max(first,math.floor(pageIndex*count/#prepared.pages));local chunks=math.ceil((last-first+1)/prepared.subtitleRows)
        local part=math.min(chunks-1,math.floor(clamp((media-page.start)/math.max(1,page.finish-page.start))*chunks))
        first=first+part*prepared.subtitleRows
        for n=first,math.min(last,first+prepared.subtitleRows-1) do
            local row=g.subtitle.rows[n];local y=top+page.height+p.font*0.35+(n-first)*g.subtitle.lineHeight
            children[#children+1]=label("lyrics.subtitle."..n,row.text,prepared.left,y,row.boxWidth,g.subtitle.lineHeight,
                g.subtitle.font,"textSecondary",0.85*lineAlpha)
        end
    end
    return {type="stack",key="lyrics.dynamic",width=width,height=height,overflow="clip",children=children}
end
return M
