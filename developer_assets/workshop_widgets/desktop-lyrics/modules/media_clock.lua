local M={}
local function finite(v) return type(v)=="number" and v==v and math.abs(v)<1e14 end
local filter=module.require("modules/media_filter.lua")
M.excluded=filter.excluded
M.pendingMetadata=filter.pendingMetadata
function M.identity(s)
    if type(s)~="table" or M.excluded(s) or M.pendingMetadata(s) or type(s.id)~="string" or type(s.title)~="string" or s.title=="" then return nil end
    local function canonical(value)return (type(value)=="string" and value or ""):lower():gsub("%s","")end
    return table.concat({s.id,canonical(s.title),canonical(s.artist)},"\31")
end
function M.supported(s)
    local t=type(s)=="table" and s.timeline
    if not M.identity(s) or s.playbackStatus=="closed" or type(t)~="table" or t.available==false or t.sessionId~=s.id then return false end
    local maximum=finite(t.durationMs) and t.durationMs or 0
    if finite(t.maximumSeekMs) then maximum=math.max(maximum,t.maximumSeekMs) end
    return finite(t.positionMs) and t.positionMs>=0 and maximum>0 and
        finite(t.updatedAtMs) and t.updatedAtMs>0
end
function M.position(c,now)
    if not c then return 0 end
    local elapsed=c.playing and math.max(0,now-c.at) or 0
    local correction=(c.correction or 0)*math.min(1,elapsed/(c.correctOver or 1000))
    return math.max(0,math.min(c.duration,c.position+elapsed*c.rate+correction))
end
function M.sample(previous,s,now,wall)
    if not M.supported(s) then return nil end
    local t=s.timeline;local key=M.identity(s)
    local playing=s.playbackStatus=="playing"
    local reportedRate=finite(t.playbackRate) and t.playbackRate>0 and t.playbackRate or 0
    local duration=math.max(finite(t.durationMs) and t.durationMs or 0,finite(t.maximumSeekMs) and t.maximumSeekMs or 0)
    local signature=table.concat({key,tostring(t.positionMs),tostring(t.updatedAtMs),tostring(reportedRate),tostring(playing)},"\30")
    -- SMTC often republishes the same timeline anchor. It must not restart a
    -- local interpolation each time the provider delivers that same sample.
    if previous and previous.signature==signature then previous.duration=duration;return previous end
    local same=previous and previous.key==key
    local inferred=same and previous.inferredRate or nil
    local calibrationRate=same and previous.calibrationRate or nil
    local calibrationCount=same and previous.calibrationCount or 0
    if reportedRate>0 then inferred=reportedRate;calibrationRate=nil;calibrationCount=0
    elseif same and playing and previous.playing then
        local stampDelta=t.updatedAtMs-(previous.sourceStamp or t.updatedAtMs)
        local positionDelta=t.positionMs-(previous.sourcePosition or t.positionMs)
        -- Unknown rate stays unknown until two consistent real sample
        -- intervals establish it. Duplicate deliveries never calibrate it.
        if stampDelta>=100 and stampDelta<=10000 and positionDelta>0 then
            local observed=positionDelta/stampDelta
            if observed>=0.1 and observed<=4 then
                if calibrationRate and math.abs(observed-calibrationRate)<=math.max(0.02,calibrationRate*0.12) then
                    calibrationRate=(calibrationRate+observed)/2;calibrationCount=math.min(3,calibrationCount+1)
                else calibrationRate=observed;calibrationCount=1 end
                if calibrationCount>=2 then
                    inferred=inferred and math.abs(inferred-calibrationRate)<=inferred*0.12 and
                        (inferred*0.8+calibrationRate*0.2) or calibrationRate
                end
            else calibrationRate=nil;calibrationCount=0 end
        elseif positionDelta<0 then calibrationRate=nil;calibrationCount=0 end
    end
    local rate=reportedRate>0 and reportedRate or inferred or 0
    local target=t.positionMs
    if playing then target=target+math.max(0,wall-t.updatedAtMs)*rate end
    target=math.max(0,math.min(duration,target))
    local c={key=key,signature=signature,position=target,duration=duration,playing=playing,rate=rate,at=now,discontinuity=true,
        sourcePosition=t.positionMs,sourceStamp=t.updatedAtMs,inferredRate=inferred,
        calibrationRate=calibrationRate,calibrationCount=calibrationCount}
    if previous and previous.key==key then
        local predicted=M.position(previous,now);local drift=target-predicted
        if math.abs(drift)<=1000 then
            c.discontinuity=false
            if playing and rate>0 then
                c.position=predicted;c.correction=drift
                -- At most 15% speed correction: jitter can never reverse time.
                c.correctOver=math.max(1000,math.abs(drift)/(rate*0.15))
            else c.position=math.max(predicted,target) end
        end
    end
    return c
end
function M.index(lines,ms)
    local lo,hi,best=1,#lines,0
    while lo<=hi do
        local mid=math.floor((lo+hi)/2)
        if lines[mid].start<=ms then best=mid;lo=mid+1 else hi=mid-1 end
    end
    return best
end
return M
