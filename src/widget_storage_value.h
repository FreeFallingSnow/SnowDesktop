#pragma once

#include "widget_interaction_region.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace snowdesktop::widget_runtime
{
inline constexpr std::string_view TypedStorageMarker =
    "snowdesktop.typed.v1";
inline constexpr std::string_view TypedStorageMetadataPrefix =
    "__host.typedStorage.";
inline constexpr std::size_t MaximumTypedStorageNodes = 256;
inline constexpr std::size_t MaximumTypedStorageDepth = 8;
inline constexpr std::size_t MaximumTypedStorageStringBytes = 16 * 1024;
inline constexpr std::size_t MaximumTypedStorageEncodedBytes = 64 * 1024;

std::string TypedStorageMetadataKey(std::string_view key);

bool EncodeTypedStorageValue(const InteractionValue& value,
    std::string& output, std::string& error);
bool DecodeTypedStorageValue(std::string_view encoded,
    InteractionValue& output, std::string& error);
}
