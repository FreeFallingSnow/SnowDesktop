local clock=module.require("modules/media_clock.lua")
local M={}
function M.update(m,envelope,permission,now,wall)
    local session=envelope and envelope.available==true and envelope.value and envelope.value.session
    local identity=clock.identity(session)
    m.playbackActive=permission and identity~=nil and not envelope.stale and session.playbackStatus=="playing"
    m.mediaValid=false;m.client:setEnabled(false)
    if not permission then m.status="media_permission";m.clock=nil;m.client:setSong(nil)
    elseif clock.excluded(session) then m.status="waiting";m.clock=nil;m.client:setSong(nil);m.pendingKey=nil
    elseif not identity or session.playbackStatus=="closed" then m.status="waiting";m.pendingKey=nil
    elseif envelope.stale then m.status="waiting"
    elseif not clock.supported(session) then m.status="unsupported"
    elseif identity~=m.pendingKey then
        -- A new track needs one more provider sample before using its timeline.
        -- Keep the client state through transient gaps or metadata enrichment.
        m.pendingKey=identity;m.pendingStamp=envelope.timestamp or 0
        m.status="waiting";m.focus=nil;m.textCache=nil
    elseif (envelope.timestamp or 0)>m.pendingStamp then
        local old=m.clock;m.clock=clock.sample(old,session,now,wall)
        if m.clock~=old and m.clock.discontinuity then m.snap=true end
        m.client:setSong({key=identity,title=session.title,artist=session.artist or "",album=session.album or "",duration=m.clock.duration})
        m.mediaValid=true;m.client:setEnabled(true)
        m.client:retry(false);m.status=m.client.state
    end
end
return M
