// SPDX-License-Identifier: MIT
#include "widget_multiplayer.h"

#if SNOWDESKTOP_HAS_STEAMWORKS
#include "widget_multiplayer_wire.h"

#include <steam/steam_api.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cwchar>
#include <limits>
#include <string>
#include <vector>

namespace snowdesktop::steam_bridge
{
namespace
{
using namespace steam_widget_multiplayer;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr char kIdentityKey[] = "snowdesktop_multiplayer";
constexpr char kRoomIdentity[] = "shared-room-v2";
constexpr std::string_view kMessagePrefix = "SDW2:";

HANDLE InheritedPipe(const wchar_t* argument)
{
    if (!argument || std::wstring_view(argument).find_first_not_of(L"0123456789") !=
        std::wstring_view::npos) return nullptr;
    const auto value = std::wcstoull(argument, nullptr, 10);
    if (!value || value >= (std::numeric_limits<std::uintptr_t>::max)()) return nullptr;
    const auto handle = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(value));
    DWORD flags = 0;
    return GetHandleInformation(handle, &flags) && (flags & HANDLE_FLAG_INHERIT) &&
        GetFileType(handle) == FILE_TYPE_PIPE ? handle : nullptr;
}

class Lobby final
{
public:
    Lobby(HANDLE input, HANDLE output)
        : input_(input), output_(output), user_(SteamUser()->GetSteamID()),
          invited_(this, &Lobby::OnInvited),
          joinRequested_(this, &Lobby::OnJoinRequested),
          launchChanged_(this, &Lobby::OnLaunchChanged),
          dataUpdated_(this, &Lobby::OnDataUpdated),
          memberUpdated_(this, &Lobby::OnMemberUpdated),
          chatMessage_(this, &Lobby::OnChatMessage)
    {}

    ~Lobby() { if (room_.IsValid()) SteamMatchmaking()->LeaveLobby(room_); }

    void Run()
    {
        Emit(Kind::Ready);
        auto nextCheck = Clock::now();
        while (running_)
        {
            Frame frame;
            const auto read = ReadFrame(input_, frame);
            if (read == ReadResult::Closed) break;
            if (read == ReadResult::Received || Clock::now() >= nextCheck)
            {
                if (!SteamUser()->BLoggedOn() || !SteamApps()->BIsSubscribed() ||
                    SteamUser()->GetSteamID() != user_) break;
                RefreshRoom();
                nextCheck = Clock::now() + 500ms;
            }
            if (read == ReadResult::Received) Handle(frame);
            SteamAPI_RunCallbacks();
            CompleteRoomOperation();
            if (read == ReadResult::Empty) Sleep(50);
        }
    }

private:
    void Emit(Kind kind, std::uint64_t request = 0, std::uint64_t value = 0,
        std::uint32_t flags = 0, std::string_view text = {})
    {
        Frame frame{request, value, kind};
        frame.flags = flags;
        if (!frame.SetText(text) || !WriteFrame(output_, frame)) running_ = false;
    }

    void Result(std::uint64_t request, std::string_view error = {})
    {
        Emit(Kind::Result, request, error.empty() ? 1 : 0, 0, error);
    }

    void Handle(const Frame& frame)
    {
        switch (frame.kind)
        {
        case Kind::OpenInvitation:
            if (frame.value) ReadInvitation(CSteamID(frame.value), true);
            else ReadLaunchInvitation();
            Result(frame.request);
            break;
        case Kind::Create:
        case Kind::Join:
            BeginRoomOperation(frame);
            break;
        case Kind::Friends:
            ListFriends(frame.request);
            break;
        case Kind::Invite:
            if (!IsWaitingHost() || std::find(friends_.begin(), friends_.end(),
                    frame.value) == friends_.end()) Result(frame.request, "inviteUnavailable");
            else Result(frame.request, SteamMatchmaking()->InviteUserToLobby(
                room_, CSteamID(frame.value)) ? "" : "inviteFailed");
            friends_.clear();
            break;
        case Kind::Send:
            Send(frame);
            break;
        case Kind::Leave:
            CloseRoom();
            Result(frame.request);
            break;
        default:
            running_ = false;
            break;
        }
    }

