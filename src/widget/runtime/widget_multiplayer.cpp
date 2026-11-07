#include "widget_engine.h"

#include "widget_multiplayer.h"

#include "common/utils.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace
{
using snowdesktop::steam_widget_multiplayer::Frame;
using snowdesktop::steam_widget_multiplayer::Kind;
using snowdesktop::steam_widget_multiplayer::Service;
using snowdesktop::widget_runtime::WidgetMultiplayer;
using namespace std::chrono_literals;
namespace room = snowdesktop::steam_room;

constexpr auto kRoomRequest = (std::numeric_limits<std::uint64_t>::max)();
constexpr auto kParticipationRequest = kRoomRequest - 1;
constexpr std::string_view kParticipationChannel = "snowdesktop.room.participation";
constexpr std::string_view kParticipationFeature = "multiplayer.room.participation";

bool CanPlay(const LuaWidget& widget)
{
    return widget.valid && !widget.preview && widget.permissions.contains("multiplayer.steam");
}

bool OpensRoom(Kind operation)
{
    return operation == Kind::Create || operation == Kind::Join;
}

bool UsesRoomParticipation(const LuaWidget& widget)
{
    const auto declares = [](const auto& features) {
        return std::find(features.begin(), features.end(), kParticipationFeature) != features.end();
    };
    return declares(widget.manifest.requiredFeatures) || declares(widget.manifest.optionalFeatures);
}

void ClearParticipation(WidgetMultiplayer& state)
{
    if (state.service) state.service->DiscardQueued(kParticipationRequest);
    state.participants.clear();
    state.pendingAnnouncements.clear();
    state.announcement.reset();
    state.announceAfter = {};
}

void PumpParticipationAnnouncement(WidgetMultiplayer& state, WidgetMultiplayer::Clock::time_point now)
{
    if (state.announcement || state.pendingAnnouncements.empty() || now < state.announceAfter) return;
    const auto& [channel, active] = *state.pendingAnnouncements.begin();
    Frame frame{kParticipationRequest, 0, Kind::Send};
    if (frame.SetChannel(kParticipationChannel) && frame.SetText((active ? "+" : "-") + channel) &&
        state.service->Submit(frame))
        state.announcement.emplace(channel, active);
    state.announceAfter = now + 500ms;
}

void StartBridge(WidgetMultiplayer& state, const std::function<void()>& wake)
{
    if (!state.service) state.service = std::make_unique<Service>(state.executable, wake);
}
}

void WidgetEngine::ConfigureSteamMultiplayer(std::filesystem::path executable,
    std::function<int()> availability, std::function<void(bool)> openSettings,
    std::function<std::optional<std::size_t>(const std::vector<std::string>&)> chooseFriend,
    std::function<void()> changed)
{
    widgetMultiplayer_ = std::make_unique<WidgetMultiplayer>();
    widgetMultiplayer_->executable = std::move(executable);
    widgetMultiplayer_->availability = std::move(availability);
    widgetMultiplayer_->openSettings = std::move(openSettings);
    widgetMultiplayer_->chooseFriend = std::move(chooseFriend);
    widgetMultiplayer_->changed = std::move(changed);
}

void WidgetEngine::OpenSteamWidgetInvitation(std::uint64_t lobby)
{
    if (!widgetMultiplayer_ || !lobby) return;
    widgetMultiplayer_->launchLobby = lobby;
    widgetMultiplayer_->launchUntil = WidgetMultiplayer::Clock::now() + 2min;
    if (taskWakeCallback_) taskWakeCallback_();
}

room::Status WidgetEngine::SteamRoomStatus() const
{
    if (!widgetMultiplayer_) return {};
    const auto& state = *widgetMultiplayer_;
    const int availability = state.availability();
    room::Status status;
    status.inRoom = state.members != 0;
    status.host = status.inRoom && state.host;
    status.busy = state.operation.has_value();
    status.failure = state.failure;
    if (availability == 0) return status;
    if (availability == 1) status.phase = room::Phase::Locked;
    else if (state.members == 2) status.phase = room::Phase::Connected;
    else if (state.members == 1) status.phase = room::Phase::Waiting;
    else if (state.invitation && WidgetMultiplayer::Clock::now() < state.invitationUntil)
        status.phase = room::Phase::Invited;
    else status.phase = room::Phase::Idle;
    return status;
}

