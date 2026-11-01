// P/R/G/H domains from the historical 107-field inventory.  These definitions
// describe inputs and observations; they do not call production acceptance
// code or classify a combination as a passing/rejecting product behavior.
//
// Fields with overlapping historical values are split below.  Axis IDs retain
// the historical prefix; derived axes must be computed from constructed data,
// never multiplied as freely selectable inputs.  Every ``na`` is conditional
// on a structural absence, not on whether the product supports an operation.
// The parent generator must implement and count these structural constraints:
//
// * A resolved zero implies P01.primary=na and P03.kind_* = absent; nonzero
//   resolved selections require a primary member, and their count must accommodate all
//   distinct selected object kinds and any duplicate (P01/P02/P03). The kind
//   set describes selected identities, not alternative formats offered by one
//   IDataObject; that independent format set belongs to T02. Hover metadata
//   (P02.count) is independent from final payload count (P02.resolved_count).
// * P03.kind_* denotes the complete set of present kinds, including P01.primary
//   and P01.second; P03.additional_kind_* applies only beyond those members.
//   Repeat identity is independent of kind. P03.support is derived from the
//   selected identities and destination, not a generation filter.
//   P03.widget_types applies only when at least one widget is selected.
// * P04 logical-source/group and physical distributions describe different
//   relationships and can both vary for the same selection. P06 class flags
//   describe all selected entries; the primary class belongs to P01.primary.
// * R01, R02 and R03 are derived from actual objects, paths and volumes; each
//   constituent property may overlap the others. R04.target_identity and
//   R04.name_relation are na when R04.target_exists is absent or na; a case
//   alias may denote either the same identity or a distinct one.
// * A release not delivered by the event sequence has G02.release=na; a later
//   hover that did not occur has G02.hover_later=na. G03=na means no menu or
//   confirmation occurred; left-button drags may also reach confirmation.
// * H02 relationships derive from the actual monitor set and source/target
//   identities (source.E01, target.E01, V05/V06); they are not interchangeable
//   with sign, first/last position, DPI, or connection events.
// * H03 edge switching and post-drop deletion are separate events; H03.page is
//   the identity at the drop. H06 changes refer to that same captured identity.
// * H04 anchor applies to a grid; requested span derives from the selected
//   object's real geometry. H04.fit and H07.capacity_relation derive from
//   H04 geometry, P02 selection and H07 occupancy; a full/insufficient page
//   is a test input and must not be excluded as a rejected operation.
// * H05 insertion factors apply to a member/label insertion target; source
//   relation, filtering and scrolling are independent when structurally
//   meaningful. H09 member=nonfirst requires a multi-item selection.
// * H08 later_direction=na means no later direction event; actual directions
//   must be instantiated as coordinates, not used only in the case name.
//
// Ordered members have separate input descriptions. The first two keep their
// historical names; later members use generated memberN axes. The default
// finite schema has 129 slots. A different bound only builds a schema; it is
// not the full product domain, an executable adapter, or coverage evidence. Set flags
// and mixed summaries are derived from those members rather than extra inputs.
// The metadata is not itself a scene adapter or an assertion: a missing
// constructor, event sequence or per-member oracle must still report the
// corresponding candidate unimplemented.
// Until those constraints and each input constructor/independent assertion are
// wired, the corresponding combinations must be reported unimplemented. These
// declarations alone are not execution or coverage evidence.
enum class SelectionSchemaVersion { legacy_v1, source_members_v2 };

inline void AppendSelectionAxesImpl(std::vector<Axis>& axes, std::size_t memberLimit,
    SelectionSchemaVersion version, std::size_t ownReferences, std::size_t taskAReferences,
    bool includeTaskAReferences)
{
    // The historical first and second member axes are always present. This
    // materialization limit is an implementation bound, not a product limit.
    if (memberLimit < 2 || memberLimit + std::size_t{1} == 0)
        throw std::invalid_argument("selection_schema_member_limit_unavailable");
    if (version != SelectionSchemaVersion::legacy_v1 && version != SelectionSchemaVersion::source_members_v2)
        throw std::invalid_argument("selection_schema_version_invalid");
    const auto aboveLimit = "above_" + std::to_string(memberLimit) + "_unresolved";
    const auto appendObservedCount = [&](std::string id, Role role, std::string_view label) {
        Axis axis{std::move(id), role,
            {"na", "unknown", aboveLimit, "unspecified_unresolved", "zero", "one", "two"},
            std::string(label)};
        for (std::size_t count = 3; count <= memberLimit; ++count)
            axis.values.push_back("count_" + std::to_string(count));
        axes.push_back(std::move(axis));
    };
    if (version == SelectionSchemaVersion::source_members_v2)
        appendObservedCount("P02.source_actual_count", Role::Input,
            "Actual source payload cardinality independent of hover observation");
    // Both versions place count selectors before member axes. V1 uses the
    // resolved-count hypothesis for slots; v2 uses the independent source
    // count. Neither version constructs or proves a production result here.
    Axis hoverCount{"P02.count", Role::Input,
        {"na", "unknown", "zero", "one", "two", aboveLimit, "unspecified_unresolved"},
        "Source-offered exact hover metadata count, not host summary"};
    for (std::size_t count = 3; count <= memberLimit; ++count)
        hoverCount.values.push_back("count_" + std::to_string(count));
    axes.push_back(std::move(hoverCount));
    AddAxis(axes, "P02.resolved_count", version == SelectionSchemaVersion::source_members_v2 ? Role::Hypothesis : Role::Sequence,
        {"na", "still_unknown", "zero", "one", "two", "many"}, "Final payload count class independent of hover metadata");
    Axis manyCount{"P02.many_count", version == SelectionSchemaVersion::source_members_v2 ? Role::Hypothesis : Role::Conditional,
        {"na", aboveLimit, "unspecified_unresolved"}, "Concrete bounded multi-selection count"};
    for (std::size_t count = 3; count <= memberLimit; ++count)
        manyCount.values.push_back("count_" + std::to_string(count));
    axes.push_back(std::move(manyCount));
    // The final member vector does not describe a partial hover payload.
    // Beyond the explicit known-prefix counts, and any content change, the
    // individual members still require a future phase-specific constructor.
    AddAxis(axes, "P02.hover_known_prefix_count", Role::Conditional,
        {"na", "zero", "one", "two", "unresolved"}, "Known hover prefix when total selection count is unknown");
    AddAxis(axes, "P02.hover_prefix_content", Role::Sequence,
        {"na", "same_as_final", "different_unresolved", "unresolved"}, "Known hover members relative to final payload");
    AddAxis(axes, "P02.later_members_change", Role::Sequence,
        {"na", "unchanged", "changed_unresolved", "unresolved"}, "Member changes after the unknown hover");
    // Source payload cardinality and offered hover metadata are inputs. The
    // host summary and preview request are observations of production output,
    // and can differ from source cardinality when data has not been read yet.
    // Nonrepresented exact cardinalities require a later constructor.
    if (version == SelectionSchemaVersion::legacy_v1)
        appendObservedCount("P02.source_actual_count", Role::Input,
            "Actual source payload cardinality independent of hover observation");
    appendObservedCount("P02.host_summary_count", Role::Derived,
        "Host-reported count observed after production data probing");
    appendObservedCount("P02.preview_requested_count", Role::Derived,
        "Host external preview request count, not a rendered-item assertion");
    AddAxis(axes, "P01.primary", Role::Conditional,
        {"na", "unknown", "file", "directory", "lnk", "url_file", "app_entry", "namespace_item", "widget",
         "collection_label", "file_source_label", "virtual_file", "image", "text", "url"},
        "Primary captured object kind");
    AddAxis(axes, "P01.second", Role::Conditional,
        {"na", "unknown", "file", "directory", "lnk", "url_file", "app_entry", "namespace_item", "widget",
         "collection_label", "file_source_label", "virtual_file", "image", "text", "url"},
        "Second captured object kind");
    AddAxis(axes, "P02.capacity_relation", Role::Derived,
        {"na", "unknown", "below", "equal", "above"}, "Selection count relative to target capacity");

    // P03 is a set of kinds rather than a single mutually exclusive category:
    // file+directory+shortcut and other mixtures must remain expressible.
    for (const auto kind : {"file", "directory", "lnk", "url_file", "app_entry", "namespace_item", "widget",
                            "collection_label", "file_source_label", "virtual_file", "image", "text", "url"})
    {
        AddAxis(axes, std::string("P03.kind_") + kind, Role::Derived,
            {"unknown", "absent", "present"}, "Selection includes this object kind");
        AddAxis(axes, std::string("P03.additional_kind_") + kind, Role::Derived,
            {"na", "unknown", "absent", "present"}, "Members beyond the first two include this kind");
    }
    for (const auto member : {"member0", "member1"})
        AddAxis(axes, std::string("P03.") + member + "_widget_type", Role::Conditional,
            {"na", "collection", "desktop_files", "mapping", "collection_group", "file_group", "lua", "guide", "other", "unknown"},
            "Selected member widget type");
    AddAxis(axes, "P03.widget_types", Role::Derived,
        {"na", "unknown", "one_type", "multiple_types"}, "Selected widget type diversity");
    AddAxis(axes, "P03.duplicate_identity", Role::Derived,
        {"na", "unknown", "absent", "present"}, "Repeated selection identity");
    AddAxis(axes, "P03.member1_identity_ref", Role::Conditional,
        {"na", "unknown", "unique", "member_0"}, "Second member identity relative to the first");
    AddAxis(axes, "P03.support", Role::Derived,
        {"na", "all_supported", "some_unsupported", "none_supported", "unknown"}, "Support of the full selection at this target");
    AddAxis(axes, "P04.logical_source_count", Role::Derived,
        {"na", "one", "multiple", "unknown"}, "Logical source count of captured members");
    AddAxis(axes, "P04.same_group_different_children", Role::Derived,
        {"na", "absent", "present", "unknown"}, "Selection spans children of the same group");
    AddAxis(axes, "P04.cross_group", Role::Derived,
        {"na", "absent", "present", "unknown"}, "Selection spans groups");
    AddAxis(axes, "P04.physical_distribution", Role::Derived,
        {"na", "same_directory", "different_directories", "unknown"}, "Physical distribution of captured members");
    AddAxis(axes, "P04.second_source_relation", Role::Conditional,
        {"na", "same_container", "different_child_same_group", "different_group", "other_source", "unknown"},
        "Second member source relative to the first");
    AddAxis(axes, "P04.additional_sources", Role::Derived,
        {"na", "same_as_first", "different", "mixed", "unknown"}, "Source summary of members beyond the first two");
    AddAxis(axes, "P05.capture_order", Role::Derived,
        {"na", "visible_order", "reverse_visible_order", "other_explicit_order", "unknown"}, "Captured order relative to source order");
    AddAxis(axes, "P05.name_order_relation", Role::Derived,
        {"na", "same", "different", "unknown"}, "Captured order relative to name order");
    AddAxis(axes, "P05.source_reorder", Role::Sequence,
        {"na", "unchanged", "before_drop"}, "Source reorder after capture");
    AddAxis(axes, "P06.entry_class", Role::Conditional,
        {"na", "unknown", "document", "image_media", "program", "application_shortcut", "directory_shortcut",
         "file_shortcut", "unclassified"}, "Primary file or entry semantic class");
    AddAxis(axes, "P06.second_entry_class", Role::Conditional,
        {"na", "unknown", "document", "image_media", "program", "application_shortcut", "directory_shortcut",
         "file_shortcut", "unclassified"}, "Second file or entry semantic class");
    for (const auto category : {"document", "image_media", "program", "application_shortcut",
                                "directory_shortcut", "file_shortcut", "unclassified"})
        AddAxis(axes, std::string("P06.class_") + category, Role::Derived,
            {"unknown", "absent", "present"}, "Selection includes this entry class");
    AddAxis(axes, "P06.primary_special_extension", Role::Conditional,
        {"na", "unknown", "absent", "present"}, "Primary entry has an extension used by a rule");
    AddAxis(axes, "P06.second_special_extension", Role::Conditional,
        {"na", "unknown", "absent", "present"}, "Second entry has an extension used by a rule");
    AddAxis(axes, "P06.additional_special_extension", Role::Derived,
        {"na", "absent", "present", "unknown"}, "Later entry extension summary; requires member vector");

    AddAxis(axes, "R01.container_identity", Role::Derived,
        {"na", "same_instance", "different_instance", "mixed"}, "Source and target container identity");
    AddAxis(axes, "R01.container_type", Role::Derived,
        {"na", "same_type", "different_type", "mixed"}, "Source and target container types");
    AddAxis(axes, "R01.group_relation", Role::Derived,
        {"na", "same_group_same_child", "same_group_different_children", "different_groups", "ungrouped", "mixed"},
        "Source and target group relationship");
    AddAxis(axes, "R01.dragged_object_target", Role::Derived,
        {"na", "same_object", "different_object", "mixed"}, "Whether the dragged object is its target");
    AddAxis(axes, "R02.directory_relation", Role::Derived,
        {"na", "same_directory", "different_directory", "mixed", "unknown"}, "Resolved physical parent relationship");
    AddAxis(axes, "R02.target_relation", Role::Derived,
        {"na", "same_as_source", "descendant_of_source", "unrelated", "mixed", "unknown"}, "Resolved target relative to source");
    AddAxis(axes, "R02.shortcut_final_relation", Role::Derived,
        {"na", "same_as_source", "different", "mixed", "unknown"}, "Shortcut target after resolution");
    AddAxis(axes, "R02.path_available", Role::Derived,
        {"na", "available", "unavailable", "mixed"}, "Availability of physical paths");
    AddAxis(axes, "R03.volume_relation", Role::Derived,
        {"na", "same_volume", "different_volume", "mixed", "unknown"}, "Actual source and target volume relationship");
    AddAxis(axes, "R03.network_location", Role::Derived,
        {"na", "local", "network", "mixed", "unknown"}, "Whether actual storage is a network location");
    for (const auto member : {"member0", "member1"})
    {
        AddAxis(axes, std::string("R01.") + member + "_container_identity", Role::Derived,
            {"na", "same_instance", "different_instance", "unknown"}, "Member source container relative to target");
        AddAxis(axes, std::string("R01.") + member + "_container_type", Role::Derived,
            {"na", "same_type", "different_type", "unknown"}, "Member source type relative to target");
        AddAxis(axes, std::string("R01.") + member + "_group_relation", Role::Derived,
            {"na", "same_group_same_child", "same_group_different_children", "different_groups", "ungrouped", "unknown"},
            "Member source group relative to target");
        AddAxis(axes, std::string("R01.") + member + "_target_identity", Role::Derived,
            {"na", "same_object", "different_object", "unknown"}, "Member identity relative to target object");
        AddAxis(axes, std::string("R02.") + member + "_directory_relation", Role::Derived,
            {"na", "same_directory", "different_directory", "unknown"}, "Member parent directory relative to target");
        AddAxis(axes, std::string("R02.") + member + "_target_relation", Role::Derived,
            {"na", "same_as_source", "descendant_of_source", "unrelated", "unknown"}, "Target relative to this member");
        AddAxis(axes, std::string("R02.") + member + "_shortcut_final_relation", Role::Derived,
            {"na", "same_as_source", "different", "unknown"}, "Resolved shortcut relation for this member");
        AddAxis(axes, std::string("R02.") + member + "_path_available", Role::Derived,
            {"na", "available", "unavailable"}, "Member path availability");
        AddAxis(axes, std::string("R03.") + member + "_volume_relation", Role::Derived,
            {"na", "same_volume", "different_volume", "unknown"}, "Member volume relative to target volume");
        AddAxis(axes, std::string("R03.") + member + "_network_location", Role::Derived,
            {"na", "local", "network", "unknown"}, "Whether member storage is networked");
    }
    AddAxis(axes, "R04.target_exists", Role::Derived,
        {"na", "absent", "present", "mixed", "unknown"}, "Summary of requested target paths");
    AddAxis(axes, "R04.target_identity", Role::Derived,
        {"na", "same_identity", "different_identity", "mixed", "unknown"}, "Summary of source and target identities");
    AddAxis(axes, "R04.name_relation", Role::Derived,
        {"na", "same_spelling", "case_alias", "different", "mixed", "unknown"}, "Summary of existing target names");
    AddAxis(axes, "R04.queued_reservation", Role::Derived,
        {"na", "absent", "present", "mixed", "unknown"}, "Summary of queued name reservations");
    AddAxis(axes, "R04.concurrent_creation", Role::Derived,
        {"na", "none", "before_commit", "after_commit_started", "mixed", "unknown"}, "Summary of concurrent target-name creation");
    for (const auto member : {"member0", "member1"})
    {
        AddAxis(axes, std::string("R04.") + member + "_target_exists", Role::Conditional,
            {"na", "unknown", "absent", "present"}, "Target conflict for this member");
        AddAxis(axes, std::string("R04.") + member + "_target_identity", Role::Conditional,
            {"na", "same_identity", "different_identity", "unknown"}, "Existing target identity for this member");
        AddAxis(axes, std::string("R04.") + member + "_name_relation", Role::Conditional,
            {"na", "same_spelling", "case_alias", "different", "unknown"}, "Existing target name for this member");
        AddAxis(axes, std::string("R04.") + member + "_queued_reservation", Role::Conditional,
            {"na", "absent", "present"}, "Queued name reservation for this member");
        AddAxis(axes, std::string("R04.") + member + "_concurrent_creation", Role::Sequence,
            {"na", "none", "before_commit", "after_commit_started"}, "Concurrent name creation for this member");
    }
    AddAxis(axes, "R04.additional_target_conflicts", Role::Derived,
        {"na", "none", "same", "different", "mixed", "unknown"}, "Conflict summary of later members");

    // A reference to an earlier member expresses equality without inventing a
    // finite global identifier. Separate references describe object identity,
    // source/container identity, group identity and incoming-name collisions.
    const auto appendReference = [&](std::string id, std::size_t index, std::string_view label, bool unresolved) {
        Axis reference{std::move(id), Role::Conditional, {}, std::string(label)};
        reference.values = unresolved ? std::vector<std::string>{"na", "unresolved", "unknown", "unique"}
                                      : std::vector<std::string>{"na", "unknown", "unique"};
        for (std::size_t prior = 0; prior < index; ++prior)
            reference.values.push_back("member_" + std::to_string(prior));
        axes.push_back(std::move(reference));
    };
    appendReference("P04.member1_source_ref", 1, "Second member source identity", false);
    appendReference("P04.member1_group_ref", 1, "Second member group identity", false);
    appendReference("R04.member1_incoming_name_ref", 1, "Second incoming name identity", false);
    AddAxis(axes, "R04.member1_incoming_name_spelling", Role::Conditional,
        {"na", "same_spelling", "case_alias", "unknown"}, "Second incoming name spelling relative to an earlier name");
    // Target directory is a node in its own right, even when it is not the
    // parent directory of a selected member. The isolated root is an anchor
    // under the task's temporary directory, never an asserted volume root.
    // Missing intermediate nodes and graph inconsistencies remain unimplemented.
    AddAxis(axes, "R02.target_directory_identity", Role::Conditional,
        {"na", "unknown", "unresolved", "unique", "isolated_root"},
        "Target directory identity relative to the isolated fixture root");
    Axis targetParent{"R02.target_directory_parent_ref", Role::Conditional,
        {"na", "unknown", "unresolved", "outside_model_unresolved", "isolated_root", "isolated_root_parent", "target_directory"},
        "Immediate parent node of the target directory"};
    const std::vector<std::string> endpointRefs{"desktop_directory", "source_mapping_directory", "target_mapping_directory"};
    for (const auto& ref : endpointRefs) targetParent.values.push_back(ref);
    const auto addCrossTaskDirectoryRefs = [taskAReferences, includeTaskAReferences](Axis& axis) {
        if (!includeTaskAReferences) return;
        for (const auto ref : {"task_a_isolated_root", "task_a_isolated_root_parent", "task_a_target_directory", "task_a_desktop_directory", "task_a_source_mapping_directory", "task_a_target_mapping_directory"})
            axis.values.emplace_back(ref);
        for (std::size_t other = 0; other < taskAReferences; ++other)
        {
            axis.values.push_back("task_a_member_" + std::to_string(other));
            axis.values.push_back("task_a_member_object_" + std::to_string(other));
        }
    };
    // The target can be identified through a later endpoint; graph resolution
    // is order-independent. Cross-task references remain unconstructed until
    // a shared two-task graph is supplied.
    for (const auto& ref : endpointRefs) axes[axes.size() - 1].values.push_back(ref);
    addCrossTaskDirectoryRefs(axes[axes.size() - 1]);
    addCrossTaskDirectoryRefs(targetParent);
    for (std::size_t other = 0; other < ownReferences; ++other)
    {
        targetParent.values.push_back("member_" + std::to_string(other));
        targetParent.values.push_back("member_object_" + std::to_string(other));
    }
    axes.push_back(std::move(targetParent));
    // Explicit graph nodes for the host desktop and both mapping endpoints.
    // Each may alias another declared node or name a distinct node. A missing
    // mapping is na; unknown topology, arbitrary intermediates, physical
    // volumes and relations to a different task are not silently invented.
    for (const auto& endpoint : endpointRefs)
    {
        Axis identity{"R02." + endpoint + "_identity", Role::Conditional,
            {"na", "unknown", "unresolved", "outside_model_unresolved", "unique", "isolated_root",
             "target_directory", "desktop_directory", "source_mapping_directory", "target_mapping_directory",
             "task_a_directory_unresolved"}, "Endpoint directory identity in the isolated graph"};
        Axis parent{"R02." + endpoint + "_parent_ref", Role::Conditional,
            {"na", "unknown", "unresolved", "outside_model_unresolved", "isolated_root", "isolated_root_parent",
             "target_directory", "desktop_directory", "source_mapping_directory", "target_mapping_directory",
             "task_a_directory_unresolved"}, "Immediate parent of endpoint directory"};
        addCrossTaskDirectoryRefs(identity);
        addCrossTaskDirectoryRefs(parent);
        for (std::size_t other = 0; other < ownReferences; ++other)
        {
            identity.values.push_back("member_" + std::to_string(other));
            identity.values.push_back("member_object_" + std::to_string(other));
            parent.values.push_back("member_" + std::to_string(other));
            parent.values.push_back("member_object_" + std::to_string(other));
        }
        axes.push_back(std::move(identity));
        axes.push_back(std::move(parent));
    }
    for (std::size_t index = 0; index < memberLimit; ++index)
    {
        const auto member = std::string("member") + std::to_string(index);
        // In task B this records the selected object's identity across tasks,
        // independently of the within-B P03 identity refs. `unique` means
        // different from every declared A member; earlier B duplicates still
        // use their within-task identity refs. A's axis is inapplicable unless
        // a future joint source explicitly assigns it a meaning. Unknown or
        // unimplemented identity is retained, not excluded.
        Axis crossTaskObject{"P03." + member + "_cross_task_identity", Role::Conditional,
            {"na", "unknown", "unresolved", "unique"}, "Selected member identity relative to task A"};
        for (std::size_t other = 0; includeTaskAReferences && other < taskAReferences; ++other)
            crossTaskObject.values.push_back("task_a_member_" + std::to_string(other));
        axes.push_back(std::move(crossTaskObject));
        const auto appendPosition = [&](std::string id, std::string_view label) {
            Axis position{std::move(id), Role::Conditional,
                index >= 2 ? std::vector<std::string>{"na", "unresolved", "unknown"}
                           : std::vector<std::string>{"na", "unknown"}, std::string(label)};
            for (std::size_t value = 0; value <= 128; ++value)
                position.values.push_back("position_" + std::to_string(value));
            position.values.push_back("after_position_128");
            axes.push_back(std::move(position));
        };
        appendPosition("P05." + member + "_visible_index", "Member index in its source visible order");
        appendPosition("P05." + member + "_storage_index", "Member index in its source complete storage order");
        if (index >= 2)
        {
            AddAxis(axes, "P01." + member + "_kind", Role::Conditional,
                {"na", "unresolved", "unknown", "file", "directory", "lnk", "url_file", "app_entry", "namespace_item",
                 "widget", "collection_label", "file_source_label", "virtual_file", "image", "text", "url"}, "Ordered member kind");
            AddAxis(axes, "P03." + member + "_widget_type", Role::Conditional,
                {"na", "unresolved", "unknown", "collection", "desktop_files", "mapping", "collection_group", "file_group", "lua", "guide", "other"},
                "Ordered member widget type");
            appendReference("P03." + member + "_identity_ref", index, "Ordered member object identity", true);
            AddAxis(axes, "P04." + member + "_source_relation", Role::Conditional,
                {"na", "unresolved", "unknown", "same_container", "different_child_same_group", "different_group", "other_source"},
                "Member source relative to primary source");
            appendReference("P04." + member + "_source_ref", index, "Member source/container identity", true);
            appendReference("P04." + member + "_group_ref", index, "Member source group identity", true);
            AddAxis(axes, "P06." + member + "_entry_class", Role::Conditional,
                {"na", "unresolved", "unknown", "document", "image_media", "program", "application_shortcut", "directory_shortcut", "file_shortcut", "unclassified"},
                "Ordered member entry class");
            AddAxis(axes, "P06." + member + "_special_extension", Role::Conditional,
                {"na", "unresolved", "unknown", "absent", "present"}, "Ordered member extension used by a rule");
            AddAxis(axes, "R04." + member + "_target_exists", Role::Conditional,
                {"na", "unresolved", "unknown", "absent", "present"}, "Existing target for this member");
            AddAxis(axes, "R04." + member + "_target_identity", Role::Conditional,
                {"na", "unresolved", "unknown", "same_identity", "different_identity"}, "Target identity for this member");
            AddAxis(axes, "R04." + member + "_name_relation", Role::Conditional,
                {"na", "unresolved", "unknown", "same_spelling", "case_alias", "different"}, "Target name for this member");
            AddAxis(axes, "R04." + member + "_queued_reservation", Role::Conditional,
                {"na", "unresolved", "unknown", "absent", "present"}, "Queued target name for this member");
            AddAxis(axes, "R04." + member + "_concurrent_creation", Role::Sequence,
                {"na", "unresolved", "unknown", "none", "before_commit", "after_commit_started"}, "Concurrent target creation for this member");
            appendReference("R04." + member + "_incoming_name_ref", index, "Incoming name identity", true);
            AddAxis(axes, "R04." + member + "_incoming_name_spelling", Role::Conditional,
                {"na", "unresolved", "unknown", "same_spelling", "case_alias"}, "Spelling relative to an earlier incoming name");
            AddAxis(axes, "R01." + member + "_container_identity", Role::Derived,
                {"na", "same_instance", "different_instance", "unknown"}, "Member source container relative to target");
            AddAxis(axes, "R01." + member + "_container_type", Role::Derived,
                {"na", "same_type", "different_type", "unknown"}, "Member source type relative to target");
            AddAxis(axes, "R01." + member + "_group_relation", Role::Derived,
                {"na", "same_group_same_child", "same_group_different_children", "different_groups", "ungrouped", "unknown"},
                "Member source group relative to target");
            AddAxis(axes, "R01." + member + "_target_identity", Role::Derived,
                {"na", "same_object", "different_object", "unknown"}, "Member identity relative to target object");
            AddAxis(axes, "R02." + member + "_directory_relation", Role::Derived,
                {"na", "same_directory", "different_directory", "unknown"}, "Member parent directory relative to target");
            AddAxis(axes, "R02." + member + "_target_relation", Role::Derived,
                {"na", "same_as_source", "descendant_of_source", "unrelated", "unknown"}, "Target relative to this member");
            AddAxis(axes, "R02." + member + "_shortcut_final_relation", Role::Derived,
                {"na", "same_as_source", "different", "unknown"}, "Resolved shortcut relation for this member");
            AddAxis(axes, "R02." + member + "_path_available", Role::Derived,
                {"na", "available", "unavailable"}, "Member path availability");
            AddAxis(axes, "R03." + member + "_volume_relation", Role::Derived,
                {"na", "same_volume", "different_volume", "unknown"}, "Member volume relative to target volume");
            AddAxis(axes, "R03." + member + "_network_location", Role::Derived,
                {"na", "local", "network", "unknown"}, "Whether member storage is networked");
        }
        const auto layout = [&](std::string id, std::initializer_list<std::string_view> values, std::string_view label) {
            AddAxis(axes, std::move(id), Role::Conditional, values, label);
            // Unknown is a genuine environment input. Unresolved marks a
            // property or member slot not yet concretely constructed.
            if (index >= 2) axes.back().values.emplace_back("unresolved");
        };
        // These describe the selected object itself, independently of the
        // source and destination container/page dimensions. Construction of
        // each configuration and its post-drop normalization is still pending.
        const auto selectedLayout = [&](std::string id, std::initializer_list<std::string_view> values, std::string_view label) {
            layout(std::move(id), values, label);
            if (index < 2) axes.back().values.emplace_back("unresolved");
        };
        selectedLayout("P01." + member + "_span_columns", {"na", "unknown", "one", "two", "three", "four", "eight"},
            "Selected member configured column span");
        selectedLayout("P01." + member + "_span_rows", {"na", "unknown", "one", "two", "three", "four", "eight"},
            "Selected member configured row span");
        selectedLayout("P01." + member + "_large_icon_config", {"na", "unknown", "none", "configured_active", "configured_unadjusted"},
            "Selected member desktop large icon configuration");
        selectedLayout("P01." + member + "_large_icon_shape", {"na", "unknown", "rounded_rectangle", "rounded_square", "circle", "flag", "diamond", "hexagon"},
            "Selected member large icon shape");
        // References describe actual shared identities, not just whether
        // each member happens to match the target. An earlier-member alias
        // preserves arbitrary equivalence classes among the bounded members.
        const auto sharedIdentity = [&](std::string id, std::string_view target, std::string_view label) {
            Axis reference{std::move(id), Role::Conditional,
                {"na", "unresolved", "unknown", "unique", std::string(target)}, std::string(label)};
            for (std::size_t prior = 0; prior < index; ++prior)
                reference.values.push_back("member_" + std::to_string(prior));
            axes.push_back(std::move(reference));
        };
        sharedIdentity("R03." + member + "_volume_ref", "target_volume", "Actual member volume identity");
        sharedIdentity("R02." + member + "_directory_ref", "target_directory", "Actual member parent directory identity");
        axes.back().values.emplace_back("isolated_root");
        axes.back().values.emplace_back("isolated_root_parent");
        for (const auto& ref : endpointRefs) axes.back().values.push_back(ref);
        addCrossTaskDirectoryRefs(axes.back());
        // A selected directory's object is not its parent directory. The
        // reference may alias a target or any selected directory node, with
        // forward references resolved after the full graph is known.
        Axis object{"R02." + member + "_directory_object_ref", Role::Conditional,
            {"na", "unresolved", "unknown", "unique", "target_directory", "isolated_root", "outside_model_unresolved"},
            "Selected directory object node identity"};
        for (const auto& ref : endpointRefs) object.values.push_back(ref);
        addCrossTaskDirectoryRefs(object);
        for (std::size_t other = 0; other < ownReferences; ++other)
        {
            object.values.push_back("member_" + std::to_string(other));
            object.values.push_back("member_object_" + std::to_string(other));
        }
        axes.push_back(std::move(object));
        // Immediate parent can point forward because containment is not
        // ordered by captured selection. A root or target has explicit labels;
        // an unrepresented intermediate directory remains unresolved. Alias
        // consistency, cycles and construction are not product rejection.
        Axis parent{"R02." + member + "_directory_parent_ref", Role::Conditional,
            {"na", "unresolved", "unknown", "root", "isolated_root", "isolated_root_parent", "target_directory", "outside_model_unresolved"},
            "Immediate parent identity of the member parent directory"};
        for (const auto& ref : endpointRefs) parent.values.push_back(ref);
        addCrossTaskDirectoryRefs(parent);
        for (std::size_t other = 0; other < ownReferences; ++other)
        {
            parent.values.push_back("member_" + std::to_string(other));
            parent.values.push_back("member_object_" + std::to_string(other));
        }
        axes.push_back(std::move(parent));
        layout("R02." + member + "_directory_to_target", {"na", "unknown", "same", "ancestor", "descendant", "unrelated"},
            "Member parent directory relation to target directory");
        // Equality to the target follows directory_ref/volume_ref by resolving
        // earlier-member aliases. Unresolved or unknown identities propagate
        // to unknown; these summaries must never be independent inputs.
        AddAxis(axes, "R02." + member + "_parent_layout", Role::Derived,
            {"na", "unknown", "same_directory", "different_directory"},
            "Member parent relative to physical target directory");
        layout("R02." + member + "_target_layout", {"na", "unknown", "same_as_source", "descendant_of_source", "unrelated"},
            "Target path relative to member path");
        layout("R02." + member + "_shortcut_layout", {"na", "unknown", "same_as_source", "different"},
            "Final target of this member shortcut");
        layout("R02." + member + "_path_state", {"na", "unknown", "available", "unavailable"},
            "Configured member path availability");
        AddAxis(axes, "R03." + member + "_volume_layout", Role::Derived,
            {"na", "unknown", "same_volume", "different_volume"},
            "Actual member-to-target volume relation");
        layout("R03." + member + "_storage_layout", {"na", "unknown", "local", "network"},
            "Configured member storage kind");
    }

    AddAxis(axes, "G01.button", Role::Input, {"na", "unknown", "left", "right", "middle"}, "Initiating button");
    // The native OLE key-state buttons at each delivery are independent of
    // the initiating button, which may be outside the host's observation.
    // Ctrl/Shift state is represented by the separate G02 phase axes.
    for (const auto phase : {"enter", "hover_first", "hover_later", "release"})
    {
        Axis buttons{std::string("G02.") + phase + "_buttons", Role::Sequence,
            {"na", "unknown", "none"}, "Delivered mouse-button bit set at phase"};
        for (unsigned mask = 1; mask < 32; ++mask)
        {
            std::string value;
            const char* names[] = {"left", "right", "middle", "x1", "x2"};
            for (unsigned bit = 0; bit < 5; ++bit)
                if (mask & (1U << bit))
                {
                    if (!value.empty()) value += '_';
                    value += names[bit];
                }
            buttons.values.push_back(std::move(value));
        }
        axes.push_back(std::move(buttons));
    }
    AddAxis(axes, "G02.press", Role::Sequence,
        {"unknown", "none", "ctrl", "shift", "alt", "ctrl_shift", "ctrl_alt", "shift_alt", "ctrl_shift_alt"},
        "Modifier keys at press");
    AddAxis(axes, "G02.enter", Role::Sequence,
        {"na", "unknown", "none", "ctrl", "shift", "alt", "ctrl_shift", "ctrl_alt", "shift_alt", "ctrl_shift_alt"},
        "Modifier keys delivered at drag enter");
    AddAxis(axes, "G02.hover_first", Role::Sequence,
        {"na", "unknown", "none", "ctrl", "shift", "alt", "ctrl_shift", "ctrl_alt", "shift_alt", "ctrl_shift_alt"},
        "Modifier keys at first hover");
    AddAxis(axes, "G02.hover_later", Role::Sequence,
        {"na", "unknown", "none", "ctrl", "shift", "alt", "ctrl_shift", "ctrl_alt", "shift_alt", "ctrl_shift_alt"},
        "Modifier keys at a later hover");
    AddAxis(axes, "G02.release", Role::Sequence,
        {"na", "unknown", "none", "ctrl", "shift", "alt", "ctrl_shift", "ctrl_alt", "shift_alt", "ctrl_shift_alt"},
        "Modifier keys at release");
    AddAxis(axes, "G03.decision", Role::Conditional,
        {"na", "move", "copy", "link", "dock_mapping", "merge", "group", "convert_continue",
         "enable_program_collection_continue", "cancel", "escape", "outside_dismiss"}, "Menu or confirmation decision");

    AddAxis(axes, "H01.region", Role::Input,
        {"free_space", "empty_slot", "occupied_cell", "icon_center", "widget_body", "move_handle", "tab",
         "before_item", "after_item", "item_gap", "page_edge", "blocked", "external_window"}, "Hit region");
    AddAxis(axes, "H02.relation", Role::Derived,
        {"na", "same_monitor", "different_monitor", "unknown"}, "Source and target monitor identity");
    AddAxis(axes, "H02.coordinates", Role::Conditional,
        {"na", "nonnegative", "negative_x", "negative_y", "negative_both"}, "Target monitor coordinate sign");
    AddAxis(axes, "H02.position", Role::Conditional,
        {"na", "only", "first", "middle", "last"}, "Target monitor position");
    AddAxis(axes, "H02.connection", Role::Sequence,
        {"na", "stable", "offline", "disconnect", "reconnect"}, "Target monitor connection sequence");
    AddAxis(axes, "H03.page", Role::Conditional,
        {"na", "current", "existing_other", "overflow"}, "Target page identity relative to source");
    AddAxis(axes, "H03.page_switch", Role::Sequence,
        {"na", "none", "edge_dwell"}, "Page switch before drop");
    AddAxis(axes, "H03.after_drop", Role::Sequence,
        {"na", "unchanged", "target_deleted"}, "Target page lifecycle after drop");
    AddAxis(axes, "H04.anchor", Role::Conditional,
        {"na", "first", "middle", "last"}, "Requested grid anchor");
    AddAxis(axes, "H04.span", Role::Derived,
        {"na", "one_cell", "multi_cell"}, "Requested object span");
    AddAxis(axes, "H04.fit", Role::Derived,
        {"na", "fits", "partially_occupied", "out_of_bounds", "unknown"}, "Requested footprint relative to target grid");
    AddAxis(axes, "H05.position", Role::Conditional,
        {"na", "first", "middle", "last"}, "Insertion boundary");
    AddAxis(axes, "H05.source_relation", Role::Conditional,
        {"na", "unrelated", "adjacent_original", "inside_own_range"}, "Insertion boundary relative to source");
    AddAxis(axes, "H05.filter", Role::Conditional,
        {"na", "unfiltered", "filtered_boundary"}, "Insertion filter context");
    AddAxis(axes, "H05.scroll", Role::Sequence,
        {"na", "unchanged", "scrolled"}, "Scroll before insertion");
    AddAxis(axes, "H05.surface", Role::Conditional,
        {"na", "members", "labels"}, "Insertion surface");
    // The chosen index identifies a selected object; unselected_target names
    // a stable object supplied by the fixture. Other identities stay pending.
    Axis indicated{"H05.indicated_identity", Role::Conditional,
        {"na", "unselected_target", "unresolved"}, "Stable identity of the indicated insertion object"};
    for (std::size_t index = 0; index < ownReferences; ++index)
        indicated.values.push_back("selected_member_" + std::to_string(index));
    axes.push_back(std::move(indicated));
    AddAxis(axes, "H06.page_record", Role::Conditional,
        {"na", "saved", "planned_new", "missing"}, "Target page record status");
    AddAxis(axes, "H06.page_dimensions", Role::Conditional,
        {"na", "valid", "missing", "invalid"}, "Target page dimensions");
    AddAxis(axes, "H06.reservation_change", Role::Sequence,
        {"na", "unchanged", "id_taken_by_other_page"}, "Page reservation while a drop is pending");
    AddAxis(axes, "H07.occupancy", Role::Conditional,
        {"na", "empty", "partial", "full"}, "Other occupancy of the target page");
    AddAxis(axes, "H07.capacity_relation", Role::Derived,
        {"na", "more_than_selection", "exactly_selection", "less_than_selection", "unknown"},
        "Remaining target capacity relative to selection");
    AddAxis(axes, "H07.occupancy_change", Role::Sequence,
        {"na", "unchanged", "add", "remove", "rearrange"}, "Occupancy change while pending");
    AddAxis(axes, "H08.first_direction", Role::Sequence,
        {"na", "unobserved", "stationary", "horizontal_positive", "horizontal_negative", "vertical_positive", "vertical_negative",
         "diagonal_equal_pp", "diagonal_equal_pn", "diagonal_equal_np", "diagonal_equal_nn"}, "Pointer movement direction");
    AddAxis(axes, "H08.later_direction", Role::Sequence,
        {"na", "stationary", "horizontal_positive", "horizontal_negative", "vertical_positive", "vertical_negative",
         "diagonal_equal_pp", "diagonal_equal_pn", "diagonal_equal_np", "diagonal_equal_nn"}, "Later pointer movement direction");
    AddAxis(axes, "H08.monitor_transition", Role::Derived,
        {"na", "same_monitor", "cross_monitor"}, "Whether the movement crosses monitors");
    AddAxis(axes, "H09.grab_area", Role::Conditional,
        {"na", "icon", "text", "widget_handle"}, "Source grab region");
    AddAxis(axes, "H09.selected_member", Role::Conditional,
        {"na", "first", "nonfirst"}, "Grabbed member in a selection");
    Axis grabbed{"H09.member_index", Role::Conditional,
        {"na", "unresolved"}, "Exact zero-based selected member grabbed"};
    for (std::size_t index = 0; index < ownReferences; ++index)
        grabbed.values.push_back("member_" + std::to_string(index));
    axes.push_back(std::move(grabbed));
    AddAxis(axes, "H09.anchor_mode", Role::Conditional,
        {"na", "pixel_offset", "pointer", "widget_relative"}, "Source anchor mode");
    AddAxis(axes, "H09.offset", Role::Conditional,
        {"na", "center", "positive", "negative", "edge"}, "Grab offset boundary class");
}

