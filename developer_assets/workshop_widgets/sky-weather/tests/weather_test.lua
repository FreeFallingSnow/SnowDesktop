local w=module.require("modules/weather.lua")
local j=module.require("modules/json.lua")
local cities=module.require("modules/cities.lua")
local function fixture()
    return [[{"current":{"temperature_2m":21,"apparent_temperature":20,"weather_code":0,"is_day":1,"time":"2026-10-02T14:30"},"daily":{"time":["2026-10-02","2026-10-03","2026-10-04"],"temperature_2m_max":[23,null,20],"temperature_2m_min":[11,17,12],"weather_code":[2,2,61]}}]]
end
local function model()
    local c={location="38.914,121.614",cityName="Dalian",interval=1800}
    local requests,canceled,saved={}, {},nil
    local now,mono,permission=100000,1000,true
    local m=w.new({config=function()return c end,language=function()return"zh-CN"end,
        now=function()return now end,monotonic=function()return mono end,permission=function()return permission end,
        start=function(args) requests[#requests+1]=args;return #requests end,
        cancel=function(id)canceled[#canceled+1]=id end,decode=j.decode,save=function(s)saved=s end})
    return m,c,requests,canceled,function()return saved end,function(n,p)mono=n;now=n;permission=p end
end
return {
    ["Every documented WMO code selects the correct day and night background"]=function()
        local groups={sky={0},cloudy={1,2,3},fog={45,48},rain={51,53,55,56,57,61,63,65,66,67,80,81,82,95,96,97,99},snow={71,73,75,77,85,86}}
        for asset,codes in pairs(groups) do for _,code in ipairs(codes) do
            assert(w.background(code,false).asset==asset,tostring(code))
            local night=w.background(code,true)
            assert(night.night and night.asset==(code==0 and "night" or asset),tostring(code))
        end end
        assert(w.background(97,false).thunder and w.background(97,false).heavy)
        assert(w.background(48,false).asset=="fog" and w.background(67,false).heavy)
        assert(w.background(nil,false).asset=="neutral" and w.background(999,true).asset=="neutral")
    end,
    ["JSON null preserves daily indexes and rejects malformed payloads"]=function()
        local data=w.forecast(j.decode(fixture()))
        assert(#data.days==2 and data.days[2].date=="2026-10-04" and data.days[2].code==61)
        assert(j.decode('"\\uD83C\\uDF24"')=="🌤")
        for _,s in ipairs({'{"x":01}','[1,]','"\\uD800"','{} trailing','1e'}) do assert(not pcall(j.decode,s),s) end
        assert(not w.forecast(j.decode('{"current":{"temperature_2m":null},"daily":{}}')))
    end,
    ["Switching cities cancels old request and rejects late completion"]=function()
        local m,c,requests,canceled,cache=model()
        assert(m:refresh(true) and requests[1].url:find('latitude=38.914',1,true))
        c.location="51.507,-0.128";c.cityName="London";m:refresh(true)
        assert(canceled[1]==1 and not m:complete({taskId=1,ok=true,value={body=fixture()}}))
        assert(m:complete({taskId=2,ok=true,value={body=fixture()}}) and m.city=="London")
        assert(cache().query==c.location and requests[2].url:find('longitude=-0.128',1,true))
    end,
    ["Network failure retains same-city cache and bounded retry"]=function()
        local m,c,requests,_,_,clock=model();m:refresh(false)
        m:complete({taskId=1,ok=true,value={body=fixture()}});m:refresh(true)
        m:complete({taskId=2,ok=false});assert(m.weather.temp==21 and m:stale())
        clock(59000,true);m:tick();assert(#requests==2)
        clock(62000,true);m:tick();assert(#requests==3)
        clock(70000,false);m:tick();assert(m.error=="permission" and not m.request)
        assert(not m:complete({taskId=3,ok=true,value={body=fixture()}}))
    end,
    ["No city performs no automatic IP lookup and disposed model cannot start"]=function()
        local m,c,requests=model();c.location="";assert(not m:refresh(false));m:tick()
        assert(#requests==0 and m.error=="choose_city")
        m:dispose();c.location="0,0";assert(not m:refresh(true) and #requests==0)
    end,
    ["Cache identity and malformed cache cannot crash the renderer"]=function()
        local m,c=model()
        m:restore({query=c.location,updated=1,weather={temp=1,days={{date="2026-10-02",high=2,low=1}}}})
        assert(not m.weather)
        m:restore({query="another",updated=1,weather=w.forecast(j.decode(fixture()))});assert(not m.weather)
    end,
    ["City search disambiguates places and recent list remains bounded"]=function()
        local results=cities.results({results={{name="Paris",country="France",admin1="Île-de-France",latitude=48.85,longitude=2.35},
            {name="Paris",country="United States",admin1="Texas",latitude=33.66,longitude=-95.55},
            {name="bad",latitude=91,longitude=0}}})
        assert(#results==2 and results[2].label=="Paris · Texas · United States")
        local recent=cities.recent({results[1],results[2],results[1]},results[2]);assert(#recent==2 and recent[1].lon==-95.55)
        assert(#cities.recent("bad")==0 and not cities.valid({name="x",label="x",lat=0/0,lon=0}))
        for i=1,9 do recent=cities.recent(recent,{name=tostring(i),label=tostring(i),lat=i,lon=i}) end
        assert(#recent==6 and recent[1].lat==9)
    end,
    ["Chinese city search retrieves the full city name and ranks the live place fixture"]=function()
        local queries=cities.queries("大连") -- l10n-allow: Original Chinese city query fixture.
        assert(#queries==2 and queries[1]=="大连" and queries[2]=="大连市") -- l10n-allow: Expected original Chinese city query data.
        assert(cities.queries("大连,辽宁")[2]=="大连市,辽宁") -- l10n-allow: Original Chinese city query fixture.
        assert(cities.queries("大连，辽宁")[2]=="大连市,辽宁") -- l10n-allow: Original Chinese city query fixture.
        assert(cities.queries(" 大连 , 辽宁 ")[2]=="大连市, 辽宁") -- l10n-allow: Original Chinese city query fixture.
        for _,query in ipairs({"大连市","北京市","朝阳区","Dalian","Paris","大"}) do assert(#cities.queries(query)==1,query) end -- l10n-allow: Original city query fixtures and administrative suffix cases.
        local payload={results={
            {id=7640558,name="大连",admin1="福建省",admin2="龙岩市",country="中国",latitude=25.09238,longitude=116.68508}, -- l10n-allow: Original Chinese geocoding response fixture.
            {id=7758940,name="大连",admin1="海南",admin2="澄迈县",country="中国",latitude=19.85415,longitude=110.10711}, -- l10n-allow: Original Chinese geocoding response fixture.
            {id=1814087,name="大连市",admin1="辽宁",admin2="大连市",country="中国",population=4913879,latitude=38.91222,longitude=121.60222}, -- l10n-allow: Original Chinese geocoding response fixture.
            {id=1814087,name="大连市",admin1="辽宁",country="中国",population=4913879,latitude=38.91222,longitude=121.60222}}} -- l10n-allow: Original Chinese geocoding response fixture.
        local results=cities.results(payload,"大连") -- l10n-allow: Original Chinese city query fixture.
        assert(#results==3 and results[1].id==1814087 and results[1].lat==38.91222 and results[1].lon==121.60222)
        assert(results[1].label=="大连市 · 辽宁 · 中国") -- l10n-allow: Expected labels derived from original Chinese geocoding data.
        assert(results[2].label=="大连 · 龙岩市 · 福建省 · 中国") -- l10n-allow: Expected labels derived from original Chinese geocoding data.
    end,
    ["City search waits for both queries and cancels every stale request"]=function()
        local requests,canceled={},{}
        local state=cities.search({start=function(args)requests[#requests+1]=args;return #requests end,
            cancel=function(id)canceled[id]=true end,encode=w.encode,decode=j.decode,language=function()return"zh"end})
        state:query("大连") -- l10n-allow: Original Chinese city query fixture.
        assert(#requests==2 and requests[2].url:find(w.encode("大连市"),1,true) and state.loading) -- l10n-allow: Expected original Chinese city query data.
        assert(state:complete({taskId=1,ok=true,value={body='{"results":[{"id":7640558,"name":"大连","admin1":"福建省","latitude":25.09238,"longitude":116.68508}]}'}})) -- l10n-allow: Original Chinese geocoding response fixture.
        assert(state.loading and #state.results==0)
        assert(state:complete({taskId=2,ok=true,value={body='{"results":[{"id":1814087,"name":"大连市","admin1":"辽宁","latitude":38.91222,"longitude":121.60222,"population":4913879}]}'}})) -- l10n-allow: Original Chinese geocoding response fixture.
        assert(not state.loading and not state.error and state.results[1].id==1814087)
        state:query("北京");state:query("London") -- l10n-allow: Original city query fixture for cancellation ordering.
        assert(canceled[3] and canceled[4] and #requests==5)
        assert(not state:complete({taskId=3,ok=true,value={body='{}'}}))
        state:cancel();assert(canceled[5] and not state.loading)
        assert(not state:complete({taskId=5,ok=true,value={body='{}'}}))
        state:query("Paris");state:complete({taskId=6,ok=false});assert(state.error and not state.loading)
    end,
    ["Date temperature and WMO codes use independent expected values"]=function()
        assert(w.weekday("2026-10-02")==5 and not w.validDate("2026-02-29") and w.validDate("2024-02-29"))
        assert(w.temperature(-1.5,"c")=="-2°" and w.temperature(0,"f")=="32°")
        local _,icon=w.condition(0,true);assert(icon=="moon")
        _,icon=w.condition(65,false);assert(icon=="rainHeavy")
        _,icon=w.condition(777,false);assert(icon=="unknown")
        assert(w.encode("New York")=="New%20York" and not w.coordinates("91,0"))
    end,
}
