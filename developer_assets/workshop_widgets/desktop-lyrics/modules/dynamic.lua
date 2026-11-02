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
    local block=Text.wrap(line.text,p.font,available/2.2,draw.measureText)
    Text.words(block,{text=line.text,words=g.words},draw.measureText)
    local result={pieces={},pages={},surfaceHeight=height};local measurements={}
    if g.subtitle and #g.subtitle.rows==0 then g.subtitle=nil end
    local room=math.max(1,height-p.padding*2-p.font*0.44)
    result.subtitleRows=g.subtitle and math.max(1,math.floor(room*0.22/g.subtitle.lineHeight)) or 0
    local subtitleHeight=g.subtitle and result.subtitleRows*g.subtitle.lineHeight+p.font*0.35 or 0
    local pageRoom=math.max(1,room-subtitleHeight)
    local function piece(first,last,start,finish,split)
        local value=line.text:sub(first,last)
        if value:match("^%s*$") then return end
        local chars=not split and Text.clusters(value) or nil;local isCjk=false
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
        if not measured then measured=draw.measureText(value,p.font,0,true);measurements[value]=measured end
        local item={key="lyrics.word."..tostring(line.start).."."..index,text=value,
            width=measured.width+p.font*0.12,height=math.max(p.font*1.35,measured.height),
            start=start or line.start,finish=finish or line.finish,scale=start and 1.2 or 1,first=first,last=last}
        item.focusStart=start or line.start+(line.finish-line.start)*(first-1)/math.max(1,#line.text)
        local peak=item.scale*1.4*1.044
        item.slotWidth=peak*extent(item.width,item.height,3)
        item.slotHeight=peak*extent(item.height,item.width,3)+p.font*0.24
        if not split and #chars>1 and (item.slotWidth>available or item.slotHeight>pageRoom) then
            local span=(finish or line.finish)-(start or line.start)
            for i,char in ipairs(chars) do piece(first+char.first-1,first+char.last-1,
                start and start+span*(i-1)/#chars or nil,start and start+span*i/#chars or nil,true) end
            return
        end
        item.fit=math.min(1,available/item.slotWidth,pageRoom/item.slotHeight)
        item.slotWidth=item.slotWidth*item.fit;item.slotHeight=item.slotHeight*item.fit
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
    local gap=p.font*0.14;local rows={};local row={pieces={},width=0,height=0}
    local function rowSize(r)
        r.width=0;r.height=0
        for i,item in ipairs(r.pieces) do
            r.width=r.width+(i>1 and item.gapBefore or 0)+item.slotWidth;r.height=math.max(r.height,item.slotHeight)
        end
    end
    local previous
    for _,item in ipairs(result.pieces) do
        item.gapBefore=previous and line.text:sub(previous.last+1,item.first-1):find("%s") and p.font*0.38 or gap
        local spacing=#row.pieces>0 and item.gapBefore or 0
        if #row.pieces>0 and (row.width+spacing+item.slotWidth>available or #row.pieces>=128) then
            rows[#rows+1]=row;row={pieces={},width=0,height=0};spacing=0
        end
        row.pieces[#row.pieces+1]=item;row.width=row.width+spacing+item.slotWidth
        row.height=math.max(row.height,item.slotHeight);previous=item
    end
    if #row.pieces>0 then rows[#rows+1]=row end
    -- Balance a short final fragment without changing the cached row breaks during playback.
    if #rows>1 then
        local a,b=rows[#rows-1],rows[#rows]
        while #a.pieces>1 and b.width<a.width*0.65 do
            local item=a.pieces[#a.pieces]
            if b.width+item.slotWidth+b.pieces[1].gapBefore>available then break end
            table.remove(a.pieces);table.insert(b.pieces,1,item);rowSize(a);rowSize(b)
        end
    end
    local page={rows={},height=0,start=line.start,pieceCount=0}
    for i,r in ipairs(rows) do
        r.angle=(random(line.start/1000,i)-0.5)*2
        if #page.rows>0 and (page.height+p.font*0.16+r.height>pageRoom or page.pieceCount+#r.pieces>180) then
            result.pages[#result.pages+1]=page;page={rows={},height=0,start=r.pieces[1].focusStart,pieceCount=0}
        end
        r.y=page.height+(#page.rows>0 and p.font*0.16 or 0)
        page.rows[#page.rows+1]=r;page.height=r.y+r.height;page.pieceCount=page.pieceCount+#r.pieces
    end
    if #page.rows>0 then result.pages[#result.pages+1]=page end
    for i,v in ipairs(result.pages) do v.finish=result.pages[i+1] and result.pages[i+1].start or line.finish end
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
local function pose(piece,media,pageStart,reduced,angle)
    local elapsed=(media-math.max(piece.start-150,pageStart))/1000
    local enter=spring(elapsed);local after=ease((media-piece.finish)/500)
    local target=piece.scale*1.4
    local scale=(reduced and 1 or 0.5+(target-0.5)*enter+(piece.scale-target)*after)*piece.fit
    local rotate=reduced and 0 or angle+1.5*(1-enter)
    local radians=math.rad(rotate)
    local ex=scale*(math.abs(math.cos(radians))*piece.width+math.abs(math.sin(radians))*piece.height)/2
    local ey=scale*(math.abs(math.cos(radians))*piece.height+math.abs(math.sin(radians))*piece.width)/2
    return {elapsed=elapsed,after=after,scale=scale,rotate=rotate,ex=ex,ey=ey,
        y=reduced and 0 or (1-enter)*0.12,
        alpha=reduced and 0.9 or ease(elapsed/0.1)*(1-after*0.18)}
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
        local poses={};local rowWidth=0
        for i,piece in ipairs(row.pieces) do
            local value=pose(piece,media,page.start,reduced,row.angle);poses[i]=value
            rowWidth=rowWidth+(i>1 and piece.gapBefore or 0)+value.ex*2
        end
        local x=(width-rowWidth)/2
        for index,piece in ipairs(row.pieces) do
            local value=poses[index];if index>1 then x=x+piece.gapBefore end
            local px=x+value.ex-piece.width/2;x=x+value.ex*2
            if reduced or value.elapsed>=0 then
                local y=value.y*p.font;local roomY=math.max(0,row.height/2-value.ey)
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
                    offset={x=px,y=py},children=items,style={opacity=value.alpha*lineAlpha},
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
            children[#children+1]=label("lyrics.subtitle."..n,row.text,(width-row.width)/2,y,row.boxWidth,g.subtitle.lineHeight,
                g.subtitle.font,"textSecondary",0.85*lineAlpha)
        end
    end
    return {type="stack",key="lyrics.dynamic",width=width,height=height,overflow="clip",children=children}
end
return M