inline void AppendSelectionAxes(std::vector<Axis>& axes, std::size_t memberLimit, SelectionSchemaVersion version)
{
    AppendSelectionAxesImpl(axes, memberLimit, version, memberLimit, memberLimit, true);
}

// Schema-only building block for an exact pure-file source family. The caller
// binds that source semantics and validates the whole candidate; this routine
// neither selects a transport nor executes production behavior. Values beyond
// the finite hover/result/position domains remain explicitly unresolved.
// The historical first two slots remain in the schema even for counts 0/1;
// the family validator must handle their applicability after source semantics
// are established. B receives unprefixed axes and is prefixed by the caller.
// A's source count and B's count are independent; B->A references use A's
// exact count, while local references use the current task's exact count.
// Failure to represent or allocate the schema reports false and leaves axes
// untouched. It is resource unavailability, not a structural exclusion.
inline bool AppendSelectionAxesV3(std::vector<Axis>& axes, std::size_t ownCount,
    std::size_t taskACount, bool isTaskB)
{
    if (ownCount + std::size_t{1} == 0 || taskACount + std::size_t{1} == 0) return false;
    try
    {
        std::vector<Axis> generated;
        const auto memberLimit = std::max(std::size_t{2}, ownCount);
        AppendSelectionAxesImpl(generated, memberLimit, SelectionSchemaVersion::source_members_v2,
            ownCount, taskACount, isTaskB);
        const auto sourceCount = std::find_if(generated.begin(), generated.end(), [](const Axis& axis) {
            return axis.id == "P02.source_actual_count";
        });
        if (sourceCount == generated.end()) return false;
        const auto value = ownCount == 0 ? std::string("zero") : ownCount == 1 ? std::string("one") :
            ownCount == 2 ? std::string("two") : "count_" + std::to_string(ownCount);
        sourceCount->values = {value};
        // Build a replacement first so that allocation failure never leaves a
        // partially appended family in the caller's schema.
        std::vector<Axis> combined = axes;
        if (generated.size() > combined.max_size() - combined.size()) return false;
        combined.reserve(combined.size() + generated.size());
        for (auto& axis : generated) combined.push_back(std::move(axis));
        axes.swap(combined);
        return true;
    }
    catch (const std::bad_alloc&) { return false; }
    catch (const std::length_error&) { return false; }
}

// Family selectors describe independent quantities. Exact numbers are
// canonical decimal strings so a hover value or result hypothesis never needs
// to fit size_t merely to be represented in the schema. Unknown means an
// observable unknown, not an alias for an unenumerated exact value.
enum class V3CountKind { not_applicable, unknown, exact };
struct V3CountValue
{
    V3CountKind kind = V3CountKind::not_applicable;
    std::string decimal;
};
// The canonical family describes only the pure-file source-vector family;
// other transports and all production execution remain separate obligations.
struct V3ExactTaskSpec
{
    std::string sourceDecimal;
    V3CountValue hover;
    V3CountValue resolved;
};
struct V3FamilyDescriptor
{
    V3ExactTaskSpec a;
    std::optional<V3ExactTaskSpec> b; // absent differs from active source count zero
};
struct V3TaskCounts
{
    // The outer, arbitrary-precision family descriptor is checked before this
    // physical slot count is converted to size_t. A conversion or allocation
    // failure cannot eliminate that family from enumeration.
    std::size_t sourceCount = 0;
    V3CountValue hover;
    V3CountValue resolved;
};
enum class V3SelectionStatus { ok, invalid_descriptor, resource_unavailable, schema_error };

inline bool V3CountValueCanonical(const V3CountValue& count)
{
    if (count.kind == V3CountKind::not_applicable || count.kind == V3CountKind::unknown)
        return count.decimal.empty();
    if (count.kind != V3CountKind::exact || count.decimal.empty() ||
        (count.decimal.size() > 1 && count.decimal.front() == '0')) return false;
    return std::all_of(count.decimal.begin(), count.decimal.end(),
        [](unsigned char ch) { return ch >= '0' && ch <= '9'; });
}

// V4 is a separate versioned family. It does not redefine or widen V3 and
// does not assert that format representations or product behavior are known.
// A source-kind tag partitions families; the file count is the number of
// actual file-path source members, never the number of OLE representations or
// the host's later parsed result. A/B have independent tags and quantities.
enum class V4SourceKind { file_only, content_only, file_and_content, unknown_unresolved, unsupported_unresolved };
enum class V4PayloadKind { not_applicable, unknown_unresolved, exact };
struct V4PayloadSlot
{
    // Stable ASCII format/medium and index identity. The index is "default",
    // "unknown_unresolved", or canonical "index_<decimal>". Bytes are exactly
    // the supplied representation, not decoded data or a product result.
    std::string format, medium, index;
    V4PayloadKind kind = V4PayloadKind::unknown_unresolved;
    std::string hex; // lowercase even-length hexadecimal; empty is valid exact bytes
};
enum class V4PathBase { raw, owned_root_relative };
struct V4IndexedUtf16Slot
{
    // index is a canonical unsigned decimal token. Exact bytes are UTF-16LE
    // code units as supplied, including embedded NUL or unpaired surrogates.
    // They do not authorize filesystem access or identify an isolated path.
    std::string index;
    V4PayloadKind kind = V4PayloadKind::unknown_unresolved;
    std::string utf16leHex;
    V4PathBase base = V4PathBase::raw; // virtual names always use raw
};
struct V4IndexedBytesSlot
{
    // Physical source-file contents, independent of any OLE FileContents
    // representation. Exact empty bytes differ from na and unknown.
    std::string index;
    V4PayloadKind kind = V4PayloadKind::unknown_unresolved;
    std::string hex;
};
enum class V4DirectoryNodeKind { file, directory, other_unresolved, unknown_unresolved };
enum class V4DirectoryCompleteness { not_applicable, unknown_unresolved, partial_unresolved, complete };
struct V4DirectoryRootSlot
{
    std::string rootIndex;
    V4DirectoryCompleteness completeness = V4DirectoryCompleteness::unknown_unresolved;
};
struct V4DirectoryNodeSlot
{
    // Slot identity is independent of the supplied path: duplicate or unknown
    // paths, aliases and unsupported filesystem structures remain expressible.
    std::string rootIndex, slotIndex;
    V4PayloadKind pathKind = V4PayloadKind::unknown_unresolved;
    std::string relativePathUtf16Hex;
    V4DirectoryNodeKind nodeKind = V4DirectoryNodeKind::unknown_unresolved;
    V4PayloadKind contentKind = V4PayloadKind::unknown_unresolved;
    std::string contentHex;
    V3CountValue objectIdentity;
};
struct V4RequestResultSlot
{
    std::string format, medium, index, phase; // phase is query, get, or read
    V4PayloadKind kind = V4PayloadKind::unknown_unresolved;
    std::string hresultHex; // exact is all eight lowercase hex digits, including success
};
enum class V4SelectionPreparationMode { none, single_click, ctrl_sequence, marquee, shift_range, ctrl_shift_range, other_unresolved, unknown_unresolved };
enum class V4SelectionPreparationAction { pointer_down, pointer_move, pointer_up, session_end, other_unresolved };
enum class V4SelectionPreparationButton { none, left, right, middle, unknown_unresolved };
struct V4SelectionPreparationStep
{
    V4SelectionPreparationAction action = V4SelectionPreparationAction::other_unresolved;
    V4SelectionPreparationButton button = V4SelectionPreparationButton::unknown_unresolved;
    V3CountValue session{V3CountKind::unknown, {}}; // explicit ordinal; never inferred from pointer-up
    V4PayloadKind modifiersKind = V4PayloadKind::unknown_unresolved;
    std::string modifiersHex; // exact 32-bit raw mask, eight most-significant-first hex digits
    V3CountValue member{V3CountKind::unknown, {}}; // intended index, not actual hit or selection
    V4PayloadKind xKind = V4PayloadKind::unknown_unresolved, yKind = V4PayloadKind::unknown_unresolved;
    std::string xHex, yHex; // exact 32-bit two's-complement screen values, most-significant digit first
};
enum class V4SelectionOutsideScope { desktop, collection, desktop_files, dock, running_items, popup, widget, other_unresolved, unknown_unresolved };
struct V4SelectionOutsideIdentity
{
    std::string index; // distinct slot; duplicate identity bytes and aliases remain expressible
    V4SelectionOutsideScope scope = V4SelectionOutsideScope::unknown_unresolved;
    V4PayloadKind identityKind = V4PayloadKind::unknown_unresolved;
    std::string identityHex; // arbitrary finite opaque bytes, not proof of a runtime object
    V3CountValue alias;
};
struct V4SelectionPreparation
{
    V4SelectionPreparationMode mode = V4SelectionPreparationMode::unknown_unresolved;
    std::vector<V4SelectionPreparationStep> steps; // ordered; repeated actions and members remain distinct
    std::vector<V4SelectionOutsideIdentity> outsideIdentities; // canonical slot-index order, not selection state
};
enum class V4DimensionTarget { source_span_columns, source_span_rows, source_page_columns, source_page_rows,
    target_span_columns, target_span_rows, target_page_columns, target_page_rows, member_span_columns, member_span_rows };
