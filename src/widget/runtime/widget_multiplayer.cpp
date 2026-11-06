#include "widget_engine.h"

#include "widget_multiplayer.h"

#include "common/utils.h"

#include <algorithm>
#include <utility>

namespace
{
using snowdesktop::steam_widget_multiplayer::Frame;
using snowdesktop::steam_widget_multiplayer::Kind;
using snowdesktop::steam_widget_multiplayer::Service;
using snowdesktop::widget_runtime::WidgetMultiplayer;
using namespace std::chrono_literals;

bool CanPlay(const LuaWidget& widget)
{
    return widget.valid && !widget.preview && widget.permissions.contains("multiplayer.steam");
}

bool OpensRoom(Kind operation)
{
    return operation == Kind::Create || operation == Kind::Join;
}

void StartBridge(WidgetMultiplayer& state, const std::function<void()>& wake)
{
    if (!state.service) state.service = std::make_unique<Service>(state.executable, wake);
}
}

void WidgetEngine::ConfigureSteamMultiplayer(std::filesystem::path executable,
    std::function<int()> availability, std::function<void()> unlock,
    std::function<std::optional<std::size_t>(const std::vector<std::string>&)> chooseFriend)
{
    widgetMultiplayer_ = std::make_unique<WidgetMultiplayer>();
    widgetMultiplayer_->executable = std::move(executable);
    widgetMultiplayer_->availability = std::move(availability);
    widgetMultiplayer_->unlock = std::move(unlock);
    widgetMultiplayer_->chooseFriend = std::move(chooseFriend);
}

void WidgetEngine::OpenSteamWidgetInvitation(std::uint64_t lobby)
{
    if (!widgetMultiplayer_ || !lobby) return;
    widgetMultiplayer_->launchLobby = lobby;
    widgetMultiplayer_->listenUntil = WidgetMultiplayer::Clock::now() + 2min;
    if (taskWakeCallback_) taskWakeCallback_();
}

std::string WidgetEngine::WidgetMultiplayerIdentity(const LuaWidget& widget)
{
    const auto package = GetWidgetPackage(Utf8ToWide(widget.packageId));
    if (!package || package->root != widget.packageRoot) return {};
    std::string source;
    if (package->development) source = "dev";
    else if (package->builtin) source = "builtin";
    else if (package->source.providerId == "steam-workshop" && !package->source.externalItemId.empty())
        source = "workshop:" + package->source.externalItemId;
    else if (!package->sha256.empty()) source = "package:" + package->sha256;
    else return {};
    return widget.packageId + "|" + package->manifest.version + "|" + source;
}

void WidgetEngine::StopWidgetMultiplayer(std::string_view error)
{
    if (!widgetMultiplayer_) return;
    auto& state = *widgetMultiplayer_;
    const auto task = std::exchange(state.task, 0);
    if (task && taskBroker_) (void)taskBroker_->Complete(task, false, std::string(error));
    state.service.reset();
    state.invitation.reset();
    state.friends.clear();
    state.taskOwner = state.owner = 0;
    state.members = 0;
    state.host = state.ready = false;
    state.listenUntil = {};
    state.retryAfter = WidgetMultiplayer::Clock::now() + 15s;
}

void WidgetEngine::ForgetWidgetMultiplayer(std::uint64_t owner)
{
    if (!widgetMultiplayer_) return;
    widgetMultiplayer_->lastState.erase(owner);
    if (widgetMultiplayer_->owner == owner || widgetMultiplayer_->taskOwner == owner)
        StopWidgetMultiplayer("instanceDisposed");
}

