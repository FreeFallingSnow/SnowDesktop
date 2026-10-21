#include "widget/tasks/widget_audio_playback.h"
#include <windows.h>
#include <atomic>
#include <chrono>
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
void ServiceTests()
{
    auto shared = std::make_shared<Shared>();
    WidgetAudioPlayback service([&](const auto&) { return std::make_unique<Player>(shared); });
    AudioPlaybackRequest play{1, 100, "audio.play", {}, L"C:\\fixture.wav", "filesystem:selected"};
    play.volume = .4; play.loop = true;
    const auto started = Complete(service, play);
    Check(started.ok && !started.playbackId.empty(), "play returns a playback identity");
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
p:seek(.4);assert(calls[nextId].args.positionMs==4000)
p:setLoop(true);assert(calls[nextId].name=='audio.setLoop' and calls[nextId].args.loop)
p:setVolume(.2);assert(calls[nextId].args.volume==.2)
p:clear();assert(#p.tracks==0 and #saved==0 and not p.playbackId)
p:audio({playbackId='current',status='ended',positionMs=10000,durationMs=10000});assert(#p.tracks==0)
)lua";
    if (luaL_dostring(state, script) != LUA_OK)
    { const std::string error = lua_tostring(state, -1); lua_close(state); throw std::runtime_error(error); }
    lua_close(state);
}
void RealBackend(const std::filesystem::path& path)
{
    WidgetAudioPlayback service;
    AudioPlaybackRequest request{1, 1, "audio.play", {}, path}; request.volume = 0; request.loop = true;
    const auto result = Complete(service, request);
    Check(result.ok, "real backend opens local audio");
    bool ready = false;
    Check(Wait([&] { for (auto& item : service.Drain()) {
        if (item.state.status == "error") throw std::runtime_error(item.state.error);
        if (item.state.status == "playing" && item.state.durationMs > 0) ready = true;
    } return ready; }), "real backend decodes local file and publishes duration");
    request = AudioPlaybackRequest{2, 1, "audio.pause", result.playbackId};
    Check(Complete(service, request).ok, "real pause");
    request.action = "audio.seek"; request.taskId = 3; request.positionMs = 100;
    Check(Complete(service, request).ok, "real seek");
    request.action = "audio.resume"; request.taskId = 4;
    Check(Complete(service, request).ok, "real resume");
    request.action = "audio.stop"; request.taskId = 5;
    Check(Complete(service, request).ok, "real stop");
}
}
int main(int argc, char** argv)
{
    try
    {
        if (argc == 3 && std::string_view(argv[1]) == "--real") RealBackend(std::filesystem::absolute(argv[2]));
        else { ServiceTests(); if (argc > 1) PlaylistTests(argv[1]); }
        std::cout << "widget audio playback tests passed\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
