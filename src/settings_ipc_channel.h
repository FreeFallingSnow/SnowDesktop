#pragma once

#include "settings_ipc_codec.h"
#include <windows.h>
#include <functional>
#include <memory>
#include <string_view>
#include <utility>

namespace snowdesktop::settings_ipc
{
/** Inherited anonymous pipes, with callbacks dispatched on the owning STA.
 * Synchronous calls service only this endpoint's messages while waiting. This
 * permits a host action to query the UI without dispatching arbitrary input
 * reentrantly. File pickers and confirmations use asynchronous completions.
 * The endpoint outlives a connection so durable backend completions can still
 * be marshalled to the application after the settings child exits.
 */
class Channel final
{
public:
    using Handler = std::function<Bytes(std::span<const std::byte>)>;
    Channel();
    ~Channel();
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    // Takes ownership of all three handles, including on failure.
    void Open(HANDLE readPipe, HANDLE writePipe, HANDLE peerProcess);
    void Close() noexcept;
    bool Connected() const noexcept;
    HWND Window() const noexcept;
    bool Post(std::function<void()> task);
    // Weak lifetime-safe callbacks for backend workers that can outlive UI
    // close. They stop accepting work before the owner endpoint is destroyed.
    std::function<bool(std::function<void()>)> Poster();
    std::function<HWND()> WindowProvider();
    void SetDisconnected(std::function<void()> callback);
    void BindRaw(std::string name, Handler handler);
    void Unbind(std::string_view name);
    Bytes Request(std::string_view name, Bytes arguments, DWORD timeoutMs = 30000);
    void NotifyRaw(std::string_view name, Bytes arguments);

    template<class R, class... A, class F> void Bind(std::string name, F function)
    {
        BindRaw(std::move(name), [function = std::move(function)](auto bytes) {
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
        auto reply = Request(name, Pack(args...));
        if constexpr (!std::is_void_v<R>) return Unpack<R>(reply);
        else Reader(reply).Finish();
    }
    template<class R, class... A> R CallWithTimeout(DWORD timeout, std::string_view name, const A&... args)
    {
        auto reply = Request(name, Pack(args...), timeout);
        if constexpr (!std::is_void_v<R>) return Unpack<R>(reply);
        else Reader(reply).Finish();
    }
    template<class... A> void Notify(std::string_view name, const A&... args)
    {
        NotifyRaw(name, Pack(args...));
    }
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace snowdesktop::settings_ipc
