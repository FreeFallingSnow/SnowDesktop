-- Pure data rules; all host and network effects are injected by main.lua.
local M = {}
local conditionKeys = {
    "lua_widget.sky_weather.clear", "lua_widget.sky_weather.mainly_clear",
    "lua_widget.sky_weather.partly_cloudy", "lua_widget.sky_weather.overcast",
    "lua_widget.sky_weather.fog", "lua_widget.sky_weather.drizzle",
    "lua_widget.sky_weather.rain", "lua_widget.sky_weather.heavy_rain",
    "lua_widget.sky_weather.snow", "lua_widget.sky_weather.thunder",
    "lua_widget.sky_weather.unknown",
}
local languages = { ["zh-CN"]="zh", ["zh-TW"]="zh", ["en-US"]="en",
    ["ja-JP"]="ja", ["ko-KR"]="ko", ["de-DE"]="de", ["fr-FR"]="fr",
    ["es-ES"]="es", ["es-419"]="es", ["pt-BR"]="pt" }

function M.finite(v)
    return type(v)=="number" and v==v and v~=math.huge and v~=-math.huge
end
function M.trim(v) return tostring(v or ""):match("^%s*(.-)%s*$") end
function M.round(v)
    if not M.finite(v) then return nil end
    return v>=0 and math.floor(v+0.5) or math.ceil(v-0.5)
end
function M.temperature(v,unit)
    if not M.finite(v) then return "—" end
    return tostring(M.round(unit=="f" and (v*9/5+32) or v)) .. "°"
end
function M.encode(v)
    return (tostring(v):gsub("[^%w%-_%.~]",function(c) return string.format("%%%02X",c:byte()) end))
end
function M.coordinates(v)
    local a,b=M.trim(v):match("^([+-]?%d+%.?%d*)%s*,%s*([+-]?%d+%.?%d*)$")
    a,b=tonumber(a),tonumber(b)
    if a and b and a>=-90 and a<=90 and b>=-180 and b<=180 then return a,b end
end
function M.validDate(v)
    if type(v)~="string" then return false end
    local y,m,d=v:match("^(%d%d%d%d)%-(%d%d)%-(%d%d)$")
    y,m,d=tonumber(y),tonumber(m),tonumber(d)
    if not y or y<1 or m<1 or m>12 or d<1 then return false end
    local days={31,28,31,30,31,30,31,31,30,31,30,31}
    if y%4==0 and (y%100~=0 or y%400==0) then days[2]=29 end
    return d<=days[m]
end
function M.weekday(v)
    if not M.validDate(v) then return nil end
    local y,m,d=v:match("^(%d%d%d%d)%-(%d%d)%-(%d%d)$")
    y,m,d=tonumber(y),tonumber(m),tonumber(d)
    if m<3 then m=m+12;y=y-1 end
    local c=math.floor(y/100);local k=y%100
    return (d+math.floor((m+1)*26/10)+k+math.floor(k/4)+math.floor(c/4)+5*c+6)%7
end
function M.condition(code,night)
    if code==0 then return conditionKeys[1],night and "moon" or "sun" end
    if code==1 then return conditionKeys[2],night and "cloudMoon" or "cloudSun" end
    if code==2 then return conditionKeys[3],night and "cloudMoon" or "cloudSun" end
    if code==3 then return conditionKeys[4],"cloud" end
    if code==45 or code==48 then return conditionKeys[5],"fog" end
    if code==51 or code==53 or code==55 or code==56 or code==57 then return conditionKeys[6],"rain" end
    if code==65 or code==67 or code==82 then return conditionKeys[8],"rainHeavy" end
    if code==61 or code==63 or code==66 or code==80 or code==81 then return conditionKeys[7],"rain" end
    if code==71 or code==73 or code==75 or code==77 or code==85 or code==86 then return conditionKeys[9],"snow" end
    if code==95 or code==96 or code==97 or code==99 then return conditionKeys[10],"thunder" end
    return conditionKeys[11],"unknown"
