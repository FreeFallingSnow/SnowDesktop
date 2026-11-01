#pragma once

// This is an isolated initial-model preparation and a value-only observation.
// It never assigns the final selection or issues pointer events.
namespace behavior
{
// Value snapshot only. Receipt sealing and the real preparation-session
// observer are separate capabilities and are not supplied by this type.
struct V4NativeMultiSelectionEvidence
{
    std::string candidateId;
    std::optional<Access::NativeMultiDesktopSelection> before, after;
    // Missing means not observed, including the distinction between an empty
    // selection/anchor and an unavailable observation.
    std::optional<std::wstring> beforeAnchor, afterAnchor;
    std::optional<std::vector<std::wstring>> beforeSelectedOutside, afterSelectedOutside;
    bool beforeFilesRevalidated = false, afterFilesRevalidated = false;
    bool memberKindBound = false;
    bool resourceUnavailable = false, observationUnavailable = true;
    std::vector<std::string> unresolved{
        "native_multi_preparation_session_receipt_unimplemented",
        "native_multi_anchor_and_outside_selection_observation_unimplemented",
        "native_multi_session_to_drag_handoff_unimplemented"};
};
// These snapshots are local observations of the second gesture, not a
// business verdict. Actual graph attribution and persistence remain separate.
struct V4NativeMultiDragEvidence
{
    std::string candidateId;
    std::optional<Access::NativeMultiDesktopSelection> beforeDrag, afterDrag, afterReload;
    std::optional<V4NativeDesktopLayoutSnapshot> beforeReloadLayout;
    bool beforeFilesRevalidated = false, afterFilesRevalidated = false, reloadFilesRevalidated = false;
    bool observationUnavailable = true, resourceUnavailable = false;
    std::vector<std::string> unresolved{"native_multi_drag_capture_and_route_unverified",
        "native_multi_drag_release_to_model_and_layout_save_unverified",
        "native_multi_drag_reload_and_late_work_causality_unverified"};
};
struct V4NativeMultiDesktopBaseline
{
    bool preparationAttempted = false, captured = false, resourceUnavailable = false;
    std::string candidateId;
    std::vector<fs::path> files;
    std::vector<std::vector<std::byte>> expectedBytes;
    std::optional<Access::NativeMultiDesktopSelection> initial;
    std::vector<std::string> unresolved{"native_multi_file_to_logical_member_identity_unimplemented",
        "native_multi_selection_gesture_and_same_host_drag_unimplemented",
        "native_multi_physical_directory_completeness_and_snapshot_atomicity_unverified"};
};
inline V4NativeMultiDesktopBaseline PrepareV4NativeMultiDesktopBaseline(
    const drag_global::event_graph::V4FamilyIdentityContext& identity,
    const drag_global::V4FamilyValidatedContext& verified, OwnedProfileDesktopRoot& root,
    const std::shared_ptr<drag_global::V4IndependentFilePayload>& payload)
{
    using namespace drag_global;
    V4NativeMultiDesktopBaseline out;
    try
    {
        if (identity.Status() != V4FamilyStatus::schema_only || verified.Status() != V4CoordinateStatus::coordinate_ready ||
            verified.TaskBActive() || identity.FamilyBinding() != verified.FamilyBinding() || identity.Axes().size() != verified.Axes().size() ||
            !root.StillOwned() || !root.profile || !root.profile->app || !payload || payload->status != V4IndependentFileStatus::input_prepared ||
            payload->files.size() != 2 || !OwnedProfileDesktopRoot::SameId(payload->rootId, root.desktopId))
        { out.unresolved.emplace_back("native_multi_context_or_desktop_root_unavailable"); return out; }
        for (std::size_t i = 0; i < identity.Axes().size(); ++i)
        {
            const auto& a = identity.Axes()[i]; const auto& b = verified.Axes()[i];
            if (a.id != b.id || a.role != b.role || a.values != b.values || a.label != b.label)
            { out.unresolved.emplace_back("native_multi_schema_mismatch"); return out; }
        }
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        if (raw.resourceUnavailable) out.resourceUnavailable = true;
        const auto value = [&](std::string_view id) { return Value(verified.Axes(), verified.Coordinate(), id); };
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty() || raw.id != payload->candidateId ||
            value("T01") != "internal_native" || value("source.E01") != "free_desktop" || value("source.E02") != "none" ||
            value("V4.source.kind") != "file_only" || value("V4.source.file_count") != "two" ||
            value("P02.source_actual_count") != "two" || value("X03.task_count") != "one")
        { out.unresolved.emplace_back("native_multi_candidate_unavailable"); return out; }
        // This fixture prepares only an initially unselected desktop. Require
        // explicit, distinct physical-to-logical references before changing
        // its initial model; unknown or different initial selections remain
        // pending rather than being silently overwritten by the fixture.
        const auto firstRelation = value("V4.file.member0_logical_relation");
        const auto secondRelation = value("V4.file.member1_logical_relation");
        if (!((firstRelation == "member_0" && secondRelation == "member_1") ||
              (firstRelation == "member_1" && secondRelation == "member_0")) ||
            value("V4.selection_prepare.baseline_member_0") != "absent" ||
            value("V4.selection_prepare.baseline_member_1") != "absent" ||
            value("V4.selection_prepare.baseline_outside_members") != "none" ||
            value("V4.selection_prepare.baseline_anchor") != "none")
        { out.unresolved.emplace_back("native_multi_initial_selection_candidate_unimplemented"); return out; }
        for (const auto& axis : verified.Axes())
            if (std::string_view(axis.id).starts_with("V4.selection_prepare.baseline_outside_") &&
                axis.id != "V4.selection_prepare.baseline_outside_members")
            { out.unresolved.emplace_back("native_multi_outside_identity_and_alias_unimplemented"); return out; }
        FILE_ID_INFO rootId{};
        if (!payload->rootHandle || payload->rootHandle == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandleEx(payload->rootHandle, FileIdInfo, &rootId, sizeof(rootId)) ||
            !OwnedProfileDesktopRoot::SameId(rootId, root.desktopId))
        { out.unresolved.emplace_back("native_multi_root_handle_unavailable"); return out; }
        constexpr char digits[] = "0123456789abcdef";
        const auto appendByte = [&](std::string& text, unsigned byte) { text.push_back(digits[byte >> 4]); text.push_back(digits[byte & 15]); };
        std::vector<std::unique_ptr<ObservedPhysicalFile>> held;
        for (std::size_t i = 0; i < payload->files.size(); ++i)
        {
            const auto& file = payload->files[i]; const auto base = std::string("V4.file.member") + std::to_string(i);
            if (!file.created || !file.identityObserved || !file.bytesObserved || value(base + "_kind") != "file" ||
                value(base + "_identity") != "unique")
            { out.unresolved.emplace_back("native_multi_file_input_unavailable"); return out; }
            std::string path = "utf16le_hex_", bytes = "hex_";
            static_assert(sizeof(wchar_t) == 2, "Windows UTF-16 code units required");
            for (const auto unit : file.path.native()) { const auto n = static_cast<std::uint16_t>(unit); appendByte(path, n & 255); appendByte(path, n >> 8); }
            for (const auto byte : file.bytes) appendByte(bytes, std::to_integer<unsigned>(byte));
            if (value("V4.file_path.index_" + std::to_string(i) + ".utf16le") != path ||
                value("V4.file_content.index_" + std::to_string(i) + ".bytes") != bytes)
            { out.unresolved.emplace_back("native_multi_candidate_file_value_mismatch"); return out; }
            struct ParentGuard { HANDLE value = INVALID_HANDLE_VALUE; ~ParentGuard() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); } } parent;
            parent.value = CreateFileW(file.path.parent_path().c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            FILE_ID_INFO parentId{}; FILE_ATTRIBUTE_TAG_INFO tag{};
            if (parent.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandleEx(parent.value, FileIdInfo, &parentId, sizeof(parentId)) ||
                !GetFileInformationByHandleEx(parent.value, FileAttributeTagInfo, &tag, sizeof(tag)) ||
                !(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                !OwnedProfileDesktopRoot::SameId(parentId, root.desktopId) || !root.StillOwned())
            { out.unresolved.emplace_back("native_multi_file_parent_unavailable"); return out; }
            auto observed = std::make_unique<ObservedPhysicalFile>(file.path);
            if (!OwnedProfileDesktopRoot::SameId(observed->Identity(), file.identity) || !observed->PathStillNamesObject(file.path) ||
                !observed->HasBytes(file.bytes) || (i && observed->SameObject(*held.front())))
            { out.unresolved.emplace_back("native_multi_file_identity_or_bytes_unavailable"); return out; }
            out.files.push_back(file.path); out.expectedBytes.push_back(file.bytes); held.push_back(std::move(observed));
        }
        auto& profile = *root.profile;
        if (Access::asyncOwner != profile.app || Access::productionDispatch != 1 || !Access::nativePointerDispatch ||
            !Access::fixtureControlWindow || !Access::messageTrace || Access::messageTrace->LostOrCrossThread())
        { out.unresolved.emplace_back("native_multi_host_unavailable"); return out; }
        Wait([&] { return Access::NativeDragIdle(*profile.app) && Access::ContainerWorkIdle(*profile.app) && Access::LayoutSaveIdle(*profile.app); });
        out.candidateId = raw.id; out.preparationAttempted = true;
        Access::PrepareNativeMultiDesktop(*profile.app, out.files);
        out.initial = Access::InspectNativeMultiDesktopSelection(*profile.app, out.files);
        if (!root.StillOwned() || !out.initial || out.initial->host != reinterpret_cast<std::uintptr_t>(profile.app) ||
            out.initial->ownerThread != GetCurrentThreadId() || out.initial->actualItemCount != 2 || out.initial->widgetCount != 0 ||
            out.initial->members.size() != 2 || !out.initial->extraItemKeys.empty())
        { out.unresolved.emplace_back("native_multi_initial_model_unavailable"); return out; }
        for (std::size_t i = 0; i < out.initial->members.size(); ++i)
            if (out.initial->members[i].selected || !OwnedProfileDesktopRoot::SameId(out.initial->members[i].fileId, held[i]->Identity()) ||
                !held[i]->PathStillNamesObject(out.files[i]) || !held[i]->HasBytes(out.expectedBytes[i]))
            { out.unresolved.emplace_back("native_multi_initial_selection_or_file_changed"); return out; }
        out.captured = true;
        out.unresolved.emplace_back("native_multi_baseline_non_atomic_and_file_lifetime_not_bound_to_execution");
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; }
    catch (const std::length_error&) { out.resourceUnavailable = true; }
    catch (...) { out.unresolved.emplace_back("native_multi_initial_observation_unavailable"); }
    return out;
}

