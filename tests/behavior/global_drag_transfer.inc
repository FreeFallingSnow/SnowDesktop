// Included inside drag_global after Axis, Role and AddAxis are defined.
// These are input domains and observed-result vocabularies, not acceptance
// decisions. The caller must report missing adapters/oracles as unimplemented.
//
// Structural relations to implement in the shared constraint engine:
// - A direction is relevant only when its corresponding sort field is not
//   manual/na. A date-group setting is relevant only to a supporting source
//   or target. Capacity/occupancy apply only to a capacity-bearing target.
// - Lua reference kinds, binding policy and receiver permission apply only to
//   the corresponding Lua surface. Host/component capability bits remain
//   independent inputs, including unsupported combinations to be rejected.
// - T03 and T04 are conditional on the transfer boundary; do not use the
//   production acceptance decision to exclude an offered effect or payload.
// - X02 failure injection must occur at a stage present in the chosen finite
//   event sequence. X03 ordering requiring two tasks requires two tasks.
// - S04 requires a native-origin OLE round trip; S05 requires two tasks;
//   S08 requires a cross-page move; S09 requires external enter/leave.
//   S01-S10 describe composable event fragments, not mutually exclusive
//   scenario names; the scheduler must merge compatible fragments into an
//   ordered finite sequence and reject only contradictory input timelines.
// - X01, T05 and C10.relation are outcomes/derived observations, never
//   independent product-result selectors. V07 is a controlled physical
//   volume fixture; the resulting R03 path relation is derived from it.
// - Unknown volume identity, hidden-desktop policy and other unspecified
//   outcomes must remain explicitly unresolved, not default to rejection.

inline constexpr std::string_view kEnvironmentChangeFields[] = {
    "V01.change", "V05.change", "V06.change", "V04.lua_change",
    "source.E23.change", "source.E24.change", "target.E23.change", "target.E24.change",
    "H02.connection", "H06.reservation_change", "H07.occupancy_change"};
inline constexpr std::string_view kLifecycleChangeFields[] = {
    "X04.source", "X04.selection", "X04.source_footprint", "X04.target",
    "X04.anchor", "X04.popup", "X04.runtime", "X04.application"};

inline bool EnvironmentChangeActive(std::string_view id, std::string_view value)
{
    return !value.empty() && value != "na" && value != "none" && value != "stable" && value != "unchanged" &&
        !(id == "H02.connection" && value == "offline");
}

inline void AppendTransferAxes(std::vector<Axis>& axes)
{
    AddAxis(axes, "G04.termination", Role::Sequence,
        {"drop", "leave", "escape", "outside_dismiss", "unspecified"}, "gesture termination");
    AddAxis(axes, "C01", Role::Conditional,
        {"na", "manual", "name", "type", "size", "modified_time"}, "source sort field");
    AddAxis(axes, "C02", Role::Conditional,
        {"na", "ascending", "descending"}, "source sort direction");
    AddAxis(axes, "C03", Role::Conditional,
        {"na", "manual", "name", "type", "size", "modified_time"}, "target sort field");
    AddAxis(axes, "C04", Role::Conditional,
        {"na", "ascending", "descending"}, "target sort direction");
    AddAxis(axes, "C05", Role::Conditional,
        {"na", "off", "all", "selected_categories"}, "source automatic collection");
    AddAxis(axes, "C06", Role::Conditional,
        {"na", "off", "all", "selected_categories"}, "target automatic collection");
    AddAxis(axes, "C07", Role::Conditional,
        {"na", "off", "on"}, "program collection policy");
    AddAxis(axes, "C08", Role::Conditional,
        {"na", "off", "on"}, "source date grouping");
    AddAxis(axes, "C09", Role::Conditional,
        {"na", "off", "on"}, "target date grouping");

    // Counts name concrete integers, not coarse size classes. A fixture may
    // contain overflow after a grid change; that is an input to check.
    // Hover P02.count and final P02.resolved_count are independent. A final
    // many value requires P02.many_count; unresolved is never guessed.
    const auto countAxis = [&](std::string id, std::string label, bool capacity) {
        Axis axis{std::move(id), Role::Conditional, {"na"}, std::move(label)};
        if (capacity) axis.values.emplace_back("no_grid");
        axis.values.insert(axis.values.end(), {"zero", "one", "two"});
        for (int count = 3; count <= 129; ++count)
            axis.values.push_back("count_" + std::to_string(count));
        axis.values.emplace_back("above_129_unresolved");
        axis.values.emplace_back("unspecified_unresolved");
        axes.push_back(std::move(axis));
    };
    countAxis("C10.capacity", "target capacity", true);
    countAxis("C10.used", "occupied target positions", false);
    AddAxis(axes, "C10.initial_state", Role::Derived,
        {"na", "empty", "partial", "full", "empty_and_full", "overflow", "no_grid", "unknown"}, "initial target occupancy");
    AddAxis(axes, "C10.relation", Role::Derived,
        {"na", "has_room", "exact_fit", "insufficient", "overflow", "no_grid", "unknown"}, "capacity relative to selection");
    AddAxis(axes, "C11.base", Role::Conditional,
        {"na", "default", "custom_extension"}, "classification base rules");
    AddAxis(axes, "C11.shortcut_target", Role::Conditional,
        {"na", "off", "on"}, "classification through shortcut target");
    AddAxis(axes, "C11.rule_conflicts", Role::Conditional,
        {"na", "none", "overlap", "exclusion", "overlap_and_exclusion"}, "classification overlaps and exclusions");
    AddAxis(axes, "C12", Role::Conditional,
        {"na", "binding", "collection"}, "logical slot kind");
    // Bit-set values enumerate all combinations, including the empty set.
    AddAxis(axes, "C13", Role::Conditional,
        {"na", "none", "app", "desktop", "filesystem", "app_desktop",
         "app_filesystem", "desktop_filesystem", "app_desktop_filesystem"}, "accepted reference kinds");
    AddAxis(axes, "C14.replacement", Role::Conditional,
        {"na", "reject", "allow"}, "binding replacement policy");
    AddAxis(axes, "C14.binding_state", Role::Conditional,
        {"na", "empty", "occupied"}, "initial binding state");

    // Capability bits can co-occur. Each bit is a distinct dimension; an
    // unsupported combination remains an executable negative path.
    AddAxis(axes, "C15.native_drop", Role::Conditional, {"na", "off", "on"}, "native drop capability");
    AddAxis(axes, "C15.pointer_reorder", Role::Conditional, {"na", "off", "on"}, "pointer reorder capability");
    AddAxis(axes, "C15.pointer_transfer", Role::Conditional, {"na", "off", "on"}, "pointer transfer capability");
    AddAxis(axes, "C15.file_drop", Role::Conditional, {"na", "off", "on"}, "file drop capability");
    AddAxis(axes, "C15.file_drop_async", Role::Conditional, {"na", "off", "on"}, "asynchronous file drop capability");
    AddAxis(axes, "C16.initial", Role::Conditional,
        {"na", "enabled", "disabled"}, "receiver initial enablement");
    AddAxis(axes, "C16.change", Role::Sequence,
        {"na", "unchanged", "disable_while_waiting"}, "receiver enablement event");

    AddAxis(axes, "T01", Role::Input,
        {"internal_native", "internal_to_ole", "external_ingress", "external_egress", "ole_return"}, "transfer boundary");
    // Format availability is a set: the same IDataObject may offer several
    // formats, and a broken virtual batch may also offer a preview image.
    AddAxis(axes, "T02.file_paths", Role::Conditional, {"na", "no", "yes"}, "offers real file paths");
    AddAxis(axes, "T02.shell_object", Role::Conditional, {"na", "no", "yes"}, "offers shell objects");
    AddAxis(axes, "T02.virtual_files", Role::Conditional, {"na", "no", "yes"}, "offers virtual file descriptors and streams");
    AddAxis(axes, "T02.virtual_completeness", Role::Conditional,
        {"na", "complete", "missing_entry", "unreadable_stream"}, "virtual batch completeness");
    AddAxis(axes, "T02.image", Role::Conditional, {"na", "no", "yes"}, "offers image data");
    AddAxis(axes, "T02.text", Role::Conditional, {"na", "no", "yes"}, "offers text data");
    AddAxis(axes, "T02.url", Role::Conditional, {"na", "no", "yes"}, "offers url data");
    AddAxis(axes, "T02.url_kind", Role::Conditional,
        {"na", "webpage", "direct_file", "invalid"}, "offered url kind");
    AddAxis(axes, "T02.invalid_format", Role::Conditional, {"na", "no", "yes"}, "offers malformed data");
    AddAxis(axes, "T02", Role::Derived,
        {"na", "file_paths", "shell_object", "virtual_descriptor_stream", "image", "text", "url",
         "multiple_formats", "invalid_format"}, "observed content representation");
    AddAxis(axes, "T03", Role::Conditional,
        {"na", "none", "copy", "move", "link", "copy_move", "copy_link", "move_link", "copy_move_link"},
        "source allowed effects");
    AddAxis(axes, "T03.extra_protocol_bits", Role::Conditional,
        {"na", "none", "scroll", "other"}, "additional protocol effect bits");
    AddAxis(axes, "T04.capability", Role::Conditional,
        {"na", "absent", "present_disabled", "enabled", "probe_failure"}, "source asynchronous capability");
    AddAxis(axes, "T04.availability", Role::Conditional,
        {"na", "immediate", "delayed_path", "delayed_virtual"}, "source content availability");
    AddAxis(axes, "T04.extraction", Role::Derived,
        {"na", "synchronous", "asynchronous", "probe_failed"}, "effective content extraction");
    AddAxis(axes, "T05", Role::Derived,
        {"na", "source_owned", "host_temporary", "published_file", "failed_partial_temporary"},
        "content ownership after extraction");

    AddAxis(axes, "V01.initial", Role::Input,
        {"unknown", "same_dpi", "mixed_dpi"}, "display scaling");
    AddAxis(axes, "V01.change", Role::Sequence,
        {"none", "dpi_changes_after_cross_screen"}, "display scaling transition");
    // Path properties are independently selectable, so a Unicode UNC path
    // can also be long, differ in case and share a directory-name prefix.
    AddAxis(axes, "V02.unicode", Role::Conditional, {"na", "no", "yes"}, "unicode path");
    AddAxis(axes, "V02.case_variant", Role::Conditional, {"na", "no", "yes"}, "path case variant");
    AddAxis(axes, "V02.long", Role::Conditional, {"na", "no", "yes"}, "long path");
    AddAxis(axes, "V02.unc", Role::Conditional, {"na", "no", "yes"}, "network unc path");
    AddAxis(axes, "V02.same_name_prefix", Role::Conditional, {"na", "no", "yes"}, "same-prefix directory boundary");
    AddAxis(axes, "V02.invalid", Role::Conditional, {"na", "no", "yes"}, "invalid path input");
    AddAxis(axes, "V03", Role::Input,
        {"unknown", "software_desktop", "native_desktop", "desktop_icons_hidden", "floating_or_popup_only"}, "desktop visibility mode");
    AddAxis(axes, "V04.source_read", Role::Conditional,
        {"na", "readable", "unreadable"}, "source readability");
    AddAxis(axes, "V04.target_write", Role::Conditional,
        {"na", "writable", "read_only"}, "target writability");
    AddAxis(axes, "V04.lua_permission", Role::Conditional,
        {"na", "granted", "not_granted", "revoked"}, "receiver file permission");
    AddAxis(axes, "V04.lua_change", Role::Sequence,
        {"na", "none", "revoke_while_waiting"}, "receiver permission transition");
    AddAxis(axes, "V04.integrity", Role::Conditional,
        {"na", "matched", "source_higher", "target_higher"}, "source and target privilege level");
    AddAxis(axes, "V05.initial", Role::Input,
        {"unknown", "no_valid_display", "single_display", "multiple_displays"}, "initial effective displays");
    AddAxis(axes, "V05.change", Role::Sequence,
        {"none", "disconnect", "reconnect", "disconnect_then_reconnect"}, "display topology event");
    AddAxis(axes, "V06.arrangement", Role::Conditional,
        {"na", "matches_physical", "differs_from_physical"}, "configured display arrangement");
    AddAxis(axes, "V06.screen_sizes", Role::Conditional,
        {"na", "same_size", "different_sizes"}, "relative display sizes");
    AddAxis(axes, "V06.end_screens", Role::Conditional,
        {"na", "original_order", "swapped"}, "first and last display order");
    AddAxis(axes, "V06.change", Role::Sequence,
        {"na", "none", "reorder_while_waiting"}, "display arrangement change");
    AddAxis(axes, "V07", Role::Conditional,
        {"na", "all_same_volume", "all_cross_volume", "mixed_volumes", "unknown_volume"}, "source batch and physical target volume fixture");

    AddAxis(axes, "X01", Role::Derived,
        {"all_succeeded", "no_change", "cancelled", "failed", "partial_success"}, "observed completion");
    AddAxis(axes, "X02", Role::Sequence,
        {"none", "format_probe", "read_or_download", "cross_thread", "async_start",
         "file_commit", "logical_commit", "layout_save", "target_callback"}, "injected failure stage");
    AddAxis(axes, "X02.position", Role::Sequence,
        {"na", "no_interruption", "before_any_item", "after_partial_success", "while_io_in_flight"}, "injected failure or cancellation position");
    AddAxis(axes, "X03.task_count", Role::Sequence,
        {"one", "two"}, "number of independently tracked tasks");
    AddAxis(axes, "X03.submit_order", Role::Sequence,
        {"na", "a_then_b", "b_then_a", "simultaneous"}, "task submission order");
    AddAxis(axes, "X03.order", Role::Sequence,
        {"single", "a_then_b", "b_then_a", "interleaved"}, "completion order");
    AddAxis(axes, "X03.callback_reentrant", Role::Sequence,
        {"no", "yes"}, "callback reentrancy");
    AddAxis(axes, "X03.callback_duplicate", Role::Sequence,
        {"no", "yes"}, "duplicate completion notification");
    AddAxis(axes, "X03.refresh", Role::Sequence,
        {"normal", "before_later_completion"}, "refresh relative to completion");
    AddAxis(axes, "X04.source", Role::Sequence,
        {"stable", "delete", "reorder"}, "source lifecycle event");
    AddAxis(axes, "X04.selection", Role::Sequence,
        {"stable", "change_global_selection"}, "source selection change");
    AddAxis(axes, "X04.source_footprint", Role::Sequence,
        {"stable", "change"}, "source footprint change");
    AddAxis(axes, "X04.target", Role::Sequence,
        {"stable", "move", "delete", "replace_identity", "change_type", "remap_directory"}, "target lifecycle event");
    AddAxis(axes, "X04.anchor", Role::Sequence,
        {"na", "stable", "disappears"}, "captured insertion anchor lifecycle event");
    AddAxis(axes, "X04.popup", Role::Sequence,
        {"na", "stable", "close"}, "popup lifecycle event");
    AddAxis(axes, "X04.runtime", Role::Sequence,
        {"na", "stable", "replace"}, "component runtime lifecycle event");
    AddAxis(axes, "X04.application", Role::Sequence,
        {"stable", "exit"}, "application lifecycle event");
    for (const auto id : kLifecycleChangeFields)
        AddAxis(axes, std::string(id) + "_phase", Role::Sequence,
            {"na", "unspecified", "before_source_read", "after_read_before_submit", "during_io",
             "after_commit_before_reload"}, "independent lifecycle event phase");
    AddAxis(axes, "X05", Role::Sequence,
        {"cold", "warm_hit", "revision_invalidated", "disabled_after_drag"}, "planning cache history");

    // Independent finite event fragments; the common scheduler supplies
    // parameterized identity, payload, time and target for each event.
    AddAxis(axes, "S01", Role::Sequence,
        {"na", "none", "press_threshold_hover_change_release"}, "gesture modifier change");
    AddAxis(axes, "S02", Role::Sequence,
        {"na", "none", "right_menu_refresh_then_decide"}, "right menu reentrancy");
    AddAxis(axes, "S03.dwell", Role::Sequence,
        {"na", "none", "enter_dwell_open", "enter_dwell_switch_tab"}, "dwell and target switching");
    AddAxis(axes, "S03.navigation", Role::Sequence,
        {"na", "none", "scroll", "page_flip"}, "navigation before release");
    AddAxis(axes, "S04", Role::Sequence,
        {"na", "none", "native_ole_external_return_release"}, "ole round trip");
    AddAxis(axes, "S05", Role::Sequence,
        {"na", "none", "capture_a_capture_b_complete_b_refresh_complete_a"}, "asynchronous interleaving");
    AddAxis(axes, "S06", Role::Sequence,
        {"na", "none", "wait_then_lifecycle_change"}, "change during pending work");
    AddAxis(axes, "S07", Role::Sequence,
        {"na", "none", "commit_enumerate_refresh_rebuild_save_reload"}, "durable committed result");
    AddAxis(axes, "S08", Role::Sequence,
        {"na", "none", "cross_page_empty_cleanup_map_save_reload"}, "page lifecycle");
    AddAxis(axes, "S09", Role::Sequence,
        {"na", "none", "leave_return_old_timer", "leave_replace_source_old_timer", "leave_submit_old_timer",
         "leave_new_session_leave_old_timer", "leave_internal_takeover_old_timer"}, "late leave timer");
    AddAxis(axes, "S10", Role::Sequence,
        {"na", "none", "sort_capture_drag_refresh_save_reload"}, "sorted member manual drag");

    // Each change has an independent phase. `unspecified` remains a modeled
    // unresolved input, and two events in the same phase do not imply order.
    for (const auto id : kEnvironmentChangeFields)
        AddAxis(axes, std::string(id) + "_phase", Role::Sequence,
            {"na", "unspecified", "after_interactions_before_release", "before_source_read",
             "after_read_before_submit", "during_io", "after_commit_before_reload"},
            "independent environment event phase");
}

