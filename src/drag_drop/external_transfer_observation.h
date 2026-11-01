#pragma once

#include <windows.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <type_traits>

// Private, opt-in diagnostics. Values never own COM, HWND or completion
// objects. Log loss is observation failure, not an input or product decision.
namespace snowdesktop::external_transfer_observation
{
enum class Phase : unsigned
{
    transfer_enter, read_queue_enter, read_queued, read_queue_failed, read_worker_enter,
    read_worker_return, read_completion_callback, completion_posted,
    completion_post_failed, completion_enter, completion_return,
    file_queue_enter, file_queued, file_queue_failed, file_completion_callback,
    start_operation, end_operation_enter, end_operation, ambiguous_nested_scope,
    worker_dequeued, worker_execute_enter, worker_execute_return,
    worker_task_finished, worker_task_cancelled,
    transfer_finished_enter, transfer_finished_return, transfer_finished_duplicate,
    transfer_finished_context_mismatch, read_content_paths, commit_content_paths,
    file_paths_getdata_enter, file_paths_getdata_return,
    fixture_filedrop_getdata_enter, fixture_filedrop_getdata_return, fixture_filedrop_getdata_unwound,
    source_marshal_enter, source_marshal_return, source_unmarshal_enter, source_unmarshal_return,
    shell_refresh_request_enter, shell_refresh_request_return,
    shell_refresh_submit_enter, shell_refresh_submit_return,
    shell_refresh_read_enter, shell_refresh_read_return,
    shell_refresh_apply_enter, shell_refresh_apply_return,
    layout_request_enter, layout_request_return, layout_timer_enter, layout_timer_scope_exit,
    layout_queue_enter, layout_queue_return, layout_write_enter, layout_write_return,
    layout_finish_enter, layout_finish_return,
    unicode_getdata_caller_enter, unicode_getdata_caller_return,
    unicode_extracted_value, text_classified_value, text_encoded_value, text_staged_path,
    content_provider_getdata_enter, content_provider_getdata_return,
    desktop_file_callsite_enter, desktop_file_callsite_return, desktop_file_transfer_origin,
    desktop_file_result_read_binding, desktop_file_result_queue_binding,
    read_content_path_value, commit_content_path_value,
    mixed_provider_getdata_enter, mixed_provider_getdata_return, mixed_provider_getdata_unwound,
    virtual_batch_enter, virtual_batch_return, virtual_batch_unwound,
    virtual_member_enter, virtual_member_return, virtual_member_unwound, virtual_member_path,
    virtual_pipeline_enter, virtual_pipeline_return, virtual_pipeline_path,
    virtual_completion_enter, virtual_completion_return,
    provider_stream_read_enter, provider_stream_read_return,
    virtual_refresh_link_enter, virtual_refresh_link_return,
    virtual_member_file_id, virtual_member_identity_status,
    virtual_result_outputs, virtual_result_source, virtual_result_destination, virtual_result_output_flags,
    shell_refresh_model_enter, shell_refresh_model_return,
    layout_write_input_path, layout_write_input_bytes,
    tracked_copy_item_source, tracked_copy_item_result, tracked_copy_item_path,
    tracked_copy_queue_return, tracked_copy_post_enter, tracked_copy_post_source,
    tracked_copy_post_created, tracked_copy_post_compare_status, tracked_copy_post_compare_order,
    tracked_copy_post_return, virtual_sync_file_scope_enter, virtual_sync_file_scope_return,
    ole_leave_begin, ole_leave_armed, ole_leave_cancel, ole_leave_finalize_enter,
    ole_leave_finalize_deferred, ole_leave_finalize_inactive, ole_leave_finalize_return,
    move_default_target_volume, move_default_source_volume, move_default_selected,
    tracked_move_item_source, tracked_move_item_result, tracked_move_item_path,
    tracked_move_queue_return, tracked_move_post_enter, tracked_move_post_source,
    tracked_move_post_created, tracked_move_post_compare_status, tracked_move_post_compare_order,
    tracked_move_post_return, move_default_destination_path,
    collection_preview_slot, collection_preview_boundary, collection_preview_index_key,
    collection_reorder_enter, collection_reorder_origin, collection_reorder_source_index,
    collection_reorder_before_index, collection_reorder_before_key,
    collection_reorder_after_index, collection_reorder_after_key, collection_reorder_truncated, collection_reorder_collection,
    collection_reorder_return,
    desktop_marquee_enter, desktop_marquee_rect, desktop_marquee_item_state, desktop_marquee_item_key,
    desktop_marquee_widget_state, desktop_marquee_widget_id, desktop_marquee_truncated, desktop_marquee_return,
    native_capture_enter, native_capture_source, native_capture_entry, native_capture_entry_kind, native_capture_key,
    native_capture_truncated, native_capture_return,
    category_classify_enter, category_classify_widget, category_classify_item, category_classify_settings, category_classify_settings_flags,
    category_classify_item_key, category_classify_item_name, category_classify_item_path, category_classify_item_clsid,
    category_classify_shortcut_extension, category_classify_rule, category_classify_rule_enabled,
    category_classify_rule_id, category_classify_rule_label, category_classify_rule_extensions,
    category_classify_rules_truncated, category_classify_namespace, category_classify_extension,
    category_classify_folder, category_classify_type, category_classify_result,
    category_classify_return, category_classify_leave, category_classify_unwound,
    category_desktop_commit_enter, category_desktop_commit_origin,
    category_desktop_remove_enter, category_desktop_remove_index, category_desktop_remove_key,
    category_desktop_remove_truncated, category_desktop_remove_return,
    category_desktop_commit_callback_enter, category_desktop_commit_callback_return,
    category_desktop_commit_return,
    group_slot_callsite_enter, group_slot_callsite_return,
    group_hit_observed, group_hit_container, group_hit_widget, group_hit_value, group_hit_destination,
    group_hit_owner_id, group_hit_active, group_hit_decision,
    group_completion_enter, group_completion_resolved, group_completion_owner_id,
    group_completion_active_before, group_completion_active_after, group_completion_return
};
// Value-only snapshot of the app's actual adapter scope. A missing snapshot is
// unavailable evidence, never a reason to change the transfer operation.
struct AdapterScopeIdentity
{
    std::uint64_t session = 0, call = 0, parent = 0;
    std::uintptr_t adapter = 0;
    DWORD thread = 0;
    std::size_t depth = 0;
    unsigned kind = 0;
    bool observed = false;
};
struct NativeScopeIdentity
{
    std::uint64_t session = 0, event = 0, delivery = 0, forward = 0;
    DWORD ownerThread = 0;
    bool forwarded = false, queuedEvent = false;
};
struct Event
{
    std::uint64_t task = 0;
    std::uintptr_t object = 0;
    DWORD thread = 0;
    Phase phase = Phase::transfer_enter;
    std::int64_t value = 0;
    std::uint64_t extra = 0;
    AdapterScopeIdentity adapterScope;
    std::uint64_t localTask = 0, localWork = 0;
    NativeScopeIdentity nativeScope;
    std::uint64_t currentRevision = 0;
    bool hasBlob = false;
    std::size_t blobOffset = 0, blobSize = 0;
    // Test-host WndProc scope, 0 when unavailable; never owns a window.
    std::uint64_t windowScope = 0;
};
inline constexpr std::size_t Capacity = 8192;
inline constexpr std::size_t BlobCapacity = 1024 * 1024;
inline constexpr std::uint32_t Active = std::uint32_t{1} << 31;
inline constexpr std::uint32_t Stopped = std::uint32_t{1} << 30;
inline constexpr std::uint32_t Configuring = std::uint32_t{1} << 29;
struct Slot { Event event{}; std::atomic_bool ready{false}; };
inline std::array<Slot, Capacity> slots;
inline std::atomic<std::uint32_t> gate{0};
inline std::atomic<std::size_t> nextSlot{0};
inline std::array<std::byte, BlobCapacity> blobs;
inline std::atomic<std::size_t> nextBlob{0};
inline std::atomic<std::uint64_t> nextTask{0};
inline std::atomic_bool claimed{false}, started{false}, lost{false}, budgetExceeded{false};
inline std::atomic_bool stopped{false};
inline std::atomic<std::size_t> readerLeases{0};
inline constexpr std::size_t ReadersClosed = std::size_t{1} << (std::numeric_limits<std::size_t>::digits - 1);
inline thread_local std::uint64_t currentTask = 0;
inline thread_local std::uint64_t currentWork = 0;
inline thread_local std::uint64_t currentWindowScope = 0;

// Optional staging handles are separate from the value log. They share read,
// write and delete with production and therefore do not pin a pathname or
// establish which object Shell subsequently opens. After Stop and writer drain,
// readers can inspect even a truncated log, then release only when the fixture
// has finished with those handles; log failure remains independently visible.
struct StagingHandleObservation
{
    HANDLE handle = INVALID_HANDLE_VALUE; // borrowed until release
    std::uint64_t batch = 0, member = 0;
    std::uintptr_t source = 0;
    DWORD thread = 0;
};
struct StagingHandleSlot
{
    StagingHandleObservation value{};
    std::atomic_bool ready{false};
};
inline constexpr std::size_t StagingHandleCapacity = 256;
inline std::array<StagingHandleSlot, StagingHandleCapacity> stagingHandles;
inline std::atomic<std::size_t> nextStagingHandle{0};

struct CaptureStorage final
{
    Slot* events = nullptr;
    std::byte* data = nullptr;
    StagingHandleSlot* handles = nullptr;
    std::size_t eventCapacity = 0, blobCapacity = 0, handleCapacity = 0, committedBytes = 0;
};
inline CaptureStorage defaultStorage{slots.data(), blobs.data(), stagingHandles.data(), Capacity, BlobCapacity, StagingHandleCapacity, 0};
inline CaptureStorage configuredStorage{};
inline std::atomic<CaptureStorage*> captureStorage{&defaultStorage};
inline std::atomic_bool configurationAttempted{false}, configurationFailed{false};

struct CaptureBudget final
{
    std::size_t events = 0, blobBytes = 0, stagingHandles = 0, totalBytes = 0;
};
struct CaptureConfiguration final
{
    bool configured = false, resourceUnavailable = false;
    std::size_t committedBytes = 0;
};
inline std::size_t EventLimit() noexcept { return captureStorage.load(std::memory_order_acquire)->eventCapacity; }
inline std::size_t BlobLimit() noexcept { return captureStorage.load(std::memory_order_acquire)->blobCapacity; }

// One opt-in attempt before the one process capture. The single VM allocation
// is page-rounded and bounded by totalBytes; on success it is intentionally
// retained until process exit. This internal capability does not establish an
// isolated launch, evidence-size sufficiency or production quiescence.
inline CaptureConfiguration ConfigureBudgetOnce(CaptureBudget budget) noexcept
{
    CaptureConfiguration out;
    const DWORD saved = GetLastError();
    struct ErrorGuard { DWORD value; ~ErrorGuard() { SetLastError(value); } } restore{saved};
    std::uint32_t idle = 0;
    if (claimed.load(std::memory_order_acquire) || !gate.compare_exchange_strong(idle, Configuring, std::memory_order_acq_rel)) return out;
    struct GateGuard { ~GateGuard() { gate.fetch_and(~Configuring, std::memory_order_release); } } gateGuard;
    bool first = false;
    if (!configurationAttempted.compare_exchange_strong(first, true, std::memory_order_acq_rel)) return out;
    const auto fail = [&]() noexcept {
        configurationFailed.store(true, std::memory_order_release);
        gate.fetch_or(Stopped, std::memory_order_acq_rel);
        out.resourceUnavailable = true;
    };
    if (claimed.load(std::memory_order_acquire) || !budget.events || !budget.totalBytes ||
        budget.events > (std::numeric_limits<std::size_t>::max)() / sizeof(Slot) ||
        budget.stagingHandles > (std::numeric_limits<std::size_t>::max)() / sizeof(StagingHandleSlot))
    { fail(); return out; }
    const auto eventsBytes = budget.events * sizeof(Slot);
    if (budget.blobBytes > (std::numeric_limits<std::size_t>::max)() - eventsBytes) { fail(); return out; }
    const auto endBlob = eventsBytes + budget.blobBytes;
    constexpr auto alignment = alignof(StagingHandleSlot);
    if (endBlob > (std::numeric_limits<std::size_t>::max)() - (alignment - 1)) { fail(); return out; }
    const auto handlesOffset = (endBlob + alignment - 1) & ~(alignment - 1);
    const auto handleBytes = budget.stagingHandles * sizeof(StagingHandleSlot);
    if (handleBytes > (std::numeric_limits<std::size_t>::max)() - handlesOffset) { fail(); return out; }
    const auto used = handlesOffset + handleBytes;
    SYSTEM_INFO info{}; GetSystemInfo(&info);
    const auto page = static_cast<std::size_t>(info.dwPageSize);
    if (!page || used > (std::numeric_limits<std::size_t>::max)() - (page - 1)) { fail(); return out; }
    const auto committed = ((used + page - 1) / page) * page;
    if (!committed || committed > budget.totalBytes) { fail(); return out; }
    auto* block = static_cast<std::byte*>(VirtualAlloc(nullptr, committed, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!block) { fail(); return out; }
    if (claimed.load(std::memory_order_acquire) || gate.load(std::memory_order_acquire) != Configuring)
    { VirtualFree(block, 0, MEM_RELEASE); fail(); return out; }
    static_assert(std::is_nothrow_default_constructible_v<Slot> && std::is_nothrow_default_constructible_v<StagingHandleSlot> &&
        std::is_trivially_destructible_v<Slot> && std::is_trivially_destructible_v<StagingHandleSlot>);
    auto* events = reinterpret_cast<Slot*>(block);
    auto* handles = reinterpret_cast<StagingHandleSlot*>(block + handlesOffset);
    for (std::size_t i = 0; i < budget.events; ++i) new (events + i) Slot{};
    for (std::size_t i = 0; i < budget.stagingHandles; ++i) new (handles + i) StagingHandleSlot{};
    if (claimed.load(std::memory_order_acquire) || gate.load(std::memory_order_acquire) != Configuring)
    { VirtualFree(block, 0, MEM_RELEASE); fail(); return out; }
    configuredStorage = {events, block + eventsBytes, handles, budget.events, budget.blobBytes, budget.stagingHandles, committed};
    captureStorage.store(&configuredStorage, std::memory_order_release);
    out.configured = true; out.committedBytes = committed;
    return out;
}

inline std::int64_t ObservedSize(std::size_t value) noexcept
{
    return static_cast<std::uintmax_t>(value) > static_cast<std::uintmax_t>((std::numeric_limits<std::int64_t>::max)())
        ? -1 : static_cast<std::int64_t>(value);
}

// One capture per process; enable only before starting a quiescent isolated
// fixture. Stop never waits. The reader must establish production quiescence
// separately and call Read only after Stop and all active writers have exited.
inline bool Enable() noexcept
{
    if (!gate.is_lock_free() || !nextSlot.is_lock_free() || !nextBlob.is_lock_free() || !nextTask.is_lock_free() ||
        !claimed.is_lock_free() || !started.is_lock_free() || !stopped.is_lock_free() || !lost.is_lock_free() || !budgetExceeded.is_lock_free() ||
        !configurationAttempted.is_lock_free() || !configurationFailed.is_lock_free() || !captureStorage.is_lock_free() ||
        !readerLeases.is_lock_free() || !slots.front().ready.is_lock_free()) return false;
    bool expected = false;
    if (!claimed.compare_exchange_strong(expected, true)) return false;
    if (configurationFailed.load(std::memory_order_acquire)) return false;
    std::uint32_t idle = 0;
    if (!gate.compare_exchange_strong(idle, Active, std::memory_order_acq_rel, std::memory_order_relaxed)) return false;
    started.store(true, std::memory_order_release);
    return true;
}
inline void Stop() noexcept
{
    if (!claimed.load(std::memory_order_acquire)) return;
    auto value = gate.load(std::memory_order_acquire);
    while (!(value & Stopped) && !gate.compare_exchange_weak(value, (value & ~Active) | Stopped,
        std::memory_order_acq_rel, std::memory_order_acquire)) {}
    stopped.store(true, std::memory_order_release);
}
inline bool Enabled() noexcept { return (gate.load(std::memory_order_acquire) & Active) != 0; }
inline bool Complete() noexcept
{
    return started.load(std::memory_order_acquire) && stopped.load(std::memory_order_acquire) && gate.load(std::memory_order_acquire) == Stopped &&
        !lost.load(std::memory_order_acquire);
}
// Read-only capability/usage snapshot. Counts sampled while writers are
// active are non-atomic as a group. A drained recorder says nothing about
// producer quiescence or the lifetime of pointers retained by readers.
struct BudgetObservation
{
    // committedBytes is the optional configured VM allocation only. Static
    // fallback arrays, allocator/runtime overhead and other recorders are not
    // counted, so this is never a process-wide memory bound.
    std::size_t eventCapacity = Capacity, blobCapacity = BlobCapacity, stagingHandleCapacity = StagingHandleCapacity, committedBytes = 0;
    std::size_t recordedEvents = 0, recordedBlobBytes = 0, recordedStagingHandles = 0;
    bool captureClaimed = false, writersDrained = false, recordsLost = false, capacityExceeded = false;
};
inline BudgetObservation ObserveBudget() noexcept
{
    BudgetObservation out;
    const auto* storage = captureStorage.load(std::memory_order_acquire);
    out.eventCapacity = storage->eventCapacity; out.blobCapacity = storage->blobCapacity;
    out.stagingHandleCapacity = storage->handleCapacity; out.committedBytes = storage->committedBytes;
    out.captureClaimed = claimed.load(std::memory_order_acquire);
    out.writersDrained = started.load(std::memory_order_acquire) && stopped.load(std::memory_order_acquire) && gate.load(std::memory_order_acquire) == Stopped;
    out.recordedEvents = nextSlot.load(std::memory_order_acquire);
    out.recordedBlobBytes = nextBlob.load(std::memory_order_acquire);
    out.recordedStagingHandles = nextStagingHandle.load(std::memory_order_acquire);
    out.recordsLost = lost.load(std::memory_order_acquire);
    out.capacityExceeded = budgetExceeded.load(std::memory_order_acquire);
    return out;
}
inline std::size_t Count() noexcept { return nextSlot.load(std::memory_order_acquire); }
// Explicit lease for copying a stopped, drained capture. It exposes no storage
// pointers, allocates nothing and is valid only for this process's one capture.
// The present static backing arrays remain alive for the process lifetime;
// this counter does not establish producer quiescence or permit reallocation.
class ReadSession final
{
public:
    ReadSession() = default;
    ReadSession(const ReadSession&) = delete;
    ReadSession& operator=(const ReadSession&) = delete;
    ReadSession(ReadSession&&) = delete;
    ReadSession& operator=(ReadSession&&) = delete;
    ~ReadSession() noexcept { if (open_) readerLeases.fetch_sub(1, std::memory_order_release); }

    // retained=true permits individually published values after log loss; it
    // never certifies a complete observation. Failure is safe to retry after
    // outstanding writers leave, without reopening or resetting the capture.
    bool Open(bool retained = false) noexcept
    {
        if (open_ || !started.load(std::memory_order_acquire) || !stopped.load(std::memory_order_acquire) ||
            gate.load(std::memory_order_acquire) != Stopped || (!retained && lost.load(std::memory_order_acquire))) return false;
        auto readers = readerLeases.load(std::memory_order_relaxed);
        do { if (readers & ReadersClosed || readers == ReadersClosed - 1) return false; }
        while (!readerLeases.compare_exchange_weak(readers, readers + 1, std::memory_order_acquire, std::memory_order_relaxed));
        if (!stopped.load(std::memory_order_acquire) || gate.load(std::memory_order_acquire) != Stopped ||
            (!retained && lost.load(std::memory_order_acquire)))
        { readerLeases.fetch_sub(1, std::memory_order_release); return false; }
        count_ = nextSlot.load(std::memory_order_acquire);
        blobBytes_ = nextBlob.load(std::memory_order_acquire);
        stagingCount_ = nextStagingHandle.load(std::memory_order_acquire);
        storage_ = captureStorage.load(std::memory_order_acquire);
        open_ = true;
        return true;
    }
    std::size_t Count() const noexcept { return open_ ? count_ : 0; }
    bool ReadEvent(std::size_t index, Event& result) const noexcept
    {
        if (!open_ || !storage_ || index >= count_ || index >= storage_->eventCapacity || !storage_->events[index].ready.load(std::memory_order_acquire)) return false;
        result = storage_->events[index].event; return true;
    }
    // On insufficient capacity, size reports the actual required byte count.
    // The event is re-read by index, so caller-supplied offsets are never used.
    bool CopyBlob(std::size_t index, std::byte* destination, std::size_t capacity, std::size_t& size) const noexcept
    {
        size = 0; Event event{};
        if (!ReadEvent(index, event) || !event.hasBlob || event.blobOffset > storage_->blobCapacity ||
            event.blobSize > storage_->blobCapacity - event.blobOffset || event.blobOffset + event.blobSize > blobBytes_) return false;
        size = event.blobSize;
        if (capacity < size || (size && !destination)) return false;
        if (size) std::memcpy(destination, storage_->data + event.blobOffset, size);
        return true;
    }
    std::size_t StagingCount() const noexcept { return open_ ? stagingCount_ : 0; }
    // The returned handle is independently owned by the caller and must be
    // closed. Duplicates share a file pointer; concurrent reads need separate
    // synchronization or an independently reopened observation handle.
    bool DuplicateStagingHandle(std::size_t index, StagingHandleObservation& out) const noexcept
    {
        out = {};
        if (!open_ || !storage_ || index >= stagingCount_ || index >= storage_->handleCapacity ||
            !storage_->handles[index].ready.load(std::memory_order_acquire)) return false;
        const DWORD saved = GetLastError();
        const auto value = storage_->handles[index].value; HANDLE duplicate = INVALID_HANDLE_VALUE;
        const bool ok = value.handle != INVALID_HANDLE_VALUE && DuplicateHandle(GetCurrentProcess(), value.handle,
            GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS) != FALSE;
        if (ok) { out = value; out.handle = duplicate; }
        SetLastError(saved);
        return ok;
    }
private:
    std::size_t count_ = 0, blobBytes_ = 0, stagingCount_ = 0;
    const CaptureStorage* storage_ = nullptr;
    bool open_ = false;
};
inline bool Acquire() noexcept
{
    auto value = gate.load(std::memory_order_acquire);
    while (value & Active)
    {
        if ((value & (Configuring - 1)) == Configuring - 1) { budgetExceeded.store(true, std::memory_order_release); lost.store(true, std::memory_order_release); return false; }
        if (gate.compare_exchange_weak(value, value + 1, std::memory_order_acquire, std::memory_order_relaxed)) return true;
    }
    return false;
}

// Opens only an already materialized path. Capture failures affect the
// diagnostic log, never materialization, the pipeline or ownership of its
// path. The live handle intentionally permits normal rename/delete.
inline void CaptureStagingHandle(std::uint64_t batch, std::uint64_t member,
    std::uintptr_t source, const wchar_t* path) noexcept
{
    if (!batch || !member || !path || !Acquire()) return;
    const DWORD saved = GetLastError();
    auto* storage = captureStorage.load(std::memory_order_acquire);
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) lost.store(true, std::memory_order_release);
    else
    {
        auto index = nextStagingHandle.load(std::memory_order_relaxed);
        while (index < storage->handleCapacity && !nextStagingHandle.compare_exchange_weak(
            index, index + 1, std::memory_order_relaxed)) {}
        if (index >= storage->handleCapacity)
        { CloseHandle(file); budgetExceeded.store(true, std::memory_order_release); lost.store(true, std::memory_order_release); }
        else
        {
            storage->handles[index].value = {file, batch, member, source, GetCurrentThreadId()};
            storage->handles[index].ready.store(true, std::memory_order_release);
        }
    }
    SetLastError(saved);
    gate.fetch_sub(1, std::memory_order_release);
}
inline std::size_t StagingHandleCount() noexcept { return nextStagingHandle.load(std::memory_order_acquire); }
// Call once after all fixture readers are finished; never during inspection.
// Process teardown is the fallback if an exceptional path cannot call this.
inline void ReleaseStagingHandles() noexcept
{
    if (!claimed.load(std::memory_order_acquire) || !stopped.load(std::memory_order_acquire) || gate.load(std::memory_order_acquire) != Stopped) return;
    std::size_t expectedReaders = 0;
    if (!readerLeases.compare_exchange_strong(expectedReaders, ReadersClosed, std::memory_order_acq_rel)) return;
    const DWORD saved = GetLastError();
    auto* storage = captureStorage.load(std::memory_order_acquire);
    const auto observed = StagingHandleCount();
    const auto count = observed < storage->handleCapacity ? observed : storage->handleCapacity;
    for (std::size_t i = 0; i < count; ++i)
        if (storage->handles[i].ready.exchange(false, std::memory_order_acq_rel))
        { CloseHandle(storage->handles[i].value.handle); storage->handles[i].value.handle = INVALID_HANDLE_VALUE; }
    SetLastError(saved);
}
// Task zero is reserved for observations that cannot be attributed to a
// transfer, including provider callbacks on a different COM apartment.
inline void RecordAny(std::uint64_t task, Phase phase, std::uintptr_t object = 0,
    std::int64_t value = 0, std::uint64_t extra = 0,
    AdapterScopeIdentity adapterScope = {}, NativeScopeIdentity nativeScope = {},
    std::uint64_t currentRevision = 0) noexcept
{
    if (!Acquire()) return;
    const DWORD savedError = GetLastError();
    auto* storage = captureStorage.load(std::memory_order_acquire);
    auto index = nextSlot.load(std::memory_order_relaxed);
    while (index < storage->eventCapacity && !nextSlot.compare_exchange_weak(index, index + 1, std::memory_order_relaxed)) {}
    if (index >= storage->eventCapacity) { budgetExceeded.store(true, std::memory_order_release); lost.store(true, std::memory_order_release); }
    else
    {
        storage->events[index].event = {task, object, GetCurrentThreadId(), phase, value, extra, adapterScope, currentTask, currentWork,
            nativeScope, currentRevision, false, 0, 0, currentWindowScope};
        storage->events[index].ready.store(true, std::memory_order_release);
    }
    SetLastError(savedError);
    gate.fetch_sub(1, std::memory_order_release);
}
// Copies only already-available product values into a process-wide fixed
// budget. Overflow/loss affects observation only and never product results.
inline void RecordBlob(Phase phase, std::uintptr_t object, std::uint64_t call,
    AdapterScopeIdentity scope, const void* bytes, std::size_t size) noexcept
{
    if (!Acquire()) return;
    const DWORD saved = GetLastError();
    auto* storage = captureStorage.load(std::memory_order_acquire);
    if ((size && !bytes) || size > storage->blobCapacity)
    { if (size > storage->blobCapacity) budgetExceeded.store(true, std::memory_order_release); lost.store(true, std::memory_order_release); SetLastError(saved); gate.fetch_sub(1, std::memory_order_release); return; }
    auto offset = nextBlob.load(std::memory_order_relaxed);
    while (offset <= storage->blobCapacity && size <= storage->blobCapacity - offset &&
        !nextBlob.compare_exchange_weak(offset, offset + size, std::memory_order_relaxed)) {}
    if (offset > storage->blobCapacity || size > storage->blobCapacity - offset)
    { budgetExceeded.store(true, std::memory_order_release); lost.store(true, std::memory_order_release); SetLastError(saved); gate.fetch_sub(1, std::memory_order_release); return; }
    auto index = nextSlot.load(std::memory_order_relaxed);
    while (index < storage->eventCapacity && !nextSlot.compare_exchange_weak(index, index + 1, std::memory_order_relaxed)) {}
    if (index >= storage->eventCapacity) { budgetExceeded.store(true, std::memory_order_release); lost.store(true, std::memory_order_release); }
    else
    {
        if (size) std::memcpy(storage->data + offset, bytes, size);
        storage->events[index].event = {0, object, GetCurrentThreadId(), phase, 0, call, scope, currentTask, currentWork,
            {}, 0, true, offset, size, currentWindowScope};
        storage->events[index].ready.store(true, std::memory_order_release);
    }
    SetLastError(saved);
    gate.fetch_sub(1, std::memory_order_release);
}
// Record an already-existing value for one explicitly bound task and member
// index. It uses the same fixed byte budget as other observations, never
// reads a source object, and loss affects diagnostics only.
inline void RecordTaskBlob(std::uint64_t task, Phase phase, std::uintptr_t object,
    std::uint64_t index, const void* bytes, std::size_t size) noexcept
{
    if (!task || !Acquire()) return;
    const DWORD saved = GetLastError();
    auto* storage = captureStorage.load(std::memory_order_acquire);
    if ((size && !bytes) || size > storage->blobCapacity)
    { if (size > storage->blobCapacity) budgetExceeded.store(true, std::memory_order_release); lost.store(true, std::memory_order_release); SetLastError(saved); gate.fetch_sub(1, std::memory_order_release); return; }
    auto offset = nextBlob.load(std::memory_order_relaxed);
    while (offset <= storage->blobCapacity && size <= storage->blobCapacity - offset &&
        !nextBlob.compare_exchange_weak(offset, offset + size, std::memory_order_relaxed)) {}
    if (offset > storage->blobCapacity || size > storage->blobCapacity - offset)
    { budgetExceeded.store(true, std::memory_order_release); lost.store(true, std::memory_order_release); SetLastError(saved); gate.fetch_sub(1, std::memory_order_release); return; }
    auto slot = nextSlot.load(std::memory_order_relaxed);
    while (slot < storage->eventCapacity && !nextSlot.compare_exchange_weak(slot, slot + 1, std::memory_order_relaxed)) {}
    if (slot >= storage->eventCapacity) { budgetExceeded.store(true, std::memory_order_release); lost.store(true, std::memory_order_release); }
    else
    {
        if (size) std::memcpy(storage->data + offset, bytes, size);
        storage->events[slot].event = {task, object, GetCurrentThreadId(), phase, 0, index, {}, currentTask, currentWork,
            {}, 0, true, offset, size, currentWindowScope};
        storage->events[slot].ready.store(true, std::memory_order_release);
    }
    SetLastError(saved);
    gate.fetch_sub(1, std::memory_order_release);
}
inline void Record(std::uint64_t task, Phase phase, std::uintptr_t object = 0,
    std::int64_t value = 0, std::uint64_t extra = 0,
    AdapterScopeIdentity adapterScope = {}) noexcept
{
    if (task) RecordAny(task, phase, object, value, extra, adapterScope);
}
inline std::uint64_t NewTask() noexcept
{
    if (!Acquire()) return 0;
    auto value = nextTask.load(std::memory_order_relaxed);
    while (value != (std::numeric_limits<std::uint64_t>::max)() &&
           !nextTask.compare_exchange_weak(value, value + 1, std::memory_order_relaxed)) {}
    if (value == (std::numeric_limits<std::uint64_t>::max)()) lost.store(true, std::memory_order_release);
    gate.fetch_sub(1, std::memory_order_release);
    return value == (std::numeric_limits<std::uint64_t>::max)() ? 0 : value + 1;
}
// The fixture owns this scope around one actual WndProc invocation. A nested
// WndProc receives a different id and restores its parent. A zero id is never
// evidence. The bounded recorder's lost flag makes mismatched nesting and ID
// exhaustion unavailable without changing the window's message or result.
class WindowScope final
{
public:
    WindowScope() noexcept
    {
        const DWORD saved = GetLastError();
        if (Enabled())
        {
            id_ = NewTask();
            if (id_) { previous_ = currentWindowScope; currentWindowScope = id_; }
        }
        SetLastError(saved);
    }
    ~WindowScope() noexcept
    {
        const DWORD saved = GetLastError();
        if (id_)
        {
            if (currentWindowScope != id_) lost.store(true, std::memory_order_release);
            currentWindowScope = previous_;
        }
        SetLastError(saved);
    }
    WindowScope(const WindowScope&) = delete;
    WindowScope& operator=(const WindowScope&) = delete;
    std::uint64_t Id() const noexcept { return id_; }
private:
    std::uint64_t id_ = 0, previous_ = 0;
};
struct Scope final
{
    explicit Scope(std::uint64_t task, std::uint64_t work = 0) noexcept
        : previous(currentTask), previousWork(currentWork)
    {
        if (task && previous) Record(task, Phase::ambiguous_nested_scope, 0, 0, previous);
        currentTask = task;
        currentWork = work;
    }
    ~Scope() { currentTask = previous; currentWork = previousWork; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    std::uint64_t previous;
    std::uint64_t previousWork;
};
}