// The private key can seal only actual value snapshots. In particular this
// observer never supplies a selected set to the product or synthesizes a
// preparation receipt. The complete cross-surface selection and two-session
// executor remain independent preconditions.
struct V4NativeMultiSelectionObservation
{
private:
    friend struct V4NativeMultiDragObservation;
    std::shared_ptr<OwnedProfileDesktopRoot> root_;
    std::shared_ptr<drag_global::V4IndependentFilePayload> payload_;
    std::shared_ptr<const drag_global::event_graph::V4FamilyIdentityContext> identityContext_;
    std::shared_ptr<const drag_global::V4FamilyValidatedContext> verified_;
    V4NativeMultiDesktopBaseline baseline_;
    std::vector<std::unique_ptr<ObservedPhysicalFile>> heldFiles_;
    V4NativeMultiSelectionEvidence evidence_;
    std::shared_ptr<const drag_global::ole_driver::V4NativeSelectionObserverIdentity> identity_{
        new drag_global::ole_driver::V4NativeSelectionObserverIdentity};
    bool bound_ = false, captureStarted_ = false;

    template <typename T> static T& Required(const std::shared_ptr<T>& value)
    {
        RequireObservation(static_cast<bool>(value), "native multi selection owner unavailable");
        return *value;
    }

    static bool SameSnapshot(const Access::NativeMultiDesktopSelection& a,
        const Access::NativeMultiDesktopSelection& b)
    {
        if (a.host != b.host || a.ownerThread != b.ownerThread || a.layoutRevision != b.layoutRevision ||
            a.selectionRevision != b.selectionRevision || a.actualItemCount != b.actualItemCount ||
            a.widgetCount != b.widgetCount || a.anchorScope != b.anchorScope || a.anchorKey != b.anchorKey ||
            a.extraItemKeys != b.extraItemKeys || a.extraSelectedItemKeys != b.extraSelectedItemKeys ||
            a.selectedDockReferences != b.selectedDockReferences || a.selectedRunningKeys != b.selectedRunningKeys ||
            a.selectedWidgetIds != b.selectedWidgetIds || a.selectedWidgetFolderEntries != b.selectedWidgetFolderEntries ||
            a.otherSurfaceReasons != b.otherSurfaceReasons ||
            a.otherSurfacesComplete != b.otherSurfacesComplete || a.members.size() != b.members.size()) return false;
        for (std::size_t i = 0; i < a.members.size(); ++i)
            if (a.members[i].path != b.members[i].path || a.members[i].key != b.members[i].key ||
                a.members[i].typeName != b.members[i].typeName || a.members[i].desktopIconClsid != b.members[i].desktopIconClsid ||
                a.members[i].fileAttributes != b.members[i].fileAttributes || a.members[i].isShortcut != b.members[i].isShortcut ||
                a.members[i].isApplicationShortcut != b.members[i].isApplicationShortcut ||
                a.members[i].selected != b.members[i].selected || !EqualRect(&a.members[i].bounds, &b.members[i].bounds) ||
                a.members[i].cell.pageId != b.members[i].cell.pageId ||
                a.members[i].cell.column != b.members[i].cell.column || a.members[i].cell.row != b.members[i].cell.row ||
                !OwnedProfileDesktopRoot::SameId(a.members[i].fileId, b.members[i].fileId)) return false;
        return true;
    }
    Access::NativeMultiDesktopSelection Read() const
    {
        RequireObservation(root_ && root_->StillOwned() && root_->profile && root_->profile->app && payload_ &&
            baseline_.captured && baseline_.initial && baseline_.files.size() == 2 && heldFiles_.size() == 2 &&
            baseline_.expectedBytes.size() == 2,
            "native multi selection source unavailable");
        auto actual = Access::InspectNativeMultiDesktopSelection(*root_->profile->app, baseline_.files);
        RequireObservation(actual.members.size() == baseline_.initial->members.size(),
            "native multi selection member count changed");
        for (std::size_t i = 0; i < actual.members.size(); ++i)
            RequireObservation(actual.members[i].path == baseline_.initial->members[i].path &&
                actual.members[i].key == baseline_.initial->members[i].key &&
                OwnedProfileDesktopRoot::SameId(actual.members[i].fileId, baseline_.initial->members[i].fileId) &&
                OwnedProfileDesktopRoot::SameId(actual.members[i].fileId, heldFiles_[i]->Identity()) &&
                heldFiles_[i]->PathStillNamesObject(baseline_.files[i]) &&
                heldFiles_[i]->HasBytes(baseline_.expectedBytes[i]),
                "native multi selection member identity changed");
        return actual;
    }
public:
    V4NativeMultiSelectionObservation(const V4NativeMultiSelectionObservation&) = delete;
    V4NativeMultiSelectionObservation& operator=(const V4NativeMultiSelectionObservation&) = delete;
    V4NativeMultiSelectionObservation(std::shared_ptr<const drag_global::event_graph::V4FamilyIdentityContext> identity,
        std::shared_ptr<const drag_global::V4FamilyValidatedContext> verified, std::shared_ptr<OwnedProfileDesktopRoot> root,
        std::shared_ptr<drag_global::V4IndependentFilePayload> payload)
        : root_(std::move(root)), payload_(std::move(payload)), identityContext_(std::move(identity)), verified_(std::move(verified)),
          baseline_(PrepareV4NativeMultiDesktopBaseline(Required(identityContext_), Required(verified_), Required(root_), payload_))
    {
        evidence_.candidateId = baseline_.candidateId;
        evidence_.resourceUnavailable = baseline_.resourceUnavailable;
        evidence_.unresolved.insert(evidence_.unresolved.end(), baseline_.unresolved.begin(), baseline_.unresolved.end());
        RequireObservation(baseline_.captured && baseline_.initial, "native multi selection baseline unavailable");
        evidence_.before = *baseline_.initial;
        RequireObservation(payload_ && payload_->files.size() == baseline_.files.size(),
            "native multi selection file lease unavailable");
        for (std::size_t i = 0; i < baseline_.files.size(); ++i)
        {
            const auto& file = payload_->files[i]; FILE_ID_INFO heldId{};
            RequireObservation(file.handle && file.handle != INVALID_HANDLE_VALUE &&
                GetFileInformationByHandleEx(file.handle, FileIdInfo, &heldId, sizeof(heldId)) &&
                OwnedProfileDesktopRoot::SameId(heldId, baseline_.initial->members[i].fileId),
                "native multi selection input handle unavailable");
            auto held = std::make_unique<ObservedPhysicalFile>(baseline_.files[i]);
            RequireObservation(OwnedProfileDesktopRoot::SameId(held->Identity(), heldId) &&
                held->PathStillNamesObject(baseline_.files[i]) && held->HasBytes(baseline_.expectedBytes[i]),
                "native multi selection input changed before lease");
            heldFiles_.push_back(std::move(held));
        }
        evidence_.beforeFilesRevalidated = true;
    }
    V4NativeMultiSelectionEvidence Evidence() const { return evidence_; }
    void BindExecution(const std::shared_ptr<drag_global::ole_driver::V4NativeSelectionExecutionKey>& key,
        const std::shared_ptr<drag_global::ole_driver::NativeHostEpoch>& epoch)
    {
        RequireObservation(key && !bound_ && !captureStarted_ && root_ && root_->StillOwned() && root_->profile && root_->profile->app &&
            Access::SameNativeHostEpoch(*root_->profile->app, epoch), "native multi selection binding unavailable");
        auto actual = Read();
        RequireObservation(evidence_.before && SameSnapshot(*evidence_.before, actual) && actual.otherSurfacesComplete &&
            actual.anchorScope.empty() && actual.anchorKey.empty() && actual.extraSelectedItemKeys.empty() &&
            actual.selectedDockReferences.empty() && actual.selectedRunningKeys.empty() &&
            actual.selectedWidgetIds.empty() && actual.selectedWidgetFolderEntries.empty(),
            "native multi selection baseline changed");
        RequireObservation(key->BindBaseline(epoch, root_->profile->app, evidence_.candidateId, identity_),
            "native multi selection key unavailable");
        bound_ = true;
    }
    void CaptureExecution(const std::shared_ptr<drag_global::ole_driver::V4NativeSelectionExecutionKey>& key,
        const std::shared_ptr<drag_global::ole_driver::NativeHostEpoch>& epoch)
    {
        RequireObservation(key && bound_ && !captureStarted_ && root_ && root_->StillOwned() && root_->profile && root_->profile->app &&
            key->BeginCapture(epoch, root_->profile->app, evidence_.candidateId, identity_),
            "native multi selection capture unavailable");
        captureStarted_ = true;
        try { evidence_.after = Read(); evidence_.afterFilesRevalidated = true; }
        catch (const std::bad_alloc&) { evidence_.resourceUnavailable = true; }
        catch (const std::length_error&) { evidence_.resourceUnavailable = true; }
        catch (...) { evidence_.unresolved.emplace_back("native_multi_selection_snapshot_unavailable"); }
        auto snapshot = std::make_shared<const V4NativeMultiSelectionEvidence>(evidence_);
        RequireObservation(key->Publish(epoch, root_->profile->app, evidence_.candidateId, identity_, std::move(snapshot)),
            "native multi selection sealing unavailable");
    }
    bool ConsumeForDrag(drag_global::ole_driver::V4NativeSelectionPreparationReceipt& receipt,
        const std::shared_ptr<drag_global::ole_driver::NativeHostEpoch>& epoch, const std::vector<bool>& present,
        const drag_global::event_graph::V4NativeJoinedGraphVariant& selected,
        const drag_global::ole_driver::MessageTrace& trace, HWND desktop, HWND control)
    {
        try
        {
            const auto sealed = receipt.SealedSelection();
            if (!sealed || sealed->candidateId != evidence_.candidateId || !sealed->after ||
                !sealed->afterFilesRevalidated || sealed->resourceUnavailable ||
                !sealed->after->otherSurfacesComplete || !root_ || !root_->StillOwned() || !root_->profile || !root_->profile->app ||
                !Access::NativeDragIdle(*root_->profile->app)) return false;
            const auto actual = Read();
            // This constrained case has exactly two unique file members,
            // mapped bijectively to the candidate's two logical members.
            // Both must actually be selected and every observed outside
            // selection must be empty before a drag may consume the receipt.
            if (!actual.otherSurfacesComplete || !SameSnapshot(*sealed->after, actual) || actual.members.size() != 2 ||
                !actual.members[0].selected || !actual.members[1].selected || !actual.extraSelectedItemKeys.empty() ||
                !actual.selectedDockReferences.empty() || !actual.selectedRunningKeys.empty() ||
                !actual.selectedWidgetIds.empty() || !actual.selectedWidgetFolderEntries.empty() ||
                !Access::NativeDragIdle(*root_->profile->app)) return false;
            // Physical file IDs and selected flags do not establish the
            // candidate's product item kinds. Capture ordering is a separate
            // post-drag observation, not a precondition invented here.
            if (!sealed->memberKindBound) return false;
            return receipt.ConsumeAfterRevalidation(epoch, root_->profile->app, *identityContext_, *verified_,
                present, selected, trace, desktop, control, identity_);
        }
        catch (...) { return false; }
    }
};

