#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace snowdesktop::widget_runtime
{
using StorageMap = std::unordered_map<std::string, std::string>;

bool IsDryLoad() noexcept;
bool IsPreviewExecution() noexcept;
StorageMap* CurrentStorageOverlay() noexcept;
bool HasStorageOverlay() noexcept;
StorageMap& ActiveStorage(StorageMap& persistentStorage) noexcept;
std::int64_t CurrentWallClockMilliseconds() noexcept;
std::int64_t CurrentMonotonicMilliseconds() noexcept;

inline constexpr std::int64_t PreviewWallClockMilliseconds =
    1'785'663'000'000LL; // 2026-08-02T09:30:00Z
inline constexpr std::int64_t PreviewMonotonicMilliseconds =
    123'456'789LL;

class StorageOverlayScope final
{
public:
    explicit StorageOverlayScope(StorageMap* storageOverlay) noexcept;
    ~StorageOverlayScope();

    StorageOverlayScope(const StorageOverlayScope&) = delete;
    StorageOverlayScope& operator=(const StorageOverlayScope&) = delete;

private:
    StorageMap* previousOverlay_ = nullptr;
};

class DryLoadScope final
{
public:
    explicit DryLoadScope(bool dryLoad = true) noexcept;
    ~DryLoadScope();

    DryLoadScope(const DryLoadScope&) = delete;
    DryLoadScope& operator=(const DryLoadScope&) = delete;

private:
    bool previousDryLoad_ = false;
};

class PreviewExecutionScope final
{
public:
    explicit PreviewExecutionScope(StorageMap* previewStorage) noexcept;
    ~PreviewExecutionScope();

    PreviewExecutionScope(const PreviewExecutionScope&) = delete;
    PreviewExecutionScope& operator=(const PreviewExecutionScope&) = delete;

private:
    StorageMap* previousOverlay_ = nullptr;
    bool previousDryLoad_ = false;
    bool previousPreviewExecution_ = false;
    bool active_ = false;
};
}