room::Status WidgetEngine::ControlSteamRoom(room::Command command)
{
    if (!widgetMultiplayer_ || command == room::Command::Query) return SteamRoomStatus();
    auto& state = *widgetMultiplayer_;
    const int availability = state.availability();
    const auto wake = [this] { if (taskWakeCallback_) taskWakeCallback_(); };
    if (command == room::Command::StopListening)
    {
        state.settingsListening = false;
        wake();
        return SteamRoomStatus();
    }
    if (command == room::Command::Listen)
    {
        state.settingsListening = true;
        wake();
        return SteamRoomStatus();
    }
    if (command == room::Command::Leave)
    {
        // Closing the owned child also leaves Steam when an operation is still
        // pending or when entitlement has changed since the UI was displayed.
        StopWidgetMultiplayer("roomClosed");
        state.launchLobby = 0;
        state.launchUntil = {};
        state.failure = room::Failure::None;
        wake();
        return SteamRoomStatus();
    }
    if (availability < 2)
    {
        if (availability == 1 && state.openSettings) state.openSettings(true);
        return SteamRoomStatus();
    }
    if (state.operation) return SteamRoomStatus();

    Frame frame;
    frame.request = kRoomRequest;
    switch (command)
    {
    case room::Command::Create:
        if (state.members) return SteamRoomStatus();
        frame.kind = Kind::Create;
        break;
    case room::Command::Join:
        if (state.members || !state.invitation ||
            WidgetMultiplayer::Clock::now() >= state.invitationUntil) return SteamRoomStatus();
        frame.kind = Kind::Join;
        frame.value = state.invitation->value;
        break;
    case room::Command::Invite:
        if (state.members != 1 || !state.host) return SteamRoomStatus();
        frame.kind = Kind::Friends;
        state.friends.clear();
        break;
    default:
        return SteamRoomStatus();
    }
    StartBridge(state, taskWakeCallback_);
    if (state.service->Submit(frame))
    {
        state.operation = frame.kind;
        state.failure = room::Failure::None;
    }
    else state.failure = room::Failure::SteamUnavailable;
    wake();
    return SteamRoomStatus();
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
    state.service.reset();
    if (taskBroker_)
        for (const auto& [request, owner] : state.sends)
            (void)taskBroker_->Complete(request, false, std::string(error));
    state.sends.clear();
    ClearParticipation(state);
    state.operation.reset();
    state.invitation.reset();
    state.friends.clear();
    state.members = 0;
    state.host = state.ready = false;
    state.retryAfter = WidgetMultiplayer::Clock::now() + 15s;
}

void WidgetEngine::CancelWidgetMultiplayerSend(std::uint64_t request)
{
    if (!widgetMultiplayer_ || !widgetMultiplayer_->sends.erase(request)) return;
    if (widgetMultiplayer_->service) widgetMultiplayer_->service->DiscardQueued(request);
}

void WidgetEngine::ForgetWidgetMultiplayer(std::uint64_t owner)
{
    if (!widgetMultiplayer_) return;
    auto& state = *widgetMultiplayer_;
    state.lastState.erase(owner);
    std::erase_if(state.participants, [&](const auto& entry) {
        if (entry.second.owner != owner) return false;
        state.pendingAnnouncements[entry.first] = false;
        return true;
    });
    std::erase_if(state.sends, [&](const auto& entry) {
        if (entry.second != owner) return false;
        if (state.service) state.service->DiscardQueued(entry.first);
        return true;
    });
}

