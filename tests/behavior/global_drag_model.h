#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <limits>
#include <optional>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// One model describes historical fields and executable inputs. Derived fields
// are reported but do not multiply the input space. Missing adapters, oracles
// and unreviewed applicability remain explicit obligations.
namespace drag_global
{
enum class Role { Input, Conditional, Sequence, Hypothesis, Derived };
struct Axis
{
    std::string id;
    Role role;
    std::vector<std::string> values;
    std::string label;
};
struct Candidate
{
    std::string ordinal;
    std::vector<std::string> values; // derived entries remain empty until computed
    std::shared_ptr<const std::unordered_map<std::string, std::size_t>> lookup;
};
struct EventStep { std::string kind; std::string parameter; };
// A predicted outcome is checked after the same production run completes; it
// is never a fixture input or a condition that must already hold before drag.
struct OutcomeHypothesis { std::string axis; std::string value; };
struct EventProgram
{
    std::vector<EventStep> steps;
    std::vector<std::string> unresolved; // pre-dispatch construction/observation gaps
    std::vector<OutcomeHypothesis> postconditions;
};
inline std::string_view Value(const std::vector<Axis>& axes, const Candidate& candidate, std::string_view id);
inline void AddAxis(std::vector<Axis>& axes, std::string id, Role role,
    std::initializer_list<std::string_view> values, std::string_view label)
{
    Axis axis{std::move(id), role, {}, std::string(label)};
    for (const auto value : values) axis.values.emplace_back(value);
    if (axis.id.empty() || axis.values.empty()) throw std::runtime_error("global axis needs an ID and nonempty domain");
    axes.push_back(std::move(axis));
}
inline std::shared_ptr<const std::unordered_map<std::string, std::size_t>> BuildIndex(const std::vector<Axis>& axes)
{
    auto result = std::make_shared<std::unordered_map<std::string, std::size_t>>();
    result->reserve(axes.size());
    for (std::size_t i = 0; i < axes.size(); ++i) result->emplace(axes[i].id, i);
    return result;
}
// Shared arbitrary-precision decimal primitives; definitions follow the model
// declarations so the selection family codec and global scanner use one math.
inline std::string Normalize(std::string value);
inline std::string Add(std::string left, std::string right);
inline std::string Subtract(std::string left, std::string right);
inline bool Less(std::string left, std::string right);
inline std::string Multiply(std::string value, std::size_t factor);
inline std::size_t Divide(std::string& value, std::size_t divisor);
#include "global_drag_endpoints.inc"
#include "global_drag_selection.inc"
#include "global_drag_transfer.inc"

inline const char* RoleName(Role role)
{
    switch (role)
    {
    case Role::Input: return "input";
    case Role::Conditional: return "conditional";
    case Role::Sequence: return "sequence";
    case Role::Hypothesis: return "outcome_hypothesis";
    case Role::Derived: return "derived";
    }
    throw std::runtime_error("unknown global axis role");
}
inline std::vector<Axis> Model(SelectionSchemaVersion version = SelectionSchemaVersion::legacy_v1,
    std::size_t memberLimit = 129)
{
    if (version != SelectionSchemaVersion::legacy_v1 && version != SelectionSchemaVersion::source_members_v2)
        throw std::runtime_error("global_model_version_invalid");
    std::vector<Axis> axes;
    AppendEndpointAxes(axes);
    AppendSelectionAxes(axes, memberLimit, version);
    AppendTransferAxes(axes);
    if (version == SelectionSchemaVersion::legacy_v1) AppendSecondTaskAxes(axes);
    else AppendSecondTaskAxesV2(axes);
    std::vector<std::string> ids;
    for (const auto& axis : axes)
    {
        ids.push_back(axis.id);
        std::vector<std::string> values = axis.values;
        std::sort(values.begin(), values.end());
        if (std::adjacent_find(values.begin(), values.end()) != values.end())
            throw std::runtime_error("duplicate value in global axis " + axis.id);
        for (const auto& value : values)
            if (value.empty() || std::any_of(value.begin(), value.end(), [](unsigned char c) {
                return !(std::islower(c) || std::isdigit(c) || c == '_');
            })) throw std::runtime_error("global value must be stable lowercase ASCII: " + axis.id);
    }
    std::sort(ids.begin(), ids.end());
    if (std::adjacent_find(ids.begin(), ids.end()) != ids.end())
        throw std::runtime_error("duplicate global axis ID");
    return axes;
}
inline std::vector<Axis> ModelLegacyV1() { return Model(SelectionSchemaVersion::legacy_v1); }
// This schema is only a migration input: v2 counting, ID, graph, fixture and
// production execution are not authorized until their joint contracts close.
inline std::vector<Axis> ModelV2() { return Model(SelectionSchemaVersion::source_members_v2); }

// Version 3 has an explicit family identity and cannot be passed to the
// legacy/v2 Context or parser. This descriptor describes the source-vector
// family only; other payload families and production execution remain open.
enum class V3FamilyStatus { schema_only, invalid_descriptor, resource_unavailable, schema_error };
struct V3FamilySchema
{
    V3FamilyStatus status = V3FamilyStatus::invalid_descriptor;
    std::string binding;
    std::vector<Axis> axes;
    std::string reason;
};
inline bool V3CanonicalSourceDecimal(std::string_view value)
{
    return !value.empty() && (value.size() == 1 || value.front() != '0') &&
        std::all_of(value.begin(), value.end(), [](unsigned char ch) { return ch >= '0' && ch <= '9'; });
}
inline bool V3SourceCountForMaterialization(std::string_view value, std::size_t& count)
{
    count = 0;
    if (!V3CanonicalSourceDecimal(value)) return false;
    for (const char ch : value)
    {
        const auto digit = static_cast<std::size_t>(ch - '0');
        if (count > ((std::numeric_limits<std::size_t>::max)() - digit) / 10) return false;
        count = count * 10 + digit;
    }
    return true;
}
inline const char* V3CountKindName(V3CountKind kind)
{
    switch (kind)
    {
    case V3CountKind::not_applicable: return "na";
    case V3CountKind::unknown: return "unknown";
    case V3CountKind::exact: return "exact";
    }
    return "invalid";
}
inline std::string V3FamilyBinding(const V3FamilyDescriptor& family)
{
    const auto valid = [](const V3ExactTaskSpec& task) {
        return V3CanonicalSourceDecimal(task.sourceDecimal) && V3CountValueCanonical(task.hover) &&
            V3CountValueCanonical(task.resolved);
    };
    if (!valid(family.a) || (family.b && !valid(*family.b))) return {};
    std::string result;
    const auto token = [&](std::string_view value) {
        result += std::to_string(value.size()); result += ':'; result.append(value);
    };
    const auto task = [&](const V3ExactTaskSpec& spec) {
        token(spec.sourceDecimal);
        token(V3CountKindName(spec.hover.kind)); token(spec.hover.decimal);
        token(V3CountKindName(spec.resolved.kind)); token(spec.resolved.decimal);
    };
    token("global_pure_file_family_v3"); task(family.a);
    token(family.b ? "active" : "inactive");
    if (family.b) task(*family.b);
    return result;
}
// The generalized source family has an independent identity. This encodes
// inputs only; it does not authorize a V4 parser, event graph or product run.
// Keep V3 binding/cursor bytes unchanged and never infer counts or format data.
inline std::string V4FamilyBinding(const V4FamilyDescriptor& family)
{
    if (!V4FamilySlotsCanonical(family)) return {};
    std::string result;
    const auto token = [&](std::string_view value) {
        result += std::to_string(value.size()); result += ':'; result.append(value);
    };
    const auto count = [&](const V3CountValue& item) {
        token(V3CountKindName(item.kind)); token(item.decimal);
    };
    const auto payloadKind = [&](V4PayloadKind kind) {
        switch (kind)
        {
        case V4PayloadKind::not_applicable: token("na"); break;
        case V4PayloadKind::unknown_unresolved: token("unknown_unresolved"); break;
        case V4PayloadKind::exact: token("exact"); break;
        }
    };
    const auto inputBoolean = [&](V4InputBoolean value) {
        switch (value)
        {
        case V4InputBoolean::not_applicable: token("na"); break;
        case V4InputBoolean::unknown_unresolved: token("unknown_unresolved"); break;
        case V4InputBoolean::no: token("no"); break;
        case V4InputBoolean::yes: token("yes"); break;
        }
    };
    const auto task = [&](const V4TaskSpec& spec) {
        token(V4SourceKindName(spec.kind));
        count(spec.selectionCount); count(spec.fileCount); count(spec.virtualCount);
        count(spec.hover); count(spec.resolved);
        token(std::to_string(spec.payloads.size()));
        for (const auto& slot : spec.payloads)
        {
            token(slot.format); token(slot.medium); token(slot.index);
            payloadKind(slot.kind); token(slot.hex);
        }
        const auto indexed = [&](const std::vector<V4IndexedUtf16Slot>& slots) {
            token(std::to_string(slots.size()));
            for (const auto& slot : slots)
            {
                token(slot.index); payloadKind(slot.kind); token(slot.utf16leHex);
                switch (slot.base)
                {
                case V4PathBase::raw: token("raw"); break;
                case V4PathBase::owned_root_relative: token("owned_root_relative"); break;
                }
            }
        };
        indexed(spec.filePaths); indexed(spec.virtualNames);
        token(std::to_string(spec.fileContents.size()));
        for (const auto& slot : spec.fileContents)
        { token(slot.index); payloadKind(slot.kind); token(slot.hex); }
        token(std::to_string(spec.directoryRoots.size()));
        for (const auto& root : spec.directoryRoots)
        {
            token(root.rootIndex);
            switch (root.completeness)
            {
            case V4DirectoryCompleteness::not_applicable: token("na"); break;
            case V4DirectoryCompleteness::unknown_unresolved: token("unknown_unresolved"); break;
            case V4DirectoryCompleteness::partial_unresolved: token("partial_unresolved"); break;
            case V4DirectoryCompleteness::complete: token("complete"); break;
            }
        }
        token(std::to_string(spec.directoryNodes.size()));
        for (const auto& node : spec.directoryNodes)
        {
            token(node.rootIndex); token(node.slotIndex);
            payloadKind(node.pathKind); token(node.relativePathUtf16Hex);
            switch (node.nodeKind)
            {
            case V4DirectoryNodeKind::file: token("file"); break;
            case V4DirectoryNodeKind::directory: token("directory"); break;
            case V4DirectoryNodeKind::other_unresolved: token("other_unresolved"); break;
            case V4DirectoryNodeKind::unknown_unresolved: token("unknown_unresolved"); break;
            }
            payloadKind(node.contentKind); token(node.contentHex); count(node.objectIdentity);
        }
        token(std::to_string(spec.requestResults.size()));
        for (const auto& slot : spec.requestResults)
        {
            token(slot.format); token(slot.medium); token(slot.index); token(slot.phase);
            payloadKind(slot.kind); token(slot.hresultHex);
        }
        switch (spec.preparation.mode)
        {
        case V4SelectionPreparationMode::none: token("none"); break;
        case V4SelectionPreparationMode::single_click: token("single_click"); break;
        case V4SelectionPreparationMode::ctrl_sequence: token("ctrl_sequence"); break;
        case V4SelectionPreparationMode::marquee: token("marquee"); break;
        case V4SelectionPreparationMode::shift_range: token("shift_range"); break;
        case V4SelectionPreparationMode::ctrl_shift_range: token("ctrl_shift_range"); break;
        case V4SelectionPreparationMode::other_unresolved: token("other_unresolved"); break;
        case V4SelectionPreparationMode::unknown_unresolved: token("unknown_unresolved"); break;
        }
        token(std::to_string(spec.preparation.steps.size()));
        for (const auto& step : spec.preparation.steps)
        {
            switch (step.action)
            {
            case V4SelectionPreparationAction::pointer_down: token("pointer_down"); break;
            case V4SelectionPreparationAction::pointer_move: token("pointer_move"); break;
            case V4SelectionPreparationAction::pointer_up: token("pointer_up"); break;
            case V4SelectionPreparationAction::session_end: token("session_end"); break;
            case V4SelectionPreparationAction::other_unresolved: token("other_unresolved"); break;
            }
            switch (step.button)
            {
            case V4SelectionPreparationButton::none: token("none"); break;
            case V4SelectionPreparationButton::left: token("left"); break;
            case V4SelectionPreparationButton::right: token("right"); break;
            case V4SelectionPreparationButton::middle: token("middle"); break;
            case V4SelectionPreparationButton::unknown_unresolved: token("unknown_unresolved"); break;
            }
            count(step.session); payloadKind(step.modifiersKind); token(step.modifiersHex); count(step.member);
            payloadKind(step.xKind); token(step.xHex); payloadKind(step.yKind); token(step.yHex);
        }
        token(std::to_string(spec.preparation.outsideIdentities.size()));
        for (const auto& outside : spec.preparation.outsideIdentities)
        {
            token(outside.index);
            switch (outside.scope)
            {
            case V4SelectionOutsideScope::desktop: token("desktop"); break;
            case V4SelectionOutsideScope::collection: token("collection"); break;
            case V4SelectionOutsideScope::desktop_files: token("desktop_files"); break;
            case V4SelectionOutsideScope::dock: token("dock"); break;
            case V4SelectionOutsideScope::running_items: token("running_items"); break;
            case V4SelectionOutsideScope::popup: token("popup"); break;
            case V4SelectionOutsideScope::widget: token("widget"); break;
            case V4SelectionOutsideScope::other_unresolved: token("other_unresolved"); break;
            case V4SelectionOutsideScope::unknown_unresolved: token("unknown_unresolved"); break;
            }
            payloadKind(outside.identityKind); token(outside.identityHex); count(outside.alias);
        }
        token(std::to_string(spec.dimensions.size()));
        for (const auto& dimension : spec.dimensions)
        {
            token(dimension.index);
            switch (dimension.target)
            {
            case V4DimensionTarget::source_span_columns: token("source_span_columns"); break;
            case V4DimensionTarget::source_span_rows: token("source_span_rows"); break;
            case V4DimensionTarget::source_page_columns: token("source_page_columns"); break;
            case V4DimensionTarget::source_page_rows: token("source_page_rows"); break;
            case V4DimensionTarget::target_span_columns: token("target_span_columns"); break;
            case V4DimensionTarget::target_span_rows: token("target_span_rows"); break;
            case V4DimensionTarget::target_page_columns: token("target_page_columns"); break;
            case V4DimensionTarget::target_page_rows: token("target_page_rows"); break;
            case V4DimensionTarget::member_span_columns: token("member_span_columns"); break;
            case V4DimensionTarget::member_span_rows: token("member_span_rows"); break;
            }
            count(dimension.member); payloadKind(dimension.kind); token(dimension.valueHex);
        }
        token(std::to_string(spec.positions.size()));
        for (const auto& position : spec.positions)
        {
            token(position.index);
            switch (position.target)
            {
            case V4PositionTarget::visible: token("visible"); break;
            case V4PositionTarget::storage: token("storage"); break;
            }
            count(position.member); count(position.position);
        }
        payloadKind(spec.categoryRulesKind); token(std::to_string(spec.categoryRules.size()));
        for (const auto& rule : spec.categoryRules)
        {
            payloadKind(rule.idKind); token(rule.idUtf16Hex);
            payloadKind(rule.labelKind); token(rule.labelUtf16Hex);
            payloadKind(rule.extensionsKind); token(rule.extensionsUtf16Hex);
            inputBoolean(rule.enabled);
        }
        token(std::to_string(spec.shortcutTargets.size()));
        for (const auto& target : spec.shortcutTargets)
        {
            token(target.index); inputBoolean(target.classified); inputBoolean(target.directory);
            inputBoolean(target.application); payloadKind(target.extensionKind); token(target.extensionUtf16Hex);
        }
        const auto group = [&](const V4GroupEndpointInput& input) {
            payloadKind(input.childrenKind); token(std::to_string(input.children.size()));
            for (const auto& child : input.children)
            {
                payloadKind(child.identityKind); token(child.idUtf16Hex);
                payloadKind(child.typeKind); token(child.typeHex);
            }
            count(input.activeChild); count(input.hitChild);
        };
        group(spec.sourceGroup); group(spec.targetGroup);
    };
    token("global_family_v4_15"); task(family.a); token(family.b ? "active" : "inactive");
    if (family.b) task(*family.b);
    return result;
}
// This builds only the exact pure-file selection family. The caller supplies
// a materialization budget, which is deliberately excluded from the logical
// identity. Failure to materialize retains the descriptor and is not pruning.
// No old model parser or production driver is allowed to consume this schema.
inline V3FamilySchema BuildV3FamilySchema(const V3FamilyDescriptor& family, std::size_t memberBudget)
{
    V3FamilySchema result;
    try
    {
        result.binding = V3FamilyBinding(family);
        if (result.binding.empty()) { result.reason = "v3_family_descriptor_invalid"; return result; }
        std::size_t a = 0, b = 0;
        if (!V3SourceCountForMaterialization(family.a.sourceDecimal, a) ||
            (family.b && !V3SourceCountForMaterialization(family.b->sourceDecimal, b)) ||
            a > memberBudget || b > memberBudget - a)
        {
            result.status = V3FamilyStatus::resource_unavailable;
            result.reason = "v3_family_member_materialization_unavailable";
            return result;
        }
        V3TaskCounts aCounts{a, family.a.hover, family.a.resolved};
        V3TaskCounts bCounts;
        if (family.b) bCounts = {b, family.b->hover, family.b->resolved};
        std::vector<Axis> axes;
        AppendEndpointAxes(axes);
        const auto selection = AppendSelectionAxesV3(axes, aCounts, a, false);
        if (selection != V3SelectionStatus::ok)
        {
            result.status = selection == V3SelectionStatus::resource_unavailable ? V3FamilyStatus::resource_unavailable :
                selection == V3SelectionStatus::invalid_descriptor ? V3FamilyStatus::invalid_descriptor : V3FamilyStatus::schema_error;
            result.reason = "v3_family_selection_schema_unavailable";
            return result;
        }
        AppendTransferAxes(axes);
        const auto second = AppendSecondTaskAxesV3(axes, aCounts, family.b ? &bCounts : nullptr);
        if (second != V3SelectionStatus::ok)
        {
            result.status = second == V3SelectionStatus::resource_unavailable ? V3FamilyStatus::resource_unavailable :
                second == V3SelectionStatus::invalid_descriptor ? V3FamilyStatus::invalid_descriptor : V3FamilyStatus::schema_error;
            result.reason = "v3_family_second_task_schema_unavailable";
            return result;
        }
        const auto taskCount = std::find_if(axes.begin(), axes.end(), [](const Axis& axis) {
            return axis.id == "X03.task_count";
        });
        const std::string taskCountValue = family.b ? "two" : "one";
        if (taskCount == axes.end() || std::find(taskCount->values.begin(), taskCount->values.end(), taskCountValue) == taskCount->values.end())
        {
            result.status = V3FamilyStatus::schema_error; result.reason = "v3_family_task_count_axis_missing"; return result;
        }
        taskCount->values = {taskCountValue};
        if (BuildIndex(axes)->size() != axes.size())
        {
            result.status = V3FamilyStatus::schema_error; result.reason = "v3_family_duplicate_axis"; return result;
        }
        result.axes = std::move(axes);
        result.status = V3FamilyStatus::schema_only;
        result.reason = "v3_family_graph_execution_and_payload_constraints_unimplemented";
    }
    catch (const std::bad_alloc&)
    { result.status = V3FamilyStatus::resource_unavailable; result.reason = "v3_family_allocation_unavailable"; result.axes.clear(); }
    catch (const std::length_error&)
    { result.status = V3FamilyStatus::resource_unavailable; result.reason = "v3_family_length_unavailable"; result.axes.clear(); }
    catch (const std::exception&)
    { result.status = V3FamilyStatus::schema_error; result.reason = "v3_family_schema_exception"; result.axes.clear(); }
    catch (...)
    { result.status = V3FamilyStatus::schema_error; result.reason = "v3_family_schema_unknown_exception"; result.axes.clear(); }
    return result;
}

// The generalized V4 schema is still only a declaration of input dimensions.
// It remains separate from old validators and executors until format
// applicability, event graph, source construction and production oracles close.
enum class V4FamilyStatus { schema_only, unresolved, invalid_descriptor, resource_unavailable, schema_error };
struct V4FamilySchema
{
    V4FamilyStatus status = V4FamilyStatus::invalid_descriptor;
    std::string binding;
    std::vector<Axis> axes;
    std::vector<std::string> unresolved;
};
inline V4FamilySchema BuildV4FamilySchema(const V4FamilyDescriptor& family, const V4AxisBudget& budget)
{
    V4FamilySchema result;
    try
    {
        result.binding = V4FamilyBinding(family);
        if (result.binding.empty()) { result.unresolved.emplace_back("v4_family_descriptor_invalid"); return result; }
        const auto logical = V4MaterializeCount(family.a.selectionCount, budget.logicalMembers, false);
        if (logical.status != V4AxisStatus::schema_only)
        {
            result.status = logical.status == V4AxisStatus::resource_unavailable ? V4FamilyStatus::resource_unavailable :
                logical.status == V4AxisStatus::invalid_descriptor ? V4FamilyStatus::invalid_descriptor : V4FamilyStatus::unresolved;
            result.unresolved.emplace_back("v4_task_a_selection_count_unavailable"); return result;
        }
        std::vector<Axis> axes;
        AppendEndpointAxes(axes);
        const auto selection = AppendSelectionAxesV4(axes, family.a, logical.value, false, budget);
        result.unresolved = selection.unresolved;
        if (selection.status != V4AxisStatus::schema_only)
        {
            result.status = selection.status == V4AxisStatus::resource_unavailable ? V4FamilyStatus::resource_unavailable :
                selection.status == V4AxisStatus::invalid_descriptor ? V4FamilyStatus::invalid_descriptor : V4FamilyStatus::unresolved;
            return result;
        }
        AppendTransferAxes(axes);
        const auto second = AppendSecondTaskAxesV4(axes, family, budget);
        result.unresolved.insert(result.unresolved.end(), second.unresolved.begin(), second.unresolved.end());
        if (second.status != V4AxisStatus::schema_only)
        {
            result.status = second.status == V4AxisStatus::resource_unavailable ? V4FamilyStatus::resource_unavailable :
                second.status == V4AxisStatus::invalid_descriptor ? V4FamilyStatus::invalid_descriptor : V4FamilyStatus::unresolved;
            return result;
        }
        const auto applyDimensions = [&](const V4TaskSpec& task, bool isTaskB) {
            const auto dimensions = ApplyV4ExactDimensions(axes, task, isTaskB);
            result.unresolved.insert(result.unresolved.end(), dimensions.unresolved.begin(), dimensions.unresolved.end());
            if (dimensions.status == V4AxisStatus::schema_only) return true;
            result.status = dimensions.status == V4AxisStatus::resource_unavailable ? V4FamilyStatus::resource_unavailable :
                dimensions.status == V4AxisStatus::invalid_descriptor ? V4FamilyStatus::invalid_descriptor : V4FamilyStatus::unresolved;
            return false;
        };
        if (!applyDimensions(family.a, false) || (family.b && !applyDimensions(*family.b, true))) return result;
        const auto taskCount = std::find_if(axes.begin(), axes.end(), [](const Axis& axis) { return axis.id == "X03.task_count"; });
        const std::string taskCountValue = family.b ? "two" : "one";
        if (taskCount == axes.end() || std::find(taskCount->values.begin(), taskCount->values.end(), taskCountValue) == taskCount->values.end() ||
            BuildIndex(axes)->size() != axes.size())
        {
            result.status = V4FamilyStatus::schema_error; result.unresolved.emplace_back("v4_family_schema_invalid"); return result;
        }
        taskCount->values = {taskCountValue};
        result.axes = std::move(axes);
        result.status = V4FamilyStatus::schema_only;
        result.unresolved.emplace_back("v4_family_format_applicability_graph_and_execution_unimplemented");
    }
    catch (const std::bad_alloc&)
    { result.status = V4FamilyStatus::resource_unavailable; result.axes.clear(); result.unresolved.emplace_back("v4_family_allocation_unavailable"); }
    catch (const std::length_error&)
    { result.status = V4FamilyStatus::resource_unavailable; result.axes.clear(); result.unresolved.emplace_back("v4_family_length_unavailable"); }
    catch (const std::exception&)
    { result.status = V4FamilyStatus::schema_error; result.axes.clear(); result.unresolved.emplace_back("v4_family_schema_exception"); }
    catch (...)
    { result.status = V4FamilyStatus::schema_error; result.axes.clear(); result.unresolved.emplace_back("v4_family_schema_unknown_exception"); }
    return result;
}

// Decimal arithmetic avoids truncating the raw product to machine word size.
inline std::string Normalize(std::string value)
{
    if (value.empty() || std::any_of(value.begin(), value.end(), [](unsigned char c) { return !std::isdigit(c); }))
        throw std::runtime_error("ordinal must be an unsigned decimal integer");
    const auto first = value.find_first_not_of('0');
    return first == std::string::npos ? "0" : value.substr(first);
}
inline std::string Multiply(std::string value, std::size_t factor)
{
    std::size_t carry = 0;
    for (std::size_t i = value.size(); i-- > 0;)
    {
        const std::size_t n = static_cast<std::size_t>(value[i] - '0') * factor + carry;
        value[i] = static_cast<char>('0' + n % 10);
        carry = n / 10;
    }
    while (carry) { value.insert(value.begin(), static_cast<char>('0' + carry % 10)); carry /= 10; }
    return Normalize(value);
}
inline std::string RawCount(const std::vector<Axis>& axes)
{
    std::string result = "1";
    for (const auto& axis : axes) if (axis.role != Role::Derived) result = Multiply(result, axis.values.size());
    return result;
}
inline std::size_t Divide(std::string& value, std::size_t divisor)
{
    std::size_t remainder = 0;
    for (auto& digit : value)
    {
        const auto n = remainder * 10 + static_cast<std::size_t>(digit - '0');
        digit = static_cast<char>('0' + n / divisor);
        remainder = n % divisor;
    }
    value = Normalize(value);
    return remainder;
}
inline void Increment(std::string& value)
{
    for (std::size_t i = value.size(); i-- > 0;)
    {
        if (value[i] != '9') { ++value[i]; return; }
        value[i] = '0';
    }
    value.insert(value.begin(), '1');
}
inline bool Less(std::string left, std::string right)
{
    left = Normalize(std::move(left)); right = Normalize(std::move(right));
    return left.size() != right.size() ? left.size() < right.size() : left < right;
}
inline std::string Add(std::string left, std::string right)
{
    left = Normalize(std::move(left)); right = Normalize(std::move(right));
    std::string result;
    int carry = 0;
    while (!left.empty() || !right.empty() || carry)
    {
        int digit = carry;
        if (!left.empty()) { digit += left.back() - '0'; left.pop_back(); }
        if (!right.empty()) { digit += right.back() - '0'; right.pop_back(); }
        result.push_back(static_cast<char>('0' + digit % 10));
        carry = digit / 10;
    }
    std::reverse(result.begin(), result.end());
    return Normalize(result);
}
inline std::string Subtract(std::string left, std::string right)
{
    left = Normalize(std::move(left)); right = Normalize(std::move(right));
    if (Less(left, right)) throw std::runtime_error("negative global count");
    int borrow = 0;
    for (std::size_t offset = 0; offset < left.size(); ++offset)
    {
        const auto at = left.size() - 1 - offset;
        int digit = left[at] - '0' - borrow;
        if (offset < right.size()) digit -= right[right.size() - 1 - offset] - '0';
        borrow = digit < 0 ? 1 : 0;
        if (borrow) digit += 10;
        left[at] = static_cast<char>('0' + digit);
    }
    return Normalize(left);
}
inline Candidate Decode(const std::vector<Axis>& axes, std::string ordinal)
{
    Candidate candidate{Normalize(ordinal), std::vector<std::string>(axes.size()), BuildIndex(axes)};
    auto quotient = candidate.ordinal;
    for (std::size_t i = axes.size(); i-- > 0;)
        if (axes[i].role != Role::Derived)
            candidate.values[i] = axes[i].values[Divide(quotient, axes[i].values.size())];
    if (quotient != "0") throw std::runtime_error("ordinal outside global raw product");
    return candidate;
}
inline std::string Id(const std::vector<Axis>& axes, const Candidate& candidate)
{
    std::string id;
    for (std::size_t i = 0; i < axes.size(); ++i)
    {
        if (axes[i].role == Role::Derived) continue;
        if (!id.empty()) id += ';';
        id += axes[i].id + '=' + candidate.values[i];
    }
    return id;
}

// This context authenticates only a raw coordinate against a reconstructed
// exact family schema. Applicability, event mapping and production evidence
// remain separate; constructing it cannot authorize execution.
enum class V3CoordinateStatus { coordinate_ready, invalid_descriptor, resource_unavailable, schema_error, invalid_coordinate };
class V3FamilyValidatedContext
{
public:
    static V3FamilyValidatedContext Create(const V3FamilyDescriptor& family,
        std::size_t memberBudget, const Candidate& input)
    {
        V3FamilyValidatedContext result;
        try
        {
            auto schema = BuildV3FamilySchema(family, memberBudget);
            result.familyBinding_ = std::move(schema.binding);
            result.reason_ = std::move(schema.reason);
            if (schema.status != V3FamilyStatus::schema_only)
            {
                result.status_ = schema.status == V3FamilyStatus::resource_unavailable ? V3CoordinateStatus::resource_unavailable :
                    schema.status == V3FamilyStatus::invalid_descriptor ? V3CoordinateStatus::invalid_descriptor : V3CoordinateStatus::schema_error;
                return result;
            }
            auto lookup = BuildIndex(schema.axes);
            if (lookup->size() != schema.axes.size())
            { result.status_ = V3CoordinateStatus::schema_error; result.reason_ = "v3_coordinate_schema_duplicate_axis"; return result; }
            if (input.values.size() != schema.axes.size() || (input.lookup && *input.lookup != *lookup))
            { result.status_ = V3CoordinateStatus::invalid_coordinate; result.reason_ = "v3_coordinate_shape_or_lookup_mismatch"; return result; }
            for (std::size_t i = 0; i < schema.axes.size(); ++i)
            {
                const auto& axis = schema.axes[i];
                const auto& value = input.values[i];
                if ((axis.role == Role::Derived && !value.empty()) ||
                    (axis.role != Role::Derived && std::find(axis.values.begin(), axis.values.end(), value) == axis.values.end()))
                { result.status_ = V3CoordinateStatus::invalid_coordinate; result.reason_ = "v3_coordinate_value_outside_schema"; return result; }
            }
            result.axes_ = std::move(schema.axes);
            result.candidate_.values = input.values;
            result.candidate_.lookup = std::move(lookup);
            // An external ordinal is not part of this identity and is not trusted.
            result.candidate_.ordinal.clear();
            result.taskBActive_ = family.b.has_value();
            if (!V3SourceCountForMaterialization(family.a.sourceDecimal, result.aCount_) ||
                (family.b && !V3SourceCountForMaterialization(family.b->sourceDecimal, result.bCount_)))
            { result.status_ = V3CoordinateStatus::schema_error; result.reason_ = "v3_coordinate_count_binding_unavailable"; return result; }
            result.status_ = V3CoordinateStatus::coordinate_ready;
            result.reason_ = "v3_coordinate_applicability_and_execution_unimplemented";
        }
        catch (const std::bad_alloc&)
        { result.status_ = V3CoordinateStatus::resource_unavailable; result.reason_ = "v3_coordinate_allocation_unavailable"; result.axes_.clear(); result.candidate_ = {}; }
        catch (const std::length_error&)
        { result.status_ = V3CoordinateStatus::resource_unavailable; result.reason_ = "v3_coordinate_length_unavailable"; result.axes_.clear(); result.candidate_ = {}; }
        catch (const std::exception&)
        { result.status_ = V3CoordinateStatus::schema_error; result.reason_ = "v3_coordinate_schema_exception"; result.axes_.clear(); result.candidate_ = {}; }
        catch (...)
        { result.status_ = V3CoordinateStatus::schema_error; result.reason_ = "v3_coordinate_unknown_exception"; result.axes_.clear(); result.candidate_ = {}; }
        return result;
    }
    V3CoordinateStatus Status() const noexcept { return status_; }
    const std::string& FamilyBinding() const noexcept { return familyBinding_; }
    const std::string& Reason() const noexcept { return reason_; }
    const std::vector<Axis>& Axes() const noexcept { return axes_; }
    const Candidate& Coordinate() const noexcept { return candidate_; }
    bool TaskBActive() const noexcept { return taskBActive_; }
    V3TransferStructuralAudit AuditTransfer() const
    {
        if (status_ != V3CoordinateStatus::coordinate_ready)
        {
            V3TransferStructuralAudit result;
            result.invalid = "v3_transfer_coordinate_unavailable";
            result.unresolved.push_back(reason_);
            return result;
        }
        return V3TransferValidatedAccess::Audit(axes_, candidate_, taskBActive_);
    }
    EventProgram TransferProgram() const
    {
        if (status_ != V3CoordinateStatus::coordinate_ready)
        {
            EventProgram result;
            result.unresolved.push_back("v3_program_coordinate_unavailable:" + reason_);
            return result;
        }
        return V3TransferValidatedAccess::Program(axes_, candidate_);
    }
    V3SelectionAudit AuditSelectionA() const
    {
        if (status_ != V3CoordinateStatus::coordinate_ready)
            return {status_ == V3CoordinateStatus::resource_unavailable ? V3SelectionAuditState::resource_unavailable :
                (status_ == V3CoordinateStatus::invalid_descriptor || status_ == V3CoordinateStatus::invalid_coordinate)
                    ? V3SelectionAuditState::invalid_candidate : V3SelectionAuditState::unresolved,
                "v3_selection_coordinate_unavailable"};
        return V3SelectionStructuralAccess::Check(axes_, candidate_, aCount_, {});
    }
    V3SelectionAudit AuditSelectionB() const
    {
        if (status_ != V3CoordinateStatus::coordinate_ready)
            return {status_ == V3CoordinateStatus::resource_unavailable ? V3SelectionAuditState::resource_unavailable :
                (status_ == V3CoordinateStatus::invalid_descriptor || status_ == V3CoordinateStatus::invalid_coordinate)
                    ? V3SelectionAuditState::invalid_candidate : V3SelectionAuditState::unresolved,
                "v3_selection_coordinate_unavailable"};
        if (!taskBActive_) return {V3SelectionAuditState::unresolved, "v3_selection_second_task_not_applicable"};
        return V3SelectionStructuralAccess::Check(axes_, candidate_, bCount_, "task_b.");
    }
private:
    V3FamilyValidatedContext() = default;
    V3CoordinateStatus status_ = V3CoordinateStatus::invalid_descriptor;
    std::string familyBinding_, reason_;
    std::vector<Axis> axes_;
    Candidate candidate_;
    std::size_t aCount_ = 0, bCount_ = 0;
    bool taskBActive_ = false;
};


// Like the V3 authority, this validates only a raw coordinate against a newly
// reconstructed V4 schema. It does not prove source materialization, format
// applicability, event causality, production dispatch or business results.
enum class V4CoordinateStatus { coordinate_ready, unresolved, invalid_descriptor, resource_unavailable, schema_error, invalid_coordinate };
class V4FamilyValidatedContext
{
public:
    static V4FamilyValidatedContext Create(const V4FamilyDescriptor& family, const V4AxisBudget& budget, const Candidate& input)
    {
        V4FamilyValidatedContext result;
        try
        {
            auto schema = BuildV4FamilySchema(family, budget);
            result.familyBinding_ = std::move(schema.binding);
            result.unresolved_ = std::move(schema.unresolved);
            if (schema.status != V4FamilyStatus::schema_only)
            {
                result.status_ = schema.status == V4FamilyStatus::resource_unavailable ? V4CoordinateStatus::resource_unavailable :
                    schema.status == V4FamilyStatus::invalid_descriptor ? V4CoordinateStatus::invalid_descriptor :
                    schema.status == V4FamilyStatus::schema_error ? V4CoordinateStatus::schema_error : V4CoordinateStatus::unresolved;
                return result;
            }
            auto lookup = BuildIndex(schema.axes);
            if (lookup->size() != schema.axes.size())
            { result.status_ = V4CoordinateStatus::schema_error; result.unresolved_.emplace_back("v4_coordinate_duplicate_axis"); return result; }
            if (input.values.size() != schema.axes.size() || (input.lookup && *input.lookup != *lookup))
            { result.status_ = V4CoordinateStatus::invalid_coordinate; result.unresolved_.emplace_back("v4_coordinate_shape_or_lookup_mismatch"); return result; }
            for (std::size_t i = 0; i < schema.axes.size(); ++i)
            {
                const auto& axis = schema.axes[i]; const auto& value = input.values[i];
                if ((axis.role == Role::Derived && !value.empty()) ||
                    (axis.role != Role::Derived && std::find(axis.values.begin(), axis.values.end(), value) == axis.values.end()))
                { result.status_ = V4CoordinateStatus::invalid_coordinate; result.unresolved_.emplace_back("v4_coordinate_value_outside_schema"); return result; }
            }
            result.axes_ = std::move(schema.axes);
            result.candidate_.values = input.values; result.candidate_.lookup = std::move(lookup);
            result.candidate_.ordinal.clear(); // caller ordinal is never an identity or proof
            result.taskBActive_ = family.b.has_value();
            result.status_ = V4CoordinateStatus::coordinate_ready;
            result.unresolved_.emplace_back("v4_coordinate_applicability_and_execution_unimplemented");
        }
        catch (const std::bad_alloc&)
        { result.status_ = V4CoordinateStatus::resource_unavailable; result.axes_.clear(); result.candidate_ = {}; }
        catch (const std::length_error&)
        { result.status_ = V4CoordinateStatus::resource_unavailable; result.axes_.clear(); result.candidate_ = {}; }
        catch (...)
        { result.status_ = V4CoordinateStatus::schema_error; result.axes_.clear(); result.candidate_ = {}; }
        return result;
    }
    V4CoordinateStatus Status() const noexcept { return status_; }
    const std::string& FamilyBinding() const noexcept { return familyBinding_; }
    const std::vector<std::string>& Unresolved() const noexcept { return unresolved_; }
    const std::vector<Axis>& Axes() const noexcept { return axes_; }
    const Candidate& Coordinate() const noexcept { return candidate_; }
    bool TaskBActive() const noexcept { return taskBActive_; }
    EventProgram TransferProgram() const
    {
        if (status_ != V4CoordinateStatus::coordinate_ready)
        {
            EventProgram result;
            result.unresolved.emplace_back("v4_program_coordinate_unavailable");
            return result;
        }
        return V4TransferValidatedAccess::Program(axes_, candidate_);
    }
private:
    V4FamilyValidatedContext() = default;
    V4CoordinateStatus status_ = V4CoordinateStatus::invalid_descriptor;
    std::string familyBinding_;
    std::vector<std::string> unresolved_;
    std::vector<Axis> axes_;
    Candidate candidate_;
    bool taskBActive_ = false;
};

inline bool PrefixAllows(const std::vector<Axis>& axes, const Candidate& prefix,
    std::string_view axis, std::string_view value)
{
    return axis.substr(0, 7) == "task_b."
        ? SecondPrefixAllows(axes, prefix, axis, value)
        : SelectionPrefixAllows(axes, prefix, axis, value);
}
// Selection prefix semantics have only been defined for the current Model().
// Parameterized schema families need their own versioned parser and counter.
inline void RequireSelectionSchema(const std::vector<Axis>& axes, SelectionSchemaVersion version)
{
    const auto supported = Model(version);
    if (axes.size() != supported.size())
        throw std::runtime_error("conditional_space_schema_unimplemented");
    for (std::size_t i = 0; i < axes.size(); ++i)
        if (axes[i].id != supported[i].id || axes[i].role != supported[i].role ||
            axes[i].values != supported[i].values || axes[i].label != supported[i].label)
            throw std::runtime_error("conditional_space_schema_unimplemented");
}
inline void RequireLegacySelectionSchema(const std::vector<Axis>& axes)
{ RequireSelectionSchema(axes, SelectionSchemaVersion::legacy_v1); }
// V2 is a migration interface only; a schema/candidate must be fully
// validated before this predicate is used. It does not authorize execution.
inline bool PrefixAllowsV2(const std::vector<Axis>& axes, const Candidate& prefix,
    std::string_view axis, std::string_view value)
{
    return axis.substr(0, 7) == "task_b."
        ? SecondPrefixAllowsV2(axes, prefix, axis, value)
        : SelectionPrefixAllowsV2(axes, prefix, axis, value);
}
inline Candidate ParseIdVersion(const std::vector<Axis>& axes, std::string_view id, SelectionSchemaVersion version)
{
    RequireSelectionSchema(axes, version);
    Candidate result{"", std::vector<std::string>(axes.size()), BuildIndex(axes)};
    std::size_t offset = 0;
    for (std::size_t i = 0; i < axes.size(); ++i)
    {
        if (axes[i].role == Role::Derived) continue;
        const auto end = id.find(';', offset);
        const auto token = id.substr(offset, end == std::string_view::npos ? id.size() - offset : end - offset);
        const auto equal = token.find('=');
        if (equal == std::string_view::npos || token.substr(0, equal) != axes[i].id)
            throw std::runtime_error("global ID has missing, reordered or unknown input axis");
        const auto value = token.substr(equal + 1);
        if (std::find(axes[i].values.begin(), axes[i].values.end(), value) == axes[i].values.end())
            throw std::runtime_error("global ID contains unknown value for " + axes[i].id);
        const bool allowed = version == SelectionSchemaVersion::legacy_v1
            ? PrefixAllows(axes, result, axes[i].id, value) : PrefixAllowsV2(axes, result, axes[i].id, value);
        if (!allowed)
            throw std::runtime_error("global ID violates a selection-count structural condition at " + axes[i].id);
        result.values[i] = std::string(value);
        if (end == std::string_view::npos) { offset = id.size(); break; }
        offset = end + 1;
    }
    if (offset != id.size() || Id(axes, result) != id)
        throw std::runtime_error("global ID is incomplete or has extra input axes");
    return result;
}
inline Candidate ParseId(const std::vector<Axis>& axes, std::string_view id)
{ return ParseIdVersion(axes, id, SelectionSchemaVersion::legacy_v1); }
inline std::string_view Value(const std::vector<Axis>& axes, const Candidate& candidate, std::string_view id)
{
    if (candidate.lookup)
    {
        const auto found = candidate.lookup->find(std::string(id));
        return found == candidate.lookup->end() ? std::string_view{} : std::string_view(candidate.values[found->second]);
    }
    for (std::size_t i = 0; i < axes.size(); ++i) if (axes[i].id == id) return candidate.values[i];
    return {};
}
// Prefix-dependent domains are controlled by selection counts and task count.
// Count contiguous ordinary axes iteratively to avoid a stack frame per member.
// Other unresolved relationships are never silently treated as exclusions.
class ConditionalSpace
{
public:
    explicit ConditionalSpace(const std::vector<Axis>& axes) : axes_(axes), index_(BuildIndex(axes))
    {
        // Prefix domains and cached counts are only established for Model().
        RequireLegacySelectionSchema(axes);
        for (const auto id : {"P02.resolved_count", "P02.many_count", "X03.task_count",
                              "task_b.P02.resolved_count", "task_b.P02.many_count"})
        {
            const auto found = index_->find(id);
            if (found == index_->end() || (!selectors_.empty() && found->second <= selectors_.back()))
                throw std::runtime_error("global selectors are missing or out of order");
            selectors_.push_back(found->second);
        }
    }
    std::string Count()
    {
        Candidate prefix{"", std::vector<std::string>(axes_.size()), index_};
        return CountFrom(0, prefix);
    }
    // Find the first complete prefix-legal candidate without first counting
    // every selector branch. Backtrack over an empty conditional domain;
    // derived entries are observations and never contribute a choice.
    Candidate First()
    {
        Candidate candidate{"0", std::vector<std::string>(axes_.size()), index_};
        std::vector<std::size_t> next(axes_.size(), 0);
        std::size_t i = 0;
        while (i < axes_.size())
        {
            const auto& axis = axes_[i];
            if (axis.role == Role::Derived) { ++i; continue; }
            bool selected = false;
            while (next[i] < axis.values.size())
            {
                const auto& value = axis.values[next[i]++];
                if (!PrefixAllows(axes_, candidate, axis.id, value)) continue;
                candidate.values[i] = value;
                selected = true;
                break;
            }
            if (selected) { ++i; continue; }
            candidate.values[i].clear();
            next[i] = 0;
            while (i > 0)
            {
                --i;
                if (axes_[i].role != Role::Derived) break;
            }
            if (i == 0 && (axes_[i].role == Role::Derived || next[i] == 0))
                throw std::runtime_error("conditional global product has no complete prefix-legal candidate");
            candidate.values[i].clear();
        }
        return candidate;
    }
    Candidate DecodeAt(std::string ordinal)
    {
        Candidate candidate{Normalize(std::move(ordinal)), std::vector<std::string>(axes_.size()), index_};
        auto remainder = candidate.ordinal;
        if (!Less(remainder, Count())) throw std::runtime_error("ordinal outside conditional global product");
        for (std::size_t i = 0; i < axes_.size(); ++i)
        {
            const auto& axis = axes_[i];
            if (axis.role == Role::Derived) continue;
            bool selected = false;
            for (const auto& value : axis.values)
            {
                if (!PrefixAllows(axes_, candidate, axis.id, value)) continue;
                candidate.values[i] = value;
                const auto branch = CountFrom(i + 1, candidate);
                if (Less(remainder, branch)) { selected = true; break; }
                remainder = Subtract(remainder, branch);
            }
            if (!selected) throw std::runtime_error("conditional global ordinal has no branch");
        }
        if (remainder != "0") throw std::runtime_error("conditional global ordinal has residual value");
        return candidate;
    }
private:
    const std::vector<Axis>& axes_;
    std::shared_ptr<const std::unordered_map<std::string, std::size_t>> index_;
    std::map<std::string, std::string> cache_;
    // Most absent member slots have a single allowed value. Share their
    // identical suffix count instead of copying a very large decimal string
    // once for every inactive field in both tasks.
    std::map<std::string, std::vector<std::shared_ptr<const std::string>>> suffixCounts_;
    std::vector<std::size_t> selectors_;
    std::string CountFrom(std::size_t i, Candidate& prefix)
    {
        if (i == axes_.size()) return "1";
        // A's selectors cannot affect B's domains. Omitting them from B cache
        // keys prevents duplicating each large suffix for every A selection.
        std::string state;
        if (i <= selectors_[2])
            for (const auto id : {"P02.resolved_count", "P02.many_count"})
                state += std::string(Value(axes_, prefix, id)) + '|';
        for (const auto id : {"X03.task_count", "task_b.P02.resolved_count", "task_b.P02.many_count"})
            state += std::string(Value(axes_, prefix, id)) + '|';
        const auto position = std::lower_bound(selectors_.begin(), selectors_.end(), i);
        if (position == selectors_.end() || *position != i)
        {
            const auto start = position == selectors_.begin() ? 0 : *(position - 1) + 1;
            const auto end = position == selectors_.end() ? axes_.size() : *position;
            const auto key = std::to_string(start) + '|' + state;
            // Counting visits a segment at its beginning; retaining every
            // suffix for every possible selection size would require memory
            // proportional to the whole high-dimensional model. Cache just
            // the scalar there. Ordinal decoding keeps suffixes only for its
            // current segment and selector state.
            if (i == start)
            {
                if (const auto found = cache_.find(key); found != cache_.end()) return found->second;
                auto total = CountFrom(end, prefix);
                for (std::size_t k = end; k-- > start;)
                {
                    const auto& item = axes_[k];
                    if (item.role == Role::Derived) continue;
                    std::size_t allowed = 0;
                    for (const auto& value : item.values)
                        if (PrefixAllows(axes_, prefix, item.id, value)) ++allowed;
                    total = allowed ? Multiply(std::move(total), allowed) : "0";
                }
                cache_.emplace(key, total);
                return total;
            }
            if (suffixCounts_.find(key) == suffixCounts_.end()) suffixCounts_.clear();
            auto [entry, inserted] = suffixCounts_.try_emplace(key);
            if (inserted)
            {
                auto& suffix = entry->second;
                suffix.resize(end - start + 1);
                suffix.back() = std::make_shared<const std::string>(CountFrom(end, prefix));
                const auto zero = std::make_shared<const std::string>("0");
                for (std::size_t k = end; k-- > start;)
                {
                    const auto& item = axes_[k];
                    if (item.role == Role::Derived) { suffix[k - start] = suffix[k - start + 1]; continue; }
                    std::size_t allowed = 0;
                    for (const auto& value : item.values)
                        if (PrefixAllows(axes_, prefix, item.id, value)) ++allowed;
                    if (!allowed) suffix[k - start] = zero;
                    else if (allowed == 1 || *suffix[k - start + 1] == "0")
                        suffix[k - start] = suffix[k - start + 1];
                    else suffix[k - start] = std::make_shared<const std::string>(
                        Multiply(*suffix[k - start + 1], allowed));
                }
            }
            return *entry->second[i - start];
        }
        const auto key = std::to_string(i) + '|' + state;
        if (const auto found = cache_.find(key); found != cache_.end()) return found->second;
        const auto& axis = axes_[i];
        std::string result = "0";
        for (const auto& value : axis.values)
        {
            if (!PrefixAllows(axes_, prefix, axis.id, value)) continue;
            prefix.values[i] = value;
            result = Add(result, CountFrom(i + 1, prefix));
        }
        prefix.values[i].clear();
        cache_.emplace(key, result);
        return result;
    }
};
// Only reviewed structural impossibilities may return an exclusion reason.
// Unknown applicability is not an exclusion and cannot imply a pass.
inline std::string StructuralExclusion(const std::vector<Axis>& axes, const Candidate& candidate)
{
    if (const auto reason = EndpointExclusion(axes, candidate); !reason.empty()) return std::string(reason);
    if (const auto reason = SelectionExclusion(axes, candidate); !reason.empty()) return std::string(reason);
    if (const auto reason = TransferExclusion(axes, candidate); !reason.empty()) return std::string(reason);
    if (const auto reason = SecondTaskExclusion(axes, candidate); !reason.empty()) return "task_b:" + std::string(reason);
    // Cross-task member and directory references may bind an earlier capture
    // than final resolved counts. Until the modeled phase is explicit, they
    // remain unresolved rather than being structurally excluded here.
    return {};
}
// V2 separates source members from the host's unobserved result hypothesis.
// This check is not an execution permit and must follow exact schema validation.
inline std::string StructuralExclusionV2(const std::vector<Axis>& axes, const Candidate& candidate)
{
    if (const auto reason = EndpointExclusion(axes, candidate); !reason.empty()) return std::string(reason);
    if (const auto reason = SelectionExclusionV2(axes, candidate); !reason.empty()) return std::string(reason);
    if (const auto reason = TransferExclusion(axes, candidate); !reason.empty()) return std::string(reason);
    if (const auto reason = SecondTaskExclusionV2(axes, candidate); !reason.empty()) return "task_b:" + std::string(reason);
    return {};
}
}
