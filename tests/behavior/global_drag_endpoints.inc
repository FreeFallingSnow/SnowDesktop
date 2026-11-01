// Included in namespace drag_global after Axis, Role and AddAxis are defined.
// The 26 E-fields describe each endpoint, not the payload. Labels are stable
// keys for an isolated scene adapter; adding a key must extend the product.
// A field's `na` is a structural value, not a product rejection or a skip.
// See the dependency notes below before deciding whether any value is na.
inline void AppendEndpointAxes(std::vector<Axis>& axes)
{
    for (const std::string_view side : {std::string_view{"source"}, std::string_view{"target"}})
    {
        const auto key = [side](std::string_view suffix) {
            return std::string(side) + "." + std::string(suffix);
        };

        AddAxis(axes, key("E01"), Role::Input,
            {"free_desktop", "desktop_dock", "floating_dock", "widget_popup", "folder_popup",
             "external_explorer", "external_program", "external_unknown"}, "host location");
        // An external IDataObject does not identify its owner window or process.
        // external_unknown remains an unimplemented adapter state, not evidence
        // that the source was Explorer or another specific application.
        // Container type, entry representation and host location are distinct.
        // A popup retains both its owning entry and logical container identity.
        // For an external IDataObject, the UI source container may be unknown
        // even if its provider was built from a known filesystem directory.
        AddAxis(axes, key("E02"), Role::Conditional,
            {"none", "collection", "desktop_files", "folder_mapping", "collection_group", "file_group",
             "lua_logical_slot", "lua_file_receiver", "lua_other", "guide_widget", "shell_folder", "shell_handler",
             "unknown", "na"},
            "container type");
        // none requires positive evidence that no entry exists; an external
        // data object's source UI representation may instead be unknown.
        AddAxis(axes, key("E03"), Role::Conditional,
            {"widget_body", "desktop_icon", "dock_entry", "group_label", "none", "unknown"}, "entry representation");
        AddAxis(axes, key("E04"), Role::Conditional,
            {"ordinary", "dock_exclusive", "dock_mapping", "frequent_generated", "running_generated",
             "fixed_control", "na"}, "entry role");
        // Waiting/closing are observable transient states and must be tied to
        // an explicit popup event sequence, not fabricated after the drop.
        AddAxis(axes, key("E05"), Role::Conditional,
            {"not_open", "hover_waiting", "open", "closing", "na"}, "popup state");
        AddAxis(axes, key("E06"), Role::Conditional,
            {"scroll_panel", "fan", "na"}, "popup style");
        AddAxis(axes, key("E07"), Role::Conditional,
            {"icon_grid", "list", "na"}, "content view");
        AddAxis(axes, key("E08"), Role::Conditional,
            {"no_child", "first_child", "middle_child", "last_child", "invalid_child", "na"}, "active child position");
        // Group root type and active child type are separate: a file group
        // can contain either a desktop-files or folder-mapping child. Unknown
        // identities remain explicit until their fixture adapter is implemented.
        AddAxis(axes, key("E08.active_child_type"), Role::Conditional,
            {"collection", "desktop_files", "folder_mapping", "unknown", "na"}, "active child container type");
        // A hit on another child and that child's ordinal position must not
        // erase the separately configured active child or its ordinal.
        AddAxis(axes, key("E08.hit_relation"), Role::Conditional,
            {"active", "inactive_valid", "invalid", "no_child", "na"}, "hit child relative to active child");
        AddAxis(axes, key("E08.hit_position"), Role::Conditional,
            {"first_child", "middle_child", "last_child", "na"}, "hit child position");
        AddAxis(axes, key("E09"), Role::Conditional,
            {"none", "category_filter", "search_hit", "cross_child_search", "empty_result", "na"}, "filter state");
        // Endpoint UI availability is independent of provider content
        // availability (T04). External UI state may be unknown; na applies
        // only when the endpoint UI state is structurally inapplicable.
        AddAxis(axes, key("E10"), Role::Conditional,
            {"ready", "loading", "inaccessible", "invalid_object", "unknown", "na"}, "endpoint UI availability");
        AddAxis(axes, key("E11"), Role::Conditional, {"expanded", "collapsed", "na"}, "title bar state");
        AddAxis(axes, key("E12"), Role::Conditional, {"on", "off", "na"}, "hover auto expand");
        AddAxis(axes, key("E13"), Role::Conditional, {"on", "off", "na"}, "keep when desktop hidden");
        AddAxis(axes, key("E14"), Role::Conditional, {"on", "off", "na"}, "hover only visibility");
        AddAxis(axes, key("E15"), Role::Conditional, {"on", "off", "na"}, "privacy mode");
        AddAxis(axes, key("E16"), Role::Conditional, {"large_folder", "scroll_panel", "na"}, "collection shell style");
        // Each value maps to a concrete span, not an ambiguous "multiple".
        // 1/2/3/4/8 are current span representatives, not evidence that every
        // supported numeric size has the same behavior as one of them.
        // Current occupancy also follows owner and layout, so E17 itself is
        // observed/derived. Width and height vary independently.
        AddAxis(axes, key("E17.columns"), Role::Conditional, {"one", "two", "three", "four", "eight", "na"}, "configured object span columns");
        AddAxis(axes, key("E17.rows"), Role::Conditional, {"one", "two", "three", "four", "eight", "na"}, "configured object span rows");
        AddAxis(axes, key("E17"), Role::Derived,
            {"single_cell", "multi_column", "multi_row", "multi_both", "multi_cell_boundary", "na"}, "current occupancy");
        AddAxis(axes, key("E18"), Role::Conditional, {"on", "off", "na"}, "compact no label mode");
        AddAxis(axes, key("E19"), Role::Conditional, {"named", "unnamed", "na"}, "page naming");
        AddAxis(axes, key("E20"), Role::Conditional,
            {"only_dragged", "other_visible_items", "only_collected_or_grouped", "loading_unknown", "na"},
            "other page contents");
        // Page role is derived from V05/V06, page order and page identity.
        AddAxis(axes, key("E21"), Role::Derived,
            {"front_fixed_monitor", "last_monitor_default", "additional_page", "na"}, "page mapping role");
        AddAxis(axes, key("E22"), Role::Conditional,
            {"none", "configured_active", "configured_unadjusted", "na"}, "desktop item large icon configuration");

        // Page sizes also include twelve, used by the native container fixture.
        // These are representatives and do not cover all supported sizes.
        // Cross-combining
        // them produces exact, insufficient and excess relationships without
        // treating those overlapping relations as input categories.
        // Wait-time size changes remain a separate event input.
        AddAxis(axes, key("E23"), Role::Conditional, {"one", "two", "three", "four", "eight", "twelve", "na"}, "page column count");
        AddAxis(axes, key("E23.fit"), Role::Derived,
            {"exact", "insufficient", "excess", "invalid", "na"}, "page width versus requested span");
        AddAxis(axes, key("E23.change"), Role::Sequence,
            {"stable", "increase_during_wait", "decrease_during_wait", "na"}, "page width change");
        AddAxis(axes, key("E24"), Role::Conditional, {"one", "two", "three", "four", "eight", "twelve", "na"}, "page row count");
        AddAxis(axes, key("E24.fit"), Role::Derived,
            {"exact", "insufficient", "excess", "invalid", "na"}, "page height versus requested span");
        AddAxis(axes, key("E24.change"), Role::Sequence,
            {"stable", "increase_during_wait", "decrease_during_wait", "na"}, "page height change");

        AddAxis(axes, key("E25"), Role::Conditional,
            {"rounded_rectangle", "rounded_square", "circle", "flag", "diamond", "hexagon", "na"}, "large icon shape");
        // Flag direction applies to E25=flag, hexagon regularity to
        // E25=hexagon, and radius policy/size to the two rounded shapes.
        // Their active rendering is inapplicable for other shapes; saved
        // dormant settings may still require preservation when shapes change.
        AddAxis(axes, key("E25.flag_direction"), Role::Conditional,
            {"right", "left", "up", "down", "na"}, "flag notch direction");
        AddAxis(axes, key("E25.regular_hexagon"), Role::Conditional,
            {"on", "off", "na"}, "regular hexagon");
        AddAxis(axes, key("E25.radius_policy"), Role::Conditional,
            {"fixed", "follow_component", "na"}, "rounded corner radius policy");
        AddAxis(axes, key("E25.radius_size"), Role::Conditional,
            {"zero", "nonzero", "maximum", "na"}, "rounded corner radius class");

        // Recursive Everything projection is source-only. Target-side
        // recursive_projection is retained here for the auditable structural
        // exclusion; persistent_member and na are not globally excluded.
        AddAxis(axes, key("E26"), Role::Conditional,
            {"persistent_member", "recursive_projection", "na"}, "member projection type");
    }
}