inline bool TransferCount(std::string_view value, std::size_t& out)
{
    constexpr std::string_view countPrefix = "count_";
    if (value.substr(0, countPrefix.size()) == countPrefix)
    {
        const auto digits = value.substr(countPrefix.size());
        if (digits.empty() || (digits.size() > 1 && digits.front() == '0')) return false;
        std::size_t parsed = 0;
        for (const char digit : digits)
        {
            if (digit < '0' || digit > '9') return false;
            parsed = parsed * 10 + static_cast<std::size_t>(digit - '0');
            if (parsed > 129) return false;
        }
        if (parsed < 3) return false;
        out = parsed;
        return true;
    }
    if (value == "zero") out = 0;
    else if (value == "one") out = 1;
    else if (value == "two") out = 2;
    else if (value == "three") out = 3;
    else if (value == "four") out = 4;
    else if (value == "eight") out = 8;
    else if (value == "one_hundred_twenty_seven") out = 127;
    else if (value == "one_hundred_twenty_eight") out = 128;
    else if (value == "one_hundred_twenty_nine") out = 129;
    else return false;
    return true;
}

// V3 source cardinality comes from a separately validated exact family
// descriptor, not the legacy 129-slot parser or a host outcome hypothesis.
inline bool TransferExactSourceCountV3(std::string_view value, std::size_t& out)
{
    if (value == "zero") { out = 0; return true; }
    if (value == "one") { out = 1; return true; }
    if (value == "two") { out = 2; return true; }
    if (value.substr(0, 6) != "count_") return false;
    const auto digits = value.substr(6);
    if (digits.empty() || digits.front() == '0') return false;
    const auto max = ~std::size_t{0};
    std::size_t parsed = 0;
    for (const char digit : digits)
    {
        if (digit < '0' || digit > '9') return false;
        const auto n = static_cast<std::size_t>(digit - '0');
        if (parsed > (max - n) / 10) return false;
        parsed = parsed * 10 + n;
    }
    if (parsed < 3) return false;
    out = parsed;
    return true;
}

// C10 is calculated from independently constructed counts, never by asking
// the production target whether it accepts the operation.
inline std::string_view CapacityInitialState(const std::vector<Axis>& axes, const Candidate& candidate)
{
    const auto capacity = Value(axes, candidate, "C10.capacity");
    if (capacity == "na") return "na";
    if (capacity == "no_grid") return "no_grid";
    std::size_t slots = 0, used = 0;
    if (!TransferCount(capacity, slots) || !TransferCount(Value(axes, candidate, "C10.used"), used))
        return "unknown";
    if (used > slots) return "overflow";
    if (used == 0 && slots == 0) return "empty_and_full";
    if (used == 0) return "empty";
    return used == slots ? "full" : "partial";
}

inline std::string_view CapacityRelation(const std::vector<Axis>& axes, const Candidate& candidate)
{
    const auto capacity = Value(axes, candidate, "C10.capacity");
    if (capacity == "na") return "na";
    if (capacity == "no_grid") return "no_grid";
    std::size_t slots = 0, used = 0, selected = 0;
    if (!TransferCount(capacity, slots) || !TransferCount(Value(axes, candidate, "C10.used"), used))
        return "unknown";
    if (used > slots) return "overflow";
    auto selection = Value(axes, candidate, "P02.resolved_count");
    if (selection == "many") selection = Value(axes, candidate, "P02.many_count");
    if (!TransferCount(selection, selected)) return "unknown";
    const auto remaining = slots - used;
    if (remaining == selected) return "exact_fit";
    return remaining > selected ? "has_room" : "insufficient";
}

