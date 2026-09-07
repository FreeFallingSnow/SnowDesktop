#pragma once
#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace snowdesktop
{
struct LargeIconAsset
{
    HBITMAP bitmap = nullptr;
    int width = 0, height = 0;
    std::uint32_t accent = 0x505866;
    std::string reference, source;
    ~LargeIconAsset() { if (bitmap) DeleteObject(bitmap); }
};
struct LargeIconAssetRequest
{
    std::wstring itemKey, parsingName;
    std::uint64_t generation = 0;
    int content = 0, pixels = 256;
    std::uint32_t appId = 0;
    bool portrait = false, localOnly = false, refresh = false;
    std::string language = "english", reference, lastGood;
    std::filesystem::path importPath;
};
struct LargeIconAssetResult
{
    LargeIconAssetRequest request;
    std::shared_ptr<LargeIconAsset> asset;
    std::string error;
};
class LargeIconAssets
{
public:
    explicit LargeIconAssets(std::filesystem::path directory, std::function<void()> ready);
    ~LargeIconAssets();
    void Request(LargeIconAssetRequest request);
    std::vector<LargeIconAssetResult> TakeCompleted();
    void Stop();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