// Independent post-drag snapshot bound to the same consumed preparation
// receipt. It observes model/file/layout values and requests the production
// reload path; it never performs a drag, moves files, or edits the result.
struct V4NativeMultiDragObservation
{
private:
    std::shared_ptr<V4NativeMultiSelectionObservation> selection_;
    std::shared_ptr<drag_global::ole_driver::V4NativeSelectionPreparationReceipt> preparation_;
    V4NativeMultiDragEvidence evidence_;
    std::shared_ptr<const drag_global::ole_driver::V4NativeMultiDragObserverIdentity> identity_{
        new drag_global::ole_driver::V4NativeMultiDragObserverIdentity};
    bool bound_ = false, captureStarted_ = false;
public:
    V4NativeMultiDragObservation(const V4NativeMultiDragObservation&) = delete;
    V4NativeMultiDragObservation& operator=(const V4NativeMultiDragObservation&) = delete;
    V4NativeMultiDragObservation(std::shared_ptr<V4NativeMultiSelectionObservation> selection,
        std::shared_ptr<drag_global::ole_driver::V4NativeSelectionPreparationReceipt> preparation)
        : selection_(std::move(selection)), preparation_(std::move(preparation))
    {
        RequireObservation(selection_ && preparation_ && selection_->root_ && selection_->verified_ &&
            selection_->identityContext_ && preparation_->SealedSelection(),
            "native multi drag selection receipt unavailable");
        const auto value = [&](std::string_view axis) {
            return drag_global::Value(selection_->verified_->Axes(), selection_->verified_->Coordinate(), axis);
        };
        RequireObservation(value("source.E01") == "free_desktop" && value("source.E02") == "none" &&
            value("target.E01") == "free_desktop" && value("target.E02") == "none" &&
            preparation_->CandidateId() == selection_->evidence_.candidateId,
            "native multi drag endpoint or candidate unavailable");
        evidence_.candidateId = selection_->evidence_.candidateId;
    }
    V4NativeMultiDragEvidence Evidence() const { return evidence_; }
    void BindExecution(const std::shared_ptr<drag_global::ole_driver::V4NativeMultiDragExecutionKey>& key,
        const std::shared_ptr<drag_global::ole_driver::NativeHostEpoch>& epoch)
    {
        RequireObservation(key && !bound_ && !captureStarted_ && preparation_->Consumed() &&
            selection_->root_->StillOwned() && selection_->root_->profile && selection_->root_->profile->app &&
            Access::SameNativeHostEpoch(*selection_->root_->profile->app, epoch),
            "native multi drag baseline unavailable");
        const auto sealed = preparation_->SealedSelection();
        const auto actual = selection_->Read();
        RequireObservation(sealed && sealed->after && sealed->memberKindBound && actual.otherSurfacesComplete &&
            V4NativeMultiSelectionObservation::SameSnapshot(*sealed->after, actual),
            "native multi drag selected source changed");
        evidence_.beforeDrag = actual; evidence_.beforeFilesRevalidated = true;
        RequireObservation(key->BindBaseline(epoch, selection_->root_->profile->app, evidence_.candidateId, identity_),
            "native multi drag key unavailable");
        bound_ = true;
    }
    void CaptureExecution(const std::shared_ptr<drag_global::ole_driver::V4NativeMultiDragExecutionKey>& key,
        const std::shared_ptr<drag_global::ole_driver::NativeHostEpoch>& epoch)
    {
        RequireObservation(key && bound_ && !captureStarted_ && selection_->root_->StillOwned() &&
            selection_->root_->profile && selection_->root_->profile->app &&
            key->BeginCapture(epoch, selection_->root_->profile->app, evidence_.candidateId, identity_),
            "native multi drag capture unavailable");
        captureStarted_ = true;
        try
        {
            auto& profile = *selection_->root_->profile;
            RequireObservation(Access::NativeDragIdle(*profile.app), "native multi drag has not ended");
            evidence_.afterDrag = selection_->Read(); evidence_.afterFilesRevalidated = true;
            evidence_.beforeReloadLayout = CaptureV4NativeDesktopLayout(*selection_->root_, 4u * 1024u * 1024u);
            if (evidence_.beforeReloadLayout->resourceUnavailable) evidence_.resourceUnavailable = true;
            RequireObservation(evidence_.beforeReloadLayout->captured, "native multi drag layout snapshot unavailable");
            Access::RequestAsync(*profile.app, true);
            Wait([&] { return Access::FileDropIdle(*profile.app) && Access::LayoutSaveIdle(*profile.app) &&
                Access::NativeDragIdle(*profile.app); });
            evidence_.afterReload = selection_->Read(); evidence_.reloadFilesRevalidated = true;
        }
        catch (const std::bad_alloc&) { evidence_.resourceUnavailable = true; }
        catch (const std::length_error&) { evidence_.resourceUnavailable = true; }
        catch (...) { evidence_.unresolved.emplace_back("native_multi_drag_snapshot_or_reload_unavailable"); }
        auto snapshot = std::make_shared<const V4NativeMultiDragEvidence>(evidence_);
        RequireObservation(key->Publish(epoch, selection_->root_->profile->app, evidence_.candidateId, identity_, std::move(snapshot)),
            "native multi drag sealing unavailable");
    }
};