struct V4DimensionSlot
{
    std::string index; // canonical descriptor slot, independent of semantic target
    V4DimensionTarget target = V4DimensionTarget::source_span_columns;
    V3CountValue member; // exact logical member for member targets; other values remain unresolved
    V4PayloadKind kind = V4PayloadKind::unknown_unresolved;
    std::string valueHex; // exact raw Windows int32 bit pattern, eight most-significant-first hex digits
};
enum class V4PositionTarget { visible, storage };
struct V4PositionSlot
{
    std::string index;
    V4PositionTarget target = V4PositionTarget::visible;
    V3CountValue member, position;
};
enum class V4InputBoolean { not_applicable, unknown_unresolved, no, yes };
struct V4CategoryRuleInput
{
    V4PayloadKind idKind = V4PayloadKind::unknown_unresolved, labelKind = V4PayloadKind::unknown_unresolved,
        extensionsKind = V4PayloadKind::unknown_unresolved;
    std::string idUtf16Hex, labelUtf16Hex, extensionsUtf16Hex;
    V4InputBoolean enabled = V4InputBoolean::unknown_unresolved;
};
struct V4ShortcutTargetInput
{
    std::string index;
    V4InputBoolean classified = V4InputBoolean::unknown_unresolved, directory = V4InputBoolean::unknown_unresolved,
        application = V4InputBoolean::unknown_unresolved;
    V4PayloadKind extensionKind = V4PayloadKind::unknown_unresolved;
    std::string extensionUtf16Hex;
};
// Ordered raw group inputs: ordinal is the slot identity, not proof of hit.
struct V4GroupChildInput
{
    V4PayloadKind identityKind = V4PayloadKind::unknown_unresolved;
    std::string idUtf16Hex;
    V4PayloadKind typeKind = V4PayloadKind::unknown_unresolved;
    std::string typeHex; // exact 32-bit raw type, most-significant digit first
};
struct V4GroupEndpointInput
{
    V4PayloadKind childrenKind = V4PayloadKind::unknown_unresolved;
    std::vector<V4GroupChildInput> children;
    V3CountValue activeChild{V3CountKind::unknown, {}}, hitChild{V3CountKind::unknown, {}};
};
struct V4TaskSpec
{
    V4SourceKind kind = V4SourceKind::unknown_unresolved;
    V3CountValue selectionCount; // logical source members, independent of physical files
    V3CountValue fileCount; // na for content-only; unknown and exact are distinct
    V3CountValue virtualCount; // declared virtual entries, independent of other counts
    V3CountValue hover;     // source-provided hover metadata, independent
    V3CountValue resolved;  // production-result hypothesis, never fixture input
    std::vector<V4PayloadSlot> payloads; // canonical (format,medium,index) order
    std::vector<V4IndexedUtf16Slot> filePaths, virtualNames;
    std::vector<V4RequestResultSlot> requestResults; // canonical (format,medium,index,phase) order
    std::vector<V4IndexedBytesSlot> fileContents; // canonical physical-file index order
    std::vector<V4DirectoryRootSlot> directoryRoots; // canonical rootIndex order
    std::vector<V4DirectoryNodeSlot> directoryNodes; // canonical (rootIndex,slotIndex) order
    V4SelectionPreparation preparation;
    std::vector<V4DimensionSlot> dimensions;
    std::vector<V4PositionSlot> positions;
    V4PayloadKind categoryRulesKind = V4PayloadKind::unknown_unresolved;
    std::vector<V4CategoryRuleInput> categoryRules; // actual order; duplicate IDs/rules retained
    std::vector<V4ShortcutTargetInput> shortcutTargets; // canonical logical index order
    V4GroupEndpointInput sourceGroup, targetGroup;
};
struct V4FamilyDescriptor
{
    V4TaskSpec a;
    std::optional<V4TaskSpec> b; // absent B differs from active B with zero files
};
// Defined by the authoritative model after all family axes are declared.
inline std::string V4FamilyBinding(const V4FamilyDescriptor& family);
enum class V4SlotStatus { schema_defined, unresolved, invalid_descriptor };
struct V4SlotAudit
{
    V4SlotStatus status = V4SlotStatus::invalid_descriptor;
    std::string_view reason;
};
inline std::string_view V4SourceKindName(V4SourceKind kind) noexcept
{
    switch (kind)
    {
    case V4SourceKind::file_only: return "file_only";
    case V4SourceKind::content_only: return "content_only";
    case V4SourceKind::file_and_content: return "file_and_content";
    case V4SourceKind::unknown_unresolved: return "unknown_unresolved";
    case V4SourceKind::unsupported_unresolved: return "unsupported_unresolved";
    }
    return {};
}
inline bool V4AsciiToken(std::string_view value) noexcept
{
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_'; });
}
inline bool V4CanonicalDecimal(std::string_view value) noexcept
{
    return !value.empty() && (value.size() == 1 || value.front() != '0') &&
        std::all_of(value.begin(), value.end(), [](unsigned char ch) { return ch >= '0' && ch <= '9'; });
}
inline bool V4CanonicalIndex(std::string_view value) noexcept
{
    return value == "default" || value == "unknown_unresolved" ||
        (value.starts_with("index_") && V4CanonicalDecimal(value.substr(6)));
}
inline bool V4LowerHex(std::string_view value) noexcept
{
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'); });
}
inline bool V4TaggedHex(V4PayloadKind kind, std::string_view hex, std::size_t alignment) noexcept
{
    if (kind == V4PayloadKind::exact) return hex.size() % alignment == 0 && V4LowerHex(hex);
    return (kind == V4PayloadKind::not_applicable || kind == V4PayloadKind::unknown_unresolved) && hex.empty();
}
inline bool V4TaskSlotsCanonical(const V4TaskSpec& task)
{
    if (V4SourceKindName(task.kind).empty() || !V3CountValueCanonical(task.selectionCount) ||
        !V3CountValueCanonical(task.fileCount) || !V3CountValueCanonical(task.virtualCount) ||
        !V3CountValueCanonical(task.hover) || !V3CountValueCanonical(task.resolved) ||
        static_cast<unsigned>(task.preparation.mode) > static_cast<unsigned>(V4SelectionPreparationMode::unknown_unresolved)) return false;
    for (const auto& step : task.preparation.steps)
    {
        if (static_cast<unsigned>(step.action) > static_cast<unsigned>(V4SelectionPreparationAction::other_unresolved) ||
            static_cast<unsigned>(step.button) > static_cast<unsigned>(V4SelectionPreparationButton::unknown_unresolved) ||
            !V3CountValueCanonical(step.session) || !V3CountValueCanonical(step.member) || !V4TaggedHex(step.modifiersKind, step.modifiersHex, 8) ||
            (step.modifiersKind == V4PayloadKind::exact && step.modifiersHex.size() != 8) ||
            !V4TaggedHex(step.xKind, step.xHex, 8) || !V4TaggedHex(step.yKind, step.yHex, 8) ||
            (step.xKind == V4PayloadKind::exact && step.xHex.size() != 8) ||
            (step.yKind == V4PayloadKind::exact && step.yHex.size() != 8)) return false;
    }
    const V4SelectionOutsideIdentity* priorOutside = nullptr;
    for (const auto& item : task.preparation.outsideIdentities)
    {
        if (!V4CanonicalDecimal(item.index) ||
            static_cast<unsigned>(item.scope) > static_cast<unsigned>(V4SelectionOutsideScope::unknown_unresolved) ||
            !V4TaggedHex(item.identityKind, item.identityHex, 2) || !V3CountValueCanonical(item.alias) ||
            (priorOutside && priorOutside->index >= item.index)) return false;
        priorOutside = &item;
    }
    const V4DimensionSlot* priorDimension = nullptr;
    for (const auto& item : task.dimensions)
    {
        if (!V4CanonicalDecimal(item.index) || static_cast<unsigned>(item.target) > static_cast<unsigned>(V4DimensionTarget::member_span_rows) ||
            !V3CountValueCanonical(item.member) || !V4TaggedHex(item.kind, item.valueHex, 8) ||
            (item.kind == V4PayloadKind::exact && item.valueHex.size() != 8) ||
            (priorDimension && priorDimension->index >= item.index)) return false;
        priorDimension = &item;
    }
    const V4PositionSlot* priorPosition = nullptr;
    for (const auto& item : task.positions)
    {
        if (!V4CanonicalDecimal(item.index) || static_cast<unsigned>(item.target) > static_cast<unsigned>(V4PositionTarget::storage) ||
            !V3CountValueCanonical(item.member) || !V3CountValueCanonical(item.position) ||
            (priorPosition && priorPosition->index >= item.index)) return false;
        priorPosition = &item;
    }
    if (static_cast<unsigned>(task.categoryRulesKind) > static_cast<unsigned>(V4PayloadKind::exact) ||
        (task.categoryRulesKind != V4PayloadKind::exact && !task.categoryRules.empty())) return false;
    for (const auto& rule : task.categoryRules)
        if (!V4TaggedHex(rule.idKind, rule.idUtf16Hex, 4) || !V4TaggedHex(rule.labelKind, rule.labelUtf16Hex, 4) ||
            !V4TaggedHex(rule.extensionsKind, rule.extensionsUtf16Hex, 4) ||
            static_cast<unsigned>(rule.enabled) > static_cast<unsigned>(V4InputBoolean::yes)) return false;
    const V4ShortcutTargetInput* priorShortcut = nullptr;
    for (const auto& item : task.shortcutTargets)
    {
        if (!V4CanonicalDecimal(item.index) || !V4TaggedHex(item.extensionKind, item.extensionUtf16Hex, 4) ||
            static_cast<unsigned>(item.classified) > static_cast<unsigned>(V4InputBoolean::yes) ||
            static_cast<unsigned>(item.directory) > static_cast<unsigned>(V4InputBoolean::yes) ||
            static_cast<unsigned>(item.application) > static_cast<unsigned>(V4InputBoolean::yes) ||
            (priorShortcut && priorShortcut->index >= item.index)) return false;
        priorShortcut = &item;
    }
    for (const auto* group : {&task.sourceGroup, &task.targetGroup})
    {
        if (static_cast<unsigned>(group->childrenKind) > static_cast<unsigned>(V4PayloadKind::exact) ||
            (group->childrenKind != V4PayloadKind::exact && !group->children.empty()) ||
            !V3CountValueCanonical(group->activeChild) || !V3CountValueCanonical(group->hitChild)) return false;
        for (const auto& child : group->children)
            if (!V4TaggedHex(child.identityKind, child.idUtf16Hex, 4) || !V4TaggedHex(child.typeKind, child.typeHex, 8) ||
                (child.typeKind == V4PayloadKind::exact && child.typeHex.size() != 8)) return false;
    }
    const V4PayloadSlot* previous = nullptr;
    for (const auto& slot : task.payloads)
    {
        if (!V4AsciiToken(slot.format) || !V4AsciiToken(slot.medium) || !V4CanonicalIndex(slot.index) ||
            !V4TaggedHex(slot.kind, slot.hex, 2)) return false;
        if (previous && !(previous->format < slot.format ||
            (previous->format == slot.format && previous->medium < slot.medium) ||
            (previous->format == slot.format && previous->medium == slot.medium && previous->index < slot.index))) return false;
        previous = &slot;
    }
    for (const auto* slots : {&task.filePaths, &task.virtualNames})
    {
        const V4IndexedUtf16Slot* preceding = nullptr;
        for (const auto& slot : *slots)
        {
            if (!V4CanonicalDecimal(slot.index) || !V4TaggedHex(slot.kind, slot.utf16leHex, 4) ||
                static_cast<unsigned>(slot.base) > static_cast<unsigned>(V4PathBase::owned_root_relative) ||
                (slots == &task.virtualNames && slot.base != V4PathBase::raw) ||
                (preceding && preceding->index >= slot.index)) return false;
            preceding = &slot;
        }
    }
    const V4IndexedBytesSlot* precedingFile = nullptr;
    for (const auto& slot : task.fileContents)
    {
        if (!V4CanonicalDecimal(slot.index) || !V4TaggedHex(slot.kind, slot.hex, 2) ||
            (precedingFile && precedingFile->index >= slot.index)) return false;
        precedingFile = &slot;
    }
    const V4DirectoryRootSlot* precedingRoot = nullptr;
    for (const auto& root : task.directoryRoots)
    {
        if (!V4CanonicalDecimal(root.rootIndex) ||
            static_cast<unsigned>(root.completeness) > static_cast<unsigned>(V4DirectoryCompleteness::complete) ||
            (precedingRoot && precedingRoot->rootIndex >= root.rootIndex)) return false;
        precedingRoot = &root;
    }
    const V4DirectoryNodeSlot* precedingNode = nullptr;
    for (const auto& node : task.directoryNodes)
    {
        if (!V4CanonicalDecimal(node.rootIndex) || !V4CanonicalDecimal(node.slotIndex) ||
            !V4TaggedHex(node.pathKind, node.relativePathUtf16Hex, 4) ||
            static_cast<unsigned>(node.nodeKind) > static_cast<unsigned>(V4DirectoryNodeKind::unknown_unresolved) ||
            !V4TaggedHex(node.contentKind, node.contentHex, 2) || !V3CountValueCanonical(node.objectIdentity) ||
            (precedingNode && !(precedingNode->rootIndex < node.rootIndex ||
                (precedingNode->rootIndex == node.rootIndex && precedingNode->slotIndex < node.slotIndex)))) return false;
        precedingNode = &node;
    }
    const V4RequestResultSlot* precedingResult = nullptr;
    for (const auto& slot : task.requestResults)
    {
        if (!V4AsciiToken(slot.format) || !V4AsciiToken(slot.medium) || !V4CanonicalIndex(slot.index) ||
            (slot.phase != "query" && slot.phase != "get" && slot.phase != "read") ||
            !V4TaggedHex(slot.kind, slot.hresultHex, 8) ||
            (slot.kind == V4PayloadKind::exact && slot.hresultHex.size() != 8)) return false;
        if (precedingResult && !(precedingResult->format < slot.format ||
            (precedingResult->format == slot.format && precedingResult->medium < slot.medium) ||
            (precedingResult->format == slot.format && precedingResult->medium == slot.medium && precedingResult->index < slot.index) ||
            (precedingResult->format == slot.format && precedingResult->medium == slot.medium && precedingResult->index == slot.index && precedingResult->phase < slot.phase))) return false;
        precedingResult = &slot;
    }
    return true;
}
inline bool V4FamilySlotsCanonical(const V4FamilyDescriptor& family)
{ return V4TaskSlotsCanonical(family.a) && (!family.b || V4TaskSlotsCanonical(*family.b)); }

// Slot assessment is not a product rejection or a coverage claim. Source
// kind/content and file-count relationships lacking a construction contract
// are retained unresolved rather than eliminated from the model. In
// particular, zero files is a concrete value, not an alias for na/unknown.
inline V4SlotAudit AuditV4TaskSlots(const V4TaskSpec& task)
{
    if (!V4TaskSlotsCanonical(task)) return {V4SlotStatus::invalid_descriptor, "v4_task_slot_encoding_invalid"};
    if (task.kind == V4SourceKind::unknown_unresolved || task.kind == V4SourceKind::unsupported_unresolved)
        return {V4SlotStatus::unresolved, "v4_source_kind_semantics_unresolved"};
    if (task.selectionCount.kind != V3CountKind::exact)
        return {V4SlotStatus::unresolved, "v4_selection_member_count_unresolved"};
    if (task.kind == V4SourceKind::content_only && task.fileCount.kind != V3CountKind::not_applicable &&
        !(task.fileCount.kind == V3CountKind::exact && task.fileCount.decimal == "0"))
        return {V4SlotStatus::unresolved, "v4_content_only_file_count_not_applicable_unresolved"};
    if (task.kind != V4SourceKind::content_only && task.fileCount.kind != V3CountKind::exact)
        return {V4SlotStatus::unresolved, "v4_file_source_count_unresolved"};
    return {V4SlotStatus::schema_defined, "v4_representation_and_production_semantics_unimplemented"};
}

