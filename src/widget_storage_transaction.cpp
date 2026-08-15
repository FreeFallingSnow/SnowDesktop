#include "widget_storage_transaction.h"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace snowdesktop::widget_runtime
{
namespace
{
bool IsValidUtf8(std::string_view value) noexcept
{
    std::size_t index = 0;
    while (index < value.size())
    {
        const auto lead = static_cast<unsigned char>(value[index]);
        std::size_t length = 0;
        std::uint32_t codePoint = 0;
        if (lead <= 0x7f)
        {
            length = 1;
            codePoint = lead;
        }
        else if (lead >= 0xc2 && lead <= 0xdf)
        {
            length = 2;
            codePoint = lead & 0x1f;
        }
        else if (lead >= 0xe0 && lead <= 0xef)
        {
            length = 3;
            codePoint = lead & 0x0f;
        }
        else if (lead >= 0xf0 && lead <= 0xf4)
        {
            length = 4;
            codePoint = lead & 0x07;
        }
        else
            return false;
        if (index + length > value.size()) return false;
        for (std::size_t offset = 1; offset < length; ++offset)
        {
            const auto continuation =
                static_cast<unsigned char>(value[index + offset]);
            if ((continuation & 0xc0) != 0x80) return false;
            codePoint = (codePoint << 6) | (continuation & 0x3f);
        }
        if ((length == 2 && codePoint < 0x80) ||
            (length == 3 && codePoint < 0x800) ||
            (length == 4 && codePoint < 0x10000) ||
            codePoint > 0x10ffff ||
            (codePoint >= 0xd800 && codePoint <= 0xdfff))
            return false;
        index += length;
    }
    return true;
}
}

WidgetStorageTransaction::WidgetStorageTransaction(
    const StorageMap& source, std::string instancePrefix)
    : original_(source), candidate_(source),
      instancePrefix_(std::move(instancePrefix))
{
}

bool WidgetStorageTransaction::ValidateKey(
    std::string_view key, std::string& error) const
{
    if (key.empty() || key.size() > kMaximumKeyBytes ||
        key.find('\0') != std::string_view::npos || !IsValidUtf8(key))
    {
        error = "key must contain 1 to 128 bytes of valid UTF-8";
        return false;
    }
    return true;
}

bool WidgetStorageTransaction::ConsumeOperation(std::string& error)
{
    if (operationCount_ >= kMaximumOperations)
    {
        error = "transaction exceeds the 1024 operation limit";
        return false;
    }
    ++operationCount_;
    return true;
}

std::string WidgetStorageTransaction::FullKey(std::string_view key) const
{
    std::string result = instancePrefix_;
    result.push_back('.');
    result.append(key);
    return result;
}

std::optional<std::string> WidgetStorageTransaction::Get(
    std::string_view key, std::string& error) const
{
    error.clear();
    if (!ValidateKey(key, error)) return std::nullopt;
    const auto found = candidate_.find(FullKey(key));
    if (found == candidate_.end()) return std::nullopt;
    return found->second;
}

bool WidgetStorageTransaction::Set(std::string key, std::string value,
    bool& changed, std::string& error)
{
    changed = false;
    error.clear();
    if (!ConsumeOperation(error) || !ValidateKey(key, error)) return false;
    if (value.size() > kMaximumValueBytes ||
        (!value.empty() && !IsValidUtf8(value)))
    {
        error = "value must be at most 65536 bytes of valid UTF-8";
        return false;
    }
    const std::string fullKey = FullKey(key);
    const auto found = candidate_.find(fullKey);
    if (found != candidate_.end() && found->second == value) return true;
    candidate_[fullKey] = std::move(value);
    changed = true;
    return true;
}

bool WidgetStorageTransaction::Remove(std::string_view key,
    bool& changed, std::string& error)
{
    changed = false;
    error.clear();
    if (!ConsumeOperation(error) || !ValidateKey(key, error)) return false;
    changed = candidate_.erase(FullKey(key)) != 0;
    return true;
}

bool WidgetStorageTransaction::ValidateCommit(std::string& error) const
{
    error.clear();
    const std::string prefix = instancePrefix_ + ".";
    std::size_t keys = 0;
    std::size_t bytes = 0;
    for (const auto& [key, value] : candidate_)
    {
        if (!key.starts_with(prefix)) continue;
        const std::string_view relative(key.data() + prefix.size(),
            key.size() - prefix.size());
        if (relative == "__host" || relative.starts_with("__host."))
            continue;
        ++keys;
        bytes += key.size() + value.size();
        if (keys > kMaximumKeys || bytes > kMaximumInstanceBytes)
        {
            error = "widget storage quota exceeded";
            return false;
        }
    }
    return true;
}

bool WidgetStorageTransaction::Changed() const noexcept
{
    return original_ != candidate_;
}

StorageMap WidgetStorageTransaction::TakeCandidate()
{
    return std::move(candidate_);
}
}
