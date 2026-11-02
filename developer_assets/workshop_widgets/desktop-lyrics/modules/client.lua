local providers=module.require("modules/providers.lua").sources
local lyrics=module.require("modules/lyrics.lua")
local M={}
function M.new(d)
    local self={key=nil,lines={},state="idle",cache={},order={},nextTry=0,failures=0,enabled=true}
    function self:clear()
        if self.request then d.cancel(self.request) end
        self.request=nil;self.deferred=nil;self.phase=nil;self.lines={};self.offset=0;self.state="idle";self.best=nil
        self.providerIndex=0;self.providerState=nil;self.instrumental=false;self.networkFailure=false
    end
    function self:requestStep(step)
        if not self.song or self.request or self.disposed then return end
        if not self.enabled then self.deferred=step;return end
        if not d.permission() then self.state="network_permission";return end
        local headers={Accept="application/json",["User-Agent"]="SnowDesktopLyrics/1.1.5 (+https://github.com/FreeFallingSnow/SnowDesktop)"}
        for key,value in pairs(step.headers or {}) do headers[key]=value end
        local ok,id=pcall(d.start,{url=step.url,method=step.method or "GET",body=step.body,
            timeoutMs=5000,cacheSeconds=3600,maxBytes=1048576,headers=headers})
        if not ok or not id then self.networkFailure=true;self:nextProvider();return end
        self.request=id;self.phase=step.phase;self.requestKey=self.key
        self.state=self.best and "ready" or "loading"
    end
    function self:setEnabled(enabled)
        self.enabled=enabled==true
        if self.enabled and self.deferred and not self.request then
            local step=self.deferred;self.deferred=nil;self:requestStep(step)
        end
    end
    function self:failed(code)
        self.state=code;self.failures=math.min(5,self.failures+1)
        self.nextTry=d.monotonic()+math.min(300000,15000*2^self.failures)
    end
    function self:nextProvider()
        self.providerIndex=self.providerIndex+1;self.providerState={}
        local provider=providers[self.providerIndex]
        if provider then self:requestStep(provider.start(self.song))
        elseif self.best then self:accept(self.best,true)
        elseif self.instrumental then self.state="instrumental";self.nextTry=math.huge
        else self:failed(self.networkFailure and "network" or "not_found") end
    end
    function self:begin()
        if not self.song or self.request or self.deferred or self.disposed then return end
        self.providerIndex=0;self.best=nil;self.instrumental=false;self.networkFailure=false;self:nextProvider()
    end
    function self:setSong(song)
        local key=song and song.key
        local same=song and self.song and lyrics.normal(lyrics.title(song.title))==lyrics.normal(lyrics.title(self.song.title)) and
            lyrics.normal(song.artist)==lyrics.normal(self.song.artist) and math.abs(song.duration-self.song.duration)<=3000
        if key==self.key or same then self.song=song;return end
        self:clear();self.key=key;self.song=song;self.failures=0;self.nextTry=0
        if not song then return end
        local cached=self.cache[key]
        if cached then self:accept(cached,true)
        else self:begin() end
    end
    function self:accept(result,cache)
        if not result or #result.lines==0 then return false end
        self.lines=result.lines;self.offset=result.offset or 0;self.state="ready";self.failures=0
        self.hasTranslation=false;self.hasRomanization=false
        for _,line in ipairs(self.lines) do
            if line.translation and line.translation~="" then self.hasTranslation=true end
            if line.romanization and line.romanization~="" then self.hasRomanization=true end
        end
        -- Keep a small VM-local cache; package and user files are never written.
        if cache and #result.lines<=1000 then
            if not self.cache[self.key] then self.order[#self.order+1]=self.key end
            self.cache[self.key]=result
            while #self.order>4 do self.cache[table.remove(self.order,1)]=nil end
        end
        return true
    end
    function self:complete(e)
        if not self.request or e.taskId~=self.request then return false end
        local phase,key=self.phase,self.requestKey;self.request=nil;self.phase=nil
        if not self.song or key~=self.key or self.disposed then return true end
        local status=e.status or (type(e.value)=="table" and e.value.status)
        local ok,body=false,nil
        if e.ok and type(e.value)=="table" and status==200 and type(e.value.body)=="string" then ok,body=pcall(d.decode,e.value.body) end
        if e.error=="permissionRevoked" or e.error=="permissionDenied" then self.state="network_permission";return true end
        if not ok and status~=404 then self.networkFailure=true end
        local provider=providers[self.providerIndex]
        local parsed,nextStep=pcall(provider.complete,self.song,phase,ok and body or nil,self.providerState)
        if not parsed then self.networkFailure=true;nextStep=nil end
        if nextStep and nextStep.url then self:requestStep(nextStep)
        else
            local result=nextStep and nextStep.result
            if nextStep and nextStep.instrumental then self.instrumental=true end
            if result then
                local score=result.matchScore or 100;local oldScore=self.best and (self.best.matchScore or 100) or -1
                if score>oldScore or (score==oldScore and lyrics.hasWords(result) and not lyrics.hasWords(self.best)) then
                    self.best=result;self:accept(result,false)
                end
                if score>=100 and lyrics.hasWords(result) then self:accept(result,true);return true end
            end
            self:nextProvider()
        end
        return true
    end
    function self:retry(force)
        if not self.song or self.request or self.deferred or not self.enabled then return end
        if force or (self.state~="ready" and d.monotonic()>=self.nextTry) then self:begin() end
    end
    function self:dispose() self:clear();self.disposed=true end
    return self
end
return M
