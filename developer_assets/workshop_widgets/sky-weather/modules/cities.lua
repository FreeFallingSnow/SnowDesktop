local M={}
local function trim(value) return value:match("^%s*(.-)%s*$") end
local function baseName(value)
    value=value:gsub("，",",")
    return trim(value:match("^[^,]+") or value):lower():gsub("市$","")
end
function M.queries(query)
    query=trim(query):gsub("，",",")
    local out={query}
    local separator=query:find(",",1,true)
    local base=trim(separator and query:sub(1,separator-1) or query)
    local count=0
    for char in base:gmatch("[^\128-\191][\128-\191]*") do
        local a,b,c=char:byte(1,3)
        if #char~=3 or not b or not c then return out end
        local code=(a-224)*4096+(b-128)*64+c-128
        if code<0x3400 or code>0x9FFF then return out end
        count=count+1
    end
    if count>=2 and not base:match("市$") and not base:match("县$") and not base:match("縣$")
        and not base:match("区$") and not base:match("區$") and not base:match("州$") and not base:match("旗$") then
        out[2]=base.."市"..(separator and query:sub(separator) or "")
    end
    return out
end
function M.valid(p)
    return type(p)=="table" and type(p.name)=="string" and p.name~=""
        and type(p.label)=="string" and type(p.lat)=="number" and type(p.lon)=="number"
        and p.lat>=-90 and p.lat<=90 and p.lon>=-180 and p.lon<=180
end
function M.results(payload,query)
    local out={}
    if type(payload)~="table" or type(payload.results)~="table" then return out end
    for _,p in ipairs(payload.results) do
        if type(p)=="table" and type(p.latitude)=="number" and type(p.longitude)=="number"
            and p.latitude>=-90 and p.latitude<=90 and p.longitude>=-180 and p.longitude<=180
            and type(p.name)=="string" and p.name~="" then
            local parts={p.name}
            for _,key in ipairs({"admin2","admin1","country"}) do
                local value=p[key]
                if type(value)=="string" and value~="" then
                    local duplicate=false
                    for _,part in ipairs(parts) do if part==value then duplicate=true;break end end
                    if not duplicate then parts[#parts+1]=value end
                end
            end
            local population=type(p.population)=="number" and p.population==p.population and p.population>0 and p.population<1e12 and p.population or 0
            local exact=query and baseName(p.name)==baseName(query) or false
            out[#out+1]={name=p.name,label=table.concat(parts," · "),lat=p.latitude,lon=p.longitude,
                id=p.id,population=population,exact=exact,index=#out+1}
        end
    end
    table.sort(out,function(a,b)
        if a.exact~=b.exact then return a.exact end
        if a.population~=b.population then return a.population>b.population end
        return a.index<b.index
    end)
    local unique,seen={},{ }
    for _,p in ipairs(out) do
        local key=type(p.id)=="number" and "id:"..tostring(p.id) or string.format("%.6f:%.6f:%s",p.lat,p.lon,p.name)
        if not seen[key] then
            seen[key]=true;p.exact=nil;p.index=nil;unique[#unique+1]=p
            if #unique==20 then break end
        end
    end
    return unique
end
function M.search(options)
    local state={requests={},results={},places={},loading=false,error=false}
    function state:cancel()
        for id in pairs(self.requests) do options.cancel(id) end
        self.requests={};self.loading=false
    end
    function state:query(query)
        self:cancel();self.queryText=trim(query);self.results={};self.places={};self.error=false
        if self.queryText=="" then return end
        for _,name in ipairs(M.queries(self.queryText)) do
            local id=options.start({url="https://geocoding-api.open-meteo.com/v1/search?name="..options.encode(name)..
                "&count=100&format=json&language="..options.language(),timeoutMs=12000,cacheSeconds=3600,maxBytes=131072})
            if id then self.requests[id]=true else self.error=true end
        end
        self.loading=next(self.requests)~=nil
    end
    function state:complete(event)
        if not self.requests[event.taskId] then return false end
        self.requests[event.taskId]=nil
        local ok,p=pcall(options.decode,event.ok and event.value and event.value.body or "")
        if ok and type(p)=="table" and not p.error and (p.results==nil or type(p.results)=="table") then
            for i,place in ipairs(p.results or {}) do
                if i>100 then break end
                self.places[#self.places+1]=place
            end
        else self.error=true end
        self.loading=next(self.requests)~=nil
        if not self.loading then self.results=M.results({results=self.places},self.queryText) end
        return true
    end
    return state
end
function M.recent(previous,place)
    local out={}
    if M.valid(place) then out[1]=place end
    if type(previous)=="table" then
        for _,p in ipairs(previous) do
            if M.valid(p) then
                local duplicate=false
                for _,q in ipairs(out) do if p.lat==q.lat and p.lon==q.lon then duplicate=true;break end end
                if not duplicate then out[#out+1]=p;if #out==6 then break end end
            end
        end
    end
    return out
end
return M
