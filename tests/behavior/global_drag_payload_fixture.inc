#pragma once

#include "global_drag_model.h"
#include "global_drag_event_graph.inc"
#include "ole_file_source.h"
#include "settings/category_settings.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <system_error>
#include <utility>
#if defined(_WIN32)
#include <windows.h>
#include <winternl.h>
#endif

struct NativeDragIntegrationAccess;
namespace drag_global
{
// This is input preparation only. No drag, product decision, transfer,
// ownership mutation, layout commit, or product result is performed here.
struct PayloadFixtureTreeEntry
{
    std::filesystem::path relative;
    std::string kind; // file or directory
    std::string bytes; // exact planned file bytes; empty for directory
};

struct PayloadFixtureMember
{
    std::filesystem::path path;
    std::string kind;
    std::string bytes; // empty for a directory
    std::size_t identity; // index of the first occurrence of this object
    std::vector<PayloadFixtureTreeEntry> tree; // complete planned directory contents
};

struct PayloadFixtureAxis
{
    std::string id;
    std::string value;
    bool constructed = false;
    std::string reason;
};

struct PayloadFixtureResult
{
    std::filesystem::path directory;
    std::filesystem::path targetDirectory; // isolated input, not a bound host target
    std::filesystem::path desktopDirectory; // isolated input; host binding unverified
    std::filesystem::path sourceMappingDirectory, targetMappingDirectory; // empty when na
    std::vector<std::filesystem::path> directoryNodes; // canonical graph-indexed paths
    std::vector<PayloadFixtureMember> members; // captured order, duplicates retained
    std::vector<PayloadFixtureAxis> axes;
    std::vector<std::string> unresolved;
    std::string error;
    bool owns_directory = false; // set only after exclusive child creation succeeds
    bool payload_prepared = false; // source inputs only; never a product result
    bool candidate_constructed = false; // false if ANY non-derived axis is unimplemented
    bool host_target_bound = false; // this fixture never binds a production host target
};

struct PayloadInputSnapshotEntry
{
    std::filesystem::path relative;
    std::string kind;
    std::string bytes;
    // Raw Windows file identity; interpreting cross-volume identity also
    // requires a separately verified volume context.
    std::uint32_t volumeSerial = 0;
    std::uint32_t fileIndexHigh = 0;
    std::uint32_t fileIndexLow = 0;
};

struct PayloadInputSnapshot
{
    bool captured = false;
    std::string error;
    std::filesystem::path root;
    std::filesystem::path targetRelative;
    std::filesystem::path desktopRelative, sourceMappingRelative, targetMappingRelative;
    // max() means absent, unavailable or not present in this snapshot.
    std::size_t desktopEntryIndex = (std::numeric_limits<std::size_t>::max)();
    std::size_t sourceMappingEntryIndex = (std::numeric_limits<std::size_t>::max)();
    std::size_t targetMappingEntryIndex = (std::numeric_limits<std::size_t>::max)();
    std::vector<PayloadInputSnapshotEntry> entries;
    // Per captured member; max() records an object missing from this snapshot.
    // Repeated members can map to the same entry without being deduplicated.
    std::vector<std::size_t> memberEntryIndices;
    std::string scope = "names_types_default_bytes_raw_file_ids_only_non_atomic";
};

// Read a controlled fixture file with an owned, share-delete handle; short
// reads, metadata changes, replacement and reparse points are unavailable.
// This remains a point-in-time observation, not an atomic filesystem snapshot.
inline bool ReadPayloadFixtureFileChecked(const std::filesystem::path& path, std::string& bytes)
{
#if !defined(_WIN32)
    (void)path; (void)bytes;
    return false;
#else
    struct Handle final { HANDLE value = INVALID_HANDLE_VALUE; ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); } };
    try
    {
        const auto open = [&](DWORD access) {
            return CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        };
        Handle file;
        file.value = open(GENERIC_READ | FILE_READ_ATTRIBUTES);
        BY_HANDLE_FILE_INFORMATION before{}, after{}, current{};
        if (file.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file.value, &before) ||
            (before.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
            (before.nFileIndexHigh == 0 && before.nFileIndexLow == 0)) return false;
        const auto equal = [](const BY_HANDLE_FILE_INFORMATION& a, const BY_HANDLE_FILE_INFORMATION& b) {
            return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber && a.nFileIndexHigh == b.nFileIndexHigh &&
                a.nFileIndexLow == b.nFileIndexLow && a.dwFileAttributes == b.dwFileAttributes &&
                a.nFileSizeHigh == b.nFileSizeHigh && a.nFileSizeLow == b.nFileSizeLow &&
                a.ftLastWriteTime.dwHighDateTime == b.ftLastWriteTime.dwHighDateTime &&
                a.ftLastWriteTime.dwLowDateTime == b.ftLastWriteTime.dwLowDateTime;
        };
        const std::uint64_t size = (static_cast<std::uint64_t>(before.nFileSizeHigh) << 32) | before.nFileSizeLow;
        std::string observed;
        if (size > static_cast<std::uint64_t>(observed.max_size())) return false;
        observed.resize(static_cast<std::size_t>(size));
        std::size_t offset = 0;
        while (offset < observed.size())
        {
            const auto amount = static_cast<DWORD>((std::min)(observed.size() - offset, std::size_t{1024 * 1024}));
            DWORD read = 0;
            if (!ReadFile(file.value, observed.data() + offset, amount, &read, nullptr) || !read || read > amount) return false;
            offset += read;
        }
        Handle named;
        named.value = open(FILE_READ_ATTRIBUTES);
        if (!GetFileInformationByHandle(file.value, &after) || named.value == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandle(named.value, &current) || !equal(before, after) || !equal(after, current)) return false;
        bytes = std::move(observed);
        return true;
    }
    catch (const std::bad_alloc&) { return false; }
    catch (const std::length_error&) { return false; }
#endif
}

