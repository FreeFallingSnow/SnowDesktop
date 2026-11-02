-- Anonymous, read-only lyric lookups. Account and playback APIs are not used.
local L=module.require("modules/lyrics.lua")
local J=module.require("modules/json.lua")
local M={}
local function get(phase,url,headers) return {phase=phase,url=url,headers=headers} end
local function textLimit(value,count)
    local result={}
    for _,code in utf8.codes(value or "") do
        if #result>=count then break end;result[#result+1]=utf8.char(code)
    end
    return table.concat(result)
end
local function query(song) return textLimit(L.title(song.title).." "..(song.artist or ""),120) end
local function names(items)
    local result={};for _,item in ipairs(type(items)=="table" and items or {}) do
        if type(item)=="table" and type(item.name)=="string" then result[#result+1]=item.name end
    end
    return table.concat(result,", ")
end
local function scored(result,song,score)
    if result then result.matchScore=L.confidence(song,score) end
    return result
end
local function amllResult(body,song,score)
    local result=type(body)=="table" and body.status==200 and type(body.data)=="table" and L.ttml(body.data.lyrics,song.duration) or nil
    return scored(result,song,score)
end
local function amllId(key,value)
    return get("amll","https://api.amll.dev/v1/lyrics/get?"..key.."="..L.encode(tostring(value)))
end
local function lyric(body,key)
    local value=type(body)=="table" and body[key]
    return type(value)=="table" and value.lyric or nil
end
local function neteaseLyrics(phase,id)
    return get(phase,"https://music.163.com/api/song/lyric/v1?id="..L.encode(tostring(id))..
        "&lv=-1&kv=-1&tv=-1&rv=-1&yv=-1&ytv=-1&yrv=-1",{Referer="https://music.163.com/"})
end
local function firstNeteaseId(body)
    local data=type(body)=="table" and type(body.data)=="table" and body.data
    for _,id in ipairs(data and type(data.ncmMusicIds)=="table" and data.ncmMusicIds or {}) do
        if type(id)=="string" and id:match("^%d+$") then return id end
    end
end
local function qqRequest(phase,method,service,param)
    return {phase=phase,url="https://u.y.qq.com/cgi-bin/musicu.fcg",method="POST",
        headers={["Content-Type"]="application/json",["User-Agent"]="okhttp/3.14.9",Referer="https://y.qq.com/"},
        body=J.encode({comm={ct=11,cv="1003006"},request={method=method,module=service,param=param}})}
end
local function qqData(body)
    return type(body)=="table" and body.code==0 and type(body.request)=="table" and body.request.code==0 and body.request.data or nil
end
local function qqLrc(song,selected)
    -- crypt=0 and qrc=0 request ordinary Base64 LRC, without encrypted QRC.
    return qqRequest("lyrics","GetPlayLyricInfo","music.musichallSong.PlayLyricInfo",{
        songID=selected.id,songName="",singerName="",albumName="",interval=math.floor(selected.duration/1000),
        crypt=0,ct=19,cv=2111,lrc_t=0,qrc=0,qrc_t=0,roma=1,roma_t=0,trans=1,trans_t=0,type=0})
end
local function lrclibResult(song,body)
    if not L.matchLrc(song,body) then return nil end
    if body.instrumental==true then return {instrumental=true} end
    return scored(L.lrc(body.syncedLyrics,song.duration),song,L.matchSong(song,{title=body.trackName,
        artist=body.artistName,album=body.albumName,duration=(tonumber(body.duration) or 0)*1000}))
end
M.sources={
    {
        id="amll",
        start=function(song)
            local url="https://api.amll.dev/v1/lyrics/search?musicName="..L.encode(textLimit(L.title(song.title),120)).."&pageSize=20"
            if song.artist and song.artist~="" then url=url.."&artistName="..L.encode(textLimit(song.artist,60)) end
            return get("search",url)
        end,
        complete=function(song,phase,body,state)
            if phase=="reference" then
                local reference=type(body)=="table" and body.code==200 and lyric(body,"lrc") or nil
                return {result=L.alignTiming(state.result,reference,song.duration)}
            end
            if phase=="lyrics" then
                local result=amllResult(body,song,state.score);local id=firstNeteaseId(body)
                if L.hasWords(result) and id then state.result=result;return neteaseLyrics("reference",id) end
                return {result=result}
            end
            local items=type(body)=="table" and body.status==200 and type(body.data)=="table" and body.data.items
            local candidate,score=L.matchAmll(song,items);state.score=score
            if candidate then return get("lyrics","https://api.amll.dev/v1/lyrics/get?id="..string.format("%.0f",candidate.id)) end
            if phase=="search" and type(items)=="table" and song.artist~="" then
                return get("title","https://api.amll.dev/v1/lyrics/search?musicName="..L.encode(textLimit(L.title(song.title),120)).."&pageSize=20")
            end
        end,
    },
    {
        id="netease",
        start=function(song)
            return {phase="search",url="https://music.163.com/api/search/get/web",method="POST",
                headers={["Content-Type"]="application/x-www-form-urlencoded",Referer="https://music.163.com/"},
                body="s="..L.encode(query(song)).."&type=1&limit=20&offset=0"}
        end,
        complete=function(song,phase,body,state)
            if phase=="search" then
                local candidates={}
                local items=type(body)=="table" and body.code==200 and type(body.result)=="table" and body.result.songs
                for i,item in ipairs(type(items)=="table" and items or {}) do
                    if i>20 then break end
                    if type(item)=="table" and type(item.id)=="number" then candidates[#candidates+1]={id=item.id,title=item.name,
                        artist=names(item.artists or item.ar),album=type(item.album or item.al)=="table" and (item.album or item.al).name,
                        duration=tonumber(item.duration or item.dt) or 0} end
                end
                state.candidate,state.score=L.bestSong(song,candidates)
                if state.candidate then return amllId("ncmMusicId",state.candidate.id) end
            elseif phase=="amll" then
                local result=amllResult(body,song,state.score)
                state.fallback=result
                return neteaseLyrics("lyrics",string.format("%.0f",state.candidate.id))
            else
                local result
                if type(body)=="table" and body.code==200 then
                    local native=L.hasWords(state.fallback) and state.fallback or L.yrc(lyric(body,"yrc"),song.duration)
                    result=native and L.alignTiming(native,lyric(body,"lrc"),song.duration) or L.lrc(lyric(body,"lrc"),song.duration)
                end
                result=scored(L.alternate(result,lyric(body,"tlyric"),lyric(body,"romalrc"),song.duration),song,state.score)
                return {result=result or state.fallback,instrumental=type(body)=="table" and body.nolyric==true}
            end
        end,
    },
    {
        id="qq",
        start=function(song)
            return qqRequest("search","DoSearchForQQMusicLite","music.search.SearchCgiService",{
                search_id="123456",remoteplace="search.android.keyboard",query=textLimit(query(song),60),search_type=7,
                num_per_page=20,page_num=1,highlight=0,nqc_flag=0,page_id=1,grp=1})
        end,
        complete=function(song,phase,body,state)
            if phase=="search" then
                local data=qqData(body);local items=type(data)=="table" and type(data.body)=="table" and data.body.item_song
                local candidates={}
                for i,item in ipairs(type(items)=="table" and items or {}) do
                    if i>20 then break end
                    if type(item)=="table" and type(item.id)=="number" and type(item.mid)=="string" then
                        candidates[#candidates+1]={id=item.id,mid=item.mid,title=item.title,artist=names(item.singer),
                            album=type(item.album)=="table" and item.album.name,duration=(tonumber(item.interval) or 0)*1000}
                    end
                end
                state.candidate,state.score=L.bestSong(song,candidates)
                if state.candidate then return amllId("qqMusicId",state.candidate.mid) end
            elseif phase=="amll" or phase=="amllNumeric" then
                local result=amllResult(body,song,state.score)
                if L.hasWords(result) then state.native=result;return qqLrc(song,state.candidate) end
                state.fallback=state.fallback or result
                if phase=="amll" then
                    local request=amllId("qqMusicId",state.candidate.id);request.phase="amllNumeric";return request
                end
                return qqLrc(song,state.candidate)
            else
                local data=phase=="cgi" and body or qqData(body)
                local value=type(data)=="table" and data.lyric
                local raw=type(value)=="string" and (value:find("%[%d+:") and value or L.base64(value))
                local result=L.lrc(raw,song.duration)
                if state.native and result then return {result=L.alignTiming(state.native,raw,song.duration)} end
                if result then return {result=scored(L.alternate(result,L.base64(data.trans),L.base64(data.roma),song.duration),song,state.score)} end
                if phase~="cgi" then return get("cgi","https://c.y.qq.com/lyric/fcgi-bin/fcg_query_lyric_new.fcg?songmid="..
                    L.encode(state.candidate.mid).."&format=json&nobase64=1",{Referer="https://y.qq.com/"}) end
                return {result=state.native or state.fallback}
            end
        end,
    },
    {
        id="kugou",
        start=function(song)
            return get("search","https://lyrics.kugou.com/search?ver=1&man=yes&client=pc&lrctxt=1&keyword="..
                L.encode(query(song)).."&duration="..string.format("%.0f",song.duration),
                {["User-Agent"]="Android14-1070-11070-201-0-Lyric-wifi"})
        end,
        complete=function(song,phase,body,state)
            if phase=="search" then
                local candidates={};local items=type(body)=="table" and body.candidates
                for i,item in ipairs(type(items)=="table" and items or {}) do
                    if i>20 then break end
                    if type(item)=="table" and type(item.id)=="string" and type(item.accesskey)=="string" then
                        candidates[#candidates+1]={id=item.id,accesskey=item.accesskey,title=item.song,artist=item.singer,duration=tonumber(item.duration)}
                    end
                end
                state.candidate,state.score=L.bestSong(song,candidates)
                if state.candidate then return get("lyrics","https://lyrics.kugou.com/download?fmt=lrc&charset=utf8&client=mobi&ver=1&id="..
                    L.encode(state.candidate.id).."&accesskey="..L.encode(state.candidate.accesskey),
                    {["User-Agent"]="Android14-1070-11070-201-0-Lyric-wifi"}) end
            else
                local value=type(body)=="table" and L.base64(body.content)
                return {result=scored(L.lrc(value,song.duration),song,state.score)}
            end
        end,
    },
    {
        id="lrclib",
        start=function(song)
            return get("get","https://lrclib.net/api/get?track_name="..L.encode(textLimit(L.title(song.title),120))..
                "&artist_name="..L.encode(textLimit(song.artist,60)).."&duration="..string.format("%.3f",song.duration/1000))
        end,
        complete=function(song,phase,body)
            if phase=="get" then
                local result=lrclibResult(song,body)
                if result then return result.instrumental and result or {result=result} end
                return get("search","https://lrclib.net/api/search?track_name="..L.encode(textLimit(L.title(song.title),120)))
            end
            local candidates={}
            for i,item in ipairs(type(body)=="table" and body or {}) do
                if i>20 then break end
                if type(item)=="table" and type(item.syncedLyrics)=="string" then candidates[#candidates+1]={title=item.trackName,
                    artist=item.artistName,album=item.albumName,duration=(tonumber(item.duration) or 0)*1000,item=item} end
            end
            local best,score=L.bestSong(song,candidates)
            return {result=scored(best and L.lrc(best.item.syncedLyrics,song.duration),song,score)}
        end,
    },
}
return M
