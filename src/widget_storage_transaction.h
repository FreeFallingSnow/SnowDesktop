#pragma once

#include "widget_preview_context.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

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
    bool ValidateCommit(std::string& error) const;

    bool Changed() const noexcept;
    StorageMap TakeCandidate();

private:
    bool ValidateKey(std::string_view key, std::string& error) const;
    bool ConsumeOperation(std::string& error);
    std::string FullKey(std::string_view key) const;

    StorageMap original_;
    StorageMap candidate_;
    std::string instancePrefix_;
    std::size_t operationCount_ = 0;
};
}
