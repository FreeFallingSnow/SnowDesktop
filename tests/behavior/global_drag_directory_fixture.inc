#pragma once

// Input-only fixture for a restricted V4 physical directory tree. Include
// after the authoritative model, V4 event-graph identity and payload helpers.
// Creating this fixture never performs a drag, transfer, membership change or
// layout commit. Unsupported nodes remain unimplemented rather than excluded.
#include <functional>
#include <set>
namespace drag_global::directory_fixture
{
struct NodeObservation
{
    std::string slot;
    std::filesystem::path relative;
    bool directory = false, createAttempted = false, created = false;
    bool identityObserved = false, bytesObserved = false;
    FILE_ID_INFO identity{};
    std::vector<std::byte> actualBytes;
};
struct FixtureObservation
{
    std::string candidateId;
    std::filesystem::path source;
    bool rootCreateAttempted = false, rootCreated = false, rootIdentityObserved = false;
    FILE_ID_INFO rootIdentity{};
    std::vector<NodeObservation> nodes;
    std::vector<std::string> unresolved;
    bool inputsObserved = false; // local input only; never production coverage
    bool handoffAttempted = false, handoffReturned = false;
};

inline bool SameId(const FILE_ID_INFO& a, const FILE_ID_INFO& b) noexcept
{
    return a.VolumeSerialNumber == b.VolumeSerialNumber &&
        std::memcmp(a.FileId.Identifier, b.FileId.Identifier, sizeof(a.FileId.Identifier)) == 0 &&
        std::any_of(std::begin(a.FileId.Identifier), std::end(a.FileId.Identifier), [](BYTE value) { return value != 0; });
}
inline bool DecodeHex(std::string_view hex, std::vector<std::byte>& out)
{
    if (hex.size() % 2) return false;
    const auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    out.clear(); out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2)
    {
        const int high = digit(hex[i]), low = digit(hex[i + 1]);
        if (high < 0 || low < 0) return false;
        out.push_back(static_cast<std::byte>((high << 4) | low));
    }
    return true;
}
inline bool DecodePath(std::string_view hex, std::filesystem::path& out)
{
    static_assert(sizeof(wchar_t) == 2, "Windows UTF-16 code units required");
    std::vector<std::byte> bytes;
    if (!DecodeHex(hex, bytes) || bytes.empty() || bytes.size() % 2) return false;
    std::wstring value; value.reserve(bytes.size() / 2);
    for (std::size_t i = 0; i < bytes.size(); i += 2)
    {
        const auto unit = std::to_integer<unsigned>(bytes[i]) | (std::to_integer<unsigned>(bytes[i + 1]) << 8);
        if (!unit) return false;
        value.push_back(static_cast<wchar_t>(unit));
    }
    out = std::filesystem::path(value);
    return true;
}
inline bool SafeRelative(const std::filesystem::path& path)
{
    if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory()) return false;
    for (const auto& part : path)
    {
        const auto name = part.native();
        if (name.empty() || name == L"." || name == L".." || name.back() == L'.' || name.back() == L' ' ||
            name.find_first_of(L"\\/:*?<>|\"") != std::wstring::npos || name.find(L'\0') != std::wstring::npos)
            return false;
    }
    return true;
}
inline bool SamePathInsensitive(const std::filesystem::path& a, const std::filesystem::path& b)
{
    const auto x = a.generic_wstring(), y = b.generic_wstring();
    if (x.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()) ||
        y.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) return false;
    return CompareStringOrdinal(x.data(), static_cast<int>(x.size()), y.data(), static_cast<int>(y.size()), TRUE) == CSTR_EQUAL;
}