// Own the objects referenced by the observer until the caller has finished
// with the receipt and any attempted handoff. The supplied host boundaries
// still require the executor's independent graph/device capability checks.
struct V4NativeMultiSelectionBridgeResult
{
    std::shared_ptr<const drag_global::event_graph::V4FamilyIdentityContext> identity;
    std::shared_ptr<const drag_global::V4FamilyValidatedContext> verified;
    std::shared_ptr<OwnedProfileDesktopRoot> root;
    std::shared_ptr<drag_global::V4IndependentFilePayload> payload;
    std::shared_ptr<drag_global::ole_driver::NativeHostEpoch> epoch;
    drag_global::ole_driver::V4NativeSelectionPreparationPlan plan;
    bool planCaptured = false;
    std::shared_ptr<V4NativeMultiSelectionObservation> observer;
    std::optional<drag_global::V4CategoryInputObservation> categoryInput;
    std::shared_ptr<drag_global::ole_driver::V4NativeSelectionPreparationReceipt> receipt;
    std::optional<drag_global::ole_driver::V4NativePreparationProgramObservation> preparationProgram;
    std::optional<drag_global::ole_driver::V4NativePreparationMarqueeObservation> preparationMarquee;
    std::optional<V4NativeMultiSelectionEvidence> localSelectionSnapshot;
    std::shared_ptr<V4NativeMultiDragObservation> dragObserver;
    std::optional<drag_global::ole_driver::V4NativeRunReceipt> dragReceipt;
    std::vector<drag_global::ole_driver::V4NativeRunReceipt> unstartedDragAttempts;
    bool dragEntryUnknown = false;
    bool fixturePreparationAttempted = false, fixturePreparationUnknown = false, preparationEntryUnknown = false;
    std::vector<std::string> dragPreconditions, dragPostconditions, dragUnclassified;
    bool resourceUnavailable = false, observationUnavailable = true;
    std::vector<std::string> unresolved;
};

