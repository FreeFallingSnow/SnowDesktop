// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "bridge_json.h"
#include "steam_app_identity.h"
#include "steam_workshop_core.h"
#include "workshop_project.h"

#include <algorithm>
#include <charconv>
#include <span>
#include <string>
#include <string_view>

namespace snowdesktop::steam_bridge
{
enum class ManagerWorkshopItemKind { Unknown, Widget, Theme, IconPack };
enum class ManagerWorkshopMetadataState
{
    Missing, Malformed, UnknownFormat, UnsupportedProtocol, Incomplete, Valid,
};

struct ManagerWorkshopItemDescriptor
{
    ManagerWorkshopItemKind kind = ManagerWorkshopItemKind::Unknown;
    ManagerWorkshopMetadataState state = ManagerWorkshopMetadataState::Missing;
    std::string identity;
    std::string version;
    std::string packageSha256;

    bool Complete() const noexcept
    {
        return state == ManagerWorkshopMetadataState::Valid;
    }
};

inline bool ManagerWorkshopHex(std::string_view value, std::size_t length)
{
    return value.size() == length &&
        std::all_of(value.begin(), value.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                (c >= 'A' && c <= 'F');
        });
}

inline bool ManagerWorkshopUuid(std::string_view value)
{
    if (value.size() != 36) return false;
    for (std::size_t i = 0; i != value.size(); ++i)
    {
        if (i == 8 || i == 13 || i == 18 || i == 23)
        {
            if (value[i] != '-') return false;
        }
        else if (!ManagerWorkshopHex(value.substr(i, 1), 1)) return false;
    }
    return true;
}

inline bool ManagerWorkshopIdentityText(std::string_view value,
    std::size_t maximum)
{
    return !value.empty() && value.size() <= maximum &&
        std::none_of(value.begin(), value.end(), [](unsigned char c) {
            return c < 0x20 || c == 0x7f;
        });
}

inline ManagerWorkshopItemDescriptor DescribeManagerWorkshopItem(
    std::string_view metadata)
{
    ManagerWorkshopItemDescriptor result;
    if (metadata.empty()) return result;
    result.state = ManagerWorkshopMetadataState::Malformed;
    JsonValue root;
    std::string error;
    if (metadata.size() > 8192 || !ParseJson(metadata, root, error) ||
        !root.IsObject()) return result;
    const auto format = JsonString(root, "format");
    result.state = ManagerWorkshopMetadataState::UnknownFormat;
    if (!format) return result;
    const char* artifact = nullptr;
    const char* protocol = nullptr;
    if (*format == "snowdesktop-widget")
    {
        result.kind = ManagerWorkshopItemKind::Widget;
        artifact = "package.snowwidget";
        protocol = "protocolVersion";
    }
    else if (*format == "snowdesktop-theme")
    {
        result.kind = ManagerWorkshopItemKind::Theme;
        artifact = "package.snowtheme";
        protocol = "themeWorkflowProtocolVersion";
    }
    else if (*format == "snowdesktop-icon-pack")
    {
        result.kind = ManagerWorkshopItemKind::IconPack;
        artifact = "package.snowicons";
        protocol = "iconPackWorkflowProtocolVersion";
    }
    else return result;

    result.state = ManagerWorkshopMetadataState::Incomplete;
    if (JsonString(root, "artifact") != artifact) return result;
    const auto version = JsonUnsigned(root, protocol);
    if (!version) return result;
    if (*version != 1)
    {
        result.state = ManagerWorkshopMetadataState::UnsupportedProtocol;
        return result;
    }
    result.identity = JsonString(root,
        result.kind == ManagerWorkshopItemKind::Theme ? "themeId" : "packageId")
        .value_or("");
    if (result.kind == ManagerWorkshopItemKind::Widget)
    {
        result.version = JsonString(root, "version").value_or("");
        if (!ManagerWorkshopUuid(result.identity) ||
            !ManagerWorkshopIdentityText(result.version, 128)) return result;
    }
    else
    {
        result.packageSha256 = JsonString(root, "packageSha256").value_or("");
        if (!ManagerWorkshopHex(result.packageSha256, 64)) return result;
        if (result.kind == ManagerWorkshopItemKind::Theme)
        {
            if (!ManagerWorkshopIdentityText(result.identity, 512)) return result;
        }
        else if (result.identity.size() != 38 ||
            !result.identity.starts_with("group-") ||
            result.identity.find_first_not_of("0123456789abcdef", 6) !=
                std::string::npos) return result;
    }
    result.state = ManagerWorkshopMetadataState::Valid;
    return result;
}

enum class ManagerWorkshopBindingIssue
{
    None, InvalidItem, Banned, ConnectionUnavailable, DifferentOwner,
    DifferentApplication, UnsupportedItem, LocalIdentityUnverified,
    IdentityMismatch, AlreadyUsed, ReplacementConfirmationRequired,
};

struct ManagerWorkshopBindingDecision
{
    ManagerWorkshopBindingIssue issue = ManagerWorkshopBindingIssue::None;
    bool Allowed() const noexcept { return issue == ManagerWorkshopBindingIssue::None; }
    bool RequiresReplacementConfirmation() const noexcept
    {
        return issue == ManagerWorkshopBindingIssue::ReplacementConfirmationRequired;
    }
};

// verifiedLocalPackageId must come from a successful current source inspection,
// never just the cached target.packageId. Listing/refresh does not call this
// function to mutate associations; only an explicit binding action may do so.
inline ManagerWorkshopBindingDecision EvaluateManagerWidgetBinding(
    const WorkshopProject& target, std::string_view verifiedLocalPackageId,
    const PublishedItem& item, const SteamStatus& status,
    std::span<const WorkshopProject> projects,
    bool replacementConfirmed = false)
{
    using Issue = ManagerWorkshopBindingIssue;
    if (!item.publishedFileId) return {Issue::InvalidItem};
    if (item.banned) return {Issue::Banned};
    if (!status.initialized || !status.loggedOn)
        return {Issue::ConnectionUnavailable};
    std::uint64_t currentOwner = 0;
    const auto owner = std::from_chars(status.steamId.data(),
        status.steamId.data() + status.steamId.size(), currentOwner);
    if (!currentOwner || owner.ec != std::errc{} ||
        owner.ptr != status.steamId.data() + status.steamId.size())
        return {Issue::ConnectionUnavailable};
    if (!item.ownerSteamId || item.ownerSteamId != currentOwner)
        return {Issue::DifferentOwner};
    if (status.appId != kSteamAppId || item.consumerAppId != kSteamAppId)
        return {Issue::DifferentApplication};
    const auto descriptor = DescribeManagerWorkshopItem(item.metadata);
    if (descriptor.kind != ManagerWorkshopItemKind::Widget || !descriptor.Complete())
        return {Issue::UnsupportedItem};
    if (target.localId.empty() || !ManagerWorkshopUuid(verifiedLocalPackageId) ||
        !ManagerWorkshopUuid(target.packageId))
        return {Issue::LocalIdentityUnverified};
    if (verifiedLocalPackageId != target.packageId ||
        descriptor.identity != verifiedLocalPackageId)
        return {Issue::IdentityMismatch};
    for (const auto& project : projects)
        if (project.localId != target.localId &&
            project.publishedFileId == item.publishedFileId)
            return {Issue::AlreadyUsed};
    if (target.publishedFileId && *target.publishedFileId != item.publishedFileId &&
        !replacementConfirmed)
        return {Issue::ReplacementConfirmationRequired};
    return {};
}
}
