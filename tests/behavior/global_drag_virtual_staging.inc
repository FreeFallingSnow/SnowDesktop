#pragma once

#include "global_drag_payload_fixture.inc"
#include "drag_drop/drop_staging.h"
#include <cstdint>
#include <limits>
#include <shellapi.h>

namespace drag_global::virtual_staging_fixture
{
// This is a preparation/observation object for a dedicated child executable.
// It never grants an execution permit and never removes a directory. The
// caller must retain it until after all child work is finished, then destroy it
// before returning from the child entry point. Production's static Session is
// destroyed later during process exit; the session handle below allows delete
// sharing so that its normal destructor can remove its own session.
class ChildIsolation final
{
public:
    ChildIsolation(const ChildIsolation&) = delete;
    ChildIsolation& operator=(const ChildIsolation&) = delete;
    ~ChildIsolation() { if (sessionHandle_ != INVALID_HANDLE_VALUE) CloseHandle(sessionHandle_); }

    // These public fields are partial diagnostics, not an execution permit.
    bool ready = false, environmentModified = false, sessionObserved = false;
    bool rootCreateAttempted = false, rootCreated = false, rootIdentityObserved = false;
    bool stagingCreateAttempted = false, stagingCreated = false, stagingIdentityObserved = false;
    bool ExecutionAuthorized() const noexcept { return false; }
    DWORD processId = 0, ownerThread = 0, error = 0;
    std::filesystem::path rootPath, stagingRootPath, sessionPath;
    FILE_ID_INFO rootId{}, stagingRootId{}, sessionId{};
    std::vector<std::string> unresolved;

