local M={}
local function trim(s) return (s:gsub("^%s+",""):gsub("%s+$","")) end
function M.normal(s)
    if type(s)~="string" then return "" end
    s=s:lower():gsub("[%s%p]","")
    for _,mark in ipairs({"　","，","。","！","？","、","（","）","【","】","《","》","·"}) do s=s:gsub(mark,"") end
    return s
end
function M.encode(s) return (s:gsub("[^%w%-._~]",function(c)return string.format("%%%02X",c:byte())end)) end
local versionMarkers={"live","remix","mix","version","ver.","cover","instrumental","karaoke","remaster","edit","伴奏","现场","現場","翻唱","カラオケ"}
function M.title(s)
    s=type(s)=="string" and s or ""
    local function bracket(inner)
        local lower=inner:lower()
        for _,marker in ipairs(versionMarkers) do if lower:find(marker,1,true) then return "("..inner..")" end end
        return ""
    end
    s=s:gsub("%((.-)%)",bracket):gsub("%[(.-)%]",bracket):gsub("（(.-)）",bracket):gsub("【(.-)】",bracket)
    return trim(s:gsub("%s+[Ff][Ee][Aa][Tt]%.?%s+.*$",""):gsub("%s+[Ff][Tt]%.?%s+.*$",""))
end
local function artists(s)
    s=type(s)=="string" and s or ""
    s=s:gsub("、",","):gsub("与",","):gsub("%s+[Ff][Ee][Aa][Tt]%.?%s+",","):gsub("%s+[Ff][Tt]%.?%s+",",")
    local result={}
    for token in s:gmatch("[^,/&;]+") do local value=M.normal(token);if value~="" then result[#result+1]=value end end
    return result
end
function M.artistMatch(a,b)
    local aa,bb=artists(a),artists(b)
    for _,x in ipairs(aa) do for _,y in ipairs(bb) do
        if x==y or (utf8.len(x)>=3 and utf8.len(y)>=3 and (x:find(y,1,true) or y:find(x,1,true))) then return true end
    end end
    return false
end
function M.matchSong(song,candidate)
    if type(candidate)~="table" or M.normal(M.title(song.title))=="" or
        M.normal(M.title(song.title))~=M.normal(M.title(candidate.title)) then return nil end
    local artist=M.artistMatch(song.artist,candidate.artist)
    local album=M.normal(song.album)~="" and M.normal(song.album)==M.normal(candidate.album)
    if M.normal(song.artist)~="" and not artist then return nil end
    if not artist and not album then return nil end
    local delta=0
    if type(candidate.duration)=="number" and candidate.duration>0 and song.duration>0 then
        delta=math.abs(candidate.duration-song.duration);if delta>3000 then return nil end
    end
    return (artist and 100 or 80)+(album and 10 or 0)-delta/1000
end
function M.bestSong(song,candidates)
    local best,score=nil,-1
    for i,candidate in ipairs(type(candidates)=="table" and candidates or {}) do
        if i>20 then break end
        local rank=M.matchSong(song,candidate)
        if rank and rank>score then best=candidate;score=rank end
    end
    return best,score
end
function M.confidence(song,score)
    local maximum=M.normal(song.album)~="" and 110 or 100
    return math.max(0,math.min(100,math.ceil((score or 0)*100/maximum)))
end
local function matches(target,candidates)
    target=M.normal(target);if target=="" then return false end
    for _,candidate in ipairs(candidates or {}) do
        local normalized=M.normal(candidate)
        if normalized==target or (#normalized>=3 and (target:find(normalized,1,true) or normalized:find(target,1,true))) then return true end
    end
    return false
end
function M.matchAmll(song,items)
    local best,score,identityScore=nil,0,0
    for i,item in ipairs(type(items)=="table" and items or {}) do
        if i>20 then break end
        if type(item)=="table" and type(item.id)=="number" then
            local title,exactTitle=false,false
            for _,name in ipairs(item.musicNames or {}) do
                if M.normal(M.title(name))==M.normal(M.title(song.title)) then title=true end
                if M.normal(name)==M.normal(song.title) then exactTitle=true end
            end
            local artist,exactArtist=false,false
            for _,name in ipairs(item.artistNames or {}) do
                if M.artistMatch(song.artist,name) then artist=true end
                if M.normal(name)==M.normal(song.artist) then exactArtist=true end
            end
            local album=false
            for _,name in ipairs(item.albumNames or {}) do
                if M.normal(song.album)~="" and M.normal(name)==M.normal(song.album) then album=true end
            end
            local knownArtist=M.normal(song.artist)~=""
            local identity=title and (artist and (100+(album and 10 or 0)) or (not knownArtist and album and 80 or 0)) or 0
            local rank=identity>0 and identity+(exactTitle and 0.25 or 0)+(exactArtist and 0.125 or 0) or 0
            if rank>score then best=item;score=rank;identityScore=identity end
        end
    end
    return best,identityScore
end
function M.matchLrc(song,item)
    return type(item)=="table" and M.matchSong(song,{title=item.trackName,artist=item.artistName,album=item.albumName,
        duration=type(item.duration)=="number" and item.duration*1000 or nil})~=nil
end
function M.time(value)
    if type(value)~="string" then return nil end
    value=trim(value)
    -- AMLL TTML also uses plain seconds (19.340) before switching to
    -- minute:second clocks. These are absolute seconds, not milliseconds.
    if value:match("^%d+%.?%d*$") then return tonumber(value)*1000 end
    local ms=value:match("^([%d.]+)ms$");if ms then return tonumber(ms) end
    local sec=value:match("^([%d.]+)s$");if sec then return tonumber(sec)*1000 end
    local parts={};for number in value:gmatch("[^:]+") do parts[#parts+1]=tonumber(number) or false end
    if (#parts~=2 and #parts~=3) then return nil end
    local total=0
    for _,number in ipairs(parts) do if number==false then return nil end;total=total*60+number end
    return total*1000
end
local function finish(lines,duration)
    table.sort(lines,function(a,b)return a.start<b.start end)
    local result={lines={},offset=0}
    for i,line in ipairs(lines) do
        if i>3000 then break end
        line.text=trim(line.text)
        if line.text~="" then
            line.finish=line.finish or (lines[i+1] and lines[i+1].start) or duration
            line.finish=math.max(line.start+1,line.finish)
            result.lines[#result.lines+1]=line
        end
    end
    return result
end
function M.lrc(text,duration)
    if type(text)~="string" or #text>1048576 then return nil end
    local lines={};local offset=tonumber(text:match("%[offset:([%+%-]?%d+)%]")) or 0
    for row in (text.."\n"):gmatch("([^\r\n]*)[\r\n]+") do
        local label=row:gsub("^%s*%b[]","")
        while label:match("^%[%d+:%d") do label=label:gsub("^%b[]","") end
        for stamp in row:gmatch("%[([%d:%.]+)%]") do
            local time=stamp:find(":",1,true) and M.time(stamp)
            if time and time>=0 then lines[#lines+1]={start=time,text=label,words={}} end
        end
    end
    local result=finish(lines,duration);result.offset=offset
    return #result.lines>0 and result or nil
end
function M.yrc(text,duration)
    if type(text)~="string" or #text>1048576 then return nil end
    local lines={}
    for start,length,row in text:gmatch("%[(%d+),(%d+)%]([^\r\n]*)") do
        local words,parts={},{}
        for at,span,value in row:gmatch("%((%d+),(%d+),%d+%)([^%(]*)") do
            parts[#parts+1]=value
            if #words<256 then words[#words+1]={start=tonumber(at),finish=tonumber(at)+tonumber(span),text=value} end
        end
        if #parts>0 then lines[#lines+1]={start=tonumber(start),finish=tonumber(start)+tonumber(length),text=table.concat(parts),words=words} end
    end
    local result=finish(lines,duration);return #result.lines>0 and result or nil
end
local function median(values)
    table.sort(values);local middle=math.floor(#values/2)
    return #values%2==1 and values[middle+1] or (values[middle]+values[middle+1])/2
end
function M.alignTiming(result,lrcText,duration)
    if not result or #result.lines>512 then return result end
    local reference=M.lrc(lrcText,duration)
    if not reference or #reference.lines>1024 then return result end
    local rows={}
    for _,line in ipairs(result.lines) do
        local parts,words={},{};local length=0
        for _,word in ipairs(line.words) do
            local text=M.normal(word.text)
            if text~="" then
                words[#words+1]={first=length+1,last=length+#text,start=word.start}
                parts[#parts+1]=text;length=length+#text
            end
        end
        rows[#rows+1]={text=table.concat(parts),words=words,start=line.start,finish=line.finish}
    end
    local anchors={};local left,comparisons=1,0
    for _,line in ipairs(reference.lines) do
        local text=M.normal(line.text);local count=utf8.len(text)
        if count and count>=4 then
            local at=line.start+(reference.offset or 0);local best
            while rows[left] and rows[left].finish+5000<at do left=left+1 end
            for i=left,#rows do
                local row=rows[i];if row.start-5000>at then break end
                comparisons=comparisons+1;if comparisons>10000 then return result end
                local cursor,repeats=1,0
                while repeats<4 do
                    local first,last=row.text:find(text,cursor,true);if not first then break end
                    local lo,hi=1,#row.words
                    while lo<hi do local mid=math.floor((lo+hi)/2)
                        if row.words[mid].last<first then lo=mid+1 else hi=mid end
                    end
                    local word=row.words[lo]
                    if word and word.first<=first and word.last>=first then
                        local delta=word.start+(result.offset or 0)-at
                        if math.abs(delta)<=5000 and (not best or math.abs(delta)<math.abs(best.delta)) then
                            best={delta=delta,at=at,row=i}
                        end
                    end
                    cursor=last+1;repeats=repeats+1
                end
            end
            if best then anchors[#anchors+1]=best end
        end
    end
    if #anchors<8 then return result end
    local deltas={};for _,a in ipairs(anchors) do deltas[#deltas+1]=a.delta end
    local center=median(deltas)
    if math.abs(center)<500 or math.abs(center)>4000 then return result end
    local inliers,distinct={},{}
    for _,a in ipairs(anchors) do if math.abs(a.delta-center)<=500 then inliers[#inliers+1]=a;distinct[a.row]=true end end
    local rowCount=0;for _ in pairs(distinct) do rowCount=rowCount+1 end
    if #inliers<8 or #inliers<#anchors*0.8 or rowCount<5 or
        inliers[#inliers].at-inliers[1].at<math.max(15000,duration*0.2) then return result end
    local values,early,late={},{},{}
    local split=(inliers[1].at+inliers[#inliers].at)/2
    for _,a in ipairs(inliers) do
        values[#values+1]=a.delta
        local half=a.at<=split and early or late;half[#half+1]=a.delta
    end
    center=median(values)
    local deviations={};for _,v in ipairs(values) do deviations[#deviations+1]=math.abs(v-center) end
    if #early<3 or #late<3 or median(deviations)>200 or math.abs(median(early)-median(late))>250 then return result end
    -- Keep every native word interval intact. The measured source offset is
    -- local to these lyrics and never changes the user's global preference.
    result.offset=(result.offset or 0)-center
    result.sourceAlignment={offsetMs=result.offset,anchors=#inliers}
    return result
end
function M.alternate(result,translation,romanization,duration)
    if not result then return nil end
    local function attach(text,key)
        local parsed=M.lrc(text,duration);if not parsed then return end
        local at=1
        for _,line in ipairs(result.lines) do
            local effective=line.start+(result.offset or 0)-(parsed.offset or 0)
            while parsed.lines[at+1] and parsed.lines[at+1].start<=effective+50 do at=at+1 end
            local other=parsed.lines[at]
            if math.abs(other.start-effective)<=150 then line[key]=other.text end
        end
    end
    attach(translation,"translation");attach(romanization,"romanization");return result
end
function M.hasWords(result)
    for _,line in ipairs(result and result.lines or {}) do if #line.words>0 then return true end end
    return false
end
local base64Chars="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
local base64Values={};for i=1,#base64Chars do base64Values[base64Chars:sub(i,i)]=i-1 end
function M.base64(text)
    if type(text)~="string" or #text>1048576 then return nil end
    text=text:gsub("%s","");if #text%4~=0 or text:find("[^%w+/=]") then return nil end
    local result={}
    for at=1,#text,4 do
        local a,b,c,d=text:sub(at,at),text:sub(at+1,at+1),text:sub(at+2,at+2),text:sub(at+3,at+3)
        if not base64Values[a] or not base64Values[b] or (c~="=" and not base64Values[c]) or (d~="=" and not base64Values[d]) then return nil end
        if (c=="=" and d~="=") or ((c=="=" or d=="=") and at+3~=#text) then return nil end
        local bits=base64Values[a]*262144+base64Values[b]*4096+(base64Values[c] or 0)*64+(base64Values[d] or 0)
        result[#result+1]=string.char(math.floor(bits/65536)%256)
        if c~="=" then result[#result+1]=string.char(math.floor(bits/256)%256) end
        if d~="=" then result[#result+1]=string.char(bits%256) end
    end
    local value=table.concat(result);return utf8.len(value) and value or nil
end
local entities={amp="&",lt="<",gt=">",quot='"',apos="'"}
local function decode(s)
    return (s:gsub("&([^;]+);",function(e)
        if entities[e] then return entities[e] end
        local code=tonumber(e:match("^#x(%x+)$") or "",16) or tonumber(e:match("^#(%d+)$") or "")
        if code and code>0 and code<=0x10ffff and not (code>=0xd800 and code<=0xdfff) then return utf8.char(code) end
        return ""
    end))
end
local function localName(s) return s:match("([^:]+)$") end
local function xml(text)
    assert(#text<=1048576 and not text:find("<!DOCTYPE",1,true) and not text:find("<!ENTITY",1,true),"unsupported XML")
    text=text:gsub("<!%-%-.-%-%->",""):gsub("<%?.-%?>","")
    local root={name="root",children={},attrs={}};local stack={root};local at,nodes=1,0
    while at<=#text do
        local first,last,tag=text:find("<([^>]+)>",at)
        if not first then
            local tail=text:sub(at);if tail~="" then table.insert(stack[#stack].children,decode(tail)) end;break
        end
        if first>at then table.insert(stack[#stack].children,decode(text:sub(at,first-1))) end
        if tag:sub(1,1)=="/" then
            assert(#stack>1 and stack[#stack].raw==trim(tag:sub(2)),"unbalanced XML")
            table.remove(stack)
        else
            local raw=tag:match("^([%w_:%-%.]+)");assert(raw,"invalid XML tag")
            local node={name=localName(raw),raw=raw,attrs={},children={}}
            for name,quote,value in tag:gmatch("([%w_:%-%.]+)%s*=%s*(['\"])(.-)%2") do node.attrs[name]=decode(value) end
            table.insert(stack[#stack].children,node);nodes=nodes+1
            assert(nodes<=30000 and #stack<32,"XML limit")
            if tag:sub(-1)~="/" then stack[#stack+1]=node end
        end
        at=last+1
    end
    assert(#stack==1,"unclosed XML");return root
end
local function content(node)
    if type(node)=="string" then return node end
    if node.name=="br" then return " " end
    local out={};for _,child in ipairs(node.children) do out[#out+1]=content(child) end
    return table.concat(out)
end
function M.ttml(text,duration)
    if type(text)~="string" then return nil end
    local ok,root=pcall(xml,text);if not ok then return nil end
    local lines={}
    local function visit(node)
        if type(node)~="table" then return end
        if node.name=="p" then
            local start=M.time(node.attrs.begin);local stop=M.time(node.attrs["end"])
            if not start then return end
            local parts,words={},{};local translation,romanization="",""
            local function gather(child)
                if type(child)=="string" then
                    if not (child:find("[\r\n]") and child:match("^%s*$")) then parts[#parts+1]=child end
                    return
                end
                local role=child.attrs["ttm:role"] or child.attrs.role or ""
                if role=="x-translation" or role=="translation" then translation=content(child);return end
                if role=="x-roman" or role=="x-romanization" then romanization=content(child);return end
                if role=="x-bg" or role=="x-background" then return end
                local a,b=M.time(child.attrs.begin),M.time(child.attrs["end"])
                if a and b and b>=a then
                    local value=content(child);parts[#parts+1]=value
                    if #words<256 then words[#words+1]={text=value,start=a,finish=b} end
                elseif child.name=="br" then parts[#parts+1]=" "
                else for _,nested in ipairs(child.children) do gather(nested) end end
            end
            for _,child in ipairs(node.children) do gather(child) end
            local full=table.concat(parts)
            -- Only use native word timings when their text covers the full line.
            local joined={};for _,w in ipairs(words) do joined[#joined+1]=w.text end
            if M.normal(table.concat(joined))~=M.normal(full) then words={} end
            lines[#lines+1]={start=start,finish=stop,text=full,words=words,translation=translation,romanization=romanization}
        else for _,child in ipairs(node.children) do visit(child) end end
    end
    visit(root)
    local result=finish(lines,duration)
    return #result.lines>0 and result or nil
end
return M