// The parent is supplied by the caller's exclusive, newly created profile or
// task fixture and must remain held through this function. This checks the
// actual directory identity before any child creation; it does not claim a
// production host binding or durable ownership across later execution.
inline FixtureObservation Prepare(const V4FamilyDescriptor& family, const V4FamilyValidatedContext& verified,
    const event_graph::V4FamilyIdentityContext& identity, const std::filesystem::path& parent,
    const V4OwnedChildDirectoryAttempt& ownedParent, std::size_t maxNodes, std::size_t maxBytes,
    const std::function<void(const FixtureObservation&, HANDLE, const std::vector<HANDLE>&)>& beforeHandlesRelease = {})
{
    FixtureObservation out;
    try
    {
        if (verified.Status() != V4CoordinateStatus::coordinate_ready || identity.Status() != V4FamilyStatus::schema_only ||
            verified.TaskBActive() || family.b || V4FamilyBinding(family) != identity.FamilyBinding() ||
            verified.FamilyBinding() != identity.FamilyBinding() || verified.Axes().size() != identity.Axes().size())
        { out.unresolved.emplace_back("directory_candidate_authority_unavailable"); return out; }
        for (std::size_t i = 0; i < identity.Axes().size(); ++i)
        {
            const auto& a = identity.Axes()[i]; const auto& b = verified.Axes()[i];
            if (a.id != b.id || a.role != b.role || a.values != b.values || a.label != b.label)
            { out.unresolved.emplace_back("directory_candidate_schema_mismatch"); return out; }
        }
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty())
        { out.unresolved.emplace_back("directory_candidate_identity_unavailable"); return out; }
        out.candidateId = raw.id;
        const auto value = [&](std::string_view id) { return Value(verified.Axes(), verified.Coordinate(), id); };
        if (family.a.kind != V4SourceKind::file_only || family.a.fileCount.kind != V3CountKind::exact ||
            family.a.fileCount.decimal != "1" || family.a.filePaths.size() != 1 ||
            family.a.filePaths.front().index != "0" || family.a.filePaths.front().kind != V4PayloadKind::exact ||
            family.a.filePaths.front().base != V4PathBase::raw ||
            family.a.directoryRoots.size() != 1 || family.a.directoryRoots.front().rootIndex != "0" ||
            family.a.directoryRoots.front().completeness != V4DirectoryCompleteness::complete ||
            value("V4.file.member0_kind") != "directory" || value("V4.file.member0_identity") != "unique")
        { out.unresolved.emplace_back("directory_single_source_shape_unimplemented"); return out; }
        if (family.a.directoryNodes.size() > maxNodes)
        { out.unresolved.emplace_back("directory_node_budget_unavailable"); return out; }
        std::filesystem::path source;
        const auto parentHandle = ownedParent.handle;
        const auto& expectedParent = ownedParent.identity;
        if (!ownedParent.available || !ownedParent.created || !ownedParent.identityObserved ||
            !DecodePath(family.a.filePaths.front().utf16leHex, source) || !source.is_absolute() ||
            source.parent_path() != parent || !SafeRelative(source.filename()) || !parentHandle || parentHandle == INVALID_HANDLE_VALUE)
        { out.unresolved.emplace_back("directory_source_path_unavailable"); return out; }
        std::error_code sameParentError;
        if (!std::filesystem::equivalent(parent, ownedParent.physicalPath, sameParentError) || sameParentError)
        { out.unresolved.emplace_back("directory_parent_path_identity_unavailable"); return out; }
        struct CloseHandleOnExit { HANDLE h; ~CloseHandleOnExit() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); } };
        CloseHandleOnExit namedParent{CreateFileW(parent.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        FILE_ID_INFO parentId{}; FILE_ATTRIBUTE_TAG_INFO parentTag{};
        if (!GetFileInformationByHandleEx(parentHandle, FileIdInfo, &parentId, sizeof(parentId)) || !SameId(parentId, expectedParent) ||
            !GetFileInformationByHandleEx(parentHandle, FileAttributeTagInfo, &parentTag, sizeof(parentTag)) ||
            !(parentTag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (parentTag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        { out.unresolved.emplace_back("directory_parent_identity_unavailable"); return out; }
        FILE_ID_INFO namedParentId{}; FILE_ATTRIBUTE_TAG_INFO namedParentTag{};
        if (namedParent.h == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandleEx(namedParent.h, FileIdInfo, &namedParentId, sizeof(namedParentId)) ||
            !SameId(namedParentId, expectedParent) ||
            !GetFileInformationByHandleEx(namedParent.h, FileAttributeTagInfo, &namedParentTag, sizeof(namedParentTag)) ||
            !(namedParentTag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (namedParentTag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        { out.unresolved.emplace_back("directory_named_parent_identity_unavailable"); return out; }
        struct InputNode { std::string slot, object; std::filesystem::path relative; bool directory; std::vector<std::byte> bytes; };
        std::vector<InputNode> inputs;
        std::size_t total = 0;
        for (const auto& node : family.a.directoryNodes)
        {
            InputNode input; input.slot = node.slotIndex;
            if (node.rootIndex != "0" || node.pathKind != V4PayloadKind::exact ||
                !DecodePath(node.relativePathUtf16Hex, input.relative) || !SafeRelative(input.relative) ||
                node.objectIdentity.kind != V3CountKind::exact ||
                (node.nodeKind != V4DirectoryNodeKind::directory && node.nodeKind != V4DirectoryNodeKind::file))
            { out.unresolved.emplace_back("directory_node_path_kind_or_identity_unimplemented:" + node.slotIndex); return out; }
            input.object = node.objectIdentity.decimal;
            input.directory = node.nodeKind == V4DirectoryNodeKind::directory;
            if ((input.directory && node.contentKind != V4PayloadKind::not_applicable) ||
                (!input.directory && (node.contentKind != V4PayloadKind::exact || !DecodeHex(node.contentHex, input.bytes))))
            { out.unresolved.emplace_back("directory_node_content_unimplemented:" + node.slotIndex); return out; }
            if (input.bytes.size() > maxBytes - total)
            { out.unresolved.emplace_back("directory_byte_budget_unavailable"); return out; }
            total += input.bytes.size();
            for (const auto& prior : inputs)
                if (prior.object == input.object || SamePathInsensitive(prior.relative, input.relative))
                { out.unresolved.emplace_back("directory_alias_or_path_conflict_unimplemented:" + node.slotIndex); return out; }
            inputs.push_back(std::move(input));
        }
        // Nested paths need handle-relative traversal with each ancestor held.
        // This fixture only constructs direct children until that exists.
        for (const auto& input : inputs)
            if (input.relative.has_parent_path())
            { out.unresolved.emplace_back("directory_nested_nodes_unimplemented:" + input.slot); return out; }
        std::sort(inputs.begin(), inputs.end(), [](const auto& a, const auto& b) {
            return std::distance(a.relative.begin(), a.relative.end()) < std::distance(b.relative.begin(), b.relative.end()); });
        out.nodes.reserve(inputs.size());
        struct HandleSet
        {
            std::vector<HANDLE> values;
            ~HandleSet() { for (const auto handle : values) if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
        } handles;
        handles.values.reserve(inputs.size());
        out.source = source; out.rootCreateAttempted = true;
        auto root = CreateV4OwnedChildDirectory(parentHandle, expectedParent, source.filename().native());
        out.rootCreated = root.created; out.rootIdentityObserved = root.identityObserved; out.rootIdentity = root.identity;
        if (!root.available) { out.unresolved.emplace_back("directory_source_create_unavailable"); return out; }
        CloseHandleOnExit namedSource{CreateFileW(source.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        FILE_ID_INFO namedSourceId{}; FILE_ATTRIBUTE_TAG_INFO namedSourceTag{};
        if (namedSource.h == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandleEx(namedSource.h, FileIdInfo, &namedSourceId, sizeof(namedSourceId)) ||
            !SameId(namedSourceId, root.identity) ||
            !GetFileInformationByHandleEx(namedSource.h, FileAttributeTagInfo, &namedSourceTag, sizeof(namedSourceTag)) ||
            !(namedSourceTag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (namedSourceTag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        { out.unresolved.emplace_back("directory_named_source_identity_unavailable"); return out; }
        for (const auto& input : inputs)
        {
            out.nodes.push_back({input.slot, input.relative, input.directory});
            auto& observed = out.nodes.back(); observed.createAttempted = true;
            const auto path = source / input.relative;
            HANDLE handle = INVALID_HANDLE_VALUE;
            if (input.directory)
            {
                auto child = CreateV4OwnedChildDirectory(root.handle, root.identity, input.relative.native());
                observed.created = child.created;
                if (!child.available) { out.unresolved.push_back("directory_create_unavailable:" + input.slot); return out; }
                handle = std::exchange(child.handle, INVALID_HANDLE_VALUE);
            }
            else handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                CREATE_NEW, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            struct Close { HANDLE h; ~Close() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); } } close{handle};
            if (handle == INVALID_HANDLE_VALUE) { out.unresolved.push_back("directory_node_open_unavailable:" + input.slot); return out; }
            handles.values.push_back(handle); close.h = INVALID_HANDLE_VALUE;
            observed.created = true;
            FILE_ATTRIBUTE_TAG_INFO tag{};
            if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag)) ||
                (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                static_cast<bool>(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != input.directory ||
                !GetFileInformationByHandleEx(handle, FileIdInfo, &observed.identity, sizeof(observed.identity)) ||
                !SameId(observed.identity, observed.identity))
            { out.unresolved.push_back("directory_node_identity_unavailable:" + input.slot); return out; }
            observed.identityObserved = true;
            for (const auto& prior : out.nodes)
                if (&prior != &observed && prior.identityObserved && SameId(prior.identity, observed.identity))
                { out.unresolved.push_back("directory_distinct_object_identity_unavailable:" + input.slot); return out; }
            if (!input.directory)
            {
                std::size_t written = 0;
                while (written < input.bytes.size())
                {
                    const DWORD amount = static_cast<DWORD>((std::min)(input.bytes.size() - written, std::size_t{4096}));
                    DWORD count = 0;
                    if (!WriteFile(handle, input.bytes.data() + written, amount, &count, nullptr) || count != amount)
                    { out.unresolved.push_back("directory_file_write_unavailable:" + input.slot); return out; }
                    written += count;
                }
                LARGE_INTEGER zero{};
                if (!FlushFileBuffers(handle) || !SetFilePointerEx(handle, zero, nullptr, FILE_BEGIN))
                { out.unresolved.push_back("directory_file_readback_unavailable:" + input.slot); return out; }
                observed.actualBytes.resize(input.bytes.size());
                std::size_t read = 0;
                while (read < observed.actualBytes.size())
                {
                    const DWORD amount = static_cast<DWORD>((std::min)(observed.actualBytes.size() - read, std::size_t{4096}));
                    DWORD count = 0;
                    if (!ReadFile(handle, observed.actualBytes.data() + read, amount, &count, nullptr) || count != amount)
                    { out.unresolved.push_back("directory_file_readback_unavailable:" + input.slot); return out; }
                    read += count;
                }
                observed.bytesObserved = observed.actualBytes == input.bytes;
                if (!observed.bytesObserved) { out.unresolved.push_back("directory_file_bytes_mismatch:" + input.slot); return out; }
            }
        }
        // Complete means all declared nodes, including an empty tree, match
        // a real enumeration of this freshly created source directory. Do not
        // infer completeness merely from successfully creating listed nodes.
        std::error_code enumerationError;
        std::filesystem::recursive_directory_iterator it(source, enumerationError), end;
        if (enumerationError) { out.unresolved.emplace_back("directory_enumeration_unavailable"); return out; }
        std::vector<bool> seen(inputs.size(), false);
        for (; it != end; it.increment(enumerationError))
        {
            if (enumerationError) { out.unresolved.emplace_back("directory_enumeration_unavailable"); return out; }
            const auto relative = it->path().lexically_relative(source);
            const auto attrs = GetFileAttributesW(it->path().c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT))
            { out.unresolved.emplace_back("directory_enumerated_node_unavailable"); return out; }
            std::size_t match = inputs.size();
            for (std::size_t i = 0; i < inputs.size(); ++i)
                if (SamePathInsensitive(inputs[i].relative, relative))
                { if (match != inputs.size()) { out.unresolved.emplace_back("directory_enumerated_node_ambiguous"); return out; } match = i; }
            if (match == inputs.size() || seen[match] ||
                static_cast<bool>(attrs & FILE_ATTRIBUTE_DIRECTORY) != inputs[match].directory)
            { out.unresolved.emplace_back("directory_unexpected_enumerated_node"); return out; }
            CloseHandleOnExit named{CreateFileW(it->path().c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                (inputs[match].directory ? FILE_FLAG_BACKUP_SEMANTICS : 0) | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
            FILE_ID_INFO actualId{};
            if (named.h == INVALID_HANDLE_VALUE || !GetFileInformationByHandleEx(named.h, FileIdInfo, &actualId, sizeof(actualId)) ||
                !SameId(actualId, out.nodes[match].identity))
            { out.unresolved.emplace_back("directory_enumerated_identity_unavailable"); return out; }
            seen[match] = true;
        }
        if (enumerationError || std::find(seen.begin(), seen.end(), false) != seen.end())
        { out.unresolved.emplace_back("directory_complete_enumeration_unavailable"); return out; }
        out.inputsObserved = true;
        out.unresolved.emplace_back("directory_other_source_slots_unimplemented");
        out.unresolved.emplace_back("directory_fixture_lease_and_host_binding_unimplemented");
        out.unresolved.emplace_back("directory_production_drag_completion_and_business_unimplemented");
        // This callback only prepares a separate observation lease. Root and
        // every direct node are still open here, in exactly input.nodes order.
        // A normal return does not itself prove overlap or authorize a drag.
        if (beforeHandlesRelease)
        {
            out.handoffAttempted = true;
            try { beforeHandlesRelease(out, root.handle, handles.values); out.handoffReturned = true; }
            catch (...) { out.unresolved.emplace_back("directory_fixture_lease_handoff_unavailable"); }
        }
    }
    catch (...) { out.inputsObserved = false; out.unresolved.emplace_back("directory_fixture_resource_or_observation_unavailable"); }
    return out;
}
} // namespace drag_global::directory_fixture