inline V4NativeMultiSelectionBridgeResult PrepareV4NativeMultiSelection(
    std::shared_ptr<const drag_global::event_graph::V4FamilyIdentityContext> identity,
    std::shared_ptr<const drag_global::V4FamilyValidatedContext> verified,
    std::shared_ptr<OwnedProfileDesktopRoot> root,
    std::shared_ptr<drag_global::V4IndependentFilePayload> payload,
    const std::vector<bool>& present, const drag_global::event_graph::V4NativeJoinedGraphVariant& selected,
    HWND desktop, HWND control, RECT bounds, const drag_global::ole_driver::MessageTrace& trace,
    std::shared_ptr<drag_global::ole_driver::NativeHostEpoch> epoch,
    const drag_global::ole_driver::NativeHostBoundaries& host, std::size_t actionBudget,
    std::size_t categoryRuleBudget = 128, std::size_t categoryMemberBudget = 128,
    std::size_t categoryUtf16ByteBudget = 4u * 1024u * 1024u)
{
    using namespace drag_global::ole_driver;
    V4NativeMultiSelectionBridgeResult out;
    bool preparationDispatchAttempted = false;
    out.identity = std::move(identity); out.verified = std::move(verified); out.root = std::move(root);
    out.payload = std::move(payload); out.epoch = std::move(epoch);
    try
    {
        if (!out.identity || !out.verified || !out.root || !out.payload || !out.epoch ||
            !out.root->StillOwned() || !out.root->profile || !out.root->profile->app ||
            !Access::SameNativeHostEpoch(*out.root->profile->app, out.epoch))
        { out.unresolved.emplace_back("native_multi_bridge_context_unavailable"); return out; }
        out.plan = BuildV4NativeSelectionPreparationPlan(*out.identity, *out.verified, present, selected, actionBudget);
        out.planCaptured = true;
        out.resourceUnavailable = out.plan.resourceUnavailable;
        out.unresolved = out.plan.preconditions;
        // In particular, today's plan carries unconditional executor/graph
        // capability obligations. Do not mutate even the initial fixture or
        // invoke Device until those are genuinely discharged.
        if (out.resourceUnavailable || !out.plan.identityValid || !out.plan.inputsMaterialized || !out.unresolved.empty())
            return out;
        out.fixturePreparationAttempted = true; out.fixturePreparationUnknown = true;
        out.observer = std::make_shared<V4NativeMultiSelectionObservation>(
            out.identity, out.verified, out.root, out.payload);
        out.fixturePreparationUnknown = false;
        // This is a local pre-session snapshot of the actual host fields. It
        // cannot establish what a later production classification consumed.
        const auto categorySource = Access::CreateV4CategoryObservationSource(
            out.root, out.payload, out.identity, out.verified, out.epoch);
        if (categorySource)
        {
            out.categoryInput = drag_global::V4CategoryInputObserver::Observe(*out.verified,
                *out.identity, *categorySource, categoryRuleBudget, categoryMemberBudget,
                categoryUtf16ByteBudget);
            out.resourceUnavailable = out.resourceUnavailable || out.categoryInput->resourceUnavailable;
        }
        out.unresolved.emplace_back("native_multi_category_inputs_production_scope_unverified");
        if (out.observationUnavailable) return out;
        V4NativeSelectionObserverBoundaries boundaries;
        const auto observer = out.observer; const auto pinnedEpoch = out.epoch;
        boundaries.bindBeforeBegin = [observer, pinnedEpoch](const auto& key) { observer->BindExecution(key, pinnedEpoch); };
        boundaries.observeBusiness = [observer, pinnedEpoch](const auto& key) { observer->CaptureExecution(key, pinnedEpoch); };
        preparationDispatchAttempted = true;
        out.receipt = V4NativeSelectionPreparationExecutor::RunBound(*out.identity, *out.verified, present, selected,
            desktop, control, bounds, trace, out.epoch, host, boundaries, actionBudget);
        preparationDispatchAttempted = false;
        if (!out.receipt) { out.unresolved.emplace_back("native_multi_bridge_receipt_unavailable"); return out; }
        out.preparationEntryUnknown = out.receipt->ProductionEntryUnknown();
        if (out.receipt->ProductionStarted())
        {
            out.preparationProgram = InspectV4NativePreparationProgram(*out.receipt, *out.identity, *out.verified,
                present, selected, out.epoch, trace, desktop, control, actionBudget);
            out.resourceUnavailable = out.resourceUnavailable || out.preparationProgram->resourceUnavailable;
            if (snowdesktop::external_transfer_observation::Complete())
            {
                out.preparationMarquee = InspectV4NativePreparationMarquee(*out.receipt, *out.identity, *out.verified,
                    present, selected, out.epoch, trace, desktop, control, actionBudget);
                out.resourceUnavailable = out.resourceUnavailable || out.preparationMarquee->resourceUnavailable;
            }
            else
            {
                out.preparationMarquee.emplace();
                out.preparationMarquee->unresolved.emplace_back("native_multi_marquee_diagnostics_not_stopped_or_complete");
            }
            if (out.receipt->SameCandidate(*out.identity, *out.verified, present, selected))
                if (const auto local = out.receipt->LocalSelectionSnapshot(out.epoch, trace, desktop, control))
                    out.localSelectionSnapshot = *local;
        }
        out.unresolved.insert(out.unresolved.end(), out.receipt->Unresolved().begin(), out.receipt->Unresolved().end());
        if (!out.receipt->SealedSelection()) out.unresolved.emplace_back("native_multi_bridge_selection_not_sealed");
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.preparationEntryUnknown = out.preparationEntryUnknown || preparationDispatchAttempted; }
    catch (const std::length_error&) { out.resourceUnavailable = true; out.preparationEntryUnknown = out.preparationEntryUnknown || preparationDispatchAttempted; }
    catch (...) { out.preparationEntryUnknown = out.preparationEntryUnknown || preparationDispatchAttempted; out.unresolved.emplace_back("native_multi_bridge_observation_unavailable"); }
    return out;
}