end
-- Open-Meteo WMO WW codes: all documented values, including 97 (heavy thunderstorm).
-- Unknown or absent codes use a neutral surface, never an invented sunny scene.
function M.background(code,night)
    local _,kind=M.condition(code,night)
    local asset="neutral"
    if kind=="sun" or kind=="moon" then asset=night and "night" or "sky"
    elseif kind=="cloudSun" or kind=="cloudMoon" or kind=="cloud" then asset="cloudy"
    elseif kind=="rain" or kind=="rainHeavy" or kind=="thunder" then asset="rain"
    elseif kind=="snow" then asset="snow"
    elseif kind=="fog" then asset="fog" end
    local heavy=kind=="rainHeavy" or code==75 or code==86 or code==97 or code==99
    return {asset=asset,night=night==true,thunder=kind=="thunder",heavy=heavy,
        overcast=code==3,mostlyClear=code==1}
end
function M.place(payload,query)
    local list=type(payload)=="table" and payload.results
    if type(list)~="table" then return nil end
    local fallback,partial
    local q=string.lower(M.trim(query))
    for _,p in ipairs(list) do
        if type(p)=="table" and M.finite(p.latitude) and M.finite(p.longitude)
            and p.latitude>=-90 and p.latitude<=90 and p.longitude>=-180 and p.longitude<=180
            and type(p.name)=="string" and p.name~="" then
            local n=string.lower(p.name)
            if n==q then return {lat=p.latitude,lon=p.longitude,city=p.name} end
            if not partial and n:sub(1,#q)==q then partial=p end
            fallback=fallback or p
        end
    end
    local p=partial or fallback
    if p then return {lat=p.latitude,lon=p.longitude,city=p.name} end
end
local function at(t,k,i) return type(t[k])=="table" and t[k][i] or nil end
function M.forecast(p)
    if type(p)~="table" or type(p.current)~="table" or type(p.daily)~="table" then return nil end
    local c,d=p.current,p.daily
    if not M.finite(c.temperature_2m) or type(c.time)~="string"
        or not M.validDate(c.time:sub(1,10)) or type(d.time)~="table" then return nil end
    local days={}
    for i,date in ipairs(d.time) do
        local hi,lo=at(d,"temperature_2m_max",i),at(d,"temperature_2m_min",i)
        if M.validDate(date) and M.finite(hi) and M.finite(lo) and hi>=lo and date>=c.time:sub(1,10)
            and (#days==0 or date>days[#days].date) then
            days[#days+1]={date=date,high=hi,low=lo,code=at(d,"weather_code",i),pop=at(d,"precipitation_probability_max",i)}
            if #days==5 then break end
        end
    end
    if #days==0 then return nil end
    return {temp=c.temperature_2m,feels=c.apparent_temperature,humidity=c.relative_humidity_2m,
        wind=c.wind_speed_10m,code=c.weather_code,night=c.is_day==0,localTime=c.time,
        utcOffset=M.finite(p.utc_offset_seconds) and p.utc_offset_seconds or 0,days=days}
end

-- Cache identity includes the exact location setting. A new query never adopts
-- another city's cached weather; canceled task IDs cannot complete a new chain.
function M.new(env)
    local self={env=env,city="",weather=nil,request=nil,kind=nil,error=nil,
        loading=false,updated=nil,lastAttempt=nil,query=nil,disposed=false}
    function self:stop()
        local id=self.request;self.request=nil;self.kind=nil
        if id then self.env.cancel(id) end
        self.loading=false
    end
    function self:fail(reason)
        self.error=reason or "error";self.loading=false;self.request=nil;self.kind=nil
    end
    function self:requestKind(kind)
        local url
        if kind=="city" then url="https://geocoding-api.open-meteo.com/v1/search?name="..M.encode(self.query)..
            "&count=10&format=json&language="..(languages[self.env.language()] or "en")
        else
            url="https://api.open-meteo.com/v1/forecast?latitude="..tostring(self.lat).."&longitude="..tostring(self.lon)..
                "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,is_day,wind_speed_10m"..
                "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max&timezone=auto&forecast_days=5"
        end
        local id,err=self.env.start({url=url,timeoutMs=12000,cacheSeconds=self.force and 0 or (kind=="forecast" and 600 or 3600),maxBytes=131072})
        if not id then self:fail(err=="permissionDenied" and "permission" or "error");return false end
        self.request=id;self.kind=kind;self.loading=true;return true
    end
    function self:refresh(force)
        if self.disposed then return false end
        local query=M.trim(self.env.config().location)
        if self.request and self.query==query and not force then return false end
        self:stop()
        if self.query~=query then
            self.weather=nil;self.updated=nil;self.city="";self.lat=nil;self.lon=nil
        end
        self.query=query;self.pendingCity=nil;self.force=force;self.error=nil;self.lastAttempt=self.env.monotonic()
        if not self.env.permission() then self:fail("permission");return false end
        local lat,lon=M.coordinates(query)
        if lat then self.lat=lat;self.lon=lon;self.city=self.env.config().cityName or query;return self:requestKind("forecast") end
        if query=="" then self:fail("choose_city");return false end
        return self:requestKind("city")
    end
    function self:complete(e)
        if self.disposed or not self.request or e.taskId~=self.request then return false end
        local kind=self.kind;self.request=nil;self.kind=nil
        if not self.env.permission() then self:fail("permission");return true end
        if not e.ok or type(e.value)~="table" or type(e.value.body)~="string" then self:fail("error");return true end
        local ok,p=pcall(self.env.decode,e.value.body)
        if not ok or type(p)~="table" then self:fail("error");return true end
        if kind=="forecast" then
            local data=M.forecast(p)
            if not data then self:fail("error");return true end
            self.weather=data;self.city=self.pendingCity or self.city;self.updated=self.env.now()
            self.loading=false;self.error=nil
            self.env.save({query=self.query,city=self.city,lat=self.lat,lon=self.lon,weather=data,updated=self.updated})
        else
            local place
            place=M.place(p,self.query)
            if not place then self:fail(kind=="city" and "city_missing" or "error");return true end
            self.lat=place.lat;self.lon=place.lon;self.pendingCity=place.city
            if not self.weather then self.city=place.city end
            self:requestKind("forecast")
        end
        return true
    end
    function self:restore(cache)
        local query=M.trim(self.env.config().location)
        if type(cache)=="table" and cache.query==query and type(cache.weather)=="table"
            and M.finite(cache.weather.temp) and type(cache.weather.days)=="table"
            and #cache.weather.days>0 and #cache.weather.days<=5 and M.finite(cache.updated)
            and type(cache.weather.localTime)=="string" and M.validDate(cache.weather.localTime:sub(1,10)) then
            local valid=true
            for _,d in ipairs(cache.weather.days) do
                if type(d)~="table" or not M.validDate(d.date) or not M.finite(d.high) or not M.finite(d.low) then valid=false;break end
            end
            if valid then
                self.query=query;self.weather=cache.weather;self.city=tostring(cache.city or "")
                self.updated=cache.updated;self.lat=cache.lat;self.lon=cache.lon
            end
        end
    end
    function self:stale()
        return self.weather~=nil and (self.error~=nil or not self.updated or
            self.env.now()-self.updated>self.env.config().interval*1000)
    end
    function self:tick()
        if self.disposed then return end
        if M.trim(self.env.config().location)=="" then return end
        if not self.env.permission() then self:stop();self:fail("permission");return end
        local elapsed=self.lastAttempt and self.env.monotonic()-self.lastAttempt or math.huge
        local delay=self.error and 60000 or self.env.config().interval*1000
        if not self.request and elapsed>=delay then self:refresh(false) end
    end
    function self:dispose() self:stop();self.disposed=true end
    return self
end
return M