void WidgetEngine::StartWidgetMultiplayerTask(
    const snowdesktop::widget_runtime::TaskBrokerAction& action, LuaWidget& widget)
{
    const auto finish = [&](const char* error) {
        (void)taskBroker_->Complete(action.id, false, error);
    };
    if (action.preview || !widgetMultiplayer_) return finish("providerUnavailable");
    auto& state = *widgetMultiplayer_;
    const int availability = state.availability();
    if (!availability) return finish("providerUnavailable");

    if (action.name == "multiplayer.openSettings")
    {
        if (!state.openSettings) return finish("providerUnavailable");
        state.openSettings(availability == 1);
        (void)taskBroker_->Complete(action.id, true);
        return;
    }

    if (action.name == "multiplayer.activate" || action.name == "multiplayer.deactivate")
    {
        if (!UsesRoomParticipation(widget)) return finish("providerUnavailable");
        const auto channel = WidgetMultiplayerIdentity(widget);
        if (channel.empty()) return finish("providerUnavailable");
        if (action.name == "multiplayer.activate")
        {
            if (!CanPlay(widget)) return finish("permissionRevoked");
            if (availability < 2) return finish("advancedFeatureLocked");
            if (state.members != 2) return finish("roomUnavailable");
            if (const auto pending = state.pendingAnnouncements.find(channel);
                pending != state.pendingAnnouncements.end() && !pending->second)
                return finish("sessionBusy");
            const auto [entry, inserted] = state.participants.try_emplace(
                channel, WidgetMultiplayer::Participation{widget.runtimeToken});
            if (entry->second.owner != widget.runtimeToken) return finish("sessionBusy");
            if (inserted) state.pendingAnnouncements[channel] = true;
        }
        else if (const auto entry = state.participants.find(channel);
                 entry != state.participants.end() && entry->second.owner == widget.runtimeToken)
        {
            state.participants.erase(entry);
            state.pendingAnnouncements[channel] = false;
        }
        (void)taskBroker_->Complete(action.id, true);
        if (taskWakeCallback_) taskWakeCallback_();
        return;
    }

    if (action.name != "multiplayer.send")
    {
        if (action.name != "multiplayer.leave" && !CanPlay(widget)) return finish("permissionRevoked");
        if (state.openSettings) state.openSettings(availability == 1);
        return finish(availability == 1 ? "advancedFeatureLocked" : "hostManaged");
    }

    if (!CanPlay(widget)) return finish("permissionRevoked");
    if (availability < 2) return finish("advancedFeatureLocked");
    if (state.members != 2 || !state.service) return finish("roomUnavailable");
    const auto channel = WidgetMultiplayerIdentity(widget);
    if (channel.empty()) return finish("providerUnavailable");
    if (UsesRoomParticipation(widget))
    {
        const auto entry = state.participants.find(channel);
        if (entry == state.participants.end() || entry->second.owner != widget.runtimeToken)
            return finish("sessionBusy");
        if (!entry->second.peerReady || entry->second.peerLeft) return finish("roomUnavailable");
    }
    else
    {
        const auto first = std::find_if(widgets_.begin(), widgets_.end(), [&](const LuaWidget& candidate) {
            return CanPlay(candidate) && candidate.packageId == widget.packageId &&
                WidgetMultiplayerIdentity(candidate) == channel;
        });
        if (first == widgets_.end() || first->runtimeToken != widget.runtimeToken)
            return finish("sessionBusy");
    }

    Frame frame{action.id, 0, Kind::Send};
    const auto text = action.arguments.find("text");
    if (text == action.arguments.end() || !frame.SetText(text->second) || !frame.SetChannel(channel))
        return finish("invalidArguments");
    if (!state.service->Submit(frame)) return finish("sessionBusy");
    state.sends.emplace(action.id, widget.runtimeToken);
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
    // Invalidate queued messages when a component is unloaded or loses consent.
    std::erase_if(state.sends, [&](const auto& send) {
        const auto snapshot = taskBroker_->Snapshot(send.first);
        if (snapshot && !snapshot->cancelRequested && find(send.second) != widgets_.end())
            return false;
        if (state.service) state.service->DiscardQueued(send.first);
        if (snapshot && !snapshot->cancelRequested)
            (void)taskBroker_->Complete(send.first, false, "permissionRevoked");
        return true;
    });
    if (now >= state.launchUntil) state.launchLobby = 0;
    if (now >= state.invitationUntil) state.invitation.reset();
    const bool keepListening = state.settingsListening || availability == 3 || now < state.launchUntil;
    if (state.service && (availability < 2 ||
        (!keepListening && !state.launchLobby && !state.members && !state.operation && !state.invitation)))
        StopWidgetMultiplayer("roomClosed");

    if (availability >= 2 && (state.launchLobby || (keepListening && now >= state.retryAfter)))
        StartBridge(state, taskWakeCallback_);
    if (state.service && state.ready && (state.launchLobby || state.readSteamLaunch))
    {
        if (state.service->Submit(Frame{0, state.launchLobby, Kind::OpenInvitation}))
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
            state.failure = room::Failure::None;
            if (taskWakeCallback_) taskWakeCallback_();
            break;
        case Kind::Disconnected:
            StopWidgetMultiplayer("steamUnavailable");
            state.failure = room::Failure::SteamUnavailable;
            break;
        case Kind::Invitation:
            if (!state.members && !state.operation && !state.invitation)
            {
                state.invitation = event;
                state.invitationUntil = now + 2min;
            }
            break;
        case Kind::Room:
            state.members = static_cast<int>(event.value);
            state.host = event.flags != 0;
            if (state.members) state.invitation.reset();
            if (state.members != 2)
            {
                ClearParticipation(state);
                messages.clear();
            }
            break;
        case Kind::Message:
            if (state.members == 2) messages.push_back(event);
            break;
        case Kind::Friend:
            if (event.request == kRoomRequest && state.operation == Kind::Friends &&
                state.friends.size() < snowdesktop::steam_widget_multiplayer::kFriendLimit)
                state.friends.push_back(event);
            break;
        case Kind::Result:
            if (event.request == kParticipationRequest)
            {
                if (event.value && state.announcement)
                {
                    const auto& [channel, active] = *state.announcement;
                    const auto pending = state.pendingAnnouncements.find(channel);
                    if (pending != state.pendingAnnouncements.end() && pending->second == active)
                        state.pendingAnnouncements.erase(pending);
                }
                state.announcement.reset();
                break;
            }
            if (state.sends.erase(event.request))
            {
                (void)taskBroker_->Complete(event.request, event.value == 1, std::string(event.Text()));
                break;
            }
            if (event.request != kRoomRequest || !state.operation) break;
            if (state.operation == Kind::Friends && event.value)
            {
                std::vector<std::string> names;
                for (const auto& entry : state.friends) names.emplace_back(entry.Text());
                const auto selected = state.chooseFriend ? state.chooseFriend(names) : std::nullopt;
                if (!state.service || state.operation != Kind::Friends) break;
                if (selected && *selected < state.friends.size() && state.availability() >= 2 &&
                    state.service->Submit(Frame{kRoomRequest, state.friends[*selected].value, Kind::Invite}))
                {
                    state.operation = Kind::Invite;
                    state.friends.clear();
                    break;
                }
                if (selected) state.failure = room::Failure::ActionFailed;
            }
            else if (!event.value)
                state.failure = event.Text() == "noFriendsOnline" ? room::Failure::NoFriends : room::Failure::ActionFailed;
            if (state.operation == Kind::Join) state.invitation.reset();
            state.operation.reset();
            state.friends.clear();
            break;
        default: break;
        }
    }

    if (state.ready && state.invitation && !state.members && !state.operation)
        (void)ControlSteamRoom(room::Command::Join);

    const auto status = SteamRoomStatus();
    if (std::exchange(state.published, status) != status && state.changed) state.changed();

    if (state.members == 2 && state.service)
    {
        std::erase_if(state.participants, [&](const auto& entry) {
            const auto widget = find(entry.second.owner);
            if (widget != widgets_.end() && WidgetMultiplayerIdentity(*widget) == entry.first) return false;
            state.pendingAnnouncements[entry.first] = false;
            return true;
        });
        for (const auto& message : messages)
        {
            if (message.Channel() != kParticipationChannel) continue;
            const auto text = message.Text();
            if (text.size() < 2 || (text.front() != '+' && text.front() != '-')) continue;
            const auto entry = state.participants.find(std::string(text.substr(1)));
            if (entry == state.participants.end()) continue;
            const bool ready = text.front() == '+';
            if (ready && !entry->second.peerReady) state.pendingAnnouncements[entry->first] = true;
            if (!ready && entry->second.peerReady) entry->second.peerLeft = true;
            entry->second.peerReady = ready;
        }
        PumpParticipationAnnouncement(state, now);
    }

    std::unordered_map<std::string, std::uint64_t> channels;
    std::vector<std::uint64_t> owners;
    for (const auto& widget : widgets_)
        if (CanPlay(widget))
        {
            owners.push_back(widget.runtimeToken);
            if (state.members == 2)
                if (auto channel = WidgetMultiplayerIdentity(widget); !channel.empty())
                {
                    if (!UsesRoomParticipation(widget))
                        channels.try_emplace(std::move(channel), widget.runtimeToken);
                    else if (const auto entry = state.participants.find(channel);
                             entry != state.participants.end() && entry->second.peerReady && !entry->second.peerLeft)
                        channels.try_emplace(std::move(channel), entry->second.owner);
                }
        }

    for (const auto token : owners)
    {
        const auto widget = find(token);
        if (widget == widgets_.end()) continue;
        std::string_view phase = "idle";
        if (!availability) phase = "unavailable";
        else if (availability == 1) phase = "locked";
        else if (state.members == 2)
        {
            const auto key = WidgetMultiplayerIdentity(*widget);
            if (UsesRoomParticipation(*widget))
            {
                const auto entry = state.participants.find(key);
                if (entry == state.participants.end()) phase = "roomReady";
                else if (entry->second.owner != token) phase = "busy";
                else phase = entry->second.peerReady && !entry->second.peerLeft ? "connected" : "waitingPeer";
            }
            else
            {
                const auto channel = channels.find(key);
                phase = channel != channels.end() && channel->second == token ? "connected" : "busy";
            }
        }
        else if (state.members == 1) phase = "waiting";
        else if (state.operation && OpensRoom(*state.operation)) phase = "connecting";
        else if (state.invitation) phase = "invited";
        const bool localHost = phase == "connected" && state.host;
        const std::string key = std::string(phase) + (localHost ? "+" : "-");
        if (state.lastState[token] == key) continue;
        state.lastState[token] = key;
        const auto widgetId = widget->widgetId;
        (void)InvokeLifecycleEvent(*widget, "multiplayer.state", [&](lua_State* lua) {
            lua_pushlstring(lua, phase.data(), phase.size());
            lua_setfield(lua, -2, "state");
            lua_pushboolean(lua, localHost);
            lua_setfield(lua, -2, "host");
        });
        RuntimeInvalidateHost(widgetId);
    }
    for (const auto& message : messages)
    {
        if (state.members != 2) break;
        if (message.Channel() == kParticipationChannel) continue;
        const auto channel = channels.find(std::string(message.Channel()));
        if (channel == channels.end()) continue;
        const auto widget = find(channel->second);
        if (widget == widgets_.end() || WidgetMultiplayerIdentity(*widget) != message.Channel()) continue;
        const auto widgetId = widget->widgetId;
        (void)InvokeLifecycleEvent(*widget, "multiplayer.message", [&](lua_State* lua) {
            lua_pushlstring(lua, message.Text().data(), message.Text().size());
            lua_setfield(lua, -2, "text");
            lua_pushboolean(lua, message.flags != 0);
            lua_setfield(lua, -2, "fromHost");
        });
        RuntimeInvalidateHost(widgetId);
    }
    bool peerReturned = false;
    for (auto& participation : state.participants)
        if (std::exchange(participation.second.peerLeft, false) && participation.second.peerReady)
            peerReturned = true;
    if (peerReturned && taskWakeCallback_) taskWakeCallback_();
}