    void BeginRoomOperation(const Frame& frame)
    {
        if (room_.IsValid() || call_ != k_uAPICallInvalid)
            return Result(frame.request, "sessionBusy");
        creating_ = frame.kind == Kind::Create;
        if (!creating_ && (!invitation_.IsValid() || frame.value != invitation_.ConvertToUint64() ||
            Clock::now() >= invitationExpiry_))
            return Result(frame.request, "invitationExpired");

        request_ = frame.request;
        deadline_ = Clock::now() + 20s;
        call_ = creating_ ? SteamMatchmaking()->CreateLobby(k_ELobbyTypePrivate, 2) :
            SteamMatchmaking()->JoinLobby(invitation_);
        if (call_ == k_uAPICallInvalid)
        {
            invitation_.Clear();
            Result(request_, "roomUnavailable");
        }
    }

    template<typename T>
    bool ReadCall(T& result)
    {
        bool failed = false;
        return SteamUtils()->GetAPICallResult(call_, &result, sizeof(T),
            T::k_iCallback, &failed) && !failed;
    }

    void CompleteRoomOperation()
    {
        if (call_ == k_uAPICallInvalid) return;
        bool failed = false;
        if (!SteamUtils()->IsAPICallCompleted(call_, &failed))
        {
            if (Clock::now() >= deadline_)
            {
                Result(request_, "roomTimeout");
                // An unanswered Steam operation can still create/join a lobby.
                // End this API session before admitting another request.
                running_ = false;
                call_ = k_uAPICallInvalid;
            }
            return;
        }
        if (!failed && creating_)
        {
            LobbyCreated_t result{};
            if (ReadCall(result) && result.m_eResult == k_EResultOK)
            {
                room_ = CSteamID(result.m_ulSteamIDLobby);
                if (!SteamMatchmaking()->SetLobbyData(room_, kIdentityKey, kRoomIdentity))
                    CloseRoom();
            }
        }
        else if (!failed)
        {
            LobbyEnter_t result{};
            if (ReadCall(result) && result.m_EChatRoomEnterResponse == k_EChatRoomEnterResponseSuccess)
            {
                room_ = CSteamID(result.m_ulSteamIDLobby);
                if (room_ != invitation_ || std::string_view(SteamMatchmaking()->GetLobbyData(room_, kIdentityKey)) != kRoomIdentity)
                    CloseRoom();
            }
        }
        call_ = k_uAPICallInvalid;
        invitation_.Clear();
        if (!room_.IsValid()) return Result(request_, "roomUnavailable");
        owner_ = SteamMatchmaking()->GetLobbyOwner(room_);
        if (!owner_.IsValid() || (owner_ == user_) != creating_)
        {
            CloseRoom();
            return Result(request_, "roomUnavailable");
        }
        Result(request_);
        RefreshRoom();
    }

    bool IsWaitingHost() const
    {
        return room_.IsValid() && owner_ == user_ &&
            SteamMatchmaking()->GetNumLobbyMembers(room_) == 1;
    }

    void ListFriends(std::uint64_t request)
    {
        friends_.clear();
        if (!IsWaitingHost()) return Result(request, "inviteUnavailable");
        const int count = SteamFriends()->GetFriendCount(k_EFriendFlagImmediate);
        for (int i = 0; i < count && friends_.size() < kFriendLimit && running_; ++i)
        {
            const auto id = SteamFriends()->GetFriendByIndex(i, k_EFriendFlagImmediate);
            const std::string_view name(SteamFriends()->GetFriendPersonaName(id));
            const auto presence = SteamFriends()->GetFriendPersonaState(id);
            if (!id.IsValid() || presence == k_EPersonaStateOffline || !IsUtf8Text(name)) continue;
            friends_.push_back(id.ConvertToUint64());
            Emit(Kind::Friend, request, friends_.back(), 0, name);
        }
        Result(request, friends_.empty() ? "noFriendsOnline" : "");
    }

