#pragma once
#include "global_drag_event_graph.inc"
#include "global_drag_ole_driver.inc"
#include "global_drag_v3_scheduler.h"
#include <optional>
#include <unordered_set>

namespace drag_global
{
EventProgram Program(const std::vector<Axis>& axes, const Candidate& candidate);
}

// Include after global_drag_model.h at global scope. This is a conservative
// preflight for the existing external real-file fixture, not an executor.
// FileCase / MatrixContainer are not called until every axis, event, derived
// observation and production completion route has an implemented contract.
namespace drag_global::ingress_adapter
{
enum class Evidence { planned_input, unimplemented, derived_unimplemented };
struct AxisAudit
{
    std::string id;
    std::string value;
    Evidence evidence;
    std::string detail;
};
struct Preflight
{
    std::vector<AxisAudit> axes;
    EventProgram program;
    std::vector<std::string> obligations;

    // A plan is not evidence that a fixture was constructed or that the
    // production implementation ran. This adapter intentionally cannot pass
    // until its execution and observation contracts are implemented.
    bool CanExecute() const { return false; }
};

inline bool OneOf(std::string_view value, std::initializer_list<std::string_view> allowed)
{
    return std::find(allowed.begin(), allowed.end(), value) != allowed.end();
}

// A registered planned_input only identifies a value that the current
// fixture can potentially supply. It is NOT a verified fixture observation.
// A newly added model axis falls through to unimplemented, not acceptance.
inline const char* PlannedInput(std::string_view id, std::string_view value,
    const std::vector<Axis>& axes, const Candidate& candidate)
{
    const auto target = Value(axes, candidate, "target.E02");
    const bool nativeTarget = OneOf(target, {"collection", "desktop_files", "folder_mapping", "collection_group", "file_group"});
    const auto actualCount = Value(axes, candidate, "P02.source_actual_count");
    const auto resolvedCount = Value(axes, candidate, "P02.resolved_count");
    const auto manyCount = Value(axes, candidate, "P02.many_count");
    const auto primary = Value(axes, candidate, "P01.primary");
    const auto second = Value(axes, candidate, "P01.second");
    // The existing provider is created from one real file or a file plus a
    // directory. This is only a candidate construction plan: in particular,
    // no source read or final count observation is implied by a rejected or
    // cancelled drop. Hover metadata and host summaries remain independent.
    const bool supportedCountPlan = primary == "file" && manyCount == "na" &&
        ((actualCount == "one" && resolvedCount == "one" && second == "na") ||
         (actualCount == "two" && resolvedCount == "two" && second == "directory"));
    if (supportedCountPlan && id == "P02.source_actual_count")
        return "planned_source_payload_count_requires_actual_source_read_observation";
    // This is the narrow case expressible by the existing desktop FileCase.
    // These entries are still only plans; production and read-only snapshots
    // must establish each input and completion before any execution can pass.
    const bool desktopSingleCopy =
        Value(axes, candidate, "T01") == "external_ingress" &&
        Value(axes, candidate, "source.E01") == "external_unknown" &&
        Value(axes, candidate, "target.E01") == "free_desktop" && target == "none" &&
        primary == "file" && second == "na" && actualCount == "one" && resolvedCount == "one" && manyCount == "na" &&
        Value(axes, candidate, "G01.button") == "unknown" && Value(axes, candidate, "G02.press") == "unknown" &&
        Value(axes, candidate, "G02.enter_buttons") == "left" && Value(axes, candidate, "G02.enter") == "none" &&
        Value(axes, candidate, "G02.hover_first") == "na" && Value(axes, candidate, "G02.hover_later") == "na" &&
        Value(axes, candidate, "G02.hover_first_buttons") == "na" && Value(axes, candidate, "G02.hover_later_buttons") == "na" &&
        Value(axes, candidate, "G02.release_buttons") == "none" && Value(axes, candidate, "G02.release") == "ctrl" &&
        Value(axes, candidate, "G04.termination") == "drop" && Value(axes, candidate, "T03") == "copy_move" &&
        Value(axes, candidate, "T03.extra_protocol_bits") == "none" && Value(axes, candidate, "T04.capability") == "enabled";
    if (desktopSingleCopy)
    {
        if (id == "target.E19" && value == "named") return "isolated_model_page_named";
        if (id == "C07" && value == "on") return "fixture_enables_program_collection";
        if (id == "G04.termination" && value == "drop") return "ole_drop_sequence_planned";
        if (Value(axes, candidate, "X02") == "none" && Value(axes, candidate, "X02.position") == "no_interruption" &&
            OneOf(id, {"X02", "X02.position"})) return "no_injected_interruption_planned";
        if (Value(axes, candidate, "X03.task_count") == "one" && Value(axes, candidate, "X03.submit_order") == "na" &&
            Value(axes, candidate, "X03.order") == "single" && OneOf(id, {"X03.task_count", "X03.submit_order", "X03.order"}))
            return "one_logical_drop_task_planned";
        if (value == "na" && OneOf(id, {
            "P03.member1_identity_ref", "P03.member1_widget_type", "P04.second_source_relation",
            "P04.member1_source_ref", "P04.member1_group_ref", "P06.second_entry_class",
            "P06.second_special_extension", "R04.member1_incoming_name_ref", "R04.member1_incoming_name_spelling"}))
            return "second_selected_member_absent";
        if (value == "na")
        {
            const auto marker = id.find(".member");
            if (marker != std::string_view::npos && id.substr(0, 7) != "task_b.")
            {
                auto at = marker + 7; // length of ".member"
                std::size_t member = 0;
                const auto first = at;
                while (at < id.size() && id[at] >= '0' && id[at] <= '9')
                    member = member * 10 + static_cast<std::size_t>(id[at++] - '0');
                if (at != first && at < id.size() && id[at] == '_' && member >= 1 && member <= 128)
                    return "selected_member_absent";
            }
        }
    }
    if (id == "T01" && value == "external_ingress") return "ole_adapter_boundary";
    if (id == "source.E01" && value == "external_unknown") return "shell_data_object_without_source_window";
    if (id == "target.E01" && value == "free_desktop") return "desktop_model_not_visible_host";
    if (id == "target.E02" && OneOf(value, {"none", "collection", "desktop_files", "folder_mapping", "collection_group", "file_group"}))
        return "seed_destination_root_type";
    if (id == "target.E08.active_child_type" &&
        ((target == "collection_group" && value == "collection") ||
         (target == "file_group" && OneOf(value, {"desktop_files", "folder_mapping"}))))
        return "seed_first_group_member_type";
    if (id == "target.E08" && OneOf(target, {"collection_group", "file_group"}) && value == "first_child")
        return "seed_first_active_group_member";
    if (id == "target.E23" && ((target == "none" && value == "four") ||
        (nativeTarget && value == "twelve"))) return "seed_page_columns";
    if (id == "target.E24" && ((target == "none" && value == "three") ||
        (nativeTarget && value == "eight"))) return "seed_page_rows";
    // The existing native fixture has only these paired representations. A
    // mixed span or a mismatched entry representation is not constructible by
    // that fixture; planning it as two independent inputs would be misleading.
    const auto entry = Value(axes, candidate, "target.E03");
    const auto columns = Value(axes, candidate, "target.E17.columns");
    const auto rows = Value(axes, candidate, "target.E17.rows");
    const bool nativeRepresentation = nativeTarget &&
        ((entry == "desktop_icon" && columns == "one" && rows == "one") ||
         (entry == "widget_body" && columns == "four" && rows == "three"));
    if (nativeRepresentation && id == "target.E17.columns") return "seed_root_span_columns";
    if (nativeRepresentation && id == "target.E17.rows") return "seed_root_span_rows";
    if (id == "C06" && nativeTarget && value == "off") return "seed_root_and_member_auto_collect_off";
    if (id == "P01.primary" && value == "file") return "seed_payload_file";
    if (id == "P01.second" && OneOf(value, {"na", "directory"})) return "seed_optional_directory";
    if (id == "G01.button" && value == "unknown") return "external_initiating_button_not_observed";
    if (id == "G02.press" && value == "unknown") return "external_source_press_not_observed";
    if (id == "G02.enter" && OneOf(value, {"none", "shift"})) return "ole_enter_modifier_state";
    if (id == "G02.enter_buttons" && value == "left") return "ole_enter_left_button_state";
    if (id == "G02.hover_first" && value == "na") return "fixture_sends_no_first_drag_over";
    if (id == "G02.hover_later" && value == "na") return "fixture_sends_no_drag_over";
    if (id == "G02.hover_first_buttons" && value == "na") return "no_first_drag_over_buttons";
    if (id == "G02.hover_later_buttons" && value == "na") return "no_later_drag_over_buttons";
    if (id == "G02.release" && OneOf(value, {"na", "ctrl", "shift", "alt"})) return "fixture_drop_or_leave_modifier";
    if (id == "G02.release_buttons" && OneOf(value, {"na", "none"})) return "fixture_drop_or_leave_buttons";
    if (id == "T03" && OneOf(value, {"none", "copy", "move", "link", "copy_move", "copy_link", "move_link", "copy_move_link"}))
        return "source_allowed_effect_mask";
    if (id == "T03.extra_protocol_bits" && value == "none") return "no_extra_effect_bits";
    if (id == "T04.capability" && OneOf(value, {"present_disabled", "enabled"})) return "source_async_capability_mode";
    return nullptr;
}

// Construct once per model, not once per candidate. Keep a copy so that the
// validated schema and lookup cannot drift during candidate inspection.
struct Context
{
    std::vector<Axis> axes;
    std::shared_ptr<const std::unordered_map<std::string, std::size_t>> lookup;
    std::vector<std::string> obligations;

    explicit Context(const std::vector<Axis>& supplied) : axes(supplied), lookup(BuildIndex(axes))
    {
        static const auto expected = Model();
        if (axes.size() != expected.size()) obligations.emplace_back("model_schema_axis_count_mismatch");
        else for (std::size_t i = 0; i < axes.size(); ++i)
            if (axes[i].id != expected[i].id || axes[i].role != expected[i].role ||
                axes[i].values != expected[i].values || axes[i].label != expected[i].label)
                obligations.push_back("model_schema_mismatch:" + axes[i].id);
        std::unordered_set<std::string> seen;
        for (const auto& axis : axes)
            if (!seen.insert(axis.id).second) obligations.push_back("duplicate_axis:" + axis.id);
    }
};

// One immediate native-move snapshot, not a commit or a complete endpoint
// oracle. The caller supplies Device::Current() after the driver waits for its
// queued move and the screen point selected by this Candidate's geometry
// binding. Device records the sample before sending the message, so this
// sample alone does not prove that the production window procedure returned.
// Only copied values escape; transient Slot pointers are never read.
struct NativeDesktopEndpointEvidence
{
    bool deviceSampleMatched = false;
    bool sourceDesktopObserved = false;
    bool targetDesktopObserved = false;
    bool uniqueReturnedMatchingTrace = false;
    std::size_t matchingTraceCount = 0;
    std::uint64_t deviceSequence = 0;
    POINT screen{};
    HitRegion storedTargetRegion = HitRegion::None;
    std::vector<std::string> unresolved;
};
inline NativeDesktopEndpointEvidence InspectNativeDesktopEndpoint(const Context& context, const Candidate& input,
    DesktopApp& app, const snowdesktop::private_pointer::State& state, POINT expectedScreen,
    std::size_t traceCheckpoint)
{
    NativeDesktopEndpointEvidence evidence;
    if (!context.obligations.empty() || input.values.size() != context.axes.size())
    { evidence.unresolved.emplace_back("native_endpoint_candidate_schema_unimplemented"); return evidence; }
    Candidate candidate = input; candidate.lookup = context.lookup;
    for (std::size_t i = 0; i < context.axes.size(); ++i)
    {
        const auto& axis = context.axes[i];
        const auto& value = candidate.values[i];
        if ((axis.role == Role::Derived && !value.empty()) ||
            (axis.role != Role::Derived &&
                (std::find(axis.values.begin(), axis.values.end(), value) == axis.values.end() ||
                 !PrefixAllows(context.axes, candidate, axis.id, value))))
        { evidence.unresolved.push_back("native_endpoint_candidate_value_invalid:" + axis.id); return evidence; }
    }
    if (const auto excluded = StructuralExclusion(context.axes, candidate); !excluded.empty())
    { evidence.unresolved.push_back("native_endpoint_candidate_structure_excluded:" + excluded); return evidence; }
    const auto v = [&](std::string_view id) { return Value(context.axes, candidate, id); };
    if (v("T01") != "internal_native" || v("source.E01") != "free_desktop" ||
        v("target.E01") != "free_desktop" || v("target.E02") != "none")
    { evidence.unresolved.emplace_back("native_desktop_endpoint_values_unimplemented"); return evidence; }
    const auto window = behavior::Access::completionWindow;
    if (behavior::Access::asyncOwner != &app || behavior::Access::productionDispatch != 1 ||
        !behavior::Access::nativePointerDispatch || !behavior::Access::messageTrace ||
        behavior::Access::messageTrace->LostOrCrossThread() || !window ||
        GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId() || !state.enabled || state.closing ||
        state.buttons != 1 || state.captured != window ||
        state.pendingEvents || !state.error.empty() || state.trace.empty())
    { evidence.unresolved.emplace_back("native_endpoint_owner_or_device_unavailable"); return evidence; }
    const auto& sample = state.trace.back();
    if (sample.sequence != state.sequence || sample.window != window || sample.message != WM_MOUSEMOVE ||
        sample.phase != "moving" || sample.buttons != state.buttons ||
        sample.point.x != state.point.x || sample.point.y != state.point.y ||
        state.point.x != expectedScreen.x || state.point.y != expectedScreen.y)
    { evidence.unresolved.emplace_back("native_endpoint_device_point_or_event_mismatch"); return evidence; }
    evidence.deviceSampleMatched = true;
    evidence.deviceSequence = state.sequence;
    evidence.screen = state.point;
    const auto& events = behavior::Access::messageTrace->Observations();
    POINT client = state.point;
    const bool noKeys = std::none_of(state.keys.begin(), state.keys.end(), [](bool pressed) { return pressed; });
    if (traceCheckpoint > events.size() || !noKeys || !ScreenToClient(window, &client) ||
        client.x < -32768 || client.x > 32767 || client.y < -32768 || client.y > 32767)
        evidence.unresolved.emplace_back("native_endpoint_trace_interval_or_coordinate_unavailable");
    else
    {
        const auto parameter = MAKELPARAM(client.x, client.y);
        bool soleReturned = false;
        for (std::size_t i = traceCheckpoint; i < events.size(); ++i)
        {
            const auto& event = events[i];
            if (event.route != ole_driver::MessageRoute::desktop_window_proc || event.window != window ||
                event.message != WM_MOUSEMOVE || event.wParam != MK_LBUTTON ||
                !event.inputLParam || *event.inputLParam != parameter) continue;
            ++evidence.matchingTraceCount;
            soleReturned = event.returned && event.enteredThread == GetCurrentThreadId() &&
                event.returnedThread == event.enteredThread && event.returnedSequence > event.enteredSequence;
        }
        evidence.uniqueReturnedMatchingTrace = evidence.matchingTraceCount == 1 && soleReturned;
        if (!evidence.uniqueReturnedMatchingTrace)
            evidence.unresolved.emplace_back("native_endpoint_matching_wndproc_trace_missing_or_ambiguous");
    }
    const auto snapshot = behavior::Access::CaptureNativeDrag(app);
    if (!snapshot.active || snapshot.desktopIdentityUnresolved)
    { evidence.unresolved.emplace_back("native_endpoint_active_desktop_identity_unavailable"); return evidence; }
    evidence.sourceDesktopObserved = snapshot.sourceIsDesktop;
    if (!evidence.sourceDesktopObserved) evidence.unresolved.emplace_back("native_source_desktop_not_observed");
    if (!snapshot.targetObservationAvailable || snapshot.targetUnknown)
        evidence.unresolved.emplace_back("native_target_owner_unresolved");
    else
    {
        evidence.targetDesktopObserved = snapshot.targetIsDesktop;
        evidence.storedTargetRegion = snapshot.targetRegion;
        if (!evidence.targetDesktopObserved) evidence.unresolved.emplace_back("native_target_desktop_not_observed");
        if (snapshot.targetSlotIdentityUnresolved) evidence.unresolved.emplace_back("native_transient_target_slot_identity_unresolved");
    }
    evidence.unresolved.emplace_back("native_endpoint_message_to_snapshot_causality_unverified");
    evidence.unresolved.emplace_back("native_endpoint_device_sample_does_not_prove_wndproc_delivery");
    evidence.unresolved.emplace_back("native_endpoint_page_geometry_and_business_commit_unimplemented");
    evidence.unresolved.emplace_back("native_remaining_endpoint_axes_unimplemented");
    return evidence;
}

// Every candidate axis receives an audit record, including derived axes and
// all generated member/task axes. Invalid or missing entries are obligations
// rather than skips. The caller supplies the complete program, including its
// symbolic and global unresolved reasons, before any production scene is
// created or driven. No supplied program can make CanExecute() true today.
inline Preflight Inspect(const Context& context, const Candidate& candidate, const EventProgram& program)
{
    Preflight result;
    const auto& axes = context.axes;
    result.obligations = context.obligations;
    result.program = program;
    for (const auto& reason : result.program.unresolved)
        result.obligations.push_back("event_unresolved:" + reason);
    for (const auto& step : result.program.steps)
        result.obligations.push_back("event_executor_unimplemented:" + step.kind);
    if (candidate.values.size() != axes.size())
    {
        result.obligations.emplace_back("candidate_axis_count_mismatch");
        return result;
    }
    bool valid = context.obligations.empty();
    // Never trust a lookup supplied for another model or another axis order.
    Candidate canonical = candidate;
    canonical.lookup = context.lookup;
    for (std::size_t i = 0; i < axes.size(); ++i)
    {
        const auto& axis = axes[i];
        const auto& value = candidate.values[i];
        if (axis.role == Role::Derived)
        {
            if (!value.empty())
            {
                result.axes.push_back({axis.id, value, Evidence::unimplemented, "candidate_derived_value_invalid"});
                result.obligations.push_back("candidate_derived_value_invalid:" + axis.id);
                valid = false;
                continue;
            }
            result.axes.push_back({axis.id, value, Evidence::derived_unimplemented, "derived_dependencies_and_observation_unimplemented"});
            result.obligations.push_back("derived:" + axis.id);
            continue;
        }
        if (std::find(axis.values.begin(), axis.values.end(), value) == axis.values.end())
        {
            result.axes.push_back({axis.id, value, Evidence::unimplemented, "candidate_value_missing_or_outside_domain"});
            result.obligations.push_back("invalid_value:" + axis.id);
            valid = false;
            continue;
        }
        if (!PrefixAllows(axes, canonical, axis.id, value))
        {
            result.axes.push_back({axis.id, value, Evidence::unimplemented, "candidate_prefix_invalid"});
            result.obligations.push_back("candidate_prefix_invalid:" + axis.id);
            valid = false;
            continue;
        }
        if (axis.id == "P02.source_actual_count" && value == "zero")
        {
            result.axes.push_back({axis.id, value, Evidence::unimplemented, "empty_shell_file_provider_fixture_unimplemented"});
            result.obligations.push_back("empty_shell_file_provider_fixture_unimplemented:" + axis.id);
            continue;
        }
        if (axis.id == "P02.resolved_count" || axis.id == "P02.many_count" ||
            axis.id == "task_b.P02.resolved_count" || axis.id == "task_b.P02.many_count")
        {
            result.axes.push_back({axis.id, value, Evidence::unimplemented, "production_resolved_count_hypothesis_not_observed"});
            result.obligations.push_back("production_resolved_count_hypothesis_unobserved:" + axis.id);
            continue;
        }
        if (const auto* plan = PlannedInput(axis.id, value, axes, canonical))
        {
            result.axes.push_back({axis.id, value, Evidence::planned_input, plan});
            result.obligations.push_back("construction_and_observation_pending:" + axis.id);
        }
        else
        {
            result.axes.push_back({axis.id, value, Evidence::unimplemented, "axis_value_or_applicability_unimplemented"});
            result.obligations.push_back("axis:" + axis.id);
        }
    }
    if (valid)
    {
        const auto exclusion = StructuralExclusion(axes, canonical);
        if (!exclusion.empty()) result.obligations.push_back("structural_exclusion:" + exclusion);
        const auto expected = drag_global::Program(axes, canonical);
        if (!event_graph::SameEventProgram(program, expected)) result.obligations.emplace_back("program_candidate_mismatch");
    }
    result.obligations.emplace_back("production_host_dispatch_and_business_oracle_unimplemented");
    return result;
}

// A concrete fixture plan, not an execution or an acceptance decision. The
// complete Candidate remains authoritative: the narrow list below only
// selects which existing production fixture could be used if all other axes,
// event identities, completion signals and the independent oracle are closed.
struct DesktopSingleCopyInputs
{
    DWORD enterKeys = MK_LBUTTON;
    DWORD releaseKeys = MK_CONTROL;
    DWORD allowedEffects = DROPEFFECT_COPY | DROPEFFECT_MOVE;
};
inline std::optional<DesktopSingleCopyInputs> DesktopSingleCopyPlan(const Context& context, const Candidate& input)
{
    if (!context.obligations.empty() || input.values.size() != context.axes.size()) return std::nullopt;
    Candidate candidate = input;
    candidate.lookup = context.lookup;
    const auto v = [&](std::string_view id) { return Value(context.axes, candidate, id); };
    if (v("T01") != "external_ingress" || v("source.E01") != "external_unknown" ||
        v("target.E01") != "free_desktop" || v("target.E02") != "none" ||
        v("target.E23") != "four" || v("target.E24") != "three" || v("target.E19") != "named" ||
        v("P01.primary") != "file" || v("P01.second") != "na" ||
        v("P02.source_actual_count") != "one" || v("P02.resolved_count") != "one" || v("P02.many_count") != "na" ||
        v("G01.button") != "unknown" || v("G02.press") != "unknown" ||
        v("G02.enter_buttons") != "left" || v("G02.enter") != "none" ||
        v("G02.hover_first") != "na" || v("G02.hover_first_buttons") != "na" ||
        v("G02.hover_later") != "na" || v("G02.hover_later_buttons") != "na" ||
        v("G02.release_buttons") != "none" || v("G02.release") != "ctrl" || v("G04.termination") != "drop" ||
        v("T03") != "copy_move" || v("T03.extra_protocol_bits") != "none" || v("T04.capability") != "enabled" ||
        v("C07") != "on" || v("X02") != "none" || v("X02.position") != "no_interruption" ||
        v("H01.region") != "empty_slot" || v("R04.member0_target_exists") != "absent" ||
        v("R04.member0_queued_reservation") != "absent" || v("R04.member0_concurrent_creation") != "none" ||
        v("X03.task_count") != "one" || v("X03.submit_order") != "na" || v("X03.order") != "single")
        return std::nullopt;
    return DesktopSingleCopyInputs{};
}

// A read-only view of the actual generated fixture. A matching snapshot does
// not establish all candidate inputs, make a filesystem snapshot atomic, or
// authorize any drag. The caller must keep `fixture` alive while using it.
struct GeneratedDesktopCopyBinding
{
    bool snapshotMatches = false;
    behavior::GeneratedPayloadProfile* fixture = nullptr;
    std::filesystem::path source;
    std::filesystem::path target;
    std::vector<std::string> obligations;
};
inline bool SamePayloadEndpointSnapshot(const drag_global::PayloadInputSnapshot& a,
    const drag_global::PayloadInputSnapshot& b)
{
    return a.targetRelative == b.targetRelative && a.desktopRelative == b.desktopRelative &&
        a.sourceMappingRelative == b.sourceMappingRelative && a.targetMappingRelative == b.targetMappingRelative &&
        a.desktopEntryIndex == b.desktopEntryIndex && a.sourceMappingEntryIndex == b.sourceMappingEntryIndex &&
        a.targetMappingEntryIndex == b.targetMappingEntryIndex;
}
inline GeneratedDesktopCopyBinding InspectGeneratedDesktopCopy(const Context& context,
    const Candidate& candidate, behavior::GeneratedPayloadProfile& fixture)
{
    GeneratedDesktopCopyBinding result;
    for (const auto& reason : fixture.payload.unresolved)
        result.obligations.push_back("generated_payload_unresolved:" + reason);
    if (!DesktopSingleCopyPlan(context, candidate))
    { result.obligations.emplace_back("desktop_single_copy_plan_unimplemented"); return result; }
    if (!fixture.profile || !fixture.profile->app || !fixture.payload.payload_prepared || !fixture.after.captured ||
        fixture.payload.members.size() != 1 || fixture.payload.members.front().kind != "file" ||
        fixture.payload.axes.size() != context.axes.size() || candidate.values.size() != context.axes.size())
    { result.obligations.emplace_back("generated_fixture_shape_unimplemented"); return result; }
    for (std::size_t i = 0; i < context.axes.size(); ++i)
        if (fixture.payload.axes[i].id != context.axes[i].id || fixture.payload.axes[i].value != candidate.values[i])
        { result.obligations.push_back("generated_fixture_candidate_mismatch:" + context.axes[i].id); return result; }
    std::error_code ec;
    if (!std::filesystem::equivalent(fixture.profile->desktop, fixture.payload.desktopDirectory, ec) || ec)
    { result.obligations.emplace_back("generated_desktop_binding_mismatch"); return result; }
    ec.clear();
    if (!std::filesystem::equivalent(fixture.payload.desktopDirectory, fixture.payload.targetDirectory, ec) || ec)
    { result.obligations.emplace_back("generated_target_binding_mismatch"); return result; }
    const auto current = drag_global::CapturePayloadInputSnapshot(fixture.payload);
    if (!current.captured || current.root != fixture.after.root || !SamePayloadEndpointSnapshot(current, fixture.after) ||
        current.entries.size() != fixture.after.entries.size() || current.memberEntryIndices != fixture.after.memberEntryIndices)
    { result.obligations.emplace_back("generated_input_snapshot_changed_or_unavailable"); return result; }
    for (std::size_t i = 0; i < current.entries.size(); ++i)
    {
        const auto& observed = current.entries[i];
        const auto& baseline = fixture.after.entries[i];
        if (observed.relative != baseline.relative || observed.kind != baseline.kind || observed.bytes != baseline.bytes ||
            observed.volumeSerial != baseline.volumeSerial || observed.fileIndexHigh != baseline.fileIndexHigh ||
            observed.fileIndexLow != baseline.fileIndexLow)
        { result.obligations.emplace_back("generated_input_snapshot_changed"); return result; }
    }
    if (current.memberEntryIndices.size() != 1 || current.memberEntryIndices.front() >= current.entries.size() ||
        current.entries[current.memberEntryIndices.front()].kind != "file")
    { result.obligations.emplace_back("generated_source_identity_unavailable"); return result; }
    result.snapshotMatches = true;
    result.fixture = &fixture;
    result.source = fixture.payload.members.front().path;
    result.target = fixture.profile->desktop;
    result.obligations.emplace_back("non_atomic_snapshot_does_not_close_all_candidate_axes");
    return result;
}

// The fixture is owned before COM members so it is destroyed after them.
// Keep this object and use/destroy it on the profile's UI/STA thread. The
// caller must still honor all obligations; preparation never permits a drag.
struct GeneratedOleInput
{
    std::shared_ptr<behavior::GeneratedPayloadProfile> fixture;
    Microsoft::WRL::ComPtr<ole_file_source_fixture::Source> source;
    Microsoft::WRL::ComPtr<OleDragDropAdapter> adapter;
    std::vector<std::filesystem::path> requestedPaths;
    std::string candidateId;
    std::string graphId;
    DWORD ownerThread = 0;
    bool prepared = false;
    std::vector<std::string> obligations;
    bool CanExecute() const { return false; }
};

inline GeneratedOleInput PrepareGeneratedOleInput(const Context& context, const Candidate& input,
    const EventProgram& program, const event_graph::Variant& selectedGraph,
    std::shared_ptr<behavior::GeneratedPayloadProfile> fixture)
{
    GeneratedOleInput result;
    result.fixture = std::move(fixture);
    const auto audit = Inspect(context, input, program);
    result.obligations = audit.obligations;
    if (result.fixture)
        for (const auto& reason : result.fixture->payload.unresolved)
            result.obligations.push_back("generated_payload_unresolved:" + reason);
    result.obligations.emplace_back("candidate_axis_event_and_business_preflight_unimplemented");
    std::vector<Axis> schema;
    Candidate candidate;
    if (const auto reason = drag_global::ValidatePayloadCandidate(context.axes, input, schema, candidate); !reason.empty())
    { result.obligations.push_back("generated_candidate:" + reason); return result; }
    const auto v = [&](std::string_view id) { return Value(schema, candidate, id); };
    if (v("T01") != "external_ingress" ||
        (v("T04.capability") != "enabled" && v("T04.capability") != "present_disabled"))
    { result.obligations.emplace_back("generated_ole_source_capability_unimplemented"); return result; }
    if (!result.fixture || !result.fixture->profile || !result.fixture->profile->app ||
        !result.fixture->payload.payload_prepared || !result.fixture->after.captured ||
        result.fixture->payload.members.empty() || result.fixture->payload.axes.size() != schema.size())
    { result.obligations.emplace_back("generated_ole_fixture_unavailable"); return result; }
    for (std::size_t i = 0; i < schema.size(); ++i)
        if (result.fixture->payload.axes[i].id != schema[i].id || result.fixture->payload.axes[i].value != candidate.values[i])
        { result.obligations.push_back("generated_ole_candidate_mismatch:" + schema[i].id); return result; }

    const event_graph::Context graphContext(schema);
    const auto mapping = event_graph::MapSimpleExternal(graphContext, candidate, program);
    if (!mapping.graphAvailable)
    {
        result.obligations.emplace_back("generated_ole_graph_mapping_unimplemented");
        result.obligations.insert(result.obligations.end(), mapping.reasons.begin(), mapping.reasons.end());
        return result;
    }
    const auto replay = event_graph::ReplayVariant(graphContext, candidate, mapping.spec, selectedGraph.id);
    if (!replay.identityValid)
    {
        result.obligations.emplace_back("generated_ole_graph_identity_invalid");
        result.obligations.insert(result.obligations.end(), replay.errors.begin(), replay.errors.end());
        return result;
    }
    result.obligations.insert(result.obligations.end(), replay.variant.unresolved.begin(), replay.variant.unresolved.end());
    result.graphId = replay.variant.id;
    result.candidateId = Id(schema, candidate);

    auto& profile = *result.fixture->profile;
    result.ownerThread = GetCurrentThreadId();
    if (!behavior::Access::completionWindow || behavior::Access::asyncOwner != profile.app ||
        GetWindowThreadProcessId(behavior::Access::completionWindow, nullptr) != result.ownerThread)
    { result.obligations.emplace_back("generated_ole_profile_thread_or_owner_mismatch"); return result; }
    std::error_code ec;
    if (!std::filesystem::equivalent(profile.desktop, result.fixture->payload.desktopDirectory, ec) || ec)
    { result.obligations.emplace_back("generated_ole_host_desktop_mismatch"); return result; }
    ec.clear();
    if (!std::filesystem::equivalent(result.fixture->payload.desktopDirectory, result.fixture->payload.targetDirectory, ec) || ec)
    { result.obligations.emplace_back("generated_ole_host_target_mismatch"); return result; }
    const auto current = drag_global::CapturePayloadInputSnapshot(result.fixture->payload);
    const auto& baseline = result.fixture->after;
    if (!current.captured || current.root != baseline.root || !SamePayloadEndpointSnapshot(current, baseline) ||
        current.entries.size() != baseline.entries.size() || current.memberEntryIndices != baseline.memberEntryIndices)
    { result.obligations.emplace_back("generated_ole_input_snapshot_changed_or_unavailable"); return result; }
    for (std::size_t i = 0; i < current.entries.size(); ++i)
    {
        const auto& actual = current.entries[i]; const auto& before = baseline.entries[i];
        if (actual.relative != before.relative || actual.kind != before.kind || actual.bytes != before.bytes ||
            actual.volumeSerial != before.volumeSerial || actual.fileIndexHigh != before.fileIndexHigh ||
            actual.fileIndexLow != before.fileIndexLow)
        { result.obligations.emplace_back("generated_ole_input_snapshot_changed"); return result; }
    }
    if (current.memberEntryIndices.size() != result.fixture->payload.members.size())
    { result.obligations.emplace_back("generated_ole_member_snapshot_missing"); return result; }
    for (std::size_t i = 0; i < result.fixture->payload.members.size(); ++i)
    {
        if (current.memberEntryIndices[i] >= current.entries.size())
        { result.obligations.emplace_back("generated_ole_member_identity_missing"); return result; }
        result.requestedPaths.push_back(result.fixture->payload.members[i].path);
    }
    // SHCreateDataObject uses one parent PIDL. Do not fabricate cross-parent
    // Shell children; that needs a separately specified provider adapter.
    const auto parent = result.requestedPaths.front().parent_path();
    for (const auto& path : result.requestedPaths)
    {
        ec.clear();
        if (!std::filesystem::equivalent(parent, path.parent_path(), ec) || ec)
        { result.obligations.emplace_back("generated_ole_multiple_parents_unimplemented"); return result; }
    }
    try
    {
        result.source = ole_file_source_fixture::Make(result.requestedPaths, v("T04.capability") == "enabled");
        result.adapter = behavior::Access::ExternalAdapter(*profile.app);
        if (!result.source || !result.adapter)
        { result.obligations.emplace_back("generated_ole_provider_or_adapter_unavailable"); return result; }
    }
    catch (const std::exception& error)
    { result.obligations.push_back(std::string("generated_ole_construction_failed:") + error.what()); return result; }
    catch (...) { result.obligations.emplace_back("generated_ole_construction_failed"); return result; }
    // SHCreateDataObject may normalize order or duplicates. Only future actual
    // production GetData observations can establish the delivered sequence.
    result.obligations.emplace_back("shell_delivered_payload_order_and_identity_unobserved");
    result.obligations.emplace_back("generated_ole_event_driver_completion_and_oracle_unimplemented");
    result.prepared = true;
    return result;
}

// V2 preflight is deliberately separate from the legacy execution bridge. The
// caller owns the already prepared profile, source, initial observation and
// callbacks on their owning STA. This function never dispatches a drag and
// cannot turn a presence or resolved-count hypothesis into fixture input.
struct V2DesktopCopyPreflight
{
    bool graphIdentityValid = false;
    bool sourceInitialMatches = false;
    std::vector<std::string> obligations;
    std::vector<std::string> notApplicable;
    bool CanExecute() const { return false; }
};

struct V2DesktopCoordinateEvidence
{
    bool ownerWindowMatched = false;
    bool coordinateRoundTripMatched = false;
    bool oleInputPointMatched = false;
    bool productionSessionPointMatched = false;
    bool productionDesktopTargetObserved = false;
    POINT client{};
    POINTL screen{};
    std::vector<std::string> unresolved;
};

// Observe only the isolated host window and candidate-bound input geometry.
// Production target selection is not queried or reproduced during preflight.
inline V2DesktopCoordinateEvidence InspectV2DesktopCoordinateInput(const event_graph::Context& context,
    const Candidate& input, const behavior::GeneratedPayloadProfile& fixture,
    const std::vector<ole_driver::Input>& inputs)
{
    V2DesktopCoordinateEvidence result;
    if (context.version != SelectionSchemaVersion::source_members_v2 || !event_graph::RevalidateContext(context).empty())
    { result.unresolved.emplace_back("v2_coordinate_context_unavailable"); return result; }
    std::vector<Axis> schema; Candidate candidate;
    if (const auto reason = ValidatePayloadCandidate(context.axes, input, schema, candidate,
        SelectionSchemaVersion::source_members_v2); !reason.empty())
    { result.unresolved.push_back("v2_coordinate_candidate_unavailable:" + reason); return result; }
    if (fixture.version != SelectionSchemaVersion::source_members_v2 || !fixture.profile || !fixture.profile->app ||
        fixture.payload.axes.size() != schema.size())
    { result.unresolved.emplace_back("v2_coordinate_fixture_unavailable"); return result; }
    for (std::size_t i = 0; i < schema.size(); ++i)
        if (fixture.payload.axes[i].id != schema[i].id || fixture.payload.axes[i].value != candidate.values[i])
        { result.unresolved.push_back("v2_coordinate_fixture_candidate_mismatch:" + schema[i].id); return result; }
    const auto v = [&](std::string_view id) { return Value(schema, candidate, id); };
    if (v("target.E01") != "free_desktop" || v("target.E02") != "none" ||
        v("target.E23") != "four" || v("target.E24") != "three" || v("H01.region") != "empty_slot")
    { result.unresolved.emplace_back("v2_coordinate_candidate_geometry_unimplemented"); return result; }
    auto& app = *fixture.profile->app;
    const auto window = behavior::Access::HostWindow(app);
    DWORD process = 0;
    if (!window || window != behavior::Access::completionWindow || !IsWindow(window) ||
        behavior::Access::asyncOwner != &app || behavior::Access::productionDispatch != 1 ||
        GetWindowThreadProcessId(window, &process) != GetCurrentThreadId() || process != GetCurrentProcessId() ||
        reinterpret_cast<DesktopApp*>(GetWindowLongPtrW(window, GWLP_USERDATA)) != &app)
    { result.unresolved.emplace_back("v2_coordinate_live_host_window_unavailable"); return result; }
    result.ownerWindowMatched = true;
    result.client = {150, 150};
    POINT screen = result.client;
    if (!ClientToScreen(window, &screen))
    { result.unresolved.emplace_back("v2_coordinate_client_to_screen_unavailable"); return result; }
    POINT roundTrip = screen;
    if (!ScreenToClient(window, &roundTrip) || roundTrip.x != result.client.x || roundTrip.y != result.client.y)
    { result.unresolved.emplace_back("v2_coordinate_screen_to_client_mismatch"); return result; }
    result.screen = {screen.x, screen.y};
    result.coordinateRoundTripMatched = true;
    bool sawInput = false, matches = true;
    for (const auto& event : inputs)
        if (event.kind != ole_driver::Kind::leave)
        {
            sawInput = true;
            if (!event.screenPoint || event.screenPoint->x != screen.x || event.screenPoint->y != screen.y) matches = false;
        }
    result.oleInputPointMatched = sawInput && matches;
    if (!result.oleInputPointMatched) result.unresolved.emplace_back("v2_coordinate_ole_input_point_unavailable");
    result.unresolved.emplace_back("v2_coordinate_production_target_observation_awaits_real_enter");
    result.unresolved.emplace_back("v2_coordinate_scene_and_remaining_geometry_axes_unimplemented");
    return result;
}

// Call immediately on the owning UI thread when the production Enter return
// is delivered. This copies the live session point and target only; the
// caller must still prove that this snapshot and event belong to one executor
// receipt without reentrant interference before treating it as case evidence.
inline V2DesktopCoordinateEvidence ObserveV2DesktopEnter(const event_graph::Context& context,
    const Candidate& candidate, const behavior::GeneratedPayloadProfile& fixture,
    const std::vector<ole_driver::Input>& inputs, const ole_driver::Observation& event)
{
    V2DesktopCoordinateEvidence result = InspectV2DesktopCoordinateInput(context, candidate, fixture, inputs);
    if (!fixture.profile || !fixture.profile->app)
    { result.unresolved.emplace_back("v2_coordinate_enter_host_unavailable"); return result; }
    auto& app = *fixture.profile->app;
    const auto window = behavior::Access::HostWindow(app);
    if (!result.ownerWindowMatched || !result.coordinateRoundTripMatched || !result.oleInputPointMatched ||
        behavior::Access::asyncOwner != &app || behavior::Access::productionDispatch != 1 ||
        window != behavior::Access::completionWindow || !window || !IsWindow(window) ||
        GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId() ||
        reinterpret_cast<DesktopApp*>(GetWindowLongPtrW(window, GWLP_USERDATA)) != &app ||
        event.kind != ole_driver::Kind::enter || !event.screenPoint || event.screenPoint->x != result.screen.x ||
        event.screenPoint->y != result.screen.y || !behavior::Access::SessionActive(app))
    { result.unresolved.emplace_back("v2_coordinate_enter_snapshot_unavailable"); return result; }
    const auto actual = behavior::Access::SessionPoint(app);
    result.productionSessionPointMatched = actual.x == result.client.x && actual.y == result.client.y;
    if (!result.productionSessionPointMatched) result.unresolved.emplace_back("v2_coordinate_production_session_point_mismatch");
    const auto desktop = behavior::Access::DesktopOrNull(app);
    if (!desktop)
    { result.unresolved.emplace_back("v2_coordinate_desktop_container_unavailable"); return result; }
    result.productionDesktopTargetObserved = behavior::Access::Target(app) == desktop &&
        behavior::Access::Region(app) == HitRegion::Empty;
    if (!result.productionDesktopTargetObserved) result.unresolved.emplace_back("v2_coordinate_production_target_not_observed");
    else result.unresolved.erase(std::remove(result.unresolved.begin(), result.unresolved.end(),
        "v2_coordinate_production_target_observation_awaits_real_enter"), result.unresolved.end());
    result.unresolved.emplace_back("v2_coordinate_event_receipt_and_reentrancy_binding_unimplemented");
    return result;
}

// Adapter-side sampler for V2GraphExecutor::RunBound. Only copied values are
// returned: no Slot pointer is retained and no target is recomputed. The
// driver may seal this as a raw sample, but a caller-supplied sampler cannot
// itself prove its provenance, nor rule out non-adapter or cross-thread work.
inline ole_driver::V2HostCoordinateSnapshot CaptureV2DesktopHostCoordinate(const event_graph::Context& context,
    const Candidate& candidate, const behavior::GeneratedPayloadProfile& fixture,
    const std::vector<ole_driver::Input>& inputs, const ole_driver::Observation& event)
{
    ole_driver::V2HostCoordinateSnapshot snapshot;
    const auto evidence = ObserveV2DesktopEnter(context, candidate, fixture, inputs, event);
    snapshot.unresolved = evidence.unresolved;
    if (!fixture.profile || !fixture.profile->app || !evidence.ownerWindowMatched ||
        !behavior::Access::SessionActive(*fixture.profile->app))
    { snapshot.unresolved.emplace_back("v2_host_snapshot_live_session_unavailable"); return snapshot; }
    const auto& app = *fixture.profile->app;
    snapshot.window = behavior::Access::HostWindow(app);
    snapshot.sessionPoint = behavior::Access::SessionPoint(app);
    snapshot.target = reinterpret_cast<std::uintptr_t>(behavior::Access::Target(app));
    snapshot.region = static_cast<int>(behavior::Access::Region(app));
    snapshot.thread = GetCurrentThreadId();
    snapshot.unresolved.emplace_back("v2_host_snapshot_sampler_provenance_and_bypass_reentrancy_unresolved");
    return snapshot;
}

enum class V2PreEvidenceState { proven, unimplemented, invalid, not_applicable };
enum class V2PostEvidenceState { pending, matched, hypothesis_not_met, observation_unavailable, product_failed, not_applicable };
struct V2PreEvidence { std::string key; V2PreEvidenceState state; std::string detail; };
struct V2PostEvidence { std::string key; V2PostEvidenceState state; std::string detail; };

// Immutable, candidate-bound bookkeeping only. No caller-supplied boolean,
// configured fixture, callback, or graph preview proves a capability. Source,
// host and execution epoch are deliberately still unbound; this object cannot
// authorize dispatch and has no mutation API for manufacturing observations.
class V2CaseEvidence
{
public:
    V2CaseEvidence(const V2CaseEvidence&) = default;
    V2CaseEvidence(V2CaseEvidence&&) = default;
    V2CaseEvidence& operator=(const V2CaseEvidence&) = default;
    V2CaseEvidence& operator=(V2CaseEvidence&&) = default;
    static V2CaseEvidence Prepare(const event_graph::Context& context, const Candidate& input,
        const EventProgram& program, const std::vector<bool>& present,
        const event_graph::PresenceActiveGraphVariant& selected)
    {
        V2CaseEvidence result;
        result.errors_ = event_graph::RevalidateContext(context);
        if (context.version != SelectionSchemaVersion::source_members_v2)
            result.errors_.emplace_back("v2_case_schema_version_required");
        if (!result.errors_.empty()) return result;
        std::vector<Axis> schema;
        Candidate candidate;
        if (const auto reason = ValidatePayloadCandidate(context.axes, input, schema, candidate,
            SelectionSchemaVersion::source_members_v2); !reason.empty())
        { result.errors_.push_back("v2_case_candidate_invalid:" + reason); return result; }
        const auto mapping = event_graph::MapSimpleExternal(context, candidate, program);
        result.diagnostics_ = mapping.reasons;
        if (!mapping.structuralExclusion.empty())
            result.errors_.push_back("v2_case_structural_exclusion:" + mapping.structuralExclusion);
        if (!mapping.graphAvailable)
        { result.errors_.emplace_back("v2_case_graph_mapping_unavailable"); return result; }
        const auto replay = event_graph::PresenceActiveGraph::Replay(context, candidate, mapping.spec, present, selected.id);
        result.errors_.insert(result.errors_.end(), replay.errors.begin(), replay.errors.end());
        if (!replay.structuralExclusion.empty())
            result.errors_.push_back("v2_case_graph_structural_exclusion:" + replay.structuralExclusion);
        if (!replay.identityValid || selected.id != replay.variant.id || selected.classes != replay.variant.classes ||
            selected.order != replay.variant.order || selected.unresolved != replay.variant.unresolved)
            result.errors_.emplace_back("v2_case_graph_identity_invalid");
        const auto presence = event_graph::PreparePresenceGraph(context, candidate, mapping.spec);
        result.errors_.insert(result.errors_.end(), presence.errors.begin(), presence.errors.end());
        if (!presence.structuralExclusion.empty() || present.size() != presence.requirements.size())
            result.errors_.emplace_back("v2_case_presence_schema_invalid");
        if (!result.errors_.empty()) return result;

        result.schemaBinding_ = context.schemaBinding;
        result.candidateId_ = Id(schema, candidate);
        result.graphId_ = selected.id;
        result.presence_ = present;
        result.program_ = program;
        for (const auto& axis : schema)
        {
            const auto value = Value(schema, candidate, axis.id);
            const bool absentB = axis.id.substr(0, 7) == "task_b." &&
                Value(schema, candidate, "X03.task_count") == "one" &&
                (axis.role == Role::Derived ? value.empty() : value == "na");
            if (absentB)
            {
                if (axis.role == Role::Derived || axis.role == Role::Hypothesis)
                    result.post_.push_back({"outcome:" + axis.id, V2PostEvidenceState::not_applicable,
                        "single_task_structural_absence"});
                else result.pre_.push_back({"axis:" + axis.id, V2PreEvidenceState::not_applicable,
                    "single_task_structural_absence"});
            }
            else if (axis.role == Role::Hypothesis || axis.role == Role::Derived)
                result.post_.push_back({"outcome:" + axis.id, V2PostEvidenceState::pending,
                    "requires_same_execution_production_observation"});
            else
                result.pre_.push_back({"axis:" + axis.id, V2PreEvidenceState::unimplemented,
                    "candidate_bound_construction_and_observation_not_proven"});
        }
        for (std::size_t i = 0; i < presence.requirements.size(); ++i)
            if (presence.requirements[i].presence == event_graph::PresencePolicy::conditional_unresolved)
                result.post_.push_back({"presence:" + presence.requirements[i].id, V2PostEvidenceState::pending,
                    "requires_complete_same_execution_observation_interval"});
        for (std::size_t i = 0; i < program.steps.size(); ++i)
            result.post_.push_back({"program_step:" + std::to_string(i), V2PostEvidenceState::pending,
                "requires_actual_same_execution_event_and_parameters"});
        for (const auto* key : {"fixture_source_host_epoch_identity", "source_payload_and_protocol_capability",
            "graph_dispatch_and_presence_observation_capability", "completion_and_independent_oracle_capability"})
            result.pre_.push_back({key, V2PreEvidenceState::unimplemented, "same_instance_capability_not_proven"});
        for (const auto* key : {"source_read_and_returned_data", "actual_terminal_and_completion",
            "independent_file_model_and_persistence_oracle"})
            result.post_.push_back({key, V2PostEvidenceState::pending, "same_execution_evidence_not_observed"});
        result.valid_ = true;
        return result;
    }
    // Check exact OLE inputs against the same candidate and selected graph.
    // This proves only a symbolic construction plan; a later dispatch must
    // bind the same live objects and verify each real event independently.
    static V2CaseEvidence WithOleInputs(const V2CaseEvidence& bound, const event_graph::Context& context,
        const Candidate& candidate, const EventProgram& program, const std::vector<bool>& present,
        const event_graph::PresenceActiveGraphVariant& selected, const std::vector<ole_driver::Input>& inputs)
    {
        auto result = Prepare(context, candidate, program, present, selected);
        if (!bound.Valid() || !result.Valid() || bound.SchemaBinding() != result.SchemaBinding() ||
            bound.CandidateId() != result.CandidateId() || bound.GraphId() != result.GraphId() ||
            bound.Presence() != result.Presence())
        { result.valid_ = false; result.errors_.emplace_back("v2_ole_input_case_binding_invalid"); return result; }
        const auto& before = bound.Program();
        if (!event_graph::SameEventProgram(before, program))
        { result.valid_ = false; result.errors_.emplace_back("v2_ole_input_program_binding_invalid"); return result; }
        const ole_driver::HostBoundaries noHost{};
        const auto plan = ole_driver::BuildV2OleExecutionPlan(context, candidate, program, present, selected,
            inputs, nullptr, nullptr, noHost);
        if (!plan.structurallyPlanned)
        {
            result.pre_.push_back({"ole_program_input_plan", V2PreEvidenceState::unimplemented,
                "selected_graph_or_exact_program_inputs_not_plannable"});
            result.diagnostics_.insert(result.diagnostics_.end(), plan.obligations.begin(), plan.obligations.end());
            return result;
        }
        result.inputs_ = inputs;
        result.pre_.push_back({"ole_program_input_plan", V2PreEvidenceState::proven,
            "candidate_and_graph_bound_values_only_not_live_source_or_dispatch"});
        result.diagnostics_.insert(result.diagnostics_.end(), plan.obligations.begin(), plan.obligations.end());
        return result;
    }
    bool Valid() const noexcept { return valid_; }
    bool CanDispatch() const noexcept { return false; }
    const std::string& SchemaBinding() const noexcept { return schemaBinding_; }
    const std::string& CandidateId() const noexcept { return candidateId_; }
    const std::string& GraphId() const noexcept { return graphId_; }
    const std::vector<bool>& Presence() const noexcept { return presence_; }
    const EventProgram& Program() const noexcept { return program_; }
    const std::vector<ole_driver::Input>& OleInputs() const noexcept { return inputs_; }
    const std::vector<V2PreEvidence>& Pre() const noexcept { return pre_; }
    const std::vector<V2PostEvidence>& Post() const noexcept { return post_; }
    const std::vector<std::string>& Errors() const noexcept { return errors_; }
    const std::vector<std::string>& Diagnostics() const noexcept { return diagnostics_; }
private:
    V2CaseEvidence() = default;
    bool valid_ = false;
    std::string schemaBinding_, candidateId_, graphId_;
    std::vector<bool> presence_;
    EventProgram program_;
    std::vector<ole_driver::Input> inputs_;
    std::vector<V2PreEvidence> pre_;
    std::vector<V2PostEvidence> post_;
    std::vector<std::string> errors_, diagnostics_;
};

// A partial postflight audit, never an execution certificate. It revalidates
// the complete model/graph binding before reading the source's existing logs;
// it does not call GetData, infer host completion or manufacture matches.
// A trustworthy same-execution host/epoch binding is not implemented yet, so
// model hypotheses and the independent product oracle remain unavailable.
class V2CasePostflightEvidence
{
public:
    V2CasePostflightEvidence(const V2CasePostflightEvidence&) = default;
    V2CasePostflightEvidence(V2CasePostflightEvidence&&) = default;
    V2CasePostflightEvidence& operator=(const V2CasePostflightEvidence&) = default;
    V2CasePostflightEvidence& operator=(V2CasePostflightEvidence&&) = default;
    static V2CasePostflightEvidence Inspect(const V2CaseEvidence& bound,
        const event_graph::Context& context, const Candidate& candidate, const EventProgram& program,
        const std::vector<bool>& present, const event_graph::PresenceActiveGraphVariant& selected,
        const behavior::GeneratedPayloadProfile* fixture, ole_file_source_fixture::Source* source,
        const ole_driver::FileSourceInitialObservation* initial)
    {
        V2CasePostflightEvidence result;
        try
        {
        const auto current = V2CaseEvidence::Prepare(context, candidate, program, present, selected);
        if (!bound.Valid() || !current.Valid() || bound.SchemaBinding() != current.SchemaBinding() ||
            bound.CandidateId() != current.CandidateId() || bound.GraphId() != current.GraphId() ||
            bound.Presence() != current.Presence())
        {
            result.obligations_.emplace_back("v2_postflight_case_binding_unavailable");
            result.obligations_.insert(result.obligations_.end(), current.Errors().begin(), current.Errors().end());
            return result;
        }
        const auto& a = bound.Program(); const auto& b = current.Program();
        if (!event_graph::SameEventProgram(a, b))
        { result.obligations_.emplace_back("v2_postflight_program_binding_unavailable"); return result; }
        result.post_ = bound.Post();
        for (auto& item : result.post_)
            if (item.state == V2PostEvidenceState::pending)
            {
                item.state = V2PostEvidenceState::observation_unavailable;
                item.detail = "same_execution_host_epoch_and_completion_not_bound";
            }
        result.obligations_.emplace_back("v2_postflight_host_epoch_and_completion_identity_unavailable");
        result.obligations_.emplace_back("v2_postflight_independent_business_oracle_unavailable");
        if (!fixture || fixture->version != SelectionSchemaVersion::source_members_v2 || !fixture->profile ||
            !fixture->profile->app || !source || !initial)
        { result.obligations_.emplace_back("v2_postflight_fixture_source_or_initial_unavailable"); return result; }
        try
        {
            result.source_ = ole_driver::InspectV2SourceAndPassive(context.axes, candidate, fixture->payload,
                *initial, *source);
            for (const auto& reason : result.source_->source.obligations)
                result.obligations_.push_back("v2_postflight_source_partial:" + reason);
            for (const auto& reason : result.source_->obligations)
                result.obligations_.push_back("v2_postflight_passive_partial:" + reason);
            result.obligations_.emplace_back("v2_postflight_source_log_not_bound_to_host_epoch_or_completion");
        }
        catch (const std::exception& error)
        { result.obligations_.push_back(std::string("v2_postflight_source_observation_unavailable:") + error.what()); }
        catch (...) { result.obligations_.emplace_back("v2_postflight_source_observation_unavailable:unknown_exception"); }
        }
        catch (const std::exception& error)
        { result.obligations_.push_back(std::string("v2_postflight_observation_unavailable:") + error.what()); }
        catch (...) { result.obligations_.emplace_back("v2_postflight_observation_unavailable:unknown_exception"); }
        return result;
    }
    bool CanReportPass() const noexcept { return false; }
    const std::vector<V2PostEvidence>& Post() const noexcept { return post_; }
    const std::optional<ole_driver::V2SourceAndPassiveEvidence>& SourcePartial() const noexcept { return source_; }
    const std::vector<std::string>& Obligations() const noexcept { return obligations_; }
private:
    V2CasePostflightEvidence() = default;
    std::vector<V2PostEvidence> post_;
    std::optional<ole_driver::V2SourceAndPassiveEvidence> source_;
    std::vector<std::string> obligations_;
};

inline V2DesktopCopyPreflight InspectV2DesktopSingleCopy(const event_graph::Context& context,
    const Candidate& input, const EventProgram& program, const std::vector<bool>& present,
    const event_graph::PresenceActiveGraphVariant& selectedGraph, const std::vector<ole_driver::Input>& oleInputs,
    behavior::GeneratedPayloadProfile* fixture, ole_file_source_fixture::Source* source,
    const ole_driver::FileSourceInitialObservation* initial, OleDragDropAdapter* adapter,
    const ole_driver::HostBoundaries& host)
{
    V2DesktopCopyPreflight result;
    const auto graph = ole_driver::InspectV2OleGraph(context, input, program, present, selectedGraph,
        oleInputs, adapter, source, host);
    result.graphIdentityValid = graph.identityValid;
    result.obligations = graph.obligations;
    result.notApplicable = graph.notApplicable;
    result.obligations.emplace_back("v2_desktop_copy_complete_axis_and_execution_bridge_unimplemented");
    result.obligations.emplace_back("v2_desktop_copy_source_to_shell_and_host_identity_unobserved");
    result.obligations.emplace_back("v2_desktop_copy_resolved_count_and_business_outcomes_unobserved");
    auto contextErrors = event_graph::RevalidateContext(context);
    result.obligations.insert(result.obligations.end(), contextErrors.begin(), contextErrors.end());
    if (!contextErrors.empty() || context.version != SelectionSchemaVersion::source_members_v2)
    { result.obligations.emplace_back("v2_desktop_copy_context_unavailable"); return result; }
    std::vector<Axis> schema;
    Candidate candidate;
    if (const auto reason = ValidatePayloadCandidate(context.axes, input, schema, candidate,
        SelectionSchemaVersion::source_members_v2); !reason.empty())
    { result.obligations.push_back("v2_desktop_copy_candidate_unavailable:" + reason); return result; }
    const auto value = [&](std::string_view id) { return Value(schema, candidate, id); };
    // This is a fixture support boundary only; outcome hypotheses do not
    // select or construct the Shell source.
    if (!SelectionV2HasExplicitFileSource(schema, candidate) || SelectionSourceSize(schema, candidate) != 1 ||
        value("P01.primary") != "file" || value("P01.second") != "na" ||
        value("T01") != "external_ingress" || value("target.E01") != "free_desktop" ||
        value("T04.capability") != "enabled")
        result.obligations.emplace_back("v2_desktop_single_file_copy_fixture_plan_unimplemented");
    if (!fixture || fixture->version != SelectionSchemaVersion::source_members_v2 || !fixture->profile ||
        !fixture->profile->app || !source || !initial)
    { result.obligations.emplace_back("v2_desktop_copy_fixture_source_or_initial_missing"); return result; }
    for (const auto& reason : fixture->payload.unresolved)
        result.obligations.push_back("generated_payload_unresolved:" + reason);
    std::error_code ec;
    if (!std::filesystem::equivalent(fixture->profile->desktop, fixture->payload.desktopDirectory, ec) || ec)
        result.obligations.emplace_back("v2_generated_desktop_binding_unavailable");
    ec.clear();
    if (!std::filesystem::equivalent(fixture->payload.desktopDirectory, fixture->payload.targetDirectory, ec) || ec)
        result.obligations.emplace_back("v2_generated_target_binding_unavailable");
    // The helper validates the private snapshot against the same source and
    // candidate without requesting data from the provider.
    const auto checked = ole_driver::ValidateSingleFileSourceInitial(schema, candidate, fixture->payload,
        fixture->after, *source, *initial, SelectionSchemaVersion::source_members_v2);
    result.sourceInitialMatches = checked.matches;
    result.obligations.insert(result.obligations.end(), checked.obligations.begin(), checked.obligations.end());
    return result;
}

enum class BridgeStatus { unimplemented, production_failed, production_observed };
struct BridgeResult
{
    struct V4TextProgramCollectionInput
    {
        std::string candidateId;
        std::uintptr_t hostIdentity = 0;
        DWORD ownerThread = 0;
        bool actual = false, candidateMatched = false;
        std::vector<std::size_t> matchingPreCallProgramIndices;
    };
    struct V4TextDesktopInput
    {
        std::string candidateId;
        std::uintptr_t hostIdentity = 0;
        DWORD ownerThread = 0;
        behavior::Access::TextDesktopInitial initial;
        bool candidateMatched = false;
        std::vector<std::size_t> matchingPreCallProgramIndices;
    };
    struct NativeQueuedInput
    {
        ole_driver::NativeDevicePhase phase;
        std::optional<std::size_t> programIndex;
        snowdesktop::private_pointer::QueuedEventIdentity token;
    };
    struct V4NativeDesktopGeometryInput
    {
        std::string candidateId;
        std::uintptr_t hostIdentity = 0;
        DWORD ownerThread = 0;
        behavior::Access::NativeDesktopSourceGeometry source;
        behavior::Access::NativeLandingGeometry landing;
        bool candidateMatched = false;
    };
    struct V4FileOleInput
    {
        std::size_t programIndex = 0;
        ole_driver::Kind kind = ole_driver::Kind::enter;
        std::string buttons, modifiers, parameter;
        std::optional<DWORD> keyState, allowedEffect;
        std::optional<POINTL> screenPoint;
        bool candidateMatched = false;
    };
    struct V4FileProviderInput
    {
        std::string candidateId;
        std::uintptr_t sourceIdentity = 0;
        DWORD ownerThread = 0;
        bool asynchronous = false, candidateMatched = false;
        int starts = 0, ends = 0;
        bool checkedBeforeEnter = false;
    };
    struct V4PinnedCopyLeaseInput
    {
        std::string candidateId;
        bool ownerBoundAndInitiallyObserved = false;
        std::vector<std::size_t> matchingPreCallProgramIndices;
        std::optional<bool> receiptPreflightObserved, receiptBeforeDropObserved, receiptBeforeLeaveObserved, receiptAfterAwaitObserved;
        std::optional<bool> receiptSameLease;
    };
    struct V4FileConflictInput
    {
        std::string candidateId;
        std::filesystem::path destination;
        DWORD ownerThread = 0, queryError = 0;
        std::optional<bool> destinationExists;
        bool candidateMatched = false, checkedBeforeDrop = false;
    };
    struct V4DirectoryInput
    {
        std::string candidateId;
        std::filesystem::path sourcePath;
        std::uintptr_t sourceIdentity = 0;
        DWORD ownerThread = 0;
        bool sourceConstructed = false, initialTreeObserved = false;
        std::vector<std::string> unresolved;
    };
    struct V4MixedInput
    {
        std::string candidateId;
        std::uintptr_t combinedSource = 0, fileSource = 0;
        DWORD ownerThread = 0;
        bool sourcePrepared = false, resourceUnavailable = false, initialCallsObserved = false;
        std::vector<mixed_fixture::Call> calls;
        std::vector<std::string> unresolved;
    };
    struct V4ConfiguredInput
    {
        std::string candidateId;
        std::uintptr_t sourceIdentity = 0;
        DWORD ownerThread = 0;
        bool inputPrepared = false, resourceUnavailable = false;
        std::vector<std::string> unresolved;
    };
    struct V4VirtualManifestInput
    {
        struct Member { std::size_t descriptorIndex = 0; std::wstring name; std::vector<std::byte> bytes; };
        std::string candidateId;
        std::uintptr_t sourceIdentity = 0;
        DWORD ownerThread = 0;
        bool captured = false, resourceUnavailable = false;
        std::optional<std::size_t> descriptorCount;
        std::vector<Member> members;
        std::vector<std::string> unresolved;
    };
    struct V4VirtualAxisInputAudit
    {
        struct Axis
        {
            std::string id, selected, state, actual, evidence;
            std::vector<std::size_t> programIndices;
        };
        std::string candidateId;
        std::vector<Axis> axes;
        bool captured = false, resourceUnavailable = false;
        std::vector<std::string> unresolved;
    };
    struct V4VirtualStagingComparison
    {
        struct Member
        {
            std::size_t descriptorIndex = 0;
            std::uint64_t batch = 0, member = 0;
            std::optional<bool> fileIdentityMatches, bytesMatch, namedIdentityMatches, sourceAndThreadMatches;
        };
        bool sameReceiptObserved = false, comparisonComplete = false, observationUnavailable = true, resourceUnavailable = false;
        std::vector<Member> members;
        std::vector<std::string> unresolved;
    };
    struct V4SingleOwnedFileBusinessBinding
    {
        bool sameReceiptObserved = false, pathFlowObserved = false, completionObserved = false;
        bool trackedCopyObserved = false;
        bool observationUnavailable = true, resourceUnavailable = false;
        std::optional<bool> sourceSnapshotMatches, outputBytesMatch, reloadMatches, firstLandingMatches;
        std::optional<std::uint64_t> readCompletionEntered, fileCompletionEntered;
        std::vector<std::string> unresolved;
    };
    struct V4CollectionBusinessBinding
    {
        bool sameReceiptObserved = false, pathFlowObserved = false, completionObserved = false;
        bool observationUnavailable = true, resourceUnavailable = false;
        std::optional<bool> sourceSnapshotMatches, sourcePreserved, outputBytesMatch, membershipMatches,
            reloadMatches, widgetPreserved, reloadedWidgetPreserved, noDesktopDuplicate;
        std::optional<std::uint64_t> readCompletionEntered, fileCompletionEntered;
        std::vector<std::string> unresolved;
    };
    BridgeStatus status = BridgeStatus::unimplemented;
    std::vector<std::string> obligations;
    std::vector<std::string> notApplicable;
    std::vector<std::string> graphUnresolved;
    std::vector<std::string> postDispatchObligations;
    std::vector<std::string> unclassifiedObligations;
    std::vector<OutcomeHypothesis> outcomeExpectations;
    std::vector<ole_driver::V3PresenceExpectation> presenceExpectations;
    std::vector<ole_driver::V4PresenceExpectation> v4PresenceExpectations;
    std::optional<V2CaseEvidence> v2Evidence;
    std::optional<V2DesktopCoordinateEvidence> v2CoordinateInput;
    std::optional<V2DesktopCoordinateEvidence> v2CoordinateEnter;
    std::optional<ole_driver::V3OleInputAudit> v3OleInput;
    std::optional<ole_driver::V3SourceAndPassiveEvidence> v3SourceEvidence;
    std::optional<ole_driver::PassiveTransferEvidence> v3PassiveEvidence;
    std::optional<ole_driver::V3LocalResolvedCountObservation> v3LocalResolvedCount;
    std::optional<ole_driver::V3DropTaskWindowObservation> v3DropTaskObservation;
    std::optional<ole_driver::V3LocalPresenceObservation> v3LocalPresence;
    std::optional<ole_driver::V3SessionPresenceObservation> v3SessionPresence;
    std::optional<ole_driver::V3FileDropAbiObservation> v3FileDropAbi;
    std::optional<ole_driver::V4UnicodeReadObservation> v4UnicodeRead;
    std::optional<ole_driver::V4UnicodeFixtureInputObservation> v4UnicodeFixtureInput;
    std::optional<ole_driver::V4UnicodeFixtureInputObservation> v4SourcePreparation, v4SourceAtExecution;
    std::optional<ole_driver::V4UnicodeCallerEvidence> v4UnicodeCaller;
    std::optional<ole_driver::V4UnicodeProviderIntervalEvidence> v4UnicodeProviderInterval;
    std::optional<ole_driver::V4UnicodeConsumptionEvidence> v4UnicodeConsumption;
    std::optional<ole_driver::V4UnicodeRefreshLinkEvidence> v4UnicodeRefreshLink;
    std::optional<ole_driver::V4TextGraphEvidence> v4TextGraph;
    std::optional<ole_driver::V4RefreshRequestObservation> v4RefreshRequest;
    std::optional<ole_driver::V4RefreshWorkerObservation> v4RefreshWorker;
    std::optional<behavior::TextRefreshWindowEvidence> v4TextRefresh;
    std::optional<behavior::TextBusinessEvidence> v4TextBusiness;
    std::optional<behavior::V4CopyBusinessEvidence> v4CopyBusiness;
    std::optional<behavior::V4CollectionCopyEvidence> v4CollectionCopyBusiness;
    bool v4CollectionBusinessSealed = false;
    std::optional<V4CollectionBusinessBinding> v4CollectionBusinessBinding;
    using V4DesktopFilesIngressInput = V4CollectionIngressInput;
    using V4DesktopFilesBusinessBinding = V4CollectionBusinessBinding;
    std::optional<V4DesktopFilesIngressInput> v4DesktopFilesIngressInput;
    std::optional<behavior::V4DesktopFilesCopyEvidence> v4DesktopFilesCopyBusiness;
    bool v4DesktopFilesBusinessSealed = false;
    std::optional<V4DesktopFilesBusinessBinding> v4DesktopFilesBusinessBinding;
    using V4FolderMappingIngressInput = V4CollectionIngressInput;
    struct V4FolderMappingBusinessBinding : V4CollectionBusinessBinding
    {
        std::optional<bool> mappedDirectoryPreserved, sentinelPreserved, existingPreserved;
    };
    std::optional<V4FolderMappingIngressInput> v4FolderMappingIngressInput;
    std::optional<behavior::V4FolderMappingCopyEvidence> v4FolderMappingCopyBusiness;
    bool v4FolderMappingBusinessSealed = false;
    std::optional<V4FolderMappingBusinessBinding> v4FolderMappingBusinessBinding;
    std::optional<V4TextDesktopInput> v4LeaveDesktopInput;
    std::optional<V4TextProgramCollectionInput> v4LeaveProgramCollectionInput;
    std::optional<behavior::V4FileLeaveEvidence> v4LeaveBusiness;
    bool v4LeaveBusinessSealed = false;
    struct V4LeaveBusinessBinding
    {
        bool sameReceiptObserved = false, leaveReturnObserved = false;
        bool observationUnavailable = true, resourceUnavailable = false;
        std::optional<bool> sourceSnapshotMatches, sourcePreserved, sentinelPreserved, initialCellsPreserved,
            layoutBytesUnchanged, layoutFileIdentityUnchanged, sourceLogAvailable;
        std::optional<std::size_t> outputCount, finalItemCount;
        std::optional<int> starts, ends, fileReads;
        std::vector<std::string> unresolved;
    };
    std::optional<V4LeaveBusinessBinding> v4LeaveBusinessBinding;
    std::optional<ole_driver::V4LeaveHostObservation> v4LeaveHostObservation;
    std::optional<V4TextDesktopInput> v4RejectDesktopInput;
    std::optional<V4TextProgramCollectionInput> v4RejectProgramCollectionInput;
    std::optional<behavior::V4FileRejectEvidence> v4RejectBusiness;
    bool v4RejectBusinessSealed = false;
    struct V4RejectBusinessBinding
    {
        bool sameReceiptObserved = false, rejectReturnObserved = false;
        bool observationUnavailable = true, resourceUnavailable = false;
        std::optional<bool> sourceSnapshotMatches, sourcePreserved, sentinelPreserved, initialCellsPreserved,
            layoutBytesUnchanged, layoutFileIdentityUnchanged, sourceLogAvailable;
        std::optional<std::size_t> outputCount, finalItemCount;
        std::optional<int> starts, ends, fileReads;
        std::vector<std::string> unresolved;
    };
    std::optional<V4RejectBusinessBinding> v4RejectBusinessBinding;
    std::optional<V4TextDesktopInput> v4MoveDesktopInput;
    std::optional<V4TextProgramCollectionInput> v4MoveProgramCollectionInput;
    std::optional<behavior::V4FileMoveEvidence> v4MoveBusiness;
    bool v4MoveBusinessSealed = false;
    struct V4SharedMoveInput
    {
        std::string candidateId;
        bool ownerBoundAndInitiallyObserved = false, receiptBeforeDropObserved = false, receiptSameLease = false;
        std::vector<std::size_t> matchingPreCallProgramIndices;
        std::optional<V4SharedMoveSourceAfter> afterAwait;
    };
    std::optional<V4SharedMoveInput> v4SharedMoveInput;
    std::optional<ole_driver::V4TrackedShellMoveObservation> v4TrackedShellMove;
    struct V4MoveBusinessBinding
    {
        bool sameReceiptObserved = false, trackedMoveObserved = false, completionObserved = false;
        bool observationUnavailable = true;
        std::optional<std::uint64_t> readCompletionEntered, fileCompletionEntered;
        std::optional<bool> sourceSnapshotMatches, outputBytesMatch, outputSameSource;
        std::vector<std::string> unresolved;
    };
    std::optional<V4MoveBusinessBinding> v4MoveBusinessBinding;
    std::optional<ole_driver::V4TrackedShellCopyObservation> v4TrackedShellCopy;
    struct V4GroupIngressInput
    {
        struct Event { std::size_t programIndex = 0; ole_driver::Kind kind{}; behavior::Access::GroupIngressGeometry geometry; };
        struct TabEvent { std::size_t programIndex = 0; ole_driver::Kind kind{}; behavior::Access::V4GroupTabIngressSnapshot geometry; };
        std::string candidateId;
        std::vector<Event> events;
        std::vector<TabEvent> tabEvents;
        std::vector<std::size_t> matchingPreCallProgramIndices;
        bool viewportInputMatched = false, inactiveTabInputMatched = false, resourceUnavailable = false, generatedFromModel = false;
    };
    std::optional<V4GroupIngressInput> v4GroupIngressInput;
    struct V4GroupAxisInput
    {
        struct Child { std::size_t index = 0; std::string id, type; std::optional<bool> idMatches, typeMatches; };
        struct Side { std::string childrenKind, active, hit; std::vector<Child> children; bool valuesComplete = false; };
        std::string candidateId;
        Side source, target;
        std::optional<behavior::Access::V4GroupChildrenSnapshot> actualTarget, actualSource;
        std::optional<bool> targetChildCountMatches, targetActiveMatches, activeContentMatches;
        std::optional<bool> sourceChildCountMatches, sourceActiveMatches, sourceActiveContentMatches;
        bool sameHostBindingObserved = false, resourceUnavailable = false, observationUnavailable = true;
        std::vector<std::string> unresolved;
    };
    std::optional<V4GroupAxisInput> v4GroupAxisInput;
    std::shared_ptr<behavior::V4NativeGroupMemberInitial> v4NativeGroupMemberInitial;
    std::optional<ole_driver::V4GroupHitActivationEvidence> v4GroupHitActivation;
    struct V4GroupHitBinding
    {
        bool localReceiptObserved = false, observationUnavailable = true, resourceUnavailable = false;
        std::optional<bool> groupObjectMatches, groupDataMatches, groupKindMatches, expectedHitMatches,
            inactiveActivationAndSealedStateAgree;
        std::vector<std::string> unresolved;
    };
    std::optional<V4GroupHitBinding> v4GroupHitBinding;
    std::optional<behavior::V4GroupCopyEvidence> v4GroupCopyBusiness;
    bool v4GroupBusinessSealed = false;
    struct V4GroupBusinessBinding
    {
        bool sameReceiptObserved = false, pathFlowObserved = false, completionObserved = false;
        bool observationUnavailable = true, resourceUnavailable = false;
        std::optional<bool> sourceSnapshotMatches, sourcePreserved, existingPreserved, outputBytesMatch,
            membershipMatches, reloadMatches, rootPreserved, activePreserved, inactivePreserved,
            reloadedRootPreserved, reloadedActivePreserved, reloadedInactivePreserved, noDesktopDuplicate,
            mappedDirectoryPreserved, sentinelPreserved;
        std::optional<std::uint64_t> readCompletionEntered, fileCompletionEntered;
        std::vector<std::string> unresolved;
    };
    std::optional<V4GroupBusinessBinding> v4GroupBusinessBinding;
    struct V4CollectionIngressInput
    {
        struct Event { std::size_t programIndex = 0; ole_driver::Kind kind{}; behavior::Access::CollectionIngressGeometry geometry; };
        std::string candidateId;
        std::vector<Event> events;
        std::vector<std::size_t> matchingPreCallProgramIndices;
        bool viewportInputMatched = false;
    };
    std::optional<V4CollectionIngressInput> v4CollectionIngressInput;
    std::optional<behavior::V4BatchCopyBusinessEvidence> v4BatchCopyBusiness;
    bool v4BatchBusinessSealed = false;
    std::optional<ole_driver::V4RestrictedCopyBusinessVerdict> v4RestrictedCopyBusinessVerdict;
    std::optional<V4SingleOwnedFileBusinessBinding> v4SingleOwnedFileBusinessBinding;
    std::optional<behavior::V4DirectoryCopyEvidence> v4DirectoryCopyBusiness;
    bool v4DirectoryBusinessSealed = false;
    struct V4DirectoryBusinessBinding
    {
        bool sameReceiptObserved = false, pathFlowObserved = false, completionObserved = false;
        bool observationUnavailable = true, resourceUnavailable = false;
        std::optional<bool> sourceLeaseMatches, landingMatched, independentAssertionsCompleted, reloadObserved;
        std::optional<std::uint64_t> readCompletionEntered, fileCompletionEntered;
        std::vector<std::string> unresolved;
    };
    std::optional<V4DirectoryBusinessBinding> v4DirectoryBusinessBinding;
    std::optional<behavior::V4VirtualBusinessEvidence> v4VirtualBusiness;
    bool v4VirtualBusinessSealed = false;
    std::optional<behavior::V4VirtualOutputBindingEvidence> v4VirtualOutputBinding;
    std::optional<behavior::V4VirtualDesktopGeometry> v4VirtualGeometry;
    std::optional<behavior::V4VirtualLayoutDiskSnapshot> v4VirtualLayoutDiskSnapshot;
    std::optional<behavior::V4VirtualLayoutEventEvidence> v4VirtualLayoutEvents;
    std::vector<std::size_t> v4VirtualGeometryPreCallIndices;
    std::optional<mixed_fixture::VirtualExpectedManifest> v4VirtualManifest;
    std::optional<ole_driver::V4ConfiguredContentSnapshot> v4ConfiguredPreparation, v4ConfiguredSourceInitial,
        v4ConfiguredSourceAtExecution, v4ConfiguredSourceAfterAwait, v4ConfiguredSourceAfterDispatch;
    std::optional<bool> v4ConfiguredAwaitReturned;
    std::optional<ole_driver::V4ConfiguredContentObservation> v4ConfiguredObservation;
    std::optional<ole_driver::V4VirtualFlowEvidence> v4VirtualFlow;
    std::optional<ole_driver::V4VirtualTrackedShellEvidence> v4VirtualTrackedShell;
    std::optional<ole_driver::V4VirtualProductionChainEvidence> v4VirtualProductionChain;
    std::optional<ole_driver::V4VirtualRefreshEvidence> v4VirtualRefresh;
    std::optional<ole_driver::V4VirtualFailureObservation> v4VirtualFailures;
    std::optional<ole_driver::V4VirtualStagingHandlesEvidence> v4VirtualStagingHandles;
    std::optional<V4VirtualStagingComparison> v4VirtualStagingComparison;
    bool v4VirtualStagingHandleReleaseDeferred = false;
    std::optional<directory_fixture::LiveSourceObservation> v4DirectoryLiveSource;
    std::optional<directory_fixture::DirectoryCopyLeaseObservation> v4DirectoryLease;
    std::optional<directory_fixture::DirectoryCopyLeaseObservation> v4DirectorySourcePreparation, v4DirectorySourceAtExecution;
    std::optional<ole_driver::V4FileTaskWindowObservation> v4DirectoryTaskWindow;
    std::optional<ole_driver::V4FileAbiEnvelope> v4DirectoryAbiEnvelope;
    std::optional<ole_driver::V4FilePathFlowObservation> v4DirectoryPathFlow;
    std::optional<ole_driver::V4FilePayloadObservation> v4DirectorySource;
    std::optional<ole_driver::V4FileGraphEvidence> v4DirectoryGraph;
    std::optional<ole_driver::V4SingleFileInputObservation> v4FileSourcePreparation, v4FileSourceAtExecution;
    std::optional<ole_driver::V4IndependentFileInputObservation> v4BatchSourcePreparation, v4BatchSourceAtExecution;
    std::optional<ole_driver::V4FilePayloadObservation> v4FilePayload;
    std::optional<ole_driver::V4FileTaskWindowObservation> v4FileTaskWindow;
    std::optional<ole_driver::V4FileGraphEvidence> v4FileGraph;
    std::optional<ole_driver::V4FilePathFlowObservation> v4FilePathFlow;
    std::optional<ole_driver::V3SourceSessionInterval> v4FileSourceInterval;
    std::optional<V4TextProgramCollectionInput> v4TextProgramCollectionInput;
    std::optional<V4TextProgramCollectionInput> v4VirtualProgramCollectionInput;
    std::optional<V4TextDesktopInput> v4TextDesktopInput;
    std::optional<V4TextDesktopInput> v4FileDesktopInput;
    std::optional<V4TextProgramCollectionInput> v4FileProgramCollectionInput;
    std::optional<V4TextDesktopInput> v4BatchDesktopInput;
    std::optional<V4TextProgramCollectionInput> v4BatchProgramCollectionInput;
    std::vector<V4FileOleInput> v4FileOleInputs;
    std::optional<V4FileProviderInput> v4FileProviderInput;
    std::optional<V4PinnedCopyLeaseInput> v4PinnedCopyLeaseInput;
    std::optional<V4FileConflictInput> v4FileConflictInput;
    std::optional<V4FileConflictInput> v4BatchConflictInput;
    std::optional<V4TextDesktopInput> v4DirectoryDesktopInput;
    std::optional<V4TextProgramCollectionInput> v4DirectoryProgramCollectionInput;
    std::vector<V4FileOleInput> v4DirectoryOleInputs;
    std::optional<V4FileConflictInput> v4DirectoryConflictInput;
    std::optional<V4DirectoryInput> v4DirectoryInput;
    std::optional<V4MixedInput> v4MixedInput;
    std::optional<V4ConfiguredInput> v4ConfiguredInput;
    std::optional<V4VirtualManifestInput> v4VirtualManifestInput;
    std::optional<V4VirtualAxisInputAudit> v4VirtualAxisInputAudit;
    std::optional<bool> v4MixedProviderRecordsCaptured;
    std::optional<bool> v4MixedProviderBaselineCaptured;
    std::optional<std::vector<mixed_fixture::Call>> v4MixedCalls;
    std::optional<std::vector<ole_file_source_fixture::FileDropRead>> v4MixedFileReads;
    std::optional<std::vector<ole_content_source_fixture::Call>> v4MixedContentCalls;
    std::optional<std::vector<mixed_fixture::Call>> v4MixedBaselineCalls;
    std::optional<std::vector<ole_file_source_fixture::FileDropRead>> v4MixedBaselineFileReads;
    std::optional<std::vector<ole_content_source_fixture::Call>> v4MixedBaselineContentCalls;
    std::optional<ole_driver::V4MixedGraphEvidence> v4MixedGraph;
    std::optional<behavior::NativeMemberBusinessEvidence> v4NativeMemberBusiness;
    bool v4NativeMemberBusinessSealed = false;
    struct V4NativeMemberBusinessBinding
    {
        bool sameReceiptObserved = false, graphIdentityObserved = false, cleanupIntervalObserved = false;
        bool observationUnavailable = true;
        std::optional<bool> sourceSnapshotMatches, layoutPathMatches, layoutBytesMatch, layoutRevisionMatches;
        std::optional<FILE_ID_INFO> snapshotFileId;
        std::optional<std::uint64_t> snapshotRevision, saveRevision;
        std::vector<std::string> unresolved;
    };
    std::optional<V4NativeMemberBusinessBinding> v4NativeMemberBusinessBinding;
    struct V4NativeCollectionInitialInput
    {
        std::string candidateId;
        std::uintptr_t host = 0;
        DWORD thread = 0;
        POINT sourceScreen{};
        behavior::Access::NativeLandingGeometry landing;
        bool landingMatches = false;
        std::optional<bool> sourceAndReleasePointsMatch;
    };
    std::optional<V4NativeCollectionInitialInput> v4NativeCollectionInitialInput;
    struct V4NativeCollectionPointProposal
    {
        bool prepared = false, revalidated = false, generatedPointsUsed = false;
        std::string candidateId;
        std::vector<ole_driver::NativePointInput> points;
        std::vector<std::string> unresolved;
    };
    std::optional<V4NativeCollectionPointProposal> v4NativeCollectionPointProposal;
    struct V4NativeCategoryInitialInput
    {
        std::string candidateId;
        behavior::Access::NativeCategorySourceGeometry source;
        behavior::Access::NativeLandingGeometry landing;
        bool landingMatches = false;
        std::optional<bool> sourceBaselineMatches;
        std::optional<bool> sourceAndReleasePointsMatch;
    };
    std::optional<V4NativeCategoryInitialInput> v4NativeCategoryInitialInput;
    std::optional<V4NativeCollectionPointProposal> v4NativeCategoryPointProposal;
    std::optional<behavior::V4NativeCategoryEvidence> v4NativeCategoryBusiness;
    std::optional<ole_driver::V4NativeCategoryCallObservation> v4NativeCategoryCalls;
    std::optional<ole_driver::V4NativeCategoryDesktopCommitObservation> v4NativeCategoryDesktopCommit;
    // Local recorder capacity/usage only; a drained recorder is not producer quiescence.
    std::optional<snowdesktop::external_transfer_observation::BudgetObservation> v4DiagnosticBudget;
    std::optional<snowdesktop::background_work_observation::BudgetObservation> v4BackgroundDiagnosticBudget;
    std::optional<snowdesktop::ole_adapter_observation::State> v4OleAdapterLogLast;
    bool v4NativeCategoryBusinessSealed = false;
    std::optional<V4NativeMemberBusinessBinding> v4NativeCategoryBusinessBinding;
    struct V4NativeReorderInitialInput
    {
        std::string candidateId;
        behavior::Access::NativeCollectionReorderGeometry geometry;
        bool sourceBaselineMatches = false;
        std::optional<bool> sourceAndReleasePointsMatch;
    };
    std::optional<V4NativeReorderInitialInput> v4NativeReorderInitialInput;
    std::optional<V4NativeCollectionPointProposal> v4NativeReorderPointProposal;
    std::optional<behavior::V4NativeCollectionReorderEvidence> v4NativeReorderBusiness;
    bool v4NativeReorderBusinessSealed = false;
    std::optional<V4NativeMemberBusinessBinding> v4NativeReorderBusinessBinding;
    std::optional<ole_driver::V4NativeReorderProductionObservation> v4NativeReorderProduction;
    struct V4NativeReorderCommitBinding
    {
        bool sameReceiptObserved = false, recordsComplete = false, observationUnavailable = true;
        std::optional<std::uint64_t> commitId;
        std::optional<bool> containerMatches, inputIndicesMatch, beforeKeysMatch, afterKeysMatch;
        std::vector<std::string> unresolved;
    };
    std::optional<V4NativeReorderCommitBinding> v4NativeReorderCommitBinding;
    std::optional<behavior::V4NativeDesktopMoveEvidence> v4NativeDesktopBusiness;
    bool v4NativeDesktopBusinessSealed = false;
    // The value snapshot is serializable; the owner keeps the actual Profile,
    // file handles, epoch and receipts alive until the caller has completed
    // observation and explicitly performs the usual same-STA teardown.
    std::optional<behavior::V4NativeMultiPartialEvidence> v4NativeMultiEvidence;
    std::shared_ptr<behavior::V4NativeMultiSelectionBridgeResult> v4NativeMultiOwner;
    struct V4NativeDesktopLayoutBinding
    {
        bool sameReceiptObserved = false, observationUnavailable = true;
        std::optional<bool> pathMatches, bytesMatch, revisionMatches;
        std::optional<FILE_ID_INFO> snapshotFileId;
        std::optional<std::uint64_t> snapshotRevision, saveRevision;
        std::vector<std::string> unresolved;
    };
    std::optional<V4NativeDesktopLayoutBinding> v4NativeDesktopLayoutBinding;
    std::optional<V4NativeDesktopGeometryInput> v4NativeDesktopGeometry;
    std::optional<behavior::V4NativeDesktopPointProposal> v4NativeDesktopPointProposal;
    bool v4NativeDesktopProposalRevalidated = false;
    bool v4NativeDesktopUsedGeneratedPoints = false;
    std::vector<ole_driver::Observation> v4OleObservations;
    std::vector<ole_driver::V2CapturedOleReturn> v4CapturedOleReturns;
    std::optional<std::vector<ole_driver::MessageObservation>> v4CompletionTrace;
    std::string failure;
    std::optional<NativeDesktopEndpointEvidence> nativeEndpoint;
    std::vector<NativeQueuedInput> nativeQueuedInputs;
    std::vector<ole_driver::V4NativeDeviceRecord> v4NativeDeviceRecords;
    std::vector<ole_driver::V4NativeQueuedRecord> v4NativeQueuedRecords;
    std::vector<ole_driver::V4NativeTraceRecord> v4NativeTraceRecords;
    std::vector<snowdesktop::private_pointer::State> v4NativeBeforeEndStates;
    std::vector<snowdesktop::private_pointer::DeliveryObservationLog> v4NativeBeforeEndDelivery;
    std::optional<ole_driver::V4NativeCleanupEvidence> v4NativeCleanup;
    std::optional<ole_driver::V4NativeLayoutSaveEvidence> v4NativeLayoutSave;
    std::optional<ole_driver::V4NativeGraphObservation> v4NativeGraph;
    // For evidence-unavailable results this still records actual production
    // entry; an unimplemented oracle does not imply the call never occurred.
    bool productionStarted = false;
    bool productionEntryUnknown = false;
    bool businessFailureObserved = false;
    bool eventOracleFailureObserved = false;
    std::optional<bool> unattributedBusinessFailureObserved, unattributedEventOracleFailureObserved;
    // A failed HRESULT was actually returned and recorded. Exceptions or
    // unavailable observations do not establish such a return.
    bool eventDispatchFailureObserved = false;
    bool eventDispatchObservationRecorded = false;
    bool hypothesisNotMetObserved = false;
    bool observationUnavailable = false;
};

// Same-candidate input snapshot only. It records the actual ordered target
// children and selected model values; it never performs or predicts a hit.
inline BridgeResult::V4GroupAxisInput CaptureV4GroupAxisInput(
    const event_graph::V4FamilyIdentityContext& identity, const V4FamilyValidatedContext& verified,
    DesktopApp& app, std::size_t maxChildren, std::size_t maxWidgets, std::size_t maxUtf16Bytes,
    bool observeSource = false)
{
    struct GroupInputBudgetExceeded {};
    BridgeResult::V4GroupAxisInput out;
    try
    {
        if (verified.Status() != V4CoordinateStatus::coordinate_ready || identity.Status() != V4FamilyStatus::schema_only ||
            verified.TaskBActive() || verified.FamilyBinding() != identity.FamilyBinding() || verified.Axes().size() != identity.Axes().size())
        { out.unresolved.emplace_back("v4_group_axis_candidate_unavailable"); return out; }
        for (std::size_t i = 0; i < verified.Axes().size(); ++i)
        {
            const auto& a = verified.Axes()[i]; const auto& b = identity.Axes()[i];
            if (a.id != b.id || a.role != b.role || a.label != b.label || a.values != b.values)
            { out.unresolved.emplace_back("v4_group_axis_schema_mismatch"); return out; }
        }
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty())
        { out.resourceUnavailable = raw.resourceUnavailable; out.unresolved.emplace_back("v4_group_axis_identity_unavailable"); return out; }
        out.candidateId = raw.id;
        std::size_t remainingBytes = maxUtf16Bytes;
        const auto selected = [&](const std::string& id) {
            const auto value = Value(verified.Axes(), verified.Coordinate(), id);
            if (value.size() > remainingBytes) throw GroupInputBudgetExceeded{};
            remainingBytes -= value.size(); return std::string(value);
        };
        const auto fill = [&](BridgeResult::V4GroupAxisInput::Side& side, std::string_view name) {
            const std::string base = std::string("V4.group_") + std::string(name);
            side.childrenKind = selected(base + ".children_kind"); side.active = selected(base + ".active"); side.hit = selected(base + ".hit");
            std::size_t count = 0;
            for (const auto& axis : verified.Axes())
                if (axis.id.starts_with(base + ".child_") && axis.id.ends_with(".id")) ++count;
            if (count > maxChildren) { out.resourceUnavailable = true; return false; }
            side.children.reserve(count);
            for (std::size_t i = 0; i < count; ++i)
            {
                const auto child = base + ".child_" + std::to_string(i);
                side.children.push_back({i, selected(child + ".id"), selected(child + ".type")});
            }
            side.valuesComplete = true;
            return true;
        };
        if (!fill(out.source, "source") || !fill(out.target, "target"))
        { out.unresolved.emplace_back("v4_group_axis_child_budget_unavailable"); return out; }
        bool resource = false;
        auto snapshot = behavior::Access::InspectV4GroupChildren(app, L"ingress-root", std::nullopt,
            maxChildren, maxWidgets, remainingBytes, resource);
        if (!snapshot)
        { out.resourceUnavailable = resource; out.unresolved.emplace_back("v4_group_axis_live_snapshot_unavailable"); return out; }
        if (observeSource) out.actualSource = std::move(snapshot); else out.actualTarget = std::move(snapshot);
        const auto& actual = observeSource ? *out.actualSource : *out.actualTarget;
        auto& side = observeSource ? out.source : out.target;
        auto& childCountMatches = observeSource ? out.sourceChildCountMatches : out.targetChildCountMatches;
        auto& activeMatches = observeSource ? out.sourceActiveMatches : out.targetActiveMatches;
        auto& contentMatches = observeSource ? out.sourceActiveContentMatches : out.activeContentMatches;
        if (side.childrenKind == "exact") childCountMatches = side.children.size() == actual.children.size();
        constexpr char digits[] = "0123456789abcdef";
        const auto utf16Matches = [&](std::string_view encoded, std::wstring_view text) {
            constexpr std::string_view prefix = "utf16le_hex_";
            if (!encoded.starts_with(prefix)) return false;
            encoded.remove_prefix(prefix.size());
            if (text.size() > encoded.size() / 4 || encoded.size() != text.size() * 4) return false;
            std::size_t index = 0;
            for (wchar_t unit : text) { const auto value = static_cast<std::uint16_t>(unit);
                for (unsigned shift : {0u, 8u}) { const auto byte = (value >> shift) & 255u;
                    if (encoded[index++] != digits[byte >> 4] || encoded[index++] != digits[byte & 15]) return false; } }
            return true;
        };
        const auto typeToken = [&](std::int64_t value) {
            if (value < (std::numeric_limits<std::int32_t>::min)() || value > (std::numeric_limits<std::int32_t>::max)()) return std::string{};
            const auto bits = static_cast<std::uint32_t>(static_cast<std::int32_t>(value));
            std::string result("exact_"); for (int shift = 28; shift >= 0; shift -= 4) result.push_back(digits[(bits >> shift) & 15u]);
            return result;
        };
        for (auto& child : side.children)
            if (child.index < actual.children.size())
            {
                const auto& observed = actual.children[child.index];
                if (child.id.starts_with("utf16le_hex_")) child.idMatches = utf16Matches(child.id, observed.id);
                if (child.type.starts_with("exact_")) { const auto type = typeToken(observed.type); if (!type.empty()) child.typeMatches = child.type == type; }
            }
        const bool exactActive = side.active == "zero" || side.active == "one" || side.active == "two" ||
            side.active.starts_with("count_");
        if (actual.activeIndex && *actual.activeIndex < actual.children.size())
            contentMatches = actual.contentData == actual.children[*actual.activeIndex].widget;
        if (exactActive && actual.activeIndex)
        {
            const auto index = *actual.activeIndex;
            const auto token = index == 0 ? std::string("zero") : index == 1 ? std::string("one") :
                index == 2 ? std::string("two") : std::string("count_") + std::to_string(index);
            activeMatches = side.active == token;
        }
        else if (exactActive) activeMatches = false;
        out.unresolved.emplace_back(observeSource ? "v4_group_target_children_not_observed" : "v4_group_source_children_not_observed");
        out.unresolved.emplace_back("v4_group_actual_hit_and_async_activation_not_observed");
        out.unresolved.emplace_back("v4_group_snapshot_non_atomic_reentry_and_address_reuse_unverified");
    }
    catch (const GroupInputBudgetExceeded&) { out.resourceUnavailable = true; }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; }
    catch (const std::length_error&) { out.resourceUnavailable = true; }
    catch (...) { out.unresolved.emplace_back("v4_group_axis_observation_unavailable"); }
    return out;
}

// Copy raw two-session evidence without transferring or releasing the live
// owner. The scheduler must independently replay the original structured case
// and may associate these values only when identityBound is true.
inline BridgeResult CaptureV4NativeMultiBridgeResult(
    std::shared_ptr<behavior::V4NativeMultiSelectionBridgeResult> owner,
    const event_graph::V4FamilyIdentityContext& identity, const V4FamilyValidatedContext& verified,
    const std::vector<bool>& present, const event_graph::V4NativeJoinedGraphVariant& selected)
{
    BridgeResult result;
    result.observationUnavailable = true;
    result.v4NativeMultiOwner = std::move(owner);
    if (!result.v4NativeMultiOwner)
    { result.obligations.emplace_back("v4_native_multi_owner_unavailable"); return result; }
    const auto& source = *result.v4NativeMultiOwner;
    result.productionEntryUnknown = source.preparationEntryUnknown || source.dragEntryUnknown;
    if (source.receipt)
    {
        // The bridge fields are public and may hold a receipt from another
        // attempt. Without live host/receipt authentication, keep actual
        // entry only as uncertainty and retain its raw facts in the snapshot.
        result.productionEntryUnknown = result.productionEntryUnknown || source.receipt->ProductionStarted() ||
            source.receipt->ProductionEntryUnknown();
    }
    const auto copyFacts = [&](const ole_driver::V4NativeRunReceipt& receipt) {
        const auto& execution = receipt.Execution();
        result.productionEntryUnknown = result.productionEntryUnknown || execution.productionStarted || execution.productionEntryUnknown;
    };
    for (const auto& attempt : source.unstartedDragAttempts) copyFacts(attempt);
    if (source.dragReceipt) copyFacts(*source.dragReceipt);
    try
    {
        result.v4NativeMultiEvidence = behavior::CaptureV4NativeMultiPartialEvidence(source, identity, verified, present, selected);
        if (!result.v4NativeMultiEvidence->identityBound)
            result.obligations.emplace_back("v4_native_multi_candidate_or_graph_identity_unavailable");
        result.obligations.insert(result.obligations.end(), source.unresolved.begin(), source.unresolved.end());
        result.obligations.insert(result.obligations.end(), source.dragPreconditions.begin(), source.dragPreconditions.end());
        result.postDispatchObligations = source.dragPostconditions;
        result.unclassifiedObligations = source.dragUnclassified;
        result.graphUnresolved = selected.unresolved;
    }
    catch (...)
    {
        // Production facts and the strong owner were captured before copying.
        // The caller may serialize those facts as unknown if memory permits.
        result.observationUnavailable = true;
    }
    return result;
}

// Value-only encoding; the writer enforces its own byte budget. Presence
// tokens distinguish an unavailable value from an observed empty collection.
template<class Writer>
inline void WriteV4NativeMultiEvidence(Writer& out, const behavior::V4NativeMultiPartialEvidence& evidence)
{
    const auto strings = [&](const auto& values) { out.Number(values.size()); for (const auto& value : values) out.Token(value); };
    const auto optionalStrings = [&](const auto& values) { out.Number(values.has_value()); if (values) strings(*values); };
    const auto optionalString = [&](const auto& value) { out.Number(value.has_value()); if (value) out.Token(*value); };
    const auto wideStrings = [&](const auto& values) { out.Number(values.size()); for (const auto& value : values) out.Wide(value); };
    const auto optionalWide = [&](const auto& value) { out.Number(value.has_value()); if (value) out.Wide(*value); };
    const auto optionalWideStrings = [&](const auto& value) { out.Number(value.has_value()); if (value) wideStrings(*value); };
    const auto token = [&](const auto& value) { out.Number(value.ownerThread); out.Number(value.session); out.Number(value.event); };
    const auto queued = [&](const auto& value) {
        out.Number(static_cast<int>(value.phase)); out.OptionalNumber(value.programIndex); token(value.token);
    };
    const auto messages = [&](const auto& values) {
        out.Number(values.size()); for (const auto& value : values) out.WindowMessage(value);
    };
    const auto device = [&](const auto& value) {
        out.Number(static_cast<int>(value.phase)); out.OptionalNumber(value.programIndex);
        out.NativeState(value.state); out.DeliveryLog(value.delivery);
        out.Number(value.queued.has_value()); if (value.queued) token(*value.queued);
        out.Number(value.expected.has_value()); if (value.expected) {
            out.Number(value.expected->message); out.Number(value.expected->screen.x); out.Number(value.expected->screen.y);
            out.Number(value.expected->clientBeforeDispatch.x); out.Number(value.expected->clientBeforeDispatch.y); out.Number(value.expected->buttons);
        }
        messages(value.trace);
    };
    const auto trace = [&](const auto& value) { out.Token(value.boundary); messages(value.messages); };
    const auto stateList = [&](const auto& values) { out.Number(values.size()); for (const auto& value : values) out.NativeState(value); };
    const auto deliveryList = [&](const auto& values) { out.Number(values.size()); for (const auto& value : values) out.DeliveryLog(value); };
    const auto execution = [&](const auto& value) {
        out.Number(static_cast<int>(value.status)); out.Number(value.productionStarted); out.Number(value.productionEntryUnknown);
        out.Number(value.businessFailureObserved); out.Number(value.eventOracleFailureObserved); out.Number(value.additionalObservationUnavailable);
        out.Token(value.failure); strings(value.obligations); strings(value.postconditionsPending); strings(value.unclassifiedObligations);
        out.OleAdapterLog(value.oleAdapterLogLast);
        out.Number(value.observations.size()); for (const auto& item : value.observations) out.OleEvent(item);
        strings(value.hostStagesReturned);
    };
    const auto model = [&](const auto& value) {
        out.Number(value.host); out.Number(value.ownerThread); out.Number(value.layoutRevision); out.Number(value.selectionRevision);
        out.Number(value.actualItemCount); out.Number(value.widgetCount); out.Wide(value.anchorScope); out.Wide(value.anchorKey);
        wideStrings(value.extraItemKeys); wideStrings(value.extraSelectedItemKeys); wideStrings(value.selectedDockReferences);
        wideStrings(value.selectedRunningKeys); wideStrings(value.selectedWidgetIds);
        out.Number(value.selectedWidgetFolderEntries.size());
        for (const auto& item : value.selectedWidgetFolderEntries) { out.Wide(item.first); out.Wide(item.second); }
        strings(value.otherSurfaceReasons); out.Number(value.otherSurfacesComplete);
        out.Number(value.members.size());
        for (const auto& item : value.members) {
            out.Wide(item.path.native()); out.Wide(item.key); out.Wide(item.typeName); out.Wide(item.desktopIconClsid);
            out.FileId(std::optional<FILE_ID_INFO>(item.fileId)); out.Number(item.fileAttributes); out.Number(item.selected);
            out.Number(item.isShortcut); out.Number(item.isApplicationShortcut);
            out.Number(item.bounds.left); out.Number(item.bounds.top); out.Number(item.bounds.right); out.Number(item.bounds.bottom);
            out.Cell(std::optional<GridCell>(item.cell));
        }
    };
    const auto optionalModel = [&](const auto& value) { out.Number(value.has_value()); if (value) model(*value); };
    const auto selection = [&](const auto& value) {
        out.Token(value.candidateId); optionalModel(value.before); optionalModel(value.after);
        optionalWide(value.beforeAnchor); optionalWide(value.afterAnchor);
        optionalWideStrings(value.beforeSelectedOutside); optionalWideStrings(value.afterSelectedOutside);
        out.Number(value.beforeFilesRevalidated); out.Number(value.afterFilesRevalidated); out.Number(value.memberKindBound);
        out.Number(value.resourceUnavailable); out.Number(value.observationUnavailable); strings(value.unresolved);
    };
    const auto business = [&](const auto& value) {
        out.Token(value.candidateId); optionalModel(value.beforeDrag); optionalModel(value.afterDrag); optionalModel(value.afterReload);
        out.Number(value.beforeReloadLayout.has_value()); if (value.beforeReloadLayout) {
            const auto& disk = *value.beforeReloadLayout; out.Wide(disk.path.native()); out.Token(disk.rawBytes);
            out.FileId(disk.fileId); out.OptionalNumber(disk.revision); out.Number(disk.captured); out.Number(disk.resourceUnavailable); strings(disk.unresolved);
        }
        out.Number(value.beforeFilesRevalidated); out.Number(value.afterFilesRevalidated); out.Number(value.reloadFilesRevalidated);
        out.Number(value.observationUnavailable); out.Number(value.resourceUnavailable); strings(value.unresolved);
    };
    const auto dragReceipt = [&](const auto& value) {
        out.Number(value.productionStarted); out.Number(value.productionEntryUnknown); out.Number(value.sealedObserved); out.Number(value.hostIdentity);
        optionalString(value.candidateId); optionalString(value.graphId);
        out.Number(value.execution.has_value()); if (value.execution) execution(*value.execution);
        out.Number(value.queued.has_value()); if (value.queued) { out.Number(value.queued->size()); for (const auto& item : *value.queued) queued(item); }
        out.Number(value.device.has_value()); if (value.device) { out.Number(value.device->size()); for (const auto& item : *value.device) device(item); }
        out.Number(value.trace.has_value()); if (value.trace) { out.Number(value.trace->size()); for (const auto& item : *value.trace) trace(item); }
        out.Number(value.beforeEndStates.has_value()); if (value.beforeEndStates) stateList(*value.beforeEndStates);
        out.Number(value.beforeEndDelivery.has_value()); if (value.beforeEndDelivery) deliveryList(*value.beforeEndDelivery);
        optionalStrings(value.unresolved); out.Number(value.sealedBusiness.has_value()); if (value.sealedBusiness) business(*value.sealedBusiness);
    };
    out.Token("v4_native_multi_partial_evidence_v8");
    out.Number(evidence.identityBound); out.Number(evidence.captureComplete); out.Number(evidence.resourceUnavailable); out.Number(evidence.observationUnavailable);
    out.Number(evidence.fixturePreparationAttempted); out.Number(evidence.fixturePreparationUnknown);
    out.Number(evidence.preparationEntryUnknown); out.Number(evidence.dragEntryUnknown); out.Number(evidence.unstartedDragAttemptCount);
    out.Number(evidence.plan.has_value()); if (evidence.plan) {
        const auto& value = *evidence.plan;
        out.Number(value.identityValid); out.Number(value.inputsMaterialized); out.Number(value.resourceUnavailable);
        out.Token(value.candidateId); out.Token(value.graphId); out.Number(value.actions.size());
        for (const auto& action : value.actions) {
            out.Number(action.programIndex); out.Token(action.kind); out.Token(action.session); out.Token(action.button);
            out.Token(action.modifiers); out.Token(action.member); out.Token(action.x); out.Token(action.y);
            out.Number(action.screen.has_value()); if (action.screen) { out.Number(action.screen->x); out.Number(action.screen->y); }
            out.OptionalNumber(action.modifierMask);
        }
        strings(value.preconditions); strings(value.graphUnresolved);
    }
    out.Number(evidence.categoryInput.has_value()); if (evidence.categoryInput) {
        const auto& value = *evidence.categoryInput;
        out.Number(value.identityValid); out.Number(value.sourceRead); out.Number(value.declaredSlotsObserved);
        out.Number(value.resourceUnavailable); out.OptionalNumber(value.ruleCountMatches); out.Token(value.reason);
        out.Number(value.rules.size()); for (const auto& rule : value.rules) {
            out.Token(rule.idUtf16Hex); out.Token(rule.labelUtf16Hex); out.Token(rule.extensionsUtf16Hex);
            out.Number(rule.enabled); out.OptionalNumber(rule.idMatches); out.OptionalNumber(rule.labelMatches);
            out.OptionalNumber(rule.extensionsMatches); out.OptionalNumber(rule.enabledMatches);
        }
        out.Number(value.shortcuts.size()); for (const auto& target : value.shortcuts) {
            out.Number(target.logicalIndex); out.Number(target.observed); out.Number(target.classified);
            out.Number(target.directory); out.Number(target.application); out.Token(target.extensionUtf16Hex);
            out.OptionalNumber(target.classifiedMatches); out.OptionalNumber(target.directoryMatches);
            out.OptionalNumber(target.applicationMatches); out.OptionalNumber(target.extensionMatches);
        }
    }
    out.Number(evidence.preparation.has_value()); if (evidence.preparation) {
        const auto& value = *evidence.preparation;
        out.Number(value.productionStarted); out.Number(value.productionEntryUnknown); out.Number(value.consumed); out.Number(value.sealedObserved);
        out.Number(value.hostIdentity);
        optionalString(value.candidateId); optionalString(value.graphId);
        out.Number(value.execution.has_value()); if (value.execution) execution(*value.execution);
        out.Number(value.sessions.has_value()); if (value.sessions) {
            out.Number(value.sessions->size()); for (const auto& session : *value.sessions) {
                out.Token(session.ordinal); out.Number(session.callJournalLost); out.Number(session.calls.size());
                for (const auto& call : session.calls) {
                    out.Number(static_cast<int>(call.kind)); out.OptionalNumber(call.programIndex); out.Number(call.returned);
                    out.OptionalNumber(call.succeeded); out.Number(call.thread); out.Number(call.traceSize);
                }
                out.Number(session.queued.size()); for (const auto& item : session.queued) queued(item);
                out.Number(session.device.size()); for (const auto& item : session.device) device(item);
                out.Number(session.trace.size()); for (const auto& item : session.trace) trace(item);
                stateList(session.beforeEndStates); deliveryList(session.beforeEndDelivery);
            }
        }
        optionalStrings(value.unresolved); optionalStrings(value.graphUnresolved);
        out.Number(value.sealedSelection.has_value()); if (value.sealedSelection) selection(*value.sealedSelection);
    }
    out.Number(evidence.preparationProgram.has_value()); if (evidence.preparationProgram) {
        const auto& value = *evidence.preparationProgram;
        out.Number(value.identityValid); out.Number(value.resourceUnavailable); out.Number(value.observationUnavailable);
        out.Number(value.hypothesisObservationRecorded); out.Number(value.hypothesisNotMetObserved);
        out.Number(value.localSelectionSnapshotObserved); out.Number(value.sessions.size());
        for (const auto& session : value.sessions) {
            out.Token(session.ordinal); out.Number(session.beginReturnedSuccess); out.Number(session.initialHoverObserved);
            out.OptionalNumber(session.beginStartIndex); out.OptionalNumber(session.beginReturnIndex); out.OptionalNumber(session.hoverTraceIndex);
            out.OptionalNumber(session.hoverEnteredSequence); out.OptionalNumber(session.hoverReturnedSequence);
            out.OptionalNumber(session.selectedHoverEnterPresent); out.OptionalNumber(session.selectedHoverReturnPresent);
        }
        out.Number(value.steps.size());
        for (const auto& step : value.steps) {
            out.Number(step.programIndex); out.Token(step.kind); out.Token(step.session); out.Number(step.wndprocObserved);
            out.Number(step.callReturnedNormally); out.Number(step.callReturnedSuccess); out.Number(step.endDeviceObserved); out.OptionalNumber(step.callStartIndex);
            out.OptionalNumber(step.callReturnIndex); out.OptionalNumber(step.traceIndex); out.OptionalNumber(step.enteredSequence);
            out.OptionalNumber(step.returnedSequence); out.OptionalNumber(step.selectedEnterPresent); out.OptionalNumber(step.selectedReturnPresent);
        }
        strings(value.unresolved);
    }
    out.Number(evidence.preparationMarquee.has_value()); if (evidence.preparationMarquee) {
        const auto& value = *evidence.preparationMarquee;
        out.Number(value.identityValid); out.Number(value.observationUnavailable); out.Number(value.resourceUnavailable);
        out.Number(value.occurrences.size()); for (const auto& item : value.occurrences) {
            out.Number(item.observationId); out.Token(item.session); out.Number(item.programIndex);
            out.Number(item.enterIndex); out.Number(item.returnIndex); out.Number(item.traceIndex);
        }
        out.Number(value.records.size()); for (const auto& item : value.records) {
            out.Number(item.index); out.AppEvent(item.event); out.Bytes(item.blob);
        }
        strings(value.unresolved);
    }
    out.Number(evidence.localSelectionSnapshot.has_value()); if (evidence.localSelectionSnapshot) selection(*evidence.localSelectionSnapshot);
    out.Number(evidence.partialSelection.has_value()); if (evidence.partialSelection) selection(*evidence.partialSelection);
    out.Number(evidence.partialDrag.has_value()); if (evidence.partialDrag) business(*evidence.partialDrag);
    out.Number(evidence.drag.has_value()); if (evidence.drag) dragReceipt(*evidence.drag);
    out.Number(evidence.unstartedDragAttempts.size()); for (const auto& item : evidence.unstartedDragAttempts) dragReceipt(item);
    optionalStrings(evidence.unresolved); optionalStrings(evidence.dragPreconditions);
    optionalStrings(evidence.dragPostconditions); optionalStrings(evidence.dragUnclassified);
}

// Reconstruct both manifest and production flow from the sealed receipt. A
// diagnostic handle can prove local file identity/bytes at inspection time;
// it cannot prove which object Shell opened or certify the final result.
inline BridgeResult::V4VirtualStagingComparison InspectV4VirtualReceiptStaging(
    const ole_driver::V4ConfiguredContentOleRunReceipt& receipt, const V4FamilyValidatedContext& verified,
    const event_graph::V4FamilyIdentityContext& identity, const std::vector<bool>& present,
    const event_graph::V4JoinedGraphVariant& selected, const mixed_fixture::ConfiguredContentBinding& binding,
    const std::shared_ptr<mixed_fixture::ConfiguredContentOwner>& owner, OleDragDropAdapter* adapter,
    const std::shared_ptr<const ole_driver::V3HostEpoch>& epoch, const ole_driver::MessageTrace& trace,
    HWND control, std::size_t byteBudget, std::size_t virtualBudget)
{
    BridgeResult::V4VirtualStagingComparison result;
    try
    {
        const auto manifest = mixed_fixture::BuildVirtualExpectedManifest(verified, identity, byteBudget, virtualBudget);
        if (!manifest.ready || manifest.resourceUnavailable || manifest.members.empty() ||
            manifest.candidateId != receipt.Plan().candidateId ||
            !receipt.SameBoundHost(binding, owner, adapter, epoch, trace, control))
        { result.resourceUnavailable = manifest.resourceUnavailable; result.unresolved.emplace_back("v4_virtual_staging_receipt_or_manifest_unavailable"); return result; }
        const auto flow = ole_driver::InspectV4VirtualFlow(receipt, verified, identity, present, selected,
            binding, owner, adapter, epoch, trace, control, manifest, byteBudget, virtualBudget);
        if (!flow.identityValid || !flow.batchObserved || !flow.batch)
        { result.unresolved.emplace_back("v4_virtual_staging_same_receipt_flow_unavailable"); return result; }
        const auto handles = ole_driver::InspectV4VirtualStagingHandles(flow, byteBudget);
        result.sameReceiptObserved = flow.identityValid;
        if (!handles.logComplete || handles.observationUnavailable)
            result.unresolved.emplace_back("v4_virtual_staging_handle_log_incomplete");
        bool complete = handles.logComplete && !handles.observationUnavailable &&
            flow.members.size() == manifest.members.size() &&
            static_cast<std::size_t>(std::count_if(handles.records.begin(), handles.records.end(), [&](const auto& item) {
                return item.batch == flow.batch; })) == manifest.members.size();
        std::vector<std::uint64_t> seenMembers;
        for (const auto& expected : manifest.members)
        {
            const auto member = std::find_if(flow.members.begin(), flow.members.end(), [&](const auto& item) {
                return item.descriptorIndex == expected.descriptorIndex; });
            if (member == flow.members.end() || std::find_if(std::next(member), flow.members.end(), [&](const auto& item) {
                return item.descriptorIndex == expected.descriptorIndex; }) != flow.members.end() || !member->memberId)
            { complete = false; result.unresolved.emplace_back("v4_virtual_staging_member_identity_unavailable"); continue; }
            if (std::find(seenMembers.begin(), seenMembers.end(), member->memberId) != seenMembers.end())
            { complete = false; result.unresolved.emplace_back("v4_virtual_staging_member_identity_reused"); continue; }
            seenMembers.push_back(member->memberId);
            BridgeResult::V4VirtualStagingComparison::Member item;
            item.descriptorIndex = expected.descriptorIndex; item.batch = flow.batch; item.member = member->memberId;
            const auto handle = std::find_if(handles.records.begin(), handles.records.end(), [&](const auto& value) {
                return value.batch == flow.batch && value.member == member->memberId; });
            if (handle == handles.records.end() || std::find_if(std::next(handle), handles.records.end(), [&](const auto& value) {
                return value.batch == flow.batch && value.member == member->memberId; }) != handles.records.end())
            { complete = false; result.unresolved.emplace_back("v4_virtual_staging_handle_identity_unavailable"); result.members.push_back(item); continue; }
            auto* actualSource = binding.SourceObject();
            if (actualSource && trace.OwnerThread())
                item.sourceAndThreadMatches =
                    handle->source == reinterpret_cast<std::uintptr_t>(static_cast<IDataObject*>(actualSource)) &&
                    handle->thread == trace.OwnerThread();
            if (handle->namedIdentityObserved) item.namedIdentityMatches = handle->nameMatchesHandle;
            if (handle->handleIdentityObserved && member->stagedFileId)
                item.fileIdentityMatches = handle->handleIdentity.VolumeSerialNumber == member->stagedFileId->VolumeSerialNumber &&
                    std::memcmp(handle->handleIdentity.FileId.Identifier, member->stagedFileId->FileId.Identifier,
                        sizeof(handle->handleIdentity.FileId.Identifier)) == 0;
            if (handle->bytesComplete) item.bytesMatch = handle->actualBytes == expected.bytes;
            if (!item.fileIdentityMatches.has_value() || !item.bytesMatch.has_value() ||
                !item.namedIdentityMatches.has_value() || !item.sourceAndThreadMatches.value_or(false)) complete = false;
            result.members.push_back(std::move(item));
        }
        result.comparisonComplete = complete && result.members.size() == manifest.members.size();
        if (!result.comparisonComplete) result.unresolved.emplace_back("v4_virtual_staging_member_comparison_incomplete");
        result.unresolved.emplace_back("v4_virtual_staging_shell_open_identity_and_complete_business_unverified");
    }
    catch (const std::bad_alloc&) { result.resourceUnavailable = true; }
    catch (const std::length_error&) { result.resourceUnavailable = true; }
    catch (...) { result.unresolved.emplace_back("v4_virtual_staging_receipt_comparison_unavailable"); }
    return result;
}

// This uses only the executor-sealed observer and reconstructs the path flow
// from the same receipt. A complete local value comparison does not establish
// Shell's opened object, future quiescence, or full graph/business coverage.
inline BridgeResult::V4SingleOwnedFileBusinessBinding InspectV4SingleOwnedFileBusiness(
    const ole_driver::V4MultiFileOleRunReceipt& receipt, const V4FamilyValidatedContext& verified,
    const event_graph::V4FamilyIdentityContext& identity, const std::vector<bool>& present,
    const event_graph::V4JoinedGraphVariant& selected, const std::shared_ptr<V4IndependentFilePayload>& payload,
    const V4IndependentShellSource& binding, const std::shared_ptr<V4IndependentShellOwner>& owner,
    OleDragDropAdapter* adapter, const std::shared_ptr<const ole_driver::V3HostEpoch>& epoch,
    const ole_driver::MessageTrace& trace, HWND control)
{
    BridgeResult::V4SingleOwnedFileBusinessBinding result;
    try
    {
        const auto business = receipt.SealedBusiness();
        if (!business || !payload || payload->files.size() != 1 || !binding.SameOwner(owner) || !binding.SamePayload(payload) ||
            !receipt.SameBoundHost(binding, owner, adapter, epoch, trace, control) || !receipt.SourceAtExecution() ||
            !receipt.SourceAtExecution()->captured || receipt.SourceAtExecution()->files.size() != 1 ||
            receipt.SourceAtExecution()->candidateId != payload->candidateId || business->members.size() != 1)
        { result.unresolved.emplace_back("v4_single_business_receipt_or_source_unavailable"); return result; }
        const auto flow = ole_driver::InspectV4MultiFilePathFlow(receipt, verified, identity, present, selected,
            payload, binding, owner, adapter, epoch, trace, control, kShellFileOperationCompletedMessage);
        result.sameReceiptObserved = flow.identityValid;
        result.pathFlowObserved = flow.identityValid && flow.valuesCaptured && flow.exactValuesMatch &&
            flow.values.size() == 1 && flow.values[0].modelIndex && *flow.values[0].modelIndex == 0 &&
            business->sourcePayloadObserved && business->modelIndexByReadOrder.size() == 1 &&
            business->modelIndexByReadOrder[0] == 0;
        const auto task = ole_driver::V4FileTaskWindowReader::Multi(receipt, verified, identity, present, selected,
            binding, owner, adapter, epoch, trace, control, kShellFileOperationCompletedMessage);
        if (task.identityValid && task.directCallsiteMatched && task.localChain &&
            task.localChain->readMessage < task.messages.size() && task.localChain->fileMessage < task.messages.size() &&
            task.localChain->readMessage != task.localChain->fileMessage)
        {
            const auto& read = task.messages[task.localChain->readMessage];
            const auto& file = task.messages[task.localChain->fileMessage];
            if (read.returned && file.returned && read.enteredSequence && file.enteredSequence)
            {
                result.readCompletionEntered = read.enteredSequence;
                result.fileCompletionEntered = file.enteredSequence;
                result.completionObserved = result.pathFlowObserved && receipt.CompletionTraceCaptured() && business->completion &&
                    business->completion->signal == ole_driver::CompletionSignal::completed_messages &&
                    business->completion->sourceEntered == read.enteredSequence &&
                    business->completion->commitEntered == file.enteredSequence;
            }
        }
        const auto tracked = ole_driver::InspectV4MultiTrackedShellCopy(receipt, verified, identity, present, selected,
            payload, binding, owner, adapter, epoch, trace, control, kShellFileOperationCompletedMessage);
        if (tracked.resourceUnavailable) result.resourceUnavailable = true;
        if (tracked.identityValid && tracked.localItemObserved && task.identityValid && task.localChain &&
            tracked.task == task.localChain->task && tracked.fileWork == task.localChain->fileWork &&
            result.pathFlowObserved && result.completionObserved)
            result.trackedCopyObserved = true;
        if (!result.trackedCopyObserved)
            result.unresolved.emplace_back("v4_single_business_tracked_copy_chain_unavailable");
        const auto& source = receipt.SourceAtExecution()->files[0];
        const auto& observed = business->members[0];
        if (observed.sourceId)
            result.sourceSnapshotMatches = observed.source == source.path && observed.expectedBytes == source.bytes &&
                behavior::OwnedProfileDesktopRoot::SameId(*observed.sourceId, source.id);
        if (observed.outputId && observed.outputBytes && observed.outputBytes->observed)
            result.outputBytesMatch = observed.outputBytes->matches;
        if (observed.outputCell && observed.reloadedCell && observed.reloadedBytes && observed.reloadedBytes->observed)
            result.reloadMatches = behavior::Same(*observed.outputCell, *observed.reloadedCell) && observed.reloadedBytes->matches;
        result.firstLandingMatches = business->firstReceivedLandingMatched;
        if (!result.pathFlowObserved || !result.completionObserved || !result.sourceSnapshotMatches.value_or(false) ||
            !result.outputBytesMatch.value_or(false) || !result.reloadMatches.value_or(false) ||
            !result.firstLandingMatches.value_or(false))
            result.unresolved.emplace_back("v4_single_business_local_match_missing_or_unavailable");
        result.unresolved.emplace_back("v4_single_business_shell_open_identity_and_late_work_unverified");
        result.unresolved.emplace_back("v4_single_business_complete_graph_and_durable_layout_unverified");
    }
    catch (const std::bad_alloc&) { result.resourceUnavailable = true; }
    catch (const std::length_error&) { result.resourceUnavailable = true; }
    catch (...) { result.unresolved.emplace_back("v4_single_business_receipt_comparison_unavailable"); }
    return result;
}

// Reconstruct local path and completion observations from the same sealed
// receipt. These comparisons do not prove the complete graph or business pass.
inline BridgeResult::V4CollectionBusinessBinding InspectV4CollectionBusiness(
    const ole_driver::V4MultiFileOleRunReceipt& receipt, const V4FamilyValidatedContext& verified,
    const event_graph::V4FamilyIdentityContext& identity, const std::vector<bool>& present,
    const event_graph::V4JoinedGraphVariant& selected, const std::shared_ptr<V4IndependentFilePayload>& payload,
    const V4IndependentShellSource& binding, const std::shared_ptr<V4IndependentShellOwner>& owner,
    OleDragDropAdapter* adapter, const std::shared_ptr<const ole_driver::V3HostEpoch>& epoch,
    const ole_driver::MessageTrace& trace, HWND control)
{
    BridgeResult::V4CollectionBusinessBinding result;
    try
    {
        const auto business = receipt.SealedCollectionBusiness();
        if (!business || !payload || payload->files.size() != 1 || !binding.SameOwner(owner) || !binding.SamePayload(payload) ||
            !receipt.SameBoundHost(binding, owner, adapter, epoch, trace, control) || !receipt.SourceAtExecution() ||
            !receipt.SourceAtExecution()->captured || receipt.SourceAtExecution()->files.size() != 1 ||
            receipt.SourceAtExecution()->candidateId != payload->candidateId || business->candidateId != payload->candidateId)
        { result.unresolved.emplace_back("v4_collection_business_receipt_or_source_unavailable"); return result; }
        const auto flow = ole_driver::InspectV4MultiFilePathFlow(receipt, verified, identity, present, selected,
            payload, binding, owner, adapter, epoch, trace, control, kShellFileOperationCompletedMessage);
        result.sameReceiptObserved = flow.identityValid;
        result.pathFlowObserved = flow.identityValid && flow.valuesCaptured && flow.exactValuesMatch &&
            flow.values.size() == 1 && flow.values[0].modelIndex && *flow.values[0].modelIndex == 0;
        const auto task = ole_driver::V4FileTaskWindowReader::Multi(receipt, verified, identity, present, selected,
            binding, owner, adapter, epoch, trace, control, kShellFileOperationCompletedMessage);
        if (task.identityValid && task.directCallsiteMatched && task.localChain &&
            task.localChain->readMessage < task.messages.size() && task.localChain->fileMessage < task.messages.size() &&
            task.localChain->readMessage != task.localChain->fileMessage)
        {
            const auto& read = task.messages[task.localChain->readMessage];
            const auto& file = task.messages[task.localChain->fileMessage];
            if (read.returned && file.returned && read.enteredSequence && file.enteredSequence)
            {
                result.readCompletionEntered = read.enteredSequence; result.fileCompletionEntered = file.enteredSequence;
                result.completionObserved = result.pathFlowObserved && receipt.CompletionTraceCaptured() && business->completion &&
                    business->completion->signal == ole_driver::CompletionSignal::completed_messages &&
                    business->completion->sourceEntered == read.enteredSequence &&
                    business->completion->commitEntered == file.enteredSequence;
            }
        }
        const auto& source = receipt.SourceAtExecution()->files.front();
        if (business->sourceId)
            result.sourceSnapshotMatches = business->source == source.path && business->expectedBytes == source.bytes &&
                behavior::OwnedProfileDesktopRoot::SameId(*business->sourceId, source.id);
        result.sourcePreserved = business->sourcePreserved; result.outputBytesMatch = business->outputBytesMatch;
        result.membershipMatches = business->membershipMatches; result.reloadMatches = business->reloadedMembershipMatches;
        result.widgetPreserved = business->widgetPreserved; result.reloadedWidgetPreserved = business->reloadedWidgetPreserved;
        result.noDesktopDuplicate = business->noDesktopDuplicate;
        if (!result.completionObserved || !result.sourceSnapshotMatches.value_or(false) ||
            !result.sourcePreserved.value_or(false) || !result.outputBytesMatch.value_or(false) ||
            !result.membershipMatches.value_or(false) || !result.reloadMatches.value_or(false) ||
            !result.widgetPreserved.value_or(false) || !result.reloadedWidgetPreserved.value_or(false) ||
            !result.noDesktopDuplicate.value_or(false))
            result.unresolved.emplace_back("v4_collection_business_local_comparison_unavailable_or_mismatch");
        result.unresolved.emplace_back("v4_collection_business_shell_open_and_late_work_unverified");
        result.unresolved.emplace_back("v4_collection_business_full_graph_and_durable_layout_unverified");
    }
    catch (const std::bad_alloc&) { result.resourceUnavailable = true; }
    catch (const std::length_error&) { result.resourceUnavailable = true; }
    catch (...) { result.unresolved.emplace_back("v4_collection_business_receipt_observation_unavailable"); }
    return result;
}

inline BridgeResult::V4DesktopFilesBusinessBinding InspectV4DesktopFilesBusiness(
    const ole_driver::V4MultiFileOleRunReceipt& receipt, const V4FamilyValidatedContext& verified,
    const event_graph::V4FamilyIdentityContext& identity, const std::vector<bool>& present,
    const event_graph::V4JoinedGraphVariant& selected, const std::shared_ptr<V4IndependentFilePayload>& payload,
    const V4IndependentShellSource& binding, const std::shared_ptr<V4IndependentShellOwner>& owner,
    OleDragDropAdapter* adapter, const std::shared_ptr<const ole_driver::V3HostEpoch>& epoch,
    const ole_driver::MessageTrace& trace, HWND control)
{
    BridgeResult::V4DesktopFilesBusinessBinding result;
    try
    {
        const auto business = receipt.SealedDesktopFilesBusiness();
        if (!business || !payload || payload->files.size() != 1 || !binding.SameOwner(owner) || !binding.SamePayload(payload) ||
            !receipt.SameBoundHost(binding, owner, adapter, epoch, trace, control) || !receipt.SourceAtExecution() ||
            !receipt.SourceAtExecution()->captured || receipt.SourceAtExecution()->files.size() != 1 ||
            receipt.SourceAtExecution()->candidateId != payload->candidateId || business->candidateId != payload->candidateId)
        { result.unresolved.emplace_back("v4_desktop_files_business_receipt_or_source_unavailable"); return result; }
        const auto flow = ole_driver::InspectV4MultiFilePathFlow(receipt, verified, identity, present, selected,
            payload, binding, owner, adapter, epoch, trace, control, kShellFileOperationCompletedMessage);
        result.sameReceiptObserved = flow.identityValid;
        result.pathFlowObserved = flow.identityValid && flow.valuesCaptured && flow.exactValuesMatch &&
            flow.values.size() == 1 && flow.values[0].modelIndex && *flow.values[0].modelIndex == 0;
        const auto task = ole_driver::V4FileTaskWindowReader::Multi(receipt, verified, identity, present, selected,
            binding, owner, adapter, epoch, trace, control, kShellFileOperationCompletedMessage);
        if (task.identityValid && task.directCallsiteMatched && task.localChain &&
            task.localChain->readMessage < task.messages.size() && task.localChain->fileMessage < task.messages.size() &&
            task.localChain->readMessage != task.localChain->fileMessage)
        {
            const auto& read = task.messages[task.localChain->readMessage];
            const auto& file = task.messages[task.localChain->fileMessage];
            if (read.returned && file.returned && read.enteredSequence && file.enteredSequence)
            {
                result.readCompletionEntered = read.enteredSequence; result.fileCompletionEntered = file.enteredSequence;
                result.completionObserved = result.pathFlowObserved && receipt.CompletionTraceCaptured() && business->completion &&
                    business->completion->signal == ole_driver::CompletionSignal::completed_messages &&
                    business->completion->sourceEntered == read.enteredSequence &&
                    business->completion->commitEntered == file.enteredSequence;
            }
        }
        const auto& source = receipt.SourceAtExecution()->files.front();
        if (business->sourceId)
            result.sourceSnapshotMatches = business->source == source.path && business->expectedBytes == source.bytes &&
                behavior::OwnedProfileDesktopRoot::SameId(*business->sourceId, source.id);
        result.sourcePreserved = business->sourcePreserved; result.outputBytesMatch = business->outputBytesMatch;
        result.membershipMatches = business->membershipMatches; result.reloadMatches = business->reloadedMembershipMatches;
        result.widgetPreserved = business->widgetPreserved; result.reloadedWidgetPreserved = business->reloadedWidgetPreserved;
        result.noDesktopDuplicate = business->noDesktopDuplicate;
        if (!result.completionObserved || !result.sourceSnapshotMatches.value_or(false) ||
            !result.sourcePreserved.value_or(false) || !result.outputBytesMatch.value_or(false) ||
            !result.membershipMatches.value_or(false) || !result.reloadMatches.value_or(false) ||
            !result.widgetPreserved.value_or(false) || !result.reloadedWidgetPreserved.value_or(false) ||
            !result.noDesktopDuplicate.value_or(false))
            result.unresolved.emplace_back("v4_desktop_files_business_local_comparison_unavailable_or_mismatch");
        result.unresolved.emplace_back("v4_desktop_files_business_shell_open_and_late_work_unverified");
        result.unresolved.emplace_back("v4_desktop_files_business_full_graph_and_durable_layout_unverified");
    }
    catch (const std::bad_alloc&) { result.resourceUnavailable = true; }
    catch (const std::length_error&) { result.resourceUnavailable = true; }
    catch (...) { result.unresolved.emplace_back("v4_desktop_files_business_receipt_observation_unavailable"); }
    return result;
}

inline BridgeResult::V4FolderMappingBusinessBinding InspectV4FolderMappingBusiness(
    const ole_driver::V4MultiFileOleRunReceipt& receipt, const V4FamilyValidatedContext& verified,
    const event_graph::V4FamilyIdentityContext& identity, const std::vector<bool>& present,
    const event_graph::V4JoinedGraphVariant& selected, const std::shared_ptr<V4IndependentFilePayload>& payload,
    const V4IndependentShellSource& binding, const std::shared_ptr<V4IndependentShellOwner>& owner,
    OleDragDropAdapter* adapter, const std::shared_ptr<const ole_driver::V3HostEpoch>& epoch,
    const ole_driver::MessageTrace& trace, HWND control)
{
    BridgeResult::V4FolderMappingBusinessBinding result;
    try
    {
        const auto business = receipt.SealedFolderMappingBusiness();
        if (!business || !payload || payload->files.size() != 1 || !binding.SameOwner(owner) || !binding.SamePayload(payload) ||
            !receipt.SameBoundHost(binding, owner, adapter, epoch, trace, control) || !receipt.SourceAtExecution() ||
            !receipt.SourceAtExecution()->captured || receipt.SourceAtExecution()->files.size() != 1 ||
            receipt.SourceAtExecution()->candidateId != payload->candidateId || business->candidateId != payload->candidateId)
        { result.unresolved.emplace_back("v4_folder_mapping_business_receipt_or_source_unavailable"); return result; }
        const auto flow = ole_driver::InspectV4MultiFilePathFlow(receipt, verified, identity, present, selected,
            payload, binding, owner, adapter, epoch, trace, control, kShellFileOperationCompletedMessage);
        result.sameReceiptObserved = flow.identityValid;
        result.pathFlowObserved = flow.identityValid && flow.valuesCaptured && flow.exactValuesMatch &&
            flow.values.size() == 1 && flow.values[0].modelIndex && *flow.values[0].modelIndex == 0;
        const auto task = ole_driver::V4FileTaskWindowReader::Multi(receipt, verified, identity, present, selected,
            binding, owner, adapter, epoch, trace, control, kShellFileOperationCompletedMessage);
        if (task.identityValid && task.directCallsiteMatched && task.localChain &&
            task.localChain->readMessage < task.messages.size() && task.localChain->fileMessage < task.messages.size() &&
            task.localChain->readMessage != task.localChain->fileMessage)
        {
            const auto& read = task.messages[task.localChain->readMessage];
            const auto& file = task.messages[task.localChain->fileMessage];
            if (read.returned && file.returned && read.enteredSequence && file.enteredSequence)
            {
                result.readCompletionEntered = read.enteredSequence; result.fileCompletionEntered = file.enteredSequence;
                result.completionObserved = result.pathFlowObserved && receipt.CompletionTraceCaptured() && business->completion &&
                    business->completion->signal == ole_driver::CompletionSignal::completed_messages &&
                    business->completion->sourceEntered == read.enteredSequence &&
                    business->completion->commitEntered == file.enteredSequence;
            }
        }
        const auto& source = receipt.SourceAtExecution()->files.front();
        if (business->sourceId)
            result.sourceSnapshotMatches = business->source == source.path && business->expectedBytes == source.bytes &&
                behavior::OwnedProfileDesktopRoot::SameId(*business->sourceId, source.id);
        result.sourcePreserved = business->sourcePreserved; result.outputBytesMatch = business->outputBytesMatch;
        result.membershipMatches = business->membershipMatches; result.reloadMatches = business->reloadedMembershipMatches;
        result.widgetPreserved = business->widgetPreserved; result.reloadedWidgetPreserved = business->reloadedWidgetPreserved;
        result.noDesktopDuplicate = business->noDesktopDuplicate;
        result.mappedDirectoryPreserved = business->mappedDirectoryPreserved;
        result.sentinelPreserved = business->sentinelPreserved;
        result.existingPreserved = business->existingPreserved;
        if (!result.completionObserved || !result.sourceSnapshotMatches.value_or(false) ||
            !result.sourcePreserved.value_or(false) || !result.outputBytesMatch.value_or(false) ||
            !result.membershipMatches.value_or(false) || !result.reloadMatches.value_or(false) ||
            !result.widgetPreserved.value_or(false) || !result.reloadedWidgetPreserved.value_or(false) ||
            !result.noDesktopDuplicate.value_or(false) || !result.mappedDirectoryPreserved.value_or(false) ||
            !result.sentinelPreserved.value_or(false) || !result.existingPreserved.value_or(false))
            result.unresolved.emplace_back("v4_folder_mapping_business_local_comparison_unavailable_or_mismatch");
        result.unresolved.emplace_back("v4_folder_mapping_business_shell_open_and_late_work_unverified");
        result.unresolved.emplace_back("v4_folder_mapping_business_full_graph_and_durable_layout_unverified");
    }
    catch (const std::bad_alloc&) { result.resourceUnavailable = true; }
    catch (const std::length_error&) { result.resourceUnavailable = true; }
    catch (...) { result.unresolved.emplace_back("v4_folder_mapping_business_receipt_observation_unavailable"); }
    return result;
}

inline BridgeResult::V4LeaveBusinessBinding InspectV4LeaveBusiness(
    const ole_driver::V4MultiFileOleRunReceipt& receipt, const std::shared_ptr<V4IndependentFilePayload>& payload,
    const V4IndependentShellSource& binding, const std::shared_ptr<V4IndependentShellOwner>& owner,
    OleDragDropAdapter* adapter, const std::shared_ptr<const ole_driver::V3HostEpoch>& epoch,
    const ole_driver::MessageTrace& trace, HWND control)
{
    BridgeResult::V4LeaveBusinessBinding result;
    try
    {
        const auto business = receipt.SealedLeaveBusiness();
        if (!business || !payload || payload->files.size() != 1 || !binding.SameOwner(owner) || !binding.SamePayload(payload) ||
            !receipt.SameBoundHost(binding, owner, adapter, epoch, trace, control) || !receipt.SourceAtExecution() ||
            !receipt.SourceAtExecution()->captured || receipt.SourceAtExecution()->files.size() != 1 ||
            receipt.SourceAtExecution()->candidateId != payload->candidateId || business->candidateId != payload->candidateId)
        { result.unresolved.emplace_back("v4_leave_business_receipt_or_source_unavailable"); return result; }
        result.sameReceiptObserved = true;
        const auto& calls = receipt.CapturedReturns();
        result.leaveReturnObserved = calls.size() == 2 && calls.front().observation.kind == ole_driver::Kind::enter &&
            calls.back().observation.kind == ole_driver::Kind::leave && SUCCEEDED(calls.back().observation.result) &&
            receipt.CompletionTraceCaptured() && business->leaveAwaitReturned;
        const auto& source = receipt.SourceAtExecution()->files.front();
        if (business->sourceId)
            result.sourceSnapshotMatches = business->source == source.path && business->expectedBytes == source.bytes &&
                behavior::OwnedProfileDesktopRoot::SameId(*business->sourceId, source.id);
        result.sourcePreserved = business->sourcePreserved; result.sentinelPreserved = business->sentinelPreserved;
        result.initialCellsPreserved = business->initialCellsPreserved; result.layoutBytesUnchanged = business->layoutBytesUnchanged;
        result.layoutFileIdentityUnchanged = business->layoutFileIdentityUnchanged;
        result.sourceLogAvailable = business->sourceLogAvailable; result.outputCount = business->outputCount;
        result.finalItemCount = business->finalItemCount; result.starts = business->starts; result.ends = business->ends;
        result.fileReads = business->fileReads;
        result.unresolved.emplace_back("v4_leave_bounded_event_absence_and_late_work_unverified");
        result.unresolved.emplace_back("v4_leave_complete_graph_and_business_unverified");
    }
    catch (const std::bad_alloc&) { result.resourceUnavailable = true; }
    catch (const std::length_error&) { result.resourceUnavailable = true; }
    catch (...) { result.unresolved.emplace_back("v4_leave_business_receipt_observation_unavailable"); }
    return result;
}

inline BridgeResult::V4RejectBusinessBinding InspectV4RejectBusiness(
    const ole_driver::V4MultiFileOleRunReceipt& receipt, const std::shared_ptr<V4IndependentFilePayload>& payload,
    const V4IndependentShellSource& binding, const std::shared_ptr<V4IndependentShellOwner>& owner,
    OleDragDropAdapter* adapter, const std::shared_ptr<const ole_driver::V3HostEpoch>& epoch,
    const ole_driver::MessageTrace& trace, HWND control)
{
    BridgeResult::V4RejectBusinessBinding result;
    try
    {
        const auto business = receipt.SealedRejectBusiness();
        if (!business || !payload || payload->files.size() != 1 || !binding.SameOwner(owner) || !binding.SamePayload(payload) ||
            !receipt.SameBoundHost(binding, owner, adapter, epoch, trace, control) || !receipt.SourceAtExecution() ||
            !receipt.SourceAtExecution()->captured || receipt.SourceAtExecution()->files.size() != 1 ||
            receipt.SourceAtExecution()->candidateId != payload->candidateId || business->candidateId != payload->candidateId)
        { result.unresolved.emplace_back("v4_reject_business_receipt_or_source_unavailable"); return result; }
        result.sameReceiptObserved = true;
        const auto& calls = receipt.CapturedReturns();
        result.rejectReturnObserved = calls.size() == 2 && calls.front().observation.kind == ole_driver::Kind::enter &&
            calls.back().observation.kind == ole_driver::Kind::drop && SUCCEEDED(calls.back().observation.result) && calls.back().observation.returnedEffect &&
            *calls.back().observation.returnedEffect == DROPEFFECT_NONE && receipt.CompletionTraceCaptured() && business->rejectAwaitReturned;
        const auto& source = receipt.SourceAtExecution()->files.front();
        if (business->sourceId)
            result.sourceSnapshotMatches = business->source == source.path && business->expectedBytes == source.bytes &&
                behavior::OwnedProfileDesktopRoot::SameId(*business->sourceId, source.id);
        result.sourcePreserved = business->sourcePreserved; result.sentinelPreserved = business->sentinelPreserved;
        result.initialCellsPreserved = business->initialCellsPreserved; result.layoutBytesUnchanged = business->layoutBytesUnchanged;
        result.layoutFileIdentityUnchanged = business->layoutFileIdentityUnchanged;
        result.sourceLogAvailable = business->sourceLogAvailable; result.outputCount = business->outputCount;
        result.finalItemCount = business->finalItemCount; result.starts = business->starts; result.ends = business->ends;
        result.fileReads = business->fileReads;
        result.unresolved.emplace_back("v4_reject_bounded_event_absence_and_late_work_unverified");
        result.unresolved.emplace_back("v4_reject_complete_graph_and_business_unverified");
    }
    catch (const std::bad_alloc&) { result.resourceUnavailable = true; }
    catch (const std::length_error&) { result.resourceUnavailable = true; }
    catch (...) { result.unresolved.emplace_back("v4_reject_business_receipt_observation_unavailable"); }
    return result;
}

inline BridgeResult::V4DirectoryBusinessBinding InspectV4DirectoryBusiness(
    const ole_driver::V4DirectoryOleRunReceipt& receipt, const V4FamilyValidatedContext& verified,
    const event_graph::V4FamilyIdentityContext& identity, const std::vector<bool>& present,
    const event_graph::V4JoinedGraphVariant& selected, const std::shared_ptr<directory_fixture::DirectoryCopyLease>& lease,
    const directory_fixture::DirectoryShellBinding& binding, const std::shared_ptr<directory_fixture::DirectoryShellOwner>& owner,
    OleDragDropAdapter* adapter, const std::shared_ptr<const ole_driver::V3HostEpoch>& epoch,
    const ole_driver::MessageTrace& trace, HWND control)
{
    BridgeResult::V4DirectoryBusinessBinding result;
    try
    {
        const auto business = receipt.SealedDirectoryBusiness();
        if (!business || !lease || !receipt.SameBoundHost(lease, binding, owner, adapter, epoch, trace, control) ||
            !receipt.SourceAtExecution() || !receipt.SourceAtExecution()->inputObserved ||
            receipt.SourceAtExecution()->resourceUnavailable || !receipt.SourceAtExecution()->fixtureLeaseOverlapObserved ||
            receipt.SourceAtExecution()->candidateId != business->candidateId || lease->CandidateId() != business->candidateId)
        { result.unresolved.emplace_back("v4_directory_business_receipt_or_source_unavailable"); return result; }
        result.sourceLeaseMatches = lease->Source() == business->source;
        const auto flow = ole_driver::InspectV4DirectoryPathFlow(receipt, verified, identity, present, selected,
            lease, binding, owner, adapter, epoch, trace, control, kShellFileOperationCompletedMessage);
        result.sameReceiptObserved = flow.identityValid;
        result.pathFlowObserved = flow.identityValid && flow.valuesCaptured && flow.exactValuesMatch &&
            flow.values.size() == 1 && business->sourcePayloadObserved;
        const auto task = ole_driver::InspectV4DirectoryTaskWindows(receipt, verified, identity, present, selected,
            lease, binding, owner, adapter, epoch, trace, control, kShellFileOperationCompletedMessage);
        if (task.identityValid && task.directCallsiteMatched && task.localChain &&
            task.localChain->readMessage < task.messages.size() && task.localChain->fileMessage < task.messages.size() &&
            task.localChain->readMessage != task.localChain->fileMessage)
        {
            const auto& read = task.messages[task.localChain->readMessage]; const auto& file = task.messages[task.localChain->fileMessage];
            if (read.returned && file.returned && read.enteredSequence && file.enteredSequence)
            {
                result.readCompletionEntered = read.enteredSequence; result.fileCompletionEntered = file.enteredSequence;
                result.completionObserved = result.pathFlowObserved && receipt.CompletionTraceCaptured() && business->completion &&
                    business->completion->signal == ole_driver::CompletionSignal::completed_messages &&
                    business->completion->sourceEntered == read.enteredSequence && business->completion->commitEntered == file.enteredSequence;
            }
        }
        result.landingMatched = business->landingMatched;
        result.independentAssertionsCompleted = business->independentAssertionsCompleted;
        result.reloadObserved = business->reloadedCount.has_value() && business->reloadedCell.has_value();
        result.unresolved.emplace_back("v4_directory_business_shell_open_and_late_work_unverified");
        result.unresolved.emplace_back("v4_directory_business_full_graph_and_durable_layout_unverified");
    }
    catch (const std::bad_alloc&) { result.resourceUnavailable = true; }
    catch (const std::length_error&) { result.resourceUnavailable = true; }
    catch (...) { result.unresolved.emplace_back("v4_directory_business_receipt_observation_unavailable"); }
    return result;
}

inline BridgeResult::V4GroupBusinessBinding InspectV4GroupBusiness(
    const ole_driver::V4MultiFileOleRunReceipt& receipt, const V4FamilyValidatedContext& verified,
    const event_graph::V4FamilyIdentityContext& identity, const std::vector<bool>& present,
    const event_graph::V4JoinedGraphVariant& selected, const std::shared_ptr<V4IndependentFilePayload>& payload,
    const V4IndependentShellSource& binding, const std::shared_ptr<V4IndependentShellOwner>& owner,
    OleDragDropAdapter* adapter, const std::shared_ptr<const ole_driver::V3HostEpoch>& epoch,
    const ole_driver::MessageTrace& trace, HWND control)
{
    BridgeResult::V4GroupBusinessBinding result;
    try
    {
        const auto business = receipt.SealedGroupBusiness();
        if (!business || !payload || payload->files.size() != 1 || !binding.SameOwner(owner) || !binding.SamePayload(payload) ||
            !receipt.SameBoundHost(binding, owner, adapter, epoch, trace, control) || !receipt.SourceAtExecution() ||
            !receipt.SourceAtExecution()->captured || receipt.SourceAtExecution()->files.size() != 1 ||
            receipt.SourceAtExecution()->candidateId != payload->candidateId || business->candidateId != payload->candidateId)
        { result.unresolved.emplace_back("v4_group_business_receipt_or_source_unavailable"); return result; }
        const auto flow = ole_driver::InspectV4MultiFilePathFlow(receipt, verified, identity, present, selected,
            payload, binding, owner, adapter, epoch, trace, control, kShellFileOperationCompletedMessage);
        result.sameReceiptObserved = flow.identityValid;
        result.pathFlowObserved = flow.identityValid && flow.valuesCaptured && flow.exactValuesMatch &&
            flow.values.size() == 1 && flow.values[0].modelIndex && *flow.values[0].modelIndex == 0;
        const auto task = ole_driver::V4FileTaskWindowReader::Multi(receipt, verified, identity, present, selected,
            binding, owner, adapter, epoch, trace, control, kShellFileOperationCompletedMessage);
        if (task.identityValid && task.directCallsiteMatched && task.localChain &&
            task.localChain->readMessage < task.messages.size() && task.localChain->fileMessage < task.messages.size() &&
            task.localChain->readMessage != task.localChain->fileMessage)
        {
            const auto& read = task.messages[task.localChain->readMessage];
            const auto& file = task.messages[task.localChain->fileMessage];
            if (read.returned && file.returned && read.enteredSequence && file.enteredSequence)
            {
                result.readCompletionEntered = read.enteredSequence; result.fileCompletionEntered = file.enteredSequence;
                result.completionObserved = result.pathFlowObserved && receipt.CompletionTraceCaptured() && business->completion &&
                    business->completion->signal == ole_driver::CompletionSignal::completed_messages &&
                    business->completion->sourceEntered == read.enteredSequence && business->completion->commitEntered == file.enteredSequence;
            }
        }
        const auto& source = receipt.SourceAtExecution()->files.front();
        if (business->sourceId)
            result.sourceSnapshotMatches = business->source == source.path && business->expectedBytes == source.bytes &&
                behavior::OwnedProfileDesktopRoot::SameId(*business->sourceId, source.id);
        result.sourcePreserved = business->sourcePreserved; result.existingPreserved = business->existingPreserved;
        result.outputBytesMatch = business->outputBytesMatch; result.membershipMatches = business->membershipMatches;
        result.reloadMatches = business->reloadedMembershipMatches; result.rootPreserved = business->rootPreserved;
        result.activePreserved = business->activePreserved; result.inactivePreserved = business->inactivePreserved;
        result.reloadedRootPreserved = business->reloadedRootPreserved; result.reloadedActivePreserved = business->reloadedActivePreserved;
        result.reloadedInactivePreserved = business->reloadedInactivePreserved; result.noDesktopDuplicate = business->noDesktopDuplicate;
        result.mappedDirectoryPreserved = business->mappedDirectoryPreserved; result.sentinelPreserved = business->sentinelPreserved;
        result.unresolved.emplace_back("v4_group_business_shell_open_and_late_work_unverified");
        result.unresolved.emplace_back("v4_group_business_full_graph_and_durable_layout_unverified");
    }
    catch (const std::bad_alloc&) { result.resourceUnavailable = true; }
    catch (const std::length_error&) { result.resourceUnavailable = true; }
    catch (...) { result.unresolved.emplace_back("v4_group_business_receipt_observation_unavailable"); }
    return result;
}

// Produce a bounded, replayable graph page for an explicitly selected
// candidate under a still-owned input root. This is a preview only: neither
// a page nor a selected graph authorizes fixture creation or dispatch.
struct V4OwnedSingleFileGraphPage
{
    bool candidateReady = false, resourceUnavailable = false;
    Candidate candidate;
    std::string candidateId;
    std::vector<std::string> candidateUnresolved;
    event_graph::V4JoinedGraphPage page;
};
inline V4OwnedSingleFileGraphPage PrepareV4OwnedSingleFileGraphPage(
    const V4OwnedFileFamilySeed& seed, const V4AxisBudget& axisBudget,
    const std::vector<std::pair<std::string, std::string>>& choices,
    event_graph::V4JoinedGraphCursor cursor, std::size_t workBudget, std::size_t outputLimit)
{
    V4OwnedSingleFileGraphPage result;
    result.page.cursor = std::move(cursor);
    try
    {
        const auto candidate = PrepareV4OwnedFileCandidate(seed, axisBudget, choices);
        result.resourceUnavailable = candidate.resourceUnavailable;
        result.candidateUnresolved = candidate.unresolved;
        if (!candidate.ready || candidate.resourceUnavailable) return result;
        const auto identity = event_graph::V4FamilyIdentityContext::Create(seed.Family(), axisBudget);
        const auto verified = V4FamilyValidatedContext::Create(seed.Family(), axisBudget, candidate.candidate);
        if (identity.Status() == V4FamilyStatus::resource_unavailable ||
            verified.Status() == V4CoordinateStatus::resource_unavailable)
        { result.resourceUnavailable = true; return result; }
        if (identity.Status() != V4FamilyStatus::schema_only ||
            verified.Status() != V4CoordinateStatus::coordinate_ready ||
            identity.FamilyBinding() != seed.Binding() || !seed.StillOwned())
        { result.candidateUnresolved.emplace_back("v4_owned_graph_context_unavailable"); return result; }
        auto page = event_graph::V4JoinedGraph::Enumerate(identity, verified, result.page.cursor, workBudget, outputLimit);
        if (page.resourceUnavailable) { result.resourceUnavailable = true; return result; }
        if (!page.errors.empty()) { result.page.errors = std::move(page.errors); return result; }
        if (!seed.StillOwned())
        { result.candidateUnresolved.emplace_back("v4_owned_graph_root_changed"); return result; }
        result.candidate = verified.Coordinate(); result.candidateId = candidate.candidateId;
        result.page = std::move(page); result.candidateReady = true;
        return result;
    }
    catch (const std::bad_alloc&) { result.resourceUnavailable = true; return result; }
    catch (const std::length_error&) { result.resourceUnavailable = true; return result; }
    catch (...) { result.candidateUnresolved.emplace_back("v4_owned_graph_observation_unavailable"); return result; }
}

// Rebind only the input path after the child has created and pinned its own
// source root. The original case is independently replayed; graph matching
// is bounded and resumable, and never represents an execution result.
struct V4OwnedTemplateCasePage
{
    bool matched = false, exhausted = false, resourceUnavailable = false, cursorAdvanced = false;
    std::size_t workUsed = 0;
    event_graph::V4JoinedGraphCursor cursor;
    std::string caseId, candidateId, graphId;
    std::vector<bool> present;
    std::string_view reason;
};
inline V4OwnedTemplateCasePage PrepareV4OwnedTemplateCasePage(
    std::string_view templateCaseId, const V4OwnedFileFamilySeed& seed,
    const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget,
    std::size_t tokenBudget, std::size_t caseByteBudget,
    event_graph::V4JoinedGraphCursor cursor, std::size_t workBudget)
{
    V4OwnedTemplateCasePage out; out.cursor = std::move(cursor);
    try
    {
        const auto source = v3_scheduler::ReplayV4StructuredCaseId(templateCaseId, coordinateBudget,
            axisBudget, tokenBudget, caseByteBudget);
        if (source.status == v3_scheduler::V4StructuredCaseStatus::resource_unavailable)
        { out.resourceUnavailable = true; out.reason = "v4_owned_template_replay_resource_unavailable"; return out; }
        if (source.status != v3_scheduler::V4StructuredCaseStatus::unexecuted || !source.family ||
            !source.candidate || !source.oleGraph || source.nativeGraph ||
            source.family->a.kind != V4SourceKind::file_only || source.family->a.filePaths.size() != 1 ||
            source.family->a.filePaths.front().index != "0" ||
            source.family->a.filePaths.front().kind != V4PayloadKind::exact || source.family->a.filePaths.front().base != V4PathBase::raw ||
            seed.Family().a.filePaths.size() != 1 || seed.Family().a.filePaths.front().index != "0" ||
            seed.Family().a.filePaths.front().kind != V4PayloadKind::exact || seed.Family().a.filePaths.front().base != V4PathBase::raw || !seed.StillOwned())
        { out.reason = "v4_owned_template_identity_unavailable"; return out; }
        const auto fileName = [](std::string_view hex) -> std::optional<std::wstring> {
            if (hex.empty() || hex.size() % 4) return std::nullopt;
            const auto digit = [](char c) -> int {
                return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            };
            std::wstring path; path.reserve(hex.size() / 4);
            for (std::size_t i = 0; i < hex.size(); i += 4)
            {
                const int a = digit(hex[i]), b = digit(hex[i + 1]), c = digit(hex[i + 2]), d = digit(hex[i + 3]);
                if (a < 0 || b < 0 || c < 0 || d < 0) return std::nullopt;
                const auto unit = static_cast<unsigned>((a << 4) | b | ((c << 4 | d) << 8));
                if (!unit) return std::nullopt;
                path.push_back(static_cast<wchar_t>(unit));
            }
            const std::filesystem::path parsed(path);
            const auto name = parsed.filename().native();
            if (!parsed.is_absolute() || name.empty() || name == L"." || name == L"..") return std::nullopt;
            return name;
        };
        const auto originalName = fileName(source.family->a.filePaths.front().utf16leHex);
        const auto newName = fileName(seed.Family().a.filePaths.front().utf16leHex);
        if (!originalName || !newName || *originalName != *newName)
        { out.reason = "v4_owned_template_file_name_mismatch"; return out; }
        auto rebound = *source.family;
        rebound.a.filePaths = seed.Family().a.filePaths;
        if (V4FamilyBinding(rebound) != seed.Binding())
        { out.reason = "v4_owned_template_non_path_family_mismatch"; return out; }
        const auto originalIdentity = event_graph::V4FamilyIdentityContext::Create(*source.family, axisBudget);
        const auto newIdentity = event_graph::V4FamilyIdentityContext::Create(seed.Family(), axisBudget);
        if (originalIdentity.Status() == V4FamilyStatus::resource_unavailable ||
            newIdentity.Status() == V4FamilyStatus::resource_unavailable)
        { out.resourceUnavailable = true; out.reason = "v4_owned_template_schema_resource_unavailable"; return out; }
        if (originalIdentity.Status() != V4FamilyStatus::schema_only || newIdentity.Status() != V4FamilyStatus::schema_only ||
            originalIdentity.Axes().size() != newIdentity.Axes().size() ||
            source.candidate->values.size() != originalIdentity.Axes().size())
        { out.reason = "v4_owned_template_schema_unavailable"; return out; }
        std::vector<std::pair<std::string, std::string>> choices;
        std::size_t pathAxes = 0;
        for (std::size_t i = 0; i < originalIdentity.Axes().size(); ++i)
        {
            const auto& a = originalIdentity.Axes()[i]; const auto& b = newIdentity.Axes()[i];
            const bool path = a.id == "V4.file_path.index_0.utf16le";
            if (path) ++pathAxes;
            if (a.id != b.id || a.role != b.role || a.label != b.label || (!path && a.values != b.values))
            { out.reason = "v4_owned_template_axis_mismatch"; return out; }
            if (a.role != Role::Derived && !path) choices.emplace_back(a.id, source.candidate->values[i]);
        }
        if (pathAxes != 1) { out.reason = "v4_owned_template_path_axis_unavailable"; return out; }
        const auto candidate = PrepareV4OwnedFileCandidate(seed, axisBudget, choices);
        if (candidate.resourceUnavailable)
        { out.resourceUnavailable = true; out.reason = "v4_owned_template_candidate_resource_unavailable"; return out; }
        if (!candidate.ready || !seed.StillOwned())
        { out.reason = "v4_owned_template_candidate_unavailable"; return out; }
        const auto verified = V4FamilyValidatedContext::Create(seed.Family(), axisBudget, candidate.candidate);
        if (verified.Status() == V4CoordinateStatus::resource_unavailable)
        { out.resourceUnavailable = true; out.reason = "v4_owned_template_candidate_resource_unavailable"; return out; }
        if (verified.Status() != V4CoordinateStatus::coordinate_ready)
        { out.reason = "v4_owned_template_candidate_unavailable"; return out; }
        const auto prefix = EncodeV4StructuredPrefix(seed.Family(), tokenBudget);
        if (!prefix.encoded)
        { out.resourceUnavailable = prefix.resourceUnavailable; out.reason = "v4_owned_template_prefix_unavailable"; return out; }
        const auto before = event_graph::V4JoinedGraph::SerializeCursor(newIdentity, verified, out.cursor);
        if (!before.errors.empty() || before.encoded.empty())
        { out.reason = "v4_owned_template_cursor_unavailable"; return out; }
        std::string currentCursor = before.encoded;
        auto workingCursor = out.cursor;
        std::size_t used = 0;
        if (!workBudget) { out.reason = "v4_owned_template_graph_pending"; return out; }
        for (std::size_t pages = 0;; ++pages)
        {
            const auto remaining = workBudget - used;
            auto page = event_graph::V4JoinedGraph::Enumerate(newIdentity, verified, workingCursor, remaining, 1);
            if (page.resourceUnavailable)
            { out.resourceUnavailable = true; out.reason = "v4_owned_template_graph_resource_unavailable"; return out; }
            if (!page.errors.empty() || page.workUsed > remaining)
            { out.reason = "v4_owned_template_graph_unavailable"; return out; }
            used += page.workUsed;
            const auto after = event_graph::V4JoinedGraph::SerializeCursor(newIdentity, verified, page.cursor);
            if (!after.errors.empty() || after.encoded.empty())
            { out.reason = "v4_owned_template_cursor_unavailable"; return out; }
            const bool advanced = after.encoded != currentCursor;
            if (!page.graphs.empty())
            {
                const auto& graph = page.graphs.front();
                if (graph.present == source.present && graph.graph.classes == source.oleGraph->classes &&
                    graph.graph.order == source.oleGraph->order)
                {
                    const auto rebuilt = v3_scheduler::MakeV4StructuredCaseId(prefix.tokens, verified.Coordinate(),
                        graph.present, "ole", graph.graph.id, coordinateBudget, axisBudget, tokenBudget, caseByteBudget);
                    if (rebuilt.status == v3_scheduler::V4StructuredCaseStatus::resource_unavailable)
                    { out.resourceUnavailable = true; out.reason = "v4_owned_template_case_resource_unavailable"; return out; }
                    if (rebuilt.status != v3_scheduler::V4StructuredCaseStatus::unexecuted ||
                        rebuilt.candidateId != candidate.candidateId || !seed.StillOwned())
                    { out.reason = "v4_owned_template_case_unavailable"; return out; }
                    out.caseId = rebuilt.caseId; out.candidateId = rebuilt.candidateId; out.graphId = rebuilt.graphId;
                    out.present = graph.present; out.cursor = std::move(page.cursor);
                    out.workUsed = used; out.cursorAdvanced = after.encoded != before.encoded;
                    out.matched = true; out.reason = "v4_owned_template_unexecuted"; return out;
                }
            }
            if (!seed.StillOwned()) { out.reason = "v4_owned_template_root_changed"; return out; }
            if (!advanced && !page.exhausted)
            { out.reason = "v4_owned_template_cursor_not_advanced"; return out; }
            workingCursor = std::move(page.cursor);
            currentCursor = after.encoded;
            if (page.exhausted)
            { out.cursor = std::move(workingCursor); out.workUsed = used; out.cursorAdvanced = currentCursor != before.encoded;
              out.exhausted = true; out.reason = "v4_owned_template_graph_not_found"; return out; }
            if (!page.workUsed || used == workBudget || pages == workBudget) break;
        }
        out.cursor = std::move(workingCursor); out.workUsed = used; out.cursorAdvanced = currentCursor != before.encoded;
        out.reason = "v4_owned_template_graph_pending"; return out;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.reason = "v4_owned_template_resource_unavailable"; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; out.reason = "v4_owned_template_resource_unavailable"; return out; }
    catch (...) { out.reason = "v4_owned_template_observation_unavailable"; return out; }
}

// Input preparation only. The caller creates and retains an isolated source
// root first, constructs the candidate with its exact absolute path, and then
// materializes that candidate into payload. This function never rewrites the
// candidate or performs an OLE call. A pinned handoff that Shell cannot open
// remains unavailable; it is not a structural exclusion.
struct V4PinnedSingleFileSourcePreparation
{
    bool prepared = false, resourceUnavailable = false, ownerLeaseBound = false;
    std::string candidateId;
    std::optional<V4FileCopyHandoff> handoff;
    std::optional<V4IndependentShellSource> binding;
    std::shared_ptr<V4IndependentShellOwner> owner;
    std::vector<std::string> unresolved;
};
inline V4PinnedSingleFileSourcePreparation PrepareV4PinnedSingleFileSource(
    const event_graph::V4FamilyIdentityContext& identity, const V4FamilyValidatedContext& verified,
    behavior::OwnedProfileDesktopRoot& ownedProfile, const std::shared_ptr<V4IndependentFilePayload>& payload)
{
    V4PinnedSingleFileSourcePreparation result;
    try
    {
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        if (raw.resourceUnavailable) result.resourceUnavailable = true;
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty() || !payload ||
            verified.Status() != V4CoordinateStatus::coordinate_ready || identity.Status() != V4FamilyStatus::schema_only ||
            verified.FamilyBinding() != identity.FamilyBinding() || verified.Axes().size() != identity.Axes().size() ||
            !ownedProfile.StillOwned() || !ownedProfile.profile || !ownedProfile.profile->app ||
            behavior::Access::asyncOwner != ownedProfile.profile->app ||
            payload->status != V4IndependentFileStatus::input_prepared || payload->files.size() != 1 ||
            payload->candidateId != raw.id ||
            behavior::OwnedProfileDesktopRoot::SameId(payload->rootId, ownedProfile.desktopId))
        { result.unresolved.emplace_back("v4_pinned_single_copy_context_or_isolated_roots_unavailable"); return result; }
        struct ParentGuard final { HANDLE handle = INVALID_HANDLE_VALUE; ~ParentGuard() {
            if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); } } parent;
        parent.handle = CreateFileW(payload->root.parent_path().c_str(), FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        FILE_ID_INFO parentId{}; FILE_ATTRIBUTE_TAG_INFO parentTag{};
        if (parent.handle == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandleEx(parent.handle, FileIdInfo, &parentId, sizeof(parentId)) ||
            !GetFileInformationByHandleEx(parent.handle, FileAttributeTagInfo, &parentTag, sizeof(parentTag)) ||
            !(parentTag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (parentTag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            !behavior::OwnedProfileDesktopRoot::SameId(parentId, ownedProfile.rootId) || !ownedProfile.StillOwned())
        { result.unresolved.emplace_back("v4_pinned_single_copy_source_parent_unavailable"); return result; }
        for (std::size_t i = 0; i < identity.Axes().size(); ++i)
        {
            const auto& a = verified.Axes()[i]; const auto& b = identity.Axes()[i];
            if (a.id != b.id || a.role != b.role || a.values != b.values || a.label != b.label)
            { result.unresolved.emplace_back("v4_pinned_single_copy_schema_mismatch"); return result; }
        }
        const auto value = [&](std::string_view id) { return Value(verified.Axes(), verified.Coordinate(), id); };
        const bool supportedTarget = value("target.E01") == "free_desktop" &&
            (value("target.E02") == "none" ||
             (value("target.E02") == "collection" && value("target.E03") == "widget_body" &&
              value("H01.region") == "widget_body" && value("G02.enter") == "ctrl") ||
             (value("target.E02") == "desktop_files" && value("target.E03") == "widget_body" &&
              value("H01.region") == "widget_body" && value("G02.enter") == "ctrl") ||
             (value("target.E02") == "folder_mapping" && value("target.E03") == "widget_body" &&
              value("H01.region") == "widget_body" && value("G02.enter") == "ctrl") ||
             (value("target.E02") == "collection_group" && value("target.E03") == "widget_body" &&
              value("target.E08") == "first_child" && value("target.E08.active_child_type") == "collection" &&
              value("target.E08.hit_relation") == "active" && value("target.E08.hit_position") == "first_child" &&
              value("H01.region") == "widget_body" && value("G02.enter") == "ctrl") ||
             (value("target.E02") == "collection_group" && value("target.E03") == "group_label" &&
              value("target.E08") == "first_child" && value("target.E08.active_child_type") == "collection" &&
              value("target.E08.hit_relation") == "inactive_valid" && value("target.E08.hit_position") == "last_child" &&
              value("H01.region") == "tab" && value("G02.enter") == "ctrl") ||
             (value("target.E02") == "file_group" && value("target.E03") == "widget_body" &&
              value("target.E08") == "first_child" &&
              (value("target.E08.active_child_type") == "desktop_files" || value("target.E08.active_child_type") == "folder_mapping") &&
              value("target.E08.hit_relation") == "active" && value("target.E08.hit_position") == "first_child" &&
              value("H01.region") == "widget_body" && value("G02.enter") == "ctrl"));
        const bool copyCandidate = supportedTarget && value("G04.termination") == "drop" &&
            value("G02.release") == "ctrl" && value("T03") == "copy_move";
        const bool copyOnlyCandidate = value("target.E01") == "free_desktop" && value("target.E02") == "none" &&
            value("H01.region") == "empty_slot" && value("G04.termination") == "drop" &&
            value("G02.release") == "none" && value("G02.release_buttons") == "none" &&
            value("G02.enter_buttons") == "left" && value("G02.enter") == "none" &&
            value("G02.hover_first") == "na" && value("G02.hover_first_buttons") == "na" &&
            value("G02.hover_later") == "na" && value("G02.hover_later_buttons") == "na" &&
            value("T03") == "copy" && value("T03.extra_protocol_bits") == "none";
        const bool rejectCandidate = value("target.E01") == "free_desktop" && value("target.E02") == "none" &&
            value("H01.region") == "empty_slot" && value("G04.termination") == "drop" &&
            value("G02.release") == "none" && value("G02.release_buttons") == "none" &&
            value("G02.enter_buttons") == "left" && value("G02.enter") == "none" &&
            value("G02.hover_first") == "na" && value("G02.hover_first_buttons") == "na" &&
            value("G02.hover_later") == "na" && value("G02.hover_later_buttons") == "na" &&
            value("T03") == "none" && value("T03.extra_protocol_bits") == "none";
        const bool moveCandidate = value("target.E01") == "free_desktop" && value("target.E02") == "none" &&
            value("H01.region") == "empty_slot" && value("G04.termination") == "drop" &&
            value("G02.release") == "none" && value("G02.release_buttons") == "none" &&
            value("G02.enter_buttons") == "left" && value("G02.enter") == "none" &&
            value("G02.hover_first") == "na" && value("G02.hover_first_buttons") == "na" &&
            value("G02.hover_later") == "na" && value("G02.hover_later_buttons") == "na" &&
            value("T03") == "copy_move" && value("T03.extra_protocol_bits") == "none";
        const bool leaveCandidate = value("target.E01") == "free_desktop" &&
            ((value("target.E02") == "none" && value("H01.region") == "empty_slot" && value("G02.enter") == "none") ||
             (value("target.E02") == "collection" && value("target.E03") == "widget_body" &&
              value("H01.region") == "widget_body" && value("G02.enter") == "ctrl")) && value("G04.termination") == "leave" &&
            value("G02.release") == "na" && value("G02.release_buttons") == "na" &&
            value("G02.enter_buttons") == "left" &&
            value("G02.hover_first") == "na" && value("G02.hover_first_buttons") == "na" &&
            value("G02.hover_later") == "na" && value("G02.hover_later_buttons") == "na" &&
            value("T03") == "copy_move_link" && value("T03.extra_protocol_bits") == "none";
        if (value("V4.source.kind") != "file_only" || value("V4.source.file_count") != "one" ||
            value("V4.file.member0_kind") != "file" || value("V4.file.member0_identity") != "unique" ||
            value("T01") != "external_ingress" || value("T02.file_paths") != "yes" ||
            value("T04.capability") != "enabled" || (!copyCandidate && !copyOnlyCandidate && !rejectCandidate && !moveCandidate && !leaveCandidate))
        { result.unresolved.emplace_back("v4_pinned_single_copy_candidate_unimplemented"); return result; }
        result.candidateId = raw.id;
        // Create the fresh Profile-owned owner before mutating the payload.
        // The factory rejects an already occupied or invalid host.
        result.owner = behavior::Access::CreateV4IndependentShellOwner(*ownedProfile.profile->app);
        if (!result.owner || behavior::Access::v4IndependentShellOwner != result.owner)
        { result.unresolved.emplace_back("v4_pinned_single_copy_source_owner_unavailable"); return result; }
        result.handoff = PrepareV4FileCopyHandoff(verified, identity, *payload,
            leaveCandidate ? V4FileCopyHandoffMode::single_file_pinned_leave :
            rejectCandidate ? V4FileCopyHandoffMode::single_file_pinned_reject :
            moveCandidate ? V4FileCopyHandoffMode::single_file_shared_move :
            copyOnlyCandidate ? V4FileCopyHandoffMode::single_file_pinned_copy_only : V4FileCopyHandoffMode::single_file_pinned_copy);
        result.resourceUnavailable = result.handoff->resourceUnavailable;
        const auto leaseBeforeMatches = [&] {
            return moveCandidate ? (result.handoff->sharedMoveLease && result.handoff->sharedMoveLease->RevalidateBefore(verified, identity, *payload)) :
                (result.handoff->pinnedLease && result.handoff->pinnedLease->Revalidate(verified, identity, *payload)); };
        if (!result.handoff->prepared || result.handoff->candidateId != raw.id || !leaseBeforeMatches())
        { result.unresolved.emplace_back("v4_pinned_single_copy_handoff_unavailable"); return result; }
        result.binding = PrepareV4IndependentShellSource(verified, identity, payload, result.owner);
        result.resourceUnavailable = result.binding->resourceUnavailable;
        if (!result.binding->inputPrepared || result.binding->candidateId != raw.id ||
            !result.binding->SameOwner(result.owner) || !result.binding->SamePayload(payload) ||
            !leaseBeforeMatches())
        { result.unresolved.emplace_back("v4_pinned_single_copy_shell_source_unavailable"); return result; }
        result.ownerLeaseBound = moveCandidate ?
            (result.owner->AttachSharedMoveLease(verified, identity, payload, result.handoff->sharedMoveLease) &&
             result.owner->MatchesSharedMoveLeaseBefore(verified, identity, payload, result.handoff->sharedMoveLease)) :
            (result.owner->AttachPinnedCopyLease(verified, identity, payload, result.handoff->pinnedLease) &&
             result.owner->MatchesPinnedCopyLease(verified, identity, payload, result.handoff->pinnedLease));
        if (!result.ownerLeaseBound)
        { result.unresolved.emplace_back("v4_pinned_single_copy_owner_lease_binding_unavailable"); return result; }
        // The restricted source is a real Shell-backed IDataObject with only
        // the explicitly selected CF_HDROP/HGLOBAL offer. Its one setup probe
        // remains in the source log and is not a production request.
        bool restrictedCandidate = copyOnlyCandidate && value("target.E02") == "none" &&
            value("H01.region") == "empty_slot" && value("T02.shell_object") == "no" &&
            value("T02.virtual_files") == "no" && value("T02.virtual_completeness") == "na" &&
            value("T02.image") == "no" && value("T02.text") == "no" && value("T02.url") == "no" &&
            value("T02.url_kind") == "na" && value("T02.invalid_format") == "no" &&
            value("X03.task_count") == "one";
        std::size_t formatAxes = 0;
        if (restrictedCandidate)
            for (const auto& axis : verified.Axes())
            {
                if (!axis.id.starts_with("V4.format.")) continue;
                ++formatAxes;
                std::string_view expected;
                if (axis.id.ends_with(".offer")) expected = axis.id == "V4.format.cf_hdrop.offer" ? "yes" : "no";
                else if (axis.id.find(".medium.") != std::string::npos)
                    expected = axis.id == "V4.format.cf_hdrop.medium.hglobal" ? "yes" : "no";
                else if (axis.id.ends_with(".unlisted_payloads")) expected = "reject_unlisted";
                else { restrictedCandidate = false; break; }
                if (value(axis.id) != expected) { restrictedCandidate = false; break; }
            }
        if (restrictedCandidate && formatAxes)
        {
            auto* source = result.binding->SourceObject();
            if (!source || !source->EnableRestrictedFileDrop() || !source->ProbeRestrictedFileDrop() ||
                !result.binding->SameOwner(result.owner) || !result.binding->SamePayload(payload) || !leaseBeforeMatches())
            { result.unresolved.emplace_back("v4_restricted_file_source_setup_or_probe_unavailable"); return result; }
        }
        result.prepared = true;
        result.unresolved.emplace_back("v4_pinned_single_copy_shell_read_compatibility_and_same_object_unverified");
        result.unresolved.emplace_back("v4_pinned_single_copy_full_axes_graph_completion_and_business_unverified");
    }
    catch (const std::bad_alloc&) { result.resourceUnavailable = true; }
    catch (const std::length_error&) { result.resourceUnavailable = true; }
    catch (...) { result.unresolved.emplace_back("v4_pinned_single_copy_preparation_unavailable"); }
    return result;
}

struct V4PinnedSingleFileDesktopInputs
{
    bool prepared = false, resourceUnavailable = false;
    std::string candidateId;
    GridCell landing;
    POINT client{}, screen{};
    std::vector<ole_driver::Input> inputs;
    std::vector<std::string> unresolved;
};
// A candidate-derived input proposal for one-file copy or Leave on the
// isolated desktop. The point is not evidence of a production hit or result.
inline V4PinnedSingleFileDesktopInputs PrepareV4PinnedSingleFileDesktopInputs(
    const event_graph::V4FamilyIdentityContext& identity, const V4FamilyValidatedContext& verified,
    behavior::OwnedProfileDesktopRoot& ownedProfile, POINT client)
{
    V4PinnedSingleFileDesktopInputs result;
    try
    {
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        if (raw.resourceUnavailable) result.resourceUnavailable = true;
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty() ||
            verified.Status() != V4CoordinateStatus::coordinate_ready || identity.Status() != V4FamilyStatus::schema_only ||
            verified.FamilyBinding() != identity.FamilyBinding() || verified.Axes().size() != identity.Axes().size() ||
            !ownedProfile.StillOwned() ||
            !ownedProfile.profile || !ownedProfile.profile->app || verified.TaskBActive())
        { result.unresolved.emplace_back("v4_pinned_single_input_context_unavailable"); return result; }
        for (std::size_t i = 0; i < identity.Axes().size(); ++i)
        {
            const auto& a = verified.Axes()[i]; const auto& b = identity.Axes()[i];
            if (a.id != b.id || a.role != b.role || a.values != b.values || a.label != b.label)
            { result.unresolved.emplace_back("v4_pinned_single_input_schema_mismatch"); return result; }
        }
        const auto& axes = verified.Axes(); const auto& candidate = verified.Coordinate();
        const auto value = [&](std::string_view id) { return Value(axes, candidate, id); };
        const bool isLeave = value("G04.termination") == "leave" && value("G02.release") == "na" &&
            value("G02.release_buttons") == "na" && value("T03") == "copy_move_link" &&
            value("T03.extra_protocol_bits") == "none" && value("G02.enter_buttons") == "left" &&
            value("G02.enter") == "none" && value("G02.hover_first") == "na" &&
            value("G02.hover_first_buttons") == "na" && value("G02.hover_later") == "na" && value("G02.hover_later_buttons") == "na";
        const bool isCopy = value("G04.termination") == "drop" && value("G02.release") == "ctrl" && value("T03") == "copy_move";
        const bool isCopyOnly = value("G04.termination") == "drop" && value("G02.release") == "none" &&
            value("G02.release_buttons") == "none" && value("G02.enter_buttons") == "left" && value("G02.enter") == "none" &&
            value("G02.hover_first") == "na" && value("G02.hover_first_buttons") == "na" &&
            value("G02.hover_later") == "na" && value("G02.hover_later_buttons") == "na" &&
            value("T03") == "copy" && value("T03.extra_protocol_bits") == "none";
        const bool isReject = value("G04.termination") == "drop" && value("G02.release") == "none" &&
            value("G02.release_buttons") == "none" && value("G02.enter_buttons") == "left" && value("G02.enter") == "none" &&
            value("G02.hover_first") == "na" && value("G02.hover_first_buttons") == "na" &&
            value("G02.hover_later") == "na" && value("G02.hover_later_buttons") == "na" &&
            value("T03") == "none" && value("T03.extra_protocol_bits") == "none";
        const bool isMoveCapable = value("G04.termination") == "drop" && value("G02.release") == "none" &&
            value("G02.release_buttons") == "none" && value("G02.enter_buttons") == "left" && value("G02.enter") == "none" &&
            value("G02.hover_first") == "na" && value("G02.hover_first_buttons") == "na" &&
            value("G02.hover_later") == "na" && value("G02.hover_later_buttons") == "na" &&
            value("T03") == "copy_move" && value("T03.extra_protocol_bits") == "none";
        if (value("V4.source.kind") != "file_only" || value("V4.source.file_count") != "one" ||
            value("T01") != "external_ingress" || value("T02.file_paths") != "yes" ||
            value("T04.capability") != "enabled" || (!isCopy && !isCopyOnly && !isReject && !isMoveCapable && !isLeave) ||
            value("target.E01") != "free_desktop" || value("target.E02") != "none" ||
            value("target.E19") != "named" || value("H01.region") != "empty_slot" ||
            value("H06.page_record") != "saved" || value("H06.page_dimensions") != "valid" ||
            value("H07.occupancy") != "partial")
        { result.unresolved.emplace_back("v4_pinned_single_input_candidate_unimplemented"); return result; }
        auto& profile = *ownedProfile.profile;
        if (behavior::Access::asyncOwner != profile.app || behavior::Access::productionDispatch != 1 ||
            !behavior::Access::completionWindow || !IsWindow(behavior::Access::completionWindow) ||
            GetWindowThreadProcessId(behavior::Access::completionWindow, nullptr) != GetCurrentThreadId())
        { result.unresolved.emplace_back("v4_pinned_single_input_host_unavailable"); return result; }
        const auto initial = behavior::Access::InspectTextDesktopInitial(*profile.app, profile.desktop / L"Sentinel.txt", client);
        if (initial.pageName.empty() || !initial.pageRecorded || !initial.savedColumns || !initial.savedRows ||
            *initial.savedColumns != initial.columns || *initial.savedRows != initial.rows ||
            EndpointSize(value("target.E23")) != initial.columns || EndpointSize(value("target.E24")) != initial.rows ||
            (value("C07") != "on" && value("C07") != "off") ||
            behavior::Access::ProgramCollectionEnabled(*profile.app) != (value("C07") == "on"))
        { result.unresolved.emplace_back("v4_pinned_single_input_host_candidate_mismatch"); return result; }
        POINT screen = client, roundTrip{};
        if (!ClientToScreen(behavior::Access::completionWindow, &screen))
        { result.unresolved.emplace_back("v4_pinned_single_input_screen_unavailable"); return result; }
        roundTrip = screen;
        if (!ScreenToClient(behavior::Access::completionWindow, &roundTrip) || roundTrip.x != client.x || roundTrip.y != client.y)
        { result.unresolved.emplace_back("v4_pinned_single_input_coordinate_roundtrip_unavailable"); return result; }
        const auto program = verified.TransferProgram();
        const auto allowed = ole_driver::EncodeAllowedEffect(value("T03"), value("T03.extra_protocol_bits"));
        if (!allowed) { result.unresolved.emplace_back("v4_pinned_single_input_effect_unavailable"); return result; }
        bool entered = false, terminated = false, completionStage = false, businessStage = false;
        for (std::size_t i = 0; i < program.steps.size(); ++i)
        {
            const auto& step = program.steps[i]; const auto kind = ole_driver::ProgramKind(step.kind);
            if (!kind)
            {
                if (step.kind == "configure_v4_source_input" && !entered) continue;
                if (step.kind == "observe_completion_or_rejection" && terminated && !completionStage && !businessStage)
                { completionStage = true; continue; }
                if (step.kind == "assert_business_results" && completionStage && !businessStage)
                { businessStage = true; continue; }
                result.unresolved.emplace_back("v4_pinned_single_input_program_stage_unimplemented"); return result;
            }
            const auto expectedTerminal = isLeave ? ole_driver::Kind::leave : ole_driver::Kind::drop;
            if ((*kind != ole_driver::Kind::enter && *kind != expectedTerminal) ||
                (*kind == ole_driver::Kind::enter && entered) || (*kind == expectedTerminal && (!entered || terminated)))
            { result.unresolved.emplace_back("v4_pinned_single_input_program_motion_unimplemented"); return result; }
            if (*kind == ole_driver::Kind::leave)
            {
                ole_driver::Input input; input.programIndex = i; input.kind = *kind; input.modelParameter = step.parameter;
                result.inputs.push_back(std::move(input)); terminated = true; continue;
            }
            const auto [buttonAxis, modifierAxis] = ole_driver::PhaseAxes(step.kind);
            const auto buttons = value(buttonAxis), modifiers = value(modifierAxis);
            const auto parsed = ole_driver::ParseKeyParameter(step.parameter);
            const auto keys = ole_driver::EncodeKeyState(buttons, modifiers);
            if (!parsed || parsed->first != buttons || parsed->second != modifiers || !keys.valid || keys.altX1Ambiguous)
            { result.unresolved.emplace_back("v4_pinned_single_input_key_unavailable"); return result; }
            ole_driver::Input input; input.programIndex = i; input.kind = *kind; input.modelParameter = step.parameter;
            input.keyState = keys.value; input.allowedEffect = *allowed; input.screenPoint = POINTL{screen.x, screen.y};
            result.inputs.push_back(std::move(input));
            if (*kind == ole_driver::Kind::enter) entered = true; else terminated = true;
        }
        if (!entered || !terminated || !completionStage || !businessStage || result.inputs.size() != 2)
        { result.inputs.clear(); result.unresolved.emplace_back("v4_pinned_single_input_program_incomplete"); return result; }
        result.candidateId = raw.id; result.landing = initial.targetCell; result.client = client; result.screen = screen;
        result.unresolved.insert(result.unresolved.end(), program.unresolved.begin(), program.unresolved.end());
        result.unresolved.emplace_back("v4_pinned_single_input_actual_target_hit_and_remaining_axes_unverified");
        result.unresolved.emplace_back("v4_pinned_single_input_snapshot_must_be_revalidated_before_dispatch");
        result.prepared = true;
    }
    catch (const std::bad_alloc&) { result.resourceUnavailable = true; }
    catch (const std::length_error&) { result.resourceUnavailable = true; }
    catch (...) { result.unresolved.emplace_back("v4_pinned_single_input_observation_unavailable"); }
    return result;
}

// Narrow V3 bridge: prepare one model-generated file and the same real host,
// source and observer, then perform version-specific graph/input preflight.
// Unsupported candidates remain unimplemented. No hypothesis configures an
// output, and the real adapter is called only after every precondition closes.
inline BridgeResult ExecuteV3DesktopSingleCopy(const event_graph::V3FamilyIdentityContext& identity,
    const V3FamilyValidatedContext& verified, const std::vector<bool>& present,
    const event_graph::V3JoinedGraphVariant& selected)
{
    BridgeResult result;
    bool productionStarted = false;
    std::shared_ptr<behavior::GeneratedPayloadProfile> generated;
    Microsoft::WRL::ComPtr<ole_file_source_fixture::Source> source;
    Microsoft::WRL::ComPtr<OleDragDropAdapter> adapter;
    std::shared_ptr<ole_driver::V3HostEpoch> hostEpoch;
    std::unique_ptr<behavior::GeneratedCopyObservation> observed;
    const auto appendUnique = [](std::vector<std::string>& destination, const std::vector<std::string>& values) {
        for (const auto& value : values)
            if (std::find(destination.begin(), destination.end(), value) == destination.end()) destination.push_back(value);
    };
    const auto releaseFixture = [&] {
        observed.reset(); hostEpoch.reset(); adapter.Reset(); source.Reset(); generated.reset();
        try { behavior::Access::ThrowAsyncException(); }
        catch (const std::exception& error)
        {
            result.observationUnavailable = true;
            if (!result.failure.empty()) result.failure += "; shutdown: ";
            result.failure += error.what();
            result.obligations.emplace_back("v3_generated_fixture_shutdown_unavailable");
        }
        catch (...)
        {
            result.observationUnavailable = true;
            if (!result.failure.empty()) result.failure += "; shutdown: ";
            result.failure += "unknown_v3_generated_fixture_shutdown_exception";
            result.obligations.emplace_back("v3_generated_fixture_shutdown_unavailable");
        }
    };
    try
    {
        if (verified.Status() != V3CoordinateStatus::coordinate_ready || identity.Status() != V3FamilyStatus::schema_only)
        { result.obligations.emplace_back("v3_copy_family_or_candidate_unavailable"); return result; }
        const auto program = verified.TransferProgram();
        const auto replay = event_graph::V3JoinedGraph::Replay(identity, verified, program, present, selected.id);
        if (!replay.identityValid || !replay.errors.empty() || selected.id != replay.variant.id ||
            selected.classes != replay.variant.classes || selected.order != replay.variant.order ||
            selected.unresolved != replay.variant.unresolved)
        {
            result.obligations.emplace_back("v3_copy_graph_identity_unavailable");
            result.obligations.insert(result.obligations.end(), replay.errors.begin(), replay.errors.end());
            return result;
        }
        result.graphUnresolved = replay.variant.unresolved;
        const auto& axes = verified.Axes(); const auto& candidate = verified.Coordinate();
        const auto v = [&](std::string_view id) { return Value(axes, candidate, id); };
        // Only supported fixture inputs appear here. Resolved-count and other
        // postconditions are intentionally absent from construction.
        if (verified.TaskBActive() || v("P02.source_actual_count") != "one" || v("P01.primary") != "file" ||
            v("P01.second") != "na" || v("T01") != "external_ingress" || v("T02.file_paths") != "yes" ||
            v("T02.shell_object") != "no" || v("T02.virtual_files") != "no" || v("T02.image") != "no" ||
            v("T02.text") != "no" || v("T02.url") != "no" || v("T02.invalid_format") != "no" ||
            v("source.E01") != "external_unknown" || v("target.E01") != "free_desktop" || v("target.E02") != "none" ||
            v("target.E23") != "four" || v("target.E24") != "three" || v("target.E19") != "named" ||
            v("G01.button") != "unknown" || v("G02.press") != "unknown" || v("G02.enter_buttons") != "left" ||
            v("G02.enter") != "none" || v("G02.hover_first") != "na" || v("G02.hover_first_buttons") != "na" ||
            v("G02.hover_later") != "na" || v("G02.hover_later_buttons") != "na" ||
            v("G02.release_buttons") != "none" || v("G02.release") != "ctrl" || v("G04.termination") != "drop" ||
            v("T03") != "copy_move" || v("T03.extra_protocol_bits") != "none" || v("T04.capability") != "enabled" ||
            v("C07") != "on" || v("X02") != "none" || v("X02.position") != "no_interruption" ||
            v("H01.region") != "empty_slot" || v("R04.member0_target_exists") != "absent" ||
            v("R04.member0_queued_reservation") != "absent" || v("R04.member0_concurrent_creation") != "none" ||
            v("X03.task_count") != "one" || v("X03.submit_order") != "na" || v("X03.order") != "single")
        { result.obligations.emplace_back("v3_single_copy_fixture_plan_unimplemented"); return result; }

        generated = std::make_shared<behavior::GeneratedPayloadProfile>(verified);
        if (generated->version || !generated->profile || !generated->profile->app || !generated->after.captured ||
            !generated->payload.payload_prepared || generated->payload.members.size() != 1 ||
            generated->payload.members.front().kind != "file")
        { result.obligations.emplace_back("v3_generated_fixture_unavailable"); releaseFixture(); return result; }
        for (const auto& reason : generated->payload.unresolved)
            result.unclassifiedObligations.push_back("v3_generated_payload_unresolved:" + reason);
        if (!generated->payload.unresolved.empty())
            result.obligations.emplace_back("v3_generated_payload_obligation_stage_unclassified");
        auto& profile = *generated->profile;
        std::error_code ec;
        if (!std::filesystem::equivalent(profile.desktop, generated->payload.desktopDirectory, ec) || ec)
        { result.obligations.emplace_back("v3_generated_desktop_binding_unavailable"); releaseFixture(); return result; }
        ec.clear();
        if (!std::filesystem::equivalent(generated->payload.desktopDirectory, generated->payload.targetDirectory, ec) || ec)
        { result.obligations.emplace_back("v3_generated_target_binding_unavailable"); releaseFixture(); return result; }
        behavior::Access::EnableProductionDispatch(*profile.app, false);
        behavior::Access::EnableFixtureControlWindow(*profile.app);
        behavior::Wait([&] { return behavior::Access::FileDropIdle(*profile.app) && behavior::Access::LayoutSaveIdle(*profile.app); });
        source = ole_file_source_fixture::Make({generated->payload.members.front().path}, true);
        if (!source)
        { result.obligations.emplace_back("v3_generated_source_unavailable"); releaseFixture(); return result; }
        auto binding = behavior::Access::CreateV3AdapterAndHostEpoch(*profile.app, source.Get());
        adapter = std::move(binding.first); hostEpoch = std::move(binding.second);
        if (!adapter || !hostEpoch)
        { result.obligations.emplace_back("v3_generated_adapter_or_host_epoch_unavailable"); releaseFixture(); return result; }
        observed = std::make_unique<behavior::GeneratedCopyObservation>(*generated, *source.Get());
        observed->PrepareV3SourceEvidence(verified);
        if (!observed->sourceInitial)
        { result.obligations.emplace_back("v3_source_initial_observation_unavailable"); releaseFixture(); return result; }
        const auto initial = ole_driver::ValidateV3SingleFileSourceInitial(verified, generated->payload,
            generated->after, *source.Get(), *observed->sourceInitial);
        result.obligations.insert(result.obligations.end(), initial.obligations.begin(), initial.obligations.end());
        if (!initial.matches)
        { result.obligations.emplace_back("v3_source_initial_binding_unavailable"); releaseFixture(); return result; }
        std::vector<ole_driver::Input> inputs;
        const auto point = behavior::Access::ExternalPoint({150, 150});
        for (std::size_t i = 0; i < program.steps.size(); ++i)
        {
            const auto& step = program.steps[i]; const auto kind = ole_driver::ProgramKind(step.kind);
            if (!kind) continue;
            if (*kind != ole_driver::Kind::enter && *kind != ole_driver::Kind::drop)
            { result.obligations.push_back("v3_copy_ole_step_unimplemented:" + step.kind); releaseFixture(); return result; }
            ole_driver::Input event;
            event.programIndex = i; event.kind = *kind; event.modelParameter = step.parameter;
            event.keyState = *kind == ole_driver::Kind::enter ? MK_LBUTTON : MK_CONTROL;
            event.allowedEffect = DROPEFFECT_COPY | DROPEFFECT_MOVE; event.screenPoint = point;
            inputs.push_back(std::move(event));
        }
        result.v3OleInput = ole_driver::InspectV3OleInputs(verified, identity, inputs);
        result.obligations.insert(result.obligations.end(), result.v3OleInput->preDispatchBlockers.begin(), result.v3OleInput->preDispatchBlockers.end());
        result.postDispatchObligations = result.v3OleInput->postDispatchObligations;
        result.outcomeExpectations = result.v3OleInput->postconditions;
        appendUnique(result.unclassifiedObligations, result.v3OleInput->unclassifiedObligations);
        if (!result.v3OleInput->unclassifiedObligations.empty())
            result.obligations.emplace_back("v3_ole_obligation_stage_unclassified");
        const auto plan = ole_driver::BuildV3JoinedOleExecutionPlan(verified, identity, present, selected, inputs);
        result.obligations.insert(result.obligations.end(), plan.preconditions.begin(), plan.preconditions.end());
        appendUnique(result.graphUnresolved, plan.graphUnresolved);
        appendUnique(result.postDispatchObligations, plan.postDispatchObligations);
        appendUnique(result.unclassifiedObligations, plan.unclassifiedObligations);
        if (!plan.postconditions.empty()) result.outcomeExpectations = plan.postconditions;
        result.presenceExpectations = plan.presence;
        if (!plan.unclassifiedObligations.empty())
            result.obligations.emplace_back("v3_joined_obligation_stage_unclassified");
        if (!plan.graphIdentityValid || !plan.structurallyPlanned)
            result.obligations.emplace_back("v3_copy_graph_plan_unavailable");
        // The real coordinate/session binding, source format delivery, task
        // epoch, postconditions and independent oracle are not closed yet.
        result.obligations.emplace_back("v3_copy_adapter_and_same_run_oracle_capability_unimplemented");
        if (!result.obligations.empty()) { releaseFixture(); return result; }

        bool completionReached = false;
        const ole_driver::HostBoundaries host{
            [&](const ole_driver::Input& event) { observed->BeforeOleCall(event); },
            [&](const ole_driver::Observation& event) { observed->ObserveOleReturn(event); },
            [&](const ole_driver::Observation& terminal) {
                observed->Await(terminal);
                completionReached = true;
                try { result.v3PassiveEvidence = observed->FinishPassiveObservation(); }
                catch (const ole_driver::ObservationUnavailable&) { throw; }
                catch (const std::exception& error)
                { throw ole_driver::ObservationUnavailable(std::string("v3_passive_transfer_observation_unavailable:") + error.what()); }
                catch (...) { throw ole_driver::ObservationUnavailable("v3_passive_transfer_observation_unavailable:unknown_exception"); }
            },
            [&] { observed->AssertResults(); }};
        const auto expectedOle = [&](const ole_driver::Observation& event) {
            behavior::Require(SUCCEEDED(event.result) && event.returnedEffect &&
                (event.kind != ole_driver::Kind::drop || *event.returnedEffect == DROPEFFECT_COPY),
                "v3 generated single-file copy has the independently expected OLE result");
        };
        const auto hostSnapshot = [&](const ole_driver::Observation& event) {
            ole_driver::V2HostCoordinateSnapshot snapshot;
            const auto window = behavior::Access::HostWindow(*profile.app);
            if (event.kind != ole_driver::Kind::enter || !window || window != behavior::Access::completionWindow ||
                behavior::Access::asyncOwner != profile.app || behavior::Access::productionDispatch != 1 ||
                GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId() ||
                !behavior::Access::SessionActive(*profile.app))
            { snapshot.unresolved.emplace_back("v3_host_snapshot_live_session_unavailable"); return snapshot; }
            snapshot.window = window;
            snapshot.sessionPoint = behavior::Access::SessionPoint(*profile.app);
            snapshot.target = reinterpret_cast<std::uintptr_t>(behavior::Access::Target(*profile.app));
            snapshot.region = static_cast<int>(behavior::Access::Region(*profile.app));
            snapshot.thread = GetCurrentThreadId();
            snapshot.unresolved.emplace_back("v3_host_snapshot_callback_provenance_and_bypass_reentrancy_unresolved");
            return snapshot;
        };
        observed->StartPassiveObservation();
        {
            const auto receipt = ole_driver::V3GraphExecutor::RunBound(verified, identity, present, selected, inputs,
                generated->payload, generated->after, *observed->sourceInitial, *source.Get(), adapter.Get(), host, expectedOle, hostSnapshot, hostEpoch);
            const auto& execution = receipt.Execution();
            productionStarted = execution.productionStarted; result.productionStarted = productionStarted;
            result.businessFailureObserved = execution.businessFailureObserved;
            result.eventOracleFailureObserved = execution.eventOracleFailureObserved;
            result.observationUnavailable = execution.additionalObservationUnavailable ||
                execution.status == ole_driver::Status::observation_unavailable ||
                execution.status == ole_driver::Status::host_observation_failed;
            if (productionStarted && (execution.status == ole_driver::Status::event_failed || execution.eventOracleFailureObserved ||
                execution.businessFailureObserved || execution.status == ole_driver::Status::business_failed))
                result.status = BridgeStatus::production_failed;
            result.failure = execution.failure;
            appendUnique(result.graphUnresolved, receipt.GraphUnresolved());
            appendUnique(result.postDispatchObligations, receipt.PostDispatchObligations());
            appendUnique(result.unclassifiedObligations, receipt.UnclassifiedObligations());
            result.obligations.insert(result.obligations.end(), receipt.Obligations().begin(), receipt.Obligations().end());
            result.obligations.insert(result.obligations.end(), execution.obligations.begin(), execution.obligations.end());
            if (productionStarted && completionReached)
            {
                try
                {
                    result.v3SourceEvidence = ole_driver::InspectV3SourceAndPassive(verified,
                        generated->payload, *observed->sourceInitial, *source.Get());
                    if (!result.v3SourceEvidence->obligations.empty()) result.observationUnavailable = true;
                    for (const auto& reason : result.v3SourceEvidence->obligations)
                        result.obligations.push_back("v3_copy_source_evidence:" + reason);
                }
                catch (const std::exception& error)
                { result.observationUnavailable = true; result.obligations.push_back(std::string("v3_copy_source_observation_unavailable:") + error.what()); }
                catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v3_copy_source_observation_unavailable:unknown_exception"); }
                try
                {
                    result.v3LocalResolvedCount = ole_driver::V3LocalResolvedCountObservation::Inspect(verified,
                        generated->payload, *observed->sourceInitial, *source.Get());
                    if (!result.v3LocalResolvedCount->Unresolved().empty() ||
                        result.v3LocalResolvedCount->Comparison() == ole_driver::V3LocalCountComparison::unavailable)
                        result.observationUnavailable = true;
                    for (const auto& reason : result.v3LocalResolvedCount->Unresolved())
                        result.obligations.push_back("v3_copy_local_count:" + reason);
                }
                catch (const std::exception& error)
                { result.observationUnavailable = true; result.obligations.push_back(std::string("v3_copy_local_count_unavailable:") + error.what()); }
                catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v3_copy_local_count_unavailable:unknown_exception"); }
                try
                {
                    if (behavior::Access::messageTrace)
                    {
                        result.v3DropTaskObservation = ole_driver::V3DropTaskWindowObservation::Inspect(receipt, verified,
                            generated->payload, *observed->sourceInitial, *source.Get(), *behavior::Access::messageTrace,
                            observed->traceStart, behavior::Access::fixtureControlWindow,
                            ole_driver::MessageRoute::control_window_proc, kShellFileOperationCompletedMessage);
                        if (!result.v3DropTaskObservation->Unresolved().empty()) result.observationUnavailable = true;
                        for (const auto& reason : result.v3DropTaskObservation->Unresolved())
                            result.obligations.push_back("v3_copy_drop_task:" + reason);
                        if (result.v3DropTaskObservation->ResourceUnavailable())
                        {
                            result.observationUnavailable = true;
                            result.obligations.emplace_back("v3_copy_drop_task_resource_unavailable");
                        }
                    }
                    else { result.observationUnavailable = true; result.obligations.emplace_back("v3_copy_drop_task_trace_unavailable"); }
                }
                catch (const std::exception& error)
                { result.observationUnavailable = true; result.obligations.push_back(std::string("v3_copy_drop_task_unavailable:") + error.what()); }
                catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v3_copy_drop_task_unavailable:unknown_exception"); }
                try
                {
                    if (behavior::Access::messageTrace)
                    {
                        result.v3LocalPresence = ole_driver::V3LocalPresenceObservation::Inspect(receipt, verified,
                            *observed->sourceInitial, *source.Get(), *behavior::Access::messageTrace,
                            behavior::Access::fixtureControlWindow);
                        if (!result.v3LocalPresence->Unresolved().empty()) result.observationUnavailable = true;
                        for (const auto& reason : result.v3LocalPresence->Unresolved())
                            result.obligations.push_back("v3_copy_local_presence:" + reason);
                        if (result.v3LocalPresence->ResourceOrObservationUnavailable())
                        { result.observationUnavailable = true; result.obligations.emplace_back("v3_copy_local_presence_unavailable"); }
                    }
                    else { result.observationUnavailable = true; result.obligations.emplace_back("v3_copy_local_presence_trace_unavailable"); }
                }
                catch (const std::exception& error)
                { result.observationUnavailable = true; result.obligations.push_back(std::string("v3_copy_local_presence_unavailable:") + error.what()); }
                catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v3_copy_local_presence_unavailable:unknown_exception"); }
                try
                {
                    if (behavior::Access::messageTrace)
                    {
                        result.v3SessionPresence = ole_driver::V3SessionPresenceObservation::Inspect(receipt, verified,
                            *observed->sourceInitial, *source.Get(), *behavior::Access::messageTrace,
                            behavior::Access::fixtureControlWindow);
                        for (const auto& reason : result.v3SessionPresence->Unresolved())
                            result.obligations.push_back("v3_copy_session_presence:" + reason);
                        if (!result.v3SessionPresence->Unresolved().empty() ||
                            result.v3SessionPresence->ResourceOrObservationUnavailable())
                            result.observationUnavailable = true;
                    }
                    else { result.observationUnavailable = true; result.obligations.emplace_back("v3_copy_session_presence_trace_unavailable"); }
                }
                catch (const std::exception& error)
                { result.observationUnavailable = true; result.obligations.push_back(std::string("v3_copy_session_presence_unavailable:") + error.what()); }
                catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v3_copy_session_presence_unavailable:unknown_exception"); }
                try
                {
                    if (behavior::Access::messageTrace)
                    {
                        result.v3FileDropAbi = ole_driver::V3FileDropAbiObservation::Inspect(receipt, verified,
                            generated->payload, *observed->sourceInitial, *source.Get(), *behavior::Access::messageTrace,
                            observed->traceStart, behavior::Access::fixtureControlWindow,
                            kShellFileOperationCompletedMessage);
                        for (const auto& reason : result.v3FileDropAbi->Unresolved())
                            result.obligations.push_back("v3_copy_filedrop_abi:" + reason);
                        if (!result.v3FileDropAbi->UniqueEnvelopeObserved() ||
                            !result.v3FileDropAbi->Unresolved().empty() ||
                            result.v3FileDropAbi->ResourceOrObservationUnavailable())
                            result.observationUnavailable = true;
                    }
                    else { result.observationUnavailable = true; result.obligations.emplace_back("v3_copy_filedrop_abi_trace_unavailable"); }
                }
                catch (const std::exception& error)
                { result.observationUnavailable = true; result.obligations.push_back(std::string("v3_copy_filedrop_abi_unavailable:") + error.what()); }
                catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v3_copy_filedrop_abi_unavailable:unknown_exception"); }
            }
            else
            {
                if (productionStarted) result.observationUnavailable = true;
                result.obligations.emplace_back("v3_copy_source_evidence_awaits_real_completion");
            }
            if (execution.status == ole_driver::Status::event_failed)
            {
                if (productionStarted) result.status = BridgeStatus::production_failed;
                else result.obligations.emplace_back("v3_copy_failure_before_production");
            }
            else if (execution.status == ole_driver::Status::business_failed)
            {
                if (!productionStarted) result.obligations.emplace_back("v3_business_failure_without_production_identity");
            }
            else if (execution.status == ole_driver::Status::event_oracle_failed)
            {
                if (!productionStarted) result.obligations.emplace_back("v3_event_oracle_failure_without_production_identity");
            }
            else if (execution.status == ole_driver::Status::host_observation_failed)
                result.obligations.emplace_back("v3_copy_host_or_oracle_failure_classification_unavailable");
            else if (execution.status == ole_driver::Status::observation_unavailable)
                result.obligations.emplace_back("v3_copy_observation_unavailable");
            else if (execution.status != ole_driver::Status::attempted)
                result.obligations.emplace_back("v3_copy_executor_not_attempted");
            // Postconditions are expectations until a same-run independent
            // observation exists. A returned callback is not a PASS.
            result.obligations.emplace_back("v3_copy_same_run_postcondition_comparison_unimplemented");
        }
        releaseFixture();
        return result;
    }
    catch (const ole_driver::ObservationUnavailable& error)
    {
        result.observationUnavailable = true;
        if (result.failure.empty()) result.failure = error.what();
        else result.failure += std::string("; observation: ") + error.what();
        result.obligations.push_back(std::string("v3_copy_observation_unavailable:") + error.what());
    }
    catch (const std::exception& error)
    {
        result.observationUnavailable = true;
        if (result.failure.empty()) result.failure = error.what();
        else result.failure += std::string("; observation: ") + error.what();
        result.obligations.push_back(std::string("v3_copy_fixture_or_observation_unavailable:") + error.what());
    }
    catch (...)
    {
        result.observationUnavailable = true;
        if (result.failure.empty()) result.failure = "unknown_v3_copy_exception";
        else result.failure += "; unknown_v3_copy_exception";
        result.obligations.emplace_back("v3_copy_fixture_or_observation_unavailable:unknown_exception");
    }
    releaseFixture();
    return result;
}

// Prepare the narrow V2 single-file copy on one generated profile and inspect
// the selected graph with its real provider and host observations. This is
// explicitly a preparation/preflight entry, never a production drag executor.
inline BridgeResult PrepareV2DesktopSingleCopy(const event_graph::Context& context, const Candidate& input,
    const EventProgram& program, const std::vector<bool>& present,
    const event_graph::PresenceActiveGraphVariant& selectedGraph)
{
    BridgeResult result;
    result.obligations.emplace_back("v2_desktop_copy_production_dispatch_unimplemented");
    try
    {
        result.v2Evidence.emplace(V2CaseEvidence::Prepare(context, input, program, present, selectedGraph));
        if (!result.v2Evidence->Valid())
        {
            result.obligations.insert(result.obligations.end(), result.v2Evidence->Errors().begin(), result.v2Evidence->Errors().end());
            return result;
        }
        const ole_driver::HostBoundaries noHost{};
        const auto graphOnly = ole_driver::InspectV2OleGraph(context, input, program, present, selectedGraph,
            {}, nullptr, nullptr, noHost);
        if (!graphOnly.identityValid)
        {
            result.obligations.insert(result.obligations.end(), graphOnly.obligations.begin(), graphOnly.obligations.end());
            return result;
        }
    }
    catch (const std::exception& error)
    { result.obligations.push_back(std::string("v2_case_preflight_observation_unavailable:") + error.what()); return result; }
    catch (...) { result.obligations.emplace_back("v2_case_preflight_observation_unavailable:unknown_exception"); return result; }
    std::vector<Axis> schema;
    Candidate candidate;
    try
    {
    if (context.version != SelectionSchemaVersion::source_members_v2 ||
        !event_graph::RevalidateContext(context).empty())
    { result.obligations.emplace_back("v2_desktop_copy_context_unavailable"); return result; }
    if (const auto reason = ValidatePayloadCandidate(context.axes, input, schema, candidate,
        SelectionSchemaVersion::source_members_v2); !reason.empty())
    { result.obligations.push_back("v2_desktop_copy_candidate_unavailable:" + reason); return result; }
    }
    catch (const std::exception& error)
    { result.obligations.push_back(std::string("v2_candidate_preflight_observation_unavailable:") + error.what()); return result; }
    catch (...) { result.obligations.emplace_back("v2_candidate_preflight_observation_unavailable:unknown_exception"); return result; }
    const auto v = [&](std::string_view id) { return Value(schema, candidate, id); };
    // Fixture support only. In particular, resolved_count/many_count are
    // outcome hypotheses and do not control provider construction.
    try
    {
    if (!SelectionV2HasExplicitFileSource(schema, candidate) || SelectionSourceSize(schema, candidate) != 1 ||
        v("P01.primary") != "file" || v("P01.second") != "na" || v("T01") != "external_ingress" ||
        v("source.E01") != "external_unknown" || v("target.E01") != "free_desktop" ||
        v("target.E02") != "none" || v("target.E23") != "four" || v("target.E24") != "three" ||
        v("target.E19") != "named" || v("G01.button") != "unknown" || v("G02.press") != "unknown" ||
        v("G02.enter_buttons") != "left" || v("G02.enter") != "none" || v("G02.hover_first") != "na" ||
        v("G02.hover_first_buttons") != "na" || v("G02.hover_later") != "na" ||
        v("G02.hover_later_buttons") != "na" || v("G02.release_buttons") != "none" ||
        v("G02.release") != "ctrl" || v("G04.termination") != "drop" || v("T03") != "copy_move" ||
        v("T03.extra_protocol_bits") != "none" || v("T04.capability") != "enabled" || v("C07") != "on" ||
        v("X02") != "none" || v("X02.position") != "no_interruption" || v("H01.region") != "empty_slot" ||
        v("R04.member0_target_exists") != "absent" || v("R04.member0_queued_reservation") != "absent" ||
        v("R04.member0_concurrent_creation") != "none" || v("X03.task_count") != "one" ||
        v("X03.submit_order") != "na" || v("X03.order") != "single")
    { result.obligations.emplace_back("v2_desktop_copy_fixture_plan_unimplemented"); return result; }
    }
    catch (const std::exception& error)
    { result.obligations.push_back(std::string("v2_fixture_plan_observation_unavailable:") + error.what()); return result; }
    catch (...) { result.obligations.emplace_back("v2_fixture_plan_observation_unavailable:unknown_exception"); return result; }

    std::shared_ptr<behavior::GeneratedPayloadProfile> generated;
    Microsoft::WRL::ComPtr<ole_file_source_fixture::Source> source;
    Microsoft::WRL::ComPtr<OleDragDropAdapter> adapter;
    std::unique_ptr<behavior::GeneratedCopyObservation> observed;
    const auto releaseFixture = [&] {
        // The observer borrows the provider and profile; its initial evidence
        // contains only a weak identity token. Destroy it before both owners.
        observed.reset(); adapter.Reset(); source.Reset(); generated.reset();
        try { behavior::Access::ThrowAsyncException(); }
        catch (const std::exception& error)
        {
            if (!result.failure.empty()) result.failure += "; shutdown: ";
            result.failure += error.what();
            result.obligations.emplace_back("v2_generated_fixture_shutdown_unavailable");
        }
        catch (...)
        {
            if (!result.failure.empty()) result.failure += "; shutdown: ";
            result.failure += "unknown_generated_fixture_shutdown_exception";
            result.obligations.emplace_back("v2_generated_fixture_shutdown_unavailable");
        }
    };
    try
    {
        generated = std::make_shared<behavior::GeneratedPayloadProfile>(schema, candidate,
            SelectionSchemaVersion::source_members_v2);
        auto& profile = *generated->profile;
        behavior::Access::EnableProductionDispatch(*profile.app, false);
        behavior::Access::EnableFixtureControlWindow(*profile.app);
        behavior::Wait([&] { return behavior::Access::FileDropIdle(*profile.app) && behavior::Access::LayoutSaveIdle(*profile.app); });
        if (generated->payload.members.size() != 1 || generated->payload.members.front().kind != "file")
        { result.obligations.emplace_back("v2_generated_single_file_source_unavailable"); releaseFixture(); return result; }
        source = ole_file_source_fixture::Make({generated->payload.members.front().path}, true);
        adapter = behavior::Access::ExternalAdapter(*profile.app);
        if (!source || !adapter)
        { result.obligations.emplace_back("v2_generated_provider_or_adapter_unavailable"); releaseFixture(); return result; }
        observed = std::make_unique<behavior::GeneratedCopyObservation>(*generated, *source.Get());
        observed->PrepareSourceEvidence(schema, candidate);
        std::vector<ole_driver::Input> inputs;
        const auto point = behavior::Access::ExternalPoint({150, 150});
        for (std::size_t i = 0; i < program.steps.size(); ++i)
        {
            const auto& step = program.steps[i];
            const auto kind = ole_driver::ProgramKind(step.kind);
            if (!kind) continue;
            if (*kind != ole_driver::Kind::enter && *kind != ole_driver::Kind::drop)
            { result.obligations.push_back("v2_desktop_copy_ole_step_unimplemented:" + step.kind); releaseFixture(); return result; }
            ole_driver::Input event;
            event.programIndex = i; event.kind = *kind; event.modelParameter = step.parameter;
            event.keyState = *kind == ole_driver::Kind::enter ? MK_LBUTTON : MK_CONTROL;
            event.allowedEffect = DROPEFFECT_COPY | DROPEFFECT_MOVE; event.screenPoint = point;
            inputs.push_back(std::move(event));
        }
        result.v2CoordinateInput = InspectV2DesktopCoordinateInput(context, candidate, *generated, inputs);
        const ole_driver::HostBoundaries host{
            [&](const ole_driver::Input& event) { observed->BeforeOleCall(event); },
            [&](const ole_driver::Observation& event) {
                if (event.kind == ole_driver::Kind::enter && result.v2CoordinateInput)
                    result.v2CoordinateEnter = ObserveV2DesktopEnter(context, candidate, *generated, inputs, event);
                observed->ObserveOleReturn(event);
            },
            [&](const ole_driver::Observation& event) { observed->Await(event); },
            [&] { observed->AssertResults(); }};
        result.v2Evidence = V2CaseEvidence::WithOleInputs(*result.v2Evidence, context, candidate, program,
            present, selectedGraph, inputs);
        if (!result.v2Evidence->Valid())
        {
            result.obligations.insert(result.obligations.end(), result.v2Evidence->Errors().begin(), result.v2Evidence->Errors().end());
            releaseFixture(); return result;
        }
        const auto checked = InspectV2DesktopSingleCopy(context, candidate, program, present, selectedGraph, inputs,
            generated.get(), source.Get(), observed->sourceInitial ? &*observed->sourceInitial : nullptr, adapter.Get(), host);
        result.obligations.insert(result.obligations.end(), checked.obligations.begin(), checked.obligations.end());
        result.notApplicable = checked.notApplicable;
        const auto plan = ole_driver::BuildV2OleExecutionPlan(context, candidate, program, present, selectedGraph,
            inputs, adapter.Get(), source.Get(), host);
        result.obligations.insert(result.obligations.end(), plan.obligations.begin(), plan.obligations.end());
        if (!plan.structurallyPlanned) result.obligations.emplace_back("v2_ole_graph_order_plan_unimplemented");
        else result.obligations.emplace_back("v2_ole_graph_order_is_only_a_plan_not_execution");
        // No v2 execution has been implemented. Preflight evidence and bound
        // callbacks do not authorize a production call or a reported pass.
        releaseFixture();
        return result;
    }
    catch (const ole_driver::ObservationUnavailable& error)
    { result.obligations.push_back(std::string("v2_preproduction_observation_unavailable:") + error.what()); }
    catch (const std::exception& error)
    { result.obligations.push_back(std::string("v2_preproduction_fixture_unavailable:") + error.what()); }
    catch (...) { result.obligations.emplace_back("v2_preproduction_fixture_unavailable:unknown_exception"); }
    releaseFixture();
    return result;
}

// Main must supply the actual selected event-graph variant from the same
// Candidate. Do not call a subset of the production fixture when preflight,
// graph identity, delivery or postflight evidence is incomplete. The current
// mapping and graph-identity obligations keep this bridge unimplemented.
inline BridgeResult ExecuteDesktopSingleCopy(const Context& context, const Candidate& candidate,
    const EventProgram& program, const event_graph::Variant& selectedGraph)
{
    BridgeResult result;
    const auto preflight = Inspect(context, candidate, program);
    result.obligations = preflight.obligations;
    const auto plan = DesktopSingleCopyPlan(context, candidate);
    if (!plan) result.obligations.emplace_back("desktop_single_copy_plan_unimplemented");
    if (!preflight.CanExecute()) result.obligations.emplace_back("complete_axis_preflight_unimplemented");
    const event_graph::Context graphContext(context.axes);
    const auto mapping = event_graph::MapSimpleExternal(graphContext, candidate, program);
    if (!mapping.graphAvailable)
        result.obligations.emplace_back("event_graph_mapping_unimplemented");
    for (const auto& reason : mapping.reasons) result.obligations.push_back("mapping:" + reason);
    if (mapping.graphAvailable)
    {
        const auto replay = event_graph::ReplayVariant(graphContext, candidate, mapping.spec, selectedGraph.id);
        if (!replay.identityValid) result.obligations.emplace_back("selected_graph_identity_invalid");
        for (const auto& error : replay.errors) result.obligations.push_back("selected_graph:" + error);
        for (const auto& reason : replay.variant.unresolved) result.obligations.push_back("graph:" + reason);
    }
    // This hard gate stays in place until every input, graph identity and
    // derived/business postflight has actual candidate-bound evidence.
    result.obligations.emplace_back("full_candidate_axis_and_graph_preflight_unimplemented");
    if (!result.obligations.empty()) return result;

    bool productionStarted = false;
    std::shared_ptr<behavior::GeneratedPayloadProfile> generated;
    GeneratedOleInput prepared;
    std::unique_ptr<behavior::GeneratedCopyObservation> observed;
    const auto releaseFixture = [&] {
        // The observer borrows the source; initial evidence retains only a
        // weak token. Destroy the observer before releasing either owner.
        observed.reset(); prepared.adapter.Reset(); prepared.source.Reset(); prepared.fixture.reset(); generated.reset();
        try { behavior::Access::ThrowAsyncException(); }
        catch (const std::exception& error)
        {
            if (!result.failure.empty()) result.failure += "; shutdown: ";
            result.failure += error.what();
            if (productionStarted) result.status = BridgeStatus::production_failed;
            else result.obligations.emplace_back("generated_fixture_shutdown_failed");
        }
        catch (...)
        {
            if (!result.failure.empty()) result.failure += "; shutdown: ";
            result.failure += "unknown_generated_fixture_shutdown_exception";
            if (productionStarted) result.status = BridgeStatus::production_failed;
            else result.obligations.emplace_back("generated_fixture_shutdown_failed");
        }
    };
    try
    {
        // One generated source graph and one host profile are shared by the
        // real provider, adapter, completion observer and business oracle.
        generated = std::make_shared<behavior::GeneratedPayloadProfile>(context.axes, candidate);
        auto& profile = *generated->profile;
        behavior::Access::EnableProductionDispatch(*profile.app, false);
        behavior::Access::EnableFixtureControlWindow(*profile.app);
        behavior::Wait([&] { return behavior::Access::FileDropIdle(*profile.app) && behavior::Access::LayoutSaveIdle(*profile.app); });
        prepared = PrepareGeneratedOleInput(context, candidate, program, selectedGraph, generated);
        result.obligations.insert(result.obligations.end(), prepared.obligations.begin(), prepared.obligations.end());
        if (!prepared.prepared || !prepared.CanExecute() || !result.obligations.empty())
        { releaseFixture(); return result; }

        observed = std::make_unique<behavior::GeneratedCopyObservation>(*generated, *prepared.source.Get());
        observed->PrepareSourceEvidence(context.axes, candidate);
        std::vector<ole_driver::Input> inputs;
        const auto point = behavior::Access::ExternalPoint({150, 150});
        for (std::size_t i = 0; i < program.steps.size(); ++i)
        {
            const auto& step = program.steps[i];
            const auto kind = ole_driver::ProgramKind(step.kind);
            if (!kind) continue; // host stages are handled by GraphExecutor
            if (*kind != ole_driver::Kind::enter && *kind != ole_driver::Kind::drop)
            { result.obligations.push_back("desktop_copy_ole_step_unimplemented:" + step.kind); releaseFixture(); return result; }
            ole_driver::Input event;
            event.programIndex = i; event.kind = *kind; event.modelParameter = step.parameter;
            event.keyState = *kind == ole_driver::Kind::enter ? plan->enterKeys : plan->releaseKeys;
            event.allowedEffect = plan->allowedEffects; event.screenPoint = point;
            inputs.push_back(std::move(event));
        }
        bool completionReached = false;
        std::optional<ole_driver::PassiveTransferEvidence> passiveEvidence;
        const ole_driver::HostBoundaries host{
            [&](const ole_driver::Input& event) { observed->BeforeOleCall(event); },
            [&](const ole_driver::Observation& event) {
                productionStarted = true;
                observed->ObserveOleReturn(event);
                if (event.kind == ole_driver::Kind::enter)
                    behavior::Require(prepared.source->starts == 0 && prepared.source->fileReads == 0,
                        "generated copy Enter does not read delayed file content");
            },
            [&](const ole_driver::Observation& terminal) {
                observed->Await(terminal);
                completionReached = true;
                // Stop at the actual completion boundary, before an oracle
                // may cause an unrelated refresh or reload. These snapshots
                // are not quiescence or per-task attribution proofs.
                try { passiveEvidence = observed->FinishPassiveObservation(); }
                catch (const ole_driver::ObservationUnavailable&) { throw; }
                catch (const std::exception& error)
                { throw ole_driver::ObservationUnavailable(std::string("passive_transfer_observation_unavailable:") + error.what()); }
                catch (...) { throw ole_driver::ObservationUnavailable("passive_transfer_observation_unavailable:unknown_exception"); }
            },
            [&] { observed->AssertResults(); }};
        // Enabling is a fixture observation operation and happens before the
        // first production OLE call. The observer's destructor stops it on
        // every early return or exception if completion is not reached.
        observed->StartPassiveObservation();
        const auto execution = ole_driver::GraphExecutor::Run(graphContext, candidate, program, selectedGraph,
            inputs, prepared.adapter.Get(), prepared.source.Get(), host);
        productionStarted = productionStarted || execution.productionStarted;
        result.productionStarted = productionStarted;
        result.obligations.insert(result.obligations.end(), execution.obligations.begin(), execution.obligations.end());
        bool sourceEvidenceUnavailable = false;
        const bool completed = completionReached;
        if (passiveEvidence)
        {
            const auto& evidence = *passiveEvidence;
            result.obligations.push_back("generated_copy_passive_partial:source_task_candidate=" + std::to_string(evidence.task) +
                ":read_enqueued=" + std::to_string(evidence.readEnqueued) +
                ":read_worker_entered=" + std::to_string(evidence.readWorkerEntered) +
                ":read_worker_returned=" + std::to_string(evidence.readWorkerReturned) +
                ":read_callbacks=" + std::to_string(evidence.readCallbacks) +
                ":file_enqueued=" + std::to_string(evidence.fileEnqueued) +
                ":file_callbacks=" + std::to_string(evidence.fileCallbacks) +
                ":source_completions=" + std::to_string(evidence.sourceCompletions) +
                ":file_completions=" + std::to_string(evidence.fileCompletions) +
                ":source_ends=" + std::to_string(evidence.sourceEnds));
            for (const auto& worker : evidence.workers)
            {
                auto detail = "generated_copy_passive_worker_partial:work=" + std::to_string(worker.work) +
                    ":worker=" + std::to_string(worker.worker) + ":kind=" + std::to_string(worker.kind) +
                    ":thread=" + std::to_string(worker.workerThread) + ":dequeued=" + std::to_string(worker.dequeued) +
                    ":entered=" + std::to_string(worker.entered) + ":returned=" + std::to_string(worker.returned) +
                    ":finished=" + std::to_string(worker.finished) + ":cancelled=" + std::to_string(worker.cancelled) +
                    ":completion=" + std::to_string(worker.completion) +
                    ":queue_entries=" + std::to_string(worker.queueEntries) +
                    ":queue_accepted=" + std::to_string(worker.queueAccepted) +
                    ":queue_failed=" + std::to_string(worker.queueFailed) +
                    ":callbacks=" + std::to_string(worker.callbacks) + ":posts=" + std::to_string(worker.posts) +
                    ":post_failed=" + std::to_string(worker.postFailed) +
                    ":message_entered=" + std::to_string(worker.messageEntered) +
                    ":message_returned=" + std::to_string(worker.messageReturned);
                if (worker.executeResult) detail += ":execute_result=" + std::to_string(*worker.executeResult);
                result.obligations.push_back(std::move(detail));
            }
            for (const auto& reason : evidence.unresolved) result.obligations.push_back("generated_copy_passive:" + reason);
        }
        else result.obligations.emplace_back("generated_copy_passive_completion_or_evidence_unavailable");
        if (productionStarted && completed)
        {
            // Keep the same observer, source and generated profile alive until
            // after the actual completion stage and this read-only audit.
            try
            {
                const auto evidence = observed->SourceEvidence(context.axes, candidate);
                for (const auto& reason : evidence.obligations)
                    result.obligations.push_back("generated_copy_source:" + reason);
                for (const auto& axis : evidence.axes)
                {
                    const auto state = [&] {
                        switch (axis.state)
                        {
                        case ole_driver::FileSourceEvidenceState::partial_observation: return "partial_observation";
                        case ole_driver::FileSourceEvidenceState::pending: return "pending";
                        case ole_driver::FileSourceEvidenceState::unresolved: return "unresolved";
                        case ole_driver::FileSourceEvidenceState::mismatch: return "mismatch";
                        }
                        return "invalid";
                    }();
                    result.obligations.push_back("generated_copy_source_axis:" + axis.id + "=" + axis.candidateValue +
                        ":" + state + ":" + axis.detail);
                }
            }
            catch (const std::exception& error)
            {
                sourceEvidenceUnavailable = true;
                result.obligations.push_back(std::string("generated_copy_source_evidence_unavailable:") + error.what());
            }
            catch (...)
            {
                sourceEvidenceUnavailable = true;
                result.obligations.emplace_back("generated_copy_source_evidence_unavailable:unknown_exception");
            }
        }
        else result.obligations.emplace_back("generated_copy_source_evidence_awaits_production_completion");
        if (execution.status == ole_driver::Status::observation_unavailable)
        {
            result.failure = execution.failure;
            result.obligations.push_back("generated_copy_observation_unavailable:" + execution.failure);
            releaseFixture(); return result;
        }
        if (execution.status == ole_driver::Status::event_failed || execution.status == ole_driver::Status::host_observation_failed)
        {
            if (productionStarted) result.status = BridgeStatus::production_failed;
            result.failure = execution.failure;
            if (!execution.observations.empty() && FAILED(execution.observations.back().result))
            {
                if (!result.failure.empty()) result.failure += "; ";
                result.failure += "ole_hresult:" + std::to_string(execution.observations.back().result);
            }
            if (result.failure.empty()) result.failure = "production_event_or_host_observation_failed";
            if (!productionStarted) result.obligations.emplace_back("generated_graph_execution_failed_before_production");
            releaseFixture(); return result;
        }
        if (execution.status != ole_driver::Status::attempted || execution.hostStagesReturned !=
            std::vector<std::string>{"observe_completion_or_rejection", "assert_business_results"})
        {
            if (productionStarted)
            {
                result.status = BridgeStatus::production_failed;
                result.failure = execution.failure.empty() ? "production_stopped_before_required_observations" : execution.failure;
            }
            else result.obligations.emplace_back("generated_graph_execution_unimplemented");
            releaseFixture(); return result;
        }
        if (execution.observations.size() != inputs.size() || inputs.size() != 2)
        { result.status = BridgeStatus::production_failed; result.failure = "generated_ole_observation_count_mismatch"; releaseFixture(); return result; }
        for (std::size_t i = 0; i < inputs.size(); ++i)
        {
            const auto& actual = execution.observations[i]; const auto& expected = inputs[i];
            if (actual.programIndex != expected.programIndex || actual.kind != expected.kind || FAILED(actual.result) ||
                actual.keyState != expected.keyState || actual.offeredEffect != expected.allowedEffect ||
                !actual.screenPoint || !expected.screenPoint || actual.screenPoint->x != expected.screenPoint->x ||
                actual.screenPoint->y != expected.screenPoint->y || !actual.returnedEffect ||
                (actual.kind == ole_driver::Kind::enter && *actual.returnedEffect != DROPEFFECT_COPY && *actual.returnedEffect != DROPEFFECT_MOVE) ||
                (actual.kind == ole_driver::Kind::drop && *actual.returnedEffect != DROPEFFECT_COPY))
            { result.status = BridgeStatus::production_failed; result.failure = "generated_ole_observation_mismatch"; releaseFixture(); return result; }
        }
        // Release the same provider/adapter before the owning profile, then
        // surface any exception reported while its workers were stopped.
        releaseFixture();
        if (result.status == BridgeStatus::production_failed || !result.failure.empty() || sourceEvidenceUnavailable) return result;
        result.status = BridgeStatus::production_observed;
        result.obligations.emplace_back("enter_default_effect_exact_oracle_unimplemented");
        result.obligations.emplace_back("global_axis_postflight_unimplemented");
    }
    catch (const ole_driver::ObservationUnavailable& error)
    {
        result.productionStarted = productionStarted;
        result.failure = error.what();
        result.obligations.push_back("generated_copy_observation_unavailable:" + result.failure);
        releaseFixture();
    }
    catch (const std::exception& error)
    {
        result.productionStarted = productionStarted;
        result.failure = error.what();
        if (productionStarted) result.status = BridgeStatus::production_failed;
        else result.obligations.emplace_back("generated_fixture_or_execution_exception_before_observed_call");
        releaseFixture();
    }
    catch (...)
    {
        result.productionStarted = productionStarted;
        result.failure = "unknown_generated_fixture_or_execution_exception";
        if (productionStarted) result.status = BridgeStatus::production_failed;
        else result.obligations.emplace_back("generated_fixture_or_execution_exception_before_observed_call");
        releaseFixture();
    }
    return result;
}

// This selects only a single-format text fixture. It is not evidence that
// any other content format, mixed offer, candidate axis or event was bound.
inline bool DesktopSingleTextPlan(const Context& context, const Candidate& input)
{
    if (!context.obligations.empty() || input.values.size() != context.axes.size()) return false;
    Candidate candidate = input; candidate.lookup = context.lookup;
    const auto v = [&](std::string_view id) { return Value(context.axes, candidate, id); };
    return v("T01") == "external_ingress" && v("source.E01") == "external_unknown" &&
        v("target.E01") == "free_desktop" && v("target.E02") == "none" &&
        v("target.E23") == "four" && v("target.E24") == "three" &&
        v("P01.primary") == "text" && v("P01.second") == "na" &&
        v("P02.source_actual_count") == "one" && v("P02.resolved_count") == "one" && v("P02.many_count") == "na" &&
        v("T02.file_paths") == "no" && v("T02.shell_object") == "no" && v("T02.virtual_files") == "no" &&
        v("T02.virtual_completeness") == "na" && v("T02.image") == "no" && v("T02.text") == "yes" &&
        v("T02.url") == "no" && v("T02.url_kind") == "na" && v("T02.invalid_format") == "no" &&
        v("T03") == "copy" && v("T03.extra_protocol_bits") == "none" &&
        v("T04.capability") == "absent" && v("T04.availability") == "immediate" &&
        v("G01.button") == "unknown" && v("G02.press") == "unknown" &&
        v("G02.enter_buttons") == "left" && v("G02.enter") == "none" &&
        v("G02.hover_first") == "na" && v("G02.hover_first_buttons") == "na" &&
        v("G02.hover_later") == "na" && v("G02.hover_later_buttons") == "na" &&
        v("G02.release_buttons") == "none" && v("G02.release") == "ctrl" && v("G04.termination") == "drop" &&
        v("H01.region") == "empty_slot" && v("R04.member0_target_exists") == "absent" &&
        v("R04.member0_queued_reservation") == "absent" && v("R04.member0_concurrent_creation") == "none" &&
        v("X02") == "none" && v("X02.position") == "no_interruption" &&
        v("X03.task_count") == "one" && v("X03.submit_order") == "na" && v("X03.order") == "single";
}

// Prepares only the existing observer's exact Unicode input and isolated host.
// This is not a V4 event executor: no caller may pass a legacy graph, and a
// constructed source/adapter does not prove production format selection,
// refresh attribution, final layout or any other generated axis. The same
// candidate must later be rebound to a validated V4 graph and executor.
// A reviewable coordinate draft for the fixed Unicode provider.  Every
// non-singleton axis must have an explicit choice; this routine never selects
// a domain's first value or treats an unmodelled input as inapplicable.  It
// does not construct a graph, run a fixture, or claim that any input is under
// control.  In particular Hypothesis values remain postconditions.
struct V4TextSeedDraft
{
    std::optional<Candidate> candidate;
    // Also populated for an incomplete draft so every selected value can be
    // reviewed alongside the missing axes; it is not observation evidence.
    std::vector<std::pair<std::string, std::string>> selected;
    std::vector<std::string> missing, errors;
};
inline V4TextSeedDraft DraftV4TextSeed(const event_graph::V4FamilyIdentityContext& identity,
    const std::vector<std::pair<std::string, std::string>>& explicitChoices)
{
    V4TextSeedDraft result;
    bool knownFamily = identity.FamilyBinding() == V4FamilyBinding(V4FirstDiscoverySeed());
    for (std::size_t i = 0; i < V4AdditionalTextSeedCount; ++i)
    {
        const auto family = V4AdditionalTextSeed(i);
        if (family && identity.FamilyBinding() == V4FamilyBinding(*family)) knownFamily = true;
    }
    if (identity.Status() != V4FamilyStatus::schema_only || !knownFamily)
    { result.errors.emplace_back("v4_text_seed_family_unavailable"); return result; }
    std::unordered_map<std::string, std::string> choices;
    for (const auto& [id, value] : explicitChoices)
        if (!choices.emplace(id, value).second) result.errors.push_back("v4_text_seed_duplicate_axis:" + id);
    // These are chosen candidate inputs, not claims about observed host state.
    // Remaining non-singleton axes must still be supplied individually.
    for (const auto& [id, value] : std::initializer_list<std::pair<const char*, const char*>>{
        {"T01", "external_ingress"}, {"source.E01", "external_unknown"},
        {"source.E02", "unknown"}, {"source.E03", "unknown"}, {"source.E10", "unknown"},
        {"target.E01", "free_desktop"}, {"target.E02", "none"}, {"target.E03", "none"},
        {"target.E04", "na"}, {"target.E05", "na"}, {"target.E06", "na"}, {"target.E07", "na"},
        {"target.E08", "na"}, {"target.E08.active_child_type", "na"}, {"target.E08.hit_relation", "na"},
        {"target.E08.hit_position", "na"}, {"target.E09", "na"},
        {"target.E11", "na"}, {"target.E12", "na"}, {"target.E13", "na"}, {"target.E14", "na"},
        {"target.E15", "na"}, {"target.E16", "na"}, {"target.E17.columns", "na"},
        {"target.E17.rows", "na"}, {"target.E18", "na"}, {"target.E26", "na"},
        {"target.E19", "named"}, {"target.E20", "other_visible_items"},
        {"target.E23", "four"}, {"target.E24", "three"}, {"H01.region", "empty_slot"},
        {"H06.page_record", "saved"}, {"H06.page_dimensions", "valid"}, {"H07.occupancy", "partial"},
        {"C07", "on"},
        {"G04.termination", "drop"}, {"G01.button", "unknown"}, {"G02.press", "unknown"},
        {"G02.enter", "none"}, {"G02.enter_buttons", "left"}, {"G02.hover_first", "na"},
        {"G02.hover_later", "na"}, {"G02.hover_first_buttons", "na"},
        {"G02.hover_later_buttons", "na"}, {"G02.release", "ctrl"}, {"G02.release_buttons", "none"},
        {"T02.file_paths", "no"}, {"T02.shell_object", "no"}, {"T02.virtual_files", "no"},
        {"T02.image", "no"}, {"T02.text", "yes"}, {"T02.url", "no"},
        {"T02.virtual_completeness", "na"}, {"T02.url_kind", "na"},
        {"T02.invalid_format", "no"}, {"T03", "copy"}, {"T03.extra_protocol_bits", "none"},
        {"T04.capability", "absent"}, {"T04.availability", "immediate"},
        {"V4.payload.text_unicode.hglobal.default.bytes", "hex_53006e006f0077002000ea960000"},
        {"P01.primary", "text"}, {"P01.second", "na"},
        {"P01.member0_kind", "text"}, {"P01.member1_kind", "na"},
        {"X02", "none"}, {"X02.position", "no_interruption"}, {"X03.submit_order", "na"},
        {"X03.order", "single"}, {"X03.callback_reentrant", "no"},
        {"X03.callback_duplicate", "no"}, {"X03.refresh", "normal"},
        {"G03.decision", "na"}, {"H03.page_switch", "none"}, {"H03.after_drop", "unchanged"},
        {"H05.position", "na"}, {"H05.source_relation", "na"}, {"H05.filter", "na"},
        {"H05.surface", "na"}, {"H05.indicated_identity", "na"},
        {"H05.scroll", "unchanged"}, {"P05.source_reorder", "unchanged"},
        {"H08.first_direction", "unobserved"}, {"H08.later_direction", "na"},
        {"V01.initial", "unknown"}, {"V03", "unknown"}, {"V05.initial", "unknown"},
        {"V01.change", "none"}, {"V05.change", "none"}, {"V06.change", "na"},
        {"V04.lua_change", "na"}, {"source.E23.change", "na"}, {"source.E24.change", "na"},
        {"target.E23.change", "stable"}, {"target.E24.change", "stable"},
        {"H02.connection", "stable"}, {"H06.reservation_change", "unchanged"},
        {"H07.occupancy_change", "unchanged"},
        {"S01", "none"}, {"S02", "none"}, {"S03.dwell", "none"}, {"S03.navigation", "none"},
        {"S04", "none"}, {"S05", "none"}, {"S06", "none"}, {"S07", "none"},
        {"S08", "none"}, {"S09", "none"}, {"S10", "none"}})
    {
        const auto [it, inserted] = choices.emplace(id, value);
        if (!inserted && it->second != value) result.errors.push_back(std::string("v4_text_seed_fixed_choice_conflict:") + id);
    }
    for (const auto id : kEnvironmentChangeFields)
    {
        const auto phase = std::string(id) + "_phase";
        const auto [it, inserted] = choices.emplace(phase, "na");
        if (!inserted && it->second != "na") result.errors.push_back("v4_text_seed_fixed_choice_conflict:" + phase);
    }
    for (const auto id : kLifecycleChangeFields)
    {
        const auto [it, inserted] = choices.emplace(std::string(id), "stable");
        if (!inserted && it->second != "stable") result.errors.push_back("v4_text_seed_fixed_choice_conflict:" + std::string(id));
        const auto phase = std::string(id) + "_phase";
        const auto [phaseIt, phaseInserted] = choices.emplace(phase, "na");
        if (!phaseInserted && phaseIt->second != "na") result.errors.push_back("v4_text_seed_fixed_choice_conflict:" + phase);
    }
    for (const auto& axis : identity.Axes())
    {
        std::optional<std::string> value;
        if (axis.id.starts_with("V4.format.") && axis.id.ends_with(".offer"))
            value = axis.id == "V4.format.text_unicode.offer" ? "yes" : "no";
        else if (axis.id.starts_with("V4.format.") && axis.id.find(".medium.") != std::string::npos)
            value = axis.id == "V4.format.text_unicode.medium.hglobal" ? "yes" : "no";
        if (!value) continue;
        const auto [it, inserted] = choices.emplace(axis.id, *value);
        if (!inserted && it->second != *value) result.errors.push_back("v4_text_seed_format_choice_conflict:" + axis.id);
    }
    Candidate candidate;
    candidate.values.resize(identity.Axes().size());
    for (std::size_t i = 0; i < identity.Axes().size(); ++i)
    {
        const auto& axis = identity.Axes()[i];
        const auto found = choices.find(axis.id);
        if (axis.role == Role::Derived)
        {
            if (found != choices.end()) { result.errors.push_back("v4_text_seed_derived_choice:" + axis.id); choices.erase(found); }
            continue;
        }
        if (found != choices.end())
        {
            candidate.values[i] = found->second;
            choices.erase(found);
        }
        else if (axis.values.size() == 1) candidate.values[i] = axis.values.front();
        else { result.missing.push_back(axis.id); continue; }
        result.selected.emplace_back(axis.id, candidate.values[i]);
        if (std::find(axis.values.begin(), axis.values.end(), candidate.values[i]) == axis.values.end())
            result.errors.push_back("v4_text_seed_value_outside_domain:" + axis.id);
    }
    for (const auto& [id, value] : choices)
    { (void)value; result.errors.push_back("v4_text_seed_unknown_axis:" + id); }
    if (!result.missing.empty() || !result.errors.empty()) return result;
    const auto raw = identity.RawCandidateIdentity(candidate);
    if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty())
    { result.errors.emplace_back("v4_text_seed_coordinate_unavailable"); return result; }
    result.candidate = std::move(candidate);
    return result;
}

// Bounded preview of the SAME candidate's shared joined graph. The caller
// must persist the returned page/cursor together, and register any selected
// graph through the scheduler's authenticated direct-seed API. This helper
// does not alter the ordinary candidate DFS or assert applicability/coverage.
struct V4TextSeedGraphPreview
{
    V4TextSeedDraft draft;
    std::optional<event_graph::V4JoinedGraphPage> page;
    std::vector<std::string> errors;
    bool resourceUnavailable = false;
};
inline V4TextSeedGraphPreview PreviewV4TextSeedGraph(const V4FamilyDescriptor& family,
    const V4AxisBudget& axisBudget, const std::vector<std::pair<std::string, std::string>>& choices,
    const event_graph::V4JoinedGraphCursor& cursor, std::size_t workBudget)
{
    V4TextSeedGraphPreview result;
    try
    {
        const auto identity = event_graph::V4FamilyIdentityContext::Create(family, axisBudget);
        if (identity.Status() == V4FamilyStatus::resource_unavailable) result.resourceUnavailable = true;
        result.draft = DraftV4TextSeed(identity, choices);
        if (!result.draft.candidate) return result;
        const auto verified = V4FamilyValidatedContext::Create(family, axisBudget, *result.draft.candidate);
        if (verified.Status() != V4CoordinateStatus::coordinate_ready)
        { result.resourceUnavailable = verified.Status() == V4CoordinateStatus::resource_unavailable;
          result.errors = verified.Unresolved(); result.errors.emplace_back("v4_text_seed_verified_coordinate_unavailable"); return result; }
        auto page = event_graph::V4JoinedGraph::Enumerate(identity, verified, cursor, workBudget, 1);
        if (page.resourceUnavailable) { result.resourceUnavailable = true; return result; }
        if (!page.errors.empty()) { result.errors = page.errors; return result; }
        for (const auto& item : page.graphs)
        {
            const auto replay = event_graph::V4JoinedGraph::Replay(identity, verified, item.present, item.graph.id);
            if (!replay.identityValid || !replay.errors.empty() || replay.variant.id != item.graph.id ||
                replay.variant.classes != item.graph.classes || replay.variant.order != item.graph.order ||
                replay.variant.unresolved != item.graph.unresolved)
            { result.resourceUnavailable = replay.status == event_graph::V4GraphReplayStatus::resource_unavailable;
              result.errors.emplace_back("v4_text_seed_graph_replay_unavailable"); return result; }
        }
        result.page = std::move(page);
    }
    catch (const std::bad_alloc&) { result.page.reset(); result.resourceUnavailable = true; }
    catch (const std::length_error&) { result.page.reset(); result.resourceUnavailable = true; }
    return result;
}

inline BridgeResult PrepareV4DesktopSingleText(const event_graph::V4FamilyIdentityContext& identity,
    const V4FamilyValidatedContext& verified, const std::vector<bool>* selectedPresence = nullptr,
    const event_graph::V4JoinedGraphVariant* selectedGraph = nullptr)
{
    BridgeResult result;
    const auto mapping = event_graph::MapV4FamilyCandidate(identity, verified);
    if (mapping.status != "mapping_unimplemented" && mapping.status != "graph_preview_unexecuted")
    {
        result.obligations.push_back("v4_text_identity:" + mapping.status);
        result.obligations.insert(result.obligations.end(), mapping.reasons.begin(), mapping.reasons.end());
        return result;
    }
    result.obligations.insert(result.obligations.end(), mapping.reasons.begin(), mapping.reasons.end());
    if ((selectedPresence == nullptr) != (selectedGraph == nullptr))
    { result.obligations.emplace_back("v4_text_graph_selection_incomplete"); return result; }
    if (selectedGraph)
    {
        const auto replay = event_graph::V4JoinedGraph::Replay(identity, verified, *selectedPresence, selectedGraph->id);
        if (!replay.identityValid || !replay.errors.empty() || replay.variant.classes != selectedGraph->classes ||
            replay.variant.order != selectedGraph->order || replay.variant.unresolved != selectedGraph->unresolved)
        {
            result.obligations.emplace_back("v4_text_graph_identity_unavailable");
            result.obligations.insert(result.obligations.end(), replay.errors.begin(), replay.errors.end());
            return result;
        }
    }
    const auto& axes = verified.Axes();
    const auto& candidate = verified.Coordinate();
    const auto value = [&](std::string_view id) { return Value(axes, candidate, id); };
    // Exact UTF-16LE bytes including the terminating NUL for Snow + space + 雪.
    // The observer's expected UTF-8 output is independent and fixed; never
    // apply it to another generated payload or a configured GetData failure.
    if (value("V4.payload.text_unicode.hglobal.default.bytes") != "hex_53006e006f0077002000ea960000" ||
        value("target.E01") != "free_desktop" || value("target.E02") != "none" ||
        value("target.E23") != "four" || value("target.E24") != "three" ||
        value("H01.region") != "empty_slot" || value("G04.termination") != "drop")
    { result.obligations.emplace_back("v4_text_fixed_input_or_desktop_fixture_unimplemented"); return result; }

    bool dispatchAttempted = false;
    std::unique_ptr<behavior::Profile> profile;
    const auto release = [&] {
        profile.reset();
        try { behavior::Access::ThrowAsyncException(); }
        catch (const std::exception& error)
        { result.observationUnavailable = true; result.obligations.push_back(std::string("v4_text_fixture_shutdown_unavailable:") + error.what()); }
        catch (...)
        { result.observationUnavailable = true; result.obligations.emplace_back("v4_text_fixture_shutdown_unavailable"); }
    };
    try
    {
        profile = std::make_unique<behavior::Profile>();
        behavior::Access::EnableProductionDispatch(*profile->app, false);
        behavior::Access::EnableFixtureControlWindow(*profile->app);
        behavior::Wait([&] { return behavior::Access::FileDropIdle(*profile->app) && behavior::Access::LayoutSaveIdle(*profile->app); });
        {
            const auto raw = identity.RawCandidateIdentity(candidate);
            const auto selected = value("C07");
            if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty() ||
                behavior::Access::asyncOwner != profile->app || behavior::Access::productionDispatch != 1 ||
                (selected != "on" && selected != "off"))
                result.obligations.emplace_back("v4_text_program_collection_input_unavailable");
            else
            {
                const bool actual = behavior::Access::ProgramCollectionEnabled(*profile->app);
                result.v4TextProgramCollectionInput = BridgeResult::V4TextProgramCollectionInput{
                    raw.id, reinterpret_cast<std::uintptr_t>(profile->app), GetCurrentThreadId(), actual,
                    actual == (selected == "on")};
                if (!result.v4TextProgramCollectionInput->candidateMatched)
                    result.obligations.emplace_back("v4_text_program_collection_input_mismatch");
                // Snapshot only; the same value still needs checking at the
                // production boundary and after any relevant host reentry.
            }
        }
        {
            const auto raw = identity.RawCandidateIdentity(candidate);
            if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty() ||
                behavior::Access::asyncOwner != profile->app || behavior::Access::productionDispatch != 1)
                result.obligations.emplace_back("v4_text_desktop_input_identity_unavailable");
            else
            {
                const auto initial = behavior::Access::InspectTextDesktopInitial(*profile->app,
                    profile->desktop / L"Sentinel.txt", POINT{150, 150});
                const bool matches = value("target.E01") == "free_desktop" && value("target.E02") == "none" &&
                    value("target.E19") == "named" && !initial.pageName.empty() &&
                    value("H06.page_record") == "saved" && initial.pageRecorded &&
                    value("H06.page_dimensions") == "valid" && initial.savedColumns && initial.savedRows &&
                    *initial.savedColumns == initial.columns && *initial.savedRows == initial.rows &&
                    value("H07.occupancy") == "partial" && EndpointSize(value("target.E23")) == initial.columns &&
                    EndpointSize(value("target.E24")) == initial.rows && value("H01.region") == "empty_slot";
                result.v4TextDesktopInput = BridgeResult::V4TextDesktopInput{
                    raw.id, reinterpret_cast<std::uintptr_t>(profile->app), GetCurrentThreadId(), initial, matches};
                if (!matches) result.obligations.emplace_back("v4_text_desktop_input_candidate_mismatch");
                result.obligations.emplace_back("v4_text_other_page_item_visibility_unverified");
                // This is a page/model snapshot; real target dispatch and
                // production results remain independently observed.
            }
        }
        {
            // The profile-owned source is invalidated on its live STA after
            // the observer and adapter leave this scope, even on exceptions.
            const auto owner = behavior::Access::CreateV4UnicodeSourceOwner(*profile->app);
            auto source = ole_driver::PrepareV4UnicodeSource(verified, identity, owner);
            result.obligations.insert(result.obligations.end(), source.Unresolved().begin(), source.Unresolved().end());
            if (source.ResourceUnavailable())
                result.obligations.emplace_back("v4_text_source_resource_unavailable");
            else if (!source.EntryBound() || !source.SourceObject() || source.ConfiguredGetFailure() != S_OK)
                result.obligations.emplace_back("v4_text_source_or_successful_get_unimplemented");
            else
            {
                result.v4UnicodeFixtureInput = ole_driver::InspectV4UnicodeFixtureInputs(verified, identity, source);
                if (!result.v4UnicodeFixtureInput->snapshotComplete || result.v4UnicodeFixtureInput->resourceUnavailable)
                    result.obligations.emplace_back("v4_text_unicode_fixture_input_unavailable");
                for (const auto& item : result.v4UnicodeFixtureInput->axes)
                    if (!item.matches) result.obligations.push_back("v4_text_unicode_fixture_axis_mismatch:" + item.axis);
                for (const auto& reason : result.v4UnicodeFixtureInput->unresolved)
                    result.obligations.push_back("v4_text_unicode_fixture_input:" + reason);
                auto hostBinding = behavior::Access::CreateV3AdapterAndHostEpoch(*profile->app, source.SourceObject());
                if (!hostBinding.first || !hostBinding.second) result.obligations.emplace_back("v4_text_production_adapter_or_epoch_unavailable");
                else
                {
                    behavior::GeneratedTextObservation observed(*profile, *source.SourceObject());
                    result.obligations.emplace_back("v4_text_prepared_fixture_only_no_production_dispatch");
                    result.obligations.emplace_back("v4_text_graph_axes_refresh_and_same_run_oracle_binding_unimplemented");
                    if (selectedGraph)
                    {
                        const auto program = verified.TransferProgram();
                        std::vector<ole_driver::Input> inputs;
                        const auto point = behavior::Access::ExternalPoint({150, 150});
                        for (std::size_t i = 0; i < program.steps.size(); ++i)
                        {
                            const auto kind = ole_driver::ProgramKind(program.steps[i].kind);
                            if (!kind) continue;
                            if (*kind != ole_driver::Kind::enter && *kind != ole_driver::Kind::drop)
                            { result.obligations.push_back("v4_text_ole_step_unimplemented:" + program.steps[i].kind); continue; }
                            ole_driver::Input input;
                            input.programIndex = i; input.kind = *kind; input.modelParameter = program.steps[i].parameter;
                            input.keyState = *kind == ole_driver::Kind::enter ? MK_LBUTTON : MK_CONTROL;
                            input.allowedEffect = DROPEFFECT_COPY; input.screenPoint = point;
                            inputs.push_back(std::move(input));
                        }
                        const auto plan = ole_driver::BuildV4JoinedOleExecutionPlan(verified, identity,
                            *selectedPresence, *selectedGraph, inputs);
                        result.obligations.insert(result.obligations.end(), plan.preconditions.begin(), plan.preconditions.end());
                        result.graphUnresolved = plan.graphUnresolved;
                        result.postDispatchObligations = plan.postDispatchObligations;
                        result.unclassifiedObligations = plan.unclassified;
                        result.outcomeExpectations = plan.postconditions;
                        result.v4PresenceExpectations = plan.presence;
                        if (!plan.unclassified.empty()) result.obligations.emplace_back("v4_text_obligation_stage_unclassified");
                        if (!plan.graphIdentityValid || !plan.structurallyPlanned)
                            result.obligations.emplace_back("v4_text_graph_plan_unavailable");
                        // The fixed observer has not yet proved the full
                        // candidate, conditional-presence observation interval,
                        // host epoch/refresh attribution and same-run oracle.
                        if (result.obligations.empty())
                        {
                            const ole_driver::HostBoundaries host{
                                [&](const ole_driver::Input& event) {
                                    if (!result.v4TextProgramCollectionInput ||
                                        !result.v4TextProgramCollectionInput->candidateMatched ||
                                        behavior::Access::asyncOwner != profile->app ||
                                        result.v4TextProgramCollectionInput->hostIdentity != reinterpret_cast<std::uintptr_t>(profile->app) ||
                                        result.v4TextProgramCollectionInput->ownerThread != GetCurrentThreadId() ||
                                        behavior::Access::ProgramCollectionEnabled(*profile->app) != result.v4TextProgramCollectionInput->actual)
                                        throw ole_driver::ObservationUnavailable("v4_text_program_collection_changed_before_ole_call");
                                    result.v4TextProgramCollectionInput->matchingPreCallProgramIndices.push_back(event.programIndex);
                                    if (!result.v4TextDesktopInput || !result.v4TextDesktopInput->candidateMatched ||
                                        result.v4TextDesktopInput->hostIdentity != reinterpret_cast<std::uintptr_t>(profile->app) ||
                                        result.v4TextDesktopInput->ownerThread != GetCurrentThreadId())
                                        throw ole_driver::ObservationUnavailable("v4_text_desktop_input_identity_unavailable");
                                    const auto current = behavior::Access::InspectTextDesktopInitial(*profile->app,
                                        profile->desktop / L"Sentinel.txt", POINT{150, 150});
                                    const auto& before = result.v4TextDesktopInput->initial;
                                    if (current.pageId != before.pageId || current.pageName != before.pageName ||
                                        current.sentinelKey != before.sentinelKey || current.columns != before.columns ||
                                        current.rows != before.rows || current.pageRecorded != before.pageRecorded ||
                                        current.savedColumns != before.savedColumns || current.savedRows != before.savedRows ||
                                        current.sentinelSpan.columns != before.sentinelSpan.columns ||
                                        current.sentinelSpan.rows != before.sentinelSpan.rows ||
                                        current.layoutRevision != before.layoutRevision ||
                                        !behavior::Same(current.sentinelCell, before.sentinelCell) ||
                                        !behavior::Same(current.targetCell, before.targetCell) ||
                                        !EqualRect(&current.pageBounds, &before.pageBounds) ||
                                        !EqualRect(&current.targetBounds, &before.targetBounds))
                                        throw ole_driver::ObservationUnavailable("v4_text_desktop_input_changed_before_ole_call");
                                    result.v4TextDesktopInput->matchingPreCallProgramIndices.push_back(event.programIndex);
                                    observed.BeforeOleCall(event);
                                },
                                [&](const ole_driver::Observation& event) { observed.ObserveOleReturn(event); },
                                [&](const ole_driver::Observation& terminal) { observed.Await(terminal); },
                                [&] { observed.AssertResults(); }};
                            const auto oracle = [](const ole_driver::Observation& event) {
                                if (!event.returnedEffect)
                                    throw ole_driver::ObservationUnavailable("v4 text OLE returned effect unavailable");
                                if (FAILED(event.result) || *event.returnedEffect != DROPEFFECT_COPY)
                                    throw ole_driver::EventAssertionFailure("v4 text OLE result differs from the independent copy expectation");
                            };
                            // One process-wide bounded diagnostic session. It
                            // observes real requests but cannot establish
                            // future producer quiescence; every exceptional
                            // exit stops it before fixture teardown.
                            if (!snowdesktop::external_transfer_observation::Enable())
                                throw ole_driver::ObservationUnavailable("v4 text refresh diagnostic session unavailable");
                            struct StopDiagnostics final
                            {
                                bool active = true, workerActive = false;
                                void Stop() noexcept
                                {
                                    if (workerActive) { snowdesktop::background_work_observation::Stop(); workerActive = false; }
                                    if (active) { snowdesktop::external_transfer_observation::Stop(); active = false; }
                                }
                                ~StopDiagnostics() { Stop(); }
                            } diagnostics;
                            if (!snowdesktop::background_work_observation::Enable())
                                throw ole_driver::ObservationUnavailable("v4 text refresh worker diagnostic session unavailable");
                            diagnostics.workerActive = true;
                            dispatchAttempted = true;
                            const auto receipt = ole_driver::V4GraphExecutor::RunBound(verified, identity,
                                *selectedPresence, *selectedGraph, inputs, source, owner,
                                hostBinding.first.Get(), hostBinding.second, host, oracle);
                            diagnostics.Stop(); result.v4DiagnosticBudget = snowdesktop::external_transfer_observation::ObserveBudget();
                            result.v4BackgroundDiagnosticBudget = snowdesktop::background_work_observation::ObserveBudget();
                            const auto& execution = receipt.Execution(); result.v4OleAdapterLogLast = execution.oleAdapterLogLast;
                            result.productionStarted = execution.productionStarted;
                            result.productionEntryUnknown = execution.productionEntryUnknown;
                            dispatchAttempted = false;
                            result.businessFailureObserved = execution.businessFailureObserved;
                            result.eventOracleFailureObserved = execution.eventOracleFailureObserved;
                            result.eventDispatchObservationRecorded = !execution.observations.empty();
                            result.eventDispatchFailureObserved = std::any_of(execution.observations.begin(), execution.observations.end(),
                                [](const ole_driver::Observation& event) { return FAILED(event.result); });
                            if (execution.productionStarted && (execution.eventOracleFailureObserved || execution.businessFailureObserved ||
                                execution.status == ole_driver::Status::business_failed))
                                result.status = BridgeStatus::production_failed;
                            try { result.v4SourcePreparation = receipt.SourcePreparation(); }
                            catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v4_text_source_preparation_copy_unavailable"); }
                            try { result.v4SourceAtExecution = receipt.SourceAtExecution(); }
                            catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v4_text_source_execution_copy_unavailable"); }
                            result.observationUnavailable = result.observationUnavailable || execution.additionalObservationUnavailable ||
                                execution.status == ole_driver::Status::observation_unavailable ||
                                execution.status == ole_driver::Status::host_observation_failed ||
                                execution.status == ole_driver::Status::event_failed;
                            result.failure = execution.failure;
                            if (execution.productionStarted)
                            {
                                try { result.v4TextBusiness = observed.businessEvidence; }
                                catch (const std::exception& error)
                                { result.observationUnavailable = true; result.obligations.push_back(std::string("v4_text_business_evidence_unavailable:") + error.what()); }
                                catch (...)
                                { result.observationUnavailable = true; result.obligations.emplace_back("v4_text_business_evidence_unavailable:unknown_exception"); }
                            }
                            try
                            {
                                result.v4OleObservations = execution.observations;
                                result.v4CapturedOleReturns = receipt.CapturedReturns();
                                if ((execution.productionStarted && result.v4OleObservations.empty()) ||
                                    result.v4OleObservations.size() != result.v4CapturedOleReturns.size())
                                { result.observationUnavailable = true; result.obligations.emplace_back("v4_text_ole_scope_capture_incomplete"); }
                            }
                            catch (const std::exception& error)
                            { result.observationUnavailable = true; result.obligations.push_back(std::string("v4_text_ole_capture_unavailable:") + error.what()); }
                            catch (...)
                            { result.observationUnavailable = true; result.obligations.emplace_back("v4_text_ole_capture_unavailable:unknown_exception"); }
                            if (execution.productionStarted)
                            {
                                try
                                {
                                    if (receipt.CompletionTraceCaptured()) result.v4CompletionTrace = receipt.CompletionTrace();
                                    else { result.observationUnavailable = true; result.obligations.emplace_back("v4_text_completion_trace_unavailable"); }
                                }
                                catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v4_text_completion_trace_copy_unavailable"); }
                            }
                            result.obligations.insert(result.obligations.end(), execution.obligations.begin(), execution.obligations.end());
                            if (execution.productionStarted && (execution.eventOracleFailureObserved ||
                                execution.status == ole_driver::Status::business_failed || execution.businessFailureObserved))
                                result.status = BridgeStatus::production_failed;
                            if (execution.productionStarted && behavior::Access::messageTrace)
                            {
                                try
                                {
                                    result.v4UnicodeRead = ole_driver::V4UnicodeReadObservation::Inspect(receipt, verified,
                                        identity, *selectedPresence, *selectedGraph, source, owner, hostBinding.first.Get(),
                                        hostBinding.second, *behavior::Access::messageTrace, behavior::Access::fixtureControlWindow);
                                    for (const auto& reason : result.v4UnicodeRead->Unresolved())
                                        result.obligations.push_back("v4_text_read:" + reason);
                                    if (result.v4UnicodeRead->ResourceOrObservationUnavailable() ||
                                        !result.v4UnicodeRead->Unresolved().empty()) result.observationUnavailable = true;
                                }
                                catch (const std::exception& error)
                                { result.observationUnavailable = true; result.obligations.push_back(std::string("v4_text_read_unavailable:") + error.what()); }
                                catch (...)
                                { result.observationUnavailable = true; result.obligations.emplace_back("v4_text_read_unavailable:unknown_exception"); }
                            }
                            else if (execution.productionStarted)
                            { result.observationUnavailable = true; result.obligations.emplace_back("v4_text_read_trace_unavailable"); }
                            if (execution.productionStarted && behavior::Access::messageTrace)
                            {
                                try
                                {
                                    result.v4UnicodeCaller = ole_driver::InspectV4UnicodeCaller(receipt, verified, identity,
                                        *selectedPresence, *selectedGraph, source, owner, hostBinding.first.Get(),
                                        hostBinding.second, *behavior::Access::messageTrace, behavior::Access::fixtureControlWindow);
                                    for (const auto& reason : result.v4UnicodeCaller->unresolved)
                                        result.obligations.push_back("v4_text_unicode_caller:" + reason);
                                    if (result.v4UnicodeCaller->observationUnavailable || !result.v4UnicodeCaller->unresolved.empty() ||
                                        !result.v4UnicodeCaller->actualCallerReturned || !result.v4UnicodeCaller->uniqueProviderCallObserved)
                                        result.observationUnavailable = true;
                                }
                                catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v4_text_unicode_caller_observation_unavailable"); }
                            }
                            else if (execution.productionStarted)
                            { result.observationUnavailable = true; result.obligations.emplace_back("v4_text_unicode_caller_trace_unavailable"); }
                            if (execution.productionStarted && behavior::Access::messageTrace)
                            {
                                try
                                {
                                    result.v4UnicodeProviderInterval = ole_driver::InspectV4UnicodeProviderInterval(receipt, verified, identity,
                                        *selectedPresence, *selectedGraph, source, owner, hostBinding.first.Get(),
                                        hostBinding.second, *behavior::Access::messageTrace, behavior::Access::fixtureControlWindow);
                                    for (const auto& reason : result.v4UnicodeProviderInterval->unresolved)
                                        result.obligations.push_back("v4_text_unicode_provider_interval:" + reason);
                                    if (result.v4UnicodeProviderInterval->observationUnavailable ||
                                        !result.v4UnicodeProviderInterval->localProviderIntervalObserved ||
                                        !result.v4UnicodeProviderInterval->unresolved.empty()) result.observationUnavailable = true;
                                }
                                catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v4_text_unicode_provider_interval_unavailable"); }
                                try
                                {
                                    result.v4TextGraph = ole_driver::InspectV4TextGraph(receipt, verified, identity,
                                        *selectedPresence, *selectedGraph, source, owner, hostBinding.first.Get(),
                                        hostBinding.second, *behavior::Access::messageTrace, behavior::Access::fixtureControlWindow);
                                    result.hypothesisNotMetObserved = result.v4TextGraph->identityValid &&
                                        result.v4TextGraph->hypothesisObservationRecorded && result.v4TextGraph->hypothesisNotMet;
                                    for (const auto& reason : result.v4TextGraph->unresolved)
                                        result.obligations.push_back("v4_text_graph_observation:" + reason);
                                    if (result.v4TextGraph->observationUnavailable || !result.v4TextGraph->identityValid ||
                                        !result.v4TextGraph->unresolved.empty()) result.observationUnavailable = true;
                                }
                                catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v4_text_graph_observation_unavailable"); }
                                try
                                {
                                    result.v4UnicodeConsumption = ole_driver::InspectV4UnicodeConsumption(receipt, verified, identity,
                                        *selectedPresence, *selectedGraph, source, owner, hostBinding.first.Get(),
                                        hostBinding.second, *behavior::Access::messageTrace, behavior::Access::fixtureControlWindow);
                                    for (const auto& reason : result.v4UnicodeConsumption->unresolved)
                                        result.obligations.push_back("v4_text_unicode_consumption:" + reason);
                                    if (result.v4UnicodeConsumption->observationUnavailable || !result.v4UnicodeConsumption->localValuesCaptured ||
                                        !result.v4UnicodeConsumption->unresolved.empty()) result.observationUnavailable = true;
                                }
                                catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v4_text_unicode_consumption_observation_unavailable"); }
                                try
                                {
                                    result.v4UnicodeRefreshLink = ole_driver::InspectV4UnicodeRefreshLink(receipt, verified, identity,
                                        *selectedPresence, *selectedGraph, source, owner, hostBinding.first.Get(),
                                        hostBinding.second, *behavior::Access::messageTrace, behavior::Access::fixtureControlWindow);
                                    for (const auto& reason : result.v4UnicodeRefreshLink->unresolved)
                                        result.obligations.push_back("v4_text_unicode_refresh_link:" + reason);
                                    if (result.v4UnicodeRefreshLink->observationUnavailable || !result.v4UnicodeRefreshLink->sameDropScopeAndOrderObserved ||
                                        !result.v4UnicodeRefreshLink->unresolved.empty()) result.observationUnavailable = true;
                                }
                                catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v4_text_unicode_refresh_link_observation_unavailable"); }
                            }
                            else if (execution.productionStarted)
                            { result.observationUnavailable = true; result.obligations.emplace_back("v4_text_unicode_consumption_and_refresh_trace_unavailable"); }
                            if (execution.productionStarted)
                            {
                                try
                                {
                                    result.v4TextRefresh = observed.CaptureRefreshWindowEvidence(receipt, source, owner,
                                        hostBinding.first.Get(), hostBinding.second);
                                    for (const auto& reason : result.v4TextRefresh->unresolved)
                                        result.obligations.push_back("v4_text_refresh:" + reason);
                                    if (result.v4TextRefresh->observationUnavailable || !result.v4TextRefresh->unresolved.empty())
                                        result.observationUnavailable = true;
                                }
                                catch (const std::exception& error)
                                { result.observationUnavailable = true; result.obligations.push_back(std::string("v4_text_refresh_unavailable:") + error.what()); }
                                catch (...)
                                { result.observationUnavailable = true; result.obligations.emplace_back("v4_text_refresh_unavailable:unknown_exception"); }
                            }
                            const bool completionReturned = std::find(execution.hostStagesReturned.begin(),
                                execution.hostStagesReturned.end(), "observe_completion_or_rejection") != execution.hostStagesReturned.end();
                            if (execution.productionStarted && completionReturned && behavior::Access::messageTrace)
                            {
                                try
                                {
                                    result.v4RefreshRequest = ole_driver::V4RefreshRequestObservation::Inspect(receipt, source, owner,
                                        hostBinding.first.Get(), hostBinding.second, *behavior::Access::messageTrace,
                                        behavior::Access::fixtureControlWindow);
                                    for (const auto& reason : result.v4RefreshRequest->Unresolved())
                                        result.obligations.push_back("v4_text_refresh_request:" + reason);
                                    if (!result.v4RefreshRequest->RequestMarkerCaptured() ||
                                        result.v4RefreshRequest->ResourceOrObservationUnavailable() ||
                                        !result.v4RefreshRequest->Unresolved().empty()) result.observationUnavailable = true;
                                    if (result.v4RefreshRequest->RequestMarkerCaptured() &&
                                        (!result.v4TextRefresh || !result.v4TextRefresh->sameBoundHost ||
                                         !result.v4TextRefresh->beforeDropRevisionObserved || !result.v4TextRefresh->requestedRevisionObserved))
                                    { result.observationUnavailable = true; result.obligations.emplace_back("v4_text_refresh_request_window_revision_unavailable"); }
                                    if (result.v4RefreshRequest->RequestMarkerCaptured() && result.v4TextRefresh &&
                                        result.v4TextRefresh->sameBoundHost && result.v4TextRefresh->beforeDropRevisionObserved &&
                                        result.v4TextRefresh->requestedRevisionObserved &&
                                        (result.v4RefreshRequest->BeforeRevision() < result.v4TextRefresh->beforeDropRevision ||
                                         result.v4RefreshRequest->AfterRevision() != result.v4TextRefresh->requestedRevision))
                                    { result.observationUnavailable = true; result.obligations.emplace_back("v4_text_refresh_request_window_revision_mismatch"); }
                                }
                                catch (const std::exception& error)
                                { result.observationUnavailable = true; result.obligations.push_back(std::string("v4_text_refresh_request_unavailable:") + error.what()); }
                                catch (...)
                                { result.observationUnavailable = true; result.obligations.emplace_back("v4_text_refresh_request_unavailable:unknown_exception"); }
                            }
                            else if (execution.productionStarted)
                            { result.observationUnavailable = true; result.obligations.emplace_back("v4_text_refresh_request_completion_or_trace_unavailable"); }
                            if (execution.productionStarted && completionReturned && behavior::Access::messageTrace && result.v4RefreshRequest)
                            {
                                try
                                {
                                    result.v4RefreshWorker = ole_driver::V4RefreshWorkerObservation::Inspect(receipt, source, owner,
                                        hostBinding.first.Get(), hostBinding.second, *behavior::Access::messageTrace,
                                        behavior::Access::fixtureControlWindow, *result.v4RefreshRequest);
                                    for (const auto& reason : result.v4RefreshWorker->Unresolved())
                                        result.obligations.push_back("v4_text_refresh_worker:" + reason);
                                    if (!result.v4RefreshWorker->LocalRecordsCaptured() ||
                                        result.v4RefreshWorker->ObservationUnavailable() ||
                                        !result.v4RefreshWorker->Unresolved().empty()) result.observationUnavailable = true;
                                }
                                catch (const std::exception& error)
                                { result.observationUnavailable = true; result.obligations.push_back(std::string("v4_text_refresh_worker_unavailable:") + error.what()); }
                                catch (...)
                                { result.observationUnavailable = true; result.obligations.emplace_back("v4_text_refresh_worker_unavailable:unknown_exception"); }
                            }
                            else if (execution.productionStarted)
                            { result.observationUnavailable = true; result.obligations.emplace_back("v4_text_refresh_worker_inputs_unavailable"); }
                            result.obligations.emplace_back("v4_text_same_run_postflight_and_full_coverage_unimplemented");
                        }
                    }
                }
            }
        }
        release();
    }
    catch (const std::exception& error)
    {
        if (dispatchAttempted && !result.productionStarted) result.productionEntryUnknown = true;
        result.observationUnavailable = true;
        result.obligations.push_back(std::string("v4_text_fixture_or_observer_unavailable:") + error.what());
        release();
    }
    catch (...)
    {
        if (dispatchAttempted && !result.productionStarted) result.productionEntryUnknown = true;
        result.observationUnavailable = true;
        result.obligations.emplace_back("v4_text_fixture_or_observer_unavailable:unknown_exception");
        release();
    }
    return result;
}

inline BridgeResult ExecuteDesktopSingleText(const Context& context, const Candidate& candidate,
    const EventProgram& program, const event_graph::Variant& selectedGraph)
{
    BridgeResult result;
    const auto preflight = Inspect(context, candidate, program);
    result.obligations = preflight.obligations;
    if (!DesktopSingleTextPlan(context, candidate)) result.obligations.emplace_back("desktop_single_text_plan_unimplemented");
    if (!preflight.CanExecute()) result.obligations.emplace_back("text_complete_axis_preflight_unimplemented");
    const event_graph::Context graphContext(context.axes);
    const auto mapping = event_graph::MapSimpleExternal(graphContext, candidate, program);
    if (!mapping.graphAvailable) result.obligations.emplace_back("text_event_graph_mapping_unimplemented");
    if (!mapping.structuralExclusion.empty()) result.obligations.push_back("text_structural_exclusion:" + mapping.structuralExclusion);
    for (const auto& reason : mapping.reasons) result.obligations.push_back("text_mapping:" + reason);
    if (mapping.graphAvailable)
    {
        const auto replay = event_graph::ReplayVariant(graphContext, candidate, mapping.spec, selectedGraph.id);
        if (!replay.identityValid) result.obligations.emplace_back("text_selected_graph_identity_invalid");
        for (const auto& error : replay.errors) result.obligations.push_back("text_selected_graph:" + error);
        for (const auto& reason : replay.variant.unresolved) result.obligations.push_back("text_graph:" + reason);
    }
    // The fixed isolated Profile is not yet bound to every generated directory
    // and event axis. No content source or production scene precedes this gate.
    result.obligations.emplace_back("text_full_candidate_axis_and_graph_preflight_unimplemented");
    if (!result.obligations.empty()) return result;

    bool productionStarted = false;
    std::unique_ptr<behavior::Profile> profile;
    ole_driver::ContentSourceBinding content;
    Microsoft::WRL::ComPtr<OleDragDropAdapter> adapter;
    std::unique_ptr<behavior::GeneratedTextObservation> observed;
    const auto releaseFixture = [&] {
        // The observer holds a file handle and references to both objects.
        // GraphExecutor is synchronous and no callbacks run after it returns.
        observed.reset(); adapter.Reset(); content.source.Reset(); profile.reset();
        try { behavior::Access::ThrowAsyncException(); }
        catch (const std::exception& error)
        {
            if (!result.failure.empty()) result.failure += "; shutdown: ";
            result.failure += error.what();
            if (productionStarted) result.status = BridgeStatus::production_failed;
            else result.obligations.emplace_back("text_fixture_shutdown_failed");
        }
        catch (...)
        {
            if (!result.failure.empty()) result.failure += "; shutdown: ";
            result.failure += "unknown_text_fixture_shutdown_exception";
            if (productionStarted) result.status = BridgeStatus::production_failed;
            else result.obligations.emplace_back("text_fixture_shutdown_failed");
        }
    };
    try
    {
        profile = std::make_unique<behavior::Profile>();
        behavior::Access::EnableProductionDispatch(*profile->app, false);
        behavior::Access::EnableFixtureControlWindow(*profile->app);
        behavior::Wait([&] { return behavior::Access::FileDropIdle(*profile->app) && behavior::Access::LayoutSaveIdle(*profile->app); });
        ole_content_source_fixture::Inputs offered;
        offered.text = L"Snow \u96ea";
        content = ole_driver::PrepareContentSource(graphContext, candidate, offered);
        result.obligations.insert(result.obligations.end(), content.obligations.begin(), content.obligations.end());
        if (!content.source || !result.obligations.empty()) { releaseFixture(); return result; }
        adapter = behavior::Access::ExternalAdapter(*profile->app);
        if (!adapter) { result.obligations.emplace_back("text_production_adapter_unavailable"); releaseFixture(); return result; }
        observed = std::make_unique<behavior::GeneratedTextObservation>(*profile, *content.source.Get());
        std::vector<ole_driver::Input> inputs;
        const auto point = behavior::Access::ExternalPoint({150, 150});
        for (std::size_t i = 0; i < program.steps.size(); ++i)
        {
            const auto kind = ole_driver::ProgramKind(program.steps[i].kind);
            if (!kind) continue;
            if (*kind != ole_driver::Kind::enter && *kind != ole_driver::Kind::drop)
            { result.obligations.push_back("text_ole_step_unimplemented:" + program.steps[i].kind); continue; }
            ole_driver::Input input;
            input.programIndex = i; input.kind = *kind;
            input.modelParameter = program.steps[i].parameter;
            input.keyState = *kind == ole_driver::Kind::enter ? MK_LBUTTON : MK_CONTROL;
            input.allowedEffect = DROPEFFECT_COPY; input.screenPoint = point;
            inputs.push_back(std::move(input));
        }
        if (!result.obligations.empty()) { releaseFixture(); return result; }
        const ole_driver::HostBoundaries host{
            [&](const ole_driver::Input& event) { observed->BeforeOleCall(event); },
            [&](const ole_driver::Observation& event) { productionStarted = true; observed->ObserveOleReturn(event); },
            [&](const ole_driver::Observation& terminal) { observed->Await(terminal); },
            [&] { observed->AssertResults(); }};
        const auto execution = ole_driver::GraphExecutor::Run(graphContext, candidate, program, selectedGraph,
            inputs, adapter.Get(), content.source.Get(), host);
        productionStarted = productionStarted || execution.productionStarted;
        result.productionStarted = productionStarted;
        result.obligations.insert(result.obligations.end(), execution.obligations.begin(), execution.obligations.end());
        if (execution.status == ole_driver::Status::observation_unavailable)
        {
            result.failure = execution.failure;
            result.obligations.push_back("text_observation_unavailable:" + execution.failure);
            releaseFixture(); return result;
        }
        if (execution.status == ole_driver::Status::event_failed || execution.status == ole_driver::Status::host_observation_failed)
        {
            result.failure = execution.failure.empty() ? "text_production_event_or_observation_failed" : execution.failure;
            if (productionStarted) result.status = BridgeStatus::production_failed;
            else result.obligations.emplace_back("text_graph_execution_failed_before_production");
            releaseFixture(); return result;
        }
        if (execution.status != ole_driver::Status::attempted || execution.hostStagesReturned !=
            std::vector<std::string>{"observe_completion_or_rejection", "assert_business_results"} ||
            execution.observations.size() != inputs.size() || inputs.size() != 2)
        {
            if (productionStarted) { result.status = BridgeStatus::production_failed; result.failure = "text_required_production_observations_incomplete"; }
            else result.obligations.emplace_back("text_graph_execution_unimplemented");
            releaseFixture(); return result;
        }
        releaseFixture();
        if (result.status == BridgeStatus::production_failed || !result.failure.empty()) return result;
        result.status = BridgeStatus::production_observed;
        result.obligations.emplace_back("text_global_axis_and_graph_postflight_unimplemented");
    }
    catch (const ole_driver::ObservationUnavailable& error)
    {
        result.productionStarted = productionStarted;
        result.failure = error.what();
        result.obligations.push_back("text_observation_unavailable:" + result.failure);
        releaseFixture();
    }
    catch (const std::exception& error)
    {
        result.productionStarted = productionStarted;
        result.failure = error.what();
        if (productionStarted) result.status = BridgeStatus::production_failed;
        else result.obligations.emplace_back("text_fixture_or_execution_exception_before_production");
        releaseFixture();
    }
    catch (...)
    {
        result.productionStarted = productionStarted;
        result.failure = "unknown_text_fixture_or_execution_exception";
        if (productionStarted) result.status = BridgeStatus::production_failed;
        else result.obligations.emplace_back("text_fixture_or_execution_exception_before_production");
        releaseFixture();
    }
    return result;
}

// V4 native-container admission only: no private-pointer event, host message,
// container mutation or business assertion is synthesized here. The
// candidate remains unexecuted until a V4-owned source graph, complete
// native event graph, actual Device/WndProc route and same-run oracle bind to
// the same isolated Profile. Outcome hypotheses are never used to construct
// the source or decide whether the product should accept a drop.
struct V4NativeContainerPreflight
{
    bool candidateIdentityValid = false, narrowInputShape = false;
    std::string candidateId, sourceContainer;
    std::vector<std::string> obligations;
    bool CanDispatch() const noexcept { return false; }
};
inline V4NativeContainerPreflight InspectV4NativeContainerCandidate(
    const event_graph::V4FamilyIdentityContext& identity, const V4FamilyValidatedContext& verified)
{
    V4NativeContainerPreflight result;
    try
    {
        if (identity.Status() != V4FamilyStatus::schema_only || verified.Status() != V4CoordinateStatus::coordinate_ready ||
            identity.FamilyBinding().empty() || identity.FamilyBinding() != verified.FamilyBinding() ||
            identity.Axes().size() != verified.Axes().size())
        { result.obligations.emplace_back("v4_native_family_or_coordinate_unavailable"); return result; }
        for (std::size_t i = 0; i < identity.Axes().size(); ++i)
        {
            const auto& a = identity.Axes()[i]; const auto& b = verified.Axes()[i];
            if (a.id != b.id || a.role != b.role || a.values != b.values || a.label != b.label)
            { result.obligations.emplace_back("v4_native_complete_schema_mismatch"); return result; }
        }
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        result.obligations.insert(result.obligations.end(), raw.errors.begin(), raw.errors.end());
        if (raw.resourceUnavailable || raw.id.empty() || !raw.errors.empty())
        { result.obligations.emplace_back("v4_native_candidate_identity_unavailable"); return result; }
        result.candidateIdentityValid = true; result.candidateId = raw.id;
        const auto& axes = verified.Axes(); const auto& candidate = verified.Coordinate();
        const auto value = [&](std::string_view id) { return Value(axes, candidate, id); };
        const auto source = value("source.E02");
        result.narrowInputShape = value("T01") == "internal_native" && value("source.E01") == "free_desktop" &&
            (source == "collection" || source == "desktop_files") && value("source.E03") == "widget_body" &&
            value("target.E01") == "free_desktop" && value("target.E02") == "none" &&
            value("H01.region") == "empty_slot" && value("P01.primary") == "file" &&
            value("P01.second") == "na" && value("P02.source_actual_count") == "one" &&
            value("G01.button") == "left" && value("G04.termination") == "drop" &&
            value("X03.task_count") == "one";
        if (result.narrowInputShape) result.sourceContainer = std::string(source);
        else result.obligations.emplace_back("v4_native_container_input_shape_unimplemented");
        result.obligations.insert(result.obligations.end(), raw.unresolved.begin(), raw.unresolved.end());
        result.obligations.insert(result.obligations.end(), verified.Unresolved().begin(), verified.Unresolved().end());
        result.obligations.emplace_back("v4_native_source_member_materialization_and_identity_unimplemented");
        result.obligations.emplace_back("v4_native_candidate_device_wndproc_graph_and_selected_presence_unimplemented");
        result.obligations.emplace_back("v4_native_target_container_identity_and_geometry_unimplemented");
        result.obligations.emplace_back("v4_native_same_run_membership_files_and_layout_reload_oracle_unimplemented");
    }
    catch (const std::bad_alloc&) { result.candidateIdentityValid = false; result.candidateId.clear(); result.obligations.emplace_back("v4_native_preflight_resource_unavailable"); }
    catch (const std::length_error&) { result.candidateIdentityValid = false; result.candidateId.clear(); result.obligations.emplace_back("v4_native_preflight_resource_unavailable"); }
    return result;
}

// Preparation bridge for one Collection member, on the caller's still-live
// isolated Profile. Neither the root observation nor the fixture's public
// snapshot proves creation provenance or provides a file lifetime lease;
// the permanent blocker below remains until root/file identities and every
// axis, graph interval, completion and oracle capability are bound to it.
// The Shell source, production adapter and copy oracle below share the same
// caller-owned Profile and model-generated file. This is a restricted
// preparation path: neither the public fixture snapshot nor the planned
// callbacks prove a live source lease, all input axes, or event attribution.
inline BridgeResult PrepareV4DesktopSingleFileCopy(
    const event_graph::V4FamilyIdentityContext& identity, const V4FamilyValidatedContext& verified,
    const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
    behavior::OwnedProfileDesktopRoot& ownedProfile, const V4SingleFileFixture& fixture,
    HANDLE trustedSourceRoot, const FILE_ID_INFO& trustedSourceRootId, GridCell landing,
    const std::vector<ole_driver::Input>& inputs)
{
    BridgeResult result;
    bool dispatchAttempted = false;
    try
    {
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        const auto replay = event_graph::V4JoinedGraph::Replay(identity, verified, present, selected.id);
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty() ||
            !replay.identityValid || !replay.errors.empty() || replay.variant.id != selected.id ||
            replay.variant.classes != selected.classes || replay.variant.order != selected.order ||
            replay.variant.unresolved != selected.unresolved)
        { result.obligations.emplace_back("v4_file_candidate_or_graph_identity_unavailable"); return result; }
        const auto& axes = verified.Axes(); const auto& candidate = verified.Coordinate();
        if (Value(axes, candidate, "V4.source.kind") != "file_only" ||
            Value(axes, candidate, "T01") != "external_ingress" ||
            Value(axes, candidate, "T02.file_paths") != "yes" ||
            Value(axes, candidate, "T04.capability") != "enabled" ||
            Value(axes, candidate, "G04.termination") != "drop" ||
            Value(axes, candidate, "target.E01") != "free_desktop" ||
            !fixture.inputPrepared || !fixture.created || !fixture.fileIdentityObserved ||
            fixture.candidateId != raw.id || !ownedProfile.StillOwned() || !ownedProfile.profile ||
            !ownedProfile.profile->app || trustedSourceRoot == INVALID_HANDLE_VALUE || !trustedSourceRoot)
        { result.obligations.emplace_back("v4_file_copy_fixture_or_candidate_unavailable"); return result; }

        auto& profile = *ownedProfile.profile;
        behavior::Access::EnableProductionDispatch(*profile.app, false);
        behavior::Access::EnableFixtureControlWindow(*profile.app);
        behavior::Wait([&] { return behavior::Access::FileDropIdle(*profile.app) &&
            behavior::Access::LayoutSaveIdle(*profile.app); });
        const auto program = verified.TransferProgram();
        const auto value = [&](std::string_view id) { return Value(axes, candidate, id); };
        const auto allowed = ole_driver::EncodeAllowedEffect(value("T03"), value("T03.extra_protocol_bits"));
        std::optional<POINT> targetClient;
        for (const auto& input : inputs)
        {
            BridgeResult::V4FileOleInput item;
            item.programIndex = input.programIndex; item.kind = input.kind; item.parameter = input.modelParameter;
            item.keyState = input.keyState; item.allowedEffect = input.allowedEffect; item.screenPoint = input.screenPoint;
            if (input.programIndex < program.steps.size())
            {
                const auto& step = program.steps[input.programIndex];
                const auto kind = ole_driver::ProgramKind(step.kind);
                const auto [buttonsAxis, modifiersAxis] = ole_driver::PhaseAxes(step.kind);
                item.buttons = std::string(value(buttonsAxis)); item.modifiers = std::string(value(modifiersAxis));
                const auto keys = ole_driver::EncodeKeyState(item.buttons, item.modifiers);
                const auto parsed = ole_driver::ParseKeyParameter(step.parameter);
                item.candidateMatched = kind && *kind == input.kind && input.kind != ole_driver::Kind::leave &&
                    input.modelParameter == step.parameter && parsed && parsed->first == item.buttons &&
                    parsed->second == item.modifiers && keys.valid && !keys.altX1Ambiguous &&
                    input.keyState && *input.keyState == keys.value && allowed && input.allowedEffect &&
                    *input.allowedEffect == *allowed && input.screenPoint;
            }
            if (!item.candidateMatched) result.obligations.emplace_back("v4_file_ole_input_candidate_mismatch_or_unavailable");
            if (input.screenPoint && input.kind == ole_driver::Kind::drop)
            {
                POINT point{input.screenPoint->x, input.screenPoint->y};
                if (!behavior::Access::completionWindow || !ScreenToClient(behavior::Access::completionWindow, &point))
                    result.obligations.emplace_back("v4_file_drop_client_coordinate_unavailable");
                else if (targetClient)
                    result.obligations.emplace_back("v4_file_multiple_drop_inputs_unimplemented");
                else targetClient = point;
            }
            result.v4FileOleInputs.push_back(std::move(item));
        }
        if (!targetClient) result.obligations.emplace_back("v4_file_drop_input_coordinate_unavailable");
        else
        {
            const auto roundTrip = behavior::Access::ExternalPoint(*targetClient);
            const auto initial = behavior::Access::InspectTextDesktopInitial(*profile.app,
                profile.desktop / L"Sentinel.txt", *targetClient);
            const bool sameScreen = std::all_of(inputs.begin(), inputs.end(), [&](const auto& input) {
                return input.kind != ole_driver::Kind::leave && input.screenPoint &&
                    input.screenPoint->x == roundTrip.x && input.screenPoint->y == roundTrip.y;
            });
            const bool matches = sameScreen && value("target.E01") == "free_desktop" && value("target.E02") == "none" &&
                value("target.E19") == "named" && !initial.pageName.empty() && value("H06.page_record") == "saved" &&
                initial.pageRecorded && value("H06.page_dimensions") == "valid" && initial.savedColumns && initial.savedRows &&
                *initial.savedColumns == initial.columns && *initial.savedRows == initial.rows &&
                value("H07.occupancy") == "partial" && EndpointSize(value("target.E23")) == initial.columns &&
                EndpointSize(value("target.E24")) == initial.rows && value("H01.region") == "empty_slot" &&
                behavior::Same(initial.targetCell, landing);
            result.v4FileDesktopInput = BridgeResult::V4TextDesktopInput{
                raw.id, reinterpret_cast<std::uintptr_t>(profile.app), GetCurrentThreadId(), initial, matches};
            if (!matches) result.obligations.emplace_back("v4_file_desktop_geometry_candidate_mismatch");
        }
        const auto selectedCollection = value("C07");
        if (selectedCollection == "on" || selectedCollection == "off")
        {
            const bool actual = behavior::Access::ProgramCollectionEnabled(*profile.app);
            result.v4FileProgramCollectionInput = BridgeResult::V4TextProgramCollectionInput{
                raw.id, reinterpret_cast<std::uintptr_t>(profile.app), GetCurrentThreadId(), actual,
                actual == (selectedCollection == "on")};
            if (!result.v4FileProgramCollectionInput->candidateMatched)
                result.obligations.emplace_back("v4_file_program_collection_candidate_mismatch");
        }
        else result.obligations.emplace_back("v4_file_program_collection_candidate_unavailable");
        result.obligations.emplace_back("v4_file_desktop_visibility_and_live_routing_unverified");
        const auto destination = profile.desktop / fixture.file.filename();
        const auto observeDestination = [&]() -> std::pair<std::optional<bool>, DWORD> {
            SetLastError(ERROR_SUCCESS);
            const DWORD attributes = GetFileAttributesW(destination.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES) return {true, ERROR_SUCCESS};
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND) return {false, error};
            return {std::nullopt, error};
        };
        const auto [destinationExists, queryError] = observeDestination();
        result.v4FileConflictInput = BridgeResult::V4FileConflictInput{
            raw.id, destination, GetCurrentThreadId(), queryError, destinationExists,
            destinationExists && !*destinationExists && value("R04.member0_target_exists") == "absent" &&
                value("R04.member0_target_identity") == "na" && value("R04.member0_name_relation") == "na"};
        if (!result.v4FileConflictInput->candidateMatched)
            result.obligations.emplace_back("v4_file_conflict_candidate_or_path_unavailable");
        // A path snapshot is not a reservation and cannot prove that another
        // producer will not create the name before or during the transfer.
        result.obligations.emplace_back("v4_file_queued_reservation_and_concurrent_creation_unverified");
        auto source = ole_file_source_fixture::Make({fixture.file}, true);
        if (!source || !source->HasConstructionShell())
        { result.obligations.emplace_back("v4_file_shell_source_unavailable"); return result; }
        result.v4FileProviderInput = BridgeResult::V4FileProviderInput{
            raw.id, reinterpret_cast<std::uintptr_t>(source.Get()), source->owner, source->asynchronous,
            value("T04.capability") == "enabled" && source->asynchronous && source->owner == GetCurrentThreadId() &&
                source->starts == 0 && source->ends == 0, source->starts, source->ends};
        if (!result.v4FileProviderInput->candidateMatched)
            result.obligations.emplace_back("v4_file_async_provider_input_unavailable");
        // Source configuration is an input snapshot; it does not show that
        // the host queried the interface or started the asynchronous lease.
        result.v4FileSourcePreparation = ole_driver::InspectV4SingleFileInputs(
            verified, identity, fixture, trustedSourceRoot, trustedSourceRootId, *source.Get());
        if (!result.v4FileSourcePreparation->captured || result.v4FileSourcePreparation->resourceUnavailable)
            result.obligations.emplace_back("v4_file_source_initial_observation_unavailable");
        for (const auto& reason : result.v4FileSourcePreparation->unresolved)
            result.obligations.push_back("v4_file_source_initial:" + reason);
        auto hostBinding = behavior::Access::CreateV3AdapterAndHostEpoch(*profile.app, source.Get());
        if (!hostBinding.first || !hostBinding.second)
        { result.obligations.emplace_back("v4_file_host_adapter_or_epoch_unavailable"); return result; }
        behavior::V4GeneratedCopyObservation observed(profile, *source.Get(), fixture, std::move(landing));
        try
        {
        const auto plan = ole_driver::BuildV4JoinedOleExecutionPlan(verified, identity, present, selected, inputs);
        result.obligations.insert(result.obligations.end(), plan.preconditions.begin(), plan.preconditions.end());
        result.graphUnresolved = plan.graphUnresolved;
        result.postDispatchObligations = plan.postDispatchObligations;
        result.unclassifiedObligations = plan.unclassified;
        result.outcomeExpectations = plan.postconditions;
        result.v4PresenceExpectations = plan.presence;
        if (!plan.graphIdentityValid || !plan.structurallyPlanned || !plan.unclassified.empty())
            result.obligations.emplace_back("v4_file_graph_or_unclassified_capability_unavailable");
        for (const auto& reason : fixture.unresolved) result.obligations.push_back("v4_file_fixture:" + reason);
        result.obligations.emplace_back("v4_file_live_source_lease_and_all_candidate_inputs_unimplemented");
        result.obligations.emplace_back("v4_file_geometry_event_attribution_and_same_run_oracle_capability_unimplemented");
        result.obligations.emplace_back("v4_file_enter_and_input_event_oracle_unimplemented");
        if (!result.obligations.empty()) { result.v4CopyBusiness = observed.evidence; return result; }

        const ole_driver::HostBoundaries host{
            [&](const ole_driver::Input& event) {
                if (event.kind == ole_driver::Kind::drop)
                {
                    const auto [exists, error] = observeDestination();
                    if (!result.v4FileConflictInput || !result.v4FileConflictInput->candidateMatched ||
                        !ownedProfile.StillOwned() || !exists || *exists || error != ERROR_FILE_NOT_FOUND)
                        throw ole_driver::ObservationUnavailable("v4 file destination conflict changed before Drop");
                    result.v4FileConflictInput->checkedBeforeDrop = true;
                }
                if (event.kind == ole_driver::Kind::enter)
                {
                    if (!result.v4FileProviderInput || !result.v4FileProviderInput->candidateMatched ||
                        result.v4FileProviderInput->sourceIdentity != reinterpret_cast<std::uintptr_t>(source.Get()) ||
                        source->owner != GetCurrentThreadId() || !source->asynchronous || source->starts || source->ends)
                        throw ole_driver::ObservationUnavailable("v4 file async provider input changed before Enter");
                    result.v4FileProviderInput->checkedBeforeEnter = true;
                }
                if (!result.v4FileDesktopInput || !result.v4FileDesktopInput->candidateMatched || !targetClient ||
                    !result.v4FileProgramCollectionInput || !result.v4FileProgramCollectionInput->candidateMatched ||
                    result.v4FileDesktopInput->hostIdentity != reinterpret_cast<std::uintptr_t>(profile.app) ||
                    result.v4FileDesktopInput->ownerThread != GetCurrentThreadId() ||
                    behavior::Access::ProgramCollectionEnabled(*profile.app) != result.v4FileProgramCollectionInput->actual)
                    throw ole_driver::ObservationUnavailable("v4 file same-host input snapshot unavailable");
                const auto current = behavior::Access::InspectTextDesktopInitial(*profile.app,
                    profile.desktop / L"Sentinel.txt", *targetClient);
                const auto& before = result.v4FileDesktopInput->initial;
                if (current.pageId != before.pageId || current.pageName != before.pageName ||
                    current.sentinelKey != before.sentinelKey || current.columns != before.columns ||
                    current.rows != before.rows || current.pageRecorded != before.pageRecorded ||
                    current.savedColumns != before.savedColumns || current.savedRows != before.savedRows ||
                    current.sentinelSpan.columns != before.sentinelSpan.columns || current.sentinelSpan.rows != before.sentinelSpan.rows ||
                    current.layoutRevision != before.layoutRevision || !behavior::Same(current.sentinelCell, before.sentinelCell) ||
                    !behavior::Same(current.targetCell, before.targetCell) || !EqualRect(&current.pageBounds, &before.pageBounds) ||
                    !EqualRect(&current.targetBounds, &before.targetBounds))
                    throw ole_driver::ObservationUnavailable("v4 file desktop snapshot changed before OLE call");
                const auto point = behavior::Access::ExternalPoint(*targetClient);
                if (!event.screenPoint || event.screenPoint->x != point.x || event.screenPoint->y != point.y)
                    throw ole_driver::ObservationUnavailable("v4 file screen coordinate changed before OLE call");
                result.v4FileDesktopInput->matchingPreCallProgramIndices.push_back(event.programIndex);
                result.v4FileProgramCollectionInput->matchingPreCallProgramIndices.push_back(event.programIndex);
                observed.BeforeOleCall(event);
            },
            [&](const ole_driver::Observation&) {},
            [&](const ole_driver::Observation& terminal) { observed.Await(terminal); },
            [&] { observed.AssertResults(); }};
        const auto oracle = [](const ole_driver::Observation& event) {
            if (event.kind != ole_driver::Kind::drop) return;
            if (!event.returnedEffect) throw ole_driver::ObservationUnavailable("v4 file Drop effect unavailable");
            if (FAILED(event.result) || *event.returnedEffect != DROPEFFECT_COPY)
                throw ole_driver::EventAssertionFailure("v4 file Drop differs from the independent copy expectation");
        };
        // The diagnostic captures real task/window markers only for this
        // process-wide session. Stop it on every exit before reading any log;
        // completeness is not proof that future work has quiesced.
        if (!snowdesktop::external_transfer_observation::Enable())
            throw ole_driver::ObservationUnavailable("v4 file transfer diagnostic session unavailable");
        struct StopFileDiagnostics final
        {
            bool active = true;
            void Stop() noexcept { if (active) { snowdesktop::external_transfer_observation::Stop(); active = false; } }
            ~StopFileDiagnostics() { Stop(); }
        } diagnostics;
        dispatchAttempted = true;
        const auto receipt = ole_driver::V4FileGraphExecutor::RunBound(verified, identity, present, selected,
            inputs, fixture, trustedSourceRoot, trustedSourceRootId, *source.Get(), hostBinding.first.Get(),
            hostBinding.second, host, oracle);
        diagnostics.Stop(); result.v4DiagnosticBudget = snowdesktop::external_transfer_observation::ObserveBudget();
        const auto& execution = receipt.Execution(); result.v4OleAdapterLogLast = execution.oleAdapterLogLast;
        result.productionStarted = execution.productionStarted;
        result.productionEntryUnknown = execution.productionEntryUnknown;
        dispatchAttempted = false;
        result.businessFailureObserved = execution.businessFailureObserved;
        result.eventOracleFailureObserved = execution.eventOracleFailureObserved;
        result.eventDispatchObservationRecorded = !execution.observations.empty();
        result.eventDispatchFailureObserved = std::any_of(execution.observations.begin(), execution.observations.end(),
            [](const ole_driver::Observation& event) { return FAILED(event.result); });
        if (execution.eventOracleFailureObserved || execution.businessFailureObserved)
            result.status = BridgeStatus::production_failed;
        result.failure = execution.failure;
        result.unclassifiedObligations.insert(result.unclassifiedObligations.end(),
            execution.unclassifiedObligations.begin(), execution.unclassifiedObligations.end());
        result.postDispatchObligations.insert(result.postDispatchObligations.end(),
            execution.postconditionsPending.begin(), execution.postconditionsPending.end());
        result.obligations.insert(result.obligations.end(), execution.obligations.begin(), execution.obligations.end());
        result.v4FileSourcePreparation = receipt.SourcePreparation();
        result.v4FileSourceAtExecution = receipt.SourceAtExecution();
        result.v4OleObservations = execution.observations;
        result.v4CapturedOleReturns = receipt.CapturedReturns();
        result.v4FileSourceInterval = receipt.SourceSessionInterval();
        if (receipt.CompletionTraceCaptured()) result.v4CompletionTrace = receipt.CompletionTrace();
        result.v4CopyBusiness = observed.evidence;
        result.observationUnavailable = execution.additionalObservationUnavailable ||
            execution.status == ole_driver::Status::observation_unavailable ||
            execution.status == ole_driver::Status::host_observation_failed ||
            execution.status == ole_driver::Status::event_failed;
        if (execution.productionStarted && behavior::Access::messageTrace)
        {
            try
            {
                result.v4FilePayload = ole_driver::InspectV4FilePayload(receipt, verified, identity, present,
                    selected, *source.Get(), hostBinding.first.Get(), hostBinding.second,
                    *behavior::Access::messageTrace, behavior::Access::fixtureControlWindow);
                if (result.v4FilePayload->observationUnavailable || !result.v4FilePayload->unresolved.empty())
                    result.observationUnavailable = true;
            }
            catch (...) { result.observationUnavailable = true; }
            try
            {
                result.v4FilePathFlow = ole_driver::InspectV4FilePathFlow(receipt, verified, identity, present, selected,
                    *source.Get(), hostBinding.first.Get(), hostBinding.second, *behavior::Access::messageTrace,
                    behavior::Access::fixtureControlWindow, kShellFileOperationCompletedMessage);
                if (result.v4FilePathFlow->observationUnavailable || !result.v4FilePathFlow->unresolved.empty())
                    result.observationUnavailable = true;
            }
            catch (...) { result.observationUnavailable = true; }
            try
            {
                result.v4FileGraph = ole_driver::InspectV4FileGraph(receipt, verified, identity, present, selected,
                    *source.Get(), hostBinding.first.Get(), hostBinding.second, *behavior::Access::messageTrace,
                    behavior::Access::fixtureControlWindow, kShellFileOperationCompletedMessage);
                result.hypothesisNotMetObserved = result.v4FileGraph->identityValid &&
                    result.v4FileGraph->hypothesisObservationRecorded && result.v4FileGraph->hypothesisNotMet;
                if (result.v4FileGraph->observationUnavailable || !result.v4FileGraph->unresolved.empty())
                    result.observationUnavailable = true;
            }
            catch (...) { result.observationUnavailable = true; }
            try
            {
                result.v4FileTaskWindow = ole_driver::InspectV4FileTaskWindows(receipt, verified, identity, present,
                    selected, *source.Get(), hostBinding.first.Get(), hostBinding.second,
                    *behavior::Access::messageTrace, behavior::Access::fixtureControlWindow,
                    kShellFileOperationCompletedMessage);
                if (result.v4FileTaskWindow->observationUnavailable || !result.v4FileTaskWindow->unresolved.empty())
                    result.observationUnavailable = true;
            }
            catch (...) { result.observationUnavailable = true; }
        }
        // Complete event-graph and outcome evidence remains unimplemented;
        // even an attempted call or a successful business assertion is partial.
        if (execution.productionStarted) result.obligations.emplace_back("v4_file_full_graph_and_outcomes_unverified");
        }
        catch (...)
        {
            // Preserve all business fields already populated before a later
            // observation/allocation failed, while the observer is still live.
            try { result.v4CopyBusiness = observed.evidence; } catch (...) {}
            throw;
        }
    }
    catch (const std::exception& error)
    {
        if (dispatchAttempted && !result.productionStarted) result.productionEntryUnknown = true;
        result.observationUnavailable = true;
        if (result.failure.empty()) result.failure = error.what();
    }
    catch (...)
    {
        if (dispatchAttempted && !result.productionStarted) result.productionEntryUnknown = true;
        result.observationUnavailable = true;
        if (result.failure.empty()) result.failure = "v4_file_preparation_or_observation_exception";
    }
    return result;
}

// A real Shell source may be prepared for a modelled directory, but the file
// executor's ordinary-file snapshot must never be used for this payload. The
// typed directory receipt is wired below; current fixed obligations still
// leave this function in preparation-only mode.
// Collection ingress uses the same v4 candidate and real Shell source as the
// file executor. The observer prepares only the initial collection and reads
// production state; it cannot authorize dispatch or establish task causality.
inline BridgeResult PrepareV4CollectionSingleFileCopy(
    const event_graph::V4FamilyIdentityContext& identity, const V4FamilyValidatedContext& verified,
    const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
    behavior::OwnedProfileDesktopRoot& ownedProfile, const std::shared_ptr<V4IndependentFilePayload>& payload,
    const V4IndependentShellSource& binding, const std::shared_ptr<V4IndependentShellOwner>& sourceOwner,
    const std::vector<ole_driver::Input>& inputs,
    const std::shared_ptr<const V4PinnedCopyLease>& pinnedCopyLease = {})
{
    BridgeResult result;
    bool dispatchAttempted = false;
    try
    {
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        const auto replay = event_graph::V4JoinedGraph::Replay(identity, verified, present, selected.id);
        const auto& axes = verified.Axes();
        const auto& candidate = verified.Coordinate();
        const auto value = [&](std::string_view id) { return Value(axes, candidate, id); };
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty() || !payload ||
            payload->status != V4IndependentFileStatus::input_prepared || payload->files.size() != 1 ||
            raw.id != payload->candidateId || !sourceOwner || !binding.SameOwner(sourceOwner) || !binding.SamePayload(payload) ||
            !binding.inputPrepared || binding.resourceUnavailable || binding.candidateId != raw.id ||
            !replay.identityValid || !replay.errors.empty() || replay.variant.id != selected.id ||
            replay.variant.classes != selected.classes || replay.variant.order != selected.order ||
            replay.variant.unresolved != selected.unresolved || !ownedProfile.StillOwned() || !ownedProfile.profile ||
            !ownedProfile.profile->app || behavior::Access::v4IndependentShellOwner != sourceOwner ||
            value("V4.source.kind") != "file_only" || value("V4.source.file_count") != "one" ||
            value("V4.file.member0_kind") != "file" || value("V4.file.member0_identity") != "unique" ||
            value("T01") != "external_ingress" || value("T02.file_paths") != "yes" ||
            value("T04.capability") != "enabled" || value("G04.termination") != "drop" ||
            value("G02.enter_buttons") != "left" || value("G02.enter") != "ctrl" ||
            value("G02.hover_first") != "na" || value("G02.hover_first_buttons") != "na" ||
            value("G02.hover_later") != "na" || value("G02.hover_later_buttons") != "na" ||
            value("G02.release_buttons") != "none" || value("G02.release") != "ctrl" ||
            value("T03") != "copy_move" || value("T03.extra_protocol_bits") != "none" ||
            value("target.E01") != "free_desktop" || value("target.E02") != "collection" ||
            value("target.E03") != "widget_body" || value("H01.region") != "widget_body")
        { result.obligations.emplace_back("v4_collection_candidate_graph_or_fixture_unavailable"); return result; }

        auto& profile = *ownedProfile.profile;
        if (behavior::Access::productionDispatch != 1)
            behavior::Access::EnableProductionDispatch(*profile.app, false);
        if (!behavior::Access::fixtureControlWindow)
            behavior::Access::EnableFixtureControlWindow(*profile.app);
        behavior::Wait([&] { return behavior::Access::FileDropIdle(*profile.app) &&
            behavior::Access::LayoutSaveIdle(*profile.app); });
        auto* source = binding.SourceObject();
        if (!source || !source->HasConstructionShell())
        { result.obligations.emplace_back("v4_collection_shell_source_unavailable"); return result; }
        result.v4PinnedCopyLeaseInput.emplace();
        result.v4PinnedCopyLeaseInput->candidateId = raw.id;
        result.v4PinnedCopyLeaseInput->ownerBoundAndInitiallyObserved = pinnedCopyLease &&
            sourceOwner->MatchesPinnedCopyLease(verified, identity, payload, pinnedCopyLease);
        if (!result.v4PinnedCopyLeaseInput->ownerBoundAndInitiallyObserved)
        { result.obligations.emplace_back("v4_collection_pinned_copy_owner_lease_unavailable"); return result; }
        result.v4BatchSourcePreparation = ole_driver::InspectV4IndependentFileInputs(verified, identity, *payload, *source);
        if (!result.v4BatchSourcePreparation->captured || result.v4BatchSourcePreparation->resourceUnavailable)
        { result.obligations.emplace_back("v4_collection_source_initial_observation_unavailable"); return result; }
        for (const auto& reason : result.v4BatchSourcePreparation->unresolved)
            result.obligations.push_back("v4_collection_source_initial:" + reason);
        auto hostBinding = behavior::Access::CreateV3AdapterAndHostEpoch(*profile.app, source);
        if (!hostBinding.first || !hostBinding.second)
        { result.obligations.emplace_back("v4_collection_host_adapter_or_epoch_unavailable"); return result; }
        behavior::V4CollectionCopyObservation observed(profile, payload, binding, sourceOwner, verified, identity);
        try
        {
            result.v4CollectionIngressInput.emplace();
            auto& ingress = *result.v4CollectionIngressInput;
            ingress.candidateId = raw.id;
            bool sawEnter = false, sawDrop = false;
            std::set<std::size_t> indices;
            for (const auto& input : inputs)
            {
                if ((input.kind != ole_driver::Kind::enter && input.kind != ole_driver::Kind::drop) ||
                    !input.screenPoint || !indices.insert(input.programIndex).second ||
                    (input.kind == ole_driver::Kind::enter && sawEnter) ||
                    (input.kind == ole_driver::Kind::drop && sawDrop))
                { result.obligations.emplace_back("v4_collection_ingress_input_unavailable"); break; }
                POINT client{input.screenPoint->x, input.screenPoint->y};
                if (!ScreenToClient(behavior::Access::completionWindow, &client))
                    throw ole_driver::ObservationUnavailable("v4 collection client coordinate unavailable");
                auto geometry = behavior::Access::InspectCollectionIngress(*profile.app, client);
                if (geometry.screen.x != input.screenPoint->x || geometry.screen.y != input.screenPoint->y)
                    throw ole_driver::ObservationUnavailable("v4 collection screen coordinate changed");
                ingress.events.push_back({input.programIndex, input.kind, geometry});
                sawEnter |= input.kind == ole_driver::Kind::enter;
                sawDrop |= input.kind == ole_driver::Kind::drop;
            }
            ingress.viewportInputMatched = sawEnter && sawDrop && ingress.events.size() == 2 && inputs.size() == 2 &&
                ingress.events.front().kind == ole_driver::Kind::enter && ingress.events.back().kind == ole_driver::Kind::drop &&
                ingress.events.front().geometry.host == ingress.events.back().geometry.host &&
                ingress.events.front().geometry.container == ingress.events.back().geometry.container &&
                ingress.events.front().geometry.ownerThread == ingress.events.back().geometry.ownerThread &&
                ingress.events.front().geometry.layoutRevision == ingress.events.back().geometry.layoutRevision &&
                EqualRect(&ingress.events.front().geometry.viewport, &ingress.events.back().geometry.viewport);
            if (!ingress.viewportInputMatched) result.obligations.emplace_back("v4_collection_ingress_geometry_unavailable");
            result.obligations.emplace_back("v4_collection_widget_subregion_and_hit_test_unverified");
            const auto plan = ole_driver::BuildV4JoinedOleExecutionPlan(verified, identity, present, selected, inputs);
            result.obligations.insert(result.obligations.end(), plan.preconditions.begin(), plan.preconditions.end());
            result.graphUnresolved = plan.graphUnresolved;
            result.postDispatchObligations = plan.postDispatchObligations;
            result.unclassifiedObligations = plan.unclassified;
            result.outcomeExpectations = plan.postconditions;
            result.v4PresenceExpectations = plan.presence;
            if (!plan.graphIdentityValid || !plan.structurallyPlanned || !plan.unclassified.empty())
                result.obligations.emplace_back("v4_collection_graph_or_unclassified_capability_unavailable");
            for (const auto& reason : binding.unresolved) result.obligations.push_back("v4_collection_binding:" + reason);
            // These are capability gaps, not claims that postflight events
            // have already occurred. The file observations are non-atomic.
            result.obligations.emplace_back("v4_collection_source_snapshot_and_async_completion_unverified");
            result.obligations.emplace_back("v4_collection_candidate_coordinates_and_all_input_axes_unimplemented");
            result.obligations.emplace_back("v4_collection_read_commit_completion_and_membership_attribution_unimplemented");
            result.obligations.emplace_back("v4_collection_independent_same_run_business_oracle_capability_unimplemented");
            result.v4CollectionCopyBusiness = observed.Evidence();
            if (!result.obligations.empty()) return result;

            if (!snowdesktop::external_transfer_observation::Enable())
                throw ole_driver::ObservationUnavailable("v4 collection transfer diagnostic session unavailable");
            struct StopDiagnostics final {
                bool active = true;
                void Stop() noexcept { if (active) { snowdesktop::external_transfer_observation::Stop(); active = false; } }
                ~StopDiagnostics() { Stop(); }
            } diagnostics;
            const ole_driver::HostBoundaries host{
                [&](const ole_driver::Input& event) {
                    if (!result.v4CollectionIngressInput || !result.v4CollectionIngressInput->viewportInputMatched ||
                        result.v4CollectionIngressInput->candidateId != raw.id || !ownedProfile.StillOwned() ||
                        !binding.SameOwner(sourceOwner) || !binding.SamePayload(payload) || binding.SourceObject() != source ||
                        !pinnedCopyLease || !sourceOwner->MatchesPinnedCopyLease(verified, identity, payload, pinnedCopyLease))
                        throw ole_driver::ObservationUnavailable("v4 collection same-case input unavailable");
                    const auto& events = result.v4CollectionIngressInput->events;
                    const auto found = std::find_if(events.begin(), events.end(), [&](const auto& item) {
                        return item.programIndex == event.programIndex && item.kind == event.kind; });
                    if (found == events.end() || !event.screenPoint || event.screenPoint->x != found->geometry.screen.x ||
                        event.screenPoint->y != found->geometry.screen.y)
                        throw ole_driver::ObservationUnavailable("v4 collection event coordinate unavailable");
                    const auto now = behavior::Access::InspectCollectionIngress(*profile.app, found->geometry.client);
                    const auto& before = found->geometry;
                    if (now.host != before.host || now.container != before.container || now.ownerThread != before.ownerThread ||
                        now.screen.x != before.screen.x || now.screen.y != before.screen.y ||
                        !EqualRect(&now.viewport, &before.viewport) || now.layoutRevision != before.layoutRevision)
                        throw ole_driver::ObservationUnavailable("v4 collection geometry changed before OLE call");
                    result.v4CollectionIngressInput->matchingPreCallProgramIndices.push_back(event.programIndex);
                    result.v4PinnedCopyLeaseInput->matchingPreCallProgramIndices.push_back(event.programIndex);
                    observed.BeforeOleCall(event);
                },
                [&](const ole_driver::Observation&) {},
                [&](const ole_driver::Observation& terminal) { observed.Await(terminal); diagnostics.Stop(); result.v4DiagnosticBudget = snowdesktop::external_transfer_observation::ObserveBudget(); },
                [&] { throw ole_driver::ObservationUnavailable("v4 collection requires sealed execution observer"); }};
            // This is the constrained Copy scenario covered by the existing
            // native collection contract. Inputs and the actual target are
            // independently checked; an absent observation remains unavailable.
            const auto oracle = [&](const ole_driver::Observation& event) {
                if (event.kind != ole_driver::Kind::enter && event.kind != ole_driver::Kind::drop)
                    throw ole_driver::ObservationUnavailable("v4 collection unexpected OLE event");
                if (event.kind == ole_driver::Kind::enter &&
                    behavior::Access::Target(*profile.app) != behavior::Access::MemberContainer(*profile.app, L"ingress-root"))
                    throw ole_driver::ObservationUnavailable("v4 collection entered target unavailable");
                if (!event.returnedEffect)
                    throw ole_driver::ObservationUnavailable("v4 collection OLE effect unavailable");
                if (FAILED(event.result) || *event.returnedEffect != DROPEFFECT_COPY)
                    throw ole_driver::EventAssertionFailure("v4 collection Copy event differs from the independent expectation");
            };
            const ole_driver::V4CollectionObserverBoundaries collectionObserver{
                [&](const std::shared_ptr<ole_driver::V4CollectionExecutionKey>& key) {
                    observed.BindExecution(key, sourceOwner, hostBinding.second, ownedProfile);
                },
                [&](const std::shared_ptr<ole_driver::V4CollectionExecutionKey>& key) {
                    observed.CaptureExecution(key, sourceOwner, hostBinding.second);
                }};
            dispatchAttempted = true;
            const auto receipt = ole_driver::V4MultiFileGraphExecutor::RunBound(verified, identity, present, selected,
                inputs, payload, binding, sourceOwner, hostBinding.first.Get(), hostBinding.second, host, oracle,
                {}, {}, pinnedCopyLease, collectionObserver);
            diagnostics.Stop(); result.v4DiagnosticBudget = snowdesktop::external_transfer_observation::ObserveBudget();
            const auto& execution = receipt.Execution(); result.v4OleAdapterLogLast = execution.oleAdapterLogLast;
            result.productionStarted = execution.productionStarted;
            result.productionEntryUnknown = execution.productionEntryUnknown;
            dispatchAttempted = false;
            result.businessFailureObserved = execution.businessFailureObserved;
            result.eventOracleFailureObserved = execution.eventOracleFailureObserved;
            result.unattributedBusinessFailureObserved = execution.unattributedBusinessFailureObserved;
            result.unattributedEventOracleFailureObserved = execution.unattributedEventOracleFailureObserved;
            result.eventDispatchObservationRecorded = !execution.observations.empty();
            result.eventDispatchFailureObserved = std::any_of(execution.observations.begin(), execution.observations.end(),
                [](const ole_driver::Observation& event) { return FAILED(event.result); });
            if (result.businessFailureObserved || result.eventOracleFailureObserved) result.status = BridgeStatus::production_failed;
            result.failure = execution.failure;
            result.observationUnavailable = execution.additionalObservationUnavailable ||
                execution.status == ole_driver::Status::observation_unavailable ||
                execution.status == ole_driver::Status::host_observation_failed || execution.status == ole_driver::Status::event_failed;
            result.obligations.insert(result.obligations.end(), execution.obligations.begin(), execution.obligations.end());
            result.unclassifiedObligations.insert(result.unclassifiedObligations.end(),
                execution.unclassifiedObligations.begin(), execution.unclassifiedObligations.end());
            result.postDispatchObligations.insert(result.postDispatchObligations.end(),
                execution.postconditionsPending.begin(), execution.postconditionsPending.end());
            result.v4BatchSourcePreparation = receipt.SourcePreparation();
            result.v4BatchSourceAtExecution = receipt.SourceAtExecution();
            result.v4PinnedCopyLeaseInput->receiptPreflightObserved = receipt.PinnedCopyPreflightObserved();
            result.v4PinnedCopyLeaseInput->receiptBeforeDropObserved = receipt.PinnedCopyBeforeDropObserved();
            result.v4PinnedCopyLeaseInput->receiptAfterAwaitObserved = receipt.PinnedCopyAfterAwaitObserved();
            result.v4PinnedCopyLeaseInput->receiptSameLease = receipt.SamePinnedCopyLease(
                verified, identity, payload, sourceOwner, pinnedCopyLease);
            result.v4OleObservations = execution.observations;
            result.v4CapturedOleReturns = receipt.CapturedReturns();
            result.v4FileSourceInterval = receipt.SourceSessionInterval();
            if (receipt.CompletionTraceCaptured()) result.v4CompletionTrace = receipt.CompletionTrace();
            if (const auto sealed = receipt.SealedCollectionBusiness())
            { result.v4CollectionCopyBusiness = *sealed; result.v4CollectionBusinessSealed = true; }
            else result.v4CollectionCopyBusiness = observed.Evidence();
            if (execution.productionStarted && behavior::Access::messageTrace)
            {
                try {
                    result.v4TrackedShellCopy = ole_driver::InspectV4MultiTrackedShellCopy(receipt, verified, identity,
                        present, selected, payload, binding, sourceOwner, hostBinding.first.Get(), hostBinding.second,
                        *behavior::Access::messageTrace, behavior::Access::fixtureControlWindow, kShellFileOperationCompletedMessage);
                    if (result.v4TrackedShellCopy->observationUnavailable || result.v4TrackedShellCopy->resourceUnavailable ||
                        !result.v4TrackedShellCopy->unresolved.empty()) result.observationUnavailable = true;
                } catch (...) { result.observationUnavailable = true; }
                try {
                    result.v4FilePathFlow = ole_driver::InspectV4MultiFilePathFlow(receipt, verified, identity, present, selected,
                        payload, binding, sourceOwner, hostBinding.first.Get(), hostBinding.second, *behavior::Access::messageTrace,
                        behavior::Access::fixtureControlWindow, kShellFileOperationCompletedMessage);
                    if (result.v4FilePathFlow->observationUnavailable || !result.v4FilePathFlow->unresolved.empty())
                        result.observationUnavailable = true;
                } catch (...) { result.observationUnavailable = true; }
                try {
                    result.v4CollectionBusinessBinding = InspectV4CollectionBusiness(receipt, verified, identity, present, selected,
                        payload, binding, sourceOwner, hostBinding.first.Get(), hostBinding.second,
                        *behavior::Access::messageTrace, behavior::Access::fixtureControlWindow);
                    if (result.v4CollectionBusinessBinding->observationUnavailable ||
                        result.v4CollectionBusinessBinding->resourceUnavailable ||
                        !result.v4CollectionBusinessBinding->unresolved.empty()) result.observationUnavailable = true;
                } catch (...) { result.observationUnavailable = true; }
            }
            if (execution.productionStarted) result.obligations.emplace_back("v4_collection_full_graph_and_outcomes_unverified");
        }
        catch (...) { try { if (!result.v4CollectionBusinessSealed) result.v4CollectionCopyBusiness = observed.Evidence(); } catch (...) {} throw; }
    }
    catch (const std::exception& error)
    {
        if (dispatchAttempted && !result.productionStarted) result.productionEntryUnknown = true;
        result.observationUnavailable = true;
        if (result.failure.empty()) result.failure = error.what();
    }
    catch (...)
    {
        if (dispatchAttempted && !result.productionStarted) result.productionEntryUnknown = true;
        result.observationUnavailable = true;
    }
    return result;
}

inline BridgeResult PrepareV4DesktopFilesSingleFileCopy(
    const event_graph::V4FamilyIdentityContext& identity, const V4FamilyValidatedContext& verified,
    const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
    behavior::OwnedProfileDesktopRoot& ownedProfile, const std::shared_ptr<V4IndependentFilePayload>& payload,
    const V4IndependentShellSource& binding, const std::shared_ptr<V4IndependentShellOwner>& sourceOwner,
    const std::vector<ole_driver::Input>& inputs,
    const std::shared_ptr<const V4PinnedCopyLease>& pinnedCopyLease = {})
{
    BridgeResult result;
    bool dispatchAttempted = false;
    try
    {
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        const auto replay = event_graph::V4JoinedGraph::Replay(identity, verified, present, selected.id);
        const auto& axes = verified.Axes();
        const auto& candidate = verified.Coordinate();
        const auto value = [&](std::string_view id) { return Value(axes, candidate, id); };
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty() || !payload ||
            payload->status != V4IndependentFileStatus::input_prepared || payload->files.size() != 1 ||
            raw.id != payload->candidateId || !sourceOwner || !binding.SameOwner(sourceOwner) || !binding.SamePayload(payload) ||
            !binding.inputPrepared || binding.resourceUnavailable || binding.candidateId != raw.id ||
            !replay.identityValid || !replay.errors.empty() || replay.variant.id != selected.id ||
            replay.variant.classes != selected.classes || replay.variant.order != selected.order ||
            replay.variant.unresolved != selected.unresolved || !ownedProfile.StillOwned() || !ownedProfile.profile ||
            !ownedProfile.profile->app || behavior::Access::v4IndependentShellOwner != sourceOwner ||
            value("V4.source.kind") != "file_only" || value("V4.source.file_count") != "one" ||
            value("V4.file.member0_kind") != "file" || value("V4.file.member0_identity") != "unique" ||
            value("T01") != "external_ingress" || value("T02.file_paths") != "yes" ||
            value("T04.capability") != "enabled" || value("G04.termination") != "drop" ||
            value("G02.enter_buttons") != "left" || value("G02.enter") != "ctrl" ||
            value("G02.hover_first") != "na" || value("G02.hover_first_buttons") != "na" ||
            value("G02.hover_later") != "na" || value("G02.hover_later_buttons") != "na" ||
            value("G02.release_buttons") != "none" || value("G02.release") != "ctrl" ||
            value("T03") != "copy_move" || value("T03.extra_protocol_bits") != "none" ||
            value("target.E01") != "free_desktop" || value("target.E02") != "desktop_files" ||
            value("target.E03") != "widget_body" || value("H01.region") != "widget_body")
        { result.obligations.emplace_back("v4_desktop_files_candidate_graph_or_fixture_unavailable"); return result; }

        auto& profile = *ownedProfile.profile;
        if (behavior::Access::productionDispatch != 1)
            behavior::Access::EnableProductionDispatch(*profile.app, false);
        if (!behavior::Access::fixtureControlWindow)
            behavior::Access::EnableFixtureControlWindow(*profile.app);
        behavior::Wait([&] { return behavior::Access::FileDropIdle(*profile.app) &&
            behavior::Access::LayoutSaveIdle(*profile.app); });
        auto* source = binding.SourceObject();
        if (!source || !source->HasConstructionShell())
        { result.obligations.emplace_back("v4_desktop_files_shell_source_unavailable"); return result; }
        result.v4PinnedCopyLeaseInput.emplace();
        result.v4PinnedCopyLeaseInput->candidateId = raw.id;
        result.v4PinnedCopyLeaseInput->ownerBoundAndInitiallyObserved = pinnedCopyLease &&
            sourceOwner->MatchesPinnedCopyLease(verified, identity, payload, pinnedCopyLease);
        if (!result.v4PinnedCopyLeaseInput->ownerBoundAndInitiallyObserved)
        { result.obligations.emplace_back("v4_desktop_files_pinned_copy_owner_lease_unavailable"); return result; }
        result.v4BatchSourcePreparation = ole_driver::InspectV4IndependentFileInputs(verified, identity, *payload, *source);
        if (!result.v4BatchSourcePreparation->captured || result.v4BatchSourcePreparation->resourceUnavailable)
        { result.obligations.emplace_back("v4_desktop_files_source_initial_observation_unavailable"); return result; }
        for (const auto& reason : result.v4BatchSourcePreparation->unresolved)
            result.obligations.push_back("v4_desktop_files_source_initial:" + reason);
        auto hostBinding = behavior::Access::CreateV3AdapterAndHostEpoch(*profile.app, source);
        if (!hostBinding.first || !hostBinding.second)
        { result.obligations.emplace_back("v4_desktop_files_host_adapter_or_epoch_unavailable"); return result; }
        behavior::V4DesktopFilesCopyObservation observed(profile, payload, binding, sourceOwner, verified, identity);
        try
        {
            result.v4DesktopFilesIngressInput.emplace();
            auto& ingress = *result.v4DesktopFilesIngressInput;
            ingress.candidateId = raw.id;
            bool sawEnter = false, sawDrop = false;
            std::set<std::size_t> indices;
            for (const auto& input : inputs)
            {
                if ((input.kind != ole_driver::Kind::enter && input.kind != ole_driver::Kind::drop) ||
                    !input.screenPoint || !indices.insert(input.programIndex).second ||
                    (input.kind == ole_driver::Kind::enter && sawEnter) ||
                    (input.kind == ole_driver::Kind::drop && sawDrop))
                { result.obligations.emplace_back("v4_desktop_files_ingress_input_unavailable"); break; }
                POINT client{input.screenPoint->x, input.screenPoint->y};
                if (!ScreenToClient(behavior::Access::completionWindow, &client))
                    throw ole_driver::ObservationUnavailable("v4 desktop-files client coordinate unavailable");
                auto geometry = behavior::Access::InspectDesktopFilesIngress(*profile.app, client);
                if (geometry.screen.x != input.screenPoint->x || geometry.screen.y != input.screenPoint->y)
                    throw ole_driver::ObservationUnavailable("v4 desktop-files screen coordinate changed");
                ingress.events.push_back({input.programIndex, input.kind, geometry});
                sawEnter |= input.kind == ole_driver::Kind::enter;
                sawDrop |= input.kind == ole_driver::Kind::drop;
            }
            ingress.viewportInputMatched = sawEnter && sawDrop && ingress.events.size() == 2 && inputs.size() == 2 &&
                ingress.events.front().kind == ole_driver::Kind::enter && ingress.events.back().kind == ole_driver::Kind::drop &&
                ingress.events.front().geometry.host == ingress.events.back().geometry.host &&
                ingress.events.front().geometry.container == ingress.events.back().geometry.container &&
                ingress.events.front().geometry.ownerThread == ingress.events.back().geometry.ownerThread &&
                ingress.events.front().geometry.layoutRevision == ingress.events.back().geometry.layoutRevision &&
                EqualRect(&ingress.events.front().geometry.viewport, &ingress.events.back().geometry.viewport);
            if (!ingress.viewportInputMatched) result.obligations.emplace_back("v4_desktop_files_ingress_geometry_unavailable");
            result.obligations.emplace_back("v4_desktop_files_widget_subregion_and_hit_test_unverified");
            const auto plan = ole_driver::BuildV4JoinedOleExecutionPlan(verified, identity, present, selected, inputs);
            result.obligations.insert(result.obligations.end(), plan.preconditions.begin(), plan.preconditions.end());
            result.graphUnresolved = plan.graphUnresolved;
            result.postDispatchObligations = plan.postDispatchObligations;
            result.unclassifiedObligations = plan.unclassified;
            result.outcomeExpectations = plan.postconditions;
            result.v4PresenceExpectations = plan.presence;
            if (!plan.graphIdentityValid || !plan.structurallyPlanned || !plan.unclassified.empty())
                result.obligations.emplace_back("v4_desktop_files_graph_or_unclassified_capability_unavailable");
            for (const auto& reason : binding.unresolved) result.obligations.push_back("v4_desktop_files_binding:" + reason);
            // These are capability gaps, not claims that postflight events
            // have already occurred. The file observations are non-atomic.
            result.obligations.emplace_back("v4_desktop_files_source_snapshot_and_async_completion_unverified");
            result.obligations.emplace_back("v4_desktop_files_candidate_coordinates_and_all_input_axes_unimplemented");
            result.obligations.emplace_back("v4_desktop_files_read_commit_completion_and_membership_attribution_unimplemented");
            result.obligations.emplace_back("v4_desktop_files_independent_same_run_business_oracle_capability_unimplemented");
            result.v4DesktopFilesCopyBusiness = observed.Evidence();
            if (!result.obligations.empty()) return result;

            if (!snowdesktop::external_transfer_observation::Enable())
                throw ole_driver::ObservationUnavailable("v4 desktop-files transfer diagnostic session unavailable");
            struct StopDiagnostics final {
                bool active = true;
                void Stop() noexcept { if (active) { snowdesktop::external_transfer_observation::Stop(); active = false; } }
                ~StopDiagnostics() { Stop(); }
            } diagnostics;
            const ole_driver::HostBoundaries host{
                [&](const ole_driver::Input& event) {
                    if (!result.v4DesktopFilesIngressInput || !result.v4DesktopFilesIngressInput->viewportInputMatched ||
                        result.v4DesktopFilesIngressInput->candidateId != raw.id || !ownedProfile.StillOwned() ||
                        !binding.SameOwner(sourceOwner) || !binding.SamePayload(payload) || binding.SourceObject() != source ||
                        !pinnedCopyLease || !sourceOwner->MatchesPinnedCopyLease(verified, identity, payload, pinnedCopyLease))
                        throw ole_driver::ObservationUnavailable("v4 desktop-files same-case input unavailable");
                    const auto& events = result.v4DesktopFilesIngressInput->events;
                    const auto found = std::find_if(events.begin(), events.end(), [&](const auto& item) {
                        return item.programIndex == event.programIndex && item.kind == event.kind; });
                    if (found == events.end() || !event.screenPoint || event.screenPoint->x != found->geometry.screen.x ||
                        event.screenPoint->y != found->geometry.screen.y)
                        throw ole_driver::ObservationUnavailable("v4 desktop-files event coordinate unavailable");
                    const auto now = behavior::Access::InspectDesktopFilesIngress(*profile.app, found->geometry.client);
                    const auto& before = found->geometry;
                    if (now.host != before.host || now.container != before.container || now.ownerThread != before.ownerThread ||
                        now.screen.x != before.screen.x || now.screen.y != before.screen.y ||
                        !EqualRect(&now.viewport, &before.viewport) || now.layoutRevision != before.layoutRevision)
                        throw ole_driver::ObservationUnavailable("v4 desktop-files geometry changed before OLE call");
                    result.v4DesktopFilesIngressInput->matchingPreCallProgramIndices.push_back(event.programIndex);
                    result.v4PinnedCopyLeaseInput->matchingPreCallProgramIndices.push_back(event.programIndex);
                    observed.BeforeOleCall(event);
                },
                [&](const ole_driver::Observation&) {},
                [&](const ole_driver::Observation& terminal) { observed.Await(terminal); diagnostics.Stop(); result.v4DiagnosticBudget = snowdesktop::external_transfer_observation::ObserveBudget(); },
                [&] { throw ole_driver::ObservationUnavailable("v4 desktop-files requires sealed execution observer"); }};
            // This is the constrained Copy scenario covered by the existing
            // native desktop-files contract. Inputs and the actual target are
            // independently checked; an absent observation remains unavailable.
            const auto oracle = [&](const ole_driver::Observation& event) {
                if (event.kind != ole_driver::Kind::enter && event.kind != ole_driver::Kind::drop)
                    throw ole_driver::ObservationUnavailable("v4 desktop-files unexpected OLE event");
                if (event.kind == ole_driver::Kind::enter &&
                    behavior::Access::Target(*profile.app) != behavior::Access::MemberContainer(*profile.app, L"ingress-root"))
                    throw ole_driver::ObservationUnavailable("v4 desktop-files entered target unavailable");
                if (!event.returnedEffect)
                    throw ole_driver::ObservationUnavailable("v4 desktop-files OLE effect unavailable");
                if (FAILED(event.result) || *event.returnedEffect != DROPEFFECT_COPY)
                    throw ole_driver::EventAssertionFailure("v4 desktop-files Copy event differs from the independent expectation");
            };
            const ole_driver::V4DesktopFilesObserverBoundaries desktopFilesObserver{
                [&](const std::shared_ptr<ole_driver::V4DesktopFilesExecutionKey>& key) {
                    observed.BindExecution(key, sourceOwner, hostBinding.second, ownedProfile);
                },
                [&](const std::shared_ptr<ole_driver::V4DesktopFilesExecutionKey>& key) {
                    observed.CaptureExecution(key, sourceOwner, hostBinding.second);
                }};
            dispatchAttempted = true;
            const auto receipt = ole_driver::V4MultiFileGraphExecutor::RunBound(verified, identity, present, selected,
                inputs, payload, binding, sourceOwner, hostBinding.first.Get(), hostBinding.second, host, oracle,
                {}, {}, pinnedCopyLease, {}, {}, desktopFilesObserver);
            diagnostics.Stop(); result.v4DiagnosticBudget = snowdesktop::external_transfer_observation::ObserveBudget();
            const auto& execution = receipt.Execution(); result.v4OleAdapterLogLast = execution.oleAdapterLogLast;
            result.productionStarted = execution.productionStarted;
            result.productionEntryUnknown = execution.productionEntryUnknown;
            dispatchAttempted = false;
            result.businessFailureObserved = execution.businessFailureObserved;
            result.eventOracleFailureObserved = execution.eventOracleFailureObserved;
            result.eventDispatchObservationRecorded = !execution.observations.empty();
            result.eventDispatchFailureObserved = std::any_of(execution.observations.begin(), execution.observations.end(),
                [](const ole_driver::Observation& event) { return FAILED(event.result); });
            if (result.businessFailureObserved || result.eventOracleFailureObserved) result.status = BridgeStatus::production_failed;
            result.failure = execution.failure;
            result.observationUnavailable = execution.additionalObservationUnavailable ||
                execution.status == ole_driver::Status::observation_unavailable ||
                execution.status == ole_driver::Status::host_observation_failed || execution.status == ole_driver::Status::event_failed;
            result.obligations.insert(result.obligations.end(), execution.obligations.begin(), execution.obligations.end());
            result.unclassifiedObligations.insert(result.unclassifiedObligations.end(),
                execution.unclassifiedObligations.begin(), execution.unclassifiedObligations.end());
            result.postDispatchObligations.insert(result.postDispatchObligations.end(),
                execution.postconditionsPending.begin(), execution.postconditionsPending.end());
            result.v4BatchSourcePreparation = receipt.SourcePreparation();
            result.v4BatchSourceAtExecution = receipt.SourceAtExecution();
            result.v4PinnedCopyLeaseInput->receiptPreflightObserved = receipt.PinnedCopyPreflightObserved();
            result.v4PinnedCopyLeaseInput->receiptBeforeDropObserved = receipt.PinnedCopyBeforeDropObserved();
            result.v4PinnedCopyLeaseInput->receiptAfterAwaitObserved = receipt.PinnedCopyAfterAwaitObserved();
            result.v4PinnedCopyLeaseInput->receiptSameLease = receipt.SamePinnedCopyLease(
                verified, identity, payload, sourceOwner, pinnedCopyLease);
            result.v4OleObservations = execution.observations;
            result.v4CapturedOleReturns = receipt.CapturedReturns();
            result.v4FileSourceInterval = receipt.SourceSessionInterval();
            if (receipt.CompletionTraceCaptured()) result.v4CompletionTrace = receipt.CompletionTrace();
            if (const auto sealed = receipt.SealedDesktopFilesBusiness())
            { result.v4DesktopFilesCopyBusiness = *sealed; result.v4DesktopFilesBusinessSealed = true; }
            else result.v4DesktopFilesCopyBusiness = observed.Evidence();
            if (execution.productionStarted && behavior::Access::messageTrace)
            {
                try {
                    result.v4TrackedShellCopy = ole_driver::InspectV4MultiTrackedShellCopy(receipt, verified, identity,
                        present, selected, payload, binding, sourceOwner, hostBinding.first.Get(), hostBinding.second,
                        *behavior::Access::messageTrace, behavior::Access::fixtureControlWindow, kShellFileOperationCompletedMessage);
                    if (result.v4TrackedShellCopy->observationUnavailable || result.v4TrackedShellCopy->resourceUnavailable ||
                        !result.v4TrackedShellCopy->unresolved.empty()) result.observationUnavailable = true;
                } catch (...) { result.observationUnavailable = true; }
                try {
                    result.v4FilePathFlow = ole_driver::InspectV4MultiFilePathFlow(receipt, verified, identity, present, selected,
                        payload, binding, sourceOwner, hostBinding.first.Get(), hostBinding.second, *behavior::Access::messageTrace,
                        behavior::Access::fixtureControlWindow, kShellFileOperationCompletedMessage);
                    if (result.v4FilePathFlow->observationUnavailable || !result.v4FilePathFlow->unresolved.empty())
                        result.observationUnavailable = true;
                } catch (...) { result.observationUnavailable = true; }
                try {
                    result.v4DesktopFilesBusinessBinding = InspectV4DesktopFilesBusiness(receipt, verified, identity, present, selected,
                        payload, binding, sourceOwner, hostBinding.first.Get(), hostBinding.second,
                        *behavior::Access::messageTrace, behavior::Access::fixtureControlWindow);
                    if (result.v4DesktopFilesBusinessBinding->observationUnavailable ||
                        result.v4DesktopFilesBusinessBinding->resourceUnavailable ||
                        !result.v4DesktopFilesBusinessBinding->unresolved.empty()) result.observationUnavailable = true;
                } catch (...) { result.observationUnavailable = true; }
            }
            if (execution.productionStarted) result.obligations.emplace_back("v4_desktop_files_full_graph_and_outcomes_unverified");
        }
        catch (...) { try { if (!result.v4DesktopFilesBusinessSealed) result.v4DesktopFilesCopyBusiness = observed.Evidence(); } catch (...) {} throw; }
    }
    catch (const std::exception& error)
    {
        if (dispatchAttempted && !result.productionStarted) result.productionEntryUnknown = true;
        result.observationUnavailable = true;
        if (result.failure.empty()) result.failure = error.what();
    }
    catch (...)
    {
        if (dispatchAttempted && !result.productionStarted) result.productionEntryUnknown = true;
        result.observationUnavailable = true;
    }
    return result;
}

inline BridgeResult PrepareV4FolderMappingSingleFileCopy(
    const event_graph::V4FamilyIdentityContext& identity, const V4FamilyValidatedContext& verified,
    const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
    behavior::OwnedProfileDesktopRoot& ownedProfile, const std::shared_ptr<V4IndependentFilePayload>& payload,
    const V4IndependentShellSource& binding, const std::shared_ptr<V4IndependentShellOwner>& sourceOwner,
    const std::vector<ole_driver::Input>& inputs,
    const std::shared_ptr<const V4PinnedCopyLease>& pinnedCopyLease = {})
{
    BridgeResult result;
    bool dispatchAttempted = false;
    try
    {
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        const auto replay = event_graph::V4JoinedGraph::Replay(identity, verified, present, selected.id);
        const auto& axes = verified.Axes();
        const auto& candidate = verified.Coordinate();
        const auto value = [&](std::string_view id) { return Value(axes, candidate, id); };
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty() || !payload ||
            payload->status != V4IndependentFileStatus::input_prepared || payload->files.size() != 1 ||
            raw.id != payload->candidateId || !sourceOwner || !binding.SameOwner(sourceOwner) || !binding.SamePayload(payload) ||
            !binding.inputPrepared || binding.resourceUnavailable || binding.candidateId != raw.id ||
            !replay.identityValid || !replay.errors.empty() || replay.variant.id != selected.id ||
            replay.variant.classes != selected.classes || replay.variant.order != selected.order ||
            replay.variant.unresolved != selected.unresolved || !ownedProfile.StillOwned() || !ownedProfile.profile ||
            !ownedProfile.profile->app || behavior::Access::v4IndependentShellOwner != sourceOwner ||
            value("V4.source.kind") != "file_only" || value("V4.source.file_count") != "one" ||
            value("V4.file.member0_kind") != "file" || value("V4.file.member0_identity") != "unique" ||
            value("T01") != "external_ingress" || value("T02.file_paths") != "yes" ||
            value("T04.capability") != "enabled" || value("G04.termination") != "drop" ||
            value("G02.enter_buttons") != "left" || value("G02.enter") != "ctrl" ||
            value("G02.hover_first") != "na" || value("G02.hover_first_buttons") != "na" ||
            value("G02.hover_later") != "na" || value("G02.hover_later_buttons") != "na" ||
            value("G02.release_buttons") != "none" || value("G02.release") != "ctrl" ||
            value("T03") != "copy_move" || value("T03.extra_protocol_bits") != "none" ||
            value("target.E01") != "free_desktop" || value("target.E02") != "folder_mapping" ||
            value("target.E03") != "widget_body" || value("H01.region") != "widget_body")
        { result.obligations.emplace_back("v4_folder_mapping_candidate_graph_or_fixture_unavailable"); return result; }

        auto& profile = *ownedProfile.profile;
        if (behavior::Access::productionDispatch != 1)
            behavior::Access::EnableProductionDispatch(*profile.app, false);
        if (!behavior::Access::fixtureControlWindow)
            behavior::Access::EnableFixtureControlWindow(*profile.app);
        behavior::Wait([&] { return behavior::Access::FileDropIdle(*profile.app) &&
            behavior::Access::LayoutSaveIdle(*profile.app); });
        auto* source = binding.SourceObject();
        if (!source || !source->HasConstructionShell())
        { result.obligations.emplace_back("v4_folder_mapping_shell_source_unavailable"); return result; }
        result.v4PinnedCopyLeaseInput.emplace();
        result.v4PinnedCopyLeaseInput->candidateId = raw.id;
        result.v4PinnedCopyLeaseInput->ownerBoundAndInitiallyObserved = pinnedCopyLease &&
            sourceOwner->MatchesPinnedCopyLease(verified, identity, payload, pinnedCopyLease);
        if (!result.v4PinnedCopyLeaseInput->ownerBoundAndInitiallyObserved)
        { result.obligations.emplace_back("v4_folder_mapping_pinned_copy_owner_lease_unavailable"); return result; }
        result.v4BatchSourcePreparation = ole_driver::InspectV4IndependentFileInputs(verified, identity, *payload, *source);
        if (!result.v4BatchSourcePreparation->captured || result.v4BatchSourcePreparation->resourceUnavailable)
        { result.obligations.emplace_back("v4_folder_mapping_source_initial_observation_unavailable"); return result; }
        for (const auto& reason : result.v4BatchSourcePreparation->unresolved)
            result.obligations.push_back("v4_folder_mapping_source_initial:" + reason);
        auto hostBinding = behavior::Access::CreateV3AdapterAndHostEpoch(*profile.app, source);
        if (!hostBinding.first || !hostBinding.second)
        { result.obligations.emplace_back("v4_folder_mapping_host_adapter_or_epoch_unavailable"); return result; }
        behavior::V4FolderMappingCopyObservation observed(profile, payload, binding, sourceOwner, verified, identity);
        try
        {
            result.v4FolderMappingIngressInput.emplace();
            auto& ingress = *result.v4FolderMappingIngressInput;
            ingress.candidateId = raw.id;
            bool sawEnter = false, sawDrop = false;
            std::set<std::size_t> indices;
            for (const auto& input : inputs)
            {
                if ((input.kind != ole_driver::Kind::enter && input.kind != ole_driver::Kind::drop) ||
                    !input.screenPoint || !indices.insert(input.programIndex).second ||
                    (input.kind == ole_driver::Kind::enter && sawEnter) ||
                    (input.kind == ole_driver::Kind::drop && sawDrop))
                { result.obligations.emplace_back("v4_folder_mapping_ingress_input_unavailable"); break; }
                POINT client{input.screenPoint->x, input.screenPoint->y};
                if (!ScreenToClient(behavior::Access::completionWindow, &client))
                    throw ole_driver::ObservationUnavailable("v4 folder-mapping client coordinate unavailable");
                auto geometry = behavior::Access::InspectFolderMappingIngress(*profile.app, client);
                if (geometry.screen.x != input.screenPoint->x || geometry.screen.y != input.screenPoint->y)
                    throw ole_driver::ObservationUnavailable("v4 folder-mapping screen coordinate changed");
                ingress.events.push_back({input.programIndex, input.kind, geometry});
                sawEnter |= input.kind == ole_driver::Kind::enter;
                sawDrop |= input.kind == ole_driver::Kind::drop;
            }
            ingress.viewportInputMatched = sawEnter && sawDrop && ingress.events.size() == 2 && inputs.size() == 2 &&
                ingress.events.front().kind == ole_driver::Kind::enter && ingress.events.back().kind == ole_driver::Kind::drop &&
                ingress.events.front().geometry.host == ingress.events.back().geometry.host &&
                ingress.events.front().geometry.container == ingress.events.back().geometry.container &&
                ingress.events.front().geometry.ownerThread == ingress.events.back().geometry.ownerThread &&
                ingress.events.front().geometry.layoutRevision == ingress.events.back().geometry.layoutRevision &&
                EqualRect(&ingress.events.front().geometry.viewport, &ingress.events.back().geometry.viewport);
            if (!ingress.viewportInputMatched) result.obligations.emplace_back("v4_folder_mapping_ingress_geometry_unavailable");
            result.obligations.emplace_back("v4_folder_mapping_widget_subregion_and_hit_test_unverified");
            const auto plan = ole_driver::BuildV4JoinedOleExecutionPlan(verified, identity, present, selected, inputs);
            result.obligations.insert(result.obligations.end(), plan.preconditions.begin(), plan.preconditions.end());
            result.graphUnresolved = plan.graphUnresolved;
            result.postDispatchObligations = plan.postDispatchObligations;
            result.unclassifiedObligations = plan.unclassified;
            result.outcomeExpectations = plan.postconditions;
            result.v4PresenceExpectations = plan.presence;
            if (!plan.graphIdentityValid || !plan.structurallyPlanned || !plan.unclassified.empty())
                result.obligations.emplace_back("v4_folder_mapping_graph_or_unclassified_capability_unavailable");
            for (const auto& reason : binding.unresolved) result.obligations.push_back("v4_folder_mapping_binding:" + reason);
            // These are capability gaps, not claims that postflight events
            // have already occurred. The file observations are non-atomic.
            result.obligations.emplace_back("v4_folder_mapping_source_snapshot_and_async_completion_unverified");
            result.obligations.emplace_back("v4_folder_mapping_candidate_coordinates_and_all_input_axes_unimplemented");
            result.obligations.emplace_back("v4_folder_mapping_read_commit_completion_and_membership_attribution_unimplemented");
            result.obligations.emplace_back("v4_folder_mapping_independent_same_run_business_oracle_capability_unimplemented");
            result.v4FolderMappingCopyBusiness = observed.Evidence();
            if (!result.obligations.empty()) return result;

            if (!snowdesktop::external_transfer_observation::Enable())
                throw ole_driver::ObservationUnavailable("v4 folder-mapping transfer diagnostic session unavailable");
            struct StopDiagnostics final {
                bool active = true;
                void Stop() noexcept { if (active) { snowdesktop::external_transfer_observation::Stop(); active = false; } }
                ~StopDiagnostics() { Stop(); }
            } diagnostics;
            const ole_driver::HostBoundaries host{
                [&](const ole_driver::Input& event) {
                    if (!result.v4FolderMappingIngressInput || !result.v4FolderMappingIngressInput->viewportInputMatched ||
                        result.v4FolderMappingIngressInput->candidateId != raw.id || !ownedProfile.StillOwned() ||
                        !binding.SameOwner(sourceOwner) || !binding.SamePayload(payload) || binding.SourceObject() != source ||
                        !pinnedCopyLease || !sourceOwner->MatchesPinnedCopyLease(verified, identity, payload, pinnedCopyLease))
                        throw ole_driver::ObservationUnavailable("v4 folder-mapping same-case input unavailable");
                    const auto& events = result.v4FolderMappingIngressInput->events;
                    const auto found = std::find_if(events.begin(), events.end(), [&](const auto& item) {
                        return item.programIndex == event.programIndex && item.kind == event.kind; });
                    if (found == events.end() || !event.screenPoint || event.screenPoint->x != found->geometry.screen.x ||
                        event.screenPoint->y != found->geometry.screen.y)
                        throw ole_driver::ObservationUnavailable("v4 folder-mapping event coordinate unavailable");
                    const auto now = behavior::Access::InspectFolderMappingIngress(*profile.app, found->geometry.client);
                    const auto& before = found->geometry;
                    if (now.host != before.host || now.container != before.container || now.ownerThread != before.ownerThread ||
                        now.screen.x != before.screen.x || now.screen.y != before.screen.y ||
                        !EqualRect(&now.viewport, &before.viewport) || now.layoutRevision != before.layoutRevision)
                        throw ole_driver::ObservationUnavailable("v4 folder-mapping geometry changed before OLE call");
                    result.v4FolderMappingIngressInput->matchingPreCallProgramIndices.push_back(event.programIndex);
                    result.v4PinnedCopyLeaseInput->matchingPreCallProgramIndices.push_back(event.programIndex);
                    observed.BeforeOleCall(event);
                },
                [&](const ole_driver::Observation&) {},
                [&](const ole_driver::Observation& terminal) { observed.Await(terminal); diagnostics.Stop(); result.v4DiagnosticBudget = snowdesktop::external_transfer_observation::ObserveBudget(); },
                [&] { throw ole_driver::ObservationUnavailable("v4 folder-mapping requires sealed execution observer"); }};
            // This is the constrained Copy scenario covered by the existing
            // native folder-mapping contract. Inputs and the actual target are
            // independently checked; an absent observation remains unavailable.
            const auto oracle = [&](const ole_driver::Observation& event) {
                if (event.kind != ole_driver::Kind::enter && event.kind != ole_driver::Kind::drop)
                    throw ole_driver::ObservationUnavailable("v4 folder-mapping unexpected OLE event");
                if (event.kind == ole_driver::Kind::enter &&
                    behavior::Access::Target(*profile.app) != behavior::Access::MemberContainer(*profile.app, L"ingress-root"))
                    throw ole_driver::ObservationUnavailable("v4 folder-mapping entered target unavailable");
                if (!event.returnedEffect)
                    throw ole_driver::ObservationUnavailable("v4 folder-mapping OLE effect unavailable");
                if (FAILED(event.result) || *event.returnedEffect != DROPEFFECT_COPY)
                    throw ole_driver::EventAssertionFailure("v4 folder-mapping Copy event differs from the independent expectation");
            };
            const ole_driver::V4FolderMappingObserverBoundaries folderMappingObserver{
                [&](const std::shared_ptr<ole_driver::V4FolderMappingExecutionKey>& key) {
                    observed.BindExecution(key, sourceOwner, hostBinding.second, ownedProfile);
                },
                [&](const std::shared_ptr<ole_driver::V4FolderMappingExecutionKey>& key) {
                    observed.CaptureExecution(key, sourceOwner, hostBinding.second);
                }};
            dispatchAttempted = true;
            const auto receipt = ole_driver::V4MultiFileGraphExecutor::RunBound(verified, identity, present, selected,
                inputs, payload, binding, sourceOwner, hostBinding.first.Get(), hostBinding.second, host, oracle,
                {}, {}, pinnedCopyLease, {}, {}, {}, folderMappingObserver);
            diagnostics.Stop(); result.v4DiagnosticBudget = snowdesktop::external_transfer_observation::ObserveBudget();
            const auto& execution = receipt.Execution(); result.v4OleAdapterLogLast = execution.oleAdapterLogLast;
            result.productionStarted = execution.productionStarted;
            result.productionEntryUnknown = execution.productionEntryUnknown;
            dispatchAttempted = false;
            result.businessFailureObserved = execution.businessFailureObserved;
            result.eventOracleFailureObserved = execution.eventOracleFailureObserved;
            result.eventDispatchObservationRecorded = !execution.observations.empty();
            result.eventDispatchFailureObserved = std::any_of(execution.observations.begin(), execution.observations.end(),
                [](const ole_driver::Observation& event) { return FAILED(event.result); });
            if (result.businessFailureObserved || result.eventOracleFailureObserved) result.status = BridgeStatus::production_failed;
            result.failure = execution.failure;
            result.observationUnavailable = execution.additionalObservationUnavailable ||
                execution.status == ole_driver::Status::observation_unavailable ||
                execution.status == ole_driver::Status::host_observation_failed || execution.status == ole_driver::Status::event_failed;
            result.obligations.insert(result.obligations.end(), execution.obligations.begin(), execution.obligations.end());
            result.unclassifiedObligations.insert(result.unclassifiedObligations.end(),
                execution.unclassifiedObligations.begin(), execution.unclassifiedObligations.end());
            result.postDispatchObligations.insert(result.postDispatchObligations.end(),
                execution.postconditionsPending.begin(), execution.postconditionsPending.end());
            result.v4BatchSourcePreparation = receipt.SourcePreparation();
            result.v4BatchSourceAtExecution = receipt.SourceAtExecution();
            result.v4PinnedCopyLeaseInput->receiptPreflightObserved = receipt.PinnedCopyPreflightObserved();
            result.v4PinnedCopyLeaseInput->receiptBeforeDropObserved = receipt.PinnedCopyBeforeDropObserved();
            result.v4PinnedCopyLeaseInput->receiptAfterAwaitObserved = receipt.PinnedCopyAfterAwaitObserved();
            result.v4PinnedCopyLeaseInput->receiptSameLease = receipt.SamePinnedCopyLease(
                verified, identity, payload, sourceOwner, pinnedCopyLease);
            result.v4OleObservations = execution.observations;
            result.v4CapturedOleReturns = receipt.CapturedReturns();
            result.v4FileSourceInterval = receipt.SourceSessionInterval();
            if (receipt.CompletionTraceCaptured()) result.v4CompletionTrace = receipt.CompletionTrace();
            if (const auto sealed = receipt.SealedFolderMappingBusiness())
            { result.v4FolderMappingCopyBusiness = *sealed; result.v4FolderMappingBusinessSealed = true; }
            else result.v4FolderMappingCopyBusiness = observed.Evidence();
            if (execution.productionStarted && behavior::Access::messageTrace)
            {
                try {
                    result.v4TrackedShellCopy = ole_driver::InspectV4MultiTrackedShellCopy(receipt, verified, identity,
                        present, selected, payload, binding, sourceOwner, hostBinding.first.Get(), hostBinding.second,
                        *behavior::Access::messageTrace, behavior::Access::fixtureControlWindow, kShellFileOperationCompletedMessage);
                    if (result.v4TrackedShellCopy->observationUnavailable || result.v4TrackedShellCopy->resourceUnavailable ||
                        !result.v4TrackedShellCopy->unresolved.empty()) result.observationUnavailable = true;
                } catch (...) { result.observationUnavailable = true; }
                try {
                    result.v4FilePathFlow = ole_driver::InspectV4MultiFilePathFlow(receipt, verified, identity, present, selected,
                        payload, binding, sourceOwner, hostBinding.first.Get(), hostBinding.second, *behavior::Access::messageTrace,
                        behavior::Access::fixtureControlWindow, kShellFileOperationCompletedMessage);
                    if (result.v4FilePathFlow->observationUnavailable || !result.v4FilePathFlow->unresolved.empty())
                        result.observationUnavailable = true;
                } catch (...) { result.observationUnavailable = true; }
                try {
                    result.v4FolderMappingBusinessBinding = InspectV4FolderMappingBusiness(receipt, verified, identity, present, selected,
                        payload, binding, sourceOwner, hostBinding.first.Get(), hostBinding.second,
                        *behavior::Access::messageTrace, behavior::Access::fixtureControlWindow);
                    if (result.v4FolderMappingBusinessBinding->observationUnavailable ||
                        result.v4FolderMappingBusinessBinding->resourceUnavailable ||
                        !result.v4FolderMappingBusinessBinding->unresolved.empty()) result.observationUnavailable = true;
                } catch (...) { result.observationUnavailable = true; }
            }
            if (execution.productionStarted) result.obligations.emplace_back("v4_folder_mapping_full_graph_and_outcomes_unverified");
        }
        catch (...) { try { if (!result.v4FolderMappingBusinessSealed) result.v4FolderMappingCopyBusiness = observed.Evidence(); } catch (...) {} throw; }
    }
    catch (const std::exception& error)
    {
        if (dispatchAttempted && !result.productionStarted) result.productionEntryUnknown = true;
        result.observationUnavailable = true;
        if (result.failure.empty()) result.failure = error.what();
    }
    catch (...)
    {
        if (dispatchAttempted && !result.productionStarted) result.productionEntryUnknown = true;
        result.observationUnavailable = true;
    }
    return result;
}

// Restricted group ingress. Root and active child remain separate identities.
inline BridgeResult PrepareV4GroupSingleFileCopy(
    const event_graph::V4FamilyIdentityContext& identity, const V4FamilyValidatedContext& verified,
    const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
    behavior::OwnedProfileDesktopRoot& ownedProfile, const std::shared_ptr<V4IndependentFilePayload>& payload,
    const V4IndependentShellSource& binding, const std::shared_ptr<V4IndependentShellOwner>& sourceOwner,
    const std::vector<ole_driver::Input>& inputs, const std::shared_ptr<const V4PinnedCopyLease>& pinnedCopyLease,
    std::size_t groupChildBudget = 128, std::size_t groupWidgetBudget = 256, std::size_t groupUtf16ByteBudget = 65536)
{
    BridgeResult result; bool dispatchAttempted = false;
    try
    {
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        const auto replay = event_graph::V4JoinedGraph::Replay(identity, verified, present, selected.id);
        const auto value = [&](std::string_view id) { return Value(verified.Axes(), verified.Coordinate(), id); };
        const bool mappedGroup = value("target.E02") == "file_group" && value("target.E08.active_child_type") == "folder_mapping";
        const bool fileGroup = value("target.E02") == "file_group" &&
            (value("target.E08.active_child_type") == "desktop_files" || mappedGroup);
        const bool collectionGroup = value("target.E02") == "collection_group" && value("target.E08.active_child_type") == "collection";
        const bool inactiveCollectionTab = collectionGroup && value("target.E08.hit_relation") == "inactive_valid" &&
            value("target.E08.hit_position") == "last_child";
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty() || !payload || payload->files.size() != 1 ||
            payload->status != V4IndependentFileStatus::input_prepared || payload->candidateId != raw.id || !sourceOwner ||
            !binding.inputPrepared || binding.resourceUnavailable || binding.candidateId != raw.id ||
            !binding.SameOwner(sourceOwner) || !binding.SamePayload(payload) || !ownedProfile.StillOwned() ||
            !ownedProfile.profile || !ownedProfile.profile->app || behavior::Access::v4IndependentShellOwner != sourceOwner ||
            !replay.identityValid || !replay.errors.empty() || replay.variant.id != selected.id ||
            replay.variant.classes != selected.classes || replay.variant.order != selected.order || replay.variant.unresolved != selected.unresolved ||
            value("V4.source.kind") != "file_only" || value("V4.source.file_count") != "one" ||
            value("V4.file.member0_kind") != "file" || value("V4.file.member0_identity") != "unique" ||
            value("T01") != "external_ingress" || value("T02.file_paths") != "yes" || value("T04.capability") != "enabled" ||
            value("G02.enter_buttons") != "left" || value("G02.enter") != "ctrl" ||
            value("G02.hover_first") != "na" || value("G02.hover_first_buttons") != "na" ||
            value("G02.hover_later") != "na" || value("G02.hover_later_buttons") != "na" ||
            value("G02.release_buttons") != "none" || value("G02.release") != "ctrl" || value("G04.termination") != "drop" ||
            value("T03") != "copy_move" || value("T03.extra_protocol_bits") != "none" ||
            value("target.E01") != "free_desktop" || (!fileGroup && !collectionGroup) ||
            value("target.E03") != (inactiveCollectionTab ? "group_label" : "widget_body") ||
            value("H01.region") != (inactiveCollectionTab ? "tab" : "widget_body") ||
            value("target.E08") != "first_child" ||
            (!inactiveCollectionTab && (value("target.E08.hit_relation") != "active" || value("target.E08.hit_position") != "first_child")))
        { result.obligations.emplace_back("v4_group_candidate_graph_or_source_unavailable"); return result; }
        auto& profile = *ownedProfile.profile;
        if (behavior::Access::productionDispatch != 1) behavior::Access::EnableProductionDispatch(*profile.app, false);
        if (!behavior::Access::fixtureControlWindow) behavior::Access::EnableFixtureControlWindow(*profile.app);
        behavior::Wait([&] { return behavior::Access::FileDropIdle(*profile.app) && behavior::Access::LayoutSaveIdle(*profile.app); });
        auto* source = binding.SourceObject();
        if (!source || !source->HasConstructionShell()) { result.obligations.emplace_back("v4_group_source_unavailable"); return result; }
        result.v4PinnedCopyLeaseInput.emplace(); result.v4PinnedCopyLeaseInput->candidateId = raw.id;
        result.v4PinnedCopyLeaseInput->ownerBoundAndInitiallyObserved = pinnedCopyLease &&
            sourceOwner->MatchesPinnedCopyLease(verified, identity, payload, pinnedCopyLease);
        if (!result.v4PinnedCopyLeaseInput->ownerBoundAndInitiallyObserved)
        { result.obligations.emplace_back("v4_group_pinned_copy_lease_unavailable"); return result; }
        result.v4BatchSourcePreparation = ole_driver::InspectV4IndependentFileInputs(verified, identity, *payload, *source);
        if (!result.v4BatchSourcePreparation->captured || result.v4BatchSourcePreparation->resourceUnavailable)
        { result.obligations.emplace_back("v4_group_source_snapshot_unavailable"); return result; }
        for (const auto& reason : result.v4BatchSourcePreparation->unresolved) result.obligations.push_back("v4_group_source:" + reason);
        auto hostBinding = behavior::Access::CreateV3AdapterAndHostEpoch(*profile.app, source);
        if (!hostBinding.first || !hostBinding.second) { result.obligations.emplace_back("v4_group_host_epoch_unavailable"); return result; }
        behavior::V4GroupCopyObservation observed(profile, payload, binding, sourceOwner, verified, identity);
        result.v4GroupCopyBusiness = observed.Evidence();
        std::vector<ole_driver::Input> effectiveInputs = inputs;
        bool generatedTabInputs = false;
        if (inactiveCollectionTab && effectiveInputs.empty())
        {
            bool resource = false;
            auto tab = behavior::Access::InspectV4GroupTabIngress(*profile.app, L"ingress-root", L"ingress-inactive",
                groupChildBudget, groupWidgetBudget, groupUtf16ByteBudget, resource);
            if (!tab || !ownedProfile.StillOwned())
            { result.obligations.emplace_back(resource ? "v4_group_tab_input_resource_unavailable" : "v4_group_tab_input_unavailable"); return result; }
            const auto program = verified.TransferProgram();
            const auto allowed = ole_driver::EncodeAllowedEffect(value("T03"), value("T03.extra_protocol_bits"));
            bool entered = false, dropped = false, completed = false, business = false;
            if (!allowed) { result.obligations.emplace_back("v4_group_tab_effect_unavailable"); return result; }
            for (std::size_t i = 0; i < program.steps.size(); ++i)
            {
                const auto& step = program.steps[i]; const auto kind = ole_driver::ProgramKind(step.kind);
                if (!kind)
                {
                    if (step.kind == "configure_v4_source_input" && !entered) continue;
                    if (step.kind == "observe_completion_or_rejection" && dropped && !completed && !business) { completed = true; continue; }
                    if (step.kind == "assert_business_results" && completed && !business) { business = true; continue; }
                    result.obligations.emplace_back("v4_group_tab_program_stage_unavailable"); return result;
                }
                if ((*kind != ole_driver::Kind::enter && *kind != ole_driver::Kind::drop) ||
                    (*kind == ole_driver::Kind::enter && entered) || (*kind == ole_driver::Kind::drop && (!entered || dropped)))
                { result.obligations.emplace_back("v4_group_tab_program_motion_unavailable"); return result; }
                const auto [buttonAxis, modifierAxis] = ole_driver::PhaseAxes(step.kind);
                const auto buttons = value(buttonAxis), modifiers = value(modifierAxis);
                const auto parsed = ole_driver::ParseKeyParameter(step.parameter);
                const auto keys = ole_driver::EncodeKeyState(buttons, modifiers);
                if (!parsed || parsed->first != buttons || parsed->second != modifiers || !keys.valid || keys.altX1Ambiguous)
                { result.obligations.emplace_back("v4_group_tab_program_keys_unavailable"); return result; }
                ole_driver::Input input; input.programIndex = i; input.kind = *kind; input.modelParameter = step.parameter;
                input.keyState = keys.value; input.allowedEffect = *allowed; input.screenPoint = POINTL{tab->screen.x, tab->screen.y};
                effectiveInputs.push_back(std::move(input));
                if (*kind == ole_driver::Kind::enter) entered = true; else dropped = true;
            }
            if (!entered || !dropped || !completed || !business || effectiveInputs.size() != 2)
            { result.obligations.emplace_back("v4_group_tab_program_incomplete"); return result; }
            generatedTabInputs = true;
        }
        result.v4GroupAxisInput = CaptureV4GroupAxisInput(identity, verified, *profile.app,
            groupChildBudget, groupWidgetBudget, groupUtf16ByteBudget);
        result.v4GroupAxisInput->sameHostBindingObserved = ownedProfile.StillOwned() && profile.app &&
            behavior::Access::v3HostEpoch.lock() == hostBinding.second && behavior::Access::asyncOwner == profile.app &&
            binding.SameOwner(sourceOwner) && binding.SamePayload(payload) && binding.SourceObject() == source &&
            pinnedCopyLease && sourceOwner->MatchesPinnedCopyLease(verified, identity, payload, pinnedCopyLease) &&
            result.v4GroupAxisInput->actualTarget &&
            result.v4GroupAxisInput->actualTarget->host == reinterpret_cast<std::uintptr_t>(profile.app);
        // Child configuration is a local pre-input snapshot. Production hit,
        // source-side group identity and asynchronous activation remain gated.
        result.observationUnavailable = true;
        result.obligations.emplace_back("v4_group_ordered_children_actual_hit_and_activation_unverified");
        const auto inspectGroup = [&](POINT client) {
            return mappedGroup ? behavior::Access::InspectFileGroupMappingIngress(*profile.app, client) :
                fileGroup ? behavior::Access::InspectFileGroupIngress(*profile.app, client) :
                behavior::Access::InspectGroupIngress(*profile.app, client);
        };
        try
        {
            result.v4GroupIngressInput.emplace(); auto& ingress = *result.v4GroupIngressInput; ingress.candidateId = raw.id;
            ingress.generatedFromModel = generatedTabInputs;
            bool sawEnter = false, sawDrop = false; std::set<std::size_t> indices;
            for (const auto& input : effectiveInputs)
            {
                if ((input.kind != ole_driver::Kind::enter && input.kind != ole_driver::Kind::drop) || !input.screenPoint ||
                    !indices.insert(input.programIndex).second || (input.kind == ole_driver::Kind::enter && sawEnter) ||
                    (input.kind == ole_driver::Kind::drop && sawDrop)) break;
                POINT client{input.screenPoint->x, input.screenPoint->y};
                if (!ScreenToClient(behavior::Access::completionWindow, &client))
                    throw ole_driver::ObservationUnavailable("v4 group client coordinate unavailable");
                if (inactiveCollectionTab)
                {
                    bool resource = false;
                    auto tab = behavior::Access::InspectV4GroupTabIngress(*profile.app, L"ingress-root", L"ingress-inactive",
                        groupChildBudget, groupWidgetBudget, groupUtf16ByteBudget, resource);
                    ingress.resourceUnavailable |= resource;
                    if (!tab || tab->screen.x != input.screenPoint->x || tab->screen.y != input.screenPoint->y ||
                        tab->client.x != client.x || tab->client.y != client.y || tab->group.activeId != L"ingress-active")
                        throw ole_driver::ObservationUnavailable("v4 inactive group tab input unavailable");
                    ingress.tabEvents.push_back({input.programIndex, input.kind, std::move(*tab)});
                    sawEnter |= input.kind == ole_driver::Kind::enter; sawDrop |= input.kind == ole_driver::Kind::drop;
                    continue;
                }
                auto geometry = inspectGroup(client);
                if (geometry.screen.x != input.screenPoint->x || geometry.screen.y != input.screenPoint->y)
                    throw ole_driver::ObservationUnavailable("v4 group screen coordinate changed");
                ingress.events.push_back({input.programIndex, input.kind, geometry});
                sawEnter |= input.kind == ole_driver::Kind::enter; sawDrop |= input.kind == ole_driver::Kind::drop;
            }
            if (sawEnter && sawDrop && ingress.events.size() == 2 && effectiveInputs.size() == 2 &&
                ingress.events.front().kind == ole_driver::Kind::enter && ingress.events.back().kind == ole_driver::Kind::drop)
            {
                const auto& a = ingress.events.front().geometry; const auto& b = ingress.events.back().geometry;
                ingress.viewportInputMatched = a.host == b.host && a.group == b.group && a.activeWidget == b.activeWidget &&
                    a.activeId == b.activeId && a.ownerThread == b.ownerThread && a.layoutRevision == b.layoutRevision && EqualRect(&a.viewport, &b.viewport);
            }
            if (inactiveCollectionTab && sawEnter && sawDrop && ingress.tabEvents.size() == 2 && effectiveInputs.size() == 2 &&
                ingress.tabEvents.front().kind == ole_driver::Kind::enter && ingress.tabEvents.back().kind == ole_driver::Kind::drop)
            {
                const auto& a = ingress.tabEvents.front().geometry; const auto& b = ingress.tabEvents.back().geometry;
                ingress.inactiveTabInputMatched = a.group.host == b.group.host && a.group.group == b.group.group &&
                    a.group.groupData == b.group.groupData && a.group.ownerThread == b.group.ownerThread &&
                    a.group.activeId == b.group.activeId && a.group.layoutRevision == b.group.layoutRevision &&
                    a.child == b.child && a.childId == b.childId && EqualRect(&a.tab, &b.tab) &&
                    a.screen.x == b.screen.x && a.screen.y == b.screen.y;
            }
            if (!(inactiveCollectionTab ? ingress.inactiveTabInputMatched : ingress.viewportInputMatched))
                result.obligations.emplace_back("v4_group_ingress_geometry_unavailable");
            const auto plan = ole_driver::BuildV4JoinedOleExecutionPlan(verified, identity, present, selected, effectiveInputs);
            result.obligations.insert(result.obligations.end(), plan.preconditions.begin(), plan.preconditions.end());
            result.graphUnresolved = plan.graphUnresolved; result.postDispatchObligations = plan.postDispatchObligations;
            result.unclassifiedObligations = plan.unclassified; result.outcomeExpectations = plan.postconditions; result.v4PresenceExpectations = plan.presence;
            if (!plan.graphIdentityValid || !plan.structurallyPlanned || !plan.unclassified.empty())
                result.obligations.emplace_back("v4_group_graph_or_unclassified_capability_unavailable");
            for (const auto& reason : binding.unresolved) result.obligations.push_back("v4_group_binding:" + reason);
            result.obligations.emplace_back("v4_group_widget_subregion_and_hit_test_unverified");
            result.obligations.emplace_back("v4_group_all_axes_and_source_snapshot_unverified");
            result.obligations.emplace_back("v4_group_read_commit_completion_and_membership_causality_unverified");
            result.obligations.emplace_back("v4_group_independent_same_run_business_oracle_capability_unimplemented");
            result.v4GroupCopyBusiness = observed.Evidence();
            if (!result.obligations.empty()) return result;
            if (!snowdesktop::external_transfer_observation::Enable())
                throw ole_driver::ObservationUnavailable("v4 group transfer diagnostics unavailable");
            struct StopDiagnostics { bool active = true; void Stop() noexcept { if (active) { snowdesktop::external_transfer_observation::Stop(); active = false; } }
                ~StopDiagnostics() { Stop(); } } diagnostics;
            const ole_driver::HostBoundaries host{
                [&](const ole_driver::Input& event) {
                    if (!result.v4GroupIngressInput || !(inactiveCollectionTab ? result.v4GroupIngressInput->inactiveTabInputMatched :
                        result.v4GroupIngressInput->viewportInputMatched) ||
                        result.v4GroupIngressInput->candidateId != raw.id || !ownedProfile.StillOwned() ||
                        !binding.SameOwner(sourceOwner) || !binding.SamePayload(payload) || binding.SourceObject() != source ||
                        !pinnedCopyLease || !sourceOwner->MatchesPinnedCopyLease(verified, identity, payload, pinnedCopyLease))
                        throw ole_driver::ObservationUnavailable("v4 group same-case input unavailable");
                    if (inactiveCollectionTab)
                    {
                        const auto& tabs = result.v4GroupIngressInput->tabEvents;
                        const auto found = std::find_if(tabs.begin(), tabs.end(), [&](const auto& item) {
                            return item.programIndex == event.programIndex && item.kind == event.kind; });
                        bool resource = false;
                        auto now = behavior::Access::InspectV4GroupTabIngress(*profile.app, L"ingress-root", L"ingress-inactive",
                            groupChildBudget, groupWidgetBudget, groupUtf16ByteBudget, resource);
                        result.v4GroupIngressInput->resourceUnavailable |= resource;
                        if (found == tabs.end() || !event.screenPoint || !now ||
                            event.screenPoint->x != found->geometry.screen.x || event.screenPoint->y != found->geometry.screen.y ||
                            now->group.host != found->geometry.group.host || now->group.group != found->geometry.group.group ||
                            now->group.groupData != found->geometry.group.groupData || now->group.activeId != found->geometry.group.activeId ||
                            now->group.ownerThread != found->geometry.group.ownerThread || now->group.layoutRevision != found->geometry.group.layoutRevision ||
                            now->child != found->geometry.child || now->childId != found->geometry.childId ||
                            !EqualRect(&now->tab, &found->geometry.tab) || now->screen.x != found->geometry.screen.x ||
                            now->screen.y != found->geometry.screen.y)
                            throw ole_driver::ObservationUnavailable("v4 inactive group tab changed before OLE call");
                        result.v4GroupIngressInput->matchingPreCallProgramIndices.push_back(event.programIndex);
                        result.v4PinnedCopyLeaseInput->matchingPreCallProgramIndices.push_back(event.programIndex);
                        observed.BeforeOleCall(event);
                        return;
                    }
                    const auto& events = result.v4GroupIngressInput->events;
                    const auto found = std::find_if(events.begin(), events.end(), [&](const auto& item) {
                        return item.programIndex == event.programIndex && item.kind == event.kind; });
                    if (found == events.end() || !event.screenPoint || event.screenPoint->x != found->geometry.screen.x ||
                        event.screenPoint->y != found->geometry.screen.y)
                        throw ole_driver::ObservationUnavailable("v4 group event coordinate unavailable");
                    const auto now = inspectGroup(found->geometry.client);
                    const auto& before = found->geometry;
                    if (now.host != before.host || now.group != before.group || now.activeWidget != before.activeWidget ||
                        now.activeId != before.activeId || now.ownerThread != before.ownerThread ||
                        now.screen.x != before.screen.x || now.screen.y != before.screen.y ||
                        now.layoutRevision != before.layoutRevision || !EqualRect(&now.viewport, &before.viewport))
                        throw ole_driver::ObservationUnavailable("v4 group geometry changed before OLE call");
                    result.v4GroupIngressInput->matchingPreCallProgramIndices.push_back(event.programIndex);
                    result.v4PinnedCopyLeaseInput->matchingPreCallProgramIndices.push_back(event.programIndex);
                    observed.BeforeOleCall(event);
                },
                [&](const ole_driver::Observation&) {},
                [&](const ole_driver::Observation& terminal) { observed.Await(terminal); diagnostics.Stop(); result.v4DiagnosticBudget = snowdesktop::external_transfer_observation::ObserveBudget(); },
                [&] { throw ole_driver::ObservationUnavailable("v4 group requires sealed execution observer"); }};
            const auto oracle = [&](const ole_driver::Observation& event) {
                if (event.kind != ole_driver::Kind::enter && event.kind != ole_driver::Kind::drop)
                    throw ole_driver::ObservationUnavailable("v4 group unexpected OLE event");
                if (event.kind == ole_driver::Kind::enter &&
                    behavior::Access::Target(*profile.app) != behavior::Access::MemberContainer(*profile.app, L"ingress-root"))
                    throw ole_driver::ObservationUnavailable("v4 group entered target unavailable");
                if (!event.returnedEffect) throw ole_driver::ObservationUnavailable("v4 group OLE effect unavailable");
                if (FAILED(event.result) || *event.returnedEffect != DROPEFFECT_COPY)
                    throw ole_driver::EventAssertionFailure("v4 group Copy event differs from the independent expectation");
            };
            const ole_driver::V4GroupObserverBoundaries groupObserver{
                [&](const std::shared_ptr<ole_driver::V4GroupExecutionKey>& key) { observed.BindExecution(key, sourceOwner, hostBinding.second, ownedProfile); },
                [&](const std::shared_ptr<ole_driver::V4GroupExecutionKey>& key) { observed.CaptureExecution(key, sourceOwner, hostBinding.second); }};
            dispatchAttempted = true;
            const auto receipt = ole_driver::V4MultiFileGraphExecutor::RunBound(verified, identity, present, selected, effectiveInputs,
                payload, binding, sourceOwner, hostBinding.first.Get(), hostBinding.second, host, oracle, {}, {}, pinnedCopyLease, {}, groupObserver);
            diagnostics.Stop(); result.v4DiagnosticBudget = snowdesktop::external_transfer_observation::ObserveBudget(); const auto& execution = receipt.Execution(); result.v4OleAdapterLogLast = execution.oleAdapterLogLast;
            result.productionStarted = execution.productionStarted; result.productionEntryUnknown = execution.productionEntryUnknown;
            dispatchAttempted = false;
            result.businessFailureObserved = execution.businessFailureObserved; result.eventOracleFailureObserved = execution.eventOracleFailureObserved;
            result.eventDispatchObservationRecorded = !execution.observations.empty();
            result.eventDispatchFailureObserved = std::any_of(execution.observations.begin(), execution.observations.end(),
                [](const ole_driver::Observation& event) { return FAILED(event.result); });
            if (result.businessFailureObserved || result.eventOracleFailureObserved) result.status = BridgeStatus::production_failed;
            result.failure = execution.failure;
            result.observationUnavailable = execution.additionalObservationUnavailable || execution.status == ole_driver::Status::observation_unavailable ||
                execution.status == ole_driver::Status::host_observation_failed || execution.status == ole_driver::Status::event_failed;
            result.obligations.insert(result.obligations.end(), execution.obligations.begin(), execution.obligations.end());
            result.unclassifiedObligations.insert(result.unclassifiedObligations.end(), execution.unclassifiedObligations.begin(), execution.unclassifiedObligations.end());
            result.postDispatchObligations.insert(result.postDispatchObligations.end(), execution.postconditionsPending.begin(), execution.postconditionsPending.end());
            result.v4BatchSourcePreparation = receipt.SourcePreparation(); result.v4BatchSourceAtExecution = receipt.SourceAtExecution();
            result.v4PinnedCopyLeaseInput->receiptPreflightObserved = receipt.PinnedCopyPreflightObserved();
            result.v4PinnedCopyLeaseInput->receiptBeforeDropObserved = receipt.PinnedCopyBeforeDropObserved();
            result.v4PinnedCopyLeaseInput->receiptAfterAwaitObserved = receipt.PinnedCopyAfterAwaitObserved();
            result.v4PinnedCopyLeaseInput->receiptSameLease = receipt.SamePinnedCopyLease(verified, identity, payload, sourceOwner, pinnedCopyLease);
            result.v4OleObservations = execution.observations; result.v4CapturedOleReturns = receipt.CapturedReturns();
            result.v4FileSourceInterval = receipt.SourceSessionInterval();
            if (receipt.CompletionTraceCaptured()) result.v4CompletionTrace = receipt.CompletionTrace();
            if (const auto sealed = receipt.SealedGroupBusiness())
            { result.v4GroupCopyBusiness = *sealed; result.v4GroupBusinessSealed = true; }
            else result.v4GroupCopyBusiness = observed.Evidence();
            if ((execution.productionStarted || execution.productionEntryUnknown) && behavior::Access::messageTrace)
            {
                try
                {
                    result.v4GroupHitActivation = ole_driver::V4GroupHitActivationReader::Inspect(receipt, verified, identity,
                        present, selected, binding, sourceOwner, hostBinding.first.Get(), hostBinding.second,
                        *behavior::Access::messageTrace, behavior::Access::fixtureControlWindow, kShellFileOperationCompletedMessage);
                    auto& comparison = result.v4GroupHitBinding.emplace();
                    const auto& actual = *result.v4GroupHitActivation;
                    comparison.resourceUnavailable = actual.resourceUnavailable;
                    if (actual.identityValid && actual.hitAdapterBound && actual.hitWindowScopeObserved &&
                        result.v4GroupAxisInput && result.v4GroupAxisInput->sameHostBindingObserved &&
                        result.v4GroupAxisInput->candidateId == raw.id && result.v4GroupAxisInput->actualTarget)
                    {
                        const auto& input = *result.v4GroupAxisInput;
                        const auto& snapshot = *input.actualTarget;
                        comparison.localReceiptObserved = true;
                        comparison.groupObjectMatches = actual.groupObject == snapshot.group;
                        comparison.groupDataMatches = actual.widgetObject == snapshot.groupData;
                        if (actual.groupKind)
                            comparison.groupKindMatches = (*actual.groupKind == 1 && snapshot.groupType == static_cast<std::int64_t>(DesktopWidgetType::CollectionGroup)) ||
                                (*actual.groupKind == 2 && snapshot.groupType == static_cast<std::int64_t>(DesktopWidgetType::FileGroup));
                        if (input.target.valuesComplete && input.targetChildCountMatches.value_or(false) &&
                            comparison.groupObjectMatches.value_or(false) && comparison.groupDataMatches.value_or(false) &&
                            comparison.groupKindMatches.value_or(false))
                        {
                            const auto& selectedHit = input.target.hit;
                            const std::string decimal = selectedHit == "zero" ? "0" : selectedHit == "one" ? "1" :
                                selectedHit == "two" ? "2" : selectedHit.starts_with("count_") ? selectedHit.substr(6) : std::string{};
                            if (!decimal.empty())
                            {
                                const auto index = V4MaterializeCount({V3CountKind::exact, decimal},
                                    (std::numeric_limits<std::size_t>::max)(), false);
                                if (index.status == V4AxisStatus::resource_unavailable) comparison.resourceUnavailable = true;
                                if (index.status == V4AxisStatus::schema_only && index.value < snapshot.children.size() &&
                                    index.value < input.target.children.size() && input.target.children[index.value].idMatches.value_or(false))
                                {
                                    const ole_driver::V4FileTaskWindowObservation::RawEvent* hit = nullptr; bool ambiguous = false;
                                    for (const auto& record : actual.records)
                                        if (record.event.phase == snowdesktop::external_transfer_observation::Phase::group_hit_value &&
                                            record.event.extra == actual.callsite && record.event.object == actual.groupObject)
                                        { if (hit) ambiguous = true; else hit = &record; }
                                    const auto& id = snapshot.children[index.value].id;
                                    if (!ambiguous && hit && hit->event.hasBlob)
                                    {
                                        if (id.size() > (std::numeric_limits<std::size_t>::max)() / sizeof(wchar_t))
                                            comparison.resourceUnavailable = true;
                                        else comparison.expectedHitMatches = hit->blob.size() == id.size() * sizeof(wchar_t) &&
                                            (hit->blob.empty() || std::memcmp(hit->blob.data(), id.data(), hit->blob.size()) == 0);
                                    }
                                }
                            }
                        }
                    }
                    comparison.unresolved.emplace_back("v4_group_hit_snapshot_non_atomic_address_reuse_and_reentry_unverified");
                    comparison.unresolved.emplace_back("v4_group_activation_membership_business_and_late_work_unverified");
                    result.observationUnavailable = true;
                }
                catch (const std::bad_alloc&) { if (!result.v4GroupHitBinding) result.v4GroupHitBinding.emplace(); result.v4GroupHitBinding->resourceUnavailable = true; result.observationUnavailable = true; }
                catch (const std::length_error&) { if (!result.v4GroupHitBinding) result.v4GroupHitBinding.emplace(); result.v4GroupHitBinding->resourceUnavailable = true; result.observationUnavailable = true; }
                catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v4_group_hit_activation_observation_unavailable"); }
            }
            if (execution.productionStarted && behavior::Access::messageTrace)
            {
                try { result.v4TrackedShellCopy = ole_driver::InspectV4MultiTrackedShellCopy(receipt, verified, identity,
                    present, selected, payload, binding, sourceOwner, hostBinding.first.Get(), hostBinding.second,
                    *behavior::Access::messageTrace, behavior::Access::fixtureControlWindow, kShellFileOperationCompletedMessage);
                    if (result.v4TrackedShellCopy->observationUnavailable || result.v4TrackedShellCopy->resourceUnavailable ||
                        !result.v4TrackedShellCopy->unresolved.empty()) result.observationUnavailable = true;
                } catch (...) { result.observationUnavailable = true; }
                try { result.v4FilePathFlow = ole_driver::InspectV4MultiFilePathFlow(receipt, verified, identity, present, selected,
                    payload, binding, sourceOwner, hostBinding.first.Get(), hostBinding.second, *behavior::Access::messageTrace,
                    behavior::Access::fixtureControlWindow, kShellFileOperationCompletedMessage);
                    if (result.v4FilePathFlow->observationUnavailable || !result.v4FilePathFlow->unresolved.empty()) result.observationUnavailable = true;
                } catch (...) { result.observationUnavailable = true; }
                try { result.v4GroupBusinessBinding = InspectV4GroupBusiness(receipt, verified, identity, present, selected,
                    payload, binding, sourceOwner, hostBinding.first.Get(), hostBinding.second, *behavior::Access::messageTrace,
                    behavior::Access::fixtureControlWindow);
                    if (result.v4GroupBusinessBinding->observationUnavailable || result.v4GroupBusinessBinding->resourceUnavailable ||
                        !result.v4GroupBusinessBinding->unresolved.empty()) result.observationUnavailable = true;
                } catch (...) { result.observationUnavailable = true; }
                if (inactiveCollectionTab && result.v4GroupHitBinding && result.v4GroupHitActivation &&
                    result.v4GroupBusinessSealed && result.v4GroupCopyBusiness && result.v4GroupBusinessBinding)
                {
                    auto& local = *result.v4GroupHitBinding;
                    const auto& hit = *result.v4GroupHitActivation;
                    const auto& business = *result.v4GroupCopyBusiness;
                    const auto& bound = *result.v4GroupBusinessBinding;
                    if (local.localReceiptObserved && local.expectedHitMatches.value_or(false) &&
                        hit.completionWindowBound && hit.activationValuesObserved && bound.sameReceiptObserved &&
                        bound.completionObserved && business.targetInactive && business.candidateId == raw.id &&
                        hit.hitDecision && hit.hitEqualsDestination && hit.completionSucceeded && hit.ownerIdUnchanged &&
                        hit.resolvedTypeMatchesHit && hit.activeAfterEqualsDestination && business.inactiveActivated &&
                        business.reloadedInactiveActivated && business.inactiveMembershipMatches &&
                        business.reloadedInactiveMembershipMatches && business.activeMembersUnchanged &&
                        business.reloadedActiveMembersUnchanged && business.rootStateExcludingActivationMatches &&
                        business.reloadedRootStateExcludingActivationMatches && business.inactiveSettingsPreserved &&
                        business.reloadedInactiveSettingsPreserved && business.outputBytesMatch && business.sourcePreserved &&
                        business.existingPreserved)
                        local.inactiveActivationAndSealedStateAgree = *hit.hitDecision == 1 && *hit.hitEqualsDestination &&
                            *hit.completionSucceeded && *hit.ownerIdUnchanged && *hit.resolvedTypeMatchesHit &&
                            *hit.activeAfterEqualsDestination && *business.inactiveActivated && *business.reloadedInactiveActivated &&
                            *business.inactiveMembershipMatches && *business.reloadedInactiveMembershipMatches &&
                            *business.activeMembersUnchanged && *business.reloadedActiveMembersUnchanged &&
                            *business.rootStateExcludingActivationMatches && *business.reloadedRootStateExcludingActivationMatches &&
                            *business.inactiveSettingsPreserved && *business.reloadedInactiveSettingsPreserved &&
                            *business.outputBytesMatch && *business.sourcePreserved && *business.existingPreserved;
                    local.unresolved.emplace_back("v4_inactive_group_local_comparison_does_not_prove_late_work_or_durable_commit");
                }
            }
        }
        catch (...) { try { if (!result.v4GroupBusinessSealed) result.v4GroupCopyBusiness = observed.Evidence(); } catch (...) {} throw; }
    }
    catch (const std::exception& error) {
        if (dispatchAttempted && !result.productionStarted) result.productionEntryUnknown = true;
        result.observationUnavailable = true; if (result.failure.empty()) result.failure = error.what();
    }
    catch (...) { if (dispatchAttempted && !result.productionStarted) result.productionEntryUnknown = true; result.observationUnavailable = true; }
    return result;
}

inline BridgeResult PrepareV4DesktopDirectoryCopy(
    const event_graph::V4FamilyIdentityContext& identity, const V4FamilyValidatedContext& verified,
    const V4FamilyDescriptor& family, const std::vector<bool>& present,
    const event_graph::V4JoinedGraphVariant& selected, behavior::OwnedProfileDesktopRoot& ownedProfile,
    const directory_fixture::FixtureObservation& input, const std::shared_ptr<directory_fixture::DirectoryCopyLease>& lease, GridCell landing,
    const std::vector<ole_driver::Input>& inputs)
{
    BridgeResult result;
    bool dispatchAttempted = false;
    try
    {
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        const auto replay = event_graph::V4JoinedGraph::Replay(identity, verified, present, selected.id);
        const auto& axes = verified.Axes(); const auto& candidate = verified.Coordinate();
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty() || raw.id != input.candidateId ||
            !replay.identityValid || !replay.errors.empty() || replay.variant.id != selected.id ||
            replay.variant.classes != selected.classes || replay.variant.order != selected.order ||
            replay.variant.unresolved != selected.unresolved || V4FamilyBinding(family) != identity.FamilyBinding() ||
            !ownedProfile.StillOwned() || !ownedProfile.profile || !ownedProfile.profile->app || !input.inputsObserved ||
            Value(axes, candidate, "T01") != "external_ingress" || Value(axes, candidate, "T02.file_paths") != "yes" ||
            Value(axes, candidate, "T04.capability") != "enabled" || Value(axes, candidate, "G04.termination") != "drop" ||
            Value(axes, candidate, "target.E01") != "free_desktop")
        { result.obligations.emplace_back("v4_directory_candidate_graph_or_fixture_unavailable"); return result; }
        result.v4DirectoryLease = directory_fixture::RevalidateDirectoryCopyLease(family, verified, identity, lease);
        if (!result.v4DirectoryLease->inputObserved || result.v4DirectoryLease->resourceUnavailable || !lease ||
            lease->CandidateId() != raw.id || lease->OwnerThread() != GetCurrentThreadId() || lease->Source() != input.source)
        { result.obligations.emplace_back("v4_directory_same_candidate_live_lease_unavailable"); return result; }
        for (const auto& reason : result.v4DirectoryLease->unresolved)
            result.obligations.push_back("v4_directory_lease:" + reason);
        auto& profile = *ownedProfile.profile;
        if (behavior::Access::productionDispatch != 1)
            behavior::Access::EnableProductionDispatch(*profile.app, false);
        if (!behavior::Access::fixtureControlWindow)
            behavior::Access::EnableFixtureControlWindow(*profile.app);
        behavior::Wait([&] { return behavior::Access::FileDropIdle(*profile.app) &&
            behavior::Access::LayoutSaveIdle(*profile.app); });
        const auto program = verified.TransferProgram();
        const auto value = [&](std::string_view id) { return Value(axes, candidate, id); };
        const auto allowed = ole_driver::EncodeAllowedEffect(value("T03"), value("T03.extra_protocol_bits"));
        std::optional<POINT> targetClient;
        for (const auto& oleInput : inputs)
        {
            BridgeResult::V4FileOleInput item;
            item.programIndex = oleInput.programIndex; item.kind = oleInput.kind; item.parameter = oleInput.modelParameter;
            item.keyState = oleInput.keyState; item.allowedEffect = oleInput.allowedEffect; item.screenPoint = oleInput.screenPoint;
            if (oleInput.programIndex < program.steps.size())
            {
                const auto& step = program.steps[oleInput.programIndex]; const auto kind = ole_driver::ProgramKind(step.kind);
                const auto [buttonsAxis, modifiersAxis] = ole_driver::PhaseAxes(step.kind);
                item.buttons = std::string(value(buttonsAxis)); item.modifiers = std::string(value(modifiersAxis));
                const auto keys = ole_driver::EncodeKeyState(item.buttons, item.modifiers);
                const auto parsed = ole_driver::ParseKeyParameter(step.parameter);
                item.candidateMatched = kind && *kind == oleInput.kind && oleInput.kind != ole_driver::Kind::leave &&
                    oleInput.modelParameter == step.parameter && parsed && parsed->first == item.buttons &&
                    parsed->second == item.modifiers && keys.valid && !keys.altX1Ambiguous &&
                    oleInput.keyState && *oleInput.keyState == keys.value && allowed && oleInput.allowedEffect &&
                    *oleInput.allowedEffect == *allowed && oleInput.screenPoint;
            }
            if (!item.candidateMatched) result.obligations.emplace_back("v4_directory_ole_input_candidate_unavailable");
            if (oleInput.screenPoint && oleInput.kind == ole_driver::Kind::drop)
            {
                POINT point{oleInput.screenPoint->x, oleInput.screenPoint->y};
                if (!behavior::Access::completionWindow || !ScreenToClient(behavior::Access::completionWindow, &point))
                    result.obligations.emplace_back("v4_directory_drop_client_coordinate_unavailable");
                else if (targetClient) result.obligations.emplace_back("v4_directory_multiple_drop_inputs_unimplemented");
                else targetClient = point;
            }
            result.v4DirectoryOleInputs.push_back(std::move(item));
        }
        if (!targetClient) result.obligations.emplace_back("v4_directory_drop_coordinate_unavailable");
        else
        {
            const auto roundTrip = behavior::Access::ExternalPoint(*targetClient);
            const auto initial = behavior::Access::InspectTextDesktopInitial(*profile.app,
                profile.desktop / L"Sentinel.txt", *targetClient);
            const bool sameScreen = std::all_of(inputs.begin(), inputs.end(), [&](const auto& event) {
                return event.kind != ole_driver::Kind::leave && event.screenPoint &&
                    event.screenPoint->x == roundTrip.x && event.screenPoint->y == roundTrip.y;
            });
            const bool matches = sameScreen && value("target.E01") == "free_desktop" && value("target.E02") == "none" &&
                value("target.E19") == "named" && !initial.pageName.empty() && value("H06.page_record") == "saved" &&
                initial.pageRecorded && value("H06.page_dimensions") == "valid" && initial.savedColumns && initial.savedRows &&
                *initial.savedColumns == initial.columns && *initial.savedRows == initial.rows &&
                value("H07.occupancy") == "partial" && EndpointSize(value("target.E23")) == initial.columns &&
                EndpointSize(value("target.E24")) == initial.rows && value("H01.region") == "empty_slot" &&
                behavior::Same(initial.targetCell, landing);
            result.v4DirectoryDesktopInput = BridgeResult::V4TextDesktopInput{
                raw.id, reinterpret_cast<std::uintptr_t>(profile.app), GetCurrentThreadId(), initial, matches};
            if (!matches) result.obligations.emplace_back("v4_directory_desktop_geometry_candidate_mismatch");
        }
        const auto selectedCollection = value("C07");
        if (selectedCollection == "on" || selectedCollection == "off")
        {
            const bool actual = behavior::Access::ProgramCollectionEnabled(*profile.app);
            result.v4DirectoryProgramCollectionInput = BridgeResult::V4TextProgramCollectionInput{
                raw.id, reinterpret_cast<std::uintptr_t>(profile.app), GetCurrentThreadId(), actual,
                actual == (selectedCollection == "on")};
            if (!result.v4DirectoryProgramCollectionInput->candidateMatched)
                result.obligations.emplace_back("v4_directory_program_collection_candidate_mismatch");
        }
        else result.obligations.emplace_back("v4_directory_program_collection_unavailable");
        const auto destination = profile.desktop / input.source.filename();
        const auto observeDestination = [&]() -> std::pair<std::optional<bool>, DWORD> {
            SetLastError(ERROR_SUCCESS); const DWORD attributes = GetFileAttributesW(destination.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES) return {true, ERROR_SUCCESS};
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND) return {false, error};
            return {std::nullopt, error};
        };
        const auto [destinationExists, queryError] = observeDestination();
        result.v4DirectoryConflictInput = BridgeResult::V4FileConflictInput{
            raw.id, destination, GetCurrentThreadId(), queryError, destinationExists,
            destinationExists && !*destinationExists && value("R04.member0_target_exists") == "absent" &&
                value("R04.member0_target_identity") == "na" && value("R04.member0_name_relation") == "na"};
        if (!result.v4DirectoryConflictInput->candidateMatched)
            result.obligations.emplace_back("v4_directory_conflict_candidate_or_path_unavailable");
        result.obligations.emplace_back("v4_directory_visibility_reservation_and_concurrent_creation_unverified");
        const auto sourceOwner = behavior::Access::CreateV4DirectoryShellOwner(*profile.app);
        const auto binding = directory_fixture::PrepareDirectoryShellSource(family, verified, identity, lease, sourceOwner);
        auto* source = binding.SourceObject();
        if (!binding.inputPrepared || binding.resourceUnavailable || binding.candidateId != raw.id || !source ||
            !source->HasConstructionShell() || source->owner != GetCurrentThreadId() ||
            !source->asynchronous || source->starts || source->ends || source->fileReads)
        { result.obligations.emplace_back("v4_directory_shell_source_unavailable"); return result; }
        for (const auto& reason : binding.unresolved) result.obligations.push_back("v4_directory_binding:" + reason);
        result.v4DirectoryInput = BridgeResult::V4DirectoryInput{
            raw.id, input.source, reinterpret_cast<std::uintptr_t>(source), source->owner, true, false,
            {"directory_source_complete_async_lifetime_and_format_unverified"}};
        // A bounded source snapshot is input evidence only. In particular it
        // does not extend the directory lifetime through an asynchronous run.
        result.v4DirectoryLiveSource = directory_fixture::ObserveLiveSource(
            family, verified, identity, input, lease, *source, 16384, 64u * 1024u * 1024u);
        if (!result.v4DirectoryLiveSource->captured || result.v4DirectoryLiveSource->resourceUnavailable)
            result.obligations.emplace_back("v4_directory_live_source_snapshot_unavailable");
        for (const auto& reason : result.v4DirectoryLiveSource->unresolved)
            result.obligations.push_back("v4_directory_live_source:" + reason);
        auto hostBinding = behavior::Access::CreateV3AdapterAndHostEpoch(*profile.app, source);
        if (!hostBinding.first || !hostBinding.second)
        { result.obligations.emplace_back("v4_directory_host_or_adapter_unavailable"); return result; }
        behavior::V4DirectoryCopyObservation observed(profile, *source, family, verified, identity, input, std::move(landing));
        result.v4DirectoryInput->initialTreeObserved = true;
        result.v4DirectoryCopyBusiness = observed.Evidence();
        const auto plan = ole_driver::BuildV4JoinedOleExecutionPlan(verified, identity, present, selected, inputs);
        result.obligations.insert(result.obligations.end(), plan.preconditions.begin(), plan.preconditions.end());
        result.graphUnresolved = plan.graphUnresolved;
        result.postDispatchObligations = plan.postDispatchObligations;
        result.unclassifiedObligations = plan.unclassified;
        result.outcomeExpectations = plan.postconditions;
        result.v4PresenceExpectations = plan.presence;
        for (const auto& reason : input.unresolved) result.obligations.push_back("v4_directory_fixture:" + reason);
        if (!plan.graphIdentityValid || !plan.structurallyPlanned || !plan.unclassified.empty())
            result.obligations.emplace_back("v4_directory_graph_or_unclassified_capability_unavailable");
        result.obligations.emplace_back("v4_directory_source_lease_dispatch_and_completion_capability_unimplemented");
        result.obligations.emplace_back("v4_directory_input_axes_geometry_and_event_oracle_unimplemented");
        result.obligations.emplace_back("v4_directory_same_run_completion_and_business_causality_unimplemented");
        if (result.obligations.empty())
        {
            if (!snowdesktop::external_transfer_observation::Enable())
                throw ole_driver::ObservationUnavailable("v4 directory diagnostic session unavailable");
            struct StopDirectoryDiagnostics final
            {
                bool active = true;
                void Stop() noexcept { if (active) { snowdesktop::external_transfer_observation::Stop(); active = false; } }
                ~StopDirectoryDiagnostics() { Stop(); }
            } diagnostics;
            const ole_driver::HostBoundaries host{
                [&](const ole_driver::Input& event) {
                    if (!result.v4DirectoryDesktopInput || !result.v4DirectoryDesktopInput->candidateMatched || !targetClient ||
                        !result.v4DirectoryProgramCollectionInput || !result.v4DirectoryProgramCollectionInput->candidateMatched ||
                        result.v4DirectoryDesktopInput->hostIdentity != reinterpret_cast<std::uintptr_t>(profile.app) ||
                        result.v4DirectoryDesktopInput->ownerThread != GetCurrentThreadId() ||
                        behavior::Access::ProgramCollectionEnabled(*profile.app) != result.v4DirectoryProgramCollectionInput->actual)
                        throw ole_driver::ObservationUnavailable("v4 directory same-host input snapshot unavailable");
                    const auto current = behavior::Access::InspectTextDesktopInitial(*profile.app,
                        profile.desktop / L"Sentinel.txt", *targetClient);
                    const auto& before = result.v4DirectoryDesktopInput->initial;
                    if (current.pageId != before.pageId || current.pageName != before.pageName ||
                        current.sentinelKey != before.sentinelKey || current.columns != before.columns || current.rows != before.rows ||
                        current.pageRecorded != before.pageRecorded || current.savedColumns != before.savedColumns ||
                        current.savedRows != before.savedRows || current.sentinelSpan.columns != before.sentinelSpan.columns ||
                        current.sentinelSpan.rows != before.sentinelSpan.rows || current.layoutRevision != before.layoutRevision ||
                        !behavior::Same(current.sentinelCell, before.sentinelCell) || !behavior::Same(current.targetCell, before.targetCell) ||
                        !EqualRect(&current.pageBounds, &before.pageBounds) || !EqualRect(&current.targetBounds, &before.targetBounds))
                        throw ole_driver::ObservationUnavailable("v4 directory desktop snapshot changed before OLE call");
                    const auto point = behavior::Access::ExternalPoint(*targetClient);
                    if (!event.screenPoint || event.screenPoint->x != point.x || event.screenPoint->y != point.y)
                        throw ole_driver::ObservationUnavailable("v4 directory screen coordinate changed before OLE call");
                    if (event.kind == ole_driver::Kind::drop)
                    {
                        const auto [exists, error] = observeDestination();
                        if (!result.v4DirectoryConflictInput || !result.v4DirectoryConflictInput->candidateMatched ||
                            !ownedProfile.StillOwned() || !exists || *exists || error != ERROR_FILE_NOT_FOUND)
                            throw ole_driver::ObservationUnavailable("v4 directory target conflict changed before Drop");
                        result.v4DirectoryConflictInput->checkedBeforeDrop = true;
                    }
                    result.v4DirectoryDesktopInput->matchingPreCallProgramIndices.push_back(event.programIndex);
                    result.v4DirectoryProgramCollectionInput->matchingPreCallProgramIndices.push_back(event.programIndex);
                    observed.BeforeOleCall(event);
                },
                [&](const ole_driver::Observation&) {},
                [&](const ole_driver::Observation& terminal) { observed.Await(terminal); diagnostics.Stop(); result.v4DiagnosticBudget = snowdesktop::external_transfer_observation::ObserveBudget(); },
                [&] { throw ole_driver::ObservationUnavailable("v4 directory requires sealed execution observer"); }};
            const auto oracle = [](const ole_driver::Observation& event) {
                if (event.kind != ole_driver::Kind::drop) return;
                if (!event.returnedEffect) throw ole_driver::ObservationUnavailable("v4 directory Drop effect unavailable");
                if (FAILED(event.result) || *event.returnedEffect != DROPEFFECT_COPY)
                    throw ole_driver::EventAssertionFailure("v4 directory Drop differs from copy expectation");
            };
            try
            {
                const ole_driver::V4DirectoryObserverBoundaries directoryObserver{
                    [&](const std::shared_ptr<ole_driver::V4DirectoryExecutionKey>& key) {
                        observed.BindExecution(key, sourceOwner, lease, hostBinding.second, ownedProfile, family, verified, identity, binding);
                    },
                    [&](const std::shared_ptr<ole_driver::V4DirectoryExecutionKey>& key) {
                        observed.CaptureExecution(key, sourceOwner, lease, hostBinding.second);
                    }};
                dispatchAttempted = true;
                const auto receipt = ole_driver::V4DirectoryGraphExecutor::RunBound(family, verified, identity,
                    present, selected, inputs, lease, binding, sourceOwner, hostBinding.first.Get(), hostBinding.second,
                    host, oracle, {}, directoryObserver);
                diagnostics.Stop(); result.v4DiagnosticBudget = snowdesktop::external_transfer_observation::ObserveBudget();
                const auto& execution = receipt.Execution(); result.v4OleAdapterLogLast = execution.oleAdapterLogLast;
                result.productionStarted = execution.productionStarted;
                result.productionEntryUnknown = execution.productionEntryUnknown;
                dispatchAttempted = false;
                result.businessFailureObserved = execution.businessFailureObserved;
                result.eventOracleFailureObserved = execution.eventOracleFailureObserved;
                result.eventDispatchObservationRecorded = !execution.observations.empty();
                result.eventDispatchFailureObserved = std::any_of(execution.observations.begin(), execution.observations.end(),
                    [](const ole_driver::Observation& event) { return FAILED(event.result); });
                if (execution.businessFailureObserved || execution.eventOracleFailureObserved)
                    result.status = BridgeStatus::production_failed;
                result.observationUnavailable = execution.additionalObservationUnavailable ||
                    execution.status == ole_driver::Status::observation_unavailable ||
                    execution.status == ole_driver::Status::host_observation_failed ||
                    execution.status == ole_driver::Status::event_failed;
                result.failure = execution.failure;
                result.obligations.insert(result.obligations.end(), execution.obligations.begin(), execution.obligations.end());
                result.unclassifiedObligations.insert(result.unclassifiedObligations.end(),
                    execution.unclassifiedObligations.begin(), execution.unclassifiedObligations.end());
                result.postDispatchObligations.insert(result.postDispatchObligations.end(),
                    execution.postconditionsPending.begin(), execution.postconditionsPending.end());
                result.v4DirectorySourcePreparation = receipt.SourcePreparation();
                result.v4DirectorySourceAtExecution = receipt.SourceAtExecution();
                result.v4OleObservations = execution.observations;
                result.v4CapturedOleReturns = receipt.CapturedReturns();
                result.v4FileSourceInterval = receipt.SourceSessionInterval();
                if (receipt.CompletionTraceCaptured()) result.v4CompletionTrace = receipt.CompletionTrace();
                if (const auto sealed = receipt.SealedDirectoryBusiness())
                { result.v4DirectoryCopyBusiness = *sealed; result.v4DirectoryBusinessSealed = true; }
                else result.v4DirectoryCopyBusiness = observed.Evidence();
                if (execution.productionStarted && behavior::Access::messageTrace)
                {
                    try
                    {
                        result.v4DirectoryTaskWindow = ole_driver::InspectV4DirectoryTaskWindows(receipt, verified, identity,
                            present, selected, lease, binding, sourceOwner, hostBinding.first.Get(), hostBinding.second,
                            *behavior::Access::messageTrace, behavior::Access::fixtureControlWindow, kShellFileOperationCompletedMessage);
                        if (result.v4DirectoryTaskWindow->observationUnavailable || !result.v4DirectoryTaskWindow->unresolved.empty())
                            result.observationUnavailable = true;
                    }
                    catch (...) { result.observationUnavailable = true; }
                    try
                    {
                        result.v4DirectoryGraph = ole_driver::InspectV4DirectoryGraph(receipt, verified, identity,
                            present, selected, lease, binding, sourceOwner, hostBinding.first.Get(), hostBinding.second,
                            *behavior::Access::messageTrace, behavior::Access::fixtureControlWindow, kShellFileOperationCompletedMessage);
                        result.hypothesisNotMetObserved = result.v4DirectoryGraph->identityValid &&
                            result.v4DirectoryGraph->hypothesisObservationRecorded && result.v4DirectoryGraph->hypothesisNotMet;
                        if (result.v4DirectoryGraph->observationUnavailable || !result.v4DirectoryGraph->unresolved.empty())
                            result.observationUnavailable = true;
                    }
                    catch (...) { result.observationUnavailable = true; }
                    try
                    {
                        result.v4DirectoryAbiEnvelope = ole_driver::InspectV4DirectoryAbiEnvelope(receipt, verified, identity,
                            present, selected, lease, binding, sourceOwner, hostBinding.first.Get(), hostBinding.second,
                            *behavior::Access::messageTrace, behavior::Access::fixtureControlWindow, kShellFileOperationCompletedMessage);
                        if (result.v4DirectoryAbiEnvelope->observationUnavailable || !result.v4DirectoryAbiEnvelope->unresolved.empty())
                            result.observationUnavailable = true;
                    }
                    catch (...) { result.observationUnavailable = true; }
                    try
                    {
                        result.v4DirectoryPathFlow = ole_driver::InspectV4DirectoryPathFlow(receipt, verified, identity,
                            present, selected, lease, binding, sourceOwner, hostBinding.first.Get(), hostBinding.second,
                            *behavior::Access::messageTrace, behavior::Access::fixtureControlWindow, kShellFileOperationCompletedMessage);
                        if (result.v4DirectoryPathFlow->observationUnavailable || !result.v4DirectoryPathFlow->unresolved.empty())
                            result.observationUnavailable = true;
                    }
                    catch (...) { result.observationUnavailable = true; }
                    try
                    {
                        result.v4DirectorySource = ole_driver::InspectV4DirectorySource(receipt, verified, identity,
                            present, selected, lease, binding, sourceOwner, hostBinding.first.Get(), hostBinding.second,
                            *behavior::Access::messageTrace, behavior::Access::fixtureControlWindow);
                        if (result.v4DirectorySource->observationUnavailable || !result.v4DirectorySource->unresolved.empty())
                            result.observationUnavailable = true;
                    }
                    catch (...) { result.observationUnavailable = true; }
                    try
                    {
                        result.v4DirectoryBusinessBinding = InspectV4DirectoryBusiness(receipt, verified, identity,
                            present, selected, lease, binding, sourceOwner, hostBinding.first.Get(), hostBinding.second,
                            *behavior::Access::messageTrace, behavior::Access::fixtureControlWindow);
                        if (result.v4DirectoryBusinessBinding->observationUnavailable ||
                            result.v4DirectoryBusinessBinding->resourceUnavailable ||
                            !result.v4DirectoryBusinessBinding->unresolved.empty()) result.observationUnavailable = true;
                    }
                    catch (...) { result.observationUnavailable = true; }
                }
                if (execution.productionStarted)
                    result.obligations.emplace_back("v4_directory_full_graph_and_outcomes_unverified");
            }
            catch (...)
            {
                try { if (!result.v4DirectoryBusinessSealed) result.v4DirectoryCopyBusiness = observed.Evidence(); } catch (...) {}
                throw;
            }
        }
    }
    catch (const std::exception& error)
    { if (dispatchAttempted && !result.productionStarted) result.productionEntryUnknown = true;
      result.observationUnavailable = true; if (result.failure.empty()) result.failure = error.what(); }
    catch (...)
    { if (dispatchAttempted && !result.productionStarted) result.productionEntryUnknown = true;
      result.observationUnavailable = true; if (result.failure.empty()) result.failure = "v4_directory_preparation_unavailable"; }
    return result;
}

// A Leave candidate uses its own terminal boundary and local no-output
// observation; it never borrows asynchronous CF_HDROP completion semantics.
inline BridgeResult PrepareV4DesktopSingleFileLeave(
    const event_graph::V4FamilyIdentityContext& identity, const V4FamilyValidatedContext& verified,
    const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
    behavior::OwnedProfileDesktopRoot& ownedProfile, const std::shared_ptr<V4IndependentFilePayload>& payload,
    const V4IndependentShellSource& binding, const std::shared_ptr<V4IndependentShellOwner>& sourceOwner,
    const std::vector<ole_driver::Input>& inputs, const std::shared_ptr<const V4PinnedCopyLease>& pinnedCopyLease)
{
    BridgeResult result; bool dispatchAttempted = false;
    try
    {
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        const auto replay = event_graph::V4JoinedGraph::Replay(identity, verified, present, selected.id);
        const auto value = [&](std::string_view id) { return Value(verified.Axes(), verified.Coordinate(), id); };
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty() || !payload || payload->files.size() != 1 ||
            payload->status != V4IndependentFileStatus::input_prepared || payload->candidateId != raw.id || !sourceOwner ||
            !binding.inputPrepared || binding.resourceUnavailable || binding.candidateId != raw.id ||
            !binding.SameOwner(sourceOwner) || !binding.SamePayload(payload) || !ownedProfile.StillOwned() ||
            !ownedProfile.profile || !ownedProfile.profile->app || behavior::Access::v4IndependentShellOwner != sourceOwner ||
            !replay.identityValid || !replay.errors.empty() || replay.variant.id != selected.id ||
            replay.variant.classes != selected.classes || replay.variant.order != selected.order || replay.variant.unresolved != selected.unresolved ||
            value("V4.source.kind") != "file_only" || value("V4.source.file_count") != "one" ||
            value("V4.file.member0_kind") != "file" || value("V4.file.member0_identity") != "unique" ||
            value("T01") != "external_ingress" || value("T02.file_paths") != "yes" || value("T04.capability") != "enabled" ||
            value("G04.termination") != "leave" || value("G02.release") != "na" || value("G02.release_buttons") != "na" ||
            value("G02.enter_buttons") != "left" || value("G02.enter") != "none" ||
            value("G02.hover_first") != "na" || value("G02.hover_first_buttons") != "na" ||
            value("G02.hover_later") != "na" || value("G02.hover_later_buttons") != "na" ||
            value("T03") != "copy_move_link" || value("T03.extra_protocol_bits") != "none" ||
            value("target.E01") != "free_desktop" || value("target.E02") != "none" || value("H01.region") != "empty_slot")
        { result.obligations.emplace_back("v4_leave_candidate_graph_or_source_unavailable"); return result; }
        auto& profile = *ownedProfile.profile;
        if (behavior::Access::productionDispatch != 1) behavior::Access::EnableProductionDispatch(*profile.app, false);
        if (!behavior::Access::fixtureControlWindow) behavior::Access::EnableFixtureControlWindow(*profile.app);
        behavior::Wait([&] { return behavior::Access::FileDropIdle(*profile.app) && behavior::Access::LayoutSaveIdle(*profile.app); });
        auto* source = binding.SourceObject();
        if (!source || !source->HasConstructionShell()) { result.obligations.emplace_back("v4_leave_source_unavailable"); return result; }
        result.v4PinnedCopyLeaseInput.emplace(); result.v4PinnedCopyLeaseInput->candidateId = raw.id;
        result.v4PinnedCopyLeaseInput->ownerBoundAndInitiallyObserved = pinnedCopyLease &&
            sourceOwner->MatchesPinnedCopyLease(verified, identity, payload, pinnedCopyLease);
        if (!result.v4PinnedCopyLeaseInput->ownerBoundAndInitiallyObserved)
        { result.obligations.emplace_back("v4_leave_pinned_source_lease_unavailable"); return result; }
        if (inputs.size() != 2 || inputs[0].kind != ole_driver::Kind::enter || inputs[1].kind != ole_driver::Kind::leave ||
            !inputs[0].screenPoint || inputs[1].screenPoint || inputs[0].programIndex == inputs[1].programIndex)
        { result.obligations.emplace_back("v4_leave_input_sequence_unavailable"); return result; }
        POINT client{inputs[0].screenPoint->x, inputs[0].screenPoint->y};
        if (!ScreenToClient(behavior::Access::completionWindow, &client))
        { result.obligations.emplace_back("v4_leave_client_point_unavailable"); return result; }
        const auto initial = behavior::Access::InspectTextDesktopInitial(*profile.app, profile.desktop / L"Sentinel.txt", client);
        const auto screen = behavior::Access::ExternalPoint(client);
        const bool geometryMatches = screen.x == inputs[0].screenPoint->x && screen.y == inputs[0].screenPoint->y &&
            value("target.E19") == "named" && !initial.pageName.empty() && value("H06.page_record") == "saved" &&
            initial.pageRecorded && value("H06.page_dimensions") == "valid" && initial.savedColumns && initial.savedRows &&
            *initial.savedColumns == initial.columns && *initial.savedRows == initial.rows && value("H07.occupancy") == "partial" &&
            EndpointSize(value("target.E23")) == initial.columns && EndpointSize(value("target.E24")) == initial.rows;
        result.v4LeaveDesktopInput = BridgeResult::V4TextDesktopInput{
            raw.id, reinterpret_cast<std::uintptr_t>(profile.app), GetCurrentThreadId(), initial, geometryMatches};
        if (!geometryMatches) result.obligations.emplace_back("v4_leave_desktop_geometry_unavailable");
        const auto selectedCollection = value("C07");
        if (selectedCollection == "on" || selectedCollection == "off")
        {
            const bool actual = behavior::Access::ProgramCollectionEnabled(*profile.app);
            result.v4LeaveProgramCollectionInput = BridgeResult::V4TextProgramCollectionInput{
                raw.id, reinterpret_cast<std::uintptr_t>(profile.app), GetCurrentThreadId(), actual, actual == (selectedCollection == "on")};
            if (!result.v4LeaveProgramCollectionInput->candidateMatched) result.obligations.emplace_back("v4_leave_c07_mismatch");
        }
        else result.obligations.emplace_back("v4_leave_c07_unavailable");
        result.v4BatchSourcePreparation = ole_driver::InspectV4IndependentFileInputs(verified, identity, *payload, *source);
        if (!result.v4BatchSourcePreparation->captured || result.v4BatchSourcePreparation->resourceUnavailable)
        { result.obligations.emplace_back("v4_leave_source_snapshot_unavailable"); return result; }
        for (const auto& reason : result.v4BatchSourcePreparation->unresolved) result.obligations.push_back("v4_leave_source:" + reason);
        auto hostBinding = behavior::Access::CreateV3AdapterAndHostEpoch(*profile.app, source);
        if (!hostBinding.first || !hostBinding.second) { result.obligations.emplace_back("v4_leave_host_epoch_unavailable"); return result; }
        behavior::V4FileLeaveObservation observed(ownedProfile, payload, binding, sourceOwner, verified, identity);
        try
        {
            const auto plan = ole_driver::BuildV4JoinedOleExecutionPlan(verified, identity, present, selected, inputs);
            result.obligations.insert(result.obligations.end(), plan.preconditions.begin(), plan.preconditions.end());
            result.graphUnresolved = plan.graphUnresolved; result.postDispatchObligations = plan.postDispatchObligations;
            result.unclassifiedObligations = plan.unclassified; result.outcomeExpectations = plan.postconditions; result.v4PresenceExpectations = plan.presence;
            if (!plan.graphIdentityValid || !plan.structurallyPlanned || !plan.unclassified.empty())
                result.obligations.emplace_back("v4_leave_graph_or_unclassified_capability_unavailable");
            for (const auto& reason : binding.unresolved) result.obligations.push_back("v4_leave_binding:" + reason);
            result.obligations.emplace_back("v4_leave_all_axes_target_hit_and_event_interval_unimplemented");
            result.obligations.emplace_back("v4_leave_late_work_and_complete_no_output_oracle_unimplemented");
            result.v4LeaveBusiness = observed.Evidence();
            if (!result.obligations.empty()) return result;
            if (!snowdesktop::external_transfer_observation::Enable())
                throw ole_driver::ObservationUnavailable("v4 leave diagnostics unavailable");
            struct StopDiagnostics { bool active = true; void Stop() noexcept { if (active) { snowdesktop::external_transfer_observation::Stop(); active = false; } }
                ~StopDiagnostics() { Stop(); } } diagnostics;
            const ole_driver::HostBoundaries host{
                [&](const ole_driver::Input& event) {
                    if (!result.v4LeaveDesktopInput || !result.v4LeaveDesktopInput->candidateMatched ||
                        !result.v4LeaveProgramCollectionInput || !result.v4LeaveProgramCollectionInput->candidateMatched ||
                        !ownedProfile.StillOwned() || !binding.SameOwner(sourceOwner) || !binding.SamePayload(payload) ||
                        binding.SourceObject() != source || !sourceOwner->MatchesPinnedCopyLease(verified, identity, payload, pinnedCopyLease) ||
                        behavior::Access::ProgramCollectionEnabled(*profile.app) != result.v4LeaveProgramCollectionInput->actual)
                        throw ole_driver::ObservationUnavailable("v4 leave same-case input unavailable");
                    const auto now = behavior::Access::InspectTextDesktopInitial(*profile.app, profile.desktop / L"Sentinel.txt", client);
                    const auto& before = result.v4LeaveDesktopInput->initial;
                    if (now.pageId != before.pageId || now.pageName != before.pageName || now.sentinelKey != before.sentinelKey ||
                        now.columns != before.columns || now.rows != before.rows || now.pageRecorded != before.pageRecorded ||
                        now.savedColumns != before.savedColumns || now.savedRows != before.savedRows ||
                        now.sentinelSpan.columns != before.sentinelSpan.columns || now.sentinelSpan.rows != before.sentinelSpan.rows ||
                        now.layoutRevision != before.layoutRevision || !behavior::Same(now.sentinelCell, before.sentinelCell) ||
                        !behavior::Same(now.targetCell, before.targetCell) || !EqualRect(&now.pageBounds, &before.pageBounds) ||
                        !EqualRect(&now.targetBounds, &before.targetBounds))
                        throw ole_driver::ObservationUnavailable("v4 leave desktop geometry changed");
                    if (event.kind == ole_driver::Kind::enter && (!event.screenPoint || event.screenPoint->x != screen.x || event.screenPoint->y != screen.y))
                        throw ole_driver::ObservationUnavailable("v4 leave Enter point changed");
                    if (event.kind == ole_driver::Kind::leave && event.screenPoint)
                        throw ole_driver::ObservationUnavailable("v4 leave unexpected terminal point");
                    result.v4LeaveDesktopInput->matchingPreCallProgramIndices.push_back(event.programIndex);
                    result.v4LeaveProgramCollectionInput->matchingPreCallProgramIndices.push_back(event.programIndex);
                    result.v4PinnedCopyLeaseInput->matchingPreCallProgramIndices.push_back(event.programIndex);
                    observed.BeforeOleCall(event);
                },
                [&](const ole_driver::Observation&) {},
                [&](const ole_driver::Observation& terminal) { observed.Await(terminal); diagnostics.Stop(); result.v4DiagnosticBudget = snowdesktop::external_transfer_observation::ObserveBudget(); },
                [&] { throw ole_driver::ObservationUnavailable("v4 leave requires sealed execution observer"); }};
            const auto oracle = [&](const ole_driver::Observation& event) {
                if (event.kind != ole_driver::Kind::enter && event.kind != ole_driver::Kind::leave)
                    throw ole_driver::ObservationUnavailable("v4 leave unexpected OLE event");
                if (event.kind == ole_driver::Kind::enter &&
                    (behavior::Access::Target(*profile.app) != behavior::Access::Desktop(*profile.app) ||
                     behavior::Access::Region(*profile.app) != HitRegion::Empty))
                    throw ole_driver::ObservationUnavailable("v4 leave entered target unavailable");
                if (FAILED(event.result)) throw ole_driver::EventAssertionFailure("v4 leave OLE event failed");
            };
            const ole_driver::V4LeaveObserverBoundaries leaveObserver{
                [&](const std::shared_ptr<ole_driver::V4LeaveExecutionKey>& key) {
                    observed.BindExecution(key, sourceOwner, pinnedCopyLease, hostBinding.second, verified, identity); },
                [&](const std::shared_ptr<ole_driver::V4LeaveExecutionKey>& key) {
                    observed.CaptureExecution(key, sourceOwner, pinnedCopyLease, hostBinding.second); }};
            dispatchAttempted = true;
            const auto receipt = ole_driver::V4MultiFileGraphExecutor::RunBound(verified, identity, present, selected, inputs,
                payload, binding, sourceOwner, hostBinding.first.Get(), hostBinding.second, host, oracle,
                {}, {}, pinnedCopyLease, {}, {}, {}, {}, leaveObserver);
            diagnostics.Stop(); result.v4DiagnosticBudget = snowdesktop::external_transfer_observation::ObserveBudget(); const auto& execution = receipt.Execution(); result.v4OleAdapterLogLast = execution.oleAdapterLogLast;
            result.productionStarted = execution.productionStarted; result.productionEntryUnknown = execution.productionEntryUnknown; dispatchAttempted = false;
            result.businessFailureObserved = execution.businessFailureObserved; result.eventOracleFailureObserved = execution.eventOracleFailureObserved;
            result.eventDispatchObservationRecorded = !execution.observations.empty();
            result.eventDispatchFailureObserved = std::any_of(execution.observations.begin(), execution.observations.end(),
                [](const ole_driver::Observation& event) { return FAILED(event.result); });
            if (result.businessFailureObserved || result.eventOracleFailureObserved) result.status = BridgeStatus::production_failed;
            result.failure = execution.failure;
            result.observationUnavailable = execution.additionalObservationUnavailable || execution.status == ole_driver::Status::observation_unavailable ||
                execution.status == ole_driver::Status::host_observation_failed || execution.status == ole_driver::Status::event_failed;
            result.obligations.insert(result.obligations.end(), execution.obligations.begin(), execution.obligations.end());
            result.postDispatchObligations.insert(result.postDispatchObligations.end(), execution.postconditionsPending.begin(), execution.postconditionsPending.end());
            result.unclassifiedObligations.insert(result.unclassifiedObligations.end(), execution.unclassifiedObligations.begin(), execution.unclassifiedObligations.end());
            result.v4BatchSourcePreparation = receipt.SourcePreparation(); result.v4BatchSourceAtExecution = receipt.SourceAtExecution();
            result.v4PinnedCopyLeaseInput->receiptPreflightObserved = receipt.PinnedCopyPreflightObserved();
            result.v4PinnedCopyLeaseInput->receiptBeforeLeaveObserved = receipt.PinnedCopyBeforeLeaveObserved();
            result.v4PinnedCopyLeaseInput->receiptAfterAwaitObserved = receipt.PinnedCopyAfterAwaitObserved();
            result.v4PinnedCopyLeaseInput->receiptSameLease = receipt.SamePinnedCopyLease(verified, identity, payload, sourceOwner, pinnedCopyLease);
            result.v4OleObservations = execution.observations; result.v4CapturedOleReturns = receipt.CapturedReturns();
            result.v4FileSourceInterval = receipt.SourceSessionInterval();
            if (receipt.CompletionTraceCaptured()) result.v4CompletionTrace = receipt.CompletionTrace();
            if (const auto sealed = receipt.SealedLeaveBusiness()) { result.v4LeaveBusiness = *sealed; result.v4LeaveBusinessSealed = true; }
            else result.v4LeaveBusiness = observed.Evidence();
            if (execution.productionStarted && behavior::Access::messageTrace)
            {
                try { result.v4LeaveHostObservation = ole_driver::V4LeaveHostObservation::Inspect(receipt, verified, identity,
                    payload, binding, sourceOwner, pinnedCopyLease, hostBinding.first.Get(), hostBinding.second,
                    *behavior::Access::messageTrace, behavior::Access::fixtureControlWindow);
                    if (result.v4LeaveHostObservation->observationUnavailable || result.v4LeaveHostObservation->resourceUnavailable ||
                        !result.v4LeaveHostObservation->unresolved.empty()) result.observationUnavailable = true;
                } catch (...) { result.observationUnavailable = true; }
                try { result.v4LeaveBusinessBinding = InspectV4LeaveBusiness(receipt, payload, binding, sourceOwner,
                    hostBinding.first.Get(), hostBinding.second, *behavior::Access::messageTrace, behavior::Access::fixtureControlWindow);
                    if (result.v4LeaveBusinessBinding->observationUnavailable || result.v4LeaveBusinessBinding->resourceUnavailable ||
                        !result.v4LeaveBusinessBinding->unresolved.empty()) result.observationUnavailable = true;
                } catch (...) { result.observationUnavailable = true; }
            }
        }
        catch (...) { try { if (!result.v4LeaveBusinessSealed) result.v4LeaveBusiness = observed.Evidence(); } catch (...) {} throw; }
    }
    catch (const std::exception& error) { if (dispatchAttempted && !result.productionStarted) result.productionEntryUnknown = true;
        result.observationUnavailable = true; if (result.failure.empty()) result.failure = error.what(); }
    catch (...) { if (dispatchAttempted && !result.productionStarted) result.productionEntryUnknown = true; result.observationUnavailable = true; }
    return result;
}

// A collection Leave uses the same terminal key and its own actual member baseline.
inline BridgeResult PrepareV4CollectionSingleFileLeave(
    const event_graph::V4FamilyIdentityContext& identity, const V4FamilyValidatedContext& verified,
    const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
    behavior::OwnedProfileDesktopRoot& ownedProfile, const std::shared_ptr<V4IndependentFilePayload>& payload,
    const V4IndependentShellSource& binding, const std::shared_ptr<V4IndependentShellOwner>& sourceOwner,
    const std::vector<ole_driver::Input>& inputs, const std::shared_ptr<const V4PinnedCopyLease>& pinnedCopyLease)
{
    BridgeResult result; bool dispatchAttempted = false;
    try
    {
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        const auto replay = event_graph::V4JoinedGraph::Replay(identity, verified, present, selected.id);
        const auto value = [&](std::string_view id) { return Value(verified.Axes(), verified.Coordinate(), id); };
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty() || !payload || payload->files.size() != 1 ||
            payload->status != V4IndependentFileStatus::input_prepared || payload->candidateId != raw.id || !sourceOwner ||
            !binding.inputPrepared || binding.resourceUnavailable || binding.candidateId != raw.id ||
            !binding.SameOwner(sourceOwner) || !binding.SamePayload(payload) || !ownedProfile.StillOwned() ||
            !ownedProfile.profile || !ownedProfile.profile->app || behavior::Access::v4IndependentShellOwner != sourceOwner ||
            !replay.identityValid || !replay.errors.empty() || replay.variant.id != selected.id ||
            replay.variant.classes != selected.classes || replay.variant.order != selected.order || replay.variant.unresolved != selected.unresolved ||
            value("V4.source.kind") != "file_only" || value("V4.source.file_count") != "one" ||
            value("V4.file.member0_kind") != "file" || value("V4.file.member0_identity") != "unique" ||
            value("T01") != "external_ingress" || value("T02.file_paths") != "yes" || value("T04.capability") != "enabled" ||
            value("G04.termination") != "leave" || value("G02.release") != "na" || value("G02.release_buttons") != "na" ||
            value("G02.enter_buttons") != "left" || value("G02.enter") != "ctrl" ||
            value("G02.hover_first") != "na" || value("G02.hover_first_buttons") != "na" ||
            value("G02.hover_later") != "na" || value("G02.hover_later_buttons") != "na" ||
            value("T03") != "copy_move_link" || value("T03.extra_protocol_bits") != "none" ||
            value("target.E01") != "free_desktop" || value("target.E02") != "collection" ||
            value("target.E03") != "widget_body" || value("H01.region") != "widget_body")
        { result.obligations.emplace_back("v4_collection_leave_candidate_graph_or_source_unavailable"); return result; }
        auto& profile = *ownedProfile.profile;
        if (behavior::Access::productionDispatch != 1) behavior::Access::EnableProductionDispatch(*profile.app, false);
        if (!behavior::Access::fixtureControlWindow) behavior::Access::EnableFixtureControlWindow(*profile.app);
        behavior::Wait([&] { return behavior::Access::FileDropIdle(*profile.app) && behavior::Access::LayoutSaveIdle(*profile.app); });
        auto* source = binding.SourceObject();
        if (!source || !source->HasConstructionShell()) { result.obligations.emplace_back("v4_collection_leave_source_unavailable"); return result; }
        result.v4PinnedCopyLeaseInput.emplace(); result.v4PinnedCopyLeaseInput->candidateId = raw.id;
        result.v4PinnedCopyLeaseInput->ownerBoundAndInitiallyObserved = pinnedCopyLease &&
            sourceOwner->MatchesPinnedCopyLease(verified, identity, payload, pinnedCopyLease);
        if (!result.v4PinnedCopyLeaseInput->ownerBoundAndInitiallyObserved)
        { result.obligations.emplace_back("v4_collection_leave_pinned_source_lease_unavailable"); return result; }
        if (inputs.size() != 2 || inputs[0].kind != ole_driver::Kind::enter || inputs[1].kind != ole_driver::Kind::leave ||
            !inputs[0].screenPoint || inputs[1].screenPoint || inputs[0].programIndex == inputs[1].programIndex)
        { result.obligations.emplace_back("v4_collection_leave_input_sequence_unavailable"); return result; }
        POINT client{inputs[0].screenPoint->x, inputs[0].screenPoint->y};
        if (!ScreenToClient(behavior::Access::completionWindow, &client))
        { result.obligations.emplace_back("v4_collection_leave_client_point_unavailable"); return result; }
        const auto selectedCollection = value("C07");
        if (selectedCollection == "on" || selectedCollection == "off")
        {
            const bool actual = behavior::Access::ProgramCollectionEnabled(*profile.app);
            result.v4LeaveProgramCollectionInput = BridgeResult::V4TextProgramCollectionInput{
                raw.id, reinterpret_cast<std::uintptr_t>(profile.app), GetCurrentThreadId(), actual, actual == (selectedCollection == "on")};
            if (!result.v4LeaveProgramCollectionInput->candidateMatched) result.obligations.emplace_back("v4_collection_leave_c07_mismatch");
        }
        else result.obligations.emplace_back("v4_collection_leave_c07_unavailable");
        result.v4BatchSourcePreparation = ole_driver::InspectV4IndependentFileInputs(verified, identity, *payload, *source);
        if (!result.v4BatchSourcePreparation->captured || result.v4BatchSourcePreparation->resourceUnavailable)
        { result.obligations.emplace_back("v4_collection_leave_source_snapshot_unavailable"); return result; }
        for (const auto& reason : result.v4BatchSourcePreparation->unresolved) result.obligations.push_back("v4_collection_leave_source:" + reason);
        auto hostBinding = behavior::Access::CreateV3AdapterAndHostEpoch(*profile.app, source);
        if (!hostBinding.first || !hostBinding.second) { result.obligations.emplace_back("v4_collection_leave_host_epoch_unavailable"); return result; }
        behavior::V4FileLeaveObservation observed(ownedProfile, payload, binding, sourceOwner, verified, identity);
        try
        {
            const auto geometry = behavior::Access::InspectCollectionIngress(*profile.app, client);
            if (geometry.screen.x != inputs[0].screenPoint->x || geometry.screen.y != inputs[0].screenPoint->y)
                throw ole_driver::ObservationUnavailable("v4 collection leave screen coordinate unavailable");
            result.v4CollectionIngressInput.emplace();
            auto& ingress = *result.v4CollectionIngressInput;
            ingress.candidateId = raw.id;
            ingress.events.push_back({inputs[0].programIndex, inputs[0].kind, geometry});
            ingress.viewportInputMatched = true;
            result.obligations.emplace_back("v4_collection_leave_real_hit_and_widget_subregion_unverified");
            const auto plan = ole_driver::BuildV4JoinedOleExecutionPlan(verified, identity, present, selected, inputs);
            result.obligations.insert(result.obligations.end(), plan.preconditions.begin(), plan.preconditions.end());
            result.graphUnresolved = plan.graphUnresolved; result.postDispatchObligations = plan.postDispatchObligations;
            result.unclassifiedObligations = plan.unclassified; result.outcomeExpectations = plan.postconditions; result.v4PresenceExpectations = plan.presence;
            if (!plan.graphIdentityValid || !plan.structurallyPlanned || !plan.unclassified.empty())
                result.obligations.emplace_back("v4_collection_leave_graph_or_unclassified_capability_unavailable");
            for (const auto& reason : binding.unresolved) result.obligations.push_back("v4_collection_leave_binding:" + reason);
            result.obligations.emplace_back("v4_collection_leave_all_axes_target_hit_and_event_interval_unimplemented");
            result.obligations.emplace_back("v4_collection_leave_late_work_and_complete_no_output_oracle_unimplemented");
            result.obligations.emplace_back("v4_collection_leave_early_source_read_causality_and_reentry_unverified");
            result.v4LeaveBusiness = observed.Evidence();
            if (!result.obligations.empty()) return result;
            if (!snowdesktop::external_transfer_observation::Enable())
                throw ole_driver::ObservationUnavailable("v4 collection leave diagnostics unavailable");
            struct StopDiagnostics { bool active = true; void Stop() noexcept { if (active) { snowdesktop::external_transfer_observation::Stop(); active = false; } }
                ~StopDiagnostics() { Stop(); } } diagnostics;
            const ole_driver::HostBoundaries host{
                [&](const ole_driver::Input& event) {
                    if (!result.v4CollectionIngressInput || !result.v4CollectionIngressInput->viewportInputMatched ||
                        result.v4CollectionIngressInput->candidateId != raw.id || !result.v4LeaveProgramCollectionInput ||
                        !result.v4LeaveProgramCollectionInput->candidateMatched || !ownedProfile.StillOwned() ||
                        !binding.SameOwner(sourceOwner) || !binding.SamePayload(payload) || binding.SourceObject() != source ||
                        !sourceOwner->MatchesPinnedCopyLease(verified, identity, payload, pinnedCopyLease) ||
                        behavior::Access::ProgramCollectionEnabled(*profile.app) != result.v4LeaveProgramCollectionInput->actual)
                        throw ole_driver::ObservationUnavailable("v4 collection leave same-case input unavailable");
                    const auto& before = result.v4CollectionIngressInput->events.front().geometry;
                    const auto now = behavior::Access::InspectCollectionIngress(*profile.app, before.client);
                    if (now.host != before.host || now.container != before.container || now.ownerThread != before.ownerThread ||
                        now.screen.x != before.screen.x || now.screen.y != before.screen.y ||
                        now.layoutRevision != before.layoutRevision || !EqualRect(&now.viewport, &before.viewport))
                        throw ole_driver::ObservationUnavailable("v4 collection leave ingress changed");
                    if (event.kind == ole_driver::Kind::enter &&
                        (!event.screenPoint || event.screenPoint->x != before.screen.x || event.screenPoint->y != before.screen.y))
                        throw ole_driver::ObservationUnavailable("v4 collection leave Enter point changed");
                    if (event.kind == ole_driver::Kind::leave && event.screenPoint)
                        throw ole_driver::ObservationUnavailable("v4 collection leave unexpected terminal point");
                    result.v4CollectionIngressInput->matchingPreCallProgramIndices.push_back(event.programIndex);
                    result.v4LeaveProgramCollectionInput->matchingPreCallProgramIndices.push_back(event.programIndex);
                    result.v4PinnedCopyLeaseInput->matchingPreCallProgramIndices.push_back(event.programIndex);
                    observed.BeforeOleCall(event);
                },
                [&](const ole_driver::Observation&) {},
                [&](const ole_driver::Observation& terminal) { observed.Await(terminal); diagnostics.Stop(); result.v4DiagnosticBudget = snowdesktop::external_transfer_observation::ObserveBudget(); },
                [&] { throw ole_driver::ObservationUnavailable("v4 collection leave requires sealed execution observer"); }};
            const auto oracle = [&](const ole_driver::Observation& event) {
                if (event.kind != ole_driver::Kind::enter && event.kind != ole_driver::Kind::leave)
                    throw ole_driver::ObservationUnavailable("v4 collection leave unexpected OLE event");
                if (event.kind == ole_driver::Kind::enter &&
                    (behavior::Access::Target(*profile.app) != behavior::Access::MemberContainer(*profile.app, L"ingress-root")))
                    throw ole_driver::ObservationUnavailable("v4 collection leave entered target unavailable");
                if (FAILED(event.result)) throw ole_driver::EventAssertionFailure("v4 collection leave OLE event failed");
                if (event.kind == ole_driver::Kind::enter && (!event.returnedEffect || *event.returnedEffect != DROPEFFECT_COPY))
                    throw ole_driver::EventAssertionFailure("v4 collection Ctrl Enter differs from the copy effect contract");
            };
            const ole_driver::V4LeaveObserverBoundaries leaveObserver{
                [&](const std::shared_ptr<ole_driver::V4LeaveExecutionKey>& key) {
                    observed.BindExecution(key, sourceOwner, pinnedCopyLease, hostBinding.second, verified, identity); },
                [&](const std::shared_ptr<ole_driver::V4LeaveExecutionKey>& key) {
                    observed.CaptureExecution(key, sourceOwner, pinnedCopyLease, hostBinding.second); }};
            dispatchAttempted = true;
            const auto receipt = ole_driver::V4MultiFileGraphExecutor::RunBound(verified, identity, present, selected, inputs,
                payload, binding, sourceOwner, hostBinding.first.Get(), hostBinding.second, host, oracle,
                {}, {}, pinnedCopyLease, {}, {}, {}, {}, leaveObserver);
            diagnostics.Stop(); result.v4DiagnosticBudget = snowdesktop::external_transfer_observation::ObserveBudget(); const auto& execution = receipt.Execution(); result.v4OleAdapterLogLast = execution.oleAdapterLogLast;
            result.productionStarted = execution.productionStarted; result.productionEntryUnknown = execution.productionEntryUnknown; dispatchAttempted = false;
            result.businessFailureObserved = execution.businessFailureObserved; result.eventOracleFailureObserved = execution.eventOracleFailureObserved;
            result.eventDispatchObservationRecorded = !execution.observations.empty();
            result.eventDispatchFailureObserved = std::any_of(execution.observations.begin(), execution.observations.end(),
                [](const ole_driver::Observation& event) { return FAILED(event.result); });
            if (result.businessFailureObserved || result.eventOracleFailureObserved) result.status = BridgeStatus::production_failed;
            result.failure = execution.failure;
            result.observationUnavailable = execution.additionalObservationUnavailable || execution.status == ole_driver::Status::observation_unavailable ||
                execution.status == ole_driver::Status::host_observation_failed || execution.status == ole_driver::Status::event_failed;
            result.obligations.insert(result.obligations.end(), execution.obligations.begin(), execution.obligations.end());
            result.postDispatchObligations.insert(result.postDispatchObligations.end(), execution.postconditionsPending.begin(), execution.postconditionsPending.end());
            result.unclassifiedObligations.insert(result.unclassifiedObligations.end(), execution.unclassifiedObligations.begin(), execution.unclassifiedObligations.end());
            result.v4BatchSourcePreparation = receipt.SourcePreparation(); result.v4BatchSourceAtExecution = receipt.SourceAtExecution();
            result.v4PinnedCopyLeaseInput->receiptPreflightObserved = receipt.PinnedCopyPreflightObserved();
            result.v4PinnedCopyLeaseInput->receiptBeforeLeaveObserved = receipt.PinnedCopyBeforeLeaveObserved();
            result.v4PinnedCopyLeaseInput->receiptAfterAwaitObserved = receipt.PinnedCopyAfterAwaitObserved();
            result.v4PinnedCopyLeaseInput->receiptSameLease = receipt.SamePinnedCopyLease(verified, identity, payload, sourceOwner, pinnedCopyLease);
            result.v4OleObservations = execution.observations; result.v4CapturedOleReturns = receipt.CapturedReturns();
            result.v4FileSourceInterval = receipt.SourceSessionInterval();
            if (receipt.CompletionTraceCaptured()) result.v4CompletionTrace = receipt.CompletionTrace();
            if (const auto sealed = receipt.SealedLeaveBusiness()) { result.v4LeaveBusiness = *sealed; result.v4LeaveBusinessSealed = true; }
            else result.v4LeaveBusiness = observed.Evidence();
            if (execution.productionStarted && behavior::Access::messageTrace)
            {
                try { result.v4LeaveHostObservation = ole_driver::V4LeaveHostObservation::Inspect(receipt, verified, identity,
                    payload, binding, sourceOwner, pinnedCopyLease, hostBinding.first.Get(), hostBinding.second,
                    *behavior::Access::messageTrace, behavior::Access::fixtureControlWindow);
                    if (result.v4LeaveHostObservation->observationUnavailable || result.v4LeaveHostObservation->resourceUnavailable ||
                        !result.v4LeaveHostObservation->unresolved.empty()) result.observationUnavailable = true;
                } catch (...) { result.observationUnavailable = true; }
                try { result.v4LeaveBusinessBinding = InspectV4LeaveBusiness(receipt, payload, binding, sourceOwner,
                    hostBinding.first.Get(), hostBinding.second, *behavior::Access::messageTrace, behavior::Access::fixtureControlWindow);
                    if (result.v4LeaveBusinessBinding->observationUnavailable || result.v4LeaveBusinessBinding->resourceUnavailable ||
                        !result.v4LeaveBusinessBinding->unresolved.empty()) result.observationUnavailable = true;
                } catch (...) { result.observationUnavailable = true; }
            }
        }
        catch (...) { try { if (!result.v4LeaveBusinessSealed) result.v4LeaveBusiness = observed.Evidence(); } catch (...) {} throw; }
    }
    catch (const std::exception& error) { if (dispatchAttempted && !result.productionStarted) result.productionEntryUnknown = true;
        result.observationUnavailable = true; if (result.failure.empty()) result.failure = error.what(); }
    catch (...) { if (dispatchAttempted && !result.productionStarted) result.productionEntryUnknown = true; result.observationUnavailable = true; }
    return result;
}

// A no-effect Drop uses its own terminal and sealed local observations.
inline BridgeResult PrepareV4DesktopSingleFileReject(
    const event_graph::V4FamilyIdentityContext& identity, const V4FamilyValidatedContext& verified,
    const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
    behavior::OwnedProfileDesktopRoot& ownedProfile, const std::shared_ptr<V4IndependentFilePayload>& payload,
    const V4IndependentShellSource& binding, const std::shared_ptr<V4IndependentShellOwner>& sourceOwner,
    const std::vector<ole_driver::Input>& inputs, const std::shared_ptr<const V4PinnedCopyLease>& pinnedCopyLease)
{
    BridgeResult result; bool dispatchAttempted = false;
    try
    {
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        const auto replay = event_graph::V4JoinedGraph::Replay(identity, verified, present, selected.id);
        const auto value = [&](std::string_view id) { return Value(verified.Axes(), verified.Coordinate(), id); };
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty() || !payload || payload->files.size() != 1 ||
            payload->status != V4IndependentFileStatus::input_prepared || payload->candidateId != raw.id || !sourceOwner ||
            !binding.inputPrepared || binding.resourceUnavailable || binding.candidateId != raw.id ||
            !binding.SameOwner(sourceOwner) || !binding.SamePayload(payload) || !ownedProfile.StillOwned() ||
            !ownedProfile.profile || !ownedProfile.profile->app || behavior::Access::v4IndependentShellOwner != sourceOwner ||
            !replay.identityValid || !replay.errors.empty() || replay.variant.id != selected.id ||
            replay.variant.classes != selected.classes || replay.variant.order != selected.order || replay.variant.unresolved != selected.unresolved ||
            value("V4.source.kind") != "file_only" || value("V4.source.file_count") != "one" ||
            value("V4.file.member0_kind") != "file" || value("V4.file.member0_identity") != "unique" ||
            value("T01") != "external_ingress" || value("T02.file_paths") != "yes" || value("T04.capability") != "enabled" ||
            value("G04.termination") != "drop" || value("G02.release") != "none" || value("G02.release_buttons") != "none" ||
            value("G02.enter_buttons") != "left" || value("G02.enter") != "none" ||
            value("G02.hover_first") != "na" || value("G02.hover_first_buttons") != "na" ||
            value("G02.hover_later") != "na" || value("G02.hover_later_buttons") != "na" ||
            value("T03") != "none" || value("T03.extra_protocol_bits") != "none" ||
            value("target.E01") != "free_desktop" || value("target.E02") != "none" || value("H01.region") != "empty_slot")
        { result.obligations.emplace_back("v4_reject_candidate_graph_or_source_unavailable"); return result; }
        auto& profile = *ownedProfile.profile;
        if (behavior::Access::productionDispatch != 1) behavior::Access::EnableProductionDispatch(*profile.app, false);
        if (!behavior::Access::fixtureControlWindow) behavior::Access::EnableFixtureControlWindow(*profile.app);
        behavior::Wait([&] { return behavior::Access::FileDropIdle(*profile.app) && behavior::Access::LayoutSaveIdle(*profile.app); });
        auto* source = binding.SourceObject();
        if (!source || !source->HasConstructionShell()) { result.obligations.emplace_back("v4_reject_source_unavailable"); return result; }
        result.v4PinnedCopyLeaseInput.emplace(); result.v4PinnedCopyLeaseInput->candidateId = raw.id;
        result.v4PinnedCopyLeaseInput->ownerBoundAndInitiallyObserved = pinnedCopyLease &&
            sourceOwner->MatchesPinnedCopyLease(verified, identity, payload, pinnedCopyLease);
        if (!result.v4PinnedCopyLeaseInput->ownerBoundAndInitiallyObserved)
        { result.obligations.emplace_back("v4_reject_pinned_source_lease_unavailable"); return result; }
        if (inputs.size() != 2 || inputs[0].kind != ole_driver::Kind::enter || inputs[1].kind != ole_driver::Kind::drop ||
            !inputs[0].screenPoint || !inputs[1].screenPoint || inputs[1].screenPoint->x != inputs[0].screenPoint->x ||
            inputs[1].screenPoint->y != inputs[0].screenPoint->y || inputs[0].programIndex == inputs[1].programIndex)
        { result.obligations.emplace_back("v4_reject_input_sequence_unavailable"); return result; }
        POINT client{inputs[0].screenPoint->x, inputs[0].screenPoint->y};
        if (!ScreenToClient(behavior::Access::completionWindow, &client))
        { result.obligations.emplace_back("v4_reject_client_point_unavailable"); return result; }
        const auto initial = behavior::Access::InspectTextDesktopInitial(*profile.app, profile.desktop / L"Sentinel.txt", client);
        const auto screen = behavior::Access::ExternalPoint(client);
        const bool geometryMatches = screen.x == inputs[0].screenPoint->x && screen.y == inputs[0].screenPoint->y &&
            value("target.E19") == "named" && !initial.pageName.empty() && value("H06.page_record") == "saved" &&
            initial.pageRecorded && value("H06.page_dimensions") == "valid" && initial.savedColumns && initial.savedRows &&
            *initial.savedColumns == initial.columns && *initial.savedRows == initial.rows && value("H07.occupancy") == "partial" &&
            EndpointSize(value("target.E23")) == initial.columns && EndpointSize(value("target.E24")) == initial.rows;
        result.v4RejectDesktopInput = BridgeResult::V4TextDesktopInput{
            raw.id, reinterpret_cast<std::uintptr_t>(profile.app), GetCurrentThreadId(), initial, geometryMatches};
        if (!geometryMatches) result.obligations.emplace_back("v4_reject_desktop_geometry_unavailable");
        const auto selectedCollection = value("C07");
        if (selectedCollection == "on" || selectedCollection == "off")
        {
            const bool actual = behavior::Access::ProgramCollectionEnabled(*profile.app);
            result.v4RejectProgramCollectionInput = BridgeResult::V4TextProgramCollectionInput{
                raw.id, reinterpret_cast<std::uintptr_t>(profile.app), GetCurrentThreadId(), actual, actual == (selectedCollection == "on")};
            if (!result.v4RejectProgramCollectionInput->candidateMatched) result.obligations.emplace_back("v4_reject_c07_mismatch");
        }
        else result.obligations.emplace_back("v4_reject_c07_unavailable");
        result.v4BatchSourcePreparation = ole_driver::InspectV4IndependentFileInputs(verified, identity, *payload, *source);
        if (!result.v4BatchSourcePreparation->captured || result.v4BatchSourcePreparation->resourceUnavailable)
        { result.obligations.emplace_back("v4_reject_source_snapshot_unavailable"); return result; }
        for (const auto& reason : result.v4BatchSourcePreparation->unresolved) result.obligations.push_back("v4_reject_source:" + reason);
        auto hostBinding = behavior::Access::CreateV3AdapterAndHostEpoch(*profile.app, source);
        if (!hostBinding.first || !hostBinding.second) { result.obligations.emplace_back("v4_reject_host_epoch_unavailable"); return result; }
        behavior::V4FileRejectObservation observed(ownedProfile, payload, binding, sourceOwner, verified, identity);
        try
        {
            const auto plan = ole_driver::BuildV4JoinedOleExecutionPlan(verified, identity, present, selected, inputs);
            result.obligations.insert(result.obligations.end(), plan.preconditions.begin(), plan.preconditions.end());
            result.graphUnresolved = plan.graphUnresolved; result.postDispatchObligations = plan.postDispatchObligations;
            result.unclassifiedObligations = plan.unclassified; result.outcomeExpectations = plan.postconditions; result.v4PresenceExpectations = plan.presence;
            if (!plan.graphIdentityValid || !plan.structurallyPlanned || !plan.unclassified.empty())
                result.obligations.emplace_back("v4_reject_graph_or_unclassified_capability_unavailable");
            for (const auto& reason : binding.unresolved) result.obligations.push_back("v4_reject_binding:" + reason);
            result.obligations.emplace_back("v4_reject_all_axes_target_hit_and_event_interval_unimplemented");
            result.obligations.emplace_back("v4_reject_late_work_and_complete_no_output_oracle_unimplemented");
            result.obligations.emplace_back("v4_reject_pre_drop_source_reads_causality_and_unobserved_reentry_unverified");
            result.v4RejectBusiness = observed.Evidence();
            if (!result.obligations.empty()) return result;
            if (!snowdesktop::external_transfer_observation::Enable())
                throw ole_driver::ObservationUnavailable("v4 reject diagnostics unavailable");
            struct StopDiagnostics { bool active = true; void Stop() noexcept { if (active) { snowdesktop::external_transfer_observation::Stop(); active = false; } }
                ~StopDiagnostics() { Stop(); } } diagnostics;
            const ole_driver::HostBoundaries host{
                [&](const ole_driver::Input& event) {
                    if (!result.v4RejectDesktopInput || !result.v4RejectDesktopInput->candidateMatched ||
                        !result.v4RejectProgramCollectionInput || !result.v4RejectProgramCollectionInput->candidateMatched ||
                        !ownedProfile.StillOwned() || !binding.SameOwner(sourceOwner) || !binding.SamePayload(payload) ||
                        binding.SourceObject() != source || !sourceOwner->MatchesPinnedCopyLease(verified, identity, payload, pinnedCopyLease) ||
                        behavior::Access::ProgramCollectionEnabled(*profile.app) != result.v4RejectProgramCollectionInput->actual)
                        throw ole_driver::ObservationUnavailable("v4 reject same-case input unavailable");
                    const auto now = behavior::Access::InspectTextDesktopInitial(*profile.app, profile.desktop / L"Sentinel.txt", client);
                    const auto& before = result.v4RejectDesktopInput->initial;
                    if (now.pageId != before.pageId || now.pageName != before.pageName || now.sentinelKey != before.sentinelKey ||
                        now.columns != before.columns || now.rows != before.rows || now.pageRecorded != before.pageRecorded ||
                        now.savedColumns != before.savedColumns || now.savedRows != before.savedRows ||
                        now.sentinelSpan.columns != before.sentinelSpan.columns || now.sentinelSpan.rows != before.sentinelSpan.rows ||
                        now.layoutRevision != before.layoutRevision || !behavior::Same(now.sentinelCell, before.sentinelCell) ||
                        !behavior::Same(now.targetCell, before.targetCell) || !EqualRect(&now.pageBounds, &before.pageBounds) ||
                        !EqualRect(&now.targetBounds, &before.targetBounds))
                        throw ole_driver::ObservationUnavailable("v4 reject desktop geometry changed");
                    if (event.kind == ole_driver::Kind::enter && (!event.screenPoint || event.screenPoint->x != screen.x || event.screenPoint->y != screen.y))
                        throw ole_driver::ObservationUnavailable("v4 reject Enter point changed");
                    if (event.kind == ole_driver::Kind::drop && (!event.screenPoint || event.screenPoint->x != screen.x || event.screenPoint->y != screen.y))
                        throw ole_driver::ObservationUnavailable("v4 reject Drop point changed");
                    result.v4RejectDesktopInput->matchingPreCallProgramIndices.push_back(event.programIndex);
                    result.v4RejectProgramCollectionInput->matchingPreCallProgramIndices.push_back(event.programIndex);
                    result.v4PinnedCopyLeaseInput->matchingPreCallProgramIndices.push_back(event.programIndex);
                    observed.BeforeOleCall(event);
                },
                [&](const ole_driver::Observation&) {},
                [&](const ole_driver::Observation& terminal) { observed.Await(terminal); diagnostics.Stop(); result.v4DiagnosticBudget = snowdesktop::external_transfer_observation::ObserveBudget(); },
                [&] { throw ole_driver::ObservationUnavailable("v4 reject requires sealed execution observer"); }};
            const auto oracle = [&](const ole_driver::Observation& event) {
                if (event.kind != ole_driver::Kind::enter && event.kind != ole_driver::Kind::drop)
                    throw ole_driver::ObservationUnavailable("v4 reject unexpected OLE event");
                if (event.kind == ole_driver::Kind::enter &&
                    (behavior::Access::Target(*profile.app) != behavior::Access::Desktop(*profile.app) ||
                     behavior::Access::Region(*profile.app) != HitRegion::Empty))
                    throw ole_driver::ObservationUnavailable("v4 reject entered target unavailable");
                if (FAILED(event.result) || !event.returnedEffect || *event.returnedEffect != DROPEFFECT_NONE)
                    throw ole_driver::EventAssertionFailure("v4 reject OLE result differs from the no-effect contract");
            };
            const ole_driver::V4RejectObserverBoundaries rejectObserver{
                [&](const std::shared_ptr<ole_driver::V4RejectExecutionKey>& key) {
                    observed.BindExecution(key, sourceOwner, pinnedCopyLease, hostBinding.second, verified, identity); },
                [&](const std::shared_ptr<ole_driver::V4RejectExecutionKey>& key) {
                    observed.CaptureExecution(key, sourceOwner, pinnedCopyLease, hostBinding.second); }};
            dispatchAttempted = true;
            const auto receipt = ole_driver::V4MultiFileGraphExecutor::RunBound(verified, identity, present, selected, inputs,
                payload, binding, sourceOwner, hostBinding.first.Get(), hostBinding.second, host, oracle,
                {}, {}, pinnedCopyLease, {}, {}, {}, {}, {}, rejectObserver);
            diagnostics.Stop(); result.v4DiagnosticBudget = snowdesktop::external_transfer_observation::ObserveBudget(); const auto& execution = receipt.Execution(); result.v4OleAdapterLogLast = execution.oleAdapterLogLast;
            result.productionStarted = execution.productionStarted; result.productionEntryUnknown = execution.productionEntryUnknown; dispatchAttempted = false;
            result.businessFailureObserved = execution.businessFailureObserved; result.eventOracleFailureObserved = execution.eventOracleFailureObserved;
            result.eventDispatchObservationRecorded = !execution.observations.empty();
            result.eventDispatchFailureObserved = std::any_of(execution.observations.begin(), execution.observations.end(),
                [](const ole_driver::Observation& event) { return FAILED(event.result); });
            if (result.businessFailureObserved || result.eventOracleFailureObserved) result.status = BridgeStatus::production_failed;
            result.failure = execution.failure;
            result.observationUnavailable = execution.additionalObservationUnavailable || execution.status == ole_driver::Status::observation_unavailable ||
                execution.status == ole_driver::Status::host_observation_failed || execution.status == ole_driver::Status::event_failed;
            result.obligations.insert(result.obligations.end(), execution.obligations.begin(), execution.obligations.end());
            result.postDispatchObligations.insert(result.postDispatchObligations.end(), execution.postconditionsPending.begin(), execution.postconditionsPending.end());
            result.unclassifiedObligations.insert(result.unclassifiedObligations.end(), execution.unclassifiedObligations.begin(), execution.unclassifiedObligations.end());
            result.v4BatchSourcePreparation = receipt.SourcePreparation(); result.v4BatchSourceAtExecution = receipt.SourceAtExecution();
            result.v4PinnedCopyLeaseInput->receiptPreflightObserved = receipt.PinnedCopyPreflightObserved();
            result.v4PinnedCopyLeaseInput->receiptBeforeDropObserved = receipt.PinnedCopyBeforeDropObserved();
            result.v4PinnedCopyLeaseInput->receiptAfterAwaitObserved = receipt.PinnedCopyAfterAwaitObserved();
            result.v4PinnedCopyLeaseInput->receiptSameLease = receipt.SamePinnedCopyLease(verified, identity, payload, sourceOwner, pinnedCopyLease);
            result.v4OleObservations = execution.observations; result.v4CapturedOleReturns = receipt.CapturedReturns();
            result.v4FileSourceInterval = receipt.SourceSessionInterval();
            if (receipt.CompletionTraceCaptured()) result.v4CompletionTrace = receipt.CompletionTrace();
            if (const auto sealed = receipt.SealedRejectBusiness()) { result.v4RejectBusiness = *sealed; result.v4RejectBusinessSealed = true; }
            else result.v4RejectBusiness = observed.Evidence();
            if (execution.productionStarted && behavior::Access::messageTrace)
            {
                try { result.v4RejectBusinessBinding = InspectV4RejectBusiness(receipt, payload, binding, sourceOwner,
                    hostBinding.first.Get(), hostBinding.second, *behavior::Access::messageTrace, behavior::Access::fixtureControlWindow);
                    if (result.v4RejectBusinessBinding->observationUnavailable || result.v4RejectBusinessBinding->resourceUnavailable ||
                        !result.v4RejectBusinessBinding->unresolved.empty()) result.observationUnavailable = true;
                } catch (...) { result.observationUnavailable = true; }
            }
        }
        catch (...) { try { if (!result.v4RejectBusinessSealed) result.v4RejectBusiness = observed.Evidence(); } catch (...) {} throw; }
    }
    catch (const std::exception& error) { if (dispatchAttempted && !result.productionStarted) result.productionEntryUnknown = true;
        result.observationUnavailable = true; if (result.failure.empty()) result.failure = error.what(); }
    catch (...) { if (dispatchAttempted && !result.productionStarted) result.productionEntryUnknown = true; result.observationUnavailable = true; }
    return result;
}

// MOVE-capable source: the actual product decides its action.
inline BridgeResult PrepareV4DesktopSingleFileMoveCapable(
    const event_graph::V4FamilyIdentityContext& identity, const V4FamilyValidatedContext& verified,
    const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
    behavior::OwnedProfileDesktopRoot& ownedProfile, const std::shared_ptr<V4IndependentFilePayload>& payload,
    const V4IndependentShellSource& binding, const std::shared_ptr<V4IndependentShellOwner>& sourceOwner,
    const std::vector<ole_driver::Input>& inputs, const std::shared_ptr<const V4SharedMoveLease>& sharedMoveLease)
{
    BridgeResult result; bool dispatchAttempted = false;
    try
    {
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        const auto replay = event_graph::V4JoinedGraph::Replay(identity, verified, present, selected.id);
        const auto value = [&](std::string_view id) { return Value(verified.Axes(), verified.Coordinate(), id); };
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty() || !payload || payload->files.size() != 1 ||
            payload->status != V4IndependentFileStatus::input_prepared || payload->candidateId != raw.id || !sourceOwner ||
            !binding.inputPrepared || binding.resourceUnavailable || binding.candidateId != raw.id ||
            !binding.SameOwner(sourceOwner) || !binding.SamePayload(payload) || !ownedProfile.StillOwned() ||
            !ownedProfile.profile || !ownedProfile.profile->app || behavior::Access::v4IndependentShellOwner != sourceOwner ||
            !replay.identityValid || !replay.errors.empty() || replay.variant.id != selected.id ||
            replay.variant.classes != selected.classes || replay.variant.order != selected.order || replay.variant.unresolved != selected.unresolved ||
            value("V4.source.kind") != "file_only" || value("V4.source.file_count") != "one" ||
            value("V4.file.member0_kind") != "file" || value("V4.file.member0_identity") != "unique" ||
            value("T01") != "external_ingress" || value("T02.file_paths") != "yes" || value("T04.capability") != "enabled" ||
            value("G04.termination") != "drop" || value("G02.release") != "none" || value("G02.release_buttons") != "none" ||
            value("G02.enter_buttons") != "left" || value("G02.enter") != "none" ||
            value("G02.hover_first") != "na" || value("G02.hover_first_buttons") != "na" ||
            value("G02.hover_later") != "na" || value("G02.hover_later_buttons") != "na" ||
            value("T03") != "copy_move" || value("T03.extra_protocol_bits") != "none" ||
            value("target.E01") != "free_desktop" || value("target.E02") != "none" || value("H01.region") != "empty_slot")
        { result.obligations.emplace_back("v4_move_candidate_graph_or_source_unavailable"); return result; }
        auto& profile = *ownedProfile.profile;
        if (behavior::Access::productionDispatch != 1) behavior::Access::EnableProductionDispatch(*profile.app, false);
        if (!behavior::Access::fixtureControlWindow) behavior::Access::EnableFixtureControlWindow(*profile.app);
        behavior::Wait([&] { return behavior::Access::FileDropIdle(*profile.app) && behavior::Access::LayoutSaveIdle(*profile.app); });
        auto* source = binding.SourceObject();
        if (!source || !source->HasConstructionShell()) { result.obligations.emplace_back("v4_move_source_unavailable"); return result; }
        result.v4SharedMoveInput.emplace(); result.v4SharedMoveInput->candidateId = raw.id;
        result.v4SharedMoveInput->ownerBoundAndInitiallyObserved = sharedMoveLease &&
            sourceOwner->MatchesSharedMoveLeaseBefore(verified, identity, payload, sharedMoveLease);
        if (!result.v4SharedMoveInput->ownerBoundAndInitiallyObserved)
        { result.obligations.emplace_back("v4_move_pinned_source_lease_unavailable"); return result; }
        if (inputs.size() != 2 || inputs[0].kind != ole_driver::Kind::enter || inputs[1].kind != ole_driver::Kind::drop ||
            !inputs[0].screenPoint || !inputs[1].screenPoint || inputs[1].screenPoint->x != inputs[0].screenPoint->x ||
            inputs[1].screenPoint->y != inputs[0].screenPoint->y || inputs[0].programIndex == inputs[1].programIndex)
        { result.obligations.emplace_back("v4_move_input_sequence_unavailable"); return result; }
        POINT client{inputs[0].screenPoint->x, inputs[0].screenPoint->y};
        if (!ScreenToClient(behavior::Access::completionWindow, &client))
        { result.obligations.emplace_back("v4_move_client_point_unavailable"); return result; }
        const auto initial = behavior::Access::InspectTextDesktopInitial(*profile.app, profile.desktop / L"Sentinel.txt", client);
        const auto screen = behavior::Access::ExternalPoint(client);
        const bool geometryMatches = screen.x == inputs[0].screenPoint->x && screen.y == inputs[0].screenPoint->y &&
            value("target.E19") == "named" && !initial.pageName.empty() && value("H06.page_record") == "saved" &&
            initial.pageRecorded && value("H06.page_dimensions") == "valid" && initial.savedColumns && initial.savedRows &&
            *initial.savedColumns == initial.columns && *initial.savedRows == initial.rows && value("H07.occupancy") == "partial" &&
            EndpointSize(value("target.E23")) == initial.columns && EndpointSize(value("target.E24")) == initial.rows;
        result.v4MoveDesktopInput = BridgeResult::V4TextDesktopInput{
            raw.id, reinterpret_cast<std::uintptr_t>(profile.app), GetCurrentThreadId(), initial, geometryMatches};
        if (!geometryMatches) result.obligations.emplace_back("v4_move_desktop_geometry_unavailable");
        const auto selectedCollection = value("C07");
        if (selectedCollection == "on" || selectedCollection == "off")
        {
            const bool actual = behavior::Access::ProgramCollectionEnabled(*profile.app);
            result.v4MoveProgramCollectionInput = BridgeResult::V4TextProgramCollectionInput{
                raw.id, reinterpret_cast<std::uintptr_t>(profile.app), GetCurrentThreadId(), actual, actual == (selectedCollection == "on")};
            if (!result.v4MoveProgramCollectionInput->candidateMatched) result.obligations.emplace_back("v4_move_c07_mismatch");
        }
        else result.obligations.emplace_back("v4_move_c07_unavailable");
        result.v4BatchSourcePreparation = ole_driver::InspectV4IndependentFileInputs(verified, identity, *payload, *source);
        if (!result.v4BatchSourcePreparation->captured || result.v4BatchSourcePreparation->resourceUnavailable)
        { result.obligations.emplace_back("v4_move_source_snapshot_unavailable"); return result; }
        for (const auto& reason : result.v4BatchSourcePreparation->unresolved) result.obligations.push_back("v4_move_source:" + reason);
        auto hostBinding = behavior::Access::CreateV3AdapterAndHostEpoch(*profile.app, source);
        if (!hostBinding.first || !hostBinding.second) { result.obligations.emplace_back("v4_move_host_epoch_unavailable"); return result; }
        behavior::V4FileMoveObservation observed(ownedProfile, payload, binding, sourceOwner, sharedMoveLease, verified, identity);
        try
        {
            const auto plan = ole_driver::BuildV4JoinedOleExecutionPlan(verified, identity, present, selected, inputs);
            result.obligations.insert(result.obligations.end(), plan.preconditions.begin(), plan.preconditions.end());
            result.graphUnresolved = plan.graphUnresolved; result.postDispatchObligations = plan.postDispatchObligations;
            result.unclassifiedObligations = plan.unclassified; result.outcomeExpectations = plan.postconditions; result.v4PresenceExpectations = plan.presence;
            if (!plan.graphIdentityValid || !plan.structurallyPlanned || !plan.unclassified.empty())
                result.obligations.emplace_back("v4_move_graph_or_unclassified_capability_unavailable");
            for (const auto& reason : binding.unresolved) result.obligations.push_back("v4_move_binding:" + reason);
            result.obligations.emplace_back("v4_move_all_axes_target_hit_and_event_interval_unimplemented");
            result.obligations.emplace_back("v4_move_actual_action_completion_file_identity_and_business_unimplemented");
            result.obligations.emplace_back("v4_move_volume_decision_and_shell_open_identity_unverified");
            result.v4MoveBusiness = observed.Evidence();
            if (!result.obligations.empty()) return result;
            if (!snowdesktop::external_transfer_observation::Enable())
                throw ole_driver::ObservationUnavailable("v4 move diagnostics unavailable");
            struct StopDiagnostics { bool active = true; void Stop() noexcept { if (active) { snowdesktop::external_transfer_observation::Stop(); active = false; } }
                ~StopDiagnostics() { Stop(); } } diagnostics;
            const ole_driver::HostBoundaries host{
                [&](const ole_driver::Input& event) {
                    if (!result.v4MoveDesktopInput || !result.v4MoveDesktopInput->candidateMatched ||
                        !result.v4MoveProgramCollectionInput || !result.v4MoveProgramCollectionInput->candidateMatched ||
                        !ownedProfile.StillOwned() || !binding.SameOwner(sourceOwner) || !binding.SamePayload(payload) ||
                        binding.SourceObject() != source || !sourceOwner->MatchesSharedMoveLeaseBefore(verified, identity, payload, sharedMoveLease) ||
                        behavior::Access::ProgramCollectionEnabled(*profile.app) != result.v4MoveProgramCollectionInput->actual)
                        throw ole_driver::ObservationUnavailable("v4 move same-case input unavailable");
                    const auto now = behavior::Access::InspectTextDesktopInitial(*profile.app, profile.desktop / L"Sentinel.txt", client);
                    const auto& before = result.v4MoveDesktopInput->initial;
                    if (now.pageId != before.pageId || now.pageName != before.pageName || now.sentinelKey != before.sentinelKey ||
                        now.columns != before.columns || now.rows != before.rows || now.pageRecorded != before.pageRecorded ||
                        now.savedColumns != before.savedColumns || now.savedRows != before.savedRows ||
                        now.sentinelSpan.columns != before.sentinelSpan.columns || now.sentinelSpan.rows != before.sentinelSpan.rows ||
                        now.layoutRevision != before.layoutRevision || !behavior::Same(now.sentinelCell, before.sentinelCell) ||
                        !behavior::Same(now.targetCell, before.targetCell) || !EqualRect(&now.pageBounds, &before.pageBounds) ||
                        !EqualRect(&now.targetBounds, &before.targetBounds))
                        throw ole_driver::ObservationUnavailable("v4 move desktop geometry changed");
                    if (event.kind == ole_driver::Kind::enter && (!event.screenPoint || event.screenPoint->x != screen.x || event.screenPoint->y != screen.y))
                        throw ole_driver::ObservationUnavailable("v4 move Enter point changed");
                    if (event.kind == ole_driver::Kind::drop && (!event.screenPoint || event.screenPoint->x != screen.x || event.screenPoint->y != screen.y))
                        throw ole_driver::ObservationUnavailable("v4 move Drop point changed");
                    result.v4MoveDesktopInput->matchingPreCallProgramIndices.push_back(event.programIndex);
                    result.v4MoveProgramCollectionInput->matchingPreCallProgramIndices.push_back(event.programIndex);
                    result.v4SharedMoveInput->matchingPreCallProgramIndices.push_back(event.programIndex);
                    observed.BeforeOleCall(event);
                },
                [&](const ole_driver::Observation&) {},
                [&](const ole_driver::Observation& terminal) { observed.Await(terminal); diagnostics.Stop(); result.v4DiagnosticBudget = snowdesktop::external_transfer_observation::ObserveBudget(); },
                [&] { throw ole_driver::ObservationUnavailable("v4 move requires sealed execution observer"); }};
            const auto oracle = [&](const ole_driver::Observation& event) {
                if (event.kind != ole_driver::Kind::enter && event.kind != ole_driver::Kind::drop)
                    throw ole_driver::ObservationUnavailable("v4 move unexpected OLE event");
                if (event.kind == ole_driver::Kind::enter &&
                    (behavior::Access::Target(*profile.app) != behavior::Access::Desktop(*profile.app) ||
                     behavior::Access::Region(*profile.app) != HitRegion::Empty))
                    throw ole_driver::ObservationUnavailable("v4 move entered target unavailable");
                if (!event.returnedEffect) throw ole_driver::ObservationUnavailable("v4 move returned effect unavailable");
            };
            const ole_driver::V4MoveObserverBoundaries moveObserver{
                [&](const std::shared_ptr<ole_driver::V4MoveExecutionKey>& key) {
                    observed.BindExecution(key, sourceOwner, sharedMoveLease, hostBinding.second, verified, identity); },
                [&](const std::shared_ptr<ole_driver::V4MoveExecutionKey>& key) {
                    observed.CaptureExecution(key, sourceOwner, sharedMoveLease, hostBinding.second); }};
            dispatchAttempted = true;
            const auto receipt = ole_driver::V4MultiFileGraphExecutor::RunBound(verified, identity, present, selected, inputs,
                payload, binding, sourceOwner, hostBinding.first.Get(), hostBinding.second, host, oracle,
                {}, {}, {}, {}, {}, {}, {}, {}, {}, sharedMoveLease, moveObserver);
            diagnostics.Stop(); result.v4DiagnosticBudget = snowdesktop::external_transfer_observation::ObserveBudget(); const auto& execution = receipt.Execution(); result.v4OleAdapterLogLast = execution.oleAdapterLogLast;
            result.productionStarted = execution.productionStarted; result.productionEntryUnknown = execution.productionEntryUnknown; dispatchAttempted = false;
            result.businessFailureObserved = execution.businessFailureObserved; result.eventOracleFailureObserved = execution.eventOracleFailureObserved;
            result.eventDispatchObservationRecorded = !execution.observations.empty();
            result.eventDispatchFailureObserved = std::any_of(execution.observations.begin(), execution.observations.end(),
                [](const ole_driver::Observation& event) { return FAILED(event.result); });
            if (result.businessFailureObserved || result.eventOracleFailureObserved) result.status = BridgeStatus::production_failed;
            result.failure = execution.failure;
            result.observationUnavailable = execution.additionalObservationUnavailable || execution.status == ole_driver::Status::observation_unavailable ||
                execution.status == ole_driver::Status::host_observation_failed || execution.status == ole_driver::Status::event_failed;
            result.obligations.insert(result.obligations.end(), execution.obligations.begin(), execution.obligations.end());
            result.postDispatchObligations.insert(result.postDispatchObligations.end(), execution.postconditionsPending.begin(), execution.postconditionsPending.end());
            result.unclassifiedObligations.insert(result.unclassifiedObligations.end(), execution.unclassifiedObligations.begin(), execution.unclassifiedObligations.end());
            result.v4BatchSourcePreparation = receipt.SourcePreparation(); result.v4BatchSourceAtExecution = receipt.SourceAtExecution();
            result.v4SharedMoveInput->receiptBeforeDropObserved = receipt.SharedMoveBeforeDropObserved();
            result.v4SharedMoveInput->afterAwait = receipt.SharedMoveAfterAwait();
            result.v4SharedMoveInput->receiptSameLease = receipt.SameSharedMoveLease(payload, sourceOwner, sharedMoveLease);
            result.v4OleObservations = execution.observations; result.v4CapturedOleReturns = receipt.CapturedReturns();
            result.v4FileSourceInterval = receipt.SourceSessionInterval();
            if (receipt.CompletionTraceCaptured()) result.v4CompletionTrace = receipt.CompletionTrace();
            if (const auto sealed = receipt.SealedMoveBusiness()) { result.v4MoveBusiness = *sealed; result.v4MoveBusinessSealed = true; }
            else result.v4MoveBusiness = observed.Evidence();
            if (execution.productionStarted && behavior::Access::messageTrace)
            {
                try {
                    result.v4TrackedShellMove = ole_driver::InspectV4MultiTrackedShellMove(receipt, verified, identity, present, selected,
                        payload, binding, sourceOwner, hostBinding.first.Get(), hostBinding.second, *behavior::Access::messageTrace,
                        behavior::Access::fixtureControlWindow, kShellFileOperationCompletedMessage);
                    if (result.v4TrackedShellMove->observationUnavailable || result.v4TrackedShellMove->resourceUnavailable ||
                        !result.v4TrackedShellMove->unresolved.empty()) result.observationUnavailable = true;
                } catch (...) { result.observationUnavailable = true; }
                try {
                    auto& local = result.v4MoveBusinessBinding.emplace();
                    const auto sealed = receipt.SealedMoveBusiness();
                    if (sealed && sealed->candidateId == raw.id && receipt.SameBoundHost(binding, sourceOwner,
                        hostBinding.first.Get(), hostBinding.second, *behavior::Access::messageTrace,
                        behavior::Access::fixtureControlWindow) && receipt.SameSharedMoveLease(payload, sourceOwner, sharedMoveLease) &&
                        receipt.SourceAtExecution() && receipt.SourceAtExecution()->captured &&
                        receipt.SourceAtExecution()->files.size() == 1 && receipt.SourceAtExecution()->candidateId == raw.id)
                    {
                        local.sameReceiptObserved = true;
                        const auto& initial = receipt.SourceAtExecution()->files.front();
                        if (sealed->sourceId) local.sourceSnapshotMatches = sealed->source == initial.path &&
                            sealed->expectedBytes == initial.bytes && behavior::OwnedProfileDesktopRoot::SameId(*sealed->sourceId, initial.id);
                        local.outputBytesMatch = sealed->outputBytesMatch; local.outputSameSource = sealed->outputSameSource;
                        if (result.v4TrackedShellMove && result.v4TrackedShellMove->volumeDecisionObserved &&
                            result.v4TrackedShellMove->shellCallbackObserved && result.v4TrackedShellMove->localChain)
                        {
                            const auto& tracked = *result.v4TrackedShellMove;
                            const auto& chain = *tracked.localChain;
                            if (chain.readMessage < tracked.messages.size() && chain.fileMessage < tracked.messages.size() &&
                                chain.readMessage != chain.fileMessage)
                            {
                                const auto& read = tracked.messages[chain.readMessage]; const auto& file = tracked.messages[chain.fileMessage];
                                if (read.returned && file.returned && read.enteredSequence && file.enteredSequence)
                                {
                                    local.trackedMoveObserved = true; local.readCompletionEntered = read.enteredSequence;
                                    local.fileCompletionEntered = file.enteredSequence;
                                    local.completionObserved = receipt.CompletionTraceCaptured() && sealed->completion &&
                                        sealed->completion->signal == ole_driver::CompletionSignal::completed_messages &&
                                        sealed->completion->sourceEntered == read.enteredSequence &&
                                        sealed->completion->commitEntered == file.enteredSequence;
                                }
                            }
                        }
                    }
                    else local.unresolved.emplace_back("v4_move_sealed_receipt_source_unavailable");
                    local.unresolved.emplace_back("v4_move_shell_open_identity_late_work_and_full_business_unverified");
                    result.observationUnavailable = true;
                } catch (...) { result.observationUnavailable = true; }

            }
        }
        catch (...) { try { if (!result.v4MoveBusinessSealed) result.v4MoveBusiness = observed.Evidence(); } catch (...) {} throw; }
    }
    catch (const std::exception& error) { if (dispatchAttempted && !result.productionStarted) result.productionEntryUnknown = true;
        result.observationUnavailable = true; if (result.failure.empty()) result.failure = error.what(); }
    catch (...) { if (dispatchAttempted && !result.productionStarted) result.productionEntryUnknown = true; result.observationUnavailable = true; }
    return result;
}

// Batch preparation retains the exact generated payload and its Shell owner.
// The observer and executor consume the same profile/source; order is checked
// only against data actually returned to production, never against Make alone.
inline BridgeResult PrepareV4DesktopBatchCopy(
    const event_graph::V4FamilyIdentityContext& identity, const V4FamilyValidatedContext& verified,
    const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
    behavior::OwnedProfileDesktopRoot& ownedProfile, const std::shared_ptr<V4IndependentFilePayload>& payload,
    const V4IndependentShellSource& binding, const std::shared_ptr<V4IndependentShellOwner>& sourceOwner,
    GridCell landing, const std::vector<ole_driver::Input>& inputs,
    const std::shared_ptr<const V4PinnedCopyLease>& pinnedCopyLease = {})
{
    BridgeResult result;
    bool dispatchAttempted = false;
    try
    {
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        const auto replay = event_graph::V4JoinedGraph::Replay(identity, verified, present, selected.id);
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty() ||
            !replay.identityValid || !replay.errors.empty() || replay.variant.id != selected.id ||
            replay.variant.classes != selected.classes || replay.variant.order != selected.order ||
            replay.variant.unresolved != selected.unresolved || !ownedProfile.StillOwned() || !ownedProfile.profile ||
            !ownedProfile.profile->app || !payload || payload->status != V4IndependentFileStatus::input_prepared ||
            payload->candidateId != raw.id || payload->files.empty() || !sourceOwner ||
            !binding.inputPrepared || binding.resourceUnavailable || binding.candidateId != raw.id ||
            !binding.SameOwner(sourceOwner) || !binding.SamePayload(payload) ||
            behavior::Access::v4IndependentShellOwner != sourceOwner)
        { result.obligations.emplace_back("v4_batch_candidate_graph_or_fixture_unavailable"); return result; }
        if (payload->files.size() == 1)
        {
            const auto value = [&](std::string_view axis) { return Value(verified.Axes(), verified.Coordinate(), axis); };
            if (value("V4.source.kind") != "file_only" || value("V4.source.file_count") != "one" ||
                value("V4.file.member0_kind") != "file" || value("V4.file.member0_identity") != "unique" ||
                value("T01") != "external_ingress" || value("T02.file_paths") != "yes" ||
                value("T04.capability") != "enabled" || value("G04.termination") != "drop" ||
                value("target.E01") != "free_desktop" || value("target.E02") != "none" ||
                value("H01.region") != "empty_slot")
            { result.obligations.emplace_back("v4_single_owned_file_desktop_candidate_unavailable"); return result; }
            result.v4PinnedCopyLeaseInput.emplace();
            result.v4PinnedCopyLeaseInput->candidateId = raw.id;
            result.v4PinnedCopyLeaseInput->ownerBoundAndInitiallyObserved = pinnedCopyLease &&
                sourceOwner->MatchesPinnedCopyLease(verified, identity, payload, pinnedCopyLease);
            if (!result.v4PinnedCopyLeaseInput->ownerBoundAndInitiallyObserved)
            { result.obligations.emplace_back("v4_single_pinned_copy_owner_lease_unavailable"); return result; }
        }
        auto* source = binding.SourceObject();
        if (!source) { result.obligations.emplace_back("v4_batch_source_owner_unavailable"); return result; }
        const auto candidateValue = [&](std::string_view axis) { return Value(verified.Axes(), verified.Coordinate(), axis); };
        const bool restrictedDiagnostic = payload->files.size() == 1 && source->RestrictedFileDrop() &&
            source->RestrictedPreflightSucceeded() && source->formatProbes.size() == 1 &&
            candidateValue("T02.shell_object") == "no" && candidateValue("T02.virtual_files") == "no" &&
            candidateValue("T02.virtual_completeness") == "na" && candidateValue("T02.image") == "no" &&
            candidateValue("T02.text") == "no" && candidateValue("T02.url") == "no" &&
            candidateValue("T02.url_kind") == "na" && candidateValue("T02.invalid_format") == "no" &&
            candidateValue("T03") == "copy" && candidateValue("T03.extra_protocol_bits") == "none" &&
            candidateValue("G02.enter_buttons") == "left" && candidateValue("G02.enter") == "none" &&
            candidateValue("G02.release_buttons") == "none" && candidateValue("G02.release") == "none" &&
            candidateValue("G04.termination") == "drop" && candidateValue("target.E02") == "none" &&
            candidateValue("H01.region") == "empty_slot" && candidateValue("X03.task_count") == "one";
        auto& profile = *ownedProfile.profile;
        std::optional<POINT> singleTargetClient;
        std::optional<std::filesystem::path> singleDestination;
        const auto observeSingleDestination = [&]() -> std::pair<std::optional<bool>, DWORD> {
            if (!singleDestination || !ownedProfile.StillOwned()) return {std::nullopt, ERROR_INVALID_HANDLE};
            SetLastError(ERROR_SUCCESS);
            const DWORD attributes = GetFileAttributesW(singleDestination->c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES) return {true, ERROR_SUCCESS};
            const DWORD error = GetLastError();
            return error == ERROR_FILE_NOT_FOUND ? std::pair<std::optional<bool>, DWORD>{false, error} :
                std::pair<std::optional<bool>, DWORD>{std::nullopt, error};
        };
        if (payload->files.size() == 1)
        {
            const auto value = [&](std::string_view axis) { return Value(verified.Axes(), verified.Coordinate(), axis); };
            singleDestination = profile.desktop / payload->files.front().path.filename();
            const auto [destinationExists, queryError] = observeSingleDestination();
            result.v4BatchConflictInput = BridgeResult::V4FileConflictInput{
                raw.id, *singleDestination, GetCurrentThreadId(), queryError, destinationExists,
                destinationExists && !*destinationExists && value("R04.member0_target_exists") == "absent" &&
                    value("R04.member0_target_identity") == "na" && value("R04.member0_name_relation") == "na"};
            if (!result.v4BatchConflictInput->candidateMatched)
                result.obligations.emplace_back("v4_single_owned_file_conflict_input_unavailable");
            if (restrictedDiagnostic)
                result.postDispatchObligations.emplace_back("v4_single_owned_file_conflict_reservation_and_concurrent_creation_unverified");
            else result.obligations.emplace_back("v4_single_owned_file_conflict_reservation_and_concurrent_creation_unverified");
            for (const auto& input : inputs) if (input.kind == ole_driver::Kind::drop)
            {
                if (singleTargetClient || !input.screenPoint || !behavior::Access::completionWindow)
                { result.obligations.emplace_back("v4_single_owned_file_drop_point_unavailable"); break; }
                POINT point{input.screenPoint->x, input.screenPoint->y};
                if (!ScreenToClient(behavior::Access::completionWindow, &point))
                { result.obligations.emplace_back("v4_single_owned_file_client_point_unavailable"); break; }
                singleTargetClient = point;
            }
            if (!singleTargetClient) result.obligations.emplace_back("v4_single_owned_file_drop_point_unavailable");
            else
            {
                const auto initial = behavior::Access::InspectTextDesktopInitial(*profile.app,
                    profile.desktop / L"Sentinel.txt", *singleTargetClient);
                const auto screen = behavior::Access::ExternalPoint(*singleTargetClient);
                const bool sameScreen = std::all_of(inputs.begin(), inputs.end(), [&](const auto& input) {
                    return input.kind != ole_driver::Kind::leave && input.screenPoint &&
                        input.screenPoint->x == screen.x && input.screenPoint->y == screen.y; });
                const bool matches = sameScreen && value("target.E19") == "named" && !initial.pageName.empty() &&
                    value("H06.page_record") == "saved" && initial.pageRecorded && value("H06.page_dimensions") == "valid" &&
                    initial.savedColumns && initial.savedRows && *initial.savedColumns == initial.columns &&
                    *initial.savedRows == initial.rows && value("H07.occupancy") == "partial" &&
                    EndpointSize(value("target.E23")) == initial.columns && EndpointSize(value("target.E24")) == initial.rows &&
                    behavior::Same(initial.targetCell, landing);
                result.v4BatchDesktopInput = BridgeResult::V4TextDesktopInput{
                    raw.id, reinterpret_cast<std::uintptr_t>(profile.app), GetCurrentThreadId(), initial, matches};
                if (!matches) result.obligations.emplace_back("v4_single_owned_file_desktop_geometry_unavailable");
            }
            const auto selected = value("C07");
            if (selected == "on" || selected == "off")
            {
                const bool actual = behavior::Access::ProgramCollectionEnabled(*profile.app);
                result.v4BatchProgramCollectionInput = BridgeResult::V4TextProgramCollectionInput{
                    raw.id, reinterpret_cast<std::uintptr_t>(profile.app), GetCurrentThreadId(), actual,
                    actual == (selected == "on")};
                if (!result.v4BatchProgramCollectionInput->candidateMatched)
                    result.obligations.emplace_back("v4_single_owned_file_program_collection_mismatch");
            }
            else result.obligations.emplace_back("v4_single_owned_file_program_collection_unavailable");
        }
        result.v4BatchSourcePreparation = ole_driver::InspectV4IndependentFileInputs(verified, identity, *payload, *source);
        if (!result.v4BatchSourcePreparation->captured || result.v4BatchSourcePreparation->resourceUnavailable)
            result.obligations.emplace_back("v4_batch_source_snapshot_unavailable");
        for (const auto& reason : result.v4BatchSourcePreparation->unresolved)
            (restrictedDiagnostic ? result.postDispatchObligations : result.obligations).push_back("v4_batch_source:" + reason);
        auto hostBinding = behavior::Access::CreateV3AdapterAndHostEpoch(*profile.app, source);
        if (!hostBinding.first || !hostBinding.second)
        { result.obligations.emplace_back("v4_batch_host_or_adapter_unavailable"); return result; }
        behavior::V4BatchCopyObservation observed(profile, *source, payload, std::move(landing));
        try
        {
            const auto plan = ole_driver::BuildV4JoinedOleExecutionPlan(verified, identity, present, selected, inputs);
            if (restrictedDiagnostic)
                result.postDispatchObligations.insert(result.postDispatchObligations.end(), plan.preconditions.begin(), plan.preconditions.end());
            else result.obligations.insert(result.obligations.end(), plan.preconditions.begin(), plan.preconditions.end());
            result.graphUnresolved = plan.graphUnresolved;
            result.postDispatchObligations.insert(result.postDispatchObligations.end(), plan.postDispatchObligations.begin(), plan.postDispatchObligations.end());
            result.unclassifiedObligations = plan.unclassified;
            result.outcomeExpectations = plan.postconditions;
            result.v4PresenceExpectations = plan.presence;
            if (!plan.graphIdentityValid || (!restrictedDiagnostic && (!plan.structurallyPlanned || !plan.unclassified.empty())))
                result.obligations.emplace_back("v4_batch_graph_or_unclassified_capability_unavailable");
            for (const auto& reason : binding.unresolved)
                (restrictedDiagnostic ? result.postDispatchObligations : result.obligations).push_back("v4_batch_binding:" + reason);
            auto& unresolved = restrictedDiagnostic ? result.postDispatchObligations : result.obligations;
            unresolved.emplace_back("v4_batch_all_axes_geometry_and_same_run_oracle_unimplemented");
            unresolved.emplace_back("v4_batch_provider_order_and_complete_event_intervals_unimplemented");
            unresolved.emplace_back("v4_batch_actual_read_to_commit_order_binding_unimplemented");
            unresolved.emplace_back("v4_batch_same_receipt_task_window_diagnostic_interval_unimplemented");
            if (!result.obligations.empty()) { result.v4BatchCopyBusiness = observed.Evidence(); return result; }
            const ole_driver::HostBoundaries host{
                [&](const ole_driver::Input& event) {
                    if (payload->files.size() == 1)
                    {
                        if (event.kind == ole_driver::Kind::drop)
                        {
                            const auto [exists, error] = observeSingleDestination();
                            if (!result.v4BatchConflictInput || !result.v4BatchConflictInput->candidateMatched ||
                                result.v4BatchConflictInput->candidateId != raw.id ||
                                result.v4BatchConflictInput->ownerThread != GetCurrentThreadId() ||
                                !exists || *exists || error != ERROR_FILE_NOT_FOUND)
                                throw ole_driver::ObservationUnavailable("v4 single owned file destination changed before Drop");
                            result.v4BatchConflictInput->checkedBeforeDrop = true;
                        }
                        if (!singleTargetClient || !result.v4BatchDesktopInput || !result.v4BatchDesktopInput->candidateMatched ||
                            !result.v4BatchProgramCollectionInput || !result.v4BatchProgramCollectionInput->candidateMatched ||
                            !result.v4PinnedCopyLeaseInput || !result.v4PinnedCopyLeaseInput->ownerBoundAndInitiallyObserved ||
                            result.v4PinnedCopyLeaseInput->candidateId != raw.id ||
                            result.v4BatchDesktopInput->candidateId != raw.id ||
                            result.v4BatchDesktopInput->hostIdentity != reinterpret_cast<std::uintptr_t>(profile.app) ||
                            result.v4BatchDesktopInput->ownerThread != GetCurrentThreadId() || !ownedProfile.StillOwned() ||
                            !binding.SameOwner(sourceOwner) || !binding.SamePayload(payload) || binding.SourceObject() != source ||
                            !pinnedCopyLease || !sourceOwner->MatchesPinnedCopyLease(verified, identity, payload, pinnedCopyLease) ||
                            behavior::Access::ProgramCollectionEnabled(*profile.app) != result.v4BatchProgramCollectionInput->actual)
                            throw ole_driver::ObservationUnavailable("v4 single owned file same-host input unavailable");
                        const auto current = behavior::Access::InspectTextDesktopInitial(*profile.app,
                            profile.desktop / L"Sentinel.txt", *singleTargetClient);
                        const auto& before = result.v4BatchDesktopInput->initial;
                        if (current.pageId != before.pageId || current.pageName != before.pageName ||
                            current.sentinelKey != before.sentinelKey || current.columns != before.columns || current.rows != before.rows ||
                            current.pageRecorded != before.pageRecorded || current.savedColumns != before.savedColumns ||
                            current.savedRows != before.savedRows || current.sentinelSpan.columns != before.sentinelSpan.columns ||
                            current.sentinelSpan.rows != before.sentinelSpan.rows || current.layoutRevision != before.layoutRevision ||
                            !behavior::Same(current.sentinelCell, before.sentinelCell) || !behavior::Same(current.targetCell, before.targetCell) ||
                            !EqualRect(&current.pageBounds, &before.pageBounds) || !EqualRect(&current.targetBounds, &before.targetBounds))
                            throw ole_driver::ObservationUnavailable("v4 single owned file desktop input changed");
                        const auto screen = behavior::Access::ExternalPoint(*singleTargetClient);
                        if (!event.screenPoint || event.screenPoint->x != screen.x || event.screenPoint->y != screen.y)
                            throw ole_driver::ObservationUnavailable("v4 single owned file screen point changed");
                        result.v4BatchDesktopInput->matchingPreCallProgramIndices.push_back(event.programIndex);
                        result.v4BatchProgramCollectionInput->matchingPreCallProgramIndices.push_back(event.programIndex);
                        result.v4PinnedCopyLeaseInput->matchingPreCallProgramIndices.push_back(event.programIndex);
                    }
                    observed.BeforeOleCall(event);
                },
                [&](const ole_driver::Observation&) {},
                [&](const ole_driver::Observation& terminal) { observed.Await(terminal); },
                [&] {
                    if (payload->files.size() == 1)
                        throw ole_driver::ObservationUnavailable("v4 single owned file requires the sealed execution observer");
                    observed.AssertUnsealedResults();
                }};
            const auto oracle = [](const ole_driver::Observation& event) {
                if (event.kind != ole_driver::Kind::drop) return;
                if (!event.returnedEffect) throw ole_driver::ObservationUnavailable("v4 batch Drop effect unavailable");
                if (FAILED(event.result) || *event.returnedEffect != DROPEFFECT_COPY)
                    throw ole_driver::EventAssertionFailure("v4 batch Drop differs from the copy expectation");
            };
            ole_driver::V4BatchObserverBoundaries batchObserver;
            if (payload->files.size() == 1)
            {
                batchObserver.bindBeforeDrop = [&](const auto& key) {
                    observed.BindExecution(key, sourceOwner, hostBinding.second, ownedProfile); };
                batchObserver.observeBusiness = [&](const auto& key) {
                    observed.CaptureExecution(key, sourceOwner, hostBinding.second); };
            }
            if (!snowdesktop::external_transfer_observation::Enable())
                throw ole_driver::ObservationUnavailable("v4 batch transfer diagnostic session unavailable");
            struct StopBatchDiagnostics final
            {
                bool active = true;
                void Stop() noexcept { if (active) { snowdesktop::external_transfer_observation::Stop(); active = false; } }
                ~StopBatchDiagnostics() { Stop(); }
            } diagnostics;
            dispatchAttempted = true;
            const auto receipt = ole_driver::V4MultiFileGraphExecutor::RunBound(verified, identity, present,
                selected, inputs, payload, binding, sourceOwner, hostBinding.first.Get(), hostBinding.second, host, oracle, {}, batchObserver,
                payload->files.size() == 1 ? pinnedCopyLease : std::shared_ptr<const V4PinnedCopyLease>{});
            diagnostics.Stop(); result.v4DiagnosticBudget = snowdesktop::external_transfer_observation::ObserveBudget();
            const auto& execution = receipt.Execution(); result.v4OleAdapterLogLast = execution.oleAdapterLogLast;
            result.unattributedBusinessFailureObserved = execution.unattributedBusinessFailureObserved;
            result.unattributedEventOracleFailureObserved = execution.unattributedEventOracleFailureObserved;
            result.productionStarted = execution.productionStarted;
            result.productionEntryUnknown = execution.productionEntryUnknown;
            dispatchAttempted = false;
            result.businessFailureObserved = execution.businessFailureObserved;
            result.eventOracleFailureObserved = execution.eventOracleFailureObserved;
            result.eventDispatchObservationRecorded = !execution.observations.empty();
            result.eventDispatchFailureObserved = std::any_of(execution.observations.begin(), execution.observations.end(),
                [](const ole_driver::Observation& event) { return FAILED(event.result); });
            if (execution.businessFailureObserved || execution.eventOracleFailureObserved)
                result.status = BridgeStatus::production_failed;
            result.observationUnavailable = execution.additionalObservationUnavailable ||
                execution.status == ole_driver::Status::observation_unavailable ||
                execution.status == ole_driver::Status::host_observation_failed ||
                execution.status == ole_driver::Status::event_failed;
            result.failure = execution.failure;
            result.obligations.insert(result.obligations.end(), execution.obligations.begin(), execution.obligations.end());
            result.unclassifiedObligations.insert(result.unclassifiedObligations.end(),
                execution.unclassifiedObligations.begin(), execution.unclassifiedObligations.end());
            result.postDispatchObligations.insert(result.postDispatchObligations.end(),
                execution.postconditionsPending.begin(), execution.postconditionsPending.end());
            result.v4BatchSourcePreparation = receipt.SourcePreparation();
            result.v4BatchSourceAtExecution = receipt.SourceAtExecution();
            result.v4OleObservations = execution.observations;
            result.v4CapturedOleReturns = receipt.CapturedReturns();
            result.v4FileSourceInterval = receipt.SourceSessionInterval();
            if (receipt.CompletionTraceCaptured()) result.v4CompletionTrace = receipt.CompletionTrace();
            result.v4BatchCopyBusiness = observed.Evidence();
            if (result.v4PinnedCopyLeaseInput)
            {
                result.v4PinnedCopyLeaseInput->receiptPreflightObserved = receipt.PinnedCopyPreflightObserved();
                result.v4PinnedCopyLeaseInput->receiptBeforeDropObserved = receipt.PinnedCopyBeforeDropObserved();
                result.v4PinnedCopyLeaseInput->receiptAfterAwaitObserved = receipt.PinnedCopyAfterAwaitObserved();
                result.v4PinnedCopyLeaseInput->receiptSameLease = receipt.SamePinnedCopyLease(
                    verified, identity, payload, sourceOwner, pinnedCopyLease);
            }
            if (const auto sealed = receipt.SealedBusiness())
            { result.v4BatchCopyBusiness = *sealed; result.v4BatchBusinessSealed = true; }
            else if (payload->files.size() == 1 && execution.productionStarted) result.observationUnavailable = true;
            if (restrictedDiagnostic && execution.productionStarted && behavior::Access::messageTrace)
                result.v4RestrictedCopyBusinessVerdict = ole_driver::ClassifyV4RestrictedCopyBusiness(receipt,
                    verified, identity, present, selected, payload, binding, sourceOwner, pinnedCopyLease,
                    hostBinding.first.Get(), hostBinding.second, *behavior::Access::messageTrace,
                    behavior::Access::fixtureControlWindow, kShellFileOperationCompletedMessage, receipt.SealedBusiness());
            if (execution.productionStarted && behavior::Access::messageTrace)
            {
                try
                {
                    result.v4TrackedShellCopy = ole_driver::InspectV4MultiTrackedShellCopy(receipt, verified, identity,
                        present, selected, payload, binding, sourceOwner, hostBinding.first.Get(), hostBinding.second,
                        *behavior::Access::messageTrace, behavior::Access::fixtureControlWindow, kShellFileOperationCompletedMessage);
                    if (result.v4TrackedShellCopy->observationUnavailable || result.v4TrackedShellCopy->resourceUnavailable ||
                        !result.v4TrackedShellCopy->unresolved.empty()) result.observationUnavailable = true;
                }
                catch (...) { result.observationUnavailable = true; }
                try
                {
                    result.v4FilePathFlow = ole_driver::InspectV4MultiFilePathFlow(receipt, verified, identity, present,
                        selected, payload, binding, sourceOwner, hostBinding.first.Get(), hostBinding.second,
                        *behavior::Access::messageTrace, behavior::Access::fixtureControlWindow,
                        kShellFileOperationCompletedMessage);
                    if (result.v4FilePathFlow->observationUnavailable || !result.v4FilePathFlow->unresolved.empty())
                        result.observationUnavailable = true;
                }
                catch (...) { result.observationUnavailable = true; }
                if (payload->files.size() == 1)
                {
                    try
                    {
                        result.v4FileGraph = ole_driver::InspectV4MultiFileGraph(receipt, verified, identity, present, selected,
                            binding, sourceOwner, hostBinding.first.Get(), hostBinding.second, *behavior::Access::messageTrace,
                            behavior::Access::fixtureControlWindow, kShellFileOperationCompletedMessage);
                        result.hypothesisNotMetObserved = result.v4FileGraph->identityValid &&
                            result.v4FileGraph->hypothesisObservationRecorded && result.v4FileGraph->hypothesisNotMet;
                        if (result.v4FileGraph->observationUnavailable || !result.v4FileGraph->unresolved.empty())
                            result.observationUnavailable = true;
                    }
                    catch (...) { result.observationUnavailable = true; }
                    try
                    {
                        result.v4SingleOwnedFileBusinessBinding = InspectV4SingleOwnedFileBusiness(receipt, verified, identity,
                            present, selected, payload, binding, sourceOwner, hostBinding.first.Get(), hostBinding.second,
                            *behavior::Access::messageTrace, behavior::Access::fixtureControlWindow);
                        if (result.v4SingleOwnedFileBusinessBinding->observationUnavailable ||
                            result.v4SingleOwnedFileBusinessBinding->resourceUnavailable ||
                            !result.v4SingleOwnedFileBusinessBinding->unresolved.empty()) result.observationUnavailable = true;
                    }
                    catch (...) { result.observationUnavailable = true; }
                }
            }
            if (execution.productionStarted) result.obligations.emplace_back("v4_batch_full_graph_and_outcomes_unverified");
        }
        catch (...)
        {
            try { if (!result.v4BatchBusinessSealed) result.v4BatchCopyBusiness = observed.Evidence(); } catch (...) {}
            throw;
        }
    }
    catch (const std::exception& error)
    {
        if (dispatchAttempted && !result.productionStarted) result.productionEntryUnknown = true;
        result.observationUnavailable = true;
        if (result.failure.empty()) result.failure = error.what();
    }
    catch (...)
    {
        if (dispatchAttempted && !result.productionStarted) result.productionEntryUnknown = true;
        result.observationUnavailable = true;
        if (result.failure.empty()) result.failure = "v4_batch_preparation_or_observation_exception";
    }
    return result;
}

// D08: the independent expected result for the restricted complete-files +
// Unicode offer is a copy of the complete file batch, with no extra text
// output. Source preparation and this oracle never choose the product route.
inline BridgeResult PrepareV4MixedFileCopy(
    const event_graph::V4FamilyIdentityContext& identity, const V4FamilyValidatedContext& verified,
    const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
    behavior::OwnedProfileDesktopRoot& ownedProfile, const std::shared_ptr<V4IndependentFilePayload>& payload,
    const V4IndependentShellSource& fileBinding, const std::shared_ptr<V4IndependentShellOwner>& fileOwner,
    GridCell landing, const std::vector<ole_driver::Input>& inputs, std::size_t contentByteBudget,
    std::size_t virtualBudget)
{
    BridgeResult result;
    bool dispatchAttempted = false;
    try
    {
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        const auto replay = event_graph::V4JoinedGraph::Replay(identity, verified, present, selected.id);
        const auto& axes = verified.Axes(); const auto& candidate = verified.Coordinate();
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty() ||
            !replay.identityValid || !replay.errors.empty() || replay.variant.id != selected.id ||
            replay.variant.classes != selected.classes || replay.variant.order != selected.order ||
            replay.variant.unresolved != selected.unresolved || !ownedProfile.StillOwned() || !ownedProfile.profile ||
            !ownedProfile.profile->app || !payload || payload->status != V4IndependentFileStatus::input_prepared ||
            payload->files.empty() || payload->candidateId != raw.id || !fileOwner ||
            behavior::Access::v4IndependentShellOwner != fileOwner || !fileBinding.inputPrepared ||
            fileBinding.candidateId != raw.id || Value(axes, candidate, "V4.source.kind") != "file_and_content" ||
            Value(axes, candidate, "T02.file_paths") != "yes" || Value(axes, candidate, "T02.text") != "yes" ||
            Value(axes, candidate, "T01") != "external_ingress" || Value(axes, candidate, "G04.termination") != "drop" ||
            Value(axes, candidate, "target.E01") != "free_desktop")
        { result.obligations.emplace_back("v4_mixed_candidate_graph_or_file_fixture_unavailable"); return result; }
        auto* fileSource = fileBinding.SourceObject();
        if (!fileSource) { result.obligations.emplace_back("v4_mixed_file_source_unavailable"); return result; }
        auto& profile = *ownedProfile.profile;
        const auto mixedOwner = behavior::Access::CreateV4MixedOwner(*profile.app);
        const auto mixed = mixed_fixture::PrepareMixedSource(verified, identity, payload, fileBinding, fileOwner,
            mixedOwner, contentByteBudget, virtualBudget);
        auto* combined = mixed.SourceObject();
        result.v4MixedInput.emplace();
        result.v4MixedInput->candidateId = raw.id;
        result.v4MixedInput->combinedSource = reinterpret_cast<std::uintptr_t>(combined);
        result.v4MixedInput->fileSource = reinterpret_cast<std::uintptr_t>(fileSource);
        result.v4MixedInput->ownerThread = combined ? combined->OwnerThread() : 0;
        result.v4MixedInput->sourcePrepared = mixed.inputPrepared;
        result.v4MixedInput->resourceUnavailable = mixed.resourceUnavailable;
        result.v4MixedInput->unresolved = mixed.unresolved;
        if (!mixed.inputPrepared || mixed.resourceUnavailable || mixed.candidateId != raw.id || !combined ||
            combined->OwnerThread() != GetCurrentThreadId())
        {
            result.obligations.emplace_back(mixed.resourceUnavailable ? "v4_mixed_source_resource_unavailable" : "v4_mixed_source_unavailable");
            for (const auto& reason : mixed.unresolved) result.obligations.push_back("v4_mixed_source:" + reason);
            return result;
        }
        result.v4MixedInput->initialCallsObserved = combined->CopyCalls(result.v4MixedInput->calls) &&
            result.v4MixedInput->calls.empty();
        if (!result.v4MixedInput->initialCallsObserved)
            result.obligations.emplace_back("v4_mixed_source_call_baseline_unavailable");
        for (const auto& reason : mixed.unresolved) result.unclassifiedObligations.push_back("v4_mixed_source:" + reason);
        result.v4BatchSourcePreparation = ole_driver::InspectV4IndependentFileInputs(verified, identity, *payload, *fileSource, true);
        if (!result.v4BatchSourcePreparation->captured || result.v4BatchSourcePreparation->resourceUnavailable)
            result.obligations.emplace_back("v4_mixed_file_initial_snapshot_unavailable");
        for (const auto& reason : result.v4BatchSourcePreparation->unresolved)
            result.unclassifiedObligations.push_back("v4_mixed_file_source:" + reason);
        auto hostBinding = behavior::Access::CreateV3AdapterAndHostEpoch(*profile.app, combined);
        if (!hostBinding.first || !hostBinding.second)
        { result.obligations.emplace_back("v4_mixed_host_adapter_unavailable"); return result; }
        behavior::V4BatchCopyObservation observed(profile, *fileSource, payload, std::move(landing));
        result.v4BatchCopyBusiness = observed.Evidence();
        const auto plan = ole_driver::BuildV4JoinedOleExecutionPlan(verified, identity, present, selected, inputs);
        result.obligations.insert(result.obligations.end(), plan.preconditions.begin(), plan.preconditions.end());
        result.graphUnresolved = plan.graphUnresolved;
        result.postDispatchObligations = plan.postDispatchObligations;
        result.unclassifiedObligations.insert(result.unclassifiedObligations.end(), plan.unclassified.begin(), plan.unclassified.end());
        result.outcomeExpectations = plan.postconditions;
        result.v4PresenceExpectations = plan.presence;
        if (!plan.graphIdentityValid || !plan.structurallyPlanned || !plan.unclassified.empty())
            result.obligations.emplace_back("v4_mixed_graph_or_unclassified_capability_unavailable");
        result.obligations.emplace_back("v4_mixed_unicode_fixture_snapshot_and_all_axes_unimplemented");
        result.obligations.emplace_back("v4_mixed_full_format_selection_observation_capability_unimplemented");
        result.obligations.emplace_back("v4_mixed_same_run_completion_source_commit_and_business_unimplemented");
        result.obligations.emplace_back("v4_mixed_same_receipt_diagnostic_interval_unimplemented");
        if (!result.unclassifiedObligations.empty())
            result.obligations.emplace_back("v4_mixed_unclassified_obligation_capability_unimplemented");
        if (result.obligations.empty())
        {
            const ole_driver::HostBoundaries host{
                [&](const ole_driver::Input& event) { observed.BeforeOleCall(event); },
                [&](const ole_driver::Observation&) {},
                [&](const ole_driver::Observation& terminal) { observed.Await(terminal); },
                [&] { observed.AssertUnsealedResults(); }};
            const auto oracle = [](const ole_driver::Observation& event) {
                if (event.kind != ole_driver::Kind::drop) return;
                if (!event.returnedEffect) throw ole_driver::ObservationUnavailable("v4 mixed Drop effect unavailable");
                if (FAILED(event.result) || *event.returnedEffect != DROPEFFECT_COPY)
                    throw ole_driver::EventAssertionFailure("v4 mixed Drop differs from copy expectation");
            };
            try
            {
                if (!snowdesktop::external_transfer_observation::Enable())
                    throw ole_driver::ObservationUnavailable("v4 mixed diagnostic session unavailable");
                struct StopMixedDiagnostics final
                {
                    bool active = true;
                    void Stop() noexcept { if (active) { snowdesktop::external_transfer_observation::Stop(); active = false; } }
                    ~StopMixedDiagnostics() { Stop(); }
                } diagnostics;
                dispatchAttempted = true;
                const auto receipt = ole_driver::V4MixedGraphExecutor::RunBound(verified, identity, present, selected,
                    inputs, payload, fileBinding, fileOwner, mixed, mixedOwner, hostBinding.first.Get(), hostBinding.second, host, oracle);
                diagnostics.Stop(); result.v4DiagnosticBudget = snowdesktop::external_transfer_observation::ObserveBudget();
                const auto& execution = receipt.Execution(); result.v4OleAdapterLogLast = execution.oleAdapterLogLast;
                result.productionStarted = execution.productionStarted;
                result.productionEntryUnknown = execution.productionEntryUnknown;
                dispatchAttempted = false;
                result.businessFailureObserved = execution.businessFailureObserved;
                result.eventOracleFailureObserved = execution.eventOracleFailureObserved;
                result.eventDispatchObservationRecorded = !execution.observations.empty();
                result.eventDispatchFailureObserved = std::any_of(execution.observations.begin(), execution.observations.end(),
                    [](const ole_driver::Observation& event) { return FAILED(event.result); });
                if (execution.businessFailureObserved || execution.eventOracleFailureObserved)
                    result.status = BridgeStatus::production_failed;
                result.observationUnavailable = execution.additionalObservationUnavailable ||
                    execution.status == ole_driver::Status::observation_unavailable ||
                    execution.status == ole_driver::Status::host_observation_failed || execution.status == ole_driver::Status::event_failed;
                result.failure = execution.failure;
                result.obligations.insert(result.obligations.end(), execution.obligations.begin(), execution.obligations.end());
                result.postDispatchObligations.insert(result.postDispatchObligations.end(),
                    execution.postconditionsPending.begin(), execution.postconditionsPending.end());
                result.unclassifiedObligations.insert(result.unclassifiedObligations.end(),
                    execution.unclassifiedObligations.begin(), execution.unclassifiedObligations.end());
                result.v4BatchSourcePreparation = receipt.FileInitial(); result.v4BatchSourceAtExecution = receipt.FileAtExecution();
                result.v4OleObservations = execution.observations; result.v4CapturedOleReturns = receipt.CapturedReturns();
                result.v4MixedProviderBaselineCaptured = receipt.ProviderBaselineCaptured();
                result.v4MixedBaselineCalls = receipt.BaselineMixedCalls();
                result.v4MixedBaselineFileReads = receipt.BaselineFileReads();
                result.v4MixedBaselineContentCalls = receipt.BaselineContentCalls();
                if (receipt.CompletionTraceCaptured()) result.v4CompletionTrace = receipt.CompletionTrace();
                if (execution.productionStarted)
                {
                    result.v4MixedProviderRecordsCaptured = receipt.ProviderRecordsCaptured();
                    result.v4MixedCalls = receipt.MixedCalls(); result.v4MixedFileReads = receipt.FileReads();
                    result.v4MixedContentCalls = receipt.ContentCalls();
                    if (!receipt.ProviderRecordsCaptured()) result.observationUnavailable = true;
                    try
                    {
                        if (!behavior::Access::messageTrace)
                            throw ole_driver::ObservationUnavailable("v4 mixed graph trace unavailable");
                        result.v4MixedGraph = ole_driver::InspectV4MixedGraphWithTask(receipt, verified, identity, present,
                            selected, fileBinding, fileOwner, mixed, mixedOwner, hostBinding.first.Get(), hostBinding.second,
                            *behavior::Access::messageTrace, behavior::Access::fixtureControlWindow, kShellFileOperationCompletedMessage);
                        result.hypothesisNotMetObserved = result.v4MixedGraph->identityValid &&
                            result.v4MixedGraph->hypothesisObservationRecorded && result.v4MixedGraph->hypothesisNotMet;
                        if (result.v4MixedGraph->observationUnavailable || !result.v4MixedGraph->unresolved.empty())
                            result.observationUnavailable = true;
                    }
                    catch (...) { result.observationUnavailable = true; }
                }
                result.v4BatchCopyBusiness = observed.Evidence();
                if (execution.productionStarted) result.obligations.emplace_back("v4_mixed_full_graph_and_outcomes_unverified");
            }
            catch (...)
            {
                try { result.v4BatchCopyBusiness = observed.Evidence(); } catch (...) {}
                throw;
            }
        }
    }
    catch (const std::exception& error)
    { if (dispatchAttempted && !result.productionStarted) result.productionEntryUnknown = true;
      result.observationUnavailable = true; if (result.failure.empty()) result.failure = error.what(); }
    catch (...)
    { if (dispatchAttempted && !result.productionStarted) result.productionEntryUnknown = true;
      result.observationUnavailable = true; if (result.failure.empty()) result.failure = "v4_mixed_preparation_unavailable"; }
    return result;
}

// Restricts the candidate to a complete synchronous virtual-file manifest.
// The candidate supplies names and bytes only; the host decides whether to
// request/materialize them. Actual staging and production calls require an
// independently established exclusive per-process staging root.
inline BridgeResult PrepareV4VirtualCopy(
    const event_graph::V4FamilyIdentityContext& identity, const V4FamilyValidatedContext& verified,
    const std::vector<bool>& present, const event_graph::V4JoinedGraphVariant& selected,
    behavior::OwnedProfileDesktopRoot& ownedProfile, GridCell landing,
    const std::vector<ole_driver::Input>& inputs, std::size_t contentByteBudget, std::size_t virtualBudget)
{
    BridgeResult result;
    bool dispatchAttempted = false;
    try
    {
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        const auto replay = event_graph::V4JoinedGraph::Replay(identity, verified, present, selected.id);
        const auto& axes = verified.Axes(); const auto& candidate = verified.Coordinate();
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty() ||
            !replay.identityValid || !replay.errors.empty() || replay.variant.id != selected.id ||
            replay.variant.classes != selected.classes || replay.variant.order != selected.order ||
            replay.variant.unresolved != selected.unresolved || !ownedProfile.StillOwned() || !ownedProfile.profile ||
            !ownedProfile.profile->app || Value(axes, candidate, "V4.source.kind") != "content_only" ||
            Value(axes, candidate, "T01") != "external_ingress" || Value(axes, candidate, "G04.termination") != "drop" ||
            Value(axes, candidate, "target.E01") != "free_desktop")
        { result.obligations.emplace_back("v4_virtual_candidate_graph_or_host_unavailable"); return result; }
        result.v4VirtualManifest = mixed_fixture::BuildVirtualExpectedManifest(verified, identity, contentByteBudget, virtualBudget);
        if (!result.v4VirtualManifest->ready || result.v4VirtualManifest->resourceUnavailable || result.v4VirtualManifest->candidateId != raw.id)
        {
            result.obligations.emplace_back(result.v4VirtualManifest->resourceUnavailable ?
                "v4_virtual_manifest_resource_unavailable" : "v4_virtual_manifest_unavailable");
            for (const auto& reason : result.v4VirtualManifest->unresolved) result.obligations.push_back("v4_virtual_manifest:" + reason);
            return result;
        }
        auto& profile = *ownedProfile.profile;
        if (behavior::Access::productionDispatch != 1) behavior::Access::EnableProductionDispatch(*profile.app, false);
        if (!behavior::Access::fixtureControlWindow) behavior::Access::EnableFixtureControlWindow(*profile.app);
        behavior::Wait([&] { return behavior::Access::FileDropIdle(*profile.app) && behavior::Access::LayoutSaveIdle(*profile.app); });
        std::optional<POINT> proposedClient;
        for (const auto& input : inputs) if (input.kind == ole_driver::Kind::drop)
        {
            if (proposedClient || !input.screenPoint || !behavior::Access::completionWindow)
            { result.obligations.emplace_back("v4_virtual_drop_point_unavailable"); return result; }
            POINT client{input.screenPoint->x, input.screenPoint->y};
            if (!ScreenToClient(behavior::Access::completionWindow, &client))
            { result.obligations.emplace_back("v4_virtual_drop_client_point_unavailable"); return result; }
            proposedClient = client;
        }
        if (!proposedClient) { result.obligations.emplace_back("v4_virtual_drop_point_unavailable"); return result; }
        result.v4VirtualGeometry = behavior::PrepareV4VirtualDesktopGeometry(ownedProfile, verified, identity, *proposedClient);
        const auto& geometry = *result.v4VirtualGeometry;
        if (!geometry.prepared || geometry.resourceUnavailable || geometry.candidateId != raw.id ||
            !behavior::Same(geometry.expectedCell, landing))
        { result.obligations.emplace_back("v4_virtual_geometry_or_landing_unavailable"); return result; }
        for (const auto& input : inputs)
        {
            if (input.programIndex >= geometry.pointsByProgramIndex.size() ||
                !geometry.pointsByProgramIndex[input.programIndex] || !input.screenPoint ||
                input.screenPoint->x != geometry.pointsByProgramIndex[input.programIndex]->x ||
                input.screenPoint->y != geometry.pointsByProgramIndex[input.programIndex]->y)
            { result.obligations.emplace_back("v4_virtual_input_program_geometry_mismatch"); return result; }
        }
        for (const auto& reason : geometry.unresolved)
            result.unclassifiedObligations.push_back("v4_virtual_geometry:" + reason);
        const auto selectedCollection = Value(axes, candidate, "C07");
        if (selectedCollection == "on" || selectedCollection == "off")
        {
            const bool actual = behavior::Access::ProgramCollectionEnabled(*profile.app);
            result.v4VirtualProgramCollectionInput = BridgeResult::V4TextProgramCollectionInput{
                raw.id, reinterpret_cast<std::uintptr_t>(profile.app), GetCurrentThreadId(), actual,
                actual == (selectedCollection == "on")};
            if (!result.v4VirtualProgramCollectionInput->candidateMatched)
                result.obligations.emplace_back("v4_virtual_program_collection_candidate_mismatch");
        }
        else result.obligations.emplace_back("v4_virtual_program_collection_input_unavailable");
        const auto owner = behavior::Access::CreateV4ConfiguredContentOwner(*profile.app);
        const auto binding = mixed_fixture::PrepareConfiguredContentSource(verified, identity, owner, contentByteBudget, virtualBudget);
        auto* source = binding.SourceObject();
        result.v4ConfiguredInput.emplace();
        result.v4ConfiguredInput->candidateId = raw.id;
        result.v4ConfiguredInput->sourceIdentity = reinterpret_cast<std::uintptr_t>(source);
        result.v4ConfiguredInput->ownerThread = source ? source->ownerThread : 0;
        result.v4ConfiguredInput->inputPrepared = binding.inputPrepared;
        result.v4ConfiguredInput->resourceUnavailable = binding.resourceUnavailable;
        result.v4ConfiguredInput->unresolved = binding.unresolved;
        if (!binding.inputPrepared || binding.resourceUnavailable || binding.candidateId != raw.id || !source ||
            source->ownerThread != GetCurrentThreadId())
        {
            result.obligations.emplace_back(binding.resourceUnavailable ? "v4_virtual_source_resource_unavailable" : "v4_virtual_source_unavailable");
            for (const auto& reason : binding.unresolved) result.obligations.push_back("v4_virtual_source:" + reason);
            return result;
        }
        result.v4ConfiguredPreparation.emplace();
        auto& initial = *result.v4ConfiguredPreparation;
        initial.entries = source->entries;
        { std::lock_guard lock(source->observationMutex); initial.calls = source->calls; }
        const auto& expected = binding.ConfiguredEntries();
        bool sameEntries = initial.entries.size() == expected.size();
        if (sameEntries) for (std::size_t i = 0; i < expected.size(); ++i)
        {
            const auto& a = initial.entries[i]; const auto& b = expected[i];
            if (a.format != b.format || a.index != b.index || a.tymed != b.tymed || a.bytes != b.bytes ||
                a.readFailure != b.readFailure || a.getDataFailure != b.getDataFailure) { sameEntries = false; break; }
        }
        initial.captured = sameEntries && initial.calls.empty() && !source->wrongApartment.load() && !source->observationFailed.load();
        initial.unresolved.emplace_back("v4_virtual_preparation_snapshot_nonatomic_and_not_host_selection");
        if (!initial.captured) result.obligations.emplace_back("v4_virtual_source_initial_snapshot_unavailable");
        // Compare the candidate-derived manifest with actual provider entries
        // while this same Profile/owner/source is alive. This reads input
        // values only: it does not call GetData or predict host format choice.
        result.v4VirtualManifestInput.emplace();
        auto& manifestInput = *result.v4VirtualManifestInput;
        manifestInput.candidateId = raw.id;
        manifestInput.sourceIdentity = reinterpret_cast<std::uintptr_t>(source);
        manifestInput.ownerThread = GetCurrentThreadId();
        try
        {
            const auto descriptorFormat = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileGroupDescriptorW"));
            const auto contentsFormat = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileContents"));
            const auto& entries = initial.entries;
            const auto descriptorMatches = [&](const auto& entry) {
                return entry.format == descriptorFormat && entry.index == -1 && entry.tymed == TYMED_HGLOBAL; };
            const auto descriptor = std::find_if(entries.begin(), entries.end(), descriptorMatches);
            bool matched = initial.captured && ownedProfile.StillOwned() && binding.SourceObject() == source &&
                behavior::Access::v4ConfiguredContentOwner == owner && source->ownerThread == GetCurrentThreadId() &&
                descriptorFormat && contentsFormat && descriptor != entries.end() &&
                std::count_if(entries.begin(), entries.end(), descriptorMatches) == 1 &&
                descriptor->getDataFailure == S_OK && !descriptor->readFailure &&
                descriptor->bytes.size() >= offsetof(FILEGROUPDESCRIPTORW, fgd);
            UINT count = 0;
            if (matched)
            {
                std::memcpy(&count, descriptor->bytes.data(), sizeof(count));
                manifestInput.descriptorCount = count;
                matched = count == result.v4VirtualManifest->members.size() &&
                    static_cast<std::size_t>(count) <= ((std::numeric_limits<std::size_t>::max)() -
                        offsetof(FILEGROUPDESCRIPTORW, fgd)) / sizeof(FILEDESCRIPTORW) &&
                    descriptor->bytes.size() == offsetof(FILEGROUPDESCRIPTORW, fgd) +
                        static_cast<std::size_t>(count) * sizeof(FILEDESCRIPTORW);
            }
            for (std::size_t i = 0; matched && i < count; ++i)
            {
                FILEDESCRIPTORW file{};
                std::memcpy(&file, descriptor->bytes.data() + offsetof(FILEGROUPDESCRIPTORW, fgd) +
                    i * sizeof(FILEDESCRIPTORW), sizeof(file));
                const auto end = std::find(std::cbegin(file.cFileName), std::cend(file.cFileName), L'\0');
                const auto& expectedMember = result.v4VirtualManifest->members[i];
                const auto streamMatches = [&](const auto& entry) {
                    return entry.format == contentsFormat && entry.tymed == TYMED_ISTREAM &&
                        entry.index >= 0 && static_cast<std::size_t>(entry.index) == i; };
                const auto stream = std::find_if(entries.begin(), entries.end(), streamMatches);
                matched = end != std::cend(file.cFileName) && expectedMember.descriptorIndex == i &&
                    std::wstring_view(std::cbegin(file.cFileName), static_cast<std::size_t>(end - std::cbegin(file.cFileName))) == expectedMember.name &&
                    stream != entries.end() && std::count_if(entries.begin(), entries.end(), streamMatches) == 1 &&
                    stream->getDataFailure == S_OK && !stream->readFailure && stream->bytes == expectedMember.bytes;
                if (matched) manifestInput.members.push_back({i, expectedMember.name, stream->bytes});
            }
            manifestInput.captured = matched && manifestInput.members.size() == result.v4VirtualManifest->members.size();
            if (!manifestInput.captured) manifestInput.unresolved.emplace_back("v4_virtual_manifest_and_provider_input_unavailable");
            manifestInput.unresolved.emplace_back("v4_virtual_manifest_snapshot_nonatomic_and_host_reads_unobserved");
            manifestInput.unresolved.emplace_back("v4_virtual_other_formats_and_all_input_axes_unverified");
        }
        catch (const std::bad_alloc&) { manifestInput.resourceUnavailable = true; }
        catch (const std::length_error&) { manifestInput.resourceUnavailable = true; }
        catch (...) { manifestInput.unresolved.emplace_back("v4_virtual_manifest_provider_snapshot_unavailable"); }
        if (!manifestInput.captured || manifestInput.resourceUnavailable)
            result.obligations.emplace_back("v4_virtual_manifest_provider_input_unavailable");
        for (const auto& reason : binding.unresolved) result.unclassifiedObligations.push_back("v4_virtual_source:" + reason);
        auto hostBinding = behavior::Access::CreateV3AdapterAndHostEpoch(*profile.app, source);
        if (!hostBinding.first || !hostBinding.second)
        { result.obligations.emplace_back("v4_virtual_host_adapter_unavailable"); return result; }
        const auto plan = ole_driver::BuildV4JoinedOleExecutionPlan(verified, identity, present, selected, inputs);
        result.obligations.insert(result.obligations.end(), plan.preconditions.begin(), plan.preconditions.end());
        result.graphUnresolved = plan.graphUnresolved;
        result.postDispatchObligations = plan.postDispatchObligations;
        result.unclassifiedObligations.insert(result.unclassifiedObligations.end(), plan.unclassified.begin(), plan.unclassified.end());
        result.outcomeExpectations = plan.postconditions; result.v4PresenceExpectations = plan.presence;
        if (!plan.graphIdentityValid || !plan.structurallyPlanned || !result.unclassifiedObligations.empty())
            result.obligations.emplace_back("v4_virtual_graph_or_unclassified_capability_unavailable");
        // Report one row per authoritative axis. A constructed input is not a
        // production observation: the real host may select a different
        // format or refuse the operation, and hypotheses remain postflight.
        result.v4VirtualAxisInputAudit.emplace();
        auto& axisAudit = *result.v4VirtualAxisInputAudit;
        axisAudit.candidateId = raw.id;
        try
        {
            if (candidate.values.size() != axes.size())
                throw ole_driver::ObservationUnavailable("v4 virtual candidate axis shape unavailable");
            std::map<std::string, std::size_t> index;
            for (std::size_t i = 0; i < axes.size(); ++i)
            {
                if (!index.emplace(axes[i].id, i).second)
                    throw ole_driver::ObservationUnavailable("v4 virtual candidate axis identity unavailable");
                const bool inactiveB = axes[i].id.starts_with("task_b.") && !verified.TaskBActive() &&
                    (candidate.values[i] == "na" || (axes[i].role == Role::Derived && candidate.values[i].empty()));
                const auto state = inactiveB ? "not_applicable" : axes[i].role == Role::Hypothesis ? "postflight_pending" :
                    axes[i].role == Role::Derived ? "derived_uncomputed" : "unimplemented";
                axisAudit.axes.push_back({axes[i].id, candidate.values[i], state, {}, {}, {}});
            }
            const auto mark = [&](std::string_view id, std::string actual, std::string_view evidence,
                std::optional<std::size_t> programIndex = std::nullopt) {
                const auto it = index.find(std::string(id));
                if (it == index.end()) return;
                auto& row = axisAudit.axes[it->second];
                if (row.state == "postflight_pending" || row.state == "derived_uncomputed" || row.state == "not_applicable") return;
                row.state = "constructed_and_read"; row.actual = std::move(actual); row.evidence = evidence;
                if (programIndex) row.programIndices.push_back(*programIndex);
            };
            constexpr char digits[] = "0123456789abcdef";
            const auto appendByte = [&](std::string& text, unsigned value) {
                text.push_back(digits[(value >> 4) & 15]); text.push_back(digits[value & 15]); };
            const auto encodeBytes = [&](const std::vector<std::byte>& bytes) {
                if (bytes.size() > ((std::numeric_limits<std::size_t>::max)() - 4) / 2) throw std::length_error("virtual bytes too large");
                std::string text = "hex_"; text.reserve(4 + bytes.size() * 2);
                for (const auto byte : bytes) appendByte(text, std::to_integer<unsigned>(byte)); return text;
            };
            const auto encodeName = [&](std::wstring_view name) {
                std::string text = "utf16le_hex_";
                for (const auto unit : name) { const auto value = static_cast<std::uint16_t>(unit);
                    appendByte(text, value & 255); appendByte(text, value >> 8); }
                return text;
            };
            if (manifestInput.captured && !manifestInput.resourceUnavailable && initial.captured &&
                result.v4VirtualManifest->candidateId == raw.id)
            {
                const auto count = manifestInput.members.size();
                const std::string countValue = count == 0 ? "zero" : count == 1 ? "one" : count == 2 ? "two" :
                    "count_" + std::to_string(count);
                if (Value(axes, candidate, "V4.source.virtual_count") == countValue)
                    mark("V4.source.virtual_count", std::to_string(count), "provider_descriptor_count_snapshot");
                if (Value(axes, candidate, "T02.virtual_files") == "yes")
                    mark("T02.virtual_files", "descriptor_and_indexed_streams", "provider_entries_snapshot");
                if (Value(axes, candidate, "T02.virtual_completeness") == "complete")
                    mark("T02.virtual_completeness", std::to_string(count), "provider_indexed_streams_snapshot_only");
                for (const auto name : {"V4.format.file_descriptor_w.offer", "V4.format.file_descriptor_w.medium.hglobal",
                    "V4.format.file_contents.offer", "V4.format.file_contents.medium.istream"})
                    if (Value(axes, candidate, name) == "yes") mark(name, "yes", "provider_entries_snapshot");
                const auto descriptorFormat = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileGroupDescriptorW"));
                const auto descriptor = std::find_if(initial.entries.begin(), initial.entries.end(), [&](const auto& entry) {
                    return entry.format == descriptorFormat && entry.index == -1 && entry.tymed == TYMED_HGLOBAL; });
                if (descriptor != initial.entries.end())
                {
                    const auto actual = encodeBytes(descriptor->bytes);
                    if (Value(axes, candidate, "V4.payload.file_descriptor_w.hglobal.default.bytes") == actual)
                        mark("V4.payload.file_descriptor_w.hglobal.default.bytes", actual, "provider_descriptor_bytes_snapshot");
                }
                for (const auto& member : manifestInput.members)
                {
                    const auto suffix = "index_" + std::to_string(member.descriptorIndex);
                    const auto nameAxis = "V4.virtual_name." + suffix + ".utf16le";
                    const auto bytesAxis = "V4.payload.file_contents.istream." + suffix + ".bytes";
                    const auto actualName = encodeName(member.name); const auto actualBytes = encodeBytes(member.bytes);
                    if (Value(axes, candidate, nameAxis) == actualName) mark(nameAxis, actualName, "provider_descriptor_name_snapshot");
                    if (Value(axes, candidate, bytesAxis) == actualBytes) mark(bytesAxis, actualBytes, "provider_stream_bytes_snapshot");
                }
            }
            if (geometry.prepared && geometry.candidateId == raw.id && ownedProfile.StillOwned())
            {
                mark("target.E19", encodeName(geometry.initial.pageName), "same_host_named_page_snapshot");
                mark("target.E23", std::to_string(geometry.initial.columns), "same_host_page_columns_snapshot");
                mark("target.E24", std::to_string(geometry.initial.rows), "same_host_page_rows_snapshot");
                mark("H01.region", "empty_initial_cell", "same_host_initial_occupancy_snapshot");
                mark("H06.page_record", "saved", "same_host_page_record_snapshot");
                mark("H06.page_dimensions", "valid", "same_host_saved_dimensions_snapshot");
                mark("H07.occupancy", "partial", "same_host_sentinel_occupancy_snapshot");
            }
            if (result.v4VirtualProgramCollectionInput && result.v4VirtualProgramCollectionInput->candidateMatched)
                mark("C07", result.v4VirtualProgramCollectionInput->actual ? "on" : "off", "same_host_program_collection_setting_snapshot");
            const auto program = verified.TransferProgram();
            const auto allowed = ole_driver::EncodeAllowedEffect(Value(axes, candidate, "T03"),
                Value(axes, candidate, "T03.extra_protocol_bits"));
            bool allCalls = allowed.has_value(); std::size_t calls = 0;
            for (std::size_t i = 0; i < program.steps.size(); ++i)
            {
                const auto kind = ole_driver::ProgramKind(program.steps[i].kind);
                if (!kind) continue;
                ++calls;
                const auto found = std::find_if(inputs.begin(), inputs.end(), [&](const auto& input) { return input.programIndex == i; });
                if (*kind == ole_driver::Kind::leave || found == inputs.end() ||
                    std::count_if(inputs.begin(), inputs.end(), [&](const auto& input) { return input.programIndex == i; }) != 1)
                { allCalls = false; continue; }
                const auto [buttonsAxis, modifiersAxis] = ole_driver::PhaseAxes(program.steps[i].kind);
                const auto buttons = Value(axes, candidate, buttonsAxis), modifiers = Value(axes, candidate, modifiersAxis);
                const auto parsed = ole_driver::ParseKeyParameter(program.steps[i].parameter);
                const auto keys = ole_driver::EncodeKeyState(buttons, modifiers);
                const bool matches = found->kind == *kind && found->modelParameter == program.steps[i].parameter &&
                    parsed && parsed->first == buttons && parsed->second == modifiers && keys.valid && !keys.altX1Ambiguous &&
                    found->keyState && *found->keyState == keys.value && found->allowedEffect && allowed &&
                    *found->allowedEffect == *allowed && found->screenPoint && i < geometry.pointsByProgramIndex.size() &&
                    geometry.pointsByProgramIndex[i] && found->screenPoint->x == geometry.pointsByProgramIndex[i]->x &&
                    found->screenPoint->y == geometry.pointsByProgramIndex[i]->y;
                if (!matches) { allCalls = false; continue; }
                mark(buttonsAxis, std::to_string(*found->keyState), "ole_call_key_state_argument", i);
                mark(modifiersAxis, std::to_string(*found->keyState), "ole_call_key_state_argument", i);
            }
            if (allCalls && calls && calls == inputs.size())
            {
                mark("T03", std::to_string(*allowed), "ole_call_allowed_effect_argument");
                mark("T03.extra_protocol_bits", std::to_string(*allowed), "ole_call_allowed_effect_argument");
            }
            Microsoft::WRL::ComPtr<IDataObjectAsyncCapability> asyncCapability;
            const HRESULT capability = source->QueryInterface(IID_PPV_ARGS(asyncCapability.GetAddressOf()));
            if (capability == E_NOINTERFACE && Value(axes, candidate, "T04.capability") == "absent")
                mark("T04.capability", std::to_string(static_cast<std::uint32_t>(capability)), "same_provider_query_interface_result");
            axisAudit.captured = true;
            axisAudit.unresolved.emplace_back("v4_virtual_axis_audit_input_only_not_production_dispatch_or_outcome");
            axisAudit.unresolved.emplace_back("v4_virtual_axis_audit_unlisted_formats_and_live_host_changes_unresolved");
        }
        catch (const std::bad_alloc&) { axisAudit.resourceUnavailable = true; }
        catch (const std::length_error&) { axisAudit.resourceUnavailable = true; }
        catch (...) { axisAudit.unresolved.emplace_back("v4_virtual_axis_audit_unavailable"); }
        if (!axisAudit.captured || axisAudit.resourceUnavailable)
            result.obligations.emplace_back("v4_virtual_axis_audit_unavailable");
        result.obligations.emplace_back("v4_virtual_fresh_process_exclusive_staging_root_unimplemented");
        result.obligations.emplace_back("v4_virtual_all_input_axes_geometry_and_event_oracle_unimplemented");
        result.obligations.emplace_back("v4_virtual_same_run_provider_materialization_refresh_and_business_unimplemented");
        if (result.obligations.empty())
        {
            // Construct this observer only after staging isolation is proved:
            // its initial host setup changes fixture maintenance state.
            behavior::V4VirtualCopyObservation observed(ownedProfile, *source, binding, *result.v4VirtualManifest, std::move(landing));
            const ole_driver::HostBoundaries host{
                [&](const ole_driver::Input& event) {
                    if (!result.v4VirtualGeometry || event.programIndex >= result.v4VirtualGeometry->pointsByProgramIndex.size() ||
                        !result.v4VirtualGeometry->pointsByProgramIndex[event.programIndex] || !event.screenPoint ||
                        event.screenPoint->x != result.v4VirtualGeometry->pointsByProgramIndex[event.programIndex]->x ||
                        event.screenPoint->y != result.v4VirtualGeometry->pointsByProgramIndex[event.programIndex]->y ||
                        !behavior::RevalidateV4VirtualDesktopGeometry(ownedProfile, verified, identity,
                            *result.v4VirtualGeometry, event.programIndex))
                        throw ole_driver::ObservationUnavailable("v4 virtual geometry changed before OLE input");
                    if (!result.v4VirtualProgramCollectionInput || !result.v4VirtualProgramCollectionInput->candidateMatched ||
                        result.v4VirtualProgramCollectionInput->candidateId != raw.id ||
                        result.v4VirtualProgramCollectionInput->hostIdentity != reinterpret_cast<std::uintptr_t>(profile.app) ||
                        result.v4VirtualProgramCollectionInput->ownerThread != GetCurrentThreadId() ||
                        behavior::Access::ProgramCollectionEnabled(*profile.app) != result.v4VirtualProgramCollectionInput->actual)
                        throw ole_driver::ObservationUnavailable("v4 virtual host collection setting changed before OLE input");
                    result.v4VirtualGeometryPreCallIndices.push_back(event.programIndex);
                    result.v4VirtualProgramCollectionInput->matchingPreCallProgramIndices.push_back(event.programIndex);
                    observed.BeforeOleCall(event);
                },
                [&](const ole_driver::Observation&) {},
                [&](const ole_driver::Observation& terminal) { observed.Await(terminal); },
                [] { throw ole_driver::ObservationUnavailable("v4 virtual business requires the sealed execution observer"); }};
            const auto oracle = [](const ole_driver::Observation& event) {
                if (event.kind != ole_driver::Kind::drop) return;
                if (!event.returnedEffect) throw ole_driver::ObservationUnavailable("v4 virtual Drop effect unavailable");
                if (FAILED(event.result) || *event.returnedEffect != DROPEFFECT_COPY)
                    throw ole_driver::EventAssertionFailure("v4 virtual Drop differs from copy expectation");
            };
            const ole_driver::V4VirtualObserverBoundaries virtualObserver{
                [&](const auto& key) { observed.BindExecution(key, owner, hostBinding.second, verified, identity); },
                [&](const auto& key) { observed.CaptureExecution(key, owner, hostBinding.second); }};
            try
            {
                if (!snowdesktop::external_transfer_observation::Enable())
                    throw ole_driver::ObservationUnavailable("v4 virtual diagnostic session unavailable");
                struct StagingHandlesGuard final
                {
                    BridgeResult& result;
                    ~StagingHandlesGuard()
                    {
                        // Writer drain is not producer quiescence. There is no
                        // authenticated producer-stop proof in this path yet;
                        // retain diagnostic handles until the child exits.
                        if (!snowdesktop::external_transfer_observation::Complete() ||
                            snowdesktop::external_transfer_observation::StagingHandleCount() != 0)
                        { result.v4VirtualStagingHandleReleaseDeferred = true; result.observationUnavailable = true; }
                    }
                } stagingHandles{result};
                struct StopVirtualDiagnostics final
                {
                    bool active = true, workActive = false;
                    void Stop() noexcept {
                        if (active) { snowdesktop::external_transfer_observation::Stop(); active = false; }
                        if (workActive) { snowdesktop::background_work_observation::Stop(); workActive = false; }
                    }
                    ~StopVirtualDiagnostics() { Stop(); }
                } diagnostics;
                if (!snowdesktop::background_work_observation::Enable())
                    throw ole_driver::ObservationUnavailable("v4 virtual worker diagnostic session unavailable");
                diagnostics.workActive = true;
                dispatchAttempted = true;
                const auto receipt = ole_driver::V4ConfiguredContentGraphExecutor::RunBound(verified, identity, present, selected,
                    inputs, binding, owner, hostBinding.first.Get(), hostBinding.second, host, oracle, {}, virtualObserver);
                diagnostics.Stop(); result.v4DiagnosticBudget = snowdesktop::external_transfer_observation::ObserveBudget();
                result.v4BackgroundDiagnosticBudget = snowdesktop::background_work_observation::ObserveBudget();
                const auto& execution = receipt.Execution(); result.v4OleAdapterLogLast = execution.oleAdapterLogLast;
                result.productionStarted = execution.productionStarted; result.productionEntryUnknown = execution.productionEntryUnknown;
                dispatchAttempted = false;
                result.businessFailureObserved = execution.businessFailureObserved;
                result.eventOracleFailureObserved = execution.eventOracleFailureObserved;
                result.eventDispatchObservationRecorded = !execution.observations.empty();
                result.eventDispatchFailureObserved = std::any_of(execution.observations.begin(), execution.observations.end(),
                    [](const ole_driver::Observation& event) { return FAILED(event.result); });
                if (execution.businessFailureObserved || execution.eventOracleFailureObserved) result.status = BridgeStatus::production_failed;
                result.observationUnavailable = execution.additionalObservationUnavailable ||
                    execution.status == ole_driver::Status::observation_unavailable ||
                    execution.status == ole_driver::Status::host_observation_failed || execution.status == ole_driver::Status::event_failed;
                result.failure = execution.failure;
                result.obligations.insert(result.obligations.end(), execution.obligations.begin(), execution.obligations.end());
                result.postDispatchObligations.insert(result.postDispatchObligations.end(),
                    execution.postconditionsPending.begin(), execution.postconditionsPending.end());
                result.unclassifiedObligations.insert(result.unclassifiedObligations.end(),
                    execution.unclassifiedObligations.begin(), execution.unclassifiedObligations.end());
                result.v4ConfiguredSourceInitial = receipt.Initial(); result.v4ConfiguredSourceAtExecution = receipt.AtExecution();
                result.v4ConfiguredSourceAfterAwait = receipt.AfterAwait(); result.v4ConfiguredAwaitReturned = receipt.AwaitReturned();
                try { result.v4ConfiguredSourceAfterDispatch = receipt.AfterDispatch(); }
                catch (...) { result.observationUnavailable = true; }
                result.v4OleObservations = execution.observations; result.v4CapturedOleReturns = receipt.CapturedReturns();
                if (receipt.CompletionTraceCaptured()) result.v4CompletionTrace = receipt.CompletionTrace();
                result.v4VirtualBusiness = observed.Evidence();
                const auto sealedBusiness = receipt.SealedBusiness();
                if (sealedBusiness) { result.v4VirtualBusiness = *sealedBusiness; result.v4VirtualBusinessSealed = true; }
                else if (execution.productionStarted) result.observationUnavailable = true;
                if (execution.productionStarted && behavior::Access::messageTrace)
                {
                    try
                    {
                        if (!sealedBusiness || !receipt.SameBoundHost(binding, owner, hostBinding.first.Get(),
                            hostBinding.second, *behavior::Access::messageTrace, behavior::Access::fixtureControlWindow))
                            throw ole_driver::ObservationUnavailable("v4 virtual layout snapshot receipt unavailable");
                        // This bounds the observer's read, not the product's
                        // layout size or the model's candidate domain.
                        constexpr std::size_t layoutObservationByteBudget = 4u * 1024u * 1024u;
                        result.v4VirtualLayoutDiskSnapshot = behavior::CaptureV4VirtualLayoutDiskSnapshot(
                            ownedProfile, verified, identity, layoutObservationByteBudget);
                        result.v4VirtualLayoutDiskSnapshot->unresolved.emplace_back(
                            "v4_virtual_layout_snapshot_after_business_reload_not_initial_save");
                        if (!result.v4VirtualLayoutDiskSnapshot->captured ||
                            result.v4VirtualLayoutDiskSnapshot->resourceUnavailable ||
                            !result.v4VirtualLayoutDiskSnapshot->unresolved.empty()) result.observationUnavailable = true;
                    }
                    catch (...) { result.observationUnavailable = true; }
                    try
                    {
                        if (!result.v4VirtualManifest)
                            throw ole_driver::ObservationUnavailable("v4 virtual layout event manifest unavailable");
                        constexpr std::size_t layoutObservationByteBudget = 4u * 1024u * 1024u;
                        result.v4VirtualLayoutEvents = behavior::InspectV4VirtualLayoutEvents(ownedProfile, receipt,
                            verified, identity, present, selected, binding, owner, hostBinding.first.Get(), hostBinding.second,
                            *behavior::Access::messageTrace, behavior::Access::fixtureControlWindow,
                            *result.v4VirtualManifest, contentByteBudget, virtualBudget, layoutObservationByteBudget);
                        if (!result.v4VirtualLayoutEvents->sameReceipt || !result.v4VirtualLayoutEvents->unresolved.empty())
                            result.observationUnavailable = true;
                    }
                    catch (...) { result.observationUnavailable = true; }
                    try
                    {
                        result.v4VirtualFailures = ole_driver::InspectV4VirtualFailures(receipt, verified, identity, present,
                            selected, binding, owner, hostBinding.first.Get(), hostBinding.second,
                            *behavior::Access::messageTrace, behavior::Access::fixtureControlWindow);
                        if (result.v4VirtualFailures->observationUnavailable || !result.v4VirtualFailures->unresolved.empty())
                            result.observationUnavailable = true;
                    }
                    catch (...) { result.observationUnavailable = true; }
                    try
                    {
                        result.v4ConfiguredObservation = ole_driver::InspectV4ConfiguredContent(receipt, verified, identity, present,
                            selected, binding, owner, hostBinding.first.Get(), hostBinding.second,
                            *behavior::Access::messageTrace, behavior::Access::fixtureControlWindow);
                        if (result.v4ConfiguredObservation->observationUnavailable || !result.v4ConfiguredObservation->unresolved.empty())
                            result.observationUnavailable = true;
                    }
                    catch (...) { result.observationUnavailable = true; }
                    try
                    {
                        result.v4VirtualFlow = ole_driver::InspectV4VirtualFlow(receipt, verified, identity, present,
                            selected, binding, owner, hostBinding.first.Get(), hostBinding.second,
                            *behavior::Access::messageTrace, behavior::Access::fixtureControlWindow,
                            *result.v4VirtualManifest, contentByteBudget, virtualBudget);
                        if (result.v4VirtualFlow->observationUnavailable || !result.v4VirtualFlow->unresolved.empty())
                            result.observationUnavailable = true;
                    }
                    catch (...) { result.observationUnavailable = true; }
                    try
                    {
                        result.v4VirtualTrackedShell = ole_driver::InspectV4VirtualTrackedShell(receipt, verified, identity, present,
                            selected, binding, owner, hostBinding.first.Get(), hostBinding.second,
                            *behavior::Access::messageTrace, behavior::Access::fixtureControlWindow,
                            *result.v4VirtualManifest, contentByteBudget, virtualBudget);
                        if (result.v4VirtualTrackedShell->observationUnavailable || result.v4VirtualTrackedShell->resourceUnavailable ||
                            !result.v4VirtualTrackedShell->unresolved.empty()) result.observationUnavailable = true;
                    }
                    catch (...) { result.observationUnavailable = true; }
                    try
                    {
                        result.v4VirtualProductionChain = ole_driver::InspectV4VirtualProductionChain(receipt, verified, identity, present,
                            selected, binding, owner, hostBinding.first.Get(), hostBinding.second,
                            *behavior::Access::messageTrace, behavior::Access::fixtureControlWindow,
                            *result.v4VirtualManifest, contentByteBudget, virtualBudget);
                        if (result.v4VirtualProductionChain->observationUnavailable || result.v4VirtualProductionChain->resourceUnavailable ||
                            !result.v4VirtualProductionChain->unresolved.empty()) result.observationUnavailable = true;
                    }
                    catch (...) { result.observationUnavailable = true; }
                    try
                    {
                        if (!result.v4VirtualFlow)
                            throw ole_driver::ObservationUnavailable("v4 virtual staging flow unavailable");
                        result.v4VirtualStagingHandles = ole_driver::InspectV4VirtualStagingHandles(
                            *result.v4VirtualFlow, contentByteBudget);
                        if (result.v4VirtualStagingHandles->observationUnavailable || !result.v4VirtualStagingHandles->logComplete)
                            result.observationUnavailable = true;
                    }
                    catch (...) { result.observationUnavailable = true; }
                    try
                    {
                        result.v4VirtualStagingComparison = InspectV4VirtualReceiptStaging(receipt, verified, identity,
                            present, selected, binding, owner, hostBinding.first.Get(), hostBinding.second,
                            *behavior::Access::messageTrace, behavior::Access::fixtureControlWindow,
                            contentByteBudget, virtualBudget);
                        if (result.v4VirtualStagingComparison->observationUnavailable ||
                            result.v4VirtualStagingComparison->resourceUnavailable ||
                            !result.v4VirtualStagingComparison->unresolved.empty()) result.observationUnavailable = true;
                    }
                    catch (...) { result.observationUnavailable = true; }
                    try
                    {
                        result.v4VirtualRefresh = ole_driver::InspectV4VirtualRefresh(receipt, verified, identity, present,
                            selected, binding, owner, hostBinding.first.Get(), hostBinding.second,
                            *behavior::Access::messageTrace, behavior::Access::fixtureControlWindow,
                            *result.v4VirtualManifest, contentByteBudget, virtualBudget);
                        if (result.v4VirtualRefresh->observationUnavailable || !result.v4VirtualRefresh->unresolved.empty())
                            result.observationUnavailable = true;
                    }
                    catch (...) { result.observationUnavailable = true; }
                    try
                    {
                        if (!result.v4VirtualManifest)
                            throw ole_driver::ObservationUnavailable("v4 virtual output binding inputs unavailable");
                        result.v4VirtualOutputBinding = behavior::InspectSealedV4VirtualOutputBinding(receipt, verified,
                            identity, present, selected, binding, owner, hostBinding.first.Get(), hostBinding.second,
                            *behavior::Access::messageTrace, behavior::Access::fixtureControlWindow,
                            *result.v4VirtualManifest, contentByteBudget, virtualBudget);
                        if (result.v4VirtualOutputBinding->observationUnavailable ||
                            !result.v4VirtualOutputBinding->unresolved.empty()) result.observationUnavailable = true;
                    }
                    catch (...) { result.observationUnavailable = true; }
                }
                if (execution.productionStarted) result.obligations.emplace_back("v4_virtual_full_graph_and_outcomes_unverified");
            }
            catch (...)
            {
                try { if (!result.v4VirtualBusinessSealed) result.v4VirtualBusiness = observed.Evidence(); } catch (...) {}
                throw;
            }
        }
    }
    catch (const std::exception& error)
    { if (dispatchAttempted && !result.productionStarted) result.productionEntryUnknown = true;
      result.observationUnavailable = true; if (result.failure.empty()) result.failure = error.what(); }
    catch (...)
    { if (dispatchAttempted && !result.productionStarted) result.productionEntryUnknown = true;
      result.observationUnavailable = true; if (result.failure.empty()) result.failure = "v4_virtual_preparation_unavailable"; }
    return result;
}

// A native group fixture retains its physical observation handles with the
// returned result. It does not issue Device input or establish production
// capture, member movement, completion, or persistence.
inline BridgeResult PrepareV4NativeGroupMemberInput(
    const event_graph::V4FamilyIdentityContext& identity, const V4FamilyValidatedContext& verified,
    const std::vector<bool>& present, const event_graph::V4NativeJoinedGraphVariant& selected,
    behavior::OwnedProfileDesktopRoot& ownedRoot, const V4SingleFileFixture& fixture,
    std::size_t groupChildBudget = 128, std::size_t groupWidgetBudget = 256, std::size_t groupUtf16ByteBudget = 65536,
    std::size_t fileByteBudget = 1024 * 1024)
{
    BridgeResult result; result.observationUnavailable = true;
    try
    {
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        const auto replay = event_graph::V4NativeJoinedGraph::Replay(identity, verified, present, selected.id);
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty() || !replay.identityValid ||
            !replay.errors.empty() || replay.variant.id != selected.id || replay.variant.classes != selected.classes ||
            replay.variant.order != selected.order || replay.variant.unresolved != selected.unresolved ||
            !ownedRoot.StillOwned() || !ownedRoot.profile || !ownedRoot.profile->app)
        { result.obligations.emplace_back("v4_native_group_candidate_or_graph_unavailable"); return result; }
        auto& app = *ownedRoot.profile->app;
        if (behavior::Access::productionDispatch != 1) behavior::Access::EnableProductionDispatch(app, false);
        if (!behavior::Access::fixtureControlWindow) behavior::Access::EnableFixtureControlWindow(app);
        if (!behavior::Access::nativePointerDispatch) behavior::Access::EnableNativePointerDispatch(app);
        behavior::Wait([&] { return behavior::Access::NativeDragIdle(app) &&
            behavior::Access::ContainerWorkIdle(app) && behavior::Access::LayoutSaveIdle(app); });
        result.v4NativeGroupMemberInitial = std::make_shared<behavior::V4NativeGroupMemberInitial>(
            behavior::PrepareV4NativeGroupMemberInitial(ownedRoot, fixture, verified, identity,
                groupChildBudget, groupWidgetBudget, groupUtf16ByteBudget, fileByteBudget));
        const auto& initial = *result.v4NativeGroupMemberInitial;
        if (!initial.prepared || !initial.source || initial.candidateId != raw.id)
        { result.obligations.emplace_back(initial.resourceUnavailable ? "v4_native_group_initial_resource_unavailable" :
              "v4_native_group_initial_unavailable"); return result; }
        result.v4GroupAxisInput = CaptureV4GroupAxisInput(identity, verified, app,
            groupChildBudget, groupWidgetBudget, groupUtf16ByteBudget, true);
        result.v4GroupAxisInput->sameHostBindingObserved = ownedRoot.StillOwned() &&
            result.v4GroupAxisInput->candidateId == raw.id && result.v4GroupAxisInput->actualSource &&
            result.v4GroupAxisInput->actualSource->host == initial.source->group.host &&
            result.v4GroupAxisInput->actualSource->group == initial.source->group.group &&
            result.v4GroupAxisInput->actualSource->groupData == initial.source->group.groupData &&
            result.v4GroupAxisInput->actualSource->contentData == initial.source->childData &&
            result.v4GroupAxisInput->actualSource->activeId == initial.source->group.activeId &&
            result.v4GroupAxisInput->actualSource->layoutRevision == initial.source->group.layoutRevision;
        result.graphUnresolved = replay.variant.unresolved;
        result.obligations.insert(result.obligations.end(), initial.unresolved.begin(), initial.unresolved.end());
        result.obligations.emplace_back("v4_native_group_device_session_and_sealed_business_not_implemented");
        result.obligations.emplace_back("v4_native_group_source_lifetime_reentry_and_late_work_unverified");
    }
    catch (const std::bad_alloc&) { result.obligations.emplace_back("v4_native_group_input_resource_unavailable"); }
    catch (const std::length_error&) { result.obligations.emplace_back("v4_native_group_input_resource_unavailable"); }
    catch (...) { result.obligations.emplace_back("v4_native_group_input_observation_unavailable"); }
    return result;
}

// No Device input can be issued while those obligations remain.
inline BridgeResult PrepareV4NativeCollectionMove(
    const event_graph::V4FamilyIdentityContext& identity, const V4FamilyValidatedContext& verified,
    const std::vector<bool>& present, const event_graph::V4NativeJoinedGraphVariant& selected,
    behavior::OwnedProfileDesktopRoot& ownedRoot, const V4SingleFileFixture& fixture,
    GridCell landing, POINT targetScreen, const std::vector<ole_driver::NativePointInput>& points)
{
    BridgeResult result;
    bool dispatchAttempted = false;
    try
    {
        const auto audit = InspectV4NativeContainerCandidate(identity, verified);
        result.obligations = audit.obligations;
        if (!audit.candidateIdentityValid || !audit.narrowInputShape || audit.sourceContainer != "collection" ||
            !fixture.inputPrepared || !fixture.created || !fixture.fileIdentityObserved ||
            fixture.candidateId != audit.candidateId || !ownedRoot.StillOwned() || !ownedRoot.profile || !ownedRoot.profile->app ||
            !behavior::OwnedProfileDesktopRoot::SameId(fixture.rootId, ownedRoot.desktopId))
        { result.obligations.emplace_back("v4_native_collection_fixture_unavailable"); return result; }
        auto& profile = *ownedRoot.profile;
        if (fixture.root != profile.desktop || fixture.file.parent_path() != fixture.root)
        { result.obligations.emplace_back("v4_native_collection_fixture_path_mismatch"); return result; }
        // Re-encode the observed fixture values for an independent comparison
        // with the authoritative input axes. This does not make the public
        // snapshot a provenance proof or prevent later mutation.
        constexpr char digits[] = "0123456789abcdef";
        const auto appendByte = [&](std::string& out, unsigned byte) {
            out.push_back(digits[(byte >> 4) & 15]); out.push_back(digits[byte & 15]);
        };
        std::string pathValue = "utf16le_hex_", bytesValue = "hex_";
        static_assert(sizeof(wchar_t) == 2, "Windows UTF-16 code units required");
        for (const auto unit : fixture.file.native())
        {
            const auto value = static_cast<std::uint16_t>(unit);
            appendByte(pathValue, value & 0xff); appendByte(pathValue, value >> 8);
        }
        for (const auto byte : fixture.bytes) appendByte(bytesValue, std::to_integer<unsigned>(byte));
        const auto& axes = verified.Axes(); const auto& candidate = verified.Coordinate();
        if (Value(axes, candidate, "V4.file_path.index_0.utf16le") != pathValue ||
            Value(axes, candidate, "V4.file_content.index_0.bytes") != bytesValue)
        { result.obligations.emplace_back("v4_native_collection_candidate_file_input_mismatch"); return result; }
        behavior::ObservedPhysicalFile fileSnapshot(fixture.file);
        if (!behavior::OwnedProfileDesktopRoot::SameId(fixture.fileId, fileSnapshot.Identity()) ||
            !fileSnapshot.PathStillNamesObject(fixture.file) || !fileSnapshot.HasBytes(fixture.bytes))
        { result.obligations.emplace_back("v4_native_collection_file_snapshot_unavailable"); return result; }
        // Retain the values that were actually checked before any device input;
        // the caller's public fixture snapshot is not itself an execution seal.
        const auto initialFilePath = fixture.file;
        const auto initialFileBytes = fixture.bytes;
        const auto initialFileId = fileSnapshot.Identity();
        const auto replay = event_graph::V4NativeJoinedGraph::Replay(identity, verified, present, selected.id);
        if (!replay.identityValid || !replay.errors.empty() || replay.variant.id != selected.id ||
            replay.variant.classes != selected.classes || replay.variant.order != selected.order ||
            replay.variant.unresolved != selected.unresolved)
        { result.obligations.emplace_back("v4_native_collection_graph_identity_unavailable"); return result; }
        result.graphUnresolved = replay.variant.unresolved;
        for (const auto& reason : fixture.unresolved) result.obligations.push_back("v4_native_fixture:" + reason);
        result.obligations.emplace_back("v4_native_profile_root_creation_provenance_and_file_lifetime_lease_unimplemented");
        result.obligations.emplace_back("v4_native_candidate_geometry_and_same_run_oracle_capability_unimplemented");

        // Prepare only the isolated starting scene and inspect its actual
        // geometry. Existing obligations remain intact at the dispatch gate.
        behavior::Access::EnableProductionDispatch(*profile.app, false);
        behavior::Access::EnableFixtureControlWindow(*profile.app);
        behavior::Access::EnableNativePointerDispatch(*profile.app);
        behavior::Wait([&] { return behavior::Access::NativeDragIdle(*profile.app) &&
            behavior::Access::ContainerWorkIdle(*profile.app) && behavior::Access::LayoutSaveIdle(*profile.app); });
        behavior::Access::PrepareExpandedNativeSource(*profile.app, {initialFilePath, profile.desktop / L"Sentinel.txt"}, 1);
        auto epoch = behavior::Access::CreateNativeHostEpoch(*profile.app);
        behavior::GeneratedNativeMemberObservation business(ownedRoot, initialFilePath, initialFileBytes, std::move(landing), verified, identity);
        result.v4NativeMemberBusiness = business.Evidence();
        const auto start = behavior::Access::ExternalPoint(behavior::Access::NativeMemberInputCandidate(*profile.app, initialFilePath));
        const auto geometry = behavior::Access::InspectNativeLanding(*profile.app, business.ExpectedLanding());
        const auto v = [&](std::string_view id) { return Value(verified.Axes(), verified.Coordinate(), id); };
        const POINT effectiveTarget = points.empty() ? geometry.screenCenter : targetScreen;
        const bool landingMatches = EndpointSize(v("target.E23")) == geometry.columns &&
            EndpointSize(v("target.E24")) == geometry.rows && v("H01.region") == "empty_slot" &&
            effectiveTarget.x == geometry.screenCenter.x && effectiveTarget.y == geometry.screenCenter.y;
        result.v4NativeCollectionInitialInput = BridgeResult::V4NativeCollectionInitialInput{
            audit.candidateId, reinterpret_cast<std::uintptr_t>(profile.app), GetCurrentThreadId(),
            POINT{start.x, start.y}, geometry, landingMatches, std::nullopt};
        if (!landingMatches)
        { result.obligations.emplace_back("v4_native_landing_geometry_candidate_mismatch"); return result; }
        const auto program = verified.TransferProgram();
        result.v4NativeCollectionPointProposal.emplace();
        auto& proposal = *result.v4NativeCollectionPointProposal;
        proposal.candidateId = audit.candidateId;
        const auto dx = static_cast<std::int64_t>(geometry.screenCenter.x) - start.x;
        const auto dy = static_cast<std::int64_t>(geometry.screenCenter.y) - start.y;
        const auto abs64 = [](std::int64_t n) { return n < 0 ? -n : n; };
        if (v("H08.first_direction") == "unobserved" && v("H08.later_direction") == "na" &&
            v("G02.hover_first") == "na" && v("G02.hover_later") == "na" &&
            PtInRect(&geometry.screenPage, POINT{start.x, start.y}) && PtInRect(&geometry.screenPage, geometry.screenCenter) &&
            (abs64(dx) > (std::max)(8, GetSystemMetrics(SM_CXDRAG)) || abs64(dy) > (std::max)(8, GetSystemMetrics(SM_CYDRAG))))
        {
            std::size_t starts = 0, moves = 0, releases = 0; bool extraMotion = false;
            for (std::size_t i = 0; i < program.steps.size(); ++i)
            {
                const auto& kind = program.steps[i].kind;
                if (kind == "source_start") { ++starts; proposal.points.push_back({i, POINT{start.x, start.y}}); }
                else if (kind == "move_past_drag_threshold") { ++moves; proposal.points.push_back({i, geometry.screenCenter}); }
                else if (kind == "release") { ++releases; proposal.points.push_back({i, geometry.screenCenter}); }
                else if (kind == "pointer_direction" || kind == "hover" || kind == "pointer_direction_later" || kind == "hover_later")
                    extraMotion = true;
            }
            proposal.prepared = starts == 1 && moves == 1 && releases == 1 && !extraMotion;
        }
        if (!proposal.prepared) { proposal.points.clear(); proposal.unresolved.emplace_back("v4_native_collection_point_motion_unimplemented"); }
        const auto samePoints = [](const auto& expected, const auto& actual) {
            if (expected.size() != actual.size()) return false;
            for (const auto& item : expected)
            {
                const auto match = [&](const auto& value) { return value.programIndex == item.programIndex; };
                const auto found = std::find_if(actual.begin(), actual.end(), match);
                if (found == actual.end() || std::count_if(expected.begin(), expected.end(), match) != 1 ||
                    std::count_if(actual.begin(), actual.end(), match) != 1 || found->screen.x != item.screen.x || found->screen.y != item.screen.y)
                    return false;
            }
            return true;
        };
        proposal.generatedPointsUsed = points.empty() && proposal.prepared;
        const auto effectivePoints = proposal.generatedPointsUsed ? proposal.points : points;
        if (!proposal.prepared || !samePoints(proposal.points, effectivePoints))
            result.obligations.emplace_back("v4_native_collection_point_proposal_unavailable");
        proposal.unresolved.emplace_back("v4_native_collection_point_actual_hit_capture_and_commit_unverified");
        behavior::NativeMemberDeviceObservation observed(business, identity, verified, present, selected,
            epoch, POINT{start.x, start.y}, effectiveTarget);
        const RECT bounds = geometry.screenPage;
        const auto plan = ole_driver::BuildV4NativeExecutionPlan(identity, verified, present, selected,
            effectivePoints, behavior::Access::completionWindow, bounds);
        result.obligations.insert(result.obligations.end(), plan.preconditions.begin(), plan.preconditions.end());
        result.postDispatchObligations = plan.postconditions;
        result.unclassifiedObligations = plan.unclassified;
        bool pointsMatch = plan.points.size() == program.steps.size() && plan.sourcePoint &&
            plan.sourcePoint->x == start.x && plan.sourcePoint->y == start.y;
        bool releaseObserved = false;
        if (!pointsMatch)
            result.obligations.emplace_back("v4_native_source_point_candidate_unavailable");
        else for (std::size_t i = 0; i < program.steps.size(); ++i)
            if (program.steps[i].kind == "release")
            {
                releaseObserved = true;
                if (!plan.points[i] || plan.points[i]->x != geometry.screenCenter.x ||
                    plan.points[i]->y != geometry.screenCenter.y)
                { pointsMatch = false; result.obligations.emplace_back("v4_native_release_point_candidate_unavailable"); }
            }
        pointsMatch = pointsMatch && releaseObserved;
        result.v4NativeCollectionInitialInput->sourceAndReleasePointsMatch = pointsMatch;
        if (!plan.unclassified.empty()) result.obligations.emplace_back("v4_native_unclassified_obligation_capability_unimplemented");
        if (!result.obligations.empty()) return result;
        auto host = observed.Boundaries();
        const auto originalBeforeBegin = host.beforeDeviceBegin;
        host.beforeDeviceBegin = [&] {
            const auto current = behavior::Access::InspectNativeLanding(*profile.app, business.ExpectedLanding());
            if (current.layoutRevision != geometry.layoutRevision || current.columns != geometry.columns ||
                current.rows != geometry.rows || !EqualRect(&current.clientCell, &geometry.clientCell) ||
                !EqualRect(&current.screenPage, &geometry.screenPage) ||
                current.screenCenter.x != geometry.screenCenter.x || current.screenCenter.y != geometry.screenCenter.y)
                throw ole_driver::ObservationUnavailable("v4_native_landing_changed_before_device_begin");
            const auto currentStart = behavior::Access::ExternalPoint(behavior::Access::NativeMemberInputCandidate(*profile.app, initialFilePath));
            if (currentStart.x != start.x || currentStart.y != start.y || !samePoints(proposal.points, effectivePoints))
                throw ole_driver::ObservationUnavailable("v4_native_collection_point_proposal_changed_before_device_begin");
            proposal.revalidated = true;
            originalBeforeBegin();
        };
        if (!snowdesktop::external_transfer_observation::Enable())
            throw ole_driver::ObservationUnavailable("v4_native_layout_diagnostic_unavailable");
        struct StopNativeDiagnostic final
        {
            bool active = true;
            void Stop() noexcept
            { if (active) { snowdesktop::external_transfer_observation::Stop(); active = false; } }
            ~StopNativeDiagnostic() { Stop(); }
        } diagnostic;
        const auto originalAwait = host.awaitCompletion;
        host.awaitCompletion = [&](const EventStep& step) {
            originalAwait(step);
            // Freeze the observation interval before the independent oracle
            // requests a disk reload, which can itself schedule layout work.
            diagnostic.Stop(); result.v4DiagnosticBudget = snowdesktop::external_transfer_observation::ObserveBudget();
        };
        const ole_driver::V4NativeMemberObserverBoundaries memberObserver{
            [&](const std::shared_ptr<ole_driver::V4NativeMemberExecutionKey>& key) { business.BindExecution(key, epoch); },
            [&](const std::shared_ptr<ole_driver::V4NativeMemberExecutionKey>& key) { business.CaptureExecution(key, epoch); }};
        dispatchAttempted = true;
        const auto receipt = ole_driver::V4NativeSequence::RunBound(identity, verified, present, selected,
            effectivePoints, behavior::Access::completionWindow, behavior::Access::fixtureControlWindow, bounds,
            *behavior::Access::messageTrace, epoch, host, {}, memberObserver);
        diagnostic.Stop(); result.v4DiagnosticBudget = snowdesktop::external_transfer_observation::ObserveBudget();
        const auto& execution = receipt.Execution();
        result.productionStarted = execution.productionStarted;
        result.productionEntryUnknown = execution.productionEntryUnknown;
        dispatchAttempted = false;
        result.businessFailureObserved = execution.businessFailureObserved;
        result.eventOracleFailureObserved = execution.eventOracleFailureObserved;
        if (execution.productionStarted && (execution.businessFailureObserved || execution.eventOracleFailureObserved))
            result.status = BridgeStatus::production_failed;
        if (execution.productionStarted)
        {
            try { if (const auto sealed = receipt.SealedMemberBusiness())
                { result.v4NativeMemberBusiness = *sealed; result.v4NativeMemberBusinessSealed = true; }
                else { result.v4NativeMemberBusiness = business.Evidence(); result.observationUnavailable = true; } }
            catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v4_native_business_evidence_unavailable"); }
        }
        result.observationUnavailable = result.observationUnavailable || execution.additionalObservationUnavailable ||
            execution.status == ole_driver::Status::observation_unavailable || execution.status == ole_driver::Status::host_observation_failed;
        result.failure = execution.failure;
        result.obligations.insert(result.obligations.end(), execution.obligations.begin(), execution.obligations.end());
        result.postDispatchObligations = execution.postconditionsPending;
        result.unclassifiedObligations = execution.unclassifiedObligations;
        result.obligations.insert(result.obligations.end(), receipt.Unresolved().begin(), receipt.Unresolved().end());
        if (!receipt.Unresolved().empty()) result.observationUnavailable = true;
        try { result.v4NativeDeviceRecords = receipt.DeviceRecords(); }
        catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v4_native_device_records_unavailable"); }
        try { result.v4NativeQueuedRecords = receipt.QueuedRecords(); }
        catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v4_native_queue_records_unavailable"); }
        try { result.v4NativeTraceRecords = receipt.TraceRecords(); }
        catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v4_native_trace_records_unavailable"); }
        try { result.v4NativeBeforeEndStates = receipt.BeforeEndStates(); }
        catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v4_native_end_state_records_unavailable"); }
        try { result.v4NativeBeforeEndDelivery = receipt.BeforeEndDelivery(); }
        catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v4_native_end_delivery_records_unavailable"); }
        if (execution.productionStarted)
        {
            try
            {
                if (!behavior::Access::messageTrace)
                    throw ole_driver::ObservationUnavailable("v4_native_graph_trace_unavailable");
                result.v4NativeGraph = ole_driver::InspectV4NativeGraph(receipt, identity, verified, present, selected,
                    epoch, *behavior::Access::messageTrace, behavior::Access::completionWindow,
                    behavior::Access::fixtureControlWindow);
                result.hypothesisNotMetObserved = result.v4NativeGraph->identityValid &&
                    result.v4NativeGraph->hypothesisObservationRecorded && result.v4NativeGraph->hypothesisNotMetObserved;
                for (const auto& reason : result.v4NativeGraph->unresolved)
                    result.obligations.push_back("v4_native_graph:" + reason);
                if (result.v4NativeGraph->observationUnavailable || !result.v4NativeGraph->unresolved.empty())
                    result.observationUnavailable = true;
            }
            catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v4_native_graph_observation_unavailable"); }
        }
        if (execution.productionStarted)
        {
            try
            {
                if (!behavior::Access::messageTrace)
                    throw ole_driver::ObservationUnavailable("v4_native_cleanup_trace_unavailable");
                result.v4NativeCleanup = ole_driver::InspectV4NativeCleanup(receipt, identity, verified, present, selected,
                    epoch, *behavior::Access::messageTrace, behavior::Access::completionWindow,
                    behavior::Access::fixtureControlWindow);
                for (const auto& reason : result.v4NativeCleanup->unresolved)
                    result.obligations.push_back("v4_native_cleanup:" + reason);
                if (result.v4NativeCleanup->observationUnavailable || !result.v4NativeCleanup->intervalObserved ||
                    !result.v4NativeCleanup->unresolved.empty()) result.observationUnavailable = true;
            }
            catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v4_native_cleanup_observation_unavailable"); }
        }
        if (execution.productionStarted)
        {
            try
            {
                if (!behavior::Access::messageTrace)
                    throw ole_driver::ObservationUnavailable("v4_native_layout_trace_unavailable");
                result.v4NativeLayoutSave = ole_driver::InspectV4NativeLayoutSave(receipt, identity, verified, present, selected,
                    epoch, *behavior::Access::messageTrace, behavior::Access::completionWindow,
                    behavior::Access::fixtureControlWindow);
                for (const auto& reason : result.v4NativeLayoutSave->unresolved)
                    result.obligations.push_back("v4_native_layout:" + reason);
                if (result.v4NativeLayoutSave->observationUnavailable || !result.v4NativeLayoutSave->unresolved.empty())
                    result.observationUnavailable = true;
            }
            catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v4_native_layout_observation_unavailable"); }
        }
        if (execution.productionStarted && behavior::Access::messageTrace)
        {
            try
            {
                auto& local = result.v4NativeMemberBusinessBinding.emplace();
                const auto sealed = receipt.SealedMemberBusiness();
                if (sealed && receipt.SameCandidate(identity, verified, present, selected) &&
                    receipt.SameHost(epoch, *behavior::Access::messageTrace, behavior::Access::completionWindow,
                        behavior::Access::fixtureControlWindow) && result.v4NativeGraph && result.v4NativeCleanup && result.v4NativeLayoutSave)
                {
                    local.sameReceiptObserved = true;
                    local.graphIdentityObserved = result.v4NativeGraph->identityValid;
                    local.cleanupIntervalObserved = result.v4NativeCleanup->intervalObserved;
                    if (sealed->movingId) local.sourceSnapshotMatches = sealed->moving == initialFilePath &&
                        sealed->expectedMoving == initialFileBytes && behavior::OwnedProfileDesktopRoot::SameId(*sealed->movingId, initialFileId);
                    const auto& save = *result.v4NativeLayoutSave;
                    if (sealed->beforeReloadLayout && save.writerInputsObserved && save.saveRevision)
                    {
                        const auto& snapshot = *sealed->beforeReloadLayout;
                        local.snapshotFileId = snapshot.fileId; local.snapshotRevision = snapshot.revision; local.saveRevision = save.saveRevision;
                        const ole_driver::V4NativeLayoutRecord* path = nullptr;
                        const ole_driver::V4NativeLayoutRecord* bytes = nullptr;
                        bool duplicate = false;
                        using Phase = snowdesktop::external_transfer_observation::Phase;
                        for (const auto& record : save.records)
                        {
                            if (record.event.extra != *save.saveRevision) continue;
                            if (record.event.phase == Phase::layout_write_input_path)
                            { if (path) duplicate = true; else path = &record; }
                            if (record.event.phase == Phase::layout_write_input_bytes)
                            { if (bytes) duplicate = true; else bytes = &record; }
                        }
                        if (snapshot.captured && !snapshot.resourceUnavailable && snapshot.fileId && snapshot.revision &&
                            path && bytes && !duplicate && path->event.hasBlob && bytes->event.hasBlob &&
                            path->blob.size() == path->event.blobSize && bytes->blob.size() == bytes->event.blobSize)
                        {
                            const auto nativePath = snapshot.path.native();
                            local.layoutPathMatches = nativePath.size() <= (std::numeric_limits<std::size_t>::max)() / sizeof(wchar_t) &&
                                path->blob.size() == nativePath.size() * sizeof(wchar_t) &&
                                (path->blob.empty() || std::memcmp(path->blob.data(), nativePath.data(), path->blob.size()) == 0);
                            local.layoutBytesMatch = bytes->blob.size() == snapshot.rawBytes.size() &&
                                (bytes->blob.empty() || std::memcmp(bytes->blob.data(), snapshot.rawBytes.data(), bytes->blob.size()) == 0);
                            local.layoutRevisionMatches = *snapshot.revision == *save.saveRevision;
                        }
                        else local.unresolved.emplace_back("v4_native_member_layout_writer_or_snapshot_unavailable");
                    }
                    else local.unresolved.emplace_back("v4_native_member_pre_reload_layout_unavailable");
                }
                else local.unresolved.emplace_back("v4_native_member_sealed_receipt_evidence_unavailable");
                local.unresolved.emplace_back("v4_native_member_source_capture_causality_reentry_and_full_business_unverified");
                local.unresolved.emplace_back("v4_native_member_layout_non_atomic_durability_and_late_work_unverified");
                result.observationUnavailable = true;
            }
            catch (...) { result.observationUnavailable = true; }
        }
    }
    catch (const std::exception& error)
    { if (dispatchAttempted && !result.productionStarted) result.productionEntryUnknown = true;
      result.observationUnavailable = true; result.failure = error.what(); result.obligations.emplace_back("v4_native_preparation_or_observation_unavailable"); }
    catch (...)
    { if (dispatchAttempted && !result.productionStarted) result.productionEntryUnknown = true;
      result.observationUnavailable = true; result.obligations.emplace_back("v4_native_preparation_or_observation_unavailable"); }
    return result;
}

// Independent FileCategories source; visible indices are never interpreted as Collection indices.
inline BridgeResult PrepareV4NativeCategoryMove(
    const event_graph::V4FamilyIdentityContext& identity, const V4FamilyValidatedContext& verified,
    const std::vector<bool>& present, const event_graph::V4NativeJoinedGraphVariant& selected,
    behavior::OwnedProfileDesktopRoot& ownedRoot, const V4SingleFileFixture& fixture,
    GridCell landing, POINT targetScreen, const std::vector<ole_driver::NativePointInput>& points)
{
    BridgeResult result;
    bool dispatchAttempted = false;
    try
    {
        const auto audit = InspectV4NativeContainerCandidate(identity, verified);
        result.obligations = audit.obligations;
        if (!audit.candidateIdentityValid || !audit.narrowInputShape || audit.sourceContainer != "desktop_files" || Value(verified.Axes(), verified.Coordinate(), "C05") != "off" ||
            !fixture.inputPrepared || !fixture.created || !fixture.fileIdentityObserved ||
            fixture.candidateId != audit.candidateId || !ownedRoot.StillOwned() || !ownedRoot.profile || !ownedRoot.profile->app ||
            !behavior::OwnedProfileDesktopRoot::SameId(fixture.rootId, ownedRoot.desktopId))
        { result.obligations.emplace_back("v4_native_category_fixture_unavailable"); return result; }
        auto& profile = *ownedRoot.profile;
        if (fixture.root != profile.desktop || fixture.file.parent_path() != fixture.root)
        { result.obligations.emplace_back("v4_native_category_fixture_path_mismatch"); return result; }
        // Re-encode the observed fixture values for an independent comparison
        // with the authoritative input axes. This does not make the public
        // snapshot a provenance proof or prevent later mutation.
        constexpr char digits[] = "0123456789abcdef";
        const auto appendByte = [&](std::string& out, unsigned byte) {
            out.push_back(digits[(byte >> 4) & 15]); out.push_back(digits[byte & 15]);
        };
        std::string pathValue = "utf16le_hex_", bytesValue = "hex_";
        static_assert(sizeof(wchar_t) == 2, "Windows UTF-16 code units required");
        for (const auto unit : fixture.file.native())
        {
            const auto value = static_cast<std::uint16_t>(unit);
            appendByte(pathValue, value & 0xff); appendByte(pathValue, value >> 8);
        }
        for (const auto byte : fixture.bytes) appendByte(bytesValue, std::to_integer<unsigned>(byte));
        const auto& axes = verified.Axes(); const auto& candidate = verified.Coordinate();
        if (Value(axes, candidate, "V4.file_path.index_0.utf16le") != pathValue ||
            Value(axes, candidate, "V4.file_content.index_0.bytes") != bytesValue)
        { result.obligations.emplace_back("v4_native_category_candidate_file_input_mismatch"); return result; }
        behavior::ObservedPhysicalFile fileSnapshot(fixture.file);
        if (!behavior::OwnedProfileDesktopRoot::SameId(fixture.fileId, fileSnapshot.Identity()) ||
            !fileSnapshot.PathStillNamesObject(fixture.file) || !fileSnapshot.HasBytes(fixture.bytes))
        { result.obligations.emplace_back("v4_native_category_file_snapshot_unavailable"); return result; }
        // Retain the values that were actually checked before any device input;
        // the caller's public fixture snapshot is not itself an execution seal.
        const auto initialFilePath = fixture.file;
        const auto initialFileBytes = fixture.bytes;
        const auto initialFileId = fileSnapshot.Identity();
        const auto replay = event_graph::V4NativeJoinedGraph::Replay(identity, verified, present, selected.id);
        if (!replay.identityValid || !replay.errors.empty() || replay.variant.id != selected.id ||
            replay.variant.classes != selected.classes || replay.variant.order != selected.order ||
            replay.variant.unresolved != selected.unresolved)
        { result.obligations.emplace_back("v4_native_category_graph_identity_unavailable"); return result; }
        result.graphUnresolved = replay.variant.unresolved;
        for (const auto& reason : fixture.unresolved) result.obligations.push_back("v4_native_fixture:" + reason);
        result.obligations.emplace_back("v4_native_profile_root_creation_provenance_and_file_lifetime_lease_unimplemented");
        result.obligations.emplace_back("v4_native_candidate_geometry_and_same_run_oracle_capability_unimplemented");

        // Prepare only the isolated starting scene and inspect its actual
        // geometry. Existing obligations remain intact at the dispatch gate.
        behavior::Access::EnableProductionDispatch(*profile.app, false);
        behavior::Access::EnableFixtureControlWindow(*profile.app);
        behavior::Access::EnableNativePointerDispatch(*profile.app);
        behavior::Wait([&] { return behavior::Access::NativeDragIdle(*profile.app) &&
            behavior::Access::ContainerWorkIdle(*profile.app) && behavior::Access::LayoutSaveIdle(*profile.app); });
        behavior::Access::PrepareExpandedNativeSource(*profile.app, {initialFilePath, profile.desktop / L"Sentinel.txt"}, 2);
        auto epoch = behavior::Access::CreateNativeHostEpoch(*profile.app);
        behavior::V4NativeCategoryObservation business(ownedRoot, initialFilePath, initialFileBytes, std::move(landing), verified, identity);
        result.v4NativeCategoryBusiness = business.Evidence();
        const auto sourceGeometry = behavior::Access::InspectNativeCategorySource(*profile.app, initialFilePath);
        const auto start = sourceGeometry.screen;
        const auto geometry = behavior::Access::InspectNativeLanding(*profile.app, business.ExpectedLanding());
        const auto v = [&](std::string_view id) { return Value(verified.Axes(), verified.Coordinate(), id); };
        const POINT effectiveTarget = points.empty() ? geometry.screenCenter : targetScreen;
        const auto& initialBusiness = *result.v4NativeCategoryBusiness;
        const bool sourceBaselineMatches = initialBusiness.inputObserved && initialBusiness.candidateId == audit.candidateId &&
            initialBusiness.initialGeometry && initialBusiness.initialState &&
            initialBusiness.initialGeometry->host == sourceGeometry.host && initialBusiness.initialGeometry->container == sourceGeometry.container &&
            initialBusiness.initialGeometry->ownerThread == sourceGeometry.ownerThread &&
            initialBusiness.initialGeometry->activeCategory == sourceGeometry.activeCategory &&
            initialBusiness.initialGeometry->memberKey == sourceGeometry.memberKey &&
            initialBusiness.initialGeometry->visibleKeys == sourceGeometry.visibleKeys &&
            initialBusiness.initialGeometry->visibleIndex == sourceGeometry.visibleIndex &&
            initialBusiness.initialGeometry->layoutRevision == sourceGeometry.layoutRevision &&
            EqualRect(&initialBusiness.initialGeometry->memberBounds, &sourceGeometry.memberBounds) &&
            EqualRect(&initialBusiness.initialGeometry->viewport, &sourceGeometry.viewport) &&
            initialBusiness.initialGeometry->screen.x == sourceGeometry.screen.x && initialBusiness.initialGeometry->screen.y == sourceGeometry.screen.y &&
            initialBusiness.initialState->layoutRevision == sourceGeometry.layoutRevision &&
            geometry.layoutRevision == sourceGeometry.layoutRevision && !initialBusiness.initialState->widget.autoCollect;
        const bool landingMatches = EndpointSize(v("target.E23")) == geometry.columns &&
            EndpointSize(v("target.E24")) == geometry.rows && v("H01.region") == "empty_slot" &&
            effectiveTarget.x == geometry.screenCenter.x && effectiveTarget.y == geometry.screenCenter.y;
        result.v4NativeCategoryInitialInput = BridgeResult::V4NativeCategoryInitialInput{
            audit.candidateId, sourceGeometry, geometry, landingMatches, sourceBaselineMatches, std::nullopt};
        if (!landingMatches || !sourceBaselineMatches)
        { result.obligations.emplace_back("v4_native_landing_geometry_candidate_mismatch"); return result; }
        const auto program = verified.TransferProgram();
        result.v4NativeCategoryPointProposal.emplace();
        auto& proposal = *result.v4NativeCategoryPointProposal;
        proposal.candidateId = audit.candidateId;
        const auto dx = static_cast<std::int64_t>(geometry.screenCenter.x) - start.x;
        const auto dy = static_cast<std::int64_t>(geometry.screenCenter.y) - start.y;
        const auto abs64 = [](std::int64_t n) { return n < 0 ? -n : n; };
        if (v("H08.first_direction") == "unobserved" && v("H08.later_direction") == "na" &&
            v("G02.hover_first") == "na" && v("G02.hover_later") == "na" &&
            PtInRect(&geometry.screenPage, POINT{start.x, start.y}) && PtInRect(&geometry.screenPage, geometry.screenCenter) &&
            (abs64(dx) > (std::max)(8, GetSystemMetrics(SM_CXDRAG)) || abs64(dy) > (std::max)(8, GetSystemMetrics(SM_CYDRAG))))
        {
            std::size_t starts = 0, moves = 0, releases = 0; bool extraMotion = false;
            for (std::size_t i = 0; i < program.steps.size(); ++i)
            {
                const auto& kind = program.steps[i].kind;
                if (kind == "source_start") { ++starts; proposal.points.push_back({i, POINT{start.x, start.y}}); }
                else if (kind == "move_past_drag_threshold") { ++moves; proposal.points.push_back({i, geometry.screenCenter}); }
                else if (kind == "release") { ++releases; proposal.points.push_back({i, geometry.screenCenter}); }
                else if (kind == "pointer_direction" || kind == "hover" || kind == "pointer_direction_later" || kind == "hover_later")
                    extraMotion = true;
            }
            proposal.prepared = starts == 1 && moves == 1 && releases == 1 && !extraMotion;
        }
        if (!proposal.prepared) { proposal.points.clear(); proposal.unresolved.emplace_back("v4_native_category_point_motion_unimplemented"); }
        const auto samePoints = [](const auto& expected, const auto& actual) {
            if (expected.size() != actual.size()) return false;
            for (const auto& item : expected)
            {
                const auto match = [&](const auto& value) { return value.programIndex == item.programIndex; };
                const auto found = std::find_if(actual.begin(), actual.end(), match);
                if (found == actual.end() || std::count_if(expected.begin(), expected.end(), match) != 1 ||
                    std::count_if(actual.begin(), actual.end(), match) != 1 || found->screen.x != item.screen.x || found->screen.y != item.screen.y)
                    return false;
            }
            return true;
        };
        proposal.generatedPointsUsed = points.empty() && proposal.prepared;
        const auto effectivePoints = proposal.generatedPointsUsed ? proposal.points : points;
        if (!proposal.prepared || !samePoints(proposal.points, effectivePoints))
            result.obligations.emplace_back("v4_native_category_point_proposal_unavailable");
        proposal.unresolved.emplace_back("v4_native_category_point_actual_hit_capture_and_commit_unverified");
        behavior::V4NativeCategoryDeviceObservation observed(business, identity, verified, present, selected,
            epoch, POINT{start.x, start.y}, effectiveTarget);
        const RECT bounds = geometry.screenPage;
        const auto plan = ole_driver::BuildV4NativeExecutionPlan(identity, verified, present, selected,
            effectivePoints, behavior::Access::completionWindow, bounds);
        result.obligations.insert(result.obligations.end(), plan.preconditions.begin(), plan.preconditions.end());
        result.postDispatchObligations = plan.postconditions;
        result.unclassifiedObligations = plan.unclassified;
        bool pointsMatch = plan.points.size() == program.steps.size() && plan.sourcePoint &&
            plan.sourcePoint->x == start.x && plan.sourcePoint->y == start.y;
        bool releaseObserved = false;
        if (!pointsMatch)
            result.obligations.emplace_back("v4_native_source_point_candidate_unavailable");
        else for (std::size_t i = 0; i < program.steps.size(); ++i)
            if (program.steps[i].kind == "release")
            {
                releaseObserved = true;
                if (!plan.points[i] || plan.points[i]->x != geometry.screenCenter.x ||
                    plan.points[i]->y != geometry.screenCenter.y)
                { pointsMatch = false; result.obligations.emplace_back("v4_native_release_point_candidate_unavailable"); }
            }
        pointsMatch = pointsMatch && releaseObserved;
        result.v4NativeCategoryInitialInput->sourceAndReleasePointsMatch = pointsMatch;
        if (!plan.unclassified.empty()) result.obligations.emplace_back("v4_native_unclassified_obligation_capability_unimplemented");
        if (!result.obligations.empty()) return result;
        auto host = observed.Boundaries();
        const auto originalBeforeBegin = host.beforeDeviceBegin;
        host.beforeDeviceBegin = [&] {
            const auto current = behavior::Access::InspectNativeLanding(*profile.app, business.ExpectedLanding());
            if (current.layoutRevision != geometry.layoutRevision || current.columns != geometry.columns ||
                current.rows != geometry.rows || !EqualRect(&current.clientCell, &geometry.clientCell) ||
                !EqualRect(&current.screenPage, &geometry.screenPage) ||
                current.screenCenter.x != geometry.screenCenter.x || current.screenCenter.y != geometry.screenCenter.y)
                throw ole_driver::ObservationUnavailable("v4_native_landing_changed_before_device_begin");
            const auto currentSource = behavior::Access::InspectNativeCategorySource(*profile.app, initialFilePath);
            if (currentSource.host != sourceGeometry.host || currentSource.container != sourceGeometry.container ||
                currentSource.ownerThread != sourceGeometry.ownerThread || currentSource.activeCategory != sourceGeometry.activeCategory ||
                currentSource.memberKey != sourceGeometry.memberKey || currentSource.visibleKeys != sourceGeometry.visibleKeys ||
                currentSource.visibleIndex != sourceGeometry.visibleIndex || currentSource.layoutRevision != sourceGeometry.layoutRevision ||
                !EqualRect(&currentSource.memberBounds, &sourceGeometry.memberBounds) ||
                !EqualRect(&currentSource.viewport, &sourceGeometry.viewport) || currentSource.screen.x != start.x ||
                currentSource.screen.y != start.y || !samePoints(proposal.points, effectivePoints))
                throw ole_driver::ObservationUnavailable("v4_native_category_point_proposal_changed_before_device_begin");
            proposal.revalidated = true;
            originalBeforeBegin();
        };
        if (!snowdesktop::external_transfer_observation::Enable())
            throw ole_driver::ObservationUnavailable("v4_native_layout_diagnostic_unavailable");
        struct StopNativeDiagnostic final
        {
            bool active = true;
            void Stop() noexcept
            { if (active) { snowdesktop::external_transfer_observation::Stop(); active = false; } }
            ~StopNativeDiagnostic() { Stop(); }
        } diagnostic;
        const auto originalAwait = host.awaitCompletion;
        host.awaitCompletion = [&](const EventStep& step) {
            originalAwait(step);
            // Freeze the observation interval before the independent oracle
            // requests a disk reload, which can itself schedule layout work.
            diagnostic.Stop(); result.v4DiagnosticBudget = snowdesktop::external_transfer_observation::ObserveBudget();
        };
        const ole_driver::V4NativeCategoryObserverBoundaries categoryObserver{
            [&](const std::shared_ptr<ole_driver::V4NativeCategoryExecutionKey>& key) { business.BindExecution(key, epoch); },
            [&](const std::shared_ptr<ole_driver::V4NativeCategoryExecutionKey>& key) { business.CaptureExecution(key, epoch); }};
        dispatchAttempted = true;
        const auto receipt = ole_driver::V4NativeSequence::RunBound(identity, verified, present, selected,
            effectivePoints, behavior::Access::completionWindow, behavior::Access::fixtureControlWindow, bounds,
            *behavior::Access::messageTrace, epoch, host, {}, {}, categoryObserver);
        diagnostic.Stop(); result.v4DiagnosticBudget = snowdesktop::external_transfer_observation::ObserveBudget();
        const auto& execution = receipt.Execution();
        result.productionStarted = execution.productionStarted;
        result.productionEntryUnknown = execution.productionEntryUnknown;
        dispatchAttempted = false;
        result.businessFailureObserved = execution.businessFailureObserved;
        result.eventOracleFailureObserved = execution.eventOracleFailureObserved;
        if (execution.productionStarted && (execution.businessFailureObserved || execution.eventOracleFailureObserved))
            result.status = BridgeStatus::production_failed;
        if (execution.productionStarted || execution.productionEntryUnknown)
        {
            try
            {
                if (!behavior::Access::messageTrace)
                    throw ole_driver::ObservationUnavailable("v4_native_category_calls_trace_unavailable");
                result.v4NativeCategoryCalls = ole_driver::InspectV4NativeCategoryCalls(receipt, identity, verified,
                    present, selected, epoch, *behavior::Access::messageTrace, behavior::Access::completionWindow,
                    behavior::Access::fixtureControlWindow);
                if (result.v4NativeCategoryCalls->resourceUnavailable || result.v4NativeCategoryCalls->observationUnavailable ||
                    !result.v4NativeCategoryCalls->unresolved.empty())
                    result.observationUnavailable = true;
            }
            catch (const std::bad_alloc&) { result.v4NativeCategoryCalls.emplace(); result.v4NativeCategoryCalls->resourceUnavailable = true; result.observationUnavailable = true; }
            catch (const std::length_error&) { result.v4NativeCategoryCalls.emplace(); result.v4NativeCategoryCalls->resourceUnavailable = true; result.observationUnavailable = true; }
            catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v4_native_category_calls_unavailable"); }
        }
        if (execution.productionStarted || execution.productionEntryUnknown)
        {
            try
            {
                if (!behavior::Access::messageTrace)
                    throw ole_driver::ObservationUnavailable("v4_native_category_commit_trace_unavailable");
                result.v4NativeCategoryDesktopCommit = ole_driver::InspectV4NativeCategoryDesktopCommit(receipt, identity, verified,
                    present, selected, epoch, *behavior::Access::messageTrace, behavior::Access::completionWindow,
                    behavior::Access::fixtureControlWindow);
                if (result.v4NativeCategoryDesktopCommit->resourceUnavailable ||
                    result.v4NativeCategoryDesktopCommit->observationUnavailable ||
                    !result.v4NativeCategoryDesktopCommit->unresolved.empty()) result.observationUnavailable = true;
            }
            catch (const std::bad_alloc&) { result.v4NativeCategoryDesktopCommit.emplace(); result.v4NativeCategoryDesktopCommit->resourceUnavailable = true; result.observationUnavailable = true; }
            catch (const std::length_error&) { result.v4NativeCategoryDesktopCommit.emplace(); result.v4NativeCategoryDesktopCommit->resourceUnavailable = true; result.observationUnavailable = true; }
            catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v4_native_category_commit_unavailable"); }
        }
        if (execution.productionStarted)
        {
            try { if (const auto sealed = receipt.SealedCategoryBusiness())
                { result.v4NativeCategoryBusiness = *sealed; result.v4NativeCategoryBusinessSealed = true; }
                else { result.v4NativeCategoryBusiness = business.Evidence(); result.observationUnavailable = true; } }
            catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v4_native_business_evidence_unavailable"); }
        }
        result.observationUnavailable = result.observationUnavailable || execution.additionalObservationUnavailable ||
            execution.status == ole_driver::Status::observation_unavailable || execution.status == ole_driver::Status::host_observation_failed;
        result.failure = execution.failure;
        result.obligations.insert(result.obligations.end(), execution.obligations.begin(), execution.obligations.end());
        result.postDispatchObligations = execution.postconditionsPending;
        result.unclassifiedObligations = execution.unclassifiedObligations;
        result.obligations.insert(result.obligations.end(), receipt.Unresolved().begin(), receipt.Unresolved().end());
        if (!receipt.Unresolved().empty()) result.observationUnavailable = true;
        try { result.v4NativeDeviceRecords = receipt.DeviceRecords(); }
        catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v4_native_device_records_unavailable"); }
        try { result.v4NativeQueuedRecords = receipt.QueuedRecords(); }
        catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v4_native_queue_records_unavailable"); }
        try { result.v4NativeTraceRecords = receipt.TraceRecords(); }
        catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v4_native_trace_records_unavailable"); }
        try { result.v4NativeBeforeEndStates = receipt.BeforeEndStates(); }
        catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v4_native_end_state_records_unavailable"); }
        try { result.v4NativeBeforeEndDelivery = receipt.BeforeEndDelivery(); }
        catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v4_native_end_delivery_records_unavailable"); }
        if (execution.productionStarted)
        {
            try
            {
                if (!behavior::Access::messageTrace)
                    throw ole_driver::ObservationUnavailable("v4_native_graph_trace_unavailable");
                result.v4NativeGraph = ole_driver::InspectV4NativeGraph(receipt, identity, verified, present, selected,
                    epoch, *behavior::Access::messageTrace, behavior::Access::completionWindow,
                    behavior::Access::fixtureControlWindow);
                result.hypothesisNotMetObserved = result.v4NativeGraph->identityValid &&
                    result.v4NativeGraph->hypothesisObservationRecorded && result.v4NativeGraph->hypothesisNotMetObserved;
                for (const auto& reason : result.v4NativeGraph->unresolved)
                    result.obligations.push_back("v4_native_graph:" + reason);
                if (result.v4NativeGraph->observationUnavailable || !result.v4NativeGraph->unresolved.empty())
                    result.observationUnavailable = true;
            }
            catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v4_native_graph_observation_unavailable"); }
        }
        if (execution.productionStarted)
        {
            try
            {
                if (!behavior::Access::messageTrace)
                    throw ole_driver::ObservationUnavailable("v4_native_cleanup_trace_unavailable");
                result.v4NativeCleanup = ole_driver::InspectV4NativeCleanup(receipt, identity, verified, present, selected,
                    epoch, *behavior::Access::messageTrace, behavior::Access::completionWindow,
                    behavior::Access::fixtureControlWindow);
                for (const auto& reason : result.v4NativeCleanup->unresolved)
                    result.obligations.push_back("v4_native_cleanup:" + reason);
                if (result.v4NativeCleanup->observationUnavailable || !result.v4NativeCleanup->intervalObserved ||
                    !result.v4NativeCleanup->unresolved.empty()) result.observationUnavailable = true;
            }
            catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v4_native_cleanup_observation_unavailable"); }
        }
        if (execution.productionStarted)
        {
            try
            {
                if (!behavior::Access::messageTrace)
                    throw ole_driver::ObservationUnavailable("v4_native_layout_trace_unavailable");
                result.v4NativeLayoutSave = ole_driver::InspectV4NativeLayoutSave(receipt, identity, verified, present, selected,
                    epoch, *behavior::Access::messageTrace, behavior::Access::completionWindow,
                    behavior::Access::fixtureControlWindow);
                for (const auto& reason : result.v4NativeLayoutSave->unresolved)
                    result.obligations.push_back("v4_native_layout:" + reason);
                if (result.v4NativeLayoutSave->observationUnavailable || !result.v4NativeLayoutSave->unresolved.empty())
                    result.observationUnavailable = true;
            }
            catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v4_native_layout_observation_unavailable"); }
        }
        if (execution.productionStarted && behavior::Access::messageTrace)
        {
            try
            {
                auto& local = result.v4NativeCategoryBusinessBinding.emplace();
                const auto sealed = receipt.SealedCategoryBusiness();
                if (sealed && receipt.SameCandidate(identity, verified, present, selected) &&
                    receipt.SameHost(epoch, *behavior::Access::messageTrace, behavior::Access::completionWindow,
                        behavior::Access::fixtureControlWindow) && result.v4NativeGraph && result.v4NativeCleanup && result.v4NativeLayoutSave)
                {
                    local.sameReceiptObserved = true;
                    local.graphIdentityObserved = result.v4NativeGraph->identityValid;
                    local.cleanupIntervalObserved = result.v4NativeCleanup->intervalObserved;
                    if (sealed->candidateId == audit.candidateId && sealed->movingId) local.sourceSnapshotMatches = sealed->moving == initialFilePath &&
                        sealed->expectedMoving == initialFileBytes && behavior::OwnedProfileDesktopRoot::SameId(*sealed->movingId, initialFileId);
                    const auto& save = *result.v4NativeLayoutSave;
                    if (sealed->beforeReloadLayout && save.writerInputsObserved && save.saveRevision)
                    {
                        const auto& snapshot = *sealed->beforeReloadLayout;
                        local.snapshotFileId = snapshot.fileId; local.snapshotRevision = snapshot.revision; local.saveRevision = save.saveRevision;
                        const ole_driver::V4NativeLayoutRecord* path = nullptr;
                        const ole_driver::V4NativeLayoutRecord* bytes = nullptr;
                        bool duplicate = false;
                        using Phase = snowdesktop::external_transfer_observation::Phase;
                        for (const auto& record : save.records)
                        {
                            if (record.event.extra != *save.saveRevision) continue;
                            if (record.event.phase == Phase::layout_write_input_path)
                            { if (path) duplicate = true; else path = &record; }
                            if (record.event.phase == Phase::layout_write_input_bytes)
                            { if (bytes) duplicate = true; else bytes = &record; }
                        }
                        if (snapshot.captured && !snapshot.resourceUnavailable && snapshot.fileId && snapshot.revision &&
                            path && bytes && !duplicate && path->event.hasBlob && bytes->event.hasBlob &&
                            path->blob.size() == path->event.blobSize && bytes->blob.size() == bytes->event.blobSize)
                        {
                            const auto nativePath = snapshot.path.native();
                            local.layoutPathMatches = nativePath.size() <= (std::numeric_limits<std::size_t>::max)() / sizeof(wchar_t) &&
                                path->blob.size() == nativePath.size() * sizeof(wchar_t) &&
                                (path->blob.empty() || std::memcmp(path->blob.data(), nativePath.data(), path->blob.size()) == 0);
                            local.layoutBytesMatch = bytes->blob.size() == snapshot.rawBytes.size() &&
                                (bytes->blob.empty() || std::memcmp(bytes->blob.data(), snapshot.rawBytes.data(), bytes->blob.size()) == 0);
                            local.layoutRevisionMatches = *snapshot.revision == *save.saveRevision;
                        }
                        else local.unresolved.emplace_back("v4_native_category_layout_writer_or_snapshot_unavailable");
                    }
                    else local.unresolved.emplace_back("v4_native_category_pre_reload_layout_unavailable");
                }
                else local.unresolved.emplace_back("v4_native_category_sealed_receipt_evidence_unavailable");
                local.unresolved.emplace_back("v4_native_category_source_capture_causality_reentry_and_full_business_unverified");
                local.unresolved.emplace_back("v4_native_category_layout_non_atomic_durability_and_late_work_unverified");
                result.observationUnavailable = true;
            }
            catch (...) { result.observationUnavailable = true; }
        }
    }
    catch (const std::exception& error)
    { if (dispatchAttempted && !result.productionStarted) result.productionEntryUnknown = true;
      result.observationUnavailable = true; result.failure = error.what(); result.obligations.emplace_back("v4_native_preparation_or_observation_unavailable"); }
    catch (...)
    { if (dispatchAttempted && !result.productionStarted) result.productionEntryUnknown = true;
      result.observationUnavailable = true; result.obligations.emplace_back("v4_native_preparation_or_observation_unavailable"); }
    return result;
}

// Same-Collection reorder, bound to real source and proposed target geometry.
inline BridgeResult PrepareV4NativeCollectionReorder(
    const event_graph::V4FamilyIdentityContext& identity, const V4FamilyValidatedContext& verified,
    const std::vector<bool>& present, const event_graph::V4NativeJoinedGraphVariant& selected,
    behavior::OwnedProfileDesktopRoot& ownedRoot, const V4SingleFileFixture& fixture,
    const std::vector<ole_driver::NativePointInput>& points)
{
    BridgeResult result;
    bool dispatchAttempted = false;
    try
    {
        V4NativeContainerPreflight audit;
        if (identity.Status() != V4FamilyStatus::schema_only || verified.Status() != V4CoordinateStatus::coordinate_ready ||
            verified.TaskBActive() || identity.FamilyBinding() != verified.FamilyBinding() || identity.Axes().size() != verified.Axes().size())
        { result.obligations.emplace_back("v4_native_reorder_context_unavailable"); return result; }
        for (std::size_t i = 0; i < identity.Axes().size(); ++i)
        {
            const auto& a = identity.Axes()[i]; const auto& b = verified.Axes()[i];
            if (a.id != b.id || a.role != b.role || a.values != b.values || a.label != b.label)
            { result.obligations.emplace_back("v4_native_reorder_schema_mismatch"); return result; }
        }
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        const auto value = [&](std::string_view id) { return Value(verified.Axes(), verified.Coordinate(), id); };
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty() || value("T01") != "internal_native" ||
            value("source.E01") != "free_desktop" || value("target.E01") != "free_desktop" ||
            value("source.E02") != "collection" || value("target.E02") != "collection" ||
            value("source.E03") != "widget_body" || value("target.E03") != "widget_body" ||
            value("H01.region") != "before_item" || value("H05.position") != "first" ||
            value("H05.indicated_identity") != "unselected_target" || value("H05.surface") != "members" ||
            value("H05.filter") != "unfiltered" || value("H05.source_relation") != "adjacent_original" ||
            value("H05.scroll") != "unchanged" || value("C01") != "manual" || value("C02") != "na" ||
            value("C03") != "manual" || value("C04") != "na" || value("C05") != "off" || value("C06") != "off" ||
            value("P01.primary") != "file" || value("P01.second") != "na" || value("P02.source_actual_count") != "one" ||
            value("G01.button") != "left" || value("G04.termination") != "drop" || value("X03.task_count") != "one" ||
            !fixture.inputPrepared || !fixture.created || !fixture.fileIdentityObserved || fixture.candidateId != raw.id ||
            !ownedRoot.StillOwned() || !ownedRoot.profile || !ownedRoot.profile->app ||
            !behavior::OwnedProfileDesktopRoot::SameId(fixture.rootId, ownedRoot.desktopId))
        { result.obligations.emplace_back("v4_native_reorder_candidate_or_fixture_unavailable"); return result; }
        audit.candidateId = raw.id;
        result.obligations.insert(result.obligations.end(), raw.unresolved.begin(), raw.unresolved.end());
        result.obligations.insert(result.obligations.end(), verified.Unresolved().begin(), verified.Unresolved().end());
        auto& profile = *ownedRoot.profile;
        if (fixture.root != profile.desktop || fixture.file.parent_path() != fixture.root)
        { result.obligations.emplace_back("v4_native_reorder_fixture_path_mismatch"); return result; }
        // Re-encode the observed fixture values for an independent comparison
        // with the authoritative input axes. This does not make the public
        // snapshot a provenance proof or prevent later mutation.
        constexpr char digits[] = "0123456789abcdef";
        const auto appendByte = [&](std::string& out, unsigned byte) {
            out.push_back(digits[(byte >> 4) & 15]); out.push_back(digits[byte & 15]);
        };
        std::string pathValue = "utf16le_hex_", bytesValue = "hex_";
        static_assert(sizeof(wchar_t) == 2, "Windows UTF-16 code units required");
        for (const auto unit : fixture.file.native())
        {
            const auto value = static_cast<std::uint16_t>(unit);
            appendByte(pathValue, value & 0xff); appendByte(pathValue, value >> 8);
        }
        for (const auto byte : fixture.bytes) appendByte(bytesValue, std::to_integer<unsigned>(byte));
        const auto& axes = verified.Axes(); const auto& candidate = verified.Coordinate();
        if (Value(axes, candidate, "V4.file_path.index_0.utf16le") != pathValue ||
            Value(axes, candidate, "V4.file_content.index_0.bytes") != bytesValue)
        { result.obligations.emplace_back("v4_native_reorder_candidate_file_input_mismatch"); return result; }
        behavior::ObservedPhysicalFile fileSnapshot(fixture.file);
        if (!behavior::OwnedProfileDesktopRoot::SameId(fixture.fileId, fileSnapshot.Identity()) ||
            !fileSnapshot.PathStillNamesObject(fixture.file) || !fileSnapshot.HasBytes(fixture.bytes))
        { result.obligations.emplace_back("v4_native_reorder_file_snapshot_unavailable"); return result; }
        // Retain the values that were actually checked before any device input;
        // the caller's public fixture snapshot is not itself an execution seal.
        const auto initialFilePath = fixture.file;
        const auto initialFileBytes = fixture.bytes;
        const auto initialFileId = fileSnapshot.Identity();
        const auto replay = event_graph::V4NativeJoinedGraph::Replay(identity, verified, present, selected.id);
        if (!replay.identityValid || !replay.errors.empty() || replay.variant.id != selected.id ||
            replay.variant.classes != selected.classes || replay.variant.order != selected.order ||
            replay.variant.unresolved != selected.unresolved)
        { result.obligations.emplace_back("v4_native_reorder_graph_identity_unavailable"); return result; }
        result.graphUnresolved = replay.variant.unresolved;
        for (const auto& reason : fixture.unresolved) result.obligations.push_back("v4_native_fixture:" + reason);
        result.obligations.emplace_back("v4_native_profile_root_creation_provenance_and_file_lifetime_lease_unimplemented");
        result.obligations.emplace_back("v4_native_candidate_geometry_and_same_run_oracle_capability_unimplemented");

        // Prepare only the isolated starting scene and inspect its actual
        // geometry. Existing obligations remain intact at the dispatch gate.
        behavior::Access::EnableProductionDispatch(*profile.app, false);
        behavior::Access::EnableFixtureControlWindow(*profile.app);
        behavior::Access::EnableNativePointerDispatch(*profile.app);
        behavior::Wait([&] { return behavior::Access::NativeDragIdle(*profile.app) &&
            behavior::Access::ContainerWorkIdle(*profile.app) && behavior::Access::LayoutSaveIdle(*profile.app); });
        behavior::Access::PrepareExpandedNativeSource(*profile.app, {profile.desktop / L"Sentinel.txt", initialFilePath}, 1);
        auto epoch = behavior::Access::CreateNativeHostEpoch(*profile.app);
        behavior::V4NativeCollectionReorderObservation business(ownedRoot, initialFilePath, initialFileBytes, verified, identity);
        result.v4NativeReorderBusiness = business.Evidence();
        const auto geometry = behavior::Access::InspectNativeCollectionReorderGeometry(*profile.app, initialFilePath, profile.desktop / L"Sentinel.txt");
        const auto start = geometry.movingScreen; const auto target = geometry.targetScreen;
        POINT topLeft{geometry.viewport.left, geometry.viewport.top}, bottomRight{geometry.viewport.right, geometry.viewport.bottom};
        if (!ClientToScreen(behavior::Access::completionWindow, &topLeft) || !ClientToScreen(behavior::Access::completionWindow, &bottomRight))
        { result.obligations.emplace_back("v4_native_reorder_viewport_conversion_unavailable"); return result; }
        const RECT bounds{topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
        const auto v = [&](std::string_view id) { return Value(verified.Axes(), verified.Coordinate(), id); };
        const auto& initialBusiness = *result.v4NativeReorderBusiness;
        const bool sourceBaselineMatches = initialBusiness.inputObserved && initialBusiness.candidateId == audit.candidateId &&
            initialBusiness.initialGeometry && initialBusiness.initialGeometry->host == geometry.host &&
            initialBusiness.initialGeometry->container == geometry.container && initialBusiness.initialGeometry->ownerThread == geometry.ownerThread &&
            initialBusiness.initialGeometry->movingKey == geometry.movingKey && initialBusiness.initialGeometry->targetKey == geometry.targetKey &&
            initialBusiness.initialGeometry->itemKeys == geometry.itemKeys && initialBusiness.initialGeometry->movingIndex == geometry.movingIndex &&
            initialBusiness.initialGeometry->targetIndex == geometry.targetIndex && initialBusiness.initialGeometry->layoutRevision == geometry.layoutRevision &&
            EqualRect(&initialBusiness.initialGeometry->movingBounds, &geometry.movingBounds) &&
            EqualRect(&initialBusiness.initialGeometry->targetBounds, &geometry.targetBounds) &&
            EqualRect(&initialBusiness.initialGeometry->viewport, &geometry.viewport) &&
            initialBusiness.initialGeometry->movingScreen.x == start.x && initialBusiness.initialGeometry->movingScreen.y == start.y &&
            initialBusiness.initialGeometry->targetScreen.x == target.x && initialBusiness.initialGeometry->targetScreen.y == target.y;
        result.v4NativeReorderInitialInput = BridgeResult::V4NativeReorderInitialInput{audit.candidateId, geometry, sourceBaselineMatches, std::nullopt};
        if (!sourceBaselineMatches || geometry.targetIndex != 0 || geometry.movingIndex != 1)
        { result.obligations.emplace_back("v4_native_reorder_source_baseline_unavailable"); return result; }
        const auto program = verified.TransferProgram();
        result.v4NativeReorderPointProposal.emplace();
        auto& proposal = *result.v4NativeReorderPointProposal;
        proposal.candidateId = audit.candidateId;
        const auto dx = static_cast<std::int64_t>(target.x) - start.x;
        const auto dy = static_cast<std::int64_t>(target.y) - start.y;
        const auto abs64 = [](std::int64_t n) { return n < 0 ? -n : n; };
        if (v("H08.first_direction") == "unobserved" && v("H08.later_direction") == "na" &&
            v("G02.hover_first") == "na" && v("G02.hover_later") == "na" &&
            PtInRect(&bounds, POINT{start.x, start.y}) && PtInRect(&bounds, target) &&
            (abs64(dx) > (std::max)(8, GetSystemMetrics(SM_CXDRAG)) || abs64(dy) > (std::max)(8, GetSystemMetrics(SM_CYDRAG))))
        {
            std::size_t starts = 0, moves = 0, releases = 0; bool extraMotion = false;
            for (std::size_t i = 0; i < program.steps.size(); ++i)
            {
                const auto& kind = program.steps[i].kind;
                if (kind == "source_start") { ++starts; proposal.points.push_back({i, POINT{start.x, start.y}}); }
                else if (kind == "move_past_drag_threshold") { ++moves; proposal.points.push_back({i, target}); }
                else if (kind == "release") { ++releases; proposal.points.push_back({i, target}); }
                else if (kind == "pointer_direction" || kind == "hover" || kind == "pointer_direction_later" || kind == "hover_later")
                    extraMotion = true;
            }
            proposal.prepared = starts == 1 && moves == 1 && releases == 1 && !extraMotion;
        }
        if (!proposal.prepared) { proposal.points.clear(); proposal.unresolved.emplace_back("v4_native_reorder_point_motion_unimplemented"); }
        const auto samePoints = [](const auto& expected, const auto& actual) {
            if (expected.size() != actual.size()) return false;
            for (const auto& item : expected)
            {
                const auto match = [&](const auto& value) { return value.programIndex == item.programIndex; };
                const auto found = std::find_if(actual.begin(), actual.end(), match);
                if (found == actual.end() || std::count_if(expected.begin(), expected.end(), match) != 1 ||
                    std::count_if(actual.begin(), actual.end(), match) != 1 || found->screen.x != item.screen.x || found->screen.y != item.screen.y)
                    return false;
            }
            return true;
        };
        proposal.generatedPointsUsed = points.empty() && proposal.prepared;
        const auto effectivePoints = proposal.generatedPointsUsed ? proposal.points : points;
        if (!proposal.prepared || !samePoints(proposal.points, effectivePoints))
            result.obligations.emplace_back("v4_native_reorder_point_proposal_unavailable");
        proposal.unresolved.emplace_back("v4_native_reorder_slot_item_identity_and_commit_causality_unverified");
        behavior::V4NativeCollectionReorderDeviceObservation observed(business, identity, verified, present, selected,
            epoch, POINT{start.x, start.y}, target);
        const auto plan = ole_driver::BuildV4NativeExecutionPlan(identity, verified, present, selected,
            effectivePoints, behavior::Access::completionWindow, bounds);
        result.obligations.insert(result.obligations.end(), plan.preconditions.begin(), plan.preconditions.end());
        result.postDispatchObligations = plan.postconditions;
        result.unclassifiedObligations = plan.unclassified;
        bool pointsMatch = plan.points.size() == program.steps.size() && plan.sourcePoint &&
            plan.sourcePoint->x == start.x && plan.sourcePoint->y == start.y;
        bool releaseObserved = false;
        if (!pointsMatch)
            result.obligations.emplace_back("v4_native_source_point_candidate_unavailable");
        else for (std::size_t i = 0; i < program.steps.size(); ++i)
            if (program.steps[i].kind == "release")
            {
                releaseObserved = true;
                if (!plan.points[i] || plan.points[i]->x != target.x ||
                    plan.points[i]->y != target.y)
                { pointsMatch = false; result.obligations.emplace_back("v4_native_release_point_candidate_unavailable"); }
            }
        pointsMatch = pointsMatch && releaseObserved;
        result.v4NativeReorderInitialInput->sourceAndReleasePointsMatch = pointsMatch;
        if (!plan.unclassified.empty()) result.obligations.emplace_back("v4_native_unclassified_obligation_capability_unimplemented");
        if (!result.obligations.empty()) return result;
        auto host = observed.Boundaries();
        const auto originalBeforeBegin = host.beforeDeviceBegin;
        host.beforeDeviceBegin = [&] {
            const auto current = behavior::Access::InspectNativeCollectionReorderGeometry(*profile.app, initialFilePath, profile.desktop / L"Sentinel.txt");
            if (current.host != geometry.host || current.container != geometry.container || current.ownerThread != geometry.ownerThread ||
                current.movingKey != geometry.movingKey || current.targetKey != geometry.targetKey || current.itemKeys != geometry.itemKeys ||
                current.movingIndex != geometry.movingIndex || current.targetIndex != geometry.targetIndex ||
                current.layoutRevision != geometry.layoutRevision || !EqualRect(&current.movingBounds, &geometry.movingBounds) ||
                !EqualRect(&current.targetBounds, &geometry.targetBounds) || !EqualRect(&current.viewport, &geometry.viewport) ||
                current.movingScreen.x != start.x || current.movingScreen.y != start.y ||
                current.targetScreen.x != target.x || current.targetScreen.y != target.y || !samePoints(proposal.points, effectivePoints))
                throw ole_driver::ObservationUnavailable("v4_native_reorder_point_proposal_changed_before_device_begin");
            proposal.revalidated = true;
            originalBeforeBegin();
        };
        if (!snowdesktop::external_transfer_observation::Enable())
            throw ole_driver::ObservationUnavailable("v4_native_layout_diagnostic_unavailable");
        struct StopNativeDiagnostic final
        {
            bool active = true;
            void Stop() noexcept
            { if (active) { snowdesktop::external_transfer_observation::Stop(); active = false; } }
            ~StopNativeDiagnostic() { Stop(); }
        } diagnostic;
        const auto originalAwait = host.awaitCompletion;
        host.awaitCompletion = [&](const EventStep& step) {
            originalAwait(step);
            // Freeze the observation interval before the independent oracle
            // requests a disk reload, which can itself schedule layout work.
            diagnostic.Stop(); result.v4DiagnosticBudget = snowdesktop::external_transfer_observation::ObserveBudget();
        };
        const ole_driver::V4NativeReorderObserverBoundaries reorderObserver{
            [&](const std::shared_ptr<ole_driver::V4NativeReorderExecutionKey>& key) { business.BindExecution(key, epoch); },
            [&](const std::shared_ptr<ole_driver::V4NativeReorderExecutionKey>& key) { business.CaptureExecution(key, epoch); }};
        dispatchAttempted = true;
        const auto receipt = ole_driver::V4NativeSequence::RunBound(identity, verified, present, selected,
            effectivePoints, behavior::Access::completionWindow, behavior::Access::fixtureControlWindow, bounds,
            *behavior::Access::messageTrace, epoch, host, {}, {}, {}, reorderObserver);
        diagnostic.Stop(); result.v4DiagnosticBudget = snowdesktop::external_transfer_observation::ObserveBudget();
        const auto& execution = receipt.Execution();
        result.productionStarted = execution.productionStarted;
        result.productionEntryUnknown = execution.productionEntryUnknown;
        dispatchAttempted = false;
        result.businessFailureObserved = execution.businessFailureObserved;
        result.eventOracleFailureObserved = execution.eventOracleFailureObserved;
        if (execution.productionStarted && (execution.businessFailureObserved || execution.eventOracleFailureObserved))
            result.status = BridgeStatus::production_failed;
        if (execution.productionStarted)
        {
            try { if (const auto sealed = receipt.SealedReorderBusiness())
                { result.v4NativeReorderBusiness = *sealed; result.v4NativeReorderBusinessSealed = true; }
                else { result.v4NativeReorderBusiness = business.Evidence(); result.observationUnavailable = true; } }
            catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v4_native_business_evidence_unavailable"); }
        }
        result.observationUnavailable = result.observationUnavailable || execution.additionalObservationUnavailable ||
            execution.status == ole_driver::Status::observation_unavailable || execution.status == ole_driver::Status::host_observation_failed;
        result.failure = execution.failure;
        result.obligations.insert(result.obligations.end(), execution.obligations.begin(), execution.obligations.end());
        result.postDispatchObligations = execution.postconditionsPending;
        result.unclassifiedObligations = execution.unclassifiedObligations;
        result.obligations.insert(result.obligations.end(), receipt.Unresolved().begin(), receipt.Unresolved().end());
        if (!receipt.Unresolved().empty()) result.observationUnavailable = true;
        try { result.v4NativeDeviceRecords = receipt.DeviceRecords(); }
        catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v4_native_device_records_unavailable"); }
        try { result.v4NativeQueuedRecords = receipt.QueuedRecords(); }
        catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v4_native_queue_records_unavailable"); }
        try { result.v4NativeTraceRecords = receipt.TraceRecords(); }
        catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v4_native_trace_records_unavailable"); }
        try { result.v4NativeBeforeEndStates = receipt.BeforeEndStates(); }
        catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v4_native_end_state_records_unavailable"); }
        try { result.v4NativeBeforeEndDelivery = receipt.BeforeEndDelivery(); }
        catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v4_native_end_delivery_records_unavailable"); }
        if (execution.productionStarted)
        {
            try
            {
                if (!behavior::Access::messageTrace)
                    throw ole_driver::ObservationUnavailable("v4_native_graph_trace_unavailable");
                result.v4NativeGraph = ole_driver::InspectV4NativeGraph(receipt, identity, verified, present, selected,
                    epoch, *behavior::Access::messageTrace, behavior::Access::completionWindow,
                    behavior::Access::fixtureControlWindow);
                result.hypothesisNotMetObserved = result.v4NativeGraph->identityValid &&
                    result.v4NativeGraph->hypothesisObservationRecorded && result.v4NativeGraph->hypothesisNotMetObserved;
                for (const auto& reason : result.v4NativeGraph->unresolved)
                    result.obligations.push_back("v4_native_graph:" + reason);
                if (result.v4NativeGraph->observationUnavailable || !result.v4NativeGraph->unresolved.empty())
                    result.observationUnavailable = true;
            }
            catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v4_native_graph_observation_unavailable"); }
        }
        if (execution.productionStarted)
        {
            try
            {
                if (!behavior::Access::messageTrace)
                    throw ole_driver::ObservationUnavailable("v4_native_cleanup_trace_unavailable");
                result.v4NativeCleanup = ole_driver::InspectV4NativeCleanup(receipt, identity, verified, present, selected,
                    epoch, *behavior::Access::messageTrace, behavior::Access::completionWindow,
                    behavior::Access::fixtureControlWindow);
                for (const auto& reason : result.v4NativeCleanup->unresolved)
                    result.obligations.push_back("v4_native_cleanup:" + reason);
                if (result.v4NativeCleanup->observationUnavailable || !result.v4NativeCleanup->intervalObserved ||
                    !result.v4NativeCleanup->unresolved.empty()) result.observationUnavailable = true;
            }
            catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v4_native_cleanup_observation_unavailable"); }
        }
        if (execution.productionStarted)
        {
            try
            {
                if (!behavior::Access::messageTrace)
                    throw ole_driver::ObservationUnavailable("v4_native_layout_trace_unavailable");
                result.v4NativeLayoutSave = ole_driver::InspectV4NativeLayoutSave(receipt, identity, verified, present, selected,
                    epoch, *behavior::Access::messageTrace, behavior::Access::completionWindow,
                    behavior::Access::fixtureControlWindow);
                for (const auto& reason : result.v4NativeLayoutSave->unresolved)
                    result.obligations.push_back("v4_native_layout:" + reason);
                if (result.v4NativeLayoutSave->observationUnavailable || !result.v4NativeLayoutSave->unresolved.empty())
                    result.observationUnavailable = true;
            }
            catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v4_native_layout_observation_unavailable"); }
        }
        if (execution.productionStarted && behavior::Access::messageTrace)
        {
            try
            {
                result.v4NativeReorderProduction = ole_driver::InspectV4NativeReorderProduction(receipt, identity, verified, present, selected,
                    epoch, *behavior::Access::messageTrace, behavior::Access::completionWindow, behavior::Access::fixtureControlWindow);
                if (result.v4NativeReorderProduction->observationUnavailable || !result.v4NativeReorderProduction->unresolved.empty())
                    result.observationUnavailable = true;
            }
            catch (...) { result.observationUnavailable = true; }
        }
        if (execution.productionStarted && behavior::Access::messageTrace)
        {
            try
            {
                auto& local = result.v4NativeReorderCommitBinding.emplace();
                const auto sealed = receipt.SealedReorderBusiness();
                if (sealed && sealed->candidateId == audit.candidateId && sealed->initialGeometry &&
                    result.v4NativeReorderProduction && result.v4NativeReorderProduction->identityValid &&
                    result.v4NativeReorderProduction->releaseScopedCommitObserved &&
                    receipt.SameCandidate(identity, verified, present, selected) &&
                    receipt.SameHost(epoch, *behavior::Access::messageTrace, behavior::Access::completionWindow,
                        behavior::Access::fixtureControlWindow))
                {
                    local.sameReceiptObserved = true;
                    const auto& production = *result.v4NativeReorderProduction;
                    local.commitId = production.commitId;
                    using Phase = snowdesktop::external_transfer_observation::Phase;
                    using Record = ole_driver::V4NativeLayoutRecord;
                    const Record *enter = nullptr, *origin = nullptr, *collection = nullptr, *source = nullptr, *returned = nullptr;
                    std::vector<const Record*> beforeIndex, beforeKey, afterIndex, afterKey;
                    bool duplicate = false;
                    for (const auto& record : production.records)
                    {
                        if (record.event.extra != production.commitId) continue;
                        const auto one = [&](const Record*& slot) { if (slot) duplicate = true; else slot = &record; };
                        switch (record.event.phase)
                        {
                        case Phase::collection_reorder_enter: one(enter); break;
                        case Phase::collection_reorder_origin: one(origin); break;
                        case Phase::collection_reorder_collection: one(collection); break;
                        case Phase::collection_reorder_source_index: one(source); break;
                        case Phase::collection_reorder_return: one(returned); break;
                        case Phase::collection_reorder_before_index: beforeIndex.push_back(&record); break;
                        case Phase::collection_reorder_before_key: beforeKey.push_back(&record); break;
                        case Phase::collection_reorder_after_index: afterIndex.push_back(&record); break;
                        case Phase::collection_reorder_after_key: afterKey.push_back(&record); break;
                        default: break;
                        }
                    }
                    const auto& initial = *sealed->initialGeometry;
                    if (enter && origin && collection && source && returned && !duplicate && initial.itemKeys.size() == 2 &&
                        beforeIndex.size() == 2 && beforeKey.size() == 2 && afterIndex.size() == 2 && afterKey.size() == 2 &&
                        enter->index < origin->index && origin->index < collection->index && collection->index < source->index &&
                        source->index < returned->index)
                    {
                        const auto sameContext = [&](const Record* item) {
                            return item && item->event.thread == enter->event.thread && item->event.windowScope == enter->event.windowScope &&
                                item->index > enter->index && item->index < returned->index; };
                        const auto sameScope = [&](const Record* item) {
                            if (!item) return false;
                            const auto& a = item->event.nativeScope; const auto& b = enter->event.nativeScope;
                            return a.session == b.session && a.event == b.event && a.delivery == b.delivery &&
                                a.forward == b.forward && a.ownerThread == b.ownerThread &&
                                a.forwarded == b.forwarded && a.queuedEvent == b.queuedEvent; };
                        // Compare the production RTTI complete-object marker, not
                        // unrelated base-subobject addresses.
                        local.containerMatches = sameContext(collection) && sameScope(collection) &&
                            collection->event.object == initial.container && collection->event.value == 1;
                        local.inputIndicesMatch = sameContext(origin) && sameContext(collection) && sameContext(source) &&
                            sameScope(origin) && sameScope(collection) && sameScope(source) &&
                            sameScope(returned) && source->event.object == enter->event.object &&
                            returned->event.object == enter->event.object && origin->event.value == 1 &&
                            source->event.value == static_cast<std::int64_t>(initial.movingIndex) &&
                            enter->event.value == static_cast<std::int64_t>(initial.targetIndex) && returned->event.value == 2;
                        const auto keysMatch = [&](const std::vector<const Record*>& indices, const std::vector<const Record*>& keys,
                            const std::vector<std::wstring>& expected) -> std::optional<bool> {
                            if (expected.size() != 2) return std::nullopt;
                            bool matches = true;
                            for (std::size_t i = 0; i < 2; ++i)
                            {
                                if (!sameContext(indices[i]) || !sameScope(indices[i]) || !sameContext(keys[i]) || indices[i]->event.object != enter->event.object ||
                                    keys[i]->event.object != enter->event.object || indices[i]->index >= keys[i]->index ||
                                    keys[i]->index - indices[i]->index != 1 ||
                                    (i && keys[i - 1]->index >= indices[i]->index) || indices[i]->event.value != static_cast<std::int64_t>(i) ||
                                    !keys[i]->event.hasBlob || keys[i]->event.blobSize != keys[i]->blob.size() ||
                                    expected[i].size() > (std::numeric_limits<std::size_t>::max)() / sizeof(wchar_t)) return std::nullopt;
                                const auto n = expected[i].size() * sizeof(wchar_t);
                                matches = matches && keys[i]->blob.size() == n &&
                                    (!n || std::memcmp(keys[i]->blob.data(), expected[i].data(), n) == 0);
                            }
                            return matches;
                        };
                        local.beforeKeysMatch = keysMatch(beforeIndex, beforeKey, initial.itemKeys);
                        if (sealed->finalObserved) local.afterKeysMatch = keysMatch(afterIndex, afterKey, sealed->afterDrop.itemKeys);
                        local.recordsComplete = local.containerMatches.has_value() && local.inputIndicesMatch.has_value() &&
                            local.beforeKeysMatch.has_value() && local.afterKeysMatch.has_value() &&
                            sameContext(origin) && sameContext(collection) && sameContext(source) && sameScope(origin) &&
                            sameScope(collection) && sameScope(source) && sameScope(returned) &&
                            source->index < beforeIndex.front()->index && beforeKey.back()->index < afterIndex.front()->index &&
                            afterKey.back()->index < returned->index;
                    }
                    if (!local.recordsComplete) local.unresolved.emplace_back("v4_native_reorder_commit_values_incomplete");
                }
                else local.unresolved.emplace_back("v4_native_reorder_commit_receipt_unavailable");
                local.unresolved.emplace_back("v4_native_reorder_slot_item_identity_and_non_device_reentry_unverified");
                local.unresolved.emplace_back("v4_native_reorder_business_durability_and_late_work_unverified");
                result.observationUnavailable = true;
            }
            catch (...) { result.observationUnavailable = true; }
        }
        if (execution.productionStarted && behavior::Access::messageTrace)
        {
            try
            {
                auto& local = result.v4NativeReorderBusinessBinding.emplace();
                const auto sealed = receipt.SealedReorderBusiness();
                if (sealed && receipt.SameCandidate(identity, verified, present, selected) &&
                    receipt.SameHost(epoch, *behavior::Access::messageTrace, behavior::Access::completionWindow,
                        behavior::Access::fixtureControlWindow) && result.v4NativeGraph && result.v4NativeCleanup && result.v4NativeLayoutSave)
                {
                    local.sameReceiptObserved = true;
                    local.graphIdentityObserved = result.v4NativeGraph->identityValid;
                    local.cleanupIntervalObserved = result.v4NativeCleanup->intervalObserved;
                    if (sealed->candidateId == audit.candidateId && sealed->movingId) local.sourceSnapshotMatches = sealed->moving == initialFilePath &&
                        sealed->expectedMoving == initialFileBytes && behavior::OwnedProfileDesktopRoot::SameId(*sealed->movingId, initialFileId);
                    const auto& save = *result.v4NativeLayoutSave;
                    if (sealed->beforeReloadLayout && save.writerInputsObserved && save.saveRevision)
                    {
                        const auto& snapshot = *sealed->beforeReloadLayout;
                        local.snapshotFileId = snapshot.fileId; local.snapshotRevision = snapshot.revision; local.saveRevision = save.saveRevision;
                        const ole_driver::V4NativeLayoutRecord* path = nullptr;
                        const ole_driver::V4NativeLayoutRecord* bytes = nullptr;
                        bool duplicate = false;
                        using Phase = snowdesktop::external_transfer_observation::Phase;
                        for (const auto& record : save.records)
                        {
                            if (record.event.extra != *save.saveRevision) continue;
                            if (record.event.phase == Phase::layout_write_input_path)
                            { if (path) duplicate = true; else path = &record; }
                            if (record.event.phase == Phase::layout_write_input_bytes)
                            { if (bytes) duplicate = true; else bytes = &record; }
                        }
                        if (snapshot.captured && !snapshot.resourceUnavailable && snapshot.fileId && snapshot.revision &&
                            path && bytes && !duplicate && path->event.hasBlob && bytes->event.hasBlob &&
                            path->blob.size() == path->event.blobSize && bytes->blob.size() == bytes->event.blobSize)
                        {
                            const auto nativePath = snapshot.path.native();
                            local.layoutPathMatches = nativePath.size() <= (std::numeric_limits<std::size_t>::max)() / sizeof(wchar_t) &&
                                path->blob.size() == nativePath.size() * sizeof(wchar_t) &&
                                (path->blob.empty() || std::memcmp(path->blob.data(), nativePath.data(), path->blob.size()) == 0);
                            local.layoutBytesMatch = bytes->blob.size() == snapshot.rawBytes.size() &&
                                (bytes->blob.empty() || std::memcmp(bytes->blob.data(), snapshot.rawBytes.data(), bytes->blob.size()) == 0);
                            local.layoutRevisionMatches = *snapshot.revision == *save.saveRevision;
                        }
                        else local.unresolved.emplace_back("v4_native_reorder_layout_writer_or_snapshot_unavailable");
                    }
                    else local.unresolved.emplace_back("v4_native_reorder_pre_reload_layout_unavailable");
                }
                else local.unresolved.emplace_back("v4_native_reorder_sealed_receipt_evidence_unavailable");
                local.unresolved.emplace_back("v4_native_reorder_source_capture_causality_reentry_and_full_business_unverified");
                local.unresolved.emplace_back("v4_native_reorder_layout_non_atomic_durability_and_late_work_unverified");
                result.observationUnavailable = true;
            }
            catch (...) { result.observationUnavailable = true; }
        }
    }
    catch (const std::exception& error)
    { if (dispatchAttempted && !result.productionStarted) result.productionEntryUnknown = true;
      result.observationUnavailable = true; result.failure = error.what(); result.obligations.emplace_back("v4_native_preparation_or_observation_unavailable"); }
    catch (...)
    { if (dispatchAttempted && !result.productionStarted) result.productionEntryUnknown = true;
      result.observationUnavailable = true; result.obligations.emplace_back("v4_native_preparation_or_observation_unavailable"); }
    return result;
}

// Restricted v4 desktop source/target preparation. The selected graph and
// device plan are authenticated, but the initial geometry is only an input
// proposal; capture, delivery, completion and the business result remain
// postflight facts. No Device is constructed while capability gaps remain.
inline BridgeResult PrepareV4NativeDesktopMove(
    const event_graph::V4FamilyIdentityContext& identity, const V4FamilyValidatedContext& verified,
    const std::vector<bool>& present, const event_graph::V4NativeJoinedGraphVariant& selected,
    behavior::OwnedProfileDesktopRoot& ownedRoot, const V4SingleFileFixture& fixture,
    GridCell landing, POINT targetScreen, const std::vector<ole_driver::NativePointInput>& points)
{
    BridgeResult result;
    bool dispatchAttempted = false;
    try
    {
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        const auto replay = event_graph::V4NativeJoinedGraph::Replay(identity, verified, present, selected.id);
        const auto& axes = verified.Axes(); const auto& candidate = verified.Coordinate();
        const auto value = [&](std::string_view id) { return Value(axes, candidate, id); };
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty() || raw.id != fixture.candidateId ||
            !replay.identityValid || !replay.errors.empty() || replay.variant.id != selected.id ||
            replay.variant.classes != selected.classes || replay.variant.order != selected.order ||
            replay.variant.unresolved != selected.unresolved || !ownedRoot.StillOwned() || !ownedRoot.profile ||
            !ownedRoot.profile->app || !fixture.inputPrepared || !fixture.created || !fixture.fileIdentityObserved ||
            !behavior::OwnedProfileDesktopRoot::SameId(fixture.rootId, ownedRoot.desktopId) ||
            fixture.root != ownedRoot.profile->desktop || fixture.file.parent_path() != fixture.root ||
            value("T01") != "internal_native" || value("V4.source.kind") != "file_only" ||
            value("V4.source.file_count") != "one" || value("V4.file.member0_kind") != "file" ||
            value("V4.file.member0_identity") != "unique" || value("source.E01") != "free_desktop" ||
            value("source.E02") != "none" || value("source.E03") != "desktop_icon" ||
            value("target.E01") != "free_desktop" || value("target.E02") != "none" ||
            value("H01.region") != "empty_slot" || value("P01.primary") != "file" ||
            value("P01.second") != "na" || value("P02.source_actual_count") != "one" ||
            value("P02.resolved_count") != "one" || value("P02.many_count") != "na" ||
            value("P01.member0_span_columns") != "one" || value("P01.member0_span_rows") != "one" ||
            value("G01.button") != "left" || value("G02.press") != "none" ||
            value("G02.release") != "none" || value("G02.release_buttons") != "none" ||
            value("G04.termination") != "drop" || value("G03.decision") != "na" ||
            value("H09.grab_area") != "icon" || value("H09.offset") != "center" ||
            value("H09.selected_member") != "first" || value("H09.member_index") != "member_0" ||
            value("X02") != "none" || value("X03.task_count") != "one" ||
            value("X03.submit_order") != "na" || value("X03.order") != "single")
        { result.obligations.emplace_back("v4_native_desktop_candidate_graph_or_fixture_unavailable"); return result; }
        result.graphUnresolved = replay.variant.unresolved;
        auto& profile = *ownedRoot.profile;
        if (behavior::Access::productionDispatch != 1)
            behavior::Access::EnableProductionDispatch(*profile.app, false);
        if (!behavior::Access::fixtureControlWindow) behavior::Access::EnableFixtureControlWindow(*profile.app);
        if (!behavior::Access::nativePointerDispatch) behavior::Access::EnableNativePointerDispatch(*profile.app);
        behavior::V4NativeDesktopMoveObservation observed(ownedRoot, fixture, verified, identity, landing);
        result.v4NativeDesktopBusiness = observed.Evidence();
        const auto source = behavior::Access::InspectNativeDesktopSource(*profile.app, fixture.file);
        const auto destination = behavior::Access::InspectNativeDesktopLanding(*profile.app, landing);
        const bool matches = source.layoutRevision == destination.layoutRevision &&
            destination.screenCenter.x == targetScreen.x && destination.screenCenter.y == targetScreen.y &&
            EndpointSize(value("source.E23")) == destination.columns && EndpointSize(value("source.E24")) == destination.rows &&
            EndpointSize(value("target.E23")) == destination.columns && EndpointSize(value("target.E24")) == destination.rows;
        result.v4NativeDesktopGeometry = BridgeResult::V4NativeDesktopGeometryInput{
            raw.id, reinterpret_cast<std::uintptr_t>(profile.app), GetCurrentThreadId(), source, destination, matches};
        if (!matches) result.obligations.emplace_back("v4_native_desktop_geometry_candidate_unavailable");
        result.v4NativeDesktopPointProposal = behavior::PrepareV4NativeDesktopPoints(observed, identity, verified, present, selected);
        const bool useGeneratedPoints = points.empty() && result.v4NativeDesktopPointProposal->prepared &&
            !result.v4NativeDesktopPointProposal->resourceUnavailable && result.v4NativeDesktopPointProposal->candidateId == raw.id;
        const auto effectivePoints = useGeneratedPoints
            ? result.v4NativeDesktopPointProposal->points : points;
        result.v4NativeDesktopUsedGeneratedPoints = useGeneratedPoints;
        const auto samePoints = [](const std::vector<ole_driver::NativePointInput>& expected,
            const std::vector<ole_driver::NativePointInput>& actual) {
            if (expected.size() != actual.size()) return false;
            for (const auto& item : expected)
            {
                const auto match = [&](const auto& value) { return value.programIndex == item.programIndex; };
                const auto found = std::find_if(actual.begin(), actual.end(), match);
                if (found == actual.end() || std::count_if(expected.begin(), expected.end(), match) != 1 ||
                    std::count_if(actual.begin(), actual.end(), match) != 1 ||
                    found->screen.x != item.screen.x || found->screen.y != item.screen.y) return false;
            }
            return true;
        };
        if (!result.v4NativeDesktopPointProposal->prepared || result.v4NativeDesktopPointProposal->resourceUnavailable ||
            result.v4NativeDesktopPointProposal->candidateId != raw.id ||
            !samePoints(result.v4NativeDesktopPointProposal->points, effectivePoints))
            result.obligations.emplace_back("v4_native_desktop_point_proposal_unavailable");
        const auto plan = ole_driver::BuildV4NativeExecutionPlan(identity, verified, present, selected,
            effectivePoints, behavior::Access::completionWindow, destination.screenPage);
        result.postDispatchObligations = plan.postconditions; result.unclassifiedObligations = plan.unclassified;
        // The selected graph is a hypothesis, not a command to suppress or
        // manufacture device messages. Preserve its unsupported intervals as
        // post-dispatch obligations; the restricted driver must independently
        // validate the concrete Program and same-host input before dispatch.
        for (const auto& pending : plan.preconditions)
            result.postDispatchObligations.push_back("v4_native_desktop_graph_plan:" + pending);
        if (!plan.graphIdentityValid)
            result.obligations.emplace_back("v4_native_desktop_graph_identity_unavailable");
        const auto program = verified.TransferProgram();
        if (!plan.sourcePoint || plan.sourcePoint->x != source.screenCenter.x || plan.sourcePoint->y != source.screenCenter.y ||
            plan.points.size() != program.steps.size())
            result.obligations.emplace_back("v4_native_desktop_press_point_unavailable");
        else for (std::size_t i = 0; i < program.steps.size(); ++i)
            if (program.steps[i].kind == "release" && (!plan.points[i] ||
                plan.points[i]->x != destination.screenCenter.x || plan.points[i]->y != destination.screenCenter.y))
                result.obligations.emplace_back("v4_native_desktop_release_point_unavailable");
        for (const auto& reason : fixture.unresolved) result.postDispatchObligations.push_back("v4_native_desktop_fixture:" + reason);
        result.postDispatchObligations.emplace_back("v4_native_desktop_all_axes_source_lease_and_geometry_unimplemented");
        result.postDispatchObligations.emplace_back("v4_native_desktop_completion_layout_and_business_oracle_capability_unimplemented");
        result.postDispatchObligations.emplace_back("v4_native_desktop_complete_graph_and_same_run_event_attribution_unimplemented");
        if (!result.obligations.empty()) return result;

        auto epoch = behavior::Access::CreateNativeHostEpoch(*profile.app);
        behavior::V4NativeDesktopDeviceObservation device(observed, identity, verified, present, selected,
            epoch, source.screenCenter, destination.screenCenter);
        auto host = device.Boundaries();
        const auto originalBeforeBegin = host.beforeDeviceBegin;
        host.beforeDeviceBegin = [&] {
            const auto actualSource = behavior::Access::InspectNativeDesktopSource(*profile.app, fixture.file);
            const auto actualTarget = behavior::Access::InspectNativeDesktopLanding(*profile.app, landing);
            if (actualSource.layoutRevision != source.layoutRevision || actualTarget.layoutRevision != destination.layoutRevision ||
                !EqualRect(&actualSource.clientBounds, &source.clientBounds) ||
                !behavior::Same(actualSource.cell, source.cell) || actualSource.screenCenter.x != source.screenCenter.x ||
                actualSource.screenCenter.y != source.screenCenter.y || !EqualRect(&actualTarget.clientCell, &destination.clientCell) ||
                !EqualRect(&actualTarget.screenPage, &destination.screenPage) || actualTarget.columns != destination.columns ||
                actualTarget.rows != destination.rows || actualTarget.screenCenter.x != destination.screenCenter.x ||
                actualTarget.screenCenter.y != destination.screenCenter.y)
                throw ole_driver::ObservationUnavailable("v4 native desktop geometry changed before device begin");
            const auto currentPoints = behavior::PrepareV4NativeDesktopPoints(observed, identity, verified, present, selected);
            if (!currentPoints.prepared || currentPoints.resourceUnavailable || currentPoints.candidateId != raw.id ||
                !samePoints(currentPoints.points, effectivePoints))
                throw ole_driver::ObservationUnavailable("v4 native desktop point proposal changed before device begin");
            result.v4NativeDesktopProposalRevalidated = true;
            originalBeforeBegin();
        };
        if (!snowdesktop::external_transfer_observation::Enable())
            throw ole_driver::ObservationUnavailable("v4 native desktop layout diagnostic unavailable");
        struct StopDiagnostic final {
            bool active = true;
            void Stop() noexcept { if (active) { snowdesktop::external_transfer_observation::Stop(); active = false; } }
            ~StopDiagnostic() { Stop(); }
        } diagnostic;
        const auto originalAwait = host.awaitCompletion;
        host.awaitCompletion = [&](const EventStep& step) { originalAwait(step); diagnostic.Stop(); result.v4DiagnosticBudget = snowdesktop::external_transfer_observation::ObserveBudget(); };
        try
        {
            const ole_driver::V4NativeDesktopObserverBoundaries desktopObserver{
                [&](const std::shared_ptr<ole_driver::V4NativeDesktopExecutionKey>& key) { observed.BindExecution(key, epoch); },
                [&](const std::shared_ptr<ole_driver::V4NativeDesktopExecutionKey>& key) { observed.CaptureExecution(key, epoch); }};
            dispatchAttempted = true;
            const auto receipt = ole_driver::V4NativeSequence::RunBound(identity, verified, present, selected, effectivePoints,
                behavior::Access::completionWindow, behavior::Access::fixtureControlWindow, destination.screenPage,
                *behavior::Access::messageTrace, epoch, host, desktopObserver);
            diagnostic.Stop(); result.v4DiagnosticBudget = snowdesktop::external_transfer_observation::ObserveBudget();
            const auto& execution = receipt.Execution();
            result.unattributedBusinessFailureObserved = execution.unattributedBusinessFailureObserved;
            result.unattributedEventOracleFailureObserved = execution.unattributedEventOracleFailureObserved;
            result.productionStarted = execution.productionStarted; result.productionEntryUnknown = execution.productionEntryUnknown;
            dispatchAttempted = false;
            result.businessFailureObserved = execution.businessFailureObserved;
            result.eventOracleFailureObserved = execution.eventOracleFailureObserved;
            if (execution.productionStarted && (execution.businessFailureObserved || execution.eventOracleFailureObserved))
                result.status = BridgeStatus::production_failed;
            result.observationUnavailable = execution.additionalObservationUnavailable ||
                execution.status == ole_driver::Status::observation_unavailable ||
                execution.status == ole_driver::Status::host_observation_failed;
            result.failure = execution.failure;
            result.obligations.insert(result.obligations.end(), execution.obligations.begin(), execution.obligations.end());
            result.postDispatchObligations.insert(result.postDispatchObligations.end(),
                execution.postconditionsPending.begin(), execution.postconditionsPending.end());
            result.unclassifiedObligations = execution.unclassifiedObligations;
            result.obligations.insert(result.obligations.end(), receipt.Unresolved().begin(), receipt.Unresolved().end());
            if (!receipt.Unresolved().empty()) result.observationUnavailable = true;
            if (const auto sealed = receipt.SealedDesktopBusiness())
            { result.v4NativeDesktopBusiness = *sealed; result.v4NativeDesktopBusinessSealed = true; }
            else { result.v4NativeDesktopBusiness = observed.Evidence(); if (execution.productionStarted) result.observationUnavailable = true; }
            try { result.v4NativeDeviceRecords = receipt.DeviceRecords(); } catch (...) { result.observationUnavailable = true; }
            try { result.v4NativeQueuedRecords = receipt.QueuedRecords(); } catch (...) { result.observationUnavailable = true; }
            try { result.v4NativeTraceRecords = receipt.TraceRecords(); } catch (...) { result.observationUnavailable = true; }
            try { result.v4NativeBeforeEndStates = receipt.BeforeEndStates(); } catch (...) { result.observationUnavailable = true; }
            try { result.v4NativeBeforeEndDelivery = receipt.BeforeEndDelivery(); } catch (...) { result.observationUnavailable = true; }
            if (execution.productionStarted && behavior::Access::messageTrace)
            {
                try {
                    result.v4NativeGraph = ole_driver::InspectV4NativeGraph(receipt, identity, verified, present, selected,
                        epoch, *behavior::Access::messageTrace, behavior::Access::completionWindow,
                        behavior::Access::fixtureControlWindow);
                    result.hypothesisNotMetObserved = result.v4NativeGraph->identityValid &&
                        result.v4NativeGraph->hypothesisObservationRecorded && result.v4NativeGraph->hypothesisNotMetObserved;
                    if (result.v4NativeGraph->observationUnavailable || !result.v4NativeGraph->unresolved.empty())
                        result.observationUnavailable = true;
                } catch (...) { result.observationUnavailable = true; }
                try {
                    result.v4NativeCleanup = ole_driver::InspectV4NativeCleanup(receipt, identity, verified, present, selected,
                        epoch, *behavior::Access::messageTrace, behavior::Access::completionWindow,
                        behavior::Access::fixtureControlWindow);
                    if (result.v4NativeCleanup->observationUnavailable || !result.v4NativeCleanup->intervalObserved ||
                        !result.v4NativeCleanup->unresolved.empty()) result.observationUnavailable = true;
                } catch (...) { result.observationUnavailable = true; }
                try {
                    result.v4NativeLayoutSave = ole_driver::InspectV4NativeLayoutSave(receipt, identity, verified, present, selected,
                        epoch, *behavior::Access::messageTrace, behavior::Access::completionWindow,
                        behavior::Access::fixtureControlWindow);
                    if (result.v4NativeLayoutSave->observationUnavailable || !result.v4NativeLayoutSave->unresolved.empty())
                        result.observationUnavailable = true;
                } catch (...) { result.observationUnavailable = true; }
                try {
                    auto& local = result.v4NativeDesktopLayoutBinding.emplace();
                    const auto sealed = receipt.SealedDesktopBusiness();
                    if (sealed && sealed->candidateId == raw.id && sealed->beforeReloadLayout &&
                        receipt.SameCandidate(identity, verified, present, selected) &&
                        receipt.SameHost(epoch, *behavior::Access::messageTrace, behavior::Access::completionWindow,
                            behavior::Access::fixtureControlWindow) && result.v4NativeLayoutSave &&
                        result.v4NativeLayoutSave->writerInputsObserved && result.v4NativeLayoutSave->saveRevision)
                    {
                        const auto& snapshot = *sealed->beforeReloadLayout;
                        const auto& save = *result.v4NativeLayoutSave;
                        local.sameReceiptObserved = true; local.snapshotFileId = snapshot.fileId;
                        local.snapshotRevision = snapshot.revision; local.saveRevision = save.saveRevision;
                        const ole_driver::V4NativeLayoutRecord* path = nullptr;
                        const ole_driver::V4NativeLayoutRecord* bytes = nullptr;
                        bool duplicate = false;
                        using Phase = snowdesktop::external_transfer_observation::Phase;
                        for (const auto& record : save.records)
                        {
                            if (record.event.extra != *save.saveRevision) continue;
                            if (record.event.phase == Phase::layout_write_input_path)
                            { if (path) duplicate = true; else path = &record; }
                            if (record.event.phase == Phase::layout_write_input_bytes)
                            { if (bytes) duplicate = true; else bytes = &record; }
                        }
                        if (snapshot.captured && !snapshot.resourceUnavailable && snapshot.fileId && snapshot.revision &&
                            path && bytes && !duplicate && path->event.hasBlob && bytes->event.hasBlob &&
                            path->blob.size() == path->event.blobSize && bytes->blob.size() == bytes->event.blobSize)
                        {
                            const auto nativePath = snapshot.path.native();
                            local.pathMatches = nativePath.size() <= (std::numeric_limits<std::size_t>::max)() / sizeof(wchar_t) &&
                                path->blob.size() == nativePath.size() * sizeof(wchar_t) &&
                                (path->blob.empty() || std::memcmp(path->blob.data(), nativePath.data(), path->blob.size()) == 0);
                            local.bytesMatch = bytes->blob.size() == snapshot.rawBytes.size() &&
                                (bytes->blob.empty() || std::memcmp(bytes->blob.data(), snapshot.rawBytes.data(), bytes->blob.size()) == 0);
                            local.revisionMatches = *snapshot.revision == *save.saveRevision;
                        }
                        else local.unresolved.emplace_back("v4_native_desktop_layout_writer_or_snapshot_unavailable");
                    }
                    else local.unresolved.emplace_back("v4_native_desktop_layout_same_receipt_unavailable");
                    local.unresolved.emplace_back("v4_native_desktop_layout_non_atomic_durability_and_full_business_unverified");
                    result.observationUnavailable = true;
                } catch (...) { result.observationUnavailable = true; }
                // Judge the concrete desktop move only after the same receipt
                // proves the real pointer release, cleanup interval and the
                // completed layout writer whose bytes were read before reload.
                // Other candidate axes and graph hypotheses stay pending.
                if (result.v4NativeDesktopBusinessSealed && result.v4NativeDesktopBusiness &&
                    result.v4NativeGraph && result.v4NativeGraph->identityValid &&
                    result.v4NativeCleanup && result.v4NativeCleanup->intervalObserved &&
                    result.v4NativeLayoutSave && result.v4NativeDesktopLayoutBinding)
                {
                    const auto& business = *result.v4NativeDesktopBusiness;
                    const auto& save = *result.v4NativeLayoutSave;
                    const auto& layout = *result.v4NativeDesktopLayoutBinding;
                    bool delivered = true;
                    for (std::size_t i = 0; i < program.steps.size(); ++i)
                    {
                        const auto& kind = program.steps[i].kind;
                        if (kind != "source_start" && kind != "move_past_drag_threshold" && kind != "release") continue;
                        for (const auto suffix : {"_wndproc_enter", "_wndproc_return"})
                        {
                            const auto id = "program_" + std::to_string(i) + suffix;
                            const auto& nodes = result.v4NativeGraph->nodes;
                            const auto found = std::find_if(nodes.begin(), nodes.end(), [&](const auto& node) { return node.id == id; });
                            if (found == nodes.end() || !found->observedLocal ||
                                std::count_if(nodes.begin(), nodes.end(), [&](const auto& node) { return node.id == id; }) != 1)
                                delivered = false;
                        }
                    }
                    const bool completed = receipt.SameCandidate(identity, verified, present, selected) &&
                        receipt.SameHost(epoch, *behavior::Access::messageTrace, behavior::Access::completionWindow,
                            behavior::Access::fixtureControlWindow) && business.candidateId == raw.id && delivered &&
                        save.requestMarkerMatched && save.timerWindowReturned && save.writerAndFinishMarked &&
                        (save.finishCompletionWindowReturned || save.finishSynchronousTimerReturned) &&
                        save.queueAccepted.value_or(false) && save.writeSucceeded.value_or(false) &&
                        save.finishReturnedSuccess.value_or(false) && save.finishRevisionMatched.value_or(false) &&
                        layout.sameReceiptObserved && layout.pathMatches.value_or(false) && layout.bytesMatch.value_or(false) &&
                        layout.revisionMatches.value_or(false) && business.initialObserved && business.finalObserved &&
                        business.diskParsed && business.targetMatched && business.membersPreserved && business.sourcePreserved &&
                        business.sentinelPreserved && business.sourceDiskCellMatches && business.sentinelDiskCellMatches;
                    if (completed)
                    {
                        const bool matches = *business.targetMatched && *business.membersPreserved && *business.sourcePreserved &&
                            *business.sentinelPreserved && *business.sourceDiskCellMatches && *business.sentinelDiskCellMatches;
                        result.unattributedBusinessFailureObserved =
                            result.unattributedBusinessFailureObserved.value_or(false) || !matches;
                        if (!matches && result.failure.empty())
                            result.failure = "native desktop local model, file or saved layout result differs";
                    }
                }
            }
        }
        catch (...) { try { if (!result.v4NativeDesktopBusinessSealed) result.v4NativeDesktopBusiness = observed.Evidence(); } catch (...) {} throw; }
    }
    catch (const std::exception& error)
    { if (dispatchAttempted && !result.productionStarted) result.productionEntryUnknown = true;
      result.observationUnavailable = true; if (result.failure.empty()) result.failure = error.what(); }
    catch (...) { if (dispatchAttempted && !result.productionStarted) result.productionEntryUnknown = true;
        result.observationUnavailable = true; }
    return result;
}

// This selects the narrow native fixture only; every remaining axis and the
// selected graph still require independent construction and observation.
inline bool NativeDesktopSingleMovePlan(const Context& context, const Candidate& input)
{
    if (!context.obligations.empty() || input.values.size() != context.axes.size()) return false;
    Candidate candidate = input; candidate.lookup = context.lookup;
    const auto v = [&](std::string_view id) { return Value(context.axes, candidate, id); };
    return v("T01") == "internal_native" && v("source.E01") == "free_desktop" && v("source.E02") == "none" &&
        v("source.E03") == "desktop_icon" && v("target.E01") == "free_desktop" && v("target.E02") == "none" &&
        v("source.E23") == "four" && v("source.E24") == "three" &&
        v("target.E23") == "four" && v("target.E24") == "three" &&
        v("H01.region") == "empty_slot" && v("P01.primary") == "file" && v("P01.second") == "na" &&
        v("P02.source_actual_count") == "one" && v("P02.resolved_count") == "one" && v("P02.many_count") == "na" &&
        v("P01.member0_span_columns") == "one" && v("P01.member0_span_rows") == "one" &&
        v("R02.member0_directory_ref") == "target_directory" && v("G01.button") == "left" &&
        v("G02.press") == "none" && v("G02.release") == "none" && v("G02.release_buttons") == "none" &&
        v("G04.termination") == "drop" && v("G03.decision") == "na" &&
        v("H09.grab_area") == "icon" && v("H09.offset") == "center" &&
        v("H09.selected_member") == "first" && v("H09.member_index") == "member_0" &&
        v("X02") == "none" && v("X03.task_count") == "one" && v("X03.submit_order") == "na" && v("X03.order") == "single";
}

inline BridgeResult ExecuteNativeDesktopSingleMove(const Context& context, const Candidate& candidate,
    const EventProgram& program, const event_graph::Variant& selectedGraph)
{
    BridgeResult result;
    const auto preflight = Inspect(context, candidate, program);
    result.obligations = preflight.obligations;
    if (!NativeDesktopSingleMovePlan(context, candidate)) result.obligations.emplace_back("native_desktop_single_move_plan_unimplemented");
    if (!preflight.CanExecute()) result.obligations.emplace_back("native_complete_axis_preflight_unimplemented");
    const event_graph::Context graphContext(context.axes);
    const auto mapping = event_graph::MapSimpleNative(graphContext, candidate, program);
    if (!mapping.graphAvailable) result.obligations.emplace_back("native_event_graph_mapping_unimplemented");
    if (!mapping.structuralExclusion.empty()) result.obligations.push_back("native_structural_exclusion:" + mapping.structuralExclusion);
    for (const auto& reason : mapping.reasons) result.obligations.push_back("native_mapping:" + reason);
    if (mapping.graphAvailable)
    {
        const auto presence = event_graph::PreparePresenceGraph(graphContext, candidate, mapping.spec);
        for (const auto& error : presence.errors) result.obligations.push_back("native_presence_schema:" + error);
        if (!presence.structuralExclusion.empty())
            result.obligations.push_back("native_presence_structural_exclusion:" + presence.structuralExclusion);
        for (const auto& reason : presence.unresolved) result.obligations.push_back("native_presence:" + reason);
        const auto replay = event_graph::ReplayVariant(graphContext, candidate, mapping.spec, selectedGraph.id);
        if (!replay.identityValid) result.obligations.emplace_back("native_selected_graph_identity_invalid");
        for (const auto& error : replay.errors) result.obligations.push_back("native_selected_graph:" + error);
        for (const auto& reason : replay.variant.unresolved) result.obligations.push_back("native_graph:" + reason);
    }
    const auto moveClasses = ole_driver::InspectNativeMoveClasses(graphContext, candidate, program, selectedGraph);
    if (!moveClasses.identityValid) result.obligations.emplace_back("native_move_classes_identity_invalid");
    for (const auto& error : moveClasses.errors) result.obligations.push_back("native_move_classes:" + error);
    for (const auto& reason : moveClasses.unresolved) result.obligations.push_back("native_move_classes_unresolved:" + reason);
    // No production pointer call may occur until every axis, selected graph,
    // event ordering and host observation is closed.
    result.obligations.emplace_back("native_generated_full_axes_graph_and_dispatch_unimplemented");
    if (!result.obligations.empty()) return result;

    bool productionStarted = false;
    bool productionEntryUnknown = false;
    bool dispatchAttempted = false;
    std::shared_ptr<behavior::GeneratedPayloadProfile> generated;
    std::unique_ptr<behavior::GeneratedNativeMoveObservation> observed;
    const auto appendFailure = [&](std::string_view detail) {
        if (!result.failure.empty()) result.failure += "; ";
        result.failure += detail;
    };
    const auto releaseGenerated = [&] {
        // Profile::Cleanup retains owned resources and reports its exception
        // through Access if workers cannot be stopped safely.
        observed.reset();
        generated.reset();
        try { behavior::Access::ThrowAsyncException(); }
        catch (const std::exception& error)
        {
            appendFailure(std::string("native_profile_shutdown_failed: ") + error.what());
            result.observationUnavailable = true;
            result.obligations.emplace_back(productionEntryUnknown ? "native_production_entry_indeterminate" : "native_fixture_shutdown_failed");
        }
        catch (...)
        {
            appendFailure("native_profile_shutdown_failed: unknown_exception");
            result.observationUnavailable = true;
            result.obligations.emplace_back(productionEntryUnknown ? "native_production_entry_indeterminate" : "native_fixture_shutdown_failed");
        }
    };
    try
    {
        generated = std::make_shared<behavior::GeneratedPayloadProfile>(context.axes, candidate);
        for (const auto& reason : generated->payload.unresolved)
            result.obligations.push_back("native_generated_payload_unresolved:" + reason);
        if (!result.obligations.empty()) { releaseGenerated(); return result; }
        auto& profile = *generated->profile;
        behavior::Access::EnableProductionDispatch(*profile.app, false);
        behavior::Access::EnableNativePointerDispatch(*profile.app);
        behavior::Wait([&] { return behavior::Access::FileDropIdle(*profile.app) &&
            behavior::Access::LayoutSaveIdle(*profile.app) && behavior::Access::NativeDragIdle(*profile.app); });
        std::error_code ec;
        if (!std::filesystem::equivalent(profile.desktop, generated->payload.desktopDirectory, ec) || ec)
        { result.obligations.emplace_back("native_generated_desktop_binding_unimplemented"); releaseGenerated(); return result; }
        ec.clear();
        if (!std::filesystem::equivalent(generated->payload.desktopDirectory, generated->payload.targetDirectory, ec) || ec)
        { result.obligations.emplace_back("native_generated_target_not_desktop_unimplemented"); releaseGenerated(); return result; }
        ec.clear();
        if (generated->payload.members.size() != 1 ||
            !std::filesystem::equivalent(generated->payload.members.front().path.parent_path(), profile.desktop, ec) || ec)
        { result.obligations.emplace_back("native_generated_source_not_same_desktop_unimplemented"); releaseGenerated(); return result; }
        observed = std::make_unique<behavior::GeneratedNativeMoveObservation>(*generated, program, moveClasses.byProgramIndex);
        // The bounds describe only the isolated model page, not a physical
        // monitor or an observed user desktop.
        const auto topLeft = behavior::Access::ExternalPoint({0, 0});
        const auto bottomRight = behavior::Access::ExternalPoint({400, 300});
        const RECT bounds{topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
        const auto pointPlan = ole_driver::InspectNativeSingleMoveGeometry(graphContext, candidate, program, selectedGraph,
            behavior::Access::completionWindow, bounds, observed->startScreen, observed->targetScreen);
        for (const auto& reason : pointPlan.graphUnresolved) result.obligations.push_back("native_geometry_graph_unresolved:" + reason);
        for (const auto& reason : pointPlan.unresolved) result.obligations.push_back("native_geometry_unresolved:" + reason);
        if (!pointPlan.bound) result.obligations.emplace_back("native_move_geometry_unimplemented");
        if (!result.obligations.empty()) { releaseGenerated(); return result; }

        bool initialHover = false, capture = false, completed = false, ended = false, business = false;
        std::size_t traceCheckpoint = 0;
        const ole_driver::NativeHostBoundaries host{
            [&] {
                observed->VerifyImmediatelyBeforeDispatch();
                traceCheckpoint = behavior::Access::messageTrace->Observations().size();
            },
            [&](ole_driver::NativeDevicePhase phase, std::optional<std::size_t> index,
                const snowdesktop::private_pointer::State& state,
                const snowdesktop::private_pointer::DeliveryObservationLog& deliveryLog,
                std::optional<snowdesktop::private_pointer::QueuedEventIdentity> queued,
                std::optional<ole_driver::NativeInputExpectation> expected) {
                observed->ObserveDeviceState(phase, index, state, deliveryLog, queued, expected);
                if (phase == ole_driver::NativeDevicePhase::initial_hover) initialHover = true;
                if (phase == ole_driver::NativeDevicePhase::program_input && index &&
                    !result.nativeEndpoint && state.point.x == observed->targetScreen.x &&
                    state.point.y == observed->targetScreen.y && !state.trace.empty() &&
                    state.trace.back().message == WM_MOUSEMOVE)
                    result.nativeEndpoint = InspectNativeDesktopEndpoint(context, candidate, *profile.app, state,
                        observed->targetScreen, traceCheckpoint);
                traceCheckpoint = behavior::Access::messageTrace->Observations().size();
            },
            [&](const EventStep&) { observed->Capture(); capture = true; },
            [&](const EventStep&) { observed->Await(); completed = true; },
            [&](const EventStep&, const snowdesktop::private_pointer::State& state,
                const snowdesktop::private_pointer::DeliveryObservationLog& log) {
                if (log.failed || log.ownerThread != GetCurrentThreadId() || !log.session)
                    throw ole_driver::ObservationUnavailable("native_before_end_delivery_log_unavailable");
                observed->BeforeEnd(state);
            },
            [&](const EventStep&) { observed->Ended(); ended = true; },
            [&](const EventStep&) { observed->AssertResults(); business = true; },
            [] { behavior::Access::ThrowAsyncException(); },
            [&] { return observed->ProductionInputEntered(); },
            [&](ole_driver::NativeDevicePhase phase, std::optional<std::size_t> index,
                const snowdesktop::private_pointer::QueuedEventIdentity& token) {
                if (token.ownerThread != GetCurrentThreadId() || !token.session || !token.event ||
                    (phase == ole_driver::NativeDevicePhase::initial_hover && index) ||
                    (phase == ole_driver::NativeDevicePhase::program_input && (!index || *index >= program.steps.size())) ||
                    (phase != ole_driver::NativeDevicePhase::initial_hover && phase != ole_driver::NativeDevicePhase::program_input))
                    throw ole_driver::ObservationUnavailable("native_queued_input_token_unavailable");
                // This records the queue identity only. The later device log
                // and production WndProc trace establish any delivery.
                result.nativeQueuedInputs.push_back({phase, index, token});
            }};
        dispatchAttempted = true;
        const auto execution = ole_driver::NativeSequence::Run(context.axes, candidate, program, selectedGraph, pointPlan.points,
            behavior::Access::completionWindow, bounds, host);
        productionStarted = execution.productionStarted;
        productionEntryUnknown = execution.productionEntryUnknown;
        dispatchAttempted = false;
        result.productionStarted = productionStarted;
        result.productionEntryUnknown = productionEntryUnknown;
        result.businessFailureObserved = execution.businessFailureObserved;
        result.observationUnavailable = result.observationUnavailable || execution.additionalObservationUnavailable;
        result.obligations.insert(result.obligations.end(), execution.obligations.begin(), execution.obligations.end());
        if (result.nativeEndpoint)
            for (const auto& reason : result.nativeEndpoint->unresolved)
                result.obligations.push_back("native_endpoint:" + reason);
        else result.obligations.emplace_back("native_endpoint_delivery_snapshot_unobserved");
        if (execution.status == ole_driver::Status::observation_unavailable)
        {
            result.observationUnavailable = true;
            appendFailure(execution.failure.empty() ? "native_delivery_observation_unavailable" : execution.failure);
            result.obligations.emplace_back("native_delivery_observation_unavailable");
            if (productionEntryUnknown) result.obligations.emplace_back("native_production_entry_indeterminate");
        }
        else if (execution.status == ole_driver::Status::fixture_failed)
        {
            appendFailure(execution.failure.empty() ? "native_pre_begin_fixture_failed" : execution.failure);
            result.observationUnavailable = true;
            result.obligations.emplace_back(productionEntryUnknown ? "native_production_entry_indeterminate" : "native_fixture_unavailable");
        }
        else if (execution.status == ole_driver::Status::event_failed)
        {
            appendFailure(execution.failure.empty() ? "native_event_failed" : execution.failure);
            if (productionStarted) result.status = BridgeStatus::production_failed;
            else result.obligations.emplace_back(productionEntryUnknown ? "native_production_entry_indeterminate" : "native_fixture_or_dispatch_unimplemented");
        }
        else if (execution.status == ole_driver::Status::business_failed || execution.businessFailureObserved)
        {
            appendFailure(execution.failure.empty() ? "native_business_assertion_failed" : execution.failure);
            if (productionStarted) result.status = BridgeStatus::production_failed;
            else result.obligations.emplace_back("native_business_failure_without_production_identity");
        }
        else if (execution.status == ole_driver::Status::host_observation_failed)
        {
            appendFailure(execution.failure.empty() ? "native_host_observation_unavailable" : execution.failure);
            result.observationUnavailable = true;
            result.obligations.emplace_back("native_host_observation_unavailable");
        }
        else if (execution.status != ole_driver::Status::attempted || !initialHover || !capture || !completed || !ended || !business)
        {
            if (productionStarted) { result.observationUnavailable = true; appendFailure("native_production_observations_incomplete"); }
            else result.obligations.emplace_back(productionEntryUnknown ? "native_production_entry_indeterminate" : "native_graph_execution_unimplemented");
        }
        // Destroy the one profile before inspecting shutdown errors.
        releaseGenerated();
        if (result.status != BridgeStatus::production_failed && productionStarted && !productionEntryUnknown &&
            result.failure.empty() && execution.status == ole_driver::Status::attempted && initialHover && capture && completed && ended && business)
        {
            result.status = BridgeStatus::production_observed;
            result.obligations.emplace_back("native_global_axis_and_graph_postflight_unimplemented");
        }
    }
    catch (const std::exception& error)
    {
        result.productionStarted = productionStarted;
        result.productionEntryUnknown = productionEntryUnknown || (dispatchAttempted && !productionStarted);
        appendFailure(error.what());
        result.observationUnavailable = true;
        result.obligations.emplace_back(result.productionEntryUnknown ? "native_production_entry_indeterminate" : "native_fixture_or_observation_unavailable");
        releaseGenerated();
    }
    catch (...)
    {
        result.productionStarted = productionStarted;
        result.productionEntryUnknown = productionEntryUnknown || (dispatchAttempted && !productionStarted);
        appendFailure("unknown_native_fixture_or_execution_exception");
        result.observationUnavailable = true;
        result.obligations.emplace_back(result.productionEntryUnknown ? "native_production_entry_indeterminate" : "native_fixture_or_observation_unavailable");
        releaseGenerated();
    }
    return result;
}
}
