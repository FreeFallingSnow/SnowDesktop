local M={}
function M.new() return {countdown={value=0,running=false,done=false},stopwatch={value=0,running=false}} end
function M.value(t,now,cd)
    local d=t.running and math.max(0,now-t.started) or 0
    if cd then return math.max(0,t.value-d) end
    return t.value+d
end
function M.start(t,now) if t.running then return end;t.started=now;t.running=true;t.done=false end
function M.pause(t,now,cd) t.value=M.value(t,now,cd);t.running=false end
function M.reset(t) t.value=0;t.running=false;t.done=false end
function M.tick(t,now)
    if t.running and M.value(t,now,true)==0 then t.value=0;t.running=false;t.done=true;return true end
    return false
end
function M.parse(text)
    if type(text)~="string" then return nil end
    local m,s=text:match("^%s*(%d+):(%d%d)%s*$")
    if m then
        if tonumber(s)>59 then return nil end
        s=tonumber(m)*60+tonumber(s)
    elseif text:match("^%s*%d+%s*$") then s=tonumber(text)*60
    else return nil end
    if s<1 or s>359999 then return nil end
    return s*1000
end
function M.format(ms,cd)
    local s=cd and math.ceil(ms/1000) or math.floor(ms/1000)
    if s>=3600 then return string.format("%02d:%02d:%02d",math.floor(s/3600),math.floor(s/60)%60,s%60) end
    return string.format("%02d:%02d",math.floor(s/60),s%60)
end
return M