// Pure structural constraints only. The returned reason is counted for each
// excluded tuple; source/target support or a product decision is never queried.
inline std::string_view TransferLocalExclusionPrefixed(const std::vector<Axis>& axes,
    const Candidate& candidate, std::string_view prefix)
{
    const auto v = [&](std::string_view id) -> std::string_view {
        return prefix.empty() ? Value(axes, candidate, id) : Value(axes, candidate, std::string(prefix) + std::string(id));
    };
    const auto sortDirectionMismatch = [](std::string_view field, std::string_view direction) {
        return (field == "na" || field == "manual") ? direction != "na" : direction == "na";
    };
    if (sortDirectionMismatch(v("C01"), v("C02"))) return "source_sort_direction_not_applicable";
    if (sortDirectionMismatch(v("C03"), v("C04"))) return "target_sort_direction_not_applicable";
    if (v("C10.capacity") == "na" && v("C10.used") != "na")
        return "occupied_positions_require_capacity_context";
    if (v("C10.capacity") == "no_grid" && v("C10.used") != "na")
        return "occupied_positions_not_applicable_without_grid";
    if (v("C10.capacity") != "na" && v("C10.capacity") != "no_grid" && v("C10.used") == "na")
        return "grid_capacity_requires_occupied_count";

    const bool binding = v("C12") == "binding";
    if (binding != (v("C14.replacement") != "na") || binding != (v("C14.binding_state") != "na"))
        return "binding_policy_requires_binding_slot";
    const bool logicalSlot = binding || v("C12") == "collection";
    if (logicalSlot != (v("C13") != "na")) return "reference_acceptance_requires_logical_slot";

    if ((v("T02.virtual_files") == "yes") != (v("T02.virtual_completeness") != "na"))
        return "virtual_completeness_requires_virtual_format";
    if ((v("T02.url") == "yes") != (v("T02.url_kind") != "na"))
        return "url_kind_requires_url_format";

    if ((v("V04.lua_permission") == "na") != (v("V04.lua_change") == "na"))
        return "permission_transition_requires_permission_context";
    if (v("X02") != "na" && v("X02") != "none" &&
        (v("X02.position") == "na" || v("X02.position") == "no_interruption"))
        return "injected_failure_requires_interruption_position";
    return {};
}
inline std::string_view TransferLocalExclusion(const std::vector<Axis>& axes, const Candidate& candidate)
{ return TransferLocalExclusionPrefixed(axes, candidate, {}); }

inline std::string_view TransferExclusion(const std::vector<Axis>& axes, const Candidate& candidate)
{
    if (const auto reason = TransferLocalExclusion(axes, candidate); !reason.empty()) return reason;
    const auto v = [&](std::string_view id) -> std::string_view { return Value(axes, candidate, id); };

    // An S09 takeover includes a preceding external session even when the
    // final T01 is native. Its OLE source inputs must remain representable.
    const bool noExternalSession = v("S09") == "na" || v("S09") == "none";
    const bool nativeOnly = v("T01") == "internal_native" && noExternalSession;
    if (nativeOnly && (v("T03") != "na" || v("T03.extra_protocol_bits") != "na" ||
        v("T04.capability") != "na" || v("T04.availability") != "na"))
        return "native_transport_has_no_ole_source";
    if (!nativeOnly && (v("T03") == "na" || v("T03.extra_protocol_bits") == "na" ||
        v("T04.capability") == "na"))
        return "ole_transport_requires_source_protocol_inputs";

    const bool twoTasks = v("X03.task_count") == "two";
    if (twoTasks == (v("X03.order") == "single")) return "completion_order_requires_matching_task_count";
    if (twoTasks == (v("X03.submit_order") == "na")) return "submission_order_requires_matching_task_count";
    if (!twoTasks && v("X03.refresh") == "before_later_completion")
        return "later_completion_requires_two_tasks";

    // Menu inputs include independent later button states. Initiating-button
    // metadata alone cannot prove whether a right-menu sequence is possible;
    // keep the case and let the unresolved program require a real adapter.
    if (v("S04") == "native_ole_external_return_release" && v("T01") != "ole_return")
        return "round_trip_sequence_requires_return_transport";
    if (v("S05") == "capture_a_capture_b_complete_b_refresh_complete_a" &&
        (!twoTasks || (v("X03.order") != "b_then_a" && v("X03.order") != "interleaved") ||
         v("X03.refresh") != "before_later_completion"))
        return "interleaved_sequence_requires_two_tasks_and_late_completion";

    // S09 may finish with an internal takeover; one T01 value cannot describe
    // both the preceding external session and the later native one. Do not
    // exclude it by T01 until the scheduler models each session separately.
    return {};
}

// Produce a plan of external inputs and observations, never substitute the
// host's selection, action negotiation, commit, ownership or persistence.
// The second task has its own scene and payload; no S sequence or X03
// scheduler is recursively copied. Selectors precede member slots so prefix
// counting only reads already assigned inputs.
inline void AppendSecondTaskAxesVersion(std::vector<Axis>& axes, bool sourceSlots)
{
    const auto original = axes;
    const auto included = [](std::string_view id) {
        if (id.substr(0, 8) == "source.E" || id.substr(0, 8) == "target.E") return true;
        if (id.size() >= 3 && (id[0] == 'P' || id[0] == 'R' || id[0] == 'G' ||
            id[0] == 'H' || id[0] == 'C' || id[0] == 'T') &&
            id[1] >= '0' && id[1] <= '9' && id[2] >= '0' && id[2] <= '9') return true;
        for (const auto field : {std::string_view{"V02"}, std::string_view{"V04"},
                                 std::string_view{"V07"}, std::string_view{"X02"},
                                 std::string_view{"X04"}, std::string_view{"X05"}})
            if (id == field || (id.size() > field.size() && id.substr(0, field.size()) == field &&
                                id[field.size()] == '.')) return true;
        return false;
    };
    const auto append = [&](const Axis& primary) {
        Axis copy = primary;
        copy.id = "task_b." + primary.id;
        if (copy.role != Role::Derived &&
            std::find(copy.values.begin(), copy.values.end(), "na") == copy.values.end())
            copy.values.insert(copy.values.begin(), "na");
        axes.push_back(std::move(copy));
    };
    const std::vector<std::string_view> selectors = sourceSlots ?
        std::vector<std::string_view>{"P02.source_actual_count", "P02.count", "P02.resolved_count", "P02.many_count"} :
        std::vector<std::string_view>{"P02.count", "P02.resolved_count", "P02.many_count"};
    for (const auto selector : selectors)
        for (const auto& axis : original)
            if (axis.id == selector) append(axis);
    for (const auto& axis : original)
        if (included(axis.id) && std::find(selectors.begin(), selectors.end(), axis.id) == selectors.end()) append(axis);
}

inline void AppendSecondTaskAxes(std::vector<Axis>& axes) { AppendSecondTaskAxesVersion(axes, false); }
inline void AppendSecondTaskAxesV2(std::vector<Axis>& axes) { AppendSecondTaskAxesVersion(axes, true); }

// V3's two finite source vectors are independent. Only endpoint/transfer
// axes are copied from A; B's selection axes and cross-task references are
// generated with B's count and A's count, respectively. This schema builder
// is not a product execution or a complete unbounded enumerator.
// A null B is the unique inactive family; active zero remains distinct.
// On failure return its classification without appending any partial schema.
inline V3SelectionStatus AppendSecondTaskAxesV3(std::vector<Axis>& axes,
    const V3TaskCounts& taskA, const V3TaskCounts* taskB)
{
    try
    {
        std::vector<Axis> aSelection, bSelection, added;
        const auto aStatus = AppendSelectionAxesV3(aSelection, taskA, taskA.sourceCount, false);
        if (aStatus != V3SelectionStatus::ok) return aStatus;
        const V3TaskCounts inactive{};
        const auto bStatus = AppendSelectionAxesV3(bSelection, taskB ? *taskB : inactive, taskA.sourceCount, true);
        if (bStatus != V3SelectionStatus::ok) return bStatus;
        std::map<std::string, bool> aSelectionIds;
        std::map<std::string, const Axis*> inputIds;
        for (const auto& axis : axes)
            if (axis.id.substr(0, 7) == "task_b." || !inputIds.emplace(axis.id, &axis).second)
                return V3SelectionStatus::schema_error;
        for (const auto& axis : aSelection)
        {
            const auto found = inputIds.find(axis.id);
            if (found == inputIds.end() || found->second->role != axis.role || found->second->values != axis.values ||
                found->second->label != axis.label) return V3SelectionStatus::schema_error;
        }
        for (const auto& axis : aSelection) aSelectionIds.emplace(axis.id, true);
        const auto included = [](std::string_view id) {
            if (id.substr(0, 8) == "source.E" || id.substr(0, 8) == "target.E") return true;
            if (id.size() >= 3 && (id[0] == 'P' || id[0] == 'R' || id[0] == 'G' ||
                id[0] == 'H' || id[0] == 'C' || id[0] == 'T') &&
                id[1] >= '0' && id[1] <= '9' && id[2] >= '0' && id[2] <= '9') return true;
            for (const auto field : {std::string_view{"V02"}, std::string_view{"V04"},
                                     std::string_view{"V07"}, std::string_view{"X02"},
                                     std::string_view{"X04"}, std::string_view{"X05"}})
                if (id == field || (id.size() > field.size() && id.substr(0, field.size()) == field &&
                                    id[field.size()] == '.')) return true;
            return false;
        };
        const auto append = [&](const Axis& primary) {
            Axis copy = primary;
            copy.id = "task_b." + primary.id;
            if (!taskB && copy.role != Role::Derived) copy.values = {"na"};
            added.push_back(std::move(copy));
        };
        const std::vector<std::string_view> selectors{"P02.source_actual_count", "P02.count",
            "P02.resolved_count", "P02.many_count"};
        for (const auto selector : selectors)
        {
            const auto found = std::find_if(bSelection.begin(), bSelection.end(),
                [&](const auto& axis) { return axis.id == selector; });
            if (found == bSelection.end()) return V3SelectionStatus::schema_error;
            append(*found);
        }
        for (const auto& axis : axes)
            if (included(axis.id) && !aSelectionIds.contains(axis.id)) append(axis);
        for (const auto& axis : bSelection)
            if (std::find(selectors.begin(), selectors.end(), axis.id) == selectors.end()) append(axis);
        if (added.size() > axes.max_size() - axes.size()) return V3SelectionStatus::resource_unavailable;
        axes.reserve(axes.size() + added.size());
        for (auto& axis : added) axes.push_back(std::move(axis));
        return V3SelectionStatus::ok;
    }
    catch (const std::bad_alloc&) { return V3SelectionStatus::resource_unavailable; }
    catch (const std::length_error&) { return V3SelectionStatus::resource_unavailable; }
}