// Read actual inputs independently at each call. Call once before the host is
// bound and again after its setup; neither snapshot is a business assertion or
// an atomic filesystem transaction. The caller must explicitly account for
// intentional setup changes and must separately verify real host binding.
inline PayloadInputSnapshot CapturePayloadInputSnapshot(const PayloadFixtureResult& fixture)
{
    PayloadInputSnapshot snapshot;
    const auto fail = [&](std::string reason) { snapshot.error = std::move(reason); return snapshot; };
    if (!fixture.payload_prepared || fixture.directory.empty() || fixture.targetDirectory.empty())
        return fail("payload_snapshot_requires_prepared_fixture");
    snapshot.root = fixture.directory;
    snapshot.targetRelative = fixture.targetDirectory.lexically_relative(fixture.directory);
    // The target may be the isolated root itself; its relative path is "."
    // and must resolve to the root entry rather than being treated as escape.
    if (snapshot.targetRelative.empty() ||
        (snapshot.targetRelative.begin() != snapshot.targetRelative.end() &&
         *snapshot.targetRelative.begin() == std::filesystem::path("..")))
        return fail("payload_snapshot_target_outside_root");
#if !defined(_WIN32)
    return fail("payload_snapshot_file_identity_unimplemented");
#else
    struct SnapshotHandle final
    {
        HANDLE value = INVALID_HANDLE_VALUE;
        ~SnapshotHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    };
    const auto open = [](const std::filesystem::path& path, DWORD access) {
        return CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    };
    const auto sameIdentity = [](const BY_HANDLE_FILE_INFORMATION& a, const BY_HANDLE_FILE_INFORMATION& b) {
        return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber && a.nFileIndexHigh == b.nFileIndexHigh &&
            a.nFileIndexLow == b.nFileIndexLow;
    };
    const auto sameState = [&](const BY_HANDLE_FILE_INFORMATION& a, const BY_HANDLE_FILE_INFORMATION& b) {
        return sameIdentity(a, b) && a.dwFileAttributes == b.dwFileAttributes &&
            a.nFileSizeHigh == b.nFileSizeHigh && a.nFileSizeLow == b.nFileSizeLow &&
            a.ftLastWriteTime.dwHighDateTime == b.ftLastWriteTime.dwHighDateTime &&
            a.ftLastWriteTime.dwLowDateTime == b.ftLastWriteTime.dwLowDateTime;
    };
    const auto capture = [&](const std::filesystem::path& path, const std::filesystem::path& relative) {
        try
        {
        SnapshotHandle attributes;
        attributes.value = open(path, FILE_READ_ATTRIBUTES);
        BY_HANDLE_FILE_INFORMATION before{};
        if (attributes.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(attributes.value, &before) ||
            (before.nFileIndexHigh == 0 && before.nFileIndexLow == 0))
        { snapshot.error = "payload_snapshot_identity_unavailable"; return false; }
        if (before.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        { snapshot.error = "payload_snapshot_reparse_point_unimplemented"; return false; }
        const bool directory = (before.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        std::error_code statusError;
        const auto status = std::filesystem::symlink_status(path, statusError);
        if (statusError || (directory ? !std::filesystem::is_directory(status) : !std::filesystem::is_regular_file(status)))
        { snapshot.error = "payload_snapshot_entry_type_unimplemented"; return false; }
        std::string bytes;
        if (!directory)
        {
            SnapshotHandle reader;
            reader.value = open(path, GENERIC_READ | FILE_READ_ATTRIBUTES);
            BY_HANDLE_FILE_INFORMATION readBefore{}, readAfter{};
            if (reader.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(reader.value, &readBefore) ||
                !sameState(before, readBefore) || (readBefore.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            { snapshot.error = "payload_snapshot_file_read_identity_unavailable"; return false; }
            const std::uint64_t size = (static_cast<std::uint64_t>(before.nFileSizeHigh) << 32) | before.nFileSizeLow;
            if (size > static_cast<std::uint64_t>(bytes.max_size()))
            { snapshot.error = "payload_snapshot_file_size_unavailable"; return false; }
            bytes.resize(static_cast<std::size_t>(size));
            std::size_t offset = 0;
            while (offset < bytes.size())
            {
                const auto amount = static_cast<DWORD>((std::min)(bytes.size() - offset, std::size_t{1024 * 1024}));
                DWORD read = 0;
                if (!ReadFile(reader.value, bytes.data() + offset, amount, &read, nullptr) || !read || read > amount)
                { snapshot.error = "payload_snapshot_file_short_read_or_error"; return false; }
                offset += read;
            }
            if (!GetFileInformationByHandle(reader.value, &readAfter) || !sameState(readBefore, readAfter))
            { snapshot.error = "payload_snapshot_input_changed_during_read"; return false; }
        }
        BY_HANDLE_FILE_INFORMATION after{}, pathAfter{};
        SnapshotHandle current;
        current.value = open(path, FILE_READ_ATTRIBUTES);
        if (!GetFileInformationByHandle(attributes.value, &after) || current.value == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandle(current.value, &pathAfter) || !sameState(before, after) || !sameState(after, pathAfter))
        { snapshot.error = "payload_snapshot_input_changed_during_read"; return false; }
        snapshot.entries.push_back({relative, directory ? "directory" : "file", std::move(bytes),
            before.dwVolumeSerialNumber, before.nFileIndexHigh, before.nFileIndexLow});
        return true;
        }
        catch (const std::bad_alloc&) { snapshot.error = "payload_snapshot_resource_unavailable"; return false; }
        catch (const std::length_error&) { snapshot.error = "payload_snapshot_resource_unavailable"; return false; }
    };
    if (!capture(snapshot.root, std::filesystem::path("."))) return snapshot;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(snapshot.root, ec), end; !ec && it != end; it.increment(ec))
    {
        const auto relative = it->path().lexically_relative(snapshot.root);
        if (relative.empty() || *relative.begin() == std::filesystem::path(".."))
            return fail("payload_snapshot_entry_outside_root");
        if (!capture(it->path(), relative)) return snapshot;
    }
    if (ec) return fail("payload_snapshot_enumeration_failed");
    std::sort(snapshot.entries.begin(), snapshot.entries.end(), [](const auto& left, const auto& right) {
        return left.relative.generic_wstring() < right.relative.generic_wstring();
    });
    const auto target = std::find_if(snapshot.entries.begin(), snapshot.entries.end(), [&](const auto& entry) {
        return entry.relative == snapshot.targetRelative && entry.kind == "directory";
    });
    if (target == snapshot.entries.end()) return fail("payload_snapshot_target_missing");
    const auto captureEndpoint = [&](const std::filesystem::path& path, std::filesystem::path& relative, std::size_t& index) {
        if (path.empty()) return true;
        relative = path.lexically_relative(snapshot.root);
        if (relative.empty() || (relative.begin() != relative.end() && *relative.begin() == std::filesystem::path("..")))
        { snapshot.error = "payload_snapshot_endpoint_outside_root"; return false; }
        const auto entry = std::find_if(snapshot.entries.begin(), snapshot.entries.end(), [&](const auto& item) {
            return item.relative == relative && item.kind == "directory";
        });
        if (entry != snapshot.entries.end()) index = static_cast<std::size_t>(entry - snapshot.entries.begin());
        return true;
    };
    if (!captureEndpoint(fixture.desktopDirectory, snapshot.desktopRelative, snapshot.desktopEntryIndex) ||
        !captureEndpoint(fixture.sourceMappingDirectory, snapshot.sourceMappingRelative, snapshot.sourceMappingEntryIndex) ||
        !captureEndpoint(fixture.targetMappingDirectory, snapshot.targetMappingRelative, snapshot.targetMappingEntryIndex)) return snapshot;
    for (const auto& member : fixture.members)
    {
        const auto relative = member.path.lexically_relative(snapshot.root);
        if (relative.empty() ||
            (relative.begin() != relative.end() && *relative.begin() == std::filesystem::path("..")))
            return fail("payload_snapshot_member_outside_root");
        const auto entry = std::find_if(snapshot.entries.begin(), snapshot.entries.end(), [&](const auto& item) {
            return item.relative == relative;
        });
        snapshot.memberEntryIndices.push_back(entry == snapshot.entries.end() ? (std::numeric_limits<std::size_t>::max)()
            : static_cast<std::size_t>(entry - snapshot.entries.begin()));
    }
    snapshot.captured = true;
    return snapshot;
#endif
}

// Shared validation for input fixtures. Rebuild the index before any model
// lookup; neither a supplied schema nor its cached index is trusted.
inline std::string ValidatePayloadCandidate(const std::vector<Axis>& axes, const Candidate& candidate,
    std::vector<Axis>& canonicalAxes, Candidate& safe,
    SelectionSchemaVersion version = SelectionSchemaVersion::legacy_v1)
{
    try { canonicalAxes = Model(version); }
    catch (const std::exception&) { return "canonical_model_schema_unimplemented"; }
    if (axes.size() != canonicalAxes.size()) return "model_schema_size_unimplemented";
    for (std::size_t i = 0; i < axes.size(); ++i)
        if (axes[i].id != canonicalAxes[i].id || axes[i].role != canonicalAxes[i].role ||
            axes[i].values != canonicalAxes[i].values || axes[i].label != canonicalAxes[i].label)
            return "model_schema_axis_unimplemented:" + std::to_string(i);
    if (candidate.values.size() != canonicalAxes.size()) return "candidate_shape_unimplemented";
    const auto lookup = BuildIndex(canonicalAxes);
    if (candidate.lookup && *candidate.lookup != *lookup) return "candidate_lookup_mismatch_unimplemented";
    safe = Candidate{candidate.ordinal, std::vector<std::string>(canonicalAxes.size()), lookup};
    for (std::size_t i = 0; i < canonicalAxes.size(); ++i)
    {
        const auto& axis = canonicalAxes[i];
        const auto& value = candidate.values[i];
        if (axis.role == Role::Derived)
        {
            if (!value.empty()) return "candidate_derived_value_unimplemented:" + axis.id;
            continue;
        }
        if (std::find(axis.values.begin(), axis.values.end(), value) == axis.values.end())
            return "candidate_domain_unimplemented:" + axis.id;
        const bool allowed = version == SelectionSchemaVersion::source_members_v2
            ? PrefixAllowsV2(canonicalAxes, safe, axis.id, value) : PrefixAllows(canonicalAxes, safe, axis.id, value);
        if (!allowed)
            return "candidate_prefix_unimplemented:" + axis.id;
        safe.values[i] = value;
    }
    const auto exclusion = version == SelectionSchemaVersion::source_members_v2
        ? StructuralExclusionV2(canonicalAxes, safe) : StructuralExclusion(canonicalAxes, safe);
    return exclusion.empty() ? std::string{} : "candidate_structure_unimplemented:" + exclusion;
}

// A normalized directory topology, not filesystem creation or test evidence.
// Node indices 0/1 are the isolated anchor and target; each active member has
// a parent slot and only directory members have an object slot. `canonical`
// resolves aliases and `parent` is indexed by canonical nodes. The anchor is
// an isolated fixture root, not a claim about a real filesystem volume root.
struct PayloadDirectoryGraph
{
    bool constructible = false;
    std::string reason;
    std::vector<std::size_t> canonical;
    std::vector<int> parent;
    std::vector<std::size_t> memberParents;
    std::vector<int> memberObjects;
    std::vector<std::string> unresolved;
    std::size_t isolatedRoot = 0;
    std::size_t isolatedRootParent = 0; // anchor only; no writes outside isolatedRoot
    std::size_t target = 1;
    int desktop = -1, sourceMapping = -1, targetMapping = -1;
};

// The caller has validated the complete unified schema before making any
// task-local substitutions. B keeps the full schema and lookup; cross-task
// references remain unimplemented until both tasks share one physical graph.
inline bool SelectPayloadTask(const std::vector<Axis>& axes, Candidate& safe, bool secondTask)
{
    if (!secondTask) return true;
    if (Value(axes, safe, "X03.task_count") != "two") return false;
    const auto lookup = BuildIndex(axes);
    for (std::size_t i = 0; i < axes.size(); ++i)
    {
        if (axes[i].id.substr(0, 7) == "task_b.") continue;
        const auto other = lookup->find("task_b." + axes[i].id);
        if (other != lookup->end()) safe.values[i] = safe.values[other->second];
    }
    return true;
}

inline PayloadDirectoryGraph ResolveValidatedPayloadDirectoryGraph(const std::vector<Axis>& schema, const Candidate& safe,
    SelectionSchemaVersion version = SelectionSchemaVersion::legacy_v1,
    const std::size_t* exactSourceCount = nullptr)
{
    PayloadDirectoryGraph result;
    const auto fail = [&](std::string reason) { result.reason = std::move(reason); return result; };
    const int oldCount = exactSourceCount ? 0 : (version == SelectionSchemaVersion::source_members_v2
        ? SelectionSourceSize(schema, safe) : SelectionSize(schema, safe));
    if (!exactSourceCount && (oldCount < 0 || oldCount > 129)) return fail("directory_graph_count_unimplemented");
    const auto n = exactSourceCount ? *exactSourceCount : static_cast<std::size_t>(oldCount);
    // Graph parent IDs are currently int. This is an implementation resource
    // boundary, not a product member limit or an excluded model combination.
    if (n > (static_cast<std::size_t>((std::numeric_limits<int>::max)()) - 6) / 2)
        return fail("directory_graph_index_resource_unavailable");
    const auto value = [&](std::string_view id) { return Value(schema, safe, id); };
    const auto kind = [&](std::size_t i) {
        if (i == 0) return value("P01.primary");
        if (i == 1) return value("P01.second");
        return value("P01.member" + std::to_string(i) + "_kind");
    };
    const auto parentNode = [](std::size_t i) { return 2 + i; };
    const auto objectNode = [n](std::size_t i) { return 2 + n + i; };
    const auto outerNode = 2 + 2 * n;
    const auto desktopNode = outerNode + 1;
    const auto sourceMappingNode = outerNode + 2;
    const auto targetMappingNode = outerNode + 3;
    const auto nodeCount = outerNode + 4;
    const std::array<std::string_view, 3> endpointNames{"desktop_directory", "source_mapping_directory", "target_mapping_directory"};
    const std::array<std::size_t, 3> endpointNodes{desktopNode, sourceMappingNode, targetMappingNode};
    std::vector<std::size_t> sets(nodeCount);
    for (std::size_t i = 0; i < nodeCount; ++i) sets[i] = i;
    const auto root = [&](std::size_t i) {
        while (sets[i] != i) i = sets[i];
        return i;
    };
    const auto join = [&](std::size_t a, std::size_t b) {
        a = root(a); b = root(b);
        if (a != b) sets[b] = a;
    };
    const auto parseIndex = [&](std::string_view text, std::size_t& index) {
        if (text.empty() || (text.size() > 1 && text.front() == '0')) return false;
        index = 0;
        for (const char c : text)
        {
            const auto digit = static_cast<std::size_t>(c - '0');
            if (c < '0' || c > '9' || index > ((std::numeric_limits<std::size_t>::max)() - digit) / 10) return false;
            index = index * 10 + digit;
        }
        return index < n;
    };
    // Resolve a declared node reference only after the complete member table
    // exists, so forward references do not depend on iteration order.
    const auto resolve = [&](std::string_view ref, int& node) {
        if (ref == "isolated_root") { node = 0; return true; }
        if (ref == "isolated_root_parent") { node = static_cast<int>(outerNode); return true; }
        if (ref == "target_directory") { node = 1; return true; }
        for (std::size_t i = 0; i < endpointNames.size(); ++i)
            if (ref == endpointNames[i])
            {
                if (value("R02." + std::string(endpointNames[i]) + "_identity") == "na") return false;
                node = static_cast<int>(endpointNodes[i]); return true;
            }
        constexpr std::string_view objectPrefix = "member_object_";
        constexpr std::string_view memberPrefix = "member_";
        std::size_t index = 0;
        if (ref.substr(0, objectPrefix.size()) == objectPrefix)
        {
            if (!parseIndex(ref.substr(objectPrefix.size()), index) || kind(index) != "directory") return false;
            node = static_cast<int>(objectNode(index)); return true;
        }
        if (ref.substr(0, memberPrefix.size()) == memberPrefix)
        {
            if (!parseIndex(ref.substr(memberPrefix.size()), index)) return false;
            node = static_cast<int>(parentNode(index)); return true;
        }
        return false; // Includes unknown, actual roots and unmodelled intermediates.
    };
    for (std::size_t i = 0; i < n; ++i)
        if (kind(i) != "file" && kind(i) != "directory") return fail("directory_graph_member_type_unimplemented:" + std::to_string(i));

    // The isolated root's actual parent is a separate external anchor; it is
    // neither a real volume root nor an authorization to write outside root.
    std::vector<std::size_t> fresh{0, outerNode};
    const auto targetIdentity = value("R02.target_directory_identity");
    if (targetIdentity == "unique") fresh.push_back(1);
    else { int node = -1; if (!resolve(targetIdentity, node)) return fail("directory_graph_target_identity_unimplemented"); join(1, static_cast<std::size_t>(node)); }
    for (std::size_t i = 0; i < endpointNames.size(); ++i)
    {
        const auto ref = value("R02." + std::string(endpointNames[i]) + "_identity");
        if (ref == "na")
        {
            if (value("R02." + std::string(endpointNames[i]) + "_parent_ref") != "na")
                return fail("directory_graph_absent_endpoint_parent_unimplemented:" + std::string(endpointNames[i]));
            continue;
        }
        if (ref == "unique") fresh.push_back(endpointNodes[i]);
        else { int node = -1; if (!resolve(ref, node)) return fail("directory_graph_endpoint_identity_unimplemented:" + std::string(endpointNames[i])); join(endpointNodes[i], static_cast<std::size_t>(node)); }
    }
    for (std::size_t i = 0; i < n; ++i)
    {
        const auto ref = value("R02.member" + std::to_string(i) + "_directory_ref");
        if (ref == "unique") fresh.push_back(parentNode(i));
        else { int node = -1; if (!resolve(ref, node)) return fail("directory_graph_parent_identity_unimplemented:" + std::to_string(i)); join(parentNode(i), static_cast<std::size_t>(node)); }
        const auto object = value("R02.member" + std::to_string(i) + "_directory_object_ref");
        if (kind(i) == "file")
        {
            if (object != "na") return fail("directory_graph_object_type_unimplemented:" + std::to_string(i));
        }
        else if (object == "unique") fresh.push_back(objectNode(i));
        else { int node = -1; if (!resolve(object, node)) return fail("directory_graph_object_identity_unimplemented:" + std::to_string(i)); join(objectNode(i), static_cast<std::size_t>(node)); }
    }
    // Duplicate selected object identity entails the same parent and, for a
    // directory, the same object node. Unknown identity cannot be guessed.
    for (std::size_t i = 1; i < n; ++i)
    {
        const auto ref = value("P03.member" + std::to_string(i) + "_identity_ref");
        if (ref == "unique") continue;
        constexpr std::string_view prefix = "member_";
        std::size_t prior = 0;
        if (ref.substr(0, prefix.size()) != prefix || !parseIndex(ref.substr(prefix.size()), prior) || prior >= i)
            return fail("directory_graph_member_identity_unimplemented:" + std::to_string(i));
        if (kind(i) != kind(prior)) return fail("directory_graph_alias_type_unimplemented:" + std::to_string(i));
        join(parentNode(i), parentNode(prior));
        if (kind(i) == "directory") join(objectNode(i), objectNode(prior));
    }
    for (std::size_t i = 0; i < fresh.size(); ++i)
        for (std::size_t j = 0; j < i; ++j)
            if (root(fresh[i]) == root(fresh[j])) return fail("directory_graph_fresh_alias_conflict_unimplemented");
    const auto grounded = [&](std::size_t node) {
        for (const auto anchor : fresh) if (root(node) == root(anchor)) return true;
        return false;
    };
    for (std::size_t i = 0; i < n; ++i)
        if (!grounded(parentNode(i)) || (kind(i) == "directory" && !grounded(objectNode(i))))
            return fail("directory_graph_unanchored_identity_unimplemented:" + std::to_string(i));
    if (!grounded(1)) return fail("directory_graph_unanchored_target_unimplemented");
    for (std::size_t i = 0; i < endpointNames.size(); ++i)
        if (value("R02." + std::string(endpointNames[i]) + "_identity") != "na" && !grounded(endpointNodes[i]))
            return fail("directory_graph_unanchored_endpoint_unimplemented:" + std::string(endpointNames[i]));
    result.canonical.resize(nodeCount);
    result.parent.assign(nodeCount, -1);
    for (std::size_t i = 0; i < nodeCount; ++i) result.canonical[i] = root(i);
    result.isolatedRoot = root(0); result.isolatedRootParent = root(outerNode); result.target = root(1);
    std::array<int*, 3> endpointResults{&result.desktop, &result.sourceMapping, &result.targetMapping};
    for (std::size_t i = 0; i < endpointNames.size(); ++i)
        if (value("R02." + std::string(endpointNames[i]) + "_identity") != "na")
            *endpointResults[i] = static_cast<int>(root(endpointNodes[i]));
    const auto assignParent = [&](std::size_t child, int parent) {
        child = root(child);
        const auto actual = static_cast<int>(root(static_cast<std::size_t>(parent)));
        if (result.parent[child] != -1 && result.parent[child] != actual) return false;
        result.parent[child] = actual;
        return true;
    };
    if (!assignParent(0, static_cast<int>(outerNode))) return fail("directory_graph_root_parent_conflict_unimplemented");
    int targetParent = -1;
    if (!resolve(value("R02.target_directory_parent_ref"), targetParent)) return fail("directory_graph_target_parent_unimplemented");
    if (!assignParent(1, targetParent)) return fail("directory_graph_alias_parent_conflict_unimplemented");
    for (std::size_t i = 0; i < endpointNames.size(); ++i)
    {
        if (*endpointResults[i] < 0) continue;
        int parent = -1;
        if (!resolve(value("R02." + std::string(endpointNames[i]) + "_parent_ref"), parent))
            return fail("directory_graph_endpoint_parent_unimplemented:" + std::string(endpointNames[i]));
        if (!assignParent(endpointNodes[i], parent)) return fail("directory_graph_alias_parent_conflict_unimplemented");
    }
    for (std::size_t i = 0; i < n; ++i)
    {
        const auto declaredParent = value("R02.member" + std::to_string(i) + "_directory_parent_ref");
        if (root(parentNode(i)) == result.isolatedRootParent)
        {
            if (declaredParent != "outside_model_unresolved" && declaredParent != "unknown")
                return fail("directory_graph_outer_parent_unimplemented:" + std::to_string(i));
            result.unresolved.push_back("isolated_root_parent_parent_outside_model:" + std::to_string(i));
        }
        else
        {
            int parent = -1;
            if (!resolve(declaredParent, parent)) return fail("directory_graph_member_parent_unimplemented:" + std::to_string(i));
            if (!assignParent(parentNode(i), parent)) return fail("directory_graph_alias_parent_conflict_unimplemented");
        }
        if (kind(i) == "directory" && !assignParent(objectNode(i), static_cast<int>(parentNode(i))))
            return fail("directory_graph_alias_parent_conflict_unimplemented");
        result.memberParents.push_back(root(parentNode(i)));
        result.memberObjects.push_back(kind(i) == "directory" ? static_cast<int>(root(objectNode(i))) : -1);
        if (result.memberParents.back() == result.isolatedRootParent &&
            (kind(i) != "directory" || static_cast<std::size_t>(result.memberObjects.back()) != result.isolatedRoot))
            return fail("directory_graph_outside_fixture_write_unimplemented:" + std::to_string(i));
    }
    if (result.parent[result.isolatedRoot] != static_cast<int>(result.isolatedRootParent) ||
        result.parent[result.isolatedRootParent] != -1) return fail("directory_graph_isolated_root_parent_unimplemented");
    for (std::size_t i = 0; i < nodeCount; ++i)
    {
        if (root(i) != i || i == result.isolatedRootParent ||
            (i >= desktopNode && value("R02." + std::string(endpointNames[i - desktopNode]) + "_identity") == "na") ||
            (i >= 2 + n && i < outerNode && kind(i - 2 - n) != "directory")) continue;
        std::vector<bool> seen(nodeCount, false);
        auto at = i;
        while (at != result.isolatedRoot)
        {
            if (seen[at]) return fail("directory_graph_cycle_unimplemented");
            seen[at] = true;
            if (at == result.isolatedRootParent) return fail("directory_graph_outside_fixture_unimplemented");
            if (result.parent[at] < 0) return fail("directory_graph_unanchored_node_unimplemented");
            at = static_cast<std::size_t>(result.parent[at]);
        }
    }
    const auto ancestor = [&](std::size_t parent, std::size_t child) {
        while (child != result.isolatedRootParent)
        {
            child = static_cast<std::size_t>(result.parent[child]);
            if (child == parent) return true;
        }
        return false;
    };
    for (std::size_t i = 0; i < n; ++i)
    {
        const auto relation = value("R02.member" + std::to_string(i) + "_directory_to_target");
        if (relation == "na" || relation == "unknown" || relation == "unresolved") continue;
        const auto member = result.memberParents[i];
        const std::string_view actual = member == result.target ? "same" :
            ancestor(member, result.target) ? "ancestor" : ancestor(result.target, member) ? "descendant" : "unrelated";
        if (relation != actual) return fail("directory_graph_target_relation_conflict_unimplemented:" + std::to_string(i));
    }
    result.constructible = true; // topology only; volume and product path remain unimplemented
    return result;
}

inline PayloadDirectoryGraph ResolvePayloadDirectoryGraph(const std::vector<Axis>& axes, const Candidate& candidate,
    bool secondTask = false, SelectionSchemaVersion version = SelectionSchemaVersion::legacy_v1)
{
    std::vector<Axis> schema;
    Candidate safe;
    if (const auto reason = ValidatePayloadCandidate(axes, candidate, schema, safe, version); !reason.empty())
    { PayloadDirectoryGraph result; result.reason = reason; return result; }
    if (!SelectPayloadTask(schema, safe, secondTask))
    { PayloadDirectoryGraph result; result.reason = "second_payload_task_not_active_unimplemented"; return result; }
    if (version == SelectionSchemaVersion::source_members_v2 && !SelectionV2HasExplicitFileSource(schema, safe))
    { PayloadDirectoryGraph result; result.reason = "v2_source_vector_format_semantics_unimplemented"; return result; }
    auto result = ResolveValidatedPayloadDirectoryGraph(schema, safe, version);
    // constructible describes only this task's local isolated topology. It
    // never establishes the identities or shared filesystem of both tasks.
    if (Value(schema, safe, "X03.task_count") == "two")
        result.unresolved.emplace_back("two_task_shared_directory_graph_and_identity_unimplemented");
    return result;
}

// The caller supplies an existing, task-owned temporary parent directory. This
// function creates a unique child and never deletes or reuses any directory.
// Every axis not verified by the constructor is explicitly unimplemented.
// A caller must separately construct remaining input and drive/observe the
// production path; `payload_prepared` is not authorization to claim a test passed.
inline PayloadFixtureResult PreparePayloadFixture(const std::vector<Axis>& axes,
    const Candidate& candidate, const std::filesystem::path& temporaryParent, bool secondTask = false,
    SelectionSchemaVersion version = SelectionSchemaVersion::legacy_v1,
    const V3FamilyValidatedContext* v3Context = nullptr, bool v3JointFirst = false)
{
    PayloadFixtureResult result;
    try
    {
    // Reserve a short failure reason before any owned child can be created.
    result.error.reserve(128);
    result.axes.reserve(axes.size());
    for (std::size_t i = 0; i < axes.size(); ++i)
        result.axes.push_back({axes[i].id, i < candidate.values.size() ? candidate.values[i] : std::string{}, false,
            axes[i].role == Role::Derived ? "derived_observation_unimplemented" :
            axes[i].role == Role::Hypothesis ? "production_hypothesis_observation_unimplemented" : "input_constructor_unimplemented"});
    for (auto& axis : result.axes)
        if (axis.id == "P02.resolved_count" || axis.id == "P02.many_count" ||
            axis.id == "task_b.P02.resolved_count" || axis.id == "task_b.P02.many_count")
            axis.reason = "production_final_count_observation_unimplemented";

    const auto fail = [&](std::string reason) {
        result.error = std::move(reason);
        return std::move(result);
    };
    std::vector<Axis> canonicalAxes;
    Candidate safe;
    std::size_t v3Count = 0;
    int count = 0;
    if (v3Context)
    {
        // Only the exact immutable data owned by the authoritative context may
        // reach this path; arbitrary vectors cannot impersonate a V3 family.
        if (&axes != &v3Context->Axes() || &candidate != &v3Context->Coordinate() ||
            v3Context->Status() != V3CoordinateStatus::coordinate_ready)
            return fail("v3_payload_authoritative_coordinate_unavailable");
        if (secondTask || v3Context->TaskBActive() != v3JointFirst)
            return fail("v3_joint_payload_unimplemented");
        const auto audit = v3Context->AuditSelectionA();
        if (audit.state != V3SelectionAuditState::unresolved ||
            audit.reason != "v3_selection_remaining_structure_and_execution_unresolved")
            return fail("v3_payload_selection_unimplemented:" + std::string(audit.reason));
        const auto transfer = v3Context->AuditTransfer();
        if (!transfer.invalid.empty() || !transfer.localExclusion.empty())
            return fail("v3_payload_transfer_structure_unimplemented");
        canonicalAxes = v3Context->Axes();
        safe = v3Context->Coordinate();
        const auto encoded = Value(canonicalAxes, safe, "P02.source_actual_count");
        if (encoded == "zero") v3Count = 0;
        else if (encoded == "one") v3Count = 1;
        else if (encoded == "two") v3Count = 2;
        else if (!encoded.starts_with("count_") ||
            !V3SourceCountForMaterialization(encoded.substr(6), v3Count) || v3Count < 3)
            return fail("v3_source_count_resource_unavailable");
        if (v3Count > (static_cast<std::size_t>((std::numeric_limits<int>::max)()) - 6) / 2)
            return fail("v3_payload_graph_index_resource_unavailable");
        count = static_cast<int>(v3Count);
    }
    else
    {
        if (const auto reason = ValidatePayloadCandidate(axes, candidate, canonicalAxes, safe, version); !reason.empty())
            return fail(reason);
        if (!SelectPayloadTask(canonicalAxes, safe, secondTask)) return fail("second_payload_task_not_active_unimplemented");
        count = version == SelectionSchemaVersion::source_members_v2
            ? SelectionSourceSize(canonicalAxes, safe) : SelectionSize(canonicalAxes, safe);
        if (count < 0 || count > 129) return fail("resolved_selection_count_unimplemented");
    }
    const auto get = [&](std::string_view id) { return Value(canonicalAxes, safe, id); };
    if (!v3Context && version == SelectionSchemaVersion::source_members_v2 &&
        !SelectionV2HasExplicitFileSource(canonicalAxes, safe))
        return fail("v2_source_vector_format_semantics_unimplemented");
    // Legacy used one vector for the source and resolved hypothesis; retain
    // its original mismatch guard. V2 constructs only the source vector and
    // leaves the independent production result unobserved.
    if (count == 0) return fail("empty_source_unimplemented");
    const std::string actualCount = count == 1 ? "one" : count == 2 ? "two" : "count_" + std::to_string(count);
    const auto offeredCount = get("P02.source_actual_count");
    if (offeredCount.empty() || offeredCount == "na" || offeredCount == "unknown" ||
        offeredCount == "above_129_unresolved" || offeredCount == "unspecified_unresolved")
        return fail("source_actual_count_unimplemented");
    if (!v3Context && version == SelectionSchemaVersion::legacy_v1 && offeredCount != actualCount)
        return fail("source_and_resolved_count_mismatch_unimplemented");
    if (secondTask)
        for (int i = 0; i < count; ++i)
            if (get("P03.member" + std::to_string(i) + "_cross_task_identity").substr(0, 14) == "task_a_member_")
                return fail("joint_member_shared_source_unimplemented:" + std::to_string(i));
    const auto memberKind = [&](int index) {
        if (index == 0) return get("P01.primary");
        if (index == 1) return get("P01.second");
        return get("P01.member" + std::to_string(index) + "_kind");
    };
    const auto mark = [&](std::string_view id, std::string_view reason = "source_payload_input_constructed") {
        const auto actualId = secondTask ? "task_b." + std::string(id) : std::string(id);
        for (auto& axis : result.axes)
            if (axis.id == actualId) { axis.constructed = true; axis.reason = std::string(reason); return; }
    };

    // Preflight first so unsupported object identities or kinds do not lead to
    // an incomplete fixture being advertised as prepared.
    std::vector<std::size_t> identities;
    identities.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index)
    {
        const auto kind = memberKind(index);
        if (kind != "file" && kind != "directory") return fail("member_kind_unimplemented:" + std::to_string(index));
        if (index == 0) { identities.push_back(0); continue; }
        const auto key = "P03.member" + std::to_string(index) + "_identity_ref";
        const auto reference = get(key);
        if (reference == "unique") { identities.push_back(static_cast<std::size_t>(index)); continue; }
        if (reference.substr(0, 7) != "member_") return fail("member_identity_unimplemented:" + std::to_string(index));
        const auto digits = reference.substr(7);
        if (digits.empty() || (digits.size() > 1 && digits.front() == '0'))
            return fail("member_identity_unimplemented:" + std::to_string(index));
        std::size_t prior = 0;
        for (const char digit : digits)
        {
            if (digit < '0' || digit > '9') return fail("member_identity_unimplemented:" + std::to_string(index));
            const auto number = static_cast<std::size_t>(digit - '0');
            if (prior > ((std::numeric_limits<std::size_t>::max)() - number) / 10)
                return fail("member_identity_unimplemented:" + std::to_string(index));
            prior = prior * 10 + number;
        }
        if (prior >= static_cast<std::size_t>(index) || memberKind(static_cast<int>(prior)) != kind)
            return fail("member_identity_conflict_unimplemented:" + std::to_string(index));
        identities.push_back(identities[prior]);
    }

    // Graph interpretation is a separate pure preflight. A declared topology
    // that is unknown, unanchored, inconsistent, or outside the bounded node
    // table cannot be silently replaced with one same-directory fixture.
    const auto graph = ResolveValidatedPayloadDirectoryGraph(canonicalAxes, safe, version, v3Context ? &v3Count : nullptr);
    if (!graph.constructible) return fail("directory_topology_unimplemented:" + graph.reason);
    result.unresolved = graph.unresolved;
    if (v3Context) result.unresolved.emplace_back("v3_family_full_structure_host_binding_and_execution_unimplemented");
    if (get("X03.task_count") == "two")
        result.unresolved.emplace_back("two_task_shared_directory_graph_and_identity_unimplemented");
    for (std::size_t i = 0; i < identities.size(); ++i)
        if (memberKind(static_cast<int>(i)) == "directory")
            for (std::size_t j = 0; j < i; ++j)
                if (memberKind(static_cast<int>(j)) == "directory" &&
                    ((identities[i] == identities[j]) != (graph.memberObjects[i] == graph.memberObjects[j])))
                    return fail("directory_object_identity_conflict_unimplemented");

    std::error_code ec;
    if (!std::filesystem::is_directory(temporaryParent, ec) || ec)
        return fail("temporary_parent_unavailable");
#if defined(_WIN32)
    // The caller owns this parent. Reject a reparse point anywhere in its
    // current ancestry before creating the child; path checks are not atomic
    // against a concurrent replacement, which remains unresolved.
    if (!temporaryParent.is_absolute()) return fail("temporary_parent_absolute_path_unimplemented");
    for (auto ancestor = temporaryParent; !ancestor.empty();)
    {
        const DWORD attributes = GetFileAttributesW(ancestor.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return fail("temporary_parent_ancestry_unimplemented");
        const auto next = ancestor.parent_path();
        if (next == ancestor) break;
        ancestor = next;
    }
#endif
    static std::atomic<unsigned long long> serial{0};
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned attempt = 0; attempt < 100; ++attempt)
    {
        const auto leaf = "drag_payload_" + std::to_string(tick) + "_" +
            std::to_string(serial.fetch_add(1, std::memory_order_relaxed));
        const auto path = temporaryParent / leaf;
        result.directory = path;
        ec.clear();
        if (std::filesystem::create_directory(path, ec)) { result.owns_directory = true; break; }
        result.directory.clear();
        if (ec) return fail("temporary_directory_creation_failed:" + ec.message());
    }
    if (result.directory.empty()) return fail("temporary_directory_name_exhausted");
    std::set<std::size_t> graphNodes{graph.isolatedRootParent, graph.isolatedRoot, graph.target};
    for (const auto endpoint : {graph.desktop, graph.sourceMapping, graph.targetMapping})
        if (endpoint >= 0) graphNodes.insert(static_cast<std::size_t>(endpoint));
    for (std::size_t i = 0; i < identities.size(); ++i)
    {
        graphNodes.insert(graph.memberParents[i]);
        if (graph.memberObjects[i] >= 0) graphNodes.insert(static_cast<std::size_t>(graph.memberObjects[i]));
    }
    // Include every declared ancestor; no implicit intermediate node is added.
    for (auto it = graphNodes.begin(); it != graphNodes.end(); ++it)
    {
        auto node = *it;
        while (node != graph.isolatedRoot && node != graph.isolatedRootParent)
        {
            if (node >= graph.parent.size() || graph.parent[node] < 0) return fail("directory_graph_parent_missing");
            node = static_cast<std::size_t>(graph.parent[node]);
            graphNodes.insert(node);
        }
    }
    result.directoryNodes.resize(graph.parent.size());
    result.directoryNodes[graph.isolatedRootParent] = temporaryParent;
    result.directoryNodes[graph.isolatedRoot] = result.directory;
    std::map<std::size_t, std::size_t> objectNames;
    for (std::size_t i = 0; i < identities.size(); ++i)
        if (graph.memberObjects[i] >= 0)
            objectNames.emplace(static_cast<std::size_t>(graph.memberObjects[i]), identities[i]);
    std::vector<std::set<std::wstring>> expectedChildren(graph.parent.size());
    std::size_t pending = graphNodes.size() - 2;
    while (pending)
    {
        bool progressed = false;
        for (const auto node : graphNodes)
        {
            if (!result.directoryNodes[node].empty()) continue;
            const auto parent = static_cast<std::size_t>(graph.parent[node]);
            if (result.directoryNodes[parent].empty()) continue;
            const auto object = objectNames.find(node);
            const auto name = object == objectNames.end() ? "directory_node_" + std::to_string(node)
                                                          : "member_" + std::to_string(object->second);
            const auto path = result.directoryNodes[parent] / name;
            ec.clear();
            if (!std::filesystem::create_directory(path, ec) || ec)
                return fail("graph_directory_creation_failed:" + ec.message());
            result.directoryNodes[node] = path;
            expectedChildren[parent].insert(path.filename().wstring());
            --pending; progressed = true;
        }
        if (!progressed) return fail("directory_graph_creation_order_unimplemented");
    }
    result.targetDirectory = result.directoryNodes[graph.target];
    if (graph.desktop >= 0) result.desktopDirectory = result.directoryNodes[static_cast<std::size_t>(graph.desktop)];
    if (graph.sourceMapping >= 0) result.sourceMappingDirectory = result.directoryNodes[static_cast<std::size_t>(graph.sourceMapping)];
    if (graph.targetMapping >= 0) result.targetMappingDirectory = result.directoryNodes[static_cast<std::size_t>(graph.targetMapping)];
    for (int index = 0; index < count; ++index)
    {
        const auto slot = static_cast<std::size_t>(index);
        const auto identity = identities[slot];
        if (identity != slot) { result.members.push_back(result.members[identity]); continue; }
        const std::string kind(memberKind(index));
        const auto parent = graph.memberParents[slot];
        const auto path = kind == "directory" ? result.directoryNodes[static_cast<std::size_t>(graph.memberObjects[slot])]
                                               : result.directoryNodes[parent] / ("member_" + std::to_string(index) + ".txt");
        const std::string bytes = kind == "file" ? "drag-payload-member-" + std::to_string(index) + "\n" : "";
        if (kind == "file")
        {
            std::ofstream out(path, std::ios::binary | std::ios::out);
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            out.close();
            if (!out) return fail("source_file_write_failed");
            std::string actual;
            if (!ReadPayloadFixtureFileChecked(path, actual)) return fail("source_file_read_failed");
            if (actual != bytes) return fail("source_file_contents_mismatch");
            expectedChildren[parent].insert(path.filename().wstring());
        }
        result.members.push_back({path, kind, bytes, identity, {}});
    }

    for (std::size_t index = 0; index < result.members.size(); ++index)
    {
        const auto& member = result.members[index];
        ec.clear();
        const bool rightType = member.kind == "file" ? std::filesystem::is_regular_file(member.path, ec)
                                                      : std::filesystem::is_directory(member.path, ec);
        if (!rightType || ec) return fail("source_member_type_mismatch");
        ec.clear();
        if (!std::filesystem::equivalent(member.path.parent_path(), result.directoryNodes[graph.memberParents[index]], ec) || ec)
            return fail("source_member_parent_mismatch");
        if (member.kind == "file")
        {
            std::string actual;
            if (!ReadPayloadFixtureFileChecked(member.path, actual)) return fail("source_file_read_failed");
            if (actual != member.bytes) return fail("source_file_contents_mismatch");
        }
        for (std::size_t prior = 0; prior < index; ++prior)
        {
            ec.clear();
            const bool same = std::filesystem::equivalent(member.path, result.members[prior].path, ec);
            if (ec || same != (member.identity == result.members[prior].identity))
                return fail("source_member_identity_mismatch");
        }
    }
    if (result.members.size() != static_cast<std::size_t>(count)) return fail("source_entry_count_mismatch");
    for (const auto node : graphNodes)
    {
        if (node != graph.isolatedRootParent)
        {
            ec.clear();
            if (!std::filesystem::equivalent(result.directoryNodes[node].parent_path(),
                    result.directoryNodes[static_cast<std::size_t>(graph.parent[node])], ec) || ec)
                return fail("graph_directory_parent_mismatch");
        }
        for (const auto other : graphNodes)
        {
            ec.clear();
            const bool same = std::filesystem::equivalent(result.directoryNodes[node], result.directoryNodes[other], ec);
            if (ec || same != (node == other)) return fail("graph_directory_identity_mismatch");
        }
        // The parent is only an identity anchor; its contents are outside the
        // owned fixture and are neither inspected nor modified here.
        if (node == graph.isolatedRootParent) continue;
        std::set<std::wstring> observed;
        ec.clear();
        for (std::filesystem::directory_iterator it(result.directoryNodes[node], ec), end; !ec && it != end; it.increment(ec))
            observed.insert(it->path().filename().wstring());
        if (ec || observed != expectedChildren[node]) return fail("graph_directory_contents_mismatch");
    }

    // A selected directory can contain other graph nodes and selected files.
    // Preserve an independent complete expectation for each directory member;
    // an empty bytes string does not imply an empty directory. Build the tree
    // from planned entities, then compare it with a physical recursive read.
    const auto relativeChild = [](const std::filesystem::path& child, const std::filesystem::path& base) {
        const auto relative = child.lexically_relative(base);
        if (relative.empty() || relative == std::filesystem::path(".")) return std::filesystem::path{};
        if (relative.begin() != relative.end() && *relative.begin() == std::filesystem::path(".."))
            return std::filesystem::path{};
        return relative;
    };
    const auto orderedTree = [](std::vector<PayloadFixtureTreeEntry>& tree) {
        std::sort(tree.begin(), tree.end(), [](const auto& a, const auto& b) {
            return a.relative.generic_wstring() < b.relative.generic_wstring();
        });
    };
    for (auto& selected : result.members)
    {
        if (selected.kind != "directory") continue;
        for (const auto node : graphNodes)
        {
            const auto relative = relativeChild(result.directoryNodes[node], selected.path);
            if (!relative.empty()) selected.tree.push_back({relative, "directory", {}});
        }
        for (std::size_t i = 0; i < result.members.size(); ++i)
        {
            const auto& member = result.members[i];
            if (member.kind != "file" || member.identity != i) continue;
            const auto relative = relativeChild(member.path, selected.path);
            if (!relative.empty()) selected.tree.push_back({relative, "file", member.bytes});
        }
        orderedTree(selected.tree);
        std::vector<PayloadFixtureTreeEntry> actual;
        ec.clear();
        for (std::filesystem::recursive_directory_iterator it(selected.path, ec), end; !ec && it != end; it.increment(ec))
        {
            const auto relative = relativeChild(it->path(), selected.path);
            if (relative.empty()) return fail("directory_tree_relative_path_mismatch");
            const auto status = it->symlink_status(ec);
            if (ec) return fail("directory_tree_status_failed");
            if (std::filesystem::is_directory(status)) actual.push_back({relative, "directory", {}});
            else if (std::filesystem::is_regular_file(status))
            {
                std::string bytes;
                if (!ReadPayloadFixtureFileChecked(it->path(), bytes)) return fail("directory_tree_file_read_failed");
                actual.push_back({relative, "file", bytes});
            }
            else return fail("directory_tree_entry_type_unimplemented");
        }
        if (ec) return fail("directory_tree_enumeration_failed");
        orderedTree(actual);
        if (actual.size() != selected.tree.size()) return fail("directory_tree_entry_count_mismatch");
        for (std::size_t i = 0; i < actual.size(); ++i)
            if (actual[i].relative != selected.tree[i].relative || actual[i].kind != selected.tree[i].kind ||
                actual[i].bytes != selected.tree[i].bytes) return fail("directory_tree_contents_mismatch");
    }

    // resolved_count/many_count describe the payload observed by production;
    // constructing a source, including when counts coincide, cannot prove it.
    mark("P02.source_actual_count");
    constexpr std::string_view graphInput = "isolated_graph_constructed_host_binding_unverified";
    mark("R02.target_directory_identity", graphInput);
    mark("R02.target_directory_parent_ref", graphInput);
    for (const auto endpoint : {"desktop_directory", "source_mapping_directory", "target_mapping_directory"})
    {
        mark(std::string("R02.") + endpoint + "_identity", graphInput);
        mark(std::string("R02.") + endpoint + "_parent_ref", graphInput);
    }
    for (int index = 0; index < count; ++index)
    {
        const auto member = "R02.member" + std::to_string(index);
        mark(member + "_directory_ref", graphInput);
        if (graph.memberParents[static_cast<std::size_t>(index)] != graph.isolatedRootParent)
            mark(member + "_directory_parent_ref", graphInput);
        mark(member + "_directory_object_ref", graphInput);
        const auto relation = get(member + "_directory_to_target");
        if (relation != "na" && relation != "unknown" && relation != "unresolved")
            mark(member + "_directory_to_target", graphInput);
    }
    if ((count == 0 && get("P01.primary") == "na") || count > 0) mark("P01.primary");
    if ((count < 2 && get("P01.second") == "na") || count >= 2) mark("P01.second");
    for (int index = 1; index < count; ++index)
        mark("P03.member" + std::to_string(index) + "_identity_ref");
    for (int index = 2; index < count; ++index)
        mark("P01.member" + std::to_string(index) + "_kind");
    result.payload_prepared = true;
    result.candidate_constructed = result.unresolved.empty();
    for (std::size_t i = 0; i < axes.size(); ++i)
        if (axes[i].role != Role::Derived && !result.axes[i].constructed) result.candidate_constructed = false;
    return result;
    }
    catch (const std::bad_alloc&)
    {
        result.payload_prepared = false;
        result.candidate_constructed = false;
        result.error = v3Context ? "v3_payload_resource_unavailable" : "payload_fixture_exception_unimplemented";
        return result;
    }
    catch (const std::length_error&)
    {
        result.payload_prepared = false;
        result.candidate_constructed = false;
        result.error = v3Context ? "v3_payload_resource_unavailable" : "payload_fixture_exception_unimplemented";
        return result;
    }
    catch (const std::exception&)
    {
        result.payload_prepared = false;
        result.candidate_constructed = false;
        result.error = "payload_fixture_exception_unimplemented";
        return result;
    }
    catch (...)
    {
        result.payload_prepared = false;
        result.candidate_constructed = false;
        result.error = "payload_fixture_unknown_exception_unimplemented";
        return result;
    }
}

// Input construction only; an authenticated raw coordinate and its local
// structural audits do not assert a passing product or complete family.
inline PayloadFixtureResult PrepareV3PayloadFixture(const V3FamilyValidatedContext& context,
    const std::filesystem::path& temporaryParent)
{
    return PreparePayloadFixture(context.Axes(), context.Coordinate(), temporaryParent, false,
        SelectionSchemaVersion::source_members_v2, &context);
}

// V4 input-only single-file subset. The caller owns and isolates the supplied
// directory and retains its handles until all production operations finish.
// File creation is relative to the verified directory handle; an unsupported
// literal path is unavailable rather than silently relocated into the fixture.
// Restricted read-only discovery of an existing drive-letter path reported
// as fixed/removable/RAM by Windows. The drive mapping and volume-root
// authority are caller assumptions; drive type alone is not provenance. No
// directory is created, removed, or claimed by this observation.
struct V4PinnedDirectoryChain
{
    std::vector<HANDLE> handles;
    std::vector<FILE_ID_INFO> identities;
    std::filesystem::path physicalPath;
    bool available = false;
    std::string_view reason;
    V4PinnedDirectoryChain() = default;
    V4PinnedDirectoryChain(const V4PinnedDirectoryChain&) = delete;
    V4PinnedDirectoryChain& operator=(const V4PinnedDirectoryChain&) = delete;
    V4PinnedDirectoryChain(V4PinnedDirectoryChain&& other) noexcept
        : handles(std::move(other.handles)), identities(std::move(other.identities)),
          physicalPath(std::move(other.physicalPath)), available(other.available), reason(other.reason)
    { other.handles.clear(); }
    ~V4PinnedDirectoryChain()
    { for (auto it = handles.rbegin(); it != handles.rend(); ++it) if (*it && *it != INVALID_HANDLE_VALUE) CloseHandle(*it); }
    HANDLE Parent() const noexcept { return !available || handles.empty() ? INVALID_HANDLE_VALUE : handles.back(); }
    const FILE_ID_INFO* ParentId() const noexcept { return !available || identities.empty() ? nullptr : &identities.back(); }
};
inline V4PinnedDirectoryChain OpenV4PinnedLocalDirectoryChain(const std::filesystem::path& input)
{
    V4PinnedDirectoryChain out;
    try
    {
        const auto raw = input.native();
        if (!input.is_absolute() || raw.find(L'\0') != std::wstring::npos || raw.size() < 3 ||
            !((raw[0] >= L'A' && raw[0] <= L'Z') || (raw[0] >= L'a' && raw[0] <= L'z')) ||
            raw[1] != L':' || (raw[2] != L'\\' && raw[2] != L'/'))
        { out.reason = "v4_chain_local_absolute_path_unavailable"; return out; }
        const auto rootPath = input.root_path();
        const auto driveType = GetDriveTypeW(rootPath.c_str());
        if (driveType != DRIVE_FIXED && driveType != DRIVE_REMOVABLE && driveType != DRIVE_RAMDISK)
        { out.reason = "v4_chain_drive_type_unavailable"; return out; }
        std::vector<std::wstring> parts;
        for (const auto& item : input.relative_path())
        {
            const auto part = item.native();
            if (part.empty() || part == L"." || part == L".." || part.back() == L'.' || part.back() == L' ' ||
                part.find_first_of(L"\\/:*?<>|\"") != std::wstring::npos ||
                part.size() > (std::numeric_limits<USHORT>::max)() / sizeof(wchar_t))
            { out.reason = "v4_chain_path_component_unavailable"; return out; }
            parts.push_back(part);
        }
        using NtCreate = NTSTATUS (NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
            PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
        const auto ntdll = GetModuleHandleW(L"ntdll.dll");
        const auto address = ntdll ? GetProcAddress(ntdll, "NtCreateFile") : nullptr;
        NtCreate open = nullptr; static_assert(sizeof(open) == sizeof(address));
        std::memcpy(&open, &address, sizeof(open));
        if (!open) { out.reason = "v4_chain_relative_open_api_unavailable"; return out; }
        const auto capture = [&](HANDLE h) {
            FILE_ATTRIBUTE_TAG_INFO tag{}; FILE_ID_INFO id{};
            if (!GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag, sizeof(tag)) ||
                !(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                !GetFileInformationByHandleEx(h, FileIdInfo, &id, sizeof(id)) ||
                !std::any_of(std::begin(id.FileId.Identifier), std::end(id.FileId.Identifier), [](BYTE b) { return b != 0; })) return false;
            out.identities.push_back(id); out.handles.push_back(h); return true;
        };
        struct HandleGuard { HANDLE h = INVALID_HANDLE_VALUE; ~HandleGuard() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); } };
        HandleGuard root{CreateFileW(rootPath.c_str(), FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE |
            (parts.empty() ? FILE_ADD_SUBDIRECTORY : 0), FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        if (root.h == INVALID_HANDLE_VALUE || !capture(root.h))
        { out.reason = "v4_chain_volume_root_unavailable"; return out; }
        root.h = INVALID_HANDLE_VALUE;
        constexpr ULONG fileOpen = 1, directory = 1, synchronousNonAlert = 0x20, openReparse = 0x00200000;
        for (std::size_t i = 0; i < parts.size(); ++i)
        {
            const auto& part = parts[i]; UNICODE_STRING name{};
            name.Buffer = const_cast<PWSTR>(part.data()); name.Length = static_cast<USHORT>(part.size() * sizeof(wchar_t));
            name.MaximumLength = name.Length;
            OBJECT_ATTRIBUTES attrs{}; attrs.Length = sizeof(attrs); attrs.RootDirectory = out.handles.back();
            attrs.ObjectName = &name; attrs.Attributes = 0x40;
            IO_STATUS_BLOCK ios{}; HandleGuard child;
            const auto status = open(&child.h, FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE |
                (i + 1 == parts.size() ? FILE_ADD_SUBDIRECTORY : 0), &attrs, &ios, nullptr, FILE_ATTRIBUTE_DIRECTORY,
                FILE_SHARE_READ | FILE_SHARE_WRITE, fileOpen, directory | synchronousNonAlert | openReparse, nullptr, 0);
            if (status < 0 || !child.h || child.h == INVALID_HANDLE_VALUE || !capture(child.h))
            { out.reason = "v4_chain_component_unavailable_or_reparse"; return out; }
            child.h = INVALID_HANDLE_VALUE;
        }
        for (std::size_t i = 0; i < out.handles.size(); ++i)
        {
            FILE_ATTRIBUTE_TAG_INFO tag{}; FILE_ID_INFO id{};
            if (!GetFileInformationByHandleEx(out.handles[i], FileAttributeTagInfo, &tag, sizeof(tag)) ||
                !(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                !GetFileInformationByHandleEx(out.handles[i], FileIdInfo, &id, sizeof(id)) ||
                id.VolumeSerialNumber != out.identities[i].VolumeSerialNumber ||
                std::memcmp(id.FileId.Identifier, out.identities[i].FileId.Identifier, sizeof(id.FileId.Identifier)) != 0)
            { out.reason = "v4_chain_component_changed_during_open"; return out; }
        }
        const auto required = GetFinalPathNameByHandleW(out.handles.back(), nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (!required) { out.reason = "v4_chain_physical_path_unavailable"; return out; }
        std::wstring path(required, L'\0');
        const auto length = GetFinalPathNameByHandleW(out.handles.back(), path.data(), required, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (!length || length >= required) { out.reason = "v4_chain_physical_path_unavailable"; return out; }
        path.resize(length); out.physicalPath = std::move(path);
        out.available = true; out.reason = "v4_chain_volume_and_mapping_trust_required";
        return out;
    }
    catch (...) { out.available = false; out.reason = "v4_chain_resource_or_observation_unavailable"; return out; }
}

// A caller-authenticated parent object and its ancestors must remain stable.
// Creation is relative to that live object; no path-based ownership inference
// or cleanup is attempted. Even a partially observed creation returns its
// retained handle and flags to the caller for conservative reconciliation.
struct V4OwnedChildDirectoryAttempt
{
    HANDLE handle = INVALID_HANDLE_VALUE;
    FILE_ID_INFO identity{};
    std::filesystem::path physicalPath;
    // attempted && !created is not proof that no side effect occurred.
    bool attempted = false, created = false, identityObserved = false, available = false;
    std::string_view reason;
    V4OwnedChildDirectoryAttempt() = default;
    V4OwnedChildDirectoryAttempt(const V4OwnedChildDirectoryAttempt&) = delete;
    V4OwnedChildDirectoryAttempt& operator=(const V4OwnedChildDirectoryAttempt&) = delete;
    V4OwnedChildDirectoryAttempt(V4OwnedChildDirectoryAttempt&& other) noexcept
        : handle(std::exchange(other.handle, INVALID_HANDLE_VALUE)), identity(other.identity),
          physicalPath(std::move(other.physicalPath)), attempted(other.attempted), created(other.created),
          identityObserved(other.identityObserved), available(other.available), reason(other.reason) {}
    ~V4OwnedChildDirectoryAttempt() { if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
};
inline V4OwnedChildDirectoryAttempt CreateV4OwnedChildDirectory(HANDLE trustedParent,
    const FILE_ID_INFO& trustedParentId, std::wstring_view leaf)
{
    V4OwnedChildDirectoryAttempt out;
    try
    {
        if (!trustedParent || trustedParent == INVALID_HANDLE_VALUE || leaf.empty() || leaf == L"." || leaf == L".." ||
            leaf.back() == L'.' || leaf.back() == L' ' || leaf.find_first_of(L"\\/:*?<>|\"") != std::wstring_view::npos ||
            leaf.find(L'\0') != std::wstring_view::npos || leaf.size() > (std::numeric_limits<USHORT>::max)() / sizeof(wchar_t))
        { out.reason = "v4_child_parent_or_name_unavailable"; return out; }
        const auto sameId = [](const FILE_ID_INFO& a, const FILE_ID_INFO& b) {
            return a.VolumeSerialNumber == b.VolumeSerialNumber &&
                std::memcmp(a.FileId.Identifier, b.FileId.Identifier, sizeof(a.FileId.Identifier)) == 0 &&
                std::any_of(std::begin(a.FileId.Identifier), std::end(a.FileId.Identifier), [](BYTE x) { return x != 0; });
        };
        const auto directoryId = [](HANDLE handle, FILE_ID_INFO& id) {
            FILE_ATTRIBUTE_TAG_INFO tag{};
            return GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag)) &&
                (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id));
        };
        FILE_ID_INFO parent{};
        if (!directoryId(trustedParent, parent) || !sameId(parent, trustedParentId))
        { out.reason = "v4_child_parent_identity_unavailable"; return out; }
        using NtCreate = NTSTATUS (NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
            PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
        const auto ntdll = GetModuleHandleW(L"ntdll.dll");
        const auto address = ntdll ? GetProcAddress(ntdll, "NtCreateFile") : nullptr;
        NtCreate create = nullptr; static_assert(sizeof(create) == sizeof(address));
        std::memcpy(&create, &address, sizeof(create));
        if (!create) { out.reason = "v4_child_relative_create_api_unavailable"; return out; }
        UNICODE_STRING name{}; name.Buffer = const_cast<PWSTR>(leaf.data());
        name.Length = static_cast<USHORT>(leaf.size() * sizeof(wchar_t)); name.MaximumLength = name.Length;
        OBJECT_ATTRIBUTES attrs{}; attrs.Length = sizeof(attrs); attrs.RootDirectory = trustedParent;
        attrs.ObjectName = &name; attrs.Attributes = 0x40; // OBJ_CASE_INSENSITIVE
        IO_STATUS_BLOCK ios{};
        constexpr ULONG fileCreate = 2, fileOpen = 1, directory = 1, synchronousNonAlert = 0x20, openReparse = 0x00200000;
        out.attempted = true;
        const auto status = create(&out.handle, FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY |
            FILE_READ_ATTRIBUTES | SYNCHRONIZE, &attrs, &ios, nullptr, FILE_ATTRIBUTE_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE, fileCreate, directory | synchronousNonAlert | openReparse, nullptr, 0);
        if (status < 0 || !out.handle || out.handle == INVALID_HANDLE_VALUE)
        { out.reason = "v4_child_relative_create_unavailable"; return out; }
        out.created = true;
        if (!directoryId(out.handle, out.identity) || !sameId(out.identity, out.identity))
        { out.reason = "v4_child_created_identity_unavailable"; return out; }
        out.identityObserved = true;
        HANDLE verify = INVALID_HANDLE_VALUE;
        const auto reopen = create(&verify, FILE_READ_ATTRIBUTES | SYNCHRONIZE, &attrs, &ios, nullptr, FILE_ATTRIBUTE_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE, fileOpen, directory | synchronousNonAlert | openReparse, nullptr, 0);
        struct CloseHandleGuard { HANDLE h; ~CloseHandleGuard() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); } } guard{verify};
        FILE_ID_INFO named{}, after{};
        if (reopen < 0 || !verify || verify == INVALID_HANDLE_VALUE || !directoryId(verify, named) ||
            !sameId(named, out.identity) || !directoryId(trustedParent, after) || !sameId(after, trustedParentId))
        { out.reason = "v4_child_name_or_parent_identity_unavailable"; return out; }
        const auto required = GetFinalPathNameByHandleW(out.handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (!required) { out.reason = "v4_child_physical_path_unavailable"; return out; }
        std::wstring path(required, L'\0');
        const auto length = GetFinalPathNameByHandleW(out.handle, path.data(), required, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (!length || length >= required) { out.reason = "v4_child_physical_path_unavailable"; return out; }
        path.resize(length); out.physicalPath = std::move(path);
        out.available = true; out.reason = "v4_child_created_parent_trust_required";
        return out;
    }
    catch (...) { out.available = false; out.reason = "v4_child_create_resource_or_observation_unavailable"; return out; }
}

// Construct an exact input slot before a family/candidate identity exists.
// The caller must already own the isolated root and keep its no-delete-share
// lease alive, including across replay. This helper performs no filesystem
// creation and does not rebase an existing candidate onto a different root.
struct V4OwnedRootPathSeed
{
    bool available = false;
    std::filesystem::path path;
    V4IndexedUtf16Slot slot;
    std::string_view reason;
};
inline V4OwnedRootPathSeed MakeV4OwnedRootPathSeed(const std::filesystem::path& rootPath,
    std::wstring_view leaf, HANDLE trustedRoot, const FILE_ID_INFO& trustedRootId)
{
    V4OwnedRootPathSeed result;
    try
    {
        if (!rootPath.is_absolute() || rootPath.native().find(L'\0') != std::wstring::npos || leaf.empty() || leaf == L"." || leaf == L".." ||
            leaf.back() == L'.' || leaf.back() == L' ' || leaf.find_first_of(L"\\/:*?<>|\"") != std::wstring_view::npos ||
            leaf.find(L'\0') != std::wstring_view::npos)
        { result.reason = "v4_seed_root_path_or_leaf_unavailable"; return result; }
        const auto sameId = [](const FILE_ID_INFO& a, const FILE_ID_INFO& b) {
            return a.VolumeSerialNumber == b.VolumeSerialNumber &&
                std::memcmp(a.FileId.Identifier, b.FileId.Identifier, sizeof(a.FileId.Identifier)) == 0 &&
                std::any_of(std::begin(a.FileId.Identifier), std::end(a.FileId.Identifier), [](BYTE x) { return x != 0; });
        };
        FILE_ID_INFO held{}; FILE_ATTRIBUTE_TAG_INFO tag{};
        if (!trustedRoot || trustedRoot == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandleEx(trustedRoot, FileIdInfo, &held, sizeof(held)) || !sameId(held, trustedRootId) ||
            !GetFileInformationByHandleEx(trustedRoot, FileAttributeTagInfo, &tag, sizeof(tag)) ||
            !(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        { result.reason = "v4_seed_trusted_root_unavailable"; return result; }
        struct HandleGuard { HANDLE h; ~HandleGuard() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); } };
        HandleGuard pathRoot{CreateFileW(rootPath.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        FILE_ID_INFO named{};
        if (pathRoot.h == INVALID_HANDLE_VALUE || !GetFileInformationByHandleEx(pathRoot.h, FileIdInfo, &named, sizeof(named)) ||
            !sameId(named, trustedRootId) || !GetFileInformationByHandleEx(pathRoot.h, FileAttributeTagInfo, &tag, sizeof(tag)) ||
            !(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        { result.reason = "v4_seed_root_path_identity_unavailable"; return result; }
        result.path = rootPath / std::wstring(leaf);
        if (!result.path.is_absolute() || result.path.parent_path() != rootPath)
        { result.reason = "v4_seed_exact_path_unavailable"; return result; }
        static_assert(sizeof(wchar_t) == 2, "Windows UTF-16 code units required");
        constexpr char digits[] = "0123456789abcdef";
        const auto pathText = result.path.native();
        result.slot.index = "0"; result.slot.kind = V4PayloadKind::exact;
        for (const auto unit : pathText)
        {
            const auto value = static_cast<std::uint16_t>(unit);
            for (unsigned shift : {0u, 8u})
            { const auto byte = (value >> shift) & 0xffu; result.slot.utf16leHex.push_back(digits[byte >> 4]); result.slot.utf16leHex.push_back(digits[byte & 15]); }
        }
        result.available = true; result.reason = "v4_seed_slot_only_candidate_not_registered";
        return result;
    }
    catch (...) { result.available = false; result.reason = "v4_seed_path_observation_unavailable"; return result; }
}

struct V4OwnedRelativePathMember
{
    std::size_t physicalIndex = 0;
    std::string relativeUtf16leHex, physicalUtf16leHex;
    std::filesystem::path physicalPath;
};
struct V4OwnedRelativePathBindingResult;
class V4OwnedRelativePathBinding final
{
    friend V4OwnedRelativePathBindingResult ObserveV4OwnedRelativePaths(const V4FamilyDescriptor&,
        const V4FamilyValidatedContext&, const event_graph::V4FamilyIdentityContext&, const std::filesystem::path&,
        HANDLE, const FILE_ID_INFO&, std::size_t, std::size_t);
    HANDLE root_ = INVALID_HANDLE_VALUE, source_ = INVALID_HANDLE_VALUE;
    FILE_ID_INFO rootId_{};
    std::filesystem::path rootPath_;
    std::string candidateId_, familyBinding_;
    std::vector<V4OwnedRelativePathMember> members_;
    V4OwnedRelativePathBinding() = default;
public:
    V4OwnedRelativePathBinding(const V4OwnedRelativePathBinding&) = delete;
    V4OwnedRelativePathBinding& operator=(const V4OwnedRelativePathBinding&) = delete;
    ~V4OwnedRelativePathBinding()
    { if (root_ && root_ != INVALID_HANDLE_VALUE) CloseHandle(root_); if (source_ && source_ != INVALID_HANDLE_VALUE) CloseHandle(source_); }
    const std::string& CandidateId() const noexcept { return candidateId_; }
    const std::string& FamilyBinding() const noexcept { return familyBinding_; }
    const std::vector<V4OwnedRelativePathMember>& Members() const noexcept { return members_; }
    const FILE_ID_INFO& RootId() const noexcept { return rootId_; }
    const std::filesystem::path& RootPath() const noexcept { return rootPath_; }
    bool StillBound() const noexcept
    {
        const auto same = [](const FILE_ID_INFO& a, const FILE_ID_INFO& b) {
            return a.VolumeSerialNumber == b.VolumeSerialNumber &&
                std::memcmp(a.FileId.Identifier, b.FileId.Identifier, sizeof(a.FileId.Identifier)) == 0 &&
                std::any_of(std::begin(a.FileId.Identifier), std::end(a.FileId.Identifier), [](BYTE x) { return x != 0; });
        };
        FILE_ID_INFO pinned{}, source{}, named{}; FILE_ATTRIBUTE_TAG_INFO tag{};
        if (!root_ || root_ == INVALID_HANDLE_VALUE || !source_ || source_ == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandleEx(root_, FileIdInfo, &pinned, sizeof(pinned)) || !same(pinned, rootId_) ||
            !GetFileInformationByHandleEx(source_, FileIdInfo, &source, sizeof(source)) || !same(source, rootId_) ||
            !GetFileInformationByHandleEx(root_, FileAttributeTagInfo, &tag, sizeof(tag)) ||
            !(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        const HANDLE namedRoot = CreateFileW(rootPath_.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (namedRoot == INVALID_HANDLE_VALUE) return false;
        const bool valid = GetFileInformationByHandleEx(namedRoot, FileIdInfo, &named, sizeof(named)) && same(named, rootId_) &&
            GetFileInformationByHandleEx(namedRoot, FileAttributeTagInfo, &tag, sizeof(tag)) &&
            (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
        CloseHandle(namedRoot); return valid;
    }
};
struct V4OwnedRelativePathBindingResult
{
    std::shared_ptr<const V4OwnedRelativePathBinding> binding;
    bool resourceUnavailable = false;
    std::string_view reason;
};
namespace v3_scheduler { class V4ExactOwnedFilesPermit; }
class V4RelativePathMaterializationKey final
{
    friend class v3_scheduler::V4ExactOwnedFilesPermit;
    std::shared_ptr<const V4OwnedRelativePathBinding> binding_;
    explicit V4RelativePathMaterializationKey(std::shared_ptr<const V4OwnedRelativePathBinding> binding) noexcept
        : binding_(std::move(binding)) {}
public:
    V4RelativePathMaterializationKey(const V4RelativePathMaterializationKey&) = delete;
    V4RelativePathMaterializationKey& operator=(const V4RelativePathMaterializationKey&) = delete;
    bool Matches(const std::shared_ptr<const V4OwnedRelativePathBinding>& binding) const noexcept
    { return binding && binding_.get() == binding.get() && !binding_.owner_before(binding) && !binding.owner_before(binding_); }
};

// This only observes the root-to-relative-input mapping for the same model
// candidate; it does not create files or authorize a production operation.
// Only single-component names are supported; all other paths stay pending.
inline V4OwnedRelativePathBindingResult ObserveV4OwnedRelativePaths(const V4FamilyDescriptor& family,
    const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
    const std::filesystem::path& rootPath, HANDLE trustedRoot, const FILE_ID_INFO& rootId,
    std::size_t maxFiles, std::size_t maxBytes)
{
    V4OwnedRelativePathBindingResult out;
    try
    {
        if (family.b || verified.TaskBActive() || verified.Status() != V4CoordinateStatus::coordinate_ready ||
            identity.Status() != V4FamilyStatus::schema_only || V4FamilyBinding(family) != identity.FamilyBinding() ||
            verified.FamilyBinding() != identity.FamilyBinding() || verified.Axes().size() != identity.Axes().size())
        { out.reason = "v4_relative_binding_context_unavailable"; return out; }
        for (std::size_t i = 0; i < verified.Axes().size(); ++i)
        {
            const auto& a = verified.Axes()[i]; const auto& b = identity.Axes()[i];
            if (a.id != b.id || a.role != b.role || a.label != b.label || a.values != b.values)
            { out.reason = "v4_relative_binding_schema_unavailable"; return out; }
        }
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        if (raw.resourceUnavailable) { out.resourceUnavailable = true; out.reason = "v4_relative_binding_identity_resource_unavailable"; return out; }
        if (!raw.errors.empty() || raw.id.empty() || family.a.fileCount.kind != V3CountKind::exact)
        { out.reason = "v4_relative_binding_identity_or_count_unavailable"; return out; }
        const auto count = V4MaterializeCount(family.a.fileCount, maxFiles, false);
        if (count.status == V4AxisStatus::resource_unavailable)
        { out.resourceUnavailable = true; out.reason = "v4_relative_binding_count_budget_unavailable"; return out; }
        if (count.status != V4AxisStatus::schema_only || !count.value || family.a.filePaths.size() != count.value)
        { out.reason = "v4_relative_binding_path_count_unavailable"; return out; }
        auto binding = std::shared_ptr<V4OwnedRelativePathBinding>(new V4OwnedRelativePathBinding());
        binding->rootPath_ = rootPath; binding->rootId_ = rootId;
        binding->root_ = CreateFileW(rootPath.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (!DuplicateHandle(GetCurrentProcess(), trustedRoot, GetCurrentProcess(), &binding->source_, 0, FALSE, DUPLICATE_SAME_ACCESS) ||
            !binding->StillBound()) { out.reason = "v4_relative_binding_root_unavailable"; return out; }
        binding->candidateId_ = raw.id; binding->familyBinding_ = identity.FamilyBinding();
        binding->members_.reserve(count.value);
        std::size_t bytes = 0;
        const auto digit = [](char c) -> int { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
        for (std::size_t i = 0; i < count.value; ++i)
        {
            const auto index = std::to_string(i);
            const auto slot = std::find_if(family.a.filePaths.begin(), family.a.filePaths.end(),
                [&](const auto& item) { return item.index == index; });
            if (slot == family.a.filePaths.end() || slot->base != V4PathBase::owned_root_relative || slot->kind != V4PayloadKind::exact ||
                Value(verified.Axes(), verified.Coordinate(), "V4.file.member" + index + "_kind") != "file" ||
                Value(verified.Axes(), verified.Coordinate(), "V4.file.member" + index + "_identity") != "unique" ||
                Value(verified.Axes(), verified.Coordinate(), "V4.file_path.index_" + index + ".utf16le") !=
                    "owned_root_relative_utf16le_hex_" + slot->utf16leHex)
            { out.reason = "v4_relative_binding_path_unimplemented"; return out; }
            // Budget the retained relative hex, physical UTF-16 path, and
            // physical hex separately; every member repeats the root prefix.
            const auto relativeBytes = slot->utf16leHex.size();
            const auto rootUnits = rootPath.native().size();
            const auto leafUnits = slot->utf16leHex.size() / 4;
            if (rootUnits == (std::numeric_limits<std::size_t>::max)() ||
                leafUnits > (std::numeric_limits<std::size_t>::max)() - rootUnits - 1)
            { out.resourceUnavailable = true; out.reason = "v4_relative_binding_byte_budget_unavailable"; return out; }
            const auto physicalUnits = rootUnits + 1 + leafUnits;
            constexpr auto perUnit = sizeof(wchar_t) + 4u;
            if (relativeBytes > maxBytes - bytes || physicalUnits > (maxBytes - bytes - relativeBytes) / perUnit)
            { out.resourceUnavailable = true; out.reason = "v4_relative_binding_byte_budget_unavailable"; return out; }
            bytes += relativeBytes + physicalUnits * perUnit;
            std::wstring leaf; leaf.reserve(slot->utf16leHex.size() / 4);
            for (std::size_t j = 0; j < slot->utf16leHex.size(); j += 4)
            {
                const int a = digit(slot->utf16leHex[j]), b = digit(slot->utf16leHex[j + 1]);
                const int c = digit(slot->utf16leHex[j + 2]), d = digit(slot->utf16leHex[j + 3]);
                if (a < 0 || b < 0 || c < 0 || d < 0 || !(a || b || c || d))
                { out.reason = "v4_relative_binding_encoding_unavailable"; return out; }
                leaf.push_back(static_cast<wchar_t>(((a << 4) | b) | (((c << 4) | d) << 8)));
            }
            const auto named = MakeV4OwnedRootPathSeed(rootPath, leaf, binding->source_, rootId);
            if (!named.available) { out.reason = named.reason; return out; }
            if (leaf.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
            { out.reason = "v4_relative_binding_leaf_unavailable"; return out; }
            for (const auto& prior : binding->members_)
            {
                const auto previous = prior.physicalPath.filename().native();
                const auto compared = CompareStringOrdinal(previous.data(), static_cast<int>(previous.size()),
                    leaf.data(), static_cast<int>(leaf.size()), TRUE);
                if (!compared || compared == CSTR_EQUAL) { out.reason = "v4_relative_binding_alias_unimplemented"; return out; }
            }
            binding->members_.push_back({i, slot->utf16leHex, named.slot.utf16leHex, named.path});
        }
        if (!binding->StillBound()) { out.reason = "v4_relative_binding_root_changed"; return out; }
        out.binding = std::move(binding);
        out.reason = "v4_relative_binding_only_length_volume_alias_and_production_unresolved";
        return out;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "v4_relative_binding_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "v4_relative_binding_resource_unavailable"; return out; }
    catch (...) { out.reason = "v4_relative_binding_observation_unavailable"; return out; }
}

// A host-owned reader supplies actual settings and actual DesktopItem target
// values. Only NativeDragIntegrationAccess may create this binding; the
// caller must bind its Profile/epoch and member identities when implementing
// the reader. A snapshot is local evidence, not a production outcome.
class V4CategoryObservationSource final
{
    friend struct ::NativeDragIntegrationAccess;
    friend struct V4CategoryInputObserver;
public:
    using Targets = std::vector<std::optional<snowdesktop::category_collection_rules::ShortcutTarget>>;
    using Reader = std::function<bool(std::size_t, std::size_t, std::size_t, CategorySettings&, Targets&, bool&)>;
private:
    std::shared_ptr<const void> hostLease_;
    std::string familyBinding_, candidateId_;
    Reader read_;
    DWORD thread_ = 0;
    V4CategoryObservationSource(std::shared_ptr<const void> hostLease, std::string familyBinding,
        std::string candidateId, Reader reader)
        : hostLease_(std::move(hostLease)), familyBinding_(std::move(familyBinding)), candidateId_(std::move(candidateId)),
          read_(std::move(reader)), thread_(GetCurrentThreadId()) {}
public:
    V4CategoryObservationSource(const V4CategoryObservationSource&) = delete;
    V4CategoryObservationSource& operator=(const V4CategoryObservationSource&) = delete;
};
struct V4ObservedCategoryRule
{
    std::string idUtf16Hex, labelUtf16Hex, extensionsUtf16Hex;
    bool enabled = false;
    std::optional<bool> idMatches, labelMatches, extensionsMatches, enabledMatches;
};
struct V4ObservedShortcutTarget
{
    std::size_t logicalIndex = 0;
    bool observed = false, classified = false, directory = false, application = false;
    std::string extensionUtf16Hex;
    std::optional<bool> classifiedMatches, directoryMatches, applicationMatches, extensionMatches;
};
struct V4CategoryInputObservation
{
    bool identityValid = false, sourceRead = false, declaredSlotsObserved = false, resourceUnavailable = false;
    std::optional<bool> ruleCountMatches;
    std::vector<V4ObservedCategoryRule> rules;
    std::vector<V4ObservedShortcutTarget> shortcuts;
    std::string_view reason;
};
struct V4CategoryInputObserver
{
    static V4CategoryInputObservation Observe(const V4FamilyValidatedContext& verified,
        const event_graph::V4FamilyIdentityContext& identity, const V4CategoryObservationSource& source,
        std::size_t maxRules, std::size_t maxMembers, std::size_t maxUtf16Bytes)
    {
        V4CategoryInputObservation out;
        try
        {
            if (verified.Status() != V4CoordinateStatus::coordinate_ready || identity.Status() != V4FamilyStatus::schema_only ||
                verified.FamilyBinding() != identity.FamilyBinding() || source.familyBinding_ != identity.FamilyBinding() ||
                !source.hostLease_ || !source.read_ || source.thread_ != GetCurrentThreadId() ||
                verified.Axes().size() != identity.Axes().size())
            { out.reason = "v4_category_observation_context_unavailable"; return out; }
            for (std::size_t i = 0; i < verified.Axes().size(); ++i)
            {
                const auto& a = verified.Axes()[i]; const auto& b = identity.Axes()[i];
                if (a.id != b.id || a.role != b.role || a.label != b.label || a.values != b.values)
                { out.reason = "v4_category_observation_schema_unavailable"; return out; }
            }
            const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
            if (raw.resourceUnavailable) { out.resourceUnavailable = true; out.reason = "v4_category_observation_identity_resource_unavailable"; return out; }
            if (!raw.errors.empty() || raw.id.empty() || raw.id != source.candidateId_)
            { out.reason = "v4_category_observation_candidate_unavailable"; return out; }
            out.identityValid = true;
            CategorySettings settings; V4CategoryObservationSource::Targets targets; bool resource = false;
            if (!source.read_(maxRules, maxMembers, maxUtf16Bytes, settings, targets, resource))
            { out.resourceUnavailable = resource; out.reason = "v4_category_observation_source_unavailable"; return out; }
            out.sourceRead = true;
            if (settings.rules.size() > maxRules || targets.size() > maxMembers)
            { out.resourceUnavailable = true; out.reason = "v4_category_observation_count_budget_unavailable"; return out; }
            const auto& axes = verified.Axes(); const auto& candidate = verified.Coordinate();
            const auto value = [&](std::string_view id) { return Value(axes, candidate, id); };
            std::size_t bytes = 0;
            const auto hex = [&](std::wstring_view text, std::string& result) {
                if (text.size() > (maxUtf16Bytes - bytes) / sizeof(wchar_t)) return false;
                bytes += text.size() * sizeof(wchar_t);
                if (text.size() > result.max_size() / 4) return false;
                constexpr char digits[] = "0123456789abcdef";
                result.reserve(text.size() * 4);
                for (auto ch : text)
                { const auto unit = static_cast<std::uint16_t>(ch);
                  for (unsigned shift : {0u, 8u}) { const auto byte = (unit >> shift) & 255u;
                    result.push_back(digits[byte >> 4]); result.push_back(digits[byte & 15]); } }
                return true;
            };
            const auto compareText = [](std::string_view expected, const std::string& actual) -> std::optional<bool> {
                constexpr std::string_view prefix = "utf16le_hex_";
                if (!expected.starts_with(prefix)) return std::nullopt;
                return expected.substr(prefix.size()) == actual;
            };
            const auto compareBool = [](std::string_view expected, bool actual) -> std::optional<bool> {
                if (expected == "yes") return actual;
                if (expected == "no") return !actual;
                return std::nullopt;
            };
            std::size_t expectedRules = 0;
            for (const auto& axis : axes)
                if (axis.id.starts_with("V4.category_rules.rule_") && axis.id.ends_with(".id")) ++expectedRules;
            const auto ruleKind = value("V4.category_rules.kind");
            if (ruleKind == "exact") out.ruleCountMatches = settings.rules.size() == expectedRules;
            out.rules.reserve(settings.rules.size());
            for (std::size_t i = 0; i < settings.rules.size(); ++i)
            {
                const auto& input = settings.rules[i]; V4ObservedCategoryRule record; record.enabled = input.enabled;
                if (!hex(input.id, record.idUtf16Hex) || !hex(input.customLabel, record.labelUtf16Hex) ||
                    !hex(input.extensions, record.extensionsUtf16Hex))
                { out.resourceUnavailable = true; out.reason = "v4_category_observation_byte_budget_unavailable"; return out; }
                const auto base = "V4.category_rules.rule_" + std::to_string(i);
                if (ruleKind == "exact" && i < expectedRules)
                { record.idMatches = compareText(value(base + ".id"), record.idUtf16Hex);
                  record.labelMatches = compareText(value(base + ".label"), record.labelUtf16Hex);
                  record.extensionsMatches = compareText(value(base + ".extensions"), record.extensionsUtf16Hex);
                  record.enabledMatches = compareBool(value(base + ".enabled"), record.enabled); }
                out.rules.push_back(std::move(record));
            }
            bool declaredSlotsConsistent = true;
            out.shortcuts.reserve(targets.size());
            for (std::size_t i = 0; i < targets.size(); ++i)
            {
                V4ObservedShortcutTarget record; record.logicalIndex = i;
                if (targets[i])
                {
                    const auto& actual = *targets[i]; record.observed = true; record.classified = actual.classified;
                    record.directory = actual.directory; record.application = actual.application;
                    if (!hex(actual.extension, record.extensionUtf16Hex))
                    { out.resourceUnavailable = true; out.reason = "v4_shortcut_observation_byte_budget_unavailable"; return out; }
                    const auto base = "V4.shortcut_target.member_" + std::to_string(i);
                    const auto exists = [&](std::string_view suffix) {
                        const auto id = base + std::string(suffix);
                        return std::any_of(axes.begin(), axes.end(), [&](const Axis& axis) { return axis.id == id; });
                    };
                    const bool a = exists(".classified"), b = exists(".directory"), c = exists(".application"), d = exists(".extension");
                    if (a && b && c && d)
                    { record.classifiedMatches = compareBool(value(base + ".classified"), record.classified);
                      record.directoryMatches = compareBool(value(base + ".directory"), record.directory);
                      record.applicationMatches = compareBool(value(base + ".application"), record.application);
                      record.extensionMatches = compareText(value(base + ".extension"), record.extensionUtf16Hex); }
                    else if (a || b || c || d) declaredSlotsConsistent = false;
                }
                out.shortcuts.push_back(std::move(record));
            }
            if (!source.hostLease_ || source.thread_ != GetCurrentThreadId())
            { out.reason = "v4_category_observation_host_changed"; return out; }
            bool membersComplete = !verified.TaskBActive() && declaredSlotsConsistent;
            constexpr std::string_view memberPrefix = "V4.shortcut_target.member_", suffix = ".classified";
            for (const auto& axis : axes)
            {
                if (!axis.id.starts_with(memberPrefix) || !axis.id.ends_with(suffix)) continue;
                const auto number = std::string_view(axis.id).substr(memberPrefix.size(), axis.id.size() - memberPrefix.size() - suffix.size());
                std::size_t index = 0;
                if (number.empty()) { membersComplete = false; continue; }
                for (char ch : number)
                {
                    if (ch < '0' || ch > '9' || index > ((std::numeric_limits<std::size_t>::max)() - static_cast<unsigned>(ch - '0')) / 10)
                    { membersComplete = false; index = targets.size(); break; }
                    index = index * 10 + static_cast<unsigned>(ch - '0');
                }
                if (index >= targets.size() || !targets[index]) membersComplete = false;
            }
            out.declaredSlotsObserved = membersComplete;
            out.reason = membersComplete ? "v4_category_input_snapshot_only_not_business_evidence" :
                "v4_category_input_snapshot_member_or_task_b_unavailable";
            return out;
        }
        catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "v4_category_observation_resource_unavailable"; return out; }
        catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "v4_category_observation_resource_unavailable"; return out; }
        catch (...) { out.reason = "v4_category_observation_unavailable"; return out; }
    }
};

// A fresh family is made only after the owned root has been pinned. Existing
// families/cases are never rebound or rewritten by this input helper.
struct V4OwnedFileFamilySeedResult;
class V4OwnedFileFamilySeed final
{
    friend struct V4OwnedFileFamilySeedResult;
    friend V4OwnedFileFamilySeedResult PrepareV4OwnedFileFamilySeed(const V4TaskSpec&, const std::filesystem::path&,
        std::wstring_view, HANDLE, const FILE_ID_INFO&, const V4CoordinateBudget&, const V4AxisBudget&);
    friend V4OwnedFileFamilySeedResult PrepareV4OwnedMultiFileFamilySeed(const V4TaskSpec&, const std::filesystem::path&,
        const std::vector<std::wstring>&, HANDLE, const FILE_ID_INFO&, const V4CoordinateBudget&, const V4AxisBudget&);
    HANDLE root_ = INVALID_HANDLE_VALUE, trustedRoot_ = INVALID_HANDLE_VALUE;
    FILE_ID_INFO rootId_{};
    std::filesystem::path rootPath_;
    V4FamilyDescriptor family_;
    std::string coordinate_, binding_;
    V4OwnedFileFamilySeed() = default;
public:
    V4OwnedFileFamilySeed(const V4OwnedFileFamilySeed&) = delete;
    V4OwnedFileFamilySeed& operator=(const V4OwnedFileFamilySeed&) = delete;
    ~V4OwnedFileFamilySeed()
    { if (root_ && root_ != INVALID_HANDLE_VALUE) CloseHandle(root_);
      if (trustedRoot_ && trustedRoot_ != INVALID_HANDLE_VALUE) CloseHandle(trustedRoot_); }
    const V4FamilyDescriptor& Family() const noexcept { return family_; }
    const std::string& Coordinate() const noexcept { return coordinate_; }
    const std::string& Binding() const noexcept { return binding_; }
    HANDLE Root() const noexcept { return root_; } // borrowed, valid only while this object lives
    // Same access as the caller's original trusted root; creation may still
    // fail for lack of rights or filesystem support, which is unavailable.
    HANDLE CreationRoot() const noexcept { return trustedRoot_; }
    const FILE_ID_INFO& RootId() const noexcept { return rootId_; }
    const std::filesystem::path& RootPath() const noexcept { return rootPath_; }
    bool StillOwned() const noexcept
    {
        if (!root_ || root_ == INVALID_HANDLE_VALUE || !trustedRoot_ || trustedRoot_ == INVALID_HANDLE_VALUE) return false;
        FILE_ID_INFO held{}, trusted{}, named{}; FILE_ATTRIBUTE_TAG_INFO tag{};
        const auto same = [](const FILE_ID_INFO& a, const FILE_ID_INFO& b) {
            return a.VolumeSerialNumber == b.VolumeSerialNumber &&
                std::memcmp(a.FileId.Identifier, b.FileId.Identifier, sizeof(a.FileId.Identifier)) == 0 &&
                std::any_of(std::begin(a.FileId.Identifier), std::end(a.FileId.Identifier), [](BYTE x) { return x != 0; });
        };
        if (!GetFileInformationByHandleEx(root_, FileIdInfo, &held, sizeof(held)) || !same(held, rootId_) ||
            !GetFileInformationByHandleEx(trustedRoot_, FileIdInfo, &trusted, sizeof(trusted)) || !same(trusted, rootId_) ||
            !GetFileInformationByHandleEx(root_, FileAttributeTagInfo, &tag, sizeof(tag)) ||
            !(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        const HANDLE path = CreateFileW(rootPath_.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (path == INVALID_HANDLE_VALUE) return false;
        const bool valid = GetFileInformationByHandleEx(path, FileIdInfo, &named, sizeof(named)) && same(named, rootId_) &&
            GetFileInformationByHandleEx(path, FileAttributeTagInfo, &tag, sizeof(tag)) &&
            (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
        CloseHandle(path); return valid;
    }
};
struct V4OwnedFileFamilySeedResult
{
    std::shared_ptr<const V4OwnedFileFamilySeed> seed;
    bool resourceUnavailable = false;
    std::string_view reason;
};
inline V4OwnedFileFamilySeedResult PrepareV4OwnedFileFamilySeed(const V4TaskSpec& draft,
    const std::filesystem::path& rootPath, std::wstring_view leaf, HANDLE trustedRoot, const FILE_ID_INFO& rootId,
    const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget)
{
    V4OwnedFileFamilySeedResult out;
    try
    {
        if ((draft.kind != V4SourceKind::file_only && draft.kind != V4SourceKind::file_and_content) ||
            draft.fileCount.kind != V3CountKind::exact || draft.fileCount.decimal != "1" || !draft.filePaths.empty() ||
            !V4TaskSlotsCanonical(draft))
        { out.reason = "v4_owned_family_draft_unavailable"; return out; }
        const auto path = MakeV4OwnedRootPathSeed(rootPath, leaf, trustedRoot, rootId);
        if (!path.available) { out.reason = path.reason; return out; }
        auto seed = std::shared_ptr<V4OwnedFileFamilySeed>(new V4OwnedFileFamilySeed());
        seed->rootPath_ = rootPath; seed->rootId_ = rootId;
        // This held name handle itself denies delete sharing, independently of
        // whether the caller's trusted handle has that sharing mode.
        seed->root_ = CreateFileW(rootPath.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (!DuplicateHandle(GetCurrentProcess(), trustedRoot, GetCurrentProcess(), &seed->trustedRoot_, 0, FALSE, DUPLICATE_SAME_ACCESS))
        { out.reason = "v4_owned_family_trusted_root_duplicate_unavailable"; return out; }
        if (!seed->StillOwned()) { out.reason = "v4_owned_family_root_unavailable"; return out; }
        seed->family_.a = draft; seed->family_.a.filePaths.push_back(path.slot);
        const auto encoded = EncodeV4FamilyCoordinate(seed->family_, coordinateBudget);
        if (encoded.status == V4FamilyCoordinateStatus::resource_unavailable)
        { out.resourceUnavailable = true; out.reason = encoded.reason; return out; }
        if (encoded.status != V4FamilyCoordinateStatus::ready)
        { out.reason = "v4_owned_family_encoding_unavailable"; return out; }
        const auto schema = BuildV4FamilySchema(seed->family_, axisBudget);
        if (schema.status == V4FamilyStatus::resource_unavailable)
        { out.resourceUnavailable = true; out.reason = "v4_owned_family_schema_resource_unavailable"; return out; }
        if (schema.status != V4FamilyStatus::schema_only || schema.binding.empty() || !seed->StillOwned())
        { out.reason = "v4_owned_family_schema_or_root_unavailable"; return out; }
        seed->coordinate_ = encoded.coordinate; seed->binding_ = schema.binding;
        out.seed = std::move(seed); out.reason = "v4_owned_family_unexecuted"; return out;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "v4_owned_family_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "v4_owned_family_resource_unavailable"; return out; }
    catch (...) { out.reason = "v4_owned_family_observation_unavailable"; return out; }
}

// Construct a new family only after the isolated root exists. The paths are
// descriptor inputs before identity creation, not replacements for any
// previously discovered case. File index follows leaves order; descriptor
// slots use the canonical lexical index order. No file is created here.
inline V4OwnedFileFamilySeedResult PrepareV4OwnedMultiFileFamilySeed(const V4TaskSpec& draft,
    const std::filesystem::path& rootPath, const std::vector<std::wstring>& leaves,
    HANDLE trustedRoot, const FILE_ID_INFO& rootId,
    const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget)
{
    V4OwnedFileFamilySeedResult out;
    try
    {
        if ((draft.kind != V4SourceKind::file_only && draft.kind != V4SourceKind::file_and_content) ||
            draft.fileCount.kind != V3CountKind::exact || !draft.filePaths.empty() || leaves.empty() ||
            !V4TaskSlotsCanonical(draft))
        { out.reason = "v4_owned_multi_family_draft_unavailable"; return out; }
        if (leaves.size() > axisBudget.fileMembers || leaves.size() > axisBudget.textSlots ||
            leaves.size() > coordinateBudget.listEntries)
        { out.resourceUnavailable = true; out.reason = "v4_owned_multi_family_member_budget_unavailable"; return out; }
        const auto count = V4MaterializeCount(draft.fileCount, leaves.size(), false);
        if (count.status != V4AxisStatus::schema_only || count.value != leaves.size())
        { out.reason = "v4_owned_multi_family_count_mismatch"; return out; }
        auto seed = std::shared_ptr<V4OwnedFileFamilySeed>(new V4OwnedFileFamilySeed());
        seed->rootPath_ = rootPath; seed->rootId_ = rootId;
        seed->root_ = CreateFileW(rootPath.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (!DuplicateHandle(GetCurrentProcess(), trustedRoot, GetCurrentProcess(), &seed->trustedRoot_,
            0, FALSE, DUPLICATE_SAME_ACCESS) || !seed->StillOwned())
        { out.reason = "v4_owned_multi_family_root_unavailable"; return out; }
        seed->family_.a = draft;
        for (std::size_t i = 0; i < leaves.size(); ++i)
        {
            if (leaves[i].size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
            { out.reason = "v4_owned_multi_family_leaf_unavailable"; return out; }
            for (std::size_t j = 0; j < i; ++j)
            {
                const auto comparison = CompareStringOrdinal(leaves[j].data(), static_cast<int>(leaves[j].size()),
                    leaves[i].data(), static_cast<int>(leaves[i].size()), TRUE);
                if (!comparison || comparison == CSTR_EQUAL)
                { out.reason = "v4_owned_multi_family_leaf_alias_unavailable"; return out; }
            }
            auto path = MakeV4OwnedRootPathSeed(rootPath, leaves[i], seed->CreationRoot(), seed->RootId());
            if (!path.available) { out.reason = path.reason; return out; }
            path.slot.index = std::to_string(i);
            seed->family_.a.filePaths.push_back(std::move(path.slot));
        }
        std::sort(seed->family_.a.filePaths.begin(), seed->family_.a.filePaths.end(),
            [](const auto& left, const auto& right) { return left.index < right.index; });
        const auto encoded = EncodeV4FamilyCoordinate(seed->family_, coordinateBudget);
        if (encoded.status == V4FamilyCoordinateStatus::resource_unavailable)
        { out.resourceUnavailable = true; out.reason = encoded.reason; return out; }
        if (encoded.status != V4FamilyCoordinateStatus::ready)
        { out.reason = "v4_owned_multi_family_encoding_unavailable"; return out; }
        const auto schema = BuildV4FamilySchema(seed->family_, axisBudget);
        if (schema.status == V4FamilyStatus::resource_unavailable)
        { out.resourceUnavailable = true; out.reason = "v4_owned_multi_family_schema_resource_unavailable"; return out; }
        if (schema.status != V4FamilyStatus::schema_only || schema.binding.empty() || !seed->StillOwned())
        { out.reason = "v4_owned_multi_family_schema_or_root_unavailable"; return out; }
        seed->coordinate_ = encoded.coordinate; seed->binding_ = schema.binding;
        out.seed = std::move(seed); out.reason = "v4_owned_multi_family_unexecuted"; return out;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "v4_owned_multi_family_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "v4_owned_multi_family_resource_unavailable"; return out; }
    catch (...) { out.reason = "v4_owned_multi_family_observation_unavailable"; return out; }
}

struct V4OwnedFileCandidateSeed
{
    bool ready = false, resourceUnavailable = false;
    Candidate candidate;
    std::string candidateId;
    std::vector<std::string> unresolved;
};
// Explicit selections are required for every non-singleton axis. No result
// hypothesis is inferred from the root, filesystem, or product output.
inline V4OwnedFileCandidateSeed PrepareV4OwnedFileCandidate(const V4OwnedFileFamilySeed& seed,
    const V4AxisBudget& budget, const std::vector<std::pair<std::string, std::string>>& choices)
{
    V4OwnedFileCandidateSeed out;
    try
    {
        if (!seed.StillOwned()) { out.unresolved.emplace_back("v4_owned_candidate_root_unavailable"); return out; }
        const auto identity = event_graph::V4FamilyIdentityContext::Create(seed.Family(), budget);
        if (identity.Status() == V4FamilyStatus::resource_unavailable)
        { out.resourceUnavailable = true; return out; }
        if (identity.Status() != V4FamilyStatus::schema_only || identity.FamilyBinding() != seed.Binding())
        { out.unresolved.emplace_back("v4_owned_candidate_schema_unavailable"); return out; }
        std::map<std::string, std::string> chosen;
        for (const auto& [id, value] : choices)
            if (!chosen.emplace(id, value).second)
            { out.unresolved.emplace_back("v4_owned_candidate_duplicate_choice"); return out; }
        for (const auto& axis : identity.Axes())
        {
            const auto it = chosen.find(axis.id);
            if (axis.role == Role::Derived)
            {
                if (it != chosen.end()) { out.unresolved.emplace_back("v4_owned_candidate_derived_choice"); return out; }
                out.candidate.values.emplace_back();
            }
            else if (it != chosen.end())
            { out.candidate.values.push_back(it->second); chosen.erase(it); }
            else if (axis.values.size() == 1) out.candidate.values.push_back(axis.values.front());
            else { out.unresolved.emplace_back("v4_owned_candidate_choice_required:" + axis.id); return out; }
        }
        if (!chosen.empty()) { out.unresolved.emplace_back("v4_owned_candidate_unknown_choice"); return out; }
        const auto validated = V4FamilyValidatedContext::Create(seed.Family(), budget, out.candidate);
        if (validated.Status() == V4CoordinateStatus::resource_unavailable) { out.resourceUnavailable = true; return out; }
        const auto raw = identity.RawCandidateIdentity(out.candidate);
        if (raw.resourceUnavailable) { out.resourceUnavailable = true; return out; }
        if (validated.Status() != V4CoordinateStatus::coordinate_ready || !raw.errors.empty() || raw.id.empty() || !seed.StillOwned())
        { out.unresolved.emplace_back("v4_owned_candidate_validation_unavailable"); return out; }
        out.candidate = validated.Coordinate(); out.candidateId = raw.id; out.ready = true;
        out.unresolved = validated.Unresolved();
        out.unresolved.emplace_back("v4_owned_candidate_graph_and_execution_unimplemented");
    }
    catch (const std::bad_alloc&) { out.ready = false; out.resourceUnavailable = true; }
    catch (const std::length_error&) { out.ready = false; out.resourceUnavailable = true; }
    catch (...) { out.ready = false; out.unresolved.emplace_back("v4_owned_candidate_observation_unavailable"); }
    return out;
}

struct V4SingleFileFixture
{
    std::filesystem::path root, file;
    std::vector<std::byte> bytes;
    std::string candidateId;
    FILE_ID_INFO rootId{}, fileId{};
    // Retains the same candidate's observed root-relative mapping. It is an
    // input binding, not evidence of a production hit or completed drag.
    std::shared_ptr<const V4OwnedRelativePathBinding> relativeBinding;
    bool createAttempted = false, created = false, fileIdentityObserved = false, inputPrepared = false;
    std::string_view reason;
    std::vector<std::string> unresolved;
};

inline V4SingleFileFixture PrepareV4PayloadFixture(const V4FamilyValidatedContext& verified,
    const event_graph::V4FamilyIdentityContext& identity, const std::filesystem::path& isolatedRoot,
    HANDLE trustedRoot, const FILE_ID_INFO& trustedRootId,
    const std::shared_ptr<const V4OwnedRelativePathBinding>& relativeBinding = {})
{
    V4SingleFileFixture result;
    try
    {
        if (verified.Status() != V4CoordinateStatus::coordinate_ready || identity.Status() != V4FamilyStatus::schema_only ||
            verified.TaskBActive() || verified.FamilyBinding() != identity.FamilyBinding() ||
            verified.Axes().size() != identity.Axes().size())
        { result.reason = "v4_file_authoritative_context_unavailable"; return result; }
        const auto& axes = verified.Axes(); const auto& candidate = verified.Coordinate();
        for (std::size_t i = 0; i < axes.size(); ++i)
        {
            const auto& a = axes[i]; const auto& b = identity.Axes()[i];
            if (a.id != b.id || a.role != b.role || a.label != b.label || a.values != b.values)
            { result.reason = "v4_file_schema_mismatch"; return result; }
        }
        const auto raw = identity.RawCandidateIdentity(candidate);
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty())
        { result.reason = "v4_file_candidate_identity_unavailable"; return result; }
        const auto get = [&](std::string_view id) { return Value(axes, candidate, id); };
        if ((get("V4.source.kind") != "file_only" && get("V4.source.kind") != "file_and_content") ||
            get("V4.source.file_count") != "one" || get("V4.file.member0_kind") != "file" ||
            get("V4.file.member0_identity") != "unique")
        { result.reason = "v4_file_single_unique_input_unimplemented"; return result; }
        const auto pathValue = get("V4.file_path.index_0.utf16le");
        const auto bytesValue = get("V4.file_content.index_0.bytes");
        constexpr std::string_view rawPrefix = "utf16le_hex_", relativePrefix = "owned_root_relative_utf16le_hex_";
        const bool relative = pathValue.starts_with(relativePrefix);
        if ((!relative && !pathValue.starts_with(rawPrefix)) || !bytesValue.starts_with("hex_"))
        { result.reason = "v4_file_exact_path_or_content_unavailable"; return result; }
        std::string_view physicalPathHex;
        if (relative)
        {
            if (!relativeBinding || !relativeBinding->StillBound() || relativeBinding->CandidateId() != raw.id ||
                relativeBinding->FamilyBinding() != identity.FamilyBinding() || relativeBinding->RootPath() != isolatedRoot ||
                relativeBinding->RootId().VolumeSerialNumber != trustedRootId.VolumeSerialNumber ||
                std::memcmp(relativeBinding->RootId().FileId.Identifier, trustedRootId.FileId.Identifier,
                    sizeof(trustedRootId.FileId.Identifier)) != 0 || relativeBinding->Members().size() != 1 ||
                relativeBinding->Members().front().physicalIndex != 0 ||
                relativeBinding->Members().front().relativeUtf16leHex != pathValue.substr(relativePrefix.size()))
            { result.reason = "v4_file_relative_binding_unavailable"; return result; }
            physicalPathHex = relativeBinding->Members().front().physicalUtf16leHex;
        }
        else
        {
            if (relativeBinding) { result.reason = "v4_file_path_base_binding_mismatch"; return result; }
            physicalPathHex = pathValue.substr(rawPrefix.size());
        }
        const auto hexDigit = [](char c) -> int { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
        const auto unhex = [&](std::string_view hex, std::vector<std::byte>& out) {
            if (hex.size() % 2) return false;
            out.reserve(hex.size() / 2);
            for (std::size_t i = 0; i < hex.size(); i += 2)
            {
                const auto hi = hexDigit(hex[i]), lo = hexDigit(hex[i + 1]);
                if (hi < 0 || lo < 0) return false;
                out.push_back(static_cast<std::byte>((hi << 4) | lo));
            }
            return true;
        };
        std::vector<std::byte> pathBytes, content;
        if (!unhex(physicalPathHex, pathBytes) || pathBytes.empty() || pathBytes.size() % 2 ||
            !unhex(bytesValue.substr(4), content))
        { result.reason = "v4_file_input_bytes_invalid"; return result; }
        static_assert(sizeof(wchar_t) == 2, "Windows UTF-16 code units required");
        std::wstring pathText; pathText.reserve(pathBytes.size() / 2);
        for (std::size_t i = 0; i < pathBytes.size(); i += 2)
        {
            const auto unit = std::to_integer<unsigned>(pathBytes[i]) | (std::to_integer<unsigned>(pathBytes[i + 1]) << 8);
            if (!unit) { result.reason = "v4_file_embedded_null_unavailable"; return result; }
            pathText.push_back(static_cast<wchar_t>(unit));
        }
        const std::filesystem::path file(pathText);
        if (relative && file != relativeBinding->Members().front().physicalPath)
        { result.reason = "v4_file_relative_physical_path_mismatch"; return result; }
        const auto leaf = file.filename().wstring();
        if (!file.is_absolute() || !isolatedRoot.is_absolute() || file.parent_path() != isolatedRoot ||
            leaf.empty() || leaf == L"." || leaf == L".." || leaf.back() == L'.' || leaf.back() == L' ' ||
            leaf.find_first_of(L"\\/:*?<>|\"") != std::wstring::npos ||
            leaf.size() > (std::numeric_limits<USHORT>::max)() / sizeof(wchar_t))
        { result.reason = "v4_file_exact_path_not_owned_root_child"; return result; }
        const auto sameId = [](const FILE_ID_INFO& a, const FILE_ID_INFO& b) {
            return a.VolumeSerialNumber == b.VolumeSerialNumber &&
                std::memcmp(a.FileId.Identifier, b.FileId.Identifier, sizeof(a.FileId.Identifier)) == 0 &&
                std::any_of(std::begin(a.FileId.Identifier), std::end(a.FileId.Identifier), [](BYTE x) { return x != 0; });
        };
        FILE_ID_INFO supplied{}; FILE_ATTRIBUTE_TAG_INFO tag{};
        if (!trustedRoot || trustedRoot == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandleEx(trustedRoot, FileIdInfo, &supplied, sizeof(supplied)) || !sameId(supplied, trustedRootId) ||
            !GetFileInformationByHandleEx(trustedRoot, FileAttributeTagInfo, &tag, sizeof(tag)) ||
            !(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        { result.reason = "v4_file_trusted_root_unavailable"; return result; }
        struct HandleGuard { HANDLE h = INVALID_HANDLE_VALUE; ~HandleGuard() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); } };
        HandleGuard root{CreateFileW(isolatedRoot.c_str(), FILE_ADD_FILE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        FILE_ID_INFO opened{};
        if (root.h == INVALID_HANDLE_VALUE || !GetFileInformationByHandleEx(root.h, FileIdInfo, &opened, sizeof(opened)) ||
            !sameId(opened, trustedRootId) || !GetFileInformationByHandleEx(root.h, FileAttributeTagInfo, &tag, sizeof(tag)) ||
            !(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        { result.reason = "v4_file_owned_root_path_unavailable"; return result; }
        using NtCreate = NTSTATUS (NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
            PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
        const auto ntdll = GetModuleHandleW(L"ntdll.dll");
        const auto address = ntdll ? GetProcAddress(ntdll, "NtCreateFile") : nullptr;
        NtCreate create = nullptr;
        static_assert(sizeof(create) == sizeof(address));
        std::memcpy(&create, &address, sizeof(create));
        if (!create) { result.reason = "v4_file_relative_create_api_unavailable"; return result; }
        UNICODE_STRING name{};
        name.Buffer = const_cast<PWSTR>(leaf.data());
        name.Length = static_cast<USHORT>(leaf.size() * sizeof(wchar_t)); name.MaximumLength = name.Length;
        OBJECT_ATTRIBUTES attrs{}; attrs.Length = sizeof(attrs); attrs.RootDirectory = root.h;
        attrs.ObjectName = &name; attrs.Attributes = 0x40; // OBJ_CASE_INSENSITIVE
        IO_STATUS_BLOCK ios{};
        result.root = isolatedRoot; result.file = file; result.bytes = content; result.candidateId = raw.id; result.rootId = trustedRootId;
        result.relativeBinding = relativeBinding;
        result.unresolved = {"v4_file_logical_member_identity_unimplemented", "v4_file_directory_topology_unimplemented",
            "v4_other_source_formats_and_input_slots_unimplemented",
            "v4_file_host_target_dispatch_completion_and_business_unimplemented"};
        if (relative) result.unresolved.emplace_back("v4_file_relative_volume_path_relations_and_non_atomic_binding_unverified");
        if (relative && !relativeBinding->StillBound()) { result.reason = "v4_file_relative_root_changed_before_create"; return result; }
        result.createAttempted = true;
        HANDLE created = INVALID_HANDLE_VALUE;
        constexpr ULONG fileCreate = 2, nonDirectory = 0x40, synchronousNonAlert = 0x20, openReparse = 0x00200000;
        const auto status = create(&created, GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE, &attrs, &ios, nullptr,
            FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ,
            fileCreate, nonDirectory | synchronousNonAlert | openReparse, nullptr, 0);
        HandleGuard output{created};
        if (status < 0 || !created || created == INVALID_HANDLE_VALUE)
        { result.reason = "v4_file_relative_create_unavailable"; return result; }
        result.created = true;
        if (!GetFileInformationByHandleEx(output.h, FileIdInfo, &result.fileId, sizeof(result.fileId)) ||
            !sameId(result.fileId, result.fileId) ||
            !GetFileInformationByHandleEx(output.h, FileAttributeTagInfo, &tag, sizeof(tag)) ||
            (tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        { result.reason = "v4_file_created_identity_unavailable"; return result; }
        result.fileIdentityObserved = true;
        std::size_t offset = 0;
        while (offset < content.size())
        {
            const auto chunk = static_cast<DWORD>((std::min)(content.size() - offset, static_cast<std::size_t>(0x40000000)));
            DWORD written = 0;
            if (!WriteFile(output.h, content.data() + offset, chunk, &written, nullptr) || !written)
            { result.reason = "v4_file_input_write_unavailable"; return result; }
            offset += written;
        }
        if (!FlushFileBuffers(output.h)) { result.reason = "v4_file_input_flush_unavailable"; return result; }
        LARGE_INTEGER zero{};
        if (!SetFilePointerEx(output.h, zero, nullptr, FILE_BEGIN)) { result.reason = "v4_file_input_seek_unavailable"; return result; }
        char buffer[8192]; offset = 0;
        while (offset < content.size())
        {
            const auto chunk = static_cast<DWORD>((std::min)(content.size() - offset, sizeof(buffer)));
            DWORD read = 0;
            if (!ReadFile(output.h, buffer, chunk, &read, nullptr) || !read || std::memcmp(buffer, content.data() + offset, read))
            { result.reason = "v4_file_input_readback_unavailable"; return result; }
            offset += read;
        }
        LARGE_INTEGER size{}; FILE_ID_INFO after{};
        if (!GetFileSizeEx(output.h, &size) || size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) != content.size() ||
            !GetFileInformationByHandleEx(output.h, FileIdInfo, &after, sizeof(after)) || !sameId(after, result.fileId) ||
            !GetFileInformationByHandleEx(root.h, FileIdInfo, &opened, sizeof(opened)) || !sameId(opened, trustedRootId))
        { result.reason = "v4_file_input_changed_or_root_unavailable"; return result; }
        // Check the exact root spelling still resolves to the trusted object,
        // then reopen the child relative to the held root and compare IDs.
        HandleGuard rootByName{CreateFileW(isolatedRoot.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        FILE_ID_INFO namedRoot{};
        if (rootByName.h == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandleEx(rootByName.h, FileIdInfo, &namedRoot, sizeof(namedRoot)) ||
            !sameId(namedRoot, trustedRootId) ||
            !GetFileInformationByHandleEx(rootByName.h, FileAttributeTagInfo, &tag, sizeof(tag)) ||
            (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        { result.reason = "v4_file_root_name_changed"; return result; }
        HANDLE namedChild = INVALID_HANDLE_VALUE;
        constexpr ULONG fileOpen = 1;
        const auto reopenStatus = create(&namedChild, FILE_READ_ATTRIBUTES | SYNCHRONIZE, &attrs, &ios, nullptr,
            FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE,
            fileOpen, nonDirectory | synchronousNonAlert | openReparse, nullptr, 0);
        HandleGuard childByName{namedChild};
        FILE_ID_INFO namedId{};
        if (reopenStatus < 0 || !namedChild || namedChild == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandleEx(childByName.h, FileIdInfo, &namedId, sizeof(namedId)) ||
            !sameId(namedId, result.fileId) ||
            !GetFileInformationByHandleEx(childByName.h, FileAttributeTagInfo, &tag, sizeof(tag)) ||
            (tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        { result.reason = "v4_file_path_identity_unavailable"; return result; }
        // Shell/Win32 must resolve the model's exact spelling to this same
        // object; NT-relative creation alone does not establish that (e.g.
        // device names). The held output handle denies write/delete sharing.
        HandleGuard win32Child{CreateFileW(file.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        if (win32Child.h == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandleEx(win32Child.h, FileIdInfo, &namedId, sizeof(namedId)) ||
            !sameId(namedId, result.fileId) ||
            !GetFileInformationByHandleEx(win32Child.h, FileAttributeTagInfo, &tag, sizeof(tag)) ||
            (tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        { result.reason = "v4_file_win32_path_identity_unavailable"; return result; }
        if (relative && !relativeBinding->StillBound()) { result.reason = "v4_file_relative_root_changed_after_create"; return result; }
        result.inputPrepared = true;
        result.reason = "v4_file_input_only_not_business_coverage";
        return result;
    }
    catch (...) { result.inputPrepared = false; result.reason = "v4_file_fixture_resource_or_observation_unavailable"; return result; }
}

struct V4IndependentFileInput
{
    std::filesystem::path path;
    std::vector<std::byte> bytes;
    FILE_ID_INFO identity{};
    HANDLE handle = INVALID_HANDLE_VALUE;
    bool attempted = false, created = false, identityObserved = false, bytesObserved = false;
    bool copyObservationHandle = false; // set only after the explicit write-lease handoff
    V4IndependentFileInput() = default;
    V4IndependentFileInput(const V4IndependentFileInput&) = delete;
    V4IndependentFileInput& operator=(const V4IndependentFileInput&) = delete;
    V4IndependentFileInput(V4IndependentFileInput&& other) noexcept
        : path(std::move(other.path)), bytes(std::move(other.bytes)), identity(other.identity),
          handle(std::exchange(other.handle, INVALID_HANDLE_VALUE)), attempted(other.attempted), created(other.created),
          identityObserved(other.identityObserved), bytesObserved(other.bytesObserved),
          copyObservationHandle(other.copyObservationHandle) {}
    ~V4IndependentFileInput() { if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
};
enum class V4IndependentFileStatus { unimplemented, resource_unavailable, input_prepared };
struct V4IndependentFilePayload
{
    V4IndependentFileStatus status = V4IndependentFileStatus::unimplemented;
    std::filesystem::path root;
    FILE_ID_INFO rootId{};
    HANDLE rootHandle = INVALID_HANDLE_VALUE;
    std::string candidateId;
    std::vector<V4IndependentFileInput> files;
    std::string_view reason;
    std::vector<std::string> unresolved;
    V4IndependentFilePayload() = default;
    V4IndependentFilePayload(const V4IndependentFilePayload&) = delete;
    V4IndependentFilePayload& operator=(const V4IndependentFilePayload&) = delete;
    V4IndependentFilePayload(V4IndependentFilePayload&& other) noexcept
        : status(other.status), root(std::move(other.root)), rootId(other.rootId),
          rootHandle(std::exchange(other.rootHandle, INVALID_HANDLE_VALUE)), candidateId(std::move(other.candidateId)),
          files(std::move(other.files)), reason(other.reason), unresolved(std::move(other.unresolved)) {}
    ~V4IndependentFilePayload()
    {
        files.clear(); // release child leases before their pinned parent
        if (rootHandle && rootHandle != INVALID_HANDLE_VALUE) CloseHandle(rootHandle);
    }
};

// Input-only, one task and one authenticated root. The returned handles pin
// file identities and deny write/delete sharing; handing them to a real Shell
// operation, all source/logical relationships and business results remain
// separate unresolved obligations. No existing path is overwritten.
// maxBytes limits the sum of file-content bytes; source path/schema budgets
// are imposed separately by the authoritative V4 axis construction.
inline V4IndependentFilePayload PrepareV4IndependentFilePayload(const V4FamilyValidatedContext& verified,
    const event_graph::V4FamilyIdentityContext& identity, const std::filesystem::path& isolatedRoot,
    HANDLE trustedRoot, const FILE_ID_INFO& trustedRootId, std::size_t maxFiles, std::size_t maxBytes,
    HANDLE requiredLiveParent = nullptr, const std::function<bool()>* requiredPreCreate = nullptr,
    const std::shared_ptr<const V4OwnedRelativePathBinding>& relativeBinding = {},
    const V4RelativePathMaterializationKey* relativeKey = nullptr)
{
    V4IndependentFilePayload out;
    try
    {
        if (verified.Status() != V4CoordinateStatus::coordinate_ready || identity.Status() != V4FamilyStatus::schema_only ||
            verified.TaskBActive() || verified.FamilyBinding() != identity.FamilyBinding() || verified.Axes().size() != identity.Axes().size())
        { out.reason = "v4_files_authoritative_context_unavailable"; return out; }
        const auto& axes = verified.Axes(); const auto& candidate = verified.Coordinate();
        for (std::size_t i = 0; i < axes.size(); ++i)
        {
            const auto& a = axes[i]; const auto& b = identity.Axes()[i];
            if (a.id != b.id || a.role != b.role || a.label != b.label || a.values != b.values)
            { out.reason = "v4_files_schema_mismatch"; return out; }
        }
        const auto raw = identity.RawCandidateIdentity(candidate);
        if (raw.resourceUnavailable) { out.status = V4IndependentFileStatus::resource_unavailable; out.reason = "v4_files_identity_resource_unavailable"; return out; }
        if (!raw.errors.empty() || raw.id.empty()) { out.reason = "v4_files_identity_unavailable"; return out; }
        const auto get = [&](std::string_view id) { return Value(axes, candidate, id); };
        if (get("V4.source.kind") != "file_only" && get("V4.source.kind") != "file_and_content")
        { out.reason = "v4_files_source_kind_unimplemented"; return out; }
        const auto count = get("V4.source.file_count");
        std::string number;
        if (count == "zero") number = "0"; else if (count == "one") number = "1"; else if (count == "two") number = "2";
        else if (count.starts_with("count_")) number = count.substr(6);
        else { out.reason = "v4_files_count_unimplemented"; return out; }
        const auto materialized = V4MaterializeCount({V3CountKind::exact, number}, maxFiles, false);
        if (materialized.status == V4AxisStatus::resource_unavailable)
        { out.status = V4IndependentFileStatus::resource_unavailable; out.reason = "v4_files_count_budget_unavailable"; return out; }
        if (materialized.status != V4AxisStatus::schema_only || !materialized.value)
        { out.reason = "v4_files_empty_or_invalid_count_unimplemented"; return out; }
        const auto digit = [](char c) -> int { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
        const auto decode = [&](std::string_view hex, std::vector<std::byte>& output) {
            if (hex.size() % 2) return false;
            output.reserve(hex.size() / 2);
            for (std::size_t i = 0; i < hex.size(); i += 2)
            { const int high = digit(hex[i]), low = digit(hex[i + 1]); if (high < 0 || low < 0) return false;
              output.push_back(static_cast<std::byte>((high << 4) | low)); }
            return true;
        };
        const auto sameId = [](const FILE_ID_INFO& a, const FILE_ID_INFO& b) {
            return a.VolumeSerialNumber == b.VolumeSerialNumber &&
                std::memcmp(a.FileId.Identifier, b.FileId.Identifier, sizeof(a.FileId.Identifier)) == 0 &&
                std::any_of(std::begin(a.FileId.Identifier), std::end(a.FileId.Identifier), [](BYTE x) { return x != 0; });
        };
        if (relativeBinding && (!relativeKey || !relativeKey->Matches(relativeBinding) || !requiredPreCreate || !requiredLiveParent ||
            !relativeBinding->StillBound() || relativeBinding->CandidateId() != raw.id ||
            relativeBinding->FamilyBinding() != identity.FamilyBinding() || relativeBinding->RootPath() != isolatedRoot ||
            !sameId(relativeBinding->RootId(), trustedRootId) || relativeBinding->Members().size() != materialized.value))
        { out.reason = "v4_files_relative_binding_unavailable"; return out; }
        std::set<std::string> expectedAxes;
        std::size_t total = 0;
        out.files.reserve(materialized.value);
        for (std::size_t i = 0; i < materialized.value; ++i)
        {
            const auto index = std::to_string(i), base = "V4.file.member" + index;
            const auto pathAxis = "V4.file_path.index_" + index + ".utf16le";
            const auto bytesAxis = "V4.file_content.index_" + index + ".bytes";
            expectedAxes.insert(pathAxis); expectedAxes.insert(bytesAxis);
            if (get(base + "_kind") != "file" || get(base + "_identity") != "unique")
            { out.reason = "v4_files_kind_or_alias_unimplemented"; return out; }
            const auto pathValue = get(pathAxis), bytesValue = get(bytesAxis);
            std::string_view physicalHex;
            if (pathValue.starts_with("utf16le_hex_") && !relativeBinding) physicalHex = pathValue.substr(12);
            else if (pathValue.starts_with("owned_root_relative_utf16le_hex_") && relativeBinding)
            {
                const auto& member = relativeBinding->Members()[i];
                constexpr std::string_view prefix = "owned_root_relative_utf16le_hex_";
                if (member.physicalIndex != i || pathValue.substr(prefix.size()) != member.relativeUtf16leHex)
                { out.reason = "v4_files_relative_member_mismatch"; return out; }
                physicalHex = member.physicalUtf16leHex;
            }
            else { out.reason = "v4_files_exact_path_or_bytes_unimplemented"; return out; }
            if (!bytesValue.starts_with("hex_"))
            { out.reason = "v4_files_exact_path_or_bytes_unimplemented"; return out; }
            const auto hex = bytesValue.substr(4);
            if (hex.size() / 2 > maxBytes - total)
            { out.status = V4IndependentFileStatus::resource_unavailable; out.reason = "v4_files_byte_budget_unavailable"; return out; }
            std::vector<std::byte> pathBytes;
            V4IndependentFileInput file;
            if (!decode(physicalHex, pathBytes) || pathBytes.empty() || pathBytes.size() % 2 || !decode(hex, file.bytes))
            { out.reason = "v4_files_input_encoding_invalid"; return out; }
            total += file.bytes.size();
            static_assert(sizeof(wchar_t) == 2, "Windows UTF-16 code units required");
            std::wstring text; text.reserve(pathBytes.size() / 2);
            for (std::size_t n = 0; n < pathBytes.size(); n += 2)
            {
                const auto unit = std::to_integer<unsigned>(pathBytes[n]) | (std::to_integer<unsigned>(pathBytes[n + 1]) << 8);
                if (!unit) { out.reason = "v4_files_embedded_null_unimplemented"; return out; }
                text.push_back(static_cast<wchar_t>(unit));
            }
            file.path = std::filesystem::path(text);
            const auto leaf = file.path.filename().native();
            if (!file.path.is_absolute() || !isolatedRoot.is_absolute() || file.path.parent_path() != isolatedRoot || leaf.empty() ||
                leaf == L"." || leaf == L".." || leaf.back() == L'.' || leaf.back() == L' ' ||
                leaf.find_first_of(L"\\/:*?<>|\"") != std::wstring::npos ||
                leaf.size() > (std::numeric_limits<USHORT>::max)() / sizeof(wchar_t) ||
                leaf.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
            { out.reason = "v4_files_path_not_owned_root_child"; return out; }
            for (const auto& prior : out.files)
            {
                const auto previous = prior.path.filename().native();
                const int comparison = CompareStringOrdinal(previous.data(), static_cast<int>(previous.size()),
                    leaf.data(), static_cast<int>(leaf.size()), TRUE);
                if (!comparison || comparison == CSTR_EQUAL)
                { out.reason = "v4_files_path_collision_or_comparison_unavailable"; return out; }
            }
            out.files.push_back(std::move(file));
        }
        for (const auto& axis : axes)
            if ((axis.id.starts_with("V4.file_path.index_") || axis.id.starts_with("V4.file_content.index_")) &&
                !expectedAxes.contains(axis.id))
            { out.reason = "v4_files_extra_indexed_input_unimplemented"; return out; }
        FILE_ID_INFO held{}; FILE_ATTRIBUTE_TAG_INFO tag{};
        if (!trustedRoot || trustedRoot == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandleEx(trustedRoot, FileIdInfo, &held, sizeof(held)) || !sameId(held, trustedRootId) ||
            !GetFileInformationByHandleEx(trustedRoot, FileAttributeTagInfo, &tag, sizeof(tag)) ||
            !(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        { out.reason = "v4_files_trusted_root_unavailable"; return out; }
        out.rootHandle = CreateFileW(isolatedRoot.c_str(), FILE_ADD_FILE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        FILE_ID_INFO opened{};
        if (out.rootHandle == INVALID_HANDLE_VALUE || !GetFileInformationByHandleEx(out.rootHandle, FileIdInfo, &opened, sizeof(opened)) ||
            !sameId(opened, trustedRootId) || !GetFileInformationByHandleEx(out.rootHandle, FileAttributeTagInfo, &tag, sizeof(tag)) ||
            !(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        { out.reason = "v4_files_root_path_identity_unavailable"; return out; }
        using NtCreate = NTSTATUS (NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
            PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
        const auto ntdll = GetModuleHandleW(L"ntdll.dll"); const auto address = ntdll ? GetProcAddress(ntdll, "NtCreateFile") : nullptr;
        NtCreate create = nullptr; static_assert(sizeof(create) == sizeof(address)); std::memcpy(&create, &address, sizeof(create));
        if (!create) { out.reason = "v4_files_relative_create_api_unavailable"; return out; }
        out.root = isolatedRoot; out.rootId = trustedRootId; out.candidateId = raw.id;
        out.unresolved = {"v4_files_logical_member_and_other_formats_unimplemented",
            "v4_files_directory_topology_unimplemented", "v4_files_shell_handle_handoff_and_host_completion_unimplemented",
            "v4_files_production_file_membership_and_layout_results_unimplemented"};
        constexpr ULONG fileCreate = 2, nonDirectory = 0x40, synchronousNonAlert = 0x20, openReparse = 0x00200000;
        for (auto& file : out.files)
        {
            if (requiredLiveParent && WaitForSingleObject(requiredLiveParent, 0) != WAIT_TIMEOUT)
            { out.reason = "v4_files_required_parent_not_live"; return out; }
            if (requiredPreCreate && !(*requiredPreCreate)())
            { out.reason = "v4_files_parent_ledger_revalidation_unavailable"; return out; }
            if (relativeBinding && !relativeBinding->StillBound())
            { out.reason = "v4_files_relative_binding_changed"; return out; }
            const auto leaf = file.path.filename().native(); UNICODE_STRING name{};
            name.Buffer = const_cast<PWSTR>(leaf.data()); name.Length = static_cast<USHORT>(leaf.size() * sizeof(wchar_t)); name.MaximumLength = name.Length;
            OBJECT_ATTRIBUTES attrs{}; attrs.Length = sizeof(attrs); attrs.RootDirectory = out.rootHandle; attrs.ObjectName = &name; attrs.Attributes = 0x40;
            IO_STATUS_BLOCK ios{}; file.attempted = true;
            const auto status = create(&file.handle, GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE, &attrs, &ios, nullptr,
                FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, fileCreate, nonDirectory | synchronousNonAlert | openReparse, nullptr, 0);
            if (status < 0 || !file.handle || file.handle == INVALID_HANDLE_VALUE)
            { out.reason = "v4_files_relative_create_unavailable"; return out; }
            file.created = true;
            if (!GetFileInformationByHandleEx(file.handle, FileIdInfo, &file.identity, sizeof(file.identity)) || !sameId(file.identity, file.identity) ||
                !GetFileInformationByHandleEx(file.handle, FileAttributeTagInfo, &tag, sizeof(tag)) ||
                (tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
            { out.reason = "v4_files_created_identity_unavailable"; return out; }
            file.identityObserved = true;
            for (const auto& prior : out.files)
                if (&prior != &file && prior.identityObserved && sameId(prior.identity, file.identity))
                { out.reason = "v4_files_actual_alias_unavailable"; return out; }
            std::size_t offset = 0;
            while (offset < file.bytes.size())
            {
                const auto chunk = static_cast<DWORD>((std::min)(file.bytes.size() - offset, static_cast<std::size_t>(0x40000000)));
                DWORD written = 0;
                if (!WriteFile(file.handle, file.bytes.data() + offset, chunk, &written, nullptr) || !written)
                { out.reason = "v4_files_write_unavailable"; return out; }
                offset += written;
            }
            LARGE_INTEGER zero{}, size{};
            if (!FlushFileBuffers(file.handle) || !SetFilePointerEx(file.handle, zero, nullptr, FILE_BEGIN))
            { out.reason = "v4_files_readback_unavailable"; return out; }
            char buffer[8192]; offset = 0;
            while (offset < file.bytes.size())
            {
                const auto chunk = static_cast<DWORD>((std::min)(file.bytes.size() - offset, sizeof(buffer))); DWORD read = 0;
                if (!ReadFile(file.handle, buffer, chunk, &read, nullptr) || !read || std::memcmp(buffer, file.bytes.data() + offset, read))
                { out.reason = "v4_files_readback_unavailable"; return out; }
                offset += read;
            }
            FILE_ID_INFO finalId{};
            if (!GetFileSizeEx(file.handle, &size) || size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) != file.bytes.size() ||
                !GetFileInformationByHandleEx(file.handle, FileIdInfo, &finalId, sizeof(finalId)) || !sameId(finalId, file.identity))
            { out.reason = "v4_files_changed_during_readback"; return out; }
            struct Guard { HANDLE h; ~Guard() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); } };
            Guard named{CreateFileW(file.path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
            FILE_ID_INFO namedId{};
            if (named.h == INVALID_HANDLE_VALUE || !GetFileInformationByHandleEx(named.h, FileIdInfo, &namedId, sizeof(namedId)) ||
                !sameId(namedId, file.identity) || !GetFileInformationByHandleEx(named.h, FileAttributeTagInfo, &tag, sizeof(tag)) ||
                (tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
            { out.reason = "v4_files_win32_path_identity_unavailable"; return out; }
            file.bytesObserved = true;
        }
        struct RootGuard { HANDLE h; ~RootGuard() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); } };
        RootGuard namedRoot{CreateFileW(isolatedRoot.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        FILE_ID_INFO namedRootId{};
        if (!GetFileInformationByHandleEx(out.rootHandle, FileIdInfo, &opened, sizeof(opened)) || !sameId(opened, trustedRootId) ||
            namedRoot.h == INVALID_HANDLE_VALUE || !GetFileInformationByHandleEx(namedRoot.h, FileIdInfo, &namedRootId, sizeof(namedRootId)) ||
            !sameId(namedRootId, trustedRootId) || !GetFileInformationByHandleEx(namedRoot.h, FileAttributeTagInfo, &tag, sizeof(tag)) ||
            !(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        { out.reason = "v4_files_root_changed_unavailable"; return out; }
        out.status = V4IndependentFileStatus::input_prepared; out.reason = "v4_files_input_only_not_business_coverage";
        return out;
    }
    catch (const std::bad_alloc&) { out.status = V4IndependentFileStatus::resource_unavailable; out.reason = "v4_files_allocation_unavailable"; return out; }
    catch (const std::length_error&) { out.status = V4IndependentFileStatus::resource_unavailable; out.reason = "v4_files_allocation_unavailable"; return out; }
    catch (...) { out.reason = "v4_files_observation_unavailable"; return out; }
}

// A Shell-backed source is prepared from the still-live independent-file
// leases, in their exact model index order. This only proves construction
// input, not formats or the order actually returned to the production host.
// The Profile-owned STA owner retains Source and the same file leases;
// callers borrow them and cannot infer a production read from construction.
class V4IndependentShellSource;
class V4PinnedCopyLease;
class V4SharedMoveLease;
struct V4FileDropOrderObservation;
namespace ole_driver { class V4MultiFileGraphExecutor; }
namespace mixed_fixture { class MixedOwner; }
class V4IndependentShellOwner final
{
    friend struct ::NativeDragIntegrationAccess;
    friend class V4IndependentShellSource;
    friend class ole_driver::V4MultiFileGraphExecutor;
    friend class mixed_fixture::MixedOwner;
    friend V4IndependentShellSource PrepareV4IndependentShellSource(const V4FamilyValidatedContext&,
        const event_graph::V4FamilyIdentityContext&, const std::shared_ptr<V4IndependentFilePayload>&,
        const std::shared_ptr<V4IndependentShellOwner>&);
    friend V4FileDropOrderObservation ObserveV4FileDropOrder(const V4FamilyValidatedContext&,
        const event_graph::V4FamilyIdentityContext&, const std::shared_ptr<V4IndependentFilePayload>&,
        const V4IndependentShellSource&, const std::shared_ptr<V4IndependentShellOwner>&, std::size_t);
public:
    V4IndependentShellOwner(const V4IndependentShellOwner&) = delete;
    V4IndependentShellOwner& operator=(const V4IndependentShellOwner&) = delete;
    // If teardown is bypassed, retain both COM and its file leases rather
    // than release on the wrong apartment or while work may still use them.
    ~V4IndependentShellOwner()
    { if (source_) { (void)source_.Detach(); (void)retainedPayload_.release(); (void)retainedPinnedLease_.release(); (void)retainedMoveLease_.release(); } }
    bool AttachPinnedCopyLease(const V4FamilyValidatedContext&, const event_graph::V4FamilyIdentityContext&,
        const std::shared_ptr<V4IndependentFilePayload>&, const std::shared_ptr<const V4PinnedCopyLease>&);
    bool MatchesPinnedCopyLease(const V4FamilyValidatedContext&, const event_graph::V4FamilyIdentityContext&,
        const std::shared_ptr<V4IndependentFilePayload>&, const std::shared_ptr<const V4PinnedCopyLease>&) const;
    bool AttachSharedMoveLease(const V4FamilyValidatedContext&, const event_graph::V4FamilyIdentityContext&,
        const std::shared_ptr<V4IndependentFilePayload>&, const std::shared_ptr<const V4SharedMoveLease>&);
    bool MatchesSharedMoveLeaseBefore(const V4FamilyValidatedContext&, const event_graph::V4FamilyIdentityContext&,
        const std::shared_ptr<V4IndependentFilePayload>&, const std::shared_ptr<const V4SharedMoveLease>&) const;
    bool OwnsSharedMoveLease(const std::shared_ptr<V4IndependentFilePayload>&,
        const std::shared_ptr<const V4SharedMoveLease>&) const noexcept;
private:
    V4IndependentShellOwner() noexcept : ownerThread_(GetCurrentThreadId()) {}
    bool Live() const noexcept
    {
        if (!active_.load(std::memory_order_acquire) || GetCurrentThreadId() != ownerThread_) return false;
        APTTYPE apartment{}; APTTYPEQUALIFIER qualifier{};
        return SUCCEEDED(CoGetApartmentType(&apartment, &qualifier)) &&
            (apartment == APTTYPE_STA || apartment == APTTYPE_MAINSTA);
    }
    bool Attach(Microsoft::WRL::ComPtr<ole_file_source_fixture::Source>&& source,
        const std::shared_ptr<V4IndependentFilePayload>& payload,
        std::unique_ptr<std::shared_ptr<V4IndependentFilePayload>>&& retained) noexcept
    {
        if (!Live() || source_ || payload_ || !source || source->owner != ownerThread_ || !payload ||
            payload->status != V4IndependentFileStatus::input_prepared || !retained || *retained != payload) return false;
        source_ = std::move(source); payload_ = payload; retainedPayload_ = std::move(retained); return true;
    }
    bool CanInvalidate() const noexcept
    { return GetCurrentThreadId() == ownerThread_ && !executing_.load(std::memory_order_acquire); }
    bool TryBeginExecution() const noexcept
    {
        if (!Live() || !source_ || !payload_ || payload_->status != V4IndependentFileStatus::input_prepared) return false;
        bool expected = false;
        return executing_.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
    }
    bool TryBeginPreparation() const noexcept
    {
        if (!Live() || source_ || payload_ || retainedPayload_) return false;
        bool expected = false;
        return executing_.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
    }
    void EndExecution() const noexcept { executing_.store(false, std::memory_order_release); }
    void Invalidate() noexcept
    {
        const DWORD saved = GetLastError();
        if (!CanInvalidate()) { SetLastError(saved); return; }
        active_.store(false, std::memory_order_release);
        source_.Reset(); payload_.reset(); retainedPayload_.reset(); retainedPinnedLease_.reset(); retainedMoveLease_.reset();
        SetLastError(saved);
    }
    DWORD ownerThread_ = 0;
    std::atomic_bool active_{true};
    mutable std::atomic_bool executing_{false};
    Microsoft::WRL::ComPtr<ole_file_source_fixture::Source> source_;
    std::shared_ptr<V4IndependentFilePayload> payload_;
    std::unique_ptr<std::shared_ptr<V4IndependentFilePayload>> retainedPayload_;
    std::unique_ptr<std::shared_ptr<const V4PinnedCopyLease>> retainedPinnedLease_;
    std::unique_ptr<std::shared_ptr<const V4SharedMoveLease>> retainedMoveLease_;
};

class V4IndependentShellSource
{
    friend V4IndependentShellSource PrepareV4IndependentShellSource(const V4FamilyValidatedContext&,
        const event_graph::V4FamilyIdentityContext&, const std::shared_ptr<V4IndependentFilePayload>&,
        const std::shared_ptr<V4IndependentShellOwner>&);
    friend V4FileDropOrderObservation ObserveV4FileDropOrder(const V4FamilyValidatedContext&,
        const event_graph::V4FamilyIdentityContext&, const std::shared_ptr<V4IndependentFilePayload>&,
        const V4IndependentShellSource&, const std::shared_ptr<V4IndependentShellOwner>&, std::size_t);
public:
    bool inputPrepared = false, resourceUnavailable = false;
    DWORD ownerThread = 0;
    std::string candidateId;
    std::vector<std::string> unresolved;
    ole_file_source_fixture::Source* SourceObject() const noexcept
    {
        const auto locked = owner_.lock();
        return locked && locked->Live() && locked->payload_ &&
            locked->payload_->status == V4IndependentFileStatus::input_prepared &&
            locked->source_.Get() == sourceIdentity_ ? sourceIdentity_ : nullptr;
    }
    bool SameOwner(const std::shared_ptr<V4IndependentShellOwner>& owner) const noexcept
    { return owner && owner_.lock() == owner && SourceObject() != nullptr; }
    bool SamePayload(const std::shared_ptr<V4IndependentFilePayload>& payload) const noexcept
    {
        const auto locked = owner_.lock();
        return payload && locked && locked->payload_ == payload && SourceObject() != nullptr;
    }
private:
    std::weak_ptr<V4IndependentShellOwner> owner_;
    ole_file_source_fixture::Source* sourceIdentity_ = nullptr;
};

struct V4FileCopyHandoffEntry
{
    bool creationHandleCloseAttempted = false, creationHandleClosed = false;
    bool observationOpenAttempted = false, observationOpened = false, identityAndBytesObserved = false;
};
enum class V4FileCopyHandoffMode { shared_observation, single_file_pinned_copy, single_file_pinned_leave, single_file_pinned_copy_only, single_file_pinned_reject, single_file_shared_move };
struct V4FileCopyHandoff;
class V4PinnedCopyLease final : public std::enable_shared_from_this<V4PinnedCopyLease>
{
    friend class V4SharedMoveLease;
    friend V4FileCopyHandoff PrepareV4FileCopyHandoff(const V4FamilyValidatedContext&,
        const event_graph::V4FamilyIdentityContext&, V4IndependentFilePayload&, V4FileCopyHandoffMode);
    HANDLE root_ = INVALID_HANDLE_VALUE, file_ = INVALID_HANDLE_VALUE;
    FILE_ID_INFO rootId_{}, fileId_{};
    std::filesystem::path rootPath_, path_;
    std::string candidateId_;
    std::vector<std::byte> bytes_;
    V4PinnedCopyLease() = default;
public:
    V4PinnedCopyLease(const V4PinnedCopyLease&) = delete;
    V4PinnedCopyLease& operator=(const V4PinnedCopyLease&) = delete;
    ~V4PinnedCopyLease() { if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_); if (root_ != INVALID_HANDLE_VALUE) CloseHandle(root_); }
    // This verifies the live input lease only. The caller must separately bind
    // the Profile-owned Shell source and observe the real production read.
    bool Revalidate(const V4FamilyValidatedContext&, const event_graph::V4FamilyIdentityContext&,
        const V4IndependentFilePayload&) const;
};
struct V4SharedMoveSourceAfter
{
    std::optional<FILE_ID_INFO> heldId, namedId;
    std::optional<bool> heldIdentityMatches, heldBytesMatch, originalNamePresent, originalNameMatchesSource;
    bool observed = false, resourceUnavailable = false;
    std::vector<std::string> unresolved{"v4_move_after_source_name_and_handle_non_atomic", "v4_move_shell_open_and_business_result_unverified"};
};
class V4SharedMoveLease final : public std::enable_shared_from_this<V4SharedMoveLease>
{
    friend V4FileCopyHandoff PrepareV4FileCopyHandoff(const V4FamilyValidatedContext&,
        const event_graph::V4FamilyIdentityContext&, V4IndependentFilePayload&, V4FileCopyHandoffMode);
    std::shared_ptr<const V4PinnedCopyLease> identity_;
    V4SharedMoveLease() = default;
public:
    V4SharedMoveLease(const V4SharedMoveLease&) = delete;
    V4SharedMoveLease& operator=(const V4SharedMoveLease&) = delete;
    std::string_view CandidateId() const noexcept { return identity_ ? std::string_view(identity_->candidateId_) : std::string_view{}; }
    bool RevalidateBefore(const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
        const V4IndependentFilePayload& payload) const
    { return identity_ && identity_->Revalidate(verified, identity, payload); }
    V4SharedMoveSourceAfter ObserveSourceAfter() const;
};
struct V4FileCopyHandoff
{
    bool prepared = false, resourceUnavailable = false;
    std::string candidateId;
    std::vector<V4FileCopyHandoffEntry> files;
    std::vector<std::string> unresolved;
    std::shared_ptr<const V4PinnedCopyLease> pinnedLease;
    std::shared_ptr<const V4SharedMoveLease> sharedMoveLease;
};

// Release write leases before a possible copy. The default observation handle
// shares read, write and delete; the opt-in single-file mode holds a read-only
// handle that denies write/delete sharing after a continuous identity handoff.
// Neither mode proves Shell compatibility or its actual source read. Partial
// handoffs invalidate inputPrepared and retain per-index side-effect records.
// Move/delete and automatic production dispatch are outside this interface.
inline V4FileCopyHandoff PrepareV4FileCopyHandoff(const V4FamilyValidatedContext& verified,
    const event_graph::V4FamilyIdentityContext& identity, V4IndependentFilePayload& payload,
    V4FileCopyHandoffMode mode = V4FileCopyHandoffMode::shared_observation)
{
    V4FileCopyHandoff out;
    try
    {
        if (verified.Status() != V4CoordinateStatus::coordinate_ready || identity.Status() != V4FamilyStatus::schema_only ||
            verified.TaskBActive() || verified.FamilyBinding() != identity.FamilyBinding() ||
            verified.Axes().size() != identity.Axes().size() || payload.status != V4IndependentFileStatus::input_prepared ||
            payload.files.empty() || !payload.rootHandle || payload.rootHandle == INVALID_HANDLE_VALUE)
        { out.unresolved.emplace_back("v4_copy_handoff_preflight_unavailable"); return out; }
        for (std::size_t i = 0; i < identity.Axes().size(); ++i)
        {
            const auto& a = verified.Axes()[i]; const auto& b = identity.Axes()[i];
            if (a.id != b.id || a.role != b.role || a.values != b.values || a.label != b.label)
            { out.unresolved.emplace_back("v4_copy_handoff_schema_mismatch"); return out; }
        }
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        if (raw.resourceUnavailable) out.resourceUnavailable = true;
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty() || raw.id != payload.candidateId)
        { out.unresolved.emplace_back("v4_copy_handoff_candidate_unavailable"); return out; }
        const auto value = [&](std::string_view id) { return Value(verified.Axes(), verified.Coordinate(), id); };
        const auto expectedCount = payload.files.size() == 1 ? std::string("one") : payload.files.size() == 2 ? std::string("two") :
            std::string("count_") + std::to_string(payload.files.size());
        if ((value("V4.source.kind") != "file_only" && value("V4.source.kind") != "file_and_content") ||
            value("V4.source.file_count") != expectedCount)
        { out.unresolved.emplace_back("v4_copy_handoff_candidate_shape_unavailable"); return out; }
        if (mode != V4FileCopyHandoffMode::shared_observation && mode != V4FileCopyHandoffMode::single_file_pinned_copy &&
            mode != V4FileCopyHandoffMode::single_file_pinned_leave && mode != V4FileCopyHandoffMode::single_file_pinned_copy_only &&
            mode != V4FileCopyHandoffMode::single_file_pinned_reject && mode != V4FileCopyHandoffMode::single_file_shared_move)
        { out.unresolved.emplace_back("v4_handoff_mode_unavailable"); return out; }
        if (mode == V4FileCopyHandoffMode::single_file_pinned_copy &&
            (payload.files.size() != 1 || value("G02.release") != "ctrl" || value("T03") != "copy_move"))
        { out.unresolved.emplace_back("v4_pinned_copy_handoff_candidate_unavailable"); return out; }
        if (mode == V4FileCopyHandoffMode::single_file_pinned_leave &&
            (payload.files.size() != 1 || value("V4.source.kind") != "file_only" || value("T01") != "external_ingress" ||
                value("G04.termination") != "leave" || value("G02.release") != "na" || value("G02.release_buttons") != "na" ||
                value("V4.file.member0_kind") != "file" || value("V4.file.member0_identity") != "unique"))
        { out.unresolved.emplace_back("v4_pinned_leave_handoff_candidate_unavailable"); return out; }
        if (mode == V4FileCopyHandoffMode::single_file_pinned_copy_only &&
            (payload.files.size() != 1 || value("V4.source.kind") != "file_only" || value("T01") != "external_ingress" ||
                value("G04.termination") != "drop" || value("G02.release") != "none" || value("T03") != "copy" ||
                value("T03.extra_protocol_bits") != "none" || value("V4.file.member0_kind") != "file" ||
                value("V4.file.member0_identity") != "unique"))
        { out.unresolved.emplace_back("v4_pinned_copy_only_handoff_candidate_unavailable"); return out; }
        if (mode == V4FileCopyHandoffMode::single_file_pinned_reject &&
            (payload.files.size() != 1 || value("V4.source.kind") != "file_only" || value("T01") != "external_ingress" ||
                value("G04.termination") != "drop" || value("G02.release") != "none" || value("T03") != "none" ||
                value("T03.extra_protocol_bits") != "none" || value("V4.file.member0_kind") != "file" ||
                value("V4.file.member0_identity") != "unique"))
        { out.unresolved.emplace_back("v4_pinned_reject_handoff_candidate_unavailable"); return out; }
        if (mode == V4FileCopyHandoffMode::single_file_shared_move &&
            (payload.files.size() != 1 || value("V4.source.kind") != "file_only" || value("T01") != "external_ingress" ||
                value("G04.termination") != "drop" || value("G02.release") != "none" || value("T03") != "copy_move" ||
                value("T03.extra_protocol_bits") != "none" || value("V4.file.member0_kind") != "file" ||
                value("V4.file.member0_identity") != "unique"))
        { out.unresolved.emplace_back("v4_shared_move_handoff_candidate_unavailable"); return out; }
        const bool sharedMove = mode == V4FileCopyHandoffMode::single_file_shared_move;
        const bool pinned = mode == V4FileCopyHandoffMode::single_file_pinned_copy || mode == V4FileCopyHandoffMode::single_file_pinned_leave ||
            mode == V4FileCopyHandoffMode::single_file_pinned_copy_only || mode == V4FileCopyHandoffMode::single_file_pinned_reject;
        constexpr char digits[] = "0123456789abcdef";
        const auto byteHex = [&](std::string& dst, unsigned byte) { dst.push_back(digits[byte >> 4]); dst.push_back(digits[byte & 15]); };
        std::set<std::string> representedAxes;
        for (std::size_t i = 0; i < payload.files.size(); ++i)
        {
            const auto index = std::to_string(i); const auto& file = payload.files[i];
            const auto pathAxis = "V4.file_path.index_" + index + ".utf16le";
            const auto bytesAxis = "V4.file_content.index_" + index + ".bytes";
            representedAxes.insert(pathAxis); representedAxes.insert(bytesAxis);
            std::string path = "utf16le_hex_", bytes = "hex_";
            for (const auto ch : file.path.wstring())
            { const auto unit = static_cast<unsigned short>(ch); byteHex(path, unit & 255); byteHex(path, unit >> 8); }
            for (const auto byte : file.bytes) byteHex(bytes, std::to_integer<unsigned>(byte));
            if (value("V4.file.member" + index + "_kind") != "file" ||
                value("V4.file.member" + index + "_identity") != "unique" || value(pathAxis) != path || value(bytesAxis) != bytes)
            { out.unresolved.emplace_back("v4_copy_handoff_candidate_file_mismatch"); return out; }
        }
        for (const auto& axis : verified.Axes())
            if ((axis.id.starts_with("V4.file_path.index_") || axis.id.starts_with("V4.file_content.index_")) &&
                !representedAxes.contains(axis.id))
            { out.unresolved.emplace_back("v4_copy_handoff_extra_file_input_unimplemented"); return out; }
        const auto sameId = [](const FILE_ID_INFO& a, const FILE_ID_INFO& b) {
            return a.VolumeSerialNumber == b.VolumeSerialNumber &&
                std::memcmp(a.FileId.Identifier, b.FileId.Identifier, sizeof(a.FileId.Identifier)) == 0 &&
                std::any_of(std::begin(a.FileId.Identifier), std::end(a.FileId.Identifier), [](BYTE x) { return x != 0; });
        };
        FILE_ID_INFO root{};
        if (!GetFileInformationByHandleEx(payload.rootHandle, FileIdInfo, &root, sizeof(root)) || !sameId(root, payload.rootId))
        { out.unresolved.emplace_back("v4_copy_handoff_root_unavailable"); return out; }
        struct RootGuard { HANDLE h; ~RootGuard() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); } };
        const auto namedRootMatches = [&] {
            RootGuard named{CreateFileW(payload.root.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
            FILE_ID_INFO id{}; FILE_ATTRIBUTE_TAG_INFO tag{};
            return named.h != INVALID_HANDLE_VALUE && GetFileInformationByHandleEx(named.h, FileIdInfo, &id, sizeof(id)) &&
                sameId(id, root) && GetFileInformationByHandleEx(named.h, FileAttributeTagInfo, &tag, sizeof(tag)) &&
                (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
        };
        if (!namedRootMatches()) { out.unresolved.emplace_back("v4_copy_handoff_named_root_unavailable"); return out; }
        for (const auto& file : payload.files)
        {
            FILE_ID_INFO id{};
            if (!file.created || !file.identityObserved || !file.bytesObserved || file.copyObservationHandle ||
                !file.handle || file.handle == INVALID_HANDLE_VALUE ||
                file.path.parent_path() != payload.root || !GetFileInformationByHandleEx(file.handle, FileIdInfo, &id, sizeof(id)) ||
                !sameId(id, file.identity))
            { out.unresolved.emplace_back("v4_copy_handoff_creation_lease_unavailable"); return out; }
        }
        out.files.resize(payload.files.size()); out.candidateId = raw.id;
        out.unresolved = {"v4_copy_handoff_later_path_or_content_change_unresolved",
            "v4_copy_handoff_actual_shell_read_and_business_unobserved",
            "v4_move_delete_operation_handoff_unimplemented"};
        if (pinned)
            out.unresolved.emplace_back(mode == V4FileCopyHandoffMode::single_file_pinned_leave ?
                "v4_pinned_leave_handoff_shell_access_and_same_object_read_unverified" :
                "v4_pinned_copy_handoff_shell_access_and_same_object_read_unverified");
        // From this point every failure leaves a partial, explicitly
        // unavailable payload. No path is removed or recreated here.
        payload.status = V4IndependentFileStatus::unimplemented;
        for (std::size_t i = 0; i < payload.files.size(); ++i)
        {
            auto& file = payload.files[i]; auto& record = out.files[i];
            if (!namedRootMatches()) { out.unresolved.emplace_back("v4_copy_handoff_named_root_changed"); return out; }
            if (pinned || sharedMove)
            {
                struct Guard { HANDLE h = INVALID_HANDLE_VALUE; ~Guard() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); } };
                record.observationOpenAttempted = true;
                Guard bridge{CreateFileW(file.path.c_str(), GENERIC_READ | FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | (sharedMove ? FILE_SHARE_DELETE : 0), nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
                FILE_ID_INFO bridgeId{}; FILE_ATTRIBUTE_TAG_INFO bridgeTag{};
                if (bridge.h == INVALID_HANDLE_VALUE || !GetFileInformationByHandleEx(bridge.h, FileIdInfo, &bridgeId, sizeof(bridgeId)) ||
                    !sameId(bridgeId, file.identity) || !GetFileInformationByHandleEx(bridge.h, FileAttributeTagInfo, &bridgeTag, sizeof(bridgeTag)) ||
                    (bridgeTag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || !namedRootMatches())
                { out.unresolved.emplace_back("v4_pinned_copy_handoff_bridge_unavailable"); return out; }
                record.creationHandleCloseAttempted = true;
                if (!CloseHandle(file.handle)) { out.unresolved.emplace_back("v4_copy_handoff_creation_close_unavailable"); return out; }
                file.handle = std::exchange(bridge.h, INVALID_HANDLE_VALUE); record.creationHandleClosed = true;
                if (!sharedMove)
                {
                Guard final{CreateFileW(file.path.c_str(), GENERIC_READ | FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
                FILE_ID_INFO finalId{}; FILE_ATTRIBUTE_TAG_INFO finalTag{};
                if (final.h == INVALID_HANDLE_VALUE || !GetFileInformationByHandleEx(final.h, FileIdInfo, &finalId, sizeof(finalId)) ||
                    !sameId(finalId, file.identity) || !GetFileInformationByHandleEx(final.h, FileAttributeTagInfo, &finalTag, sizeof(finalTag)) ||
                    (finalTag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || !namedRootMatches())
                { out.unresolved.emplace_back("v4_pinned_copy_handoff_final_unavailable"); return out; }
                if (!CloseHandle(file.handle)) { out.unresolved.emplace_back("v4_pinned_copy_handoff_bridge_close_unavailable"); return out; }
                file.handle = std::exchange(final.h, INVALID_HANDLE_VALUE);
                }
                record.observationOpened = true; file.copyObservationHandle = true;
            }
            else
            {
            record.creationHandleCloseAttempted = true;
            if (!CloseHandle(file.handle)) { out.unresolved.emplace_back("v4_copy_handoff_creation_close_unavailable"); return out; }
            file.handle = INVALID_HANDLE_VALUE; record.creationHandleClosed = true;
            record.observationOpenAttempted = true;
            file.handle = CreateFileW(file.path.c_str(), GENERIC_READ | FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (file.handle == INVALID_HANDLE_VALUE) { out.unresolved.emplace_back("v4_copy_handoff_observation_open_unavailable"); return out; }
            record.observationOpened = true; file.copyObservationHandle = true;
            }
            FILE_ID_INFO id{}, afterId{}; FILE_ATTRIBUTE_TAG_INFO tag{}; FILE_BASIC_INFO before{}, after{};
            LARGE_INTEGER size{}, afterSize{}, zero{};
            if (!GetFileInformationByHandleEx(file.handle, FileIdInfo, &id, sizeof(id)) || !sameId(id, file.identity) ||
                !GetFileInformationByHandleEx(file.handle, FileAttributeTagInfo, &tag, sizeof(tag)) ||
                (tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
                !GetFileInformationByHandleEx(file.handle, FileBasicInfo, &before, sizeof(before)) ||
                !GetFileSizeEx(file.handle, &size) || size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) != file.bytes.size() ||
                !SetFilePointerEx(file.handle, zero, nullptr, FILE_BEGIN))
            { out.unresolved.emplace_back("v4_copy_handoff_identity_or_size_unavailable"); return out; }
            std::size_t offset = 0; char buffer[8192];
            while (offset < file.bytes.size())
            {
                const auto amount = static_cast<DWORD>((std::min)(sizeof(buffer), file.bytes.size() - offset)); DWORD got = 0;
                if (!ReadFile(file.handle, buffer, amount, &got, nullptr) || !got || std::memcmp(buffer, file.bytes.data() + offset, got))
                { out.unresolved.emplace_back("v4_copy_handoff_bytes_unavailable"); return out; }
                offset += got;
            }
            if (!GetFileInformationByHandleEx(file.handle, FileIdInfo, &afterId, sizeof(afterId)) || !sameId(afterId, id) ||
                !GetFileInformationByHandleEx(file.handle, FileBasicInfo, &after, sizeof(after)) ||
                before.LastWriteTime.QuadPart != after.LastWriteTime.QuadPart ||
                !GetFileSizeEx(file.handle, &afterSize) || afterSize.QuadPart != size.QuadPart)
            { out.unresolved.emplace_back("v4_copy_handoff_file_changed_during_observation"); return out; }
            struct Guard { HANDLE h; ~Guard() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); } };
            Guard named{CreateFileW(file.path.c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
            FILE_ID_INFO namedId{};
            if (named.h == INVALID_HANDLE_VALUE || !GetFileInformationByHandleEx(named.h, FileIdInfo, &namedId, sizeof(namedId)) ||
                !sameId(namedId, id) || !GetFileInformationByHandleEx(named.h, FileAttributeTagInfo, &tag, sizeof(tag)) ||
                (tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
            { out.unresolved.emplace_back("v4_copy_handoff_named_file_unavailable"); return out; }
            record.identityAndBytesObserved = true;
        }
        if (!GetFileInformationByHandleEx(payload.rootHandle, FileIdInfo, &root, sizeof(root)) || !sameId(root, payload.rootId) ||
            !namedRootMatches())
        { out.unresolved.emplace_back("v4_copy_handoff_root_changed"); return out; }
        if (pinned || sharedMove)
        {
            auto lease = std::shared_ptr<V4PinnedCopyLease>(new V4PinnedCopyLease());
            lease->rootPath_ = payload.root; lease->path_ = payload.files[0].path;
            lease->candidateId_ = raw.id; lease->bytes_ = payload.files[0].bytes;
            lease->rootId_ = payload.rootId; lease->fileId_ = payload.files[0].identity;
            const HANDLE process = GetCurrentProcess();
            if (!DuplicateHandle(process, payload.rootHandle, process, &lease->root_, 0, FALSE, DUPLICATE_SAME_ACCESS) ||
                !DuplicateHandle(process, payload.files[0].handle, process, &lease->file_, 0, FALSE, DUPLICATE_SAME_ACCESS))
            { out.unresolved.emplace_back("v4_pinned_copy_handoff_lease_unavailable"); return out; }
            if (sharedMove)
            {
                auto moveLease = std::shared_ptr<V4SharedMoveLease>(new V4SharedMoveLease());
                moveLease->identity_ = std::move(lease); out.sharedMoveLease = std::move(moveLease);
                out.unresolved.emplace_back("v4_shared_move_name_and_content_can_change_shell_open_unverified");
            }
            else out.pinnedLease = std::move(lease);
        }
        payload.status = V4IndependentFileStatus::input_prepared; out.prepared = true;
        return out;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; return out; }
    catch (...) { return out; }
}

inline bool V4PinnedCopyLease::Revalidate(const V4FamilyValidatedContext& verified,
    const event_graph::V4FamilyIdentityContext& identity, const V4IndependentFilePayload& payload) const
{
    try
    {
        if (root_ == INVALID_HANDLE_VALUE || file_ == INVALID_HANDLE_VALUE ||
            verified.Status() != V4CoordinateStatus::coordinate_ready || identity.Status() != V4FamilyStatus::schema_only ||
            verified.TaskBActive() || verified.FamilyBinding() != identity.FamilyBinding() ||
            payload.status != V4IndependentFileStatus::input_prepared || payload.files.size() != 1 ||
            !payload.files[0].copyObservationHandle || payload.root != rootPath_ || payload.files[0].path != path_ ||
            payload.candidateId != candidateId_ || payload.files[0].bytes != bytes_ || path_.parent_path() != rootPath_)
            return false;
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id != candidateId_) return false;
        const auto sameId = [](const FILE_ID_INFO& a, const FILE_ID_INFO& b) {
            return a.VolumeSerialNumber == b.VolumeSerialNumber &&
                std::memcmp(a.FileId.Identifier, b.FileId.Identifier, sizeof(a.FileId.Identifier)) == 0 &&
                std::any_of(std::begin(a.FileId.Identifier), std::end(a.FileId.Identifier), [](BYTE x) { return x != 0; });
        };
        FILE_ID_INFO id{};
        if (!GetFileInformationByHandleEx(root_, FileIdInfo, &id, sizeof(id)) || !sameId(id, rootId_) ||
            !GetFileInformationByHandleEx(file_, FileIdInfo, &id, sizeof(id)) || !sameId(id, fileId_) ||
            !payload.rootHandle || payload.rootHandle == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandleEx(payload.rootHandle, FileIdInfo, &id, sizeof(id)) || !sameId(id, rootId_) ||
            !payload.files[0].handle || payload.files[0].handle == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandleEx(payload.files[0].handle, FileIdInfo, &id, sizeof(id)) || !sameId(id, fileId_)) return false;
        struct Guard { HANDLE h; ~Guard() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); } };
        Guard root{CreateFileW(rootPath_.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        Guard file{CreateFileW(path_.c_str(), GENERIC_READ | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        FILE_ATTRIBUTE_TAG_INFO tag{};
        if (root.h == INVALID_HANDLE_VALUE || file.h == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandleEx(root.h, FileIdInfo, &id, sizeof(id)) || !sameId(id, rootId_) ||
            !GetFileInformationByHandleEx(root.h, FileAttributeTagInfo, &tag, sizeof(tag)) ||
            !(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            !GetFileInformationByHandleEx(file.h, FileIdInfo, &id, sizeof(id)) || !sameId(id, fileId_) ||
            !GetFileInformationByHandleEx(file.h, FileAttributeTagInfo, &tag, sizeof(tag)) ||
            (tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return false;
        FILE_BASIC_INFO before{}, after{}; LARGE_INTEGER size{}, afterSize{};
        if (!GetFileInformationByHandleEx(file.h, FileBasicInfo, &before, sizeof(before)) || !GetFileSizeEx(file.h, &size) ||
            size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) != bytes_.size()) return false;
        std::size_t offset = 0; char buffer[8192];
        while (offset < bytes_.size())
        {
            DWORD read = 0; const auto n = static_cast<DWORD>((std::min)(sizeof(buffer), bytes_.size() - offset));
            if (!ReadFile(file.h, buffer, n, &read, nullptr) || !read || std::memcmp(buffer, bytes_.data() + offset, read)) return false;
            offset += read;
        }
        return GetFileInformationByHandleEx(file.h, FileIdInfo, &id, sizeof(id)) && sameId(id, fileId_) &&
            GetFileInformationByHandleEx(file.h, FileBasicInfo, &after, sizeof(after)) &&
            before.LastWriteTime.QuadPart == after.LastWriteTime.QuadPart && GetFileSizeEx(file.h, &afterSize) &&
            afterSize.QuadPart == size.QuadPart;
    }
    catch (...) { return false; }
}

inline bool V4IndependentShellOwner::AttachPinnedCopyLease(const V4FamilyValidatedContext& verified,
    const event_graph::V4FamilyIdentityContext& identity, const std::shared_ptr<V4IndependentFilePayload>& payload,
    const std::shared_ptr<const V4PinnedCopyLease>& lease)
{
    if (!TryBeginExecution()) return false;
    const auto release = [](const V4IndependentShellOwner* owner) noexcept { owner->EndExecution(); };
    const std::unique_ptr<const V4IndependentShellOwner, decltype(release)> guard(this, release);
    try
    {
        const auto canonicalLease = lease ? lease->weak_from_this().lock() : std::shared_ptr<const V4PinnedCopyLease>{};
        if (!Live() || !source_ || !payload || payload_ != payload || payload_.owner_before(payload) ||
            payload.owner_before(payload_) || !lease || canonicalLease != lease ||
            canonicalLease.owner_before(lease) || lease.owner_before(canonicalLease) || retainedPinnedLease_ || retainedMoveLease_ ||
            !lease->Revalidate(verified, identity, *payload)) return false;
        auto retained = std::make_unique<std::shared_ptr<const V4PinnedCopyLease>>(lease);
        retainedPinnedLease_ = std::move(retained);
        return true;
    }
    catch (...) { return false; }
}

inline V4SharedMoveSourceAfter V4SharedMoveLease::ObserveSourceAfter() const
{
    V4SharedMoveSourceAfter out;
    try
    {
        if (!identity_ || identity_->root_ == INVALID_HANDLE_VALUE || identity_->file_ == INVALID_HANDLE_VALUE) return out;
        const auto same = [](const FILE_ID_INFO& a, const FILE_ID_INFO& b) {
            return a.VolumeSerialNumber == b.VolumeSerialNumber &&
                std::memcmp(a.FileId.Identifier, b.FileId.Identifier, sizeof(a.FileId.Identifier)) == 0 &&
                std::any_of(std::begin(a.FileId.Identifier), std::end(a.FileId.Identifier), [](BYTE x) { return x != 0; });
        };
        FILE_ID_INFO rootId{}, held{};
        if (!GetFileInformationByHandleEx(identity_->root_, FileIdInfo, &rootId, sizeof(rootId)) || !same(rootId, identity_->rootId_) ||
            !GetFileInformationByHandleEx(identity_->file_, FileIdInfo, &held, sizeof(held)) ||
            std::none_of(std::begin(held.FileId.Identifier), std::end(held.FileId.Identifier), [](BYTE x) { return x != 0; })) return out;
        out.heldId = held; out.heldIdentityMatches = same(held, identity_->fileId_);
        struct Guard { HANDLE h; ~Guard() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); } };
        Guard root{CreateFileW(identity_->rootPath_.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        FILE_ID_INFO namedRoot{}; FILE_ATTRIBUTE_TAG_INFO tag{};
        if (root.h == INVALID_HANDLE_VALUE || !GetFileInformationByHandleEx(root.h, FileIdInfo, &namedRoot, sizeof(namedRoot)) ||
            !same(namedRoot, rootId) || !GetFileInformationByHandleEx(root.h, FileAttributeTagInfo, &tag, sizeof(tag)) ||
            !(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return out;
        SetLastError(ERROR_SUCCESS);
        Guard named{CreateFileW(identity_->path_.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        const DWORD nameError = named.h == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
        if (named.h == INVALID_HANDLE_VALUE)
        {
            if (nameError != ERROR_FILE_NOT_FOUND) return out;
            out.originalNamePresent = false;
        }
        else
        {
            FILE_ID_INFO id{};
            if (!GetFileInformationByHandleEx(named.h, FileIdInfo, &id, sizeof(id)) ||
                std::none_of(std::begin(id.FileId.Identifier), std::end(id.FileId.Identifier), [](BYTE x) { return x != 0; }) ||
                !GetFileInformationByHandleEx(named.h, FileAttributeTagInfo, &tag, sizeof(tag)) ||
                (tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return out;
            out.namedId = id; out.originalNamePresent = true; out.originalNameMatchesSource = same(id, identity_->fileId_);
        }
        Guard reopened{ReOpenFile(identity_->file_, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0)};
        if (reopened.h != INVALID_HANDLE_VALUE)
        {
            FILE_ID_INFO id{}; FILE_BASIC_INFO before{}, after{}; LARGE_INTEGER size{}, end{};
            if (GetFileInformationByHandleEx(reopened.h, FileIdInfo, &id, sizeof(id)) && same(id, identity_->fileId_) &&
                GetFileInformationByHandleEx(reopened.h, FileBasicInfo, &before, sizeof(before)) && GetFileSizeEx(reopened.h, &size) && size.QuadPart >= 0)
            {
                bool matches = static_cast<std::uint64_t>(size.QuadPart) == identity_->bytes_.size();
                std::size_t offset = 0; char buffer[8192]; bool complete = matches;
                while (complete && offset < identity_->bytes_.size())
                {
                    DWORD read = 0; const auto amount = static_cast<DWORD>((std::min)(sizeof(buffer), identity_->bytes_.size() - offset));
                    if (!ReadFile(reopened.h, buffer, amount, &read, nullptr) || !read) { complete = false; break; }
                    if (std::memcmp(buffer, identity_->bytes_.data() + offset, read)) matches = false;
                    offset += read;
                }
                if (complete && GetFileInformationByHandleEx(reopened.h, FileBasicInfo, &after, sizeof(after)) &&
                    before.LastWriteTime.QuadPart == after.LastWriteTime.QuadPart && before.ChangeTime.QuadPart == after.ChangeTime.QuadPart &&
                    GetFileSizeEx(reopened.h, &end) && end.QuadPart == size.QuadPart) out.heldBytesMatch = matches;
            }
        }
        out.observed = out.heldIdentityMatches.has_value() && out.originalNamePresent.has_value();
        if (!out.heldBytesMatch) out.unresolved.emplace_back("v4_move_after_held_bytes_unavailable");
        out.unresolved.emplace_back("v4_move_after_output_path_and_identity_unobserved");
        return out;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; return out; }
    catch (...) { return out; }
}

inline bool V4IndependentShellOwner::AttachSharedMoveLease(const V4FamilyValidatedContext& verified,
    const event_graph::V4FamilyIdentityContext& identity, const std::shared_ptr<V4IndependentFilePayload>& payload,
    const std::shared_ptr<const V4SharedMoveLease>& lease)
{
    if (!TryBeginExecution()) return false;
    const auto release = [](const V4IndependentShellOwner* owner) noexcept { owner->EndExecution(); };
    const std::unique_ptr<const V4IndependentShellOwner, decltype(release)> guard(this, release);
    try
    {
        const auto canonical = lease ? lease->weak_from_this().lock() : std::shared_ptr<const V4SharedMoveLease>{};
        if (!Live() || !source_ || !payload || payload_ != payload || payload_.owner_before(payload) || payload.owner_before(payload_) ||
            !lease || canonical != lease || canonical.owner_before(lease) || lease.owner_before(canonical) ||
            retainedMoveLease_ || retainedPinnedLease_ || !lease->RevalidateBefore(verified, identity, *payload)) return false;
        retainedMoveLease_ = std::make_unique<std::shared_ptr<const V4SharedMoveLease>>(lease); return true;
    }
    catch (...) { return false; }
}
inline bool V4IndependentShellOwner::OwnsSharedMoveLease(const std::shared_ptr<V4IndependentFilePayload>& payload,
    const std::shared_ptr<const V4SharedMoveLease>& lease) const noexcept
{
    return Live() && source_ && payload && payload_ == payload && !payload_.owner_before(payload) && !payload.owner_before(payload_) &&
        lease && !lease->CandidateId().empty() && payload->candidateId == lease->CandidateId() && retainedMoveLease_ &&
        *retainedMoveLease_ == lease && !retainedMoveLease_->owner_before(lease) && !lease.owner_before(*retainedMoveLease_);
}
inline bool V4IndependentShellOwner::MatchesSharedMoveLeaseBefore(const V4FamilyValidatedContext& verified,
    const event_graph::V4FamilyIdentityContext& identity, const std::shared_ptr<V4IndependentFilePayload>& payload,
    const std::shared_ptr<const V4SharedMoveLease>& lease) const
{ return OwnsSharedMoveLease(payload, lease) && lease->RevalidateBefore(verified, identity, *payload); }

inline bool V4IndependentShellOwner::MatchesPinnedCopyLease(const V4FamilyValidatedContext& verified,
    const event_graph::V4FamilyIdentityContext& identity, const std::shared_ptr<V4IndependentFilePayload>& payload,
    const std::shared_ptr<const V4PinnedCopyLease>& lease) const
{
    return Live() && source_ && payload && payload_ == payload && !payload_.owner_before(payload) &&
        !payload.owner_before(payload_) && lease && retainedPinnedLease_ && *retainedPinnedLease_ == lease &&
        !retainedPinnedLease_->owner_before(lease) && !lease.owner_before(*retainedPinnedLease_) &&
        lease->Revalidate(verified, identity, *payload);
}

inline V4IndependentShellSource PrepareV4IndependentShellSource(
    const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
    const std::shared_ptr<V4IndependentFilePayload>& payloadOwner, const std::shared_ptr<V4IndependentShellOwner>& owner)
{
    V4IndependentShellSource out;
    const auto pinnedPayload = payloadOwner;
    const auto pinnedOwner = owner;
    try
    {
        if (!pinnedPayload || !pinnedOwner || !pinnedOwner->TryBeginPreparation())
        { out.unresolved.emplace_back("v4_multi_shell_payload_or_preparation_unavailable"); return out; }
        const auto end = [](const V4IndependentShellOwner* value) noexcept { value->EndExecution(); };
        const std::unique_ptr<const V4IndependentShellOwner, decltype(end)> guard(pinnedOwner.get(), end);
        const auto& payload = *pinnedPayload;
        if (verified.Status() != V4CoordinateStatus::coordinate_ready || identity.Status() != V4FamilyStatus::schema_only ||
            verified.TaskBActive() || verified.FamilyBinding() != identity.FamilyBinding() ||
            verified.Axes().size() != identity.Axes().size() || payload.status != V4IndependentFileStatus::input_prepared ||
            payload.files.empty() || payload.files.size() > (std::numeric_limits<UINT>::max)() || !pinnedOwner->Live())
        { out.unresolved.emplace_back("v4_multi_shell_candidate_or_file_leases_unavailable"); return out; }
        for (std::size_t i = 0; i < identity.Axes().size(); ++i)
        {
            const auto& a = verified.Axes()[i]; const auto& b = identity.Axes()[i];
            if (a.id != b.id || a.role != b.role || a.values != b.values || a.label != b.label)
            { out.unresolved.emplace_back("v4_multi_shell_schema_mismatch"); return out; }
        }
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        if (raw.resourceUnavailable) out.resourceUnavailable = true;
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty() || raw.id != payload.candidateId)
        { out.unresolved.emplace_back("v4_multi_shell_candidate_identity_unavailable"); return out; }
        const auto value = [&](std::string_view id) { return Value(verified.Axes(), verified.Coordinate(), id); };
        if ((value("V4.source.kind") != "file_only" && value("V4.source.kind") != "file_and_content") ||
            value("T04.capability") != "enabled")
        { out.unresolved.emplace_back("v4_multi_shell_source_or_async_mode_unimplemented"); return out; }
        APTTYPE apartment{}; APTTYPEQUALIFIER qualifier{};
        if (FAILED(CoGetApartmentType(&apartment, &qualifier)) || (apartment != APTTYPE_STA && apartment != APTTYPE_MAINSTA))
        { out.unresolved.emplace_back("v4_multi_shell_sta_unavailable"); return out; }
        const auto sameId = [](const FILE_ID_INFO& a, const FILE_ID_INFO& b) {
            return a.VolumeSerialNumber == b.VolumeSerialNumber &&
                std::memcmp(a.FileId.Identifier, b.FileId.Identifier, sizeof(a.FileId.Identifier)) == 0 &&
                std::any_of(std::begin(a.FileId.Identifier), std::end(a.FileId.Identifier), [](BYTE x) { return x != 0; });
        };
        FILE_ID_INFO rootId{}; FILE_ATTRIBUTE_TAG_INFO tag{};
        if (!payload.rootHandle || payload.rootHandle == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandleEx(payload.rootHandle, FileIdInfo, &rootId, sizeof(rootId)) || !sameId(rootId, payload.rootId) ||
            !GetFileInformationByHandleEx(payload.rootHandle, FileAttributeTagInfo, &tag, sizeof(tag)) ||
            !(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        { out.unresolved.emplace_back("v4_multi_shell_root_lease_unavailable"); return out; }
        struct Guard { HANDLE h; ~Guard() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); } };
        const auto namedRootMatches = [&] {
            Guard named{CreateFileW(payload.root.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
            FILE_ID_INFO namedId{}; FILE_ATTRIBUTE_TAG_INFO namedTag{};
            return named.h != INVALID_HANDLE_VALUE && GetFileInformationByHandleEx(named.h, FileIdInfo, &namedId, sizeof(namedId)) &&
                sameId(namedId, rootId) && GetFileInformationByHandleEx(named.h, FileAttributeTagInfo, &namedTag, sizeof(namedTag)) &&
                (namedTag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(namedTag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
        };
        if (!namedRootMatches()) { out.unresolved.emplace_back("v4_multi_shell_root_name_unavailable"); return out; }
        const auto count = value("V4.source.file_count");
        const auto expectedCount = payload.files.size() == 1 ? std::string("one") : payload.files.size() == 2 ? std::string("two") :
            std::string("count_") + std::to_string(payload.files.size());
        if (count != expectedCount) { out.unresolved.emplace_back("v4_multi_shell_count_mismatch"); return out; }
        constexpr char digits[] = "0123456789abcdef";
        const auto byteHex = [&](std::string& dst, unsigned byte) { dst.push_back(digits[byte >> 4]); dst.push_back(digits[byte & 15]); };
        std::vector<std::filesystem::path> paths; paths.reserve(payload.files.size());
        std::set<std::string> representedAxes;
        for (std::size_t i = 0; i < payload.files.size(); ++i)
        {
            const auto& file = payload.files[i]; const auto index = std::to_string(i);
            representedAxes.insert("V4.file_path.index_" + index + ".utf16le");
            representedAxes.insert("V4.file_content.index_" + index + ".bytes");
            if (!file.created || !file.identityObserved || !file.bytesObserved || !file.copyObservationHandle ||
                !file.handle || file.handle == INVALID_HANDLE_VALUE ||
                file.path.parent_path() != payload.root || value("V4.file.member" + index + "_kind") != "file" ||
                value("V4.file.member" + index + "_identity") != "unique")
            { out.unresolved.emplace_back("v4_multi_shell_file_lease_or_kind_unavailable"); return out; }
            std::string pathValue = "utf16le_hex_", bytesValue = "hex_";
            for (const auto ch : file.path.wstring())
            { const auto unit = static_cast<unsigned short>(ch); byteHex(pathValue, unit & 255); byteHex(pathValue, unit >> 8); }
            for (const auto byte : file.bytes) byteHex(bytesValue, std::to_integer<unsigned>(byte));
            if (value("V4.file_path.index_" + index + ".utf16le") != pathValue ||
                value("V4.file_content.index_" + index + ".bytes") != bytesValue)
            { out.unresolved.emplace_back("v4_multi_shell_file_candidate_mismatch"); return out; }
            FILE_ID_INFO id{}; LARGE_INTEGER size{}, zero{};
            if (!GetFileInformationByHandleEx(file.handle, FileIdInfo, &id, sizeof(id)) || !sameId(id, file.identity) ||
                !GetFileInformationByHandleEx(file.handle, FileAttributeTagInfo, &tag, sizeof(tag)) ||
                (tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
                !GetFileSizeEx(file.handle, &size) || size.QuadPart < 0 ||
                static_cast<std::uint64_t>(size.QuadPart) != file.bytes.size() ||
                !SetFilePointerEx(file.handle, zero, nullptr, FILE_BEGIN))
            { out.unresolved.emplace_back("v4_multi_shell_file_identity_or_size_unavailable"); return out; }
            std::size_t offset = 0; char buffer[8192];
            while (offset < file.bytes.size())
            {
                const auto amount = static_cast<DWORD>((std::min)(sizeof(buffer), file.bytes.size() - offset)); DWORD got = 0;
                if (!ReadFile(file.handle, buffer, amount, &got, nullptr) || !got || std::memcmp(buffer, file.bytes.data() + offset, got))
                { out.unresolved.emplace_back("v4_multi_shell_file_bytes_unavailable"); return out; }
                offset += got;
            }
            Guard named{CreateFileW(file.path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
            FILE_ID_INFO namedId{};
            if (named.h == INVALID_HANDLE_VALUE || !GetFileInformationByHandleEx(named.h, FileIdInfo, &namedId, sizeof(namedId)) ||
                !sameId(namedId, id) || !GetFileInformationByHandleEx(named.h, FileAttributeTagInfo, &tag, sizeof(tag)) ||
                (tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
            { out.unresolved.emplace_back("v4_multi_shell_named_file_unavailable"); return out; }
            for (std::size_t j = 0; j < i; ++j)
                if (sameId(payload.files[j].identity, id))
                { out.unresolved.emplace_back("v4_multi_shell_file_alias_unimplemented"); return out; }
            paths.push_back(file.path);
        }
        for (const auto& axis : verified.Axes())
            if ((axis.id.starts_with("V4.file_path.index_") || axis.id.starts_with("V4.file_content.index_")) &&
                !representedAxes.contains(axis.id))
            { out.unresolved.emplace_back("v4_multi_shell_extra_file_input_unimplemented"); return out; }
        auto retained = std::make_unique<std::shared_ptr<V4IndependentFilePayload>>(pinnedPayload);
        auto source = ole_file_source_fixture::Make(paths, true);
        if (!source || !source->HasConstructionShell() || source->owner != GetCurrentThreadId() ||
            !source->asynchronous || source->ConstructionPaths().size() != paths.size())
        { out.unresolved.emplace_back("v4_multi_shell_construction_unavailable"); return out; }
        for (std::size_t i = 0; i < paths.size(); ++i)
        {
            auto native = paths[i]; native.make_preferred();
            if (source->ConstructionPaths()[i] != native)
            { out.unresolved.emplace_back("v4_multi_shell_construction_order_unavailable"); return out; }
        }
        if (!namedRootMatches())
        { out.unresolved.emplace_back("v4_multi_shell_root_name_changed"); return out; }
        auto* const sourceIdentity = source.Get();
        if (!pinnedOwner->Attach(std::move(source), pinnedPayload, std::move(retained)))
        { out.unresolved.emplace_back("v4_multi_shell_owner_attach_unavailable"); return out; }
        out.owner_ = pinnedOwner; out.sourceIdentity_ = sourceIdentity;
        out.ownerThread = GetCurrentThreadId(); out.candidateId = raw.id; out.inputPrepared = true;
        out.unresolved = {"v4_multi_shell_actual_formats_and_getdata_order_unobserved",
            "v4_multi_shell_other_candidate_axes_and_lease_handoff_unimplemented",
            "v4_multi_shell_production_dispatch_completion_and_business_unimplemented"};
        return out;
    }
    catch (const std::bad_alloc&) { out.inputPrepared = false; out.resourceUnavailable = true; return out; }
    catch (const std::length_error&) { out.inputPrepared = false; out.resourceUnavailable = true; return out; }
    catch (...) { out.inputPrepared = false; return out; }
}

enum class V4FileDropOrderStatus { unavailable, resource_unavailable, observed_model_order, observed_permutation };
struct V4FileDropOrderObservation
{
    V4FileDropOrderStatus status = V4FileDropOrderStatus::unavailable;
    std::string candidateId;
    std::size_t readIndex = 0;
    HRESULT sourceResult = E_PENDING;
    DWORD thread = 0;
    ole_file_source_fixture::FileDropCapture capture = ole_file_source_fixture::FileDropCapture::provider_failed;
    int startsAtEntry = 0, endsAtEntry = 0;
    // Actual CF_HDROP position -> exact model file-member index.
    std::vector<std::size_t> modelIndexByActualOrder;
    std::vector<std::filesystem::path> actualPaths;
    std::vector<std::string> unresolved;
};

// Read the fixture's already-recorded GetData observation; never request
// data here. Different order is a legitimate observed input, not a product
// failure. A receipt/epoch must still prove the selected call's production
// provenance; source logs and these non-atomic path observations alone do not.
inline V4FileDropOrderObservation ObserveV4FileDropOrder(const V4FamilyValidatedContext& verified,
    const event_graph::V4FamilyIdentityContext& identity, const std::shared_ptr<V4IndependentFilePayload>& payload,
    const V4IndependentShellSource& binding, const std::shared_ptr<V4IndependentShellOwner>& owner,
    std::size_t readIndex)
{
    V4FileDropOrderObservation out; out.readIndex = readIndex;
    try
    {
        if (!payload || !owner || binding.owner_.lock() != owner || owner->payload_ != payload || !owner->Live() ||
            !binding.inputPrepared || binding.resourceUnavailable || payload->status != V4IndependentFileStatus::input_prepared ||
            verified.Status() != V4CoordinateStatus::coordinate_ready || identity.Status() != V4FamilyStatus::schema_only ||
            verified.TaskBActive() || verified.FamilyBinding() != identity.FamilyBinding() ||
            verified.Axes().size() != identity.Axes().size())
        { out.unresolved.emplace_back("v4_file_order_context_or_owner_unavailable"); return out; }
        for (std::size_t i = 0; i < identity.Axes().size(); ++i)
        {
            const auto& a = verified.Axes()[i]; const auto& b = identity.Axes()[i];
            if (a.id != b.id || a.role != b.role || a.values != b.values || a.label != b.label)
            { out.unresolved.emplace_back("v4_file_order_schema_mismatch"); return out; }
        }
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        if (raw.resourceUnavailable) out.status = V4FileDropOrderStatus::resource_unavailable;
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty() || raw.id != payload->candidateId || raw.id != binding.candidateId)
        { out.unresolved.emplace_back("v4_file_order_candidate_unavailable"); return out; }
        auto* source = binding.SourceObject();
        if (!source || source != owner->source_.Get() || source->wrongApartment || source->fileDropObservationFailed ||
            source->owner != GetCurrentThreadId() || source->fileReads < 0 ||
            static_cast<std::size_t>(source->fileReads) != source->fileDropReads.size() ||
            readIndex >= source->fileDropReads.size() || source->ConstructionPaths().size() != payload->files.size())
        { out.unresolved.emplace_back("v4_file_order_source_log_unavailable"); return out; }
        const auto& read = source->fileDropReads[readIndex];
        out.sourceResult = read.result; out.thread = read.threadId; out.capture = read.capture;
        out.startsAtEntry = read.startsAtEntry; out.endsAtEntry = read.endsAtEntry;
        out.actualPaths = read.paths; // preserve actual data even if it cannot be mapped
        if (read.threadId != source->owner || FAILED(read.result) || read.capture != ole_file_source_fixture::FileDropCapture::captured ||
            read.paths.size() != payload->files.size() || payload->files.empty())
        { out.unresolved.emplace_back("v4_file_order_actual_payload_unavailable"); return out; }
        const auto value = [&](std::string_view id) { return Value(verified.Axes(), verified.Coordinate(), id); };
        const auto count = payload->files.size() == 1 ? std::string("one") : payload->files.size() == 2 ? std::string("two") :
            std::string("count_") + std::to_string(payload->files.size());
        if ((value("V4.source.kind") != "file_only" && value("V4.source.kind") != "file_and_content") ||
            value("V4.source.file_count") != count)
        { out.unresolved.emplace_back("v4_file_order_source_shape_unavailable"); return out; }
        const auto sameId = [](const FILE_ID_INFO& a, const FILE_ID_INFO& b) {
            return a.VolumeSerialNumber == b.VolumeSerialNumber &&
                std::memcmp(a.FileId.Identifier, b.FileId.Identifier, sizeof(a.FileId.Identifier)) == 0 &&
                std::any_of(std::begin(a.FileId.Identifier), std::end(a.FileId.Identifier), [](BYTE x) { return x != 0; });
        };
        FILE_ID_INFO rootId{};
        if (!payload->rootHandle || payload->rootHandle == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandleEx(payload->rootHandle, FileIdInfo, &rootId, sizeof(rootId)) || !sameId(rootId, payload->rootId))
        { out.unresolved.emplace_back("v4_file_order_root_lease_unavailable"); return out; }
        struct Guard { HANDLE h; ~Guard() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); } };
        const auto namedRootMatches = [&] {
            Guard root{CreateFileW(payload->root.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
            FILE_ID_INFO id{}; FILE_ATTRIBUTE_TAG_INFO tag{};
            return root.h != INVALID_HANDLE_VALUE && GetFileInformationByHandleEx(root.h, FileIdInfo, &id, sizeof(id)) &&
                sameId(id, rootId) && GetFileInformationByHandleEx(root.h, FileAttributeTagInfo, &tag, sizeof(tag)) &&
                (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
        };
        if (!namedRootMatches()) { out.unresolved.emplace_back("v4_file_order_root_name_unavailable"); return out; }
        constexpr char digits[] = "0123456789abcdef";
        const auto appendByte = [&](std::string& outValue, unsigned byte) { outValue.push_back(digits[byte >> 4]); outValue.push_back(digits[byte & 15]); };
        std::set<std::string> represented;
        for (std::size_t i = 0; i < payload->files.size(); ++i)
        {
            const auto& file = payload->files[i]; const auto index = std::to_string(i);
            const auto pathAxis = "V4.file_path.index_" + index + ".utf16le";
            const auto bytesAxis = "V4.file_content.index_" + index + ".bytes";
            represented.insert(pathAxis); represented.insert(bytesAxis);
            auto native = file.path; native.make_preferred();
            std::string path = "utf16le_hex_", bytes = "hex_";
            for (const auto ch : file.path.wstring())
            { const auto unit = static_cast<unsigned short>(ch); appendByte(path, unit & 255); appendByte(path, unit >> 8); }
            for (const auto byte : file.bytes) appendByte(bytes, std::to_integer<unsigned>(byte));
            FILE_ID_INFO held{};
            if (!file.created || !file.identityObserved || !file.bytesObserved || !file.copyObservationHandle ||
                !file.handle || file.handle == INVALID_HANDLE_VALUE || file.path.parent_path() != payload->root ||
                source->ConstructionPaths()[i] != native || value("V4.file.member" + index + "_kind") != "file" ||
                value("V4.file.member" + index + "_identity") != "unique" || value(pathAxis) != path || value(bytesAxis) != bytes ||
                !GetFileInformationByHandleEx(file.handle, FileIdInfo, &held, sizeof(held)) || !sameId(held, file.identity))
            { out.unresolved.emplace_back("v4_file_order_model_member_unavailable"); return out; }
            for (std::size_t j = 0; j < i; ++j)
                if (sameId(file.identity, payload->files[j].identity))
                { out.unresolved.emplace_back("v4_file_order_alias_unimplemented"); return out; }
        }
        for (const auto& axis : verified.Axes())
            if ((axis.id.starts_with("V4.file_path.index_") || axis.id.starts_with("V4.file_content.index_")) &&
                !represented.contains(axis.id))
            { out.unresolved.emplace_back("v4_file_order_extra_input_unavailable"); return out; }
        std::vector<bool> used(payload->files.size(), false); bool inOrder = true;
        out.modelIndexByActualOrder.reserve(read.paths.size());
        for (std::size_t actual = 0; actual < read.paths.size(); ++actual)
        {
            const auto observed = read.paths[actual].native();
            if (observed.empty() || observed.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
            { out.unresolved.emplace_back("v4_file_order_actual_path_unavailable"); return out; }
            std::size_t match = payload->files.size();
            for (std::size_t i = 0; i < payload->files.size(); ++i)
            {
                auto expectedPath = payload->files[i].path; expectedPath.make_preferred();
                const auto expected = expectedPath.native();
                if (expected.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
                { out.unresolved.emplace_back("v4_file_order_expected_path_unavailable"); return out; }
                const int compared = CompareStringOrdinal(observed.data(), static_cast<int>(observed.size()),
                    expected.data(), static_cast<int>(expected.size()), FALSE);
                if (!compared) { out.unresolved.emplace_back("v4_file_order_path_comparison_unavailable"); return out; }
                if (compared == CSTR_EQUAL)
                { if (match != payload->files.size()) { out.unresolved.emplace_back("v4_file_order_path_ambiguous"); return out; } match = i; }
            }
            if (match == payload->files.size() || used[match] || !namedRootMatches())
            { out.unresolved.emplace_back("v4_file_order_member_or_root_unavailable"); return out; }
            // Open only a model-owned path, never an arbitrary source path.
            Guard named{CreateFileW(payload->files[match].path.c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
            FILE_ID_INFO id{}; FILE_ATTRIBUTE_TAG_INFO tag{};
            if (named.h == INVALID_HANDLE_VALUE || !GetFileInformationByHandleEx(named.h, FileIdInfo, &id, sizeof(id)) ||
                !sameId(id, payload->files[match].identity) || !GetFileInformationByHandleEx(named.h, FileAttributeTagInfo, &tag, sizeof(tag)) ||
                (tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
            { out.unresolved.emplace_back("v4_file_order_named_identity_unavailable"); return out; }
            used[match] = true; out.modelIndexByActualOrder.push_back(match); inOrder &= match == actual;
        }
        if (!namedRootMatches()) { out.unresolved.emplace_back("v4_file_order_root_changed"); return out; }
        out.candidateId = raw.id;
        out.unresolved = {"v4_file_order_selected_production_call_and_epoch_unverified",
            "v4_file_order_shared_file_contents_and_path_observation_non_atomic",
            "v4_file_order_product_completion_and_business_unverified"};
        out.status = inOrder ? V4FileDropOrderStatus::observed_model_order : V4FileDropOrderStatus::observed_permutation;
        return out;
    }
    catch (const std::bad_alloc&) { out.status = V4FileDropOrderStatus::resource_unavailable; return out; }
    catch (const std::length_error&) { out.status = V4FileDropOrderStatus::resource_unavailable; return out; }
    catch (...) { out.status = V4FileDropOrderStatus::unavailable; return out; }
}

// Narrow read-only evidence for future same-root A/B input constructors. It
// validates explicit B-to-A directory references and every selected member's
// cross-task object identity against actual filesystem objects. This is not a
// joint graph constructor, a whole-model check, a volume check, a host binding,
// or product execution. The missing joint graph and business work remain
// explicit even when the declared cross-task references are verified.
struct JointPayloadReferenceEvidence
{
    bool sharedRootObserved = false;
    bool declaredCrossTaskReferencesObserved = false;
    std::size_t directoryReferencesObserved = 0;
    std::size_t memberIdentitiesObserved = 0;
    std::string error;
    std::vector<std::string> unresolved{
        "joint_directory_graph_construction_and_local_topology_unimplemented",
        "cross_task_snapshot_not_atomic",
        "joint_host_binding_and_product_execution_unimplemented"};
};

// Compare live, simultaneous handles rather than the legacy 64-bit file
// index, which need not uniquely identify objects on every filesystem.
// Unsupported IDs and reparse points remain unresolved. This is a point-in-
// time observation, not an atomic guarantee for future path operations.
inline bool ObserveJointSameObject(const std::filesystem::path& a, const std::filesystem::path& b, bool& equal)
{
    equal = false;
    if (a.empty() || b.empty()) return false;
#if !defined(_WIN32)
    return false;
#else
    struct Handle final
    {
        HANDLE value = INVALID_HANDLE_VALUE;
        ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    };
    const auto open = [](const std::filesystem::path& path) {
        return CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    };
    Handle left, right;
    left.value = open(a); right.value = open(b);
    if (left.value == INVALID_HANDLE_VALUE || right.value == INVALID_HANDLE_VALUE) return false;
    FILE_ATTRIBUTE_TAG_INFO leftAttributes{}, rightAttributes{};
    FILE_ID_INFO leftId{}, rightId{};
    if (!GetFileInformationByHandleEx(left.value, FileAttributeTagInfo, &leftAttributes, sizeof(leftAttributes)) ||
        !GetFileInformationByHandleEx(right.value, FileAttributeTagInfo, &rightAttributes, sizeof(rightAttributes)) ||
        (leftAttributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        (rightAttributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        !GetFileInformationByHandleEx(left.value, FileIdInfo, &leftId, sizeof(leftId)) ||
        !GetFileInformationByHandleEx(right.value, FileIdInfo, &rightId, sizeof(rightId))) return false;
    const auto hasId = [](const FILE_ID_INFO& info) {
        return std::any_of(std::begin(info.FileId.Identifier), std::end(info.FileId.Identifier), [](auto byte) { return byte != 0; });
    };
    if (!hasId(leftId) || !hasId(rightId)) return false;
    equal = leftId.VolumeSerialNumber == rightId.VolumeSerialNumber &&
        std::equal(std::begin(leftId.FileId.Identifier), std::end(leftId.FileId.Identifier), std::begin(rightId.FileId.Identifier));
    return true;
#endif
}

// Ordered input observation for a future real multi-member OLE adapter. This
// does not query IDataObject, construct Shell PIDLs, dispatch a drag, decide a
// drop, or assert its results. File IDs and bytes are point-in-time snapshots;
// neither path stability nor the Shell's delivered ordering is guaranteed.
struct V2OrderedSourceObservation
{
    bool observed = false;
    bool commonParentObserved = false;
    std::vector<std::filesystem::path> orderedPaths;
    std::vector<std::size_t> entryIndices;
    std::string error;
    std::vector<std::string> unresolved{
        "source_snapshot_not_atomic_or_held_through_dispatch",
        "shell_actual_offered_order_duplicates_and_data_unobserved",
        "production_member_ownership_completion_and_results_unobserved"};
};

inline V2OrderedSourceObservation InspectV2OrderedSource(const std::vector<Axis>& axes,
    const Candidate& candidate, const PayloadFixtureResult& fixture, bool secondTask = false)
{
    V2OrderedSourceObservation result;
    const auto fail = [&](std::string reason) {
        result.observed = false;
        result.commonParentObserved = false;
        result.orderedPaths.clear();
        result.entryIndices.clear();
        result.error = "ordered_source_input_observation_unavailable:" + reason;
        return result;
    };
    try
    {
        std::vector<Axis> schema;
        Candidate safe;
        if (const auto reason = ValidatePayloadCandidate(axes, candidate, schema, safe,
                SelectionSchemaVersion::source_members_v2); !reason.empty()) return fail(reason);
        const auto fullValues = safe.values;
        if (!SelectPayloadTask(schema, safe, secondTask)) return fail("ordered_source_task_unavailable");
        if (!SelectionV2HasExplicitFileSource(schema, safe)) return fail("ordered_source_vector_semantics_unimplemented");
        const int count = SelectionSourceSize(schema, safe);
        if (count <= 0 || static_cast<std::size_t>(count) != fixture.members.size() || !fixture.payload_prepared ||
            fixture.axes.size() != schema.size()) return fail("ordered_source_count_or_fixture_unavailable");
        for (std::size_t i = 0; i < schema.size(); ++i)
            if (fixture.axes[i].id != schema[i].id || fixture.axes[i].value != fullValues[i])
                return fail("ordered_source_candidate_binding_unavailable");
        const auto before = CapturePayloadInputSnapshot(fixture);
        if (!before.captured || before.memberEntryIndices.size() != fixture.members.size())
            return fail("ordered_source_snapshot_unavailable");
        const auto parsePrior = [](std::string_view ref, std::size_t limit, std::size_t& prior) {
            constexpr std::string_view prefix = "member_";
            if (ref.substr(0, prefix.size()) != prefix) return false;
            const auto digits = ref.substr(prefix.size());
            if (digits.empty() || (digits.size() > 1 && digits.front() == '0')) return false;
            prior = 0;
            for (const auto digit : digits)
            {
                if (digit < '0' || digit > '9' || prior > 128) return false;
                prior = prior * 10 + static_cast<std::size_t>(digit - '0');
            }
            return prior < limit;
        };
        bool commonParent = true;
        for (std::size_t i = 0; i < fixture.members.size(); ++i)
        {
            const auto& member = fixture.members[i];
            const auto index = before.memberEntryIndices[i];
            const auto kind = Value(schema, safe, i == 0 ? "P01.primary" : i == 1 ? "P01.second" :
                "P01.member" + std::to_string(i) + "_kind");
            if (index >= before.entries.size() || before.entries[index].relative != member.path.lexically_relative(fixture.directory) ||
                member.kind != kind || before.entries[index].kind != member.kind ||
                (member.kind == "file" && before.entries[index].bytes != member.bytes))
                return fail("ordered_source_member_snapshot_mismatch:" + std::to_string(i));
            if (member.kind == "directory")
            {
                std::vector<PayloadFixtureTreeEntry> tree;
                for (const auto& entry : before.entries)
                {
                    const auto full = (before.root / entry.relative).lexically_normal();
                    const auto relative = full.lexically_relative(member.path);
                    if (relative.empty() || relative == std::filesystem::path(".") ||
                        (relative.begin() != relative.end() && *relative.begin() == std::filesystem::path(".."))) continue;
                    tree.push_back({relative, entry.kind, entry.bytes});
                }
                std::sort(tree.begin(), tree.end(), [](const auto& a, const auto& b) {
                    return a.relative.generic_wstring() < b.relative.generic_wstring();
                });
                if (tree.size() != member.tree.size()) return fail("ordered_source_directory_tree_mismatch:" + std::to_string(i));
                for (std::size_t j = 0; j < tree.size(); ++j)
                    if (tree[j].relative != member.tree[j].relative || tree[j].kind != member.tree[j].kind ||
                        tree[j].bytes != member.tree[j].bytes)
                        return fail("ordered_source_directory_tree_mismatch:" + std::to_string(i));
            }
            if (i)
            {
                bool sameParent = false;
                if (!ObserveJointSameObject(member.path.parent_path(), fixture.members.front().path.parent_path(), sameParent))
                    return fail("ordered_source_parent_identity_unavailable");
                commonParent = commonParent && sameParent;
                const auto ref = Value(schema, safe, "P03.member" + std::to_string(i) + "_identity_ref");
                std::size_t prior = 0;
                if (ref == "unique")
                {
                    for (std::size_t j = 0; j < i; ++j)
                    {
                        bool equal = false;
                        if (before.memberEntryIndices[j] == index ||
                            !ObserveJointSameObject(member.path, fixture.members[j].path, equal) || equal)
                            return fail("ordered_source_member_identity_unavailable:" + std::to_string(i));
                    }
                }
                else
                {
                    bool equal = false;
                    if (!parsePrior(ref, i, prior) || !ObserveJointSameObject(member.path, fixture.members[prior].path, equal) || !equal)
                        return fail("ordered_source_member_alias_unavailable:" + std::to_string(i));
                    const auto priorIndex = before.memberEntryIndices[prior];
                    if (priorIndex >= before.entries.size()) return fail("ordered_source_member_alias_snapshot_unavailable");
                    const auto& current = before.entries[index]; const auto& previous = before.entries[priorIndex];
                    if (current.volumeSerial != previous.volumeSerial || current.fileIndexHigh != previous.fileIndexHigh ||
                        current.fileIndexLow != previous.fileIndexLow)
                        return fail("ordered_source_member_alias_snapshot_unavailable");
                }
            }
            result.orderedPaths.push_back(member.path);
            result.entryIndices.push_back(index);
        }
        const auto after = CapturePayloadInputSnapshot(fixture);
        if (!after.captured || after.entries.size() != before.entries.size() ||
            after.memberEntryIndices != before.memberEntryIndices || after.root != before.root)
            return fail("ordered_source_changed_during_observation");
        for (std::size_t i = 0; i < before.entries.size(); ++i)
        {
            const auto& a = before.entries[i]; const auto& b = after.entries[i];
            if (a.relative != b.relative || a.kind != b.kind || a.bytes != b.bytes ||
                a.volumeSerial != b.volumeSerial || a.fileIndexHigh != b.fileIndexHigh || a.fileIndexLow != b.fileIndexLow)
                return fail("ordered_source_changed_during_observation");
        }
        result.observed = true;
        result.commonParentObserved = commonParent;
        if (!commonParent) result.unresolved.emplace_back("shell_multiple_parent_provider_unimplemented");
        return result;
    }
    catch (...) { return fail("ordered_source_observation_exception_unimplemented"); }
}

inline JointPayloadReferenceEvidence ObserveJointPayloadReferences(const std::vector<Axis>& axes,
    const Candidate& candidate, const PayloadFixtureResult& first, const PayloadFixtureResult& second,
    SelectionSchemaVersion version = SelectionSchemaVersion::legacy_v1)
{
    JointPayloadReferenceEvidence evidence;
    const auto fail = [&](std::string reason) { evidence.error = std::move(reason); return evidence; };
    std::vector<Axis> schema;
    Candidate safe;
    if (const auto reason = ValidatePayloadCandidate(axes, candidate, schema, safe, version); !reason.empty()) return fail(reason);
    if (Value(schema, safe, "X03.task_count") != "two") return fail("joint_requires_two_tasks_unimplemented");
    Candidate taskB = safe;
    if (!SelectPayloadTask(schema, taskB, true)) return fail("joint_second_task_unimplemented");
    if (version == SelectionSchemaVersion::source_members_v2 &&
        (!SelectionV2HasExplicitFileSource(schema, safe) || !SelectionV2HasExplicitFileSource(schema, taskB)))
        return fail("joint_v2_source_vector_format_semantics_unimplemented");
    if (!first.payload_prepared || !second.payload_prepared || first.directory.empty() || second.directory.empty())
        return fail("joint_prepared_inputs_unavailable");
    const auto matchesCandidate = [&](const PayloadFixtureResult& fixture) {
        if (fixture.axes.size() != schema.size()) return false;
        for (std::size_t i = 0; i < schema.size(); ++i)
            if (fixture.axes[i].id != schema[i].id || fixture.axes[i].value != safe.values[i]) return false;
        return true;
    };
    if (!matchesCandidate(first) || !matchesCandidate(second)) return fail("joint_candidate_identity_mismatch");
    bool sameRoot = false;
    if (!ObserveJointSameObject(first.directory, second.directory, sameRoot) || !sameRoot)
        return fail("joint_same_isolated_root_required_unimplemented");
    evidence.sharedRootObserved = true;
    const auto aSnapshot = CapturePayloadInputSnapshot(first);
    const auto bSnapshot = CapturePayloadInputSnapshot(second);
    if (!aSnapshot.captured || !bSnapshot.captured) return fail("joint_actual_snapshot_unavailable");
    const int aCount = version == SelectionSchemaVersion::source_members_v2
        ? SelectionSourceSize(schema, safe) : SelectionSize(schema, safe);
    const int bCount = version == SelectionSchemaVersion::source_members_v2
        ? SelectionSourceSize(schema, taskB) : SelectionSize(schema, taskB);
    if (aCount < 0 || bCount < 0 || first.members.size() != static_cast<std::size_t>(aCount) ||
        second.members.size() != static_cast<std::size_t>(bCount) ||
        aSnapshot.memberEntryIndices.size() != first.members.size() || bSnapshot.memberEntryIndices.size() != second.members.size())
        return fail("joint_selected_members_unavailable");
    const auto sameObject = [&](const std::filesystem::path& a, const std::filesystem::path& b, bool& equal) {
        return ObserveJointSameObject(a, b, equal);
    };
    const auto memberKind = [&](const Candidate& task, std::size_t i) {
        return Value(schema, task, i == 0 ? "P01.primary" : i == 1 ? "P01.second" : "P01.member" + std::to_string(i) + "_kind");
    };
    const auto validMembers = [&](const PayloadFixtureResult& fixture, const PayloadInputSnapshot& snapshot, const Candidate& task) {
        for (std::size_t i = 0; i < fixture.members.size(); ++i)
        {
            const auto index = snapshot.memberEntryIndices[i];
            if (index >= snapshot.entries.size() || snapshot.entries[index].kind != fixture.members[i].kind ||
                fixture.members[i].kind != memberKind(task, i)) return false;
            if (fixture.members[i].kind == "file" && snapshot.entries[index].bytes != fixture.members[i].bytes) return false;
        }
        return true;
    };
    if (!validMembers(first, aSnapshot, safe) || !validMembers(second, bSnapshot, taskB))
        return fail("joint_actual_member_identity_or_bytes_unavailable");
    const auto withinTaskIdentitiesAgree = [&](const PayloadFixtureResult& fixture, const Candidate& task) {
        for (std::size_t i = 1; i < fixture.members.size(); ++i)
        {
            const auto ref = Value(schema, task, "P03.member" + std::to_string(i) + "_identity_ref");
            if (ref == "unique")
            {
                for (std::size_t j = 0; j < i; ++j)
                {
                    bool equal = false;
                    if (!sameObject(fixture.members[i].path, fixture.members[j].path, equal) || equal) return false;
                }
                continue;
            }
            constexpr std::string_view prefix = "member_";
            if (ref.substr(0, prefix.size()) != prefix) return false;
            const auto digits = ref.substr(prefix.size());
            if (digits.empty() || (digits.size() > 1 && digits.front() == '0')) return false;
            std::size_t prior = 0;
            for (const auto c : digits)
            {
                if (c < '0' || c > '9' || prior > 128) return false;
                prior = prior * 10 + static_cast<std::size_t>(c - '0');
            }
            bool equal = false;
            if (prior >= i || !sameObject(fixture.members[i].path, fixture.members[prior].path, equal) || !equal) return false;
        }
        return true;
    };
    if (!withinTaskIdentitiesAgree(first, safe) || !withinTaskIdentitiesAgree(second, taskB))
        return fail("joint_within_task_member_identity_unimplemented");
    for (std::size_t i = 0; i < first.members.size(); ++i)
        if (Value(schema, safe, "P03.member" + std::to_string(i) + "_cross_task_identity") != "na")
            return fail("joint_task_a_cross_identity_unimplemented:" + std::to_string(i));
    const auto aNode = [&](std::string_view reference, std::filesystem::path& path) {
        if (reference == "task_a_isolated_root") { path = first.directory; return true; }
        if (reference == "task_a_isolated_root_parent") { path = first.directory.parent_path(); return true; }
        if (reference == "task_a_target_directory") { path = first.targetDirectory; return !path.empty(); }
        if (reference == "task_a_desktop_directory") { path = first.desktopDirectory; return !path.empty(); }
        if (reference == "task_a_source_mapping_directory") { path = first.sourceMappingDirectory; return !path.empty(); }
        if (reference == "task_a_target_mapping_directory") { path = first.targetMappingDirectory; return !path.empty(); }
        constexpr std::string_view parent = "task_a_member_", object = "task_a_member_object_";
        const bool isObject = reference.substr(0, object.size()) == object;
        if (!isObject && reference.substr(0, parent.size()) != parent) return false;
        const auto digits = reference.substr(isObject ? object.size() : parent.size());
        if (digits.empty() || (digits.size() > 1 && digits.front() == '0')) return false;
        std::size_t index = 0;
        for (const auto c : digits) { if (c < '0' || c > '9' || index > 128) return false; index = index * 10 + static_cast<std::size_t>(c - '0'); }
        if (index >= first.members.size() || (isObject && first.members[index].kind != "directory")) return false;
        path = isObject ? first.members[index].path : first.members[index].path.parent_path();
        return true;
    };
    const auto verifyRef = [&](std::string_view field, const std::filesystem::path& actual) {
        const auto ref = Value(schema, taskB, field);
        if (ref.substr(0, 7) != "task_a_") return true; // Only explicit cross-task refs are covered.
        std::filesystem::path expected;
        bool equal = false;
        if (!aNode(ref, expected) || !sameObject(actual, expected, equal) || !equal) return false;
        ++evidence.directoryReferencesObserved;
        return true;
    };
    if (!verifyRef("R02.target_directory_identity", second.targetDirectory) ||
        !verifyRef("R02.target_directory_parent_ref", second.targetDirectory.parent_path()))
        return fail("joint_target_directory_reference_unimplemented");
    const std::array<std::pair<std::string_view, const std::filesystem::path*>, 3> endpoints{{
        {"desktop_directory", &second.desktopDirectory}, {"source_mapping_directory", &second.sourceMappingDirectory},
        {"target_mapping_directory", &second.targetMappingDirectory}}};
    for (const auto& [name, path] : endpoints)
        if (!verifyRef("R02." + std::string(name) + "_identity", *path) ||
            !verifyRef("R02." + std::string(name) + "_parent_ref", path->parent_path()))
            return fail("joint_endpoint_directory_reference_unimplemented:" + std::string(name));
    for (std::size_t i = 0; i < second.members.size(); ++i)
    {
        const auto stem = "R02.member" + std::to_string(i);
        const auto& member = second.members[i];
        if (!verifyRef(stem + "_directory_ref", member.path.parent_path()) ||
            !verifyRef(stem + "_directory_object_ref", member.kind == "directory" ? member.path : std::filesystem::path{}) ||
            !verifyRef(stem + "_directory_parent_ref", member.path.parent_path().parent_path()))
            return fail("joint_member_directory_reference_unimplemented:" + std::to_string(i));
        const auto ref = Value(schema, taskB, "P03.member" + std::to_string(i) + "_cross_task_identity");
        if (ref == "unique")
        {
            for (const auto& a : first.members)
            {
                bool equal = false;
                if (!sameObject(member.path, a.path, equal) || equal) return fail("joint_member_unique_identity_unimplemented:" + std::to_string(i));
            }
        }
        else
        {
            constexpr std::string_view prefix = "task_a_member_";
            if (ref.substr(0, prefix.size()) != prefix) return fail("joint_member_cross_identity_unimplemented:" + std::to_string(i));
            const auto digits = ref.substr(prefix.size());
            if (digits.empty() || (digits.size() > 1 && digits.front() == '0')) return fail("joint_member_cross_identity_unimplemented:" + std::to_string(i));
            std::size_t index = 0;
            for (const auto c : digits) { if (c < '0' || c > '9' || index > 128) return fail("joint_member_cross_identity_unimplemented:" + std::to_string(i)); index = index * 10 + static_cast<std::size_t>(c - '0'); }
            bool equal = false;
            if (index >= first.members.size() || !sameObject(member.path, first.members[index].path, equal) || !equal)
                return fail("joint_member_cross_identity_mismatch:" + std::to_string(i));
        }
        ++evidence.memberIdentitiesObserved;
    }
    evidence.declaredCrossTaskReferencesObserved = true;
    return evidence;
}

// Restricted input construction: B reuses A's members or creates unique
// files and directories inside the owned root. B member parent references may
// point forward to an already resolvable B member directory object. No second
// root or product action exists. Other nodes and actual volume relations stay
// unresolved; a successful subset is not a complete candidate or test.
struct JointPayloadFixtureResult
{
    PayloadFixtureResult first, second;
    JointPayloadReferenceEvidence evidence;
    bool sharedSubsetPrepared = false;
    std::string error;
    std::vector<std::string> unresolved{
        "joint_b_additional_directory_identities_and_unmodelled_topology_unimplemented",
        "joint_volume_host_binding_and_product_execution_unimplemented"};
};

inline JointPayloadFixtureResult PrepareJointSharedPayloadSubset(const std::vector<Axis>& axes,
    const Candidate& candidate, const std::filesystem::path& temporaryParent,
    SelectionSchemaVersion version = SelectionSchemaVersion::legacy_v1)
{
    JointPayloadFixtureResult result;
    const auto fail = [&](std::string reason) {
        // A later B operation may already have changed A's selected directory
        // trees. A failed joint preparation must not expose either partial
        // fixture as a valid standalone input; retain the owned root for cleanup.
        result.first.payload_prepared = false;
        result.first.candidate_constructed = false;
        result.second.payload_prepared = false;
        result.second.candidate_constructed = false;
        result.sharedSubsetPrepared = false;
        result.error = std::move(reason);
        return result;
    };
    try
    {
    std::vector<Axis> schema;
    Candidate a;
    if (const auto reason = ValidatePayloadCandidate(axes, candidate, schema, a, version); !reason.empty()) return fail(reason);
    Candidate b = a;
    if (!SelectPayloadTask(schema, b, true)) return fail("joint_requires_two_tasks_unimplemented");
    const int aCount = version == SelectionSchemaVersion::source_members_v2
        ? SelectionSourceSize(schema, a) : SelectionSize(schema, a);
    const int bCount = version == SelectionSchemaVersion::source_members_v2
        ? SelectionSourceSize(schema, b) : SelectionSize(schema, b);
    if (aCount <= 0 || bCount <= 0 || aCount > 129 || bCount > 129)
        return fail("joint_selection_count_unimplemented");
    const auto kind = [&](const Candidate& task, int i) {
        return Value(schema, task, i == 0 ? "P01.primary" : i == 1 ? "P01.second" : "P01.member" + std::to_string(i) + "_kind");
    };
    const auto v = [&](std::string_view key) { return Value(schema, b, key); };
    if (version == SelectionSchemaVersion::source_members_v2 && !SelectionV2HasExplicitFileSource(schema, b))
        return fail("joint_v2_source_vector_format_semantics_unimplemented");
    const std::string actualCount = bCount == 1 ? "one" : bCount == 2 ? "two" : "count_" + std::to_string(bCount);
    if (v("P02.source_actual_count") != actualCount) return fail("joint_second_source_count_unimplemented");
    const auto parseIndex = [](std::string_view ref, std::string_view prefix, std::size_t limit, std::size_t& index) {
        if (ref.substr(0, prefix.size()) != prefix) return false;
        const auto digits = ref.substr(prefix.size());
        if (digits.empty() || (digits.size() > 1 && digits.front() == '0')) return false;
        index = 0;
        for (const auto c : digits) { if (c < '0' || c > '9' || index > 128) return false; index = index * 10 + static_cast<std::size_t>(c - '0'); }
        return index < limit;
    };
    std::vector<std::size_t> sourceIndices;
    const auto uniqueB = (std::numeric_limits<std::size_t>::max)();
    for (int i = 0; i < bCount; ++i)
    {
        const auto crossIdentity = v("P03.member" + std::to_string(i) + "_cross_task_identity");
        if (crossIdentity == "unique")
        {
            if (kind(b, i) != "file" && kind(b, i) != "directory")
                return fail("joint_second_unique_kind_unimplemented:" + std::to_string(i));
            sourceIndices.push_back(uniqueB);
            continue;
        }
        std::size_t index = 0;
        if (!parseIndex(crossIdentity, "task_a_member_", static_cast<std::size_t>(aCount), index) ||
            kind(b, i) != kind(a, static_cast<int>(index)) || (kind(b, i) != "file" && kind(b, i) != "directory"))
            return fail("joint_second_member_shared_source_unimplemented:" + std::to_string(i));
        sourceIndices.push_back(index);
    }
    const auto targetRef = v("R02.target_directory_identity");
    if (targetRef != "unique" && targetRef != "isolated_root" && targetRef != "desktop_directory" &&
        targetRef != "source_mapping_directory" && targetRef != "target_mapping_directory" && targetRef.substr(0, 7) != "task_a_")
        return fail("joint_second_target_identity_unimplemented");
    // Construct A only after the supported B member and target subset is known.
    result.first = PreparePayloadFixture(schema, candidate, temporaryParent, false, version);
    if (!result.first.payload_prepared) return fail("joint_first_payload_unimplemented:" + result.first.error);
    auto& second = result.second;
    second.directory = result.first.directory;
    second.axes = result.first.axes;
    const auto aNode = [&](std::string_view ref, std::filesystem::path& path) {
        if (ref == "task_a_isolated_root") { path = result.first.directory; return true; }
        if (ref == "task_a_isolated_root_parent") { path = result.first.directory.parent_path(); return true; }
        if (ref == "task_a_target_directory") { path = result.first.targetDirectory; return !path.empty(); }
        if (ref == "task_a_desktop_directory") { path = result.first.desktopDirectory; return !path.empty(); }
        if (ref == "task_a_source_mapping_directory") { path = result.first.sourceMappingDirectory; return !path.empty(); }
        if (ref == "task_a_target_mapping_directory") { path = result.first.targetMappingDirectory; return !path.empty(); }
        std::size_t index = 0;
        if (parseIndex(ref, "task_a_member_object_", result.first.members.size(), index))
        { if (result.first.members[index].kind != "directory") return false; path = result.first.members[index].path; return true; }
        if (parseIndex(ref, "task_a_member_", result.first.members.size(), index))
        { path = result.first.members[index].path.parent_path(); return true; }
        return false;
    };
    const auto inRoot = [&](const std::filesystem::path& path) {
        const auto relative = path.lexically_relative(second.directory);
        return !relative.empty() && (relative.begin() == relative.end() || *relative.begin() != std::filesystem::path(".."));
    };
    // Creation is limited to existing parents observed inside the owned root.
    // The caller owns the returned root on any later partial failure.
    const auto ownedParent = [&](const std::filesystem::path& parent) {
        if (parent.empty() || !inRoot(parent)) return false;
        const auto before = CapturePayloadInputSnapshot(result.first);
        if (!before.captured) return false;
        const auto parentRelative = parent.lexically_relative(second.directory);
        if (std::none_of(before.entries.begin(), before.entries.end(), [&](const auto& entry) {
                return entry.relative == parentRelative && entry.kind == "directory";
            })) return false;
        std::error_code error;
        const auto root = std::filesystem::weakly_canonical(second.directory, error);
        if (error) return false;
        const auto canonicalParent = std::filesystem::weakly_canonical(parent, error);
        if (error) return false;
        const auto relative = canonicalParent.lexically_relative(root);
        return !relative.empty() && (relative.begin() == relative.end() || *relative.begin() != std::filesystem::path(".."));
    };
    // Resolve B slots independently of model order. A member's directory may
    // be a later declared B directory object; only already-created parents
    // are used for filesystem writes. Cycles and missing nodes remain open.
    std::vector<PayloadFixtureMember> slots(sourceIndices.size()), addedMembers;
    std::vector<bool> ready(sourceIndices.size(), false), createdUniqueDirectory(sourceIndices.size(), false);
    std::size_t remaining = sourceIndices.size();
    while (remaining)
    {
        bool progressed = false;
        for (std::size_t i = 0; i < sourceIndices.size(); ++i)
        {
            if (ready[i]) continue;
            PayloadFixtureMember member;
            if (sourceIndices[i] != uniqueB) member = result.first.members[sourceIndices[i]];
            else
            {
            const auto identity = i == 0 ? std::string_view("unique") : v("P03.member" + std::to_string(i) + "_identity_ref");
            std::size_t prior = 0;
            if (identity != "unique")
            {
                if (!parseIndex(identity, "member_", i, prior) || sourceIndices[prior] != uniqueB ||
                    kind(b, static_cast<int>(i)) != kind(b, static_cast<int>(prior)))
                    return fail("joint_unique_member_alias_unimplemented:" + std::to_string(i));
                if (!ready[prior]) continue;
                member = slots[prior];
            }
            else
            {
                const auto parentRef = v("R02.member" + std::to_string(i) + "_directory_ref");
                std::filesystem::path parent;
                if (parentRef == "isolated_root") parent = second.directory;
                else if (parentRef.substr(0, 7) == "task_a_") (void)aNode(parentRef, parent);
                else if (parseIndex(parentRef, "member_object_", slots.size(), prior))
                {
                    if (!ready[prior]) continue;
                    if (slots[prior].kind != "directory") return fail("joint_unique_member_parent_type_unimplemented:" + std::to_string(i));
                    parent = slots[prior].path;
                }
                else if (parseIndex(parentRef, "member_", slots.size(), prior))
                {
                    if (!ready[prior]) continue;
                    parent = slots[prior].path.parent_path();
                }
                if (!ownedParent(parent)) return fail("joint_unique_member_parent_unimplemented:" + std::to_string(i));
                const bool directory = kind(b, static_cast<int>(i)) == "directory";
                if (directory && v("R02.member" + std::to_string(i) + "_directory_object_ref") != "unique")
                    return fail("joint_unique_directory_object_identity_unimplemented:" + std::to_string(i));
                const auto path = parent / ("joint_b_member_" + std::to_string(i) + (directory ? "" : ".txt"));
                const auto bytes = "joint-b-payload-member-" + std::to_string(i) + "\n";
#if !defined(_WIN32)
                return fail("joint_unique_member_creation_unimplemented");
#else
                if (directory)
                {
                    if (!CreateDirectoryW(path.c_str(), nullptr)) return fail("joint_unique_directory_create_failed:" + std::to_string(i));
                    member = {path, "directory", {}, i, {}};
                    createdUniqueDirectory[i] = true;
                }
                else
                {
                const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file == INVALID_HANDLE_VALUE) return fail("joint_unique_member_create_failed:" + std::to_string(i));
                const auto expectedBytes = static_cast<DWORD>(bytes.size());
                DWORD written = 0;
                const bool wrote = WriteFile(file, bytes.data(), expectedBytes, &written, nullptr) != FALSE && written == expectedBytes;
                const bool closed = CloseHandle(file) != FALSE;
                if (!wrote || !closed) return fail("joint_unique_member_write_failed:" + std::to_string(i));
                member = {path, "file", bytes, i, {}};
                }
                addedMembers.push_back(member);
#endif
            }
            }
            slots[i] = std::move(member);
            ready[i] = true;
            --remaining;
            progressed = true;
        }
        if (!progressed) return fail("joint_member_parent_cycle_or_unresolved_unimplemented");
    }
    for (std::size_t i = 0; i < slots.size(); ++i)
    {
        auto member = std::move(slots[i]);
        member.identity = second.members.size();
        for (std::size_t prior = 0; prior < second.members.size(); ++prior)
        {
            bool same = false;
            if (!ObserveJointSameObject(member.path, second.members[prior].path, same)) return fail("joint_member_identity_unavailable");
            if (same) { member.identity = second.members[prior].identity; break; }
        }
        second.members.push_back(std::move(member));
    }
    for (auto* fixture : {&result.first, &second})
        for (auto& member : fixture->members)
        {
            if (member.kind != "directory") continue;
            for (const auto& added : addedMembers)
            {
                const auto relative = added.path.lexically_relative(member.path);
                if (relative.empty() || relative == std::filesystem::path(".") ||
                    (relative.begin() != relative.end() && *relative.begin() == std::filesystem::path(".."))) continue;
                member.tree.push_back({relative, added.kind, added.bytes});
            }
            std::sort(member.tree.begin(), member.tree.end(), [](const auto& left, const auto& right) {
                return left.relative.generic_wstring() < right.relative.generic_wstring();
            });
        }
    const std::array<std::pair<std::string_view, std::filesystem::path*>, 3> endpoints{{
        {"desktop_directory", &second.desktopDirectory}, {"source_mapping_directory", &second.sourceMappingDirectory},
        {"target_mapping_directory", &second.targetMappingDirectory}}};
    const auto memberNode = [&](std::string_view ref, std::filesystem::path& path) {
        std::size_t index = 0;
        if (parseIndex(ref, "member_object_", second.members.size(), index))
        { if (second.members[index].kind != "directory") return false; path = second.members[index].path; return true; }
        if (parseIndex(ref, "member_", second.members.size(), index))
        { path = second.members[index].path.parent_path(); return true; }
        return false;
    };
    // Resolve endpoints independent of the target first. This permits a target
    // to alias a later endpoint and lets a unique target use an existing parent.
    for (std::size_t pass = 0; pass < endpoints.size(); ++pass)
        for (const auto& [name, path] : endpoints)
        {
            const auto ref = v("R02." + std::string(name) + "_identity");
            if (ref == "na" || !path->empty()) continue;
            if (ref == "isolated_root") *path = second.directory;
            else if (ref.substr(0, 7) == "task_a_") { if (!aNode(ref, *path)) return fail("joint_endpoint_reference_unimplemented:" + std::string(name)); }
            else if (ref.substr(0, 7) == "member_") { if (!memberNode(ref, *path)) return fail("joint_endpoint_member_reference_unimplemented:" + std::string(name)); }
            else for (const auto& [other, otherPath] : endpoints) if (ref == other && !otherPath->empty()) *path = *otherPath;
        }
    if (targetRef == "isolated_root") second.targetDirectory = second.directory;
    else if (targetRef == "unique")
    {
        const auto parentRef = v("R02.target_directory_parent_ref");
        std::filesystem::path parent;
        if (parentRef == "isolated_root") parent = second.directory;
        else if (parentRef.substr(0, 7) == "task_a_") (void)aNode(parentRef, parent);
        else if (parentRef.substr(0, 7) == "member_") (void)memberNode(parentRef, parent);
        else for (const auto& [name, path] : endpoints) if (parentRef == name) parent = *path;
        if (parent.empty() || !inRoot(parent)) return fail("joint_unique_target_parent_unimplemented");
        // Recheck the owned graph and resolved parent before creating a node.
        // This does not make the filesystem snapshot atomic against races.
        const auto before = CapturePayloadInputSnapshot(result.first);
        if (!before.captured) return fail("joint_unique_target_parent_snapshot_unavailable");
        const auto parentRelative = parent.lexically_relative(second.directory);
        if (std::none_of(before.entries.begin(), before.entries.end(), [&](const auto& entry) {
                return entry.relative == parentRelative && entry.kind == "directory";
            })) return fail("joint_unique_target_parent_not_in_snapshot");
        std::error_code canonicalError;
        const auto canonicalRoot = std::filesystem::weakly_canonical(second.directory, canonicalError);
        if (canonicalError) return fail("joint_unique_target_root_unavailable");
        const auto canonicalParent = std::filesystem::weakly_canonical(parent, canonicalError);
        const auto inside = canonicalParent.lexically_relative(canonicalRoot);
        if (canonicalError || inside.empty() ||
            (inside.begin() != inside.end() && *inside.begin() == std::filesystem::path("..")))
            return fail("joint_unique_target_parent_outside_root_unimplemented");
        const auto path = parent / L"joint_b_target";
        std::error_code error;
        if (!std::filesystem::create_directory(path, error) || error)
            return fail("joint_unique_target_creation_failed:" + error.message());
        second.targetDirectory = path;
        // A selected directory can contain the new B target. Keep its planned
        // input tree consistent; this prepares input, not a drop outcome.
        for (auto* fixture : {&result.first, &second})
            for (auto& member : fixture->members)
            {
                if (member.kind != "directory") continue;
                const auto relative = path.lexically_relative(member.path);
                if (relative.empty() || relative == std::filesystem::path(".") ||
                    (relative.begin() != relative.end() && *relative.begin() == std::filesystem::path(".."))) continue;
                member.tree.push_back({relative, "directory", {}});
                std::sort(member.tree.begin(), member.tree.end(), [](const auto& left, const auto& right) {
                    return left.relative.generic_wstring() < right.relative.generic_wstring();
                });
            }
        result.unresolved.emplace_back("joint_unique_target_cross_task_identity_and_volume_unverified");
    }
    else if (targetRef.substr(0, 7) == "task_a_")
    { if (!aNode(targetRef, second.targetDirectory)) return fail("joint_second_target_reference_unimplemented"); }
    else for (const auto& [name, path] : endpoints) if (targetRef == name) second.targetDirectory = *path;
    if (second.targetDirectory.empty() || !inRoot(second.targetDirectory)) return fail("joint_second_target_outside_root_unimplemented");
    for (std::size_t pass = 0; pass < endpoints.size(); ++pass)
        for (const auto& [name, path] : endpoints)
        {
            const auto ref = v("R02." + std::string(name) + "_identity");
            if (ref == "na" || !path->empty()) continue;
            if (ref == "isolated_root") *path = second.directory;
            else if (ref == "target_directory") *path = second.targetDirectory;
            else if (ref.substr(0, 7) == "task_a_") { if (!aNode(ref, *path)) return fail("joint_endpoint_reference_unimplemented:" + std::string(name)); }
            else if (ref.substr(0, 7) == "member_") { if (!memberNode(ref, *path)) return fail("joint_endpoint_member_reference_unimplemented:" + std::string(name)); }
            else for (const auto& [other, otherPath] : endpoints) if (ref == other && !otherPath->empty()) *path = *otherPath;
        }
    for (const auto& [name, path] : endpoints)
    {
        const auto identity = v("R02." + std::string(name) + "_identity");
        if (identity == "na")
        { if (!path->empty() || v("R02." + std::string(name) + "_parent_ref") != "na") return fail("joint_absent_endpoint_unimplemented:" + std::string(name)); }
        else if (path->empty() || !inRoot(*path)) return fail("joint_endpoint_identity_unimplemented:" + std::string(name));
    }
    const auto bNode = [&](std::string_view ref, std::filesystem::path& path) {
        if (ref == "isolated_root") { path = second.directory; return true; }
        if (ref == "isolated_root_parent") { path = second.directory.parent_path(); return true; }
        if (ref == "target_directory") { path = second.targetDirectory; return true; }
        for (const auto& [name, endpoint] : endpoints) if (ref == name) { path = *endpoint; return !path.empty(); }
        if (ref.substr(0, 7) == "task_a_") return aNode(ref, path);
        std::size_t index = 0;
        if (parseIndex(ref, "member_object_", second.members.size(), index))
        { if (second.members[index].kind != "directory") return false; path = second.members[index].path; return true; }
        if (parseIndex(ref, "member_", second.members.size(), index))
        { path = second.members[index].path.parent_path(); return true; }
        return false;
    };
    const auto matches = [&](std::string_view ref, const std::filesystem::path& actual) {
        std::filesystem::path expected;
        bool equal = false;
        return bNode(ref, expected) && ObserveJointSameObject(expected, actual, equal) && equal;
    };
    if (!matches(v("R02.target_directory_parent_ref"), second.targetDirectory.parent_path()))
        return fail("joint_second_target_parent_unimplemented");
    for (const auto& [name, path] : endpoints)
        if (!path->empty() && !matches(v("R02." + std::string(name) + "_parent_ref"), path->parent_path()))
            return fail("joint_endpoint_parent_unimplemented:" + std::string(name));
    for (std::size_t i = 0; i < second.members.size(); ++i)
    {
        const auto stem = "R02.member" + std::to_string(i);
        const auto& member = second.members[i];
        if (!matches(v(stem + "_directory_ref"), member.path.parent_path()) ||
            !matches(v(stem + "_directory_parent_ref"), member.path.parent_path().parent_path()) ||
            (member.kind == "directory" ? (v(stem + "_directory_object_ref") == "unique"
                ? !createdUniqueDirectory[i] : !matches(v(stem + "_directory_object_ref"), member.path))
                                        : v(stem + "_directory_object_ref") != "na"))
            return fail("joint_second_member_topology_unimplemented:" + std::to_string(i));
        const auto relation = v(stem + "_directory_to_target");
        if (relation != "na" && relation != "unknown" && relation != "unresolved")
        {
            const auto parent = member.path.parent_path();
            bool equal = false;
            if (!ObserveJointSameObject(parent, second.targetDirectory, equal)) return fail("joint_second_relation_unavailable:" + std::to_string(i));
            const auto isAncestor = [](const std::filesystem::path& ancestor, const std::filesystem::path& child) {
                const auto relative = child.lexically_relative(ancestor);
                return !relative.empty() && relative != std::filesystem::path(".") &&
                    relative.begin() != relative.end() && *relative.begin() != std::filesystem::path("..");
            };
            const std::string_view actual = equal ? "same" : isAncestor(parent, second.targetDirectory) ? "ancestor" :
                isAncestor(second.targetDirectory, parent) ? "descendant" : "unrelated";
            if (relation != actual) return fail("joint_second_relation_conflict_unimplemented:" + std::to_string(i));
        }
        const auto objectRelation = v(stem + "_target_layout");
        if (objectRelation != "na" && objectRelation != "unknown" && objectRelation != "unresolved")
        {
            bool equal = false;
            if (!ObserveJointSameObject(member.path, second.targetDirectory, equal)) return fail("joint_second_object_relation_unavailable:" + std::to_string(i));
            const auto relative = second.targetDirectory.lexically_relative(member.path);
            const bool descendant = member.kind == "directory" && !relative.empty() && relative != std::filesystem::path(".") &&
                relative.begin() != relative.end() && *relative.begin() != std::filesystem::path("..");
            const std::string_view actual = equal ? "same_as_source" : descendant ? "descendant_of_source" : "unrelated";
            if (objectRelation != actual) return fail("joint_second_object_relation_conflict_unimplemented:" + std::to_string(i));
        }
    }
    second.payload_prepared = true;
    second.unresolved.emplace_back("joint_second_full_input_and_remaining_relations_unimplemented");
    // A unique B target can add a directory inside selected A/B directory
    // objects. Compare each updated planned tree to a fresh physical snapshot.
    const auto verifyTrees = [&](const PayloadFixtureResult& fixture) {
        const auto snapshot = CapturePayloadInputSnapshot(fixture);
        if (!snapshot.captured) return false;
        for (const auto& member : fixture.members)
        {
            if (member.kind != "directory") continue;
            std::vector<PayloadFixtureTreeEntry> observed;
            for (const auto& entry : snapshot.entries)
            {
                const auto full = (snapshot.root / entry.relative).lexically_normal();
                const auto relative = full.lexically_relative(member.path);
                if (relative.empty() || relative == std::filesystem::path(".") ||
                    (relative.begin() != relative.end() && *relative.begin() == std::filesystem::path(".."))) continue;
                observed.push_back({relative, entry.kind, entry.bytes});
            }
            std::sort(observed.begin(), observed.end(), [](const auto& left, const auto& right) {
                return left.relative.generic_wstring() < right.relative.generic_wstring();
            });
            if (observed.size() != member.tree.size()) return false;
            for (std::size_t i = 0; i < observed.size(); ++i)
                if (observed[i].relative != member.tree[i].relative || observed[i].kind != member.tree[i].kind ||
                    observed[i].bytes != member.tree[i].bytes) return false;
        }
        return true;
    };
    if (!verifyTrees(result.first) || !verifyTrees(second))
    { second.payload_prepared = false; return fail("joint_selected_directory_tree_mismatch"); }
    result.evidence = ObserveJointPayloadReferences(schema, candidate, result.first, second, version);
    if (!result.evidence.error.empty() || !result.evidence.declaredCrossTaskReferencesObserved)
    { second.payload_prepared = false; return fail("joint_cross_reference_unimplemented:" + result.evidence.error); }
    result.sharedSubsetPrepared = true;
    return result;
    }
    catch (const std::exception&)
    {
        return fail("joint_fixture_exception_unimplemented");
    }
    catch (...)
    {
        return fail("joint_fixture_unknown_exception_unimplemented");
    }
}
// V3 joint input subset: B may select A objects or create B-only objects under
// the same isolated root. It never performs a drop. B's member dependencies
// can point forward; missing/cyclic topology remains unimplemented.
inline JointPayloadFixtureResult PrepareV3JointSharedPayloadSubset(
    const V3FamilyValidatedContext& context, const std::filesystem::path& temporaryParent)
{
    JointPayloadFixtureResult result;
    const auto fail = [&](std::string reason) {
        result.first.payload_prepared = false;
        result.first.candidate_constructed = false;
        result.second.payload_prepared = false;
        result.second.candidate_constructed = false;
        result.sharedSubsetPrepared = false;
        result.error = std::move(reason);
        return std::move(result);
    };
    try
    {
        // Reserve before first owns an isolated child, so failure reporting is
        // less likely to allocate after that ownership has been acquired.
        result.error.reserve(128);
        if (context.Status() != V3CoordinateStatus::coordinate_ready || !context.TaskBActive())
            return fail("v3_joint_authoritative_two_task_coordinate_unavailable");
        for (const auto audit : {context.AuditSelectionA(), context.AuditSelectionB()})
            if (audit.state != V3SelectionAuditState::unresolved ||
                audit.reason != "v3_selection_remaining_structure_and_execution_unresolved")
                return fail("v3_joint_selection_unimplemented:" + std::string(audit.reason));
        const auto transfer = context.AuditTransfer();
        if (!transfer.invalid.empty() || !transfer.localExclusion.empty())
            return fail("v3_joint_transfer_structure_unimplemented");
        const auto& axes = context.Axes();
        const auto& candidate = context.Coordinate();
        const auto a = [&](std::string_view id) { return Value(axes, candidate, id); };
        const auto b = [&](std::string_view id) { return Value(axes, candidate, "task_b." + std::string(id)); };
        const auto decodeCount = [](std::string_view text, std::size_t& count) {
            if (text == "zero") { count = 0; return true; }
            if (text == "one") { count = 1; return true; }
            if (text == "two") { count = 2; return true; }
            return text.starts_with("count_") && V3SourceCountForMaterialization(text.substr(6), count) && count >= 3;
        };
        std::size_t aCount = 0, bCount = 0;
        if (!decodeCount(a("P02.source_actual_count"), aCount) ||
            !decodeCount(b("P02.source_actual_count"), bCount))
            return fail("v3_joint_source_count_unavailable");
        if (!aCount || !bCount) return fail("v3_joint_empty_source_unimplemented");
        const auto parse = [](std::string_view ref, std::string_view prefix, std::size_t limit, std::size_t& index) {
            if (!ref.starts_with(prefix)) return false;
            const auto digits = ref.substr(prefix.size());
            if (digits.empty() || (digits.size() > 1 && digits.front() == '0')) return false;
            if (!std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) return false;
            return V3SourceCountForMaterialization(digits, index) && index < limit;
        };
        const auto kind = [&](bool second, std::size_t index) {
            const auto id = index == 0 ? std::string("P01.primary") : index == 1 ? std::string("P01.second") :
                "P01.member" + std::to_string(index) + "_kind";
            return second ? b(id) : a(id);
        };
        std::vector<std::size_t> selected;
        selected.reserve(bCount);
        const auto uniqueB = (std::numeric_limits<std::size_t>::max)();
        for (std::size_t i = 0; i < bCount; ++i)
        {
            std::size_t fromA = 0;
            const auto cross = b("P03.member" + std::to_string(i) + "_cross_task_identity");
            if (kind(true, i) != "file" && kind(true, i) != "directory")
                return fail("v3_joint_shared_member_unimplemented:" + std::to_string(i));
            if (cross == "unique") selected.push_back(uniqueB);
            else if (parse(cross, "task_a_member_", aCount, fromA) && kind(true, i) == kind(false, fromA))
                selected.push_back(fromA);
            else return fail("v3_joint_shared_member_unimplemented:" + std::to_string(i));
        }
        for (std::size_t i = 0; i < aCount; ++i)
            if (a("P03.member" + std::to_string(i) + "_cross_task_identity") != "na")
                return fail("v3_joint_a_cross_identity_unimplemented");
        if (b("R02.target_directory_identity") != "task_a_target_directory")
            return fail("v3_joint_target_identity_unimplemented");

        result.first = PreparePayloadFixture(axes, candidate, temporaryParent, false,
            SelectionSchemaVersion::source_members_v2, &context, true);
        if (!result.first.payload_prepared) return fail("v3_joint_first_payload_unavailable:" + result.first.error);
        auto& first = result.first;
        auto& second = result.second;
        if (first.members.size() != aCount) return fail("v3_joint_first_count_mismatch");
        second.directory = first.directory;
        second.targetDirectory = first.targetDirectory;
        second.axes = first.axes;
        std::size_t verifiedCrossDirectoryRefs = 0;
        bool sharedTarget = false;
        if (!ObserveJointSameObject(first.targetDirectory, second.targetDirectory, sharedTarget) || !sharedTarget)
            return fail("v3_joint_target_identity_unavailable");
        ++verifiedCrossDirectoryRefs;
        const auto aNode = [&](std::string_view ref, std::filesystem::path& path) {
            if (ref == "task_a_isolated_root") path = first.directory;
            else if (ref == "task_a_isolated_root_parent") path = first.directory.parent_path();
            else if (ref == "task_a_target_directory") path = first.targetDirectory;
            else if (ref == "task_a_desktop_directory") path = first.desktopDirectory;
            else if (ref == "task_a_source_mapping_directory") path = first.sourceMappingDirectory;
            else if (ref == "task_a_target_mapping_directory") path = first.targetMappingDirectory;
            else
            {
                std::size_t i = 0;
                if (parse(ref, "task_a_member_object_", first.members.size(), i))
                { if (first.members[i].kind != "directory") return false; path = first.members[i].path; }
                else if (parse(ref, "task_a_member_", first.members.size(), i)) path = first.members[i].path.parent_path();
                else return false;
            }
            return !path.empty();
        };
        const std::array<std::pair<std::string_view, std::filesystem::path*>, 3> endpoints{{
            {"desktop_directory", &second.desktopDirectory}, {"source_mapping_directory", &second.sourceMappingDirectory},
            {"target_mapping_directory", &second.targetMappingDirectory}}};
        for (const auto& [name, path] : endpoints)
        {
            const auto ref = b("R02." + std::string(name) + "_identity");
            if (ref == "na")
            { if (b("R02." + std::string(name) + "_parent_ref") != "na") return fail("v3_joint_absent_endpoint_unimplemented"); }
            else
            {
                std::filesystem::path expected;
                bool same = false;
                if (!aNode(ref, expected)) return fail("v3_joint_endpoint_identity_unimplemented");
                *path = expected;
                if (!ObserveJointSameObject(*path, expected, same) || !same)
                    return fail("v3_joint_endpoint_identity_unavailable");
                ++verifiedCrossDirectoryRefs;
            }
        }
        const auto before = CapturePayloadInputSnapshot(first);
        if (!before.captured) return fail("v3_joint_initial_snapshot_unavailable");
        std::vector<PayloadFixtureMember> slots(bCount), added;
        std::vector<bool> ready(bCount, false), createdDirectory(bCount, false);
        const auto availableParent = [&](const std::filesystem::path& path) {
            const auto relative = path.lexically_relative(first.directory);
            if (relative.empty() || (relative.begin() != relative.end() && *relative.begin() == std::filesystem::path(".."))) return false;
            const auto snapshot = CapturePayloadInputSnapshot(first);
            if (!snapshot.captured || std::none_of(snapshot.entries.begin(), snapshot.entries.end(), [&](const auto& item) {
                return item.relative == relative && item.kind == "directory";
            })) return false;
            std::error_code error;
            const auto root = std::filesystem::weakly_canonical(first.directory, error);
            if (error) return false;
            const auto actual = std::filesystem::weakly_canonical(path, error);
            const auto inside = actual.lexically_relative(root);
            return !error && !inside.empty() &&
                (inside.begin() == inside.end() || *inside.begin() != std::filesystem::path(".."));
        };
        std::size_t remaining = bCount;
        while (remaining)
        {
            bool progressed = false;
            for (std::size_t i = 0; i < bCount; ++i)
            {
                if (ready[i]) continue;
                PayloadFixtureMember member;
                if (selected[i] != uniqueB) member = first.members[selected[i]];
                else
                {
                    const auto ref = i == 0 ? std::string_view("unique") :
                        b("P03.member" + std::to_string(i) + "_identity_ref");
                    if (ref != "unique")
                    {
                        std::size_t prior = 0;
                        if (!parse(ref, "member_", i, prior) || selected[prior] != uniqueB ||
                            kind(true, i) != kind(true, prior))
                            return fail("v3_joint_unique_alias_unimplemented:" + std::to_string(i));
                        if (!ready[prior]) continue;
                        member = slots[prior];
                    }
                    else
                    {
                        const auto refParent = b("R02.member" + std::to_string(i) + "_directory_ref");
                        std::filesystem::path parent;
                        if (refParent == "isolated_root") parent = first.directory;
                        else if (refParent == "target_directory") parent = second.targetDirectory;
                        else if (refParent.starts_with("task_a_")) (void)aNode(refParent, parent);
                        else
                        {
                            for (const auto& [name, path] : endpoints) if (refParent == name) parent = *path;
                            std::size_t prior = 0;
                            const bool object = parse(refParent, "member_object_", bCount, prior);
                            const bool memberRef = !object && parse(refParent, "member_", bCount, prior);
                            if (object || memberRef)
                            {
                                if (!ready[prior]) continue;
                                if (object && slots[prior].kind != "directory")
                                    return fail("v3_joint_unique_parent_type_unimplemented");
                                parent = object ? slots[prior].path : slots[prior].path.parent_path();
                            }
                        }
                        if (!availableParent(parent)) return fail("v3_joint_unique_parent_unimplemented:" + std::to_string(i));
                        const bool directory = kind(true, i) == "directory";
                        if (directory && b("R02.member" + std::to_string(i) + "_directory_object_ref") != "unique")
                            return fail("v3_joint_unique_directory_identity_unimplemented");
                        const auto path = parent / ("joint_v3_b_member_" + std::to_string(i) + (directory ? "" : ".txt"));
                        const auto bytes = "v3-joint-b-member-" + std::to_string(i) + "\n";
#if !defined(_WIN32)
                        return fail("v3_joint_unique_creation_unimplemented");
#else
                        if (directory)
                        {
                            if (!CreateDirectoryW(path.c_str(), nullptr)) return fail("v3_joint_unique_directory_creation_unavailable");
                            member = {path, "directory", {}, i, {}};
                            createdDirectory[i] = true;
                        }
                        else
                        {
                            const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
                            if (file == INVALID_HANDLE_VALUE) return fail("v3_joint_unique_file_creation_unavailable");
                            DWORD written = 0;
                            const auto expectedBytes = static_cast<DWORD>(bytes.size());
                            const bool wrote = WriteFile(file, bytes.data(), expectedBytes, &written, nullptr) &&
                                written == expectedBytes;
                            const bool closed = CloseHandle(file) != FALSE;
                            if (!wrote || !closed) return fail("v3_joint_unique_file_write_unavailable");
                            member = {path, "file", bytes, i, {}};
                        }
                        added.push_back(member);
#endif
                    }
                }
                slots[i] = std::move(member); ready[i] = true; --remaining; progressed = true;
            }
            if (!progressed) return fail("v3_joint_member_dependency_cycle_or_unknown_unimplemented");
        }
        for (std::size_t i = 0; i < bCount; ++i)
        {
            auto member = std::move(slots[i]);
            if (selected[i] != uniqueB)
            {
                bool same = false;
                if (!ObserveJointSameObject(member.path, first.members[selected[i]].path, same) || !same)
                    return fail("v3_joint_shared_member_identity_unavailable");
            }
            member.identity = i;
            for (std::size_t prior = 0; prior < second.members.size(); ++prior)
            {
                bool same = false;
                if (!ObserveJointSameObject(member.path, second.members[prior].path, same))
                    return fail("v3_joint_member_identity_unavailable");
                if (same) { member.identity = second.members[prior].identity; break; }
            }
            if (selected[i] == uniqueB)
                for (const auto& aMember : first.members)
                {
                    bool same = false;
                    if (!ObserveJointSameObject(member.path, aMember.path, same) || same)
                        return fail("v3_joint_unique_cross_identity_unavailable");
                }
            if (i)
            {
                const auto ref = b("P03.member" + std::to_string(i) + "_identity_ref");
                if (ref == "unique")
                { if (member.identity != i) return fail("v3_joint_member_identity_mismatch"); }
                else
                {
                    std::size_t prior = 0;
                    if (!parse(ref, "member_", i, prior) || member.identity != second.members[prior].identity)
                        return fail("v3_joint_member_identity_unimplemented");
                }
            }
            second.members.push_back(std::move(member));
        }
        // Add only the independently planned new objects to any selected
        // directory tree, then compare the complete physical snapshot below.
        for (auto* fixture : {&first, &second})
            for (auto& member : fixture->members)
            {
                if (member.kind != "directory") continue;
                for (const auto& created : added)
                {
                    const auto relative = created.path.lexically_relative(member.path);
                    if (relative.empty() || relative == std::filesystem::path(".") ||
                        (relative.begin() != relative.end() && *relative.begin() == std::filesystem::path(".."))) continue;
                    member.tree.push_back({relative, created.kind, created.bytes});
                }
                std::sort(member.tree.begin(), member.tree.end(), [](const auto& left, const auto& right) {
                    return left.relative.generic_wstring() < right.relative.generic_wstring();
                });
            }
        const auto node = [&](std::string_view ref, std::filesystem::path& path) {
            if (ref == "isolated_root") path = second.directory;
            else if (ref == "isolated_root_parent") path = second.directory.parent_path();
            else if (ref == "target_directory") path = second.targetDirectory;
            else if (ref.starts_with("task_a_")) return aNode(ref, path);
            else
            {
                for (const auto& [name, endpoint] : endpoints)
                    if (ref == name) { path = *endpoint; return !path.empty(); }
                std::size_t i = 0;
                if (parse(ref, "member_object_", second.members.size(), i))
                { if (second.members[i].kind != "directory") return false; path = second.members[i].path; }
                else if (parse(ref, "member_", second.members.size(), i)) path = second.members[i].path.parent_path();
                else return false;
            }
            return !path.empty();
        };
        const auto matches = [&](std::string_view ref, const std::filesystem::path& actual) {
            std::filesystem::path expected; bool same = false;
            if (!node(ref, expected) || !ObserveJointSameObject(expected, actual, same) || !same) return false;
            if (ref.starts_with("task_a_")) ++verifiedCrossDirectoryRefs;
            return true;
        };
        if (!matches(b("R02.target_directory_parent_ref"), second.targetDirectory.parent_path()))
            return fail("v3_joint_target_parent_unimplemented");
        for (const auto& [name, path] : endpoints)
            if (!path->empty() && !matches(b("R02." + std::string(name) + "_parent_ref"), path->parent_path()))
                return fail("v3_joint_endpoint_parent_unimplemented");
        for (std::size_t i = 0; i < second.members.size(); ++i)
        {
            const auto stem = "R02.member" + std::to_string(i);
            const auto& member = second.members[i];
            if (!matches(b(stem + "_directory_ref"), member.path.parent_path()) ||
                !matches(b(stem + "_directory_parent_ref"), member.path.parent_path().parent_path()) ||
                (member.kind == "directory" ? (b(stem + "_directory_object_ref") == "unique" ? !createdDirectory[i] :
                    !matches(b(stem + "_directory_object_ref"), member.path)) :
                    b(stem + "_directory_object_ref") != "na"))
                return fail("v3_joint_member_topology_unimplemented:" + std::to_string(i));
            // Unverified relative-layout assertions cannot be advertised as constructed.
            for (const auto suffix : {"_directory_to_target", "_target_layout"})
            {
                const auto relation = b(stem + suffix);
                if (relation != "na" && relation != "unknown" && relation != "unresolved")
                    return fail("v3_joint_relative_layout_unimplemented:" + std::to_string(i));
            }
        }
        second.unresolved.emplace_back("v3_joint_remaining_attributes_volume_host_and_execution_unimplemented");
        second.payload_prepared = true;
        const auto firstSnapshot = CapturePayloadInputSnapshot(first);
        const auto secondSnapshot = CapturePayloadInputSnapshot(second);
        if (!firstSnapshot.captured || !secondSnapshot.captured || secondSnapshot.memberEntryIndices.size() != bCount)
            return fail("v3_joint_snapshot_unavailable");
        if (firstSnapshot.root != secondSnapshot.root || firstSnapshot.entries.size() != secondSnapshot.entries.size())
            return fail("v3_joint_shared_input_changed_between_snapshots");
        for (std::size_t i = 0; i < firstSnapshot.entries.size(); ++i)
        {
            const auto& left = firstSnapshot.entries[i];
            const auto& right = secondSnapshot.entries[i];
            if (left.relative != right.relative || left.kind != right.kind || left.bytes != right.bytes ||
                left.volumeSerial != right.volumeSerial || left.fileIndexHigh != right.fileIndexHigh ||
                left.fileIndexLow != right.fileIndexLow)
                return fail("v3_joint_shared_input_changed_between_snapshots");
        }
        if (firstSnapshot.entries.size() != before.entries.size() + added.size())
            return fail("v3_joint_input_tree_entry_count_mismatch");
        for (const auto& original : before.entries)
        {
            const auto found = std::find_if(firstSnapshot.entries.begin(), firstSnapshot.entries.end(),
                [&](const auto& entry) { return entry.relative == original.relative; });
            if (found == firstSnapshot.entries.end() || found->kind != original.kind || found->bytes != original.bytes ||
                found->volumeSerial != original.volumeSerial || found->fileIndexHigh != original.fileIndexHigh ||
                found->fileIndexLow != original.fileIndexLow)
                return fail("v3_joint_initial_input_changed");
        }
        for (const auto& created : added)
        {
            const auto relative = created.path.lexically_relative(first.directory);
            const auto found = std::find_if(firstSnapshot.entries.begin(), firstSnapshot.entries.end(),
                [&](const auto& entry) { return entry.relative == relative; });
            if (found == firstSnapshot.entries.end() || found->kind != created.kind || found->bytes != created.bytes)
                return fail("v3_joint_created_input_mismatch");
        }
        for (std::size_t i = 0; i < second.members.size(); ++i)
        {
            const auto at = secondSnapshot.memberEntryIndices[i];
            if (at >= secondSnapshot.entries.size() ||
                secondSnapshot.entries[at].kind != second.members[i].kind ||
                (second.members[i].kind == "file" && secondSnapshot.entries[at].bytes != second.members[i].bytes))
                return fail("v3_joint_member_snapshot_unavailable");
        }
        const auto treesMatch = [&](const PayloadFixtureResult& fixture, const PayloadInputSnapshot& snapshot) {
            for (const auto& member : fixture.members)
            {
                if (member.kind != "directory") continue;
                std::vector<PayloadFixtureTreeEntry> observed;
                for (const auto& entry : snapshot.entries)
                {
                    const auto full = (snapshot.root / entry.relative).lexically_normal();
                    const auto relative = full.lexically_relative(member.path);
                    if (relative.empty() || relative == std::filesystem::path(".") ||
                        (relative.begin() != relative.end() && *relative.begin() == std::filesystem::path(".."))) continue;
                    observed.push_back({relative, entry.kind, entry.bytes});
                }
                std::sort(observed.begin(), observed.end(), [](const auto& left, const auto& right) {
                    return left.relative.generic_wstring() < right.relative.generic_wstring();
                });
                if (observed.size() != member.tree.size()) return false;
                for (std::size_t i = 0; i < observed.size(); ++i)
                    if (observed[i].relative != member.tree[i].relative || observed[i].kind != member.tree[i].kind ||
                        observed[i].bytes != member.tree[i].bytes) return false;
            }
            return true;
        };
        if (!treesMatch(first, firstSnapshot) || !treesMatch(second, secondSnapshot))
            return fail("v3_joint_selected_directory_tree_mismatch");
        result.evidence.sharedRootObserved = true;
        result.evidence.declaredCrossTaskReferencesObserved = true;
        result.evidence.directoryReferencesObserved = verifiedCrossDirectoryRefs;
        result.evidence.memberIdentitiesObserved = bCount;
        result.unresolved.emplace_back("v3_joint_path_checks_not_atomic_against_concurrent_reparse_or_replacement");
        result.sharedSubsetPrepared = true;
        return result;
    }
    catch (const std::bad_alloc&)
    {
        result.first.payload_prepared = result.second.payload_prepared = result.sharedSubsetPrepared = false;
        result.first.candidate_constructed = result.second.candidate_constructed = false;
        result.error = "v3_joint_resource_unavailable";
        return result;
    }
    catch (const std::length_error&)
    {
        result.first.payload_prepared = result.second.payload_prepared = result.sharedSubsetPrepared = false;
        result.first.candidate_constructed = result.second.candidate_constructed = false;
        result.error = "v3_joint_resource_unavailable";
        return result;
    }
    catch (...)
    {
        result.first.payload_prepared = result.second.payload_prepared = result.sharedSubsetPrepared = false;
        result.first.candidate_constructed = result.second.candidate_constructed = false;
        result.error = "v3_joint_fixture_exception_unimplemented";
        return result;
    }
}
} // namespace drag_global
