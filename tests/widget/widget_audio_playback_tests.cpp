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
    Check(Wait([&] { for (auto& item : service.Drain()) if (item.taskId == id) result = item; return result.taskId != 0; }), "completion deadline");
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
    Check(luaL_dofile(state, modulePath.string().c_str()) == LUA_OK, "load playlist for entry integration");
    lua_setglobal(state, "moduleUnderTest");
    const char* fixture = R"lua(
local values, inTransaction, definition, taskId = {}, false, nil, 0
module={require=function() return moduleUnderTest end}
l10n={tr=function(key) return key end}
storage={get=function(key) assert(not inTransaction,'global storage read inside transaction');return values[key] end}
function storage.transaction(callback)
    assert(not inTransaction);inTransaction=true
    callback({set=function(_,key,value) values[key]=value end,remove=function(_,key) values[key]=nil end})
    inTransaction=false;return true
end
function storage.set(key,value) values[key]=value end
task={start=function() taskId=taskId+1;return taskId end,cancel=function() end}
widget={define=function(d) definition=d;return d end,invalidate=function() end,openPanel=function() end}
ui={menu=function(items) return items end}
function checkEntry(d)
    local model=d.setup({preview=false})
    assert(model.player.volume==1 and model.player.playbackRate==1)
    d.event({},model,{kind='action',id='import',action='fileDrop',items={
        {handle='a',kind='file',name='a.mp3'},{handle='b',kind='file',name='b.mp3'},{handle='c',kind='file',name='c.mp3'}}})
    assert(values.playlistCount==1 and #values.playlist1==3)
    model.itemExtent=30;model.listOffset=0
    local key=model.player.tracks[1].key
    d.event({},model,{kind='action',id='drag.start',button=1,value=key,y=10})
    d.event({},model,{kind='action',id='drag.move',y=70})
    d.event({},model,{kind='action',id='drag.end',y=70})
    assert(model.player.tracks[3].name=='a.mp3' and values.playlist1[3].name=='a.mp3')
    d.event({},model,{kind='action',id='track.remove',value=key})
    local restored=d.setup({preview=false})
    assert(#restored.player.tracks==2 and restored.player.tracks[1].name=='b.mp3')
    assert(not restored.player.playingTask and not restored.player.playbackId)
    d.event({},restored,{kind='action',id='clear'})
    assert(values.playlistCount==0 and values.sourceCount==0 and values.playlist1==nil)
    values.volume=0.42
    assert(d.setup({preview=false}).player.volume==0.42)
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
        bool paused = false;
        Check(Wait([&] { for (const auto& item : service.Drain()) paused |= item.state.status == "paused"; return paused; }), "system pause reaches native player");
        Check(session.TryPlayAsync().get(), "system play accepted");
        Check(session.TryChangePlaybackPositionAsync(1000000).get(), "system seek accepted");
        const auto command = [&](const char* name, auto request) {
            std::cout << "SMTC command: " << name << std::endl;
            Check(request().get(), "system command accepted");
            bool found = false;
            Check(Wait([&] {
                for (const auto& item : service.Drain()) if (item.command.command == name)
                {
                    Check(item.owner == 1 && item.playbackId == started.playbackId, "system command targets own playback");
                    if (item.command.command == "setShuffle") Check(item.command.value == 1, "system shuffle payload");
                    if (item.command.command == "setPlaybackRate") Check(item.command.value == 1.5, "system rate payload");
                    if (item.command.command == "setRepeatMode") Check(item.command.repeatMode == "list", "system repeat payload");
                    found = true;
                }
                return found;
            }), "system command delivered to component queue");
        };
        const auto switchTrack = [&] {
            // Consume navigation by really replacing the voice, as the Lua
            // player does. Subsequent controls must target the new session.
            session = nullptr;
            const auto stopped = Complete(service, {++serial, 1, "audio.stop", started.playbackId});
            Check(stopped.ok, "component consumes navigation by stopping the old track");
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