    void RefreshRoom()
    {
        if (!room_.IsValid()) return;
        const auto count = SteamMatchmaking()->GetNumLobbyMembers(room_);
        if (count < 1 || count > 2 || (count == 1 && (members_ == 2 || owner_ != user_)) ||
            SteamMatchmaking()->GetLobbyOwner(room_) != owner_ ||
            std::string_view(SteamMatchmaking()->GetLobbyData(room_, kIdentityKey)) != kRoomIdentity)
        {
            CloseRoom();
            return;
        }
        if (count == 2)
        {
            const auto first = SteamMatchmaking()->GetLobbyMemberByIndex(room_, 0);
            const auto second = SteamMatchmaking()->GetLobbyMemberByIndex(room_, 1);
            const auto peer = first == user_ ? second : first;
            if (!peer.IsValid() || peer == user_ || (first != user_ && second != user_) ||
                (peer_.IsValid() && peer_ != peer))
            {
                CloseRoom();
                return;
            }
            peer_ = peer;
        }
        if (members_ == count) return;
        members_ = count;
        Emit(Kind::Room, 0, static_cast<std::uint64_t>(count), owner_ == user_ ? 1u : 0u);
    }

    void CloseRoom()
    {
        if (!room_.IsValid()) return;
        SteamMatchmaking()->LeaveLobby(room_);
        room_.Clear();
        owner_.Clear();
        peer_.Clear();
        if (members_) Emit(Kind::Room);
        members_ = 0;
        friends_.clear();
    }

    void OnInvited(LobbyInvite_t* event)
    {
        if (CGameID(event->m_ulGameID).AppID() == SteamUtils()->GetAppID() &&
            SteamFriends()->HasFriend(CSteamID(event->m_ulSteamIDUser), k_EFriendFlagImmediate))
            ReadInvitation(CSteamID(event->m_ulSteamIDLobby));
    }

    void OnJoinRequested(GameLobbyJoinRequested_t* event)
    {
        ReadInvitation(event->m_steamIDLobby, true);
    }

    void OnLaunchChanged(NewUrlLaunchParameters_t*) { ReadLaunchInvitation(); }

    void OnMemberUpdated(LobbyChatUpdate_t* event)
    {
        if (room_.ConvertToUint64() == event->m_ulSteamIDLobby) RefreshRoom();
    }

    void ReadLaunchInvitation()
    {
        std::array<char, 96> command{};
        SteamApps()->GetLaunchCommandLine(command.data(), static_cast<int>(command.size()));
        command.back() = '\0';
        std::string_view argument(command.data());
        constexpr std::string_view prefix = "+connect_lobby ";
        if (!argument.starts_with(prefix)) return;
        argument.remove_prefix(prefix.size());
        std::uint64_t id = 0;
        const auto result = std::from_chars(argument.data(), argument.data() + argument.size(), id);
        if (result.ec == std::errc{} && result.ptr == argument.data() + argument.size())
            ReadInvitation(CSteamID(id), true);
    }

    void ReadInvitation(CSteamID id, bool explicitRequest = false)
    {
        const auto now = Clock::now();
        if (room_.IsValid() || call_ != k_uAPICallInvalid) return;
        if (!id.IsValid() || id.GetEAccountType() != k_EAccountTypeChat ||
            !(id.GetUnAccountInstance() & k_EChatInstanceFlagLobby)) return;
        if (invitation_.IsValid() && now < invitationExpiry_)
        {
            if (!explicitRequest) return;
            if (id == invitation_)
            {
                Emit(Kind::Invitation, 0, id.ConvertToUint64());
                return;
            }
        }
        if (requestedInvitation_.IsValid() && now < requestedExpiry_ &&
            (!explicitRequest || id == requestedInvitation_)) return;
        invitation_.Clear();
        requestedInvitation_ = id;
        requestedExpiry_ = now + 15s;
        if (!SteamMatchmaking()->RequestLobbyData(id)) requestedInvitation_.Clear();
    }

    void OnDataUpdated(LobbyDataUpdate_t* event)
    {
        if (!requestedInvitation_.IsValid() || event->m_ulSteamIDLobby != requestedInvitation_.ConvertToUint64() ||
            event->m_ulSteamIDMember != event->m_ulSteamIDLobby || Clock::now() >= requestedExpiry_) return;
        const auto lobby = requestedInvitation_;
        requestedInvitation_.Clear();
        if (!event->m_bSuccess || room_.IsValid() || call_ != k_uAPICallInvalid) return;
        const std::string_view identity(SteamMatchmaking()->GetLobbyData(lobby, kIdentityKey));
        if (identity != kRoomIdentity) return;
        invitation_ = lobby;
        invitationExpiry_ = Clock::now() + 2min;
        Emit(Kind::Invitation, 0, invitation_.ConvertToUint64());
    }

