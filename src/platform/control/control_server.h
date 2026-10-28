#pragma once

#include "control_protocol.h"
#include <windows.h>
#include <memory>
#include <string>

namespace snowdesktop::control
{
// Stable within an interactive user/session. Does not disclose the data path.
std::wstring EndpointName();

class Server final
{
public:
    Server();
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    // Call Start/Stop on the owning UI thread. Handler runs on that thread.
    bool Start(Handler handler, std::wstring endpoint = {}, DWORD timeoutMilliseconds = 5000);
    void Stop() noexcept;
    [[nodiscard]] bool Running() const noexcept;
    [[nodiscard]] DWORD LastError() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
