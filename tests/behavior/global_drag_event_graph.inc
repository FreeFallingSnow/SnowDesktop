#pragma once

#include "global_drag_model.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace drag_global
{
// Defined by the global command; includes symbolic and global obligations.
EventProgram Program(const std::vector<Axis>& axes, const Candidate& candidate);
EventProgram ProgramVersion(const std::vector<Axis>& axes, const Candidate& candidate, SelectionSchemaVersion version);
}
namespace drag_global::v3_scheduler { class CheckpointStore; class V4NativeStagedFamilyRecovery; }

// Generic finite graph input generation for the SAME global Candidate. No
// product operation, completion, result assertion, or independent behavior
// slice is performed here. The S/G/H/X -> Requirement mapping and the
// production executor remain explicitly unimplemented.
namespace drag_global::event_graph
{
// Presence is an event proposition, not a test-side product decision. A
// conditional proposition has both possible outcomes until its actual
// condition, observation interval and production evidence are modeled.
enum class PresencePolicy { required, conditional_unresolved };
// Requirement IDs name semantic occurrences, not mutable vector positions.
// The occurrence is local to this role/session/task. `propertiesResolved`
// means kind/session/task/object/parameter have concrete, comparable values;
// use an explicit "na" token for structurally absent fields, not empty text.
struct Requirement
{
    std::string id;
    // A semantic requirement role is distinct from the unique requirement ID.
    // Occurrence numbers are meaningful within role/session/task.
    std::string role;
    std::string kind;
    std::string session;
    std::string task;
    std::string object;
    std::string parameter;
    std::size_t occurrence = 0;
    bool propertiesResolved = false;
    PresencePolicy presence = PresencePolicy::required;
    // Stable descriptive identity only; it is not executable input or proof
    // that a conditional event occurred or did not occur.
    std::string presenceCondition;
    // Optional exact identity of an execution session. Different nonempty
    // groups cannot denote the same occurrence, even when other properties
    // are unresolved. Empty retains the legacy unconstrained behavior.
    std::string exclusiveSessionGroup;
};
enum class PairRelation { same, distinct, either, unresolved };
inline bool DistinctSessionGroups(const Requirement& left, const Requirement& right) noexcept
{
    return !left.exclusiveSessionGroup.empty() && !right.exclusiveSessionGroup.empty() &&
        left.exclusiveSessionGroup != right.exclusiveSessionGroup;
}
struct PairRule
{
    std::string left;
    std::string right;
    PairRelation relation = PairRelation::unresolved;
};
struct Before { std::string earlier; std::string later; };
enum class PresenceRelation { present_implies_present, present_excludes_present };
inline std::string_view PresenceRelationName(PresenceRelation relation)
{
    switch (relation)
    {
    case PresenceRelation::present_implies_present: return "present_implies_present";
    case PresenceRelation::present_excludes_present: return "present_excludes_present";
    }
    return "invalid";
}
struct PresenceConstraint
{
    std::string premise;
    std::string consequence;
    PresenceRelation relation = PresenceRelation::present_implies_present;
};
struct Spec
{
    std::vector<Requirement> requirements;
    std::vector<PairRule> pairs;
    std::vector<Before> strictBefore;
    // Relations constrain presence only. Pair and order rules continue to
    // apply to present requirements; absence never implies product success.
    std::vector<PresenceConstraint> presenceConstraints;
    std::vector<std::string> unresolved;
    // False means missing causal relations cannot be assumed independent.
    bool causalCoverageComplete = false;
};
struct Variant
{
    std::string ordinal;
    std::string id;
    // Class per sorted requirement; first occurrence renumbers classes 0,1,...
    std::vector<std::size_t> classes;
    // Each item is the stable requirement-ID representative of a class.
    std::vector<std::string> order;
    std::vector<std::string> unresolved;
};
struct Page
{
    std::vector<Variant> variants;
    // The legacy rescan interface does not expose per-branch prune events.
    std::vector<std::string> obligations;
    std::string nextOrdinal;
    // Opaque binding to the complete candidate and normalized graph input.
    // Save it with nextOrdinal; do not reuse either with another Spec.
    std::string continuationBinding;
    bool exhausted = false;
    std::string structuralExclusion;
    std::vector<std::string> errors;
};
inline void AppendToken(std::string& result, std::string_view token);

// Context validates the full, ordered schema once. A stale caller lookup is
// never used. Retain the same Context, Spec and schema version for resumption.
struct Context
{
    SelectionSchemaVersion version;
    std::vector<Axis> axes;
    std::shared_ptr<const std::unordered_map<std::string, std::size_t>> lookup;
    std::string schemaBinding;
    std::vector<std::string> errors;
    explicit Context(const std::vector<Axis>& supplied, SelectionSchemaVersion selected = SelectionSchemaVersion::legacy_v1)
        : version(selected), axes(supplied), lookup(BuildIndex(axes))
    {
        if (version != SelectionSchemaVersion::legacy_v1 && version != SelectionSchemaVersion::source_members_v2)
        { errors.emplace_back("model_schema_version_invalid"); return; }
        std::vector<Axis> expected;
        std::string_view schemaTag;
        switch (version)
        {
        case SelectionSchemaVersion::legacy_v1:
            expected = ModelLegacyV1(); schemaTag = "global_model_schema_v1"; break;
        case SelectionSchemaVersion::source_members_v2:
            expected = ModelV2(); schemaTag = "global_model_schema_v2"; break;
        default: errors.emplace_back("model_schema_version_invalid"); return;
        }
        if (axes.size() != expected.size()) errors.emplace_back("model_schema_axis_count_mismatch");
        else for (std::size_t i = 0; i < axes.size(); ++i)
            if (axes[i].id != expected[i].id || axes[i].role != expected[i].role ||
                axes[i].values != expected[i].values || axes[i].label != expected[i].label)
                errors.push_back("model_schema_mismatch:" + axes[i].id);
        if (lookup->size() != axes.size()) errors.emplace_back("duplicate_model_axis");
        AppendToken(schemaBinding, schemaTag);
        AppendToken(schemaBinding, std::to_string(axes.size()));
        for (const auto& axis : axes)
        {
            AppendToken(schemaBinding, axis.id); AppendToken(schemaBinding, RoleName(axis.role));
            AppendToken(schemaBinding, axis.label); AppendToken(schemaBinding, std::to_string(axis.values.size()));
            for (const auto& value : axis.values) AppendToken(schemaBinding, value);
        }
    }
    Context(SelectionSchemaVersion selected, const std::vector<Axis>& supplied) : Context(supplied, selected) {}
};

inline void AppendToken(std::string& result, std::string_view token)
{
    result += std::to_string(token.size());
    result += ':';
    result.append(token);
}
inline std::vector<std::string> RevalidateContext(const Context& supplied)
{
    const Context current(supplied.axes, supplied.version);
    auto errors = current.errors;
    errors.insert(errors.end(), supplied.errors.begin(), supplied.errors.end());
    if (supplied.schemaBinding != current.schemaBinding || !supplied.lookup ||
        (supplied.lookup && *supplied.lookup != *current.lookup))
        errors.emplace_back("context_schema_or_lookup_mismatch");
    return errors;
}
// Explicit dispatch avoids treating any future or corrupted schema version as
// v2. V3 has a distinct identity context and cannot enter these helpers.
inline bool ExistingPrefixAllows(const Context& context, const Candidate& candidate,
    std::string_view id, std::string_view value)
{
    switch (context.version)
    {
    case SelectionSchemaVersion::legacy_v1: return PrefixAllows(context.axes, candidate, id, value);
    case SelectionSchemaVersion::source_members_v2: return PrefixAllowsV2(context.axes, candidate, id, value);
    default: throw std::runtime_error("event_graph_schema_version_unsupported");
    }
}
inline std::string ExistingStructuralExclusion(const Context& context, const Candidate& candidate)
{
    switch (context.version)
    {
    case SelectionSchemaVersion::legacy_v1: return StructuralExclusion(context.axes, candidate);
    case SelectionSchemaVersion::source_members_v2: return StructuralExclusionV2(context.axes, candidate);
    default: throw std::runtime_error("event_graph_schema_version_unsupported");
    }
}
inline std::string_view ExistingSelectionVersionTag(SelectionSchemaVersion version)
{
    switch (version)
    {
    case SelectionSchemaVersion::legacy_v1: return "selection_schema_legacy_v1";
    case SelectionSchemaVersion::source_members_v2: return "selection_schema_source_members_v2";
    default: throw std::runtime_error("event_graph_schema_version_unsupported");
    }
}
inline std::string_view ExistingGraphVersionTag(SelectionSchemaVersion version)
{
    switch (version)
    {
    case SelectionSchemaVersion::legacy_v1: return "global_drag_event_graph_v1";
    case SelectionSchemaVersion::source_members_v2: return "global_drag_event_graph_v2";
    default: throw std::runtime_error("event_graph_schema_version_unsupported");
    }
}

// Identity-only context for a V3 exact pure-file family. It is deliberately
// not convertible to the legacy/v2 graph Context: candidate applicability,
// ProgramV3, event mapping and production execution are not implemented yet.
// The budget controls materialization only and is never part of either ID.
struct V3RawCandidateIdentity
{
    V3FamilyStatus contextStatus = V3FamilyStatus::invalid_descriptor;
    bool resourceUnavailable = false;
    std::string id;
    std::vector<std::string> errors;
    std::vector<std::string> unresolved;
};
class V3FamilyIdentityContext
{
public:
    V3FamilyIdentityContext(const V3FamilyIdentityContext&) = default;
    V3FamilyIdentityContext(V3FamilyIdentityContext&&) = default;
    V3FamilyIdentityContext& operator=(const V3FamilyIdentityContext&) = default;
    V3FamilyIdentityContext& operator=(V3FamilyIdentityContext&&) = default;
    static V3FamilyIdentityContext Create(const V3FamilyDescriptor& family, std::size_t memberBudget)
    {
        V3FamilyIdentityContext result;
        try
        {
            const auto generated = BuildV3FamilySchema(family, memberBudget);
            result.status_ = generated.status;
            result.familyBinding_ = generated.binding;
            result.reason_ = generated.reason;
            if (generated.status != V3FamilyStatus::schema_only) return result;
            result.axes_ = generated.axes;
            result.lookup_ = BuildIndex(result.axes_);
            if (result.lookup_->size() != result.axes_.size())
            {
                result.status_ = V3FamilyStatus::schema_error; result.reason_ = "v3_identity_duplicate_axis";
                result.axes_.clear(); result.lookup_.reset(); return result;
            }
            AppendToken(result.schemaBinding_, "global_model_schema_v3_exact_family");
            AppendToken(result.schemaBinding_, result.familyBinding_);
            AppendToken(result.schemaBinding_, std::to_string(result.axes_.size()));
            for (const auto& axis : result.axes_)
            {
                AppendToken(result.schemaBinding_, axis.id); AppendToken(result.schemaBinding_, RoleName(axis.role));
                AppendToken(result.schemaBinding_, axis.label); AppendToken(result.schemaBinding_, std::to_string(axis.values.size()));
                for (const auto& value : axis.values) AppendToken(result.schemaBinding_, value);
            }
        }
        catch (const std::bad_alloc&)
        { result.status_ = V3FamilyStatus::resource_unavailable; result.reason_ = "v3_identity_allocation_unavailable"; result.axes_.clear(); result.lookup_.reset(); result.schemaBinding_.clear(); }
        catch (const std::length_error&)
        { result.status_ = V3FamilyStatus::resource_unavailable; result.reason_ = "v3_identity_length_unavailable"; result.axes_.clear(); result.lookup_.reset(); result.schemaBinding_.clear(); }
        catch (const std::exception&)
        { result.status_ = V3FamilyStatus::schema_error; result.reason_ = "v3_identity_schema_unavailable"; result.axes_.clear(); result.lookup_.reset(); result.schemaBinding_.clear(); }
        catch (...)
        { result.status_ = V3FamilyStatus::schema_error; result.reason_ = "v3_identity_unknown_error"; result.axes_.clear(); result.lookup_.reset(); result.schemaBinding_.clear(); }
        return result;
    }
    V3RawCandidateIdentity RawCandidateIdentity(const Candidate& candidate) const
    {
        V3RawCandidateIdentity result;
        result.contextStatus = status_;
        try
        {
        if (status_ != V3FamilyStatus::schema_only || !lookup_)
        { result.errors.emplace_back("v3_raw_candidate_schema_unavailable"); return result; }
        if (candidate.values.size() != axes_.size())
        { result.errors.emplace_back("v3_raw_candidate_shape_invalid"); return result; }
        if (candidate.lookup && *candidate.lookup != *lookup_)
        { result.errors.emplace_back("v3_raw_candidate_lookup_mismatch"); return result; }
        for (std::size_t i = 0; i < axes_.size(); ++i)
        {
            const auto& axis = axes_[i]; const auto& value = candidate.values[i];
            if (axis.role == Role::Derived)
            {
                if (!value.empty()) result.errors.push_back("v3_raw_candidate_derived_value_invalid:" + axis.id);
            }
            else if (std::find(axis.values.begin(), axis.values.end(), value) == axis.values.end())
                result.errors.push_back("v3_raw_candidate_domain_invalid:" + axis.id);
        }
        if (!result.errors.empty()) return result;
        AppendToken(result.id, "global_v3_raw_candidate_coordinate_v1");
        AppendToken(result.id, familyBinding_); AppendToken(result.id, schemaBinding_);
        AppendToken(result.id, Id(axes_, candidate));
        result.unresolved.emplace_back("v3_candidate_prefix_and_structure_validation_unimplemented");
        result.unresolved.emplace_back("v3_program_event_graph_and_execution_unimplemented");
        }
        catch (const std::bad_alloc&)
        { result.resourceUnavailable = true; result.id.clear(); result.errors.emplace_back("v3_raw_candidate_resource_unavailable"); }
        catch (const std::length_error&)
        { result.resourceUnavailable = true; result.id.clear(); result.errors.emplace_back("v3_raw_candidate_resource_unavailable"); }
        return result;
    }
    V3FamilyStatus Status() const noexcept { return status_; }
    const std::string& FamilyBinding() const noexcept { return familyBinding_; }
    const std::string& SchemaBinding() const noexcept { return schemaBinding_; }
    const std::string& Reason() const noexcept { return reason_; }
    const std::vector<Axis>& Axes() const noexcept { return axes_; }
    const std::shared_ptr<const std::unordered_map<std::string, std::size_t>>& Lookup() const noexcept { return lookup_; }
    bool CanGenerateGraphOrExecute() const noexcept { return false; }
private:
    V3FamilyIdentityContext() = default;
    V3FamilyStatus status_ = V3FamilyStatus::invalid_descriptor;
    std::string familyBinding_, schemaBinding_, reason_;
    std::vector<Axis> axes_;
    std::shared_ptr<const std::unordered_map<std::string, std::size_t>> lookup_;
};

// V4 has an independent generalized source descriptor and schema. This is
// identity-only: neither domain-valid values nor a binding establish source
// applicability, event mapping, product execution or business results. It
// cannot be converted to an older graph Context. Materialization budgets are
// deliberately absent from logical family, schema and candidate identities.
struct V4RawCandidateIdentity
{
    V4FamilyStatus contextStatus = V4FamilyStatus::invalid_descriptor;
    bool resourceUnavailable = false;
    std::string id;
    std::vector<std::string> errors, unresolved;
};
class V4FamilyIdentityContext
{
public:
    static V4FamilyIdentityContext Create(const V4FamilyDescriptor& family, const V4AxisBudget& budget)
    {
        V4FamilyIdentityContext result;
        try
        {
            const auto generated = BuildV4FamilySchema(family, budget);
            result.status_ = generated.status;
            result.familyBinding_ = generated.binding;
            result.unresolved_ = generated.unresolved;
            if (generated.status != V4FamilyStatus::schema_only) return result;
            result.axes_ = generated.axes;
            result.lookup_ = BuildIndex(result.axes_);
            if (!result.lookup_ || result.lookup_->size() != result.axes_.size())
            {
                result.status_ = V4FamilyStatus::schema_error;
                result.unresolved_.emplace_back("v4_identity_duplicate_axis");
                result.axes_.clear(); result.lookup_.reset(); return result;
            }
            AppendToken(result.schemaBinding_, "global_model_schema_v4_generalized_family_v1");
            AppendToken(result.schemaBinding_, result.familyBinding_);
            AppendToken(result.schemaBinding_, std::to_string(result.axes_.size()));
            for (const auto& axis : result.axes_)
            {
                AppendToken(result.schemaBinding_, axis.id); AppendToken(result.schemaBinding_, RoleName(axis.role));
                AppendToken(result.schemaBinding_, axis.label); AppendToken(result.schemaBinding_, std::to_string(axis.values.size()));
                for (const auto& value : axis.values) AppendToken(result.schemaBinding_, value);
            }
        }
        catch (const std::bad_alloc&)
        { result.status_ = V4FamilyStatus::resource_unavailable; result.axes_.clear(); result.lookup_.reset(); result.schemaBinding_.clear(); }
        catch (const std::length_error&)
        { result.status_ = V4FamilyStatus::resource_unavailable; result.axes_.clear(); result.lookup_.reset(); result.schemaBinding_.clear(); }
        catch (...)
        { result.status_ = V4FamilyStatus::schema_error; result.axes_.clear(); result.lookup_.reset(); result.schemaBinding_.clear(); }
        return result;
    }
    V4RawCandidateIdentity RawCandidateIdentity(const Candidate& candidate) const
    {
        V4RawCandidateIdentity result;
        result.contextStatus = status_;
        result.resourceUnavailable = status_ == V4FamilyStatus::resource_unavailable;
        try
        {
            if (status_ != V4FamilyStatus::schema_only || !lookup_)
            { result.errors.emplace_back("v4_raw_candidate_schema_unavailable"); return result; }
            if (candidate.values.size() != axes_.size())
            { result.errors.emplace_back("v4_raw_candidate_shape_invalid"); return result; }
            if (candidate.lookup && *candidate.lookup != *lookup_)
            { result.errors.emplace_back("v4_raw_candidate_lookup_mismatch"); return result; }
            for (std::size_t i = 0; i < axes_.size(); ++i)
            {
                const auto& axis = axes_[i]; const auto& value = candidate.values[i];
                if (axis.role == Role::Derived)
                {
                    if (!value.empty()) result.errors.push_back("v4_raw_candidate_derived_value_invalid:" + axis.id);
                }
                else if (std::find(axis.values.begin(), axis.values.end(), value) == axis.values.end())
                    result.errors.push_back("v4_raw_candidate_domain_invalid:" + axis.id);
            }
            if (!result.errors.empty()) return result;
            AppendToken(result.id, "global_v4_raw_candidate_coordinate_v1");
            AppendToken(result.id, familyBinding_); AppendToken(result.id, schemaBinding_);
            AppendToken(result.id, std::to_string(axes_.size()));
            for (std::size_t i = 0; i < axes_.size(); ++i)
            {
                AppendToken(result.id, axes_[i].id);
                AppendToken(result.id, candidate.values[i]);
            }
            result.unresolved = unresolved_;
            result.unresolved.emplace_back("v4_candidate_prefix_and_structure_validation_unimplemented");
            result.unresolved.emplace_back("v4_program_event_graph_and_execution_unimplemented");
        }
        catch (const std::bad_alloc&)
        { result.resourceUnavailable = true; result.id.clear(); }
        catch (const std::length_error&)
        { result.resourceUnavailable = true; result.id.clear(); }
        return result;
    }
    V4FamilyStatus Status() const noexcept { return status_; }
    const std::string& FamilyBinding() const noexcept { return familyBinding_; }
    const std::string& SchemaBinding() const noexcept { return schemaBinding_; }
    const std::vector<std::string>& Unresolved() const noexcept { return unresolved_; }
    const std::vector<Axis>& Axes() const noexcept { return axes_; }
    const std::shared_ptr<const std::unordered_map<std::string, std::size_t>>& Lookup() const noexcept { return lookup_; }
    bool CanGenerateGraphOrExecute() const noexcept { return false; }
private:
    V4FamilyIdentityContext() = default;
    V4FamilyStatus status_ = V4FamilyStatus::invalid_descriptor;
    std::string familyBinding_, schemaBinding_;
    std::vector<std::string> unresolved_;
    std::vector<Axis> axes_;
    std::shared_ptr<const std::unordered_map<std::string, std::size_t>> lookup_;
};

inline std::string_view RelationName(PairRelation relation)
{
    switch (relation)
    {
    case PairRelation::same: return "same";
    case PairRelation::distinct: return "distinct";
    case PairRelation::either: return "either";
    case PairRelation::unresolved: return "unresolved";
    }
    return "invalid";
}

// Fixed FNV-1a 64-bit algorithm over the UTF-8/ASCII bytes of the full ID.
// Unsigned multiplication wraps modulo 2^64. Changing shard count changes
// assignment; identical IDs and shard counts have stable assignment.
inline std::size_t Shard(std::string_view id, std::size_t shards)
{
    if (!shards) throw std::invalid_argument("event graph shard count must be positive");
    std::uint64_t hash = 14695981039346656037ull;
    for (const char character : id)
    {
        hash ^= static_cast<unsigned char>(character);
        hash *= 1099511628211ull;
    }
    return static_cast<std::size_t>(hash % static_cast<std::uint64_t>(shards));
}

// This limited mapping exposes a graph for inspection only. Even when the
// event subset is described, whole-candidate input mapping, object binding,
// conditional completion, gates and business assertions remain unfinished.
struct Mapping
{
    bool graphAvailable = false;
    std::string status = "mapping_unimplemented";
    std::string structuralExclusion;
    Spec spec;
    EventProgram program;
    std::vector<std::string> reasons;
    // Typed only where the source can prove resource failure; unclassified
    // internal failures must remain unavailable, never guessed from text.
    bool resourceUnavailable = false;
};

inline bool SameEventProgram(const EventProgram& left, const EventProgram& right)
{
    if (left.unresolved != right.unresolved || left.steps.size() != right.steps.size() ||
        left.postconditions.size() != right.postconditions.size()) return false;
    for (std::size_t i = 0; i < left.steps.size(); ++i)
        if (left.steps[i].kind != right.steps[i].kind || left.steps[i].parameter != right.steps[i].parameter) return false;
    for (std::size_t i = 0; i < left.postconditions.size(); ++i)
        if (left.postconditions[i].axis != right.postconditions[i].axis ||
            left.postconditions[i].value != right.postconditions[i].value) return false;
    return true;
}

inline Mapping MapExternalSpecCore(const std::vector<Axis>& axes, const Candidate& candidate,
    const EventProgram& suppliedProgram, Mapping result, bool allowV4SourceSteps = false);

// V4 admission validates the complete authority on both sides before the
// shared graph core may ever be used. A caller-supplied program or Spec is
// intentionally not accepted. V4 source configuration remains unimplemented;
// only independently mapped OLE call boundaries enter the preview graph.
inline Mapping MapV4FamilyCandidate(const V4FamilyIdentityContext& identity,
    const V4FamilyValidatedContext& validated)
{
    Mapping result;
    try
    {
        switch (identity.Status())
        {
        case V4FamilyStatus::schema_only: break;
        case V4FamilyStatus::resource_unavailable: result.status = "resource_unavailable"; result.resourceUnavailable = true; break;
        case V4FamilyStatus::invalid_descriptor: result.status = "invalid_descriptor"; break;
        case V4FamilyStatus::schema_error: result.status = "schema_error"; break;
        case V4FamilyStatus::unresolved: result.status = "schema_unavailable"; break;
        }
        if (identity.Status() != V4FamilyStatus::schema_only)
        {
            result.reasons = identity.Unresolved();
            result.reasons.emplace_back("v4_family_identity_schema_unavailable"); return result;
        }
        switch (validated.Status())
        {
        case V4CoordinateStatus::coordinate_ready: break;
        case V4CoordinateStatus::resource_unavailable: result.status = "resource_unavailable"; result.resourceUnavailable = true; break;
        case V4CoordinateStatus::invalid_descriptor: result.status = "invalid_descriptor"; break;
        case V4CoordinateStatus::schema_error: result.status = "schema_error"; break;
        case V4CoordinateStatus::invalid_coordinate: result.status = "invalid_candidate"; break;
        case V4CoordinateStatus::unresolved: result.status = "schema_unavailable"; break;
        }
        if (validated.Status() != V4CoordinateStatus::coordinate_ready)
        {
            result.reasons = validated.Unresolved();
            result.reasons.emplace_back("v4_validated_coordinate_unavailable"); return result;
        }
        if (identity.FamilyBinding().empty() || identity.FamilyBinding() != validated.FamilyBinding() ||
            identity.Axes().size() != validated.Axes().size())
        { result.status = "invalid_candidate"; result.reasons.emplace_back("v4_family_schema_context_mismatch"); return result; }
        for (std::size_t i = 0; i < identity.Axes().size(); ++i)
        {
            const auto& a = identity.Axes()[i]; const auto& b = validated.Axes()[i];
            if (a.id != b.id || a.role != b.role || a.label != b.label || a.values != b.values)
            { result.status = "invalid_candidate"; result.reasons.emplace_back("v4_family_schema_context_mismatch"); return result; }
        }
        const auto raw = identity.RawCandidateIdentity(validated.Coordinate());
        if (raw.resourceUnavailable) { result.status = "resource_unavailable"; result.resourceUnavailable = true; }
        else if (!raw.errors.empty() || raw.id.empty()) result.status = "invalid_candidate";
        result.reasons.insert(result.reasons.end(), raw.errors.begin(), raw.errors.end());
        result.reasons.insert(result.reasons.end(), raw.unresolved.begin(), raw.unresolved.end());
        result.reasons.insert(result.reasons.end(), validated.Unresolved().begin(), validated.Unresolved().end());
        if (result.status != "mapping_unimplemented") return result;
        const auto program = validated.TransferProgram();
        result.program = program;
        for (const auto& reason : program.unresolved)
            result.reasons.push_back("v4_program_preview:" + reason);
        result.reasons.emplace_back("v4_format_source_host_completion_and_business_unimplemented");
        if (Value(identity.Axes(), validated.Coordinate(), "T01") != "external_ingress")
        { result.reasons.emplace_back("v4_transport_specific_event_mapping_required"); return result; }
        result = MapExternalSpecCore(identity.Axes(), validated.Coordinate(), program, std::move(result), true);
        if (result.graphAvailable) result.status = "graph_preview_unexecuted";
    }
    catch (const std::bad_alloc&)
    { result.graphAvailable = false; result.status = "resource_unavailable"; result.resourceUnavailable = true; result.spec = {}; result.program = {}; }
    catch (const std::length_error&)
    { result.graphAvailable = false; result.status = "resource_unavailable"; result.resourceUnavailable = true; result.spec = {}; result.program = {}; }
    return result;
}

// This is an admission point into the shared mapping contract, not another
// generator. V3 raw coordinates are not yet structurally validated and no
// authoritative V3 Program is available here, so never construct an empty
// graph or pass them through the v1/v2 enumerator as if they were compatible.
inline Mapping MapV3FamilyCandidate(const V3FamilyIdentityContext& context, const Candidate& candidate)
{
    Mapping result;
    switch (context.Status())
    {
    case V3FamilyStatus::resource_unavailable:
        result.status = "resource_unavailable"; result.reasons.push_back(context.Reason()); return result;
    case V3FamilyStatus::invalid_descriptor:
        result.status = "invalid_descriptor"; result.reasons.push_back(context.Reason()); return result;
    case V3FamilyStatus::schema_error:
        result.status = "schema_error"; result.reasons.push_back(context.Reason()); return result;
    case V3FamilyStatus::schema_only: break;
    default: result.status = "schema_error"; result.reasons.emplace_back("v3_family_status_invalid"); return result;
    }
    const auto raw = context.RawCandidateIdentity(candidate);
    result.reasons.insert(result.reasons.end(), raw.errors.begin(), raw.errors.end());
    result.reasons.insert(result.reasons.end(), raw.unresolved.begin(), raw.unresolved.end());
    if (raw.resourceUnavailable)
        result.status = "resource_unavailable";
    else if (!raw.errors.empty()) result.status = "invalid_candidate";
    if (raw.id.empty()) result.reasons.emplace_back("v3_candidate_raw_identity_unavailable");
    result.reasons.emplace_back("v3_candidate_complete_structure_and_program_semantics_unavailable");
    result.reasons.emplace_back("v3_full_shared_event_graph_admission_unimplemented");
    return result;
}

// V3 family/candidate admission uses the model-owned validated object. Its
// selection/transfer checks are local audits, and TransferProgram remains a
// preview: neither supplies the complete applicability or event-identity
// contract required by the shared graph enumerator. Do not promote local
// exclusions to whole-candidate exclusions until that contract is closed.
inline Mapping MapV3FamilyCandidate(const V3FamilyIdentityContext& identity,
    const V3FamilyValidatedContext& validated, const EventProgram& suppliedProgram)
{
    Mapping result;
    try
    {
    if (identity.Status() != V3FamilyStatus::schema_only)
        return MapV3FamilyCandidate(identity, validated.Coordinate());
    switch (validated.Status())
    {
    case V3CoordinateStatus::coordinate_ready: break;
    case V3CoordinateStatus::invalid_descriptor: result.status = "invalid_descriptor"; break;
    case V3CoordinateStatus::resource_unavailable: result.status = "resource_unavailable"; break;
    case V3CoordinateStatus::schema_error: result.status = "schema_error"; break;
    case V3CoordinateStatus::invalid_coordinate: result.status = "invalid_candidate"; break;
    default: result.status = "schema_error"; break;
    }
    if (validated.Status() != V3CoordinateStatus::coordinate_ready)
    { result.reasons.push_back("v3_validated_coordinate_unavailable:" + validated.Reason()); return result; }
    if (identity.FamilyBinding() != validated.FamilyBinding() || identity.Axes().size() != validated.Axes().size())
    { result.status = "invalid_candidate"; result.reasons.emplace_back("v3_family_identity_context_mismatch"); return result; }
    for (std::size_t i = 0; i < identity.Axes().size(); ++i)
    {
        const auto& a = identity.Axes()[i]; const auto& b = validated.Axes()[i];
        if (a.id != b.id || a.role != b.role || a.values != b.values || a.label != b.label)
        { result.status = "invalid_candidate"; result.reasons.emplace_back("v3_family_schema_context_mismatch"); return result; }
    }
    result = MapV3FamilyCandidate(identity, validated.Coordinate());
    if (result.status != "mapping_unimplemented") return result;
    const auto expected = validated.TransferProgram();
    if (!SameEventProgram(suppliedProgram, expected))
    { result.status = "program_mismatch"; result.reasons.emplace_back("v3_program_candidate_mismatch"); return result; }
    result.program = expected;
    for (const auto& reason : expected.unresolved) result.reasons.push_back("v3_program_preview:" + reason);
    bool localExclusion = false;
    const auto recordSelection = [&](std::string_view task, const V3SelectionAudit& audit) {
        std::string_view state = "invalid";
        switch (audit.state)
        {
        case V3SelectionAuditState::resource_unavailable: state = "resource_unavailable"; result.status = "resource_unavailable"; break;
        case V3SelectionAuditState::invalid_candidate: state = "invalid_candidate"; result.status = "invalid_candidate"; break;
        case V3SelectionAuditState::excluded: state = "local_exclusion_not_promoted"; localExclusion = true; break;
        case V3SelectionAuditState::unresolved: state = "unresolved"; break;
        default: result.status = "schema_error"; break;
        }
        result.reasons.push_back("v3_selection_local_audit:" + std::string(task) + ":" + std::string(state) + ":" + std::string(audit.reason));
    };
    recordSelection("a", validated.AuditSelectionA());
    if (result.status != "mapping_unimplemented") return result;
    if (validated.TaskBActive()) recordSelection("b", validated.AuditSelectionB());
    else result.reasons.emplace_back("v3_selection_b_structurally_not_applicable");
    if (result.status != "mapping_unimplemented") return result;
    const auto transfer = validated.AuditTransfer();
    if (!transfer.invalid.empty())
    { result.status = "invalid_candidate"; result.reasons.push_back("v3_transfer_local_audit_invalid:" + transfer.invalid); }
    if (!transfer.localExclusion.empty())
    {
        result.reasons.push_back("v3_transfer_local_exclusion_not_promoted:" + transfer.localExclusion);
        localExclusion = true;
    }
    for (const auto& reason : transfer.unresolved) result.reasons.push_back("v3_transfer_local_audit:" + reason);
    result.reasons.emplace_back("v3_complete_applicability_and_event_identity_unavailable");
    result.reasons.emplace_back("v3_conditional_presence_and_background_events_unmodeled");
    if (!localExclusion && result.status == "mapping_unimplemented")
        return MapExternalSpecCore(identity.Axes(), validated.Coordinate(), expected, std::move(result));
    }
    catch (const std::bad_alloc&)
    { result.status = "resource_unavailable"; result.reasons.emplace_back("v3_mapping_allocation_unavailable"); }
    catch (const std::length_error&)
    { result.status = "resource_unavailable"; result.reasons.emplace_back("v3_mapping_length_unavailable"); }
    return result;
}

inline Mapping MapSimpleExternal(const Context& context, const Candidate& input, const EventProgram& suppliedProgram)
{
    Mapping result;
    result.program = suppliedProgram;
    result.reasons = RevalidateContext(context);
    if (!result.reasons.empty()) return result;
    if (input.values.size() != context.axes.size()) result.reasons.emplace_back("candidate_axis_count_mismatch");
    if (!result.reasons.empty()) return result;
    Candidate candidate = input;
    candidate.lookup = context.lookup;
    for (std::size_t i = 0; i < context.axes.size(); ++i)
    {
        const auto& axis = context.axes[i];
        const auto& value = candidate.values[i];
        if (axis.role == Role::Derived)
        {
            if (!value.empty()) result.reasons.push_back("candidate_derived_value_invalid:" + axis.id);
        }
        else if (std::find(axis.values.begin(), axis.values.end(), value) == axis.values.end())
            result.reasons.push_back("candidate_value_invalid:" + axis.id);
        else if (!ExistingPrefixAllows(context, candidate, axis.id, value))
            result.reasons.push_back("candidate_prefix_invalid:" + axis.id);
    }
    if (!result.reasons.empty()) return result;
    result.structuralExclusion = ExistingStructuralExclusion(context, candidate);
    if (!result.structuralExclusion.empty()) { result.status = "structural_exclusion"; return result; }
    const auto expected = drag_global::ProgramVersion(context.axes, candidate, context.version);
    if (!SameEventProgram(suppliedProgram, expected))
    { result.reasons.emplace_back("program_candidate_mismatch"); return result; }
    for (const auto& reason : suppliedProgram.unresolved) result.reasons.push_back("program_unresolved:" + reason);
    // A graph describes event structure, not proof that any candidate input
    // was constructed or any derived result observed. Keep each obligation
    // explicit even for inputs that happen not to affect this limited graph.
    result.reasons.emplace_back("whole_candidate_axis_mapping_unimplemented");
    for (const auto& axis : context.axes)
    {
        if (axis.role == Role::Hypothesis && axis.id.substr(0, 7) == "task_b." &&
            Value(context.axes, candidate, axis.id) == "na" &&
            Value(context.axes, candidate, "X03.task_count") == "one")
            continue; // No task B exists; this is a structural absence, not an outcome observation.
        result.reasons.push_back((axis.role == Role::Derived ? "derived_observation_unimplemented:" :
            axis.role == Role::Hypothesis ? "outcome_hypothesis_observation_unimplemented:" :
            "axis_construction_and_observation_unimplemented:") + axis.id);
    }
    if (context.version == SelectionSchemaVersion::source_members_v2)
        result.reasons.emplace_back("v2_source_payload_format_and_host_outcome_binding_unimplemented");
    return MapExternalSpecCore(context.axes, candidate, suppliedProgram, std::move(result));
}

// Shared symbolic external-call graph construction. Both v1/v2 and V3 must
// first validate their own candidate and Program before reaching this point.
inline Mapping MapExternalSpecCore(const std::vector<Axis>& axes, const Candidate& candidate,
    const EventProgram& suppliedProgram, Mapping result, bool allowV4SourceSteps)
{
    std::vector<std::string> scopeReasons;
    const auto v = [&](std::string_view id) { return Value(axes, candidate, id); };
    const auto is = [](std::string_view value, std::initializer_list<std::string_view> choices) {
        return std::find(choices.begin(), choices.end(), value) != choices.end();
    };
    const auto need = [&](bool condition, std::string_view id) {
        if (!condition) scopeReasons.push_back("simple_external_input_unimplemented:" + std::string(id));
    };
    need(v("T01") == "external_ingress", "T01");
    need(v("X03.task_count") == "one" && v("X03.submit_order") == "na" && v("X03.order") == "single", "X03.task_count_order");
    need(v("X03.callback_reentrant") == "no", "X03.callback_reentrant");
    need(v("X03.callback_duplicate") == "no", "X03.callback_duplicate");
    need(v("X03.refresh") == "normal", "X03.refresh");
    need(v("X02") == "none" && is(v("X02.position"), {"na", "no_interruption"}), "X02");
    need(v("G03.decision") == "na", "G03.decision");
    need(is(v("G04.termination"), {"drop", "leave"}), "G04.termination");
    if (v("G04.termination") == "leave")
        need(v("G02.release") == "na" && v("G02.release_buttons") == "na", "G02.release");
    need(is(v("H03.page_switch"), {"na", "none"}), "H03.page_switch");
    need(is(v("H03.after_drop"), {"na", "unchanged"}), "H03.after_drop");
    need(is(v("H05.scroll"), {"na", "unchanged"}), "H05.scroll");
    need(is(v("P05.source_reorder"), {"na", "unchanged"}), "P05.source_reorder");
    need(v("H08.later_direction") == "na", "H08.later_direction");
    for (const auto id : {"S01", "S02", "S03.dwell", "S03.navigation", "S04", "S05", "S06", "S07", "S08", "S09", "S10"})
        need(is(v(id), {"na", "none"}), id);
    for (const auto id : kLifecycleChangeFields)
    {
        need(is(v(id), {"na", "stable"}), id);
        const auto phase = std::string(id) + "_phase";
        need(v(phase) == "na", phase);
    }
    for (const auto id : kEnvironmentChangeFields)
    {
        need(!v(id).empty() && !EnvironmentChangeActive(id, v(id)), id);
        const auto phase = std::string(id) + "_phase";
        need(v(phase) == "na", phase);
    }
    if (!scopeReasons.empty())
    {
        result.reasons.insert(result.reasons.end(), scopeReasons.begin(), scopeReasons.end());
        return result;
    }

    bool entered = false, terminated = false, observed = false, asserted = false;
    std::size_t hoverOccurrence = 0;
    std::string previous;
    const auto add = [&](std::size_t index, std::string_view suffix, std::string_view role,
                         std::string_view kind, std::string_view parameter, std::size_t occurrence) {
        Requirement item;
        item.id = "program_" + std::to_string(index) + "_" + std::string(suffix);
        item.role = std::string(role); item.kind = std::string(kind);
        item.session = "session_a"; item.task = "task_a";
        item.object = "candidate_target_and_source"; item.parameter = std::string(parameter);
        item.occurrence = occurrence;
        // Session/target/source are symbolic until a real isolated fixture is
        // bound and observed. Never claim concrete production object identity.
        item.propertiesResolved = false;
        result.spec.requirements.push_back(item);
        if (!previous.empty()) result.spec.strictBefore.push_back({previous, item.id});
        previous = item.id;
    };
    for (std::size_t i = 0; i < suppliedProgram.steps.size(); ++i)
    {
        const auto& step = suppliedProgram.steps[i];
        if (allowV4SourceSteps && step.kind == "configure_v4_source_input")
        {
            // These are input-construction obligations, not production
            // events. Keep their original program indices and obligation;
            // never present their configuration as an executed graph node.
            result.reasons.push_back("v4_graph_source_step_unimplemented:" + std::to_string(i));
            continue;
        }
        const bool enter = step.kind == "external_enter";
        const bool hover = step.kind == "hover" || step.kind == "hover_later";
        const bool drop = step.kind == "release";
        const bool leave = step.kind == "external_leave";
        if (enter || hover || drop || leave)
        {
            if ((enter && (entered || terminated)) || (!enter && (!entered || terminated)) || observed || asserted ||
                (drop && v("G04.termination") != "drop") || (leave && v("G04.termination") != "leave"))
            { result.reasons.push_back("ole_program_sequence_unimplemented:" + std::to_string(i)); result.spec = {}; return result; }
            if (enter) entered = true;
            if (drop || leave) terminated = true;
            const auto role = hover ? std::string_view{"drag_over"} : std::string_view{step.kind};
            const auto occurrence = hover ? hoverOccurrence++ : 0;
            add(i, "call_start", role, "ole_call_start", step.parameter, occurrence);
            add(i, "call_return", role, "ole_call_return", step.parameter, occurrence);
        }
        else if (step.kind == "observe_completion_or_rejection" && terminated && !observed && !asserted && step.parameter == "single")
        {
            observed = true;
            add(i, "observe", step.kind, "production_observation", step.parameter, 0);
        }
        else if (step.kind == "assert_business_results" && observed && !asserted && step.parameter == "independent_oracle")
        {
            asserted = true;
            add(i, "observe", step.kind, "business_assertion", step.parameter, 0);
        }
        else { result.reasons.push_back("program_step_mapping_unimplemented:" + step.kind); result.spec = {}; return result; }
    }
    if (!entered || !terminated || !observed || !asserted)
    { result.reasons.emplace_back("ole_program_incomplete"); result.spec = {}; return result; }
    for (std::size_t i = 0; i < result.spec.requirements.size(); ++i)
        for (std::size_t j = i + 1; j < result.spec.requirements.size(); ++j)
            result.spec.pairs.push_back({result.spec.requirements[i].id, result.spec.requirements[j].id, PairRelation::distinct});
    // A linear sequence of API calls/observation requests says nothing about
    // production events happening inside those calls or conditional waits.
    result.spec.causalCoverageComplete = false;
    result.graphAvailable = true;
    result.reasons.emplace_back("event_object_coordinate_and_effect_binding_unimplemented");
    result.reasons.emplace_back("conditional_production_completion_and_gate_unimplemented");
    result.reasons.emplace_back("independent_business_oracle_unimplemented");
    // The graph and every derived variant retain the same per-axis and
    // program obligations as the mapping, not just a generic summary.
    result.spec.unresolved = result.reasons;
    return result;
}

class V3JoinedGraph;
// Only the V3 joined facade may construct this view. Internal presence and
// graph machinery receives the exact model-owned candidate and Program; it
// cannot reinterpret the view as a legacy Context.
class V3GraphKernelContext
{
    friend class V3JoinedGraph;
    V3GraphKernelContext(const V3FamilyIdentityContext& i, const V3FamilyValidatedContext& v,
        const EventProgram& p) : identity_(&i), validated_(&v), program_(&p) {}
public:
    const V3FamilyIdentityContext& Identity() const noexcept { return *identity_; }
    const V3FamilyValidatedContext& Validated() const noexcept { return *validated_; }
    const EventProgram& Program() const noexcept { return *program_; }
private:
    const V3FamilyIdentityContext* identity_;
    const V3FamilyValidatedContext* validated_;
    const EventProgram* program_;
};

inline std::string V3ProgramGraphBinding(const V3FamilyIdentityContext& identity, const EventProgram& program)
{
    std::string binding;
    AppendToken(binding, "v3_call_graph_candidate_program_binding_v1");
    AppendToken(binding, identity.SchemaBinding());
    AppendToken(binding, std::to_string(program.steps.size()));
    for (const auto& step : program.steps) { AppendToken(binding, step.kind); AppendToken(binding, step.parameter); }
    AppendToken(binding, std::to_string(program.unresolved.size()));
    for (const auto& reason : program.unresolved) AppendToken(binding, reason);
    AppendToken(binding, std::to_string(program.postconditions.size()));
    for (const auto& outcome : program.postconditions) { AppendToken(binding, outcome.axis); AppendToken(binding, outcome.value); }
    return binding;
}

inline std::vector<std::string> RevalidateContext(const V3GraphKernelContext& context)
{
    const auto mapping = MapV3FamilyCandidate(context.Identity(), context.Validated(), context.Program());
    if (mapping.graphAvailable) return {};
    auto errors = mapping.reasons;
    errors.push_back("v3_joined_graph_mapping_unavailable:" + mapping.status);
    return errors;
}

class V4JoinedGraph;
class V4NativeJoinedGraph;
inline Mapping MapV4NativePresenceCandidate(const V4FamilyIdentityContext&, const V4FamilyValidatedContext&);
class V4GraphKernelContext
{
    friend class V4JoinedGraph;
    friend class V4NativeJoinedGraph;
    V4GraphKernelContext(const V4FamilyIdentityContext& i, const V4FamilyValidatedContext& v, bool native = false)
        : identity_(&i), validated_(&v), native_(native) {}
public:
    const V4FamilyIdentityContext& Identity() const noexcept { return *identity_; }
    const V4FamilyValidatedContext& Validated() const noexcept { return *validated_; }
    bool Native() const noexcept { return native_; }
private:
    const V4FamilyIdentityContext* identity_;
    const V4FamilyValidatedContext* validated_;
    bool native_ = false;
};
inline std::vector<std::string> RevalidateContext(const V4GraphKernelContext& context)
{
    const auto mapping = context.Native() ? MapV4NativePresenceCandidate(context.Identity(), context.Validated()) :
        MapV4FamilyCandidate(context.Identity(), context.Validated());
    if (mapping.graphAvailable && mapping.status == "graph_preview_unexecuted") return {};
    // Preserve a typed resource condition through the shared graph core. The
    // V4 facade catches it; other mapping failures remain ordinary errors.
    if (mapping.resourceUnavailable) throw std::bad_alloc{};
    auto errors = mapping.reasons;
    errors.push_back("v4_joined_graph_mapping_unavailable:" + mapping.status);
    return errors;
}
inline std::string V4ProgramGraphBinding(const V4FamilyIdentityContext& identity,
    const Candidate& candidate, const EventProgram& program)
{
    const auto raw = identity.RawCandidateIdentity(candidate);
    if (raw.resourceUnavailable) throw std::bad_alloc{};
    if (!raw.errors.empty() || raw.id.empty()) return {};
    std::string binding;
    AppendToken(binding, "v4_call_graph_candidate_program_binding_v1");
    AppendToken(binding, identity.SchemaBinding()); AppendToken(binding, raw.id);
    AppendToken(binding, std::to_string(program.steps.size()));
    for (const auto& step : program.steps) { AppendToken(binding, step.kind); AppendToken(binding, step.parameter); }
    AppendToken(binding, std::to_string(program.unresolved.size()));
    for (const auto& reason : program.unresolved) AppendToken(binding, reason);
    AppendToken(binding, std::to_string(program.postconditions.size()));
    for (const auto& outcome : program.postconditions) { AppendToken(binding, outcome.axis); AppendToken(binding, outcome.value); }
    return binding;
}

// Native requirements are generated from this same V4 authority. The graph
// represents conditional real call/message boundaries, not an assertion that
// the device, host, capture or business operation succeeded. In particular,
// no ordering between separately delivered moves is inferred from preview
// Program order; identity and unmodeled platform reentrancy remain unresolved.
inline Mapping MapV4NativePresenceCandidate(const V4FamilyIdentityContext& identity,
    const V4FamilyValidatedContext& verified)
{
    auto result = MapV4FamilyCandidate(identity, verified);
    if (result.status != "mapping_unimplemented" && result.status != "graph_preview_unexecuted") return result;
    result.graphAvailable = false; result.spec = {};
    const auto& candidate = verified.Coordinate(); const auto& axes = identity.Axes();
    if (Value(axes, candidate, "T01") != "internal_native" || Value(axes, candidate, "X03.task_count") != "one")
    { result.status = "mapping_unimplemented"; result.reasons.emplace_back("v4_native_single_session_scope_unimplemented"); return result; }
    const auto add = [&](std::string id, std::string role, std::string kind, std::string parameter) {
        Requirement item;
        item.id = std::move(id); item.role = std::move(role); item.kind = std::move(kind);
        item.session = "native_session_a"; item.task = "task_a";
        item.exclusiveSessionGroup = "native_drag_session_a";
        item.object = "native_source_and_host_identity_unknown"; item.parameter = std::move(parameter);
        item.presence = PresencePolicy::conditional_unresolved;
        item.presenceCondition = "actual_" + item.id;
        const auto name = item.id; result.spec.requirements.push_back(std::move(item)); return name;
    };
    const auto pair = [&](const std::string& first, const std::string& last) {
        result.spec.pairs.push_back({first, last, PairRelation::distinct});
        result.spec.strictBefore.push_back({first, last});
        result.spec.presenceConstraints.push_back({last, first, PresenceRelation::present_implies_present});
    };
    // Preparation sessions are distinct from the subsequent drag session.
    // The ordinal comes from the authoritative axis, not from a pointer-up
    // event or from the order in which steps happen to appear.
    const auto addPreparation = [&](std::string id, std::string role, std::string kind,
        std::string parameter, const std::string& session) {
        Requirement item;
        item.id = std::move(id); item.role = std::move(role); item.kind = std::move(kind);
        item.session = "native_selection_session_" + session; item.task = "task_a";
        item.exclusiveSessionGroup = item.session;
        item.object = "native_selection_device_and_host_identity_unknown";
        item.parameter = std::move(parameter); item.presence = PresencePolicy::conditional_unresolved;
        item.presenceCondition = "actual_" + item.id;
        const auto name = item.id; result.spec.requirements.push_back(std::move(item)); return name;
    };
    const auto exactSession = [](std::string_view value) {
        if (value == "zero" || value == "one" || value == "two") return true;
        if (!value.starts_with("count_")) return false;
        value.remove_prefix(6);
        if (value.empty() || value.front() == '0' ||
            !std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; })) return false;
        return value.size() > 1 || value.front() >= '3';
    };
    std::map<std::string, std::string> preparationBeginReturns;
    std::vector<std::string> preparationEndReturns;
    const auto begin = add("v4_native_device_begin_start", "device_begin", "fixture_call_start", "source_point");
    const auto beginReturn = add("v4_native_device_begin_return", "device_begin", "fixture_call_return", "source_point");
    pair(begin, beginReturn);
    const auto hover = add("v4_native_initial_hover_wndproc_enter", "initial_hover", "production_message_enter", "wm_mousemove");
    const auto hoverReturn = add("v4_native_initial_hover_wndproc_return", "initial_hover", "production_message_return", "wm_mousemove");
    pair(hover, hoverReturn);
    bool down = false, move = false, up = false, completion = false, end = false, business = false;
    for (std::size_t i = 0; i < result.program.steps.size(); ++i)
    {
        const auto& step = result.program.steps[i]; const auto base = "program_" + std::to_string(i);
        if (step.kind == "configure_v4_source_input")
        { result.reasons.push_back("v4_native_source_step_unimplemented:" + std::to_string(i)); continue; }
        const bool preparationDown = step.kind == "selection_prepare_pointer_down";
        const bool preparationMove = step.kind == "selection_prepare_pointer_move";
        const bool preparationUp = step.kind == "selection_prepare_pointer_up";
        const bool preparationEnd = step.kind == "selection_prepare_session_end";
        if (preparationDown || preparationMove || preparationUp || preparationEnd)
        {
            const auto separator = step.parameter.find(';');
            const auto axisBase = step.parameter.substr(0, separator);
            if (separator == std::string::npos || !axisBase.starts_with("V4.selection_prepare.step_"))
            { result.spec = {}; result.status = "mapping_unimplemented"; result.reasons.emplace_back("v4_native_preparation_identity_unavailable"); return result; }
            const auto sessionValue = Value(axes, candidate, axisBase + ".session");
            if (!exactSession(sessionValue))
            { result.spec = {}; result.status = "mapping_unimplemented"; result.reasons.emplace_back("v4_native_preparation_session_unavailable"); return result; }
            const std::string session(sessionValue);
            auto found = preparationBeginReturns.find(session);
            if (found == preparationBeginReturns.end())
            {
                const auto sessionBase = "v4_native_selection_session_" + session;
                const auto first = addPreparation(sessionBase + "_begin_start", "selection_device_begin",
                    "fixture_call_start", session, session);
                const auto last = addPreparation(sessionBase + "_begin_return", "selection_device_begin",
                    "fixture_call_return", session, session);
                pair(first, last);
                // Device::Begin queues a real initial move. Its point and
                // delivery are separate from the first modeled action and
                // remain conditional until the executor observes them.
                const auto hoverFirst = addPreparation(sessionBase + "_initial_hover_wndproc_enter",
                    "selection_initial_hover", "production_message_enter", "wm_mousemove_initial_point_unresolved", session);
                const auto hoverLast = addPreparation(sessionBase + "_initial_hover_wndproc_return",
                    "selection_initial_hover", "production_message_return", "wm_mousemove_initial_point_unresolved", session);
                pair(hoverFirst, hoverLast);
                result.spec.presenceConstraints.push_back({hoverFirst, first, PresenceRelation::present_implies_present});
                result.spec.strictBefore.push_back({first, hoverFirst});
                found = preparationBeginReturns.emplace(session, last).first;
            }
            const std::string role = preparationDown ? "selection_prepare_down" : preparationMove ?
                "selection_prepare_move" : preparationUp ? "selection_prepare_up" : "selection_device_end";
            if (preparationEnd)
            {
                const auto first = addPreparation(base + "_call_start", role, "fixture_call_start", step.parameter, session);
                const auto last = addPreparation(base + "_call_return", role, "fixture_call_return", step.parameter, session);
                pair(first, last);
                result.spec.strictBefore.push_back({found->second, first});
                preparationEndReturns.push_back(last);
            }
            else
            {
                const auto first = addPreparation(base + "_queue_start", role, "fixture_call_start", step.parameter, session);
                const auto last = addPreparation(base + "_queue_return", role, "fixture_call_return", step.parameter, session);
                const auto entered = addPreparation(base + "_wndproc_enter", role, "production_message_enter", step.parameter, session);
                const auto exited = addPreparation(base + "_wndproc_return", role, "production_message_return", step.parameter, session);
                pair(first, last); pair(entered, exited);
                result.spec.strictBefore.push_back({found->second, first});
            }
            continue;
        }
        const bool isDown = step.kind == "source_start";
        const bool isMove = step.kind == "move_past_drag_threshold" || step.kind == "pointer_direction" ||
            step.kind == "hover" || step.kind == "pointer_direction_later" || step.kind == "hover_later";
        const bool isUp = step.kind == "release";
        if (isDown || isMove || isUp)
        {
            if ((isDown && (down || up)) || (isMove && (!down || up)) || (isUp && (!down || up)))
            { result.spec = {}; result.status = "mapping_unimplemented"; result.reasons.emplace_back("v4_native_program_order_unimplemented"); return result; }
            down = down || isDown; move = move || isMove; up = up || isUp;
            const std::string role = isDown ? "native_down" : isUp ? "native_up" : "native_move";
            const std::string message = isDown ? "wm_lbuttondown" : isUp ? "wm_lbuttonup" : "wm_mousemove";
            const auto start = add(base + "_queue_start", role, "fixture_call_start", step.parameter);
            const auto returned = add(base + "_queue_return", role, "fixture_call_return", step.parameter);
            const auto entered = add(base + "_wndproc_enter", role, "production_message_enter", message);
            const auto exited = add(base + "_wndproc_return", role, "production_message_return", message);
            pair(start, returned); pair(entered, exited);
        }
        else if (step.kind == "observe_source_capture" || step.kind == "observe_completion_or_rejection" ||
            step.kind == "assert_business_results")
        {
            if (step.kind == "observe_completion_or_rejection") completion = true;
            if (step.kind == "assert_business_results") business = true;
            add(base + "_observe", step.kind, "conditional_observation", step.parameter);
        }
        else if (step.kind == "end_pointer_session")
        {
            end = true;
            const auto start = add(base + "_call_start", "device_end", "fixture_call_start", step.parameter);
            const auto returned = add(base + "_call_return", "device_end", "fixture_call_return", step.parameter);
            pair(start, returned);
        }
        else
        { result.spec = {}; result.status = "mapping_unimplemented"; result.reasons.push_back("v4_native_program_step_unimplemented:" + step.kind); return result; }
    }
    if (!down || !move || !up || !completion || !end || !business)
    { result.spec = {}; result.status = "mapping_unimplemented"; result.reasons.emplace_back("v4_native_program_incomplete"); return result; }
    for (const auto& endReturn : preparationEndReturns)
        result.spec.strictBefore.push_back({endReturn, begin});
    if (!preparationBeginReturns.empty())
    {
        // Cross-session distinctness is represented by each requirement's
        // exclusiveSessionGroup, rather than a quadratic list of PairRules.
        result.reasons.emplace_back("v4_native_selection_sessions_capture_and_drag_handoff_unimplemented");
        result.reasons.emplace_back("v4_native_selection_initial_hover_point_and_delivery_unimplemented");
    }
    result.reasons.emplace_back("v4_native_move_identity_queue_delivery_and_platform_order_unimplemented");
    result.reasons.emplace_back("v4_native_source_container_geometry_and_observation_unimplemented");
    result.reasons.emplace_back("v4_native_complete_event_presence_and_business_unimplemented");
    result.spec.unresolved = result.reasons; result.spec.causalCoverageComplete = false;
    result.status = "graph_preview_unexecuted"; result.graphAvailable = true;
    return result;
}

inline Mapping MapV4PresenceCandidate(const V4FamilyIdentityContext& identity,
    const V4FamilyValidatedContext& verified)
{
    auto result = MapV4FamilyCandidate(identity, verified);
    if (!result.graphAvailable) return result;
    const auto v = [&](std::string_view id) { return Value(identity.Axes(), verified.Coordinate(), id); };
    bool onlyText = v("T02.file_paths") == "no" && v("T02.shell_object") == "no" && v("T02.virtual_files") == "no" &&
        v("T02.image") == "no" && v("T02.text") == "yes" && v("T02.url") == "no" &&
        v("T02.invalid_format") == "no" && v("V4.format.text_unicode.offer") == "yes";
    bool onlyFile = v("T02.file_paths") == "yes" && (v("T02.shell_object") == "yes" || v("T02.shell_object") == "no") && v("T02.virtual_files") == "no" &&
        v("T02.image") == "no" && v("T02.text") == "no" && v("T02.url") == "no" &&
        v("T02.invalid_format") == "no" && v("V4.format.cf_hdrop.offer") == "yes";
    bool mixedFileText = v("T02.file_paths") == "yes" && (v("T02.shell_object") == "yes" || v("T02.shell_object") == "no") &&
        v("T02.virtual_files") == "no" && v("T02.image") == "no" && v("T02.text") == "yes" &&
        v("T02.url") == "no" && v("T02.invalid_format") == "no" &&
        v("V4.format.cf_hdrop.offer") == "yes" && v("V4.format.text_unicode.offer") == "yes";
    // Further explicitly represented mixed formats are still graph
    // hypotheses: availability and successful consumption are not inferred
    // from an offer. Unknown or unsupported offers stay mapping_unimplemented.
    bool mixedExtended = v("V4.source.kind") == "file_and_content" && v("T02.file_paths") == "yes" &&
        (v("T02.shell_object") == "yes" || v("T02.shell_object") == "no") && v("T02.url") == "no" &&
        (v("T02.invalid_format") == "yes" || v("T02.invalid_format") == "no") &&
        v("V4.format.cf_hdrop.offer") == "yes";
    bool contentExtended = v("V4.source.kind") == "content_only" && v("T02.file_paths") == "no" &&
        v("T02.shell_object") == "no" && v("T02.url") == "no" &&
        (v("T02.invalid_format") == "yes" || v("T02.invalid_format") == "no") &&
        v("V4.format.cf_hdrop.offer") == "no" && v("V4.format.shell_id_list.offer") == "no";
    const auto offered = [&](std::string_view name) { return v(std::string("V4.format.") + std::string(name) + ".offer") == "yes"; };
    const bool offeredText = offered("text_unicode");
    const bool offeredImage = offered("dib") || offered("dibv5") || offered("png") || offered("image_png");
    const bool offeredDescriptor = offered("file_descriptor_w"), offeredContents = offered("file_contents");
    if (v("T02.text") != (offeredText ? "yes" : "no") || v("T02.image") != (offeredImage ? "yes" : "no") ||
        v("T02.virtual_files") != (offeredDescriptor ? "yes" : "no") ||
        (!offeredText && !offeredImage && !offeredDescriptor && !offeredContents))
    { mixedExtended = false; contentExtended = false; }
    for (const auto& axis : identity.Axes())
    {
        if (!axis.id.starts_with("V4.format.") || !axis.id.ends_with(".offer")) continue;
        const auto offered = v(axis.id);
        if (axis.id != "V4.format.text_unicode.offer" && offered != "no") onlyText = false;
        if (axis.id != "V4.format.cf_hdrop.offer" && axis.id != "V4.format.shell_id_list.offer" && offered != "no") onlyFile = false;
        if (axis.id == "V4.format.shell_id_list.offer" && offered != "yes" && offered != "no") onlyFile = false;
        if (axis.id != "V4.format.cf_hdrop.offer" && axis.id != "V4.format.text_unicode.offer" &&
            axis.id != "V4.format.shell_id_list.offer" && offered != "no") mixedFileText = false;
        if (axis.id == "V4.format.shell_id_list.offer" && offered != "yes" && offered != "no") mixedFileText = false;
        const bool extendedKnown = axis.id == "V4.format.cf_hdrop.offer" || axis.id == "V4.format.shell_id_list.offer" ||
            axis.id == "V4.format.text_unicode.offer" || axis.id == "V4.format.dib.offer" || axis.id == "V4.format.dibv5.offer" ||
            axis.id == "V4.format.png.offer" || axis.id == "V4.format.image_png.offer" ||
            axis.id == "V4.format.file_descriptor_w.offer" || axis.id == "V4.format.file_contents.offer";
        if ((offered != "yes" && offered != "no") || (!extendedKnown && offered != "no")) mixedExtended = false;
        if ((offered != "yes" && offered != "no") || (!extendedKnown && offered != "no") ||
            ((axis.id == "V4.format.cf_hdrop.offer" || axis.id == "V4.format.shell_id_list.offer") && offered != "no"))
            contentExtended = false;
    }
    if (!onlyText && !onlyFile && !mixedFileText && !mixedExtended && !contentExtended)
    {
        result.graphAvailable = false; result.status = "mapping_unimplemented"; result.spec = {};
        result.reasons.emplace_back("v4_mixed_unknown_or_unsupported_format_event_mapping_unimplemented");
        return result;
    }
    // Whether production requests this format, its exact provider invocation
    // and its return are post-dispatch propositions, never fixture inputs.
    // Other formats/calls and any association with the host remain unresolved.
    const auto addFormat = [&](bool textFormat) {
        Requirement enter;
        enter.id = textFormat ? "v4_first_unicode_getdata_enter" : "v4_first_cfhdrop_getdata_enter";
        enter.role = textFormat ? "unicode_getdata" : "source_file_getdata";
        enter.kind = "source_getdata_enter"; enter.session = "session_a"; enter.task = "task_a";
        enter.object = "candidate_data_object"; enter.parameter = textFormat ? "cf_unicodetext" : "cf_hdrop";
        enter.presence = PresencePolicy::conditional_unresolved;
        enter.presenceCondition = textFormat ? "first_unicode_getdata_is_invoked" : "first_cfhdrop_getdata_is_invoked";
        Requirement returned = enter;
        returned.id = textFormat ? "v4_first_unicode_getdata_return" : "v4_first_cfhdrop_getdata_return";
        returned.kind = "source_getdata_return";
        returned.presenceCondition = textFormat ? "first_unicode_getdata_returns" : "first_cfhdrop_getdata_returns";
        result.spec.pairs.push_back({enter.id, returned.id, PairRelation::distinct});
        result.spec.strictBefore.push_back({enter.id, returned.id});
        result.spec.presenceConstraints.push_back({returned.id, enter.id, PresenceRelation::present_implies_present});
        result.spec.requirements.push_back(std::move(enter)); result.spec.requirements.push_back(std::move(returned));
    };
    const bool mixed = mixedFileText || mixedExtended;
    if (onlyText || ((mixed || contentExtended) && offeredText)) addFormat(true);
    if (onlyFile || mixed) addFormat(false);
    if (mixed || contentExtended)
    {
        // One GetData invocation has one requested FORMATETC format. These
        // boundary events therefore cannot share identity; their order and
        // whether either occurs are not inferred from the offered formats.
        std::vector<std::pair<std::string, std::string>> requests;
        if (mixed) requests.emplace_back("v4_first_cfhdrop_getdata_enter", "v4_first_cfhdrop_getdata_return");
        if (offeredText) requests.emplace_back("v4_first_unicode_getdata_enter", "v4_first_unicode_getdata_return");
        const auto addAdditional = [&](const std::string& key, const std::string& parameter) {
            Requirement enter;
            enter.id = "v4_first_" + key + "_getdata_enter";
            enter.role = "mixed_content_getdata"; enter.kind = "source_getdata_enter";
            enter.session = "session_a"; enter.task = "task_a"; enter.object = "candidate_data_object";
            enter.parameter = parameter; enter.presence = PresencePolicy::conditional_unresolved;
            enter.presenceCondition = "first_" + key + "_getdata_is_invoked";
            Requirement returned = enter;
            returned.id = "v4_first_" + key + "_getdata_return"; returned.kind = "source_getdata_return";
            returned.presenceCondition = "first_" + key + "_getdata_returns";
            result.spec.pairs.push_back({enter.id, returned.id, PairRelation::distinct});
            result.spec.strictBefore.push_back({enter.id, returned.id});
            result.spec.presenceConstraints.push_back({returned.id, enter.id, PresenceRelation::present_implies_present});
            requests.emplace_back(enter.id, returned.id);
            result.spec.requirements.push_back(std::move(enter)); result.spec.requirements.push_back(std::move(returned));
        };
        for (const auto name : {"dib", "dibv5", "png", "image_png", "file_descriptor_w"})
            if (offered(name)) addAdditional(name, name);
        if (offeredContents)
        {
            constexpr std::string_view prefix = "V4.payload.file_contents.istream.index_", suffix = ".bytes";
            for (const auto& axis : identity.Axes())
                if (axis.id.starts_with(prefix) && axis.id.ends_with(suffix))
                {
                    const auto index = axis.id.substr(prefix.size(), axis.id.size() - prefix.size() - suffix.size());
                    addAdditional("file_contents_index_" + index, "file_contents_index_" + index);
                }
            result.reasons.emplace_back("v4_file_contents_unlisted_indices_and_additional_requests_unimplemented");
        }
        for (std::size_t i = 0; i < requests.size(); ++i)
            for (std::size_t j = 0; j < i; ++j)
                for (const auto* left : {&requests[i].first, &requests[i].second})
                    for (const auto* right : {&requests[j].first, &requests[j].second})
                        result.spec.pairs.push_back({*left, *right, PairRelation::distinct});
        result.reasons.emplace_back(mixed ? "v4_mixed_format_host_selection_and_independent_request_intervals_unimplemented" :
            "v4_content_format_host_selection_and_independent_request_intervals_unimplemented");
    }
    if (onlyFile || mixed)
    {
        // Markers are internal production points for one selected occurrence,
        // not caller ABI returns or proof that a task/window belongs to it.
        for (const auto& [id, kind, object] : std::initializer_list<std::tuple<const char*, const char*, const char*>>{
            {"v4_read_queue_enter", "read_queue_enter", "read_queue_unknown"},
            {"v4_read_queued", "read_queued", "read_queue_unknown"},
            {"v4_read_queue_failed", "read_queue_failed", "read_queue_unknown"},
            {"v4_read_worker_enter", "read_worker_enter", "read_work_unknown"},
            {"v4_read_content_paths", "read_content_paths", "read_content_unknown"},
            {"v4_read_worker_return", "read_worker_return", "read_work_unknown"},
            {"v4_read_completion_callback", "read_completion_callback", "read_completion_unknown"},
            {"v4_read_completion_posted", "completion_posted", "read_completion_unknown"},
            {"v4_read_completion_post_failed", "completion_post_failed", "read_completion_unknown"},
            {"v4_read_completion_enter", "completion_enter", "read_completion_unknown"},
            {"v4_read_completion_return", "completion_return", "read_completion_unknown"},
            {"v4_commit_content_paths", "commit_content_paths", "commit_content_unknown"},
            {"v4_file_queue_enter", "file_queue_enter", "file_queue_unknown"},
            {"v4_file_queued", "file_queued", "file_queue_unknown"},
            {"v4_file_queue_failed", "file_queue_failed", "file_queue_unknown"},
            {"v4_file_worker_execute_enter", "worker_execute_enter", "file_work_unknown"},
            {"v4_file_worker_execute_return", "worker_execute_return", "file_work_unknown"},
            {"v4_file_completion_callback", "file_completion_callback", "file_completion_unknown"},
            {"v4_file_completion_posted", "completion_posted", "file_completion_unknown"},
            {"v4_file_completion_post_failed", "completion_post_failed", "file_completion_unknown"},
            {"v4_file_completion_enter", "completion_enter", "file_completion_unknown"},
            {"v4_file_completion_return", "completion_return", "file_completion_unknown"}})
        {
            Requirement marker;
            marker.id = id; marker.role = kind; marker.kind = kind;
            marker.session = "session_a"; marker.task = "task_a"; marker.object = object;
            marker.parameter = "first_selected_occurrence"; marker.presence = PresencePolicy::conditional_unresolved;
            marker.presenceCondition = std::string("selected_") + id;
            result.spec.requirements.push_back(std::move(marker));
        }
        const auto pair = [&](const char* first, const char* last) {
            result.spec.pairs.push_back({first, last, PairRelation::distinct});
            result.spec.strictBefore.push_back({first, last});
            result.spec.presenceConstraints.push_back({last, first, PresenceRelation::present_implies_present});
        };
        for (const auto& [first, last] : std::initializer_list<std::pair<const char*, const char*>>{
            {"v4_read_queue_enter", "v4_read_queued"}, {"v4_read_queue_enter", "v4_read_queue_failed"},
            {"v4_read_worker_enter", "v4_read_content_paths"}, {"v4_read_worker_enter", "v4_read_worker_return"},
            {"v4_read_completion_callback", "v4_read_completion_posted"}, {"v4_read_completion_callback", "v4_read_completion_post_failed"},
            {"v4_read_completion_enter", "v4_read_completion_return"},
            {"v4_file_queue_enter", "v4_file_queued"}, {"v4_file_queue_enter", "v4_file_queue_failed"},
            {"v4_file_worker_execute_enter", "v4_file_worker_execute_return"},
            {"v4_file_completion_callback", "v4_file_completion_posted"}, {"v4_file_completion_callback", "v4_file_completion_post_failed"},
            {"v4_file_completion_enter", "v4_file_completion_return"}}) pair(first, last);
        result.spec.strictBefore.push_back({"v4_read_content_paths", "v4_read_worker_return"});
        for (const auto& [first, last] : std::initializer_list<std::pair<const char*, const char*>>{
            {"v4_read_queued", "v4_read_queue_failed"}, {"v4_file_queued", "v4_file_queue_failed"},
            {"v4_read_completion_posted", "v4_read_completion_post_failed"},
            {"v4_file_completion_posted", "v4_file_completion_post_failed"}})
            result.spec.presenceConstraints.push_back({first, last, PresenceRelation::present_excludes_present});
        result.reasons.emplace_back("v4_file_work_occurrence_correlation_and_extra_events_unimplemented");
    }
    // Conditional request and delivery boundaries do not imply that the
    // refresh was caused by this Drop. A request may coalesce; a ready
    // message may serve unrelated work and may never arrive. Only the paired
    // boundaries of the same selected invocation have a proved order.
    const auto boundary = [&](const char* id, const char* role, const char* kind,
        const char* condition) {
        Requirement value;
        value.id = id; value.role = role; value.kind = kind;
        value.session = "session_a"; value.task = "task_a";
        value.object = "host_refresh_identity_unknown"; value.parameter = "first_selected_occurrence";
        value.presence = PresencePolicy::conditional_unresolved; value.presenceCondition = condition;
        result.spec.requirements.push_back(std::move(value));
    };
    boundary("v4_refresh_request_enter", "refresh_request", "refresh_request_enter", "selected_refresh_request_enters");
    boundary("v4_refresh_request_return", "refresh_request", "refresh_request_return", "selected_refresh_request_returns");
    boundary("v4_refresh_window_enter", "refresh_window", "window_message_enter", "selected_refresh_window_message_enters");
    boundary("v4_refresh_window_return", "refresh_window", "window_message_return", "selected_refresh_window_message_returns");
    const auto sameInvocation = [&](const char* first, const char* last) {
        result.spec.pairs.push_back({first, last, PairRelation::distinct});
        result.spec.strictBefore.push_back({first, last});
        result.spec.presenceConstraints.push_back({last, first, PresenceRelation::present_implies_present});
    };
    sameInvocation("v4_refresh_request_enter", "v4_refresh_request_return");
    sameInvocation("v4_refresh_window_enter", "v4_refresh_window_return");
    if (onlyText || ((mixed || contentExtended) && offeredText)) result.reasons.emplace_back("v4_unicode_getdata_presence_identity_interval_unobserved");
    if (onlyFile || mixed) result.reasons.emplace_back("v4_cfhdrop_getdata_presence_identity_interval_unobserved");
    result.reasons.emplace_back("v4_refresh_request_work_message_and_drop_attribution_unimplemented");
    result.reasons.emplace_back("v4_other_format_reads_and_background_events_unmodeled");
    result.spec.unresolved = result.reasons; result.spec.causalCoverageComplete = false;
    return result;
}

// Extend the same candidate's restricted call graph with the conditional
// first CF_HDROP GetData invocation and selected transfer/work boundaries.
// These propositions describe production occurrences, not fixture controls.
// First occurrences and work identities are not yet bound to actual logs;
// further reads, works and other production events remain unmodeled.
inline Mapping MapV3PresenceCandidate(const V3FamilyIdentityContext& identity,
    const V3FamilyValidatedContext& validated, const EventProgram& program)
{
    auto result = MapV3FamilyCandidate(identity, validated, program);
    if (!result.graphAvailable) return result;
    Requirement enter;
    enter.id = "v3_first_cfhdrop_getdata_enter";
    enter.role = "source_file_getdata"; enter.kind = "source_getdata_enter";
    enter.session = "session_a"; enter.task = "task_a"; enter.object = "candidate_data_object";
    enter.parameter = "cf_hdrop"; enter.occurrence = 0; enter.propertiesResolved = false;
    enter.presence = PresencePolicy::conditional_unresolved;
    enter.presenceCondition = "first_cfhdrop_getdata_is_invoked";
    Requirement returned = enter;
    returned.id = "v3_first_cfhdrop_getdata_return";
    returned.kind = "source_getdata_return";
    returned.presenceCondition = "first_cfhdrop_getdata_returns";
    for (const auto& prior : result.spec.requirements)
    {
        result.spec.pairs.push_back({prior.id, enter.id, PairRelation::distinct});
        result.spec.pairs.push_back({prior.id, returned.id, PairRelation::distinct});
    }
    result.spec.pairs.push_back({enter.id, returned.id, PairRelation::distinct});
    result.spec.strictBefore.push_back({enter.id, returned.id});
    result.spec.presenceConstraints.push_back({returned.id, enter.id, PresenceRelation::present_implies_present});
    result.spec.requirements.push_back(std::move(enter));
    result.spec.requirements.push_back(std::move(returned));
    result.reasons.emplace_back("v3_first_cfhdrop_read_presence_is_unobserved_and_additional_reads_unmodeled");

    // The names correspond to the passive phase markers in
    // external_slot_transfer, ShellFileOperationWorker and
    // OnShellFileOperationCompleted. They refer to the first selected work or
    // completion, whose actual task/work/object identities remain unresolved.
    // Every read marker denotes the same selected read work and every file
    // marker the same selected file work; the read and file works' mutual
    // identity and causal relationship are not assumed. A selected call may
    // be absent, and additional calls are not represented by these markers.
    // A marker records a point inside a function, not an ABI return or a
    // guarantee that a window message was delivered. Diagnostic log absence
    // alone is not evidence of the corresponding production event's absence.
    const auto marker = [&](const char* id, const char* role, const char* kind,
        const char* object, const char* condition) {
        Requirement value;
        value.id = id; value.role = role; value.kind = kind;
        value.session = "session_a"; value.task = "task_a";
        value.object = object; value.parameter = "first_selected_occurrence";
        value.occurrence = 0; value.propertiesResolved = false;
        value.presence = PresencePolicy::conditional_unresolved;
        value.presenceCondition = condition;
        result.spec.requirements.push_back(std::move(value));
    };
    marker("v3_read_worker_enter", "read_worker_boundary", "read_worker_enter",
        "first_read_work_unknown", "first_selected_read_worker_enters");
    marker("v3_read_queue_enter", "read_queue_boundary", "read_queue_enter",
        "first_read_completion_unknown", "first_selected_read_work_queue_enters");
    marker("v3_read_queued", "read_queue_boundary", "read_queued",
        "first_read_completion_unknown", "first_selected_read_queue_reports_accepted");
    marker("v3_read_queue_failed", "read_queue_boundary", "read_queue_failed",
        "first_read_completion_unknown", "first_selected_read_queue_reports_failed");
    marker("v3_read_worker_return", "read_worker_boundary", "read_worker_return",
        "first_read_work_unknown", "first_selected_read_worker_return_marker");
    marker("v3_read_content_paths", "read_content_boundary", "read_content_paths",
        "first_read_content_unknown", "first_selected_read_produces_content_paths");
    marker("v3_read_completion_callback", "read_completion_boundary", "read_completion_callback",
        "first_read_completion_unknown", "first_selected_read_callback_invoked");
    marker("v3_read_completion_posted", "read_post_boundary", "completion_posted",
        "first_read_completion_unknown", "first_selected_read_post_reports_success");
    marker("v3_read_completion_post_failed", "read_post_boundary", "completion_post_failed",
        "first_read_completion_unknown", "first_selected_read_post_reports_failure");
    marker("v3_read_completion_enter", "read_window_completion_boundary", "completion_enter",
        "first_read_completion_unknown", "first_selected_read_window_completion_enters");
    marker("v3_read_completion_return", "read_window_completion_boundary", "completion_return",
        "first_read_completion_unknown", "first_selected_read_window_completion_return_marker");
    marker("v3_commit_content_paths", "commit_content_boundary", "commit_content_paths",
        "first_commit_content_unknown", "first_selected_completion_reaches_commit");
    marker("v3_file_queue_enter", "file_queue_boundary", "file_queue_enter",
        "first_file_completion_unknown", "first_selected_file_work_queue_enters");
    marker("v3_file_queued", "file_queue_boundary", "file_queued",
        "first_file_completion_unknown", "first_selected_file_queue_reports_accepted");
    marker("v3_file_queue_failed", "file_queue_boundary", "file_queue_failed",
        "first_file_completion_unknown", "first_selected_file_queue_reports_failed");
    marker("v3_file_worker_execute_enter", "file_worker_boundary", "worker_execute_enter",
        "first_file_work_unknown", "first_selected_file_work_executes");
    marker("v3_file_worker_execute_return", "file_worker_boundary", "worker_execute_return",
        "first_file_work_unknown", "first_selected_file_execute_returns");
    marker("v3_file_completion_callback", "file_completion_boundary", "file_completion_callback",
        "first_file_completion_unknown", "first_selected_file_callback_invoked");
    marker("v3_file_completion_posted", "file_post_boundary", "completion_posted",
        "first_file_completion_unknown", "first_selected_file_post_reports_success");
    marker("v3_file_completion_post_failed", "file_post_boundary", "completion_post_failed",
        "first_file_completion_unknown", "first_selected_file_post_reports_failure");
    marker("v3_file_completion_enter", "file_window_completion_boundary", "completion_enter",
        "first_file_completion_unknown", "first_selected_file_window_completion_enters");
    marker("v3_file_completion_return", "file_window_completion_boundary", "completion_return",
        "first_file_completion_unknown", "first_selected_file_window_completion_return_marker");

    // These constraints apply to the two boundaries of the SAME selected
    // invocation only. Cancellation, failed posts, reentrancy, cross-thread
    // scheduling and additional occurrences stay representable. In
    // particular, read_queued/file_queued and completion_posted are recorded
    // after the enqueue/post calls, so their ordering against worker/window
    // entry cannot be inferred from those marker positions.
    const auto paired = [&](const char* first, const char* last) {
        result.spec.pairs.push_back({first, last, PairRelation::distinct});
        result.spec.strictBefore.push_back({first, last});
        result.spec.presenceConstraints.push_back({last, first, PresenceRelation::present_implies_present});
    };
    paired("v3_read_worker_enter", "v3_read_worker_return");
    paired("v3_read_worker_enter", "v3_read_content_paths");
    paired("v3_read_queue_enter", "v3_read_queued");
    paired("v3_read_queue_enter", "v3_read_queue_failed");
    paired("v3_read_completion_callback", "v3_read_completion_posted");
    paired("v3_read_completion_callback", "v3_read_completion_post_failed");
    paired("v3_read_completion_enter", "v3_read_completion_return");
    paired("v3_file_queue_enter", "v3_file_queued");
    paired("v3_file_queue_enter", "v3_file_queue_failed");
    paired("v3_file_worker_execute_enter", "v3_file_worker_execute_return");
    paired("v3_file_completion_callback", "v3_file_completion_posted");
    paired("v3_file_completion_callback", "v3_file_completion_post_failed");
    paired("v3_file_completion_enter", "v3_file_completion_return");
    result.spec.strictBefore.push_back({"v3_read_content_paths", "v3_read_worker_return"});
    // Each pair is the two outcomes of one selected call; neither outcome is
    // required. In particular, failed queue/post paths remain represented.
    const auto exclusive = [&](const char* first, const char* second) {
        result.spec.presenceConstraints.push_back({first, second, PresenceRelation::present_excludes_present});
    };
    exclusive("v3_read_queued", "v3_read_queue_failed");
    exclusive("v3_file_queued", "v3_file_queue_failed");
    exclusive("v3_read_completion_posted", "v3_read_completion_post_failed");
    exclusive("v3_file_completion_posted", "v3_file_completion_post_failed");
    result.reasons.emplace_back("v3_transfer_work_and_window_identity_correlation_unimplemented");
    result.reasons.emplace_back("v3_transfer_occurrence_presence_and_observation_interval_unimplemented");
    result.reasons.emplace_back("v3_transfer_additional_work_and_completion_events_unmodeled");
    result.spec.unresolved = result.reasons;
    result.spec.causalCoverageComplete = false;
    return result;
}

// Describe a restricted single-window native pointer session for inspection.
// This maps the SAME candidate/program as the global model. It does not
// execute Device, force a host decision, or decide any business result.
inline Mapping MapSimpleNative(const Context& context, const Candidate& input, const EventProgram& suppliedProgram)
{
    Mapping result;
    result.program = suppliedProgram;
    result.reasons = RevalidateContext(context);
    if (!result.reasons.empty()) return result;
    if (input.values.size() != context.axes.size()) result.reasons.emplace_back("candidate_axis_count_mismatch");
    if (!result.reasons.empty()) return result;
    Candidate candidate = input;
    candidate.lookup = context.lookup;
    for (std::size_t i = 0; i < context.axes.size(); ++i)
    {
        const auto& axis = context.axes[i]; const auto& value = candidate.values[i];
        if (axis.role == Role::Derived)
        {
            if (!value.empty()) result.reasons.push_back("candidate_derived_value_invalid:" + axis.id);
        }
        else if (std::find(axis.values.begin(), axis.values.end(), value) == axis.values.end())
            result.reasons.push_back("candidate_value_invalid:" + axis.id);
        else if (!ExistingPrefixAllows(context, candidate, axis.id, value))
            result.reasons.push_back("candidate_prefix_invalid:" + axis.id);
    }
    if (!result.reasons.empty()) return result;
    result.structuralExclusion = ExistingStructuralExclusion(context, candidate);
    if (!result.structuralExclusion.empty()) { result.status = "structural_exclusion"; return result; }
    const auto expected = drag_global::ProgramVersion(context.axes, candidate, context.version);
    if (!SameEventProgram(suppliedProgram, expected))
    { result.reasons.emplace_back("program_candidate_mismatch"); return result; }
    for (const auto& reason : suppliedProgram.unresolved) result.reasons.push_back("program_unresolved:" + reason);
    result.reasons.emplace_back("whole_candidate_axis_mapping_unimplemented");
    for (const auto& axis : context.axes)
    {
        if (axis.role == Role::Hypothesis && axis.id.substr(0, 7) == "task_b." &&
            Value(context.axes, candidate, axis.id) == "na" &&
            Value(context.axes, candidate, "X03.task_count") == "one")
            continue;
        result.reasons.push_back((axis.role == Role::Derived ? "derived_observation_unimplemented:" :
            axis.role == Role::Hypothesis ? "outcome_hypothesis_observation_unimplemented:" :
            "axis_construction_and_observation_unimplemented:") + axis.id);
    }
    if (context.version == SelectionSchemaVersion::source_members_v2)
        result.reasons.emplace_back("v2_native_source_count_and_slot_semantics_unimplemented");

    std::vector<std::string> scope;
    const auto v = [&](std::string_view id) { return Value(context.axes, candidate, id); };
    const auto is = [](std::string_view value, std::initializer_list<std::string_view> choices) {
        return std::find(choices.begin(), choices.end(), value) != choices.end();
    };
    const auto need = [&](bool condition, std::string_view id) {
        if (!condition) scope.push_back("simple_native_input_unimplemented:" + std::string(id));
    };
    need(v("T01") == "internal_native", "T01");
    need(v("source.E01") == "free_desktop" && v("source.E02") == "none" && v("source.E03") == "desktop_icon", "source_endpoint");
    need(v("target.E01") == "free_desktop" && v("target.E02") == "none" && v("H01.region") == "empty_slot", "target_endpoint");
    bool oneSource = false;
    switch (context.version)
    {
    case SelectionSchemaVersion::legacy_v1:
        oneSource = v("P02.resolved_count") == "one" && v("P02.many_count") == "na"; break;
    case SelectionSchemaVersion::source_members_v2:
        oneSource = SelectionSourceSize(context.axes, candidate) == 1; break;
    default: result.reasons.emplace_back("native_graph_schema_version_unsupported"); return result;
    }
    need(v("P01.primary") == "file" && v("P01.second") == "na" && oneSource, "selection");
    need(v("X03.task_count") == "one" && v("X03.submit_order") == "na" && v("X03.order") == "single", "X03.task_count_order");
    need(v("X03.callback_reentrant") == "no" && v("X03.callback_duplicate") == "no" && v("X03.refresh") == "normal", "X03.callback_refresh");
    need(v("X02") == "none" && is(v("X02.position"), {"na", "no_interruption"}), "X02");
    need(v("G01.button") == "left" && v("G02.press") == "none" && v("G02.release") == "none" &&
        v("G02.release_buttons") == "none" && v("G04.termination") == "drop" && v("G03.decision") == "na", "gesture");
    need(v("G02.enter") == "na" && v("G02.enter_buttons") == "na", "G02.enter");
    for (const auto phase : {"hover_first", "hover_later"})
    {
        const auto field = std::string("G02.") + phase;
        const auto buttons = field + "_buttons";
        need((v(field) == "na" && v(buttons) == "na") || (v(field) == "none" && v(buttons) == "left"), field);
    }
    need(is(v("H03.page_switch"), {"na", "none"}) && is(v("H03.after_drop"), {"na", "unchanged"}), "H03");
    need(is(v("H05.scroll"), {"na", "unchanged"}) && is(v("P05.source_reorder"), {"na", "unchanged"}), "H05_P05");
    for (const auto id : {"S01", "S02", "S03.dwell", "S03.navigation", "S04", "S05", "S06", "S07", "S08", "S09", "S10"})
        need(is(v(id), {"na", "none"}), id);
    for (const auto id : kLifecycleChangeFields)
    {
        need(is(v(id), {"na", "stable"}), id);
        const auto phase = std::string(id) + "_phase";
        need(v(phase) == "na", phase);
    }
    for (const auto id : kEnvironmentChangeFields)
    {
        need(!v(id).empty() && !EnvironmentChangeActive(id, v(id)), id);
        const auto phase = std::string(id) + "_phase";
        need(v(phase) == "na", phase);
    }
    if (!scope.empty()) { result.reasons.insert(result.reasons.end(), scope.begin(), scope.end()); return result; }

    std::string tail;
    const auto add = [&](std::string id, std::string_view role, std::string_view kind,
                         std::string_view parameter, std::size_t occurrence, bool chain) {
        Requirement item;
        item.id = std::move(id); item.role = std::string(role); item.kind = std::string(kind);
        item.session = "native_session_a"; item.task = "task_a";
        item.object = "candidate_host_and_source"; item.parameter = std::string(parameter);
        item.occurrence = occurrence; item.propertiesResolved = false;
        result.spec.requirements.push_back(item);
        if (chain && !tail.empty()) result.spec.strictBefore.push_back({tail, item.id});
        if (chain) tail = item.id;
        return item.id;
    };
    add("device_begin_start", "device_begin", "fixture_call_start", "source_point", 0, true);
    const auto initialQueued = add("device_initial_hover_enqueued", "initial_hover", "fixture_queue_event", "initial_move", 0, true);
    add("device_begin_return", "device_begin", "fixture_call_return", "source_point", 0, true);
    // The queued initial move and Begin's return are independent of when the
    // resulting window message enters/returns. In particular, Begin's caller
    // may enqueue source_start before the initial move is delivered; nested
    // dispatch may also interleave message intervals. Enumerate those orders
    // instead of embedding the current driver's wait policy in the model.
    // Delivery is conditional on a live owned destination and successful
    // dispatch; this graph still needs explicit presence/failure alternatives.
    const auto hoverEnter = add("initial_hover_wndproc_enter", "initial_hover",
        "conditional_production_message_enter", "wm_mousemove_no_buttons_at_source_point", 0, false);
    const auto hoverReturn = add("initial_hover_wndproc_return", "initial_hover",
        "conditional_production_message_return", "wm_mousemove_no_buttons_at_source_point", 0, false);
    for (auto& requirement : result.spec.requirements)
        if (requirement.id == hoverEnter || requirement.id == hoverReturn)
        {
            requirement.presence = PresencePolicy::conditional_unresolved;
            requirement.presenceCondition = requirement.id == hoverEnter
                ? "initial_hover_reaches_owned_wndproc" : "initial_hover_owned_wndproc_returns";
        }
    // A returned WndProc invocation necessarily entered; the converse is
    // deliberately not assumed (nested dispatch, failure or interruption).
    result.spec.presenceConstraints.push_back({hoverReturn, hoverEnter, PresenceRelation::present_implies_present});
    result.spec.strictBefore.push_back({initialQueued, hoverEnter});
    result.spec.strictBefore.push_back({hoverEnter, hoverReturn});

    bool pressed = false, moved = false, captured = false, released = false, completed = false, ended = false, asserted = false;
    std::string downReturn, thresholdMove, captureObservation;
    std::vector<std::pair<std::string, std::string>> moveRequirements;
    std::vector<std::string> endProbes;
    for (std::size_t i = 0; i < suppliedProgram.steps.size(); ++i)
    {
        const auto& step = suppliedProgram.steps[i];
        const auto base = "program_" + std::to_string(i);
        const bool down = step.kind == "source_start";
        const bool move = step.kind == "move_past_drag_threshold" || step.kind == "pointer_direction" || step.kind == "hover" ||
            step.kind == "pointer_direction_later" || step.kind == "hover_later";
        const bool up = step.kind == "release";
        if (down)
        {
            if (pressed || released || ended || asserted)
            { result.reasons.push_back("native_program_sequence_unimplemented:" + std::to_string(i)); result.spec = {}; return result; }
            pressed = true;
            add(base + "_queue_start", "native_down", "fixture_call_start", step.parameter, 0, true);
            add(base + "_queue_return", "native_down", "fixture_call_return", step.parameter, 0, true);
            add(base + "_wndproc_enter", "native_down", "production_message_enter", "wm_lbuttondown", 0, true);
            downReturn = add(base + "_wndproc_return", "native_down", "production_message_return", "wm_lbuttondown", 0, true);
        }
        else if (move)
        {
            if (!pressed || released || ended || asserted)
            { result.reasons.push_back("native_program_sequence_unimplemented:" + std::to_string(i)); result.spec = {}; return result; }
            moved = true;
            // A semantic movement requirement can share a single actual
            // delivered move with another requirement, or denote a distinct
            // delivery. Do not choose or order them merely from Program order.
            const auto movement = add(base + "_move_requirement", step.kind, "native_move_delivery_requirement", step.parameter, 0, false);
            result.spec.strictBefore.push_back({downReturn, movement});
            moveRequirements.emplace_back(step.kind, movement);
            if (step.kind == "move_past_drag_threshold") thresholdMove = movement;
        }
        else if (up)
        {
            if (!pressed || released || !captured || ended || asserted)
            { result.reasons.push_back("native_program_sequence_unimplemented:" + std::to_string(i)); result.spec = {}; return result; }
            released = true;
            const auto upStart = add(base + "_queue_start", "native_up", "fixture_call_start", step.parameter, 0, false);
            result.spec.strictBefore.push_back({captureObservation, upStart});
            for (const auto& movement : moveRequirements) result.spec.strictBefore.push_back({movement.second, upStart});
            tail = upStart;
            add(base + "_queue_return", "native_up", "fixture_call_return", step.parameter, 0, true);
            add(base + "_wndproc_enter", "native_up", "production_message_enter", "wm_lbuttonup", 0, true);
            add(base + "_wndproc_return", "native_up", "production_message_return", "wm_lbuttonup", 0, true);
        }
        else if (step.kind == "observe_source_capture" && pressed && moved && !released && !captured && step.parameter == "production")
        {
            if (thresholdMove.empty()) { result.reasons.emplace_back("native_threshold_identity_unimplemented"); result.spec = {}; return result; }
            captured = true;
            captureObservation = add(base + "_observe", step.kind, "production_observation", step.parameter, 0, false);
            result.spec.strictBefore.push_back({thresholdMove, captureObservation});
        }
        else if (step.kind == "observe_completion_or_rejection" && released && !completed && !ended && step.parameter == "single")
        {
            completed = true; add(base + "_observe", step.kind, "conditional_production_observation", step.parameter, 0, true);
        }
        else if (step.kind == "end_pointer_session" && released && completed && !ended && step.parameter == "production_cleanup_messages")
        {
            ended = true;
            add(base + "_call_start", "device_end", "fixture_call_start", step.parameter, 0, true);
            const auto endReturn = add(base + "_call_return", "device_end", "fixture_call_return", step.parameter, 0, true);
            // End delivers these message kinds conditionally to the actual
            // captured/hovered/focused targets. These nodes require inspecting
            // the trace; they do NOT assert each message was delivered.
            for (const auto message : {"wm_cancelmode", "wm_capturechanged", "wm_mouseleave", "conditional_focus_and_key_cleanup"})
            {
                const auto probe = add(base + "_inspect_" + message, "device_end_trace", "conditional_message_observation", message, 0, false);
                result.spec.strictBefore.push_back({endReturn, probe});
                endProbes.push_back(probe);
            }
        }
        else if (step.kind == "assert_business_results" && ended && !asserted && step.parameter == "independent_oracle")
        {
            asserted = true;
            const auto assertion = add(base + "_observe", step.kind, "business_assertion", step.parameter, 0, true);
            for (const auto& probe : endProbes) result.spec.strictBefore.push_back({probe, assertion});
        }
        else { result.reasons.push_back("native_program_step_unimplemented:" + step.kind); result.spec = {}; return result; }
    }
    if (!pressed || !moved || !captured || !released || !completed || !ended || !asserted)
    { result.reasons.emplace_back("native_program_incomplete"); result.spec = {}; return result; }
    for (const auto& [laterKind, later] : moveRequirements)
    {
        const auto earlierKind = laterKind == "pointer_direction_later" ? "pointer_direction" : laterKind == "hover_later" ? "hover" : "";
        if (!*earlierKind) continue;
        const auto earlier = std::find_if(moveRequirements.begin(), moveRequirements.end(), [&](const auto& item) {
            return item.first == earlierKind;
        });
        if (earlier == moveRequirements.end())
        { result.reasons.push_back("native_later_movement_identity_unresolved:" + laterKind); result.spec = {}; return result; }
        result.spec.strictBefore.push_back({earlier->second, later});
    }
    for (std::size_t i = 0; i < result.spec.requirements.size(); ++i)
        for (std::size_t j = i + 1; j < result.spec.requirements.size(); ++j)
        {
            const auto& first = result.spec.requirements[i].id;
            const auto& second = result.spec.requirements[j].id;
            const bool firstProbe = std::find(endProbes.begin(), endProbes.end(), first) != endProbes.end();
            const bool secondProbe = std::find(endProbes.begin(), endProbes.end(), second) != endProbes.end();
            const auto isMove = [&](const std::string& id) {
                return std::any_of(moveRequirements.begin(), moveRequirements.end(), [&](const auto& item) { return item.second == id; });
            };
            // One real trace inspection can cover multiple conditional message
            // kinds; neither shared nor separate observation is assumed.
            result.spec.pairs.push_back({first, second,
                (firstProbe && secondProbe) || (isMove(first) && isMove(second))
                    ? PairRelation::unresolved : PairRelation::distinct});
        }
    result.spec.causalCoverageComplete = false;
    result.graphAvailable = true;
    result.reasons.emplace_back("native_initial_hover_candidate_coordinates_and_wndproc_binding_unimplemented");
    result.reasons.emplace_back("native_initial_hover_conditional_presence_and_failure_alternatives_unimplemented");
    result.reasons.emplace_back("native_initial_hover_queue_and_nested_delivery_causality_unresolved");
    result.reasons.emplace_back("native_device_failure_cleanup_and_conditional_message_identity_unimplemented");
    result.reasons.emplace_back("native_end_message_call_interval_attribution_unimplemented");
    result.reasons.emplace_back("native_capture_completion_and_message_trace_binding_unimplemented");
    result.reasons.emplace_back("native_event_graph_executor_and_business_oracle_unimplemented");
    result.reasons.emplace_back("native_move_event_alias_and_other_orderings_not_fully_mapped");
    result.reasons.emplace_back("native_move_queue_and_wndproc_interval_identity_unimplemented");
    result.reasons.emplace_back("native_move_capture_focus_leave_notifications_conditional_unmodeled");
    result.reasons.emplace_back("native_refresh_save_reload_event_identity_unimplemented");
    result.spec.unresolved = result.reasons;
    return result;
}

struct PreparedGraph
{
    std::vector<Requirement> requirements;
    std::map<std::pair<std::size_t, std::size_t>, PairRelation> rules;
    std::set<std::pair<std::size_t, std::size_t>> edges;
    std::string binding;
    std::vector<std::string> unresolved;
    bool causalCoverageComplete = false;
};

// Legacy ordinal implementation: every nonzero page rescans from zero. It is
// retained for compatibility only; use the bounded stateful DFS API below.
// Returns a bounded page in canonical RGS-partition, then lexicographic
// topological order. `startOrdinal` counts valid generated graph variants
// for this Candidate and Spec; scanning may revisit earlier variants. The
// returned nextOrdinal is a resumable cursor, not a cross-schema identity.
// Every Variant ID contains the complete Candidate, normalized Spec,
// partition and order as unambiguously length-prefixed tokens.
struct DfsCursor;
struct DfsPage;
struct VariantReplay;
class V3Graph;
class V4Graph;
class SharedGraphKernel final
{
    static Page Enumerate(const std::vector<Axis>& axes, const Candidate& candidate,
        std::string_view schemaBinding, std::string_view graphTag, const Spec& supplied,
        std::string startOrdinal, std::size_t limit, std::string_view continuationBinding, PreparedGraph* prepared);
    static DfsPage EnumerateDfsCore(const PreparedGraph& graph, DfsCursor cursor,
        std::size_t workBudget, std::size_t outputLimit);
    static VariantReplay ReplayCore(const PreparedGraph& graph, std::string_view id);
    friend Page EnumerateInternal(const Context&, const Candidate&, const Spec&, std::string,
        std::size_t, std::string_view, PreparedGraph*);
    friend Page EnumerateInternal(const V3GraphKernelContext&, const Candidate&, const Spec&, std::string,
        std::size_t, std::string_view, PreparedGraph*);
    friend Page EnumerateInternal(const V4GraphKernelContext&, const Candidate&, const Spec&, std::string,
        std::size_t, std::string_view, PreparedGraph*);
    friend DfsPage EnumerateDfs(const Context&, const Candidate&, const Spec&, DfsCursor,
        std::size_t, std::size_t);
    friend DfsPage EnumerateDfs(const V3GraphKernelContext&, const Candidate&, const Spec&, DfsCursor,
        std::size_t, std::size_t);
    friend DfsPage EnumerateDfs(const V4GraphKernelContext&, const Candidate&, const Spec&, DfsCursor,
        std::size_t, std::size_t);
    friend VariantReplay ReplayVariant(const Context&, const Candidate&, const Spec&, std::string_view);
    friend VariantReplay ReplayVariant(const V3GraphKernelContext&, const Candidate&, const Spec&, std::string_view);
    friend VariantReplay ReplayVariant(const V4GraphKernelContext&, const Candidate&, const Spec&, std::string_view);
    friend class V3Graph;
    friend class V4Graph;
};

inline Page EnumerateInternal(const Context& context, const Candidate& input, const Spec& supplied,
    std::string startOrdinal, std::size_t limit, std::string_view continuationBinding, PreparedGraph* prepared)
{
    Page page;
    if (!prepared) page.obligations.emplace_back("legacy_pruning_audit_unimplemented");
    page.errors = RevalidateContext(context);
    if (!limit) page.errors.emplace_back("page_limit_must_be_positive");
    try { startOrdinal = Normalize(std::move(startOrdinal)); }
    catch (const std::exception&) { page.errors.emplace_back("invalid_start_ordinal"); }
    if (input.values.size() != context.axes.size()) page.errors.emplace_back("candidate_axis_count_mismatch");
    if (!page.errors.empty()) return page;
    Candidate candidate = input;
    candidate.lookup = context.lookup;
    for (std::size_t i = 0; i < context.axes.size(); ++i)
    {
        const auto& axis = context.axes[i];
        const auto& value = candidate.values[i];
        if (axis.role == Role::Derived)
        {
            if (!value.empty()) page.errors.push_back("candidate_derived_value_invalid:" + axis.id);
        }
        else if (std::find(axis.values.begin(), axis.values.end(), value) == axis.values.end())
            page.errors.push_back("candidate_value_invalid:" + axis.id);
        else if (!ExistingPrefixAllows(context, candidate, axis.id, value))
            page.errors.push_back("candidate_prefix_invalid:" + axis.id);
    }
    if (!page.errors.empty()) return page;
    page.structuralExclusion = ExistingStructuralExclusion(context, candidate);
    if (!page.structuralExclusion.empty()) return page;

    return SharedGraphKernel::Enumerate(context.axes, candidate, context.schemaBinding,
        ExistingGraphVersionTag(context.version), supplied, std::move(startOrdinal), limit, continuationBinding, prepared);
}

inline Page EnumerateInternal(const V3GraphKernelContext& context, const Candidate& input, const Spec& supplied,
    std::string startOrdinal, std::size_t limit, std::string_view continuationBinding, PreparedGraph* prepared)
{
    Page page;
    page.errors = RevalidateContext(context);
    if (!limit) page.errors.emplace_back("page_limit_must_be_positive");
    try { startOrdinal = Normalize(std::move(startOrdinal)); }
    catch (const std::exception&) { page.errors.emplace_back("invalid_start_ordinal"); }
    const auto& canonical = context.Validated().Coordinate();
    if (input.values != canonical.values || (input.lookup && canonical.lookup && *input.lookup != *canonical.lookup))
        page.errors.emplace_back("v3_graph_candidate_binding_mismatch");
    if (!page.errors.empty()) return page;
    return SharedGraphKernel::Enumerate(context.Identity().Axes(), canonical,
        V3ProgramGraphBinding(context.Identity(), context.Program()), "global_drag_event_graph_v3_exact_family",
        supplied, std::move(startOrdinal), limit, continuationBinding, prepared);
}

inline Page EnumerateInternal(const V4GraphKernelContext& context, const Candidate& input, const Spec& supplied,
    std::string startOrdinal, std::size_t limit, std::string_view continuationBinding, PreparedGraph* prepared)
{
    Page page;
    page.errors = RevalidateContext(context);
    if (!limit) page.errors.emplace_back("page_limit_must_be_positive");
    try { startOrdinal = Normalize(std::move(startOrdinal)); }
    catch (const std::exception&) { page.errors.emplace_back("invalid_start_ordinal"); }
    const auto& canonical = context.Validated().Coordinate();
    if (input.values != canonical.values || (input.lookup && canonical.lookup && *input.lookup != *canonical.lookup))
        page.errors.emplace_back("v4_graph_candidate_binding_mismatch");
    if (!page.errors.empty()) return page;
    const auto binding = V4ProgramGraphBinding(context.Identity(), canonical, context.Validated().TransferProgram());
    if (binding.empty()) { page.errors.emplace_back("v4_graph_program_binding_unavailable"); return page; }
    return SharedGraphKernel::Enumerate(context.Identity().Axes(), canonical, binding,
        context.Native() ? "global_drag_event_graph_v4_native_family" : "global_drag_event_graph_v4_generalized_family",
        supplied, std::move(startOrdinal), limit, continuationBinding, prepared);
}

// Pure requirement/pair/order normalization and enumeration. Only the
// version-specific validators above (or the V3 validator below) can enter it.
inline Page SharedGraphKernel::Enumerate(const std::vector<Axis>& axes, const Candidate& candidate,
    std::string_view schemaBinding, std::string_view graphTag, const Spec& supplied,
    std::string startOrdinal, std::size_t limit, std::string_view continuationBinding, PreparedGraph* prepared)
{
    Page page;
    if (!prepared) page.obligations.emplace_back("legacy_pruning_audit_unimplemented");

    auto requirements = supplied.requirements;
    std::sort(requirements.begin(), requirements.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    std::map<std::string, std::size_t> index;
    bool presenceExtension = false;
    for (std::size_t i = 0; i < requirements.size(); ++i)
    {
        const auto& r = requirements[i];
        if (r.id.empty() || !index.emplace(r.id, i).second) page.errors.emplace_back("invalid_or_duplicate_requirement_id");
        if (r.presence == PresencePolicy::required)
        {
            if (!r.presenceCondition.empty()) page.errors.push_back("required_presence_condition_invalid:" + r.id);
        }
        else if (r.presence == PresencePolicy::conditional_unresolved)
        {
            presenceExtension = true;
            if (r.presenceCondition.empty()) page.errors.push_back("conditional_presence_condition_missing:" + r.id);
        }
        else page.errors.push_back("presence_policy_invalid:" + r.id);
        const auto unresolvedToken = [](std::string_view value) {
            return value.empty() || value.find("unknown") != std::string_view::npos ||
                value.find("unresolved") != std::string_view::npos;
        };
        if (r.propertiesResolved && (unresolvedToken(r.role) || unresolvedToken(r.kind) || unresolvedToken(r.session) || unresolvedToken(r.task) ||
            unresolvedToken(r.object) || unresolvedToken(r.parameter)))
            page.errors.push_back("resolved_requirement_has_symbolic_property:" + r.id);
    }
    using Key = std::pair<std::size_t, std::size_t>;
    std::set<std::pair<Key, PresenceRelation>> presenceRules;
    for (const auto& constraint : supplied.presenceConstraints)
    {
        presenceExtension = true;
        const auto premise = index.find(constraint.premise), consequence = index.find(constraint.consequence);
        if (premise == index.end() || consequence == index.end() || premise->second == consequence->second ||
            PresenceRelationName(constraint.relation) == "invalid")
        { page.errors.emplace_back("invalid_presence_constraint"); continue; }
        Key key{premise->second, consequence->second};
        if (constraint.relation == PresenceRelation::present_excludes_present && key.second < key.first)
            std::swap(key.first, key.second);
        if (!presenceRules.emplace(key, constraint.relation).second)
            page.errors.emplace_back("duplicate_presence_constraint");
    }
    std::map<Key, PairRelation> rules;
    for (const auto& pair : supplied.pairs)
    {
        const auto left = index.find(pair.left), right = index.find(pair.right);
        if (left == index.end() || right == index.end() || left->second == right->second || RelationName(pair.relation) == "invalid")
        { page.errors.emplace_back("invalid_pair_rule"); continue; }
        const Key key{(std::min)(left->second, right->second), (std::max)(left->second, right->second)};
        if (pair.relation == PairRelation::same && DistinctSessionGroups(requirements[key.first], requirements[key.second]))
            page.errors.emplace_back("pair_conflicts_with_exclusive_session_groups");
        if (!rules.emplace(key, pair.relation).second) page.errors.emplace_back("duplicate_pair_rule");
    }
    std::set<Key> edges;
    for (const auto& edge : supplied.strictBefore)
    {
        const auto left = index.find(edge.earlier), right = index.find(edge.later);
        if (left == index.end() || right == index.end() || left->second == right->second)
        { page.errors.emplace_back("invalid_strict_before"); continue; }
        edges.emplace(left->second, right->second);
    }
    if (!page.errors.empty()) return page;
    // Do not flatten conditional event presence into the old all-present
    // graph. Until presence DFS and replay have versioned identities, reject
    // this schema before emitting a legacy ID, cursor or apparent coverage.
    // Specs with only required requirements and no presence constraints keep
    // precisely the existing v1 binding and v2 cursor byte representation.
    if (presenceExtension)
    { page.errors.emplace_back("presence_generation_unimplemented"); return page; }

    std::string prefix;
    AppendToken(prefix, graphTag);
    AppendToken(prefix, schemaBinding);
    AppendToken(prefix, Id(axes, candidate));
    AppendToken(prefix, supplied.causalCoverageComplete ? "causality_complete" : "causality_unresolved");
    auto specUnresolved = supplied.unresolved;
    std::sort(specUnresolved.begin(), specUnresolved.end());
    specUnresolved.erase(std::unique(specUnresolved.begin(), specUnresolved.end()), specUnresolved.end());
    AppendToken(prefix, std::to_string(specUnresolved.size()));
    for (const auto& reason : specUnresolved) AppendToken(prefix, reason);
    AppendToken(prefix, std::to_string(requirements.size()));
    for (const auto& r : requirements)
    {
        AppendToken(prefix, r.id); AppendToken(prefix, r.role); AppendToken(prefix, r.kind); AppendToken(prefix, r.session);
        AppendToken(prefix, r.task); AppendToken(prefix, r.object); AppendToken(prefix, r.parameter);
        AppendToken(prefix, std::to_string(r.occurrence)); AppendToken(prefix, r.propertiesResolved ? "resolved" : "unresolved");
    }
    // Preserve exact legacy bindings when no session group is supplied. The
    // extension binds every group, including empty values, in requirement order.
    if (std::any_of(requirements.begin(), requirements.end(), [](const auto& item) { return !item.exclusiveSessionGroup.empty(); }))
    {
        AppendToken(prefix, "exclusive_session_groups_v1");
        AppendToken(prefix, std::to_string(requirements.size()));
        for (const auto& item : requirements) AppendToken(prefix, item.exclusiveSessionGroup);
    }
    AppendToken(prefix, std::to_string(rules.size()));
    for (const auto& [key, relation] : rules)
    {
        AppendToken(prefix, requirements[key.first].id); AppendToken(prefix, requirements[key.second].id);
        AppendToken(prefix, RelationName(relation));
    }
    AppendToken(prefix, std::to_string(edges.size()));
    for (const auto& [left, right] : edges) { AppendToken(prefix, requirements[left].id); AppendToken(prefix, requirements[right].id); }
    if ((startOrdinal != "0" && continuationBinding.empty()) ||
        (!continuationBinding.empty() && continuationBinding != prefix))
    {
        page.errors.emplace_back("continuation_binding_mismatch");
        return page;
    }
    page.continuationBinding = prefix;
    if (prepared)
    {
        prepared->requirements = requirements;
        prepared->rules = rules;
        prepared->edges = edges;
        prepared->binding = prefix;
        prepared->unresolved = specUnresolved;
        prepared->causalCoverageComplete = supplied.causalCoverageComplete;
        return page;
    }

    std::string ordinal = "0";
    bool stopped = false;
    std::vector<std::size_t> partition(requirements.size());
    const auto relation = [&](std::size_t a, std::size_t b) {
        if (DistinctSessionGroups(requirements[a], requirements[b])) return PairRelation::distinct;
        const auto found = rules.find({(std::min)(a, b), (std::max)(a, b)});
        return found == rules.end() ? PairRelation::unresolved : found->second;
    };
    const auto resolvedMismatch = [](const Requirement& a, const Requirement& b) {
        return a.propertiesResolved && b.propertiesResolved &&
            (a.kind != b.kind || a.session != b.session || a.task != b.task ||
             a.object != b.object || a.parameter != b.parameter ||
             (a.role == b.role && a.occurrence != b.occurrence));
    };
    const auto emitPartition = [&](std::size_t classCount) {
        std::vector<std::string> representatives(classCount);
        std::vector<std::string> unresolved = specUnresolved;
        unresolved.emplace_back("candidate_event_mapping_unimplemented");
        unresolved.emplace_back("production_event_execution_and_completion_unimplemented");
        if (!supplied.causalCoverageComplete) unresolved.emplace_back("causal_relation_coverage_unresolved");
        for (std::size_t i = 0; i < requirements.size(); ++i)
        {
            if (representatives[partition[i]].empty()) representatives[partition[i]] = requirements[i].id;
            if (!requirements[i].propertiesResolved) unresolved.push_back("requirement_properties_unresolved:" + requirements[i].id);
            for (std::size_t j = 0; j < i; ++j)
                if (relation(j, i) == PairRelation::unresolved)
                    unresolved.push_back("pair_relation_unresolved:" + requirements[j].id + ":" + requirements[i].id);
        }
        std::vector<std::set<std::size_t>> outgoing(classCount);
        std::vector<std::size_t> indegree(classCount, 0);
        for (const auto& [a, b] : edges)
        {
            const auto from = partition[a], to = partition[b];
            if (from == to) return; // strict-before cannot hold within one event
            if (outgoing[from].insert(to).second) ++indegree[to];
        }
        std::vector<bool> used(classCount, false);
        std::vector<std::string> order;
        std::function<void()> walkOrder = [&] {
            if (stopped) return;
            if (order.size() == classCount)
            {
                if (Less(ordinal, startOrdinal)) { Increment(ordinal); return; }
                if (page.variants.size() == limit) { stopped = true; return; }
                Variant result;
                result.ordinal = ordinal; result.classes = partition; result.order = order; result.unresolved = unresolved;
                result.id = prefix;
                AppendToken(result.id, std::to_string(partition.size()));
                for (const auto value : partition) AppendToken(result.id, std::to_string(value));
                AppendToken(result.id, std::to_string(order.size()));
                for (const auto& event : order) AppendToken(result.id, event);
                page.variants.push_back(std::move(result));
                Increment(ordinal);
                return;
            }
            // Class numbering follows sorted requirement IDs, so this is a
            // stable representative order independent of caller vector order.
            for (std::size_t node = 0; node < classCount && !stopped; ++node)
            {
                if (used[node] || indegree[node]) continue;
                used[node] = true; order.push_back(representatives[node]);
                for (const auto next : outgoing[node]) --indegree[next];
                walkOrder();
                for (const auto next : outgoing[node]) ++indegree[next];
                order.pop_back(); used[node] = false;
            }
        };
        walkOrder(); // a cycle has no topological extension and is structural
    };
    std::function<void(std::size_t, std::size_t)> partitionWalk = [&](std::size_t at, std::size_t classes) {
        if (stopped) return;
        if (at == requirements.size()) { emitPartition(classes); return; }
        for (std::size_t choice = 0; choice <= classes && !stopped; ++choice)
        {
            bool allowed = true;
            for (std::size_t prior = 0; prior < at && allowed; ++prior)
            {
                const bool same = partition[prior] == choice;
                const auto rule = relation(prior, at);
                if ((rule == PairRelation::same && !same) || (rule == PairRelation::distinct && same) ||
                    (same && resolvedMismatch(requirements[prior], requirements[at]))) allowed = false;
            }
            if (!allowed) continue;
            partition[at] = choice;
            partitionWalk(at + 1, choice == classes ? classes + 1 : classes);
        }
    };
    partitionWalk(0, 0);
    page.nextOrdinal = ordinal;
    page.exhausted = !stopped;
    return page;
}

inline Page Enumerate(const Context& context, const Candidate& input, const Spec& supplied,
    std::string startOrdinal, std::size_t limit, std::string_view continuationBinding = {})
{
    return EnumerateInternal(context, input, supplied, std::move(startOrdinal), limit, continuationBinding, nullptr);
}

// A versioned schema binding for the future presence -> partition -> order
// traversal of this same Candidate. It is not a graph variant, an execution
// token, a count, or a substitute for a resumable presence cursor.
struct PresenceGraphPreparation
{
    std::string binding;
    std::vector<Requirement> requirements; // canonical requirement-ID order
    std::vector<PresenceConstraint> constraints; // canonical directed order
    std::vector<std::string> unresolved{
        "presence_cross_hypothesis_active_graph_enumeration_unimplemented",
        "presence_conditions_are_not_controlled_inputs_or_observed_results"};
    std::string structuralExclusion;
    std::vector<std::string> errors;
};

template <typename GraphContext>
inline PresenceGraphPreparation PreparePresenceGraph(const GraphContext& context, const Candidate& candidate,
    const Spec& supplied)
{
    PresenceGraphPreparation result;
    // Context is a public value type: validate its current contents rather
    // than trusting cached schema/lookup after a caller mutates a copy.
    result.errors = RevalidateContext(context);
    if (!result.errors.empty()) return result;
    result.requirements = supplied.requirements;
    std::sort(result.requirements.begin(), result.requirements.end(), [](const auto& left, const auto& right) {
        return left.id < right.id;
    });
    std::set<std::string> ids;
    for (const auto& item : result.requirements)
    {
        if (item.id.empty() || !ids.insert(item.id).second) result.errors.emplace_back("invalid_or_duplicate_requirement_id");
        if (item.presence == PresencePolicy::required)
        {
            if (!item.presenceCondition.empty()) result.errors.push_back("required_presence_condition_invalid:" + item.id);
        }
        else if (item.presence == PresencePolicy::conditional_unresolved)
        {
            if (item.presenceCondition.empty()) result.errors.push_back("conditional_presence_condition_missing:" + item.id);
        }
        else result.errors.push_back("presence_policy_invalid:" + item.id);
    }
    result.constraints = supplied.presenceConstraints;
    for (auto& rule : result.constraints)
        if (rule.relation == PresenceRelation::present_excludes_present && rule.consequence < rule.premise)
            std::swap(rule.premise, rule.consequence);
    std::sort(result.constraints.begin(), result.constraints.end(), [](const auto& left, const auto& right) {
        if (left.premise != right.premise) return left.premise < right.premise;
        if (left.consequence != right.consequence) return left.consequence < right.consequence;
        return static_cast<unsigned>(left.relation) < static_cast<unsigned>(right.relation);
    });
    for (std::size_t i = 0; i < result.constraints.size(); ++i)
    {
        const auto& rule = result.constraints[i];
        if (!ids.contains(rule.premise) || !ids.contains(rule.consequence) || rule.premise == rule.consequence ||
            PresenceRelationName(rule.relation) == "invalid")
            result.errors.emplace_back("invalid_presence_constraint");
        if (i && result.constraints[i - 1].premise == rule.premise &&
            result.constraints[i - 1].consequence == rule.consequence &&
            result.constraints[i - 1].relation == rule.relation)
            result.errors.emplace_back("duplicate_presence_constraint");
    }
    if (!result.errors.empty()) return result;

    // Reuse the existing complete candidate/schema, pair and causal-rule
    // validation without enumerating the all-present graph. The temporary
    // schema is never emitted as a presence-capable variant or cursor.
    Spec structural = supplied;
    structural.presenceConstraints.clear();
    for (auto& item : structural.requirements)
    { item.presence = PresencePolicy::required; item.presenceCondition.clear(); }
    PreparedGraph base;
    const auto validated = EnumerateInternal(context, candidate, structural, "0", 1, {}, &base);
    result.errors = validated.errors;
    result.structuralExclusion = validated.structuralExclusion;
    if (!result.errors.empty() || !result.structuralExclusion.empty()) return result;
    std::set<std::string> seenUnresolved(result.unresolved.begin(), result.unresolved.end());
    for (const auto& reason : base.unresolved)
        if (seenUnresolved.insert(reason).second) result.unresolved.push_back(reason);
    if (!base.causalCoverageComplete &&
        std::find(result.unresolved.begin(), result.unresolved.end(), "causal_relation_coverage_unresolved") == result.unresolved.end())
        result.unresolved.emplace_back("causal_relation_coverage_unresolved");

    // v2 also binds the preparation obligations, so a cursor cannot resume
    // across a change in what those hypothetical pages claim is unresolved.
    AppendToken(result.binding, "global_drag_presence_schema_v2");
    AppendToken(result.binding, base.binding);
    AppendToken(result.binding, std::to_string(result.unresolved.size()));
    for (const auto& reason : result.unresolved) AppendToken(result.binding, reason);
    AppendToken(result.binding, std::to_string(result.requirements.size()));
    for (const auto& item : result.requirements)
    {
        AppendToken(result.binding, item.id);
        AppendToken(result.binding, item.presence == PresencePolicy::required ? "required" : "conditional_unresolved");
        AppendToken(result.binding, item.presenceCondition);
    }
    AppendToken(result.binding, std::to_string(result.constraints.size()));
    for (const auto& rule : result.constraints)
    {
        AppendToken(result.binding, rule.premise); AppendToken(result.binding, rule.consequence);
        AppendToken(result.binding, PresenceRelationName(rule.relation));
    }
    return result;
}

// An assignment is only a proposition to check against a future bounded
// production observation. In particular, choosing absent neither controls
// the host nor proves that a message did not occur. This is not a variant ID.
struct PresenceHypothesis
{
    bool structurallyValid = false;
    std::string binding;
    std::vector<std::string> unresolved;
    std::vector<std::string> errors;
};
template <typename GraphContext>
inline PresenceHypothesis InspectPresenceHypothesis(const GraphContext& context, const Candidate& candidate,
    const Spec& supplied, const std::vector<bool>& present)
{
    PresenceHypothesis result;
    const auto schema = PreparePresenceGraph(context, candidate, supplied);
    result.unresolved = schema.unresolved;
    result.unresolved.emplace_back("presence_hypothesis_not_a_product_observation");
    result.unresolved.emplace_back("candidate_event_mapping_unimplemented");
    result.unresolved.emplace_back("production_event_execution_and_completion_unimplemented");
    if (!schema.errors.empty() || !schema.structuralExclusion.empty() || schema.binding.empty())
    {
        result.errors.insert(result.errors.end(), schema.errors.begin(), schema.errors.end());
        if (!schema.structuralExclusion.empty()) result.errors.push_back("presence_schema_structure_excluded:" + schema.structuralExclusion);
        result.errors.emplace_back("presence_schema_unavailable"); return result;
    }
    if (present.size() != schema.requirements.size())
    { result.errors.emplace_back("presence_hypothesis_size_invalid"); return result; }
    std::map<std::string, std::size_t> indices;
    for (std::size_t i = 0; i < schema.requirements.size(); ++i)
    {
        const auto& requirement = schema.requirements[i];
        indices.emplace(requirement.id, i);
        if (requirement.presence == PresencePolicy::required && !present[i])
            result.errors.push_back("presence_hypothesis_required_absent:" + requirement.id);
        if (requirement.presence == PresencePolicy::conditional_unresolved)
            result.unresolved.push_back("presence_condition_unobserved:" + requirement.id);
    }
    for (const auto& rule : schema.constraints)
    {
        const auto premise = indices.find(rule.premise), consequence = indices.find(rule.consequence);
        if (premise == indices.end() || consequence == indices.end() ||
            PresenceRelationName(rule.relation) == "invalid")
        { result.errors.emplace_back("presence_schema_constraint_invalid"); continue; }
        if (rule.relation == PresenceRelation::present_implies_present &&
            present[premise->second] && !present[consequence->second])
            result.errors.push_back("presence_hypothesis_implication_conflict:" + rule.premise + ":" + rule.consequence);
        if (rule.relation == PresenceRelation::present_excludes_present &&
            present[premise->second] && present[consequence->second])
            result.errors.push_back("presence_hypothesis_exclusive_conflict:" + rule.premise + ":" + rule.consequence);
    }
    if (!result.errors.empty()) return result;
    std::map<std::pair<std::size_t, std::size_t>, PairRelation> pairRules;
    for (const auto& rule : supplied.pairs)
    {
        const auto left = indices.at(rule.left), right = indices.at(rule.right);
        pairRules.emplace(std::make_pair((std::min)(left, right), (std::max)(left, right)), rule.relation);
    }
    for (std::size_t i = 0; i < schema.requirements.size(); ++i)
    {
        if (!present[i]) continue;
        if (!schema.requirements[i].propertiesResolved)
            result.unresolved.push_back("requirement_properties_unresolved:" + schema.requirements[i].id);
        for (std::size_t j = 0; j < i; ++j)
        {
            if (!present[j]) continue;
            const auto relation = pairRules.find({j, i});
            if (!DistinctSessionGroups(schema.requirements[j], schema.requirements[i]) &&
                (relation == pairRules.end() || relation->second == PairRelation::unresolved))
                result.unresolved.push_back("pair_relation_unresolved:" + schema.requirements[j].id + ":" + schema.requirements[i].id);
        }
    }
    AppendToken(result.binding, "global_drag_presence_hypothesis_v1");
    AppendToken(result.binding, schema.binding);
    AppendToken(result.binding, std::to_string(present.size()));
    for (const bool value : present) AppendToken(result.binding, value ? "present" : "absent");
    result.structurallyValid = true;
    return result;
}

// A validated projection is still a hypothetical input to future graph
// generation. It deliberately is not a Spec, Variant, product result or v1
// graph ID, so an old executor cannot silently consume an all-present graph.
struct PresenceProjection
{
    bool structurallyValid = false;
    std::string schemaBinding, hypothesisBinding, binding;
    std::vector<Requirement> requirements;
    std::vector<PairRule> pairs;
    std::vector<Before> strictBefore;
    std::vector<std::string> unresolved;
    std::string structuralExclusion;
    std::vector<std::string> errors;
};

template <typename GraphContext>
inline PresenceProjection ProjectPresenceHypothesis(const GraphContext& context, const Candidate& candidate,
    const Spec& supplied, const std::vector<bool>& present)
{
    PresenceProjection result;
    // Rebuild both objects from authoritative inputs: a caller cannot inject
    // a mutated preparation or stale hypothesis binding into this path.
    const auto schema = PreparePresenceGraph(context, candidate, supplied);
    result.errors = schema.errors;
    result.structuralExclusion = schema.structuralExclusion;
    if (!result.errors.empty() || !result.structuralExclusion.empty() || schema.binding.empty())
    {
        if (result.errors.empty() && result.structuralExclusion.empty()) result.errors.emplace_back("presence_projection_schema_unavailable");
        return result;
    }
    const auto hypothesis = InspectPresenceHypothesis(context, candidate, supplied, present);
    result.unresolved = hypothesis.unresolved;
    result.errors = hypothesis.errors;
    if (!hypothesis.structurallyValid || !result.errors.empty())
    {
        if (result.errors.empty()) result.errors.emplace_back("presence_projection_hypothesis_invalid");
        return result;
    }
    std::string expectedHypothesis;
    AppendToken(expectedHypothesis, "global_drag_presence_hypothesis_v1");
    AppendToken(expectedHypothesis, schema.binding);
    AppendToken(expectedHypothesis, std::to_string(present.size()));
    for (const bool value : present) AppendToken(expectedHypothesis, value ? "present" : "absent");
    if (hypothesis.binding != expectedHypothesis)
    { result.errors.emplace_back("presence_projection_hypothesis_binding_mismatch"); return result; }

    std::set<std::string> active;
    for (std::size_t i = 0; i < schema.requirements.size(); ++i)
        if (present[i])
        {
            result.requirements.push_back(schema.requirements[i]);
            active.insert(schema.requirements[i].id);
        }
    for (const auto& rule : supplied.pairs)
    {
        if (!active.contains(rule.left) || !active.contains(rule.right)) continue;
        const auto& left = (std::min)(rule.left, rule.right);
        const auto& right = (std::max)(rule.left, rule.right);
        result.pairs.push_back({left, right, rule.relation});
    }
    std::sort(result.pairs.begin(), result.pairs.end(), [](const auto& left, const auto& right) {
        return left.left != right.left ? left.left < right.left : left.right < right.right;
    });
    std::set<std::pair<std::string, std::string>> edges;
    for (const auto& edge : supplied.strictBefore)
        if (active.contains(edge.earlier) && active.contains(edge.later)) edges.emplace(edge.earlier, edge.later);
    for (const auto& [earlier, later] : edges) result.strictBefore.push_back({earlier, later});
    // Do not infer a new A->C edge merely because A->B->C had an absent B.
    result.schemaBinding = schema.binding;
    result.hypothesisBinding = hypothesis.binding;
    AppendToken(result.binding, "global_drag_presence_projection_v1");
    AppendToken(result.binding, result.hypothesisBinding);
    result.unresolved.emplace_back("presence_active_graph_execution_and_cross_hypothesis_enumeration_unimplemented");
    result.unresolved.emplace_back("presence_projection_is_not_production_evidence");
    result.structurallyValid = true;
    return result;
}

// Incremental, in-process construction of the same projection values. This
// object is created only from a validated authority and owns its continuation;
// callers cannot import or manufacture a cursor. Durable cursor authentication
// and integration into JoinedPresenceGraph remain separate capabilities.
enum class PresenceProjectionWorkStage { prefix, conditions, unresolved, requirements, pairs, edges, suffix, done };
struct PresenceProjectionWorkCursor
{
    std::string version = "presence_projection_work_v2", binding;
    PresenceProjectionWorkStage stage = PresenceProjectionWorkStage::prefix;
    std::size_t index = 0, prior = 0;
    bool itemStarted = false;
};
struct PresenceProjectionWorkPage
{
    PresenceProjectionWorkCursor before, after;
    std::vector<Requirement> requirements;
    std::vector<PairRule> pairs;
    std::vector<Before> strictBefore;
    std::vector<std::string> unresolved, errors;
    std::size_t workUsed = 0;
    bool done = false, paused = false, resourceUnavailable = false;
};
class PresenceProjectionWork;
class PresenceActiveGraph;
class PresenceProjectionWorkProposal final
{
    friend class PresenceProjectionWork;
public:
    PresenceProjectionWorkProposal(const PresenceProjectionWorkProposal&) = delete;
    PresenceProjectionWorkProposal& operator=(const PresenceProjectionWorkProposal&) = delete;
    const PresenceProjectionWorkPage& Page() const noexcept { return page_; }
private:
    PresenceProjectionWorkProposal(const PresenceProjectionWork* owner, std::weak_ptr<const int> capability, PresenceProjectionWorkPage page)
        : owner_(owner), capability_(std::move(capability)), page_(std::move(page)) {}
    const PresenceProjectionWork* owner_;
    std::weak_ptr<const int> capability_;
    PresenceProjectionWorkPage page_;
};
class PresenceProjectionWork final
{
    friend class PresenceActiveGraph;
public:
    PresenceProjectionWork(const PresenceProjectionWork&) = delete;
    PresenceProjectionWork& operator=(const PresenceProjectionWork&) = delete;
    const PresenceProjectionWorkCursor& Cursor() const noexcept { return cursor_; }
    const std::string& SchemaBinding() const noexcept { return schemaBinding_; }
    const std::string& HypothesisBinding() const noexcept { return hypothesisBinding_; }
    const std::string& ProjectionBinding() const noexcept { return projectionBinding_; }
    // Available for comparison only. Its binding and contents do not yet
    // authenticate a restored checkpoint or authorize an active graph.
    std::optional<PresenceProjection> CompletedForInspection() const
    { if (cursor_.stage != PresenceProjectionWorkStage::done) return std::nullopt; return projection_; }
    // This only authorizes construction of an in-process event-hypothesis
    // graph after authority/presence revalidation. It never authorizes product
    // input or a pass verdict; complete graph and execution evidence remain
    // separate. It is not a durable authenticated cursor.
    bool CanAuthorizeActiveGraph() const noexcept
    { return cursor_.stage == PresenceProjectionWorkStage::done && projection_.structurallyValid; }

    template <typename GraphContext>
    static std::unique_ptr<PresenceProjectionWork> Create(const GraphContext& context, const Candidate& candidate,
        const Spec& supplied, const std::vector<bool>& present, std::size_t preambleBudget,
        bool& resourceUnavailable, std::vector<std::string>& errors)
    {
        resourceUnavailable = false;
        errors.clear();
        try
        {
            // A caller-controlled input-volume budget is checked before
            // constructing the authority schema. It is not a wall-clock
            // bound: schema validation/sorting still runs synchronously.
            auto remaining = preambleBudget;
            const auto charge = [&](std::size_t amount) {
                if (amount > remaining) return false;
                remaining -= amount; return true;
            };
            const auto token = [&](std::string_view value) { return charge(1) && charge(value.size()); };
            if (!charge(present.size()) || !token(candidate.ordinal)) { resourceUnavailable = true; return {}; }
            for (const auto& value : candidate.values)
                if (!token(value)) { resourceUnavailable = true; return {}; }
            for (const auto& item : supplied.requirements)
                if (!token(item.id) || !token(item.role) || !token(item.kind) || !token(item.session) || !token(item.task) ||
                    !token(item.object) || !token(item.parameter) || !token(item.presenceCondition) || !token(item.exclusiveSessionGroup))
                { resourceUnavailable = true; return {}; }
            for (const auto& item : supplied.pairs)
                if (!token(item.left) || !token(item.right)) { resourceUnavailable = true; return {}; }
            for (const auto& item : supplied.strictBefore)
                if (!token(item.earlier) || !token(item.later)) { resourceUnavailable = true; return {}; }
            for (const auto& item : supplied.presenceConstraints)
                if (!token(item.premise) || !token(item.consequence)) { resourceUnavailable = true; return {}; }
            for (const auto& item : supplied.unresolved)
                if (!token(item)) { resourceUnavailable = true; return {}; }
            const auto schema = PreparePresenceGraph(context, candidate, supplied);
            errors = schema.errors;
            if (!schema.structuralExclusion.empty()) errors.push_back("presence_projection_structure_excluded:" + schema.structuralExclusion);
            if (!errors.empty()) return {};
            if (schema.binding.empty()) { errors.emplace_back("presence_projection_schema_unavailable"); return {}; }
            if (!token(schema.binding)) { resourceUnavailable = true; return {}; }
            if (present.size() != schema.requirements.size()) { errors.emplace_back("presence_hypothesis_size_invalid"); return {}; }
            std::map<std::string, std::size_t> indices;
            for (std::size_t i = 0; i < schema.requirements.size(); ++i)
            {
                indices.emplace(schema.requirements[i].id, i);
                if (schema.requirements[i].presence == PresencePolicy::required && !present[i])
                    errors.push_back("presence_hypothesis_required_absent:" + schema.requirements[i].id);
            }
            for (const auto& rule : schema.constraints)
            {
                const auto a = indices.find(rule.premise), b = indices.find(rule.consequence);
                if (a == indices.end() || b == indices.end()) { errors.emplace_back("presence_schema_constraint_invalid"); continue; }
                if (rule.relation == PresenceRelation::present_implies_present && present[a->second] && !present[b->second])
                    errors.push_back("presence_hypothesis_implication_conflict:" + rule.premise + ":" + rule.consequence);
                if (rule.relation == PresenceRelation::present_excludes_present && present[a->second] && present[b->second])
                    errors.push_back("presence_hypothesis_exclusive_conflict:" + rule.premise + ":" + rule.consequence);
            }
            if (!errors.empty()) return {};
            auto work = std::unique_ptr<PresenceProjectionWork>(new PresenceProjectionWork);
            work->capability_ = std::make_shared<const int>(0);
            work->requirements_ = schema.requirements; work->present_ = present;
            work->prefix_ = schema.unresolved;
            work->prefix_.emplace_back("presence_hypothesis_not_a_product_observation");
            work->prefix_.emplace_back("candidate_event_mapping_unimplemented");
            work->prefix_.emplace_back("production_event_execution_and_completion_unimplemented");
            work->suffix_ = {"presence_active_graph_execution_and_cross_hypothesis_enumeration_unimplemented",
                "presence_projection_is_not_production_evidence"};
            work->schemaBinding_ = schema.binding;
            AppendToken(work->hypothesisBinding_, "global_drag_presence_hypothesis_v1");
            AppendToken(work->hypothesisBinding_, schema.binding);
            AppendToken(work->hypothesisBinding_, std::to_string(present.size()));
            for (const bool bit : present) AppendToken(work->hypothesisBinding_, bit ? "present" : "absent");
            AppendToken(work->projectionBinding_, "global_drag_presence_projection_v1");
            AppendToken(work->projectionBinding_, work->hypothesisBinding_);
            work->projection_.schemaBinding = work->schemaBinding_;
            work->projection_.hypothesisBinding = work->hypothesisBinding_;
            work->projection_.binding = work->projectionBinding_;
            AppendToken(work->cursor_.binding, "presence_projection_work_binding_v2");
            AppendToken(work->cursor_.binding, work->projectionBinding_);
            for (const auto& rule : supplied.pairs)
            {
                const auto a = indices.at(rule.left), b = indices.at(rule.right);
                work->rules_.emplace(std::make_pair((std::min)(a, b), (std::max)(a, b)), rule.relation);
                if (present[a] && present[b]) work->pairs_.push_back({(std::min)(rule.left, rule.right),
                    (std::max)(rule.left, rule.right), rule.relation});
            }
            std::sort(work->pairs_.begin(), work->pairs_.end(), [](const auto& a, const auto& b) {
                return a.left != b.left ? a.left < b.left : a.right < b.right;
            });
            std::set<std::pair<std::string, std::string>> edges;
            for (const auto& edge : supplied.strictBefore)
                if (present[indices.at(edge.earlier)] && present[indices.at(edge.later)]) edges.emplace(edge.earlier, edge.later);
            for (const auto& [a, b] : edges) work->edges_.push_back({a, b});
            return work;
        }
        catch (const std::bad_alloc&) { resourceUnavailable = true; return {}; }
        catch (const std::length_error&) { resourceUnavailable = true; return {}; }
        catch (const std::exception&) { errors.emplace_back("presence_projection_creation_unavailable"); return {}; }
    }

    // The caller publishes Page() and its returned continuation together;
    // only after that transaction succeeds may it commit this proposal.
    std::unique_ptr<PresenceProjectionWorkProposal> PrepareStep(std::size_t workBudget,
        std::size_t outputLimit, bool& resourceUnavailable) const
    {
        resourceUnavailable = false;
        try
        {
            auto page = Preview(workBudget, outputLimit);
            resourceUnavailable = page.resourceUnavailable;
            return std::unique_ptr<PresenceProjectionWorkProposal>(new PresenceProjectionWorkProposal(this, capability_, std::move(page)));
        }
        catch (const std::bad_alloc&) { resourceUnavailable = true; return {}; }
        catch (const std::length_error&) { resourceUnavailable = true; return {}; }
    }
    bool Commit(const PresenceProjectionWorkProposal& proposal, bool& resourceUnavailable) noexcept
    {
        resourceUnavailable = false;
        const auto& page = proposal.page_;
        const auto& before = page.before;
        const auto capability = proposal.capability_.lock();
        if (proposal.owner_ != this || !capability || !capability_ || capability.get() != capability_.get() ||
            capability.owner_before(capability_) || capability_.owner_before(capability) || page.resourceUnavailable || !page.errors.empty() ||
            before.version != cursor_.version || before.binding != cursor_.binding || before.stage != cursor_.stage ||
            before.index != cursor_.index || before.prior != cursor_.prior || before.itemStarted != cursor_.itemStarted ||
            page.after.version != cursor_.version || page.after.binding != cursor_.binding) return false;
        const auto requirements = projection_.requirements.size(), pairs = projection_.pairs.size();
        const auto edges = projection_.strictBefore.size(), unresolved = projection_.unresolved.size();
        try
        {
            projection_.requirements.insert(projection_.requirements.end(), page.requirements.begin(), page.requirements.end());
            projection_.pairs.insert(projection_.pairs.end(), page.pairs.begin(), page.pairs.end());
            projection_.strictBefore.insert(projection_.strictBefore.end(), page.strictBefore.begin(), page.strictBefore.end());
            projection_.unresolved.insert(projection_.unresolved.end(), page.unresolved.begin(), page.unresolved.end());
        }
        catch (const std::bad_alloc&)
        {
            projection_.requirements.resize(requirements); projection_.pairs.resize(pairs);
            projection_.strictBefore.resize(edges); projection_.unresolved.resize(unresolved);
            resourceUnavailable = true; return false;
        }
        catch (const std::length_error&)
        {
            projection_.requirements.resize(requirements); projection_.pairs.resize(pairs);
            projection_.strictBefore.resize(edges); projection_.unresolved.resize(unresolved);
            resourceUnavailable = true; return false;
        }
        catch (...)
        {
            projection_.requirements.resize(requirements); projection_.pairs.resize(pairs);
            projection_.strictBefore.resize(edges); projection_.unresolved.resize(unresolved);
            return false;
        }
        cursor_.stage = page.after.stage; cursor_.index = page.after.index; cursor_.prior = page.after.prior;
        cursor_.itemStarted = page.after.itemStarted;
        projection_.structurallyValid = cursor_.stage == PresenceProjectionWorkStage::done;
        return true;
    }
private:
    PresenceProjectionWorkPage Preview(std::size_t workBudget, std::size_t outputLimit) const
    {
        PresenceProjectionWorkPage out;
        try
        {
            out.before = cursor_; out.after = cursor_;
            if (!outputLimit) { out.errors.emplace_back("presence_projection_output_limit_zero"); return out; }
            auto next = cursor_;
            const auto full = [&] {
                auto remaining = outputLimit;
                for (const auto size : {out.requirements.size(), out.pairs.size(), out.strictBefore.size(), out.unresolved.size()})
                { if (size >= remaining) return true; remaining -= size; }
                return false;
            };
            while (out.workUsed < workBudget && !full() && next.stage != PresenceProjectionWorkStage::done)
            {
                if (next.stage == PresenceProjectionWorkStage::prefix || next.stage == PresenceProjectionWorkStage::suffix)
                {
                    const auto& values = next.stage == PresenceProjectionWorkStage::prefix ? prefix_ : suffix_;
                    if (next.index == values.size()) { next.stage = next.stage == PresenceProjectionWorkStage::prefix ? PresenceProjectionWorkStage::conditions : PresenceProjectionWorkStage::done; next.index = 0; continue; }
                    ++out.workUsed; out.unresolved.push_back(values[next.index++]); continue;
                }
                if (next.stage == PresenceProjectionWorkStage::conditions)
                {
                    if (next.index == requirements_.size()) { next.stage = PresenceProjectionWorkStage::unresolved; next.index = 0; continue; }
                    const auto i = next.index++; ++out.workUsed;
                    if (requirements_[i].presence == PresencePolicy::conditional_unresolved)
                        out.unresolved.push_back("presence_condition_unobserved:" + requirements_[i].id);
                    continue;
                }
                if (next.stage == PresenceProjectionWorkStage::unresolved)
                {
                    if (next.index == requirements_.size()) { next.stage = PresenceProjectionWorkStage::requirements; next.index = next.prior = 0; next.itemStarted = false; continue; }
                    const auto i = next.index;
                    if (!next.itemStarted)
                    {
                        ++out.workUsed;
                        if (!present_[i]) { ++next.index; next.prior = 0; continue; }
                        next.itemStarted = true;
                        if (!requirements_[i].propertiesResolved)
                            out.unresolved.push_back("requirement_properties_unresolved:" + requirements_[i].id);
                        continue;
                    }
                    if (next.prior == i) { ++next.index; next.prior = 0; next.itemStarted = false; continue; }
                    const auto j = next.prior++; ++out.workUsed;
                    if (!present_[i] || !present_[j] || DistinctSessionGroups(requirements_[j], requirements_[i])) continue;
                    const auto found = rules_.find({j, i});
                    if (found == rules_.end() || found->second == PairRelation::unresolved)
                        out.unresolved.push_back("pair_relation_unresolved:" + requirements_[j].id + ":" + requirements_[i].id);
                    continue;
                }
                if (next.stage == PresenceProjectionWorkStage::requirements)
                {
                    if (next.index == requirements_.size()) { next.stage = PresenceProjectionWorkStage::pairs; next.index = 0; continue; }
                    const auto i = next.index++; ++out.workUsed; if (present_[i]) out.requirements.push_back(requirements_[i]); continue;
                }
                if (next.stage == PresenceProjectionWorkStage::pairs)
                {
                    if (next.index == pairs_.size()) { next.stage = PresenceProjectionWorkStage::edges; next.index = 0; continue; }
                    ++out.workUsed; out.pairs.push_back(pairs_[next.index++]); continue;
                }
                if (next.stage == PresenceProjectionWorkStage::edges)
                {
                    if (next.index == edges_.size()) { next.stage = PresenceProjectionWorkStage::suffix; next.index = 0; continue; }
                    ++out.workUsed; out.strictBefore.push_back(edges_[next.index++]); continue;
                }
            }
            out.after = next;
            out.done = next.stage == PresenceProjectionWorkStage::done; out.paused = !out.done;
        }
        catch (const std::bad_alloc&) { out.requirements.clear(); out.pairs.clear(); out.strictBefore.clear(); out.unresolved.clear(); out.workUsed = 0; out.resourceUnavailable = true; out.paused = true; }
        catch (const std::length_error&) { out.requirements.clear(); out.pairs.clear(); out.strictBefore.clear(); out.unresolved.clear(); out.workUsed = 0; out.resourceUnavailable = true; out.paused = true; }
        return out;
    }
    PresenceProjectionWork() = default;
    std::shared_ptr<const int> capability_;
    PresenceProjectionWorkCursor cursor_;
    PresenceProjection projection_;
    std::string schemaBinding_, hypothesisBinding_, projectionBinding_;
    std::vector<Requirement> requirements_;
    std::vector<bool> present_;
    std::map<std::pair<std::size_t, std::size_t>, PairRelation> rules_;
    std::vector<PairRule> pairs_;
    std::vector<Before> edges_;
    std::vector<std::string> prefix_, suffix_;
};

// Serialized coordinates are requests to replay, not capabilities. In
// particular a caller cannot authorize a completed projection by submitting
// a cursor whose fields merely look complete.
inline bool ParseCanonicalSize(std::string_view value, std::size_t& result);
inline bool ReadGraphToken(std::string_view& remaining, std::string_view& value);
struct PresenceProjectionCursorCodec
{
    std::optional<PresenceProjectionWorkCursor> cursor;
    std::string encoded;
    std::vector<std::string> errors;
    bool resourceUnavailable = false;
};
inline PresenceProjectionCursorCodec EncodePresenceProjectionCursor(const PresenceProjectionWorkCursor& cursor)
{
    PresenceProjectionCursorCodec out;
    try
    {
        if (cursor.version != "presence_projection_work_v2" || cursor.binding.empty() ||
            static_cast<unsigned>(cursor.stage) > static_cast<unsigned>(PresenceProjectionWorkStage::done))
        { out.errors.emplace_back("presence_projection_cursor_invalid"); return out; }
        AppendToken(out.encoded, "presence_projection_cursor_encoding_v1"); AppendToken(out.encoded, cursor.version);
        AppendToken(out.encoded, cursor.binding); AppendToken(out.encoded, std::to_string(static_cast<unsigned>(cursor.stage)));
        AppendToken(out.encoded, std::to_string(cursor.index)); AppendToken(out.encoded, std::to_string(cursor.prior));
        AppendToken(out.encoded, cursor.itemStarted ? "true" : "false"); out.cursor = cursor;
    }
    catch (const std::bad_alloc&) { out.encoded.clear(); out.cursor.reset(); out.resourceUnavailable = true; }
    catch (const std::length_error&) { out.encoded.clear(); out.cursor.reset(); out.resourceUnavailable = true; }
    return out;
}
inline PresenceProjectionCursorCodec ParsePresenceProjectionCursor(std::string_view encoded)
{
    PresenceProjectionCursorCodec out;
    try
    {
        auto remaining = encoded; std::string_view token; PresenceProjectionWorkCursor cursor; std::size_t stage = 0;
        if (!ReadGraphToken(remaining, token) || token != "presence_projection_cursor_encoding_v1" ||
            !ReadGraphToken(remaining, token) || token != "presence_projection_work_v2")
        { out.errors.emplace_back("presence_projection_cursor_version_unsupported"); return out; }
        cursor.version = std::string(token);
        if (!ReadGraphToken(remaining, token) || token.empty()) { out.errors.emplace_back("presence_projection_cursor_invalid"); return out; }
        cursor.binding = std::string(token);
        if (!ReadGraphToken(remaining, token) || !ParseCanonicalSize(token, stage) ||
            stage > static_cast<std::size_t>(PresenceProjectionWorkStage::done) ||
            !ReadGraphToken(remaining, token) || !ParseCanonicalSize(token, cursor.index) ||
            !ReadGraphToken(remaining, token) || !ParseCanonicalSize(token, cursor.prior) ||
            !ReadGraphToken(remaining, token) || (token != "true" && token != "false") || !remaining.empty())
        { out.errors.emplace_back("presence_projection_cursor_invalid"); return out; }
        cursor.stage = static_cast<PresenceProjectionWorkStage>(stage); cursor.itemStarted = token == "true";
        auto canonical = EncodePresenceProjectionCursor(cursor);
        if (canonical.resourceUnavailable || !canonical.errors.empty()) return canonical;
        if (canonical.encoded != encoded) { out.errors.emplace_back("presence_projection_cursor_noncanonical"); return out; }
        return canonical;
    }
    catch (const std::bad_alloc&) { out.resourceUnavailable = true; return out; }
    catch (const std::length_error&) { out.resourceUnavailable = true; return out; }
}

struct PresenceProjectionRebuildPage
{
    std::size_t workUsed = 0;
    bool reached = false, paused = false, resourceUnavailable = false;
    std::vector<std::string> errors;
};
class PresenceProjectionRebuild final
{
public:
    PresenceProjectionRebuild(const PresenceProjectionRebuild&) = delete;
    PresenceProjectionRebuild& operator=(const PresenceProjectionRebuild&) = delete;
    const PresenceProjectionWorkCursor* ReplayedCursor() const noexcept
    { return work_ ? &work_->Cursor() : nullptr; }
    template <typename GraphContext>
    static std::unique_ptr<PresenceProjectionRebuild> Create(const GraphContext& context, const Candidate& candidate,
        const Spec& supplied, const std::vector<bool>& present, std::string_view encodedCursor,
        std::size_t preambleBudget, bool& resourceUnavailable, std::vector<std::string>& errors)
    {
        resourceUnavailable = false; errors.clear();
        try
        {
            const auto parsed = ParsePresenceProjectionCursor(encodedCursor);
            if (parsed.resourceUnavailable) { resourceUnavailable = true; return {}; }
            if (!parsed.cursor || !parsed.errors.empty()) { errors = parsed.errors; return {}; }
            auto work = PresenceProjectionWork::Create(context, candidate, supplied, present, preambleBudget, resourceUnavailable, errors);
            if (!work) return {};
            if (parsed.cursor->binding != work->Cursor().binding || parsed.cursor->version != work->Cursor().version)
            { errors.emplace_back("presence_projection_rebuild_binding_mismatch"); return {}; }
            return std::unique_ptr<PresenceProjectionRebuild>(new PresenceProjectionRebuild(std::move(work), *parsed.cursor));
        }
        catch (const std::bad_alloc&) { resourceUnavailable = true; return {}; }
        catch (const std::length_error&) { resourceUnavailable = true; return {}; }
    }
    PresenceProjectionRebuildPage Step(std::size_t workBudget)
    {
        PresenceProjectionRebuildPage out;
        try
        {
            if (!work_) { out.errors.emplace_back("presence_projection_rebuild_already_taken"); return out; }
            while (out.workUsed < workBudget && !Same(work_->Cursor(), target_))
            {
                if (work_->Cursor().stage == PresenceProjectionWorkStage::done)
                { out.errors.emplace_back("presence_projection_rebuild_cursor_unreachable"); return out; }
                bool resource = false;
                auto proposal = work_->PrepareStep(1, 1, resource);
                if (resource || !proposal || proposal->Page().resourceUnavailable)
                { out.resourceUnavailable = resource || (proposal && proposal->Page().resourceUnavailable); out.paused = true; return out; }
                const auto& page = proposal->Page();
                if (!page.errors.empty()) { out.errors = page.errors; return out; }
                const bool advanced = !Same(page.before, page.after);
                if (!page.workUsed && !advanced) { out.errors.emplace_back("presence_projection_rebuild_no_progress"); return out; }
                bool commitResource = false;
                if (!work_->Commit(*proposal, commitResource))
                { out.resourceUnavailable = commitResource; if (!commitResource) out.errors.emplace_back("presence_projection_rebuild_commit_invalid"); out.paused = true; return out; }
                out.workUsed += page.workUsed;
            }
            out.reached = Same(work_->Cursor(), target_); out.paused = !out.reached;
        }
        catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.paused = true; }
        catch (const std::length_error&) { out.resourceUnavailable = true; out.paused = true; }
        return out;
    }
    // The rebuilt object contains the authoritative prefix, not caller-supplied
    // projection data. After a crash, reconstruction begins again from zero.
    std::unique_ptr<PresenceProjectionWork> Take() noexcept
    { return work_ && Same(work_->Cursor(), target_) ? std::move(work_) : nullptr; }
private:
    PresenceProjectionRebuild(std::unique_ptr<PresenceProjectionWork> work, PresenceProjectionWorkCursor target)
        : work_(std::move(work)), target_(std::move(target)) {}
    static bool Same(const PresenceProjectionWorkCursor& a, const PresenceProjectionWorkCursor& b) noexcept
    { return a.version == b.version && a.binding == b.binding && a.stage == b.stage && a.index == b.index && a.prior == b.prior && a.itemStarted == b.itemStarted; }
    std::unique_ptr<PresenceProjectionWork> work_;
    PresenceProjectionWorkCursor target_;
};

enum class DfsStage { partition, topology, done };
struct DfsCursor
{
    // Version 2 adds topologyProduced and cumulative prune-count state.
    // Older cursor encodings cannot be safely resumed as this version.
    std::string version = "event_graph_dfs_v2";
    std::string binding;
    // Detects accidental corruption only. FNV is not authentication and does
    // not prove an untampered cursor or complete earlier execution history.
    std::string integrity;
    std::string nextOrdinal = "0";
    DfsStage stage = DfsStage::partition;
    std::vector<std::size_t> partition;
    // One frame per selected requirement, plus the current choice frame.
    std::vector<std::size_t> partitionNext{0};
    bool partitionLeafProcessed = false;
    std::vector<std::size_t> topology;
    std::vector<std::size_t> topologyNext;
    bool topologyProduced = false;
    // Keys carry the unit and reason; values are arbitrary-precision decimal
    // counts of rejected prefixes/partitions, never counts of complete cases.
    std::map<std::string, std::string> pruneCounts;
};
inline std::string CursorIntegrity(const DfsCursor& cursor)
{
    std::string encoded;
    AppendToken(encoded, cursor.version); AppendToken(encoded, cursor.binding); AppendToken(encoded, cursor.nextOrdinal);
    AppendToken(encoded, std::to_string(static_cast<unsigned>(cursor.stage)));
    AppendToken(encoded, cursor.partitionLeafProcessed ? "true" : "false");
    AppendToken(encoded, cursor.topologyProduced ? "true" : "false");
    const auto appendVector = [&](const std::vector<std::size_t>& values) {
        AppendToken(encoded, std::to_string(values.size()));
        for (const auto value : values) AppendToken(encoded, std::to_string(value));
    };
    appendVector(cursor.partition); appendVector(cursor.partitionNext);
    appendVector(cursor.topology); appendVector(cursor.topologyNext);
    AppendToken(encoded, std::to_string(cursor.pruneCounts.size()));
    for (const auto& [key, value] : cursor.pruneCounts) { AppendToken(encoded, key); AppendToken(encoded, value); }
    std::uint64_t hash = 14695981039346656037ull;
    for (const char character : encoded) { hash ^= static_cast<unsigned char>(character); hash *= 1099511628211ull; }
    return std::to_string(hash);
}
struct DfsPrune
{
    // Scope is a partition-prefix branch or one complete partition, never an
    // inferred number of complete topological cases underneath that branch.
    std::string scope;
    std::string reason;
    std::vector<std::size_t> partition;
    std::string earlierRequirement;
    std::string currentRequirement;
};
struct DfsPage
{
    std::vector<Variant> variants;
    std::vector<DfsPrune> pruned;
    DfsCursor cursor;
    bool exhausted = false;
    bool paused = false;
    std::size_t workUsed = 0;
    std::string structuralExclusion;
    std::vector<std::string> errors;
};

// State is resumable without revisiting any previous partition or topology
// choice. A work unit is one DFS choice, backtrack, graph initialization or
// emitted leaf. Each such unit may scan this finite Spec's requirements or
// edges; schema/spec validation is a finite, non-combinatorial preamble.
// Hitting either limit returns the advanced cursor without guessing whether
// more variants exist. Only an actually exhausted DFS sets exhausted=true.
inline DfsPage EnumerateDfs(const Context& context, const Candidate& input, const Spec& supplied,
    DfsCursor cursor, std::size_t workBudget, std::size_t outputLimit)
{
    DfsPage result;
    result.cursor = std::move(cursor);
    if (!outputLimit) { result.errors.emplace_back("output_limit_must_be_positive"); return result; }
    PreparedGraph graph;
    const auto preparation = EnumerateInternal(context, input, supplied, "0", 1, {}, &graph);
    result.errors = preparation.errors;
    result.structuralExclusion = preparation.structuralExclusion;
    if (!result.errors.empty() || !result.structuralExclusion.empty()) return result;
    return SharedGraphKernel::EnumerateDfsCore(graph, std::move(result.cursor), workBudget, outputLimit);
}

inline DfsPage EnumerateDfs(const V3GraphKernelContext& context, const Candidate& input, const Spec& supplied,
    DfsCursor cursor, std::size_t workBudget, std::size_t outputLimit)
{
    DfsPage result;
    result.cursor = std::move(cursor);
    if (!outputLimit) { result.errors.emplace_back("output_limit_must_be_positive"); return result; }
    PreparedGraph graph;
    const auto preparation = EnumerateInternal(context, input, supplied, "0", 1, {}, &graph);
    result.errors = preparation.errors;
    result.structuralExclusion = preparation.structuralExclusion;
    if (!result.errors.empty() || !result.structuralExclusion.empty()) return result;
    return SharedGraphKernel::EnumerateDfsCore(graph, std::move(result.cursor), workBudget, outputLimit);
}

inline DfsPage EnumerateDfs(const V4GraphKernelContext& context, const Candidate& input, const Spec& supplied,
    DfsCursor cursor, std::size_t workBudget, std::size_t outputLimit)
{
    DfsPage result;
    result.cursor = std::move(cursor);
    if (!outputLimit) { result.errors.emplace_back("output_limit_must_be_positive"); return result; }
    PreparedGraph graph;
    const auto preparation = EnumerateInternal(context, input, supplied, "0", 1, {}, &graph);
    result.errors = preparation.errors; result.structuralExclusion = preparation.structuralExclusion;
    if (!result.errors.empty() || !result.structuralExclusion.empty()) return result;
    return SharedGraphKernel::EnumerateDfsCore(graph, std::move(result.cursor), workBudget, outputLimit);
}

inline DfsPage SharedGraphKernel::EnumerateDfsCore(const PreparedGraph& graph, DfsCursor cursor,
    std::size_t workBudget, std::size_t outputLimit)
{
    DfsPage result;
    result.cursor = std::move(cursor);
    if (!outputLimit) { result.errors.emplace_back("output_limit_must_be_positive"); return result; }
    auto& state = result.cursor;
    if (state.version != "event_graph_dfs_v2")
    {
        result.errors.emplace_back("cursor_version_invalid");
        return result;
    }
    try
    {
        if (Normalize(state.nextOrdinal) != state.nextOrdinal) result.errors.emplace_back("cursor_ordinal_not_canonical");
    }
    catch (const std::exception&) { result.errors.emplace_back("cursor_ordinal_invalid"); }
    if (state.binding.empty())
    {
        if (state.stage != DfsStage::partition || state.nextOrdinal != "0" || !state.partition.empty() ||
            state.partitionNext != std::vector<std::size_t>{0} || state.partitionLeafProcessed ||
            !state.topology.empty() || !state.topologyNext.empty() || state.topologyProduced ||
            !state.pruneCounts.empty() || !state.integrity.empty()) result.errors.emplace_back("cursor_initial_state_invalid");
        else state.binding = graph.binding;
    }
    else if (state.binding != graph.binding) result.errors.emplace_back("cursor_binding_mismatch");
    else if (state.integrity.empty() || state.integrity != CursorIntegrity(state)) result.errors.emplace_back("cursor_integrity_invalid");
    for (const auto& [key, value] : state.pruneCounts)
    {
        const bool recognized = key == "partition_prefix:pair_requires_same" || key == "partition_prefix:pair_requires_distinct" ||
            key == "partition_prefix:known_property_mismatch" || key == "complete_partition:strict_edge_same_event" ||
            key == "complete_partition:strict_causal_cycle";
        try
        {
            if (!recognized || Normalize(value) != value || value == "0") result.errors.emplace_back("cursor_prune_count_invalid");
        }
        catch (const std::exception&) { result.errors.emplace_back("cursor_prune_count_invalid"); }
    }
    if (!result.errors.empty()) return result;

    const auto countClasses = [](const std::vector<std::size_t>& partition) {
        std::size_t count = 0;
        for (const auto value : partition) if (value >= count) count = value + 1;
        return count;
    };
    const auto relation = [&](std::size_t a, std::size_t b) {
        if (DistinctSessionGroups(graph.requirements[a], graph.requirements[b])) return PairRelation::distinct;
        const auto found = graph.rules.find({(std::min)(a, b), (std::max)(a, b)});
        return found == graph.rules.end() ? PairRelation::unresolved : found->second;
    };
    const auto mismatch = [](const Requirement& a, const Requirement& b) {
        return a.propertiesResolved && b.propertiesResolved &&
            (a.kind != b.kind || a.session != b.session || a.task != b.task || a.object != b.object ||
             a.parameter != b.parameter || (a.role == b.role && a.occurrence != b.occurrence));
    };
    const auto graphForPartition = [&](const std::vector<std::size_t>& partition,
                                      std::vector<std::set<std::size_t>>& outgoing) {
        outgoing.assign(countClasses(partition), {});
        for (const auto& [a, b] : graph.edges)
        {
            const auto from = partition[a], to = partition[b];
            if (from == to) return false;
            outgoing[from].insert(to);
        }
        return true;
    };
    const auto validate = [&]() {
        const auto n = graph.requirements.size();
        if (state.stage == DfsStage::done)
            return state.partition.empty() && state.partitionNext.empty() && !state.partitionLeafProcessed &&
                state.topology.empty() && state.topologyNext.empty() && !state.topologyProduced;
        if (state.stage != DfsStage::partition && state.stage != DfsStage::topology) return false;
        if (state.partition.size() > n || state.partitionNext.size() != state.partition.size() + 1) return false;
        std::size_t classes = 0;
        for (std::size_t i = 0; i < state.partition.size(); ++i)
        {
            const auto choice = state.partition[i];
            if (choice > classes || state.partitionNext[i] != choice + 1) return false;
            for (std::size_t j = 0; j < i; ++j)
            {
                const bool same = state.partition[j] == choice;
                const auto rule = relation(j, i);
                if ((rule == PairRelation::same && !same) || (rule == PairRelation::distinct && same) ||
                    (same && mismatch(graph.requirements[j], graph.requirements[i]))) return false;
            }
            if (choice == classes) ++classes;
        }
        if (state.partitionNext.back() > classes + 1) return false;
        if (state.stage == DfsStage::partition)
            return state.topology.empty() && state.topologyNext.empty() && !state.topologyProduced &&
                (!state.partitionLeafProcessed || state.partition.size() == n);
        if (state.partition.size() != n || state.partitionLeafProcessed ||
            state.topology.size() > classes || state.topologyNext.size() != state.topology.size() + 1 ||
            state.topologyNext.back() > classes) return false;
        std::vector<std::set<std::size_t>> outgoing;
        if (!graphForPartition(state.partition, outgoing)) return false;
        std::vector<std::size_t> indegree(classes, 0);
        for (const auto& next : outgoing) for (const auto node : next) ++indegree[node];
        std::vector<bool> used(classes, false);
        for (std::size_t i = 0; i < state.topology.size(); ++i)
        {
            const auto node = state.topology[i];
            if (node >= classes || used[node] || indegree[node] || state.topologyNext[i] != node + 1) return false;
            used[node] = true;
            for (const auto next : outgoing[node]) --indegree[next];
        }
        return true;
    };
    if (!validate()) { result.errors.emplace_back("cursor_state_invalid"); return result; }
    if (state.stage == DfsStage::done) { result.exhausted = true; return result; }

    const auto recordPrune = [&](std::string_view scope, std::string_view reason,
        std::vector<std::size_t> partition, std::string_view earlier, std::string_view current) {
        DfsPrune event{std::string(scope), std::string(reason), std::move(partition), std::string(earlier), std::string(current)};
        auto& count = state.pruneCounts[event.scope + ":" + event.reason];
        if (count.empty()) count = "0";
        Increment(count);
        result.pruned.push_back(std::move(event));
    };

    while (result.workUsed < workBudget && result.variants.size() < outputLimit && state.stage != DfsStage::done)
    {
        ++result.workUsed;
        const auto n = graph.requirements.size();
        if (state.stage == DfsStage::partition)
        {
            if (state.partition.size() == n)
            {
                if (state.partitionLeafProcessed)
                {
                    state.partitionLeafProcessed = false;
                    if (state.partition.empty()) { state.partitionNext.clear(); state.stage = DfsStage::done; }
                    else { state.partition.pop_back(); state.partitionNext.pop_back(); }
                    continue;
                }
                std::vector<std::set<std::size_t>> outgoing;
                if (!graphForPartition(state.partition, outgoing))
                {
                    for (const auto& [a, b] : graph.edges)
                        if (state.partition[a] == state.partition[b])
                        {
                            recordPrune("complete_partition", "strict_edge_same_event", state.partition,
                                graph.requirements[a].id, graph.requirements[b].id);
                            break;
                        }
                    state.partitionLeafProcessed = true;
                    continue;
                }
                state.stage = DfsStage::topology;
                state.topology.clear(); state.topologyNext = {0}; state.topologyProduced = false;
                continue;
            }
            const auto classes = countClasses(state.partition);
            auto& next = state.partitionNext.back();
            if (next > classes)
            {
                if (state.partition.empty()) { state.partitionNext.clear(); state.stage = DfsStage::done; }
                else { state.partition.pop_back(); state.partitionNext.pop_back(); }
                continue;
            }
            const auto choice = next++;
            bool allowed = true;
            std::string_view rejected;
            std::size_t conflicting = 0;
            for (std::size_t i = 0; i < state.partition.size() && allowed; ++i)
            {
                const bool same = state.partition[i] == choice;
                const auto rule = relation(i, state.partition.size());
                if (rule == PairRelation::same && !same) rejected = "pair_requires_same";
                else if (rule == PairRelation::distinct && same) rejected = "pair_requires_distinct";
                else if (same && mismatch(graph.requirements[i], graph.requirements[state.partition.size()])) rejected = "known_property_mismatch";
                if (!rejected.empty()) { allowed = false; conflicting = i; }
            }
            if (allowed) { state.partition.push_back(choice); state.partitionNext.push_back(0); }
            else
            {
                auto rejectedPrefix = state.partition;
                rejectedPrefix.push_back(choice);
                recordPrune("partition_prefix", rejected, std::move(rejectedPrefix),
                    graph.requirements[conflicting].id, graph.requirements[state.partition.size()].id);
            }
            continue;
        }

        const auto classes = countClasses(state.partition);
        if (state.topology.size() == classes)
        {
            Variant item;
            item.ordinal = state.nextOrdinal; item.classes = state.partition; item.unresolved = graph.unresolved;
            item.unresolved.emplace_back("candidate_event_mapping_unimplemented");
            item.unresolved.emplace_back("production_event_execution_and_completion_unimplemented");
            if (!graph.causalCoverageComplete) item.unresolved.emplace_back("causal_relation_coverage_unresolved");
            std::vector<std::string> representatives(classes);
            for (std::size_t i = 0; i < n; ++i)
            {
                if (representatives[state.partition[i]].empty()) representatives[state.partition[i]] = graph.requirements[i].id;
                if (!graph.requirements[i].propertiesResolved) item.unresolved.push_back("requirement_properties_unresolved:" + graph.requirements[i].id);
                for (std::size_t j = 0; j < i; ++j)
                    if (relation(j, i) == PairRelation::unresolved)
                        item.unresolved.push_back("pair_relation_unresolved:" + graph.requirements[j].id + ":" + graph.requirements[i].id);
            }
            item.id = graph.binding;
            AppendToken(item.id, std::to_string(state.partition.size()));
            for (const auto value : state.partition) AppendToken(item.id, std::to_string(value));
            AppendToken(item.id, std::to_string(state.topology.size()));
            for (const auto node : state.topology)
            {
                item.order.push_back(representatives[node]);
                AppendToken(item.id, representatives[node]);
            }
            Increment(state.nextOrdinal);
            state.topologyProduced = true;
            if (state.topology.empty())
            { state.topologyNext.clear(); state.stage = DfsStage::partition; state.partitionLeafProcessed = true; state.topologyProduced = false; }
            else { state.topology.pop_back(); state.topologyNext.pop_back(); }
            result.variants.push_back(std::move(item));
            continue;
        }
        auto& next = state.topologyNext.back();
        if (next >= classes)
        {
            if (state.topology.empty())
            {
                if (!state.topologyProduced)
                    recordPrune("complete_partition", "strict_causal_cycle", state.partition, "", "");
                state.topologyNext.clear(); state.stage = DfsStage::partition; state.partitionLeafProcessed = true; state.topologyProduced = false;
            }
            else { state.topology.pop_back(); state.topologyNext.pop_back(); }
            continue;
        }
        const auto node = next++;
        std::vector<std::set<std::size_t>> outgoing;
        if (!graphForPartition(state.partition, outgoing)) { result.errors.emplace_back("cursor_graph_invalid"); return result; }
        std::vector<std::size_t> indegree(classes, 0);
        for (const auto& successors : outgoing) for (const auto successor : successors) ++indegree[successor];
        std::vector<bool> used(classes, false);
        for (const auto selected : state.topology)
        {
            used[selected] = true;
            for (const auto successor : outgoing[selected]) --indegree[successor];
        }
        if (!used[node] && !indegree[node]) { state.topology.push_back(node); state.topologyNext.push_back(0); }
    }
    result.exhausted = state.stage == DfsStage::done;
    result.paused = !result.exhausted;
    state.integrity = CursorIntegrity(state);
    return result;
}

struct VariantReplay
{
    bool identityValid = false;
    Variant variant;
    std::string structuralExclusion;
    std::vector<std::string> errors;
};

// Parse canonical decimal size_t fields without wrapping, accepting neither
// signs nor leading zeros. The ID is untrusted input, not an execution token.
inline bool ParseCanonicalSize(std::string_view value, std::size_t& result)
{
    if (value.empty() || (value.size() > 1 && value.front() == '0')) return false;
    result = 0;
    constexpr auto maximum = (std::numeric_limits<std::size_t>::max)();
    for (const char character : value)
    {
        if (character < '0' || character > '9') return false;
        const auto digit = static_cast<std::size_t>(character - '0');
        if (result > (maximum - digit) / 10) return false;
        result = result * 10 + digit;
    }
    return true;
}

inline bool ReadGraphToken(std::string_view& remaining, std::string_view& value)
{
    const auto colon = remaining.find(':');
    if (colon == std::string_view::npos) return false;
    std::size_t length = 0;
    if (!ParseCanonicalSize(remaining.substr(0, colon), length)) return false;
    remaining.remove_prefix(colon + 1);
    if (length > remaining.size()) return false;
    value = remaining.substr(0, length);
    remaining.remove_prefix(length);
    return true;
}

// This cursor enumerates presence propositions only. It is not a graph-case
// cursor and cannot be submitted to the legacy graph or production executor.
// FNV detects accidental damage only, not forgery or prior-page completeness.
struct PresenceCursor
{
    std::string version = "presence_hypothesis_dfs_v1";
    std::string binding, integrity;
    std::string nextOrdinal = "0";
    bool exhausted = false;
    std::vector<std::size_t> prefix;
    std::vector<std::size_t> next{0};
    // Counts rejected prefix attempts, not complete graph variants or cases.
    std::map<std::string, std::string> pruneCounts;
};
inline std::string PresenceCursorIntegrity(const PresenceCursor& state)
{
    std::string encoded;
    AppendToken(encoded, state.version); AppendToken(encoded, state.binding); AppendToken(encoded, state.nextOrdinal);
    AppendToken(encoded, state.exhausted ? "true" : "false");
    for (const auto* values : {&state.prefix, &state.next})
    {
        AppendToken(encoded, std::to_string(values->size()));
        for (const auto value : *values) AppendToken(encoded, std::to_string(value));
    }
    AppendToken(encoded, std::to_string(state.pruneCounts.size()));
    for (const auto& [key, value] : state.pruneCounts) { AppendToken(encoded, key); AppendToken(encoded, value); }
    std::uint64_t hash = 14695981039346656037ull;
    for (const char c : encoded) { hash ^= static_cast<unsigned char>(c); hash *= 1099511628211ull; }
    return std::to_string(hash);
}
struct PresencePrefixPrune
{
    std::vector<std::size_t> prefix;
    std::string reason, premise, consequence;
};
struct PresencePageItem
{
    std::string ordinal;
    std::vector<bool> present;
    PresenceHypothesis hypothesis;
    // The bit vector and binding are validated, but relation diagnostics
    // must be generated by a budgeted projection before use as an active graph.
    bool unresolvedDeferred = false;
};
struct PresencePage
{
    // This page and its returned cursor form one in-memory publication unit.
    // Durable writers must commit both together to avoid lost/repeated pages.
    std::string startOrdinal;
    std::vector<PresencePageItem> hypotheses;
    std::vector<PresencePrefixPrune> pruned;
    PresenceCursor cursor;
    bool exhausted = false, paused = false;
    std::size_t workUsed = 0;
    std::string structuralExclusion;
    std::vector<std::string> errors;
};

template <typename GraphContext>
inline PresencePage EnumeratePresenceHypotheses(const GraphContext& context, const Candidate& candidate,
    const Spec& supplied, PresenceCursor cursor, std::size_t workBudget, std::size_t outputLimit,
    bool deferUnresolved = false)
{
    PresencePage page;
    page.cursor = std::move(cursor);
    if (!outputLimit) { page.errors.emplace_back("presence_output_limit_must_be_positive"); return page; }
    const auto schema = PreparePresenceGraph(context, candidate, supplied);
    page.errors = schema.errors;
    page.structuralExclusion = schema.structuralExclusion;
    if (!page.errors.empty() || !page.structuralExclusion.empty()) return page;
    auto& state = page.cursor;
    if (state.version != "presence_hypothesis_dfs_v1")
    { page.errors.emplace_back("presence_cursor_version_invalid"); return page; }
    try
    {
        if (Normalize(state.nextOrdinal) != state.nextOrdinal) page.errors.emplace_back("presence_cursor_ordinal_not_canonical");
    }
    catch (const std::exception&) { page.errors.emplace_back("presence_cursor_ordinal_invalid"); }
    if (state.binding.empty())
    {
        if (state.nextOrdinal != "0" || state.exhausted || !state.prefix.empty() ||
            state.next != std::vector<std::size_t>{0} || !state.pruneCounts.empty() || !state.integrity.empty())
            page.errors.emplace_back("presence_cursor_initial_state_invalid");
        else state.binding = schema.binding;
    }
    else if (state.binding != schema.binding) page.errors.emplace_back("presence_cursor_binding_mismatch");
    else if (state.integrity.empty() || state.integrity != PresenceCursorIntegrity(state))
        page.errors.emplace_back("presence_cursor_integrity_invalid");
    for (const auto& [key, value] : state.pruneCounts)
    {
        try
        {
            if ((key != "prefix:present_implies_present" && key != "prefix:present_excludes_present") ||
                Normalize(value) != value || value == "0")
                page.errors.emplace_back("presence_cursor_prune_count_invalid");
        }
        catch (const std::exception&) { page.errors.emplace_back("presence_cursor_prune_count_invalid"); }
    }
    if (!page.errors.empty()) return page;
    const auto n = schema.requirements.size();
    std::map<std::string, std::size_t> indices;
    for (std::size_t i = 0; i < n; ++i) indices.emplace(schema.requirements[i].id, i);
    const auto conflict = [&](const std::vector<std::size_t>& prefix) -> const PresenceConstraint* {
        for (const auto& rule : schema.constraints)
        {
            const auto premise = indices.at(rule.premise), consequence = indices.at(rule.consequence);
            if (premise < prefix.size() && consequence < prefix.size() && prefix[premise] == 1 &&
                ((rule.relation == PresenceRelation::present_implies_present && prefix[consequence] == 0) ||
                 (rule.relation == PresenceRelation::present_excludes_present && prefix[consequence] == 1)))
                return &rule;
        }
        return nullptr;
    };
    const auto validState = [&] {
        if (state.exhausted) return state.prefix.empty() && state.next.empty();
        if (state.prefix.size() > n || state.next.size() != state.prefix.size() + 1) return false;
        for (std::size_t i = 0; i < state.prefix.size(); ++i)
        {
            const bool required = schema.requirements[i].presence == PresencePolicy::required;
            if (state.prefix[i] > 1 || (required && state.prefix[i] != 1)) return false;
            const std::size_t chosenIndex = required ? 0 : state.prefix[i];
            if (state.next[i] != chosenIndex + 1) return false;
        }
        const auto choices = state.prefix.size() == n ? std::size_t{0} :
            (schema.requirements[state.prefix.size()].presence == PresencePolicy::required ? std::size_t{1} : std::size_t{2});
        return state.next.back() <= choices && !conflict(state.prefix);
    };
    if (!validState()) { page.errors.emplace_back("presence_cursor_state_invalid"); return page; }
    page.startOrdinal = state.nextOrdinal;
    if (state.exhausted) { page.exhausted = true; return page; }

    while (page.workUsed < workBudget && page.hypotheses.size() < outputLimit && !state.exhausted)
    {
        ++page.workUsed;
        if (state.prefix.size() == n)
        {
            PresencePageItem item;
            item.ordinal = state.nextOrdinal;
            for (const auto bit : state.prefix) item.present.push_back(bit != 0);
            // Rebuild from the supplied schema and candidate; no publicly
            // mutable preparation object can be paired with a stale binding.
            if (deferUnresolved)
            {
                item.unresolvedDeferred = true;
                AppendToken(item.hypothesis.binding, "global_drag_presence_hypothesis_v1");
                AppendToken(item.hypothesis.binding, schema.binding);
                AppendToken(item.hypothesis.binding, std::to_string(item.present.size()));
                for (const bool bit : item.present) AppendToken(item.hypothesis.binding, bit ? "present" : "absent");
                item.hypothesis.unresolved.emplace_back("presence_relation_diagnostics_deferred");
                item.hypothesis.structurallyValid = true;
            }
            else item.hypothesis = InspectPresenceHypothesis(context, candidate, supplied, item.present);
            if (!item.hypothesis.structurallyValid || !item.hypothesis.errors.empty())
            {
                page.hypotheses.clear(); page.pruned.clear();
                page.errors.emplace_back("presence_internal_leaf_invalid"); return page;
            }
            Increment(state.nextOrdinal);
            // Advance before handing out the leaf, including the empty graph.
            if (state.prefix.empty()) { state.next.clear(); state.exhausted = true; }
            else { state.prefix.pop_back(); state.next.pop_back(); }
            page.hypotheses.push_back(std::move(item));
            continue;
        }
        const bool required = schema.requirements[state.prefix.size()].presence == PresencePolicy::required;
        const std::size_t choices = required ? 1 : 2;
        auto& next = state.next.back();
        if (next >= choices)
        {
            if (state.prefix.empty()) { state.next.clear(); state.exhausted = true; }
            else { state.prefix.pop_back(); state.next.pop_back(); }
            continue;
        }
        const auto choice = next++;
        auto prefix = state.prefix;
        prefix.push_back(required ? 1 : choice);
        if (const auto* rule = conflict(prefix))
        {
            const auto reason = PresenceRelationName(rule->relation);
            auto& count = state.pruneCounts["prefix:" + std::string(reason)];
            if (count.empty()) count = "0";
            Increment(count);
            page.pruned.push_back({std::move(prefix), std::string(reason), rule->premise, rule->consequence});
        }
        else { state.prefix = std::move(prefix); state.next.push_back(0); }
    }
    page.exhausted = state.exhausted;
    page.paused = !state.exhausted;
    state.integrity = PresenceCursorIntegrity(state);
    return page;
}

struct PresenceCursorCodecResult
{
    PresenceCursor cursor;
    std::string encoded, structuralExclusion;
    std::vector<std::string> errors;
};
template <typename GraphContext>
inline PresenceCursorCodecResult SerializePresenceCursor(const GraphContext& context, const Candidate& candidate,
    const Spec& supplied, PresenceCursor cursor)
{
    PresenceCursorCodecResult result;
    const auto checked = EnumeratePresenceHypotheses(context, candidate, supplied, std::move(cursor), 0, 1);
    result.cursor = checked.cursor;
    result.errors = checked.errors;
    result.structuralExclusion = checked.structuralExclusion;
    if (!result.errors.empty() || !result.structuralExclusion.empty()) return result;
    const auto& state = result.cursor;
    auto& out = result.encoded;
    AppendToken(out, "presence_cursor_encoding_v1");
    AppendToken(out, state.version); AppendToken(out, state.binding); AppendToken(out, state.integrity);
    AppendToken(out, state.nextOrdinal); AppendToken(out, state.exhausted ? "true" : "false");
    for (const auto* values : {&state.prefix, &state.next})
    {
        AppendToken(out, std::to_string(values->size()));
        for (const auto value : *values) AppendToken(out, std::to_string(value));
    }
    AppendToken(out, std::to_string(state.pruneCounts.size()));
    for (const auto& [key, value] : state.pruneCounts) { AppendToken(out, key); AppendToken(out, value); }
    return result;
}
template <typename GraphContext>
inline PresenceCursorCodecResult ParsePresenceCursor(const GraphContext& context, const Candidate& candidate,
    const Spec& supplied, std::string_view encoded)
{
    PresenceCursorCodecResult result;
    auto remaining = encoded;
    std::string_view token;
    const auto read = [&]() { return ReadGraphToken(remaining, token); };
    const auto fail = [&]() { result.errors.emplace_back("presence_cursor_encoding_invalid"); return result; };
    if (!read() || token != "presence_cursor_encoding_v1") return fail();
    auto& state = result.cursor;
    if (!read()) return fail();
    state.version = std::string(token);
    if (!read()) return fail();
    state.binding = std::string(token);
    if (!read()) return fail();
    state.integrity = std::string(token);
    if (!read()) return fail();
    state.nextOrdinal = std::string(token);
    if (!read() || (token != "true" && token != "false")) return fail();
    state.exhausted = token == "true";
    const auto vector = [&](std::vector<std::size_t>& values) {
        std::size_t count = 0;
        if (!read() || !ParseCanonicalSize(token, count) || count > remaining.size() / 2) return false;
        values.clear(); values.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            std::size_t value = 0;
            if (!read() || !ParseCanonicalSize(token, value)) return false;
            values.push_back(value);
        }
        return true;
    };
    if (!vector(state.prefix) || !vector(state.next)) return fail();
    std::size_t count = 0;
    if (!read() || !ParseCanonicalSize(token, count) || count > remaining.size() / 4) return fail();
    state.pruneCounts.clear();
    for (std::size_t i = 0; i < count; ++i)
    {
        if (!read()) return fail();
        const std::string key(token);
        if (!read() || !state.pruneCounts.emplace(key, std::string(token)).second) return fail();
    }
    if (!remaining.empty()) return fail();
    auto checked = SerializePresenceCursor(context, candidate, supplied, std::move(state));
    if (!checked.errors.empty() || !checked.structuralExclusion.empty()) return checked;
    if (checked.encoded != encoded)
    { checked.errors.emplace_back("presence_cursor_encoding_noncanonical"); checked.encoded.clear(); }
    return checked;
}

struct DfsCursorCodecResult
{
    DfsCursor cursor;
    std::string encoded;
    std::string structuralExclusion;
    std::vector<std::string> errors;
};

inline void EncodeDfsCursorState(std::string& out, const DfsCursor& state, std::string_view encoding)
{
    AppendToken(out, encoding);
    AppendToken(out, state.version); AppendToken(out, state.binding); AppendToken(out, state.integrity);
    AppendToken(out, state.nextOrdinal); AppendToken(out, std::to_string(static_cast<unsigned>(state.stage)));
    AppendToken(out, state.partitionLeafProcessed ? "true" : "false");
    AppendToken(out, state.topologyProduced ? "true" : "false");
    const auto appendVector = [&](const std::vector<std::size_t>& values) {
        AppendToken(out, std::to_string(values.size()));
        for (const auto value : values) AppendToken(out, std::to_string(value));
    };
    appendVector(state.partition); appendVector(state.partitionNext);
    appendVector(state.topology); appendVector(state.topologyNext);
    AppendToken(out, std::to_string(state.pruneCounts.size()));
    for (const auto& [key, value] : state.pruneCounts) { AppendToken(out, key); AppendToken(out, value); }
}

// A cursor includes the entire DFS stack and cumulative decimal counters.
// Revalidate it against the same complete schema, Candidate and Spec before
// persistence; the integrity checksum detects accidents, not hostile edits.
// Binding an initial cursor at zero budget performs no enumeration.
template <typename GraphContext>
inline DfsCursorCodecResult SerializeDfsCursor(const GraphContext& context, const Candidate& candidate,
    const Spec& spec, DfsCursor cursor)
{
    DfsCursorCodecResult result;
    const auto checked = EnumerateDfs(context, candidate, spec, std::move(cursor), 0, 1);
    result.cursor = checked.cursor;
    result.structuralExclusion = checked.structuralExclusion;
    result.errors = checked.errors;
    if (!result.errors.empty() || !result.structuralExclusion.empty()) return result;
    EncodeDfsCursorState(result.encoded, result.cursor, "event_graph_cursor_encoding_v1");
    return result;
}

// Untrusted persisted bytes are length-checked, canonicalized and then passed
// through the same graph binding, checksum and DFS-state validation as resume.
// Never restart at zero after an error, or infer execution from a valid cursor.
inline DfsCursorCodecResult DecodeDfsCursorState(std::string_view encoded, std::string_view encoding)
{
    DfsCursorCodecResult result;
    auto remaining = encoded;
    std::string_view token;
    const auto read = [&]() { return ReadGraphToken(remaining, token); };
    const auto fail = [&]() { result.errors.emplace_back("cursor_encoding_invalid"); return result; };
    if (!read() || token != encoding) return fail();
    auto& state = result.cursor;
    if (!read()) return fail();
    state.version = std::string(token);
    if (!read()) return fail();
    state.binding = std::string(token);
    if (!read()) return fail();
    state.integrity = std::string(token);
    if (!read()) return fail();
    state.nextOrdinal = std::string(token);
    std::size_t stage = 0;
    if (!read() || !ParseCanonicalSize(token, stage) || stage > static_cast<std::size_t>(DfsStage::done)) return fail();
    state.stage = static_cast<DfsStage>(stage);
    const auto boolean = [&](bool& value) {
        if (!read() || (token != "true" && token != "false")) return false;
        value = token == "true"; return true;
    };
    if (!boolean(state.partitionLeafProcessed) || !boolean(state.topologyProduced)) return fail();
    const auto vector = [&](std::vector<std::size_t>& values) {
        std::size_t count = 0;
        if (!read() || !ParseCanonicalSize(token, count) || count > remaining.size() / 2) return false;
        values.clear(); values.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            std::size_t value = 0;
            if (!read() || !ParseCanonicalSize(token, value)) return false;
            values.push_back(value);
        }
        return true;
    };
    if (!vector(state.partition) || !vector(state.partitionNext) || !vector(state.topology) || !vector(state.topologyNext)) return fail();
    std::size_t count = 0;
    if (!read() || !ParseCanonicalSize(token, count) || count > remaining.size() / 4) return fail();
    state.pruneCounts.clear();
    for (std::size_t i = 0; i < count; ++i)
    {
        if (!read()) return fail();
        const std::string key(token);
        if (!read() || !state.pruneCounts.emplace(key, std::string(token)).second) return fail();
    }
    if (!remaining.empty()) return fail();
    return result;
}

template <typename GraphContext>
inline DfsCursorCodecResult ParseDfsCursor(const GraphContext& context, const Candidate& candidate,
    const Spec& spec, std::string_view encoded)
{
    auto decoded = DecodeDfsCursorState(encoded, "event_graph_cursor_encoding_v1");
    if (!decoded.errors.empty()) return decoded;
    auto checked = SerializeDfsCursor(context, candidate, spec, std::move(decoded.cursor));
    if (!checked.errors.empty() || !checked.structuralExclusion.empty()) return checked;
    if (checked.encoded != encoded)
    { checked.errors.emplace_back("cursor_encoding_noncanonical"); checked.encoded.clear(); }
    return checked;
}

// The complete ID includes the exact schema, Candidate and normalized Spec.
// This verifies a selected partition/order directly, without enumerating or
// claiming any preceding graph. It reconstructs obligations from the trusted
// input, never from a caller-supplied Variant or execution flag.
inline VariantReplay ReplayVariant(const Context& context, const Candidate& candidate,
    const Spec& supplied, std::string_view id)
{
    VariantReplay result;
    PreparedGraph graph;
    const auto preparation = EnumerateInternal(context, candidate, supplied, "0", 1, {}, &graph);
    result.errors = preparation.errors;
    result.structuralExclusion = preparation.structuralExclusion;
    if (!result.errors.empty() || !result.structuralExclusion.empty()) return result;
    return SharedGraphKernel::ReplayCore(graph, id);
}

inline VariantReplay ReplayVariant(const V3GraphKernelContext& context, const Candidate& candidate,
    const Spec& supplied, std::string_view id)
{
    VariantReplay result;
    PreparedGraph graph;
    const auto preparation = EnumerateInternal(context, candidate, supplied, "0", 1, {}, &graph);
    result.errors = preparation.errors;
    result.structuralExclusion = preparation.structuralExclusion;
    if (!result.errors.empty() || !result.structuralExclusion.empty()) return result;
    return SharedGraphKernel::ReplayCore(graph, id);
}

inline VariantReplay ReplayVariant(const V4GraphKernelContext& context, const Candidate& candidate,
    const Spec& supplied, std::string_view id)
{
    VariantReplay result;
    PreparedGraph graph;
    const auto preparation = EnumerateInternal(context, candidate, supplied, "0", 1, {}, &graph);
    result.errors = preparation.errors; result.structuralExclusion = preparation.structuralExclusion;
    if (!result.errors.empty() || !result.structuralExclusion.empty()) return result;
    return SharedGraphKernel::ReplayCore(graph, id);
}

inline VariantReplay SharedGraphKernel::ReplayCore(const PreparedGraph& graph, std::string_view id)
{
    VariantReplay result;
    if (id.size() < graph.binding.size() || id.substr(0, graph.binding.size()) != graph.binding)
    { result.errors.emplace_back("variant_binding_mismatch"); return result; }
    auto remaining = id.substr(graph.binding.size());
    std::string_view token;
    std::size_t count = 0;
    if (!ReadGraphToken(remaining, token) || !ParseCanonicalSize(token, count) || count != graph.requirements.size())
    { result.errors.emplace_back("variant_partition_count_invalid"); return result; }
    auto& variant = result.variant;
    variant.id = std::string(id);
    variant.classes.reserve(count);
    std::size_t classes = 0;
    for (std::size_t i = 0; i < count; ++i)
    {
        std::size_t choice = 0;
        if (!ReadGraphToken(remaining, token) || !ParseCanonicalSize(token, choice) || choice > classes)
        { result.errors.emplace_back("variant_partition_invalid"); return result; }
        for (std::size_t j = 0; j < i; ++j)
        {
            const auto found = graph.rules.find({j, i});
            const auto relation = DistinctSessionGroups(graph.requirements[j], graph.requirements[i]) ? PairRelation::distinct :
                found == graph.rules.end() ? PairRelation::unresolved : found->second;
            const bool same = variant.classes[j] == choice;
            const auto& left = graph.requirements[j];
            const auto& right = graph.requirements[i];
            const bool mismatch = left.propertiesResolved && right.propertiesResolved &&
                (left.kind != right.kind || left.session != right.session || left.task != right.task ||
                 left.object != right.object || left.parameter != right.parameter ||
                 (left.role == right.role && left.occurrence != right.occurrence));
            if ((relation == PairRelation::same && !same) || (relation == PairRelation::distinct && same) || (same && mismatch))
            { result.errors.emplace_back("variant_partition_incompatible"); return result; }
        }
        variant.classes.push_back(choice);
        if (choice == classes) ++classes;
    }
    std::size_t orderCount = 0;
    if (!ReadGraphToken(remaining, token) || !ParseCanonicalSize(token, orderCount) || orderCount != classes)
    { result.errors.emplace_back("variant_order_count_invalid"); return result; }
    std::vector<std::string> representatives(classes);
    for (std::size_t i = 0; i < count; ++i)
        if (representatives[variant.classes[i]].empty()) representatives[variant.classes[i]] = graph.requirements[i].id;
    std::map<std::string, std::size_t> representativeIndex;
    for (std::size_t i = 0; i < classes; ++i) representativeIndex.emplace(representatives[i], i);
    std::vector<std::size_t> rank(classes, classes);
    for (std::size_t i = 0; i < classes; ++i)
    {
        if (!ReadGraphToken(remaining, token)) { result.errors.emplace_back("variant_order_token_invalid"); return result; }
        const auto found = representativeIndex.find(std::string(token));
        if (found == representativeIndex.end() || rank[found->second] != classes)
        { result.errors.emplace_back("variant_order_identity_invalid"); return result; }
        rank[found->second] = i;
        variant.order.emplace_back(token);
    }
    if (!remaining.empty()) { result.errors.emplace_back("variant_extra_data"); return result; }
    for (const auto& [earlier, later] : graph.edges)
        if (variant.classes[earlier] == variant.classes[later] ||
            rank[variant.classes[earlier]] >= rank[variant.classes[later]])
        { result.errors.emplace_back("variant_strict_causality_invalid"); return result; }

    variant.unresolved = graph.unresolved;
    variant.unresolved.emplace_back("candidate_event_mapping_unimplemented");
    variant.unresolved.emplace_back("production_event_execution_and_completion_unimplemented");
    if (!graph.causalCoverageComplete) variant.unresolved.emplace_back("causal_relation_coverage_unresolved");
    for (std::size_t i = 0; i < count; ++i)
    {
        if (!graph.requirements[i].propertiesResolved)
            variant.unresolved.push_back("requirement_properties_unresolved:" + graph.requirements[i].id);
        for (std::size_t j = 0; j < i; ++j)
        {
            const auto found = graph.rules.find({j, i});
            if (!DistinctSessionGroups(graph.requirements[j], graph.requirements[i]) &&
                (found == graph.rules.end() || found->second == PairRelation::unresolved))
                variant.unresolved.push_back("pair_relation_unresolved:" + graph.requirements[j].id + ":" + graph.requirements[i].id);
        }
    }
    // A stable ID does not prove a DFS ordinal or any execution history.
    variant.ordinal.clear();
    result.identityValid = true;
    return result;
}

// A V3 call-graph preview has its own type and ID. This restricted mapping
// describes only required OLE call/return and host-observation requests;
// conditional/background event presence, actual delivery and all product
// results remain unresolved. It cannot be converted into an old Variant.
struct V3GraphVariant
{
    std::string ordinal, id;
    std::vector<std::size_t> classes;
    std::vector<std::string> order, unresolved;
};
struct V3GraphCursor { DfsCursor inner; };
struct V3GraphPage
{
    std::vector<V3GraphVariant> variants;
    std::vector<DfsPrune> pruned;
    V3GraphCursor cursor;
    bool exhausted = false, paused = false;
    std::size_t workUsed = 0;
    std::vector<std::string> errors;
};
struct V3GraphReplay
{
    bool identityValid = false;
    V3GraphVariant variant;
    std::vector<std::string> errors;
};
struct V3GraphCursorCodec
{
    V3GraphCursor cursor;
    std::string encoded;
    std::vector<std::string> errors;
};
class V3Graph final
{
    static bool Prepare(const V3FamilyIdentityContext& identity, const V3FamilyValidatedContext& validated,
        const EventProgram& program, PreparedGraph& graph, std::vector<std::string>& errors)
    {
        const auto mapping = MapV3FamilyCandidate(identity, validated, program);
        if (!mapping.graphAvailable)
        {
            errors.push_back("v3_call_graph_unavailable:" + mapping.status);
            errors.insert(errors.end(), mapping.reasons.begin(), mapping.reasons.end());
            return false;
        }
        // Bind every Program field, including outcome hypotheses, without
        // converting postconditions into input or claiming them observed.
        std::string binding;
        AppendToken(binding, "v3_call_graph_candidate_program_binding_v1");
        AppendToken(binding, identity.SchemaBinding());
        AppendToken(binding, std::to_string(program.steps.size()));
        for (const auto& step : program.steps) { AppendToken(binding, step.kind); AppendToken(binding, step.parameter); }
        AppendToken(binding, std::to_string(program.unresolved.size()));
        for (const auto& reason : program.unresolved) AppendToken(binding, reason);
        AppendToken(binding, std::to_string(program.postconditions.size()));
        for (const auto& outcome : program.postconditions) { AppendToken(binding, outcome.axis); AppendToken(binding, outcome.value); }
        const auto prepared = SharedGraphKernel::Enumerate(identity.Axes(), validated.Coordinate(), binding,
            "global_drag_event_graph_v3_exact_family", mapping.spec, "0", 1, {}, &graph);
        errors.insert(errors.end(), prepared.errors.begin(), prepared.errors.end());
        if (!prepared.structuralExclusion.empty()) errors.push_back("v3_call_graph_structure_excluded:" + prepared.structuralExclusion);
        return errors.empty();
    }
    static V3GraphVariant Convert(const Variant& variant)
    { return {variant.ordinal, variant.id, variant.classes, variant.order, variant.unresolved}; }
public:
    static V3GraphPage Enumerate(const V3FamilyIdentityContext& identity, const V3FamilyValidatedContext& validated,
        const EventProgram& program, V3GraphCursor cursor, std::size_t workBudget, std::size_t outputLimit)
    {
        V3GraphPage result;
        result.cursor = std::move(cursor);
        try
        {
            PreparedGraph graph;
            if (!Prepare(identity, validated, program, graph, result.errors)) return result;
            auto page = SharedGraphKernel::EnumerateDfsCore(graph, result.cursor.inner, workBudget, outputLimit);
            if (!page.errors.empty()) { result.errors = std::move(page.errors); return result; }
            std::vector<V3GraphVariant> converted;
            converted.reserve(page.variants.size());
            for (const auto& variant : page.variants) converted.push_back(Convert(variant));
            result.cursor.inner = std::move(page.cursor); result.pruned = std::move(page.pruned);
            result.exhausted = page.exhausted; result.paused = page.paused; result.workUsed = page.workUsed;
            result.variants = std::move(converted);
        }
        catch (const std::bad_alloc&) { result.exhausted = false; result.paused = true; result.errors.emplace_back("v3_graph_resource_unavailable"); }
        catch (const std::length_error&) { result.exhausted = false; result.paused = true; result.errors.emplace_back("v3_graph_resource_unavailable"); }
        return result;
    }
    // Outputs and the returned cursor still require one caller-owned durable
    // transaction. The checksum detects accidental corruption only.
    static V3GraphCursorCodec SerializeCursor(const V3FamilyIdentityContext& identity,
        const V3FamilyValidatedContext& validated, const EventProgram& program, V3GraphCursor cursor)
    {
        V3GraphCursorCodec result;
        result.cursor = std::move(cursor);
        try
        {
            auto page = Enumerate(identity, validated, program, result.cursor, 0, 1);
            result.errors = std::move(page.errors);
            if (!result.errors.empty()) return result;
            std::string encoded;
            EncodeDfsCursorState(encoded, page.cursor.inner, "v3_call_graph_cursor_encoding_v1");
            result.encoded = std::move(encoded); result.cursor = std::move(page.cursor);
        }
        catch (const std::bad_alloc&) { result.encoded.clear(); result.errors.emplace_back("v3_graph_cursor_resource_unavailable"); }
        catch (const std::length_error&) { result.encoded.clear(); result.errors.emplace_back("v3_graph_cursor_resource_unavailable"); }
        return result;
    }
    static V3GraphCursorCodec ParseCursor(const V3FamilyIdentityContext& identity,
        const V3FamilyValidatedContext& validated, const EventProgram& program, std::string_view encoded)
    {
        V3GraphCursorCodec result;
        try
        {
        auto decoded = DecodeDfsCursorState(encoded, "v3_call_graph_cursor_encoding_v1");
        if (!decoded.errors.empty()) { result.errors = std::move(decoded.errors); return result; }
        result = SerializeCursor(identity, validated, program, V3GraphCursor{std::move(decoded.cursor)});
        if (result.errors.empty() && result.encoded != encoded)
        { result.errors.emplace_back("v3_graph_cursor_encoding_noncanonical"); result.encoded.clear(); }
        }
        catch (const std::bad_alloc&) { result.encoded.clear(); result.errors.emplace_back("v3_graph_cursor_resource_unavailable"); }
        catch (const std::length_error&) { result.encoded.clear(); result.errors.emplace_back("v3_graph_cursor_resource_unavailable"); }
        return result;
    }
    static V3GraphReplay Replay(const V3FamilyIdentityContext& identity, const V3FamilyValidatedContext& validated,
        const EventProgram& program, std::string_view id)
    {
        V3GraphReplay result;
        try
        {
            PreparedGraph graph;
            if (!Prepare(identity, validated, program, graph, result.errors)) return result;
            const auto replay = SharedGraphKernel::ReplayCore(graph, id);
            result.errors = replay.errors;
            result.identityValid = replay.identityValid && result.errors.empty();
            if (result.identityValid) result.variant = Convert(replay.variant);
        }
        catch (const std::bad_alloc&) { result.identityValid = false; result.variant = {}; result.errors.emplace_back("v3_graph_resource_unavailable"); }
        catch (const std::length_error&) { result.identityValid = false; result.variant = {}; result.errors.emplace_back("v3_graph_resource_unavailable"); }
        return result;
    }
};

// V4 required-call preview only. This deliberately has a distinct type,
// identity and cursor encoding, and is not the full conditional-event graph
// or an execution receipt. The authority, Program and Spec are rebuilt for
// every page/replay; callers cannot supply a synthetic Program or Spec.
struct V4GraphVariant
{
    std::string ordinal, id;
    std::vector<std::size_t> classes;
    std::vector<std::string> order, unresolved;
};
struct V4GraphCursor { DfsCursor inner; };
struct V4GraphPage
{
    std::vector<V4GraphVariant> variants;
    std::vector<DfsPrune> pruned;
    V4GraphCursor cursor;
    bool exhausted = false, paused = false;
    std::size_t workUsed = 0;
    std::vector<std::string> errors;
};
struct V4GraphReplay
{
    bool identityValid = false;
    V4GraphVariant variant;
    std::vector<std::string> errors;
};
struct V4GraphCursorCodec
{
    V4GraphCursor cursor;
    std::string encoded;
    std::vector<std::string> errors;
};
class V4Graph final
{
    static bool Prepare(const V4FamilyIdentityContext& identity, const V4FamilyValidatedContext& verified,
        PreparedGraph& graph, std::vector<std::string>& errors)
    {
        const auto mapping = MapV4FamilyCandidate(identity, verified);
        if (!mapping.graphAvailable || mapping.status != "graph_preview_unexecuted")
        {
            errors.push_back("v4_call_graph_unavailable:" + mapping.status);
            errors.insert(errors.end(), mapping.reasons.begin(), mapping.reasons.end());
            return false;
        }
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        if (raw.resourceUnavailable || raw.id.empty() || !raw.errors.empty())
        { errors.emplace_back("v4_call_graph_candidate_identity_unavailable"); return false; }
        std::string binding;
        AppendToken(binding, "v4_call_graph_candidate_program_binding_v1");
        AppendToken(binding, identity.SchemaBinding()); AppendToken(binding, raw.id);
        AppendToken(binding, std::to_string(mapping.program.steps.size()));
        for (const auto& step : mapping.program.steps) { AppendToken(binding, step.kind); AppendToken(binding, step.parameter); }
        AppendToken(binding, std::to_string(mapping.program.unresolved.size()));
        for (const auto& reason : mapping.program.unresolved) AppendToken(binding, reason);
        AppendToken(binding, std::to_string(mapping.program.postconditions.size()));
        for (const auto& outcome : mapping.program.postconditions) { AppendToken(binding, outcome.axis); AppendToken(binding, outcome.value); }
        const auto prepared = SharedGraphKernel::Enumerate(identity.Axes(), verified.Coordinate(), binding,
            "global_drag_event_graph_v4_generalized_family", mapping.spec, "0", 1, {}, &graph);
        errors.insert(errors.end(), prepared.errors.begin(), prepared.errors.end());
        if (!prepared.structuralExclusion.empty()) errors.push_back("v4_call_graph_structure_excluded:" + prepared.structuralExclusion);
        return errors.empty();
    }
    static V4GraphVariant Convert(const Variant& variant)
    { return {variant.ordinal, variant.id, variant.classes, variant.order, variant.unresolved}; }
public:
    static V4GraphPage Enumerate(const V4FamilyIdentityContext& identity, const V4FamilyValidatedContext& verified,
        V4GraphCursor cursor, std::size_t workBudget, std::size_t outputLimit)
    {
        V4GraphPage result;
        result.cursor = std::move(cursor);
        try
        {
            PreparedGraph graph;
            if (!Prepare(identity, verified, graph, result.errors)) return result;
            auto page = SharedGraphKernel::EnumerateDfsCore(graph, result.cursor.inner, workBudget, outputLimit);
            if (!page.errors.empty()) { result.errors = std::move(page.errors); return result; }
            std::vector<V4GraphVariant> converted;
            converted.reserve(page.variants.size());
            for (const auto& variant : page.variants) converted.push_back(Convert(variant));
            result.cursor.inner = std::move(page.cursor); result.pruned = std::move(page.pruned);
            result.exhausted = page.exhausted; result.paused = page.paused; result.workUsed = page.workUsed;
            result.variants = std::move(converted);
        }
        catch (const std::bad_alloc&) { result.exhausted = false; result.paused = true; result.errors.emplace_back("v4_graph_resource_unavailable"); }
        catch (const std::length_error&) { result.exhausted = false; result.paused = true; result.errors.emplace_back("v4_graph_resource_unavailable"); }
        return result;
    }
    static V4GraphCursorCodec SerializeCursor(const V4FamilyIdentityContext& identity,
        const V4FamilyValidatedContext& verified, V4GraphCursor cursor)
    {
        V4GraphCursorCodec result;
        result.cursor = std::move(cursor);
        try
        {
            auto page = Enumerate(identity, verified, result.cursor, 0, 1);
            result.errors = std::move(page.errors);
            if (!result.errors.empty()) return result;
            std::string encoded;
            EncodeDfsCursorState(encoded, page.cursor.inner, "v4_call_graph_cursor_encoding_v1");
            result.encoded = std::move(encoded); result.cursor = std::move(page.cursor);
        }
        catch (const std::bad_alloc&) { result.encoded.clear(); result.errors.emplace_back("v4_graph_cursor_resource_unavailable"); }
        catch (const std::length_error&) { result.encoded.clear(); result.errors.emplace_back("v4_graph_cursor_resource_unavailable"); }
        return result;
    }
    static V4GraphCursorCodec ParseCursor(const V4FamilyIdentityContext& identity,
        const V4FamilyValidatedContext& verified, std::string_view encoded)
    {
        V4GraphCursorCodec result;
        try
        {
            auto decoded = DecodeDfsCursorState(encoded, "v4_call_graph_cursor_encoding_v1");
            if (!decoded.errors.empty()) { result.errors = std::move(decoded.errors); return result; }
            result = SerializeCursor(identity, verified, V4GraphCursor{std::move(decoded.cursor)});
            if (result.errors.empty() && result.encoded != encoded)
            { result.errors.emplace_back("v4_graph_cursor_encoding_noncanonical"); result.encoded.clear(); }
        }
        catch (const std::bad_alloc&) { result.encoded.clear(); result.errors.emplace_back("v4_graph_cursor_resource_unavailable"); }
        catch (const std::length_error&) { result.encoded.clear(); result.errors.emplace_back("v4_graph_cursor_resource_unavailable"); }
        return result;
    }
    static V4GraphReplay Replay(const V4FamilyIdentityContext& identity, const V4FamilyValidatedContext& verified,
        std::string_view id)
    {
        V4GraphReplay result;
        try
        {
            PreparedGraph graph;
            if (!Prepare(identity, verified, graph, result.errors)) return result;
            const auto replay = SharedGraphKernel::ReplayCore(graph, id);
            result.errors = replay.errors;
            result.identityValid = replay.identityValid && result.errors.empty();
            if (result.identityValid) result.variant = Convert(replay.variant);
        }
        catch (const std::bad_alloc&) { result.identityValid = false; result.variant = {}; result.errors.emplace_back("v4_graph_resource_unavailable"); }
        catch (const std::length_error&) { result.identityValid = false; result.variant = {}; result.errors.emplace_back("v4_graph_resource_unavailable"); }
        return result;
    }
};
// These types are deliberately distinct from the legacy v1 Variant and DFS
// cursor. They represent partitions and orders for ONE presence hypothesis,
// not a Cartesian run over all hypotheses or a product execution result.
struct PresenceActiveGraphVariant
{
    std::string ordinal, id;
    std::vector<std::size_t> classes; // canonical active requirement-ID order
    std::vector<std::string> order, unresolved;
};
struct PresenceActiveGraphCursor
{
    std::string version = "presence_active_graph_cursor_v1";
    std::string projectionBinding;
    DfsCursor inner;
};
struct PresenceActiveGraphPage
{
    std::string startOrdinal;
    std::vector<PresenceActiveGraphVariant> variants;
    std::vector<DfsPrune> pruned;
    PresenceActiveGraphCursor cursor;
    bool exhausted = false, paused = false;
    bool resourceUnavailable = false;
    std::size_t workUsed = 0;
    std::string structuralExclusion;
    std::vector<std::string> errors;
};
struct PresenceActiveGraphReplay
{
    bool identityValid = false;
    bool resourceUnavailable = false;
    PresenceActiveGraphVariant variant;
    std::string structuralExclusion;
    std::vector<std::string> errors;
};
struct PresenceActiveCursorCodec
{
    PresenceActiveGraphCursor cursor;
    std::string encoded, structuralExclusion;
    std::vector<std::string> errors;
    bool resourceUnavailable = false;
};

class PresenceActiveGraph final
{
    static bool BuildActive(const PresenceProjection& projection, Spec& active,
        std::string& structuralExclusion, std::vector<std::string>& errors)
    {
        structuralExclusion = projection.structuralExclusion; errors = projection.errors;
        if (!projection.structurallyValid || !errors.empty() || !structuralExclusion.empty())
        {
            if (errors.empty() && structuralExclusion.empty()) errors.emplace_back("active_graph_projection_unavailable");
            return false;
        }
        active.requirements = projection.requirements;
        for (auto& requirement : active.requirements)
        { requirement.presence = PresencePolicy::required; requirement.presenceCondition.clear(); }
        active.pairs = projection.pairs; active.strictBefore = projection.strictBefore;
        active.unresolved = projection.unresolved;
        active.unresolved.emplace_back("active_graph_presence_not_production_observed");
        active.unresolved.emplace_back("active_graph_cross_hypothesis_product_unimplemented");
        active.causalCoverageComplete = false;
        return true;
    }
    template <typename GraphContext>
    static bool Prepare(const GraphContext& context, const Candidate& candidate, const Spec& supplied,
        const std::vector<bool>& present, PresenceProjection& projection, Spec& active,
        std::string& structuralExclusion, std::vector<std::string>& errors)
    {
        // Always reconstruct from the authoritative candidate and original
        // schema. Never accept a caller-mutated projection or its cached ID.
        projection = ProjectPresenceHypothesis(context, candidate, supplied, present);
        return BuildActive(projection, active, structuralExclusion, errors);
    }
    template <typename GraphContext>
    static bool PrepareCompleted(const GraphContext& context, const Candidate& candidate, const Spec& supplied,
        const std::vector<bool>& present, const PresenceProjectionWork& work, PresenceProjection& projection,
        Spec& active, std::string& structuralExclusion, std::vector<std::string>& errors)
    {
        if (!work.CanAuthorizeActiveGraph() || present != work.present_)
        { errors.emplace_back("active_graph_completed_projection_unavailable"); return false; }
        // This revalidates only schema/authority and binding. In particular it
        // does not call the quadratic InspectPresenceHypothesis path. Schema
        // construction and sorting are still synchronous preambles.
        const auto schema = PreparePresenceGraph(context, candidate, supplied);
        errors = schema.errors; structuralExclusion = schema.structuralExclusion;
        if (!errors.empty() || !structuralExclusion.empty() || schema.binding.empty()) return false;
        if (schema.binding != work.schemaBinding_ || work.projection_.schemaBinding != work.schemaBinding_ ||
            work.projection_.hypothesisBinding != work.hypothesisBinding_ || work.projection_.binding != work.projectionBinding_)
        { errors.emplace_back("active_graph_completed_projection_binding_mismatch"); return false; }
        projection = work.projection_;
        return BuildActive(projection, active, structuralExclusion, errors);
    }
    static std::string IdFor(const std::string& projectionBinding,
        const std::vector<std::size_t>& classes, const std::vector<std::string>& order)
    {
        std::string id;
        AppendToken(id, "global_drag_presence_active_graph_v1");
        AppendToken(id, projectionBinding);
        AppendToken(id, std::to_string(classes.size()));
        for (const auto value : classes) AppendToken(id, std::to_string(value));
        AppendToken(id, std::to_string(order.size()));
        for (const auto& value : order) AppendToken(id, value);
        return id;
    }
    static bool BindCursor(PresenceActiveGraphCursor& cursor, std::string_view binding,
        std::vector<std::string>& errors)
    {
        if (cursor.version != "presence_active_graph_cursor_v1")
        { errors.emplace_back("active_graph_cursor_version_invalid"); return false; }
        if (cursor.projectionBinding.empty())
        {
            const DfsCursor initial;
            if (cursor.inner.version != initial.version || cursor.inner.binding != initial.binding ||
                cursor.inner.integrity != initial.integrity || cursor.inner.nextOrdinal != initial.nextOrdinal ||
                cursor.inner.stage != initial.stage || cursor.inner.partition != initial.partition ||
                cursor.inner.partitionNext != initial.partitionNext || cursor.inner.partitionLeafProcessed ||
                !cursor.inner.topology.empty() || !cursor.inner.topologyNext.empty() || cursor.inner.topologyProduced ||
                !cursor.inner.pruneCounts.empty())
            { errors.emplace_back("active_graph_cursor_initial_state_invalid"); return false; }
            cursor.projectionBinding = std::string(binding);
        }
        else if (cursor.projectionBinding != binding)
        { errors.emplace_back("active_graph_cursor_projection_mismatch"); return false; }
        return true;
    }
public:
    template <typename GraphContext>
    static PresenceActiveCursorCodec SerializeCompleted(const GraphContext& context, const Candidate& candidate,
        const Spec& supplied, const std::vector<bool>& present, const PresenceProjectionWork& work,
        const PresenceActiveGraphCursor& cursor)
    {
        PresenceActiveCursorCodec result;
        try
        {
            result.cursor = cursor; PresenceProjection projection; Spec active;
            if (!PrepareCompleted(context, candidate, supplied, present, work, projection, active,
                    result.structuralExclusion, result.errors) || !BindCursor(result.cursor, projection.binding, result.errors)) return result;
            const auto saved = SerializeDfsCursor(context, candidate, active, result.cursor.inner);
            result.errors = saved.errors; result.structuralExclusion = saved.structuralExclusion;
            if (!result.errors.empty() || !result.structuralExclusion.empty()) return result;
            std::string encoded;
            AppendToken(encoded, "presence_active_cursor_encoding_v1"); AppendToken(encoded, result.cursor.version);
            AppendToken(encoded, result.cursor.projectionBinding); AppendToken(encoded, saved.encoded);
            result.cursor.inner = saved.cursor; result.encoded = std::move(encoded);
        }
        catch (const std::bad_alloc&) { result.encoded.clear(); result.resourceUnavailable = true; }
        catch (const std::length_error&) { result.encoded.clear(); result.resourceUnavailable = true; }
        return result;
    }
    template <typename GraphContext>
    static PresenceActiveCursorCodec ParseCompleted(const GraphContext& context, const Candidate& candidate,
        const Spec& supplied, const std::vector<bool>& present, const PresenceProjectionWork& work, std::string_view encoded)
    {
        PresenceActiveCursorCodec result;
        try
        {
            PresenceProjection projection; Spec active;
            if (!PrepareCompleted(context, candidate, supplied, present, work, projection, active,
                    result.structuralExclusion, result.errors)) return result;
            auto remaining = encoded; std::string_view token;
            if (!ReadGraphToken(remaining, token) || token != "presence_active_cursor_encoding_v1")
            { result.errors.emplace_back("active_graph_cursor_encoding_invalid"); return result; }
            if (!ReadGraphToken(remaining, token)) { result.errors.emplace_back("active_graph_cursor_encoding_invalid"); return result; }
            result.cursor.version = std::string(token);
            if (!ReadGraphToken(remaining, token)) { result.errors.emplace_back("active_graph_cursor_encoding_invalid"); return result; }
            result.cursor.projectionBinding = std::string(token);
            if (!BindCursor(result.cursor, projection.binding, result.errors)) return result;
            if (!ReadGraphToken(remaining, token) || !remaining.empty())
            { result.errors.emplace_back("active_graph_cursor_encoding_invalid"); return result; }
            const auto parsed = ParseDfsCursor(context, candidate, active, token);
            result.errors = parsed.errors; result.structuralExclusion = parsed.structuralExclusion;
            if (!result.errors.empty() || !result.structuralExclusion.empty()) return result;
            result.cursor.inner = parsed.cursor;
            const auto canonical = SerializeCompleted(context, candidate, supplied, present, work, result.cursor);
            if (canonical.resourceUnavailable || !canonical.errors.empty() || !canonical.structuralExclusion.empty()) return canonical;
            if (canonical.encoded != encoded) { result.errors.emplace_back("active_graph_cursor_encoding_noncanonical"); return result; }
            result.encoded = canonical.encoded;
        }
        catch (const std::bad_alloc&) { result.encoded.clear(); result.resourceUnavailable = true; }
        catch (const std::length_error&) { result.encoded.clear(); result.resourceUnavailable = true; }
        return result;
    }
    // Consume only a completed, private in-process projection capability.
    // This avoids repeating presence-pair expansion on each DFS step; it is
    // still a hypothetical graph and does not authenticate a durable cursor.
    template <typename GraphContext>
    static PresenceActiveGraphPage EnumerateCompleted(const GraphContext& context, const Candidate& candidate,
        const Spec& supplied, const std::vector<bool>& present, const PresenceProjectionWork& work,
        const PresenceActiveGraphCursor& cursor, std::size_t workBudget, std::size_t outputLimit)
    {
        PresenceActiveGraphPage result;
        try
        {
            result.cursor = cursor;
            PresenceProjection projection; Spec active;
            if (!PrepareCompleted(context, candidate, supplied, present, work, projection, active,
                    result.structuralExclusion, result.errors)) return result;
            auto working = cursor;
            if (!BindCursor(working, projection.binding, result.errors)) return result;
            const auto page = EnumerateDfs(context, candidate, active, working.inner, workBudget, outputLimit);
            result.errors = page.errors; result.structuralExclusion = page.structuralExclusion;
            if (!result.errors.empty() || !result.structuralExclusion.empty()) return result;
            std::vector<PresenceActiveGraphVariant> variants;
            variants.reserve(page.variants.size());
            for (const auto& item : page.variants)
                variants.push_back({item.ordinal, IdFor(projection.binding, item.classes, item.order),
                    item.classes, item.order, item.unresolved});
            working.inner = page.cursor;
            result.startOrdinal = cursor.inner.nextOrdinal; result.pruned = page.pruned;
            result.variants = std::move(variants); result.cursor = std::move(working);
            result.exhausted = page.exhausted; result.paused = page.paused; result.workUsed = page.workUsed;
        }
        catch (const std::bad_alloc&) { result.variants.clear(); result.pruned.clear(); result.workUsed = 0; result.resourceUnavailable = true; result.paused = true; }
        catch (const std::length_error&) { result.variants.clear(); result.pruned.clear(); result.workUsed = 0; result.resourceUnavailable = true; result.paused = true; }
        return result;
    }
    template <typename GraphContext>
    static PresenceActiveGraphReplay ReplayCompleted(const GraphContext& context, const Candidate& candidate,
        const Spec& supplied, const std::vector<bool>& present, const PresenceProjectionWork& work, std::string_view id)
    {
        PresenceActiveGraphReplay result;
        try
        {
            PresenceProjection projection; Spec active;
            if (!PrepareCompleted(context, candidate, supplied, present, work, projection, active,
                    result.structuralExclusion, result.errors)) return result;
            auto remaining = id; std::string_view token; std::size_t count = 0;
            if (!ReadGraphToken(remaining, token) || token != "global_drag_presence_active_graph_v1" ||
                !ReadGraphToken(remaining, token) || token != projection.binding ||
                !ReadGraphToken(remaining, token) || !ParseCanonicalSize(token, count) || count != active.requirements.size())
            { result.errors.emplace_back("active_graph_id_binding_or_version_invalid"); return result; }
            std::vector<std::size_t> classes; classes.reserve(count);
            for (std::size_t i = 0; i < count; ++i)
            {
                std::size_t choice = 0;
                if (!ReadGraphToken(remaining, token) || !ParseCanonicalSize(token, choice))
                { result.errors.emplace_back("active_graph_partition_encoding_invalid"); return result; }
                classes.push_back(choice);
            }
            std::size_t orderCount = 0;
            if (!ReadGraphToken(remaining, token) || !ParseCanonicalSize(token, orderCount) || orderCount > count)
            { result.errors.emplace_back("active_graph_order_count_invalid"); return result; }
            std::vector<std::string> order; order.reserve(orderCount);
            for (std::size_t i = 0; i < orderCount; ++i)
            {
                if (!ReadGraphToken(remaining, token)) { result.errors.emplace_back("active_graph_order_encoding_invalid"); return result; }
                order.emplace_back(token);
            }
            if (!remaining.empty() || IdFor(projection.binding, classes, order) != id)
            { result.errors.emplace_back("active_graph_id_noncanonical"); return result; }
            PreparedGraph graph;
            const auto prepared = EnumerateInternal(context, candidate, active, "0", 1, {}, &graph);
            result.errors = prepared.errors; result.structuralExclusion = prepared.structuralExclusion;
            if (!result.errors.empty() || !result.structuralExclusion.empty()) return result;
            std::string internal = graph.binding; AppendToken(internal, std::to_string(classes.size()));
            for (const auto value : classes) AppendToken(internal, std::to_string(value));
            AppendToken(internal, std::to_string(order.size())); for (const auto& value : order) AppendToken(internal, value);
            const auto replay = ReplayVariant(context, candidate, active, internal);
            result.errors = replay.errors; result.structuralExclusion = replay.structuralExclusion;
            if (!replay.identityValid || !result.errors.empty() || !result.structuralExclusion.empty())
            { if (result.errors.empty() && result.structuralExclusion.empty()) result.errors.emplace_back("active_graph_identity_invalid"); return result; }
            result.variant = {"", std::string(id), replay.variant.classes, replay.variant.order, replay.variant.unresolved};
            result.identityValid = true;
        }
        catch (const std::bad_alloc&) { result.identityValid = false; result.resourceUnavailable = true; }
        catch (const std::length_error&) { result.identityValid = false; result.resourceUnavailable = true; }
        return result;
    }
    template <typename GraphContext>
    static PresenceActiveGraphPage EnumerateUnchecked(const GraphContext& context, const Candidate& candidate, const Spec& supplied,
        const std::vector<bool>& present, PresenceActiveGraphCursor cursor, std::size_t workBudget, std::size_t outputLimit)
    {
        PresenceActiveGraphPage result;
        result.cursor = std::move(cursor);
        PresenceProjection projection;
        Spec active;
        if (!Prepare(context, candidate, supplied, present, projection, active, result.structuralExclusion, result.errors)) return result;
        if (!BindCursor(result.cursor, projection.binding, result.errors)) return result;
        result.startOrdinal = result.cursor.inner.nextOrdinal;
        const auto page = EnumerateDfs(context, candidate, active, result.cursor.inner, workBudget, outputLimit);
        result.errors = page.errors;
        result.structuralExclusion = page.structuralExclusion;
        if (!result.errors.empty() || !result.structuralExclusion.empty()) return result;
        result.cursor.inner = page.cursor;
        result.pruned = page.pruned;
        result.exhausted = page.exhausted; result.paused = page.paused; result.workUsed = page.workUsed;
        for (const auto& item : page.variants)
            result.variants.push_back({item.ordinal, IdFor(projection.binding, item.classes, item.order),
                item.classes, item.order, item.unresolved});
        return result;
    }

    template <typename GraphContext>
    static PresenceActiveGraphReplay ReplayUnchecked(const GraphContext& context, const Candidate& candidate, const Spec& supplied,
        const std::vector<bool>& present, std::string_view id)
    {
        PresenceActiveGraphReplay result;
        PresenceProjection projection;
        Spec active;
        if (!Prepare(context, candidate, supplied, present, projection, active, result.structuralExclusion, result.errors)) return result;
        auto remaining = id;
        std::string_view token;
        if (!ReadGraphToken(remaining, token) || token != "global_drag_presence_active_graph_v1" ||
            !ReadGraphToken(remaining, token) || token != projection.binding)
        { result.errors.emplace_back("active_graph_id_binding_or_version_invalid"); return result; }
        std::size_t count = 0;
        if (!ReadGraphToken(remaining, token) || !ParseCanonicalSize(token, count) || count != active.requirements.size())
        { result.errors.emplace_back("active_graph_partition_count_invalid"); return result; }
        std::vector<std::size_t> classes;
        classes.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            std::size_t choice = 0;
            if (!ReadGraphToken(remaining, token) || !ParseCanonicalSize(token, choice))
            { result.errors.emplace_back("active_graph_partition_encoding_invalid"); return result; }
            classes.push_back(choice);
        }
        std::size_t orderCount = 0;
        if (!ReadGraphToken(remaining, token) || !ParseCanonicalSize(token, orderCount) || orderCount > count)
        { result.errors.emplace_back("active_graph_order_count_invalid"); return result; }
        std::vector<std::string> order;
        order.reserve(orderCount);
        for (std::size_t i = 0; i < orderCount; ++i)
        {
            if (!ReadGraphToken(remaining, token)) { result.errors.emplace_back("active_graph_order_encoding_invalid"); return result; }
            order.emplace_back(token);
        }
        if (!remaining.empty() || IdFor(projection.binding, classes, order) != id)
        { result.errors.emplace_back("active_graph_id_noncanonical"); return result; }

        // Reuse only the existing version-specific validator internally. Its
        // temporary ID never escapes or authorizes the conditional graph.
        PreparedGraph graph;
        const auto prepared = EnumerateInternal(context, candidate, active, "0", 1, {}, &graph);
        result.errors = prepared.errors;
        result.structuralExclusion = prepared.structuralExclusion;
        if (!result.errors.empty() || !result.structuralExclusion.empty()) return result;
        std::string internal = graph.binding;
        AppendToken(internal, std::to_string(classes.size()));
        for (const auto value : classes) AppendToken(internal, std::to_string(value));
        AppendToken(internal, std::to_string(order.size()));
        for (const auto& value : order) AppendToken(internal, value);
        const auto replay = ReplayVariant(context, candidate, active, internal);
        result.errors = replay.errors;
        result.structuralExclusion = replay.structuralExclusion;
        if (!replay.identityValid || !result.errors.empty() || !result.structuralExclusion.empty())
        {
            if (result.errors.empty() && result.structuralExclusion.empty()) result.errors.emplace_back("active_graph_identity_invalid");
            return result;
        }
        result.variant = {"", std::string(id), replay.variant.classes, replay.variant.order, replay.variant.unresolved};
        result.identityValid = true;
        return result;
    }

    // Keep the original cursor at the caller on resource failure. Errors and
    // resource status are separate from model exclusions and product results.
    template <typename GraphContext>
    static PresenceActiveGraphPage Enumerate(const GraphContext& context, const Candidate& candidate, const Spec& supplied,
        const std::vector<bool>& present, const PresenceActiveGraphCursor& cursor, std::size_t workBudget, std::size_t outputLimit)
    {
        try { return EnumerateUnchecked(context, candidate, supplied, present, cursor, workBudget, outputLimit); }
        catch (const std::bad_alloc&) { PresenceActiveGraphPage out; out.resourceUnavailable = true; out.paused = true; return out; }
        catch (const std::length_error&) { PresenceActiveGraphPage out; out.resourceUnavailable = true; out.paused = true; return out; }
    }
    template <typename GraphContext>
    static PresenceActiveGraphReplay Replay(const GraphContext& context, const Candidate& candidate, const Spec& supplied,
        const std::vector<bool>& present, std::string_view id)
    {
        try { return ReplayUnchecked(context, candidate, supplied, present, id); }
        catch (const std::bad_alloc&) { PresenceActiveGraphReplay out; out.resourceUnavailable = true; return out; }
        catch (const std::length_error&) { PresenceActiveGraphReplay out; out.resourceUnavailable = true; return out; }
    }

    template <typename GraphContext>
    static PresenceActiveCursorCodec SerializeCursor(const GraphContext& context, const Candidate& candidate,
        const Spec& supplied, const std::vector<bool>& present, PresenceActiveGraphCursor cursor)
    {
        PresenceActiveCursorCodec result;
        result.cursor = std::move(cursor);
        PresenceProjection projection;
        Spec active;
        if (!Prepare(context, candidate, supplied, present, projection, active, result.structuralExclusion, result.errors)) return result;
        if (!BindCursor(result.cursor, projection.binding, result.errors)) return result;
        const auto saved = SerializeDfsCursor(context, candidate, active, result.cursor.inner);
        result.errors = saved.errors;
        result.structuralExclusion = saved.structuralExclusion;
        if (!result.errors.empty() || !result.structuralExclusion.empty()) return result;
        result.cursor.inner = saved.cursor;
        AppendToken(result.encoded, "presence_active_cursor_encoding_v1");
        AppendToken(result.encoded, result.cursor.version);
        AppendToken(result.encoded, result.cursor.projectionBinding);
        AppendToken(result.encoded, saved.encoded);
        return result;
    }

    template <typename GraphContext>
    static PresenceActiveCursorCodec ParseCursor(const GraphContext& context, const Candidate& candidate,
        const Spec& supplied, const std::vector<bool>& present, std::string_view encoded)
    {
        PresenceActiveCursorCodec result;
        PresenceProjection projection;
        Spec active;
        if (!Prepare(context, candidate, supplied, present, projection, active, result.structuralExclusion, result.errors)) return result;
        auto remaining = encoded;
        std::string_view token;
        if (!ReadGraphToken(remaining, token) || token != "presence_active_cursor_encoding_v1")
        { result.errors.emplace_back("active_graph_cursor_encoding_invalid"); return result; }
        if (!ReadGraphToken(remaining, token)) { result.errors.emplace_back("active_graph_cursor_encoding_invalid"); return result; }
        result.cursor.version = std::string(token);
        if (!ReadGraphToken(remaining, token)) { result.errors.emplace_back("active_graph_cursor_encoding_invalid"); return result; }
        result.cursor.projectionBinding = std::string(token);
        if (!BindCursor(result.cursor, projection.binding, result.errors)) return result;
        if (!ReadGraphToken(remaining, token) || !remaining.empty())
        { result.errors.emplace_back("active_graph_cursor_encoding_invalid"); return result; }
        const auto parsed = ParseDfsCursor(context, candidate, active, token);
        result.errors = parsed.errors;
        result.structuralExclusion = parsed.structuralExclusion;
        if (!result.errors.empty() || !result.structuralExclusion.empty()) return result;
        result.cursor.inner = parsed.cursor;
        const auto canonical = SerializeCursor(context, candidate, supplied, present, result.cursor);
        if (!canonical.errors.empty() || !canonical.structuralExclusion.empty()) return canonical;
        if (canonical.encoded != encoded)
        { result.errors.emplace_back("active_graph_cursor_encoding_noncanonical"); return result; }
        result.encoded = canonical.encoded;
        return result;
    }

};

// One resumable traversal of presence propositions and, for each proposition,
// its active-node partitions and topological orders. This is an unexecuted
// graph hypothesis, never an applicable product case or a PASS count.
struct JoinedGraphCursor
{
    std::string version = "presence_joined_graph_cursor_v1";
    std::string binding, integrity, nextOrdinal = "0";
    bool exhausted = false, hasActive = false;
    // Canonical encoded nested cursors bind to the same original inputs.
    std::string nextPresence, beforePresence, activeCursor, presenceOrdinal;
    std::vector<bool> present;
    std::map<std::string, std::string> completedActivePrunes;
};
struct JoinedGraphItem
{
    std::string ordinal, presenceOrdinal;
    std::vector<bool> present;
    PresenceActiveGraphVariant graph;
};
struct JoinedGraphPrune
{
    std::string presenceOrdinal;
    std::vector<bool> present;
    DfsPrune prune;
};
struct JoinedGraphPage
{
    std::string startOrdinal;
    std::vector<JoinedGraphItem> graphs;
    std::vector<PresencePrefixPrune> presencePruned;
    std::vector<JoinedGraphPrune> activePruned;
    JoinedGraphCursor cursor;
    bool exhausted = false, paused = false;
    bool resourceUnavailable = false;
    std::size_t workUsed = 0;
    std::string structuralExclusion;
    std::vector<std::string> errors;
};
struct JoinedGraphCursorCodec
{
    JoinedGraphCursor cursor;
    std::string encoded, structuralExclusion;
    std::vector<std::string> errors;
    bool resourceUnavailable = false;
};
struct JoinedDeferredPresencePage
{
    JoinedGraphCursor cursor;
    std::vector<PresencePrefixPrune> pruned;
    std::vector<bool> pendingPresent;
    std::string pendingOrdinal, structuralExclusion;
    std::vector<std::string> errors;
    std::size_t workUsed = 0;
    bool projectionRequired = false, exhausted = false, paused = false, resourceUnavailable = false;
};

class JoinedPresenceGraph final
{
    static std::string Payload(const JoinedGraphCursor& cursor)
    {
        std::string bytes;
        AppendToken(bytes, cursor.version); AppendToken(bytes, cursor.binding); AppendToken(bytes, cursor.nextOrdinal);
        AppendToken(bytes, cursor.exhausted ? "true" : "false"); AppendToken(bytes, cursor.hasActive ? "true" : "false");
        AppendToken(bytes, cursor.nextPresence); AppendToken(bytes, cursor.beforePresence);
        AppendToken(bytes, cursor.activeCursor); AppendToken(bytes, cursor.presenceOrdinal);
        AppendToken(bytes, std::to_string(cursor.present.size()));
        for (const bool bit : cursor.present) AppendToken(bytes, bit ? "1" : "0");
        AppendToken(bytes, std::to_string(cursor.completedActivePrunes.size()));
        for (const auto& [key, value] : cursor.completedActivePrunes) { AppendToken(bytes, key); AppendToken(bytes, value); }
        return bytes;
    }
    static std::string Integrity(const JoinedGraphCursor& cursor)
    {
        std::uint64_t hash = 14695981039346656037ull;
        for (const char c : Payload(cursor)) { hash ^= static_cast<unsigned char>(c); hash *= 1099511628211ull; }
        return std::to_string(hash); // Corruption check, not authentication or history proof.
    }
    template <typename GraphContext>
    static bool Validate(const GraphContext& context, const Candidate& candidate, const Spec& supplied,
        JoinedGraphCursor& cursor, std::string& excluded, std::vector<std::string>& errors,
        const PresenceProjectionWork* completed = nullptr)
    {
        const auto schema = PreparePresenceGraph(context, candidate, supplied);
        errors = schema.errors; excluded = schema.structuralExclusion;
        if (!errors.empty() || !excluded.empty()) return false;
        if (cursor.version != "presence_joined_graph_cursor_v1")
        { errors.emplace_back("joined_cursor_version_invalid"); return false; }
        try
        {
            if (Normalize(cursor.nextOrdinal) != cursor.nextOrdinal) errors.emplace_back("joined_cursor_ordinal_invalid");
        }
        catch (const std::exception&) { errors.emplace_back("joined_cursor_ordinal_invalid"); }
        if (cursor.binding.empty())
        {
            if (!cursor.integrity.empty() || cursor.nextOrdinal != "0" || cursor.exhausted || cursor.hasActive ||
                !cursor.nextPresence.empty() || !cursor.beforePresence.empty() || !cursor.activeCursor.empty() ||
                !cursor.presenceOrdinal.empty() || !cursor.present.empty() || !cursor.completedActivePrunes.empty())
                errors.emplace_back("joined_cursor_initial_state_invalid");
            else
            {
                const auto initial = SerializePresenceCursor(context, candidate, supplied, {});
                if (!initial.errors.empty() || !initial.structuralExclusion.empty())
                { errors.insert(errors.end(), initial.errors.begin(), initial.errors.end()); excluded = initial.structuralExclusion; }
                else { cursor.binding = schema.binding; cursor.nextPresence = initial.encoded; }
            }
        }
        else if (cursor.binding != schema.binding) errors.emplace_back("joined_cursor_binding_mismatch");
        else if (cursor.integrity.empty() || cursor.integrity != Integrity(cursor)) errors.emplace_back("joined_cursor_integrity_invalid");
        for (const auto& [key, value] : cursor.completedActivePrunes)
        {
            const bool validKey = key == "partition_prefix:pair_requires_same" || key == "partition_prefix:pair_requires_distinct" ||
                key == "partition_prefix:known_property_mismatch" || key == "complete_partition:strict_edge_same_event" ||
                key == "complete_partition:strict_causal_cycle";
            try { if (!validKey || Normalize(value) != value || value == "0") errors.emplace_back("joined_cursor_prune_count_invalid"); }
            catch (const std::exception&) { errors.emplace_back("joined_cursor_prune_count_invalid"); }
        }
        if (!errors.empty() || !excluded.empty()) return false;
        const auto next = ParsePresenceCursor(context, candidate, supplied, cursor.nextPresence);
        errors.insert(errors.end(), next.errors.begin(), next.errors.end()); excluded = next.structuralExclusion;
        if (!errors.empty() || !excluded.empty()) return false;
        if (!cursor.hasActive)
        {
            if (!cursor.beforePresence.empty() || !cursor.activeCursor.empty() || !cursor.presenceOrdinal.empty() || !cursor.present.empty() ||
                cursor.exhausted != next.cursor.exhausted)
                errors.emplace_back("joined_cursor_inactive_state_invalid");
        }
        else
        {
            if (cursor.exhausted || cursor.beforePresence.empty() || cursor.activeCursor.empty() || cursor.presenceOrdinal.empty())
            { errors.emplace_back("joined_cursor_active_state_invalid"); return false; }
            const auto before = ParsePresenceCursor(context, candidate, supplied, cursor.beforePresence);
            errors.insert(errors.end(), before.errors.begin(), before.errors.end()); excluded = before.structuralExclusion;
            if (!errors.empty() || !excluded.empty()) return false;
            // The preceding outer cursor was paused immediately before the
            // emitted presence leaf, so exactly one bounded step must recreate
            // that leaf and the saved next outer cursor. No prefix rescan.
            const auto leaf = EnumeratePresenceHypotheses(context, candidate, supplied, before.cursor, 1, 1, completed != nullptr);
            if (!leaf.errors.empty() || !leaf.structuralExclusion.empty() || leaf.workUsed != 1 || leaf.hypotheses.size() != 1 ||
                leaf.hypotheses.front().ordinal != cursor.presenceOrdinal || leaf.hypotheses.front().present != cursor.present)
            { errors.emplace_back("joined_cursor_presence_transition_invalid"); return false; }
            const auto after = SerializePresenceCursor(context, candidate, supplied, leaf.cursor);
            if (!after.errors.empty() || !after.structuralExclusion.empty() || after.encoded != cursor.nextPresence)
            { errors.emplace_back("joined_cursor_presence_resume_mismatch"); return false; }
            const auto active = completed ? PresenceActiveGraph::ParseCompleted(context, candidate, supplied, cursor.present, *completed, cursor.activeCursor) :
                PresenceActiveGraph::ParseCursor(context, candidate, supplied, cursor.present, cursor.activeCursor);
            if (active.resourceUnavailable) throw std::bad_alloc{};
            errors.insert(errors.end(), active.errors.begin(), active.errors.end()); excluded = active.structuralExclusion;
            if (errors.empty() && excluded.empty() && active.cursor.inner.stage == DfsStage::done)
                errors.emplace_back("joined_cursor_active_already_exhausted");
        }
        return errors.empty() && excluded.empty();
    }
public:
    // Advance only the presence DFS. At a leaf, leave that leaf uncommitted
    // and return its bits for the separately budgeted projection transaction.
    // This is an event hypothesis, not an executed or applicable case.
    template <typename GraphContext>
    static JoinedDeferredPresencePage AdvanceDeferred(const GraphContext& context, const Candidate& candidate,
        const Spec& supplied, const JoinedGraphCursor& cursor, std::size_t workBudget)
    {
        JoinedDeferredPresencePage result;
        try
        {
            result.cursor = cursor; auto state = cursor;
            if (!Validate(context, candidate, supplied, state, result.structuralExclusion, result.errors)) return result;
            if (state.hasActive) { result.errors.emplace_back("joined_deferred_cursor_already_active"); return result; }
            while (result.workUsed < workBudget && !state.exhausted)
            {
                const auto parsed = ParsePresenceCursor(context, candidate, supplied, state.nextPresence);
                if (!parsed.errors.empty() || !parsed.structuralExclusion.empty())
                { result.errors = parsed.errors; result.structuralExclusion = parsed.structuralExclusion; result.pruned.clear(); result.workUsed = 0; return result; }
                const auto page = EnumeratePresenceHypotheses(context, candidate, supplied, parsed.cursor, 1, 1, true);
                if (!page.errors.empty() || !page.structuralExclusion.empty())
                { result.errors = page.errors; result.structuralExclusion = page.structuralExclusion; result.pruned.clear(); result.workUsed = 0; return result; }
                result.workUsed += page.workUsed;
                if (!page.hypotheses.empty())
                {
                    if (page.hypotheses.size() != 1 || !page.hypotheses.front().unresolvedDeferred)
                    { result.errors.emplace_back("joined_deferred_leaf_invalid"); result.pruned.clear(); result.workUsed = 0; return result; }
                    result.projectionRequired = true; result.pendingPresent = page.hypotheses.front().present;
                    result.pendingOrdinal = page.hypotheses.front().ordinal; break;
                }
                const auto saved = SerializePresenceCursor(context, candidate, supplied, page.cursor);
                if (!saved.errors.empty() || !saved.structuralExclusion.empty())
                { result.errors = saved.errors; result.structuralExclusion = saved.structuralExclusion; result.pruned.clear(); result.workUsed = 0; return result; }
                state.nextPresence = saved.encoded; result.pruned.insert(result.pruned.end(), page.pruned.begin(), page.pruned.end());
                state.exhausted = page.exhausted;
                if (!page.workUsed && !page.exhausted) { result.errors.emplace_back("joined_deferred_no_progress"); result.pruned.clear(); result.workUsed = 0; return result; }
            }
            state.integrity = Integrity(state); result.cursor = std::move(state);
            result.exhausted = result.cursor.exhausted; result.paused = !result.exhausted;
        }
        catch (const std::bad_alloc&) { result.pruned.clear(); result.pendingPresent.clear(); result.pendingOrdinal.clear(); result.projectionRequired = false; result.workUsed = 0; result.resourceUnavailable = true; result.paused = true; }
        catch (const std::length_error&) { result.pruned.clear(); result.pendingPresent.clear(); result.pendingOrdinal.clear(); result.projectionRequired = false; result.workUsed = 0; result.resourceUnavailable = true; result.paused = true; }
        return result;
    }
    // Attach a completed private projection only when one exact deferred leaf
    // is independently regenerated from the still-uncommitted presence cursor.
    template <typename GraphContext>
    static JoinedGraphPage AttachCompletedProjection(const GraphContext& context, const Candidate& candidate,
        const Spec& supplied, const JoinedGraphCursor& cursor, const PresenceProjectionWork& work)
    {
        JoinedGraphPage result;
        try
        {
            result.cursor = cursor; auto state = cursor;
            if (!Validate(context, candidate, supplied, state, result.structuralExclusion, result.errors)) return result;
            if (state.hasActive || state.exhausted)
            { result.errors.emplace_back("joined_projection_attach_state_invalid"); return result; }
            const auto before = state.nextPresence;
            const auto parsed = ParsePresenceCursor(context, candidate, supplied, before);
            if (!parsed.errors.empty() || !parsed.structuralExclusion.empty())
            { result.errors = parsed.errors; result.structuralExclusion = parsed.structuralExclusion; return result; }
            const auto leaf = EnumeratePresenceHypotheses(context, candidate, supplied, parsed.cursor, 1, 1, true);
            if (!leaf.errors.empty() || !leaf.structuralExclusion.empty() || leaf.workUsed != 1 ||
                leaf.hypotheses.size() != 1 || !leaf.hypotheses.front().unresolvedDeferred)
            { result.errors.emplace_back("joined_projection_leaf_unavailable"); return result; }
            const auto saved = SerializePresenceCursor(context, candidate, supplied, leaf.cursor);
            if (!saved.errors.empty() || !saved.structuralExclusion.empty())
            { result.errors = saved.errors; result.structuralExclusion = saved.structuralExclusion; return result; }
            const auto active = PresenceActiveGraph::SerializeCompleted(context, candidate, supplied,
                leaf.hypotheses.front().present, work, {});
            if (active.resourceUnavailable) { result.resourceUnavailable = true; result.paused = true; return result; }
            if (!active.errors.empty() || !active.structuralExclusion.empty())
            { result.errors = active.errors; result.structuralExclusion = active.structuralExclusion; return result; }
            state.beforePresence = before; state.presenceOrdinal = leaf.hypotheses.front().ordinal;
            state.present = leaf.hypotheses.front().present; state.nextPresence = saved.encoded;
            state.activeCursor = active.encoded; state.hasActive = true; state.integrity = Integrity(state);
            result.cursor = std::move(state); result.workUsed = 1; result.paused = true;
        }
        catch (const std::bad_alloc&) { result.workUsed = 0; result.resourceUnavailable = true; result.paused = true; }
        catch (const std::length_error&) { result.workUsed = 0; result.resourceUnavailable = true; result.paused = true; }
        return result;
    }
    template <typename GraphContext>
    static JoinedGraphPage EnumerateCompletedProjection(const GraphContext& context, const Candidate& candidate,
        const Spec& supplied, const JoinedGraphCursor& cursor, const PresenceProjectionWork& work,
        std::size_t workBudget, std::size_t outputLimit)
    {
        JoinedGraphPage result;
        try
        {
            result.cursor = cursor; auto state = cursor;
            if (!outputLimit) { result.errors.emplace_back("joined_output_limit_must_be_positive"); return result; }
            if (!Validate(context, candidate, supplied, state, result.structuralExclusion, result.errors, &work)) return result;
            if (!state.hasActive) { result.errors.emplace_back("joined_completed_projection_not_active"); return result; }
            const auto parsed = PresenceActiveGraph::ParseCompleted(context, candidate, supplied, state.present, work, state.activeCursor);
            if (parsed.resourceUnavailable) { result.resourceUnavailable = true; result.paused = true; return result; }
            if (!parsed.errors.empty() || !parsed.structuralExclusion.empty())
            { result.errors = parsed.errors; result.structuralExclusion = parsed.structuralExclusion; return result; }
            const auto page = PresenceActiveGraph::EnumerateCompleted(context, candidate, supplied, state.present, work,
                parsed.cursor, workBudget, outputLimit);
            if (page.resourceUnavailable) { result.resourceUnavailable = true; result.paused = true; return result; }
            if (!page.errors.empty() || !page.structuralExclusion.empty())
            { result.errors = page.errors; result.structuralExclusion = page.structuralExclusion; return result; }
            const auto saved = PresenceActiveGraph::SerializeCompleted(context, candidate, supplied, state.present, work, page.cursor);
            if (saved.resourceUnavailable) { result.resourceUnavailable = true; result.paused = true; return result; }
            if (!saved.errors.empty() || !saved.structuralExclusion.empty())
            { result.errors = saved.errors; result.structuralExclusion = saved.structuralExclusion; return result; }
            result.startOrdinal = state.nextOrdinal;
            for (const auto& prune : page.pruned) result.activePruned.push_back({state.presenceOrdinal, state.present, prune});
            state.activeCursor = saved.encoded;
            for (const auto& graph : page.variants)
            { result.graphs.push_back({state.nextOrdinal, state.presenceOrdinal, state.present, graph}); Increment(state.nextOrdinal); }
            if (page.exhausted)
            {
                for (const auto& [key, count] : page.cursor.inner.pruneCounts)
                { auto& total = state.completedActivePrunes[key]; total = Add(total.empty() ? "0" : total, count); }
                state.hasActive = false; state.beforePresence.clear(); state.activeCursor.clear();
                state.presenceOrdinal.clear(); state.present.clear();
                const auto next = ParsePresenceCursor(context, candidate, supplied, state.nextPresence);
                if (!next.errors.empty() || !next.structuralExclusion.empty())
                { result.errors = next.errors; result.structuralExclusion = next.structuralExclusion; result.graphs.clear(); result.activePruned.clear(); return result; }
                state.exhausted = next.cursor.exhausted;
            }
            state.integrity = Integrity(state); result.cursor = std::move(state);
            result.workUsed = page.workUsed; result.exhausted = result.cursor.exhausted; result.paused = !result.exhausted;
        }
        catch (const std::bad_alloc&) { result.graphs.clear(); result.activePruned.clear(); result.workUsed = 0; result.resourceUnavailable = true; result.paused = true; }
        catch (const std::length_error&) { result.graphs.clear(); result.activePruned.clear(); result.workUsed = 0; result.resourceUnavailable = true; result.paused = true; }
        return result;
    }
    template <typename GraphContext>
    static JoinedGraphPage EnumerateUnchecked(const GraphContext& context, const Candidate& candidate, const Spec& supplied,
        JoinedGraphCursor cursor, std::size_t workBudget, std::size_t outputLimit)
    {
        JoinedGraphPage result;
        const auto original = cursor;
        result.cursor = std::move(cursor);
        if (!outputLimit) { result.errors.emplace_back("joined_output_limit_must_be_positive"); return result; }
        if (!Validate(context, candidate, supplied, result.cursor, result.structuralExclusion, result.errors)) return result;
        auto& state = result.cursor;
        result.startOrdinal = state.nextOrdinal;
        const auto fail = [&] {
            result.graphs.clear(); result.presencePruned.clear(); result.activePruned.clear();
            result.cursor = original;
        };
        while (result.workUsed < workBudget && result.graphs.size() < outputLimit && !state.exhausted)
        {
            if (!state.hasActive)
            {
                const auto parsed = ParsePresenceCursor(context, candidate, supplied, state.nextPresence);
                if (!parsed.errors.empty() || !parsed.structuralExclusion.empty())
                { result.errors = parsed.errors; result.structuralExclusion = parsed.structuralExclusion; fail(); return result; }
                const auto before = state.nextPresence;
                const auto page = EnumeratePresenceHypotheses(context, candidate, supplied, parsed.cursor, 1, 1);
                if (!page.errors.empty() || !page.structuralExclusion.empty())
                { result.errors = page.errors; result.structuralExclusion = page.structuralExclusion; fail(); return result; }
                result.workUsed += page.workUsed;
                result.presencePruned.insert(result.presencePruned.end(), page.pruned.begin(), page.pruned.end());
                const auto saved = SerializePresenceCursor(context, candidate, supplied, page.cursor);
                if (!saved.errors.empty() || !saved.structuralExclusion.empty())
                { result.errors = saved.errors; result.structuralExclusion = saved.structuralExclusion; fail(); return result; }
                state.nextPresence = saved.encoded;
                if (!page.hypotheses.empty())
                {
                    const auto& item = page.hypotheses.front();
                    state.beforePresence = before; state.presenceOrdinal = item.ordinal; state.present = item.present;
                    const auto initial = PresenceActiveGraph::SerializeCursor(context, candidate, supplied, state.present, {});
                    if (!initial.errors.empty() || !initial.structuralExclusion.empty())
                    { result.errors = initial.errors; result.structuralExclusion = initial.structuralExclusion; fail(); return result; }
                    state.activeCursor = initial.encoded; state.hasActive = true;
                }
                else if (page.exhausted) state.exhausted = true;
                else if (!page.workUsed) { result.errors.emplace_back("joined_presence_no_progress"); fail(); return result; }
                continue;
            }
            const auto parsed = PresenceActiveGraph::ParseCursor(context, candidate, supplied, state.present, state.activeCursor);
            if (!parsed.errors.empty() || !parsed.structuralExclusion.empty())
            { result.errors = parsed.errors; result.structuralExclusion = parsed.structuralExclusion; fail(); return result; }
            const auto page = PresenceActiveGraph::Enumerate(context, candidate, supplied, state.present, parsed.cursor,
                workBudget - result.workUsed, outputLimit - result.graphs.size());
            if (page.resourceUnavailable)
                throw std::bad_alloc{}; // outer versioned facade preserves its original cursor
            if (!page.errors.empty() || !page.structuralExclusion.empty())
            { result.errors = page.errors; result.structuralExclusion = page.structuralExclusion; fail(); return result; }
            result.workUsed += page.workUsed;
            for (const auto& item : page.pruned) result.activePruned.push_back({state.presenceOrdinal, state.present, item});
            const auto saved = PresenceActiveGraph::SerializeCursor(context, candidate, supplied, state.present, page.cursor);
            if (!saved.errors.empty() || !saved.structuralExclusion.empty())
            { result.errors = saved.errors; result.structuralExclusion = saved.structuralExclusion; fail(); return result; }
            state.activeCursor = saved.encoded;
            for (const auto& graph : page.variants)
            {
                result.graphs.push_back({state.nextOrdinal, state.presenceOrdinal, state.present, graph});
                Increment(state.nextOrdinal);
            }
            if (page.exhausted)
            {
                for (const auto& [key, count] : page.cursor.inner.pruneCounts)
                {
                    auto& cumulative = state.completedActivePrunes[key];
                    cumulative = Add(cumulative.empty() ? "0" : cumulative, count);
                }
                state.hasActive = false; state.beforePresence.clear(); state.activeCursor.clear();
                state.presenceOrdinal.clear(); state.present.clear();
                const auto outer = ParsePresenceCursor(context, candidate, supplied, state.nextPresence);
                if (!outer.errors.empty() || !outer.structuralExclusion.empty())
                { result.errors = outer.errors; result.structuralExclusion = outer.structuralExclusion; fail(); return result; }
                if (outer.cursor.exhausted) state.exhausted = true;
            }
            else if (!page.workUsed) { result.errors.emplace_back("joined_active_no_progress"); fail(); return result; }
        }
        result.exhausted = state.exhausted; result.paused = !state.exhausted;
        state.integrity = Integrity(state);
        return result;
    }

    template <typename GraphContext>
    static JoinedGraphPage Enumerate(const GraphContext& context, const Candidate& candidate, const Spec& supplied,
        const JoinedGraphCursor& cursor, std::size_t workBudget, std::size_t outputLimit)
    {
        try { return EnumerateUnchecked(context, candidate, supplied, cursor, workBudget, outputLimit); }
        catch (const std::bad_alloc&) { JoinedGraphPage out; out.resourceUnavailable = true; out.paused = true; return out; }
        catch (const std::length_error&) { JoinedGraphPage out; out.resourceUnavailable = true; out.paused = true; return out; }
    }

    template <typename GraphContext>
    static JoinedGraphCursorCodec SerializeCompletedCursor(const GraphContext& context, const Candidate& candidate,
        const Spec& supplied, const JoinedGraphCursor& cursor, const PresenceProjectionWork& work)
    {
        JoinedGraphCursorCodec result;
        try
        {
            result.cursor = cursor; auto checked = cursor;
            if (!Validate(context, candidate, supplied, checked, result.structuralExclusion, result.errors, &work)) return result;
            if (!checked.hasActive) { result.errors.emplace_back("joined_completed_projection_not_active"); return result; }
            std::string encoded; AppendToken(encoded, "presence_joined_graph_cursor_encoding_v1");
            AppendToken(encoded, Payload(checked)); AppendToken(encoded, checked.integrity);
            result.cursor = std::move(checked); result.encoded = std::move(encoded);
        }
        catch (const std::bad_alloc&) { result.encoded.clear(); result.resourceUnavailable = true; }
        catch (const std::length_error&) { result.encoded.clear(); result.resourceUnavailable = true; }
        return result;
    }
    template <typename GraphContext>
    static JoinedGraphCursorCodec ParseCompletedCursor(const GraphContext& context, const Candidate& candidate,
        const Spec& supplied, std::string_view encoded, const PresenceProjectionWork& work)
    {
        JoinedGraphCursorCodec result;
        try
        {
            auto outer = encoded; std::string_view token, payload, checksum;
            const auto fail = [&] { result.errors.emplace_back("joined_cursor_encoding_invalid"); return result; };
            if (!ReadGraphToken(outer, token) || token != "presence_joined_graph_cursor_encoding_v1" ||
                !ReadGraphToken(outer, payload) || !ReadGraphToken(outer, checksum) || !outer.empty()) return fail();
            auto remaining = payload; auto& cursor = result.cursor;
            const auto read = [&] { return ReadGraphToken(remaining, token); };
            if (!read()) return fail();
            cursor.version = std::string(token);
            if (!read()) return fail();
            cursor.binding = std::string(token);
            if (!read()) return fail();
            cursor.nextOrdinal = std::string(token);
            if (!read() || (token != "true" && token != "false")) return fail();
            cursor.exhausted = token == "true";
            if (!read() || (token != "true" && token != "false")) return fail();
            cursor.hasActive = token == "true";
            if (!read()) return fail();
            cursor.nextPresence = std::string(token);
            if (!read()) return fail();
            cursor.beforePresence = std::string(token);
            if (!read()) return fail();
            cursor.activeCursor = std::string(token);
            if (!read()) return fail();
            cursor.presenceOrdinal = std::string(token);
            std::size_t count = 0;
            if (!read() || !ParseCanonicalSize(token, count) || count > remaining.size() / 2) return fail();
            cursor.present.reserve(count);
            for (std::size_t i = 0; i < count; ++i)
            { if (!read() || (token != "0" && token != "1")) return fail(); cursor.present.push_back(token == "1"); }
            if (!read() || !ParseCanonicalSize(token, count) || count > remaining.size() / 4) return fail();
            for (std::size_t i = 0; i < count; ++i)
            {
                if (!read()) return fail();
                const std::string key(token);
                if (!read() || !cursor.completedActivePrunes.emplace(key, std::string(token)).second) return fail();
            }
            if (!remaining.empty()) return fail();
            cursor.integrity = std::string(checksum);
            const auto canonical = SerializeCompletedCursor(context, candidate, supplied, cursor, work);
            if (canonical.resourceUnavailable || !canonical.errors.empty() || !canonical.structuralExclusion.empty()) return canonical;
            if (canonical.encoded != encoded) { result.errors.emplace_back("joined_cursor_encoding_noncanonical"); return result; }
            return canonical;
        }
        catch (const std::bad_alloc&) { result.encoded.clear(); result.resourceUnavailable = true; }
        catch (const std::length_error&) { result.encoded.clear(); result.resourceUnavailable = true; }
        return result;
    }
    template <typename GraphContext>
    static JoinedGraphCursorCodec SerializeCursor(const GraphContext& context, const Candidate& candidate,
        const Spec& supplied, JoinedGraphCursor cursor)
    {
        JoinedGraphCursorCodec result;
        const auto checked = Enumerate(context, candidate, supplied, std::move(cursor), 0, 1);
        if (checked.resourceUnavailable) throw std::bad_alloc{};
        result.cursor = checked.cursor; result.errors = checked.errors; result.structuralExclusion = checked.structuralExclusion;
        if (!result.errors.empty() || !result.structuralExclusion.empty()) return result;
        AppendToken(result.encoded, "presence_joined_graph_cursor_encoding_v1");
        AppendToken(result.encoded, Payload(result.cursor));
        AppendToken(result.encoded, result.cursor.integrity);
        return result;
    }

    template <typename GraphContext>
    static JoinedGraphCursorCodec ParseCursor(const GraphContext& context, const Candidate& candidate,
        const Spec& supplied, std::string_view encoded)
    {
        JoinedGraphCursorCodec result;
        auto outer = encoded;
        std::string_view token, payload, checksum;
        const auto fail = [&] { result.errors.emplace_back("joined_cursor_encoding_invalid"); return result; };
        if (!ReadGraphToken(outer, token) || token != "presence_joined_graph_cursor_encoding_v1" ||
            !ReadGraphToken(outer, payload) || !ReadGraphToken(outer, checksum) || !outer.empty()) return fail();
        auto& cursor = result.cursor;
        auto remaining = payload;
        const auto read = [&] { return ReadGraphToken(remaining, token); };
        if (!read()) return fail();
        cursor.version = std::string(token);
        if (!read()) return fail();
        cursor.binding = std::string(token);
        if (!read()) return fail();
        cursor.nextOrdinal = std::string(token);
        if (!read() || (token != "true" && token != "false")) return fail();
        cursor.exhausted = token == "true";
        if (!read() || (token != "true" && token != "false")) return fail();
        cursor.hasActive = token == "true";
        if (!read()) return fail();
        cursor.nextPresence = std::string(token);
        if (!read()) return fail();
        cursor.beforePresence = std::string(token);
        if (!read()) return fail();
        cursor.activeCursor = std::string(token);
        if (!read()) return fail();
        cursor.presenceOrdinal = std::string(token);
        std::size_t count = 0;
        if (!read() || !ParseCanonicalSize(token, count) || count > remaining.size() / 2) return fail();
        cursor.present.clear(); cursor.present.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            if (!read() || (token != "0" && token != "1")) return fail();
            cursor.present.push_back(token == "1");
        }
        if (!read() || !ParseCanonicalSize(token, count) || count > remaining.size() / 4) return fail();
        cursor.completedActivePrunes.clear();
        for (std::size_t i = 0; i < count; ++i)
        {
            if (!read()) return fail();
            const std::string key(token);
            if (!read() || !cursor.completedActivePrunes.emplace(key, std::string(token)).second) return fail();
        }
        if (!remaining.empty()) return fail();
        cursor.integrity = std::string(checksum);
        const auto canonical = SerializeCursor(context, candidate, supplied, cursor);
        if (!canonical.errors.empty() || !canonical.structuralExclusion.empty()) return canonical;
        if (canonical.encoded != encoded) { result.errors.emplace_back("joined_cursor_encoding_noncanonical"); return result; }
        result = canonical;
        return result;
    }
};
// V3 joined hypotheses have their own outer identity and cursor encoding.
// Their inner traversal is the same presence/partition/topology machinery as
// existing candidates; no V3 hypothesis is an executed or applicable case.
struct V3JoinedGraphVariant
{
    std::string ordinal, id;
    std::vector<std::size_t> classes;
    std::vector<std::string> order, unresolved;
};
struct V3JoinedGraphItem
{
    std::string ordinal, presenceOrdinal;
    std::vector<bool> present;
    V3JoinedGraphVariant graph;
};
struct V3JoinedGraphCursor { JoinedGraphCursor inner; };
struct V3JoinedGraphPage
{
    std::string startOrdinal;
    std::vector<V3JoinedGraphItem> graphs;
    std::vector<PresencePrefixPrune> presencePruned;
    std::vector<JoinedGraphPrune> activePruned;
    V3JoinedGraphCursor cursor;
    bool exhausted = false, paused = false;
    std::size_t workUsed = 0;
    std::vector<std::string> errors;
};
struct V3JoinedGraphReplay
{
    bool identityValid = false;
    V3JoinedGraphVariant variant;
    std::vector<std::string> errors;
};
struct V3JoinedGraphCursorCodec
{
    V3JoinedGraphCursor cursor;
    std::string encoded;
    std::vector<std::string> errors;
};
class V3JoinedGraph final
{
    static std::string OuterId(std::string_view inner)
    {
        std::string id;
        AppendToken(id, "global_v3_joined_presence_variant_v3"); AppendToken(id, inner);
        return id;
    }
    static V3JoinedGraphVariant Convert(const PresenceActiveGraphVariant& item)
    { return {item.ordinal, OuterId(item.id), item.classes, item.order, item.unresolved}; }
public:
    // The facade always regenerates the Spec from this exact authority. It
    // never accepts a caller-supplied Spec or a legacy graph context.
    static V3JoinedGraphPage Enumerate(const V3FamilyIdentityContext& identity,
        const V3FamilyValidatedContext& validated, const EventProgram& program,
        V3JoinedGraphCursor cursor, std::size_t workBudget, std::size_t outputLimit)
    {
        V3JoinedGraphPage result;
        result.cursor = std::move(cursor);
        try
        {
            const auto mapping = MapV3PresenceCandidate(identity, validated, program);
            if (!mapping.graphAvailable)
            {
                result.errors.push_back("v3_joined_mapping_unavailable:" + mapping.status);
                result.errors.insert(result.errors.end(), mapping.reasons.begin(), mapping.reasons.end());
                return result;
            }
            const V3GraphKernelContext context(identity, validated, program);
            auto page = JoinedPresenceGraph::Enumerate(context, validated.Coordinate(), mapping.spec,
                result.cursor.inner, workBudget, outputLimit);
            if (page.resourceUnavailable) throw std::bad_alloc{};
            result.errors = page.errors;
            if (!page.structuralExclusion.empty()) result.errors.push_back("v3_joined_structure_excluded:" + page.structuralExclusion);
            if (!result.errors.empty()) return result;
            std::vector<V3JoinedGraphItem> converted;
            converted.reserve(page.graphs.size());
            for (const auto& item : page.graphs)
                converted.push_back({item.ordinal, item.presenceOrdinal, item.present, Convert(item.graph)});
            result.startOrdinal = std::move(page.startOrdinal); result.cursor.inner = std::move(page.cursor);
            result.presencePruned = std::move(page.presencePruned); result.activePruned = std::move(page.activePruned);
            result.exhausted = page.exhausted; result.paused = page.paused; result.workUsed = page.workUsed;
            result.graphs = std::move(converted);
        }
        catch (const std::bad_alloc&) { result.graphs.clear(); result.exhausted = false; result.paused = true; result.errors.emplace_back("v3_joined_resource_unavailable"); }
        catch (const std::length_error&) { result.graphs.clear(); result.exhausted = false; result.paused = true; result.errors.emplace_back("v3_joined_resource_unavailable"); }
        return result;
    }
    static V3JoinedGraphReplay Replay(const V3FamilyIdentityContext& identity,
        const V3FamilyValidatedContext& validated, const EventProgram& program,
        const std::vector<bool>& present, std::string_view id)
    {
        V3JoinedGraphReplay result;
        try
        {
            const auto mapping = MapV3PresenceCandidate(identity, validated, program);
            if (!mapping.graphAvailable)
            { result.errors.push_back("v3_joined_mapping_unavailable:" + mapping.status); return result; }
            auto remaining = id; std::string_view token, inner;
            if (!ReadGraphToken(remaining, token) || token != "global_v3_joined_presence_variant_v3" ||
                !ReadGraphToken(remaining, inner) || !remaining.empty() || OuterId(inner) != id)
            { result.errors.emplace_back("v3_joined_graph_id_invalid"); return result; }
            const V3GraphKernelContext context(identity, validated, program);
            const auto replay = PresenceActiveGraph::Replay(context, validated.Coordinate(), mapping.spec, present, inner);
            if (replay.resourceUnavailable) throw std::bad_alloc{};
            result.errors = replay.errors;
            if (!replay.structuralExclusion.empty()) result.errors.push_back("v3_joined_structure_excluded:" + replay.structuralExclusion);
            result.identityValid = replay.identityValid && result.errors.empty();
            if (result.identityValid) result.variant = Convert(replay.variant);
        }
        catch (const std::bad_alloc&) { result.identityValid = false; result.variant = {}; result.errors.emplace_back("v3_joined_resource_unavailable"); }
        catch (const std::length_error&) { result.identityValid = false; result.variant = {}; result.errors.emplace_back("v3_joined_resource_unavailable"); }
        return result;
    }
    static V3JoinedGraphCursorCodec SerializeCursor(const V3FamilyIdentityContext& identity,
        const V3FamilyValidatedContext& validated, const EventProgram& program, V3JoinedGraphCursor cursor)
    {
        V3JoinedGraphCursorCodec result;
        result.cursor = std::move(cursor);
        try
        {
            const auto mapping = MapV3PresenceCandidate(identity, validated, program);
            if (!mapping.graphAvailable)
            { result.errors.push_back("v3_joined_mapping_unavailable:" + mapping.status); return result; }
            const V3GraphKernelContext context(identity, validated, program);
            auto saved = JoinedPresenceGraph::SerializeCursor(context, validated.Coordinate(), mapping.spec, result.cursor.inner);
            result.errors = saved.errors;
            if (!saved.structuralExclusion.empty()) result.errors.push_back("v3_joined_structure_excluded:" + saved.structuralExclusion);
            if (!result.errors.empty()) return result;
            AppendToken(result.encoded, "v3_joined_presence_cursor_encoding_v3"); AppendToken(result.encoded, saved.encoded);
            result.cursor.inner = std::move(saved.cursor);
        }
        catch (const std::bad_alloc&) { result.encoded.clear(); result.errors.emplace_back("v3_joined_resource_unavailable"); }
        catch (const std::length_error&) { result.encoded.clear(); result.errors.emplace_back("v3_joined_resource_unavailable"); }
        return result;
    }
    static V3JoinedGraphCursorCodec ParseCursor(const V3FamilyIdentityContext& identity,
        const V3FamilyValidatedContext& validated, const EventProgram& program, std::string_view encoded)
    {
        V3JoinedGraphCursorCodec result;
        try
        {
            const auto mapping = MapV3PresenceCandidate(identity, validated, program);
            if (!mapping.graphAvailable)
            { result.errors.push_back("v3_joined_mapping_unavailable:" + mapping.status); return result; }
            auto remaining = encoded; std::string_view token, inner;
            if (!ReadGraphToken(remaining, token))
            { result.errors.emplace_back("v3_joined_cursor_encoding_invalid"); return result; }
            if (token != "v3_joined_presence_cursor_encoding_v3")
            { result.errors.emplace_back("v3_joined_cursor_version_unsupported"); return result; }
            if (!ReadGraphToken(remaining, inner) || !remaining.empty())
            { result.errors.emplace_back("v3_joined_cursor_encoding_invalid"); return result; }
            const V3GraphKernelContext context(identity, validated, program);
            const auto parsed = JoinedPresenceGraph::ParseCursor(context, validated.Coordinate(), mapping.spec, inner);
            result.errors = parsed.errors;
            if (!parsed.structuralExclusion.empty()) result.errors.push_back("v3_joined_structure_excluded:" + parsed.structuralExclusion);
            if (!result.errors.empty()) return result;
            result.cursor.inner = parsed.cursor;
            const auto canonical = SerializeCursor(identity, validated, program, result.cursor);
            if (!canonical.errors.empty()) return canonical;
            if (canonical.encoded != encoded)
            { result.errors.emplace_back("v3_joined_cursor_encoding_noncanonical"); return result; }
            result.encoded = canonical.encoded;
        }
        catch (const std::bad_alloc&) { result.encoded.clear(); result.errors.emplace_back("v3_joined_resource_unavailable"); }
        catch (const std::length_error&) { result.encoded.clear(); result.errors.emplace_back("v3_joined_resource_unavailable"); }
        return result;
    }
};
// Independent V4 conditional-presence graph preview; never a production receipt.
struct V4JoinedGraphVariant
{
    std::string ordinal, id;
    std::vector<std::size_t> classes;
    std::vector<std::string> order, unresolved;
};
struct V4JoinedGraphItem
{
    std::string ordinal, presenceOrdinal;
    std::vector<bool> present;
    V4JoinedGraphVariant graph;
};
struct V4JoinedGraphCursor { JoinedGraphCursor inner; };
struct V4JoinedGraphPage
{
    std::string startOrdinal;
    std::vector<V4JoinedGraphItem> graphs;
    std::vector<PresencePrefixPrune> presencePruned;
    std::vector<JoinedGraphPrune> activePruned;
    V4JoinedGraphCursor cursor;
    bool exhausted = false, paused = false;
    bool resourceUnavailable = false;
    std::size_t workUsed = 0;
    std::vector<std::string> errors;
};
enum class V4GraphReplayStatus { unavailable, resource_unavailable, valid };
struct V4JoinedGraphReplay
{
    bool identityValid = false;
    V4JoinedGraphVariant variant;
    std::vector<std::string> errors;
    V4GraphReplayStatus status = V4GraphReplayStatus::unavailable;
};
struct V4JoinedGraphCursorCodec
{
    V4JoinedGraphCursor cursor;
    std::string encoded;
    std::vector<std::string> errors;
};
class V4JoinedGraph final
{
    static std::string OuterId(std::string_view inner)
    {
        std::string id;
        AppendToken(id, "global_v4_joined_presence_variant_v8"); AppendToken(id, inner);
        return id;
    }
    static V4JoinedGraphVariant Convert(const PresenceActiveGraphVariant& item)
    { return {item.ordinal, OuterId(item.id), item.classes, item.order, item.unresolved}; }
public:
    // The facade always regenerates the Spec from this exact authority. It
    // never accepts a caller-supplied Spec or a legacy graph context.
    static V4JoinedGraphPage Enumerate(const V4FamilyIdentityContext& identity,
        const V4FamilyValidatedContext& validated,
        V4JoinedGraphCursor cursor, std::size_t workBudget, std::size_t outputLimit)
    {
        V4JoinedGraphPage result;
        result.cursor = std::move(cursor);
        try
        {
            const auto mapping = MapV4PresenceCandidate(identity, validated);
            if (!mapping.graphAvailable)
            {
                result.resourceUnavailable = mapping.resourceUnavailable;
                result.errors.push_back("v4_joined_mapping_unavailable:" + mapping.status);
                result.errors.insert(result.errors.end(), mapping.reasons.begin(), mapping.reasons.end());
                return result;
            }
            const V4GraphKernelContext context(identity, validated);
            auto page = JoinedPresenceGraph::Enumerate(context, validated.Coordinate(), mapping.spec,
                result.cursor.inner, workBudget, outputLimit);
            if (page.resourceUnavailable) { result.resourceUnavailable = true; result.paused = true; return result; }
            result.errors = page.errors;
            if (!page.structuralExclusion.empty()) result.errors.push_back("v4_joined_structure_excluded:" + page.structuralExclusion);
            if (!result.errors.empty()) return result;
            std::vector<V4JoinedGraphItem> converted;
            converted.reserve(page.graphs.size());
            for (const auto& item : page.graphs)
                converted.push_back({item.ordinal, item.presenceOrdinal, item.present, Convert(item.graph)});
            result.startOrdinal = std::move(page.startOrdinal); result.cursor.inner = std::move(page.cursor);
            result.presencePruned = std::move(page.presencePruned); result.activePruned = std::move(page.activePruned);
            result.exhausted = page.exhausted; result.paused = page.paused; result.workUsed = page.workUsed;
            result.graphs = std::move(converted);
        }
        catch (const std::bad_alloc&) { result.graphs.clear(); result.exhausted = false; result.paused = true; result.resourceUnavailable = true; }
        catch (const std::length_error&) { result.graphs.clear(); result.exhausted = false; result.paused = true; result.resourceUnavailable = true; }
        return result;
    }
    static V4JoinedGraphReplay Replay(const V4FamilyIdentityContext& identity,
        const V4FamilyValidatedContext& validated,
        const std::vector<bool>& present, std::string_view id)
    {
        V4JoinedGraphReplay result;
        try
        {
            const auto mapping = MapV4PresenceCandidate(identity, validated);
            if (!mapping.graphAvailable)
            { if (mapping.resourceUnavailable) result.status = V4GraphReplayStatus::resource_unavailable;
              result.errors.push_back("v4_joined_mapping_unavailable:" + mapping.status); return result; }
            auto remaining = id; std::string_view token, inner;
            if (!ReadGraphToken(remaining, token) || token != "global_v4_joined_presence_variant_v8" ||
                !ReadGraphToken(remaining, inner) || !remaining.empty() || OuterId(inner) != id)
            { result.errors.emplace_back("v4_joined_graph_id_invalid"); return result; }
            const V4GraphKernelContext context(identity, validated);
            const auto replay = PresenceActiveGraph::Replay(context, validated.Coordinate(), mapping.spec, present, inner);
            if (replay.resourceUnavailable) { result.status = V4GraphReplayStatus::resource_unavailable; return result; }
            result.errors = replay.errors;
            if (!replay.structuralExclusion.empty()) result.errors.push_back("v4_joined_structure_excluded:" + replay.structuralExclusion);
            result.identityValid = replay.identityValid && result.errors.empty();
            if (result.identityValid) { result.variant = Convert(replay.variant); result.status = V4GraphReplayStatus::valid; }
        }
        catch (const std::bad_alloc&) { result.identityValid = false; result.variant = {}; result.status = V4GraphReplayStatus::resource_unavailable; }
        catch (const std::length_error&) { result.identityValid = false; result.variant = {}; result.status = V4GraphReplayStatus::resource_unavailable; }
        return result;
    }
    static V4JoinedGraphCursorCodec SerializeCursor(const V4FamilyIdentityContext& identity,
        const V4FamilyValidatedContext& validated, V4JoinedGraphCursor cursor)
    {
        V4JoinedGraphCursorCodec result;
        result.cursor = std::move(cursor);
        try
        {
            const auto mapping = MapV4PresenceCandidate(identity, validated);
            if (!mapping.graphAvailable)
            { result.errors.push_back("v4_joined_mapping_unavailable:" + mapping.status); return result; }
            const V4GraphKernelContext context(identity, validated);
            auto saved = JoinedPresenceGraph::SerializeCursor(context, validated.Coordinate(), mapping.spec, result.cursor.inner);
            result.errors = saved.errors;
            if (!saved.structuralExclusion.empty()) result.errors.push_back("v4_joined_structure_excluded:" + saved.structuralExclusion);
            if (!result.errors.empty()) return result;
            AppendToken(result.encoded, "v4_joined_presence_cursor_encoding_v8"); AppendToken(result.encoded, saved.encoded);
            result.cursor.inner = std::move(saved.cursor);
        }
        catch (const std::bad_alloc&) { result.encoded.clear(); result.errors.emplace_back("v4_joined_resource_unavailable"); }
        catch (const std::length_error&) { result.encoded.clear(); result.errors.emplace_back("v4_joined_resource_unavailable"); }
        return result;
    }
    static V4JoinedGraphCursorCodec ParseCursor(const V4FamilyIdentityContext& identity,
        const V4FamilyValidatedContext& validated, std::string_view encoded)
    {
        V4JoinedGraphCursorCodec result;
        try
        {
            const auto mapping = MapV4PresenceCandidate(identity, validated);
            if (!mapping.graphAvailable)
            { result.errors.push_back("v4_joined_mapping_unavailable:" + mapping.status); return result; }
            auto remaining = encoded; std::string_view token, inner;
            if (!ReadGraphToken(remaining, token))
            { result.errors.emplace_back("v4_joined_cursor_encoding_invalid"); return result; }
            if (token != "v4_joined_presence_cursor_encoding_v8")
            { result.errors.emplace_back("v4_joined_cursor_version_unsupported"); return result; }
            if (!ReadGraphToken(remaining, inner) || !remaining.empty())
            { result.errors.emplace_back("v4_joined_cursor_encoding_invalid"); return result; }
            const V4GraphKernelContext context(identity, validated);
            const auto parsed = JoinedPresenceGraph::ParseCursor(context, validated.Coordinate(), mapping.spec, inner);
            result.errors = parsed.errors;
            if (!parsed.structuralExclusion.empty()) result.errors.push_back("v4_joined_structure_excluded:" + parsed.structuralExclusion);
            if (!result.errors.empty()) return result;
            result.cursor.inner = parsed.cursor;
            const auto canonical = SerializeCursor(identity, validated, result.cursor);
            if (!canonical.errors.empty()) return canonical;
            if (canonical.encoded != encoded)
            { result.errors.emplace_back("v4_joined_cursor_encoding_noncanonical"); return result; }
            result.encoded = canonical.encoded;
        }
        catch (const std::bad_alloc&) { result.encoded.clear(); result.errors.emplace_back("v4_joined_resource_unavailable"); }
        catch (const std::length_error&) { result.encoded.clear(); result.errors.emplace_back("v4_joined_resource_unavailable"); }
        return result;
    }
};
// Independent V4 native conditional graph preview; no production receipt.
struct V4NativeJoinedGraphVariant
{
    std::string ordinal, id;
    std::vector<std::size_t> classes;
    std::vector<std::string> order, unresolved;
};
struct V4NativeJoinedGraphItem
{
    std::string ordinal, presenceOrdinal;
    std::vector<bool> present;
    V4NativeJoinedGraphVariant graph;
};
struct V4NativeJoinedGraphCursor { JoinedGraphCursor inner; };
struct V4NativeJoinedGraphPage
{
    std::string startOrdinal;
    std::vector<V4NativeJoinedGraphItem> graphs;
    std::vector<PresencePrefixPrune> presencePruned;
    std::vector<JoinedGraphPrune> activePruned;
    V4NativeJoinedGraphCursor cursor;
    bool exhausted = false, paused = false;
    bool resourceUnavailable = false;
    std::size_t workUsed = 0;
    std::vector<std::string> errors;
};
struct V4NativeJoinedGraphReplay
{
    bool identityValid = false;
    V4NativeJoinedGraphVariant variant;
    std::vector<std::string> errors;
    V4GraphReplayStatus status = V4GraphReplayStatus::unavailable;
};
struct V4NativeJoinedGraphCursorCodec
{
    V4NativeJoinedGraphCursor cursor;
    std::string encoded;
    std::vector<std::string> errors;
    bool resourceUnavailable = false;
};
struct V4NativeJoinedGraphDeferredPage
{
    V4NativeJoinedGraphCursor cursor;
    std::vector<PresencePrefixPrune> pruned;
    std::vector<bool> pendingPresent;
    std::string pendingOrdinal;
    std::vector<std::string> errors;
    std::size_t workUsed = 0;
    bool projectionRequired = false, exhausted = false, paused = false, resourceUnavailable = false;
};
class V4NativeJoinedGraph final
{
    friend class V4FamilyCandidateGraph;
    static std::string OuterId(std::string_view inner)
    {
        std::string id;
        AppendToken(id, "global_v4_native_joined_presence_variant_v13"); AppendToken(id, inner);
        return id;
    }
    static V4NativeJoinedGraphVariant Convert(const PresenceActiveGraphVariant& item)
    { return {item.ordinal, OuterId(item.id), item.classes, item.order, item.unresolved}; }
    // This only reconstructs a graph identity against a completed projection;
    // publication history and production authorization require separate proof.
    static V4NativeJoinedGraphReplay ReplayCompleted(const V4FamilyIdentityContext& identity,
        const V4FamilyValidatedContext& validated, const std::vector<bool>& present,
        const PresenceProjectionWork& work, std::string_view id)
    {
        V4NativeJoinedGraphReplay result;
        try
        {
            const auto mapping = MapV4NativePresenceCandidate(identity, validated);
            if (!mapping.graphAvailable)
            { if (mapping.resourceUnavailable) result.status = V4GraphReplayStatus::resource_unavailable;
              result.errors.push_back("v4_native_joined_mapping_unavailable:" + mapping.status); return result; }
            auto remaining = id; std::string_view token, inner;
            if (!ReadGraphToken(remaining, token) || token != "global_v4_native_joined_presence_variant_v13" ||
                !ReadGraphToken(remaining, inner) || !remaining.empty() || OuterId(inner) != id)
            { result.errors.emplace_back("v4_native_joined_graph_id_invalid"); return result; }
            const V4GraphKernelContext context(identity, validated, true);
            const auto replay = PresenceActiveGraph::ReplayCompleted(context, validated.Coordinate(), mapping.spec, present, work, inner);
            if (replay.resourceUnavailable) { result.status = V4GraphReplayStatus::resource_unavailable; return result; }
            result.errors = replay.errors;
            if (!replay.structuralExclusion.empty()) result.errors.push_back("v4_native_joined_structure_excluded:" + replay.structuralExclusion);
            result.identityValid = replay.identityValid && result.errors.empty();
            if (result.identityValid) { result.variant = Convert(replay.variant); result.status = V4GraphReplayStatus::valid; }
        }
        catch (const std::bad_alloc&) { result.identityValid = false; result.variant = {}; result.status = V4GraphReplayStatus::resource_unavailable; }
        catch (const std::length_error&) { result.identityValid = false; result.variant = {}; result.status = V4GraphReplayStatus::resource_unavailable; }
        return result;
    }
public:
    static V4NativeJoinedGraphDeferredPage AdvanceDeferred(const V4FamilyIdentityContext& identity,
        const V4FamilyValidatedContext& validated, const V4NativeJoinedGraphCursor& cursor, std::size_t workBudget)
    {
        V4NativeJoinedGraphDeferredPage out;
        try
        {
            out.cursor = cursor; const auto mapping = MapV4NativePresenceCandidate(identity, validated);
            if (!mapping.graphAvailable) { out.resourceUnavailable = mapping.resourceUnavailable; out.errors.push_back("v4_native_joined_mapping_unavailable:" + mapping.status); return out; }
            const V4GraphKernelContext context(identity, validated, true);
            const auto page = JoinedPresenceGraph::AdvanceDeferred(context, validated.Coordinate(), mapping.spec, cursor.inner, workBudget);
            out.resourceUnavailable = page.resourceUnavailable; out.errors = page.errors;
            if (!page.structuralExclusion.empty()) out.errors.push_back("v4_native_joined_structure_excluded:" + page.structuralExclusion);
            if (out.resourceUnavailable || !out.errors.empty()) return out;
            auto after = page.cursor; out.pruned = page.pruned; out.pendingPresent = page.pendingPresent;
            out.pendingOrdinal = page.pendingOrdinal; out.cursor.inner = std::move(after);
            out.workUsed = page.workUsed; out.projectionRequired = page.projectionRequired;
            out.exhausted = page.exhausted; out.paused = page.paused;
        }
        catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.workUsed = 0; out.pruned.clear(); out.pendingPresent.clear(); out.pendingOrdinal.clear(); out.projectionRequired = false; }
        catch (const std::length_error&) { out.resourceUnavailable = true; out.workUsed = 0; out.pruned.clear(); out.pendingPresent.clear(); out.pendingOrdinal.clear(); out.projectionRequired = false; }
        return out;
    }
    static std::unique_ptr<PresenceProjectionWork> CreateProjectionWork(const V4FamilyIdentityContext& identity,
        const V4FamilyValidatedContext& validated, const std::vector<bool>& present, std::size_t preambleBudget,
        bool& resourceUnavailable, std::vector<std::string>& errors)
    {
        resourceUnavailable = false; errors.clear();
        try
        {
            const auto mapping = MapV4NativePresenceCandidate(identity, validated);
            if (!mapping.graphAvailable) { resourceUnavailable = mapping.resourceUnavailable; errors.push_back("v4_native_joined_mapping_unavailable:" + mapping.status); return {}; }
            const V4GraphKernelContext context(identity, validated, true);
            return PresenceProjectionWork::Create(context, validated.Coordinate(), mapping.spec, present, preambleBudget, resourceUnavailable, errors);
        }
        catch (const std::bad_alloc&) { resourceUnavailable = true; return {}; }
        catch (const std::length_error&) { resourceUnavailable = true; return {}; }
    }
    static std::unique_ptr<PresenceProjectionRebuild> RebuildProjection(const V4FamilyIdentityContext& identity,
        const V4FamilyValidatedContext& validated, const std::vector<bool>& present, std::string_view encodedCursor,
        std::size_t preambleBudget, bool& resourceUnavailable, std::vector<std::string>& errors)
    {
        resourceUnavailable = false; errors.clear();
        try
        {
            const auto mapping = MapV4NativePresenceCandidate(identity, validated);
            if (!mapping.graphAvailable) { resourceUnavailable = mapping.resourceUnavailable; errors.push_back("v4_native_joined_mapping_unavailable:" + mapping.status); return {}; }
            const V4GraphKernelContext context(identity, validated, true);
            return PresenceProjectionRebuild::Create(context, validated.Coordinate(), mapping.spec, present, encodedCursor,
                preambleBudget, resourceUnavailable, errors);
        }
        catch (const std::bad_alloc&) { resourceUnavailable = true; return {}; }
        catch (const std::length_error&) { resourceUnavailable = true; return {}; }
    }
    static V4NativeJoinedGraphPage AttachCompletedProjection(const V4FamilyIdentityContext& identity,
        const V4FamilyValidatedContext& validated, const V4NativeJoinedGraphCursor& cursor, const PresenceProjectionWork& work)
    {
        V4NativeJoinedGraphPage out;
        try
        {
            out.cursor = cursor; const auto mapping = MapV4NativePresenceCandidate(identity, validated);
            if (!mapping.graphAvailable) { out.resourceUnavailable = mapping.resourceUnavailable; out.errors.push_back("v4_native_joined_mapping_unavailable:" + mapping.status); return out; }
            const V4GraphKernelContext context(identity, validated, true);
            const auto page = JoinedPresenceGraph::AttachCompletedProjection(context, validated.Coordinate(), mapping.spec, cursor.inner, work);
            out.resourceUnavailable = page.resourceUnavailable; out.errors = page.errors;
            if (!page.structuralExclusion.empty()) out.errors.push_back("v4_native_joined_structure_excluded:" + page.structuralExclusion);
            if (out.resourceUnavailable || !out.errors.empty()) return out;
            auto after = page.cursor; out.cursor.inner = std::move(after);
            out.workUsed = page.workUsed; out.paused = page.paused; out.exhausted = page.exhausted;
        }
        catch (const std::bad_alloc&) { out.resourceUnavailable = true; out.workUsed = 0; }
        catch (const std::length_error&) { out.resourceUnavailable = true; out.workUsed = 0; }
        return out;
    }
    static V4NativeJoinedGraphPage EnumerateCompletedProjection(const V4FamilyIdentityContext& identity,
        const V4FamilyValidatedContext& validated, const V4NativeJoinedGraphCursor& cursor, const PresenceProjectionWork& work,
        std::size_t workBudget, std::size_t outputLimit)
    {
        V4NativeJoinedGraphPage out;
        try
        {
            out.cursor = cursor; const auto mapping = MapV4NativePresenceCandidate(identity, validated);
            if (!mapping.graphAvailable) { out.resourceUnavailable = mapping.resourceUnavailable; out.errors.push_back("v4_native_joined_mapping_unavailable:" + mapping.status); return out; }
            const V4GraphKernelContext context(identity, validated, true);
            const auto page = JoinedPresenceGraph::EnumerateCompletedProjection(context, validated.Coordinate(), mapping.spec,
                cursor.inner, work, workBudget, outputLimit);
            out.resourceUnavailable = page.resourceUnavailable; out.errors = page.errors;
            if (!page.structuralExclusion.empty()) out.errors.push_back("v4_native_joined_structure_excluded:" + page.structuralExclusion);
            if (out.resourceUnavailable || !out.errors.empty()) return out;
            std::vector<V4NativeJoinedGraphItem> converted; converted.reserve(page.graphs.size());
            for (const auto& item : page.graphs) converted.push_back({item.ordinal, item.presenceOrdinal, item.present, Convert(item.graph)});
            auto after = page.cursor; out.startOrdinal = page.startOrdinal; out.graphs = std::move(converted);
            out.activePruned = page.activePruned; out.cursor.inner = std::move(after);
            out.workUsed = page.workUsed; out.paused = page.paused; out.exhausted = page.exhausted;
        }
        catch (const std::bad_alloc&) { out.graphs.clear(); out.activePruned.clear(); out.workUsed = 0; out.resourceUnavailable = true; }
        catch (const std::length_error&) { out.graphs.clear(); out.activePruned.clear(); out.workUsed = 0; out.resourceUnavailable = true; }
        return out;
    }
    static V4NativeJoinedGraphCursorCodec SerializeCompletedCursor(const V4FamilyIdentityContext& identity,
        const V4FamilyValidatedContext& validated, const V4NativeJoinedGraphCursor& cursor, const PresenceProjectionWork& work)
    {
        V4NativeJoinedGraphCursorCodec out;
        try
        {
            out.cursor = cursor; const auto mapping = MapV4NativePresenceCandidate(identity, validated);
            if (!mapping.graphAvailable) { out.resourceUnavailable = mapping.resourceUnavailable; out.errors.push_back("v4_native_joined_mapping_unavailable:" + mapping.status); return out; }
            const V4GraphKernelContext context(identity, validated, true);
            const auto saved = JoinedPresenceGraph::SerializeCompletedCursor(context, validated.Coordinate(), mapping.spec, cursor.inner, work);
            out.resourceUnavailable = saved.resourceUnavailable; out.errors = saved.errors;
            if (!saved.structuralExclusion.empty()) out.errors.push_back("v4_native_joined_structure_excluded:" + saved.structuralExclusion);
            if (out.resourceUnavailable || !out.errors.empty()) return out;
            std::string encoded; AppendToken(encoded, "v4_native_joined_presence_cursor_encoding_v13"); AppendToken(encoded, saved.encoded);
            auto after = saved.cursor; out.cursor.inner = std::move(after); out.encoded = std::move(encoded);
        }
        catch (const std::bad_alloc&) { out.encoded.clear(); out.resourceUnavailable = true; }
        catch (const std::length_error&) { out.encoded.clear(); out.resourceUnavailable = true; }
        return out;
    }
    static V4NativeJoinedGraphCursorCodec ParseCompletedCursor(const V4FamilyIdentityContext& identity,
        const V4FamilyValidatedContext& validated, std::string_view encoded, const PresenceProjectionWork& work)
    {
        V4NativeJoinedGraphCursorCodec out;
        try
        {
            const auto mapping = MapV4NativePresenceCandidate(identity, validated);
            if (!mapping.graphAvailable) { out.resourceUnavailable = mapping.resourceUnavailable; out.errors.push_back("v4_native_joined_mapping_unavailable:" + mapping.status); return out; }
            auto remaining = encoded; std::string_view token, inner;
            if (!ReadGraphToken(remaining, token) || token != "v4_native_joined_presence_cursor_encoding_v13" ||
                !ReadGraphToken(remaining, inner) || !remaining.empty())
            { out.errors.emplace_back("v4_native_joined_cursor_version_or_encoding_invalid"); return out; }
            const V4GraphKernelContext context(identity, validated, true);
            const auto parsed = JoinedPresenceGraph::ParseCompletedCursor(context, validated.Coordinate(), mapping.spec, inner, work);
            out.resourceUnavailable = parsed.resourceUnavailable; out.errors = parsed.errors;
            if (!parsed.structuralExclusion.empty()) out.errors.push_back("v4_native_joined_structure_excluded:" + parsed.structuralExclusion);
            if (out.resourceUnavailable || !out.errors.empty()) return out;
            out.cursor.inner = parsed.cursor;
            const auto canonical = SerializeCompletedCursor(identity, validated, out.cursor, work);
            if (canonical.resourceUnavailable || !canonical.errors.empty()) return canonical;
            if (canonical.encoded != encoded) { out.errors.emplace_back("v4_native_joined_cursor_encoding_noncanonical"); return out; }
            out.encoded = canonical.encoded;
        }
        catch (const std::bad_alloc&) { out.encoded.clear(); out.resourceUnavailable = true; }
        catch (const std::length_error&) { out.encoded.clear(); out.resourceUnavailable = true; }
        return out;
    }
    // The facade always regenerates the Spec from this exact authority. It
    // never accepts a caller-supplied Spec or a legacy graph context.
    static V4NativeJoinedGraphPage Enumerate(const V4FamilyIdentityContext& identity,
        const V4FamilyValidatedContext& validated,
        V4NativeJoinedGraphCursor cursor, std::size_t workBudget, std::size_t outputLimit)
    {
        V4NativeJoinedGraphPage result;
        result.cursor = std::move(cursor);
        try
        {
            const auto mapping = MapV4NativePresenceCandidate(identity, validated);
            if (!mapping.graphAvailable)
            {
                result.resourceUnavailable = mapping.resourceUnavailable;
                result.errors.push_back("v4_native_joined_mapping_unavailable:" + mapping.status);
                result.errors.insert(result.errors.end(), mapping.reasons.begin(), mapping.reasons.end());
                return result;
            }
            const V4GraphKernelContext context(identity, validated, true);
            auto page = JoinedPresenceGraph::Enumerate(context, validated.Coordinate(), mapping.spec,
                result.cursor.inner, workBudget, outputLimit);
            if (page.resourceUnavailable) { result.resourceUnavailable = true; result.paused = true; return result; }
            result.errors = page.errors;
            if (!page.structuralExclusion.empty()) result.errors.push_back("v4_native_joined_structure_excluded:" + page.structuralExclusion);
            if (!result.errors.empty()) return result;
            std::vector<V4NativeJoinedGraphItem> converted;
            converted.reserve(page.graphs.size());
            for (const auto& item : page.graphs)
                converted.push_back({item.ordinal, item.presenceOrdinal, item.present, Convert(item.graph)});
            result.startOrdinal = std::move(page.startOrdinal); result.cursor.inner = std::move(page.cursor);
            result.presencePruned = std::move(page.presencePruned); result.activePruned = std::move(page.activePruned);
            result.exhausted = page.exhausted; result.paused = page.paused; result.workUsed = page.workUsed;
            result.graphs = std::move(converted);
        }
        catch (const std::bad_alloc&) { result.graphs.clear(); result.exhausted = false; result.paused = true; result.resourceUnavailable = true; }
        catch (const std::length_error&) { result.graphs.clear(); result.exhausted = false; result.paused = true; result.resourceUnavailable = true; }
        return result;
    }
    static V4NativeJoinedGraphReplay Replay(const V4FamilyIdentityContext& identity,
        const V4FamilyValidatedContext& validated,
        const std::vector<bool>& present, std::string_view id)
    {
        V4NativeJoinedGraphReplay result;
        try
        {
            const auto mapping = MapV4NativePresenceCandidate(identity, validated);
            if (!mapping.graphAvailable)
            { if (mapping.resourceUnavailable) result.status = V4GraphReplayStatus::resource_unavailable;
              result.errors.push_back("v4_native_joined_mapping_unavailable:" + mapping.status); return result; }
            auto remaining = id; std::string_view token, inner;
            if (!ReadGraphToken(remaining, token) || token != "global_v4_native_joined_presence_variant_v13" ||
                !ReadGraphToken(remaining, inner) || !remaining.empty() || OuterId(inner) != id)
            { result.errors.emplace_back("v4_native_joined_graph_id_invalid"); return result; }
            const V4GraphKernelContext context(identity, validated, true);
            const auto replay = PresenceActiveGraph::Replay(context, validated.Coordinate(), mapping.spec, present, inner);
            if (replay.resourceUnavailable) { result.status = V4GraphReplayStatus::resource_unavailable; return result; }
            result.errors = replay.errors;
            if (!replay.structuralExclusion.empty()) result.errors.push_back("v4_native_joined_structure_excluded:" + replay.structuralExclusion);
            result.identityValid = replay.identityValid && result.errors.empty();
            if (result.identityValid) { result.variant = Convert(replay.variant); result.status = V4GraphReplayStatus::valid; }
        }
        catch (const std::bad_alloc&) { result.identityValid = false; result.variant = {}; result.status = V4GraphReplayStatus::resource_unavailable; }
        catch (const std::length_error&) { result.identityValid = false; result.variant = {}; result.status = V4GraphReplayStatus::resource_unavailable; }
        return result;
    }
    static V4NativeJoinedGraphCursorCodec SerializeCursor(const V4FamilyIdentityContext& identity,
        const V4FamilyValidatedContext& validated, V4NativeJoinedGraphCursor cursor)
    {
        V4NativeJoinedGraphCursorCodec result;
        result.cursor = std::move(cursor);
        try
        {
            const auto mapping = MapV4NativePresenceCandidate(identity, validated);
            if (!mapping.graphAvailable)
            { result.resourceUnavailable = mapping.resourceUnavailable; result.errors.push_back("v4_native_joined_mapping_unavailable:" + mapping.status); return result; }
            const V4GraphKernelContext context(identity, validated, true);
            auto saved = JoinedPresenceGraph::SerializeCursor(context, validated.Coordinate(), mapping.spec, result.cursor.inner);
            result.errors = saved.errors;
            if (!saved.structuralExclusion.empty()) result.errors.push_back("v4_native_joined_structure_excluded:" + saved.structuralExclusion);
            if (!result.errors.empty()) return result;
            AppendToken(result.encoded, "v4_native_joined_presence_cursor_encoding_v13"); AppendToken(result.encoded, saved.encoded);
            result.cursor.inner = std::move(saved.cursor);
        }
        catch (const std::bad_alloc&) { result.encoded.clear(); result.resourceUnavailable = true; result.errors.emplace_back("v4_native_joined_resource_unavailable"); }
        catch (const std::length_error&) { result.encoded.clear(); result.resourceUnavailable = true; result.errors.emplace_back("v4_native_joined_resource_unavailable"); }
        return result;
    }
    static V4NativeJoinedGraphCursorCodec ParseCursor(const V4FamilyIdentityContext& identity,
        const V4FamilyValidatedContext& validated, std::string_view encoded)
    {
        V4NativeJoinedGraphCursorCodec result;
        try
        {
            const auto mapping = MapV4NativePresenceCandidate(identity, validated);
            if (!mapping.graphAvailable)
            { result.resourceUnavailable = mapping.resourceUnavailable; result.errors.push_back("v4_native_joined_mapping_unavailable:" + mapping.status); return result; }
            auto remaining = encoded; std::string_view token, inner;
            if (!ReadGraphToken(remaining, token))
            { result.errors.emplace_back("v4_native_joined_cursor_encoding_invalid"); return result; }
            if (token != "v4_native_joined_presence_cursor_encoding_v13")
            { result.errors.emplace_back("v4_native_joined_cursor_version_unsupported"); return result; }
            if (!ReadGraphToken(remaining, inner) || !remaining.empty())
            { result.errors.emplace_back("v4_native_joined_cursor_encoding_invalid"); return result; }
            const V4GraphKernelContext context(identity, validated, true);
            const auto parsed = JoinedPresenceGraph::ParseCursor(context, validated.Coordinate(), mapping.spec, inner);
            result.errors = parsed.errors;
            if (!parsed.structuralExclusion.empty()) result.errors.push_back("v4_native_joined_structure_excluded:" + parsed.structuralExclusion);
            if (!result.errors.empty()) return result;
            result.cursor.inner = parsed.cursor;
            const auto canonical = SerializeCursor(identity, validated, result.cursor);
            if (!canonical.errors.empty()) return canonical;
            if (canonical.encoded != encoded)
            { result.errors.emplace_back("v4_native_joined_cursor_encoding_noncanonical"); return result; }
            result.encoded = canonical.encoded;
        }
        catch (const std::bad_alloc&) { result.encoded.clear(); result.resourceUnavailable = true; result.errors.emplace_back("v4_native_joined_resource_unavailable"); }
        catch (const std::length_error&) { result.encoded.clear(); result.resourceUnavailable = true; result.errors.emplace_back("v4_native_joined_resource_unavailable"); }
        return result;
    }
};
// Incremental prefix-legal candidate scan for the same global Model. This
// traversal never calls ConditionalSpace::Count/DecodeAt. Non-derived axes
// include outcome hypotheses: enumerating them does not set a source input or
// claim a production outcome. Prefix rejections are branch attempts, not
// counts of full excluded candidates.
struct CandidateScanCursor
{
    std::string version = "global_candidate_prefix_dfs_v1";
    std::string binding, integrity, nextOrdinal = "0";
    bool exhausted = false;
    std::vector<std::size_t> prefix, next{0};
    std::string rejectedPrefixes = "0";
};
struct CandidatePrefixPrune
{
    std::string axisId, value;
    std::vector<std::size_t> prefix;
};
struct CandidateScanPage
{
    std::optional<Candidate> candidate;
    std::string structuralExclusion;
    std::vector<CandidatePrefixPrune> pruned;
    CandidateScanCursor cursor;
    bool exhausted = false, paused = false;
    std::size_t workUsed = 0;
    std::vector<std::string> errors;
};
// Candidate scanning shares the same versioned context; graph execution remains gated.
using CandidateTraversalContext = Context;
inline std::string CandidateScanBinding(const CandidateTraversalContext& context)
{
    std::string binding;
    // Bump this version whenever prefix applicability or axis traversal
    // semantics change; a changed schema is separately encoded below.
    AppendToken(binding, "global_candidate_prefix_dfs_schema_v1");
    AppendToken(binding, ExistingSelectionVersionTag(context.version));
    AppendToken(binding, context.schemaBinding);
    return binding;
}
inline std::string CandidateScanIntegrity(const CandidateScanCursor& cursor)
{
    std::string bytes;
    AppendToken(bytes, cursor.version); AppendToken(bytes, cursor.binding); AppendToken(bytes, cursor.nextOrdinal);
    AppendToken(bytes, cursor.exhausted ? "true" : "false"); AppendToken(bytes, cursor.rejectedPrefixes);
    for (const auto* values : {&cursor.prefix, &cursor.next})
    {
        AppendToken(bytes, std::to_string(values->size()));
        for (const auto value : *values) AppendToken(bytes, std::to_string(value));
    }
    std::uint64_t hash = 14695981039346656037ull;
    for (const char c : bytes) { hash ^= static_cast<unsigned char>(c); hash *= 1099511628211ull; }
    return std::to_string(hash); // Accidental corruption only, not authentication.
}
class V3FamilyCandidateGraph;
class V4FamilyCandidateGraph;
class CandidateScanKernel final
{
    template <typename PrefixRule, typename ExclusionRule>
    static CandidateScanPage Scan(const std::vector<Axis>& axes,
        const std::shared_ptr<const std::unordered_map<std::string, std::size_t>>& lookup,
        std::string_view binding, const PrefixRule& prefixAllows, const ExclusionRule& exclusion,
        CandidateScanCursor cursor, std::size_t workBudget);
    friend CandidateScanPage ScanNextCandidate(const CandidateTraversalContext&, CandidateScanCursor, std::size_t);
    friend class V3FamilyCandidateGraph;
    friend class V4FamilyCandidateGraph;
};
inline CandidateScanPage ScanNextCandidate(const CandidateTraversalContext& supplied, CandidateScanCursor cursor, std::size_t workBudget)
{
    CandidateScanPage page;
    page.cursor = std::move(cursor);
    // Rebuild public Context fields before using cached schema or lookup.
    const CandidateTraversalContext context(supplied.version, supplied.axes);
    page.errors = context.errors;
    if (!supplied.errors.empty()) page.errors.insert(page.errors.end(), supplied.errors.begin(), supplied.errors.end());
    if (supplied.schemaBinding != context.schemaBinding || !supplied.lookup ||
        (supplied.lookup && *supplied.lookup != *context.lookup)) page.errors.emplace_back("candidate_scan_context_mismatch");
    if (!page.errors.empty()) return page;
    const auto prefix = [&](const Candidate& candidate, std::string_view id, std::string_view value) {
        return ExistingPrefixAllows(context, candidate, id, value);
    };
    const auto exclusion = [&](const Candidate& candidate) { return ExistingStructuralExclusion(context, candidate); };
    return CandidateScanKernel::Scan(context.axes, context.lookup, CandidateScanBinding(context), prefix, exclusion,
        std::move(page.cursor), workBudget);
}

template <typename PrefixRule, typename ExclusionRule>
inline CandidateScanPage CandidateScanKernel::Scan(const std::vector<Axis>& axes,
    const std::shared_ptr<const std::unordered_map<std::string, std::size_t>>& lookup,
    std::string_view binding, const PrefixRule& prefixAllows, const ExclusionRule& exclusion,
    CandidateScanCursor cursor, std::size_t workBudget)
{
    CandidateScanPage page;
    page.cursor = std::move(cursor);
    auto& state = page.cursor;
    if (state.version != "global_candidate_prefix_dfs_v1")
    { page.errors.emplace_back("candidate_scan_cursor_version_invalid"); return page; }
    try
    {
        if (Normalize(state.nextOrdinal) != state.nextOrdinal || Normalize(state.rejectedPrefixes) != state.rejectedPrefixes)
            page.errors.emplace_back("candidate_scan_decimal_invalid");
    }
    catch (const std::exception&) { page.errors.emplace_back("candidate_scan_decimal_invalid"); }
    if (state.binding.empty())
    {
        if (!state.integrity.empty() || state.nextOrdinal != "0" || state.rejectedPrefixes != "0" ||
            state.exhausted || !state.prefix.empty() || state.next != std::vector<std::size_t>{0})
            page.errors.emplace_back("candidate_scan_initial_state_invalid");
        else state.binding = std::string(binding);
    }
    else if (state.binding != binding) page.errors.emplace_back("candidate_scan_binding_mismatch");
    else if (state.integrity.empty() || state.integrity != CandidateScanIntegrity(state))
        page.errors.emplace_back("candidate_scan_integrity_invalid");
    if (!page.errors.empty()) return page;
    std::vector<std::size_t> inputs;
    for (std::size_t i = 0; i < axes.size(); ++i)
        if (axes[i].role != Role::Derived) inputs.push_back(i);
    Candidate partial{"", std::vector<std::string>(axes.size()), lookup};
    if (state.exhausted)
    {
        if (!state.prefix.empty() || !state.next.empty()) page.errors.emplace_back("candidate_scan_done_state_invalid");
        else page.exhausted = true;
        return page;
    }
    if (state.prefix.size() > inputs.size() || state.next.size() != state.prefix.size() + 1)
    { page.errors.emplace_back("candidate_scan_stack_invalid"); return page; }
    for (std::size_t i = 0; i < state.prefix.size(); ++i)
    {
        const auto& axis = axes[inputs[i]];
        const auto choice = state.prefix[i];
        if (choice >= axis.values.size() || state.next[i] != choice + 1 ||
            !prefixAllows(partial, axis.id, axis.values[choice]))
        { page.errors.emplace_back("candidate_scan_prefix_invalid"); return page; }
        partial.values[inputs[i]] = axis.values[choice];
    }
    if (state.next.back() > (state.prefix.size() == inputs.size() ? 0 : axes[inputs[state.prefix.size()]].values.size()))
    { page.errors.emplace_back("candidate_scan_choice_invalid"); return page; }
    while (page.workUsed < workBudget && !state.exhausted && !page.candidate)
    {
        ++page.workUsed;
        if (state.prefix.size() == inputs.size())
        {
            partial.ordinal = state.nextOrdinal;
            page.candidate = partial;
            page.structuralExclusion = exclusion(partial);
            Increment(state.nextOrdinal);
            if (state.prefix.empty()) { state.next.clear(); state.exhausted = true; }
            else
            {
                partial.values[inputs[state.prefix.size() - 1]].clear();
                state.prefix.pop_back(); state.next.pop_back();
            }
            continue;
        }
        const auto axisIndex = inputs[state.prefix.size()];
        const auto& axis = axes[axisIndex];
        auto& next = state.next.back();
        if (next >= axis.values.size())
        {
            if (state.prefix.empty()) { state.next.clear(); state.exhausted = true; }
            else
            {
                partial.values[inputs[state.prefix.size() - 1]].clear();
                state.prefix.pop_back(); state.next.pop_back();
            }
            continue;
        }
        const auto choice = next++;
        if (!prefixAllows(partial, axis.id, axis.values[choice]))
        {
            auto rejected = state.prefix; rejected.push_back(choice);
            page.pruned.push_back({axis.id, axis.values[choice], std::move(rejected)});
            Increment(state.rejectedPrefixes);
        }
        else
        {
            partial.values[axisIndex] = axis.values[choice];
            state.prefix.push_back(choice); state.next.push_back(0);
        }
    }
    page.exhausted = state.exhausted;
    page.paused = !state.exhausted;
    state.integrity = CandidateScanIntegrity(state);
    return page;
}
struct CandidateScanCodec
{
    CandidateScanCursor cursor;
    std::string encoded;
    std::vector<std::string> errors;
};
inline void EncodeCandidateScanState(std::string& out, const CandidateScanCursor& state, std::string_view encoding)
{
    AppendToken(out, encoding);
    AppendToken(out, state.version); AppendToken(out, state.binding); AppendToken(out, state.integrity);
    AppendToken(out, state.nextOrdinal); AppendToken(out, state.rejectedPrefixes);
    AppendToken(out, state.exhausted ? "true" : "false");
    for (const auto* values : {&state.prefix, &state.next})
    {
        AppendToken(out, std::to_string(values->size()));
        for (const auto value : *values) AppendToken(out, std::to_string(value));
    }
}
inline CandidateScanCodec SerializeCandidateScanCursor(const CandidateTraversalContext& context, CandidateScanCursor cursor)
{
    CandidateScanCodec result;
    const auto checked = ScanNextCandidate(context, std::move(cursor), 0);
    result.cursor = checked.cursor; result.errors = checked.errors;
    if (!result.errors.empty()) return result;
    EncodeCandidateScanState(result.encoded, result.cursor, "global_candidate_prefix_cursor_encoding_v1");
    return result;
}
inline CandidateScanCodec DecodeCandidateScanState(std::string_view encoded, std::string_view encoding)
{
    CandidateScanCodec result;
    auto remaining = encoded;
    std::string_view token;
    const auto read = [&] { return ReadGraphToken(remaining, token); };
    const auto fail = [&] { result.errors.emplace_back("candidate_scan_cursor_encoding_invalid"); return result; };
    if (!read() || token != encoding) return fail();
    auto& cursor = result.cursor;
    if (!read()) return fail();
    cursor.version = std::string(token);
    if (!read()) return fail();
    cursor.binding = std::string(token);
    if (!read()) return fail();
    cursor.integrity = std::string(token);
    if (!read()) return fail();
    cursor.nextOrdinal = std::string(token);
    if (!read()) return fail();
    cursor.rejectedPrefixes = std::string(token);
    if (!read() || (token != "true" && token != "false")) return fail();
    cursor.exhausted = token == "true";
    const auto vector = [&](std::vector<std::size_t>& values) {
        std::size_t count = 0;
        if (!read() || !ParseCanonicalSize(token, count) || count > remaining.size() / 2) return false;
        values.clear(); values.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            std::size_t value = 0;
            if (!read() || !ParseCanonicalSize(token, value)) return false;
            values.push_back(value);
        }
        return true;
    };
    if (!vector(cursor.prefix) || !vector(cursor.next) || !remaining.empty()) return fail();
    return result;
}
inline CandidateScanCodec ParseCandidateScanCursor(const CandidateTraversalContext& context, std::string_view encoded)
{
    auto decoded = DecodeCandidateScanState(encoded, "global_candidate_prefix_cursor_encoding_v1");
    if (!decoded.errors.empty()) return decoded;
    const auto canonical = SerializeCandidateScanCursor(context, std::move(decoded.cursor));
    if (!canonical.errors.empty()) return canonical;
    if (canonical.encoded != encoded) { decoded.errors.emplace_back("candidate_scan_cursor_encoding_noncanonical"); return decoded; }
    return canonical;
}

inline constexpr std::string_view V3FamilyCandidateGraphAlgorithm = "v3_family_candidate_joined_dfs_v3";
struct V3FamilyCandidateCursor
{
    std::string version = "v3_family_candidate_joined_cursor_v3", binding, integrity;
    CandidateScanCursor scan, before;
    bool active = false, exhausted = false;
    std::string candidateId, candidateOrdinal, joinedCursor, graphCount = "0";
};
struct V3FamilyCandidateRecord
{
    std::string kind, candidateOrdinal, candidateId, reason;
    std::vector<std::string> unresolved;
    std::optional<V3JoinedGraphItem> graph;
    std::optional<Candidate> coordinate; // domain-checked, applicability still unresolved
};
enum class V3FamilyTraversalStatus { ready, invalid, resource_unavailable, schema_error, unavailable };
struct V3FamilyPresencePrune
{
    std::string candidateOrdinal, candidateId;
    PresencePrefixPrune prune;
};
struct V3FamilyActivePrune
{
    std::string candidateOrdinal, candidateId;
    JoinedGraphPrune prune;
};
struct V3FamilyCandidateStep
{
    V3FamilyTraversalStatus status = V3FamilyTraversalStatus::invalid;
    V3FamilyCandidateCursor cursor;
    std::optional<V3FamilyCandidateRecord> record;
    // Prefix/partition rejections are not counts of complete product cases.
    // Durable callers must commit these records with the returned cursor.
    std::vector<V3FamilyPresencePrune> presencePruned;
    std::vector<V3FamilyActivePrune> activePruned;
    bool exhausted = false, paused = false;
    std::size_t workUsed = 0;
    std::vector<std::string> errors;
};
struct V3FamilyCandidateCodec
{
    V3FamilyTraversalStatus status = V3FamilyTraversalStatus::invalid;
    V3FamilyCandidateCursor cursor;
    std::string encoded;
    std::vector<std::string> errors;
};

// One family slice for a fair outer scheduler. This uses the same prefix DFS
// kernel with a conservative accept-all V3 prefix rule; applicability that is
// not proved is reported, never pruned. Returned records are unexecuted graph
// hypotheses or explicit mapping-unimplemented results, never product cases.
class V3FamilyCandidateGraph final
{
    static std::string Binding(const V3FamilyIdentityContext& identity)
    {
        std::string result;
        AppendToken(result, V3FamilyCandidateGraphAlgorithm);
        AppendToken(result, V3FamilyCoordinateAlgorithm);
        AppendToken(result, identity.FamilyBinding()); AppendToken(result, identity.SchemaBinding());
        return result;
    }
    static std::string ScanBinding(const V3FamilyIdentityContext& identity)
    {
        std::string result; AppendToken(result, "v3_family_raw_prefix_dfs_v1");
        AppendToken(result, Binding(identity)); return result;
    }
    static std::string ScanBytes(const CandidateScanCursor& state)
    { std::string bytes; EncodeCandidateScanState(bytes, state, "v3_family_scan_state_v1"); return bytes; }
    static std::string Payload(const V3FamilyCandidateCursor& state)
    {
        std::string bytes;
        AppendToken(bytes, state.version); AppendToken(bytes, state.binding);
        AppendToken(bytes, ScanBytes(state.scan)); AppendToken(bytes, ScanBytes(state.before));
        AppendToken(bytes, state.active ? "1" : "0"); AppendToken(bytes, state.exhausted ? "1" : "0");
        AppendToken(bytes, state.candidateId); AppendToken(bytes, state.candidateOrdinal);
        AppendToken(bytes, state.joinedCursor); AppendToken(bytes, state.graphCount);
        return bytes;
    }
    static std::string Integrity(const V3FamilyCandidateCursor& state)
    {
        std::uint64_t hash = 14695981039346656037ull;
        for (const char c : Payload(state)) { hash ^= static_cast<unsigned char>(c); hash *= 1099511628211ull; }
        return std::to_string(hash); // corruption check, not authentication or history proof
    }
    static CandidateScanPage Scan(const V3FamilyIdentityContext& identity, CandidateScanCursor state, std::size_t budget)
    {
        const auto keep = [](const Candidate&, std::string_view, std::string_view) { return true; };
        const auto noUnprovedExclusion = [](const Candidate&) { return std::string{}; };
        return CandidateScanKernel::Scan(identity.Axes(), identity.Lookup(), ScanBinding(identity), keep,
            noUnprovedExclusion, std::move(state), budget);
    }
    static bool Validate(const V3FamilyIdentityContext& identity, const V3FamilyDescriptor& family,
        std::size_t memberBudget, V3FamilyCandidateCursor& state, std::vector<std::string>& errors,
        std::optional<Candidate>& activeCandidate, V3FamilyTraversalStatus& status)
    {
        if (identity.Status() != V3FamilyStatus::schema_only || identity.FamilyBinding() != V3FamilyBinding(family))
        { errors.emplace_back("v3_family_candidate_context_unavailable"); return false; }
        if (state.version != "v3_family_candidate_joined_cursor_v3")
        { errors.emplace_back("v3_family_candidate_cursor_version_invalid"); return false; }
        try { if (Normalize(state.graphCount) != state.graphCount) errors.emplace_back("v3_family_graph_count_invalid"); }
        catch (const std::exception&) { errors.emplace_back("v3_family_graph_count_invalid"); }
        const auto binding = Binding(identity);
        if (state.binding.empty())
        {
            if (!state.integrity.empty() || state.active || state.exhausted || !state.candidateId.empty() ||
                !state.candidateOrdinal.empty() || !state.joinedCursor.empty() || state.graphCount != "0" ||
                ScanBytes(state.scan) != ScanBytes(CandidateScanCursor{}) || ScanBytes(state.before) != ScanBytes(CandidateScanCursor{}))
                errors.emplace_back("v3_family_candidate_initial_state_invalid");
            else state.binding = binding;
        }
        else if (state.binding != binding) errors.emplace_back("v3_family_candidate_cursor_binding_mismatch");
        else if (state.integrity.empty() || state.integrity != Integrity(state)) errors.emplace_back("v3_family_candidate_cursor_integrity_invalid");
        if (!errors.empty()) return false;
        const auto checked = Scan(identity, state.scan, 0);
        if (!checked.errors.empty()) { errors = checked.errors; return false; }
        state.scan = checked.cursor;
        if (!state.active)
        {
            if (!state.candidateId.empty() || !state.candidateOrdinal.empty() || !state.joinedCursor.empty() ||
                state.graphCount != "0" || ScanBytes(state.before) != ScanBytes(CandidateScanCursor{}) ||
                state.exhausted != state.scan.exhausted)
            { errors.emplace_back("v3_family_candidate_inactive_state_invalid"); return false; }
            return true;
        }
        if (state.exhausted || state.candidateId.empty() || state.candidateOrdinal.empty() || state.joinedCursor.empty())
        { errors.emplace_back("v3_family_candidate_active_state_invalid"); return false; }
        const auto previous = Scan(identity, state.before, 1);
        if (!previous.errors.empty() || !previous.candidate || ScanBytes(previous.cursor) != ScanBytes(state.scan) ||
            previous.candidate->ordinal != state.candidateOrdinal)
        { errors.emplace_back("v3_family_candidate_active_history_invalid"); return false; }
        const auto raw = identity.RawCandidateIdentity(*previous.candidate);
        if (raw.resourceUnavailable)
        { status = V3FamilyTraversalStatus::resource_unavailable; errors.emplace_back("v3_family_active_identity_resource_unavailable"); return false; }
        if (!raw.errors.empty() || raw.id != state.candidateId)
        { errors.emplace_back("v3_family_candidate_active_identity_invalid"); return false; }
        auto verified = V3FamilyValidatedContext::Create(family, memberBudget, *previous.candidate);
        if (verified.Status() != V3CoordinateStatus::coordinate_ready)
        {
            status = verified.Status() == V3CoordinateStatus::resource_unavailable ? V3FamilyTraversalStatus::resource_unavailable :
                verified.Status() == V3CoordinateStatus::schema_error ? V3FamilyTraversalStatus::schema_error : V3FamilyTraversalStatus::invalid;
            errors.push_back("v3_family_candidate_active_context_unavailable:" + verified.Reason()); return false;
        }
        const auto parsed = V3JoinedGraph::ParseCursor(identity, verified, verified.TransferProgram(), state.joinedCursor);
        if (!parsed.errors.empty()) { status = V3FamilyTraversalStatus::unavailable; errors = parsed.errors; return false; }
        if (parsed.cursor.inner.nextOrdinal != state.graphCount)
        { errors.emplace_back("v3_family_active_graph_count_mismatch"); return false; }
        activeCandidate = *previous.candidate;
        return true;
    }
public:
    // Work units cover candidate DFS and joined graph steps. Schema and
    // authoritative binding reconstruction are finite preambles; callers
    // atomically commit each emitted record and its returned cursor.
    static V3FamilyCandidateStep Step(const V3FamilyDescriptor& family, std::size_t memberBudget,
        V3FamilyCandidateCursor cursor, std::size_t workBudget)
    {
        V3FamilyCandidateStep result;
        result.cursor = std::move(cursor);
        try
        {
            auto state = result.cursor;
            std::vector<V3FamilyPresencePrune> stagedPresencePrunes;
            std::vector<V3FamilyActivePrune> stagedActivePrunes;
            const auto identity = V3FamilyIdentityContext::Create(family, memberBudget);
            if (identity.Status() != V3FamilyStatus::schema_only)
            {
                result.status = identity.Status() == V3FamilyStatus::resource_unavailable ? V3FamilyTraversalStatus::resource_unavailable :
                    identity.Status() == V3FamilyStatus::schema_error ? V3FamilyTraversalStatus::schema_error : V3FamilyTraversalStatus::invalid;
                result.errors.push_back("v3_family_schema_unavailable:" + identity.Reason()); return result;
            }
            std::optional<Candidate> active;
            if (!Validate(identity, family, memberBudget, state, result.errors, active, result.status)) return result;
            while (result.workUsed < workBudget && !state.exhausted && !result.record)
            {
                ++result.workUsed;
                if (!state.active)
                {
                    const auto before = state.scan;
                    const auto page = Scan(identity, before, 1);
                    if (!page.errors.empty()) { result.errors = page.errors; return result; }
                    state.scan = page.cursor;
                    if (!page.candidate) { state.exhausted = state.scan.exhausted; continue; }
                    const auto raw = identity.RawCandidateIdentity(*page.candidate);
                    if (raw.resourceUnavailable)
                    { result.status = V3FamilyTraversalStatus::resource_unavailable; result.errors.emplace_back("v3_family_raw_candidate_resource_unavailable"); return result; }
                    if (!raw.errors.empty() || raw.id.empty())
                    { result.errors.emplace_back("v3_family_raw_candidate_unavailable"); return result; }
                    auto verified = V3FamilyValidatedContext::Create(family, memberBudget, *page.candidate);
                    if (verified.Status() != V3CoordinateStatus::coordinate_ready)
                    { result.status = verified.Status() == V3CoordinateStatus::resource_unavailable ? V3FamilyTraversalStatus::resource_unavailable : verified.Status() == V3CoordinateStatus::schema_error ? V3FamilyTraversalStatus::schema_error : V3FamilyTraversalStatus::invalid; result.errors.push_back("v3_family_candidate_context_unavailable:" + verified.Reason()); return result; }
                    const auto program = verified.TransferProgram();
                    const auto mapping = MapV3PresenceCandidate(identity, verified, program);
                    if (!mapping.graphAvailable)
                    {
                        if (mapping.status != "mapping_unimplemented")
                        {
                            result.status = mapping.status == "resource_unavailable" ? V3FamilyTraversalStatus::resource_unavailable :
                                mapping.status == "schema_error" ? V3FamilyTraversalStatus::schema_error : V3FamilyTraversalStatus::invalid;
                            result.errors.push_back("v3_family_mapping_unavailable:" + mapping.status);
                            result.errors.insert(result.errors.end(), mapping.reasons.begin(), mapping.reasons.end());
                            return result;
                        }
                        result.record = V3FamilyCandidateRecord{"mapping_unimplemented", page.candidate->ordinal,
                            raw.id, mapping.status, mapping.reasons, std::nullopt, *page.candidate};
                        state.exhausted = state.scan.exhausted;
                        continue;
                    }
                    const auto initial = V3JoinedGraph::SerializeCursor(identity, verified, program, {});
                    if (!initial.errors.empty()) { result.status = V3FamilyTraversalStatus::unavailable; result.errors = initial.errors; return result; }
                    state.before = before; state.candidateId = raw.id; state.candidateOrdinal = page.candidate->ordinal;
                    state.joinedCursor = initial.encoded; state.graphCount = "0"; state.active = true;
                    active = *page.candidate;
                    continue;
                }
                if (!active) { result.errors.emplace_back("v3_family_active_candidate_missing"); return result; }
                auto verified = V3FamilyValidatedContext::Create(family, memberBudget, *active);
                if (verified.Status() != V3CoordinateStatus::coordinate_ready)
                { result.status = verified.Status() == V3CoordinateStatus::resource_unavailable ? V3FamilyTraversalStatus::resource_unavailable : verified.Status() == V3CoordinateStatus::schema_error ? V3FamilyTraversalStatus::schema_error : V3FamilyTraversalStatus::invalid; result.errors.push_back("v3_family_active_context_unavailable:" + verified.Reason()); return result; }
                const auto program = verified.TransferProgram();
                const auto parsed = V3JoinedGraph::ParseCursor(identity, verified, program, state.joinedCursor);
                if (!parsed.errors.empty()) { result.status = V3FamilyTraversalStatus::unavailable; result.errors = parsed.errors; return result; }
                const auto page = V3JoinedGraph::Enumerate(identity, verified, program, parsed.cursor, 1, 1);
                if (!page.errors.empty()) { result.status = V3FamilyTraversalStatus::unavailable; result.errors = page.errors; return result; }
                const auto saved = V3JoinedGraph::SerializeCursor(identity, verified, program, page.cursor);
                if (!saved.errors.empty()) { result.status = V3FamilyTraversalStatus::unavailable; result.errors = saved.errors; return result; }
                for (const auto& prune : page.presencePruned)
                    stagedPresencePrunes.push_back({state.candidateOrdinal, state.candidateId, prune});
                for (const auto& prune : page.activePruned)
                    stagedActivePrunes.push_back({state.candidateOrdinal, state.candidateId, prune});
                state.joinedCursor = saved.encoded;
                if (!page.graphs.empty())
                {
                    result.record = V3FamilyCandidateRecord{"graph_hypothesis", state.candidateOrdinal,
                        state.candidateId, "unexecuted", page.graphs.front().graph.unresolved, page.graphs.front(), *active};
                    Increment(state.graphCount);
                }
                if (page.exhausted)
                {
                    if (state.graphCount == "0")
                        result.record = V3FamilyCandidateRecord{"no_graph_hypothesis", state.candidateOrdinal,
                            state.candidateId, "unexecuted", {"v3_no_graph_is_not_a_product_result"}, std::nullopt, *active};
                    state.active = false; state.before = {}; state.candidateId.clear(); state.candidateOrdinal.clear();
                    state.joinedCursor.clear(); state.graphCount = "0"; active.reset(); state.exhausted = state.scan.exhausted;
                }
            }
            state.integrity = Integrity(state);
            result.cursor = std::move(state); result.exhausted = result.cursor.exhausted; result.paused = !result.exhausted;
            result.presencePruned = std::move(stagedPresencePrunes);
            result.activePruned = std::move(stagedActivePrunes);
            result.status = V3FamilyTraversalStatus::ready;
        }
        catch (const std::bad_alloc&) { result.status = V3FamilyTraversalStatus::resource_unavailable; result.record.reset(); result.workUsed = 0; result.errors.emplace_back("v3_family_candidate_resource_unavailable"); }
        catch (const std::length_error&) { result.status = V3FamilyTraversalStatus::resource_unavailable; result.record.reset(); result.workUsed = 0; result.errors.emplace_back("v3_family_candidate_resource_unavailable"); }
        return result;
    }
    static V3FamilyCandidateCodec SerializeCursor(const V3FamilyDescriptor& family, std::size_t memberBudget,
        V3FamilyCandidateCursor cursor)
    {
        V3FamilyCandidateCodec result;
        result.cursor = std::move(cursor);
        try
        {
            auto checked = Step(family, memberBudget, result.cursor, 0);
            result.status = checked.status; result.errors = std::move(checked.errors);
            if (result.status != V3FamilyTraversalStatus::ready || !result.errors.empty()) return result;
            std::string encoded;
            AppendToken(encoded, "v3_family_candidate_joined_cursor_encoding_v3");
            AppendToken(encoded, Payload(checked.cursor)); AppendToken(encoded, checked.cursor.integrity);
            result.encoded = std::move(encoded); result.cursor = std::move(checked.cursor);
        }
        catch (const std::bad_alloc&) { result.status = V3FamilyTraversalStatus::resource_unavailable; result.encoded.clear(); result.errors.emplace_back("v3_family_cursor_resource_unavailable"); }
        catch (const std::length_error&) { result.status = V3FamilyTraversalStatus::resource_unavailable; result.encoded.clear(); result.errors.emplace_back("v3_family_cursor_resource_unavailable"); }
        return result;
    }
    static V3FamilyCandidateCodec ParseCursor(const V3FamilyDescriptor& family, std::size_t memberBudget,
        std::string_view encoded)
    {
        V3FamilyCandidateCodec result;
        try
        {
            auto remaining = encoded; std::string_view token, payload, checksum;
            if (!ReadGraphToken(remaining, token))
            { result.errors.emplace_back("v3_family_cursor_encoding_invalid"); return result; }
            if (token != "v3_family_candidate_joined_cursor_encoding_v3")
            { result.errors.emplace_back("v3_family_cursor_version_unsupported"); return result; }
            if (!ReadGraphToken(remaining, payload) || !ReadGraphToken(remaining, checksum) || !remaining.empty())
            { result.errors.emplace_back("v3_family_cursor_encoding_invalid"); return result; }
            auto fields = payload;
            const auto read = [&] { return ReadGraphToken(fields, token); };
            auto& state = result.cursor;
            if (!read()) { result.errors.emplace_back("v3_family_cursor_encoding_invalid"); return result; }
            state.version = std::string(token);
            if (!read()) { result.errors.emplace_back("v3_family_cursor_encoding_invalid"); return result; }
            state.binding = std::string(token);
            if (!read()) { result.errors.emplace_back("v3_family_cursor_encoding_invalid"); return result; }
            auto scan = DecodeCandidateScanState(token, "v3_family_scan_state_v1");
            if (!scan.errors.empty()) { result.errors = std::move(scan.errors); return result; }
            state.scan = std::move(scan.cursor);
            if (!read()) { result.errors.emplace_back("v3_family_cursor_encoding_invalid"); return result; }
            auto before = DecodeCandidateScanState(token, "v3_family_scan_state_v1");
            if (!before.errors.empty()) { result.errors = std::move(before.errors); return result; }
            state.before = std::move(before.cursor);
            if (!read() || (token != "0" && token != "1")) { result.errors.emplace_back("v3_family_cursor_encoding_invalid"); return result; }
            state.active = token == "1";
            if (!read() || (token != "0" && token != "1")) { result.errors.emplace_back("v3_family_cursor_encoding_invalid"); return result; }
            state.exhausted = token == "1";
            if (!read()) { result.errors.emplace_back("v3_family_cursor_encoding_invalid"); return result; }
            state.candidateId = std::string(token);
            if (!read()) { result.errors.emplace_back("v3_family_cursor_encoding_invalid"); return result; }
            state.candidateOrdinal = std::string(token);
            if (!read()) { result.errors.emplace_back("v3_family_cursor_encoding_invalid"); return result; }
            state.joinedCursor = std::string(token);
            if (!read() || !fields.empty()) { result.errors.emplace_back("v3_family_cursor_encoding_invalid"); return result; }
            state.graphCount = std::string(token); state.integrity = std::string(checksum);
            auto canonical = SerializeCursor(family, memberBudget, state);
            if (canonical.status != V3FamilyTraversalStatus::ready || !canonical.errors.empty()) return canonical;
            if (canonical.encoded != encoded)
            { result.errors.emplace_back("v3_family_cursor_encoding_noncanonical"); return result; }
            return canonical;
        }
        catch (const std::bad_alloc&) { result.status = V3FamilyTraversalStatus::resource_unavailable; result.encoded.clear(); result.errors.emplace_back("v3_family_cursor_resource_unavailable"); }
        catch (const std::length_error&) { result.status = V3FamilyTraversalStatus::resource_unavailable; result.encoded.clear(); result.errors.emplace_back("v3_family_cursor_resource_unavailable"); }
        return result;
    }
};
// Independent V4 family traversal, sharing only the version-neutral scan/graph kernels.
// Bump this token and its cursor/encoding versions whenever V4 Program,
// candidate admission or graph mapping changes, including while the cursor is
// between candidates: an inactive cursor cannot otherwise bind earlier maps.
inline constexpr std::string_view V4FamilyCandidateGraphAlgorithm = "v4_family_candidate_joined_dfs_v19";
struct V4FamilyCandidateCursor
{
    std::string version = "v4_family_candidate_joined_cursor_v19", binding, integrity;
    CandidateScanCursor scan, before;
    bool active = false, exhausted = false;
    std::string candidateId, candidateOrdinal, joinedCursor, graphCount = "0";
};
struct V4FamilyCandidateRecord
{
    std::string kind, candidateOrdinal, candidateId, reason;
    std::vector<std::string> unresolved;
    std::optional<V4JoinedGraphItem> graph;
    std::optional<Candidate> coordinate; // domain-checked, applicability still unresolved
    std::optional<V4NativeJoinedGraphItem> nativeGraph;
};
enum class V4FamilyTraversalStatus { ready, invalid, resource_unavailable, schema_error, unavailable };
struct V4FamilyPresencePrune
{
    std::string candidateOrdinal, candidateId;
    PresencePrefixPrune prune;
};
struct V4FamilyActivePrune
{
    std::string candidateOrdinal, candidateId;
    JoinedGraphPrune prune;
};
struct V4FamilyCandidateStep
{
    V4FamilyTraversalStatus status = V4FamilyTraversalStatus::invalid;
    V4FamilyCandidateCursor cursor;
    std::optional<V4FamilyCandidateRecord> record;
    // Prefix/partition rejections are not counts of complete product cases.
    // Durable callers must commit these records with the returned cursor.
    std::vector<V4FamilyPresencePrune> presencePruned;
    std::vector<V4FamilyActivePrune> activePruned;
    bool exhausted = false, paused = false;
    std::size_t workUsed = 0;
    std::vector<std::string> errors;
};
struct V4FamilyCandidateCodec
{
    V4FamilyTraversalStatus status = V4FamilyTraversalStatus::invalid;
    V4FamilyCandidateCursor cursor;
    std::string encoded;
    std::vector<std::string> errors;
};
struct V4FamilyStagedNativeInspection
{
    V4FamilyTraversalStatus status = V4FamilyTraversalStatus::invalid;
    V4FamilyCandidateCursor cursor;
    std::optional<Candidate> candidate;
    std::optional<V4NativeJoinedGraphCursor> joined;
    std::string candidateId, candidateOrdinal, graphCount;
    std::vector<std::string> errors;
};
struct V4FamilyStagedNativePage
{
    V4FamilyCandidateCursor before, after;
    Candidate candidate;
    std::string candidateId, candidateOrdinal, pendingOrdinal;
    std::vector<bool> pendingPresent;
    std::vector<CandidatePrefixPrune> candidatePruned;
    std::vector<V4FamilyPresencePrune> presencePruned;
    std::vector<V4FamilyActivePrune> activePruned;
    std::optional<V4FamilyCandidateRecord> record;
    std::size_t workUsed = 0;
    bool projectionRequired = false, exhausted = false, paused = false;
};
class V4FamilyStagedNativeProposal final
{
    friend class V4FamilyCandidateGraph;
public:
    V4FamilyStagedNativeProposal(const V4FamilyStagedNativeProposal&) = delete;
    V4FamilyStagedNativeProposal& operator=(const V4FamilyStagedNativeProposal&) = delete;
    const V4FamilyStagedNativePage& Page() const noexcept { return page_; }
private:
    explicit V4FamilyStagedNativeProposal(V4FamilyStagedNativePage page) : page_(std::move(page)) {}
    V4FamilyStagedNativePage page_;
};
struct V4FamilyStagedNativeProposalResult
{
    V4FamilyTraversalStatus status = V4FamilyTraversalStatus::invalid;
    std::unique_ptr<V4FamilyStagedNativeProposal> proposal;
    std::vector<std::string> errors;
};
struct V4FamilyStagedNativePageEncoding
{
    std::string bytes;
    bool encoded = false, resourceUnavailable = false;
    std::string_view reason;
};

// One family slice for a fair outer scheduler. This uses the same prefix DFS
// kernel with a conservative accept-all V4 prefix rule; applicability that is
// not proved is reported, never pruned. Returned records are unexecuted graph
// hypotheses or explicit mapping-unimplemented results, never product cases.
class V4FamilyCandidateGraph final
{
    friend class ::drag_global::v3_scheduler::CheckpointStore;
    friend class ::drag_global::v3_scheduler::V4NativeStagedFamilyRecovery;
    // Friend-only graph replay; caller must first authenticate the journal,
    // candidate and presence under its live reservation and postcheck.
    static V4NativeJoinedGraphReplay ReplayStagedNativeGraph(const V4FamilyIdentityContext& identity,
        const V4FamilyValidatedContext& verified, const std::vector<bool>& present,
        const PresenceProjectionWork& work, std::string_view graphId)
    { return V4NativeJoinedGraph::ReplayCompleted(identity, verified, present, work, graphId); }
    static std::optional<bool> PeekNativeJoinedActive(std::string_view encoded)
    {
        std::string_view token, inner, payload, checksum;
        if (!ReadGraphToken(encoded, token) || token != "v4_native_joined_presence_cursor_encoding_v13" ||
            !ReadGraphToken(encoded, inner) || !encoded.empty() ||
            !ReadGraphToken(inner, token) || token != "presence_joined_graph_cursor_encoding_v1" ||
            !ReadGraphToken(inner, payload) || !ReadGraphToken(inner, checksum) || !inner.empty()) return std::nullopt;
        for (unsigned i = 0; i < 4; ++i) if (!ReadGraphToken(payload, token)) return std::nullopt;
        if (!ReadGraphToken(payload, token) || (token != "true" && token != "false")) return std::nullopt;
        return token == "true";
    }
    static std::string Binding(const V4FamilyIdentityContext& identity)
    {
        std::string result;
        AppendToken(result, V4FamilyCandidateGraphAlgorithm);
        AppendToken(result, identity.FamilyBinding()); AppendToken(result, identity.SchemaBinding());
        return result;
    }
    static std::string ScanBinding(const V4FamilyIdentityContext& identity)
    {
        std::string result; AppendToken(result, "v4_family_raw_prefix_dfs_v1");
        AppendToken(result, Binding(identity)); return result;
    }
    static std::string ScanBytes(const CandidateScanCursor& state)
    { std::string bytes; EncodeCandidateScanState(bytes, state, "v4_family_scan_state_v1"); return bytes; }
    static std::string Payload(const V4FamilyCandidateCursor& state)
    {
        std::string bytes;
        AppendToken(bytes, state.version); AppendToken(bytes, state.binding);
        AppendToken(bytes, ScanBytes(state.scan)); AppendToken(bytes, ScanBytes(state.before));
        AppendToken(bytes, state.active ? "1" : "0"); AppendToken(bytes, state.exhausted ? "1" : "0");
        AppendToken(bytes, state.candidateId); AppendToken(bytes, state.candidateOrdinal);
        AppendToken(bytes, state.joinedCursor); AppendToken(bytes, state.graphCount);
        return bytes;
    }
    static std::string Integrity(const V4FamilyCandidateCursor& state)
    {
        std::uint64_t hash = 14695981039346656037ull;
        for (const char c : Payload(state)) { hash ^= static_cast<unsigned char>(c); hash *= 1099511628211ull; }
        return std::to_string(hash); // corruption check, not authentication or history proof
    }
    static CandidateScanPage Scan(const V4FamilyIdentityContext& identity, CandidateScanCursor state, std::size_t budget)
    {
        const auto keep = [](const Candidate&, std::string_view, std::string_view) { return true; };
        const auto noUnprovedExclusion = [](const Candidate&) { return std::string{}; };
        return CandidateScanKernel::Scan(identity.Axes(), identity.Lookup(), ScanBinding(identity), keep,
            noUnprovedExclusion, std::move(state), budget);
    }
    static bool Validate(const V4FamilyIdentityContext& identity, const V4FamilyDescriptor& family,
        const V4AxisBudget& axisBudget, V4FamilyCandidateCursor& state, std::vector<std::string>& errors,
        std::optional<Candidate>& activeCandidate, V4FamilyTraversalStatus& status,
        bool stagedNative = false, const PresenceProjectionWork* completed = nullptr)
    {
        if (identity.Status() != V4FamilyStatus::schema_only || identity.FamilyBinding() != V4FamilyBinding(family))
        { errors.emplace_back("v4_family_candidate_context_unavailable"); return false; }
        if (state.version != "v4_family_candidate_joined_cursor_v19")
        { errors.emplace_back("v4_family_candidate_cursor_version_invalid"); return false; }
        try { if (Normalize(state.graphCount) != state.graphCount) errors.emplace_back("v4_family_graph_count_invalid"); }
        catch (const std::exception&) { errors.emplace_back("v4_family_graph_count_invalid"); }
        const auto binding = Binding(identity);
        if (state.binding.empty())
        {
            if (!state.integrity.empty() || state.active || state.exhausted || !state.candidateId.empty() ||
                !state.candidateOrdinal.empty() || !state.joinedCursor.empty() || state.graphCount != "0" ||
                ScanBytes(state.scan) != ScanBytes(CandidateScanCursor{}) || ScanBytes(state.before) != ScanBytes(CandidateScanCursor{}))
                errors.emplace_back("v4_family_candidate_initial_state_invalid");
            else state.binding = binding;
        }
        else if (state.binding != binding) errors.emplace_back("v4_family_candidate_cursor_binding_mismatch");
        else if (state.integrity.empty() || state.integrity != Integrity(state)) errors.emplace_back("v4_family_candidate_cursor_integrity_invalid");
        if (!errors.empty()) return false;
        const auto checked = Scan(identity, state.scan, 0);
        if (!checked.errors.empty()) { errors = checked.errors; return false; }
        state.scan = checked.cursor;
        if (!state.active)
        {
            if (!state.candidateId.empty() || !state.candidateOrdinal.empty() || !state.joinedCursor.empty() ||
                state.graphCount != "0" || ScanBytes(state.before) != ScanBytes(CandidateScanCursor{}) ||
                state.exhausted != state.scan.exhausted)
            { errors.emplace_back("v4_family_candidate_inactive_state_invalid"); return false; }
            return true;
        }
        if (state.exhausted || state.candidateId.empty() || state.candidateOrdinal.empty() || state.joinedCursor.empty())
        { errors.emplace_back("v4_family_candidate_active_state_invalid"); return false; }
        const auto previous = Scan(identity, state.before, 1);
        if (!previous.errors.empty() || !previous.candidate || ScanBytes(previous.cursor) != ScanBytes(state.scan) ||
            previous.candidate->ordinal != state.candidateOrdinal)
        { errors.emplace_back("v4_family_candidate_active_history_invalid"); return false; }
        const auto raw = identity.RawCandidateIdentity(*previous.candidate);
        if (raw.resourceUnavailable)
        { status = V4FamilyTraversalStatus::resource_unavailable; errors.emplace_back("v4_family_active_identity_resource_unavailable"); return false; }
        if (!raw.errors.empty() || raw.id != state.candidateId)
        { errors.emplace_back("v4_family_candidate_active_identity_invalid"); return false; }
        auto verified = V4FamilyValidatedContext::Create(family, axisBudget, *previous.candidate);
        if (verified.Status() != V4CoordinateStatus::coordinate_ready)
        {
            status = verified.Status() == V4CoordinateStatus::resource_unavailable ? V4FamilyTraversalStatus::resource_unavailable :
                verified.Status() == V4CoordinateStatus::schema_error ? V4FamilyTraversalStatus::schema_error : verified.Status() == V4CoordinateStatus::unresolved ? V4FamilyTraversalStatus::unavailable : V4FamilyTraversalStatus::invalid;
            errors.emplace_back("v4_family_candidate_active_context_unavailable");
            errors.insert(errors.end(), verified.Unresolved().begin(), verified.Unresolved().end()); return false;
        }
        if (Value(identity.Axes(), verified.Coordinate(), "T01") == "internal_native")
        {
            const auto active = stagedNative ? PeekNativeJoinedActive(state.joinedCursor) : std::optional<bool>{};
            if (stagedNative && !active) { status = V4FamilyTraversalStatus::unavailable; errors.emplace_back("v4_family_staged_joined_cursor_invalid"); return false; }
            if (stagedNative && *active && !completed)
            { status = V4FamilyTraversalStatus::unavailable; errors.emplace_back("v4_family_staged_projection_required"); return false; }
            const auto parsed = stagedNative && *active ? V4NativeJoinedGraph::ParseCompletedCursor(identity, verified, state.joinedCursor, *completed) :
                V4NativeJoinedGraph::ParseCursor(identity, verified, state.joinedCursor);
            if (parsed.resourceUnavailable || !parsed.errors.empty()) { status = parsed.resourceUnavailable ? V4FamilyTraversalStatus::resource_unavailable : V4FamilyTraversalStatus::unavailable; errors = parsed.errors; return false; }
            if (parsed.cursor.inner.nextOrdinal != state.graphCount)
            { errors.emplace_back("v4_family_active_graph_count_mismatch"); return false; }
        }
        else
        {
            const auto parsed = V4JoinedGraph::ParseCursor(identity, verified, state.joinedCursor);
            if (!parsed.errors.empty()) { status = V4FamilyTraversalStatus::unavailable; errors = parsed.errors; return false; }
            if (parsed.cursor.inner.nextOrdinal != state.graphCount)
            { errors.emplace_back("v4_family_active_graph_count_mismatch"); return false; }
        }
        activeCandidate = *previous.candidate;
        return true;
    }
public:
    // Read-only bridge from a family cursor to the authoritative active native
    // candidate. The original before/after scan history is rechecked; an
    // active joined cursor requires its completed projection capability.
    static V4FamilyStagedNativeInspection InspectStagedNative(const V4FamilyDescriptor& family,
        const V4AxisBudget& axisBudget, const V4FamilyCandidateCursor& cursor,
        const PresenceProjectionWork* completed = nullptr)
    {
        V4FamilyStagedNativeInspection out;
        try
        {
            out.cursor = cursor; auto state = cursor;
            const auto identity = V4FamilyIdentityContext::Create(family, axisBudget);
            if (identity.Status() != V4FamilyStatus::schema_only)
            { out.status = identity.Status() == V4FamilyStatus::resource_unavailable ? V4FamilyTraversalStatus::resource_unavailable : V4FamilyTraversalStatus::unavailable;
              out.errors.emplace_back("v4_family_staged_schema_unavailable"); return out; }
            std::optional<Candidate> candidate;
            if (!Validate(identity, family, axisBudget, state, out.errors, candidate, out.status, true, completed)) return out;
            if (!state.active || !candidate)
            { out.status = V4FamilyTraversalStatus::unavailable; out.errors.emplace_back("v4_family_staged_native_candidate_unavailable"); return out; }
            const auto verified = V4FamilyValidatedContext::Create(family, axisBudget, *candidate);
            if (verified.Status() != V4CoordinateStatus::coordinate_ready || Value(verified.Axes(), verified.Coordinate(), "T01") != "internal_native")
            { out.status = verified.Status() == V4CoordinateStatus::resource_unavailable ? V4FamilyTraversalStatus::resource_unavailable : V4FamilyTraversalStatus::unavailable;
              out.errors.emplace_back("v4_family_staged_native_context_unavailable"); return out; }
            const auto active = PeekNativeJoinedActive(state.joinedCursor);
            if (!active || (*active && !completed))
            { out.status = V4FamilyTraversalStatus::unavailable; out.errors.emplace_back("v4_family_staged_projection_required"); return out; }
            const auto parsed = *active ? V4NativeJoinedGraph::ParseCompletedCursor(identity, verified, state.joinedCursor, *completed) :
                V4NativeJoinedGraph::ParseCursor(identity, verified, state.joinedCursor);
            if (parsed.resourceUnavailable || !parsed.errors.empty())
            { out.status = parsed.resourceUnavailable ? V4FamilyTraversalStatus::resource_unavailable : V4FamilyTraversalStatus::unavailable; out.errors = parsed.errors; return out; }
            out.candidate = *candidate; out.joined = parsed.cursor; out.candidateId = state.candidateId;
            out.candidateOrdinal = state.candidateOrdinal; out.graphCount = state.graphCount;
            out.cursor = std::move(state); out.status = V4FamilyTraversalStatus::ready;
        }
        catch (const std::bad_alloc&) { out.status = V4FamilyTraversalStatus::resource_unavailable; out.candidate.reset(); out.joined.reset(); }
        catch (const std::length_error&) { out.status = V4FamilyTraversalStatus::resource_unavailable; out.candidate.reset(); out.joined.reset(); }
        return out;
    }
    // The immutable proposal owns both page and after-state. The scheduler
    // publishes them with its family head before adopting the cursor.
    static V4FamilyStagedNativeProposalResult PrepareStagedNativeAdvance(const V4FamilyDescriptor& family,
        const V4AxisBudget& axisBudget, const V4FamilyCandidateCursor& cursor, std::size_t workBudget)
    {
        V4FamilyStagedNativeProposalResult out;
        try
        {
            const auto inspection = InspectStagedNative(family, axisBudget, cursor);
            if (inspection.status != V4FamilyTraversalStatus::ready || !inspection.candidate || !inspection.joined)
            { out.status = inspection.status; out.errors = inspection.errors; return out; }
            if (inspection.joined->inner.hasActive)
            { out.status = V4FamilyTraversalStatus::unavailable; out.errors.emplace_back("v4_family_staged_native_already_active"); return out; }
            const auto identity = V4FamilyIdentityContext::Create(family, axisBudget);
            const auto verified = V4FamilyValidatedContext::Create(family, axisBudget, *inspection.candidate);
            if (identity.Status() != V4FamilyStatus::schema_only || verified.Status() != V4CoordinateStatus::coordinate_ready)
            { out.status = identity.Status() == V4FamilyStatus::resource_unavailable || verified.Status() == V4CoordinateStatus::resource_unavailable ?
                V4FamilyTraversalStatus::resource_unavailable : V4FamilyTraversalStatus::unavailable; return out; }
            const auto page = V4NativeJoinedGraph::AdvanceDeferred(identity, verified, *inspection.joined, workBudget);
            if (page.resourceUnavailable || !page.errors.empty())
            { out.status = page.resourceUnavailable ? V4FamilyTraversalStatus::resource_unavailable : V4FamilyTraversalStatus::unavailable; out.errors = page.errors; return out; }
            const auto saved = V4NativeJoinedGraph::SerializeCursor(identity, verified, page.cursor);
            if (saved.resourceUnavailable || !saved.errors.empty())
            { out.status = saved.resourceUnavailable ? V4FamilyTraversalStatus::resource_unavailable : V4FamilyTraversalStatus::unavailable; out.errors = saved.errors; return out; }
            V4FamilyStagedNativePage result;
            result.before = cursor; result.after = inspection.cursor; result.candidate = *inspection.candidate;
            result.candidateId = inspection.candidateId; result.candidateOrdinal = inspection.candidateOrdinal;
            result.pendingPresent = page.pendingPresent; result.pendingOrdinal = page.pendingOrdinal;
            result.projectionRequired = page.projectionRequired; result.workUsed = page.workUsed;
            for (const auto& prune : page.pruned) result.presencePruned.push_back({result.candidateOrdinal, result.candidateId, prune});
            result.after.joinedCursor = saved.encoded;
            if (page.exhausted)
            {
                if (result.after.graphCount == "0")
                    result.record = V4FamilyCandidateRecord{"no_graph_hypothesis", result.candidateOrdinal, result.candidateId,
                        "unexecuted", {"v4_no_graph_is_not_a_product_result"}, std::nullopt, result.candidate};
                result.after.active = false; result.after.before = {}; result.after.candidateId.clear();
                result.after.candidateOrdinal.clear(); result.after.joinedCursor.clear(); result.after.graphCount = "0";
                result.after.exhausted = result.after.scan.exhausted;
            }
            result.after.integrity = Integrity(result.after); result.exhausted = result.after.exhausted; result.paused = !result.exhausted;
            out.proposal = std::unique_ptr<V4FamilyStagedNativeProposal>(new V4FamilyStagedNativeProposal(std::move(result)));
            out.status = V4FamilyTraversalStatus::ready;
        }
        catch (const std::bad_alloc&) { out.status = V4FamilyTraversalStatus::resource_unavailable; out.proposal.reset(); }
        catch (const std::length_error&) { out.status = V4FamilyTraversalStatus::resource_unavailable; out.proposal.reset(); }
        return out;
    }
    // Canonical audit bytes, not a publication or execution capability. The
    // immutable proposal must be regenerated from authority during recovery.
    static V4FamilyStagedNativePageEncoding EncodeStagedNativePage(const V4FamilyDescriptor& family,
        const V4AxisBudget& axisBudget, const V4FamilyStagedNativeProposal& proposal,
        const PresenceProjectionWork* completed, std::size_t byteBudget)
    {
        struct BudgetExceeded {};
        V4FamilyStagedNativePageEncoding out;
        try
        {
            const auto& page = proposal.Page();
            const auto before = SerializeAnyStagedCursor(family, axisBudget, page.before, completed);
            const auto after = SerializeAnyStagedCursor(family, axisBudget, page.after, completed);
            if (before.status != V4FamilyTraversalStatus::ready || after.status != V4FamilyTraversalStatus::ready ||
                !before.errors.empty() || !after.errors.empty())
            { out.resourceUnavailable = before.status == V4FamilyTraversalStatus::resource_unavailable ||
                  after.status == V4FamilyTraversalStatus::resource_unavailable;
              out.reason = "v4_staged_native_page_cursor_unavailable"; return out; }
            const auto put = [&](std::string_view value) {
                const auto length = std::to_string(value.size());
                if (out.bytes.size() > byteBudget || length.size() + 1 > byteBudget - out.bytes.size() ||
                    value.size() > byteBudget - out.bytes.size() - length.size() - 1) throw BudgetExceeded{};
                AppendToken(out.bytes, value);
            };
            const auto number = [&](std::size_t value) { put(std::to_string(value)); };
            const auto flag = [&](bool value) { put(value ? "1" : "0"); };
            const auto bits = [&](const std::vector<bool>& values) { number(values.size()); for (bool value : values) flag(value); };
            const auto candidate = [&](const Candidate& value) {
                put(value.ordinal); number(value.values.size()); for (const auto& item : value.values) put(item);
            };
            const auto strings = [&](const std::vector<std::string>& values) { number(values.size()); for (const auto& value : values) put(value); };
            put("v4_family_staged_native_page_v2"); put(before.encoded); put(after.encoded);
            candidate(page.candidate); put(page.candidateId); put(page.candidateOrdinal); put(page.pendingOrdinal);
            bits(page.pendingPresent); number(page.workUsed); flag(page.projectionRequired); flag(page.exhausted); flag(page.paused);
            number(page.candidatePruned.size());
            for (const auto& prune : page.candidatePruned)
            { put(prune.axisId); put(prune.value); number(prune.prefix.size()); for (auto value : prune.prefix) number(value); }
            number(page.presencePruned.size());
            for (const auto& entry : page.presencePruned)
            {
                put(entry.candidateOrdinal); put(entry.candidateId); number(entry.prune.prefix.size());
                for (auto value : entry.prune.prefix) number(value);
                put(entry.prune.reason); put(entry.prune.premise); put(entry.prune.consequence);
            }
            number(page.activePruned.size());
            for (const auto& entry : page.activePruned)
            {
                put(entry.candidateOrdinal); put(entry.candidateId); put(entry.prune.presenceOrdinal); bits(entry.prune.present);
                const auto& prune = entry.prune.prune; put(prune.scope); put(prune.reason); number(prune.partition.size());
                for (auto value : prune.partition) number(value);
                put(prune.earlierRequirement); put(prune.currentRequirement);
            }
            flag(page.record.has_value());
            if (page.record)
            {
                const auto& record = *page.record;
                if (record.graph) { out.bytes.clear(); out.reason = "v4_staged_native_page_non_native_graph"; return out; }
                put(record.kind); put(record.candidateOrdinal); put(record.candidateId); put(record.reason); strings(record.unresolved);
                flag(record.coordinate.has_value()); if (record.coordinate) candidate(*record.coordinate);
                flag(record.nativeGraph.has_value());
                if (record.nativeGraph)
                {
                    const auto& item = *record.nativeGraph; put(item.ordinal); put(item.presenceOrdinal); bits(item.present);
                    put(item.graph.ordinal); put(item.graph.id); number(item.graph.classes.size());
                    for (auto value : item.graph.classes) number(value);
                    strings(item.graph.order); strings(item.graph.unresolved);
                }
            }
            out.encoded = true; out.reason = "v4_staged_native_page_bytes_only";
        }
        catch (const BudgetExceeded&) { out.bytes.clear(); out.resourceUnavailable = true; out.reason = "v4_staged_native_page_byte_budget"; }
        catch (const std::bad_alloc&) { out.bytes.clear(); out.resourceUnavailable = true; out.reason = "v4_staged_native_page_resource_unavailable"; }
        catch (const std::length_error&) { out.bytes.clear(); out.resourceUnavailable = true; out.reason = "v4_staged_native_page_resource_unavailable"; }
        return out;
    }
private:
    // These internal operations do not certify durable publication. The
    // checkpoint store must first independently authenticate the complete
    // projection journal and its head, then publish each proposal and the
    // resulting family cursor in one transaction.
    static V4FamilyStagedNativeProposalResult PrepareStagedNativeContinue(const V4FamilyDescriptor& family,
        const V4AxisBudget& axisBudget, const V4FamilyCandidateCursor& cursor, std::size_t workBudget)
    {
        auto result = PrepareStagedNativeAdvance(family, axisBudget, cursor, workBudget);
        if (result.status != V4FamilyTraversalStatus::ready || !result.proposal || !result.errors.empty()) return result;
        const auto& page = result.proposal->Page();
        const auto before = SerializeCursor(family, axisBudget, page.before);
        const auto after = SerializeCursor(family, axisBudget, page.after);
        if (before.status != V4FamilyTraversalStatus::ready || after.status != V4FamilyTraversalStatus::ready ||
            !before.errors.empty() || !after.errors.empty())
        {
            result.status = before.status == V4FamilyTraversalStatus::resource_unavailable ||
                after.status == V4FamilyTraversalStatus::resource_unavailable ?
                V4FamilyTraversalStatus::resource_unavailable : V4FamilyTraversalStatus::unavailable;
            result.proposal.reset(); return result;
        }
        if (before.encoded == after.encoded && !page.projectionRequired && !page.record &&
            page.presencePruned.empty() && page.activePruned.empty())
        { result.status = V4FamilyTraversalStatus::unavailable; result.proposal.reset(); }
        return result;
    }
    // Resume candidate scanning only after the previous candidate has ended.
    // Stop immediately after creating a candidate's initial joined cursor;
    // graph/presence enumeration is a separate staged transaction.
    static V4FamilyStagedNativeProposalResult PrepareStagedNativeNextCandidate(const V4FamilyDescriptor& family,
        const V4AxisBudget& axisBudget, const V4FamilyCandidateCursor& cursor, std::size_t workBudget)
    {
        V4FamilyStagedNativeProposalResult out;
        try
        {
            const auto initial = Step(family, axisBudget, cursor, 0);
            if (initial.status != V4FamilyTraversalStatus::ready || !initial.errors.empty() ||
                initial.cursor.active || initial.cursor.exhausted)
            { out.status = initial.status == V4FamilyTraversalStatus::ready ? V4FamilyTraversalStatus::unavailable : initial.status;
              out.errors = initial.errors; return out; }
            const auto identity = V4FamilyIdentityContext::Create(family, axisBudget);
            if (identity.Status() != V4FamilyStatus::schema_only)
            { out.status = identity.Status() == V4FamilyStatus::resource_unavailable ? V4FamilyTraversalStatus::resource_unavailable : V4FamilyTraversalStatus::unavailable; return out; }
            V4FamilyStagedNativePage result; result.before = cursor; result.after = initial.cursor;
            while (result.workUsed < workBudget && !result.after.exhausted && !result.after.active && !result.record)
            {
                const auto before = result.after.scan;
                const auto scanned = Scan(identity, before, 1);
                if (!scanned.errors.empty() || !scanned.structuralExclusion.empty())
                { out.status = V4FamilyTraversalStatus::unavailable; out.errors = scanned.errors; return out; }
                result.workUsed += scanned.workUsed;
                result.candidatePruned.insert(result.candidatePruned.end(), scanned.pruned.begin(), scanned.pruned.end());
                result.after.scan = scanned.cursor;
                if (!scanned.candidate)
                {
                    result.after.exhausted = scanned.cursor.exhausted;
                    if (!scanned.workUsed && !result.after.exhausted)
                    { out.status = V4FamilyTraversalStatus::unavailable; return out; }
                    continue;
                }
                result.candidate = *scanned.candidate;
                const auto raw = identity.RawCandidateIdentity(*scanned.candidate);
                if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty())
                { out.status = raw.resourceUnavailable ? V4FamilyTraversalStatus::resource_unavailable : V4FamilyTraversalStatus::unavailable; out.errors = raw.errors; return out; }
                result.candidateId = raw.id; result.candidateOrdinal = scanned.candidate->ordinal;
                const auto verified = V4FamilyValidatedContext::Create(family, axisBudget, *scanned.candidate);
                if (verified.Status() != V4CoordinateStatus::coordinate_ready)
                { out.status = verified.Status() == V4CoordinateStatus::resource_unavailable ? V4FamilyTraversalStatus::resource_unavailable : V4FamilyTraversalStatus::unavailable; return out; }
                const bool native = Value(identity.Axes(), verified.Coordinate(), "T01") == "internal_native";
                const auto mapping = native ? MapV4NativePresenceCandidate(identity, verified) : MapV4PresenceCandidate(identity, verified);
                if (!mapping.graphAvailable)
                {
                    if (mapping.status != "mapping_unimplemented")
                    { out.status = mapping.resourceUnavailable ? V4FamilyTraversalStatus::resource_unavailable : V4FamilyTraversalStatus::unavailable;
                      out.errors.push_back("v4_family_mapping_unavailable:" + mapping.status); return out; }
                    result.record = V4FamilyCandidateRecord{"mapping_unimplemented", result.candidateOrdinal,
                        result.candidateId, mapping.status, mapping.reasons, std::nullopt, result.candidate};
                    result.after.exhausted = result.after.scan.exhausted; break;
                }
                std::string encoded;
                if (native)
                {
                    const auto joined = V4NativeJoinedGraph::SerializeCursor(identity, verified, {});
                    if (joined.resourceUnavailable || !joined.errors.empty())
                    { out.status = joined.resourceUnavailable ? V4FamilyTraversalStatus::resource_unavailable : V4FamilyTraversalStatus::unavailable; out.errors = joined.errors; return out; }
                    encoded = joined.encoded;
                }
                else
                {
                    const auto joined = V4JoinedGraph::SerializeCursor(identity, verified, {});
                    if (!joined.errors.empty()) { out.status = V4FamilyTraversalStatus::unavailable; out.errors = joined.errors; return out; }
                    encoded = joined.encoded;
                }
                result.after.before = before; result.after.candidateId = raw.id;
                result.after.candidateOrdinal = scanned.candidate->ordinal; result.after.joinedCursor = std::move(encoded);
                result.after.graphCount = "0"; result.after.active = true;
            }
            result.after.integrity = Integrity(result.after); result.exhausted = result.after.exhausted; result.paused = !result.exhausted;
            const auto original = SerializeCursor(family, axisBudget, result.before);
            const auto after = SerializeCursor(family, axisBudget, result.after);
            if (original.status != V4FamilyTraversalStatus::ready || after.status != V4FamilyTraversalStatus::ready ||
                !original.errors.empty() || !after.errors.empty())
            { out.status = original.status == V4FamilyTraversalStatus::resource_unavailable || after.status == V4FamilyTraversalStatus::resource_unavailable ?
                V4FamilyTraversalStatus::resource_unavailable : V4FamilyTraversalStatus::unavailable; return out; }
            if (original.encoded == after.encoded && !result.record && result.candidatePruned.empty())
            { out.status = V4FamilyTraversalStatus::unavailable; return out; }
            out.proposal = std::unique_ptr<V4FamilyStagedNativeProposal>(new V4FamilyStagedNativeProposal(std::move(result)));
            out.status = V4FamilyTraversalStatus::ready;
        }
        catch (const std::bad_alloc&) { out.status = V4FamilyTraversalStatus::resource_unavailable; out.proposal.reset(); }
        catch (const std::length_error&) { out.status = V4FamilyTraversalStatus::resource_unavailable; out.proposal.reset(); }
        return out;
    }
    static V4FamilyCandidateCodec SerializeStagedNativeCursor(const V4FamilyDescriptor& family,
        const V4AxisBudget& axisBudget, V4FamilyCandidateCursor cursor, const PresenceProjectionWork& work)
    {
        V4FamilyCandidateCodec out;
        try
        {
            out.cursor = cursor;
            const auto checked = InspectStagedNative(family, axisBudget, cursor, &work);
            if (checked.status != V4FamilyTraversalStatus::ready || !checked.joined || !checked.joined->inner.hasActive)
            { out.status = checked.status == V4FamilyTraversalStatus::ready ? V4FamilyTraversalStatus::unavailable : checked.status;
              out.errors = checked.errors; return out; }
            AppendToken(out.encoded, "v4_family_candidate_joined_cursor_encoding_v19");
            AppendToken(out.encoded, Payload(checked.cursor)); AppendToken(out.encoded, checked.cursor.integrity);
            out.cursor = checked.cursor; out.status = V4FamilyTraversalStatus::ready;
        }
        catch (const std::bad_alloc&) { out.status = V4FamilyTraversalStatus::resource_unavailable; out.encoded.clear(); }
        catch (const std::length_error&) { out.status = V4FamilyTraversalStatus::resource_unavailable; out.encoded.clear(); }
        return out;
    }
    // Structural encoding only: a staged journal receipt is still required
    // before the checkpoint store can adopt the returned cursor.
    static V4FamilyCandidateCodec SerializeAnyStagedCursor(const V4FamilyDescriptor& family,
        const V4AxisBudget& axisBudget, const V4FamilyCandidateCursor& cursor,
        const PresenceProjectionWork* completed)
    {
        const auto active = cursor.active ? PeekNativeJoinedActive(cursor.joinedCursor) : std::optional<bool>{false};
        // Non-native (or malformed) encodings are independently checked by
        // the ordinary family codec; they are never accepted by inspection.
        if (!active || !*active) return SerializeCursor(family, axisBudget, cursor);
        return completed ? SerializeStagedNativeCursor(family, axisBudget, cursor, *completed) : V4FamilyCandidateCodec{};
    }
    static V4FamilyStagedNativeProposalResult PrepareStagedNativeAttach(const V4FamilyDescriptor& family,
        const V4AxisBudget& axisBudget, const V4FamilyCandidateCursor& cursor, const PresenceProjectionWork& work)
    {
        V4FamilyStagedNativeProposalResult out;
        try
        {
            const auto inspection = InspectStagedNative(family, axisBudget, cursor);
            if (inspection.status != V4FamilyTraversalStatus::ready || !inspection.candidate || !inspection.joined)
            { out.status = inspection.status; out.errors = inspection.errors; return out; }
            const auto identity = V4FamilyIdentityContext::Create(family, axisBudget);
            const auto verified = V4FamilyValidatedContext::Create(family, axisBudget, *inspection.candidate);
            if (identity.Status() != V4FamilyStatus::schema_only || verified.Status() != V4CoordinateStatus::coordinate_ready)
            { out.status = identity.Status() == V4FamilyStatus::resource_unavailable || verified.Status() == V4CoordinateStatus::resource_unavailable ?
                V4FamilyTraversalStatus::resource_unavailable : V4FamilyTraversalStatus::unavailable; return out; }
            const auto attached = V4NativeJoinedGraph::AttachCompletedProjection(identity, verified, *inspection.joined, work);
            if (attached.resourceUnavailable || !attached.errors.empty() || !attached.cursor.inner.hasActive)
            { out.status = attached.resourceUnavailable ? V4FamilyTraversalStatus::resource_unavailable : V4FamilyTraversalStatus::unavailable;
              out.errors = attached.errors; return out; }
            const auto saved = V4NativeJoinedGraph::SerializeCompletedCursor(identity, verified, attached.cursor, work);
            if (saved.resourceUnavailable || !saved.errors.empty())
            { out.status = saved.resourceUnavailable ? V4FamilyTraversalStatus::resource_unavailable : V4FamilyTraversalStatus::unavailable;
              out.errors = saved.errors; return out; }
            V4FamilyStagedNativePage page;
            page.before = cursor; page.after = inspection.cursor; page.candidate = *inspection.candidate;
            page.candidateId = inspection.candidateId; page.candidateOrdinal = inspection.candidateOrdinal;
            page.pendingOrdinal = attached.cursor.inner.presenceOrdinal; page.pendingPresent = attached.cursor.inner.present;
            page.workUsed = attached.workUsed; page.after.joinedCursor = saved.encoded;
            page.after.integrity = Integrity(page.after); page.paused = true;
            out.proposal = std::unique_ptr<V4FamilyStagedNativeProposal>(new V4FamilyStagedNativeProposal(std::move(page)));
            out.status = V4FamilyTraversalStatus::ready;
        }
        catch (const std::bad_alloc&) { out.status = V4FamilyTraversalStatus::resource_unavailable; out.proposal.reset(); }
        catch (const std::length_error&) { out.status = V4FamilyTraversalStatus::resource_unavailable; out.proposal.reset(); }
        return out;
    }
    static V4FamilyStagedNativeProposalResult PrepareStagedNativeGraphs(const V4FamilyDescriptor& family,
        const V4AxisBudget& axisBudget, const V4FamilyCandidateCursor& cursor, const PresenceProjectionWork& work,
        std::size_t workBudget)
    {
        V4FamilyStagedNativeProposalResult out;
        try
        {
            const auto inspection = InspectStagedNative(family, axisBudget, cursor, &work);
            if (inspection.status != V4FamilyTraversalStatus::ready || !inspection.candidate || !inspection.joined || !inspection.joined->inner.hasActive)
            { out.status = inspection.status == V4FamilyTraversalStatus::ready ? V4FamilyTraversalStatus::unavailable : inspection.status;
              out.errors = inspection.errors; return out; }
            const auto identity = V4FamilyIdentityContext::Create(family, axisBudget);
            const auto verified = V4FamilyValidatedContext::Create(family, axisBudget, *inspection.candidate);
            if (identity.Status() != V4FamilyStatus::schema_only || verified.Status() != V4CoordinateStatus::coordinate_ready)
            { out.status = identity.Status() == V4FamilyStatus::resource_unavailable || verified.Status() == V4CoordinateStatus::resource_unavailable ?
                V4FamilyTraversalStatus::resource_unavailable : V4FamilyTraversalStatus::unavailable; return out; }
            const auto graphs = V4NativeJoinedGraph::EnumerateCompletedProjection(identity, verified, *inspection.joined, work, workBudget, 1);
            if (graphs.resourceUnavailable || !graphs.errors.empty() || graphs.graphs.size() > 1)
            { out.status = graphs.resourceUnavailable ? V4FamilyTraversalStatus::resource_unavailable : V4FamilyTraversalStatus::unavailable;
              out.errors = graphs.errors; return out; }
            const auto saved = graphs.cursor.inner.hasActive ?
                V4NativeJoinedGraph::SerializeCompletedCursor(identity, verified, graphs.cursor, work) :
                V4NativeJoinedGraph::SerializeCursor(identity, verified, graphs.cursor);
            if (saved.resourceUnavailable || !saved.errors.empty())
            { out.status = saved.resourceUnavailable ? V4FamilyTraversalStatus::resource_unavailable : V4FamilyTraversalStatus::unavailable;
              out.errors = saved.errors; return out; }
            V4FamilyStagedNativePage page;
            page.before = cursor; page.after = inspection.cursor; page.candidate = *inspection.candidate;
            page.candidateId = inspection.candidateId; page.candidateOrdinal = inspection.candidateOrdinal;
            page.workUsed = graphs.workUsed; page.after.joinedCursor = saved.encoded;
            for (const auto& prune : graphs.activePruned) page.activePruned.push_back({page.candidateOrdinal, page.candidateId, prune});
            if (!graphs.graphs.empty())
            {
                page.record = V4FamilyCandidateRecord{"graph_hypothesis", page.candidateOrdinal, page.candidateId,
                    "unexecuted", graphs.graphs.front().graph.unresolved, std::nullopt, page.candidate};
                page.record->nativeGraph = graphs.graphs.front(); Increment(page.after.graphCount);
            }
            if (graphs.exhausted)
            {
                if (page.after.graphCount == "0")
                    page.record = V4FamilyCandidateRecord{"no_graph_hypothesis", page.candidateOrdinal, page.candidateId,
                        "unexecuted", {"v4_no_graph_is_not_a_product_result"}, std::nullopt, page.candidate};
                page.after.active = false; page.after.before = {}; page.after.candidateId.clear();
                page.after.candidateOrdinal.clear(); page.after.joinedCursor.clear(); page.after.graphCount = "0";
                page.after.exhausted = page.after.scan.exhausted;
            }
            page.after.integrity = Integrity(page.after); page.exhausted = page.after.exhausted; page.paused = !page.exhausted;
            out.proposal = std::unique_ptr<V4FamilyStagedNativeProposal>(new V4FamilyStagedNativeProposal(std::move(page)));
            out.status = V4FamilyTraversalStatus::ready;
        }
        catch (const std::bad_alloc&) { out.status = V4FamilyTraversalStatus::resource_unavailable; out.proposal.reset(); }
        catch (const std::length_error&) { out.status = V4FamilyTraversalStatus::resource_unavailable; out.proposal.reset(); }
        return out;
    }
public:
    // Work units cover candidate DFS and joined graph steps. Schema and
    // authoritative binding reconstruction are finite preambles; callers
    // atomically commit each emitted record and its returned cursor.
    static V4FamilyCandidateStep Step(const V4FamilyDescriptor& family, const V4AxisBudget& axisBudget,
        V4FamilyCandidateCursor cursor, std::size_t workBudget)
    {
        V4FamilyCandidateStep result;
        result.cursor = std::move(cursor);
        try
        {
            auto state = result.cursor;
            std::vector<V4FamilyPresencePrune> stagedPresencePrunes;
            std::vector<V4FamilyActivePrune> stagedActivePrunes;
            const auto identity = V4FamilyIdentityContext::Create(family, axisBudget);
            if (identity.Status() != V4FamilyStatus::schema_only)
            {
                result.status = identity.Status() == V4FamilyStatus::resource_unavailable ? V4FamilyTraversalStatus::resource_unavailable :
                    identity.Status() == V4FamilyStatus::schema_error ? V4FamilyTraversalStatus::schema_error : identity.Status() == V4FamilyStatus::unresolved ? V4FamilyTraversalStatus::unavailable : V4FamilyTraversalStatus::invalid;
                result.errors.emplace_back("v4_family_schema_unavailable");
                result.errors.insert(result.errors.end(), identity.Unresolved().begin(), identity.Unresolved().end()); return result;
            }
            std::optional<Candidate> active;
            if (!Validate(identity, family, axisBudget, state, result.errors, active, result.status)) return result;
            while (result.workUsed < workBudget && !state.exhausted && !result.record)
            {
                ++result.workUsed;
                if (!state.active)
                {
                    const auto before = state.scan;
                    const auto page = Scan(identity, before, 1);
                    if (!page.errors.empty()) { result.errors = page.errors; return result; }
                    state.scan = page.cursor;
                    if (!page.candidate) { state.exhausted = state.scan.exhausted; continue; }
                    const auto raw = identity.RawCandidateIdentity(*page.candidate);
                    if (raw.resourceUnavailable)
                    { result.status = V4FamilyTraversalStatus::resource_unavailable; result.errors.emplace_back("v4_family_raw_candidate_resource_unavailable"); return result; }
                    if (!raw.errors.empty() || raw.id.empty())
                    { result.errors.emplace_back("v4_family_raw_candidate_unavailable"); return result; }
                    auto verified = V4FamilyValidatedContext::Create(family, axisBudget, *page.candidate);
                    if (verified.Status() != V4CoordinateStatus::coordinate_ready)
                    { result.status = verified.Status() == V4CoordinateStatus::resource_unavailable ? V4FamilyTraversalStatus::resource_unavailable : verified.Status() == V4CoordinateStatus::schema_error ? V4FamilyTraversalStatus::schema_error : verified.Status() == V4CoordinateStatus::unresolved ? V4FamilyTraversalStatus::unavailable : V4FamilyTraversalStatus::invalid; result.errors.emplace_back("v4_family_candidate_context_unavailable"); result.errors.insert(result.errors.end(), verified.Unresolved().begin(), verified.Unresolved().end()); return result; }
                    const bool native = Value(identity.Axes(), verified.Coordinate(), "T01") == "internal_native";
                    const auto mapping = native ? MapV4NativePresenceCandidate(identity, verified) :
                        MapV4PresenceCandidate(identity, verified);
                    if (!mapping.graphAvailable)
                    {
                        if (mapping.status != "mapping_unimplemented")
                        {
                            result.status = mapping.status == "resource_unavailable" ? V4FamilyTraversalStatus::resource_unavailable :
                                mapping.status == "schema_error" ? V4FamilyTraversalStatus::schema_error :
                                mapping.status == "schema_unavailable" ? V4FamilyTraversalStatus::unavailable : V4FamilyTraversalStatus::invalid;
                            result.errors.push_back("v4_family_mapping_unavailable:" + mapping.status);
                            result.errors.insert(result.errors.end(), mapping.reasons.begin(), mapping.reasons.end());
                            return result;
                        }
                        result.record = V4FamilyCandidateRecord{"mapping_unimplemented", page.candidate->ordinal,
                            raw.id, mapping.status, mapping.reasons, std::nullopt, *page.candidate};
                        state.exhausted = state.scan.exhausted;
                        continue;
                    }
                    std::string initialEncoded;
                    if (native)
                    {
                        const auto initial = V4NativeJoinedGraph::SerializeCursor(identity, verified, {});
                        if (initial.resourceUnavailable || !initial.errors.empty()) { result.status = initial.resourceUnavailable ? V4FamilyTraversalStatus::resource_unavailable : V4FamilyTraversalStatus::unavailable; result.workUsed = 0; result.errors = initial.errors; return result; }
                        initialEncoded = initial.encoded;
                    }
                    else
                    {
                        const auto initial = V4JoinedGraph::SerializeCursor(identity, verified, {});
                        if (!initial.errors.empty()) { result.status = V4FamilyTraversalStatus::unavailable; result.errors = initial.errors; return result; }
                        initialEncoded = initial.encoded;
                    }
                    state.before = before; state.candidateId = raw.id; state.candidateOrdinal = page.candidate->ordinal;
                    state.joinedCursor = std::move(initialEncoded); state.graphCount = "0"; state.active = true;
                    active = *page.candidate;
                    continue;
                }
                if (!active) { result.errors.emplace_back("v4_family_active_candidate_missing"); return result; }
                auto verified = V4FamilyValidatedContext::Create(family, axisBudget, *active);
                if (verified.Status() != V4CoordinateStatus::coordinate_ready)
                { result.status = verified.Status() == V4CoordinateStatus::resource_unavailable ? V4FamilyTraversalStatus::resource_unavailable : verified.Status() == V4CoordinateStatus::schema_error ? V4FamilyTraversalStatus::schema_error : verified.Status() == V4CoordinateStatus::unresolved ? V4FamilyTraversalStatus::unavailable : V4FamilyTraversalStatus::invalid; result.errors.emplace_back("v4_family_active_context_unavailable"); result.errors.insert(result.errors.end(), verified.Unresolved().begin(), verified.Unresolved().end()); return result; }
                bool graphExhausted = false;
                if (Value(identity.Axes(), verified.Coordinate(), "T01") == "internal_native")
                {
                    const auto parsed = V4NativeJoinedGraph::ParseCursor(identity, verified, state.joinedCursor);
                    if (parsed.resourceUnavailable || !parsed.errors.empty()) { result.status = parsed.resourceUnavailable ? V4FamilyTraversalStatus::resource_unavailable : V4FamilyTraversalStatus::unavailable; result.workUsed = 0; result.errors = parsed.errors; return result; }
                    const auto page = V4NativeJoinedGraph::Enumerate(identity, verified, parsed.cursor, 1, 1);
                    if (page.resourceUnavailable) { result.status = V4FamilyTraversalStatus::resource_unavailable; result.workUsed = 0; return result; }
                    if (!page.errors.empty()) { result.status = V4FamilyTraversalStatus::unavailable; result.errors = page.errors; return result; }
                    const auto saved = V4NativeJoinedGraph::SerializeCursor(identity, verified, page.cursor);
                    if (saved.resourceUnavailable || !saved.errors.empty()) { result.status = saved.resourceUnavailable ? V4FamilyTraversalStatus::resource_unavailable : V4FamilyTraversalStatus::unavailable; result.workUsed = 0; result.errors = saved.errors; return result; }
                    for (const auto& prune : page.presencePruned) stagedPresencePrunes.push_back({state.candidateOrdinal, state.candidateId, prune});
                    for (const auto& prune : page.activePruned) stagedActivePrunes.push_back({state.candidateOrdinal, state.candidateId, prune});
                    state.joinedCursor = saved.encoded; graphExhausted = page.exhausted;
                    if (!page.graphs.empty())
                    {
                        result.record = V4FamilyCandidateRecord{"graph_hypothesis", state.candidateOrdinal,
                            state.candidateId, "unexecuted", page.graphs.front().graph.unresolved, std::nullopt, *active};
                        result.record->nativeGraph = page.graphs.front(); Increment(state.graphCount);
                    }
                }
                else
                {
                    const auto parsed = V4JoinedGraph::ParseCursor(identity, verified, state.joinedCursor);
                    if (!parsed.errors.empty()) { result.status = V4FamilyTraversalStatus::unavailable; result.errors = parsed.errors; return result; }
                    const auto page = V4JoinedGraph::Enumerate(identity, verified, parsed.cursor, 1, 1);
                    if (page.resourceUnavailable) { result.status = V4FamilyTraversalStatus::resource_unavailable; result.workUsed = 0; return result; }
                    if (!page.errors.empty()) { result.status = V4FamilyTraversalStatus::unavailable; result.errors = page.errors; return result; }
                    const auto saved = V4JoinedGraph::SerializeCursor(identity, verified, page.cursor);
                    if (!saved.errors.empty()) { result.status = V4FamilyTraversalStatus::unavailable; result.errors = saved.errors; return result; }
                    for (const auto& prune : page.presencePruned) stagedPresencePrunes.push_back({state.candidateOrdinal, state.candidateId, prune});
                    for (const auto& prune : page.activePruned) stagedActivePrunes.push_back({state.candidateOrdinal, state.candidateId, prune});
                    state.joinedCursor = saved.encoded; graphExhausted = page.exhausted;
                    if (!page.graphs.empty())
                    {
                        result.record = V4FamilyCandidateRecord{"graph_hypothesis", state.candidateOrdinal,
                            state.candidateId, "unexecuted", page.graphs.front().graph.unresolved, page.graphs.front(), *active};
                        Increment(state.graphCount);
                    }
                }
                if (graphExhausted)
                {
                    if (state.graphCount == "0")
                        result.record = V4FamilyCandidateRecord{"no_graph_hypothesis", state.candidateOrdinal,
                            state.candidateId, "unexecuted", {"v4_no_graph_is_not_a_product_result"}, std::nullopt, *active};
                    state.active = false; state.before = {}; state.candidateId.clear(); state.candidateOrdinal.clear();
                    state.joinedCursor.clear(); state.graphCount = "0"; active.reset(); state.exhausted = state.scan.exhausted;
                }
            }
            state.integrity = Integrity(state);
            result.cursor = std::move(state); result.exhausted = result.cursor.exhausted; result.paused = !result.exhausted;
            result.presencePruned = std::move(stagedPresencePrunes);
            result.activePruned = std::move(stagedActivePrunes);
            result.status = V4FamilyTraversalStatus::ready;
        }
        catch (const std::bad_alloc&) { result.status = V4FamilyTraversalStatus::resource_unavailable; result.record.reset(); result.workUsed = 0; result.errors.emplace_back("v4_family_candidate_resource_unavailable"); }
        catch (const std::length_error&) { result.status = V4FamilyTraversalStatus::resource_unavailable; result.record.reset(); result.workUsed = 0; result.errors.emplace_back("v4_family_candidate_resource_unavailable"); }
        return result;
    }
    static V4FamilyCandidateCodec SerializeCursor(const V4FamilyDescriptor& family, const V4AxisBudget& axisBudget,
        V4FamilyCandidateCursor cursor)
    {
        V4FamilyCandidateCodec result;
        result.cursor = std::move(cursor);
        try
        {
            auto checked = Step(family, axisBudget, result.cursor, 0);
            result.status = checked.status; result.errors = std::move(checked.errors);
            if (result.status != V4FamilyTraversalStatus::ready || !result.errors.empty()) return result;
            std::string encoded;
            AppendToken(encoded, "v4_family_candidate_joined_cursor_encoding_v19");
            AppendToken(encoded, Payload(checked.cursor)); AppendToken(encoded, checked.cursor.integrity);
            result.encoded = std::move(encoded); result.cursor = std::move(checked.cursor);
        }
        catch (const std::bad_alloc&) { result.status = V4FamilyTraversalStatus::resource_unavailable; result.encoded.clear(); result.errors.emplace_back("v4_family_cursor_resource_unavailable"); }
        catch (const std::length_error&) { result.status = V4FamilyTraversalStatus::resource_unavailable; result.encoded.clear(); result.errors.emplace_back("v4_family_cursor_resource_unavailable"); }
        return result;
    }
    // A completed work permits structural decoding of an active staged graph
    // cursor only. The returned value does not authenticate its journal or
    // prove that any page or family head was published.
    static V4FamilyCandidateCodec ParseCursor(const V4FamilyDescriptor& family, const V4AxisBudget& axisBudget,
        std::string_view encoded, const PresenceProjectionWork* completed = nullptr)
    {
        V4FamilyCandidateCodec result;
        try
        {
            auto remaining = encoded; std::string_view token, payload, checksum;
            if (!ReadGraphToken(remaining, token))
            { result.errors.emplace_back("v4_family_cursor_encoding_invalid"); return result; }
            if (token != "v4_family_candidate_joined_cursor_encoding_v19")
            { result.errors.emplace_back("v4_family_cursor_version_unsupported"); return result; }
            if (!ReadGraphToken(remaining, payload) || !ReadGraphToken(remaining, checksum) || !remaining.empty())
            { result.errors.emplace_back("v4_family_cursor_encoding_invalid"); return result; }
            auto fields = payload;
            const auto read = [&] { return ReadGraphToken(fields, token); };
            auto& state = result.cursor;
            if (!read()) { result.errors.emplace_back("v4_family_cursor_encoding_invalid"); return result; }
            state.version = std::string(token);
            if (!read()) { result.errors.emplace_back("v4_family_cursor_encoding_invalid"); return result; }
            state.binding = std::string(token);
            if (!read()) { result.errors.emplace_back("v4_family_cursor_encoding_invalid"); return result; }
            auto scan = DecodeCandidateScanState(token, "v4_family_scan_state_v1");
            if (!scan.errors.empty()) { result.errors = std::move(scan.errors); return result; }
            state.scan = std::move(scan.cursor);
            if (!read()) { result.errors.emplace_back("v4_family_cursor_encoding_invalid"); return result; }
            auto before = DecodeCandidateScanState(token, "v4_family_scan_state_v1");
            if (!before.errors.empty()) { result.errors = std::move(before.errors); return result; }
            state.before = std::move(before.cursor);
            if (!read() || (token != "0" && token != "1")) { result.errors.emplace_back("v4_family_cursor_encoding_invalid"); return result; }
            state.active = token == "1";
            if (!read() || (token != "0" && token != "1")) { result.errors.emplace_back("v4_family_cursor_encoding_invalid"); return result; }
            state.exhausted = token == "1";
            if (!read()) { result.errors.emplace_back("v4_family_cursor_encoding_invalid"); return result; }
            state.candidateId = std::string(token);
            if (!read()) { result.errors.emplace_back("v4_family_cursor_encoding_invalid"); return result; }
            state.candidateOrdinal = std::string(token);
            if (!read()) { result.errors.emplace_back("v4_family_cursor_encoding_invalid"); return result; }
            state.joinedCursor = std::string(token);
            if (!read() || !fields.empty()) { result.errors.emplace_back("v4_family_cursor_encoding_invalid"); return result; }
            state.graphCount = std::string(token); state.integrity = std::string(checksum);
            auto canonical = completed ? SerializeStagedNativeCursor(family, axisBudget, state, *completed) :
                SerializeCursor(family, axisBudget, state);
            if (canonical.status != V4FamilyTraversalStatus::ready || !canonical.errors.empty()) return canonical;
            if (canonical.encoded != encoded)
            { result.errors.emplace_back("v4_family_cursor_encoding_noncanonical"); return result; }
            return canonical;
        }
        catch (const std::bad_alloc&) { result.status = V4FamilyTraversalStatus::resource_unavailable; result.encoded.clear(); result.errors.emplace_back("v4_family_cursor_resource_unavailable"); }
        catch (const std::length_error&) { result.status = V4FamilyTraversalStatus::resource_unavailable; result.encoded.clear(); result.errors.emplace_back("v4_family_cursor_resource_unavailable"); }
        return result;
    }
};
struct UnifiedGraphCursor
{
    std::string version = "global_candidate_joined_cursor_v4";
    std::string binding, integrity, nextGraphOrdinal = "0";
    bool exhausted = false, hasCandidate = false;
    std::string nextCandidate, beforeCandidate, candidateId, candidateOrdinal, joinedCursor;
    std::string currentGraphCount = "0", structuralExcluded = "0", mappingUnimplemented = "0", noGraphHypotheses = "0";
    std::string completedPresencePrefixPrunes = "0";
    std::map<std::string, std::string> completedActivePrunes;
};
struct UnifiedGraphRecord
{
    // kind: graph_hypothesis, structural_exclusion, mapping_unimplemented,
    // or no_graph_hypothesis. None is a product pass or execution claim.
    std::string kind, candidateOrdinal, candidateId, graphOrdinal, reason;
    std::vector<std::string> unresolved;
    std::optional<JoinedGraphItem> graph;
    // Explicit version and complete schema are part of this opaque candidate
    // identity. The adjacent human-readable ID alone is not cross-version ID.
    std::string candidateVersionedId;
};
struct UnifiedPresencePrune
{
    std::string candidateOrdinal, candidateId;
    PresencePrefixPrune prune;
};
struct UnifiedActivePrune
{
    std::string candidateOrdinal, candidateId;
    JoinedGraphPrune prune;
};
struct UnifiedGraphPage
{
    std::vector<UnifiedGraphRecord> records;
    std::vector<CandidatePrefixPrune> candidatePruned;
    std::vector<UnifiedPresencePrune> presencePruned;
    std::vector<UnifiedActivePrune> activePruned;
    UnifiedGraphCursor cursor;
    bool exhausted = false, paused = false;
    std::size_t workUsed = 0;
    std::vector<std::string> errors;
};
struct UnifiedGraphCursorCodec
{
    UnifiedGraphCursor cursor;
    std::string encoded;
    std::vector<std::string> errors;
};

// Preview-only orchestration. Candidate scanner and joined graph are advanced
// under one budget and returned with one cursor. Persistence is the caller's
// responsibility: publish the page and cursor in one durable transaction.
class UnifiedCandidateGraph final
{
    static std::string Binding(const CandidateTraversalContext& context)
    {
        std::string binding;
        AppendToken(binding, "global_unified_candidate_joined_algorithm_v4");
        AppendToken(binding, ExistingSelectionVersionTag(context.version));
        AppendToken(binding, context.schemaBinding);
        return binding;
    }
    static std::string CandidateVersionedId(const CandidateTraversalContext& context, const Candidate& candidate)
    {
        std::string id;
        AppendToken(id, "global_unified_candidate_id_v1");
        AppendToken(id, ExistingSelectionVersionTag(context.version));
        AppendToken(id, context.schemaBinding);
        AppendToken(id, Id(context.axes, candidate));
        return id;
    }
    static std::string Payload(const UnifiedGraphCursor& cursor)
    {
        std::string bytes;
        AppendToken(bytes, cursor.version); AppendToken(bytes, cursor.binding); AppendToken(bytes, cursor.nextGraphOrdinal);
        AppendToken(bytes, cursor.exhausted ? "true" : "false"); AppendToken(bytes, cursor.hasCandidate ? "true" : "false");
        AppendToken(bytes, cursor.nextCandidate); AppendToken(bytes, cursor.beforeCandidate);
        AppendToken(bytes, cursor.candidateId); AppendToken(bytes, cursor.candidateOrdinal); AppendToken(bytes, cursor.joinedCursor);
        AppendToken(bytes, cursor.currentGraphCount); AppendToken(bytes, cursor.structuralExcluded);
        AppendToken(bytes, cursor.mappingUnimplemented); AppendToken(bytes, cursor.noGraphHypotheses);
        AppendToken(bytes, cursor.completedPresencePrefixPrunes);
        AppendToken(bytes, std::to_string(cursor.completedActivePrunes.size()));
        for (const auto& [key, value] : cursor.completedActivePrunes) { AppendToken(bytes, key); AppendToken(bytes, value); }
        return bytes;
    }
    static std::string Integrity(const UnifiedGraphCursor& cursor)
    {
        std::uint64_t hash = 14695981039346656037ull;
        for (const char c : Payload(cursor)) { hash ^= static_cast<unsigned char>(c); hash *= 1099511628211ull; }
        return std::to_string(hash); // Accidental corruption only; not authentication or history proof.
    }
    static Mapping MapCandidate(const Context& context, const Candidate& candidate)
    {
        const auto program = drag_global::ProgramVersion(context.axes, candidate, context.version);
        return Value(context.axes, candidate, "T01") == "internal_native"
            ? MapSimpleNative(context, candidate, program) : MapSimpleExternal(context, candidate, program);
    }
    static bool Validate(const Context& supplied, UnifiedGraphCursor& cursor, std::vector<std::string>& errors)
    {
        const Context context(supplied.axes, supplied.version);
        errors = context.errors;
        if (!supplied.errors.empty()) errors.insert(errors.end(), supplied.errors.begin(), supplied.errors.end());
        if (supplied.schemaBinding != context.schemaBinding || !supplied.lookup ||
            (supplied.lookup && *supplied.lookup != *context.lookup)) errors.emplace_back("unified_context_binding_mismatch");
        if (cursor.version != "global_candidate_joined_cursor_v4") errors.emplace_back("unified_cursor_version_invalid");
        for (const auto* value : {&cursor.nextGraphOrdinal, &cursor.currentGraphCount, &cursor.structuralExcluded,
                                  &cursor.mappingUnimplemented, &cursor.noGraphHypotheses, &cursor.completedPresencePrefixPrunes})
        {
            try { if (Normalize(*value) != *value) errors.emplace_back("unified_cursor_decimal_invalid"); }
            catch (const std::exception&) { errors.emplace_back("unified_cursor_decimal_invalid"); }
        }
        if (!errors.empty()) return false;
        if (cursor.binding.empty())
        {
            if (!cursor.integrity.empty() || cursor.exhausted || cursor.hasCandidate || cursor.nextGraphOrdinal != "0" ||
                !cursor.nextCandidate.empty() || !cursor.beforeCandidate.empty() || !cursor.candidateId.empty() ||
                !cursor.candidateOrdinal.empty() || !cursor.joinedCursor.empty() || cursor.currentGraphCount != "0" ||
                cursor.structuralExcluded != "0" || cursor.mappingUnimplemented != "0" || cursor.noGraphHypotheses != "0" ||
                cursor.completedPresencePrefixPrunes != "0" || !cursor.completedActivePrunes.empty())
                errors.emplace_back("unified_cursor_initial_state_invalid");
            else
            {
                const auto initial = SerializeCandidateScanCursor(context, {});
                errors = initial.errors;
                if (errors.empty()) { cursor.binding = Binding(context); cursor.nextCandidate = initial.encoded; }
            }
        }
        else if (cursor.binding != Binding(context)) errors.emplace_back("unified_cursor_binding_mismatch");
        else if (cursor.integrity.empty() || cursor.integrity != Integrity(cursor)) errors.emplace_back("unified_cursor_integrity_invalid");
        for (const auto& [key, value] : cursor.completedActivePrunes)
        {
            const bool valid = key == "partition_prefix:pair_requires_same" || key == "partition_prefix:pair_requires_distinct" ||
                key == "partition_prefix:known_property_mismatch" || key == "complete_partition:strict_edge_same_event" ||
                key == "complete_partition:strict_causal_cycle";
            try { if (!valid || Normalize(value) != value || value == "0") errors.emplace_back("unified_active_prune_count_invalid"); }
            catch (const std::exception&) { errors.emplace_back("unified_active_prune_count_invalid"); }
        }
        if (!errors.empty()) return false;
        const auto next = ParseCandidateScanCursor(context, cursor.nextCandidate);
        errors = next.errors;
        if (!errors.empty()) return false;
        if (!cursor.hasCandidate)
        {
            if (!cursor.beforeCandidate.empty() || !cursor.candidateId.empty() || !cursor.candidateOrdinal.empty() ||
                !cursor.joinedCursor.empty() || cursor.currentGraphCount != "0" || cursor.exhausted != next.cursor.exhausted)
                errors.emplace_back("unified_cursor_inactive_state_invalid");
            return errors.empty();
        }
        if (cursor.exhausted || cursor.beforeCandidate.empty() || cursor.candidateId.empty() ||
            cursor.candidateOrdinal.empty() || cursor.joinedCursor.empty())
        { errors.emplace_back("unified_cursor_active_state_invalid"); return false; }
        const auto before = ParseCandidateScanCursor(context, cursor.beforeCandidate);
        errors = before.errors;
        if (!errors.empty()) return false;
        const auto leaf = ScanNextCandidate(context, before.cursor, 1);
        if (!leaf.errors.empty() || leaf.workUsed != 1 || !leaf.candidate || !leaf.structuralExclusion.empty() ||
            leaf.candidate->ordinal != cursor.candidateOrdinal || Id(context.axes, *leaf.candidate) != cursor.candidateId)
        { errors.emplace_back("unified_cursor_candidate_transition_invalid"); return false; }
        const auto after = SerializeCandidateScanCursor(context, leaf.cursor);
        if (!after.errors.empty() || after.encoded != cursor.nextCandidate)
        { errors.emplace_back("unified_cursor_candidate_resume_mismatch"); return false; }
        const auto mapping = MapCandidate(context, *leaf.candidate);
        if (!mapping.structuralExclusion.empty() || !mapping.graphAvailable)
        { errors.emplace_back("unified_cursor_candidate_mapping_unavailable"); return false; }
        const auto joined = JoinedPresenceGraph::ParseCursor(context, *leaf.candidate, mapping.spec, cursor.joinedCursor);
        errors = joined.errors;
        if (!joined.structuralExclusion.empty()) errors.push_back("unified_cursor_graph_structure_mismatch:" + joined.structuralExclusion);
        if (errors.empty() && joined.cursor.exhausted) errors.emplace_back("unified_cursor_active_graph_already_exhausted");
        return errors.empty();
    }
public:
    static UnifiedGraphPage Enumerate(const Context& context, UnifiedGraphCursor cursor,
        std::size_t workBudget, std::size_t outputLimit)
    {
        UnifiedGraphPage result;
        const auto original = cursor;
        result.cursor = std::move(cursor);
        if (!outputLimit) { result.errors.emplace_back("unified_output_limit_must_be_positive"); return result; }
        if (!Validate(context, result.cursor, result.errors)) return result;
        const CandidateTraversalContext traversal(context.version, context.axes);
        auto& state = result.cursor;
        const auto fail = [&] {
            result.records.clear(); result.candidatePruned.clear(); result.presencePruned.clear(); result.activePruned.clear();
            result.cursor = original;
        };
        while (result.workUsed < workBudget && result.records.size() < outputLimit && !state.exhausted)
        {
            if (!state.hasCandidate)
            {
                const auto parsed = ParseCandidateScanCursor(context, state.nextCandidate);
                if (!parsed.errors.empty()) { result.errors = parsed.errors; fail(); return result; }
                const auto before = state.nextCandidate;
                const auto scanned = ScanNextCandidate(context, parsed.cursor, 1);
                if (!scanned.errors.empty()) { result.errors = scanned.errors; fail(); return result; }
                result.workUsed += scanned.workUsed;
                result.candidatePruned.insert(result.candidatePruned.end(), scanned.pruned.begin(), scanned.pruned.end());
                const auto after = SerializeCandidateScanCursor(context, scanned.cursor);
                if (!after.errors.empty()) { result.errors = after.errors; fail(); return result; }
                state.nextCandidate = after.encoded;
                if (scanned.candidate)
                {
                    const auto& candidate = *scanned.candidate;
                    const auto id = Id(context.axes, candidate);
                    if (!scanned.structuralExclusion.empty())
                    {
                        result.records.push_back({"structural_exclusion", candidate.ordinal, id, "", scanned.structuralExclusion, {}, std::nullopt, ""});
                        result.records.back().candidateVersionedId = CandidateVersionedId(traversal, candidate);
                        Increment(state.structuralExcluded);
                    }
                    else
                    {
                        const auto mapping = MapCandidate(context, candidate);
                        if (!mapping.structuralExclusion.empty())
                        { result.errors.push_back("unified_mapping_structure_inconsistent:" + mapping.structuralExclusion); fail(); return result; }
                        if (!mapping.graphAvailable)
                        {
                            result.records.push_back({"mapping_unimplemented", candidate.ordinal, id, "", mapping.status,
                                mapping.reasons, std::nullopt, ""});
                            result.records.back().candidateVersionedId = CandidateVersionedId(traversal, candidate);
                            Increment(state.mappingUnimplemented);
                        }
                        else
                        {
                            const auto initial = JoinedPresenceGraph::SerializeCursor(context, candidate, mapping.spec, {});
                            if (!initial.errors.empty() || !initial.structuralExclusion.empty())
                            {
                                result.errors = initial.errors;
                                if (!initial.structuralExclusion.empty())
                                    result.errors.push_back("unified_graph_structure_mismatch:" + initial.structuralExclusion);
                                fail(); return result;
                            }
                            else
                            {
                                state.beforeCandidate = before; state.candidateId = id; state.candidateOrdinal = candidate.ordinal;
                                state.joinedCursor = initial.encoded; state.currentGraphCount = "0"; state.hasCandidate = true;
                            }
                        }
                    }
                }
                if (!state.hasCandidate && scanned.exhausted) state.exhausted = true;
                else if (!state.hasCandidate && !scanned.workUsed && !scanned.candidate)
                { result.errors.emplace_back("unified_candidate_no_progress"); fail(); return result; }
                continue;
            }
            // Recover the exact candidate from the bounded prefix leaf that
            // was saved before activation. Do not parse v2 through the legacy
            // ParseId or inject any observed-result hypothesis into the source.
            const auto before = ParseCandidateScanCursor(context, state.beforeCandidate);
            if (!before.errors.empty()) { result.errors = before.errors; fail(); return result; }
            const auto leaf = ScanNextCandidate(context, before.cursor, 1);
            if (!leaf.errors.empty() || leaf.workUsed != 1 || !leaf.candidate || !leaf.structuralExclusion.empty() ||
                leaf.candidate->ordinal != state.candidateOrdinal || Id(context.axes, *leaf.candidate) != state.candidateId)
            { result.errors.emplace_back("unified_active_candidate_transition_invalid"); fail(); return result; }
            const auto& candidate = *leaf.candidate;
            const auto mapping = MapCandidate(context, candidate);
            if (!mapping.graphAvailable || !mapping.structuralExclusion.empty())
            { result.errors.emplace_back("unified_active_mapping_changed"); fail(); return result; }
            const auto parsed = JoinedPresenceGraph::ParseCursor(context, candidate, mapping.spec, state.joinedCursor);
            if (!parsed.errors.empty() || !parsed.structuralExclusion.empty())
            { result.errors = parsed.errors; if (!parsed.structuralExclusion.empty()) result.errors.push_back("unified_graph_structure_mismatch:" + parsed.structuralExclusion); fail(); return result; }
            const auto page = JoinedPresenceGraph::Enumerate(context, candidate, mapping.spec, parsed.cursor,
                workBudget - result.workUsed, outputLimit - result.records.size());
            if (page.resourceUnavailable) throw std::bad_alloc{};
            if (!page.errors.empty() || !page.structuralExclusion.empty())
            { result.errors = page.errors; if (!page.structuralExclusion.empty()) result.errors.push_back("unified_graph_structure_mismatch:" + page.structuralExclusion); fail(); return result; }
            result.workUsed += page.workUsed;
            for (const auto& prune : page.presencePruned)
                result.presencePruned.push_back({state.candidateOrdinal, state.candidateId, prune});
            for (const auto& prune : page.activePruned)
                result.activePruned.push_back({state.candidateOrdinal, state.candidateId, prune});
            const auto saved = JoinedPresenceGraph::SerializeCursor(context, candidate, mapping.spec, page.cursor);
            if (!saved.errors.empty() || !saved.structuralExclusion.empty())
            { result.errors = saved.errors; if (!saved.structuralExclusion.empty()) result.errors.push_back("unified_graph_structure_mismatch:" + saved.structuralExclusion); fail(); return result; }
            state.joinedCursor = saved.encoded;
            for (const auto& graph : page.graphs)
            {
                result.records.push_back({"graph_hypothesis", state.candidateOrdinal, state.candidateId,
                    state.nextGraphOrdinal, "", graph.graph.unresolved, graph, ""});
                result.records.back().candidateVersionedId = CandidateVersionedId(traversal, candidate);
                Increment(state.nextGraphOrdinal); Increment(state.currentGraphCount);
            }
            if (page.exhausted)
            {
                const auto presence = ParsePresenceCursor(context, candidate, mapping.spec, page.cursor.nextPresence);
                if (!presence.errors.empty() || !presence.structuralExclusion.empty())
                { result.errors = presence.errors; if (!presence.structuralExclusion.empty()) result.errors.push_back("unified_presence_structure_mismatch:" + presence.structuralExclusion); fail(); return result; }
                const auto presenceCount = presence.cursor.pruneCounts.find("prefix:present_implies_present");
                if (presenceCount != presence.cursor.pruneCounts.end())
                    state.completedPresencePrefixPrunes = Add(state.completedPresencePrefixPrunes, presenceCount->second);
                for (const auto& [key, count] : page.cursor.completedActivePrunes)
                {
                    auto& total = state.completedActivePrunes[key];
                    total = Add(total.empty() ? "0" : total, count);
                }
                if (state.currentGraphCount == "0")
                {
                    result.records.push_back({"no_graph_hypothesis", state.candidateOrdinal, state.candidateId, "",
                        "all_structural_graph_branches_exhausted", {"candidate_applicability_and_production_unimplemented"}, std::nullopt, ""});
                    result.records.back().candidateVersionedId = CandidateVersionedId(traversal, candidate);
                    Increment(state.noGraphHypotheses);
                }
                state.hasCandidate = false; state.beforeCandidate.clear(); state.candidateId.clear(); state.candidateOrdinal.clear();
                state.joinedCursor.clear(); state.currentGraphCount = "0";
                const auto outer = ParseCandidateScanCursor(context, state.nextCandidate);
                if (!outer.errors.empty()) { result.errors = outer.errors; fail(); return result; }
                if (outer.cursor.exhausted) state.exhausted = true;
            }
            else if (!page.workUsed) { result.errors.emplace_back("unified_joined_no_progress"); fail(); return result; }
        }
        result.exhausted = state.exhausted; result.paused = !state.exhausted;
        state.integrity = Integrity(state);
        return result;
    }
    static UnifiedGraphCursorCodec SerializeCursor(const CandidateTraversalContext& context, UnifiedGraphCursor cursor)
    {
        UnifiedGraphCursorCodec result;
        const auto checked = Enumerate(context, std::move(cursor), 0, 1);
        result.cursor = checked.cursor; result.errors = checked.errors;
        if (!result.errors.empty()) return result;
        AppendToken(result.encoded, "global_candidate_joined_cursor_encoding_v4");
        AppendToken(result.encoded, Payload(result.cursor));
        AppendToken(result.encoded, result.cursor.integrity);
        return result;
    }
    template<typename ContextType>
    static UnifiedGraphCursorCodec ParseCursorImpl(const ContextType& context, std::string_view encoded)
    {
        UnifiedGraphCursorCodec result;
        auto outer = encoded;
        std::string_view token, payload, checksum;
        const auto fail = [&] { result.errors.emplace_back("unified_cursor_encoding_invalid"); return result; };
        if (!ReadGraphToken(outer, token)) return fail();
        if (token == "global_candidate_joined_cursor_encoding_v1")
        { result.errors.emplace_back("unified_cursor_encoding_v1_unsupported"); return result; }
        if (token == "global_candidate_joined_cursor_encoding_v2")
        { result.errors.emplace_back("unified_cursor_encoding_v2_unsupported"); return result; }
        if (token == "global_candidate_joined_cursor_encoding_v3")
        { result.errors.emplace_back("unified_cursor_encoding_v3_unsupported"); return result; }
        if (token != "global_candidate_joined_cursor_encoding_v4" ||
            !ReadGraphToken(outer, payload) || !ReadGraphToken(outer, checksum) || !outer.empty()) return fail();
        auto remaining = payload;
        const auto read = [&] { return ReadGraphToken(remaining, token); };
        auto& cursor = result.cursor;
        if (!read()) return fail();
        cursor.version = std::string(token);
        if (!read()) return fail();
        cursor.binding = std::string(token);
        if (!read()) return fail();
        cursor.nextGraphOrdinal = std::string(token);
        if (!read() || (token != "true" && token != "false")) return fail();
        cursor.exhausted = token == "true";
        if (!read() || (token != "true" && token != "false")) return fail();
        cursor.hasCandidate = token == "true";
        if (!read()) return fail();
        cursor.nextCandidate = std::string(token);
        if (!read()) return fail();
        cursor.beforeCandidate = std::string(token);
        if (!read()) return fail();
        cursor.candidateId = std::string(token);
        if (!read()) return fail();
        cursor.candidateOrdinal = std::string(token);
        if (!read()) return fail();
        cursor.joinedCursor = std::string(token);
        if (!read()) return fail();
        cursor.currentGraphCount = std::string(token);
        if (!read()) return fail();
        cursor.structuralExcluded = std::string(token);
        if (!read()) return fail();
        cursor.mappingUnimplemented = std::string(token);
        if (!read()) return fail();
        cursor.noGraphHypotheses = std::string(token);
        if (!read()) return fail();
        cursor.completedPresencePrefixPrunes = std::string(token);
        std::size_t count = 0;
        if (!read() || !ParseCanonicalSize(token, count) || count > remaining.size() / 4) return fail();
        cursor.completedActivePrunes.clear();
        for (std::size_t i = 0; i < count; ++i)
        {
            if (!read()) return fail();
            const std::string key(token);
            if (!read() || !cursor.completedActivePrunes.emplace(key, std::string(token)).second) return fail();
        }
        if (!remaining.empty()) return fail();
        cursor.integrity = std::string(checksum);
        const auto canonical = SerializeCursor(context, cursor);
        if (!canonical.errors.empty()) return canonical;
        if (canonical.encoded != encoded) { result.errors.emplace_back("unified_cursor_encoding_noncanonical"); return result; }
        return canonical;
    }
    static UnifiedGraphCursorCodec ParseCursor(const CandidateTraversalContext& context, std::string_view encoded)
    { return ParseCursorImpl(context, encoded); }
};
} // namespace drag_global::event_graph