// Schema-only V4 B task. Each count is derived from its own descriptor;
// unknown counts are unavailable, never interpreted as an empty selection.
// Reference values describe potential source identities, not constructed
// objects or a product decision. Full applicability and graph semantics stay
// unresolved, including cross-kind alias relationships.
inline V4AxisResult AppendSecondTaskAxesV4(std::vector<Axis>& axes,
    const V4FamilyDescriptor& family, const V4AxisBudget& budget)
{
    V4AxisResult result;
    try
    {
        const auto aLogical = V4MaterializeCount(family.a.selectionCount, budget.logicalMembers, false);
        const auto aFiles = V4MaterializeCount(family.a.fileCount, budget.fileMembers,
            family.a.kind == V4SourceKind::content_only);
        const auto aVirtual = V4MaterializeCount(family.a.virtualCount, budget.virtualMembers, true);
        for (const auto status : {aLogical.status, aFiles.status, aVirtual.status})
            if (status != V4AxisStatus::schema_only)
            { result.status = status; result.unresolved.emplace_back("v4_task_a_count_unavailable"); return result; }
        std::vector<Axis> aSelection, bSelection, added;
        const auto aStatus = AppendSelectionAxesV4(aSelection, family.a, aLogical.value, false, budget);
        if (aStatus.status != V4AxisStatus::schema_only) return aStatus;
        result.unresolved = aStatus.unresolved;
        V4TaskSpec inactive;
        inactive.kind = V4SourceKind::content_only;
        inactive.selectionCount = {V3CountKind::exact, "0"};
        const auto& bTask = family.b ? *family.b : inactive;
        V4AxisBudget remaining = budget;
        remaining.logicalMembers -= aLogical.value;
        remaining.fileMembers -= aFiles.value;
        remaining.virtualMembers -= aVirtual.value;
        const auto takeReferenceDomain = [&] {
            if (aLogical.value > remaining.referenceMembers) return false;
            remaining.referenceMembers -= aLogical.value; return true;
        };
        // A uses one logical directory-reference domain and one anchor
        // domain, plus one logical domain per physical/virtual member.
        if (!takeReferenceDomain() || !takeReferenceDomain())
        { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_task_a_reference_budget_unavailable"); return result; }
        for (unsigned domain = 0; domain < 2; ++domain)
        {
            if (family.a.preparation.outsideIdentities.size() > remaining.referenceMembers)
            { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_task_a_outside_reference_budget_unavailable"); return result; }
            remaining.referenceMembers -= family.a.preparation.outsideIdentities.size();
        }
        if (family.a.dimensions.size() > remaining.referenceMembers)
        { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_task_a_dimension_reference_budget_unavailable"); return result; }
        remaining.referenceMembers -= family.a.dimensions.size();
        if (family.a.positions.size() > remaining.referenceMembers)
        { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_task_a_position_reference_budget_unavailable"); return result; }
        remaining.referenceMembers -= family.a.positions.size();
        if (family.a.shortcutTargets.size() > remaining.referenceMembers)
        { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_task_a_shortcut_reference_budget_unavailable"); return result; }
        remaining.referenceMembers -= family.a.shortcutTargets.size();
        for (const auto count : {family.a.sourceGroup.children.size(), family.a.targetGroup.children.size()})
        {
            if (count > remaining.referenceMembers)
            { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_task_a_group_child_reference_budget_unavailable"); return result; }
            remaining.referenceMembers -= count;
        }
        for (std::size_t i = 0; i < aFiles.value; ++i)
            if (!takeReferenceDomain())
            { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_task_a_reference_budget_unavailable"); return result; }
        for (std::size_t i = 0; i < aVirtual.value; ++i)
            if (!takeReferenceDomain())
            { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_task_a_reference_budget_unavailable"); return result; }
        if (family.a.payloads.size() > remaining.payloadSlots)
        { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_payload_slot_budget_unavailable"); return result; }
        remaining.payloadSlots -= family.a.payloads.size();
        for (const auto count : {family.a.filePaths.size(), family.a.virtualNames.size()})
        {
            if (count > remaining.textSlots)
            { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_text_slot_budget_unavailable"); return result; }
            remaining.textSlots -= count;
        }
        for (unsigned field = 0; field < 3; ++field)
        {
            if (family.a.categoryRules.size() > remaining.textSlots)
            { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_category_rule_text_budget_unavailable"); return result; }
            remaining.textSlots -= family.a.categoryRules.size();
        }
        if (family.a.shortcutTargets.size() > remaining.textSlots)
        { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_shortcut_target_text_budget_unavailable"); return result; }
        remaining.textSlots -= family.a.shortcutTargets.size();
        for (const auto count : {family.a.sourceGroup.children.size(), family.a.targetGroup.children.size()})
        {
            if (count > remaining.textSlots)
            { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_group_child_text_budget_unavailable"); return result; }
            remaining.textSlots -= count;
        }
        if (family.a.requestResults.size() > remaining.requestResultSlots)
        { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_request_result_slot_budget_unavailable"); return result; }
        remaining.requestResultSlots -= family.a.requestResults.size();
        if (family.a.fileContents.size() > remaining.fileContentSlots)
        { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_file_content_slot_budget_unavailable"); return result; }
        remaining.fileContentSlots -= family.a.fileContents.size();
        if (family.a.directoryNodes.size() > remaining.directoryNodeSlots)
        { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_directory_node_slot_budget_unavailable"); return result; }
        remaining.directoryNodeSlots -= family.a.directoryNodes.size();
        if (family.a.directoryRoots.size() > remaining.directoryRootSlots)
        { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_directory_root_slot_budget_unavailable"); return result; }
        remaining.directoryRootSlots -= family.a.directoryRoots.size();
        if (family.a.preparation.steps.size() > remaining.preparationSteps)
        { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_preparation_step_budget_unavailable"); return result; }
        remaining.preparationSteps -= family.a.preparation.steps.size();
        const auto takeBytes = [&](std::size_t bytes) {
            if (bytes > remaining.payloadBytes) return false;
            remaining.payloadBytes -= bytes;
            return true;
        };
        for (const auto& slot : family.a.payloads)
            if (!takeBytes(slot.hex.size() / 2))
            { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_payload_byte_budget_unavailable"); return result; }
        for (const auto& slot : family.a.fileContents)
            if (!takeBytes(slot.hex.size() / 2))
            { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_file_content_byte_budget_unavailable"); return result; }
        for (const auto& node : family.a.directoryNodes)
            if (!takeBytes(node.relativePathUtf16Hex.size() / 2) || !takeBytes(node.contentHex.size() / 2))
            { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_directory_node_byte_budget_unavailable"); return result; }
        for (const auto* slots : {&family.a.filePaths, &family.a.virtualNames})
            for (const auto& slot : *slots)
                if (!takeBytes(slot.utf16leHex.size() / 2))
                { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_text_byte_budget_unavailable"); return result; }
        for (const auto& slot : family.a.requestResults)
            if (!takeBytes(slot.hresultHex.size() / 2))
            { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_hresult_byte_budget_unavailable"); return result; }
        for (const auto& step : family.a.preparation.steps)
            if (!takeBytes(step.modifiersHex.size() / 2) || !takeBytes(step.xHex.size() / 2) || !takeBytes(step.yHex.size() / 2))
            { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_preparation_byte_budget_unavailable"); return result; }
        for (const auto& outside : family.a.preparation.outsideIdentities)
            if (!takeBytes(outside.identityHex.size() / 2))
            { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_outside_identity_byte_budget_unavailable"); return result; }
        for (const auto& dimension : family.a.dimensions)
            if (!takeBytes(dimension.valueHex.size() / 2))
            { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_dimension_byte_budget_unavailable"); return result; }
        for (const auto& position : family.a.positions)
            if (!takeBytes(position.member.decimal.size()) || !takeBytes(position.position.decimal.size()))
            { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_position_byte_budget_unavailable"); return result; }
        for (const auto& rule : family.a.categoryRules)
            if (!takeBytes(rule.idUtf16Hex.size() / 2) || !takeBytes(rule.labelUtf16Hex.size() / 2) ||
                !takeBytes(rule.extensionsUtf16Hex.size() / 2))
            { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_category_rule_byte_budget_unavailable"); return result; }
        for (const auto& shortcut : family.a.shortcutTargets)
            if (!takeBytes(shortcut.extensionUtf16Hex.size() / 2))
            { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_shortcut_target_byte_budget_unavailable"); return result; }
        for (const auto* group : {&family.a.sourceGroup, &family.a.targetGroup})
        {
            if (!takeBytes(group->activeChild.decimal.size()) || !takeBytes(group->hitChild.decimal.size()))
            { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_group_index_byte_budget_unavailable"); return result; }
            for (const auto& child : group->children)
                if (!takeBytes(child.idUtf16Hex.size() / 2) || !takeBytes(child.typeHex.size() / 2))
                { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_group_child_byte_budget_unavailable"); return result; }
        }
        const auto bLogical = V4MaterializeCount(bTask.selectionCount, remaining.logicalMembers, false);
        const auto bFiles = V4MaterializeCount(bTask.fileCount, remaining.fileMembers,
            bTask.kind == V4SourceKind::content_only);
        const auto bVirtual = V4MaterializeCount(bTask.virtualCount, remaining.virtualMembers, true);
        for (const auto status : {bLogical.status, bFiles.status, bVirtual.status})
            if (status != V4AxisStatus::schema_only)
            { result.status = status; result.unresolved.emplace_back("v4_task_b_count_unavailable"); return result; }
        // Account for B's own logical domains and its A logical references
        // before budgeting the independent cross-task object-reference axes.
        auto bReferences = remaining.referenceMembers;
        const auto takeBReferences = [&](std::size_t count) {
            if (count > bReferences) return false;
            bReferences -= count; return true;
        };
        const auto bCrossReferences = family.b ? aLogical.value : std::size_t{0};
        if (!takeBReferences(bCrossReferences) || !takeBReferences(bLogical.value) || !takeBReferences(bLogical.value))
        { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_task_b_reference_budget_unavailable"); return result; }
        if (!takeBReferences(bTask.preparation.outsideIdentities.size()) || !takeBReferences(bTask.preparation.outsideIdentities.size()))
        { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_task_b_outside_reference_budget_unavailable"); return result; }
        if (!takeBReferences(bTask.dimensions.size()))
        { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_task_b_dimension_reference_budget_unavailable"); return result; }
        if (!takeBReferences(bTask.positions.size()))
        { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_task_b_position_reference_budget_unavailable"); return result; }
        if (!takeBReferences(bTask.shortcutTargets.size()))
        { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_task_b_shortcut_reference_budget_unavailable"); return result; }
        for (const auto count : {bTask.sourceGroup.children.size(), bTask.targetGroup.children.size()})
            if (!takeBReferences(count))
            { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_task_b_group_child_reference_budget_unavailable"); return result; }
        for (std::size_t i = 0; i < bFiles.value; ++i)
            if (!takeBReferences(bLogical.value))
            { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_task_b_reference_budget_unavailable"); return result; }
        for (std::size_t i = 0; i < bVirtual.value; ++i)
            if (!takeBReferences(bLogical.value))
            { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_task_b_reference_budget_unavailable"); return result; }
        if (family.b)
        {
            for (const auto count : {bFiles.value, bVirtual.value})
                for (std::size_t i = 0; i < count; ++i)
                    for (const auto width : {aFiles.value, aVirtual.value, bFiles.value, bVirtual.value})
                        if (!takeBReferences(width))
                        { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_cross_task_reference_budget_unavailable"); return result; }
        }
        const auto bStatus = AppendSelectionAxesV4(bSelection, bTask, bCrossReferences, true, remaining);
        result.unresolved.insert(result.unresolved.end(), bStatus.unresolved.begin(), bStatus.unresolved.end());
        if (bStatus.status != V4AxisStatus::schema_only) { result.status = bStatus.status; return result; }
        std::map<std::string, const Axis*> existing;
        for (const auto& axis : axes)
            if (axis.id.substr(0, 7) == "task_b." || !existing.emplace(axis.id, &axis).second)
            { result.status = V4AxisStatus::invalid_descriptor; result.unresolved.emplace_back("v4_duplicate_or_existing_b_axis"); return result; }
        std::map<std::string, bool> aSelectionIds;
        for (const auto& axis : aSelection)
        {
            const auto found = existing.find(axis.id);
            if (found == existing.end() || found->second->role != axis.role || found->second->values != axis.values ||
                found->second->label != axis.label || !aSelectionIds.emplace(axis.id, true).second)
            { result.status = V4AxisStatus::invalid_descriptor; result.unresolved.emplace_back("v4_task_a_schema_mismatch"); return result; }
        }
        const auto included = [](std::string_view id) {
            if (id.substr(0, 8) == "source.E" || id.substr(0, 8) == "target.E") return true;
            if (id.size() >= 3 && (id[0] == 'P' || id[0] == 'R' || id[0] == 'G' || id[0] == 'H' ||
                id[0] == 'C' || id[0] == 'T') && id[1] >= '0' && id[1] <= '9' && id[2] >= '0' && id[2] <= '9') return true;
            for (const auto field : {std::string_view{"V02"}, std::string_view{"V04"}, std::string_view{"V07"},
                std::string_view{"X02"}, std::string_view{"X04"}, std::string_view{"X05"}})
                if (id == field || (id.size() > field.size() && id.substr(0, field.size()) == field && id[field.size()] == '.')) return true;
            return false;
        };
        const auto append = [&](const Axis& primary) {
            Axis copy = primary; copy.id = "task_b." + primary.id;
            if (!family.b && copy.role != Role::Derived) copy.values = {"na"};
            added.push_back(std::move(copy));
        };
        const std::vector<std::string_view> selectors{"P02.source_actual_count", "P02.count", "P02.resolved_count", "P02.many_count"};
        for (const auto selector : selectors)
        {
            const auto found = std::find_if(bSelection.begin(), bSelection.end(), [&](const auto& axis) { return axis.id == selector; });
            if (found == bSelection.end()) { result.status = V4AxisStatus::invalid_descriptor; result.unresolved.emplace_back("v4_task_b_selector_missing"); return result; }
            append(*found);
        }
        for (const auto& axis : axes) if (included(axis.id) && !aSelectionIds.contains(axis.id)) append(axis);
        for (const auto& axis : bSelection)
            if (std::find(selectors.begin(), selectors.end(), axis.id) == selectors.end()) append(axis);
        if (family.b)
        {
            std::size_t references = 0;
            for (const auto count : {aFiles.value, aVirtual.value, bFiles.value, bVirtual.value})
            {
                if (count > budget.referenceMembers - references)
                { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_cross_task_reference_budget_unavailable"); return result; }
                references += count;
            }
            std::vector<std::string> domains{"none", "unknown_unresolved"};
            const auto addReferences = [&](std::string_view label, std::size_t count) {
                for (std::size_t i = 0; i < count; ++i) domains.push_back(std::string(label) + std::to_string(i));
            };
            addReferences("task_a_file_", aFiles.value); addReferences("task_a_virtual_", aVirtual.value);
            addReferences("task_b_file_", bFiles.value); addReferences("task_b_virtual_", bVirtual.value);
            for (const auto& [kind, count] : {std::pair{std::string_view{"file"}, bFiles.value},
                                              std::pair{std::string_view{"virtual"}, bVirtual.value}})
                for (std::size_t i = 0; i < count; ++i)
                    added.push_back(Axis{"task_b.V4." + std::string(kind) + ".member" + std::to_string(i) + "_object_ref",
                        Role::Input, domains, "B source object identity reference; construction unresolved"});
        }
        if (added.size() > axes.max_size() - axes.size())
        { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_second_axis_capacity_unavailable"); return result; }
        std::vector<Axis> combined = axes;
        combined.reserve(combined.size() + added.size());
        for (auto& axis : added) combined.push_back(std::move(axis));
        result.unresolved.emplace_back("v4_second_task_source_identity_and_execution_unimplemented");
        axes.swap(combined);
        result.status = V4AxisStatus::schema_only;
        return result;
    }
    catch (const std::bad_alloc&) { result.status = V4AxisStatus::resource_unavailable; return result; }
    catch (const std::length_error&) { result.status = V4AxisStatus::resource_unavailable; return result; }
}

inline bool SecondPrefixAllows(const std::vector<Axis>& axes, const Candidate& prefix,
    std::string_view axisId, std::string_view proposedValue)
{
    constexpr std::string_view taskPrefix = "task_b.";
    if (axisId.substr(0, taskPrefix.size()) != taskPrefix) return true;
    const auto id = axisId.substr(taskPrefix.size());
    if (Value(axes, prefix, "X03.task_count") != "two") return proposedValue == "na";
    auto effective = Value(axes, prefix, "task_b.P02.resolved_count");
    if (id == "P02.many_count") return effective == "many" ? proposedValue != "na" : proposedValue == "na";
    if (effective == "many") effective = Value(axes, prefix, "task_b.P02.many_count");
    std::size_t size = 0;
    const int selected = TransferCount(effective, size) ? static_cast<int>(size) : -1;
    return SelectionMemberAxisAllows(selected, id, proposedValue);
}

inline bool SecondPrefixAllowsV2(const std::vector<Axis>& axes, const Candidate& prefix,
    std::string_view axisId, std::string_view proposedValue)
{
    constexpr std::string_view taskPrefix = "task_b.";
    if (axisId.substr(0, taskPrefix.size()) != taskPrefix) return true;
    const auto id = axisId.substr(taskPrefix.size());
    if (Value(axes, prefix, "X03.task_count") != "two") return proposedValue == "na";
    // This pair describes a hypothetical host observation, not the source's
    // actual selection. It cannot select or truncate the B source slots.
    if (id == "P02.many_count")
        return Value(axes, prefix, "task_b.P02.resolved_count") == "many" ? proposedValue != "na" : proposedValue == "na";
    // Source cardinality cannot be mapped to these member slots until the
    // later transport/format axes are known. Retain all values here; the
    // complete-candidate V2 exclusion handles only a proven file source.
    return true;
}

inline bool SecondPrefixAllowsV3(const std::vector<Axis>& axes, const Candidate& prefix,
    std::string_view axisId, std::string_view proposedValue, bool taskBActive)
{
    constexpr std::string_view taskPrefix = "task_b.";
    if (axisId.substr(0, taskPrefix.size()) != taskPrefix) return true;
    const auto taskCount = Value(axes, prefix, "X03.task_count");
    if (taskCount != (taskBActive ? "two" : "one")) return false;
    if (!taskBActive) return proposedValue == "na";
    const auto id = axisId.substr(taskPrefix.size());
    // In this finite pure-file schema B's exact source size is part of the
    // descriptor; active zero is "zero", while absent B is "na".
    if (id == "P02.source_actual_count") return proposedValue != "na";
    if (id == "P02.many_count")
        return Value(axes, prefix, "task_b.P02.resolved_count") == "many" ? proposedValue != "na" : proposedValue == "na";
    return true;
}

// A task-local view lets existing single-task structural and pure derived
// functions inspect B's own inputs. Global S/X03 fields remain in the view
// only for context: do not apply global sequence rules to B through it.
struct SecondTaskProjection
{
    std::vector<Axis> axes;
    Candidate candidate;
};

inline SecondTaskProjection ProjectSecondTask(const std::vector<Axis>& axes, const Candidate& candidate)
{
    SecondTaskProjection result;
    result.candidate.ordinal = candidate.ordinal;
    const auto index = BuildIndex(axes);
    for (std::size_t i = 0; i < axes.size(); ++i)
    {
        if (axes[i].id.substr(0, 7) == "task_b.") continue;
        result.axes.push_back(axes[i]);
        const auto other = index->find("task_b." + axes[i].id);
        result.candidate.values.push_back(other == index->end() ? candidate.values[i] : candidate.values[other->second]);
    }
    result.candidate.lookup = BuildIndex(result.axes);
    return result;
}

inline std::string_view SecondTaskExclusion(const std::vector<Axis>& axes, const Candidate& candidate)
{
    if (Value(axes, candidate, "X03.task_count") != "two") return {};
    const auto projection = ProjectSecondTask(axes, candidate);
    if (const auto reason = EndpointExclusion(projection.axes, projection.candidate); !reason.empty()) return reason;
    if (const auto reason = SelectionExclusion(projection.axes, projection.candidate); !reason.empty()) return reason;
    // The S09 session and X03/S task scheduling refer to the combined event
    // program, not necessarily task B. No decision based on them is made here.
    return TransferLocalExclusion(projection.axes, projection.candidate);
}

inline std::string_view SecondTaskExclusionV2(const std::vector<Axis>& axes, const Candidate& candidate)
{
    if (Value(axes, candidate, "X03.task_count") != "two") return {};
    const auto projection = ProjectSecondTask(axes, candidate);
    if (const auto reason = EndpointExclusion(projection.axes, projection.candidate); !reason.empty()) return reason;
    if (const auto reason = SelectionExclusionV2(projection.axes, projection.candidate); !reason.empty()) return reason;
    return TransferLocalExclusion(projection.axes, projection.candidate);
}

inline std::string_view SecondTaskCapacityInitialState(const std::vector<Axis>& axes, const Candidate& candidate)
{
    if (Value(axes, candidate, "X03.task_count") != "two") return "na";
    const auto projection = ProjectSecondTask(axes, candidate);
    return CapacityInitialState(projection.axes, projection.candidate);
}

inline std::string_view SecondTaskCapacityRelation(const std::vector<Axis>& axes, const Candidate& candidate)
{
    if (Value(axes, candidate, "X03.task_count") != "two") return "na";
    const auto projection = ProjectSecondTask(axes, candidate);
    return CapacityRelation(projection.axes, projection.candidate);
}

inline std::string_view SecondTaskCapacityRelationV2(const std::vector<Axis>& axes, const Candidate& candidate)
{
    if (Value(axes, candidate, "X03.task_count") != "two") return "na";
    const auto capacity = Value(axes, candidate, "task_b.C10.capacity");
    if (capacity == "na") return "na";
    if (capacity == "no_grid") return "no_grid";
    // Source count and the hypothesized resolved count cannot prove what the
    // production target actually receives; await an independent observation.
    return "unknown";
}

// Names in EventStep.kind form the adapter contract; missing handlers must fail.
class TransferProgramEngine final
{
    friend class V3TransferValidatedAccess;
    friend class V4TransferValidatedAccess;
public:
    static EventProgram Legacy(const std::vector<Axis>& axes, const Candidate& candidate, bool sourceSlots)
    { return Build(axes, candidate, sourceSlots, false); }
private:
static EventProgram Build(const std::vector<Axis>& axes, const Candidate& candidate,
    bool sourceSlots, bool v3, bool v4 = false)
{
    EventProgram result;
    const auto v = [&](std::string_view id) -> std::string_view { return Value(axes, candidate, id); };
    const auto add = [&](std::string_view kind, std::string_view parameter) {
        result.steps.push_back({std::string(kind), std::string(parameter)});
    };
    const auto unresolved = [&](std::string_view reason) {
        result.unresolved.emplace_back(reason);
    };
    const auto active = [](std::string_view value) {
        return value != "na" && value != "none" && value != "stable" && value != "unchanged";
    };
    const auto oleInput = [&](std::string_view kind, std::string_view modifierId, std::string_view buttonId) {
        const auto modifiers = v(modifierId), buttons = v(buttonId);
        if (modifiers.empty() || modifiers == "na" || modifiers == "unknown" ||
            buttons.empty() || buttons == "na" || buttons == "unknown")
        {
            unresolved(std::string("ole_event_key_state_unspecified:") + std::string(kind));
            return;
        }
        add(kind, std::string("buttons=") + std::string(buttons) + ";modifiers=" + std::string(modifiers));
    };
    const auto lifecycleChanged = [&](std::string_view id) { return active(v(id)); };
    const auto lifecycleAt = [&](std::string_view phase) {
        std::size_t count = 0;
        for (const auto id : kLifecycleChangeFields)
            if (lifecycleChanged(id) && v(std::string(id) + "_phase") == phase) ++count;
        return count;
    };

    const bool twoTasks = v("X03.task_count") == "two";
    const bool interleaved = active(v("S05"));
    const bool lateLeave = active(v("S09"));
    const bool roundTrip = active(v("S04"));
    const bool externalSource = v("T01") == "external_ingress";
    const bool nativeSourceConfigured = v("G01.button") != "unknown" && v("G01.button") != "na" &&
        v("G02.press") != "unknown" && v("G02.press") != "na";
    if (sourceSlots)
        for (const auto& axis : axes)
            if (axis.role == Role::Hypothesis &&
                !(v("X03.task_count") == "one" && axis.id.substr(0, 7) == "task_b." && v(axis.id) == "na"))
            {
                if (v3) result.postconditions.push_back({axis.id, std::string(v(axis.id))});
                else unresolved("outcome_hypothesis_requires_production_observation:" + axis.id);
            }
    // The isolated message-only model page cannot observe actual display
    // scaling, desktop mode or monitor topology; none may be inferred from it.
    for (const auto id : {"V01.initial", "V03", "V05.initial"})
        if (v(id) == "unknown") unresolved(std::string("environment_state_unobserved:") + id);
    // An event vocabulary is not an executable adapter or completion oracle.
    // The executor must reject any program containing unresolved obligations
    // before performing its first step, rather than partially executing it.
    if (active(v("S01"))) unresolved("modifier_transition_production_observation_unimplemented");
    if (active(v("S02"))) unresolved("menu_appearance_and_refresh_completion_unimplemented");
    if (active(v("S03.dwell"))) unresolved("dwell_open_or_tab_switch_completion_unimplemented");
    if (active(v("S03.navigation"))) unresolved("navigation_production_event_adapter_unimplemented");
    if ((v("S03.navigation") == "scroll" && active(v("H05.scroll"))) ||
        (v("S03.navigation") == "page_flip" && active(v("H03.page_switch"))))
        unresolved("navigation_event_identity_or_order_unspecified");
    if (roundTrip) unresolved("ole_return_session_inputs_and_delivery_unimplemented");
    if (active(v("S06"))) unresolved("pending_change_boundary_and_completion_unimplemented");
    if (active(v("S07"))) unresolved("refresh_save_reload_completion_unimplemented");
    if (active(v("S08"))) unresolved("cross_page_cleanup_and_mapping_completion_unimplemented");
    if (active(v("S10"))) unresolved("sorting_capture_refresh_and_persistence_completion_unimplemented");
    if (externalSource && v("G02.press") != "unknown")
        unresolved("external_source_press_not_observed_by_host");
    const auto environmentAt = [&](std::string_view phase) {
        std::size_t count = 0;
        for (const auto id : kEnvironmentChangeFields)
            if (EnvironmentChangeActive(id, v(id)) && v(std::string(id) + "_phase") == phase) ++count;
        return count;
    };
    const auto environmentCanSchedule = [&](std::string_view phase) {
        if (lateLeave || twoTasks || environmentAt(phase) != 1) return false;
        if (phase == "after_interactions_before_release" &&
            (v("T01") != "internal_native" && v("T01") != "internal_to_ole" && !roundTrip)) return false;
        if (phase == "after_interactions_before_release" && !nativeSourceConfigured) return false;
        if ((phase == "before_source_read" || phase == "after_read_before_submit" || phase == "during_io") &&
            (v("T04.capability") == "absent" || v("T04.capability") == "present_disabled" ||
             v("T04.capability") == "probe_failure")) return false;
        if (lifecycleAt(phase)) return false;
        for (const auto id : kEnvironmentChangeFields)
            if (v(std::string(id) + "_phase") == phase && id == "V05.change" &&
                v(id) == "disconnect_then_reconnect") return false;
        return true;
    };
    const auto emitEnvironment = [&](std::string_view phase) {
        if (!environmentCanSchedule(phase)) return;
        for (const auto id : kEnvironmentChangeFields)
            if (EnvironmentChangeActive(id, v(id)) && v(std::string(id) + "_phase") == phase)
                add("environment_change", std::string(id) + "=" + std::string(v(id)) + "@" + std::string(phase));
    };
    const auto lifecycleCanSchedule = [&](std::string_view phase) {
        if (lateLeave || twoTasks || lifecycleAt(phase) != 1 || environmentAt(phase)) return false;
        if ((phase == "before_source_read" || phase == "after_read_before_submit" || phase == "during_io") &&
            (v("T04.capability") == "absent" || v("T04.capability") == "present_disabled" ||
             v("T04.capability") == "probe_failure")) return false;
        return true;
    };
    const auto lifecycle = [&](std::string_view phase) {
        if (!lifecycleCanSchedule(phase)) return;
        for (const auto id : kLifecycleChangeFields)
            if (lifecycleChanged(id) && v(std::string(id) + "_phase") == phase)
                add("lifecycle_change", std::string(id) + "=" + std::string(v(id)) + "@" + std::string(phase));
    };
    auto selectedCount = sourceSlots ? v("P02.source_actual_count") : v("P02.resolved_count");
    if (!sourceSlots && selectedCount == "many") selectedCount = v("P02.many_count");
    std::size_t exactSelectedCount = 0;
    if (!(v3 ? TransferExactSourceCountV3(selectedCount, exactSelectedCount) : TransferCount(selectedCount, exactSelectedCount)))
        unresolved(v4 ? "v4_logical_selection_count_unresolved" : "selection_payload_count_unresolved");
    if (sourceSlots)
    {
        unresolved("host_resolved_payload_and_member_observation_unimplemented");
        if (v4) unresolved("v4_source_format_and_payload_binding_unimplemented");
        else if (v3) unresolved("v3_source_format_and_payload_binding_unimplemented");
        else if (!SelectionV2HasExplicitFileSource(axes, candidate))
            unresolved("non_file_source_and_host_payload_relation_unimplemented");
        if (v("C10.capacity") != "na" && v("C10.capacity") != "no_grid")
            unresolved("capacity_relation_requires_observed_host_payload_count");
    }
    if (!sourceSlots && v("C10.capacity") != "na" && v("C10.capacity") != "no_grid" &&
        !TransferCount(selectedCount, exactSelectedCount))
        unresolved("capacity_relation_requires_exact_selection_count");
    std::size_t exactCapacity = 0, exactUsed = 0;
    if (v("C10.capacity") != "na" && v("C10.capacity") != "no_grid" &&
        (!TransferCount(v("C10.capacity"), exactCapacity) || !TransferCount(v("C10.used"), exactUsed)))
        unresolved("capacity_or_occupancy_count_unresolved");
    bool hasLifecycle = false;
    for (const auto id : kLifecycleChangeFields)
    {
        const bool changed = lifecycleChanged(id);
        const auto phase = v(std::string(id) + "_phase");
        hasLifecycle = hasLifecycle || changed;
        if (changed && (phase.empty() || phase == "na" || phase == "unspecified"))
            unresolved(std::string("lifecycle_event_phase_unspecified:") + std::string(id));
        if (!changed && phase != "na")
            unresolved(std::string("lifecycle_phase_without_change:") + std::string(id));
        if (twoTasks)
        {
            const auto bid = std::string("task_b.") + std::string(id);
            if (active(v(bid)))
                unresolved(std::string("second_task_lifecycle_event_adapter_unimplemented:") + std::string(id));
            else if (v(bid + "_phase") != "na")
                unresolved(std::string("second_task_lifecycle_phase_without_change:") + std::string(id));
        }
    }
    for (const auto phase : {"before_source_read", "after_read_before_submit", "during_io", "after_commit_before_reload"})
        if (lifecycleAt(phase) && !lifecycleCanSchedule(phase))
            unresolved(std::string("lifecycle_event_order_or_session_unspecified:") + phase);
    if (v("P05.source_reorder") == "before_drop" && v("X04.source") == "reorder")
        unresolved("source_reorder_event_identity_or_order_unspecified");
    if (twoTasks)
    {
        // A complete independent scene/payload is handed to a future
        // production adapter. These records do not prove execution.
        for (std::size_t i = 0; i < axes.size(); ++i)
            if (axes[i].id.substr(0, 7) == "task_b." &&
                (sourceSlots ? (axes[i].role == Role::Input || axes[i].role == Role::Conditional ||
                                axes[i].role == Role::Sequence) : axes[i].role != Role::Derived))
                add("configure_second_task_input", axes[i].id + "=" + candidate.values[i]);
        for (const auto id : {"task_b.source.E01", "task_b.target.E01", "task_b.T01"})
            if (v(id).empty() || v(id) == "na")
                unresolved(std::string("second_task_required_input_unspecified:") + id);
        auto secondCount = sourceSlots ? v("task_b.P02.source_actual_count") : v("task_b.P02.resolved_count");
        if (!sourceSlots && secondCount == "many") secondCount = v("task_b.P02.many_count");
        std::size_t concreteCount = 0;
        if (!(v3 ? TransferExactSourceCountV3(secondCount, concreteCount) : TransferCount(secondCount, concreteCount)))
            unresolved("second_task_selection_count_unresolved");
        if (sourceSlots)
        {
            unresolved("second_task_host_resolved_payload_and_member_observation_unimplemented");
            if (v4) unresolved("v4_second_task_source_format_and_payload_binding_unimplemented");
            else if (v3) unresolved("v3_second_task_source_format_and_payload_binding_unimplemented");
            else
            {
                const auto projection = ProjectSecondTask(axes, candidate);
                if (!SelectionV2HasExplicitFileSource(projection.axes, projection.candidate))
                    unresolved("second_task_non_file_source_and_host_payload_relation_unimplemented");
            }
        }
        unresolved("second_task_production_adapter_unimplemented");
    }
    if (v("X03.submit_order") == "simultaneous") unresolved("simultaneous_submission_order_unspecified");
    if (interleaved && v("X03.submit_order") == "b_then_a")
        unresolved("capture_and_reverse_submission_order_unimplemented");
    if (v("X03.order") == "interleaved" && !interleaved)
        unresolved("interleaved_read_and_commit_order_unspecified");
    if (v("T03.extra_protocol_bits") == "other")
        unresolved("extra_protocol_bits_not_specified");
    if (roundTrip && lateLeave) unresolved("round_trip_and_leave_session_order_unspecified");
    if (lateLeave && twoTasks) unresolved("leave_and_multi_task_session_order_unspecified");
    if (lateLeave)
        unresolved("prior_external_session_inputs_not_independently_specified");
    if (v("S09") == "leave_replace_source_old_timer" || v("S09") == "leave_new_session_leave_old_timer")
        unresolved("replacement_session_source_not_independently_specified");
    for (const auto id : kEnvironmentChangeFields)
    {
        const auto phase = v(std::string(id) + "_phase");
        const bool changed = EnvironmentChangeActive(id, v(id));
        if (changed && (phase == "na" || phase == "unspecified" || phase.empty()))
            unresolved(std::string("environment_event_phase_unspecified:") + std::string(id));
        if (!changed && phase != "na")
            unresolved(std::string("environment_phase_without_change:") + std::string(id));
        if (changed && id == "V05.change" && v(id) == "disconnect_then_reconnect")
            unresolved("disconnect_reconnect_relative_phases_unspecified");
        if (changed && (twoTasks || lateLeave))
            unresolved(std::string("environment_event_session_or_task_order_unspecified:") + std::string(id));
        if (twoTasks)
        {
            const auto bid = std::string("task_b.") + std::string(id);
            if (EnvironmentChangeActive(id, v(bid)))
                unresolved(std::string("second_task_environment_event_adapter_unimplemented:") + std::string(id));
            else if (!v(bid).empty() && v(bid + "_phase") != "na")
                unresolved(std::string("second_task_environment_phase_without_change:") + std::string(id));
        }
    }
    for (const auto phase : {"after_interactions_before_release", "before_source_read", "after_read_before_submit",
                             "during_io", "after_commit_before_reload"})
        if (environmentAt(phase) && !environmentCanSchedule(phase))
            unresolved(std::string("environment_event_order_or_session_unspecified:") + phase);
    bool hasEnvironmentChange = false;
    for (const auto id : kEnvironmentChangeFields)
        hasEnvironmentChange = hasEnvironmentChange || EnvironmentChangeActive(id, v(id));
    if (active(v("S06")) && !hasLifecycle && !hasEnvironmentChange)
        unresolved("pending_change_sequence_has_no_change");
    const bool synchronousOle = v("T04.capability") == "absent" ||
        v("T04.capability") == "present_disabled" || v("T04.capability") == "probe_failure";
    if (synchronousOle && (lifecycleAt("before_source_read") || lifecycleAt("after_read_before_submit") ||
         lifecycleAt("during_io")))
        unresolved("synchronous_interleaving_boundary_unspecified");
    if (synchronousOle && (environmentAt("before_source_read") ||
        environmentAt("after_read_before_submit") || environmentAt("during_io")))
        unresolved("synchronous_environment_interleaving_boundary_unspecified");

    // Failure injection configures only an external failure boundary. The
    // host still owns and executes the operation that encounters the fault.
    if (v("X02") != "none")
        add("configure_fault", std::string(v("X02")) + "@" + std::string(v("X02.position")));
    if (v("X02") == "none" && v("X02.position") != "na" && v("X02.position") != "no_interruption")
    {
        if (v("G03.decision") != "cancel" && v("G03.decision") != "escape" &&
            v("G03.decision") != "outside_dismiss")
            unresolved("interruption_input_not_specified");
        add("arm_cancellation", v("X02.position"));
    }
    if (active(v("S10"))) add("configure_sort_via_host", v("C01"));
    if (v("X03.callback_reentrant") == "yes") add("arm_reentrant_completion", "completion");
    if (v("X03.callback_duplicate") == "yes") add("arm_duplicate_notification", "completion");
    if (lifecycleCanSchedule("before_source_read") || environmentCanSchedule("before_source_read"))
        add("arm_source_read_gate", "selected_task");
    if (lifecycleCanSchedule("after_read_before_submit") ||
        environmentCanSchedule("after_read_before_submit") || interleaved)
        add("arm_source_read_completion_gate", interleaved ? "a" : "selected_task");
    if (lifecycleCanSchedule("during_io") || environmentCanSchedule("during_io"))
        add("arm_file_io_gate", "selected_task");
    if (active(v("S02"))) add("arm_menu_refresh", "production_menu");
    if (v("G03.decision") != "na") add("arm_decision_boundary", v("G03.decision"));
    if (twoTasks && v("X03.submit_order") == "b_then_a")
        add("submit_second_task", "b");
    if (lateLeave && v("T01") == "internal_native") add("external_enter", "prior_session_unresolved");
    if (externalSource)
        oleInput("external_enter", "G02.enter", "G02.enter_buttons");
    else if (nativeSourceConfigured)
        add("source_start", std::string(v("T01")) + ":" + std::string(v("G01.button")) +
            ":" + std::string(v("G02.press")));
    else unresolved("native_source_start_inputs_unspecified");
    if (nativeSourceConfigured && (v("T01") == "internal_native" || v("T01") == "internal_to_ole" || roundTrip))
    {
        add("move_past_drag_threshold", "source");
        add("observe_source_capture", "production");
    }
    if (roundTrip)
    {
        add("enter_ole_transport", "external");
        add("return_to_host", "same_captured_source");
    }
    if (!externalSource && v("H08.first_direction") != "unknown" && v("H08.first_direction") != "unobserved" && v("H08.first_direction") != "na")
        add("pointer_direction", v("H08.first_direction"));
    else if (!externalSource) unresolved("native_initial_direction_unspecified");
    else if (v("H08.first_direction") != "unknown" && v("H08.first_direction") != "unobserved" && v("H08.first_direction") != "na")
        unresolved("external_source_initial_direction_not_observed_by_host");
    if (v("G02.hover_first") != "na")
    {
        if (externalSource) oleInput("hover", "G02.hover_first", "G02.hover_first_buttons");
        else if (v("G02.hover_first") != "unknown") add("hover", v("G02.hover_first"));
        else unresolved("native_first_hover_modifiers_unspecified");
    }
    if (active(v("S03.dwell"))) add("dwell", v("S03.dwell"));
    if (active(v("S03.navigation"))) add("navigate", v("S03.navigation"));
    if (active(v("H03.page_switch"))) add("page_switch", v("H03.page_switch"));
    if (active(v("H05.scroll"))) add("scroll", v("H05.scroll"));
    if (active(v("P05.source_reorder"))) add("reorder_source", v("P05.source_reorder"));
    if (v("H08.later_direction") == "unknown") unresolved("later_direction_unspecified");
    else if (v("H08.later_direction") != "na") add("pointer_direction_later", v("H08.later_direction"));
    if (v("G02.hover_later") != "na")
    {
        if (externalSource) oleInput("hover_later", "G02.hover_later", "G02.hover_later_buttons");
        else if (v("G02.hover_later") != "unknown") add("hover_later", v("G02.hover_later"));
        else unresolved("native_later_hover_modifiers_unspecified");
    }
    emitEnvironment("after_interactions_before_release");
    if (active(v("S01")) && v("G02.hover_later") == "na" &&
        v("G02.press") == v("G02.hover_first") && v("G02.release") == v("G02.hover_first"))
        unresolved("modifier_change_not_described");
    // S09 is a real ordered sequence of two possible sessions; it is not a
    // request to force internal timer-cleanup business logic in the fixture.
    if (lateLeave)
    {
        add("external_leave", "schedule_production_cleanup");
        const auto variant = v("S09");
        if (variant == "leave_return_old_timer") add("external_reenter", "same_source");
        else if (variant == "leave_replace_source_old_timer") add("external_reenter", "replacement_source");
        else if (variant == "leave_submit_old_timer")
        {
            add("external_reenter", "same_source");
            if (v("G02.release") == "na") unresolved("leave_submit_requires_release_input");
            else if (externalSource) oleInput("release", "G02.release", "G02.release_buttons");
            else if (v("G02.release") != "unknown") add("release", v("G02.release"));
            else unresolved("native_release_modifiers_unspecified");
        }
        else if (variant == "leave_new_session_leave_old_timer")
        {
            add("external_reenter", "new_session");
            add("external_leave", "new_session");
        }
        else if (variant == "leave_internal_takeover_old_timer") add("start_internal_gesture", "new_session");
        else unresolved("unknown_leave_sequence");
        add("deliver_queued_timer", "old_session");
    }
    const auto termination = v("G04.termination");
    if (termination != "drop" && (v("G02.release") != "na" || v("G02.release_buttons") != "na"))
        unresolved(lateLeave ? "release_fields_session_identity_unspecified" : "release_fields_without_drop_unimplemented");
    const bool priorDrop = lateLeave && v("S09") == "leave_submit_old_timer";
    if (priorDrop && termination != "drop")
        unresolved("prior_session_drop_and_final_termination_order_unspecified");
    else if (!priorDrop && termination == "drop")
    {
        if (v("G02.release") != "na")
        {
            if (externalSource) oleInput("release", "G02.release", "G02.release_buttons");
            else if (v("G02.release") != "unknown") add("release", v("G02.release"));
            else unresolved("native_release_modifiers_unspecified");
        }
        else unresolved("drop_release_modifiers_unspecified");
    }
    else if (!priorDrop && termination == "leave")
    {
        if (externalSource || roundTrip) add("external_leave", "gesture_termination");
        else unresolved("native_leave_termination_not_modeled");
    }
    else if (!priorDrop && (termination == "escape" || termination == "outside_dismiss"))
    {
        if (externalSource) add("external_leave", std::string("source_cancel_") + std::string(termination));
        else add("deliver_direct_cancellation", termination);
    }
    else if (!priorDrop) unresolved("gesture_termination_not_described");
    if (v("G03.decision") != "na") add("observe_decision_boundary", "production");
    if (lifecycleCanSchedule("before_source_read") || environmentCanSchedule("before_source_read"))
    {
        add("await_source_read_gate_or_rejection", "selected_task");
        add("if_gate_reached", "source_read");
        lifecycle("before_source_read");
        emitEnvironment("before_source_read");
        add("release_source_read_gate", "selected_task");
        add("end_if", "source_read");
    }
    if (lifecycleCanSchedule("after_read_before_submit") || environmentCanSchedule("after_read_before_submit"))
    {
        add("await_source_read_completion_gate_or_rejection", interleaved ? "a" : "selected_task");
        add("if_gate_reached", "source_completion");
        lifecycle("after_read_before_submit");
        emitEnvironment("after_read_before_submit");
        if (!interleaved) add("deliver_source_read_completion", "selected_task");
        add("end_if", "source_completion");
    }
    if (lifecycleCanSchedule("during_io") || environmentCanSchedule("during_io"))
    {
        add("await_file_io_gate_or_rejection", "selected_task");
        add("if_gate_reached", "file_io");
        lifecycle("during_io");
        emitEnvironment("during_io");
        add("release_file_io_gate", "selected_task");
        add("end_if", "file_io");
    }
    if (twoTasks)
    {
        add("submit_task_order", v("X03.submit_order"));
        if (interleaved)
        {
            if (!lifecycleCanSchedule("after_read_before_submit"))
                add("await_source_read_completion_gate_or_rejection", "a");
            if (v("X03.submit_order") != "b_then_a") add("submit_second_task", "b");
            add("observe_completion_or_rejection", "b");
            add("refresh_before_later_completion", "b");
            add("if_gate_reached", "source_completion_a");
            add("deliver_source_read_completion", "a");
            add("end_if", "source_completion_a");
            add("observe_completion_or_rejection", "a");
        }
        else
        {
            if (v("X03.submit_order") != "b_then_a") add("submit_second_task", "b");
            add("observe_two_task_completion_order", v("X03.order"));
        }
    }
    else add("observe_completion_or_rejection", "single");
    if (lifecycleCanSchedule("after_commit_before_reload") || environmentCanSchedule("after_commit_before_reload"))
    {
        add("if_commit_succeeded", "selected_task");
        lifecycle("after_commit_before_reload");
        emitEnvironment("after_commit_before_reload");
        add("end_if", "commit_succeeded");
    }
    if (active(v("H03.after_drop"))) add("target_page_after_drop", v("H03.after_drop"));
    if (!twoTasks && v("X03.refresh") == "before_later_completion")
        unresolved("refresh_before_missing_later_completion");
    if (active(v("S08")))
    {
        add("if_commit_succeeded", "selected_task");
        add("observe_empty_source_page", "production");
        add("observe_page_cleanup_and_mapping", "production");
        add("end_if", "commit_succeeded");
    }
    if (active(v("S07")))
    {
        add("if_commit_succeeded", "selected_task");
        add("observe_committed_outputs", "production");
        add("await_file_enumeration", "production");
        add("request_refresh", "production");
        add("observe_container_rebuild", "production");
        add("request_save", "production");
        add("request_reload", "production");
        add("end_if", "commit_succeeded");
    }
    if (active(v("S10"))) add("observe_sort_and_member_order", "production");
    if (!externalSource && nativeSourceConfigured)
    {
        // The private input device's End synchronously delivers cancel,
        // capture and leave notifications to participating real host windows.
        // It is a fixture lifecycle event, separate from the product drop;
        // final assertions must observe the model after those notifications.
        add("end_pointer_session", "production_cleanup_messages");
        unresolved("native_pointer_end_failure_and_abort_order_unimplemented");
        if (v("T01") != "internal_native" || roundTrip)
            unresolved("native_pointer_end_nested_ole_order_unimplemented");
        if (twoTasks || interleaved || lateLeave)
            unresolved("native_pointer_end_multiple_sessions_order_unimplemented");
    }
    add("assert_business_results", "independent_oracle");
    return result;
}
};

inline EventProgram BuildEventProgram(const std::vector<Axis>& axes, const Candidate& candidate)
{ return TransferProgramEngine::Legacy(axes, candidate, false); }
inline EventProgram BuildEventProgramV2(const std::vector<Axis>& axes, const Candidate& candidate)
{ return TransferProgramEngine::Legacy(axes, candidate, true); }

// These local helpers do not validate the outer V3FamilyDescriptor or its
// canonical schema: the versioned caller must do so before using their output.
// In particular, neither a preview nor a local exclusion is an executable
// result or a complete V3 structural classification.
inline std::string ValidateV3TransferCandidate(const std::vector<Axis>& axes,
    const Candidate& input, Candidate& safe)
{
    if (input.values.size() != axes.size()) return "v3_transfer_candidate_shape_invalid";
    const auto lookup = BuildIndex(axes);
    if (lookup->size() != axes.size() || (input.lookup && *input.lookup != *lookup))
        return "v3_transfer_candidate_index_invalid";
    for (std::size_t i = 0; i < axes.size(); ++i)
    {
        if (axes[i].role == Role::Derived)
        { if (!input.values[i].empty()) return "v3_transfer_derived_value_invalid:" + axes[i].id; }
        else if (std::find(axes[i].values.begin(), axes[i].values.end(), input.values[i]) == axes[i].values.end())
            return "v3_transfer_candidate_value_invalid:" + axes[i].id;
    }
    safe = input;
    safe.lookup = lookup;
    return {};
}

struct V3TransferStructuralAudit
{
    std::string invalid, localExclusion;
    std::vector<std::string> unresolved;
};
class V3FamilyValidatedContext;
class V3TransferValidatedAccess final
{
    friend class V3FamilyValidatedContext;
    static V3TransferStructuralAudit Audit(const std::vector<Axis>& axes,
    const Candidate& input, bool taskBActive)
{
    V3TransferStructuralAudit result;
    Candidate candidate;
    result.invalid = ValidateV3TransferCandidate(axes, input, candidate);
    if (!result.invalid.empty()) return result;
    const auto require = [&](std::string_view id) { return candidate.lookup->contains(std::string(id)); };
    for (const auto id : {"C01", "C02", "C03", "C04", "C10.capacity", "C10.used", "C12", "C13",
        "C14.replacement", "C14.binding_state", "T01", "T02.virtual_files", "T02.virtual_completeness",
        "T02.url", "T02.url_kind", "T03", "T03.extra_protocol_bits", "T04.capability", "T04.availability",
        "V04.lua_permission", "V04.lua_change", "X02", "X02.position", "X03.task_count", "X03.order",
        "X03.submit_order", "X03.refresh", "S04", "S05", "S09"})
        if (!require(id)) { result.invalid = std::string("v3_transfer_required_axis_missing:") + id; return result; }
    if (Value(axes, candidate, "X03.task_count") != (taskBActive ? "two" : "one"))
    { result.invalid = "v3_transfer_task_family_mismatch"; return result; }
    if (taskBActive)
        for (const auto id : {"C01", "C02", "C03", "C04", "C10.capacity", "C10.used", "C12", "C13",
            "C14.replacement", "C14.binding_state", "T02.virtual_files", "T02.virtual_completeness",
            "T02.url", "T02.url_kind", "V04.lua_permission", "V04.lua_change", "X02", "X02.position"})
            if (!require(std::string("task_b.") + id))
            { result.invalid = std::string("v3_transfer_second_axis_missing:") + id; return result; }
    result.unresolved.emplace_back("v3_family_schema_and_descriptor_validation_required");
    result.unresolved.emplace_back("v3_selection_endpoint_and_cross_task_structure_unimplemented");
    result.unresolved.emplace_back("v3_transfer_audit_is_not_a_product_acceptance_or_execution");
    if (const auto reason = TransferExclusion(axes, candidate); !reason.empty())
        result.localExclusion = std::string(reason);
    else if (taskBActive)
        if (const auto reason = TransferLocalExclusionPrefixed(axes, candidate, "task_b."); !reason.empty())
            result.localExclusion = std::string(reason);
    return result;
}

static EventProgram Program(const std::vector<Axis>& axes, const Candidate& input)
{
    Candidate candidate;
    if (const auto reason = ValidateV3TransferCandidate(axes, input, candidate); !reason.empty())
        return EventProgram{{}, {reason}, {}};
    auto result = TransferProgramEngine::Build(axes, candidate, true, true);
    result.unresolved.emplace_back("v3_family_schema_and_candidate_validation_required");
    result.unresolved.emplace_back("v3_selection_cross_task_structure_and_event_graph_unimplemented");
    result.unresolved.emplace_back("v3_production_dispatch_completion_and_business_oracle_unimplemented");
    return result;
}
};

// Only the V4 authority may invoke this engine. It builds a symbolic program
// from that authority's already reconstructed schema and candidate. V4's
// logical selection count is never used as a file/virtual payload count.
class V4TransferValidatedAccess final
{
    friend class V4FamilyValidatedContext;
    static EventProgram Program(const std::vector<Axis>& axes, const Candidate& candidate)
    {
        auto result = TransferProgramEngine::Build(axes, candidate, true, true, true);
        std::vector<EventStep> sourceInputs;
        std::vector<EventStep> preparation;
        std::vector<std::string> preparationBaseline;
        std::size_t preparationCount = 0;
        const auto oneTask = Value(axes, candidate, "X03.task_count") == "one";
        const auto mode = Value(axes, candidate, "V4.selection_prepare.mode");
        for (std::size_t i = 0; i < axes.size(); ++i)
        {
            const auto& axis = axes[i];
            const bool second = axis.id.substr(0, 7) == "task_b.";
            const std::string_view local = second ? std::string_view(axis.id).substr(7) : std::string_view(axis.id);
            if (second && Value(axes, candidate, "X03.task_count") == "one") continue;
            if ((local.starts_with("P05.member") && std::string_view(candidate.values[i]).starts_with("position_")) ||
                std::string_view(candidate.values[i]).starts_with("int32_hex_"))
                result.unresolved.push_back("v4_exact_position_or_dimension_observation_unimplemented:" + axis.id + "=" + candidate.values[i]);
            if (!local.starts_with("V4.")) continue;
            if (local.starts_with("V4.selection_prepare."))
            {
                // Preparation inputs are real pointer actions, not provider
                // configuration. The second task's placement relative to A
                // remains unknown until a cross-task schedule is defined.
                if (second || !oneTask)
                {
                    result.unresolved.push_back("v4_selection_prepare_cross_task_order_unimplemented:" + axis.id + "=" + candidate.values[i]);
                    continue;
                }
                if (local.starts_with("V4.selection_prepare.baseline_"))
                    preparationBaseline.push_back(axis.id + "=" + candidate.values[i]);
                constexpr std::string_view prefix = "V4.selection_prepare.step_";
                constexpr std::string_view suffix = ".action";
                if (local.starts_with(prefix) && local.ends_with(suffix))
                {
                    const auto base = axis.id.substr(0, axis.id.size() - suffix.size());
                    const auto action = Value(axes, candidate, axis.id);
                    const auto session = Value(axes, candidate, base + ".session");
                    const auto button = Value(axes, candidate, base + ".button");
                    const auto modifiers = Value(axes, candidate, base + ".modifiers");
                    const auto member = Value(axes, candidate, base + ".member");
                    const auto x = Value(axes, candidate, base + ".x");
                    const auto y = Value(axes, candidate, base + ".y");
                    const bool known = action == "pointer_down" || action == "pointer_move" ||
                        action == "pointer_up" || action == "session_end";
                    const auto parameter = base + ";session=" + std::string(session) + ";button=" + std::string(button) +
                        ";modifiers=" + std::string(modifiers) + ";member=" + std::string(member) +
                        ";x=" + std::string(x) + ";y=" + std::string(y);
                    preparation.push_back({known ? "selection_prepare_" + std::string(action) : "selection_prepare_unknown", parameter});
                    if (!known || session.empty() || session == "na" || session == "unknown_unresolved" ||
                        button.empty() || button == "unknown_unresolved" || modifiers.empty() || modifiers == "unknown_unresolved" ||
                        member.empty() || member == "unknown_unresolved" || x.empty() || x == "unknown_unresolved" ||
                        y.empty() || y == "unknown_unresolved")
                        result.unresolved.push_back("v4_selection_prepare_step_values_unavailable:" + base);
                    ++preparationCount;
                }
                continue;
            }
            if (axis.role == Role::Input || axis.role == Role::Conditional || axis.role == Role::Sequence)
            {
                sourceInputs.push_back({"configure_v4_source_input", axis.id + "=" + candidate.values[i]});
                result.unresolved.push_back("v4_source_input_construction_unimplemented:" + axis.id);
            }
        }
        if (oneTask)
        {
            const auto declared = Value(axes, candidate, "V4.selection_prepare.step_count");
            const auto baselineAnchor = Value(axes, candidate, "V4.selection_prepare.baseline_anchor");
            const auto baselineOutside = Value(axes, candidate, "V4.selection_prepare.baseline_outside_members");
            if (declared != "count_" + std::to_string(preparationCount) || mode.empty() || mode == "unknown_unresolved" ||
                mode == "other_unresolved" || (mode == "none" && preparationCount) || (mode != "none" && !preparationCount))
                result.unresolved.emplace_back("v4_selection_prepare_mode_or_count_unavailable");
            if (baselineAnchor.empty() || baselineOutside.empty())
                result.unresolved.emplace_back("v4_selection_prepare_baseline_axes_unavailable");
            for (const auto& baseline : preparationBaseline)
                result.unresolved.push_back("v4_selection_prepare_baseline_observation_unimplemented:" + baseline);
            if (preparationCount) result.unresolved.emplace_back("v4_selection_prepare_session_and_observation_unimplemented");
            for (auto& step : preparation) sourceInputs.push_back(std::move(step));
        }
        for (auto& step : result.steps)
        {
            if (step.kind == "configure_second_task_input" &&
                step.parameter.starts_with("task_b.V4.selection_prepare.")) continue;
            sourceInputs.push_back(std::move(step));
        }
        result.steps = std::move(sourceInputs);
        result.unresolved.emplace_back("v4_logical_selection_and_physical_virtual_payload_relation_unimplemented");
        result.unresolved.emplace_back("v4_format_applicability_and_event_graph_unimplemented");
        result.unresolved.emplace_back("v4_production_dispatch_completion_and_business_oracle_unimplemented");
        return result;
    }
};