// A separate natural-coordinate space for the canonical V4 family only.
// Budgets govern physical work, never family identity. Invalid naturals are
// encoding holes, not product exclusions; every canonical finite descriptor
// has a unique coordinate and every coordinate is checked before use.
inline constexpr std::string_view V4FamilyCoordinateAlgorithm = "v4_family_compact_interleaved_lists_v13_lua_permissions";
struct V4CoordinateBudget
{
    std::size_t decimalDigits = 0, tokenBytes = 0, listEntries = 0, arithmeticSteps = 0;
};
enum class V4FamilyCoordinateStatus { ready, encoding_hole, invalid_descriptor, resource_unavailable };
struct V4FamilyCoordinateEncoding
{
    V4FamilyCoordinateStatus status = V4FamilyCoordinateStatus::invalid_descriptor;
    std::string coordinate;
    std::string_view reason;
};
struct V4FamilyCoordinateStep
{
    V4FamilyCoordinateStatus status = V4FamilyCoordinateStatus::encoding_hole;
    std::string coordinate, nextCoordinate;
    std::optional<V4FamilyDescriptor> family;
    std::string_view reason;
};
namespace v4_coordinate_detail
{
struct Math
{
    const V4CoordinateBudget& budget;
    std::size_t steps = 0, bytes = 0, entries = 0;
    bool unavailable = false, invalid = false;
    bool Work() noexcept
    { if (steps >= budget.arithmeticSteps) { unavailable = true; return false; } ++steps; return true; }
    bool Digits(const std::string& value) noexcept
    { if (!budget.decimalDigits || value.size() > budget.decimalDigits) { unavailable = true; return false; } return true; }
    bool Bytes(std::size_t count) noexcept
    { if (count > budget.tokenBytes - bytes) { unavailable = true; return false; } bytes += count; return true; }
    bool Entry() noexcept
    { if (entries >= budget.listEntries) { unavailable = true; return false; } ++entries; return true; }
    std::string String(std::string_view token)
    {
        if (unavailable || !Bytes(token.size())) return {};
        std::string out = "0";
        for (const unsigned char byte : token)
        {
            if (!Work()) return {};
            out = Add(Multiply(std::move(out), 257), std::to_string(static_cast<unsigned>(byte) + 1));
            if (!Digits(out)) return {};
        }
        return out;
    }
    std::string Unstring(std::string value)
    {
        std::string out;
        while (value != "0")
        {
            if (!Work() || !Bytes(1)) return {};
            const auto remainder = Divide(value, 257);
            if (!remainder) { invalid = true; return {}; }
            out.push_back(static_cast<char>(remainder - 1));
        }
        std::reverse(out.begin(), out.end());
        return out;
    }
    std::string Interleave(std::vector<std::string> values)
    {
        std::string out = "0", weight = "1";
        while (std::any_of(values.begin(), values.end(), [](const auto& x) { return x != "0"; }))
            for (auto& value : values)
            {
                if (!Work()) return {};
                if (Divide(value, 2)) out = Add(std::move(out), weight);
                if (!Digits(out)) return {};
                if (std::any_of(values.begin(), values.end(), [](const auto& x) { return x != "0"; }))
                { weight = Multiply(std::move(weight), 2); if (!Digits(weight)) return {}; }
            }
        return out;
    }
    std::vector<std::string> Uninterleave(std::string value, std::size_t dimensions)
    {
        std::vector<std::string> values(dimensions, "0"), weights(dimensions, "1");
        std::size_t at = 0;
        while (value != "0")
        {
            if (!Work()) return {};
            if (Divide(value, 2)) values[at] = Add(std::move(values[at]), weights[at]);
            if (!Digits(values[at])) return {};
            if (value != "0") { weights[at] = Multiply(std::move(weights[at]), 2); if (!Digits(weights[at])) return {}; }
            at = (at + 1) % dimensions;
        }
        return values;
    }
    std::string List(const std::vector<std::string>& items)
    {
        std::string tail = "0";
        for (auto it = items.rbegin(); it != items.rend(); ++it)
        {
            if (!Entry()) return {};
            tail = Interleave({*it, tail});
            if (unavailable) return {};
            tail = Add(std::move(tail), "1"); if (!Digits(tail)) return {};
        }
        return tail;
    }
    std::vector<std::string> Unlist(std::string value)
    {
        std::vector<std::string> items;
        while (value != "0")
        {
            if (!Entry() || !Work()) return {};
            auto pair = Uninterleave(Subtract(std::move(value), "1"), 2);
            if (unavailable || pair.size() != 2) return {};
            items.push_back(std::move(pair[0])); value = std::move(pair[1]);
        }
        return items;
    }
    std::string Count(const V3CountValue& count)
    {
        if (count.kind == V3CountKind::not_applicable) return "0";
        if (count.kind == V3CountKind::unknown) return "1";
        if (unavailable || !Digits(count.decimal)) return {};
        auto value = Add(count.decimal, "2"); if (!Digits(value)) return {}; return value;
    }
    V3CountValue Uncount(const std::string& value)
    {
        if (value == "0") return {V3CountKind::not_applicable, {}};
        if (value == "1") return {V3CountKind::unknown, {}};
        return {V3CountKind::exact, Subtract(value, "2")};
    }
};

inline constexpr std::string_view Formats[] = {"cf_hdrop", "shell_id_list", "file_descriptor_w", "file_descriptor_a",
    "file_contents", "dib", "dibv5", "png", "image_png", "bitmap", "text_unicode", "text_ansi", "url_w", "url_a"};
inline constexpr std::string_view Media[] = {"hglobal", "file", "istream", "istorage", "gdi", "mfpict", "enhmf"};
template <std::size_t N>
inline std::string EncodeSymbol(Math& math, std::string_view value, const std::string_view (&known)[N])
{
    for (std::size_t i = 0; i < N; ++i) if (known[i] == value) return std::to_string(i);
    auto encoded = math.String(value);
    if (math.unavailable) return {};
    auto result = Add(std::move(encoded), std::to_string(N));
    if (!math.Digits(result)) return {};
    return result;
}
template <std::size_t N>
inline std::string DecodeSymbol(Math& math, const std::string& code, const std::string_view (&known)[N])
{
    for (std::size_t i = 0; i < N; ++i) if (code == std::to_string(i)) return std::string(known[i]);
    if (Less(code, std::to_string(N))) { math.invalid = true; return {}; }
    auto value = math.Unstring(Subtract(code, std::to_string(N)));
    for (const auto symbol : known) if (symbol == value) math.invalid = true;
    return value;
}
inline std::string EncodeIndex(Math& math, std::string_view index)
{
    if (index == "default") return "0";
    if (index == "unknown_unresolved") return "1";
    std::string number(index.substr(6));
    if (!math.Digits(number)) return {};
    auto value = Add(std::move(number), "2");
    if (!math.Digits(value)) return {};
    return value;
}
inline std::string DecodeIndex(const std::string& code)
{
    if (code == "0") return "default";
    if (code == "1") return "unknown_unresolved";
    return "index_" + Subtract(code, "2");
}
inline std::string HexBytes(std::string_view hex)
{
    std::string bytes; bytes.reserve(hex.size() / 2);
    const auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (std::size_t i = 0; i < hex.size(); i += 2)
        bytes.push_back(static_cast<char>((digit(hex[i]) << 4) | digit(hex[i + 1])));
    return bytes;
}
inline std::string BytesHex(std::string_view bytes)
{
    constexpr char digits[] = "0123456789abcdef";
    if (bytes.size() > (std::numeric_limits<std::size_t>::max)() / 2) throw std::length_error("v4 hex size unavailable");
    std::string hex; hex.reserve(bytes.size() * 2);
    for (const unsigned char byte : bytes) { hex.push_back(digits[byte >> 4]); hex.push_back(digits[byte & 15]); }
    return hex;
}
inline std::string EncodeHex(Math& math, std::string_view hex)
{
    if (hex.size() / 2 > math.budget.tokenBytes - math.bytes)
    { math.unavailable = true; return {}; }
    return math.String(HexBytes(hex));
}

inline std::string EncodePayload(Math& math, const V4PayloadSlot& slot)
{
    std::vector<std::string> values;
    values.push_back(EncodeSymbol(math, slot.format, Formats)); values.push_back(EncodeSymbol(math, slot.medium, Media));
    values.push_back(EncodeIndex(math, slot.index));
    values.push_back(std::to_string(static_cast<unsigned>(slot.kind))); values.push_back(EncodeHex(math, slot.hex));
    return math.unavailable ? std::string{} : math.Interleave(std::move(values));
}
inline std::string EncodeText(Math& math, const V4IndexedUtf16Slot& slot)
{
    std::vector<std::string> values;
    values.push_back(slot.index); if (!math.Digits(values.back())) return {};
    values.push_back(std::to_string(static_cast<unsigned>(slot.kind)));
    values.push_back(EncodeHex(math, slot.utf16leHex));
    values.push_back(std::to_string(static_cast<unsigned>(slot.base)));
    return math.unavailable ? std::string{} : math.Interleave(std::move(values));
}
inline std::string EncodeFileBytes(Math& math, const V4IndexedBytesSlot& slot)
{
    std::vector<std::string> values;
    values.push_back(slot.index); if (!math.Digits(values.back())) return {};
    values.push_back(std::to_string(static_cast<unsigned>(slot.kind)));
    values.push_back(EncodeHex(math, slot.hex));
    return math.unavailable ? std::string{} : math.Interleave(std::move(values));
}
inline std::string EncodeDirectoryNode(Math& math, const V4DirectoryNodeSlot& node)
{
    if (!math.Digits(node.rootIndex) || !math.Digits(node.slotIndex)) return {};
    std::vector<std::string> values{node.rootIndex, node.slotIndex,
        std::to_string(static_cast<unsigned>(node.pathKind)), EncodeHex(math, node.relativePathUtf16Hex),
        std::to_string(static_cast<unsigned>(node.nodeKind)), std::to_string(static_cast<unsigned>(node.contentKind)),
        EncodeHex(math, node.contentHex), math.Count(node.objectIdentity)};
    return math.unavailable ? std::string{} : math.Interleave(std::move(values));
}
inline std::string EncodeDirectoryRoot(Math& math, const V4DirectoryRootSlot& root)
{
    if (!math.Digits(root.rootIndex)) return {};
    return math.Interleave({root.rootIndex, std::to_string(static_cast<unsigned>(root.completeness))});
}
inline std::string EncodeRequest(Math& math, const V4RequestResultSlot& slot)
{
    std::vector<std::string> values;
    values.push_back(EncodeSymbol(math, slot.format, Formats)); values.push_back(EncodeSymbol(math, slot.medium, Media));
    values.push_back(EncodeIndex(math, slot.index));
    values.push_back(slot.phase == "query" ? "0" : slot.phase == "get" ? "1" : "2");
    values.push_back(std::to_string(static_cast<unsigned>(slot.kind)));
    values.push_back(EncodeHex(math, slot.hresultHex));
    return math.unavailable ? std::string{} : math.Interleave(std::move(values));
}
inline std::string EncodePreparationStep(Math& math, const V4SelectionPreparationStep& step)
{
    std::vector<std::string> values{std::to_string(static_cast<unsigned>(step.action)), std::to_string(static_cast<unsigned>(step.button)), math.Count(step.session),
        std::to_string(static_cast<unsigned>(step.modifiersKind)), EncodeHex(math, step.modifiersHex), math.Count(step.member),
        std::to_string(static_cast<unsigned>(step.xKind)), EncodeHex(math, step.xHex),
        std::to_string(static_cast<unsigned>(step.yKind)), EncodeHex(math, step.yHex)};
    return math.unavailable ? std::string{} : math.Interleave(std::move(values));
}
inline std::vector<std::string> EncodeTask(Math& math, const V4TaskSpec& task)
{
    std::vector<std::string> values;
    values.push_back(std::to_string(static_cast<unsigned>(task.kind)));
    for (const auto* count : {&task.selectionCount, &task.fileCount, &task.virtualCount, &task.hover, &task.resolved})
        values.push_back(math.Count(*count));
    if (math.unavailable) return {};
    std::vector<std::string> items;
    if (task.payloads.size() > math.budget.listEntries - math.entries) { math.unavailable = true; return {}; }
    for (const auto& slot : task.payloads) { items.push_back(EncodePayload(math, slot)); if (math.unavailable) return {}; }
    values.push_back(math.List(items)); items.clear();
    if (math.unavailable || task.filePaths.size() > math.budget.listEntries - math.entries) { math.unavailable = true; return {}; }
    for (const auto& slot : task.filePaths) { items.push_back(EncodeText(math, slot)); if (math.unavailable) return {}; }
    values.push_back(math.List(items)); items.clear();
    if (math.unavailable || task.virtualNames.size() > math.budget.listEntries - math.entries) { math.unavailable = true; return {}; }
    for (const auto& slot : task.virtualNames) { items.push_back(EncodeText(math, slot)); if (math.unavailable) return {}; }
    values.push_back(math.List(items)); items.clear();
    if (math.unavailable || task.requestResults.size() > math.budget.listEntries - math.entries) { math.unavailable = true; return {}; }
    for (const auto& slot : task.requestResults) { items.push_back(EncodeRequest(math, slot)); if (math.unavailable) return {}; }
    values.push_back(math.List(items)); items.clear();
    if (math.unavailable || task.fileContents.size() > math.budget.listEntries - math.entries) { math.unavailable = true; return {}; }
    for (const auto& slot : task.fileContents) { items.push_back(EncodeFileBytes(math, slot)); if (math.unavailable) return {}; }
    values.push_back(math.List(items)); items.clear();
    if (math.unavailable || task.directoryRoots.size() > math.budget.listEntries - math.entries) { math.unavailable = true; return {}; }
    for (const auto& root : task.directoryRoots) { items.push_back(EncodeDirectoryRoot(math, root)); if (math.unavailable) return {}; }
    values.push_back(math.List(items)); items.clear();
    if (math.unavailable || task.directoryNodes.size() > math.budget.listEntries - math.entries) { math.unavailable = true; return {}; }
    for (const auto& node : task.directoryNodes) { items.push_back(EncodeDirectoryNode(math, node)); if (math.unavailable) return {}; }
    values.push_back(math.List(items)); items.clear();
    values.push_back(std::to_string(static_cast<unsigned>(task.preparation.mode)));
    if (math.unavailable || task.preparation.steps.size() > math.budget.listEntries - math.entries) { math.unavailable = true; return {}; }
    for (const auto& step : task.preparation.steps) { items.push_back(EncodePreparationStep(math, step)); if (math.unavailable) return {}; }
    values.push_back(math.List(items));
    items.clear();
    if (math.unavailable || task.preparation.outsideIdentities.size() > math.budget.listEntries - math.entries)
    { math.unavailable = true; return {}; }
    for (const auto& outside : task.preparation.outsideIdentities)
    {
        if (!math.Digits(outside.index)) return {};
        items.push_back(math.Interleave({outside.index, std::to_string(static_cast<unsigned>(outside.scope)),
            std::to_string(static_cast<unsigned>(outside.identityKind)), EncodeHex(math, outside.identityHex), math.Count(outside.alias)}));
        if (math.unavailable) return {};
    }
    values.push_back(math.List(items));
    items.clear();
    if (math.unavailable || task.dimensions.size() > math.budget.listEntries - math.entries)
    { math.unavailable = true; return {}; }
    for (const auto& dimension : task.dimensions)
    {
        if (!math.Digits(dimension.index)) return {};
        items.push_back(math.Interleave({dimension.index, std::to_string(static_cast<unsigned>(dimension.target)),
            math.Count(dimension.member), std::to_string(static_cast<unsigned>(dimension.kind)), EncodeHex(math, dimension.valueHex)}));
        if (math.unavailable) return {};
    }
    values.push_back(math.List(items));
    items.clear();
    if (math.unavailable || task.positions.size() > math.budget.listEntries - math.entries)
    { math.unavailable = true; return {}; }
    for (const auto& position : task.positions)
    {
        if (!math.Digits(position.index)) return {};
        items.push_back(math.Interleave({position.index, std::to_string(static_cast<unsigned>(position.target)),
            math.Count(position.member), math.Count(position.position)}));
        if (math.unavailable) return {};
    }
    values.push_back(math.List(items));
    items.clear();
    if (math.unavailable || task.categoryRules.size() > math.budget.listEntries - math.entries)
    { math.unavailable = true; return {}; }
    for (const auto& rule : task.categoryRules)
    {
        items.push_back(math.Interleave({std::to_string(static_cast<unsigned>(rule.idKind)), EncodeHex(math, rule.idUtf16Hex),
            std::to_string(static_cast<unsigned>(rule.labelKind)), EncodeHex(math, rule.labelUtf16Hex),
            std::to_string(static_cast<unsigned>(rule.extensionsKind)), EncodeHex(math, rule.extensionsUtf16Hex),
            std::to_string(static_cast<unsigned>(rule.enabled))}));
        if (math.unavailable) return {};
    }
    values.push_back(math.Interleave({std::to_string(static_cast<unsigned>(task.categoryRulesKind)), math.List(items)}));
    items.clear();
    if (math.unavailable || task.shortcutTargets.size() > math.budget.listEntries - math.entries)
    { math.unavailable = true; return {}; }
    for (const auto& shortcut : task.shortcutTargets)
    {
        if (!math.Digits(shortcut.index)) return {};
        items.push_back(math.Interleave({shortcut.index, std::to_string(static_cast<unsigned>(shortcut.classified)),
            std::to_string(static_cast<unsigned>(shortcut.directory)), std::to_string(static_cast<unsigned>(shortcut.application)),
            std::to_string(static_cast<unsigned>(shortcut.extensionKind)), EncodeHex(math, shortcut.extensionUtf16Hex)}));
        if (math.unavailable) return {};
    }
    values.push_back(math.List(items));
    for (const auto* group : {&task.sourceGroup, &task.targetGroup})
    {
        items.clear();
        if (math.unavailable || group->children.size() > math.budget.listEntries - math.entries)
        { math.unavailable = true; return {}; }
        for (const auto& child : group->children)
        {
            items.push_back(math.Interleave({std::to_string(static_cast<unsigned>(child.identityKind)), EncodeHex(math, child.idUtf16Hex),
                std::to_string(static_cast<unsigned>(child.typeKind)), EncodeHex(math, child.typeHex)}));
            if (math.unavailable) return {};
        }
        values.push_back(math.Interleave({std::to_string(static_cast<unsigned>(group->childrenKind)), math.List(items),
            math.Count(group->activeChild), math.Count(group->hitChild)}));
    }
    return values;
}
inline bool PayloadKind(const std::string& value, V4PayloadKind& kind) noexcept
{
    if (value == "0") kind = V4PayloadKind::not_applicable;
    else if (value == "1") kind = V4PayloadKind::unknown_unresolved;
    else if (value == "2") kind = V4PayloadKind::exact;
    else return false;
    return true;
}
inline V4TaskSpec DecodeTask(Math& math, const std::vector<std::string>& values, std::size_t first)
{
    V4TaskSpec task;
    const auto& kind = values[first];
    if (kind == "0") task.kind = V4SourceKind::file_only;
    else if (kind == "1") task.kind = V4SourceKind::content_only;
    else if (kind == "2") task.kind = V4SourceKind::file_and_content;
    else if (kind == "3") task.kind = V4SourceKind::unknown_unresolved;
    else if (kind == "4") task.kind = V4SourceKind::unsupported_unresolved;
    else { math.invalid = true; return task; }
    task.selectionCount = math.Uncount(values[first + 1]); task.fileCount = math.Uncount(values[first + 2]);
    task.virtualCount = math.Uncount(values[first + 3]); task.hover = math.Uncount(values[first + 4]);
    task.resolved = math.Uncount(values[first + 5]);
    for (auto item : math.Unlist(values[first + 6]))
    {
        auto fields = math.Uninterleave(std::move(item), 5);
        if (math.unavailable || fields.size() != 5) return task;
        V4PayloadSlot slot;
        slot.format = DecodeSymbol(math, fields[0], Formats); slot.medium = DecodeSymbol(math, fields[1], Media); slot.index = DecodeIndex(fields[2]);
        if (!PayloadKind(fields[3], slot.kind)) math.invalid = true;
        slot.hex = BytesHex(math.Unstring(fields[4])); task.payloads.push_back(std::move(slot));
        if (math.unavailable || math.invalid) return task;
    }
    const auto text = [&](std::string encoded, std::vector<V4IndexedUtf16Slot>& output) {
        for (auto item : math.Unlist(std::move(encoded)))
        {
            auto fields = math.Uninterleave(std::move(item), 4);
            if (math.unavailable || fields.size() != 4) return;
            V4IndexedUtf16Slot slot;
            slot.index = fields[0];
            if (!PayloadKind(fields[1], slot.kind)) math.invalid = true;
            if (fields[3] == "0") slot.base = V4PathBase::raw;
            else if (fields[3] == "1") slot.base = V4PathBase::owned_root_relative;
            else math.invalid = true;
            slot.utf16leHex = BytesHex(math.Unstring(fields[2])); output.push_back(std::move(slot));
            if (math.unavailable || math.invalid) return;
        }
    };
    text(values[first + 7], task.filePaths);
    text(values[first + 8], task.virtualNames);
    for (auto item : math.Unlist(values[first + 9]))
    {
        auto fields = math.Uninterleave(std::move(item), 6);
        if (math.unavailable || fields.size() != 6) return task;
        V4RequestResultSlot slot;
        slot.format = DecodeSymbol(math, fields[0], Formats); slot.medium = DecodeSymbol(math, fields[1], Media); slot.index = DecodeIndex(fields[2]);
        if (fields[3] == "0") slot.phase = "query";
        else if (fields[3] == "1") slot.phase = "get";
        else if (fields[3] == "2") slot.phase = "read";
        else math.invalid = true;
        if (!PayloadKind(fields[4], slot.kind)) math.invalid = true;
        slot.hresultHex = BytesHex(math.Unstring(fields[5])); task.requestResults.push_back(std::move(slot));
        if (math.unavailable || math.invalid) return task;
    }
    for (auto item : math.Unlist(values[first + 10]))
    {
        auto fields = math.Uninterleave(std::move(item), 3);
        if (math.unavailable || fields.size() != 3) return task;
        V4IndexedBytesSlot slot;
        slot.index = fields[0];
        if (!PayloadKind(fields[1], slot.kind)) math.invalid = true;
        slot.hex = BytesHex(math.Unstring(fields[2])); task.fileContents.push_back(std::move(slot));
        if (math.unavailable || math.invalid) return task;
    }
    for (auto item : math.Unlist(values[first + 11]))
    {
        auto fields = math.Uninterleave(std::move(item), 2);
        if (math.unavailable || fields.size() != 2) return task;
        V4DirectoryRootSlot root; root.rootIndex = fields[0];
        if (fields[1] == "0") root.completeness = V4DirectoryCompleteness::not_applicable;
        else if (fields[1] == "1") root.completeness = V4DirectoryCompleteness::unknown_unresolved;
        else if (fields[1] == "2") root.completeness = V4DirectoryCompleteness::partial_unresolved;
        else if (fields[1] == "3") root.completeness = V4DirectoryCompleteness::complete;
        else math.invalid = true;
        task.directoryRoots.push_back(std::move(root));
        if (math.unavailable || math.invalid) return task;
    }
    for (auto item : math.Unlist(values[first + 12]))
    {
        auto fields = math.Uninterleave(std::move(item), 8);
        if (math.unavailable || fields.size() != 8) return task;
        V4DirectoryNodeSlot node; node.rootIndex = fields[0]; node.slotIndex = fields[1];
        if (!PayloadKind(fields[2], node.pathKind)) math.invalid = true;
        node.relativePathUtf16Hex = BytesHex(math.Unstring(fields[3]));
        if (fields[4] == "0") node.nodeKind = V4DirectoryNodeKind::file;
        else if (fields[4] == "1") node.nodeKind = V4DirectoryNodeKind::directory;
        else if (fields[4] == "2") node.nodeKind = V4DirectoryNodeKind::other_unresolved;
        else if (fields[4] == "3") node.nodeKind = V4DirectoryNodeKind::unknown_unresolved;
        else math.invalid = true;
        if (!PayloadKind(fields[5], node.contentKind)) math.invalid = true;
        node.contentHex = BytesHex(math.Unstring(fields[6])); node.objectIdentity = math.Uncount(fields[7]);
        task.directoryNodes.push_back(std::move(node));
        if (math.unavailable || math.invalid) return task;
    }
    const auto& mode = values[first + 13];
    if (mode == "0") task.preparation.mode = V4SelectionPreparationMode::none;
    else if (mode == "1") task.preparation.mode = V4SelectionPreparationMode::single_click;
    else if (mode == "2") task.preparation.mode = V4SelectionPreparationMode::ctrl_sequence;
    else if (mode == "3") task.preparation.mode = V4SelectionPreparationMode::marquee;
    else if (mode == "4") task.preparation.mode = V4SelectionPreparationMode::shift_range;
    else if (mode == "5") task.preparation.mode = V4SelectionPreparationMode::ctrl_shift_range;
    else if (mode == "6") task.preparation.mode = V4SelectionPreparationMode::other_unresolved;
    else if (mode == "7") task.preparation.mode = V4SelectionPreparationMode::unknown_unresolved;
    else { math.invalid = true; return task; }
    for (auto item : math.Unlist(values[first + 14]))
    {
        auto fields = math.Uninterleave(std::move(item), 10);
        if (math.unavailable || fields.size() != 10) return task;
        V4SelectionPreparationStep step;
        if (fields[0] == "0") step.action = V4SelectionPreparationAction::pointer_down;
        else if (fields[0] == "1") step.action = V4SelectionPreparationAction::pointer_move;
        else if (fields[0] == "2") step.action = V4SelectionPreparationAction::pointer_up;
        else if (fields[0] == "3") step.action = V4SelectionPreparationAction::session_end;
        else if (fields[0] == "4") step.action = V4SelectionPreparationAction::other_unresolved;
        else math.invalid = true;
        if (fields[1] == "0") step.button = V4SelectionPreparationButton::none;
        else if (fields[1] == "1") step.button = V4SelectionPreparationButton::left;
        else if (fields[1] == "2") step.button = V4SelectionPreparationButton::right;
        else if (fields[1] == "3") step.button = V4SelectionPreparationButton::middle;
        else if (fields[1] == "4") step.button = V4SelectionPreparationButton::unknown_unresolved;
        else math.invalid = true;
        step.session = math.Uncount(fields[2]);
        if (!PayloadKind(fields[3], step.modifiersKind) || !PayloadKind(fields[6], step.xKind) ||
            !PayloadKind(fields[8], step.yKind)) math.invalid = true;
        step.modifiersHex = BytesHex(math.Unstring(fields[4])); step.member = math.Uncount(fields[5]);
        step.xHex = BytesHex(math.Unstring(fields[7])); step.yHex = BytesHex(math.Unstring(fields[9]));
        task.preparation.steps.push_back(std::move(step));
        if (math.unavailable || math.invalid) return task;
    }
    for (auto item : math.Unlist(values[first + 15]))
    {
        auto fields = math.Uninterleave(std::move(item), 5);
        if (math.unavailable || fields.size() != 5) return task;
        V4SelectionOutsideIdentity outside; outside.index = fields[0];
        if (fields[1].size() != 1 || fields[1][0] < '0' || fields[1][0] > '8') math.invalid = true;
        else outside.scope = static_cast<V4SelectionOutsideScope>(fields[1][0] - '0');
        if (!PayloadKind(fields[2], outside.identityKind)) math.invalid = true;
        outside.identityHex = BytesHex(math.Unstring(fields[3])); outside.alias = math.Uncount(fields[4]);
        task.preparation.outsideIdentities.push_back(std::move(outside));
        if (math.unavailable || math.invalid) return task;
    }
    for (auto item : math.Unlist(values[first + 16]))
    {
        auto fields = math.Uninterleave(std::move(item), 5);
        if (math.unavailable || fields.size() != 5) return task;
        V4DimensionSlot dimension; dimension.index = fields[0];
        if (fields[1].size() != 1 || fields[1][0] < '0' || fields[1][0] > '9') math.invalid = true;
        else dimension.target = static_cast<V4DimensionTarget>(fields[1][0] - '0');
        dimension.member = math.Uncount(fields[2]);
        if (!PayloadKind(fields[3], dimension.kind)) math.invalid = true;
        dimension.valueHex = BytesHex(math.Unstring(fields[4]));
        task.dimensions.push_back(std::move(dimension));
        if (math.unavailable || math.invalid) return task;
    }
    for (auto item : math.Unlist(values[first + 17]))
    {
        auto fields = math.Uninterleave(std::move(item), 4);
        if (math.unavailable || fields.size() != 4) return task;
        V4PositionSlot position; position.index = fields[0];
        if (fields[1] == "0") position.target = V4PositionTarget::visible;
        else if (fields[1] == "1") position.target = V4PositionTarget::storage;
        else math.invalid = true;
        position.member = math.Uncount(fields[2]); position.position = math.Uncount(fields[3]);
        task.positions.push_back(std::move(position));
        if (math.unavailable || math.invalid) return task;
    }
    const auto inputBoolean = [&](const std::string& value, V4InputBoolean& output) {
        if (value.size() != 1 || value[0] < '0' || value[0] > '3') { math.invalid = true; return; }
        output = static_cast<V4InputBoolean>(value[0] - '0');
    };
    const auto ruleGroup = math.Uninterleave(values[first + 18], 2);
    if (math.unavailable || ruleGroup.size() != 2 || !PayloadKind(ruleGroup[0], task.categoryRulesKind)) { math.invalid = true; return task; }
    for (auto item : math.Unlist(ruleGroup[1]))
    {
        auto fields = math.Uninterleave(std::move(item), 7);
        if (math.unavailable || fields.size() != 7) return task;
        V4CategoryRuleInput rule;
        if (!PayloadKind(fields[0], rule.idKind) || !PayloadKind(fields[2], rule.labelKind) ||
            !PayloadKind(fields[4], rule.extensionsKind)) math.invalid = true;
        rule.idUtf16Hex = BytesHex(math.Unstring(fields[1])); rule.labelUtf16Hex = BytesHex(math.Unstring(fields[3]));
        rule.extensionsUtf16Hex = BytesHex(math.Unstring(fields[5])); inputBoolean(fields[6], rule.enabled);
        task.categoryRules.push_back(std::move(rule));
        if (math.unavailable || math.invalid) return task;
    }
    for (auto item : math.Unlist(values[first + 19]))
    {
        auto fields = math.Uninterleave(std::move(item), 6);
        if (math.unavailable || fields.size() != 6) return task;
        V4ShortcutTargetInput shortcut; shortcut.index = fields[0];
        inputBoolean(fields[1], shortcut.classified); inputBoolean(fields[2], shortcut.directory); inputBoolean(fields[3], shortcut.application);
        if (!PayloadKind(fields[4], shortcut.extensionKind)) math.invalid = true;
        shortcut.extensionUtf16Hex = BytesHex(math.Unstring(fields[5]));
        task.shortcutTargets.push_back(std::move(shortcut));
        if (math.unavailable || math.invalid) return task;
    }
    for (std::size_t side = 0; side < 2; ++side)
    {
        auto& group = side == 0 ? task.sourceGroup : task.targetGroup;
        const auto fields = math.Uninterleave(values[first + 20 + side], 4);
        if (math.unavailable || fields.size() != 4 || !PayloadKind(fields[0], group.childrenKind)) { math.invalid = true; return task; }
        group.activeChild = math.Uncount(fields[2]); group.hitChild = math.Uncount(fields[3]);
        for (auto item : math.Unlist(fields[1]))
        {
            const auto childFields = math.Uninterleave(std::move(item), 4);
            if (math.unavailable || childFields.size() != 4) return task;
            V4GroupChildInput child;
            if (!PayloadKind(childFields[0], child.identityKind) || !PayloadKind(childFields[2], child.typeKind)) math.invalid = true;
            child.idUtf16Hex = BytesHex(math.Unstring(childFields[1])); child.typeHex = BytesHex(math.Unstring(childFields[3]));
            group.children.push_back(std::move(child));
            if (math.unavailable || math.invalid) return task;
        }
    }
    return task;
}
} // namespace v4_coordinate_detail

inline V4FamilyCoordinateEncoding EncodeV4FamilyCoordinate(const V4FamilyDescriptor& family,
    const V4CoordinateBudget& budget)
{
    V4FamilyCoordinateEncoding result;
    if (!V4FamilySlotsCanonical(family)) { result.reason = "v4_family_descriptor_invalid"; return result; }
    try
    {
        v4_coordinate_detail::Math math{budget};
        auto values = v4_coordinate_detail::EncodeTask(math, family.a);
        if (family.b)
        {
            auto second = v4_coordinate_detail::EncodeTask(math, *family.b);
            for (auto& value : second) values.push_back(std::move(value));
        }
        if (!math.unavailable)
        {
            auto payload = math.Interleave(std::move(values));
            if (!math.unavailable)
            {
                result.coordinate = Multiply(std::move(payload), 2);
                if (family.b) result.coordinate = Add(std::move(result.coordinate), "1");
                math.Digits(result.coordinate);
            }
        }
        if (math.unavailable)
        { result.coordinate.clear(); result.status = V4FamilyCoordinateStatus::resource_unavailable; result.reason = "v4_coordinate_budget_unavailable"; return result; }
        result.status = V4FamilyCoordinateStatus::ready;
        return result;
    }
    catch (const std::bad_alloc&) { result.coordinate.clear(); result.status = V4FamilyCoordinateStatus::resource_unavailable; result.reason = "v4_coordinate_resource_unavailable"; return result; }
    catch (const std::length_error&) { result.coordinate.clear(); result.status = V4FamilyCoordinateStatus::resource_unavailable; result.reason = "v4_coordinate_resource_unavailable"; return result; }
}

inline V4FamilyCoordinateStep NextV4FamilyCoordinate(std::string_view coordinate, const V4CoordinateBudget& budget)
{
    V4FamilyCoordinateStep result;
    if (!V4CanonicalDecimal(coordinate)) { result.reason = "v4_coordinate_decimal_invalid"; return result; }
    try
    {
        v4_coordinate_detail::Math math{budget};
        std::string payload(coordinate);
        if (!math.Digits(payload) || !math.Work())
        { result.status = V4FamilyCoordinateStatus::resource_unavailable; result.reason = "v4_coordinate_budget_unavailable"; return result; }
        const bool activeB = Divide(payload, 2) != 0;
        auto values = math.Uninterleave(std::move(payload), activeB ? 44 : 22);
        if (!math.unavailable && !values.empty())
        {
            V4FamilyDescriptor family;
            family.a = v4_coordinate_detail::DecodeTask(math, values, 0);
            if (activeB && !math.unavailable && !math.invalid) family.b = v4_coordinate_detail::DecodeTask(math, values, 22);
            if (!math.unavailable && !math.invalid && V4FamilySlotsCanonical(family)) result.family = std::move(family);
        }
        if (math.unavailable)
        { result.status = V4FamilyCoordinateStatus::resource_unavailable; result.reason = "v4_coordinate_budget_unavailable"; return result; }
        result.coordinate = coordinate;
        result.nextCoordinate = Add(std::string(coordinate), "1");
        if (!math.Digits(result.nextCoordinate))
        { result.coordinate.clear(); result.nextCoordinate.clear(); result.family.reset(); result.status = V4FamilyCoordinateStatus::resource_unavailable; result.reason = "v4_coordinate_budget_unavailable"; return result; }
        result.status = result.family ? V4FamilyCoordinateStatus::ready : V4FamilyCoordinateStatus::encoding_hole;
        if (!result.family) result.reason = "v4_coordinate_encoding_hole";
        return result;
    }
    catch (const std::bad_alloc&) { result.family.reset(); result.status = V4FamilyCoordinateStatus::resource_unavailable; result.reason = "v4_coordinate_resource_unavailable"; return result; }
    catch (const std::length_error&) { result.family.reset(); result.status = V4FamilyCoordinateStatus::resource_unavailable; result.reason = "v4_coordinate_resource_unavailable"; return result; }
}

