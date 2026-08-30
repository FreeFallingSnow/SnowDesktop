// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace snowdesktop::steam_bridge
{
struct PreviewTexture
{
    ID3D11ShaderResourceView* view = nullptr;
    int width = 0;
    int height = 0;
    bool loading = false;
    std::string error;
};

class PreviewCache
{
public:
    explicit PreviewCache(std::filesystem::path root);
    ~PreviewCache();
    PreviewCache(const PreviewCache&) = delete;
    PreviewCache& operator=(const PreviewCache&) = delete;

    void Request(std::uint64_t itemId, std::string url);
    void RequestLocal(std::uint64_t key, const std::filesystem::path& path);
    void Pump(ID3D11Device* device);
    PreviewTexture Get(std::uint64_t itemId) const;
    const std::filesystem::path& Root() const { return root_; }

private:
    struct Entry;
    std::filesystem::path root_;
    mutable std::mutex mutex_;
    std::map<std::uint64_t, std::unique_ptr<Entry>> entries_;
};
}