    static bool AdmitMessage(Clock::time_point& window, unsigned& count)
    {
        const auto now = Clock::now();
        if (now - window >= 1s) { window = now; count = 0; }
        if (count >= kMessagesPerSecond) return false;
        ++count;
        return true;
    }

    void Send(const Frame& frame)
    {
        RefreshRoom();
        if (members_ != 2) return Result(frame.request, "roomUnavailable");
        Frame checked;
        if (!IsUtf8Text(frame.Text()) || !checked.SetChannel(frame.Channel())) return Result(frame.request, "invalidArguments");
        if (!AdmitMessage(sendWindow_, sendCount_)) return Result(frame.request, "rateLimited");
        std::string message(kMessagePrefix);
        message.append(frame.Channel());
        message.push_back('\n');
        message.append(frame.Text());
        Result(frame.request, SteamMatchmaking()->SendLobbyChatMsg(room_, message.data(),
            static_cast<int>(message.size())) ? "" : "sendFailed");
    }

    void OnChatMessage(LobbyChatMsg_t* event)
    {
        if (!room_.IsValid() || event->m_ulSteamIDLobby != room_.ConvertToUint64() ||
            event->m_ulSteamIDUser == user_.ConvertToUint64()) return;
        RefreshRoom();
        if (members_ != 2) return;
        if (!peer_.IsValid() || event->m_ulSteamIDUser != peer_.ConvertToUint64() ||
            !AdmitMessage(receiveWindow_, receiveCount_)) return;
        std::array<char, kPayloadLimit + kChannelLimit + kMessagePrefix.size() + 2> text{};
        CSteamID sender;
        EChatEntryType type{};
        const int size = SteamMatchmaking()->GetLobbyChatEntry(room_,
            static_cast<int>(event->m_iChatID), &sender, text.data(), static_cast<int>(text.size()), &type);
        if (sender != peer_ ||
            type != k_EChatEntryTypeChatMsg || size <= 0 || static_cast<std::size_t>(size) >= text.size()) return;
        std::string_view message(text.data(), static_cast<std::size_t>(size));
        if (!message.starts_with(kMessagePrefix)) return;
        message.remove_prefix(kMessagePrefix.size());
        const auto delimiter = message.find('\n');
        if (delimiter == std::string_view::npos) return;
        Frame frame{0, 0, Kind::Message};
        frame.flags = sender == owner_ ? 1u : 0u;
        const auto payload = message.substr(delimiter + 1);
        if (frame.SetChannel(message.substr(0, delimiter)) && IsUtf8Text(payload) &&
            frame.SetText(payload) && !WriteFrame(output_, frame)) running_ = false;
    }

    HANDLE input_;
    HANDLE output_;
    CSteamID user_;
    CSteamID room_;
    CSteamID owner_;
    CSteamID peer_;
    CSteamID invitation_;
    CSteamID requestedInvitation_;
    CCallback<Lobby, LobbyInvite_t> invited_;
    CCallback<Lobby, GameLobbyJoinRequested_t> joinRequested_;
    CCallback<Lobby, NewUrlLaunchParameters_t> launchChanged_;
    CCallback<Lobby, LobbyDataUpdate_t> dataUpdated_;
    CCallback<Lobby, LobbyChatUpdate_t> memberUpdated_;
    CCallback<Lobby, LobbyChatMsg_t> chatMessage_;
    Clock::time_point deadline_{}, invitationExpiry_{}, requestedExpiry_{};
    Clock::time_point sendWindow_{}, receiveWindow_{};
    std::vector<std::uint64_t> friends_;
    SteamAPICall_t call_ = k_uAPICallInvalid;
    std::uint64_t request_ = 0;
    unsigned sendCount_ = 0, receiveCount_ = 0;
    int members_ = 0;
    bool creating_ = false, running_ = true;
};
}

int RunWidgetMultiplayer(int argc, wchar_t** argv)
{
    if (argc != 4) return 64;
    const HANDLE input = InheritedPipe(argv[2]);
    const HANDLE output = InheritedPipe(argv[3]);
    if (!input || !output || input == output) return 64;
    if (!SteamUser() || !SteamApps() || !SteamMatchmaking() || !SteamFriends() ||
        !SteamUser()->BLoggedOn() || !SteamApps()->BIsSubscribed()) return 4;
    Lobby(input, output).Run();
    return 0;
}
}
#endif