// Initial, versioned structural layer over the exact task-field tuple. The
// five list-valued fields still use their existing natural encodings: this is
// not yet direct traversal of individual slot fields or evidence of coverage.
// Each finite tuple appears once: sum defines the layer, active B selects 11
// or 22 fields, and within each branch all weak compositions are enumerated.
inline constexpr std::string_view V4FieldLayerAlgorithm = "v4_field_sum_layer_v1";
struct V4FieldLayerCursor
{
    std::string layer = "0";
    bool activeB = false;
    std::vector<std::string> fields = std::vector<std::string>(11, "0");
};
enum class V4FieldLayerStatus { ready, encoding_hole, deferred, invalid, unavailable, resource_unavailable };
struct V4FieldLayerStep
{
    V4FieldLayerStatus status = V4FieldLayerStatus::invalid;
    V4FieldLayerCursor current, next;
    std::optional<V4FamilyDescriptor> family;
    std::string coordinate;
    std::string_view reason;
};

inline bool V4FieldLayerCursorCanonical(const V4FieldLayerCursor& cursor)
{
    if (!V4CanonicalDecimal(cursor.layer) || cursor.fields.size() != (cursor.activeB ? 22u : 11u)) return false;
    std::string sum = "0";
    for (const auto& field : cursor.fields)
    {
        if (!V4CanonicalDecimal(field)) return false;
        sum = Add(std::move(sum), field);
    }
    return sum == cursor.layer;
}

inline V4FieldLayerStep NextV4FieldLayer(const V4FieldLayerCursor& cursor, const V4CoordinateBudget& budget)
{
    V4FieldLayerStep result;
    try
    {
        if (!V4CanonicalDecimal(cursor.layer) || cursor.fields.size() != (cursor.activeB ? 22u : 11u))
        { result.reason = "v4_field_layer_cursor_invalid"; return result; }
        if (!budget.decimalDigits || cursor.layer.size() > budget.decimalDigits)
        { result.status = V4FieldLayerStatus::resource_unavailable; result.reason = "v4_field_layer_digits_unavailable"; return result; }
        std::string sum = "0";
        for (const auto& field : cursor.fields)
        {
            if (!V4CanonicalDecimal(field)) { result.reason = "v4_field_layer_field_invalid"; return result; }
            if (field.size() > budget.decimalDigits)
            { result.status = V4FieldLayerStatus::resource_unavailable; result.reason = "v4_field_layer_digits_unavailable"; return result; }
            sum = Add(std::move(sum), field);
            if (sum.size() > budget.decimalDigits)
            { result.status = V4FieldLayerStatus::resource_unavailable; result.reason = "v4_field_layer_digits_unavailable"; return result; }
        }
        if (sum != cursor.layer) { result.reason = "v4_field_layer_sum_invalid"; return result; }
        result.current = cursor; result.next = cursor;
        std::string suffix = cursor.fields.back();
        bool advanced = false;
        for (std::size_t i = cursor.fields.size() - 1; i-- > 0;)
        {
            if (suffix != "0")
            {
                result.next.fields[i] = Add(cursor.fields[i], "1");
                for (std::size_t j = i + 1; j + 1 < cursor.fields.size(); ++j) result.next.fields[j] = "0";
                result.next.fields.back() = Subtract(suffix, "1");
                advanced = true; break;
            }
            suffix = Add(std::move(suffix), cursor.fields[i]);
        }
        if (!advanced)
        {
            if (!cursor.activeB) result.next.activeB = true;
            else { result.next.activeB = false; result.next.layer = Add(cursor.layer, "1"); }
            if (result.next.layer.size() > budget.decimalDigits)
            { result.status = V4FieldLayerStatus::resource_unavailable; result.reason = "v4_field_layer_next_unavailable"; return result; }
            result.next.fields.assign(result.next.activeB ? 22u : 11u, "0");
            result.next.fields.back() = result.next.layer;
        }
        // V13 field tuples have 11/22 fields. V4 family identity now has
        // twenty-two per task; preserve old cursors as audit only, never decode
        // them with the new layout or silently reinterpret their IDs.
        if (cursor.fields.size() != (cursor.activeB ? 44u : 22u))
        { result.status = V4FieldLayerStatus::unavailable; result.reason = "v4_legacy_field_layer_layout_unavailable"; return result; }
        v4_coordinate_detail::Math math{budget};
        V4FamilyDescriptor family;
        family.a = v4_coordinate_detail::DecodeTask(math, cursor.fields, 0);
        if (cursor.activeB && !math.unavailable && !math.invalid)
            family.b = v4_coordinate_detail::DecodeTask(math, cursor.fields, 22);
        if (math.unavailable)
        { result.status = V4FieldLayerStatus::deferred; result.reason = "v4_field_layer_decode_budget_unavailable"; return result; }
        if (math.invalid || !V4FamilySlotsCanonical(family))
        { result.status = V4FieldLayerStatus::encoding_hole; result.reason = "v4_field_layer_encoding_hole"; return result; }
        const auto encoded = EncodeV4FamilyCoordinate(family, budget);
        if (encoded.status == V4FamilyCoordinateStatus::resource_unavailable)
        { result.status = V4FieldLayerStatus::deferred; result.reason = encoded.reason; return result; }
        if (encoded.status != V4FamilyCoordinateStatus::ready)
        { result.reason = "v4_field_layer_reencode_invalid"; return result; }
        result.status = V4FieldLayerStatus::ready; result.coordinate = encoded.coordinate; result.family = std::move(family);
        return result;
    }
    catch (const std::bad_alloc&)
    { result.status = V4FieldLayerStatus::resource_unavailable; result.reason = "v4_field_layer_allocation_unavailable"; return result; }
    catch (const std::length_error&)
    { result.status = V4FieldLayerStatus::resource_unavailable; result.reason = "v4_field_layer_allocation_unavailable"; return result; }
}

// A deterministic discovery hint, not a separate model or coverage slice.
// It uses the same descriptor and coordinate codec as the complete family
// space; its eventual discovery by the full traversal must be deduplicated.
inline V4FamilyDescriptor V4FirstDiscoverySeed()
{
    V4FamilyDescriptor family;
    family.a.kind = V4SourceKind::content_only;
    family.a.preparation.mode = V4SelectionPreparationMode::none;
    family.a.selectionCount = {V3CountKind::exact, "1"};
    family.a.fileCount = {V3CountKind::not_applicable, {}};
    family.a.virtualCount = {V3CountKind::not_applicable, {}};
    family.a.hover = {V3CountKind::unknown, {}};
    family.a.resolved = {V3CountKind::unknown, {}};
    family.a.payloads.push_back({"text_unicode", "hglobal", "default", V4PayloadKind::exact,
        "53006e006f0077002000ea960000"});
    return family;
}

// Additional independent result hypotheses for the same controlled input.
// These are discovery hints only; they do not configure or predict the host.
inline constexpr std::size_t V4AdditionalTextSeedCount = 4;
inline std::optional<V4FamilyDescriptor> V4AdditionalTextSeed(std::size_t ordinal)
{
    if (ordinal >= V4AdditionalTextSeedCount) return std::nullopt;
    auto family = V4FirstDiscoverySeed();
    family.a.resolved = ordinal == 0 ? V3CountValue{V3CountKind::not_applicable, {}} :
        V3CountValue{V3CountKind::exact, std::to_string(ordinal - 1)};
    return family;
}

// Typed prefix grammar for the exact same V4 descriptor. A token chooses one
// alternative at the current grammar position; 0 terminates variable-length
// sequences, never denotes an unknown or a product decision. Completed lists
// are checked for the descriptor's canonical sort/uniqueness only. This API
// is a parsing primitive: until a bounded DFS and scheduler transaction use
// it, it does not represent enumeration or execution coverage.
inline constexpr std::string_view V4StructuredPrefixAlgorithm = "v4_typed_family_prefix_v11_lua_permissions";
enum class V4StructuredPrefixStatus { need_token, complete, noncanonical, resource_unavailable };
struct V4StructuredPrefixResult
{
    V4StructuredPrefixStatus status = V4StructuredPrefixStatus::noncanonical;
    std::uint32_t nextMaximum = 0;
    std::optional<V4FamilyDescriptor> family;
    std::string_view reason;
};
namespace v4_structured_detail
{
struct Reader
{
    const std::vector<std::uint32_t>& tokens;
    std::size_t at = 0;
    bool need = false, invalid = false;
    std::uint32_t maximum = 0;
    bool fileEquivalenceClasses = false;
    bool Read(std::uint32_t max, std::uint32_t& value)
    {
        if (need || invalid) return false;
        if (at == tokens.size()) { need = true; maximum = max; return false; }
        value = tokens[at++];
        if (value > max) { invalid = true; return false; }
        return true;
    }
    bool Decimal(std::string& out)
    {
        std::uint32_t token = 0;
        while (Read(10, token))
        {
            if (!token) { if (out.empty()) invalid = true; return !invalid; }
            const char digit = static_cast<char>('0' + token - 1);
            if (out.size() == 1 && out.front() == '0') { invalid = true; return false; }
            out.push_back(digit);
        }
        return false;
    }
    bool Count(V3CountValue& count)
    {
        std::uint32_t tag = 0; if (!Read(2, tag)) return false;
        count.kind = tag == 0 ? V3CountKind::not_applicable : tag == 1 ? V3CountKind::unknown : V3CountKind::exact;
        return tag != 2 || Decimal(count.decimal);
    }
    template <std::size_t N>
    bool Symbol(std::string& value, const std::string_view (&known)[N])
    {
        std::uint32_t tag = 0; if (!Read(static_cast<std::uint32_t>(N), tag)) return false;
        if (tag < N) { value = known[tag]; return true; }
        constexpr std::string_view chars = "abcdefghijklmnopqrstuvwxyz0123456789_";
        std::uint32_t token = 0;
        while (Read(static_cast<std::uint32_t>(chars.size()), token))
        {
            if (!token)
            {
                if (value.empty()) invalid = true;
                for (const auto entry : known) if (entry == std::string_view(value)) invalid = true;
                return !invalid;
            }
            value.push_back(chars[token - 1]);
        }
        return false;
    }
    bool Index(std::string& value)
    {
        std::uint32_t tag = 0; if (!Read(2, tag)) return false;
        if (tag == 0) { value = "default"; return true; }
        if (tag == 1) { value = "unknown_unresolved"; return true; }
        std::string number; if (!Decimal(number)) return false;
        value = "index_" + number; return true;
    }
    bool Kind(V4PayloadKind& kind)
    {
        std::uint32_t tag = 0; if (!Read(2, tag)) return false;
        kind = static_cast<V4PayloadKind>(tag); return true;
    }
    static void Byte(std::string& hex, std::uint32_t byte)
    {
        constexpr char digits[] = "0123456789abcdef";
        hex.push_back(digits[(byte >> 4) & 15]); hex.push_back(digits[byte & 15]);
    }
    bool Bytes(V4PayloadKind kind, std::string& hex, bool utf16)
    {
        if (kind != V4PayloadKind::exact) return true;
        std::uint32_t token = 0;
        while (Read(utf16 ? 65536u : 256u, token))
        {
            if (!token) return true;
            const auto value = token - 1;
            Byte(hex, value & 255u); if (utf16) Byte(hex, value >> 8);
        }
        return false;
    }
    // Name, alias, type and path relations come from the independent P/R/V
    // axes. This is a common placeholder, never an actual bound path.
    bool ClassPath(V4PayloadKind kind, std::string& hex, std::string_view index)
    {
        if (!fileEquivalenceClasses) return Bytes(kind, hex, true);
        if (kind != V4PayloadKind::exact) return true;
        (void)index;
        for (char ch : std::string_view("class_placeholder")) { Byte(hex, static_cast<unsigned char>(ch)); Byte(hex, 0); }
        return true;
    }
    // Directory-node paths do not have per-node P/R topology axes. Enumerate
    // symbolic parent components and shared/unique/case-alias leaf names;
    // each component is an arbitrary ordinal, not arbitrary path bytes.
    bool ClassDirectoryPath(V4PayloadKind kind, std::string& hex, std::string_view index)
    {
        if (!fileEquivalenceClasses) return Bytes(kind, hex, true);
        if (kind != V4PayloadKind::exact) return true;
        const auto append = [&](std::string_view text) {
            for (const char ch : text) { Byte(hex, static_cast<unsigned char>(ch)); Byte(hex, 0); }
        };
        std::uint32_t more = 0;
        while (Read(1, more))
        {
            if (!more) break;
            std::string parent; if (!Decimal(parent)) return false;
            append("dir_"); append(parent); append("\\");
        }
        if (need || invalid) return false;
        std::uint32_t leaf = 0; if (!Read(2, leaf)) return false;
        if (leaf == 0) append("item");
        else if (leaf == 1) { append("item_"); append(index); }
        else append("ITEM");
        return true;
    }
    bool ClassFileBytes(V4PayloadKind kind, std::string& hex)
    {
        if (!fileEquivalenceClasses) return Bytes(kind, hex, false);
        if (kind != V4PayloadKind::exact) return true;
        std::uint32_t value = 0; if (!Read(1, value)) return false;
        if (value) Byte(hex, 0x41); // typical nonempty content, not every byte sequence
        return true;
    }
    bool RequestBytes(V4PayloadKind kind, std::string& hex)
    {
        if (kind != V4PayloadKind::exact) return true;
        for (unsigned i = 0; i < 4; ++i) { std::uint32_t byte = 0; if (!Read(255, byte)) return false; Byte(hex, byte); }
        return true;
    }
    bool Task(V4TaskSpec& task)
    {
        std::uint32_t token = 0; if (!Read(4, token)) return false;
        task.kind = static_cast<V4SourceKind>(token);
        for (auto* count : {&task.selectionCount, &task.fileCount, &task.virtualCount, &task.hover, &task.resolved})
            if (!Count(*count)) return false;
        for (;;)
        {
            if (!Read(1, token)) return false; if (!token) break;
            V4PayloadSlot slot;
            if (!Symbol(slot.format, v4_coordinate_detail::Formats) || !Symbol(slot.medium, v4_coordinate_detail::Media) ||
                !Index(slot.index) || !Kind(slot.kind)) return false;
            if (slot.format == "file_contents" ? !ClassFileBytes(slot.kind, slot.hex) : !Bytes(slot.kind, slot.hex, false)) return false;
            task.payloads.push_back(std::move(slot));
        }
        for (auto* slots : {&task.filePaths, &task.virtualNames})
            for (;;)
            {
                if (!Read(1, token)) return false; if (!token) break;
                V4IndexedUtf16Slot slot;
                if (!Decimal(slot.index) || !Kind(slot.kind) || !ClassPath(slot.kind, slot.utf16leHex, slot.index) || !Read(1, token)) return false;
                slot.base = static_cast<V4PathBase>(token);
                slots->push_back(std::move(slot));
            }
        for (;;)
        {
            if (!Read(1, token)) return false; if (!token) break;
            V4RequestResultSlot slot;
            if (!Symbol(slot.format, v4_coordinate_detail::Formats) || !Symbol(slot.medium, v4_coordinate_detail::Media) ||
                !Index(slot.index) || !Read(2, token)) return false;
            slot.phase = token == 0 ? "query" : token == 1 ? "get" : "read";
            if (!Kind(slot.kind) || !RequestBytes(slot.kind, slot.hresultHex)) return false;
            task.requestResults.push_back(std::move(slot));
        }
        for (;;)
        {
            if (!Read(1, token)) return false; if (!token) break;
            V4IndexedBytesSlot slot;
            if (!Decimal(slot.index) || !Kind(slot.kind) || !ClassFileBytes(slot.kind, slot.hex)) return false;
            task.fileContents.push_back(std::move(slot));
        }
        for (;;)
        {
            if (!Read(1, token)) return false; if (!token) break;
            V4DirectoryRootSlot root;
            if (!Decimal(root.rootIndex) || !Read(3, token)) return false;
            root.completeness = static_cast<V4DirectoryCompleteness>(token);
            task.directoryRoots.push_back(std::move(root));
        }
        for (;;)
        {
            if (!Read(1, token)) return false; if (!token) break;
            V4DirectoryNodeSlot node;
            if (!Decimal(node.rootIndex) || !Decimal(node.slotIndex) || !Kind(node.pathKind) ||
                !ClassDirectoryPath(node.pathKind, node.relativePathUtf16Hex, node.slotIndex) || !Read(3, token)) return false;
            node.nodeKind = static_cast<V4DirectoryNodeKind>(token);
            if (!Kind(node.contentKind) || !ClassFileBytes(node.contentKind, node.contentHex) || !Count(node.objectIdentity)) return false;
            task.directoryNodes.push_back(std::move(node));
        }
        if (!Read(7, token)) return false;
        task.preparation.mode = static_cast<V4SelectionPreparationMode>(token);
        for (;;)
        {
            if (!Read(1, token)) return false; if (!token) break;
            V4SelectionPreparationStep step;
            if (!Read(4, token)) return false; step.action = static_cast<V4SelectionPreparationAction>(token);
            if (!Read(4, token)) return false; step.button = static_cast<V4SelectionPreparationButton>(token);
            if (!Count(step.session) || !Kind(step.modifiersKind) || !RequestBytes(step.modifiersKind, step.modifiersHex) ||
                !Count(step.member) || !Kind(step.xKind) || !RequestBytes(step.xKind, step.xHex) ||
                !Kind(step.yKind) || !RequestBytes(step.yKind, step.yHex)) return false;
            task.preparation.steps.push_back(std::move(step));
        }
        for (;;)
        {
            if (!Read(1, token)) return false; if (!token) break;
            V4SelectionOutsideIdentity outside;
            if (!Decimal(outside.index) || !Read(8, token)) return false;
            outside.scope = static_cast<V4SelectionOutsideScope>(token);
            if (!Kind(outside.identityKind) || !Bytes(outside.identityKind, outside.identityHex, false) || !Count(outside.alias)) return false;
            task.preparation.outsideIdentities.push_back(std::move(outside));
        }
        for (;;)
        {
            if (!Read(1, token)) return false; if (!token) break;
            V4DimensionSlot dimension;
            if (!Decimal(dimension.index) || !Read(9, token)) return false;
            dimension.target = static_cast<V4DimensionTarget>(token);
            if (!Count(dimension.member) || !Kind(dimension.kind) || !RequestBytes(dimension.kind, dimension.valueHex)) return false;
            task.dimensions.push_back(std::move(dimension));
        }
        for (;;)
        {
            if (!Read(1, token)) return false; if (!token) break;
            V4PositionSlot position;
            if (!Decimal(position.index) || !Read(1, token)) return false;
            position.target = static_cast<V4PositionTarget>(token);
            if (!Count(position.member) || !Count(position.position)) return false;
            task.positions.push_back(std::move(position));
        }
        if (!Kind(task.categoryRulesKind)) return false;
        if (task.categoryRulesKind == V4PayloadKind::exact)
            for (;;)
            {
                if (!Read(1, token)) return false; if (!token) break;
                V4CategoryRuleInput rule;
                if (!Kind(rule.idKind) || !Bytes(rule.idKind, rule.idUtf16Hex, true) ||
                    !Kind(rule.labelKind) || !Bytes(rule.labelKind, rule.labelUtf16Hex, true) ||
                    !Kind(rule.extensionsKind) || !Bytes(rule.extensionsKind, rule.extensionsUtf16Hex, true) || !Read(3, token)) return false;
                rule.enabled = static_cast<V4InputBoolean>(token); task.categoryRules.push_back(std::move(rule));
            }
        for (;;)
        {
            if (!Read(1, token)) return false; if (!token) break;
            V4ShortcutTargetInput shortcut;
            if (!Decimal(shortcut.index) || !Read(3, token)) return false; shortcut.classified = static_cast<V4InputBoolean>(token);
            if (!Read(3, token)) return false; shortcut.directory = static_cast<V4InputBoolean>(token);
            if (!Read(3, token)) return false; shortcut.application = static_cast<V4InputBoolean>(token);
            if (!Kind(shortcut.extensionKind) || !Bytes(shortcut.extensionKind, shortcut.extensionUtf16Hex, true)) return false;
            task.shortcutTargets.push_back(std::move(shortcut));
        }
        for (auto* group : {&task.sourceGroup, &task.targetGroup})
        {
            if (!Kind(group->childrenKind)) return false;
            if (group->childrenKind == V4PayloadKind::exact)
                for (;;)
                {
                    if (!Read(1, token)) return false; if (!token) break;
                    V4GroupChildInput child;
                    if (!Kind(child.identityKind) || !Bytes(child.identityKind, child.idUtf16Hex, true) ||
                        !Kind(child.typeKind) || !RequestBytes(child.typeKind, child.typeHex)) return false;
                    group->children.push_back(std::move(child));
                }
            if (!Count(group->activeChild) || !Count(group->hitChild)) return false;
        }
        return true;
    }
};
} // namespace v4_structured_detail

inline V4StructuredPrefixResult InspectV4StructuredPrefix(const std::vector<std::uint32_t>& prefix, std::size_t tokenBudget,
    bool fileEquivalenceClasses = false)
{
    V4StructuredPrefixResult result;
    if (prefix.size() > tokenBudget) { result.status = V4StructuredPrefixStatus::resource_unavailable; result.reason = "v4_prefix_token_budget_unavailable"; return result; }
    try
    {
        v4_structured_detail::Reader reader{prefix}; reader.fileEquivalenceClasses = fileEquivalenceClasses;
        std::uint32_t activeB = 0; V4FamilyDescriptor family;
        if (reader.Read(1, activeB) && reader.Task(family.a))
        {
            if (activeB) { V4TaskSpec second; if (reader.Task(second)) family.b = std::move(second); }
            if (!reader.need && !reader.invalid && reader.at == prefix.size() && V4FamilySlotsCanonical(family))
            { result.status = V4StructuredPrefixStatus::complete; result.family = std::move(family); return result; }
        }
        if (reader.need && !reader.invalid)
        { result.status = V4StructuredPrefixStatus::need_token; result.nextMaximum = reader.maximum; return result; }
        result.reason = "v4_prefix_noncanonical"; return result;
    }
    catch (const std::bad_alloc&) { result.status = V4StructuredPrefixStatus::resource_unavailable; result.reason = "v4_prefix_allocation_unavailable"; return result; }
    catch (const std::length_error&) { result.status = V4StructuredPrefixStatus::resource_unavailable; result.reason = "v4_prefix_allocation_unavailable"; return result; }
}