void WidgetEngine::StartWidgetMultiplayerTask(
    const snowdesktop::widget_runtime::TaskBrokerAction& action, LuaWidget& widget)
{
    const auto finish = [&](std::string error = {}) {
        const bool ok = error.empty();
        (void)taskBroker_->Complete(action.id, ok, std::move(error));
    };
    if (action.preview || !widgetMultiplayer_) return finish("providerUnavailable");
    auto& state = *widgetMultiplayer_;
    const bool leave = action.name == "multiplayer.leave";
    if (leave && state.owner != widget.runtimeToken) return finish();
    if (leave && state.task)
    {
        StopWidgetMultiplayer("canceled");
        if (taskWakeCallback_) taskWakeCallback_();
        return finish();
    }
    if (!leave)
    {
        if (!CanPlay(widget)) return finish("permissionRevoked");
        const int availability = state.availability();
        if (!availability) return finish("providerUnavailable");
        if (availability == 1)
        {
            if (action.name != "multiplayer.send" && state.unlock) state.unlock();
            return finish("advancedFeatureLocked");
        }
    }
    if (state.task) return finish("sessionBusy");
    if (state.owner && state.owner != widget.runtimeToken) return finish("sessionBusy");

    Frame frame;
    frame.request = action.id;
    if (action.name == "multiplayer.listen")
    {
        if (state.owner) return finish("sessionBusy");
        frame.kind = Kind::Listen;
        state.listenUntil = WidgetMultiplayer::Clock::now() + 2min;
        StartBridge(state, taskWakeCallback_);
    }
    else if (action.name == "multiplayer.create" || action.name == "multiplayer.join")
    {
        if (state.owner) return finish("sessionBusy");
        const auto identity = WidgetMultiplayerIdentity(widget);
        if (identity.empty() || !frame.SetText(identity)) return finish("providerUnavailable");
        frame.kind = action.name == "multiplayer.create" ? Kind::Create : Kind::Join;
        if (frame.kind == Kind::Join)
        {
            if (!state.invitation || state.invitation->Text() != identity ||
                WidgetMultiplayer::Clock::now() >= state.invitationUntil) return finish("invitationExpired");
            frame.value = state.invitation->value;
        }
        StartBridge(state, taskWakeCallback_);
    }
    else if (action.name == "multiplayer.invite")
    {
        if (state.owner != widget.runtimeToken || !state.host || state.members != 1)
            return finish("inviteUnavailable");
        frame.kind = Kind::Friends;
        state.friends.clear();
    }
    else if (action.name == "multiplayer.send")
    {
        if (state.owner != widget.runtimeToken || state.members != 2)
            return finish("roomUnavailable");
        const auto value = action.arguments.find("text");
        if (value == action.arguments.end() || !frame.SetText(value->second)) return finish("invalidArguments");
        frame.kind = Kind::Send;
    }
    else if (leave) frame.kind = Kind::Leave;
    else return finish("invalidArguments");

    if (!state.service || !state.service->Submit(frame)) return finish("steamUnavailable");
    state.task = action.id;
    state.taskOwner = widget.runtimeToken;
    state.operation = frame.kind;
    if (OpensRoom(frame.kind)) state.owner = widget.runtimeToken;
}