// The caller retains the bridge, trace and host for this synchronous call.
// A preparation snapshot alone cannot enter this path: the typed receipt must
// have been sealed from actual preparation, and RunBound consumes it once
// inside the same host epoch before any second Device input.
inline void CompleteV4NativeMultiSelectionDrag(V4NativeMultiSelectionBridgeResult& bridge,
    const std::vector<bool>& present, const drag_global::event_graph::V4NativeJoinedGraphVariant& selected,
    const std::vector<drag_global::ole_driver::NativePointInput>& points,
    HWND desktop, HWND control, RECT bounds, const drag_global::ole_driver::MessageTrace& trace,
    const drag_global::ole_driver::NativeHostBoundaries& host)
{
    using namespace drag_global::ole_driver;
    bool dispatchAttempted = false;
    try
    {
        if (bridge.dragReceipt)
        {
            const auto& previous = bridge.dragReceipt->Execution();
            // Only a typed, unstarted preflight result may be retried. Keep
            // the prior receipt so its original obligations are not lost.
            if (!bridge.receipt || bridge.receipt->Consumed() || previous.status != Status::unimplemented ||
                previous.productionStarted || previous.productionEntryUnknown || bridge.dragEntryUnknown)
            { bridge.unresolved.emplace_back("native_multi_drag_retry_requires_reconciliation"); return; }
            bridge.unstartedDragAttempts.push_back(std::move(*bridge.dragReceipt));
            bridge.dragReceipt.reset(); bridge.dragObserver.reset();
        }
        if (!bridge.identity || !bridge.verified || !bridge.root || !bridge.epoch || !bridge.observer ||
            !bridge.receipt || bridge.dragObserver || bridge.dragReceipt || !bridge.receipt->SealedSelection() ||
            !bridge.receipt->SameCandidate(*bridge.identity, *bridge.verified, present, selected) ||
            !bridge.receipt->SameHost(bridge.epoch, trace, desktop, control))
        { bridge.unresolved.emplace_back("native_multi_drag_preparation_receipt_unavailable"); return; }
        bridge.dragObserver = std::make_shared<V4NativeMultiDragObservation>(bridge.observer, bridge.receipt);
        V4NativeMultiDragObserverBoundaries boundaries;
        const auto selection = bridge.observer; const auto preparation = bridge.receipt;
        const auto observer = bridge.dragObserver; const auto epoch = bridge.epoch;
        boundaries.consumePreparation = [selection, preparation, epoch, &present, &selected, &trace, desktop, control] {
            return selection->ConsumeForDrag(*preparation, epoch, present, selected, trace, desktop, control);
        };
        boundaries.bindBeforeBegin = [observer, epoch](const auto& key) { observer->BindExecution(key, epoch); };
        boundaries.observeBusiness = [observer, epoch](const auto& key) { observer->CaptureExecution(key, epoch); };
        dispatchAttempted = true;
        bridge.dragReceipt.emplace(V4NativeSequence::RunBound(*bridge.identity, *bridge.verified, present, selected,
            points, desktop, control, bounds, trace, bridge.epoch, host, {}, {}, {}, {}, bridge.receipt, boundaries));
        dispatchAttempted = false;
        bridge.dragEntryUnknown = bridge.dragReceipt->Execution().productionEntryUnknown;
        bridge.dragPreconditions = bridge.dragReceipt->Execution().obligations;
        bridge.dragPostconditions = bridge.dragReceipt->Execution().postconditionsPending;
        bridge.dragUnclassified = bridge.dragReceipt->Execution().unclassifiedObligations;
        bridge.unresolved.insert(bridge.unresolved.end(), bridge.dragReceipt->Unresolved().begin(), bridge.dragReceipt->Unresolved().end());
        if (!bridge.dragReceipt->SealedMultiDragBusiness())
            bridge.unresolved.emplace_back("native_multi_drag_business_not_sealed");
    }
    catch (const std::bad_alloc&) { bridge.resourceUnavailable = true; bridge.dragEntryUnknown = bridge.dragEntryUnknown || dispatchAttempted; }
    catch (const std::length_error&) { bridge.resourceUnavailable = true; bridge.dragEntryUnknown = bridge.dragEntryUnknown || dispatchAttempted; }
    catch (...) { bridge.dragEntryUnknown = bridge.dragEntryUnknown || dispatchAttempted; bridge.unresolved.emplace_back("native_multi_drag_bridge_unavailable"); }
}

