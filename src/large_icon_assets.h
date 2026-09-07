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
    std::string reference, previewReference, source;
    ~LargeIconAsset() { if (bitmap) DeleteObject(bitmap); }
};
struct LargeIconAssetRequest
{
    std::wstring itemKey, parsingName;
    std::uint64_t generation = 0;
    std::uint64_t sourceStamp = 0;
    int sourceIconIndex = -1;
    int variant = 0; // 0: desktop content, 1/2: landscape/portrait chooser
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
struct LargeIconAssetLimits
{
    std::uint64_t decodedBytes = 128ull * 1024 * 1024;
    std::uint64_t automaticDiskBytes = 512ull * 1024 * 1024;
};
class LargeIconAssets
{
public:
    explicit LargeIconAssets(std::filesystem::path directory, std::function<void()> ready,
        std::filesystem::path steamDirectory = {}, LargeIconAssetLimits limits = {});
    ~LargeIconAssets();
    void Request(LargeIconAssetRequest request);
    void Cancel(const std::wstring& itemKey);
    void RetainReferences(std::vector<std::string> references);
    std::vector<LargeIconAssetResult> TakeCompleted();
    void Stop();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
