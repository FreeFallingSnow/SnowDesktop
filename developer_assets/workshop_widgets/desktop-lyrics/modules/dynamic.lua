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
local function prepare(line,g,p,width)
    if g.replica then return g.replica end
    local available=math.max(1,p.width-p.font*0.2)
    local block=Text.wrap(line.text,p.font,available/2.2,draw.measureText)
    Text.words(block,{text=line.text,words=g.words},draw.measureText)
    local result={pieces={}}
    local function piece(row,n,first,last,x1,x2,start,finish)
        local value=line.text:sub(first,last)
        if value:match("^%s*$") then return end
        local index=#result.pieces+1;local seed=line.start/1000+index
        local measured=draw.measureText(value,p.font,0,true)
        local item={key="lyrics.word."..tostring(line.start).."."..index,text=value,width=measured.width+p.font*0.12,
            height=math.max(p.font*1.35,measured.height),start=start or line.start,finish=finish or line.finish,
            scale=start and 1.1+random(seed,4)*0.2 or 1,
            dx=start and (random(seed,1)-0.5)*p.font*0.56 or 0,
            dy=start and (random(seed,2)-0.5)*p.font*0.56 or 0,
            rotate=start and (random(seed,3)-0.5)*10 or 0,
            passedRotate=start and (random(seed,8)-0.5)*45 or 0}
        -- Measure each word's transformed footprint instead of scaling an entire
        -- text run and assuming every word needs the same extra spacing.
        local function extent(w,h,angle)
            angle=math.min(math.rad(angle),math.atan(h/w))
            return w*math.cos(angle)+h*math.sin(angle)
        end
        local peak=item.scale*1.4*1.044
        local activeAngle=math.abs(item.rotate)+3
        local settledAngle=math.abs(item.rotate)+math.max(20,math.abs(item.passedRotate))
        item.slotWidth=math.max(peak*extent(item.width,item.height,activeAngle),
            item.scale*extent(item.width,item.height,settledAngle))
        item.slotHeight=math.max(peak*extent(item.height,item.width,activeAngle),
            item.scale*extent(item.height,item.width,settledAngle))+p.font*0.56
        item.first=first;item.last=last
        result.pieces[#result.pieces+1]=item
    end
    for n,row in ipairs(block.rows) do
        local cursor=row.first;local x=0
        for _,segment in ipairs(row.segments) do
            if segment.first>cursor then piece(row,n,cursor,segment.first-1,x,segment.x1) end
            local span=segment.word.finish-segment.word.start
            piece(row,n,segment.first,segment.last,segment.x1,segment.x2,
                segment.word.start+span*segment.from,segment.word.start+span*segment.to)
            cursor=segment.last+1;x=segment.x2
        end
        if cursor<=row.last then piece(row,n,cursor,row.last,x,row.width) end
    end
    local rows={};local row={pieces={},width=0,height=0};local previous
    for _,item in ipairs(result.pieces) do
        local gap=previous and (line.text:sub(previous.last+1,item.first-1):find("%s") and p.font*0.24 or p.font*0.05) or 0
        if #row.pieces>0 and row.width+gap+item.slotWidth>available then
            rows[#rows+1]=row;row={pieces={},width=0,height=0};gap=0
        end
        item.slotX=row.width+gap;row.width=item.slotX+item.slotWidth
        row.height=math.max(row.height,item.slotHeight);row.pieces[#row.pieces+1]=item;previous=item
    end
    if #row.pieces>0 then rows[#rows+1]=row end
    local top=0
    for _,r in ipairs(rows) do
        for _,item in ipairs(r.pieces) do
            item.x=(width-r.width)/2+item.slotX+(item.slotWidth-item.width)/2
            item.y=top+(r.height-item.height)/2;item.slotHeight=r.height
        end
        top=top+r.height+p.font*0.08
    end
    result.height=math.max(0,top-p.font*0.08);result.lineHeight=rows[1] and rows[1].height or p.font*2
    result.subtitleY=result.height+p.font*0.55
    result.height=result.height+(g.subtitle and p.font*0.55+g.subtitle.height or 0)
    g.replica=result;return result
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
function M.scene(m,g,line,media,width,height,p,primary,accent)
    local prepared=prepare(line,g,p,width);local children={}
    local available=math.max(1,height-p.padding*2);local top=(height-prepared.height)/2
    if prepared.height>available then
        local focus=0
        for _,piece in ipairs(prepared.pieces) do
            if media>=piece.start then
                local progress=clamp((media-piece.start)/math.max(1,piece.finish-piece.start))
                focus=math.max(focus,piece.y+p.font*(0.5+progress))
            end
        end
        top=p.padding-math.max(0,math.min(prepared.height-available,focus-available/2))
    end
    local reduced=m.reducedMotion==true
    local breath=reduced and 0 or p.font*0.20*math.sin((media-line.start)/7000*math.pi*2)
    local lineAlpha=reduced and 1 or 0.2+0.8*ease((media-line.start)/260)
    local glowBudget=72
    local allowed
    if #prepared.pieces>180 then
        local ranked={};allowed={}
        for i,piece in ipairs(prepared.pieces) do
            if reduced or media>=piece.start-150 then ranked[#ranked+1]={index=i,distance=math.abs(top+piece.y+piece.height/2-height/2)} end
        end
        table.sort(ranked,function(a,b)return a.distance==b.distance and a.index<b.index or a.distance<b.distance end)
        for i=1,math.min(180,#ranked) do allowed[ranked[i].index]=true end
    end
    for index,piece in ipairs(prepared.pieces) do
        local elapsed=(media-piece.start+150)/1000
        if (not allowed or allowed[index]) and (reduced or elapsed>=0) then
            local enter=spring(elapsed);local after=ease((media-piece.finish)/500)
            local target=piece.scale*1.4
            local scale=reduced and 1 or 0.5+(target-0.5)*enter+(piece.scale-target)*after
            local x=piece.dx+(math.sin(piece.dy)*p.font*1.39)*(1-enter)
            local y=piece.dy+(math.cos(piece.dx)*p.font*0.69)*(1-enter)
            local rotate=piece.rotate+20*(1-enter)+piece.passedRotate*clamp((media-piece.finish)/5000)
            -- Entry drift stays inside the footprint reserved for this word.
            -- The settled Folia-style offsets remain intact; neighboring words
            -- and rows cannot collide during the entry or scale overshoot.
            local radians=math.rad(rotate)
            local extentX=scale*(math.abs(math.cos(radians))*piece.width+math.abs(math.sin(radians))*piece.height)/2
            local extentY=scale*(math.abs(math.cos(radians))*piece.height+math.abs(math.sin(radians))*piece.width)/2
            local roomX=math.max(0,piece.slotWidth/2-extentX);local roomY=math.max(0,piece.slotHeight/2-extentY)
            x=math.max(-roomX,math.min(roomX,x));y=math.max(-roomY,math.min(roomY,y))
            local alpha=(reduced and 0.9 or ease(elapsed/0.1)*(1-after*0.18))*lineAlpha
            local py=top+piece.y+breath+(reduced and 0 or y)
            if py+piece.height*2>=0 and py-piece.height<height and alpha>0.001 then
                local progress=clamp((media-piece.start)/math.max(1,piece.finish-piece.start))
                local brightness=progress*(1-ease((media-piece.finish)/800))
                local items={}
                if not reduced and glowBudget>=8 and media>=piece.start and media<=piece.finish+1200 then
                    local letters=glyphs(piece,p.font)
                    for i,glyph in ipairs(letters) do
                    local duration=math.max(70,glyph.finish-glyph.start)
                    local age=media-glyph.start;local strength=0
                    if #letters==1 then
                        strength=ease(age/math.max(80,duration*0.25))*math.exp(-math.max(0,media-piece.finish)/300)
                    else
                        local t=age/(duration*6)
                        strength=t<0.3 and ease(t/0.3) or 1-ease((t-0.3)/0.7)
                        strength=strength*math.exp(-math.max(0,media-piece.finish)/300)
                    end
                    if strength>0.025 and glowBudget>=8 then
                        for j,direction in ipairs(directions) do
                            local radius=p.font*0.13
                            local glow=label(piece.key..".glow."..i.."."..j,glyph.text,
                                glyph.x+direction[1]*radius,direction[2]*radius,glyph.width,piece.height,p.font,
                                accent,0.075*strength)
                            glow.accessibility={hidden=true};items[#items+1]=glow
                        end
                        glowBudget=glowBudget-8
                    end
                end end
                items[#items+1]=label(piece.key..".body",piece.text,0,0,piece.width,piece.height,p.font,
                    color(primary,accent,brightness),1)
                children[#children+1]={type="stack",key=piece.key,width=piece.width,height=piece.height,
                    offset={x=piece.x+(reduced and 0 or x),y=py},children=items,style={opacity=alpha},
                    transform={scale=scale,rotate=reduced and 0 or rotate,originX=0.5,originY=0.5}}
            end
        end
    end
    if g.subtitle then for n,row in ipairs(g.subtitle.rows) do
        local y=top+prepared.subtitleY+(n-1)*g.subtitle.lineHeight+breath
        if y+row.height>=0 and y<height then children[#children+1]=label("lyrics.subtitle."..n,row.text,
            (width-row.width)/2,y,row.boxWidth,row.height,g.subtitle.font,"textSecondary",0.85*lineAlpha) end
    end end
    return {type="stack",key="lyrics.dynamic",width=width,height=height,overflow="clip",children=children}
end
return M
