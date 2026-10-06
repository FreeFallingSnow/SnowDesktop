-- Bounded, strict JSON decoder. Null remains distinct from absent fields and
-- zero balances; arrays retain their indexes when a value is unavailable.
local M={null={}}
function M.decode(text)
    if type(text)~="string" or #text>131072 then error("invalid JSON size") end
    local pos,len=1,#text
    local function skip() local _,e=text:find("^[ \t\r\n]*",pos);pos=(e or pos-1)+1 end
    local function fail() error("invalid JSON at "..tostring(pos)) end
    local function hex()
        local s=text:sub(pos,pos+3)
        if #s~=4 or not s:match("^%x%x%x%x$") then fail() end
        pos=pos+4;return tonumber(s,16)
    end
    local function chars(code)
        if code<=127 then return string.char(code) end
        if code<=2047 then return string.char(192+math.floor(code/64),128+code%64) end
        if code<=65535 then return string.char(224+math.floor(code/4096),128+math.floor(code/64)%64,128+code%64) end
        return string.char(240+math.floor(code/262144),128+math.floor(code/4096)%64,128+math.floor(code/64)%64,128+code%64)
    end
    local escapes={['"']='"',['\\']='\\',['/']='/',b='\b',f='\f',n='\n',r='\r',t='\t'}
    local function str()
        pos=pos+1;local out={};local start=pos
        while pos<=len do
            local byte=text:byte(pos)
            if byte==34 then out[#out+1]=text:sub(start,pos-1);pos=pos+1;return table.concat(out) end
            if byte<32 then fail() end
            if byte==92 then
                out[#out+1]=text:sub(start,pos-1);pos=pos+1
                local e=text:sub(pos,pos);pos=pos+1
                if e=="u" then
                    local code=hex()
                    if code>=0xD800 and code<=0xDBFF then
                        if text:sub(pos,pos+1)~="\\u" then fail() end
                        pos=pos+2;local low=hex()
                        if low<0xDC00 or low>0xDFFF then fail() end
                        code=0x10000+(code-0xD800)*1024+low-0xDC00
                    elseif code>=0xDC00 and code<=0xDFFF then fail() end
                    out[#out+1]=chars(code)
                elseif escapes[e] then out[#out+1]=escapes[e] else fail() end
                start=pos
            else pos=pos+1 end
        end
        fail()
    end
    local parse
    parse=function(depth)
        if depth>32 then fail() end
        skip();local c=text:sub(pos,pos)
        if c=='"' then return str() end
        if c=="{" or c=="[" then
            local object=c=="{";local close=object and "}" or "]";local out={}
            pos=pos+1;skip()
            if text:sub(pos,pos)==close then pos=pos+1;return out end
            while true do
                local key
                if object then
                    skip();if text:sub(pos,pos)~='"' then fail() end
                    key=str();skip();if text:sub(pos,pos)~=":" then fail() end;pos=pos+1
                else key=#out+1 end
                out[key]=parse(depth+1);skip();c=text:sub(pos,pos);pos=pos+1
                if c==close then return out end
                if c~="," then fail() end
            end
        end
        for _,pair in ipairs({{"true",true},{"false",false},{"null",M.null}}) do
            if text:sub(pos,pos+#pair[1]-1)==pair[1] then pos=pos+#pair[1];return pair[2] end
        end
        local start=pos
        if c=="-" then pos=pos+1 end
        c=text:sub(pos,pos)
        if c=="0" then pos=pos+1
        elseif c:match("[1-9]") then repeat pos=pos+1 until not text:sub(pos,pos):match("%d")
        else fail() end
        if text:sub(pos,pos)=="." then
            pos=pos+1;if not text:sub(pos,pos):match("%d") then fail() end
            repeat pos=pos+1 until not text:sub(pos,pos):match("%d")
        end
        c=text:sub(pos,pos)
        if c=="e" or c=="E" then
            pos=pos+1;c=text:sub(pos,pos);if c=="+" or c=="-" then pos=pos+1 end
            if not text:sub(pos,pos):match("%d") then fail() end
            repeat pos=pos+1 until not text:sub(pos,pos):match("%d")
        end
        local n=tonumber(text:sub(start,pos-1))
        if not n or n~=n or n==math.huge or n==-math.huge then fail() end
        return n
    end
    local v=parse(0);skip();if pos<=len then fail() end
    return v
end
return M