// These values are fixture inputs. -1 means the axis is structurally not
// applicable; unknown keys fail rather than quietly choosing a dimension.
inline int EndpointSize(std::string_view value)
{
    if (value == "one") return 1;
    if (value == "two") return 2;
    if (value == "three") return 3;
    if (value == "four") return 4;
    if (value == "eight") return 8;
    if (value == "twelve") return 12;
    if (value == "na") return -1;
    throw std::invalid_argument("unknown endpoint size value");
}

// V4 distinguishes an exact signed value (including -1) from structural na.
// The eight digits are the uint32 bit pattern in most-significant-first order;
// unknown/malformed tokens remain unavailable rather than becoming zero.
enum class V4EndpointDimensionStatus { exact, not_applicable, unavailable };
struct V4EndpointDimension
{
    V4EndpointDimensionStatus status = V4EndpointDimensionStatus::unavailable;
    std::int32_t value = 0;
};
inline V4EndpointDimension ParseV4EndpointDimension(std::string_view token) noexcept
{
    if (token == "na") return {V4EndpointDimensionStatus::not_applicable, 0};
    if (token == "one") return {V4EndpointDimensionStatus::exact, 1};
    if (token == "two") return {V4EndpointDimensionStatus::exact, 2};
    if (token == "three") return {V4EndpointDimensionStatus::exact, 3};
    if (token == "four") return {V4EndpointDimensionStatus::exact, 4};
    if (token == "eight") return {V4EndpointDimensionStatus::exact, 8};
    if (token == "twelve") return {V4EndpointDimensionStatus::exact, 12};
    constexpr std::string_view prefix = "int32_hex_";
    if (!token.starts_with(prefix) || token.size() != prefix.size() + 8) return {};
    std::uint32_t bits = 0;
    for (const char ch : token.substr(prefix.size()))
    {
        const unsigned digit = ch >= '0' && ch <= '9' ? static_cast<unsigned>(ch - '0') :
            ch >= 'a' && ch <= 'f' ? static_cast<unsigned>(ch - 'a' + 10) : 16u;
        if (digit > 15) return {};
        bits = (bits << 4) | digit;
    }
    const std::int64_t signedValue = (bits & 0x80000000u) != 0
        ? static_cast<std::int64_t>(bits) - (std::int64_t{1} << 32) : static_cast<std::int64_t>(bits);
    return {V4EndpointDimensionStatus::exact, static_cast<std::int32_t>(signedValue)};
}
inline std::optional<std::string_view> V4EndpointFit(std::string_view pageToken, std::string_view spanToken) noexcept
{
    const auto page = ParseV4EndpointDimension(pageToken), span = ParseV4EndpointDimension(spanToken);
    if (page.status == V4EndpointDimensionStatus::unavailable || span.status == V4EndpointDimensionStatus::unavailable)
        return std::nullopt;
    if (page.status == V4EndpointDimensionStatus::not_applicable || span.status == V4EndpointDimensionStatus::not_applicable)
        return "na";
    if (page.value <= 0 || span.value <= 0) return "invalid";
    if (page.value == span.value) return "exact";
    return page.value < span.value ? "insufficient" : "excess";
}

// Compare the actual test input dimensions independently of host placement
// logic. Callers must supply the relevant requested span (which may differ
// from a saved large-icon configuration after ownership normalization).
inline std::string_view EndpointFit(int pageSize, int requestedSpan)
{
    if (pageSize < -1 || requestedSpan < -1) throw std::invalid_argument("negative endpoint size");
    if (pageSize == -1 || requestedSpan == -1) return "na";
    if (pageSize == 0 || requestedSpan == 0) return "invalid";
    if (pageSize == requestedSpan) return "exact";
    return pageSize < requestedSpan ? "insufficient" : "excess";
}

// Only proven structural exclusions belong here. In particular, shape
// settings may be dormant but persisted; a page absent at capture may appear
// during the event sequence; malformed or omitted configuration can itself
// be a negative input. Those cases remain in the execution set until their
// exact construction and expectations are specified. Derived fit and span
// are observations, not independently assigned values available to Value().
inline std::string_view EndpointExclusion(const std::vector<Axis>& axes, const Candidate& candidate)
{
    if (Value(axes, candidate, "target.E26") == "recursive_projection")
        return "target_recursive_projection_is_source_only";
    return {};
}
