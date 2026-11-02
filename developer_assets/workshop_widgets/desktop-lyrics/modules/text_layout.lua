local M={}
local closing={};for _,code in utf8.codes("，。！？；：、,.!?;:)]}）】》」』〉…") do closing[code]=true end
local opening={};for _,code in utf8.codes("([{（【《「『〈") do opening[code]=true end
local pauses={};for _,code in utf8.codes("，。！？；：、,.!?;:") do pauses[code]=true end
local function combining(code)
    return (code>=0x300 and code<=0x36f) or (code>=0x1ab0 and code<=0x1aff) or
        (code>=0x1dc0 and code<=0x1dff) or (code>=0x20d0 and code<=0x20ff) or
        (code>=0xfe00 and code<=0xfe0f) or (code>=0xfe20 and code<=0xfe2f) or
        (code>=0x1f3fb and code<=0x1f3ff) or (code>=0xe0100 and code<=0xe01ef)
end
local function cjk(code)
    return (code>=0x2e80 and code<=0xa4cf) or (code>=0xac00 and code<=0xd7af) or
        (code>=0xf900 and code<=0xfaff) or (code>=0x20000 and code<=0x3134f)
end
local function clusters(value)
    local result={};local previous,regional=0,0
    for at,code in utf8.codes(value) do
        if #result>=4096 then break end
        local flag=code>=0x1f1e6 and code<=0x1f1ff
        local joined=combining(code) or code==0x200d or previous==0x200d or (flag and regional%2==1)
        if joined and #result>0 then result[#result].last=at+#utf8.char(code)-1
        else result[#result+1]={first=at,last=at+#utf8.char(code)-1,code=code} end
        regional=flag and regional+1 or 0;previous=code
    end
    return result
end
local function space(code)return code==32 or code==9 or code==0x3000 end
function M.estimateWords(line)
    if line.words and #line.words>0 then return line.words end
    local duration=(line.finish or 0)-(line.start or 0)
    if duration<=0 then return {} end
    local tokens={};local total=0;local latin
    for _,char in ipairs(clusters(line.text)) do
        local value=line.text:sub(char.first,char.last)
        local blank=space(char.code) or char.code==10 or char.code==13
        local punctuation=closing[char.code] or opening[char.code] or value:match("^%p$")
        local letter=not blank and not punctuation and not cjk(char.code) and char.code<0x1f000
        if not blank then
            local token=letter and latin or nil
            if #tokens>=256 then token=tokens[256] end
            if not token then
                token={first=char.first,last=char.last,weight=0,count=0}
                tokens[#tokens+1]=token
            end
            token.last=char.last
            if not punctuation then
                local weight=letter and token.count>0 and 0.2 or 1
                token.weight=token.weight+weight;total=total+weight;token.count=token.count+1
            end
            latin=letter and token or nil
        else latin=nil end
    end
    if total==0 then return {} end
    -- Leave a short hold at the end; a long instrumental gap must not stretch every character.
    local active=math.min(duration,total*500+1500)*0.9
    local words={};local cursor=line.start
    for _,token in ipairs(tokens) do
        local stop=cursor+active*token.weight/total
        words[#words+1]={text=line.text:sub(token.first,token.last),start=cursor,finish=stop}
        cursor=stop
    end
    return words
end
function M.wrap(value,font,width,measure)
    local chars=clusters(value);local rows={};local first=1
    local available=math.max(width*0.5,width-font/16)
    local lineHeight=font*1.35
    while first<=#chars and #rows<64 do
        while chars[first] and space(chars[first].code) do first=first+1 end
        if not chars[first] then break end
        local hard=first
        while hard<=#chars and chars[hard].code~=10 and chars[hard].code~=13 do hard=hard+1 end
        local paragraphEnd=hard-1;local last=paragraphEnd
        local function size(at)return measure(value:sub(chars[first].first,chars[at].last),font,0,true).width end
        if last>=first and size(last)>available then
            local low,high=first,last
            while low<high do
                local middle=math.floor((low+high+1)/2)
                if size(middle)<=available then low=middle else high=middle-1 end
            end
            last=low
            local preferred,wordBreak
            for at=first,last do
                local code,nextCode=chars[at].code,chars[at+1] and chars[at+1].code
                if pauses[code] and at>=first+(last-first)*0.55 then preferred=at end
                if space(code) or (nextCode and space(nextCode)) or
                    (nextCode and (cjk(code) or cjk(nextCode)) and not opening[code] and not closing[nextCode]) then wordBreak=at end
            end
            last=preferred or wordBreak or last
            if last>first and opening[chars[last].code] then last=last-1 end
            if chars[last+1] and closing[chars[last+1].code] and last>first then last=last-1 end
            -- Avoid leaving one or two CJK characters alone on a final row.
            local tail=last+1;while chars[tail] and tail<=paragraphEnd and space(chars[tail].code) do tail=tail+1 end
            if tail<=paragraphEnd then
                local tailText=value:sub(chars[tail].first,chars[paragraphEnd].last)
                if paragraphEnd-tail+1<=2 and cjk(chars[tail].code) and measure(tailText,font,0,true).width<=available then
                    local middle=math.floor((first+paragraphEnd-1)/2)
                    if middle>=first and middle<=last and not opening[chars[middle].code] and not closing[chars[middle+1].code] and
                        measure(value:sub(chars[middle+1].first,chars[paragraphEnd].last),font,0,true).width<=available then last=middle end
                else
                    local head=value:sub(chars[first].first,chars[last].last)
                    local shortWord=head:match("%s+([^%s]+)%s*$")
                    if shortWord and #shortWord<=3 and not tailText:find("%s") then
                        local cut=last
                        while cut>first and space(chars[cut].code) do cut=cut-1 end
                        while cut>first and not space(chars[cut].code) do cut=cut-1 end
                        if cut>first and measure(value:sub(chars[cut+1].first,chars[paragraphEnd].last),font,0,true).width<=available then last=cut end
                    end
                end
            end
        end
        if last<first then
            rows[#rows+1]={text="",first=chars[first].first,last=chars[first].first-1,width=0,boxWidth=width,height=font*2}
            first=first+1
        else
            local finish=last
            while finish>first and space(chars[finish].code) do finish=finish-1 end
            local text=value:sub(chars[first].first,chars[finish].last)
            local measured=measure(text,font,0,true);lineHeight=math.max(lineHeight,measured.height)
            rows[#rows+1]={text=text,first=chars[first].first,last=chars[finish].last,width=measured.width,
                boxWidth=math.max(width,math.ceil(measured.width+font/8)),height=math.max(font*2,measured.height*1.5),segments={}}
            first=last+1
            if first==hard then
                local code=chars[first] and chars[first].code;first=first+1
                if code==13 and chars[first] and chars[first].code==10 then first=first+1 end
            end
        end
    end
    return {rows=rows,font=font,lineHeight=lineHeight,height=#rows*lineHeight}
end
function M.words(block,line,measure)
    local cursor=1
    for _,word in ipairs(line.words or {}) do
        local value=word.text
        local first,last=line.text:find(value,cursor,true)
        if not first then value=value:gsub("^%s+",""):gsub("%s+$","");first,last=line.text:find(value,cursor,true) end
        if first and last>=first then
            cursor=last+1;local count=utf8.len(value)
            for _,row in ipairs(block.rows) do
                local a,b=math.max(first,row.first),math.min(last,row.last)
                if a<=b then
                    local x1=measure(line.text:sub(row.first,a-1),block.font,0,true).width
                    local x2=measure(line.text:sub(row.first,b),block.font,0,true).width
                    row.segments[#row.segments+1]={word=word,x1=x1,x2=x2,first=a,last=b,
                        from=utf8.len(line.text:sub(first,a-1))/count,to=utf8.len(line.text:sub(first,b))/count}
                end
            end
        end
    end
end
return M
