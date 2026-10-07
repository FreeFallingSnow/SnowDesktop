local M={}
function M.new(deps)
    local self={deps=deps,loading=false,data=nil,error=nil,nextAttempt=0,failures=0,disposed=false}
    function self:cancel()
        if self.request then self.deps.cancel(self.request) end
        self.request=nil;self.loading=false
    end
    function self:reset()
        self:cancel();self.identity=nil;self.data=nil;self.updated=nil;self.error=nil
        self.nextAttempt=0;self.lastAttempt=nil;self.failures=0;self.blocked=false
    end
    function self:fail(code,status)
        if code=="secretUnavailable" then self.error="key_missing";self.blocked=true
        elseif code=="permissionRevoked" or code=="permissionDenied" then self.error="permission"
        elseif status==401 then self.error="auth";self.blocked=true
        elseif status==403 then self.error="forbidden";self.blocked=true
        elseif status==402 then self.error="depleted"
        elseif status==429 then self.error="rate"
        elseif status and status>=500 then self.error="server"
        elseif code=="invalid_data" then self.error="invalid_data"
        elseif code=="invalidArguments" then self.error="configure"
        else self.error="network" end
        self.failures=math.min(5,self.failures+1)
        self.nextAttempt=self.deps.monotonic()+math.min(900000,60000*2^(self.failures-1))
    end
    function self:refresh(force)
        if self.disposed then return false end
        local cfg=self.deps.config()
        if cfg.identity~=self.identity then self:reset();self.identity=cfg.identity end
        if not cfg.url or not self.deps.safeUrl(cfg.url) or cfg.valid==false then
            self:cancel();self.error="configure";return false
        end
        if type(cfg.secretRef)~="string" or not cfg.secretRef:match("^secret:v1:[0-9a-f]+$") or #cfg.secretRef~=42 then
            self:cancel();self.error="key_missing";return false
        end
        if not self.deps.permission() then self:cancel();self.error="permission";return false end
        local now=self.deps.monotonic()
        if self.loading or (not force and (self.blocked or now<self.nextAttempt)) or
            (force and self.lastAttempt and now-self.lastAttempt<5000) then return false end
        self.lastAttempt=now;self.error=nil
        local args={url=cfg.url,method="GET",headers={Accept="application/json",["Content-Type"]="application/json"},timeoutMs=15000,cacheSeconds=0,maxBytes=131072}
        args.headers[cfg.header or "Authorization"]={secretRef=cfg.secretRef,prefix=cfg.rawAuth and "" or "Bearer "}
        -- A host-side argument error must not destroy setup or the settings listener.
        local ok,id,err=pcall(self.deps.start,args)
        if not ok then self:fail("invalidArguments");return false end
        if not id then self:fail(err);return false end
        self.request=id;self.loading=true;return true
    end
    function self:settingsChanged(keys)
        local cfg=self.deps.config()
        for _,key in ipairs(keys or {}) do
            if key=="key_"..cfg.provider or key=="interval" then self:reset();break end
        end
        -- Replacements keep the same opaque reference; an explicit reset also
        -- cancels old requests and clears authorization/backoff gates immediately.
        return self:refresh(false)
    end
    function self:complete(event)
        if self.disposed or not self.request or event.taskId~=self.request then return false end
        local cfg=self.deps.config()
        if cfg.identity~=self.identity then self:reset();self:refresh(false);return true end
        self.request=nil;self.loading=false
        if not self.deps.permission() then self.error="permission";return true end
        if not event.ok then self:fail(event.error,event.status);return true end
        local value=event.value
        if type(value)~="table" or type(value.body)~="string" then self:fail("invalid_data");return true end
        if type(value.status)~="number" then self:fail("invalid_data");return true end
        if value.status<200 or value.status>=300 then self:fail("httpStatus",value.status);return true end
        local ok,payload=pcall(self.deps.decode,value.body)
        local parsedOk,parsed=false,nil
        if ok then parsedOk,parsed=pcall(self.deps.parse,cfg.provider,payload,cfg) end
        if not parsedOk or not parsed then self:fail("invalid_data");return true end
        self.data=parsed;self.updated=self.deps.now();self.error=nil;self.failures=0;self.blocked=false
        self.nextAttempt=self.deps.monotonic()+cfg.interval*1000;return true
    end
    function self:dispose() self:cancel();self.disposed=true end
    return self
end
return M
