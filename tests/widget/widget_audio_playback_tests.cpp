#include "widget/tasks/widget_audio_playback.h"
#include "widget/runtime/widget_runtime_image.h"
#include "widget/packages/widget_audio_format.h"
#include <windows.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Storage.Streams.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
extern "C" {
#include <lauxlib.h>
#include <lualib.h>
}
using namespace snowdesktop::widget_runtime;
using namespace std::chrono_literals;
namespace
{
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool traceSystemMedia = false;
void TraceAudio(const AudioPlaybackMessage& item)
{
    if (traceSystemMedia && (!item.ok || item.state.status == "ended" ||
            item.state.status == "stopped" || !item.command.command.empty()))
        std::cout << "SMTC event: " << item.playbackId << " task=" << item.taskId
            << " status=" << item.state.status << " loop=" << item.state.loop
            << " seek=" << item.state.canSeek << " error=" << item.state.error
            << " command=" << item.command.command << std::endl;
}
struct Shared
{
    std::mutex mutex;
    AudioPlaybackState state;
    std::atomic<int> live{0};
    std::vector<AudioPlaybackCommand> commands;
};
struct Player final : WidgetAudioPlayer
{
    std::shared_ptr<Shared> shared;
    explicit Player(std::shared_ptr<Shared> value) : shared(std::move(value)) { ++shared->live; }
    ~Player() override { --shared->live; }
    void Play() override { std::lock_guard lock(shared->mutex); shared->state.status = "playing"; shared->state.durationMs = 60000; }
    void Pause() override { std::lock_guard lock(shared->mutex); shared->state.status = "paused"; }
    void Seek(std::int64_t ms) override { if (ms > 60000) throw std::runtime_error("seekOutOfRange"); std::lock_guard lock(shared->mutex); shared->state.positionMs = ms; }
    void SetVolume(double value) override { std::lock_guard lock(shared->mutex); shared->state.volume = value; }
    void SetLoop(bool value) override { std::lock_guard lock(shared->mutex); shared->state.loop = value; }
    void Configure(bool read, bool smtc) override
    {
        std::lock_guard lock(shared->mutex);
        shared->state.systemMediaControls = smtc;
        shared->state.title = read ? "Fixture title" : "";
    }
    void SetPlaybackRate(double rate) override { std::lock_guard lock(shared->mutex); shared->state.playbackRate = rate; }
    void SetPlaybackMode(bool shuffle, const std::string& mode) override
    {
        std::lock_guard lock(shared->mutex);
        shared->state.shuffle = shuffle; shared->state.repeatMode = mode; shared->state.loop = mode == "track";
    }
    std::vector<AudioPlaybackCommand> DrainCommands() override
    {
        std::lock_guard lock(shared->mutex);
        std::vector<AudioPlaybackCommand> commands; commands.swap(shared->commands); return commands;
    }
    AudioPlaybackState State() override { std::lock_guard lock(shared->mutex); return shared->state; }
};
template<class Predicate> bool Wait(Predicate predicate)
{
    const auto end = std::chrono::steady_clock::now() + 5s;
    while (!predicate()) { if (std::chrono::steady_clock::now() > end) return false; std::this_thread::sleep_for(10ms); }
    return true;
}
AudioPlaybackMessage Complete(WidgetAudioPlayback& service, AudioPlaybackRequest request)
{
    const auto id = request.taskId;
    Check(service.Submit(std::move(request)), "valid request must enter audio queue");
    AudioPlaybackMessage result;
    Check(Wait([&] { for (auto& item : service.Drain()) { TraceAudio(item); if (item.taskId == id) result = item; } return result.taskId != 0; }), "completion deadline");
    return result;
}
void FormatTests()
{
    using namespace snowdesktop::widget;
    for (const auto& format : WidgetAudioFormats)
    {
        Check(FindWidgetAudioFormat(format.extension) != nullptr, "all admitted audio extensions have a MIME mapping");
        Check(WidgetAudioPlayback::SupportedFile(std::filesystem::path("C:/fixture" + std::string(format.extension))), "resource and playback format tables agree");
        Check(!WidgetAudioContentIsValid(format.extension, "not audio"), "false audio headers are rejected");
    }
    Check(WidgetAudioPlayback::SupportedFile(L"C:/UPPERCASE.FLAC"), "audio extension matching ignores ASCII case");
    Check(!WidgetAudioPlayback::SupportedFile(L"C:/song.flac.exe"), "last extension is authoritative");
    Check(WidgetAudioContentIsValid(".flac", "fLaCfixture") && !WidgetAudioContentIsValid(".wav", "fLaCfixture"), "FLAC signature cannot be mislabeled WAV");
    Check(WidgetAudioContentIsValid(".m4a", std::string("\0\0\0\x18" "ftypM4A ", 12)) &&
        !WidgetAudioContentIsValid(".m4a", "notftyp"), "MPEG-4 audio requires an ftyp box");
}
void ServiceTests()
{
    auto shared = std::make_shared<Shared>();
    WidgetAudioPlayback service([&](const auto&) { return std::make_unique<Player>(shared); });
    AudioPlaybackRequest play{1, 100, "audio.play", {}, L"C:\\fixture.wav", "filesystem:selected"};
    play.volume = .4; play.loop = true; play.readMetadata = true; play.systemMediaControls = true;
    const auto started = Complete(service, play);
    Check(started.ok && !started.playbackId.empty(), "play returns a playback identity");
    Check(shared->state.systemMediaControls && shared->state.title == "Fixture title", "opt-in metadata and SMTC reach backend");
    { std::lock_guard lock(shared->mutex); shared->commands = {{"next"}, {"previous"}, {"setShuffle", 1}, {"setRepeatMode", 0, "list"}, {"setPlaybackRate", 1.5}}; }
    std::vector<AudioPlaybackCommand> commands;
    Check(Wait([&] { for (auto& item : service.Drain()) if (!item.command.command.empty()) {
        Check(item.owner == 100 && item.playbackId == started.playbackId, "commands stay within owner and playback");
        commands.push_back(item.command);
    } return commands.size() == 5; }), "system commands survive progress coalescing");
    Check(commands[0].command == "next" && commands[1].command == "previous" && commands[2].value == 1 &&
        commands[3].repeatMode == "list" && commands[4].value == 1.5, "system command order and payload");
    auto setting = AudioPlaybackRequest{90, 100, "audio.setPlaybackRate", started.playbackId}; setting.playbackRate = 1.5;
    Check(Complete(service, setting).ok && shared->state.playbackRate == 1.5, "owned playback rate reaches backend");
    setting.taskId = 91; setting.action = "audio.setPlaybackMode"; setting.shuffle = true; setting.repeatMode = "list";
    Check(Complete(service, setting).ok && shared->state.shuffle && shared->state.repeatMode == "list" && !shared->state.loop,
        "list and shuffle state do not enable native single-track loop");
    setting.playbackRate = 0; Check(!service.Submit(setting), "invalid playback rate cannot enter worker");
    setting.playbackRate = 1; setting.repeatMode = "invalid"; Check(!service.Submit(setting), "invalid repeat mode cannot enter worker");
    auto action = AudioPlaybackRequest{2, 200, "audio.pause", started.playbackId};
    Check(!Complete(service, action).ok, "another VM cannot control this playback");
    action.owner = 100; action.taskId = 3;
    Check(Complete(service, action).ok && shared->state.status == "paused", "pause reaches owned backend");
    action.action = "audio.seek"; action.positionMs = 9000; action.taskId = 4;
    Check(Complete(service, action).ok && shared->state.positionMs == 9000, "seek uses milliseconds");
    action.positionMs = 90000; action.taskId = 5;
    const auto badSeek = Complete(service, action);
    Check(!badSeek.ok && badSeek.state.error == "seekOutOfRange", "seek errors are returned without killing playback");
    action.action = "audio.setVolume"; action.volume = .25; action.taskId = 6;
    Check(Complete(service, action).ok && shared->state.volume == .25, "per-playback volume");
    action.action = "audio.setLoop"; action.loop = false; action.taskId = 7;
    Check(Complete(service, action).ok && !shared->state.loop, "loop changes independently");
    action.action = "audio.resume"; action.taskId = 8;
    Check(Complete(service, action).ok, "resume");
    service.RevokeFile(100, "filesystem:selected");
    Check(Wait([&] { return shared->live == 0; }), "releasing a source destroys its player");
    for (std::uint64_t id = 10; id < 18; ++id) { play.taskId = id; Check(Complete(service, play).ok, "eight owned voices are allowed"); }
    play.taskId = 18;
    Check(!Complete(service, play).ok && shared->live == 8, "per-owner quota cannot leak a ninth player");
    service.ForgetOwner(100);
    Check(Wait([&] { return shared->live == 0; }), "VM retirement destroys all its players");
    Check(!service.Submit(play), "a retired VM cannot submit late work");
    Check(service.Drain().empty(), "retirement suppresses stale callbacks");
    play.owner = 300; play.taskId = 20;
    const auto next = Complete(service, play);
    Check(next.ok, "new VM works after old VM is retired");
    service.Cancel(20);
    Check(Wait([&] { return shared->live == 0; }), "canceling accepted play stops the created voice");
    play.taskId = 21; const auto terminal = Complete(service, play); Check(terminal.ok, "terminal fixture starts");
    { std::lock_guard lock(shared->mutex); shared->state.status = "ended"; }
    bool ended = false;
    Check(Wait([&] { for (auto& item : service.Drain()) if (!item.taskId && item.state.status == "ended") ended = true;
        return ended && shared->live == 0; }), "natural completion publishes terminal state and releases player");
}
void PlaylistTests(const std::string& modulePath)
{
    auto* state = luaL_newstate(); luaL_openlibs(state);
    Check(luaL_dofile(state, modulePath.c_str()) == LUA_OK, "load production playlist module");
    lua_setglobal(state, "moduleUnderTest");
    const char* script = R"lua(
local calls, saved, nextId = {}, nil, 0
local p = moduleUnderTest.new({start=function(name,args) nextId=nextId+1;calls[nextId]={name=name,args=args};return nextId end,
    cancel=function() end,save=function(value) saved=value end})
assert(moduleUnderTest.audioFile('MUSIC.MP3') and moduleUnderTest.audioFile('tone.wav'))
assert(not moduleUnderTest.audioFile('song.mp3.exe'))
for _, extension in ipairs(moduleUnderTest.extensions) do assert(moduleUnderTest.audioFile('track.'..extension)) end
assert(moduleUnderTest.audioFile('LOSSLESS.FLAC') and moduleUnderTest.audioFile('album.m4a'))
p:ingest({{handle='f1',kind='file',name='song.mp3'},{handle='dir',kind='folder',name='Album'}})
assert(#p.tracks==1 and #saved==2)
local scan=p.scanning
assert(calls[scan].args.grantHandles==false)
p:complete({taskId=scan,ok=true,value={items={{kind='file',name='second.wav'},{kind='file',name='cover.png'},
    {kind='folder',name='Nested'}},hasMore=true,nextOffset=100}})
assert(#p.tracks==2 and p.tracks[2].child=='second.wav')
scan=p.scanning
p:complete({taskId=scan,ok=true,value={items={},hasMore=false}})
p:ingest({{handle='f1',kind='file',name='song.mp3'}});assert(#p.tracks==2 and #p.sources==2)
p:play(1);local old=p.playingTask
p:play(2);local current=p.playingTask
p:complete({taskId=old,ok=true,value={playbackId='old'}})
assert(calls[nextId].name=='audio.stop' and calls[nextId].args.playbackId=='old')
p:complete({taskId=current,ok=true,value={playbackId='current'}})
p:audio({playbackId='current',status='playing',positionMs=500,durationMs=10000})
assert(calls[current].args.systemMediaControls and calls[current].args.readMetadata and calls[current].args.volume==1)
p:audio({playbackId='current',status='playing',positionMs=500,durationMs=10000,title='Tagged',artist='Artist',album='Album',artwork='cover'})
assert(p.title=='Tagged' and p.artist=='Artist' and p.artwork=='cover')
p:seek(.4);assert(calls[nextId].args.positionMs==4000)
p:move(2,1);assert(p.index==1 and p.tracks[1].name=='second.wav' and p.playbackId=='current')
p:remove(2);assert(#p.tracks==1 and p.playbackId=='current' and p.index==1)
p:command({playbackId='old',command='next'});assert(p.playbackId=='current')
p:command({playbackId='current',command='setPlaybackRate',playbackRate=1.75})
assert(p.playbackRate==1.75 and calls[nextId].name=='audio.setPlaybackRate')
p:command({playbackId='current',command='setShuffle',shuffle=true})
assert(p.shuffle and calls[nextId].name=='audio.setPlaybackMode')
p:setLoop(true);assert(calls[nextId].name=='audio.setPlaybackMode' and calls[nextId].args.repeatMode=='track')
p:setVolume(.2);assert(calls[nextId].args.volume==.2)
p:clear();assert(#p.tracks==0 and #saved==0 and not p.playbackId)
p:audio({playbackId='current',status='ended',positionMs=10000,durationMs=10000});assert(#p.tracks==0)
p:ingest({{handle='a',kind='file',name='a.mp3'},{handle='b',kind='file',name='b.mp3'},{handle='c',kind='file',name='c.mp3'}})
p:setMode(true,'none');p:play(1)
local seen={[p.index]=true}
for _=1,2 do p:step(1,true);assert(not seen[p.index]);seen[p.index]=true end
local last=p.playingTask;p:step(1,true);assert(p.playingTask==last)
p:setMode(false,'list');p:play(3);p:step(1,true);assert(p.index==1)
p:remove(1);assert(p.index==1 and #p.tracks==2 and p.tracks[1].name=='b.mp3')
local restored=moduleUnderTest.new({start=function() return 1 end,cancel=function() end,save=function() end})
restored:restore(p.sources,p.tracks);assert(#restored.tracks==2 and restored.tracks[1].name=='b.mp3' and not restored.playingTask)
)lua";
    if (luaL_dostring(state, script) != LUA_OK)
    { const std::string error = lua_tostring(state, -1); lua_close(state); throw std::runtime_error(error); }
    lua_close(state);
}
void EntryStorageTests(const std::filesystem::path& modulePath)
{
    auto* state = luaL_newstate(); luaL_openlibs(state);
    const auto packageRoot = modulePath.parent_path().parent_path().generic_u8string();
    lua_pushlstring(state, reinterpret_cast<const char*>(packageRoot.data()), packageRoot.size());
    lua_setglobal(state, "entryPackageRoot");
    const char* fixture = R"lua(
local values, inTransaction, definition, taskId = {}, false, nil, 0
local loaded, capabilities, hovered = {}, {}, ''
local calls, mediaSession, denied, rejectPause = {}, nil, false, false
module={require=function(path)
    assert(path:match('^modules/[%w_]+%.lua$'),'literal package module expected')
    if not loaded[path] then
        local chunk,err=loadfile(entryPackageRoot..'/'..path)
        assert(chunk,err);loaded[path]=chunk()
    end
    return loaded[path]
end}
l10n={tr=function(key) return key end}
storage={get=function(key) assert(not inTransaction,'global storage read inside transaction');return values[key] end}
function storage.transaction(callback)
    assert(not inTransaction);inTransaction=true
    callback({set=function(_,key,value) values[key]=value end,remove=function(_,key) values[key]=nil end})
    inTransaction=false;return true
end
function storage.set(key,value) values[key]=value end
task={start=function(name,args)
    calls[#calls+1]={name=name,args=args}
    if rejectPause and name=='media.pause' then return nil,'source refused pause' end
    taskId=taskId+1;return taskId
end,cancel=function() end}
widget={define=function(d) definition=d;return d end,invalidate=function() end,openPanel=function() end,
    hasFeature=function(name) return capabilities[name]~=false end,
    hasPermission=function(name) return not (denied and name=='media.action') end,log=function() end,
    theme=function() return {contentTheme=0} end}
data={subscribe=function(topic) return {value=function()
    if topic=='media.current' and mediaSession then return {available=true,value={session=mediaSession}} end
    if topic=='audio.output.default' then return {available=true,value={id='shared-output',name='Shared speakers'}} end
    if topic=='audio.output.volume' then return {available=true,value={endpointId='shared-output',volume=0.6,muted=false}} end
    return {available=false}
end,unsubscribe=function() end} end}
slots={binding=function() return {item=function() return nil end} end}
ui={menu=function(items) return items end,metrics=function() return {layoutRowHeight=28} end}
time={now=function() return 1000 end}
layout={width=function() return 392 end,height=function() return 240 end}
animation={requestFrame=function() return true end,cancelFrame=function() end}
schedule={every=function() return true end,cancel=function() end}
interaction={isHovered=function(key) return hovered==key end,isPressed=function() return true end}
view=setmetatable({virtualRange=function() return {offset=0} end},{__index=function(_,_) return function(spec) return spec end end})
local function contains(node,key)
    if node.key==key then return true end
    for _,child in ipairs(node.children or {}) do if contains(child,key) then return true end end
    return false
end
function checkEntry(d)
    local context={preview=false,accessibility={reducedMotion=false,highContrast=false},theme={mode='dark'}}
    local model=d.setup(context)
    local localModel=model.localPlayback
    mediaSession={id='external-1',playbackStatus='playing',controls={canPause=true}}
    localModel.panelKind='options'
    local systemPanel=d.panel(context,model)
    assert(not contains(systemPanel,'local.preferences.card') and not contains(systemPanel,'music.playlist'))
    assert(not contains(systemPanel,'system.shuffle.card') and not contains(systemPanel,'system.rate.card'))
    local function findMenu(items,id)
        for _,item in ipairs(items) do
            if item.id==id then return item end
            local child=item.children and findMenu(item.children,id)
            if child then return child end
        end
    end
    local menu=d.menu(context,model,{id='media.menu'})
    assert(not findMenu(menu,'media.shuffle.on') and not findMenu(menu,'media.rate.1'))
    mediaSession.controls.canToggleShuffle=true
    mediaSession.shuffle=false
    systemPanel=d.panel(context,model)
    assert(contains(systemPanel,'system.shuffle.card') and not contains(systemPanel,'system.repeat.card'))
    menu=d.menu(context,model,{id='media.menu'})
    assert(findMenu(menu,'media.shuffle.off').checked and not findMenu(menu,'media.shuffle.on').checked)
    local before=#calls
    d.event(context,model,{kind='action',id='system.shuffle.on',value='external-1'})
    assert(#calls==before+1 and calls[#calls].name=='media.setShuffle' and calls[#calls].args.shuffle)
    mediaSession.id='replacement'
    d.event(context,model,{kind='action',id='system.shuffle.off',value='external-1'})
    assert(#calls==before+1)
    mediaSession.controls.canToggleShuffle=false
    d.event(context,model,{kind='action',id='system.shuffle.on',value='replacement'})
    assert(#calls==before+1)
    mediaSession.controls.canChangePlaybackRate=true;mediaSession.controls.canChangeRepeatMode=true
    mediaSession.playbackRate=1.5;mediaSession.repeatMode='track'
    systemPanel=d.panel(context,model)
    assert(contains(systemPanel,'system.rate.card') and contains(systemPanel,'system.repeat.card'))
    menu=d.menu(context,model,{id='media.menu'})
    assert(findMenu(menu,'media.rate.1_5').checked and findMenu(menu,'media.repeat.track').checked)
    denied=true
    assert(not contains(d.panel(context,model),'system.rate.card'))
    denied=false
    rejectPause=true
    before=#calls
    assert(localModel.player.volume==1 and localModel.player.playbackRate==1)
    d.event(context,model,{kind='action',id='local.import',action='fileDrop',items={
        {handle='a',kind='file',name='a.mp3'},{handle='b',kind='file',name='b.mp3'},{handle='c',kind='file',name='c.mp3'}}})
    assert(values.playlistCount==1 and #values.playlist1==3)
    assert(localModel.source=='local')
    assert(calls[before+1].name=='media.pause' and calls[before+1].args.sessionId=='replacement')
    assert(contains(d.panel(context,model),'local.preferences.card') and contains(d.panel(context,model),'music.playlist'))
    before=#calls
    d.event(context,model,{kind='action',id='system.rate.1_5',value='replacement'})
    d.event(context,model,{kind='action',id='media.rate.1_5'})
    assert(localModel.player.playbackRate==1,'stale system menu must not change local preferences')
    d.event(context,model,{kind='action',id='local.source.local'})
    assert(#calls==before,'stale system actions and repeated local selection do not send tasks')
    d.event(context,model,{kind='action',id='local.source.system'})
    before=#calls
    mediaSession.playbackStatus='paused'
    d.event(context,model,{kind='action',id='local.track',value=localModel.player.tracks[1].key})
    assert(localModel.source=='local' and calls[before+1].name=='audio.play')
    assert(contains(d.view(context,model),'aurora.utilities'),'loading local playback keeps utilities visible')
    -- Reset the new playback scenario before the original idle/hover checks.
    localModel.player:stop()
    assert(not contains(d.view(context,model),'aurora.utilities'))
    hovered='aurora.surface';assert(contains(d.view(context,model),'aurora.utilities'))
    local function findNode(node,key)
        if node.key==key then return node end
        for _,child in ipairs(node.children or {}) do local found=findNode(child,key);if found then return found end end
    end
    local sound=findNode(d.view(context,model),'system.audio.open')
    assert(sound.events.wheel.id=='system.audio.volume.wheel','the exposed sound icon binds native wheel input')
    before=#calls
    d.event(context,model,{kind='action',id=sound.events.wheel.id,delta=120})
    assert(#calls==before+1 and calls[#calls].name=='audio.output.setVolume' and calls[#calls].args.coalesce)
    assert(math.abs(calls[#calls].args.volume-0.62)<0.00001 and localModel.player.volume==1,
        'the exposed icon changes shared system volume while local playback retains its own volume')
    hovered='media.toggle';assert(contains(d.view(context,model),'aurora.utilities'))
    hovered='';localModel.player.status='playing';assert(contains(d.view(context,model),'aurora.utilities'))
    localModel.player.status='loading';assert(contains(d.view(context,model),'aurora.utilities'))
    localModel.player.status='paused';assert(not contains(d.view(context,model),'aurora.utilities'))
    localModel.itemExtent=30;localModel.listOffset=0;localModel.listTop=0;localModel.listViewport=120
    local key=localModel.player.tracks[1].key
    d.event(context,model,{kind='action',id='local.drag.start',button=1,value=key,y=10})
    d.event(context,model,{kind='action',id='local.drag.move',y=80})
    d.event(context,model,{kind='action',id='local.drag.end',y=80})
    assert(localModel.player.tracks[3].name=='a.mp3' and values.playlist1[3].name=='a.mp3')
    d.event(context,model,{kind='action',id='local.track.remove',value=key})
    local restored=d.setup(context)
    assert(#restored.localPlayback.player.tracks==2 and restored.localPlayback.player.tracks[1].name=='b.mp3')
    assert(not restored.localPlayback.player.playingTask and not restored.localPlayback.player.playbackId)
    d.event(context,restored,{kind='action',id='local.clear'})
    assert(values.playlistCount==0 and values.sourceCount==0 and values.playlist1==nil)
    values.volume=0.42
    assert(d.setup(context).localPlayback.player.volume==0.42)
    capabilities['audio.playback.rate']=false
    assert(d.setup(context).localPlayback.source=='system')
end
)lua";
    Check(luaL_dostring(state, fixture) == LUA_OK, "entry storage fixture initializes");
    const auto entry = modulePath.parent_path().parent_path() / "main.lua";
    if (luaL_dofile(state, entry.string().c_str()) != LUA_OK)
    { const std::string error = lua_tostring(state, -1); lua_close(state); throw std::runtime_error(error); }
    lua_getglobal(state, "checkEntry"); lua_pushvalue(state, -2);
    if (lua_pcall(state, 1, 0, 0) != LUA_OK)
    { const std::string error = lua_tostring(state, -1); lua_close(state); throw std::runtime_error(error); }
    lua_close(state);
}
void RealBackend(const std::filesystem::path& path, bool shortFixture = true, bool optionalSeek = false)
{
    WidgetAudioPlayback service;
    AudioPlaybackRequest request{1, 1, "audio.play", {}, path}; request.volume = 0; request.loop = true;
    const auto result = Complete(service, request);
    if (!result.ok) throw std::runtime_error("real audio open: " + result.state.error);
    bool ready = false;
    Check(Wait([&] { for (auto& item : service.Drain()) {
        if (item.state.status == "error") throw std::runtime_error(item.state.error);
        if (item.state.status == "playing" && (optionalSeek || item.state.durationMs > 0)) ready = true;
    } return ready; }), "real backend decodes local file and publishes duration");
    // Both fixtures are shorter than 1.4 s. Crossing their duration must keep
    // a looping voice alive rather than turning a MediaEnded callback terminal.
    std::this_thread::sleep_for(1600ms);
    for (const auto& item : service.Drain())
        Check(item.state.status != "ended" && item.state.status != "error", "real loop remains active beyond the source duration");
    request = AudioPlaybackRequest{2, 1, "audio.pause", result.playbackId};
    Check(Complete(service, request).ok, "real pause");
    request.action = "audio.seek"; request.taskId = 3; request.positionMs = 100;
    const auto seek = Complete(service, request);
    Check(seek.ok || (optionalSeek && seek.state.error == "seekOutOfRange"), "real seek or explicit native unavailability");
    if (!seek.ok) std::cout << "native seek unavailable for this format\n";
    request.action = "audio.resume"; request.taskId = 4;
    Check(Complete(service, request).ok, "real resume");
    request.action = "audio.setVolume"; request.taskId = 5; request.volume = 0;
    Check(Complete(service, request).ok, "real per-playback volume");
    request.action = "audio.setLoop"; request.taskId = 6; request.loop = false;
    Check(Complete(service, request).ok, "real loop can be disabled");
    if (shortFixture)
    {
        bool ended = false;
        Check(Wait([&] { for (const auto& item : service.Drain()) if (item.state.status == "ended") ended = true;
            return ended; }), "real non-looping playback ends and retires its voice");
    }
    else
    {
        request.action = "audio.setPlaybackRate"; request.taskId = 9; request.playbackRate = 1.5;
        Check(Complete(service, request).ok, "selected MP3 speed change does not block");
        request.action = "audio.stop"; request.taskId = 10;
        Check(Complete(service, request).ok, "selected MP3 stop does not block");
    }
    request = AudioPlaybackRequest{7, 1, "audio.play", {}, path}; request.volume = 0;
    const auto replay = Complete(service, request); Check(replay.ok, "real replay");
    request = AudioPlaybackRequest{8, 1, "audio.stop", replay.playbackId};
    Check(Complete(service, request).ok, "real stop");

}
void RealSystemMedia(const std::filesystem::path& path)
{
    traceSystemMedia = true;
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    {
        WidgetAudioPlayback service;
        AudioPlaybackRequest play{1, 1, "audio.play", {}, path};
        std::uint64_t serial = 1;
        play.volume = 0; play.loop = true; play.readMetadata = true; play.systemMediaControls = true;
        auto started = Complete(service, play);
        if (!started.ok) throw std::runtime_error("SMTC fixture starts muted: " + started.state.error);
        bool metadata = false;
        std::int64_t duration = 0;
        Check(Wait([&] {
            for (const auto& item : service.Drain())
                if (item.state.status == "playing")
                {
                    Check(item.state.title == "SnowDesktop Audio Fixture 20261021" && item.state.artist == "Audio Regression" &&
                        item.state.album == "Local File Tests", "local title/artist/album tags");
                    Check(item.state.artwork && IsValidWidgetRuntimeImage(*item.state.artwork, 512) &&
                        item.state.artworkToken.starts_with("@audio:"), "embedded cover decoded to bounded runtime image");
                    Check(item.state.systemMediaControls, "SMTC is enabled only for opted-in playback");
                    metadata = true;
                    duration = item.state.durationMs;
                }
            return metadata;
        }), "metadata and cover event");
        if (duration > 0 && duration <= 2000)
        {
            const auto afterCycle = std::chrono::steady_clock::now() + std::chrono::milliseconds(duration + 100);
            std::int64_t previousPosition = -1;
            bool wrapped = false, playingBeyondCycle = false;
            Check(Wait([&] {
                for (const auto& item : service.Drain()) if (item.playbackId == started.playbackId)
                {
                    Check(item.state.status != "ended" && item.state.status != "error", "looping SMTC voice must not retire at natural end");
                    wrapped |= previousPosition > duration / 2 && item.state.positionMs < previousPosition;
                    previousPosition = item.state.positionMs;
                    playingBeyondCycle |= wrapped && item.state.status == "playing" && std::chrono::steady_clock::now() >= afterCycle;
                }
                return playingBeyondCycle;
            }), "looping SMTC voice stays playing with actual position wrap beyond one cycle");
        }
        using namespace winrt::Windows::Media::Control;
        const auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        GlobalSystemMediaTransportControlsSession session{nullptr};
        const auto findSession = [&] { Check(Wait([&] {
            for (const auto& candidate : manager.GetSessions())
                if (candidate.TryGetMediaPropertiesAsync().get().Title() == L"SnowDesktop Audio Fixture 20261021")
                    session = candidate;
            return session != nullptr;
        }), "own tagged fixture is discoverable in Windows SMTC"); };
        findSession();
        const auto media = session.TryGetMediaPropertiesAsync().get();
        Check(media.Artist() == L"Audio Regression" && media.AlbumTitle() == L"Local File Tests",
            "SMTC receives local artist and album tags");
        Check(media.Thumbnail() && media.Thumbnail().OpenReadAsync().get().Size() > 0,
            "SMTC receives readable cover bytes");
        Check(session.TryPauseAsync().get(), "system pause accepted");
        bool paused = false, nativeCanSeek = false;
        Check(Wait([&] { for (const auto& item : service.Drain()) { TraceAudio(item); paused |= item.state.status == "paused"; nativeCanSeek = item.state.canSeek; } return paused; }), "system pause reaches native player");
        std::cout << "SMTC seek capability: native=" << nativeCanSeek << " system="
            << session.GetPlaybackInfo().Controls().IsPlaybackPositionEnabled() << std::endl;
        if (nativeCanSeek && session.GetPlaybackInfo().Controls().IsPlaybackPositionEnabled())
        {
            // A cover-only MP3 fixture may contain just one audio frame. Never
            // send a fixed 100 ms seek beyond its real duration.
            const auto seekMs = duration > 0 ? (std::min)(std::int64_t{100}, duration / 4) : 0;
            Check(session.TryChangePlaybackPositionAsync(seekMs * 10000).get(), "system in-range seek accepted");
        }
        Check(session.TryPlayAsync().get(), "system play accepted");
        bool resumed = false;
        Check(Wait([&] {
            for (const auto& item : service.Drain()) { TraceAudio(item); resumed |= item.state.status == "playing"; }
            return resumed;
        }), "system play reaches native player before navigation");
        const auto command = [&](const char* name, auto request) {
            std::cout << "SMTC command: " << name << std::endl;
            Check(request().get(), "system command accepted");
            bool found = false;
            Check(Wait([&] {
                for (const auto& item : service.Drain()) { TraceAudio(item); if (item.command.command == name)
                {
                    Check(item.owner == 1 && item.playbackId == started.playbackId, "system command targets own playback");
                    if (item.command.command == "setShuffle") Check(item.command.value == 1, "system shuffle payload");
                    if (item.command.command == "setPlaybackRate") Check(item.command.value == 1.5, "system rate payload");
                    if (item.command.command == "setRepeatMode") Check(item.command.repeatMode == "list", "system repeat payload");
                    found = true;
                } }
                return found;
            }), "system command delivered to component queue");
        };
        const auto switchTrack = [&] {
            // Consume navigation by really replacing the voice, as the Lua
            // player does. Subsequent controls must target the new session.
            session = nullptr;
            const auto stopped = Complete(service, {++serial, 1, "audio.stop", started.playbackId});
            if (!stopped.ok) throw std::runtime_error("component navigation stop: " + stopped.state.error);
            Check(Wait([&] {
                for (const auto& candidate : manager.GetSessions())
                    if (candidate.TryGetMediaPropertiesAsync().get().Title() == L"SnowDesktop Audio Fixture 20261021") return false;
                return true;
            }), "old navigation session is retired");
            play.taskId = ++serial;
            started = Complete(service, play);
            Check(started.ok, "component starts the selected track");
            bool playing = false;
            Check(Wait([&] {
                for (const auto& item : service.Drain())
                    if (item.playbackId == started.playbackId && item.state.status == "playing") playing = true;
                return playing;
            }), "selected track reaches playing state");
            findSession();
        };
        command("next", [&] { return session.TrySkipNextAsync(); });
        switchTrack();
        command("previous", [&] { return session.TrySkipPreviousAsync(); });
        switchTrack();
        command("setShuffle", [&] { return session.TryChangeShuffleActiveAsync(true); });
        command("setRepeatMode", [&] { return session.TryChangeAutoRepeatModeAsync(winrt::Windows::Media::MediaPlaybackAutoRepeatMode::List); });
        command("setPlaybackRate", [&] { return session.TryChangePlaybackRateAsync(1.5); });
        auto control = AudioPlaybackRequest{++serial, 1, "audio.setPlaybackRate", started.playbackId}; control.playbackRate = 1.5;
        Check(Complete(service, control).ok, "component applies speed");
        control.taskId = ++serial; control.action = "audio.setPlaybackMode"; control.shuffle = true; control.repeatMode = "list";
        Check(Complete(service, control).ok, "component applies playback mode");
        bool synchronized = false;
        Check(Wait([&] {
            const auto info = session.GetPlaybackInfo();
            synchronized = info.IsShuffleActive() && info.IsShuffleActive().Value() && info.AutoRepeatMode() &&
                info.AutoRepeatMode().Value() == winrt::Windows::Media::MediaPlaybackAutoRepeatMode::List &&
                info.PlaybackRate() && std::abs(info.PlaybackRate().Value() - 1.5) < .001;
            return synchronized;
        }), "component mode and speed synchronize back to SMTC");
        service.ForgetOwner(1);
        Check(Wait([&] {
            for (const auto& candidate : manager.GetSessions())
                if (candidate.TryGetMediaPropertiesAsync().get().Title() == L"SnowDesktop Audio Fixture 20261021") return false;
            return true;
        }), "retirement removes own SMTC session");
    }
    winrt::uninit_apartment();
}
}
int main(int argc, char** argv)
{
    try
    {
        if (argc == 3 && std::string_view(argv[1]) == "--smtc") RealSystemMedia(std::filesystem::absolute(argv[2]));
        else if (argc == 3 && std::string_view(argv[1]) == "--format") RealBackend(std::filesystem::absolute(argv[2]), false, true);
        else if (argc == 3 && std::string_view(argv[1]) == "--probe") RealBackend(std::filesystem::absolute(argv[2]), false);
        else if (argc == 3 && std::string_view(argv[1]) == "--real") RealBackend(std::filesystem::absolute(argv[2]));
        else { FormatTests(); ServiceTests(); if (argc > 1) { PlaylistTests(argv[1]); EntryStorageTests(argv[1]); } }
        std::cout << "widget audio playback tests passed\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
