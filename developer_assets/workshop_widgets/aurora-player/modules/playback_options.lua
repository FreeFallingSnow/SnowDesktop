local player=module.require("modules/player.lua")
local panel=module.require("modules/panel_ui.lua")
local M={}
local rates={0.5,0.75,1,1.25,1.5,2}

function M.state(session,permission)
    local result={}
    if type(session)~="table" or type(session.id)~="string" or session.id=="" or
        session.playbackStatus=="closed" then return result end
    result.id=session.id
    result.rate=player.canControl(session,permission,"canChangePlaybackRate")
    result.shuffle=player.canControl(session,permission,"canToggleShuffle")
    result.repeatMode=player.canControl(session,permission,"canChangeRepeatMode")
    if type(session.shuffle)=="boolean" then result.shuffleValue=session.shuffle end
    if session.repeatMode=="none" or session.repeatMode=="track" or session.repeatMode=="list" then
        result.repeatValue=session.repeatMode
    end
    local rate=session.playbackRate
    if type(rate)=="number" and rate>0 and rate<math.huge then result.rateValue=rate end
    return result
end

-- Bind panel actions to the rendered session; a delayed click must not control
-- a replacement session or the local player after switching sources.
function M.command(session,permission,event)
    local state=M.state(session,permission)
    if not state.id or event.value~=state.id then return nil end
    local id=event.id or ""
    if state.shuffle and (id=="system.shuffle.on" or id=="system.shuffle.off") then
        return "media.setShuffle",{sessionId=state.id,shuffle=id=="system.shuffle.on"}
    end
    local repeatMode=id:match("^system%.repeat%.([a-z]+)$")
    if state.repeatMode and (repeatMode=="none" or repeatMode=="track" or repeatMode=="list") then
        return "media.setRepeat",{sessionId=state.id,mode=repeatMode}
    end
    local rateText=(id:match("^system%.rate%.([%d_]+)$") or ""):gsub("_",".")
    local rate=tonumber(rateText)
    if state.rate then
        for _,choice in ipairs(rates) do
            if rate==choice then return "media.setRate",{sessionId=state.id,rate=rate} end
        end
    end
end

function M.pauseRequest(session,permission)
    if M.state(session,permission).id and session.playbackStatus=="playing" and
        player.canControl(session,permission,"canPause") then
        return "media.pause",{sessionId=session.id}
    end
end

function M.view(session,permission,row)
    local state=M.state(session,permission)
    local children={}
    local function label(key,text)
        return panel.text(key,text,row,{style={foreground=panel.theme().secondary}})
    end
    local function choices(key,values,selected)
        local result={}
        for _,item in ipairs(values) do
            result[#result+1]=panel.choice(key.."."..item[1],l10n.tr(item[2]),row,
                "system."..key.."."..item[1],selected==item[1],true,state.id)
        end
        return view.row({key="system."..key..".choices",width="fill",height=row*1.12,gap=row*0.2,children=result})
    end
    if state.shuffle then
        local selected
        if state.shuffleValue~=nil then selected=state.shuffleValue and "on" or "off" end
        children[#children+1]=panel.well("system.shuffle.card",row,{
            label("system.shuffle.label",l10n.tr("workshop.aurora_player.shuffle")),
            choices("shuffle",{{"off","workshop.aurora_player.shuffle_off"},{"on","workshop.aurora_player.shuffle_on"}},selected)})
    end
    if state.repeatMode then
        children[#children+1]=panel.well("system.repeat.card",row,{
            label("system.repeat.label",l10n.tr("workshop.aurora_player.repeat")),
            choices("repeat",{{"none","workshop.aurora_player.repeat_none"},{"list","workshop.aurora_player.repeat_list"},
                {"track","workshop.aurora_player.repeat_track"}},state.repeatValue)})
    end
    if state.rate then
        local rows={view.row({key="system.rate.heading",width="fill",height=row*0.7,children={
            label("system.rate.label",l10n.tr("music.rate")),
            panel.text("system.rate.value",state.rateValue and state.rateValue.."×" or "—",row,
                {width=row*2,textAlign="end",fontWeight=600})}})}
        for start=1,#rates,3 do
            local buttons={}
            for index=start,start+2 do
                local rate=rates[index]
                local suffix=tostring(rate):gsub("%.","_")
                buttons[#buttons+1]=panel.choice("system.rate."..suffix,rate.."×",row,
                    "system.rate."..suffix,state.rateValue==rate,true,state.id)
            end
            rows[#rows+1]=view.row({key="system.rate.row."..start,width="fill",height=row*1.12,gap=row*0.2,children=buttons})
        end
        children[#children+1]=panel.well("system.rate.card",row,rows)
    end
    return children
end
return M
