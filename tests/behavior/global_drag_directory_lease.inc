#pragma once

// Include after global_drag_directory_fixture.inc. File handles permit read,
// write and deletion sharing. Directory handles deny deletion to fence the
// direct-child path while allowing Shell copy; move/delete is unsupported.
// They are neither an execution permit nor a product-result assertion.
namespace drag_global::ole_driver { class V4DirectoryGraphExecutor; }
namespace drag_global::directory_fixture
{
struct DirectoryCopyLeaseResult;
struct DirectoryCopyLeaseObservation;
class DirectoryCopyLease;
DirectoryCopyLeaseResult PrepareDirectoryCopyLease(const V4FamilyDescriptor&, const V4FamilyValidatedContext&,
    const event_graph::V4FamilyIdentityContext&, const FixtureObservation&,
    const std::shared_ptr<V4OwnedChildDirectoryAttempt>&, std::size_t, std::size_t);
DirectoryCopyLeaseResult PrepareDirectoryCopyLeaseDuringFixture(const V4FamilyDescriptor&, const V4FamilyValidatedContext&,
    const event_graph::V4FamilyIdentityContext&, const FixtureObservation&,
    const std::shared_ptr<V4OwnedChildDirectoryAttempt>&, HANDLE, const std::vector<HANDLE>&, std::size_t, std::size_t);
DirectoryCopyLeaseObservation RevalidateDirectoryCopyLease(const V4FamilyDescriptor&, const V4FamilyValidatedContext&,
    const event_graph::V4FamilyIdentityContext&, const std::shared_ptr<DirectoryCopyLease>&);

class DirectoryCopyLease final
{
    friend DirectoryCopyLeaseResult PrepareDirectoryCopyLease(const V4FamilyDescriptor&, const V4FamilyValidatedContext&,
        const event_graph::V4FamilyIdentityContext&, const FixtureObservation&,
        const std::shared_ptr<V4OwnedChildDirectoryAttempt>&, std::size_t, std::size_t);
    friend DirectoryCopyLeaseResult PrepareDirectoryCopyLeaseDuringFixture(const V4FamilyDescriptor&, const V4FamilyValidatedContext&,
        const event_graph::V4FamilyIdentityContext&, const FixtureObservation&,
        const std::shared_ptr<V4OwnedChildDirectoryAttempt>&, HANDLE, const std::vector<HANDLE>&, std::size_t, std::size_t);
    friend DirectoryCopyLeaseObservation RevalidateDirectoryCopyLease(const V4FamilyDescriptor&, const V4FamilyValidatedContext&,
        const event_graph::V4FamilyIdentityContext&, const std::shared_ptr<DirectoryCopyLease>&);
    struct Handle
    {
        HANDLE value = INVALID_HANDLE_VALUE;
        Handle() = default;
        explicit Handle(HANDLE h) : value(h) {}
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;
        Handle(Handle&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
        Handle& operator=(Handle&& other) noexcept
        {
            if (this != &other) { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
                value = std::exchange(other.value, INVALID_HANDLE_VALUE); }
            return *this;
        }
        ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    };
    struct Node
    {
        std::string slot, modelIdentity;
        std::filesystem::path relative;
        bool directory = false;
        FILE_ID_INFO id{};
        std::vector<std::byte> bytes;
        Handle handle;
    };
public:
    DirectoryCopyLease(const DirectoryCopyLease&) = delete;
    DirectoryCopyLease& operator=(const DirectoryCopyLease&) = delete;
    DWORD OwnerThread() const noexcept { return ownerThread_; }
    const std::string& CandidateId() const noexcept { return candidateId_; }
    const std::filesystem::path& Source() const noexcept { return source_; }
private:
    DirectoryCopyLease() = default;
    // Declaration order releases children before the source and pinned parent.
    std::shared_ptr<V4OwnedChildDirectoryAttempt> parent_;
    Handle root_;
    std::vector<Node> nodes_;
    std::filesystem::path source_;
    std::string familyBinding_, candidateId_;
    FILE_ID_INFO parentId_{}, rootId_{};
    DWORD ownerThread_ = 0;
    bool fixtureOverlap_ = false;
};

struct DirectoryCopyLeaseObservation
{
    bool inputObserved = false, resourceUnavailable = false, fixtureLeaseOverlapObserved = false;
    std::string candidateId;
    std::vector<std::string> unresolved;
};
struct DirectoryCopyLeaseResult
{
    std::shared_ptr<DirectoryCopyLease> lease;
    DirectoryCopyLeaseObservation observation;
};

// Identity and bytes are rechecked from live held objects and their names.
// Enumeration and file reads are not atomic against writers; callers must
// retain the unresolved boundary and never infer operation completion here.
inline DirectoryCopyLeaseObservation RevalidateDirectoryCopyLease(const V4FamilyDescriptor& family,
    const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
    const std::shared_ptr<DirectoryCopyLease>& lease)
{
    DirectoryCopyLeaseObservation out;
    try
    {
        if (!lease || GetCurrentThreadId() != lease->ownerThread_ || !lease->parent_ || !lease->parent_->available ||
            !lease->parent_->handle || lease->parent_->handle == INVALID_HANDLE_VALUE ||
            verified.Status() != V4CoordinateStatus::coordinate_ready || identity.Status() != V4FamilyStatus::schema_only ||
            verified.TaskBActive() || family.b || V4FamilyBinding(family) != identity.FamilyBinding() ||
            verified.FamilyBinding() != identity.FamilyBinding() || lease->familyBinding_ != identity.FamilyBinding() ||
            verified.Axes().size() != identity.Axes().size())
        { out.unresolved.emplace_back("directory_lease_context_or_owner_unavailable"); return out; }
        for (std::size_t i = 0; i < identity.Axes().size(); ++i)
        {
            const auto& a = verified.Axes()[i]; const auto& b = identity.Axes()[i];
            if (a.id != b.id || a.role != b.role || a.values != b.values || a.label != b.label)
            { out.unresolved.emplace_back("directory_lease_schema_mismatch"); return out; }
        }
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        if (raw.resourceUnavailable) out.resourceUnavailable = true;
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty() || raw.id != lease->candidateId_)
        { out.unresolved.emplace_back("directory_lease_candidate_unavailable"); return out; }
        const auto value = [&](std::string_view id) { return Value(verified.Axes(), verified.Coordinate(), id); };
        if (family.a.kind != V4SourceKind::file_only || family.a.fileCount.kind != V3CountKind::exact ||
            family.a.fileCount.decimal != "1" || family.a.filePaths.size() != 1 || family.a.filePaths[0].index != "0" ||
            family.a.filePaths[0].kind != V4PayloadKind::exact || family.a.filePaths[0].base != V4PathBase::raw || family.a.directoryRoots.size() != 1 ||
            family.a.directoryRoots[0].rootIndex != "0" ||
            family.a.directoryRoots[0].completeness != V4DirectoryCompleteness::complete ||
            value("V4.file.member0_kind") != "directory" || value("V4.file.member0_identity") != "unique" ||
            family.a.directoryNodes.size() != lease->nodes_.size())
        { out.unresolved.emplace_back("directory_lease_candidate_shape_unimplemented"); return out; }
        if (family.a.filePaths[0].utf16leHex.size() > 131068)
        { out.resourceUnavailable = true; out.unresolved.emplace_back("directory_lease_source_path_budget_unavailable"); return out; }
        std::filesystem::path source;
        if (!DecodePath(family.a.filePaths[0].utf16leHex, source) || source != lease->source_ ||
            source.parent_path() != lease->parent_->physicalPath || !SafeRelative(source.filename()))
        { out.unresolved.emplace_back("directory_lease_source_path_unavailable"); return out; }
        const auto checkDirectory = [&](HANDLE handle, const FILE_ID_INFO& expected) {
            FILE_ID_INFO id{}; FILE_ATTRIBUTE_TAG_INFO tag{};
            return handle && handle != INVALID_HANDLE_VALUE &&
                GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id)) && SameId(id, expected) &&
                GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag)) &&
                (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
        };
        auto openDirectory = [](const std::filesystem::path& path) {
            return DirectoryCopyLease::Handle{CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        };
        const auto namesMatch = [](const std::filesystem::path& path, std::vector<std::wstring> expected) {
            std::error_code error; std::filesystem::directory_iterator it(path, error), end;
            if (error) return false;
            std::vector<std::wstring> names;
            for (; it != end; it.increment(error)) { if (error) return false; names.push_back(it->path().filename().native()); }
            if (error) return false;
            std::sort(names.begin(), names.end()); std::sort(expected.begin(), expected.end()); return names == expected;
        };
        const auto parentName = openDirectory(lease->parent_->physicalPath), rootName = openDirectory(source);
        if (!checkDirectory(lease->parent_->handle, lease->parentId_) || !checkDirectory(parentName.value, lease->parentId_) ||
            !checkDirectory(lease->root_.value, lease->rootId_) || !checkDirectory(rootName.value, lease->rootId_))
        { out.unresolved.emplace_back("directory_lease_root_or_parent_changed"); return out; }
        std::vector<std::wstring> names;
        for (std::size_t i = 0; i < lease->nodes_.size(); ++i)
        {
            const auto& declared = family.a.directoryNodes[i]; const auto& node = lease->nodes_[i];
            std::filesystem::path relative; std::vector<std::byte> bytes;
            if (declared.relativePathUtf16Hex.size() > 131068)
            { out.resourceUnavailable = true; out.unresolved.emplace_back("directory_lease_node_path_budget_unavailable"); return out; }
            if (declared.rootIndex != "0" || declared.slotIndex != node.slot || declared.pathKind != V4PayloadKind::exact ||
                !DecodePath(declared.relativePathUtf16Hex, relative) || !SafeRelative(relative) || relative.has_parent_path() ||
                relative != node.relative || declared.objectIdentity.kind != V3CountKind::exact ||
                declared.objectIdentity.decimal != node.modelIdentity ||
                (declared.nodeKind != V4DirectoryNodeKind::directory && declared.nodeKind != V4DirectoryNodeKind::file) ||
                (declared.nodeKind == V4DirectoryNodeKind::directory) != node.directory ||
                (node.directory && declared.contentKind != V4PayloadKind::not_applicable) ||
                (!node.directory && (declared.contentKind != V4PayloadKind::exact || !DecodeHex(declared.contentHex, bytes) || bytes != node.bytes)))
            { out.unresolved.emplace_back("directory_lease_node_model_unavailable"); return out; }
            FILE_ID_INFO held{}, named{}; FILE_ATTRIBUTE_TAG_INFO tag{};
            DirectoryCopyLease::Handle current{CreateFileW((source / relative).c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT | (node.directory ? FILE_FLAG_BACKUP_SEMANTICS : 0), nullptr)};
            if (!node.handle.value || node.handle.value == INVALID_HANDLE_VALUE || current.value == INVALID_HANDLE_VALUE ||
                !GetFileInformationByHandleEx(node.handle.value, FileIdInfo, &held, sizeof(held)) || !SameId(held, node.id) ||
                !GetFileInformationByHandleEx(current.value, FileIdInfo, &named, sizeof(named)) || !SameId(named, held) ||
                !GetFileInformationByHandleEx(current.value, FileAttributeTagInfo, &tag, sizeof(tag)) ||
                (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                static_cast<bool>(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != node.directory)
            { out.unresolved.emplace_back("directory_lease_node_identity_unavailable"); return out; }
            if (SameId(held, lease->rootId_))
            { out.unresolved.emplace_back("directory_lease_root_alias_unimplemented"); return out; }
            for (std::size_t j = 0; j < i; ++j)
                if (lease->nodes_[j].modelIdentity == node.modelIdentity || SameId(lease->nodes_[j].id, held))
                { out.unresolved.emplace_back("directory_lease_node_alias_unimplemented"); return out; }
            if (node.directory)
            {
                if (!namesMatch(source / relative, {})) { out.unresolved.emplace_back("directory_lease_nested_content_unavailable"); return out; }
            }
            else
            {
                FILE_BASIC_INFO before{}, after{}; LARGE_INTEGER size{}, end{}, zero{};
                if (!GetFileInformationByHandleEx(node.handle.value, FileBasicInfo, &before, sizeof(before)) ||
                    !GetFileSizeEx(node.handle.value, &size) || size.QuadPart < 0 ||
                    static_cast<std::uint64_t>(size.QuadPart) != node.bytes.size() ||
                    !SetFilePointerEx(node.handle.value, zero, nullptr, FILE_BEGIN))
                { out.unresolved.emplace_back("directory_lease_file_read_unavailable"); return out; }
                std::size_t offset = 0; char buffer[8192];
                while (offset < node.bytes.size())
                {
                    const auto amount = static_cast<DWORD>((std::min)(sizeof(buffer), node.bytes.size() - offset)); DWORD read = 0;
                    if (!ReadFile(node.handle.value, buffer, amount, &read, nullptr) || !read || read > amount ||
                        std::memcmp(buffer, node.bytes.data() + offset, read))
                    { out.unresolved.emplace_back("directory_lease_file_bytes_unavailable"); return out; }
                    offset += read;
                }
                if (!GetFileInformationByHandleEx(node.handle.value, FileBasicInfo, &after, sizeof(after)) ||
                    before.LastWriteTime.QuadPart != after.LastWriteTime.QuadPart ||
                    !GetFileSizeEx(node.handle.value, &end) || end.QuadPart != size.QuadPart)
                { out.unresolved.emplace_back("directory_lease_file_changed_during_read"); return out; }
                DirectoryCopyLease::Handle namedAfter{CreateFileW((source / relative).c_str(), FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
                FILE_ID_INFO afterName{}; FILE_ATTRIBUTE_TAG_INFO afterTag{};
                if (namedAfter.value == INVALID_HANDLE_VALUE ||
                    !GetFileInformationByHandleEx(namedAfter.value, FileIdInfo, &afterName, sizeof(afterName)) || !SameId(afterName, held) ||
                    !GetFileInformationByHandleEx(namedAfter.value, FileAttributeTagInfo, &afterTag, sizeof(afterTag)) ||
                    (afterTag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
                { out.unresolved.emplace_back("directory_lease_file_name_changed_during_read"); return out; }
            }
            names.push_back(relative.native());
        }
        if (!namesMatch(source, std::move(names)) || !checkDirectory(lease->root_.value, lease->rootId_) ||
            !checkDirectory(openDirectory(source).value, lease->rootId_) || !checkDirectory(lease->parent_->handle, lease->parentId_))
        { out.unresolved.emplace_back("directory_lease_tree_or_root_changed"); return out; }
        out.candidateId = raw.id; out.inputObserved = true; out.fixtureLeaseOverlapObserved = lease->fixtureOverlap_;
        out.unresolved = {"directory_lease_shared_write_delete_and_nonatomic_snapshot_unresolved",
            "directory_lease_directory_delete_or_move_unimplemented",
            "directory_lease_profile_source_epoch_and_other_axes_unimplemented",
            "directory_lease_production_dispatch_completion_and_business_unverified"};
        if (!lease->fixtureOverlap_)
            out.unresolved.emplace_back("directory_lease_fixture_to_lease_identity_gap_unverified");
        return out;
    }
    catch (const std::bad_alloc&) { out.inputObserved = false; out.resourceUnavailable = true; return out; }
    catch (const std::length_error&) { out.inputObserved = false; out.resourceUnavailable = true; return out; }
    catch (...) { out.inputObserved = false; return out; }
}

inline DirectoryCopyLeaseResult PrepareDirectoryCopyLease(const V4FamilyDescriptor& family,
    const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
    const FixtureObservation& input, const std::shared_ptr<V4OwnedChildDirectoryAttempt>& parent,
    std::size_t maxNodes, std::size_t maxBytes)
{
    DirectoryCopyLeaseResult out;
    try
    {
        if (family.a.directoryNodes.size() > maxNodes)
        { out.observation.resourceUnavailable = true; out.observation.unresolved.emplace_back("directory_lease_node_budget_unavailable"); return out; }
        if (!parent || !parent->available || !parent->created || !parent->identityObserved || !input.inputsObserved ||
            !input.rootCreated || !input.rootIdentityObserved || input.nodes.size() != family.a.directoryNodes.size() ||
            verified.Status() != V4CoordinateStatus::coordinate_ready || identity.Status() != V4FamilyStatus::schema_only ||
            verified.TaskBActive() || family.b || V4FamilyBinding(family) != identity.FamilyBinding() ||
            verified.FamilyBinding() != identity.FamilyBinding() || verified.Axes().size() != identity.Axes().size())
        { out.observation.unresolved.emplace_back("directory_lease_fixture_or_context_unavailable"); return out; }
        for (std::size_t i = 0; i < identity.Axes().size(); ++i)
        {
            const auto& a = verified.Axes()[i]; const auto& b = identity.Axes()[i];
            if (a.id != b.id || a.role != b.role || a.values != b.values || a.label != b.label)
            { out.observation.unresolved.emplace_back("directory_lease_schema_mismatch"); return out; }
        }
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        if (raw.resourceUnavailable) out.observation.resourceUnavailable = true;
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty() || raw.id != input.candidateId)
        { out.observation.unresolved.emplace_back("directory_lease_candidate_unavailable"); return out; }
        auto lease = std::shared_ptr<DirectoryCopyLease>(new DirectoryCopyLease);
        lease->parent_ = parent; lease->parentId_ = parent->identity; lease->rootId_ = input.rootIdentity;
        lease->source_ = input.source; lease->familyBinding_ = identity.FamilyBinding();
        lease->candidateId_ = raw.id; lease->ownerThread_ = GetCurrentThreadId();
        if (family.a.filePaths.size() == 1 && family.a.filePaths[0].utf16leHex.size() > 131068)
        { out.observation.resourceUnavailable = true; out.observation.unresolved.emplace_back("directory_lease_source_path_budget_unavailable"); return out; }
        std::filesystem::path modelSource;
        if (family.a.kind != V4SourceKind::file_only || family.a.fileCount.kind != V3CountKind::exact ||
            family.a.fileCount.decimal != "1" || family.a.filePaths.size() != 1 || family.a.filePaths[0].index != "0" ||
            family.a.filePaths[0].kind != V4PayloadKind::exact || family.a.filePaths[0].base != V4PathBase::raw || !DecodePath(family.a.filePaths[0].utf16leHex, modelSource) ||
            modelSource != input.source || !input.source.is_absolute() || input.source.parent_path() != parent->physicalPath ||
            !SafeRelative(input.source.filename()))
        { out.observation.unresolved.emplace_back("directory_lease_owned_source_path_unavailable"); return out; }
        lease->root_ = DirectoryCopyLease::Handle{CreateFileW(input.source.c_str(), FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        FILE_ID_INFO heldRoot{}, heldParent{}; FILE_ATTRIBUTE_TAG_INFO rootTag{}, parentTag{};
        if (lease->root_.value == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandleEx(lease->root_.value, FileIdInfo, &heldRoot, sizeof(heldRoot)) || !SameId(heldRoot, input.rootIdentity) ||
            !GetFileInformationByHandleEx(lease->root_.value, FileAttributeTagInfo, &rootTag, sizeof(rootTag)) ||
            !(rootTag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (rootTag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            !GetFileInformationByHandleEx(parent->handle, FileIdInfo, &heldParent, sizeof(heldParent)) || !SameId(heldParent, parent->identity) ||
            !GetFileInformationByHandleEx(parent->handle, FileAttributeTagInfo, &parentTag, sizeof(parentTag)) ||
            !(parentTag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (parentTag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        { out.observation.unresolved.emplace_back("directory_lease_source_open_unavailable"); return out; }
        lease->nodes_.reserve(family.a.directoryNodes.size()); std::size_t total = 0;
        for (const auto& declared : family.a.directoryNodes)
        {
            std::filesystem::path relative; std::vector<std::byte> bytes;
            const auto observed = std::find_if(input.nodes.begin(), input.nodes.end(),
                [&](const NodeObservation& node) { return node.slot == declared.slotIndex; });
            if (declared.relativePathUtf16Hex.size() > 131068)
            { out.observation.resourceUnavailable = true; out.observation.unresolved.emplace_back("directory_lease_node_path_budget_unavailable"); return out; }
            if (observed == input.nodes.end() || !observed->created || !observed->identityObserved ||
                declared.rootIndex != "0" || declared.pathKind != V4PayloadKind::exact ||
                !DecodePath(declared.relativePathUtf16Hex, relative) || !SafeRelative(relative) || relative.has_parent_path() ||
                observed->relative != relative || declared.objectIdentity.kind != V3CountKind::exact ||
                (declared.nodeKind != V4DirectoryNodeKind::directory && declared.nodeKind != V4DirectoryNodeKind::file))
            { out.observation.unresolved.emplace_back("directory_lease_direct_node_unavailable"); return out; }
            const bool isDirectory = declared.nodeKind == V4DirectoryNodeKind::directory;
            if (!isDirectory && declared.contentHex.size() / 2 > maxBytes - total)
            { out.observation.resourceUnavailable = true; out.observation.unresolved.emplace_back("directory_lease_byte_budget_unavailable"); return out; }
            if (observed->directory != isDirectory || (isDirectory && declared.contentKind != V4PayloadKind::not_applicable) ||
                (!isDirectory && (declared.contentKind != V4PayloadKind::exact || !DecodeHex(declared.contentHex, bytes) ||
                    !observed->bytesObserved || observed->actualBytes != bytes)))
            { out.observation.unresolved.emplace_back("directory_lease_node_content_unavailable"); return out; }
            if (bytes.size() > maxBytes - total)
            { out.observation.resourceUnavailable = true; out.observation.unresolved.emplace_back("directory_lease_byte_budget_unavailable"); return out; }
            total += bytes.size();
            DirectoryCopyLease::Node node; node.slot = declared.slotIndex; node.modelIdentity = declared.objectIdentity.decimal; node.relative = relative;
            node.directory = isDirectory; node.id = observed->identity; node.bytes = std::move(bytes);
            node.handle = DirectoryCopyLease::Handle{CreateFileW((input.source / relative).c_str(),
                isDirectory ? FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY : GENERIC_READ | FILE_READ_ATTRIBUTES,
                isDirectory ? FILE_SHARE_READ | FILE_SHARE_WRITE : FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT | (isDirectory ? FILE_FLAG_BACKUP_SEMANTICS : 0), nullptr)};
            if (node.handle.value == INVALID_HANDLE_VALUE)
            { out.observation.unresolved.emplace_back("directory_lease_node_open_unavailable"); return out; }
            lease->nodes_.push_back(std::move(node));
        }
        out.observation = RevalidateDirectoryCopyLease(family, verified, identity, lease);
        if (out.observation.inputObserved) out.lease = std::move(lease);
        return out;
    }
    catch (const std::bad_alloc&) { out.observation.resourceUnavailable = true; return out; }
    catch (const std::length_error&) { out.observation.resourceUnavailable = true; return out; }
    catch (...) { return out; }
}

// Call synchronously from fixture construction while its root and direct
// node handles are still open. heldNodes follow input.nodes, not model order.
// Only actual handle identity overlap clears the fixture-to-lease gap.
inline DirectoryCopyLeaseResult PrepareDirectoryCopyLeaseDuringFixture(const V4FamilyDescriptor& family,
    const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
    const FixtureObservation& input, const std::shared_ptr<V4OwnedChildDirectoryAttempt>& parent,
    HANDLE heldRoot, const std::vector<HANDLE>& heldNodes, std::size_t maxNodes, std::size_t maxBytes)
{
    auto out = PrepareDirectoryCopyLease(family, verified, identity, input, parent, maxNodes, maxBytes);
    if (!out.lease || !out.observation.inputObserved) return out;
    try
    {
        if (!heldRoot || heldRoot == INVALID_HANDLE_VALUE || heldNodes.size() != input.nodes.size() ||
            input.nodes.size() != out.lease->nodes_.size())
        { out.observation.inputObserved = false; out.observation.unresolved.emplace_back("directory_fixture_overlap_shape_unavailable"); out.lease.reset(); return out; }
        const auto overlaps = [](HANDLE handle, const FILE_ID_INFO& expected, bool directory) {
            FILE_ID_INFO id{}; FILE_ATTRIBUTE_TAG_INFO tag{};
            return handle && handle != INVALID_HANDLE_VALUE &&
                GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id)) && SameId(id, expected) &&
                GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag)) &&
                !(tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                static_cast<bool>(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == directory;
        };
        if (!overlaps(heldRoot, out.lease->rootId_, true))
        { out.observation.inputObserved = false; out.observation.unresolved.emplace_back("directory_fixture_root_overlap_unavailable"); out.lease.reset(); return out; }
        for (std::size_t i = 0; i < input.nodes.size(); ++i)
        {
            const auto& item = input.nodes[i];
            const auto node = std::find_if(out.lease->nodes_.begin(), out.lease->nodes_.end(),
                [&](const auto& value) { return value.slot == item.slot; });
            if (node == out.lease->nodes_.end() || !overlaps(heldNodes[i], node->id, node->directory))
            { out.observation.inputObserved = false; out.observation.unresolved.emplace_back("directory_fixture_node_overlap_unavailable"); out.lease.reset(); return out; }
        }
        out.lease->fixtureOverlap_ = true;
        out.observation = RevalidateDirectoryCopyLease(family, verified, identity, out.lease);
        if (!out.observation.inputObserved) out.lease.reset();
        return out;
    }
    catch (const std::bad_alloc&) { out.lease.reset(); out.observation.inputObserved = false; out.observation.resourceUnavailable = true; return out; }
    catch (const std::length_error&) { out.lease.reset(); out.observation.inputObserved = false; out.observation.resourceUnavailable = true; return out; }
    catch (...) { out.lease.reset(); out.observation.inputObserved = false; return out; }
}

class DirectoryShellBinding;
class DirectoryShellOwner final
{
    friend struct ::NativeDragIntegrationAccess;
    friend class DirectoryShellBinding;
    friend class ::drag_global::ole_driver::V4DirectoryGraphExecutor;
    friend DirectoryShellBinding PrepareDirectoryShellSource(const V4FamilyDescriptor&, const V4FamilyValidatedContext&,
        const event_graph::V4FamilyIdentityContext&, const std::shared_ptr<DirectoryCopyLease>&,
        const std::shared_ptr<DirectoryShellOwner>&);
public:
    DirectoryShellOwner(const DirectoryShellOwner&) = delete;
    DirectoryShellOwner& operator=(const DirectoryShellOwner&) = delete;
    ~DirectoryShellOwner()
    {
        // The profile must invalidate on its live STA after background work
        // is settled. A missed protocol retains both COM and the lease rather
        // than release either while an operation may still be using them.
        if (source_) { (void)source_.Detach(); (void)retainedLease_.release(); }
    }
private:
    DirectoryShellOwner() noexcept : ownerThread_(GetCurrentThreadId()) {}
    bool Live() const noexcept
    {
        if (!active_.load(std::memory_order_acquire) || GetCurrentThreadId() != ownerThread_) return false;
        APTTYPE apartment{}; APTTYPEQUALIFIER qualifier{};
        return SUCCEEDED(CoGetApartmentType(&apartment, &qualifier)) &&
            (apartment == APTTYPE_STA || apartment == APTTYPE_MAINSTA);
    }
    bool Attach(Microsoft::WRL::ComPtr<ole_file_source_fixture::Source>&& source,
        std::unique_ptr<std::shared_ptr<DirectoryCopyLease>>&& retained) noexcept
    {
        if (!Live() || source_ || retainedLease_ || !source || source->owner != ownerThread_ ||
            !retained || !*retained || (*retained)->OwnerThread() != ownerThread_) return false;
        source_ = std::move(source); retainedLease_ = std::move(retained); return true;
    }
    bool CanInvalidate() const noexcept
    { return GetCurrentThreadId() == ownerThread_ && !executing_.load(std::memory_order_acquire); }
    bool TryBeginExecution() const noexcept
    {
        if (!Live() || !source_ || !retainedLease_ || !*retainedLease_) return false;
        bool expected = false;
        return executing_.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
    }
    bool TryBeginPreparation() const noexcept
    {
        if (!Live() || source_ || retainedLease_) return false;
        bool expected = false;
        return executing_.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
    }
    void EndExecution() const noexcept { executing_.store(false, std::memory_order_release); }
    void Invalidate() noexcept
    {
        const DWORD saved = GetLastError();
        if (CanInvalidate())
        {
            active_.store(false, std::memory_order_release);
            source_.Reset(); retainedLease_.reset();
        }
        SetLastError(saved);
    }
    DWORD ownerThread_ = 0;
    std::atomic_bool active_{true};
    mutable std::atomic_bool executing_{false};
    Microsoft::WRL::ComPtr<ole_file_source_fixture::Source> source_;
    std::unique_ptr<std::shared_ptr<DirectoryCopyLease>> retainedLease_;
};

class DirectoryShellBinding final
{
    friend DirectoryShellBinding PrepareDirectoryShellSource(const V4FamilyDescriptor&, const V4FamilyValidatedContext&,
        const event_graph::V4FamilyIdentityContext&, const std::shared_ptr<DirectoryCopyLease>&,
        const std::shared_ptr<DirectoryShellOwner>&);
public:
    bool inputPrepared = false, resourceUnavailable = false;
    std::string candidateId;
    std::vector<std::string> unresolved;
    ole_file_source_fixture::Source* SourceObject() const noexcept
    {
        const auto owner = owner_.lock();
        return owner && owner->Live() && owner->retainedLease_ && *owner->retainedLease_ &&
            owner->source_.Get() == sourceIdentity_ ? sourceIdentity_ : nullptr;
    }
private:
    std::weak_ptr<DirectoryShellOwner> owner_;
    ole_file_source_fixture::Source* sourceIdentity_ = nullptr;
};

// No QueryGetData/GetData and no production call occurs here. The returned
// binding borrows the source, while the Profile-owned owner holds the COM
// object and the exact lease together until safe owner-STA invalidation.
inline DirectoryShellBinding PrepareDirectoryShellSource(const V4FamilyDescriptor& family,
    const V4FamilyValidatedContext& verified, const event_graph::V4FamilyIdentityContext& identity,
    const std::shared_ptr<DirectoryCopyLease>& lease, const std::shared_ptr<DirectoryShellOwner>& owner)
{
    DirectoryShellBinding out;
    // The caller's shared_ptr variables may be changed by nested UI work.
    const auto pinnedOwner = owner;
    const auto pinnedLease = lease;
    try
    {
        if (!pinnedOwner || !pinnedOwner->Live() || !pinnedLease || pinnedLease->OwnerThread() != GetCurrentThreadId())
        { out.unresolved.emplace_back("directory_shell_owner_or_lease_unavailable"); return out; }
        if (!pinnedOwner->TryBeginPreparation())
        { out.unresolved.emplace_back("directory_shell_owner_preparation_busy"); return out; }
        const auto end = [](const DirectoryShellOwner* value) noexcept { value->EndExecution(); };
        const std::unique_ptr<const DirectoryShellOwner, decltype(end)> guard(pinnedOwner.get(), end);
        const auto before = RevalidateDirectoryCopyLease(family, verified, identity, pinnedLease);
        if (!before.inputObserved || before.resourceUnavailable || !before.fixtureLeaseOverlapObserved)
        { out.resourceUnavailable = before.resourceUnavailable; out.unresolved.emplace_back("directory_shell_lease_unavailable"); return out; }
        const auto value = [&](std::string_view id) { return Value(verified.Axes(), verified.Coordinate(), id); };
        if (value("T04.capability") != "enabled")
        { out.unresolved.emplace_back("directory_shell_async_mode_unimplemented"); return out; }
        auto retained = std::make_unique<std::shared_ptr<DirectoryCopyLease>>(pinnedLease);
        auto source = ole_file_source_fixture::Make({pinnedLease->Source()}, true);
        auto native = pinnedLease->Source(); native.make_preferred();
        if (!source || !source->HasConstructionShell() || source->owner != GetCurrentThreadId() || !source->asynchronous ||
            source->ConstructionPaths().size() != 1 || source->ConstructionPaths().front() != native)
        { out.unresolved.emplace_back("directory_shell_source_construction_unavailable"); return out; }
        const auto after = RevalidateDirectoryCopyLease(family, verified, identity, pinnedLease);
        if (!after.inputObserved || after.resourceUnavailable || !after.fixtureLeaseOverlapObserved || after.candidateId != before.candidateId)
        { out.resourceUnavailable = after.resourceUnavailable; out.unresolved.emplace_back("directory_shell_lease_changed_during_construction"); return out; }
        auto* const sourceIdentity = source.Get();
        if (!pinnedOwner->Attach(std::move(source), std::move(retained)))
        { out.unresolved.emplace_back("directory_shell_owner_attach_unavailable"); return out; }
        out.owner_ = pinnedOwner; out.sourceIdentity_ = sourceIdentity;
        out.candidateId = before.candidateId; out.inputPrepared = true;
        out.unresolved = {"directory_shell_formats_and_actual_getdata_unobserved",
            "directory_shell_profile_completion_and_business_unverified",
            "directory_shell_shared_write_snapshot_nonatomic"};
        return out;
    }
    catch (const std::bad_alloc&) { out.inputPrepared = false; out.resourceUnavailable = true; return out; }
    catch (const std::length_error&) { out.inputPrepared = false; out.resourceUnavailable = true; return out; }
    catch (...) { out.inputPrepared = false; return out; }
}
} // namespace drag_global::directory_fixture