// Independent value evidence. A missing optional means copying or observation
// did not occur; it must not be serialized as an empty observed log. The
// caller retains the bridge owner until all source/host work is finished.
struct V4NativeMultiPreparationReceiptEvidence
{
    bool productionStarted = false, productionEntryUnknown = false, consumed = false, sealedObserved = false;
    std::uintptr_t hostIdentity = 0; // raw local address only; not an identity proof
    std::optional<std::string> candidateId, graphId;
    std::optional<drag_global::ole_driver::Result> execution;
    std::optional<std::vector<drag_global::ole_driver::V4NativeSelectionSessionRecord>> sessions;
    std::optional<std::vector<std::string>> unresolved, graphUnresolved;
    std::optional<V4NativeMultiSelectionEvidence> sealedSelection;
};
struct V4NativeMultiDragReceiptEvidence
{
    bool productionStarted = false, productionEntryUnknown = false, sealedObserved = false;
    std::uintptr_t hostIdentity = 0;
    std::optional<std::string> candidateId, graphId;
    std::optional<drag_global::ole_driver::Result> execution;
    std::optional<std::vector<drag_global::ole_driver::V4NativeQueuedRecord>> queued;
    std::optional<std::vector<drag_global::ole_driver::V4NativeDeviceRecord>> device;
    std::optional<std::vector<drag_global::ole_driver::V4NativeTraceRecord>> trace;
    std::optional<std::vector<snowdesktop::private_pointer::State>> beforeEndStates;
    std::optional<std::vector<snowdesktop::private_pointer::DeliveryObservationLog>> beforeEndDelivery;
    std::optional<std::vector<std::string>> unresolved;
    std::optional<V4NativeMultiDragEvidence> sealedBusiness;
};
struct V4NativeMultiPartialEvidence
{
    bool identityBound = false, captureComplete = false, resourceUnavailable = false, observationUnavailable = true;
    bool fixturePreparationAttempted = false, fixturePreparationUnknown = false;
    bool preparationEntryUnknown = false, dragEntryUnknown = false;
    std::size_t unstartedDragAttemptCount = 0;
    std::optional<drag_global::ole_driver::V4NativeSelectionPreparationPlan> plan;
    std::optional<drag_global::V4CategoryInputObservation> categoryInput;
    std::optional<V4NativeMultiPreparationReceiptEvidence> preparation;
    std::optional<drag_global::ole_driver::V4NativePreparationProgramObservation> preparationProgram;
    std::optional<drag_global::ole_driver::V4NativePreparationMarqueeObservation> preparationMarquee;
    std::optional<V4NativeMultiSelectionEvidence> localSelectionSnapshot;
    std::optional<V4NativeMultiSelectionEvidence> partialSelection;
    std::optional<V4NativeMultiDragEvidence> partialDrag;
    std::optional<V4NativeMultiDragReceiptEvidence> drag;
    std::vector<V4NativeMultiDragReceiptEvidence> unstartedDragAttempts;
    std::optional<std::vector<std::string>> unresolved, dragPreconditions, dragPostconditions, dragUnclassified;
};

