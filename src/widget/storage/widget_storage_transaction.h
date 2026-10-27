#pragma once

#include "widget/preview/widget_preview_context.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace snowdesktop::widget_runtime
{
class WidgetStorageTransaction final
{
public:
    static constexpr std::size_t kMaximumOperations = 1024;
    static constexpr std::size_t kMaximumKeys = 256;
    static constexpr std::size_t kMaximumKeyBytes = 128;
    static constexpr std::size_t kMaximumValueBytes = 64 * 1024;
    static constexpr std::size_t kMaximumInstanceBytes = 1024 * 1024;

    WidgetStorageTransaction(
        const StorageMap& source, std::string instancePrefix);

    std::optional<std::string> Get(
        std::string_view key, std::string& error) const;
    bool Set(std::string key, std::string value,
        bool& changed, std::string& error);
    bool Remove(std::string_view key,
        bool& changed, std::string& error);
    std::optional<std::string> GetHostMetadata(
        std::string_view key, std::string& error) const;
    bool SetHostMetadata(std::string key, std::string value,
        bool& changed, std::string& error);
    bool RemoveHostMetadata(std::string_view key,
        bool& changed, std::string& error);
    bool ValidateCommit(std::string& error) const;

    bool Changed() const noexcept;
    StorageMap TakeCandidate();

private:
    bool ValidateKey(std::string_view key, std::string& error) const;
    bool ValidateHostMetadataKey(
        std::string_view key, std::string& error) const;
    bool ConsumeOperation(std::string& error);
    std::string FullKey(std::string_view key) const;
    void RememberOriginal(const std::string& fullKey,
        StorageMap::const_iterator found);

    StorageMap candidate_;
    // The candidate owns the complete snapshot. Only touched keys need a
    // second copy to detect transactions that restore their original values.
    std::unordered_map<std::string, std::optional<std::string>> originals_;
    std::string instancePrefix_;
    std::size_t operationCount_ = 0;
};
}