struct V4StructuredPrefixEncoding
{
    bool encoded = false, resourceUnavailable = false;
    std::vector<std::uint32_t> tokens;
    std::string_view reason;
};
// Reverse mapping used for replay of an already identified family. It emits
// the same typed grammar as DFS, not an ordinal or a claim of traversal.
inline V4StructuredPrefixEncoding EncodeV4StructuredPrefix(const V4FamilyDescriptor& family, std::size_t tokenBudget)
{
    V4StructuredPrefixEncoding out;
    if (!V4FamilySlotsCanonical(family)) { out.reason = "v4_prefix_family_invalid"; return out; }
    try
    {
        bool full = false;
        const auto put = [&](std::uint32_t value) {
            if (out.tokens.size() >= tokenBudget) { full = true; return; }
            out.tokens.push_back(value);
        };
        const auto decimal = [&](std::string_view value) {
            for (char ch : value) { put(static_cast<std::uint32_t>(ch - '0') + 1u); if (full) return; }
            put(0);
        };
        const auto count = [&](const V3CountValue& value) {
            put(static_cast<std::uint32_t>(value.kind));
            if (value.kind == V3CountKind::exact && !full) decimal(value.decimal);
        };
        const auto symbol = [&](std::string_view value, const auto& known) {
            const auto knownCount = sizeof(known) / sizeof(known[0]);
            for (std::size_t i = 0; i < knownCount; ++i)
                if (known[i] == value) { put(static_cast<std::uint32_t>(i)); return; }
            put(static_cast<std::uint32_t>(knownCount));
            constexpr std::string_view chars = "abcdefghijklmnopqrstuvwxyz0123456789_";
            for (char ch : value)
            {
                const auto position = chars.find(ch);
                if (position == std::string_view::npos) { full = true; return; }
                put(static_cast<std::uint32_t>(position) + 1u); if (full) return;
            }
            put(0);
        };
        const auto index = [&](std::string_view value) {
            if (value == "default") put(0);
            else if (value == "unknown_unresolved") put(1);
            else { put(2); if (!full) decimal(value.substr(6)); }
        };
        const auto hexValue = [](char c) -> std::uint32_t { return static_cast<std::uint32_t>(c <= '9' ? c - '0' : c - 'a' + 10); };
        const auto byteAt = [&](std::string_view hex, std::size_t i) { return (hexValue(hex[i]) << 4) | hexValue(hex[i + 1]); };
        const auto bytes = [&](V4PayloadKind kind, std::string_view hex, bool utf16) {
            if (kind != V4PayloadKind::exact) return;
            const std::size_t stride = utf16 ? 4 : 2;
            for (std::size_t i = 0; i < hex.size(); i += stride)
            {
                auto value = byteAt(hex, i);
                if (utf16) value |= byteAt(hex, i + 2) << 8;
                put(value + 1u); if (full) return;
            }
            put(0);
        };
        const auto task = [&](const V4TaskSpec& item) {
            put(static_cast<std::uint32_t>(item.kind));
            for (const auto* value : {&item.selectionCount, &item.fileCount, &item.virtualCount, &item.hover, &item.resolved})
            { if (full) return; count(*value); }
            for (const auto& slot : item.payloads)
            {
                if (full) return; put(1); symbol(slot.format, v4_coordinate_detail::Formats); symbol(slot.medium, v4_coordinate_detail::Media);
                index(slot.index); put(static_cast<std::uint32_t>(slot.kind)); bytes(slot.kind, slot.hex, false);
            }
            put(0);
            for (const auto* list : {&item.filePaths, &item.virtualNames})
            {
                for (const auto& slot : *list)
                { if (full) return; put(1); decimal(slot.index); put(static_cast<std::uint32_t>(slot.kind)); bytes(slot.kind, slot.utf16leHex, true); put(static_cast<std::uint32_t>(slot.base)); }
                put(0);
            }
            for (const auto& slot : item.requestResults)
            {
                if (full) return; put(1); symbol(slot.format, v4_coordinate_detail::Formats); symbol(slot.medium, v4_coordinate_detail::Media);
                index(slot.index); put(slot.phase == "query" ? 0u : slot.phase == "get" ? 1u : 2u);
                put(static_cast<std::uint32_t>(slot.kind));
                if (slot.kind == V4PayloadKind::exact)
                    for (std::size_t i = 0; i < slot.hresultHex.size(); i += 2) { put(byteAt(slot.hresultHex, i)); if (full) return; }
            }
            put(0);
            for (const auto& slot : item.fileContents)
            { if (full) return; put(1); decimal(slot.index); put(static_cast<std::uint32_t>(slot.kind)); bytes(slot.kind, slot.hex, false); }
            put(0);
            for (const auto& root : item.directoryRoots)
            { if (full) return; put(1); decimal(root.rootIndex); put(static_cast<std::uint32_t>(root.completeness)); }
            put(0);
            for (const auto& node : item.directoryNodes)
            {
                if (full) return; put(1); decimal(node.rootIndex); decimal(node.slotIndex);
                put(static_cast<std::uint32_t>(node.pathKind)); bytes(node.pathKind, node.relativePathUtf16Hex, true);
                put(static_cast<std::uint32_t>(node.nodeKind)); put(static_cast<std::uint32_t>(node.contentKind));
                bytes(node.contentKind, node.contentHex, false); count(node.objectIdentity);
            }
            put(0);
            put(static_cast<std::uint32_t>(item.preparation.mode));
            for (const auto& step : item.preparation.steps)
            {
                if (full) return; put(1); put(static_cast<std::uint32_t>(step.action)); put(static_cast<std::uint32_t>(step.button));
                count(step.session); put(static_cast<std::uint32_t>(step.modifiersKind));
                if (step.modifiersKind == V4PayloadKind::exact)
                    for (std::size_t i = 0; i < step.modifiersHex.size(); i += 2) { put(byteAt(step.modifiersHex, i)); if (full) return; }
                count(step.member);
                for (const auto& coordinate : {std::pair{step.xKind, &step.xHex}, std::pair{step.yKind, &step.yHex}})
                {
                    put(static_cast<std::uint32_t>(coordinate.first));
                    if (coordinate.first == V4PayloadKind::exact)
                        for (std::size_t i = 0; i < coordinate.second->size(); i += 2) { put(byteAt(*coordinate.second, i)); if (full) return; }
                }
            }
            put(0);
            for (const auto& outside : item.preparation.outsideIdentities)
            {
                if (full) return; put(1); decimal(outside.index); put(static_cast<std::uint32_t>(outside.scope));
                put(static_cast<std::uint32_t>(outside.identityKind)); bytes(outside.identityKind, outside.identityHex, false);
                count(outside.alias);
            }
            put(0);
            for (const auto& dimension : item.dimensions)
            {
                if (full) return; put(1); decimal(dimension.index); put(static_cast<std::uint32_t>(dimension.target));
                count(dimension.member); put(static_cast<std::uint32_t>(dimension.kind));
                if (dimension.kind == V4PayloadKind::exact)
                    for (std::size_t i = 0; i < dimension.valueHex.size(); i += 2) { put(byteAt(dimension.valueHex, i)); if (full) return; }
            }
            put(0);
            for (const auto& position : item.positions)
            {
                if (full) return; put(1); decimal(position.index); put(static_cast<std::uint32_t>(position.target));
                count(position.member); count(position.position);
            }
            put(0);
            put(static_cast<std::uint32_t>(item.categoryRulesKind));
            if (item.categoryRulesKind == V4PayloadKind::exact)
            {
                for (const auto& rule : item.categoryRules)
                {
                    if (full) return; put(1); put(static_cast<std::uint32_t>(rule.idKind)); bytes(rule.idKind, rule.idUtf16Hex, true);
                    put(static_cast<std::uint32_t>(rule.labelKind)); bytes(rule.labelKind, rule.labelUtf16Hex, true);
                    put(static_cast<std::uint32_t>(rule.extensionsKind)); bytes(rule.extensionsKind, rule.extensionsUtf16Hex, true);
                    put(static_cast<std::uint32_t>(rule.enabled));
                }
                put(0);
            }
            for (const auto& shortcut : item.shortcutTargets)
            {
                if (full) return; put(1); decimal(shortcut.index); put(static_cast<std::uint32_t>(shortcut.classified));
                put(static_cast<std::uint32_t>(shortcut.directory)); put(static_cast<std::uint32_t>(shortcut.application));
                put(static_cast<std::uint32_t>(shortcut.extensionKind)); bytes(shortcut.extensionKind, shortcut.extensionUtf16Hex, true);
            }
            put(0);
            for (const auto* group : {&item.sourceGroup, &item.targetGroup})
            {
                put(static_cast<std::uint32_t>(group->childrenKind));
                if (group->childrenKind == V4PayloadKind::exact)
                {
                    for (const auto& child : group->children)
                    {
                        if (full) return; put(1); put(static_cast<std::uint32_t>(child.identityKind));
                        bytes(child.identityKind, child.idUtf16Hex, true);
                        put(static_cast<std::uint32_t>(child.typeKind));
                        if (child.typeKind == V4PayloadKind::exact)
                            for (std::size_t i = 0; i < child.typeHex.size(); i += 2) { put(byteAt(child.typeHex, i)); if (full) return; }
                    }
                    put(0);
                }
                count(group->activeChild); count(group->hitChild);
            }
        };
        put(family.b ? 1u : 0u); if (!full) task(family.a); if (family.b && !full) task(*family.b);
        if (full) { out.tokens.clear(); out.resourceUnavailable = true; out.reason = "v4_prefix_token_budget_unavailable"; return out; }
        const auto checked = InspectV4StructuredPrefix(out.tokens, tokenBudget);
        if (checked.status == V4StructuredPrefixStatus::resource_unavailable)
        { out.tokens.clear(); out.resourceUnavailable = true; out.reason = checked.reason; return out; }
        if (checked.status != V4StructuredPrefixStatus::complete || !checked.family ||
            V4FamilyBinding(*checked.family) != V4FamilyBinding(family))
        { out.tokens.clear(); out.reason = "v4_prefix_internal_encoding_invalid"; return out; }
        out.encoded = true; return out;
    }
    catch (const std::bad_alloc&) { out.tokens.clear(); out.resourceUnavailable = true; out.reason = "v4_prefix_allocation_unavailable"; return out; }
    catch (const std::length_error&) { out.tokens.clear(); out.resourceUnavailable = true; out.reason = "v4_prefix_allocation_unavailable"; return out; }
}

// One resumable bounded DFS within a fixed token-count layer. `stopDepth`
// fences a deferred subtree from re-enumerating its ancestors. A caller must
// persist the output, successor, and every deferred subtree atomically.
inline constexpr std::string_view V4StructuredDfsAlgorithm = "v4_typed_family_dfs_v11_lua_permissions";
struct V4StructuredDfsCursor
{
    std::size_t layer = 0, stopDepth = 0;
    std::vector<std::uint32_t> prefix;
    std::vector<std::uint32_t> nextChoice{0};
    bool exhausted = false;
};
enum class V4StructuredDfsStatus { progress, emitted, grammar_pruned, deferred, exhausted, invalid, resource_unavailable };
struct V4StructuredDfsStep
{
    V4StructuredDfsStatus status = V4StructuredDfsStatus::invalid;
    V4StructuredDfsCursor next;
    std::optional<V4StructuredDfsCursor> deferred;
    std::optional<V4FamilyDescriptor> family;
    std::vector<std::uint32_t> prunedPrefix;
    std::string coordinate;
    std::size_t workUsed = 0;
    std::string_view reason;
};
inline V4StructuredDfsStep StepV4StructuredDfs(const V4StructuredDfsCursor& input,
    const V4CoordinateBudget& coordinateBudget, std::size_t tokenBudget, std::size_t workBudget,
    bool fileEquivalenceClasses = false)
{
    V4StructuredDfsStep result;
    try
    {
        if (input.stopDepth > input.prefix.size() || input.prefix.size() > input.layer ||
            input.nextChoice.size() != input.prefix.size() + 1 || (input.exhausted && input.prefix.size() != input.stopDepth))
        { result.reason = "v4_structured_cursor_shape_invalid"; return result; }
        if (input.prefix.size() > tokenBudget)
        { result.status = V4StructuredDfsStatus::resource_unavailable; result.reason = "v4_structured_prefix_budget_unavailable"; return result; }
        // Reconstruct every branch decision rather than trusting a persisted
        // frame. This validates syntax, not the history of a checkpoint.
        std::vector<std::uint32_t> checked;
        checked.reserve(input.prefix.size());
        for (std::size_t i = 0; i < input.prefix.size(); ++i)
        {
            const auto node = InspectV4StructuredPrefix(checked, tokenBudget, fileEquivalenceClasses);
            if (node.status == V4StructuredPrefixStatus::resource_unavailable)
            { result.status = V4StructuredDfsStatus::resource_unavailable; result.reason = node.reason; return result; }
            if (node.status != V4StructuredPrefixStatus::need_token || input.prefix[i] > node.nextMaximum ||
                input.nextChoice[i] != input.prefix[i] + 1u)
            { result.reason = "v4_structured_cursor_branch_invalid"; return result; }
            checked.push_back(input.prefix[i]);
        }
        const auto top = InspectV4StructuredPrefix(input.prefix, tokenBudget, fileEquivalenceClasses);
        if (top.status == V4StructuredPrefixStatus::resource_unavailable)
        { result.status = V4StructuredDfsStatus::resource_unavailable; result.reason = top.reason; return result; }
        if ((top.status == V4StructuredPrefixStatus::need_token && input.nextChoice.back() > top.nextMaximum + 1u) ||
            (top.status != V4StructuredPrefixStatus::need_token && input.nextChoice.back() != 0))
        { result.reason = "v4_structured_cursor_choice_invalid"; return result; }
        result.next = input;
        if (input.exhausted) { result.status = V4StructuredDfsStatus::exhausted; return result; }
        const auto retreat = [](V4StructuredDfsCursor& cursor) {
            if (cursor.prefix.size() == cursor.stopDepth) { cursor.exhausted = true; return; }
            cursor.prefix.pop_back(); cursor.nextChoice.pop_back();
        };
        while (result.workUsed < workBudget)
        {
            const auto node = InspectV4StructuredPrefix(result.next.prefix, tokenBudget, fileEquivalenceClasses);
            if (node.status == V4StructuredPrefixStatus::resource_unavailable)
            { result.next = input; result.workUsed = 0; result.status = V4StructuredDfsStatus::resource_unavailable; result.reason = node.reason; return result; }
            ++result.workUsed;
            if (node.status == V4StructuredPrefixStatus::noncanonical)
            {
                result.prunedPrefix = result.next.prefix; retreat(result.next);
                result.status = V4StructuredDfsStatus::grammar_pruned; result.reason = node.reason; return result;
            }
            if (node.status == V4StructuredPrefixStatus::complete)
            {
                if (result.next.prefix.size() != result.next.layer) { retreat(result.next); if (result.next.exhausted) break; continue; }
                const auto encoded = EncodeV4FamilyCoordinate(*node.family, coordinateBudget);
                if (encoded.status == V4FamilyCoordinateStatus::resource_unavailable)
                {
                    auto pending = result.next; pending.stopDepth = pending.prefix.size(); pending.nextChoice.back() = 0;
                    result.deferred = std::move(pending); retreat(result.next);
                    result.status = V4StructuredDfsStatus::deferred; result.reason = encoded.reason; return result;
                }
                if (encoded.status != V4FamilyCoordinateStatus::ready)
                { result.next = input; result.workUsed = 0; result.reason = "v4_structured_family_reencode_invalid"; return result; }
                result.family = node.family; result.coordinate = encoded.coordinate; retreat(result.next);
                result.status = V4StructuredDfsStatus::emitted; return result;
            }
            if (result.next.prefix.size() == result.next.layer || result.next.nextChoice.back() > node.nextMaximum)
            { retreat(result.next); if (result.next.exhausted) break; continue; }
            const auto choice = result.next.nextChoice.back()++;
            result.next.prefix.push_back(choice); result.next.nextChoice.push_back(0);
            if (result.next.prefix.size() > tokenBudget)
            {
                auto pending = result.next; pending.stopDepth = pending.prefix.size();
                result.deferred = std::move(pending); retreat(result.next);
                result.status = V4StructuredDfsStatus::deferred; result.reason = "v4_structured_subtree_token_budget_unavailable"; return result;
            }
        }
        result.status = result.next.exhausted ? V4StructuredDfsStatus::exhausted : V4StructuredDfsStatus::progress;
        return result;
    }
    catch (const std::bad_alloc&) { result = {}; result.status = V4StructuredDfsStatus::resource_unavailable; result.reason = "v4_structured_dfs_allocation_unavailable"; return result; }
    catch (const std::length_error&) { result = {}; result.status = V4StructuredDfsStatus::resource_unavailable; result.reason = "v4_structured_dfs_allocation_unavailable"; return result; }
}

// Reversible enumeration of the existing V3 family descriptor, not another
// candidate generator or production executor. Decimal strings are canonical;
// budget exhaustion is retryable resource unavailability, never exclusion.
inline constexpr std::string_view V3FamilyCoordinateAlgorithm = "v3_family_bit_interleave_v1";
enum class V3FamilyCoordinateStatus { ready, invalid_coordinate, resource_unavailable };
struct V3FamilyCoordinateEncoding
{
    V3FamilyCoordinateStatus status = V3FamilyCoordinateStatus::invalid_coordinate;
    std::string coordinate;
    std::string_view reason;
};
struct V3FamilyCoordinateStep
{
    V3FamilyCoordinateStatus status = V3FamilyCoordinateStatus::invalid_coordinate;
    std::string coordinate, nextCoordinate;
    std::optional<V3FamilyDescriptor> family;
    std::string_view reason;
};
inline bool V3FamilyCanonicalDecimal(std::string_view value)
{
    return !value.empty() && (value.size() == 1 || value.front() != '0') &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= '0' && c <= '9'; });
}
inline V3FamilyCoordinateEncoding EncodeV3FamilyCoordinate(const V3FamilyDescriptor& family,
    std::size_t digitBudget)
{
    V3FamilyCoordinateEncoding result;
    const auto validTask = [](const V3ExactTaskSpec& task) {
        return V3FamilyCanonicalDecimal(task.sourceDecimal) &&
            V3CountValueCanonical(task.hover) && V3CountValueCanonical(task.resolved);
    };
    if (!validTask(family.a) || (family.b && !validTask(*family.b)))
    { result.reason = "v3_family_coordinate_descriptor_invalid"; return result; }
    try
    {
        if (!digitBudget) { result.status = V3FamilyCoordinateStatus::resource_unavailable; result.reason = "v3_family_coordinate_digit_budget"; return result; }
        const auto fits = [&](const V3ExactTaskSpec& task) {
            return task.sourceDecimal.size() <= digitBudget && task.hover.decimal.size() <= digitBudget &&
                task.resolved.decimal.size() <= digitBudget;
        };
        if (!fits(family.a) || (family.b && !fits(*family.b)))
        { result.status = V3FamilyCoordinateStatus::resource_unavailable; result.reason = "v3_family_coordinate_digit_budget"; return result; }
        const auto value = [](const V3CountValue& item) {
            if (item.kind == V3CountKind::not_applicable) return std::string("0");
            if (item.kind == V3CountKind::unknown) return std::string("1");
            return Add(item.decimal, "2");
        };
        std::vector<std::string> numbers;
        const auto append = [&](const V3ExactTaskSpec& task) {
            numbers.push_back(task.sourceDecimal); numbers.push_back(value(task.hover)); numbers.push_back(value(task.resolved));
        };
        append(family.a);
        if (family.b) append(*family.b);
        for (const auto& n : numbers)
            if (n.size() > digitBudget)
            { result.status = V3FamilyCoordinateStatus::resource_unavailable; result.reason = "v3_family_coordinate_digit_budget"; return result; }
        std::string payload = "0", weight = "1";
        while (std::any_of(numbers.begin(), numbers.end(), [](const auto& n) { return n != "0"; }))
            for (auto& number : numbers)
            {
                if (Divide(number, 2)) payload = Add(std::move(payload), weight);
                weight = Multiply(std::move(weight), 2);
                if (payload.size() > digitBudget || (weight.size() > digitBudget &&
                    std::any_of(numbers.begin(), numbers.end(), [](const auto& n) { return n != "0"; })))
                { result.status = V3FamilyCoordinateStatus::resource_unavailable; result.reason = "v3_family_coordinate_digit_budget"; return result; }
            }
        result.coordinate = Multiply(std::move(payload), 2);
        if (family.b) result.coordinate = Add(std::move(result.coordinate), "1");
        if (result.coordinate.size() > digitBudget)
        { result.coordinate.clear(); result.status = V3FamilyCoordinateStatus::resource_unavailable; result.reason = "v3_family_coordinate_digit_budget"; return result; }
        result.status = V3FamilyCoordinateStatus::ready;
        return result;
    }
    catch (const std::bad_alloc&) { result.coordinate.clear(); result.status = V3FamilyCoordinateStatus::resource_unavailable; result.reason = "v3_family_coordinate_allocation_unavailable"; }
    catch (const std::length_error&) { result.coordinate.clear(); result.status = V3FamilyCoordinateStatus::resource_unavailable; result.reason = "v3_family_coordinate_allocation_unavailable"; }
    return result;
}

inline V3FamilyCoordinateStep NextV3FamilyCoordinate(std::string_view coordinate, std::size_t digitBudget)
{
    V3FamilyCoordinateStep result;
    if (!V3FamilyCanonicalDecimal(coordinate))
    { result.reason = "v3_family_coordinate_invalid"; return result; }
    try
    {
        if (!digitBudget || coordinate.size() > digitBudget)
        { result.status = V3FamilyCoordinateStatus::resource_unavailable; result.reason = "v3_family_coordinate_digit_budget"; return result; }
        std::string payload(coordinate);
        const bool activeB = Divide(payload, 2) != 0;
        const std::size_t dimension = activeB ? 6 : 3;
        std::vector<std::string> numbers(dimension, "0"), weights(dimension, "1");
        std::size_t slot = 0;
        while (payload != "0")
        {
            if (Divide(payload, 2)) numbers[slot] = Add(std::move(numbers[slot]), weights[slot]);
            weights[slot] = Multiply(std::move(weights[slot]), 2);
            slot = (slot + 1) % dimension;
        }
        const auto decode = [](const std::string& number) {
            if (number == "0") return V3CountValue{V3CountKind::not_applicable, {}};
            if (number == "1") return V3CountValue{V3CountKind::unknown, {}};
            return V3CountValue{V3CountKind::exact, Subtract(number, "2")};
        };
        V3FamilyDescriptor family;
        family.a = {numbers[0], decode(numbers[1]), decode(numbers[2])};
        if (activeB) family.b = V3ExactTaskSpec{numbers[3], decode(numbers[4]), decode(numbers[5])};
        const auto next = Add(std::string(coordinate), "1");
        if (next.size() > digitBudget)
        { result.status = V3FamilyCoordinateStatus::resource_unavailable; result.reason = "v3_family_coordinate_digit_budget"; return result; }
        result.coordinate = coordinate;
        result.nextCoordinate = next;
        result.family = std::move(family);
        result.status = V3FamilyCoordinateStatus::ready;
        return result;
    }
    catch (const std::bad_alloc&) { result.family.reset(); result.status = V3FamilyCoordinateStatus::resource_unavailable; result.reason = "v3_family_coordinate_allocation_unavailable"; }
    catch (const std::length_error&) { result.family.reset(); result.status = V3FamilyCoordinateStatus::resource_unavailable; result.reason = "v3_family_coordinate_allocation_unavailable"; }
    return result;
}

// Append a single tagged family, not a finite prefix of the independent hover
// or result count domains. The caller must bind the descriptor to its schema,
// candidate ID and cursor, and must observe rather than set the hypothesis.
inline V3SelectionStatus AppendSelectionAxesV3(std::vector<Axis>& axes,
    const V3TaskCounts& counts, std::size_t taskACount, bool isTaskB)
{
    if (!V3CountValueCanonical(counts.hover) || !V3CountValueCanonical(counts.resolved))
        return V3SelectionStatus::invalid_descriptor;
    try
    {
        std::vector<Axis> generated;
        if (!AppendSelectionAxesV3(generated, counts.sourceCount, taskACount, isTaskB))
            return V3SelectionStatus::resource_unavailable;
        const auto find = [&](std::string_view id) {
            return std::find_if(generated.begin(), generated.end(),
                [&](const Axis& axis) { return axis.id == id; });
        };
        const auto hover = find("P02.count");
        const auto resolved = find("P02.resolved_count");
        const auto many = find("P02.many_count");
        if (hover == generated.end() || resolved == generated.end() || many == generated.end() ||
            resolved->role != Role::Hypothesis || many->role != Role::Hypothesis)
            return V3SelectionStatus::schema_error;
        const auto exactValue = [](const std::string& value) {
            if (value == "0") return std::string("zero");
            if (value == "1") return std::string("one");
            if (value == "2") return std::string("two");
            return "count_" + value;
        };
        hover->values = {counts.hover.kind == V3CountKind::not_applicable ? "na" :
            counts.hover.kind == V3CountKind::unknown ? "unknown" : exactValue(counts.hover.decimal)};
        if (counts.resolved.kind == V3CountKind::not_applicable)
        { resolved->values = {"na"}; many->values = {"na"}; }
        else if (counts.resolved.kind == V3CountKind::unknown)
        { resolved->values = {"still_unknown"}; many->values = {"na"}; }
        else
        {
            const auto value = exactValue(counts.resolved.decimal);
            if (value == "zero" || value == "one" || value == "two")
            { resolved->values = {value}; many->values = {"na"}; }
            else { resolved->values = {"many"}; many->values = {value}; }
        }
        std::vector<Axis> combined = axes;
        if (generated.size() > combined.max_size() - combined.size())
            return V3SelectionStatus::resource_unavailable;
        combined.reserve(combined.size() + generated.size());
        for (auto& axis : generated) combined.push_back(std::move(axis));
        axes.swap(combined);
        return V3SelectionStatus::ok;
    }
    catch (const std::bad_alloc&) { return V3SelectionStatus::resource_unavailable; }
    catch (const std::length_error&) { return V3SelectionStatus::resource_unavailable; }
}

struct V4AxisBudget
{
    std::size_t logicalMembers = 0, fileMembers = 0, virtualMembers = 0, referenceMembers = 0;
    std::size_t payloadSlots = 0, textSlots = 0, requestResultSlots = 0, fileContentSlots = 0, payloadBytes = 0;
    std::size_t directoryNodeSlots = 0;
    std::size_t directoryRootSlots = 0;
    std::size_t preparationSteps = 0;
};
enum class V4AxisStatus { schema_only, unresolved, invalid_descriptor, resource_unavailable };
struct V4MaterializedCount
{
    V4AxisStatus status = V4AxisStatus::invalid_descriptor;
    std::size_t value = 0;
};
inline V4MaterializedCount V4MaterializeCount(const V3CountValue& count, std::size_t limit, bool naMeansZero) noexcept
{
    if (!V3CountValueCanonical(count)) return {};
    if (count.kind == V3CountKind::unknown || (count.kind == V3CountKind::not_applicable && !naMeansZero))
        return {V4AxisStatus::unresolved, 0};
    if (count.kind == V3CountKind::not_applicable) return {V4AxisStatus::schema_only, 0};
    std::size_t value = 0;
    for (const char ch : count.decimal)
    {
        const auto digit = static_cast<std::size_t>(ch - '0');
        if (digit > limit || value > (limit - digit) / 10) return {V4AxisStatus::resource_unavailable, 0};
        value = value * 10 + digit;
    }
    return {V4AxisStatus::schema_only, value};
}
struct V4AxisResult
{
    V4AxisStatus status = V4AxisStatus::invalid_descriptor;
    std::vector<std::string> unresolved;
};