inline V4NativeMultiPartialEvidence CaptureV4NativeMultiPartialEvidence(
    const V4NativeMultiSelectionBridgeResult& bridge,
    const drag_global::event_graph::V4FamilyIdentityContext& identity,
    const drag_global::V4FamilyValidatedContext& verified, const std::vector<bool>& present,
    const drag_global::event_graph::V4NativeJoinedGraphVariant& selected)
{
    using namespace drag_global;
    V4NativeMultiPartialEvidence out;
    out.fixturePreparationAttempted = bridge.fixturePreparationAttempted;
    out.fixturePreparationUnknown = bridge.fixturePreparationUnknown;
    out.preparationEntryUnknown = bridge.preparationEntryUnknown;
    out.dragEntryUnknown = bridge.dragEntryUnknown;
    out.unstartedDragAttemptCount = bridge.unstartedDragAttempts.size();
    out.resourceUnavailable = bridge.resourceUnavailable;
    bool complete = true;
    const auto copy = [&](auto&& operation) {
        try { operation(); }
        catch (const std::bad_alloc&) { out.resourceUnavailable = true; complete = false; }
        catch (const std::length_error&) { out.resourceUnavailable = true; complete = false; }
        catch (...) { complete = false; }
    };
    copy([&] {
        if (!bridge.identity || !bridge.verified || identity.FamilyBinding() != verified.FamilyBinding() ||
            bridge.identity->FamilyBinding() != identity.FamilyBinding() ||
            bridge.identity->SchemaBinding() != identity.SchemaBinding() ||
            bridge.verified->FamilyBinding() != verified.FamilyBinding()) return;
        const auto expected = identity.RawCandidateIdentity(verified.Coordinate());
        const auto actual = bridge.identity->RawCandidateIdentity(bridge.verified->Coordinate());
        const auto graph = event_graph::V4NativeJoinedGraph::Replay(identity, verified, present, selected.id);
        if (expected.resourceUnavailable || actual.resourceUnavailable ||
            graph.status == event_graph::V4GraphReplayStatus::resource_unavailable) out.resourceUnavailable = true;
        out.identityBound = !expected.resourceUnavailable && !actual.resourceUnavailable && expected.errors.empty() &&
            actual.errors.empty() && !expected.id.empty() && expected.id == actual.id && graph.identityValid &&
            graph.errors.empty() && graph.variant.id == selected.id && graph.variant.classes == selected.classes &&
            graph.variant.order == selected.order && graph.variant.unresolved == selected.unresolved;
    });
    if (bridge.planCaptured) copy([&] { out.plan = bridge.plan; });
    if (bridge.categoryInput) copy([&] { out.categoryInput = *bridge.categoryInput; });
    copy([&] { out.unresolved = bridge.unresolved; });
    copy([&] { out.dragPreconditions = bridge.dragPreconditions; });
    copy([&] { out.dragPostconditions = bridge.dragPostconditions; });
    copy([&] { out.dragUnclassified = bridge.dragUnclassified; });
    if (bridge.preparationProgram) copy([&] { out.preparationProgram = *bridge.preparationProgram; });
    if (bridge.preparationMarquee) copy([&] { out.preparationMarquee = *bridge.preparationMarquee; });
    if (bridge.localSelectionSnapshot) copy([&] { out.localSelectionSnapshot = *bridge.localSelectionSnapshot; });
    if (bridge.observer) copy([&] { out.partialSelection = bridge.observer->Evidence(); });
    if (bridge.dragObserver) copy([&] { out.partialDrag = bridge.dragObserver->Evidence(); });
    if (bridge.receipt)
    {
        out.preparation.emplace(); auto& target = *out.preparation; const auto& source = *bridge.receipt;
        target.productionStarted = source.ProductionStarted(); target.productionEntryUnknown = source.ProductionEntryUnknown();
        target.hostIdentity = source.HostIdentity();
        target.consumed = source.Consumed(); const auto sealed = source.SealedSelection(); target.sealedObserved = static_cast<bool>(sealed);
        copy([&] { target.candidateId = std::string(source.CandidateId()); });
        copy([&] { target.graphId = std::string(source.GraphId()); });
        copy([&] { target.execution = source.Execution(); });
        copy([&] { target.sessions = source.Sessions(); });
        copy([&] { target.unresolved = source.Unresolved(); });
        copy([&] { target.graphUnresolved = source.GraphUnresolved(); });
        if (sealed) copy([&] { target.sealedSelection = *sealed; });
    }
    const auto captureDrag = [&](const ole_driver::V4NativeRunReceipt& source, V4NativeMultiDragReceiptEvidence& target) {
        target.productionStarted = source.Execution().productionStarted;
        target.productionEntryUnknown = source.Execution().productionEntryUnknown;
        target.hostIdentity = source.HostIdentity(); const auto sealed = source.SealedMultiDragBusiness();
        target.sealedObserved = static_cast<bool>(sealed);
        copy([&] { target.candidateId = std::string(source.CandidateId()); });
        copy([&] { target.graphId = std::string(source.GraphId()); });
        copy([&] { target.execution = source.Execution(); });
        copy([&] { target.queued = source.QueuedRecords(); });
        copy([&] { target.device = source.DeviceRecords(); });
        copy([&] { target.trace = source.TraceRecords(); });
        copy([&] { target.beforeEndStates = source.BeforeEndStates(); });
        copy([&] { target.beforeEndDelivery = source.BeforeEndDelivery(); });
        copy([&] { target.unresolved = source.Unresolved(); });
        if (sealed) copy([&] { target.sealedBusiness = *sealed; });
    };
    if (bridge.dragReceipt) { out.drag.emplace(); captureDrag(*bridge.dragReceipt, *out.drag); }
    for (const auto& receipt : bridge.unstartedDragAttempts)
    {
        const auto before = out.unstartedDragAttempts.size();
        copy([&] { out.unstartedDragAttempts.emplace_back(); });
        if (out.unstartedDragAttempts.size() == before) break;
        captureDrag(receipt, out.unstartedDragAttempts.back());
    }
    out.captureComplete = complete;
    return out;
}
}
