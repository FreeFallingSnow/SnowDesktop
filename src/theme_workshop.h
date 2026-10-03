#pragma once
#include "theme_library.h"
#include "../steam_bridge/src/theme_workshop_publish.h"
#include <atomic>

namespace snowdesktop::themes::workshop
{
bool Capabilities(std::string_view configuration, std::string_view hostVersion);
bool Available(const std::filesystem::path& bridge, std::string_view hostVersion);
struct Download
{
    std::string item, owner, sha256;
    Package package;
};
struct SubscriptionSnapshot
{
    bool authoritative = false;
    std::string account;
    std::set<std::string> subscribed, deleted;
    std::vector<Download> downloads;
};
bool DecodeSubscriptions(std::string_view json, SubscriptionSnapshot&, std::string& error);
bool Reconcile(Library&, const SubscriptionSnapshot&, std::string& error, const NewId& = CreateId);
bool Sync(const std::filesystem::path& bridge, const std::filesystem::path& library,
    Library& output, std::string& error, const std::atomic_bool* cancel = nullptr);
// renderer writes package + cover from exactly the supplied immutable snapshot.
using Renderer = std::function<bool(const Package&, std::string_view, unsigned, const std::filesystem::path&,
    std::filesystem::path&, std::string&, const std::atomic_bool*)>;
bool Prepare(const Package&, std::string_view root, unsigned scope,
    const std::filesystem::path& directory, const std::filesystem::path& data,
    const std::filesystem::path& customCover, const Renderer&, steam_bridge::ThemePublishPlan&, std::string& error,
    const std::atomic_bool* cancel = nullptr);
bool Publish(const std::filesystem::path& bridge, const steam_bridge::ThemePublishPlan&,
    const std::filesystem::path& data, std::string& output, std::string& error, const std::atomic_bool* cancel = nullptr);
}