// Append one V4 task's unprefixed selection/input axes. The caller owns task A
// count validation, B-prefixing, full family identity and the transaction.
// This constructs schema only: no format selection or product result is set.
inline V4AxisResult AppendSelectionAxesV4(std::vector<Axis>& axes, const V4TaskSpec& task,
    std::size_t taskASelectionCount, bool isTaskB, const V4AxisBudget& budget)
{
    V4AxisResult result;
    if (!V4TaskSlotsCanonical(task)) { result.unresolved.emplace_back("v4_task_slot_encoding_invalid"); return result; }
    const auto audit = AuditV4TaskSlots(task);
    if (audit.status != V4SlotStatus::schema_defined)
    { result.status = V4AxisStatus::unresolved; result.unresolved.emplace_back(audit.reason); return result; }
    try
    {
        const auto logicalResult = V4MaterializeCount(task.selectionCount, budget.logicalMembers, false);
        const auto fileResult = V4MaterializeCount(task.fileCount, budget.fileMembers, task.kind == V4SourceKind::content_only);
        const auto virtualResult = V4MaterializeCount(task.virtualCount, budget.virtualMembers, true);
        for (const auto status : {logicalResult.status, fileResult.status, virtualResult.status})
            if (status != V4AxisStatus::schema_only)
            { result.status = status; result.unresolved.emplace_back("v4_member_count_materialization_unavailable"); return result; }
        const auto logical = logicalResult.value, files = fileResult.value, virtualFiles = virtualResult.value;
        // One directory reference domain can include both local and A
        // logical-member identities. Budget its total index width separately
        // from the B task's remaining logical-member allocation.
        const auto crossReferences = isTaskB ? taskASelectionCount : std::size_t{0};
        if (task.payloads.size() > budget.payloadSlots || task.filePaths.size() > budget.textSlots ||
            task.virtualNames.size() > budget.textSlots - task.filePaths.size() ||
            task.requestResults.size() > budget.requestResultSlots || task.fileContents.size() > budget.fileContentSlots ||
            task.directoryNodes.size() > budget.directoryNodeSlots || task.directoryRoots.size() > budget.directoryRootSlots ||
            task.preparation.steps.size() > budget.preparationSteps ||
            crossReferences > budget.referenceMembers ||
            logical > budget.referenceMembers - crossReferences)
        { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_member_or_payload_budget_unavailable"); return result; }
        // Every physical/virtual member independently selects a logical
        // reference, as does the initial anchor. Budget all domain entries.
        auto referenceRemaining = budget.referenceMembers - crossReferences - logical;
        for (unsigned pass = 0; pass < 2; ++pass)
        {
            if (task.preparation.outsideIdentities.size() > referenceRemaining)
            { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_outside_reference_budget_unavailable"); return result; }
            referenceRemaining -= task.preparation.outsideIdentities.size();
        }
        if (task.dimensions.size() > referenceRemaining)
        { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_dimension_slot_budget_unavailable"); return result; }
        referenceRemaining -= task.dimensions.size();
        if (task.positions.size() > referenceRemaining)
        { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_position_slot_budget_unavailable"); return result; }
        referenceRemaining -= task.positions.size();
        if (task.shortcutTargets.size() > referenceRemaining)
        { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_shortcut_slot_budget_unavailable"); return result; }
        referenceRemaining -= task.shortcutTargets.size();
        auto remainingText = budget.textSlots - task.filePaths.size() - task.virtualNames.size();
        for (std::size_t i = 0; i < task.categoryRules.size(); ++i)
        {
            if (remainingText < 3)
            { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_category_rule_slot_budget_unavailable"); return result; }
            remainingText -= 3;
        }
        if (task.shortcutTargets.size() > remainingText)
        { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_shortcut_text_budget_unavailable"); return result; }
        remainingText -= task.shortcutTargets.size();
        for (const auto* group : {&task.sourceGroup, &task.targetGroup})
        {
            if (group->children.size() > remainingText || group->children.size() > referenceRemaining)
            { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_group_child_slot_budget_unavailable"); return result; }
            remainingText -= group->children.size(); referenceRemaining -= group->children.size();
        }
        const auto consumeLogicalDomain = [&] {
            if (logical > referenceRemaining) return false;
            referenceRemaining -= logical; return true;
        };
        if (!consumeLogicalDomain())
        { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_logical_reference_budget_unavailable"); return result; }
        for (std::size_t i = 0; i < files; ++i) if (!consumeLogicalDomain())
        { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_logical_reference_budget_unavailable"); return result; }
        for (std::size_t i = 0; i < virtualFiles; ++i) if (!consumeLogicalDomain())
        { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_logical_reference_budget_unavailable"); return result; }
        std::size_t bytes = 0;
        for (const auto& slot : task.payloads)
        {
            const auto size = slot.hex.size() / 2;
            if (size > budget.payloadBytes - bytes)
            { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_payload_byte_budget_unavailable"); return result; }
            bytes += size;
        }
        for (const auto* slots : {&task.filePaths, &task.virtualNames})
            for (const auto& slot : *slots)
            {
                const auto size = slot.utf16leHex.size() / 2;
                if (size > budget.payloadBytes - bytes)
                { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_payload_byte_budget_unavailable"); return result; }
                bytes += size;
            }
        for (const auto& slot : task.requestResults)
        {
            const auto size = slot.hresultHex.size() / 2;
            if (size > budget.payloadBytes - bytes)
            { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_payload_byte_budget_unavailable"); return result; }
            bytes += size;
        }
        for (const auto& slot : task.fileContents)
        {
            const auto size = slot.hex.size() / 2;
            if (size > budget.payloadBytes - bytes)
            { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_payload_byte_budget_unavailable"); return result; }
            bytes += size;
        }
        for (const auto& node : task.directoryNodes)
            for (const auto size : {node.relativePathUtf16Hex.size() / 2, node.contentHex.size() / 2})
            {
                if (size > budget.payloadBytes - bytes)
                { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_directory_node_byte_budget_unavailable"); return result; }
                bytes += size;
            }
        for (const auto& step : task.preparation.steps)
            for (const auto size : {step.modifiersHex.size() / 2, step.xHex.size() / 2, step.yHex.size() / 2})
            {
                if (size > budget.payloadBytes - bytes)
                { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_preparation_byte_budget_unavailable"); return result; }
                bytes += size;
            }
        for (const auto& outside : task.preparation.outsideIdentities)
        {
            const auto size = outside.identityHex.size() / 2;
            if (size > budget.payloadBytes - bytes)
            { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_outside_identity_byte_budget_unavailable"); return result; }
            bytes += size;
        }
        for (const auto& dimension : task.dimensions)
        {
            const auto size = dimension.valueHex.size() / 2;
            if (size > budget.payloadBytes - bytes)
            { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_dimension_byte_budget_unavailable"); return result; }
            bytes += size;
        }
        for (const auto& position : task.positions)
            for (const auto size : {position.member.decimal.size(), position.position.decimal.size()})
            {
                if (size > budget.payloadBytes - bytes)
                { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_position_byte_budget_unavailable"); return result; }
                bytes += size;
            }
        for (const auto& rule : task.categoryRules)
            for (const auto size : {rule.idUtf16Hex.size() / 2, rule.labelUtf16Hex.size() / 2, rule.extensionsUtf16Hex.size() / 2})
            {
                if (size > budget.payloadBytes - bytes)
                { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_category_rule_byte_budget_unavailable"); return result; }
                bytes += size;
            }
        for (const auto& shortcut : task.shortcutTargets)
        {
            const auto size = shortcut.extensionUtf16Hex.size() / 2;
            if (size > budget.payloadBytes - bytes)
            { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_shortcut_byte_budget_unavailable"); return result; }
            bytes += size;
        }
        for (const auto* group : {&task.sourceGroup, &task.targetGroup})
        {
            for (const auto size : {group->activeChild.decimal.size(), group->hitChild.decimal.size()})
            {
                if (size > budget.payloadBytes - bytes)
                { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_group_reference_byte_budget_unavailable"); return result; }
                bytes += size;
            }
            for (const auto& child : group->children)
                for (const auto size : {child.idUtf16Hex.size() / 2, child.typeHex.size() / 2})
                {
                    if (size > budget.payloadBytes - bytes)
                    { result.status = V4AxisStatus::resource_unavailable; result.unresolved.emplace_back("v4_group_child_byte_budget_unavailable"); return result; }
                    bytes += size;
                }
        }
        std::vector<Axis> generated;
        const auto prior = AppendSelectionAxesV3(generated, V3TaskCounts{logical, task.hover, task.resolved},
            taskASelectionCount, isTaskB);
        if (prior != V3SelectionStatus::ok)
        { result.status = prior == V3SelectionStatus::resource_unavailable ? V4AxisStatus::resource_unavailable : V4AxisStatus::invalid_descriptor;
          result.unresolved.emplace_back("v4_selection_template_unavailable"); return result; }
        const auto countValue = [](const V3CountValue& count) {
            if (count.kind == V3CountKind::not_applicable) return std::string("na");
            if (count.kind == V3CountKind::unknown) return std::string("unknown_unresolved");
            if (count.decimal == "0") return std::string("zero");
            if (count.decimal == "1") return std::string("one");
            if (count.decimal == "2") return std::string("two");
            return std::string("count_") + count.decimal;
        };
        constexpr std::string_view modes[]{"none", "single_click", "ctrl_sequence", "marquee", "shift_range", "ctrl_shift_range", "other_unresolved", "unknown_unresolved"};
        constexpr std::string_view actions[]{"pointer_down", "pointer_move", "pointer_up", "session_end", "other_unresolved"};
        constexpr std::string_view buttons[]{"none", "left", "right", "middle", "unknown_unresolved"};
        const auto tagged = [](V4PayloadKind kind, const std::string& value) {
            return kind == V4PayloadKind::not_applicable ? std::string("na") :
                kind == V4PayloadKind::unknown_unresolved ? std::string("unknown_unresolved") : std::string("exact_") + value;
        };
        AddAxis(generated, "V4.selection_prepare.mode", Role::Input,
            {std::string(modes[static_cast<unsigned>(task.preparation.mode)])}, "Selection preparation mode, not observed selection");
        for (std::size_t i = 0; i < logical; ++i)
            AddAxis(generated, "V4.selection_prepare.baseline_member_" + std::to_string(i), Role::Input,
                {"absent", "present", "unknown_unresolved"}, "Initial selection membership before preparation");
        AddAxis(generated, "V4.selection_prepare.baseline_outside_members", Role::Input,
            {"none", "unknown_unresolved", "other_unresolved"}, "Initial selection outside the represented logical members");
        std::vector<std::string> baselineAnchors{"none", "unknown_unresolved", "other_unresolved"};
        for (std::size_t i = 0; i < logical; ++i) baselineAnchors.push_back("member_" + std::to_string(i));
        constexpr std::string_view outsideScopes[]{"desktop", "collection", "desktop_files", "dock", "running_items", "popup", "widget", "other_unresolved", "unknown_unresolved"};
        for (const auto& outside : task.preparation.outsideIdentities)
        {
            const auto base = "V4.selection_prepare.baseline_outside_" + outside.index;
            AddAxis(generated, base + ".scope", Role::Input,
                {std::string(outsideScopes[static_cast<unsigned>(outside.scope)])}, "Declared external surface, not an observed runtime object");
            AddAxis(generated, base + ".identity", Role::Input,
                {outside.identityKind == V4PayloadKind::not_applicable ? "na" : outside.identityKind == V4PayloadKind::unknown_unresolved ?
                    "unknown_unresolved" : std::string("hex_") + outside.identityHex}, "Declared opaque identity bytes");
            AddAxis(generated, base + ".alias", Role::Input, {countValue(outside.alias)}, "Independent alias label, not observed equality");
            AddAxis(generated, base + ".selected", Role::Input, {"absent", "present", "unknown_unresolved"}, "External baseline membership input");
            baselineAnchors.push_back("outside_index_" + outside.index);
        }
        generated.push_back(Axis{"V4.selection_prepare.baseline_anchor", Role::Input,
            std::move(baselineAnchors), "Initial range-selection anchor before preparation"});
        AddAxis(generated, "V4.selection_prepare.step_count", Role::Input,
            {std::string("count_") + std::to_string(task.preparation.steps.size())}, "Ordered preparation input step count");
        for (std::size_t i = 0; i < task.preparation.steps.size(); ++i)
        {
            const auto& step = task.preparation.steps[i]; const auto base = "V4.selection_prepare.step_" + std::to_string(i);
            AddAxis(generated, base + ".action", Role::Input, {std::string(actions[static_cast<unsigned>(step.action)])}, "Ordered raw preparation pointer action");
            AddAxis(generated, base + ".button", Role::Input, {std::string(buttons[static_cast<unsigned>(step.button)])}, "Preparation pointer button");
            AddAxis(generated, base + ".session", Role::Input, {countValue(step.session)}, "Explicit preparation session ordinal");
            AddAxis(generated, base + ".modifiers", Role::Input, {tagged(step.modifiersKind, step.modifiersHex)}, "Raw preparation modifiers");
            AddAxis(generated, base + ".member", Role::Input, {countValue(step.member)}, "Intended logical member, not observed hit");
            AddAxis(generated, base + ".x", Role::Input, {tagged(step.xKind, step.xHex)}, "Raw screen X bit pattern");
            AddAxis(generated, base + ".y", Role::Input, {tagged(step.yKind, step.yHex)}, "Raw screen Y bit pattern");
        }
        if (task.preparation.mode != V4SelectionPreparationMode::none || !task.preparation.steps.empty())
            result.unresolved.emplace_back("v4_selection_preparation_execution_and_selection_observation_unimplemented");
        const auto inputBoolean = [](V4InputBoolean value) -> std::string {
            switch (value) { case V4InputBoolean::not_applicable: return "na";
            case V4InputBoolean::unknown_unresolved: return "unknown_unresolved";
            case V4InputBoolean::no: return "no"; case V4InputBoolean::yes: return "yes"; }
            return "unknown_unresolved";
        };
        const auto inputText = [](V4PayloadKind kind, const std::string& hex) {
            return kind == V4PayloadKind::not_applicable ? std::string("na") :
                kind == V4PayloadKind::unknown_unresolved ? std::string("unknown_unresolved") : std::string("utf16le_hex_") + hex;
        };
        AddAxis(generated, "V4.category_rules.kind", Role::Input,
            {task.categoryRulesKind == V4PayloadKind::not_applicable ? "na" :
                task.categoryRulesKind == V4PayloadKind::unknown_unresolved ? "unknown_unresolved" : "exact"},
            "Ordered raw category configuration, not an observed classifier result");
        for (std::size_t i = 0; i < task.categoryRules.size(); ++i)
        {
            const auto& rule = task.categoryRules[i]; const auto base = "V4.category_rules.rule_" + std::to_string(i);
            AddAxis(generated, base + ".id", Role::Input, {inputText(rule.idKind, rule.idUtf16Hex)}, "Raw category rule identity");
            AddAxis(generated, base + ".label", Role::Input, {inputText(rule.labelKind, rule.labelUtf16Hex)}, "Raw category rule label");
            AddAxis(generated, base + ".extensions", Role::Input, {inputText(rule.extensionsKind, rule.extensionsUtf16Hex)}, "Raw extension rule text");
            AddAxis(generated, base + ".enabled", Role::Input, {inputBoolean(rule.enabled)}, "Raw enabled input");
        }
        for (const auto& shortcut : task.shortcutTargets)
        {
            const auto base = "V4.shortcut_target.member_" + shortcut.index;
            AddAxis(generated, base + ".classified", Role::Input, {inputBoolean(shortcut.classified)}, "Shortcut classification input");
            AddAxis(generated, base + ".directory", Role::Input, {inputBoolean(shortcut.directory)}, "Shortcut target directory input");
            AddAxis(generated, base + ".application", Role::Input, {inputBoolean(shortcut.application)}, "Shortcut target application input");
            AddAxis(generated, base + ".extension", Role::Input, {inputText(shortcut.extensionKind, shortcut.extensionUtf16Hex)}, "Raw target extension input");
        }
        for (const auto& side : {std::pair{std::string_view{"source"}, &task.sourceGroup}, std::pair{std::string_view{"target"}, &task.targetGroup}})
        {
            const auto base = std::string("V4.group_") + std::string(side.first);
            const auto& group = *side.second;
            AddAxis(generated, base + ".children_kind", Role::Input,
                {group.childrenKind == V4PayloadKind::not_applicable ? "na" : group.childrenKind == V4PayloadKind::unknown_unresolved ? "unknown_unresolved" : "exact"},
                "Ordered group children input, not an observed hit");
            AddAxis(generated, base + ".active", Role::Input, {countValue(group.activeChild)}, "Declared active child slot");
            AddAxis(generated, base + ".hit", Role::Input, {countValue(group.hitChild)}, "Declared intended hit child slot");
            for (std::size_t i = 0; i < group.children.size(); ++i)
            {
                const auto& child = group.children[i]; const auto slot = base + ".child_" + std::to_string(i);
                AddAxis(generated, slot + ".id", Role::Input, {inputText(child.identityKind, child.idUtf16Hex)}, "Raw ordered child ID");
                AddAxis(generated, slot + ".type", Role::Input, {tagged(child.typeKind, child.typeHex)}, "Raw child type bits");
            }
            if (group.childrenKind != V4PayloadKind::not_applicable)
                result.unresolved.emplace_back("v4_group_input_and_actual_hit_not_observed:" + std::string(side.first));
        }
        // Declaration and grant are independent configured inputs. These
        // values do not prove the live runtime permission or any file access.
        for (const auto permission : {std::string_view{"read"}, std::string_view{"write"}, std::string_view{"watch"}})
            for (const auto state : {std::string_view{"declared"}, std::string_view{"granted"}})
                AddAxis(generated, std::string("V4.lua_filesystem_permission.") + std::string(permission) + "." + std::string(state),
                    Role::Input, {"na", "unknown_unresolved", "no", "yes"},
                    "Configured permission input; live owner, runtime and grant remain unobserved");
        if (task.categoryRulesKind != V4PayloadKind::not_applicable)
            result.unresolved.emplace_back("v4_category_rules_input_not_observed_or_configured");
        if (!task.shortcutTargets.empty())
            result.unresolved.emplace_back("v4_shortcut_target_inputs_not_observed_or_configured");
        generated.push_back(Axis{"V4.source.kind", Role::Input, {std::string(V4SourceKindName(task.kind))}, "V4 source-family tag"});
        generated.push_back(Axis{"V4.source.file_count", Role::Input, {countValue(task.fileCount)}, "Actual file-path input count"});
        generated.push_back(Axis{"V4.source.virtual_count", Role::Input, {countValue(task.virtualCount)}, "Declared virtual input count"});
        for (std::size_t i = 0; i < files; ++i)
        {
            const auto base = "V4.file.member" + std::to_string(i);
            AddAxis(generated, base + "_kind", Role::Input, {"file", "directory", "unknown_unresolved"}, "Physical file-path member kind");
            AddAxis(generated, base + "_identity", Role::Input, {"unique", "alias_unresolved", "unknown_unresolved"}, "Physical member identity");
            std::vector<std::string> relation{"none", "member_reference_unresolved", "unknown_unresolved"};
            for (std::size_t member = 0; member < logical; ++member) relation.push_back("member_" + std::to_string(member));
            generated.push_back(Axis{base + "_logical_relation", Role::Input, std::move(relation), "File-to-logical-selection identity"});
            AddAxis(generated, base + "_path", Role::Derived, {"na", "exact", "unknown", "unresolved"}, "Path summary derived from explicit indexed input");
        }
        for (std::size_t i = 0; i < virtualFiles; ++i)
        {
            const auto base = "V4.virtual.member" + std::to_string(i);
            AddAxis(generated, base + "_name", Role::Derived, {"na", "exact", "unknown", "unresolved"}, "Name summary derived from explicit indexed input");
            AddAxis(generated, base + "_identity", Role::Input, {"unique", "alias_unresolved", "unknown_unresolved"}, "Virtual member identity");
            std::vector<std::string> relation{"none", "member_reference_unresolved", "unknown_unresolved"};
            for (std::size_t member = 0; member < logical; ++member) relation.push_back("member_" + std::to_string(member));
            generated.push_back(Axis{base + "_logical_relation", Role::Input, std::move(relation), "Virtual-to-logical-selection identity"});
        }
        std::vector<std::string> formats{"cf_hdrop", "shell_id_list", "file_descriptor_w", "file_descriptor_a", "file_contents",
            "dib", "dibv5", "png", "image_png", "bitmap", "text_unicode", "text_ansi", "url_w", "url_a", "other_unresolved"};
        for (const auto& slot : task.payloads)
            if (std::find(formats.begin(), formats.end(), slot.format) == formats.end()) formats.push_back(slot.format);
        for (const auto& slot : task.requestResults)
            if (std::find(formats.begin(), formats.end(), slot.format) == formats.end()) formats.push_back(slot.format);
        for (const auto& format : formats)
        {
            const std::string base = std::string("V4.format.") + format;
            AddAxis(generated, base + ".offer", Role::Input, {"no", "yes", "unknown_unresolved"}, "Format offered independently");
            std::vector<std::string> media{"hglobal", "file", "istream", "istorage", "gdi", "mfpict", "enhmf"};
            for (const auto& slot : task.payloads)
                if (slot.format == format && std::find(media.begin(), media.end(), slot.medium) == media.end()) media.push_back(slot.medium);
            for (const auto& slot : task.requestResults)
                if (slot.format == format && std::find(media.begin(), media.end(), slot.medium) == media.end()) media.push_back(slot.medium);
            for (const auto& medium : media)
                AddAxis(generated, base + ".medium." + medium, Role::Input, {"no", "yes", "unknown_unresolved"}, "Offered TYMED bit");
            AddAxis(generated, base + ".unlisted_payloads", Role::Input, {"unresolved", "reject_unlisted"},
                "Whether this format rejects unlisted indexed representations");
        }
        for (const auto& slot : task.payloads)
        {
            const auto base = "V4.payload." + slot.format + "." + slot.medium + "." + slot.index;
            const auto raw = slot.kind == V4PayloadKind::not_applicable ? std::string("na") :
                slot.kind == V4PayloadKind::unknown_unresolved ? std::string("unknown_unresolved") : std::string("hex_") + slot.hex;
            generated.push_back(Axis{base + ".bytes", Role::Input, {raw}, "Exact supplied bytes or explicitly unresolved"});
        }
        const auto addText = [&](const std::vector<V4IndexedUtf16Slot>& slots, std::string_view label) {
            for (const auto& slot : slots)
            {
                auto value = slot.kind == V4PayloadKind::not_applicable ? std::string("na") :
                    slot.kind == V4PayloadKind::unknown_unresolved ? std::string("unknown_unresolved") :
                    std::string("utf16le_hex_") + slot.utf16leHex;
                if (slot.base == V4PathBase::owned_root_relative) value = "owned_root_relative_" + value;
                generated.push_back(Axis{"V4." + std::string(label) + ".index_" + slot.index + ".utf16le",
                    Role::Input, {value}, "Exact input code units; isolated materialization remains unimplemented"});
            }
        };
        addText(task.filePaths, "file_path");
        addText(task.virtualNames, "virtual_name");
        for (const auto& slot : task.fileContents)
        {
            const auto value = slot.kind == V4PayloadKind::not_applicable ? std::string("na") :
                slot.kind == V4PayloadKind::unknown_unresolved ? std::string("unknown_unresolved") : std::string("hex_") + slot.hex;
            generated.push_back(Axis{"V4.file_content.index_" + slot.index + ".bytes", Role::Input,
                {value}, "Exact physical file input bytes; isolated materialization unimplemented"});
        }
        for (const auto& root : task.directoryRoots)
        {
            const auto value = root.completeness == V4DirectoryCompleteness::not_applicable ? "na" :
                root.completeness == V4DirectoryCompleteness::unknown_unresolved ? "unknown_unresolved" :
                root.completeness == V4DirectoryCompleteness::partial_unresolved ? "partial_unresolved" : "complete";
            generated.push_back(Axis{"V4.directory.root_" + root.rootIndex + ".completeness", Role::Input, {value},
                "Independent directory listing completeness"});
        }
        for (const auto& node : task.directoryNodes)
        {
            const auto base = "V4.directory.root_" + node.rootIndex + ".slot_" + node.slotIndex;
            const auto path = node.pathKind == V4PayloadKind::not_applicable ? std::string("na") :
                node.pathKind == V4PayloadKind::unknown_unresolved ? std::string("unknown_unresolved") :
                std::string("utf16le_hex_") + node.relativePathUtf16Hex;
            const auto content = node.contentKind == V4PayloadKind::not_applicable ? std::string("na") :
                node.contentKind == V4PayloadKind::unknown_unresolved ? std::string("unknown_unresolved") :
                std::string("hex_") + node.contentHex;
            const auto kind = node.nodeKind == V4DirectoryNodeKind::file ? "file" :
                node.nodeKind == V4DirectoryNodeKind::directory ? "directory" :
                node.nodeKind == V4DirectoryNodeKind::other_unresolved ? "other_unresolved" : "unknown_unresolved";
            const auto object = node.objectIdentity.kind == V3CountKind::not_applicable ? std::string("na") :
                node.objectIdentity.kind == V3CountKind::unknown ? std::string("unknown_unresolved") :
                std::string("identity_") + node.objectIdentity.decimal;
            generated.push_back(Axis{base + ".path", Role::Input, {path}, "Independent directory-node relative path"});
            generated.push_back(Axis{base + ".kind", Role::Input, {kind}, "Independent directory-node kind"});
            generated.push_back(Axis{base + ".content", Role::Input, {content}, "Independent directory-node bytes"});
            generated.push_back(Axis{base + ".identity", Role::Input, {object}, "Independent directory-node object identity"});
        }
        for (const auto& slot : task.requestResults)
        {
            const auto value = slot.kind == V4PayloadKind::not_applicable ? std::string("na") :
                slot.kind == V4PayloadKind::unknown_unresolved ? std::string("unknown_unresolved") :
                std::string("hresult_") + slot.hresultHex;
            generated.push_back(Axis{"V4.request." + slot.format + "." + slot.medium + "." + slot.index + "." + slot.phase + ".hresult",
                Role::Input, {value}, "Independent requested HRESULT input; provider mapping remains unimplemented"});
        }
        AddAxis(generated, "V4.request.unlisted_results", Role::Input, {"unresolved", "provider_default"},
            "Unlisted FORMATETC Query/Get follow the controlled provider rule; stream reads remain unmodeled");
        result.unresolved.emplace_back("v4_format_to_legacy_summary_and_full_representation_mapping_unimplemented");
        result.unresolved.emplace_back("v4_file_and_virtual_member_construction_unimplemented");
        result.unresolved.emplace_back("v4_physical_directory_tree_topology_unimplemented");
        result.unresolved.emplace_back("v4_payload_source_and_production_observation_unimplemented");
        std::vector<Axis> combined = axes;
        if (generated.size() > combined.max_size() - combined.size())
        { result.status = V4AxisStatus::resource_unavailable; return result; }
        combined.reserve(combined.size() + generated.size());
        for (auto& axis : generated) combined.push_back(std::move(axis));
        axes.swap(combined);
        result.status = V4AxisStatus::schema_only;
        return result;
    }
    catch (const std::bad_alloc&) { result.status = V4AxisStatus::resource_unavailable; return result; }
    catch (const std::length_error&) { result.status = V4AxisStatus::resource_unavailable; return result; }
}

// Applied once to the completed A/B schema. A raw int32 input is not a
// promise that a product operation accepts it or that any clamp occurred.
inline V4AxisResult ApplyV4ExactDimensions(std::vector<Axis>& axes, const V4TaskSpec& task, bool isTaskB)
{
    V4AxisResult result;
    try
    {
        constexpr std::string_view names[]{"source.E17.columns", "source.E17.rows", "source.E23", "source.E24",
            "target.E17.columns", "target.E17.rows", "target.E23", "target.E24"};
        std::map<std::string, bool> seen;
        std::vector<std::pair<std::size_t, std::string>> updates;
        for (const auto& dimension : task.dimensions)
        {
            const auto target = static_cast<unsigned>(dimension.target);
            std::string id;
            if (target < std::size(names))
            {
                if (dimension.member.kind != V3CountKind::not_applicable)
                { result.status = V4AxisStatus::unresolved; result.unresolved.emplace_back("v4_dimension_endpoint_member_unresolved"); return result; }
                id = names[target];
            }
            else
            {
                if (dimension.member.kind != V3CountKind::exact)
                { result.status = V4AxisStatus::unresolved; result.unresolved.emplace_back("v4_dimension_member_unresolved"); return result; }
                id = "P01.member" + dimension.member.decimal + (target == 8 ? "_span_columns" : "_span_rows");
            }
            if (isTaskB) id = "task_b." + id;
            if (!seen.emplace(id, true).second)
            { result.status = V4AxisStatus::unresolved; result.unresolved.emplace_back("v4_dimension_duplicate_target_unresolved"); return result; }
            const auto axis = std::find_if(axes.begin(), axes.end(), [&](const Axis& item) { return item.id == id; });
            if (axis == axes.end() || axis->role == Role::Derived)
            { result.status = V4AxisStatus::unresolved; result.unresolved.emplace_back("v4_dimension_target_unavailable"); return result; }
            auto value = dimension.kind == V4PayloadKind::not_applicable ? std::string("na") :
                dimension.kind == V4PayloadKind::unknown_unresolved ? std::string("unknown_unresolved") :
                std::string("int32_hex_") + dimension.valueHex;
            updates.emplace_back(static_cast<std::size_t>(axis - axes.begin()), std::move(value));
        }
        for (const auto& position : task.positions)
        {
            if (position.member.kind != V3CountKind::exact)
            { result.status = V4AxisStatus::unresolved; result.unresolved.emplace_back("v4_position_member_unresolved"); return result; }
            std::string id = "P05.member" + position.member.decimal +
                (position.target == V4PositionTarget::visible ? "_visible_index" : "_storage_index");
            if (isTaskB) id = "task_b." + id;
            if (!seen.emplace(id, true).second)
            { result.status = V4AxisStatus::unresolved; result.unresolved.emplace_back("v4_position_duplicate_target_unresolved"); return result; }
            const auto axis = std::find_if(axes.begin(), axes.end(), [&](const Axis& item) { return item.id == id; });
            if (axis == axes.end() || axis->role == Role::Derived)
            { result.status = V4AxisStatus::unresolved; result.unresolved.emplace_back("v4_position_target_unavailable"); return result; }
            auto value = position.position.kind == V3CountKind::not_applicable ? std::string("na") :
                position.position.kind == V3CountKind::unknown ? std::string("unknown") :
                std::string("position_") + position.position.decimal;
            updates.emplace_back(static_cast<std::size_t>(axis - axes.begin()), std::move(value));
        }
        for (auto& [index, value] : updates) axes[index].values = {std::move(value)};
        result.status = V4AxisStatus::schema_only;
        if (!updates.empty()) result.unresolved.emplace_back("v4_exact_dimension_or_position_product_validation_and_binding_unimplemented");
        return result;
    }
    catch (const std::bad_alloc&) { result.status = V4AxisStatus::resource_unavailable; return result; }
    catch (const std::length_error&) { result.status = V4AxisStatus::resource_unavailable; return result; }
}

enum class V3SelectionAuditState { excluded, unresolved, invalid_candidate, resource_unavailable };
struct V3SelectionAudit
{
    V3SelectionAuditState state = V3SelectionAuditState::unresolved;
    std::string_view reason;
};

class V3FamilyValidatedContext;
class V3SelectionStructuralAccess
{
    friend class V3FamilyValidatedContext;
    // The friend must rebuild and compare the authoritative family schema and
    // bind ownCount to its task descriptor before this complete-candidate
    // check. An unresolved result is not product acceptance or coverage.
    // No V2 129-slot parser or host-result count is used here.
    static V3SelectionAudit Check(const std::vector<Axis>& axes,
        const Candidate& candidate, std::size_t ownCount, std::string_view taskPrefix)
{
    if (taskPrefix != "" && taskPrefix != "task_b.")
        return {V3SelectionAuditState::invalid_candidate, "v3_selection_task_prefix_invalid"};
    try
    {
        if (candidate.values.size() != axes.size())
            return {V3SelectionAuditState::invalid_candidate, "v3_selection_candidate_size_invalid"};
        const auto index = BuildIndex(axes);
        if (index->size() != axes.size() || (candidate.lookup && *candidate.lookup != *index))
            return {V3SelectionAuditState::invalid_candidate, "v3_selection_candidate_lookup_invalid"};
        for (std::size_t i = 0; i < axes.size(); ++i)
        {
            const auto& axis = axes[i];
            const auto& item = candidate.values[i];
            if (axis.role == Role::Derived ? !item.empty() :
                std::find(axis.values.begin(), axis.values.end(), item) == axis.values.end())
                return {V3SelectionAuditState::invalid_candidate, "v3_selection_candidate_domain_invalid"};
        }
        const auto value = [&](std::string_view id) -> std::string_view {
            const auto found = index->find(std::string(taskPrefix) + std::string(id));
            return found == index->end() ? std::string_view{} : candidate.values[found->second];
        };
        const auto expectedCount = ownCount == 0 ? std::string("zero") : ownCount == 1 ? std::string("one") :
            ownCount == 2 ? std::string("two") : "count_" + std::to_string(ownCount);
        if (value("P02.source_actual_count") != expectedCount)
            return {V3SelectionAuditState::invalid_candidate, "v3_selection_source_descriptor_mismatch"};
        if (value("T01") != "external_ingress" || value("T02.file_paths") != "yes")
            return {V3SelectionAuditState::unresolved, "v3_selection_file_source_semantics_unresolved"};
        for (const auto id : {"T02.shell_object", "T02.virtual_files", "T02.image", "T02.text", "T02.url", "T02.invalid_format"})
            if (value(id) != "no")
                return {V3SelectionAuditState::unresolved, "v3_selection_file_source_semantics_unresolved"};
        const auto fileKind = [](std::string_view kind) {
            return kind == "na" || kind == "unknown" || kind == "unresolved" ||
                kind == "file" || kind == "directory";
        };
        if (!fileKind(value("P01.primary")) || !fileKind(value("P01.second")))
            return {V3SelectionAuditState::unresolved, "v3_selection_member_kind_semantics_unresolved"};
        for (std::size_t i = 2; i < ownCount; ++i)
            if (!fileKind(value("P01.member" + std::to_string(i) + "_kind")))
                return {V3SelectionAuditState::unresolved, "v3_selection_member_kind_semantics_unresolved"};
        if ((ownCount == 0) != (value("P01.primary") == "na"))
            return {V3SelectionAuditState::excluded, "v3_selection_primary_count_mismatch"};
        if ((ownCount < 2) != (value("P01.second") == "na"))
            return {V3SelectionAuditState::excluded, "v3_selection_second_count_mismatch"};
        if (ownCount == 0 && (value("P06.entry_class") != "na" || value("P06.primary_special_extension") != "na"))
            return {V3SelectionAuditState::excluded, "v3_selection_missing_primary_attributes"};
        if (ownCount < 2 && (value("P04.second_source_relation") != "na" ||
            value("P06.second_entry_class") != "na" || value("P06.second_special_extension") != "na"))
            return {V3SelectionAuditState::excluded, "v3_selection_missing_second_attributes"};
        for (std::size_t i = 0; i < axes.size(); ++i)
        {
            if (axes[i].role == Role::Derived) continue;
            const std::string_view id = axes[i].id;
            if (!id.starts_with(taskPrefix) || (taskPrefix.empty() && id.starts_with("task_b."))) continue;
            const auto local = id.substr(taskPrefix.size());
            const auto marker = local.find(".member");
            if (marker == std::string_view::npos) continue;
            auto at = marker + std::string_view(".member").size();
            if (at == local.size() || local[at] < '0' || local[at] > '9') continue;
            std::size_t member = 0;
            for (; at < local.size() && local[at] >= '0' && local[at] <= '9'; ++at)
            {
                const auto digit = static_cast<std::size_t>(local[at] - '0');
                if (member > ((std::numeric_limits<std::size_t>::max)() - digit) / 10)
                    return {V3SelectionAuditState::invalid_candidate, "v3_selection_member_axis_invalid"};
                member = member * 10 + digit;
            }
            if (at == local.size() || local[at] != '_') continue;
            if (member >= ownCount && candidate.values[i] != "na")
                return {V3SelectionAuditState::excluded, "v3_selection_absent_member_attribute"};
            if (member >= 2 && member < ownCount && local.substr(0, marker) == "P01" &&
                local.substr(at) == "_kind" && candidate.values[i] == "na")
                return {V3SelectionAuditState::excluded, "v3_selection_present_member_kind_missing"};
        }
        return {V3SelectionAuditState::unresolved, "v3_selection_remaining_structure_and_execution_unresolved"};
    }
    catch (const std::bad_alloc&) { return {V3SelectionAuditState::resource_unavailable, "v3_selection_audit_allocation_unavailable"}; }
    catch (const std::length_error&) { return {V3SelectionAuditState::resource_unavailable, "v3_selection_audit_length_unavailable"}; }
}
};

inline void AppendSelectionAxes(std::vector<Axis>& axes)
{
    AppendSelectionAxes(axes, 129, SelectionSchemaVersion::legacy_v1);
}

inline void AppendSelectionAxes(std::vector<Axis>& axes, std::size_t memberLimit)
{
    AppendSelectionAxes(axes, memberLimit, SelectionSchemaVersion::legacy_v1);
}

// Returns -1 when the actual count is unknown/unresolved. Partial member data
// can still be known; unknown count never implies an empty selection or a
// completed execution. Unconstructed attributes must remain unimplemented.
// `taskPrefix` is empty for A or "task_b." for B. Reading B directly keeps
// the complete schema and avoids copying every axis solely to count members.
inline int SelectionSize(const std::vector<Axis>& axes, const Candidate& candidate, std::string_view taskPrefix = {})
{
    auto count = taskPrefix.empty() ? Value(axes, candidate, "P02.resolved_count") :
        taskPrefix == "task_b." ? Value(axes, candidate, "task_b.P02.resolved_count") :
        Value(axes, candidate, std::string(taskPrefix) + "P02.resolved_count");
    if (count == "many") count = taskPrefix.empty() ? Value(axes, candidate, "P02.many_count") :
        taskPrefix == "task_b." ? Value(axes, candidate, "task_b.P02.many_count") :
        Value(axes, candidate, std::string(taskPrefix) + "P02.many_count");
    if (count == "zero") return 0;
    if (count == "one") return 1;
    if (count == "two") return 2;
    if (count.substr(0, 6) == "count_")
    {
        int number = 0;
        const auto digits = count.substr(6);
        if (digits.empty() || (digits.size() > 1 && digits.front() == '0')) return -1;
        for (const char digit : digits)
        {
            if (digit < '0' || digit > '9' || number > 129) return -1;
            number = number * 10 + (digit - '0');
        }
        if (number >= 3 && number <= 129) return number;
    }
    return -1;
}

// V2 source count is independent of the host's future resolved-count
// hypothesis. Unknown, inapplicable and counts outside the currently wired
// 129-slot consumers remain unresolved; none is silently treated as zero.
inline int SelectionSourceSize(const std::vector<Axis>& axes, const Candidate& candidate, std::string_view taskPrefix = {})
{
    const auto count = taskPrefix.empty() ? Value(axes, candidate, "P02.source_actual_count") :
        taskPrefix == "task_b." ? Value(axes, candidate, "task_b.P02.source_actual_count") :
        Value(axes, candidate, std::string(taskPrefix) + "P02.source_actual_count");
    if (count == "zero") return 0;
    if (count == "one") return 1;
    if (count == "two") return 2;
    if (count.substr(0, 6) != "count_") return -1;
    const auto digits = count.substr(6);
    if (digits.empty() || (digits.size() > 1 && digits.front() == '0')) return -1;
    int number = 0;
    for (const char digit : digits)
    {
        if (digit < '0' || digit > '9' || number > 129) return -1;
        number = number * 10 + (digit - '0');
    }
    return number >= 3 && number <= 129 ? number : -1;
}

inline bool SelectionMemberAxisAllows(int count, std::string_view axisId, std::string_view proposedValue)
{
    const auto marker = axisId.find(".member");
    if (marker == std::string_view::npos) return true;
    std::size_t at = marker + std::string_view(".member").size();
    if (at == axisId.size() || axisId[at] < '0' || axisId[at] > '9') return true;
    std::size_t member = 0;
    while (at < axisId.size() && axisId[at] >= '0' && axisId[at] <= '9')
        member = member * 10 + static_cast<std::size_t>(axisId[at++] - '0');
    if (member > 128 || at == axisId.size() || axisId[at] != '_') return true;
    const bool crossTaskObject = axisId.substr(0, marker) == "P03" &&
        axisId.substr(at) == "_cross_task_identity";
    if (member < 2 && !crossTaskObject) return true;
    // An unresolved total does not tell us whether this slot exists, nor does
    // it erase any already-known member properties. Preserve the entire domain.
    if (count < 0) return true;
    if (member >= static_cast<std::size_t>(count)) return proposedValue == "na";
    if (member < 2) return true;
    if (axisId.substr(0, marker) == "P01" && axisId.substr(at) == "_kind") return proposedValue != "na";
    // An active member can have legitimately inapplicable attributes, e.g.
    // widget_type for a file or a physical collision for a widget.
    return true;
}

// Prefix-domain calculation depends only on resolved_count and many_count,
// which are declared before every member2..member128 input axis. P02.count is
// independent hover metadata. Other structural facts
// are checked after a candidate is complete and must not be used for subtree
// size calculations by this interface.
inline bool SelectionPrefixAllows(const std::vector<Axis>& axes, const Candidate& prefix,
    std::string_view axisId, std::string_view proposedValue)
{
    // B has independent selectors and is handled by SecondPrefixAllows.
    if (axisId.substr(0, 7) == "task_b.") return true;
    // This axis compares a B member with A; the A-side copy has no meaning.
    // B's projection is checked separately by SecondPrefixAllows.
    if (axisId.substr(0, 10) == "P03.member" && axisId.ends_with("_cross_task_identity"))
        return proposedValue == "na";
    const auto resolved = Value(axes, prefix, "P02.resolved_count");
    if (axisId == "P02.many_count") return resolved == "many" ? proposedValue != "na" : proposedValue == "na";
    return SelectionMemberAxisAllows(SelectionSize(axes, prefix), axisId, proposedValue);
}

// V2 transport/format selectors occur later than the source slots. Therefore
// this prefix cannot yet know whether a source count describes the vector
// represented by P01/P03. Keep source member domains complete until the full
// candidate can establish that interpretation. The many selector only encodes
// the independent host-result hypothesis. B is handled separately.
inline bool SelectionPrefixAllowsV2(const std::vector<Axis>& axes, const Candidate& prefix,
    std::string_view axisId, std::string_view proposedValue)
{
    if (axisId.substr(0, 7) == "task_b.") return true;
    if (axisId.substr(0, 10) == "P03.member" && axisId.ends_with("_cross_task_identity"))
        return proposedValue == "na";
    if (axisId == "P02.many_count")
        return Value(axes, prefix, "P02.resolved_count") == "many" ? proposedValue != "na" : proposedValue == "na";
    return true;
}

// The current source-vector interpretation is defined only for a pure
// external CF_HDROP of ordinary file/directory members. An offered text/image,
// another format, a non-file kind, or unknown member identity requires a
// separate model of the source vector and must not be filtered by this count.
inline bool SelectionV2HasExplicitFileSource(const std::vector<Axis>& axes, const Candidate& candidate)
{
    const auto value = [&](std::string_view id) { return Value(axes, candidate, id); };
    if (value("T01") != "external_ingress" || value("T02.file_paths") != "yes") return false;
    for (const auto id : {"T02.shell_object", "T02.virtual_files", "T02.image", "T02.text", "T02.url", "T02.invalid_format"})
        if (value(id) != "no") return false;
    const auto fileKind = [](std::string_view kind) { return kind == "na" || kind == "file" || kind == "directory"; };
    if (!fileKind(value("P01.primary")) || !fileKind(value("P01.second"))) return false;
    for (std::size_t i = 2; i < 129; ++i)
        if (!fileKind(value("P01.member" + std::to_string(i) + "_kind"))) return false;
    return true;
}

// Only structural facts are excluded here. In particular, source/target
// capability, permissions, a full destination, and the availability of a
// requested product action remain executable negative inputs.
inline std::string_view SelectionExclusionForVersion(const std::vector<Axis>& axes, const Candidate& candidate,
    SelectionSchemaVersion version)
{
    const auto value = [&](std::string_view id) { return Value(axes, candidate, id); };
    const auto resolved = value("P02.resolved_count");
    const auto effective = resolved;
    const auto many = value("P02.many_count");
    if (effective == "many" && many == "na") return "p02_many_requires_concrete_count_or_unresolved";
    if (effective != "many" && many != "na") return "p02_many_count_only_for_many";
    const int size = version == SelectionSchemaVersion::source_members_v2
        ? (SelectionV2HasExplicitFileSource(axes, candidate) ? SelectionSourceSize(axes, candidate) : -1)
        : SelectionSize(axes, candidate);
    const bool zero = size == 0;
    const bool one = size == 1;
    const bool multiple = size >= 2 || (version == SelectionSchemaVersion::legacy_v1 && effective == "many");
    const auto primary = value("P01.primary");
    const auto second = value("P01.second");
    if (zero && (primary != "na" || second != "na")) return "p01_empty_selection_has_no_members";
    if ((size > 0 || (version == SelectionSchemaVersion::legacy_v1 && effective == "many")) && primary == "na")
        return "p01_nonempty_selection_needs_primary";
    if ((zero || one) && second != "na") return "p01_second_member_requires_two";
    if (multiple && second == "na") return "p01_second_member_missing";
    const auto secondIdentity = value("P03.member1_identity_ref");
    if ((zero || one) && secondIdentity != "na") return "p03_second_identity_requires_two_members";
    if (multiple && secondIdentity == "na") return "p03_second_identity_state_missing";
    const auto widget0 = value("P03.member0_widget_type");
    const auto widget1 = value("P03.member1_widget_type");
    if ((primary == "widget" && widget0 == "na") || (primary == "na" && widget0 != "na"))
        return "p03_primary_widget_type_applicability";
    if ((second == "widget" && widget1 == "na") || (second == "na" && widget1 != "na"))
        return "p03_second_widget_type_applicability";
    for (std::size_t i = 0; i < axes.size(); ++i)
        if (axes[i].id.substr(0, 7) != "task_b." && axes[i].role != Role::Derived &&
            !SelectionMemberAxisAllows(size, axes[i].id, candidate.values[i]))
            return "p02_member_slot_does_not_match_count";
    if ((zero || one) && value("P04.second_source_relation") != "na")
        return "p04_second_source_requires_two";
    if ((zero || one) && (value("P04.member1_source_ref") != "na" || value("P04.member1_group_ref") != "na" ||
        value("R04.member1_incoming_name_ref") != "na" || value("R04.member1_incoming_name_spelling") != "na"))
        return "second_member_references_require_two";
    if (zero && (value("P06.entry_class") != "na" || value("P06.primary_special_extension") != "na"))
        return "p06_empty_selection_has_no_primary_class";
    if ((zero || one) && (value("P06.second_entry_class") != "na" || value("P06.second_special_extension") != "na"))
        return "p06_second_class_requires_two";
    for (const auto member : {"member0", "member1"})
    {
        const auto prefix = std::string("R04.") + member;
        const auto exists = value(prefix + "_target_exists");
        const bool noMember = zero || (one && std::string_view(member) == "member1");
        if (noMember && value(std::string("P03.") + member + "_cross_task_identity") != "na")
            return "p03_missing_member_has_no_cross_task_identity";
        if (noMember && (value(std::string("P01.") + member + "_span_columns") != "na" ||
            value(std::string("P01.") + member + "_span_rows") != "na" ||
            value(std::string("P01.") + member + "_large_icon_config") != "na" ||
            value(std::string("P01.") + member + "_large_icon_shape") != "na"))
            return "p01_missing_member_has_no_extent_or_appearance";
        if (noMember && (value(std::string("P05.") + member + "_visible_index") != "na" ||
            value(std::string("P05.") + member + "_storage_index") != "na"))
            return "p05_missing_member_has_no_order_position";
        if (noMember && (exists != "na" || value(prefix + "_target_identity") != "na" ||
            value(prefix + "_name_relation") != "na" || value(prefix + "_queued_reservation") != "na" ||
            value(prefix + "_concurrent_creation") != "na"))
            return "r04_missing_member_has_no_target_conflict";
        const auto conflictsWithAbsentTarget = [](std::string_view detail) {
            return detail != "na" && detail != "unknown" && detail != "unresolved";
        };
        if ((exists == "absent" || exists == "na") &&
            (conflictsWithAbsentTarget(value(prefix + "_target_identity")) ||
             conflictsWithAbsentTarget(value(prefix + "_name_relation"))))
            return "r04_absent_target_has_no_identity_or_name";
        if (noMember && (value(std::string("R02.") + member + "_directory_ref") != "na" ||
            value(std::string("R02.") + member + "_directory_object_ref") != "na" ||
            value(std::string("R02.") + member + "_directory_parent_ref") != "na" ||
            value(std::string("R02.") + member + "_directory_to_target") != "na" ||
            value(std::string("R03.") + member + "_volume_ref") != "na" ||
            value(std::string("R02.") + member + "_target_layout") != "na" ||
            value(std::string("R02.") + member + "_shortcut_layout") != "na" ||
            value(std::string("R02.") + member + "_path_state") != "na" ||
            value(std::string("R03.") + member + "_storage_layout") != "na"))
            return "r02_missing_member_has_no_path_or_volume";
    }
    for (int index = 2; index < size; ++index)
    {
        const auto member = std::string("member") + std::to_string(index);
        const auto kind = value("P01." + member + "_kind");
        if (kind == "widget" && value("P03." + member + "_widget_type") == "na")
            return "p03_selected_widget_needs_type";
        if (value("P03." + member + "_identity_ref") == "na")
            return "p03_selected_member_needs_identity_state";
        const auto prefix = std::string("R04.") + member;
        const auto exists = value(prefix + "_target_exists");
        const auto conflictsWithAbsentTarget = [](std::string_view detail) {
            return detail != "na" && detail != "unknown" && detail != "unresolved";
        };
        if ((exists == "absent" || exists == "na") &&
            (conflictsWithAbsentTarget(value(prefix + "_target_identity")) ||
             conflictsWithAbsentTarget(value(prefix + "_name_relation"))))
            return "r04_absent_target_has_no_identity_or_name";
    }
    if (value("G02.hover_first") == "na" && value("G02.hover_later") != "na")
        return "g02_later_hover_requires_first_hover";
    if (zero && value("H09.selected_member") != "na")
        return "h09_empty_selection_has_no_grabbed_member";
    if ((zero || one) && value("H09.selected_member") == "nonfirst")
        return "h09_nonfirst_member_requires_multiple";
    return {};
}

inline std::string_view SelectionExclusion(const std::vector<Axis>& axes, const Candidate& candidate)
{
    return SelectionExclusionForVersion(axes, candidate, SelectionSchemaVersion::legacy_v1);
}

inline std::string_view SelectionExclusionV2(const std::vector<Axis>& axes, const Candidate& candidate)
{
    return SelectionExclusionForVersion(axes, candidate, SelectionSchemaVersion::source_members_v2);
}
