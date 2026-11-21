#pragma once

#include "settings_ipc_channel.h"
#include "settings_ipc_codec.h"

namespace snowdesktop::settings_ipc
{
// A borrowed adapter. Registered handlers and pending replies belong to Channel.
class TypedChannel final
{
public:
    explicit TypedChannel(Channel& channel) noexcept : channel_(channel) {}

    template<class R, class... A, class F> void Bind(std::string name, F function)
    {
        channel_.BindRaw(std::move(name), [function = std::move(function)](auto bytes) {
            auto args = Unpack<std::tuple<std::decay_t<A>...>>(bytes);
            if constexpr (std::is_void_v<R>)
            {
                std::apply(function, std::move(args));
                return Bytes{};
            }
            else return Pack(std::apply(function, std::move(args)));
        });
    }
    template<class R, class... A> R Call(std::string_view name, const A&... args)
    {
        auto reply = channel_.Request(name, Pack(args...));
        if constexpr (!std::is_void_v<R>) return Unpack<R>(reply);
        else Reader(reply).Finish();
    }
    template<class R, class... A> R CallWithTimeout(DWORD timeout, std::string_view name, const A&... args)
    {
        auto reply = channel_.Request(name, Pack(args...), timeout);
        if constexpr (!std::is_void_v<R>) return Unpack<R>(reply);
        else Reader(reply).Finish();
    }
    template<class... A> void Notify(std::string_view name, const A&... args)
    {
        channel_.NotifyRaw(name, Pack(args...));
    }

private:
    Channel& channel_;
};
} // namespace snowdesktop::settings_ipc
