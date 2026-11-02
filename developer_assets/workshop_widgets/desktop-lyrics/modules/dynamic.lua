local M={}
local function clamp(value)return math.max(0,math.min(1,value))end
local function ease(value)value=clamp(value);return value*value*(3-2*value)end
local function text(x,y,value,font,width,color,alpha)
    draw.text(x,y,value,font,color,math.max(width+font/8,1),true,true,font*2,alpha)
end
local function prepare(line,g)
    if g.dynamic then return end
    for _,row in ipairs(g.main.rows) do
        local pieces={};local cursor=row.first;local x=0
        for _,segment in ipairs(row.segments) do
            if segment.first>cursor then
                pieces[#pieces+1]={text=line.text:sub(cursor,segment.first-1),x=x,width=segment.x1-x}
            end
            pieces[#pieces+1]={text=line.text:sub(segment.first,segment.last),x=segment.x1,
                width=segment.x2-segment.x1,start=segment.word.start+(segment.word.finish-segment.word.start)*segment.from,
                finish=segment.word.start+(segment.word.finish-segment.word.start)*segment.to}
            cursor=segment.last+1;x=segment.x2
        end
        if cursor<=row.last then pieces[#pieces+1]={text=line.text:sub(cursor,row.last),x=x,width=row.width-x} end
        row.dynamic=pieces
    end
    g.dynamic=true
end
function M.render(m,g,line,media,width,height,p,primary,accent)
    prepare(line,g)
    local entry=m.reducedMotion and 1 or ease((media-line.start)/240)
    local alpha=0.5+0.5*entry
    local available=math.max(1,height-2*p.padding)
    local top=(height-g.height)/2
    if g.height>available then
        local focus=#g.words==0 and g.main.height*clamp((media-line.start)/math.max(1,line.finish-line.start)) or 0
        for n,row in ipairs(g.main.rows) do for _,piece in ipairs(row.dynamic) do
            if piece.start and media>=piece.start then
                local progress=clamp((media-piece.start)/math.max(1,piece.finish-piece.start))
                local fraction=clamp((piece.x+piece.width*progress)/math.max(1,row.width))
                focus=math.max(focus,(n-1+fraction)*g.main.lineHeight)
            end
        end end
        local offset=math.max(0,math.min(g.height-available,focus-available/2))
        top=p.padding-offset
    end
    top=top+(m.reducedMotion and 0 or p.font*0.16*(1-entry))
    draw.pushClip(0,0,width,height)
    for n,row in ipairs(g.main.rows) do
        local y=top+(n-1)*g.main.lineHeight
        if y+row.height>=0 and y<height then
            local x=(width-row.width)/2
            for _,piece in ipairs(row.dynamic) do
                if piece.start then
                    local progress=clamp((media-piece.start)/math.max(1,piece.finish-piece.start))
                    local reveal=ease((media-piece.start+100)/180)
                    local phase=clamp((media-piece.start+80)/math.max(160,piece.finish-piece.start+160))
                    local lift=m.reducedMotion and 0 or -p.font*0.10*math.sin(math.pi*phase)
                    text(x+piece.x,y+lift,piece.text,p.font,piece.width,primary,(0.20+0.45*reveal)*alpha)
                    if progress>0 then
                        draw.pushClip(x+piece.x,y+lift,piece.width*progress,row.height)
                        text(x+piece.x,y+lift,piece.text,p.font,piece.width,accent,0.96*alpha)
                        draw.popClip()
                    end
                else text(x+piece.x,y,piece.text,p.font,piece.width,#g.words==0 and accent or primary,0.80*alpha) end
            end
        end
    end
    if g.subtitle then for n,row in ipairs(g.subtitle.rows) do
        local y=top+g.subtitleY+(n-1)*g.subtitle.lineHeight
        if y+row.height>=0 and y<height then
            text((width-row.width)/2,y,row.text,g.subtitle.font,row.width,primary,0.65*alpha)
        end
    end end
    draw.popClip()
end
return M
