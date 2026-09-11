local M = {}
local function leap(y) return y % 4 == 0 and (y % 100 ~= 0 or y % 400 == 0) end
function M.parts(s)
    if type(s) ~= "string" then return end
    local y,m,d=s:match("^(%d%d%d%d)%-(%d%d)%-(%d%d)$")
    y,m,d=tonumber(y),tonumber(m),tonumber(d)
    if not y or y<1 or y>9999 or m<1 or m>12 then return end
    local lengths={31,leap(y) and 29 or 28,31,30,31,30,31,31,30,31,30,31}
    if d<1 or d>lengths[m] then return end
    return y,m,d,lengths
end
function M.ordinal(s)
    local y,m,d,lengths=M.parts(s)
    if not y then return end
    local p=y-1
    local n=p*365+math.floor(p/4)-math.floor(p/100)+math.floor(p/400)+d
    for i=1,m-1 do n=n+lengths[i] end
    return n
end
function M.difference(target,today)
    local a,b=M.ordinal(target),M.ordinal(today)
    if a and b then return a-b end
end
function M.monthRange(date)
    local y,m,_,lengths=M.parts(date)
    if y then return string.format("%04d-%02d-01",y,m),string.format("%04d-%02d-%02d",y,m,lengths[m]) end
end
function M.new(deps)
    local self={id=deps.getId() or "",disposed=false}
    function self:refresh()
        if self.subscription then self.subscription:unsubscribe(); self.subscription=nil end
        if not self.disposed and self.id~="" then
            self.subscription=deps.subscribe({eventId=self.id,whenHidden="pause",maxAgeMs=86400000})
        end
    end
    function self:bind(id)
        if self.disposed or self.pending or type(id)~="string" then return false end
        self.id=id; deps.setId(id); self:refresh(); return true
    end
    function self:current()
        if not deps.canRead() then return "permission" end
        if self.id=="" then return "empty" end
        local s=self.subscription and self.subscription:value()
        if s and s.error=="permissionDenied" then return "permission" end
        if not s or not s.available or not s.value then return "unavailable" end
        for _,event in ipairs(s.value.events or {}) do
            if event.id==self.id then return "ready",event end
        end
        return "deleted"
    end
    function self:create(title,date)
        if self.disposed or self.pending then return false,"busy" end
        if not deps.canWrite() then return false,"write_permission" end
        title=type(title)=="string" and title:match("^%s*(.-)%s*$") or ""
        if title=="" or #title>512 then return false,"invalid_title" end
        if not M.parts(date) then return false,"invalid_date" end
        local id,err=deps.start({title=title,date=date,allDay=true,startMinutes=0,endMinutes=0,notes="",reminderMinutes=-1})
        if not id then return false,err or "save_failed" end
        self.pending=id; return true
    end
    function self:complete(e)
        if self.disposed or not self.pending or self.pending~=e.taskId then return false end
        self.pending=nil
        if e.ok and e.value and type(e.value.id)=="string" and e.value.id~="" then
            self:bind(e.value.id); return true
        end
        return false,e.error or "save_failed"
    end
    function self:dispose()
        self.disposed=true
        if self.pending then deps.cancel(self.pending); self.pending=nil end
        if self.subscription then self.subscription:unsubscribe(); self.subscription=nil end
    end
    self:refresh()
    return self
end
return M