void WidgetEngine::PumpWidgetMultiplayer()
{
    if (!widgetMultiplayer_) return;
    auto& state = *widgetMultiplayer_;
    const auto now = WidgetMultiplayer::Clock::now();
    const auto find = [&](std::uint64_t token) {
        return std::find_if(widgets_.begin(), widgets_.end(), [&](const LuaWidget& widget) {
            return widget.runtimeToken == token && CanPlay(widget);
        });
    };
    const int availability = state.availability();
    const bool hasPlayers = std::any_of(widgets_.begin(), widgets_.end(), CanPlay);
    const auto task = state.task ? taskBroker_->Snapshot(state.task) : std::nullopt;
    if (state.task && (!task || task->cancelRequested)) StopWidgetMultiplayer("canceled");
    if (now >= state.listenUntil) state.launchLobby = 0;
    if (now >= state.invitationUntil) state.invitation.reset();
    if (state.service && (availability < 2 || !hasPlayers ||
        (state.owner && find(state.owner) == widgets_.end()) ||
        (availability != 3 && !state.owner && !state.task && now >= state.listenUntil && !state.invitation)))
        StopWidgetMultiplayer("roomClosed");

    if (availability >= 2 && hasPlayers && now >= state.retryAfter &&
        (availability == 3 || state.launchLobby || now < state.listenUntil))
        StartBridge(state, taskWakeCallback_);
    if (state.service && state.ready && (state.launchLobby || state.readSteamLaunch))
    {
        const Frame frame{0, state.launchLobby, Kind::OpenInvitation};
        if (state.service->Submit(frame))
        {
            state.launchLobby = 0;
            state.readSteamLaunch = false;
        }
    }

    const auto events = state.service ? state.service->Drain() : std::vector<Frame>{};
    std::vector<Frame> messages;
    for (const auto& event : events)
    {
        if (!state.service) break;
        switch (event.kind)
        {
        case Kind::Ready:
            state.ready = true;
            if (taskWakeCallback_) taskWakeCallback_();
            break;
        case Kind::Disconnected:
            StopWidgetMultiplayer("steamUnavailable");
            break;
        case Kind::Invitation:
            state.invitation = event;
            state.invitationUntil = now + 2min;
            break;
        case Kind::Room:
            if (!state.owner) break;
            state.members = static_cast<int>(event.value);
            state.host = event.flags != 0;
            if (!state.members) state.owner = 0;
            break;
        case Kind::Message:
            if (state.members == 2) messages.push_back(event);
            break;
        case Kind::Friend:
            if (state.task == event.request && state.operation == Kind::Friends &&
                state.friends.size() < snowdesktop::steam_widget_multiplayer::kFriendLimit)
                state.friends.push_back(event);
            break;
        case Kind::Result:
            if (!event.request || state.task != event.request) break;
            if (state.operation == Kind::Friends && event.value)
            {
                std::vector<std::string> names;
                for (const auto& entry : state.friends) names.emplace_back(entry.Text());
                const auto selected = state.chooseFriend ? state.chooseFriend(names) : std::nullopt;
                if (!state.service || state.task != event.request) break;
                const auto snapshot = taskBroker_->Snapshot(state.task);
                if (selected && *selected < state.friends.size() && snapshot && !snapshot->cancelRequested &&
                    state.availability() >= 2 && find(state.owner) != widgets_.end() &&
                    state.service->Submit(Frame{state.task, state.friends[*selected].value, Kind::Invite}))
                {
                    state.operation = Kind::Invite;
                    state.friends.clear();
                    break;
                }
                (void)taskBroker_->Complete(state.task, false, "canceled");
            }
            else
            {
                (void)taskBroker_->Complete(state.task, event.value == 1, std::string(event.Text()));
                if (!event.value && OpensRoom(state.operation)) state.owner = 0;
                if (state.operation == Kind::Join) state.invitation.reset();
            }
            state.task = state.taskOwner = 0;
            state.friends.clear();
            break;
        default: break;
        }
    }

    // Broadcast only changed state to each eligible local instance. The first
    // matching instance that accepts an invitation becomes its exclusive owner.
    std::vector<std::uint64_t> owners;
    for (const auto& widget : widgets_) if (CanPlay(widget)) owners.push_back(widget.runtimeToken);
    for (const auto token : owners)
    {
        const auto widget = find(token);
        if (widget == widgets_.end()) continue;
        const bool localHost = state.owner == token && state.host;
        std::string_view status = "idle";
        if (!availability) status = "unavailable";
        else if (availability == 1) status = "locked";
        else if (state.owner == token) status = state.members == 2 ? "connected" : state.members ? "waiting" : "connecting";
        else if (state.owner) status = "busy";
        else if (state.invitation && state.invitation->Text() == WidgetMultiplayerIdentity(*widget)) status = "invited";
        else if (state.ready && now < state.listenUntil) status = "listening";
        const std::string key = std::string(status) + (localHost ? "+" : "-");
        if (state.lastState[token] == key) continue;
        state.lastState[token] = key;
        const auto widgetId = widget->widgetId;
        (void)InvokeLifecycleEvent(*widget, "multiplayer.state", [&](lua_State* lua) {
            lua_pushlstring(lua, status.data(), status.size());
            lua_setfield(lua, -2, "state");
            lua_pushboolean(lua, localHost);
            lua_setfield(lua, -2, "host");
        });
        RuntimeInvalidateHost(widgetId);
    }
    for (const auto& message : messages)
    {
        const auto widget = find(state.owner);
        if (state.members != 2 || widget == widgets_.end()) break;
        const auto widgetId = widget->widgetId;
        (void)InvokeLifecycleEvent(*widget, "multiplayer.message", [&](lua_State* lua) {
            lua_pushlstring(lua, message.Text().data(), message.Text().size());
            lua_setfield(lua, -2, "text");
            lua_pushboolean(lua, message.flags != 0);
            lua_setfield(lua, -2, "fromHost");
        });
        RuntimeInvalidateHost(widgetId);
    }
}
