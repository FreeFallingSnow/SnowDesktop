local M={}
function M.valid(p)
    return type(p)=="table" and type(p.name)=="string" and p.name~=""
        and type(p.label)=="string" and type(p.lat)=="number" and type(p.lon)=="number"
        and p.lat>=-90 and p.lat<=90 and p.lon>=-180 and p.lon<=180
end
function M.results(payload)
    local out={}
    if type(payload)~="table" or type(payload.results)~="table" then return out end
    for _,p in ipairs(payload.results) do
        if type(p)=="table" and type(p.latitude)=="number" and type(p.longitude)=="number"
            and p.latitude>=-90 and p.latitude<=90 and p.longitude>=-180 and p.longitude<=180
            and type(p.name)=="string" and p.name~="" then
            local parts={p.name}
            if type(p.admin1)=="string" and p.admin1~="" and p.admin1~=p.name then parts[#parts+1]=p.admin1 end
            if type(p.country)=="string" and p.country~="" then parts[#parts+1]=p.country end
            out[#out+1]={name=p.name,label=table.concat(parts," · "),lat=p.latitude,lon=p.longitude}
            if #out==20 then break end
        end
    end
    return out
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