    // Local, non-atomic revalidation only. In particular, this does not
    // prove an isolated child launch, prevent future writers, or authorize a
    // production call. A failed check must keep the staging gate closed.
    bool StillOwned() const noexcept
    {
        const DWORD saved = GetLastError();
        struct ErrorGuard { DWORD value; ~ErrorGuard() { SetLastError(value); } } restore{saved};
        try
        {
            if (!ready || !environmentModified || !sessionObserved || processId != GetCurrentProcessId() ||
                ownerThread != GetCurrentThreadId() || !parent_ || !parent_->available || parent_->handles.empty() ||
                parent_->handles.size() != parent_->identities.size() || !root_ || !staging_ ||
                !root_->available || !staging_->available || sessionHandle_ == INVALID_HANDLE_VALUE) return false;
            const auto sameId = [](const FILE_ID_INFO& a, const FILE_ID_INFO& b) {
                return Nonzero(a) && a.VolumeSerialNumber == b.VolumeSerialNumber &&
                    std::memcmp(a.FileId.Identifier, b.FileId.Identifier, sizeof(a.FileId.Identifier)) == 0;
            };
            const auto held = [&](HANDLE handle, const FILE_ID_INFO& expected) {
                FILE_ID_INFO id{}; FILE_ATTRIBUTE_TAG_INFO tag{};
                return handle && handle != INVALID_HANDLE_VALUE &&
                    GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag)) &&
                    (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                    GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id)) && sameId(id, expected);
            };
            struct HandleGuard { HANDLE h; ~HandleGuard() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); } };
            const auto named = [&](const std::filesystem::path& path, const FILE_ID_INFO& id) {
                HandleGuard handle{CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
                return held(handle.h, id);
            };
            if (!held(parent_->handles.back(), parent_->identities.back()) ||
                !held(root_->handle, rootId) || !held(staging_->handle, stagingRootId) || !held(sessionHandle_, sessionId) ||
                !named(parent_->physicalPath, parent_->identities.back()) || !named(rootPath, rootId) ||
                !named(stagingRootPath, stagingRootId) || !named(sessionPath, sessionId)) return false;
            std::error_code ec;
            if (!std::filesystem::equivalent(rootPath.parent_path(), parent_->physicalPath, ec) || ec ||
                !std::filesystem::equivalent(stagingRootPath.parent_path(), rootPath, ec) || ec ||
                !std::filesystem::equivalent(sessionPath.parent_path(), stagingRootPath, ec) || ec ||
                sessionPath.filename().native().empty() ||
                !sessionPath.filename().native().starts_with(L"session-" + std::to_wstring(processId) + L"-")) return false;
            const auto matchesEnvironment = [&](const wchar_t* name) {
                std::vector<wchar_t> value(32768);
                const DWORD count = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
                return count && count < value.size() && std::wstring_view(value.data(), count) == rootPath.native();
            };
            if (!matchesEnvironment(L"TEMP") || !matchesEnvironment(L"TMP")) return false;
            const auto effective = std::filesystem::temp_directory_path(ec);
            if (ec || !std::filesystem::equivalent(effective, rootPath, ec) || ec ||
                snowdesktop::drop_staging::Directory() != sessionPath) return false;
            std::filesystem::directory_iterator it(stagingRootPath, ec), end;
            if (ec || it == end || !std::filesystem::equivalent(it->path(), sessionPath, ec) || ec) return false;
            it.increment(ec); if (ec || it != end) return false;
            const auto marker = sessionPath / L".owner.lock";
            const DWORD attributes = GetFileAttributesW(marker.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return false;
            SetLastError(ERROR_SUCCESS);
            HandleGuard probe{CreateFileW(marker.c_str(), DELETE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
            return probe.h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION;
        }
        catch (...) { return false; }
    }

    // Borrowed only while this object is alive on its owning thread. Each
    // accessor revalidates current ownership; neither grants execution or
    // makes the two calls an atomic snapshot. The caller must not close it.
    HANDLE RootHandle() const noexcept
    { return StillOwned() ? root_->handle : nullptr; }
    std::optional<FILE_ID_INFO> RootIdentity() const noexcept
    { return StillOwned() ? std::optional<FILE_ID_INFO>(root_->identity) : std::nullopt; }

    // The executable must have entered through one of the dedicated child modes.
    // A nonce is a routing discriminator, not a security credential or proof
    // of causal completion. The parent must create the child and call this at
    // its earliest entry, before any staging use or worker creation.
    static std::unique_ptr<ChildIsolation> Prepare(std::wstring_view nonce)
    {
        auto out = std::unique_ptr<ChildIsolation>(new ChildIsolation);
        out->processId = GetCurrentProcessId(); out->ownerThread = GetCurrentThreadId();
        try
        {
            if (nonce.size() != 32 || std::any_of(nonce.begin(), nonce.end(), [](wchar_t c) {
                return !((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f')); }))
            { out->unresolved.emplace_back("virtual_child_nonce_invalid"); return out; }
            int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
            struct LocalGuard { LPWSTR* p; ~LocalGuard() { if (p) LocalFree(p); } } args{argv};
            if (!argv || argc != 4 ||
                (std::wstring_view(argv[1]) != L"--v4-virtual-staging-child" &&
                 std::wstring_view(argv[1]) != L"--v4-owned-file-child" &&
                 std::wstring_view(argv[1]) != L"--v4-exact-source-child") || std::wstring_view(argv[2]) != nonce)
            { out->unresolved.emplace_back("virtual_child_entry_identity_unavailable"); return out; }
            const std::wstring_view handleText(argv[3]);
            if (handleText.empty() || (handleText.size() > 1 && handleText.front() == L'0'))
            { out->unresolved.emplace_back("virtual_child_handle_invalid"); return out; }
            std::uintptr_t handleValue = 0;
            for (const auto ch : handleText)
            {
                if (ch < L'0' || ch > L'9')
                { out->unresolved.emplace_back("virtual_child_handle_invalid"); return out; }
                const auto digit = static_cast<std::uintptr_t>(ch - L'0');
                if (handleValue > ((std::numeric_limits<std::uintptr_t>::max)() - digit) / 10)
                { out->unresolved.emplace_back("virtual_child_handle_invalid"); return out; }
                handleValue = handleValue * 10 + digit;
            }
            if (!handleValue || handleValue == reinterpret_cast<std::uintptr_t>(INVALID_HANDLE_VALUE))
            { out->unresolved.emplace_back("virtual_child_handle_invalid"); return out; }
            std::vector<wchar_t> temp(32768);
            const DWORD length = GetTempPathW(static_cast<DWORD>(temp.size()), temp.data());
            if (!length || length >= temp.size())
            { out->error = GetLastError(); out->unresolved.emplace_back("virtual_child_parent_temp_unavailable"); return out; }
            auto parent = OpenV4PinnedLocalDirectoryChain(std::filesystem::path(std::wstring(temp.data(), length)));
            if (!parent.available || parent.handles.empty() || parent.identities.size() != parent.handles.size())
            { out->unresolved.emplace_back("virtual_child_parent_directory_unavailable"); return out; }
            out->parent_ = std::make_unique<V4PinnedDirectoryChain>(std::move(parent));
            const auto leaf = std::wstring(L"SnowVirtual-") + std::wstring(nonce) + L"-" + std::to_wstring(out->processId);
            // Allocate holders and record intended names before the exclusive
            // create. A later allocation/observation failure must not hide an
            // already-created directory or release its handle prematurely.
            out->root_ = std::make_unique<V4OwnedChildDirectoryAttempt>();
            out->rootPath = out->parent_->physicalPath / leaf;
            auto root = CreateV4OwnedChildDirectory(out->parent_->handles.back(), out->parent_->identities.back(), leaf);
            out->rootCreateAttempted = root.attempted; out->rootCreated = root.created;
            out->rootIdentityObserved = root.identityObserved; out->rootId = root.identity;
            Transfer(*out->root_, root);
            if (!out->root_->available || !out->rootCreated || !out->rootIdentityObserved)
            { out->unresolved.emplace_back("virtual_child_exclusive_root_unavailable"); return out; }
            out->rootPath = out->root_->physicalPath;
            out->staging_ = std::make_unique<V4OwnedChildDirectoryAttempt>();
            out->stagingRootPath = out->rootPath / L"SnowDesktop.DropContent";
            auto staging = CreateV4OwnedChildDirectory(out->root_->handle, out->root_->identity, L"SnowDesktop.DropContent");
            out->stagingCreateAttempted = staging.attempted; out->stagingCreated = staging.created;
            out->stagingIdentityObserved = staging.identityObserved; out->stagingRootId = staging.identity;
            Transfer(*out->staging_, staging);
            if (!out->staging_->available || !out->stagingCreated || !out->stagingIdentityObserved)
            { out->unresolved.emplace_back("virtual_child_staging_root_unavailable"); return out; }
            out->stagingRootPath = out->staging_->physicalPath;
            std::error_code ec;
            std::filesystem::directory_iterator before(out->stagingRootPath, ec), end;
            if (ec || before != end)
            { out->unresolved.emplace_back("virtual_child_staging_root_not_empty"); return out; }
            if (!SetEnvironmentVariableW(L"TEMP", out->rootPath.c_str()))
            { out->error = GetLastError(); out->unresolved.emplace_back("virtual_child_temp_set_unavailable"); return out; }
            out->environmentModified = true;
            if (!SetEnvironmentVariableW(L"TMP", out->rootPath.c_str()))
            { out->error = GetLastError(); out->unresolved.emplace_back("virtual_child_tmp_set_unavailable"); return out; }
            const auto matchesEnvironment = [&](const wchar_t* name) {
                std::vector<wchar_t> value(32768);
                const DWORD count = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
                return count && count < value.size() && std::wstring_view(value.data(), count) == out->rootPath.native();
            };
            if (!matchesEnvironment(L"TEMP") || !matchesEnvironment(L"TMP"))
            { out->unresolved.emplace_back("virtual_child_temp_environment_mismatch"); return out; }
            const auto actualTemp = std::filesystem::temp_directory_path(ec);
            if (ec || !std::filesystem::equivalent(actualTemp, out->rootPath, ec) || ec)
            { out->unresolved.emplace_back("virtual_child_effective_temp_mismatch"); return out; }
            // This is the first permitted production staging call in the
            // dedicated child; an already-initialized foreign Session yields
            // a different path and cannot be accepted below.
            out->sessionPath = snowdesktop::drop_staging::Directory();
            if (out->sessionPath.empty() || !out->sessionPath.is_absolute() ||
                !std::filesystem::equivalent(out->sessionPath.parent_path(), out->stagingRootPath, ec) || ec ||
                !out->sessionPath.filename().native().starts_with(L"session-" + std::to_wstring(out->processId) + L"-"))
            { out->unresolved.emplace_back("virtual_child_production_session_path_unavailable"); return out; }
            out->sessionHandle_ = CreateFileW(out->sessionPath.c_str(), FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            FILE_ATTRIBUTE_TAG_INFO tag{};
            if (out->sessionHandle_ == INVALID_HANDLE_VALUE ||
                !GetFileInformationByHandleEx(out->sessionHandle_, FileAttributeTagInfo, &tag, sizeof(tag)) ||
                !(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                !GetFileInformationByHandleEx(out->sessionHandle_, FileIdInfo, &out->sessionId, sizeof(out->sessionId)) ||
                !Nonzero(out->sessionId))
            { out->unresolved.emplace_back("virtual_child_production_session_identity_unavailable"); return out; }
            std::vector<std::filesystem::path> sessions;
            for (std::filesystem::directory_iterator it(out->stagingRootPath, ec); !ec && it != end; it.increment(ec))
                sessions.push_back(it->path());
            if (ec || sessions.size() != 1 || !std::filesystem::equivalent(sessions.front(), out->sessionPath, ec) || ec)
            { out->unresolved.emplace_back("virtual_child_session_enumeration_unavailable"); return out; }
            struct HandleGuard { HANDLE h; ~HandleGuard() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); } };
            HandleGuard named{CreateFileW(out->sessionPath.c_str(), FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
            FILE_ID_INFO namedId{}; FILE_ATTRIBUTE_TAG_INFO namedTag{};
            if (named.h == INVALID_HANDLE_VALUE ||
                !GetFileInformationByHandleEx(named.h, FileIdInfo, &namedId, sizeof(namedId)) ||
                namedId.VolumeSerialNumber != out->sessionId.VolumeSerialNumber ||
                std::memcmp(namedId.FileId.Identifier, out->sessionId.FileId.Identifier, sizeof(namedId.FileId.Identifier)) != 0 ||
                !GetFileInformationByHandleEx(named.h, FileAttributeTagInfo, &namedTag, sizeof(namedTag)) ||
                !(namedTag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (namedTag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            { out->unresolved.emplace_back("virtual_child_session_name_identity_unavailable"); return out; }
            std::vector<std::wstring> entries;
            for (std::filesystem::directory_iterator it(out->sessionPath, ec); !ec && it != end; it.increment(ec))
                entries.push_back(it->path().filename().native());
            if (ec || entries.size() != 1 || entries.front() != L".owner.lock")
            { out->unresolved.emplace_back("virtual_child_session_marker_unavailable"); return out; }
            const auto marker = out->sessionPath / L".owner.lock";
            const DWORD markerAttributes = GetFileAttributesW(marker.c_str());
            if (markerAttributes == INVALID_FILE_ATTRIBUTES ||
                (markerAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
            { out->unresolved.emplace_back("virtual_child_session_marker_invalid"); return out; }
            SetLastError(ERROR_SUCCESS);
            HandleGuard probe{CreateFileW(marker.c_str(), DELETE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
            const DWORD probeError = GetLastError();
            if (probe.h != INVALID_HANDLE_VALUE || probeError != ERROR_SHARING_VIOLATION)
            { out->error = probeError; out->unresolved.emplace_back("virtual_child_session_owner_lock_unavailable"); return out; }
            out->sessionObserved = true; out->ready = true;
            out->unresolved = {"virtual_child_fresh_entry_and_parent_handoff_unverified",
                "virtual_child_snapshot_nonatomic_and_session_cleanup_unverified",
                "virtual_child_candidate_dispatch_and_business_unimplemented"};
        }
        catch (const std::bad_alloc&) { out->unresolved.emplace_back("virtual_child_resource_unavailable"); }
        catch (const std::length_error&) { out->unresolved.emplace_back("virtual_child_resource_unavailable"); }
        catch (...) { out->unresolved.emplace_back("virtual_child_observation_unavailable"); }
        return out;
    }
private:
    ChildIsolation() = default;
    static void Transfer(V4OwnedChildDirectoryAttempt& target, V4OwnedChildDirectoryAttempt& source) noexcept
    {
        target.physicalPath = std::move(source.physicalPath);
        target.handle = std::exchange(source.handle, INVALID_HANDLE_VALUE); target.identity = source.identity;
        target.attempted = source.attempted; target.created = source.created;
        target.identityObserved = source.identityObserved; target.available = source.available; target.reason = source.reason;
    }
    static bool Nonzero(const FILE_ID_INFO& id) noexcept
    { return std::any_of(std::begin(id.FileId.Identifier), std::end(id.FileId.Identifier), [](BYTE b) { return b != 0; }); }
    // Destruction order is sessionHandle (explicit destructor), staging,
    // root, pinned parent. All handles live until the child entry returns.
    std::unique_ptr<V4PinnedDirectoryChain> parent_;
    std::unique_ptr<V4OwnedChildDirectoryAttempt> root_, staging_;
    HANDLE sessionHandle_ = INVALID_HANDLE_VALUE;
};
} // namespace drag_global::virtual_staging_fixture
