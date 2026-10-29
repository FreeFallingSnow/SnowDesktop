// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "../../steam_bridge/src/manager_workshop_item.h"

template<typename Check>
void TestManagerWorkshopItem(Check&& check)
{
    using namespace snowdesktop::steam_bridge;
    using Kind = ManagerWorkshopItemKind;
    using State = ManagerWorkshopMetadataState;
    using Issue = ManagerWorkshopBindingIssue;
    const std::string uuid = "11111111-2222-3333-4444-555555555555";
    const std::string component = BuildWorkshopMetadata(uuid, "1.2.3");
    auto described = DescribeManagerWorkshopItem(component);
    check(described.kind == Kind::Widget && described.Complete() &&
            described.identity == uuid && described.version == "1.2.3" &&
            described.packageSha256.empty(),
        "metadata emitted by the real component builder identifies its UUID and version");

    // Exact metadata emitted by ExecuteThemePublishPlan and
    // ExecuteIconPackPublishPlan; neither contract has a component version.
    const std::string hash(64, 'a');
    auto theme = JsonValue::Object();
    theme.object["format"] = JsonValue::String("snowdesktop-theme");
    theme.object["artifact"] = JsonValue::String("package.snowtheme");
    theme.object["themeWorkflowProtocolVersion"] = JsonValue::Number(1);
    theme.object["themeId"] = JsonValue::String("theme/example");
    theme.object["packageSha256"] = JsonValue::String(hash);
    auto icons = JsonValue::Object();
    icons.object["format"] = JsonValue::String("snowdesktop-icon-pack");
    icons.object["artifact"] = JsonValue::String("package.snowicons");
    icons.object["iconPackWorkflowProtocolVersion"] = JsonValue::Number(1);
    icons.object["packageId"] = JsonValue::String("group-" + std::string(32, 'b'));
    icons.object["packageSha256"] = JsonValue::String(hash);
    const std::string themeMetadata = WriteJson(theme, -1);
    const std::string iconMetadata = WriteJson(icons, -1);
    described = DescribeManagerWorkshopItem(themeMetadata);
    check(described.kind == Kind::Theme && described.Complete() &&
            described.identity == "theme/example" && described.version.empty() &&
            described.packageSha256 == hash,
        "published theme metadata preserves its theme identity without inventing a widget version");
    described = DescribeManagerWorkshopItem(iconMetadata);
    check(described.kind == Kind::IconPack && described.Complete() &&
            described.identity == "group-" + std::string(32, 'b') &&
            described.version.empty(),
        "published icon-pack metadata preserves its group identity");

    check(DescribeManagerWorkshopItem("").state == State::Missing &&
            DescribeManagerWorkshopItem("{").state == State::Malformed &&
            DescribeManagerWorkshopItem("[]").state == State::Malformed &&
            DescribeManagerWorkshopItem(R"({"format":"other"})").state ==
                State::UnknownFormat &&
            DescribeManagerWorkshopItem(R"({"format":"snowdesktop-theme","format":"snowdesktop-widget"})").state ==
                State::Malformed,
        "empty, malformed, foreign and duplicate-key metadata cannot become managed items");
    auto incompatible = theme;
    incompatible.object["themeWorkflowProtocolVersion"] = JsonValue::Number(2);
    described = DescribeManagerWorkshopItem(WriteJson(incompatible, -1));
    check(described.kind == Kind::Theme && described.state == State::UnsupportedProtocol,
        "a future theme protocol retains its display type but disallows management");
    incompatible = icons;
    incompatible.object["artifact"] = JsonValue::String("package.snowwidget");
    check(DescribeManagerWorkshopItem(WriteJson(incompatible, -1)).state ==
            State::Incomplete,
        "a format and artifact mismatch cannot become a complete identity");
    incompatible = icons;
    incompatible.object["packageId"] = JsonValue::String(uuid);
    check(!DescribeManagerWorkshopItem(WriteJson(incompatible, -1)).Complete(),
        "an icon-pack identity cannot be substituted with a component UUID");
    incompatible = theme;
    incompatible.object.erase("packageSha256");
    check(!DescribeManagerWorkshopItem(WriteJson(incompatible, -1)).Complete(),
        "incomplete theme metadata remains readable without becoming actionable");

    WorkshopProject target;
    target.localId = "local-target";
    target.packageId = uuid;
    SteamStatus status;
    status.initialized = status.loggedOn = true;
    status.appId = kSteamAppId;
    status.steamId = "42";
    PublishedItem item;
    item.publishedFileId = 123;
    item.ownerSteamId = 42;
    item.consumerAppId = kSteamAppId;
    item.metadata = component;
    item.tags = {"Icon Pack", "Theme"};
    const auto evaluate = [&](const WorkshopProject& local,
        std::string_view verified, const PublishedItem& remote,
        const SteamStatus& connection,
        std::span<const WorkshopProject> projects = {},
        bool replacement = false) {
        return EvaluateManagerWidgetBinding(local, verified, remote,
            connection, projects, replacement);
    };
    check(evaluate(target, uuid, item, status).Allowed(),
        "binding uses a verified matching component identity independently of misleading tags");
    for (const auto& metadata : {themeMetadata, iconMetadata, std::string{},
            std::string(R"({"format":"other"})")})
    {
        auto remote = item;
        remote.metadata = metadata;
        check(evaluate(target, uuid, remote, status).issue == Issue::UnsupportedItem,
            "theme, icon-pack, missing and foreign metadata never enter component associations");
    }
    auto remote = item;
    remote.publishedFileId = 0;
    check(evaluate(target, uuid, remote, status).issue == Issue::InvalidItem,
        "a missing remote ID cannot bind");
    remote = item;
    remote.banned = true;
    check(evaluate(target, uuid, remote, status).issue == Issue::Banned,
        "a banned owned item cannot bind");
    remote = item;
    remote.ownerSteamId = 43;
    check(evaluate(target, uuid, remote, status).issue == Issue::DifferentOwner,
        "a different owner cannot bind");
    remote = item;
    remote.consumerAppId = kSteamAppId + 1;
    check(evaluate(target, uuid, remote, status).issue == Issue::DifferentApplication,
        "a different application cannot bind");
    auto offline = status;
    offline.loggedOn = false;
    check(evaluate(target, uuid, item, offline).issue == Issue::ConnectionUnavailable,
        "binding requires a current connected Steam identity");
    offline = status;
    offline.steamId = "42junk";
    check(evaluate(target, uuid, item, offline).issue == Issue::ConnectionUnavailable,
        "partially parsed account identifiers cannot authorize a binding");
    auto unchecked = target;
    unchecked.packageId.clear();
    check(evaluate(unchecked, uuid, item, status).issue == Issue::LocalIdentityUnverified &&
            evaluate(target, "", item, status).issue == Issue::LocalIdentityUnverified,
        "a cached or uninspected directory cannot bind without a verified local UUID");
    check(evaluate(target, "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", item, status).issue ==
            Issue::IdentityMismatch,
        "a source whose UUID changed cannot bind through its stale cache");
    auto occupied = target;
    occupied.localId = "another-local-project";
    occupied.publishedFileId = item.publishedFileId;
    std::vector<WorkshopProject> projects{occupied};
    check(evaluate(target, uuid, item, status, projects).issue == Issue::AlreadyUsed,
        "one published item cannot be silently assigned to multiple local projects");
    auto existing = target;
    existing.publishedFileId = 456;
    check(evaluate(existing, uuid, item, status).RequiresReplacementConfirmation() &&
            evaluate(existing, uuid, item, status, {}, true).Allowed(),
        "replacing a different publication requires explicit confirmation");
    check(evaluate(existing, uuid, item, status, projects, true).issue == Issue::AlreadyUsed,
        "replacement confirmation cannot bypass duplicate-association checks");
    existing.publishedFileId = item.publishedFileId;
    check(evaluate(existing, uuid, item, status).Allowed(),
        "reaffirming the current valid association is idempotent");
    check(!target.publishedFileId && occupied.publishedFileId == 123 &&
            existing.publishedFileId == 123,
        "classification and binding decisions never modify projects");

    std::string error;
    check(!CanBindWorkshopItem(unchecked, component, 42, 42,
            kSteamAppId, kSteamAppId, error),
        "the shared component association boundary also rejects an empty cached UUID");
    JsonValue componentJson;
    ParseJson(component, componentJson, error);
    componentJson.object["protocolVersion"] = JsonValue::Number(2);
    check(!ParseWorkshopMetadata(WriteJson(componentJson, -1), error),
        "the shared component parser rejects unsupported protocols");
    componentJson.object.erase("protocolVersion");
    check(!ParseWorkshopMetadata(WriteJson(componentJson, -1), error),
        "the shared component parser rejects associations with missing protocols");
}
